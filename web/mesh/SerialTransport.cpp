// SPDX-License-Identifier: GPL-3.0-or-later
// Native companion transports for the host build: a USB serial port (POSIX termios)
// and a TCP socket (the node's TCP companion server, port 5000). Both are byte streams
// with the '<'/'>' len16 framing (CompanionTransport.h). Not built for the browser.
#include "CompanionTransport.h"
#include <stdio.h>

#if defined(__EMSCRIPTEN__) || defined(_WIN32)

CompanionTransport* wadaCreateSerialTransport(const char*) { return nullptr; }
CompanionTransport* wadaCreateTcpTransport(const char*, int) { return nullptr; }

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <termios.h>
#include <unistd.h>

namespace {

// Shared by both: a non-blocking fd carrying the '<'/'>' stream framing, reopened
// every RETRY_MS after a failure (a node that resets re-enumerates its USB port).
class FdStreamTransport : public CompanionTransport {
public:
  bool autoReconnect() const override { return true; }
  bool isConnected() const override { return _fd >= 0 && _up; }
  bool open() override {
    _want_open = true;
    _next_try = 0;
    return true;
  }
  void close() override {
    _want_open = false;
    drop(nullptr);
  }
  bool sendFrame(const uint8_t* frame, size_t len) override {
    if (_fd < 0) return false;
    const std::vector<uint8_t> out = companionEncodeHostFrame(frame, len);
    size_t off = 0;
    int spins = 0;
    while (off < out.size()) {
      const ssize_t n = ::write(_fd, out.data() + off, out.size() - off);
      if (n > 0) { off += (size_t)n; continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && spins++ < 200) {
        struct pollfd p = {_fd, POLLOUT, 0};
        ::poll(&p, 1, 5);
        continue;
      }
      drop("write failed");
      return false;
    }
    return true;
  }
  void poll(uint32_t now_ms) override {
    if (_fd < 0) {
      if (!_want_open || (int32_t)(now_ms - _next_try) < 0) return;
      _next_try = now_ms + RETRY_MS;
      std::string why;
      _fd = connectFd(why);
      if (_fd < 0) {
        if (why != _last_err) fprintf(stderr, "[companion] %s %s: %s (retrying)\n", name(), describe(), why.c_str());
        _last_err = why;
        return;
      }
      _last_err.clear();
      _deframer.reset();
      _up = false;
    }
    if (!_up) {
      if (!connectDone()) return;
      _up = true;
      fprintf(stderr, "[companion] %s %s: open\n", name(), describe());
      if (_listener) _listener->onTransportConnected();
      if (_fd < 0) return;
    }
    uint8_t buf[4096];
    for (int guard = 0; guard < 16 && _fd >= 0; ++guard) {
      const ssize_t n = ::read(_fd, buf, sizeof buf);
      if (n > 0) {
        _deframer.feed(buf, (size_t)n, [this](const uint8_t* f, size_t l) {
          if (_listener) _listener->onTransportFrame(f, l);
        });
        continue;
      }
      if (n == 0 && isSocket()) { drop("closed by the node"); return; }
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { drop(strerror(errno)); return; }
      break;
    }
  }

protected:
  static const uint32_t RETRY_MS = 2000;
  virtual int connectFd(std::string& why) = 0;
  virtual bool connectDone() { return true; }
  virtual bool isSocket() const { return false; }
  void drop(const char* why) {
    if (_fd >= 0) ::close(_fd);
    _fd = -1;
    const bool was_up = _up;
    _up = false;
    _deframer.reset();
    if (was_up && why) {
      fprintf(stderr, "[companion] %s %s: %s\n", name(), describe(), why);
      if (_listener) _listener->onTransportDisconnected(why);
    }
  }
  int _fd = -1;
  bool _up = false;
  bool _want_open = false;
  uint32_t _next_try = 0;
  std::string _last_err;
  StreamDeframer _deframer;
};

// USB serial. Opening a port asserts DTR and RTS; on an ESP32-S3 a DTR/RTS sequence can
// reset the chip or drop it into the bootloader. Both lines are released in ONE ioctl
// right after open (never passing through DTR=0/RTS=1, the reset state), and HUPCL is
// cleared so closing the port does not toggle them either. The phone apps and the
// scratchpad companion_test.py open the same way (dtr = rts = False).
class SerialTransport : public FdStreamTransport {
public:
  explicit SerialTransport(const char* path) : _path(path ? path : "") {}
  const char* name() const override { return "serial"; }
  const char* describe() const override { return _path.c_str(); }

protected:
  int connectFd(std::string& why) override {
    const int fd = ::open(_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { why = strerror(errno); return -1; }
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &lines);
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
      cfmakeraw(&t);
      t.c_cflag |= (CLOCAL | CREAD);
      t.c_cflag &= ~(tcflag_t)HUPCL;
      t.c_cc[VMIN] = 0;
      t.c_cc[VTIME] = 0;
      cfsetispeed(&t, B115200);   // USB CDC ignores it; a UART bridge needs it
      cfsetospeed(&t, B115200);
      tcsetattr(fd, TCSANOW, &t);
    }
    ioctl(fd, TIOCMBIC, &lines);  // some drivers re-assert on tcsetattr
    return fd;
  }

private:
  std::string _path;
};

// TCP companion server (the node's port 5000; Wi-Fi on). Non-blocking connect.
class TcpTransport : public FdStreamTransport {
public:
  TcpTransport(const char* host, int port) : _host(host ? host : ""), _port(port) {
    snprintf(_desc, sizeof _desc, "%s:%d", _host.c_str(), _port);
  }
  const char* name() const override { return "tcp"; }
  const char* describe() const override { return _desc; }

protected:
  bool isSocket() const override { return true; }
  int connectFd(std::string& why) override {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof port, "%d", _port);
    const int rc = getaddrinfo(_host.c_str(), port, &hints, &res);
    if (rc != 0 || !res) { why = gai_strerror(rc); return -1; }
    const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { why = strerror(errno); freeaddrinfo(res); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
      why = strerror(errno);
      ::close(fd);
      freeaddrinfo(res);
      return -1;
    }
    freeaddrinfo(res);
    return fd;
  }
  bool connectDone() override {
    struct pollfd p = {_fd, POLLOUT, 0};
    if (::poll(&p, 1, 0) <= 0) return false;
    int err = 0;
    socklen_t l = sizeof err;
    getsockopt(_fd, SOL_SOCKET, SO_ERROR, &err, &l);
    if (err != 0) {
      fprintf(stderr, "[companion] tcp %s: %s (retrying)\n", _desc, strerror(err));
      ::close(_fd);
      _fd = -1;
      return false;
    }
    return true;
  }

private:
  std::string _host;
  int _port;
  char _desc[96];
};

}  // namespace

CompanionTransport* wadaCreateSerialTransport(const char* device_path) { return new SerialTransport(device_path); }
CompanionTransport* wadaCreateTcpTransport(const char* host, int port) { return new TcpTransport(host, port); }

#endif
