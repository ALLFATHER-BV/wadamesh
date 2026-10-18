#pragma once
// HTTPClient for the web board: every request goes through wadaHttpRequest()
// (compat/wada_http.h, the one network hook) and the reply is served from memory.
// That hook serves the map tile server only (see wada_http.h); every other request is
// refused with HTTPC_ERROR_CONNECTION_REFUSED and takes its offline path.
#include "Arduino.h"
#include "WiFiClient.h"
#include "wada_http.h"
#define HTTPC_ERROR_CONNECTION_REFUSED  (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED  (-2)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED       (-4)
#define HTTPC_ERROR_CONNECTION_LOST     (-5)
#define HTTPC_ERROR_NO_STREAM           (-6)
#define HTTPC_ERROR_NO_HTTP_SERVER      (-7)
#define HTTPC_ERROR_TOO_LESS_RAM        (-8)
#define HTTPC_ERROR_ENCODING            (-9)
#define HTTPC_ERROR_STREAM_WRITE        (-10)
#define HTTPC_ERROR_READ_TIMEOUT        (-11)
typedef enum { HTTPC_DISABLE_FOLLOW_REDIRECTS, HTTPC_STRICT_FOLLOW_REDIRECTS, HTTPC_FORCE_FOLLOW_REDIRECTS } followRedirects_t;
typedef enum {
  HTTP_CODE_CONTINUE = 100, HTTP_CODE_OK = 200, HTTP_CODE_NO_CONTENT = 204, HTTP_CODE_PARTIAL_CONTENT = 206,
  HTTP_CODE_MOVED_PERMANENTLY = 301, HTTP_CODE_FOUND = 302, HTTP_CODE_NOT_MODIFIED = 304,
  HTTP_CODE_BAD_REQUEST = 400, HTTP_CODE_FORBIDDEN = 403, HTTP_CODE_NOT_FOUND = 404,
  HTTP_CODE_INTERNAL_SERVER_ERROR = 500
} t_http_codes;
// The reply body as the Stream the UI's readers expect (getStreamPtr()).
class WadaBodyClient : public WiFiClient {
public:
  void load(const std::vector<uint8_t>* b) { _b = b; _pos = 0; }
  int available() override { return _b ? (int)(_b->size() - _pos) : 0; }
  int read() override { return available() > 0 ? (*_b)[_pos++] : -1; }
  int read(uint8_t* buf, size_t n) override {
    const int a = available();
    if (a <= 0) return -1;
    if ((int)n > a) n = (size_t)a;
    memcpy(buf, _b->data() + _pos, n); _pos += n;
    return (int)n;
  }
  int peek() override { return available() > 0 ? (*_b)[_pos] : -1; }
  uint8_t connected() override { return available() > 0; }
  operator bool() override { return _b != nullptr; }
private:
  const std::vector<uint8_t>* _b = nullptr;
  size_t _pos = 0;
};
class HTTPClient {
public:
  bool begin(const String& url) { _url = url; return true; }
  bool begin(const String& url, const char* ca) { (void)ca; _url = url; return true; }
  bool begin(WiFiClient& c, const String& url) { (void)c; _url = url; return true; }
  bool begin(WiFiClient& c, const String& host, uint16_t port, const String& uri = "/", bool https = false) {
    (void)c;
    _url = String(https ? "https://" : "http://") + host + ":" + String(port) + uri;
    return true;
  }
  void end() { _reply = WadaHttpReply(); _stream.load(nullptr); _headers.clear(); }
  bool connected() { return _stream.available() > 0; }
  void setReuse(bool) {}
  void setUserAgent(const String& ua) { _headers.push_back(std::string("User-Agent: ") + ua.c_str()); }
  void setAuthorization(const char*, const char*) {}
  void setTimeout(uint16_t ms) { _timeout = ms; }
  void setConnectTimeout(int32_t) {}
  void setFollowRedirects(followRedirects_t) {}
  void setRedirectLimit(uint16_t) {}
  void useHTTP10(bool = true) {}
  void addHeader(const String& n, const String& v, bool = false, bool = true) {
    _headers.push_back(std::string(n.c_str()) + ": " + v.c_str());
  }
  void collectHeaders(const char* [], const size_t) {}
  String header(const char*) { return String(); }
  bool hasHeader(const char*) { return false; }
  int GET() { return sendRequest("GET"); }
  int POST(const uint8_t* p, size_t n) { return sendRequest("POST", p, n); }
  int POST(const String& p) { return sendRequest("POST", (const uint8_t*)p.c_str(), p.length()); }
  int sendRequest(const char* method, const uint8_t* p = nullptr, size_t n = 0) {
    const int code = wadaHttpRequest(method, std::string(_url.c_str()), _headers, p, n, _timeout, _reply);
    _stream.load(code > 0 ? &_reply.body : nullptr);
    return code;
  }
  int getSize() { return _stream ? (int)_reply.body.size() : -1; }
  WiFiClient* getStreamPtr() { return _stream ? &_stream : nullptr; }
  WiFiClient& getStream() { return _stream; }
  int writeToStream(Stream* s) {
    if (!s || !_stream) return HTTPC_ERROR_NO_STREAM;
    return (int)s->write(_reply.body.data(), _reply.body.size());
  }
  String getString() {
    if (!_stream) return String();
    String out; out.reserve(_reply.body.size());
    for (uint8_t c : _reply.body) out += (char)c;
    return out;
  }
  static String errorToString(int) { return String("not reachable from the web build"); }
  const String& getLocation() { return _url; }
private:
  String _url;
  uint32_t _timeout = 5000;
  std::vector<std::string> _headers;
  WadaHttpReply _reply;
  WadaBodyClient _stream;
};
