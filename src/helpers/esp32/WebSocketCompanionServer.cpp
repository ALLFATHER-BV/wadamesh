#include "WebSocketCompanionServer.h"
#include <Arduino.h>
#include <mbedtls/sha1.h>
#include <string.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include "WebMirror.h"

#ifndef WS_FRAME_DEBUG
#define WS_FRAME_DEBUG 0
#endif

#define TCP_WRITE_TIMEOUT_MS   120
#define WS_WEDGED_DROP_MS      10000
#define WS_MIRROR_WRITE_TIMEOUT_MS 250   // mirror bands are bigger than companion frames — a little more headroom before we treat the frame as unrecoverable
#define WS_MAGIC               "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MIRROR_TXBUF        33000        // max popped mirror frame (band <= 32000 + header)
#define WS_MIRROR_TX_BUDGET    (48 * 1024)  // max mirror bytes pushed per serviceMirror() call
#define WS_MIRROR_CLIENT_TXBUF 8192         // per-client outgoing frame buffer (one WS frame; bands <= ~4 KB)

// Browser opens http://device:8765/ — the web UI mirror page. It opens a
// WebSocket to /mirror, paints framebuffer bands the device streams (RGB565 LE,
// LV_COLOR_16_SWAP=0), and forwards pointer taps back. Self-contained (no CDN):
// a strict LAN, plain-HTTP page. The MeshCore companion app still connects with a
// WebSocket upgrade to "/" and is routed to the companion protocol, not here.
static const char WS_HTTP_INFO_PAGE[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/html; charset=utf-8\r\n"
  "Connection: close\r\n"
  "\r\n"
  "<!DOCTYPE html><html><head><meta charset=utf-8>\n"
  "<meta name=viewport content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>\n"
  "<link href='https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@700&display=swap' rel=stylesheet>\n"
  "<title>wadamesh</title>\n"
  "<style>\n"
  "html,body{margin:0;height:100%;background:#0b0b0c;color:#e8e8ea;font-family:system-ui,-apple-system,sans-serif;overflow:hidden;-webkit-user-select:none;user-select:none;touch-action:none}\n"
  "#w{display:flex;flex-direction:column;align-items:center;justify-content:flex-start;height:100%;gap:8px;padding-top:6px;box-sizing:border-box}\n"
  "#c{background:#000;image-rendering:auto;max-width:100vw;box-shadow:0 0 26px #000a;border-radius:8px}\n"
  "#s{font-size:13px;opacity:.7} #s.ok{color:#19d6c2} #s.err{color:#ff6b6b}\n"
  "b{color:#19d6c2;letter-spacing:.5px}\n"
  "#kb{margin-top:8px;padding:9px 18px;font-size:14px;background:#1c1c1f;color:#e8e8ea;border:1px solid #333;border-radius:8px}\n"
  "#kb:active{background:#19d6c2;color:#000;border-color:#19d6c2}\n"
  "#rot{margin-top:6px;padding:7px 15px;font-size:12px;background:#141416;color:#c8ccce;border:1px solid #2a2a2e;border-radius:8px;display:none}\n"
  "#rot:active{background:#19d6c2;color:#000;border-color:#19d6c2}\n"
  "#k{position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;border:0;padding:0;font-size:16px}\n"
  "#hdr{position:fixed;top:14px;left:18px;display:none;align-items:center;gap:11px;z-index:5}\n"
  "#hdr .wm{font-family:'JetBrains Mono','Courier New',monospace;font-weight:700;font-size:22px;letter-spacing:2px;color:#e8e8ea}\n"
  "#hdr .wm .m{color:#15B6A6}\n"
  // Desktop = a real mouse (hover + fine pointer), NOT window width — a half-width or
  // display-scaled desktop window is still a desktop. Touch devices get the compact layout.
  "@media(hover:hover) and (pointer:fine){#hdr{display:flex}#cap{display:none}}\n"
  "</style></head><body>\n"
  "<div id=hdr><svg width='40' height='27' viewBox='0 0 180 120' fill='none'><path d='M10 60 L50 108 L90 60 L130 108 L170 60' stroke='#dfe3e8' stroke-width='9' stroke-linecap='round' stroke-linejoin='round'/><path d='M10 60 L50 12 L90 60 L130 12 L170 60' stroke='#dfe3e8' stroke-width='9' stroke-linecap='round' stroke-linejoin='round'/><g fill='#15B6A6'><circle cx='10' cy='60' r='7'/><circle cx='90' cy='60' r='7'/><circle cx='170' cy='60' r='7'/><circle cx='50' cy='12' r='7'/><circle cx='130' cy='12' r='7'/><circle cx='50' cy='108' r='7'/><circle cx='130' cy='108' r='7'/></g></svg><span class=wm>WADA<span class=m>MESH</span></span></div>\n"
  "<div id=w>\n"
  "<div id=cap><b>wadamesh</b> &nbsp; live UI</div>\n"
  "<canvas id=c width=320 height=240></canvas>\n"
  "<div id=s>connecting...</div>\n"
  "<button id=kb>&#9000; Keyboard</button>\n"
  "<button id=rot>&#8635; Rotate</button>\n"
  "<textarea id=k autocomplete=off autocorrect=off autocapitalize=off spellcheck=false></textarea>\n"
  "</div>\n"
  "<script>\n"
  "var C=document.getElementById('c'),X=C.getContext('2d'),S=document.getElementById('s');\n"
  "var OT=document.createElement('canvas'),OX=OT.getContext('2d');\n"
  "var DW=320,DH=240,down=false,last=0,ws;\n"
  "var isTouch=('ontouchstart' in window)||navigator.maxTouchPoints>0,kbT=null;\n"
  "var ROT=document.getElementById('rot');\n"
  "function st(t,k){S.textContent=t;S.className=k||''}\n"
  "function conn(){\n"
  " ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/mirror');\n"
  " ws.binaryType='arraybuffer';\n"
  " ws.onopen=function(){st('connected','ok')};\n"
  " ws.onclose=function(){st('disconnected - retrying','err');setTimeout(conn,1500)};\n"
  " ws.onerror=function(){st('connection error','err')};\n"
  " ws.onmessage=function(e){\n"
  "  var a=new Uint8Array(e.data);\n"
  "  if(a[0]==2){DW=a[1]|(a[2]<<8);DH=a[3]|(a[4]<<8);if(C.width!=DW)C.width=DW;if(C.height!=DH)C.height=DH;ROT.style.display=(a.length>5&&(a[5]&1))?'':'none';fit();return}\n"
  "  if(a[0]==3){clearTimeout(kbT);if(a[1])K.focus();else K.blur();return}\n"
  "  if(a[0]==1){\n"
  "   var fl=a[1],x=a[2]|(a[3]<<8),y=a[4]|(a[5]<<8),w=a[6]|(a[7]<<8),h=a[8]|(a[9]<<8);\n"
  "   var hf=fl&2,ow=hf?((w+1)>>1):w,oh=hf?((h+1)>>1):h;\n"
  "   var img=X.createImageData(ow,oh),d=img.data,L=d.length,p=10,o=0,v,r,g,b,c;\n"
  "   if(fl&1){\n"
  "    while(o<L){c=a[p++];v=a[p]|(a[p+1]<<8);p+=2;r=(v>>8)&0xf8;g=(v>>3)&0xfc;b=(v<<3)&0xf8;\n"
  "     while(c-->0){d[o++]=r;d[o++]=g;d[o++]=b;d[o++]=255}}\n"
  "   }else{\n"
  "    while(o<L){v=a[p]|(a[p+1]<<8);p+=2;d[o++]=(v>>8)&0xf8;d[o++]=(v>>3)&0xfc;d[o++]=(v<<3)&0xf8;d[o++]=255}\n"
  "   }\n"
  "   if(hf){OT.width=ow;OT.height=oh;OX.putImageData(img,0,0);X.imageSmoothingEnabled=true;X.drawImage(OT,0,0,ow,oh,x,y,w,h)}\n"
  "   else{X.putImageData(img,x,y)}\n"
  "  }\n"
  " }\n"
  "}\n"
  "function xy(e){var r=C.getBoundingClientRect();\n"
  " var x=(e.clientX-r.left)*DW/r.width,y=(e.clientY-r.top)*DH/r.height;\n"
  " x=x<0?0:x>=DW?DW-1:x;y=y<0?0:y>=DH?DH-1:y;return[x|0,y|0]}\n"
  "function snd(x,y,pr){if(!ws||ws.readyState!=1)return;ws.send(new Uint8Array([1,x&255,x>>8,y&255,y>>8,pr]))}\n"
  "C.addEventListener('pointerdown',function(e){down=true;try{C.setPointerCapture(e.pointerId)}catch(x){}var p=xy(e);snd(p[0],p[1],1);e.preventDefault()});\n"
  "C.addEventListener('pointermove',function(e){if(!down)return;var t=Date.now();if(t-last<33)return;last=t;var p=xy(e);snd(p[0],p[1],1)});\n"
  "function up(e){if(!down)return;down=false;var p=xy(e);snd(p[0],p[1],0);\n"
  " if(isTouch){K.value=' ';K.focus();try{K.setSelectionRange(1,1)}catch(x){}clearTimeout(kbT);kbT=setTimeout(function(){K.blur()},350)}}\n"
  "C.addEventListener('pointerup',up);C.addEventListener('pointercancel',up);\n"
  "var K=document.getElementById('k');\n"
  "document.getElementById('kb').addEventListener('click',function(){K.value=' ';K.focus();try{K.setSelectionRange(1,1)}catch(x){}});\n"
  "ROT.addEventListener('click',function(){if(!ws||ws.readyState!=1)return;st('rotating - reconnecting','');ws.send(new Uint8Array([4,DW<DH?1:0]))});\n"
  "function skey(cp){if(!ws||ws.readyState!=1)return;ws.send(new Uint8Array([2,cp&255,(cp>>8)&255]))}\n"
  "K.addEventListener('beforeinput',function(e){var t=e.inputType;\n"
  " if(t=='insertText'&&e.data){for(var i=0;i<e.data.length;i++)skey(e.data.codePointAt(i));e.preventDefault()}\n"
  " else if(t=='deleteContentBackward'){skey(8);e.preventDefault()}\n"
  " else if(t=='insertLineBreak'||t=='insertParagraph'){skey(13);e.preventDefault()}\n"
  " K.value=' ';try{K.setSelectionRange(1,1)}catch(x){}});\n"
  "window.addEventListener('keydown',function(e){\n"
  " if(document.activeElement===K)return;if(e.ctrlKey||e.metaKey||e.altKey)return;\n"
  " if(e.key=='Backspace'){skey(8);e.preventDefault()}\n"
  " else if(e.key=='Enter'){skey(13);e.preventDefault()}\n"
  " else if(e.key.length==1){skey(e.key.codePointAt(0));e.preventDefault()}});\n"
  "function fit(){var vv=window.visualViewport,vw=vv?vv.width:window.innerWidth,vh=vv?vv.height:window.innerHeight;\n"
  " var desk=matchMedia('(hover:hover) and (pointer:fine)').matches,ch=desk?102:60,f=desk?0.72:0.99;\n"
  " var aw=vw-8,ah=vh-ch;if(aw<60)aw=60;if(ah<60)ah=60;var s=Math.min(aw/DW,ah/DH)*f;\n"
  " C.style.width=Math.max(1,Math.round(DW*s))+'px';C.style.height=Math.max(1,Math.round(DH*s))+'px'}\n"
  "if(window.visualViewport)window.visualViewport.addEventListener('resize',fit);\n"
  "window.addEventListener('resize',fit);fit();\n"
  "conn();\n"
  "</script></body></html>";

#define COMP_STATE_IDLE        0
#define COMP_STATE_HDR_FOUND   1
#define COMP_STATE_LEN1_FOUND  2
#define COMP_STATE_LEN2_FOUND  3

#define WS_STATE_HEADER_0      0
#define WS_STATE_HEADER_1      1
#define WS_STATE_LEN_EXT       2
#define WS_STATE_MASK          3
#define WS_STATE_PAYLOAD       4

static const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64Encode20(const uint8_t* in, char* out) {
  for (int i = 0; i < 20; i += 3) {
    uint32_t v = in[i] << 16;
    if (i + 1 < 20) v |= in[i + 1] << 8;
    if (i + 2 < 20) v |= in[i + 2];
    out[0] = BASE64[(v >> 18) & 63];
    out[1] = BASE64[(v >> 12) & 63];
    out[2] = (i + 1 < 20) ? BASE64[(v >> 6) & 63] : '=';
    out[3] = (i + 2 < 20) ? BASE64[v & 63] : '=';
    out += 4;
  }
  *out = '\0';
}

// True when lwIP can accept bytes for this socket right now — see the identical probe
// in TCPCompanionServer.cpp. Without it, WiFiClient::write() against a peer that
// stopped ACKing (half-open socket) blocks in 1 s select() waits on the loop thread.
static bool socketWritableNow(WiFiClient& client) {
  int fd = client.fd();
  if (fd < 0) return false;
  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(fd, &wset);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  return select(fd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(fd, &wset);
}

static bool writeAllBytes(WiFiClient& client, const uint8_t* buf, size_t len, uint32_t timeout_ms) {
  size_t sent = 0;
  uint32_t start = millis();
  while (sent < len) {
    if (!client.connected()) return false;
    if (socketWritableNow(client)) {
      size_t n = client.write(buf + sent, len - sent);
      if (n > 0) {
        sent += n;
        continue;
      }
    }
    if (millis() - start >= timeout_ms) return false;
    delay(1);
  }
  return true;
}

// RAII lock for the _clients array. Recursive so nested same-thread calls
// (serviceMirror -> writeBinaryFrame -> drainClientTx -> disconnectClient, or
// tickHandshake -> acceptNewClients/pruneDisconnected) never self-deadlock.
namespace {
struct WsClientsLock {
  SemaphoreHandle_t m;
  explicit WsClientsLock(SemaphoreHandle_t mtx) : m(mtx) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
  ~WsClientsLock() { if (m) xSemaphoreGiveRecursive(m); }
  WsClientsLock(const WsClientsLock&) = delete;
  WsClientsLock& operator=(const WsClientsLock&) = delete;
};
}

WebSocketCompanionServer::WebSocketCompanionServer()
  : _server(WiFiServer()), _port(0), _listening(false), _poll_start_idx(0), _mirror_txbuf(nullptr) {
  _client_mtx = xSemaphoreCreateRecursiveMutex();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    _clients[i].in_use = false;
    _clients[i].accept_ms = 0;
    _clients[i].handshake_done = false;
    _clients[i].handshake_len = 0;
    _clients[i].ws_state = WS_STATE_HEADER_0;
    _clients[i].comp_state = COMP_STATE_IDLE;
    _clients[i].stall_ms = 0;
    _clients[i].is_mirror = false;
    _clients[i].meta_sent = false;
    _clients[i].tx_buf = nullptr;
    _clients[i].tx_len = 0;
    _clients[i].tx_sent = 0;
  }
}

void WebSocketCompanionServer::begin(uint16_t port) {
  _port = port;
  _listening = true;
  _server.begin(port);
  Serial.printf("[WS] listening port=%u (plain)\n", (unsigned)port);
}

void WebSocketCompanionServer::stop() {
  WsClientsLock _lk(_client_mtx);
  _listening = false;
  _server.stop();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].in_use) {
      _clients[i].client.stop();
      _clients[i].in_use = false;
    }
  }
}

void WebSocketCompanionServer::pauseListen() {
  if (!_listening) return;
  _server.stop();
  _listening = false;
}

void WebSocketCompanionServer::resumeListen() {
  if (_listening || _port == 0) return;
  _server.begin(_port);
  _listening = true;
  Serial.printf("[WS] listen resumed port=%u (plain)\n", (unsigned)_port);
}

void WebSocketCompanionServer::acceptNewClients() {
  if (!_listening) return;
  while (_server.hasClient()) {
    WiFiClient incoming = _server.accept();
    if (!incoming) continue;
    int slot = -1;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      if (!_clients[i].in_use) {
        slot = i;
        break;
      }
    }
    // No free slot: evict the oldest existing client so a fresh connect can
    // recover from stuck slots (half-closed sockets where connected() still
    // reports true). Without this, every new connect is accept()ed then
    // immediately stop()ed, producing SYN/SYN+ACK/ACK/FIN-ACK with zero data.
    if (slot < 0) {
      uint32_t now = millis();
      uint32_t oldest_age = 0;
      for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
        uint32_t age = now - _clients[i].accept_ms;
        if (slot < 0 || age > oldest_age) {
          slot = i;
          oldest_age = age;
        }
      }
      _clients[slot].client.stop();
      _clients[slot].in_use = false;
    }
    _clients[slot].client = incoming;
    _clients[slot].in_use = true;
    _clients[slot].accept_ms = millis();
    _clients[slot].handshake_done = false;
    _clients[slot].handshake_len = 0;
    _clients[slot].ws_state = WS_STATE_HEADER_0;
    _clients[slot].comp_state = COMP_STATE_IDLE;
    _clients[slot].stall_ms = 0;
    _clients[slot].is_mirror = false;
    _clients[slot].meta_sent = false;
    _clients[slot].tx_len = 0;      // drop any stale pending frame from the previous occupant (tx_buf is reused)
    _clients[slot].tx_sent = 0;
  }
}

void WebSocketCompanionServer::pruneDisconnected() {
  uint32_t now = millis();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (!_clients[i].in_use) continue;
    bool stale_handshake = !_clients[i].handshake_done &&
                           (now - _clients[i].accept_ms) > WS_HANDSHAKE_TIMEOUT_MS;
    if (!_clients[i].client.connected() || stale_handshake) {
      _clients[i].client.stop();
      _clients[i].in_use = false;
    }
  }
}

bool WebSocketCompanionServer::doHandshake(int idx) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  while (cl->available() && c->handshake_len < WS_HANDSHAKE_MAX_LEN - 1) {
    char ch = (char)cl->read();
    c->handshake_buf[c->handshake_len++] = ch;
    c->handshake_buf[c->handshake_len] = '\0';
    if (c->handshake_len >= 4 &&
        c->handshake_buf[c->handshake_len - 4] == '\r' &&
        c->handshake_buf[c->handshake_len - 3] == '\n' &&
        c->handshake_buf[c->handshake_len - 2] == '\r' &&
        c->handshake_buf[c->handshake_len - 1] == '\n') {
      const char* key_hdr = "Sec-WebSocket-Key:";
      char* buf = c->handshake_buf;
      for (size_t i = 0; i + 20 < c->handshake_len; i++) {
        if (strncasecmp(buf + i, key_hdr, 18) == 0) {
          char* key_start = buf + i + 18;
          while (*key_start == ' ' || *key_start == '\t') key_start++;
          char* key_end = key_start;
          while (*key_end && *key_end != '\r' && *key_end != '\n') key_end++;
          size_t key_len = key_end - key_start;
          if (key_len == 0 || key_len > 128) break;

          char concat[128 + sizeof(WS_MAGIC)];
          memcpy(concat, key_start, key_len);
          memcpy(concat + key_len, WS_MAGIC, sizeof(WS_MAGIC) - 1);
          size_t concat_len = key_len + sizeof(WS_MAGIC) - 1;
          uint8_t hash[20];
          mbedtls_sha1((const unsigned char*)concat, concat_len, hash);
          char b64[32];
          base64Encode20(hash, b64);

          const char* resp = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
          size_t resp_len = strlen(resp);
          if (!writeAllBytes(*cl, (const uint8_t*)resp, resp_len, TCP_WRITE_TIMEOUT_MS))
            return false;
          if (!writeAllBytes(*cl, (const uint8_t*)b64, 28, TCP_WRITE_TIMEOUT_MS))
            return false;
          if (!writeAllBytes(*cl, (const uint8_t*)"\r\n\r\n", 4, TCP_WRITE_TIMEOUT_MS))
            return false;

          // Route GET /mirror to the web-UI mirror channel (display out + pointer
          // in); every other WS upgrade (the companion app connects to "/") stays
          // a companion peer on the shared protocol.
          c->is_mirror = (strncmp(c->handshake_buf, "GET /mirror", 11) == 0);
          if (c->is_mirror) c->client.setNoDelay(true);   // low latency: many small display frames, no Nagle coalescing
          c->meta_sent = false;
          c->handshake_done = true;
          c->ws_state = WS_STATE_HEADER_0;
          c->comp_state = COMP_STATE_IDLE;
          return true;
        }
      }
      (void)writeAllBytes(*cl, (const uint8_t*)WS_HTTP_INFO_PAGE, strlen(WS_HTTP_INFO_PAGE), TCP_WRITE_TIMEOUT_MS);
      c->client.stop();
      c->in_use = false;
      return false;
    }
  }
  if (c->handshake_len >= WS_HANDSHAKE_MAX_LEN - 1) {
    c->client.stop();
    c->in_use = false;
    return false;
  }
  return false;
}

void WebSocketCompanionServer::tickHandshake() {
  WsClientsLock _lk(_client_mtx);
  acceptNewClients();
  pruneDisconnected();
}

size_t WebSocketCompanionServer::pollRecvFrame(uint8_t dest[], int* client_index_out) {
  WsClientsLock _lk(_client_mtx);
  acceptNewClients();
  pruneDisconnected();

  int start = _poll_start_idx;
  if (start < 0 || start >= WS_COMPANION_MAX_CLIENTS) start = 0;

  for (int off = 0; off < WS_COMPANION_MAX_CLIENTS; off++) {
    int idx = (start + off) % WS_COMPANION_MAX_CLIENTS;
    if (!_clients[idx].in_use || !_clients[idx].client.connected()) continue;
    WSClientState* c = &_clients[idx];

    if (!c->handshake_done) {
      doHandshake(idx);
      continue;
    }
    // Mirror clients carry framebuffer/pointer traffic, not companion frames —
    // serviceMirror() owns their socket. Never feed their bytes to the parser.
    if (c->is_mirror) continue;

    WiFiClient* cl = &c->client;
    while (cl->available()) {
      if (c->ws_state == WS_STATE_HEADER_0) {
        uint8_t b = (uint8_t)cl->read();
        c->ws_opcode = b & 0x0F;
        c->ws_state = WS_STATE_HEADER_1;
        continue;
      }
      if (c->ws_state == WS_STATE_HEADER_1) {
        uint8_t b = (uint8_t)cl->read();
        uint8_t len7 = b & 0x7F;
        c->ws_payload_len = len7;
        c->ws_payload_read = 0;
        if (len7 == 126) {
          c->ws_state = WS_STATE_LEN_EXT;
          continue;
        }
        if (len7 == 127) {
          c->ws_state = WS_STATE_LEN_EXT;
          c->ws_payload_len = 0;
          continue;
        }
        c->ws_state = WS_STATE_MASK;
        continue;
      }
      if (c->ws_state == WS_STATE_LEN_EXT) {
        if (c->ws_payload_len == 126) {
          if (cl->available() < 2) break;
          uint8_t lo = (uint8_t)cl->read();
          uint8_t hi = (uint8_t)cl->read();
          c->ws_payload_len = (uint16_t)lo | ((uint16_t)hi << 8);
        } else {
          if (cl->available() < 8) break;
          c->ws_payload_len = 0;
          for (int i = 0; i < 8; i++)
            c->ws_payload_len |= (uint64_t)(uint8_t)cl->read() << (i * 8);
        }
        c->ws_state = WS_STATE_MASK;
        continue;
      }
      if (c->ws_state == WS_STATE_MASK) {
        if (cl->available() < 4) break;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = WS_STATE_PAYLOAD;
        continue;
      }

      if (c->ws_payload_read >= c->ws_payload_len) {
        c->ws_state = WS_STATE_HEADER_0;
        continue;
      }
      uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
      c->ws_payload_read++;

      if (c->ws_opcode != 0x02) continue;

      switch (c->comp_state) {
        case COMP_STATE_IDLE:
          if (b == '<') c->comp_state = COMP_STATE_HDR_FOUND;
          break;
        case COMP_STATE_HDR_FOUND:
          c->comp_frame_len = (uint16_t)b;
          c->comp_state = COMP_STATE_LEN1_FOUND;
          break;
        case COMP_STATE_LEN1_FOUND:
          c->comp_frame_len |= ((uint16_t)b) << 8;
          c->comp_rx_len = 0;
          c->comp_state = (c->comp_frame_len > 0) ? COMP_STATE_LEN2_FOUND : COMP_STATE_IDLE;
          break;
        default:
          if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len] = b;
          c->comp_rx_len++;
          if (c->comp_rx_len >= c->comp_frame_len) {
            size_t copy_len = c->comp_frame_len;
            if (copy_len > MAX_FRAME_SIZE) copy_len = MAX_FRAME_SIZE;
            memcpy(dest, c->comp_rx_buf, copy_len);
            c->comp_state = COMP_STATE_IDLE;
            if (client_index_out) *client_index_out = idx;
            _poll_start_idx = (idx + 1) % WS_COMPANION_MAX_CLIENTS;
            return copy_len;
          }
          break;
      }
    }
  }
  _poll_start_idx = (start + 1) % WS_COMPANION_MAX_CLIENTS;
  return 0;
}

size_t WebSocketCompanionServer::writeToClient(int client_index, const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS || len > MAX_FRAME_SIZE) return 0;
  if (!_clients[client_index].in_use || !_clients[client_index].client.connected()) return 0;

  WiFiClient* cl = &_clients[client_index].client;
  uint8_t hdr[4];
  size_t hdr_len;
  hdr[0] = 0x82;
  if (len < 126) {
    hdr[1] = (uint8_t)len;
    hdr_len = 2;
  } else {
    hdr[1] = 126;
    hdr[2] = (len >> 8) & 0xFF;
    hdr[3] = len & 0xFF;
    hdr_len = 4;
  }
  if (!writeAllBytes(*cl, hdr, hdr_len, TCP_WRITE_TIMEOUT_MS) ||
      !writeAllBytes(*cl, src, len, TCP_WRITE_TIMEOUT_MS)) {
#if WS_FRAME_DEBUG
    Serial.printf("WS frame client=%d code=%u len=%u written=0\n", client_index, (unsigned)(len ? src[0] : 0), (unsigned)len);
#endif
    // Same wedged-peer reaper as the TCP server: a client that stays unwritable for
    // WS_WEDGED_DROP_MS straight is half-open (browser tab gone, phone asleep) and
    // would otherwise eat the write-timeout budget on every pushed frame forever.
    WSClientState* c = &_clients[client_index];
    if (c->stall_ms == 0) {
      c->stall_ms = millis() | 1;
    } else if (millis() - c->stall_ms >= WS_WEDGED_DROP_MS) {
      disconnectClient(client_index);
    }
    return 0;
  }
  _clients[client_index].stall_ms = 0;
#if WS_FRAME_DEBUG
  Serial.printf("WS frame client=%d code=%u len=%u written=%u\n", client_index, (unsigned)(len ? src[0] : 0), (unsigned)len, (unsigned)len);
#endif
  return len;
}

size_t WebSocketCompanionServer::writeToAllClients(const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (len == 0 || len > MAX_FRAME_SIZE) return 0;
  int connected = 0;
  int sent = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    bool ok = _clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror;
    if (ok) {
      connected++;
      if (writeToClient(i, src, len) == len) sent++;
    }
  }
  return (sent == connected) ? len : 0;
}

bool WebSocketCompanionServer::isClientConnected(int client_index) const {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS) return false;
  const WSClientState* c = &_clients[client_index];
  return c->in_use && c->client.connected() && c->handshake_done && !c->is_mirror;
}

int WebSocketCompanionServer::connectedCount() const {
  WsClientsLock _lk(_client_mtx);
  int n = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror)
      n++;
  }
  return n;
}

void WebSocketCompanionServer::disconnectClient(int client_index) {
  WsClientsLock _lk(_client_mtx);
  if (client_index >= 0 && client_index < WS_COMPANION_MAX_CLIENTS && _clients[client_index].in_use) {
    _clients[client_index].client.stop();
    _clients[client_index].in_use = false;
    _clients[client_index].stall_ms = 0;
    _clients[client_index].tx_len = 0;   // discard any half-sent frame; tx_buf stays for slot reuse
    _clients[client_index].tx_sent = 0;
  }
}

// ============================================================================
// Web UI mirror
// ============================================================================
int WebSocketCompanionServer::mirrorClientCount() const {
  WsClientsLock _lk(_client_mtx);
  int n = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++)
    if (_clients[i].in_use && _clients[i].client.connected() &&
        _clients[i].handshake_done && _clients[i].is_mirror) n++;
  return n;
}

// Queue one binary WS frame (FIN+binary, len up to 65535 via the 126 extended header)
// into the client's tx_buf, then push what the socket accepts right now — all
// NON-BLOCKING. The frame is drained in full over later drainClientTx() calls, so the
// loop never spins on a write and a slow link never corrupts the stream or drops the
// client. Returns 0 (rejected) if a previous frame is still draining (caller waits) or
// the frame is too big to buffer. On success the whole frame is guaranteed atomic.
size_t WebSocketCompanionServer::writeBinaryFrame(int client_index, const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS || len == 0 || len > 0xFFFF) return 0;
  WSClientState* c = &_clients[client_index];
  if (!c->in_use || !c->client.connected()) return 0;
  if (c->tx_len != 0) return 0;                          // a frame is still draining -> not ready

  if (!c->tx_buf) {
    c->tx_buf = (uint8_t*)heap_caps_malloc(WS_MIRROR_CLIENT_TXBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->tx_buf) c->tx_buf = (uint8_t*)malloc(WS_MIRROR_CLIENT_TXBUF);
    if (!c->tx_buf) return 0;
  }
  const size_t hdr_len = (len < 126) ? 2 : 4;
  if (hdr_len + len > WS_MIRROR_CLIENT_TXBUF) return 0;  // never for our <=4 KB bands, but stay safe
  uint8_t* p = c->tx_buf;
  p[0] = 0x82;                                           // FIN + binary
  if (len < 126) { p[1] = (uint8_t)len; }
  else           { p[1] = 126; p[2] = (len >> 8) & 0xFF; p[3] = len & 0xFF; }
  memcpy(p + hdr_len, src, len);
  c->tx_len  = (uint16_t)(hdr_len + len);
  c->tx_sent = 0;
  drainClientTx(client_index);                          // push whatever fits now (non-blocking)
  return len;
}

// Push a mirror client's pending tx_buf bytes as far as the socket will take them right
// now, without ever blocking. Frame boundaries are byte-exact (tx_sent offset), so a
// partial push just resumes next call — the stream never desyncs. A client that can't
// take a single byte for WS_WEDGED_DROP_MS is genuinely dead and gets reaped.
void WebSocketCompanionServer::drainClientTx(int idx) {
  WSClientState* c = &_clients[idx];
  if (!c->in_use || c->tx_len == 0) return;
  if (!c->client.connected()) { c->tx_len = c->tx_sent = 0; return; }
  while (c->tx_sent < c->tx_len && socketWritableNow(c->client)) {
    size_t n = c->client.write(c->tx_buf + c->tx_sent, c->tx_len - c->tx_sent);
    if (n == 0) break;
    c->tx_sent += (uint16_t)n;
  }
  if (c->tx_sent >= c->tx_len) { c->tx_len = 0; c->tx_sent = 0; c->stall_ms = 0; return; }
  // Still bytes pending -> the socket is momentarily full. Just wait; only a peer that
  // stays unwritable for the full drop window is treated as wedged.
  if (c->stall_ms == 0) c->stall_ms = millis() | 1;
  else if (millis() - c->stall_ms >= WS_WEDGED_DROP_MS) disconnectClient(idx);
}

// Read masked WebSocket binary frames from a mirror client and dispatch pointer
// events. Reuses this client's ws_* parse state (pollRecvFrame skips mirror
// clients, so it is exclusively ours). Payload: [0x01, x_lo,x_hi, y_lo,y_hi, pressed].
void WebSocketCompanionServer::drainMirrorInput(int idx, WebMirror& m) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  int guard = 0;
  while (cl->available() && guard++ < 1024) {
    switch (c->ws_state) {
      case WS_STATE_HEADER_0: {
        uint8_t b = (uint8_t)cl->read();
        c->ws_opcode = b & 0x0F;
        c->ws_state = WS_STATE_HEADER_1;
        break;
      }
      case WS_STATE_HEADER_1: {
        uint8_t b = (uint8_t)cl->read();
        uint8_t l7 = b & 0x7F;
        c->ws_payload_read = 0;
        c->comp_rx_len = 0;
        if (l7 == 126 || l7 == 127) { c->ws_payload_len = l7; c->ws_state = WS_STATE_LEN_EXT; }
        else { c->ws_payload_len = l7; c->ws_state = WS_STATE_MASK; }
        break;
      }
      case WS_STATE_LEN_EXT: {
        if (c->ws_payload_len == 126) {
          if (cl->available() < 2) return;
          uint8_t hi = (uint8_t)cl->read(), lo = (uint8_t)cl->read();
          c->ws_payload_len = ((uint16_t)hi << 8) | lo;
        } else {
          if (cl->available() < 8) return;
          c->ws_payload_len = 0;
          for (int i = 0; i < 8; i++) c->ws_payload_len = (c->ws_payload_len << 8) | (uint8_t)cl->read();
        }
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_MASK;
        break;
      }
      case WS_STATE_MASK: {
        if (cl->available() < 4) return;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_PAYLOAD;
        break;
      }
      default: {  // WS_STATE_PAYLOAD
        uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
        if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len++] = b;
        c->ws_payload_read++;
        if (c->ws_payload_read >= c->ws_payload_len) {
          if (c->ws_opcode == 0x02 && c->comp_rx_len >= 1) {
            const uint8_t ty = c->comp_rx_buf[0];
            if (ty == 0x01 && c->comp_rx_len >= 6) {          // pointer: [0x01, x,y (LE16), pressed]
              int16_t x = (int16_t)(c->comp_rx_buf[1] | (c->comp_rx_buf[2] << 8));
              int16_t y = (int16_t)(c->comp_rx_buf[3] | (c->comp_rx_buf[4] << 8));
              m.pushPointer(x, y, c->comp_rx_buf[5]);
            } else if (ty == 0x02 && c->comp_rx_len >= 3) {   // key: [0x02, codepoint LE16]
              m.pushKey((uint16_t)(c->comp_rx_buf[1] | (c->comp_rx_buf[2] << 8)));
            } else if (ty == 0x04 && c->comp_rx_len >= 2) {   // orientation: [0x04, want_landscape]
              m.requestOrient(c->comp_rx_buf[1] ? 1 : 2);     // 1=landscape, 2=portrait; UI thread reboots into it
            }
          } else if (c->ws_opcode == 0x08) {   // client close
            disconnectClient(idx);
            return;
          }
          c->ws_state = WS_STATE_HEADER_0;
        }
        break;
      }
    }
  }
}

// Called every loop: refresh the client count for the producer gate, send each
// client its one-time screen-size meta, drain remote pointer input, and broadcast
// queued framebuffer bands. Strictly non-blocking (socketWritableNow-gated) so it
// never stalls the mesh loop (the beta_32 Wi-Fi-freeze discipline).
void WebSocketCompanionServer::serviceMirror(WebMirror& m) {
  WsClientsLock _lk(_client_mtx);          // held for the whole (quick, non-blocking) pass
  const int mc = mirrorClientCount();
  m.noteClients(mc);
  if (mc == 0) return;

  if (!_mirror_txbuf) {
    _mirror_txbuf = (uint8_t*)heap_caps_malloc(WS_MIRROR_TXBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_mirror_txbuf) _mirror_txbuf = (uint8_t*)malloc(WS_MIRROR_TXBUF);
    if (!_mirror_txbuf) return;
  }

  // Per-client: first push any bytes still pending from the previous frame (non-blocking),
  // then queue the one-time size meta + keyboard-focus changes (only while idle, so on-wire
  // frame order is preserved), then drain remote input.
  bool kf = false;
  const bool kb_changed = m.kbTake(&kf);
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror) continue;
    drainClientTx(i);
    if (!c->in_use) continue;                 // a wedged client may have just been reaped
    if (c->tx_len == 0 && !c->meta_sent) {
      uint8_t meta[6] = { 0x02,
        (uint8_t)(m.screenW() & 0xFF), (uint8_t)(m.screenW() >> 8),
        (uint8_t)(m.screenH() & 0xFF), (uint8_t)(m.screenH() >> 8),
        (uint8_t)(m.remote() ? 1 : 0) };   // byte5 bit0 = remote mode -> browser shows the Rotate button
      if (writeBinaryFrame(i, meta, 6) == 6) { c->meta_sent = true; m.requestFullRepaint(); }
    }
    if (kb_changed && c->tx_len == 0) { uint8_t km[2] = { 0x03, (uint8_t)(kf ? 1 : 0) }; writeBinaryFrame(i, km, 2); }
    drainMirrorInput(i, m);
  }

  // Broadcast queued display bands: pop the next band only once EVERY mirror client has
  // fully drained its previous frame (keeps all clients byte-synced + backpressures to the
  // slowest peer). writeBinaryFrame buffers + pushes non-blocking, so a slow link just
  // drains across later calls instead of blocking the loop or dropping the socket.
  size_t budget = WS_MIRROR_TX_BUDGET;
  while (budget > 0) {
    bool any = false, all_idle = true;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      WSClientState* c = &_clients[i];
      if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror) continue;
      any = true;
      if (c->tx_len != 0) { all_idle = false; break; }
    }
    if (!any || !all_idle) break;
    size_t n = m.popFrame(_mirror_txbuf, WS_MIRROR_TXBUF);
    if (n == 0) break;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      WSClientState* c = &_clients[i];
      if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror) continue;
      writeBinaryFrame(i, _mirror_txbuf, n);
    }
    budget = (n < budget) ? (budget - n) : 0;
  }
}
