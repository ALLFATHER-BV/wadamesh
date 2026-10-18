#pragma once
// HTTPClient for the web board. Phase 1: every request fails with
// HTTPC_ERROR_CONNECTION_REFUSED, so the map tiles, the version check and the Reader
// take their offline paths. Phase 2 implements GET/POST over the browser's fetch().
#include "Arduino.h"
#include "WiFiClient.h"
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
class HTTPClient {
public:
  bool begin(const String& url) { _url = url; return true; }
  bool begin(const String& url, const char* ca) { (void)ca; _url = url; return true; }
  bool begin(WiFiClient& c, const String& url) { (void)c; _url = url; return true; }
  bool begin(WiFiClient& c, const String& host, uint16_t port, const String& uri = "/", bool https = false) {
    (void)c; (void)port; (void)https; _url = host + uri; return true;
  }
  void end() {}
  bool connected() { return false; }
  void setReuse(bool) {}
  void setUserAgent(const String&) {}
  void setAuthorization(const char*, const char*) {}
  void setTimeout(uint16_t) {}
  void setConnectTimeout(int32_t) {}
  void setFollowRedirects(followRedirects_t) {}
  void setRedirectLimit(uint16_t) {}
  void useHTTP10(bool = true) {}
  void addHeader(const String&, const String&, bool = false, bool = true) {}
  void collectHeaders(const char* [], const size_t) {}
  String header(const char*) { return String(); }
  bool hasHeader(const char*) { return false; }
  int GET() { return HTTPC_ERROR_CONNECTION_REFUSED; }
  int POST(const uint8_t*, size_t) { return HTTPC_ERROR_CONNECTION_REFUSED; }
  int POST(const String&) { return HTTPC_ERROR_CONNECTION_REFUSED; }
  int sendRequest(const char*, const uint8_t* = nullptr, size_t = 0) { return HTTPC_ERROR_CONNECTION_REFUSED; }
  int getSize() { return -1; }
  WiFiClient* getStreamPtr() { return nullptr; }
  WiFiClient& getStream() { return _client; }
  int writeToStream(Stream*) { return HTTPC_ERROR_NO_STREAM; }
  String getString() { return String(); }
  static String errorToString(int) { return String("no network in the web build yet"); }
  const String& getLocation() { return _url; }
private:
  String _url;
  WiFiClient _client;
};
