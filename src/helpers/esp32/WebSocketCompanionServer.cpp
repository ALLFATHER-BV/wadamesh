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
#define WS_MIRROR_CLIENT_TXBUF 16384        // per-client outgoing frame buffer (mirror bands <=4 KB; terminal JSON data frames can be larger)

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
  "#xit{margin-top:6px;padding:7px 15px;font-size:12px;background:#1e1416;color:#e0a6a6;border:1px solid #4a2a2e;border-radius:8px;display:none}\n"
  "#xit:active{background:#ff6b6b;color:#000;border-color:#ff6b6b}\n"
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
  "<button id=xit>&#10005; Exit remote</button>\n"
  "<textarea id=k autocomplete=off autocorrect=off autocapitalize=off spellcheck=false></textarea>\n"
  "</div>\n"
  "<script>\n"
  "var C=document.getElementById('c'),X=C.getContext('2d'),S=document.getElementById('s');\n"
  "var OT=document.createElement('canvas'),OX=OT.getContext('2d');\n"
  "var DW=320,DH=240,down=false,last=0,ws;\n"
  "var isTouch=('ontouchstart' in window)||navigator.maxTouchPoints>0,kbT=null;\n"
  "var ROT=document.getElementById('rot'),XIT=document.getElementById('xit');\n"
  "function st(t,k){S.textContent=t;S.className=k||''}\n"
  "function conn(){\n"
  " ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/mirror');\n"
  " ws.binaryType='arraybuffer';\n"
  " ws.onopen=function(){st('connected','ok')};\n"
  " ws.onclose=function(){st('disconnected - retrying','err');setTimeout(conn,1500)};\n"
  " ws.onerror=function(){st('connection error','err')};\n"
  " ws.onmessage=function(e){\n"
  "  var a=new Uint8Array(e.data);\n"
  "  if(a[0]==2){DW=a[1]|(a[2]<<8);DH=a[3]|(a[4]<<8);if(C.width!=DW)C.width=DW;if(C.height!=DH)C.height=DH;var rem=(a.length>5&&(a[5]&1))?'':'none';ROT.style.display=rem;XIT.style.display=rem;fit();return}\n"
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
  "XIT.addEventListener('click',function(){if(!ws||ws.readyState!=1)return;if(!confirm('Leave remote mode? The device reboots to its normal screen.'))return;st('leaving remote - rebooting','');ws.send(new Uint8Array([5]))});\n"
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

// Self-contained web mesh terminal (served at / while web_terminal is on). Talks the
// device's runLocalCli over a /term WebSocket: type a command, get the reply streamed
// back. Command history (up/down), tab-completion, clear, styled monospace output.
static const char WS_HTML_TERMINAL_PAGE[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/html; charset=utf-8\r\n"
  "Connection: close\r\n"
  "\r\n"
  "<!DOCTYPE html><html><head><meta charset=utf-8>\n"
  "<meta name=viewport content='width=device-width,initial-scale=1,maximum-scale=1'>\n"
  "<title>wadamesh</title>\n"
  "<style>\n"
  "*{box-sizing:border-box}html,body{margin:0;height:100%;background:#0b0b0c;color:#d7dbdd;font-family:-apple-system,'Segoe UI',Roboto,sans-serif;font-size:14px;-webkit-text-size-adjust:100%}\n"
  "#w{display:flex;flex-direction:column;height:100%}\n"
  "#hd{padding:8px 10px;border-bottom:1px solid #1c1c1f;display:flex;align-items:center;gap:8px}#hdl{flex:1}\n"
  "#hd b{color:#19d6c2;font-weight:700;letter-spacing:2px}#st{font-size:12px}\n"
  "#tabs{display:flex;border-bottom:1px solid #1c1c1f}\n"
  "#tabs button{flex:1;background:none;border:none;color:#7f868c;padding:11px 4px;font:inherit;font-size:13px;border-bottom:2px solid transparent;cursor:pointer}\n"
  "#tabs button.on{color:#19d6c2;border-bottom-color:#19d6c2}\n"
  ".pane{flex:1;min-height:0;display:flex;flex-direction:column}\n"
  ".list{flex:1;overflow-y:auto}\n"
  ".row{display:flex;padding:11px 13px;border-bottom:1px solid #141416;cursor:pointer;gap:10px;align-items:center}\n"
  ".row:active{background:#141416}.rmain{flex:1;min-width:0}\n"
  ".rname{font-weight:600;color:#e8e8ea;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
  ".rsub{font-size:12px;color:#7f868c;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}\n"
  ".rtime{font-size:11px;color:#63696e;white-space:nowrap}\n"
  ".badge{background:#19d6c2;color:#04201d;border-radius:9px;padding:0 6px;font-size:11px;margin-left:6px;font-weight:700}\n"
  ".sec{padding:9px 13px 4px;font-size:11px;color:#63696e;letter-spacing:1px}\n"
  ".empty{padding:26px 13px;color:#63696e;text-align:center}\n"
  "#cview{flex:1;min-height:0;flex-direction:column}\n"
  "#cvh{display:flex;align-items:center;gap:6px;padding:8px 8px;border-bottom:1px solid #1c1c1f}\n"
  "#cvback{background:none;border:none;color:#19d6c2;font-size:24px;line-height:1;cursor:pointer;padding:0 6px}\n"
  "#cvname{font-weight:600}\n"
  "#cvmsgs{flex:1;overflow-y:auto;padding:10px;display:flex;flex-direction:column;gap:6px}\n"
  ".b{max-width:80%;padding:6px 9px;border-radius:11px;word-break:break-word}\n"
  ".b.in{align-self:flex-start;background:#17181b}.b.out{align-self:flex-end;background:#123f3a}\n"
  ".bname{font-size:11px;color:#19d6c2;margin-bottom:2px}.btext{white-space:pre-wrap}\n"
  ".bmeta{font-size:10px;color:#8a9095;text-align:right;margin-top:2px}\n"
  ".cb{display:flex;border-top:1px solid #1c1c1f;padding:8px;gap:8px}\n"
  ".cb input{flex:1;background:#111214;color:#e8e8ea;border:1px solid #2a2a2e;border-radius:18px;padding:9px 14px;font:inherit;outline:none}\n"
  ".cb input:focus{border-color:#19d6c2}\n"
  ".cb button{background:#19d6c2;color:#04201d;border:none;border-radius:18px;padding:0 16px;font-weight:700;cursor:pointer}\n"
  "#o{flex:1;overflow-y:auto;padding:10px 12px;white-space:pre-wrap;word-break:break-word;line-height:1.4;font-family:'JetBrains Mono',Menlo,monospace;font-size:13px}\n"
  "#tbar{display:flex;border-top:1px solid #1c1c1f;padding:8px 10px;gap:8px;align-items:center}\n"
  "#p{color:#19d6c2;font-weight:700}\n"
  "#i{flex:1;background:#111214;color:#e8e8ea;border:1px solid #2a2a2e;border-radius:6px;padding:8px 10px;font-family:'JetBrains Mono',Menlo,monospace;font-size:13px;outline:none}\n"
  "#i:focus{border-color:#19d6c2}\n"
  ".e{color:#7f868c}.c{color:#e8b84b}.ok{color:#19d6c2}.err{color:#ff6b6b}\n"
  "#hdl{display:flex;align-items:center;gap:6px;min-width:0}#hnm{color:#9aa0a6;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
  "#dot{font-size:11px;color:#63696e}#hdr{display:flex;align-items:center;gap:8px;font-size:12px;color:#9aa0a6;white-space:nowrap}\n"
  "#hdr svg{vertical-align:middle;display:block}#hdr>span{display:inline-flex;align-items:center}.bpct{font-size:11px;margin-left:3px;color:#9aa0a6}\n"
  ".bars{letter-spacing:-1px;color:#19d6c2}.bars i{color:#3a3f44;font-style:normal}\n"
  ".bgrn{color:#19d6c2}.byel{color:#e8b84b}.bred{color:#ff6b6b}\n"
  "#ctools{display:flex;gap:6px;padding:7px 8px;border-bottom:1px solid #1c1c1f}\n"
  "#csearch{flex:1;background:#111214;color:#e8e8ea;border:1px solid #2a2a2e;border-radius:14px;padding:6px 12px;font:inherit;font-size:13px;outline:none;min-width:0}\n"
  "#ctools button{background:#17181b;color:#cfd3d6;border:1px solid #2a2a2e;border-radius:14px;padding:0 10px;font:inherit;font-size:12px;white-space:nowrap;cursor:pointer}\n"
  ".tag{font-size:10px;padding:1px 5px;border-radius:6px;margin-left:6px;vertical-align:middle}.trep{background:#243b52;color:#7fc0ff}.troom{background:#3b2a52;color:#c69cff}.tfav{color:#e8b84b}\n"
  "#scrim{position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:19;display:none}#scrim.on{display:block}\n"
  "#sheet{position:fixed;left:0;right:0;bottom:0;background:#151619;border-top:1px solid #2a2a2e;border-radius:14px 14px 0 0;transform:translateY(101%);transition:transform .15s;z-index:20;padding:6px 0 max(10px,env(safe-area-inset-bottom));max-height:74vh;overflow-y:auto}\n"
  "#qcmd{background:#17181b;color:#19d6c2;border:1px solid #2a2a2e;border-radius:6px;padding:0 11px;font-size:16px;line-height:34px;cursor:pointer}\n"
  "#sheet.on{transform:translateY(0)}#sheet h4{margin:8px 16px 4px;font-size:13px;color:#9aa0a6}\n"
  ".si{display:block;width:100%;text-align:left;background:none;border:none;color:#d7dbdd;padding:13px 16px;font:inherit;font-size:15px;cursor:pointer;border-top:1px solid #202225}.si:active{background:#1d1f22}.si.dng{color:#ff6b6b}\n"
  "#modal{position:fixed;inset:0;display:none;align-items:center;justify-content:center;z-index:21;padding:20px}#modal.on{display:flex}\n"
  "#mbox{background:#151619;border:1px solid #2a2a2e;border-radius:12px;max-width:340px;width:100%;padding:16px;max-height:82%;overflow:auto}#mbox h3{margin:0 0 10px;color:#19d6c2;font-size:15px;word-break:break-word}\n"
  "#mbox .kv{display:flex;justify-content:space-between;gap:12px;padding:5px 0;font-size:13px;border-bottom:1px solid #202225}#mbox .kv b{color:#9aa0a6;font-weight:400}#mbox .kv span{text-align:right;word-break:break-word}\n"
  "#mclose{margin-top:12px;width:100%;background:#19d6c2;color:#04201d;border:none;border-radius:8px;padding:10px;font-weight:700;cursor:pointer}\n"
  "#gear{background:none;border:none;color:#9aa0a6;font-size:17px;cursor:pointer;padding:0 2px;line-height:1}\n"
  ".sset label{display:block;font-size:11px;color:#63696e;letter-spacing:1px;margin:12px 0 4px}\n"
  ".sset input[type=text],.sset input[type=number]{background:#111214;color:#e8e8ea;border:1px solid #2a2a2e;border-radius:6px;padding:7px 9px;font:inherit;outline:none;width:100%}\n"
  ".srow{display:flex;gap:8px;align-items:center;margin:4px 0}.srow input{flex:1;min-width:0}\n"
  ".sset .sbtn{background:#19d6c2;color:#04201d;border:none;border-radius:6px;padding:0 12px;font-weight:700;line-height:32px;cursor:pointer;white-space:nowrap}\n"
  ".sgrid{display:grid;grid-template-columns:1fr 1fr;gap:6px}.sgrid .f label{margin:2px 0}.sgrid .f{min-width:0}\n"
  ".stog{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid #202225}.stog b{font-weight:400;color:#d7dbdd}\n"
  ".sw{position:relative;width:40px;height:22px;background:#2a2a2e;border-radius:11px;cursor:pointer;transition:background .15s;flex:none}.sw.on{background:#19d6c2}.sw i{position:absolute;top:2px;left:2px;width:18px;height:18px;border-radius:50%;background:#e8e8ea;transition:left .15s}.sw.on i{left:20px}\n"
  ".srow2{display:flex;gap:8px;margin-top:14px}.srow2 button{flex:1;background:#17181b;color:#cfd3d6;border:1px solid #2a2a2e;border-radius:8px;padding:10px;font:inherit;cursor:pointer}.srow2 button.dng{color:#ff6b6b;border-color:#5a2a2a}\n"
  "#toast{position:fixed;left:8px;right:8px;top:8px;background:#17342f;border:1px solid #19d6c2;border-radius:10px;padding:9px 12px;z-index:30;box-shadow:0 4px 16px rgba(0,0,0,.45);transform:translateY(-150%);transition:transform .2s;cursor:pointer}\n"
  "#toast.on{transform:translateY(0)}#toast b{color:#19d6c2;display:block;font-size:13px;margin-bottom:1px}\n"
  "#toast span{font-size:12px;color:#cfeee9;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;display:block}\n"
  "</style></head><body>\n"
  "<div id=w>\n"
  "<div id=hd><div id=hdl><span id=dot>&bull;</span><b>WADAMESH</b><span id=hnm></span></div>\n"
  " <div id=hdr><span id=hsig></span><span id=hwifi></span><span id=hble></span><span id=hbatt></span><span id=hclk></span></div><button id=gear title=Settings>&#9881;</button></div>\n"
  "<div id=tabs><button data-t=chats class=on>Chats</button><button data-t=contacts>Contacts</button><button data-t=term>Terminal</button></div>\n"
  "<div id=pchats class=pane>\n"
  " <div id=tlist class=list></div>\n"
  " <div id=cview style=display:none>\n"
  "  <div id=cvh><button id=cvback>&lsaquo;</button><span id=cvname></span></div>\n"
  "  <div id=cvmsgs></div>\n"
  "  <div class=cb><input id=cvi placeholder='Message' autocomplete=off><button id=cvsend>Send</button></div>\n"
  " </div>\n"
  "</div>\n"
  "<div id=pcontacts class=pane style=display:none>\n"
  " <div id=ctools><input id=csearch placeholder='Search' autocomplete=off autocapitalize=off spellcheck=false><button id=csort>Sort</button><button id=cfilt>All</button></div>\n"
  " <div id=clist class=list></div>\n"
  "</div>\n"
  "<div id=pterm class=pane style=display:none>\n"
  " <div id=o></div>\n"
  " <div id=tbar><span id=p>&gt;</span><input id=i autocomplete=off autocorrect=off autocapitalize=off spellcheck=false placeholder='type a command - e.g. help'><button id=qcmd title='Commands'>&#9889;</button></div>\n"
  "</div>\n"
  "</div>\n"
  "<div id=scrim></div>\n"
  "<div id=sheet></div>\n"
  "<div id=modal><div id=mbox></div></div>\n"
  "<div id=toast></div>\n"
  "<script>\n"
  "var WS,dec=new TextDecoder(),curTab='chats',chat=null,threads=[],contacts=[],channels=[];\n"
  "var csort=0,cfilter=0,csearchq='',self={la:0,lo:0},lpAt=0,useMi=0,discCount=0;\n"
  "var SORTL=['Recent','A-Z','Distance'],FILTL=['All','Repeaters','Peers','Favorites','Located','Direct'];\n"
  "function age(ts){if(!ts||ts<1000000000)return'';var s=Math.floor(Date.now()/1000)-ts;if(s<0)s=0;if(s<60)return'now';if(s<3600)return Math.floor(s/60)+'m';if(s<86400)return Math.floor(s/3600)+'h';if(s<604800)return Math.floor(s/86400)+'d';return Math.floor(s/604800)+'w'}\n"
  "function fmtDist(km){if(km>=1e17)return'';if(useMi){var mi=km*0.621371;return (mi<10?mi.toFixed(1):Math.round(mi))+' mi'}return (km<10?km.toFixed(1):Math.round(km))+' km'}\n"
  "function E(i){return document.getElementById(i)}\n"
  "function each(l,f){Array.prototype.forEach.call(l,f)}\n"
  "function esc(s){return (s==null?'':''+s).replace(/[&<>\"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'})[c]})}\n"
  "function fmt(t){if(!t||t<1000000000)return'';var d=new Date(t*1000);return('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}\n"
  "function fmtDate(t){return (t>1000000000)?new Date(t*1000).toLocaleString():'--'}\n"
  "function stat(t,k){var el=E('dot');el.style.color=(k=='ok')?'#19d6c2':((k=='err')?'#ff6b6b':'#63696e');el.title=t}\n"
  "function wsSend(s){if(WS&&WS.readyState==1){WS.send(s);return true}return false}\n"
  "function cById(i){for(var k=0;k<contacts.length;k++)if(contacts[k].i==i)return contacts[k];return null}\n"
  "function showTab(t){curTab=t;['chats','contacts','term'].forEach(function(x){E('p'+x).style.display=(x==t)?'':'none'});\n"
  " each(document.querySelectorAll('#tabs button'),function(b){b.className=(b.getAttribute('data-t')==t)?'on':''});\n"
  " if(t=='chats'){if(!chat){closeChat();wsSend('@t')}}else if(t=='contacts'){wsSend('@c')}else if(t=='term'){E('i').focus()}}\n"
  "function svgSig(l){var b='';for(var i=0;i<4;i++){var h=3+i*3;b+='<rect x='+(i*4)+' y='+(12-h)+' width=3 height='+h+' rx=1 fill='+(i<l?'#19d6c2':'#3a3f44')+' />'}return '<svg width=15 height=12 viewBox=\"0 0 15 12\">'+b+'</svg>'}\n"
  "function svgWifi(on){var c=on?'#19d6c2':'#3a3f44';return '<svg width=16 height=12 viewBox=\"0 0 16 12\" fill=none stroke='+c+' stroke-width=1.5 stroke-linecap=round><path d=\"M2.2 4.6a8 8 0 0 1 11.6 0\" /><path d=\"M4.6 7.1a4.6 4.6 0 0 1 6.8 0\" /><circle cx=8 cy=9.6 r=0.9 fill='+c+' stroke=none /></svg>'}\n"
  "function svgBt(){return '<svg width=10 height=14 viewBox=\"0 0 10 14\" fill=none stroke=#7fb0ff stroke-width=1.3 stroke-linejoin=round stroke-linecap=round><path d=\"M2 4l6 6-3 2.5V1.5l3 2.5-6 6\" /></svg>'}\n"
  "function svgBatt(p,chg){var col=p<=20?'#ff6b6b':(p<=50?'#e8b84b':'#19d6c2');var w=Math.max(1,Math.round(p/100*16));var bolt=chg?'<path d=\"M12 3l-3 4h2l-1 3 3-4h-2z\" fill=#e8b84b />':'';return '<svg width=27 height=13 viewBox=\"0 0 27 13\"><rect x=0.5 y=2 width=21 height=9 rx=2 fill=none stroke=#8a9095 /><rect x=22 y=4.5 width=2.2 height=4 rx=1 fill=#8a9095 /><rect x=2 y=3.5 width='+w+' height=6 rx=1 fill='+col+' />'+bolt+'</svg>'}\n"
  "function renderStatus(d){E('hnm').textContent=d.nm||'';self.la=d.sla||0;self.lo=d.slo||0;useMi=d.mi?1:0;\n"
  " var lvl=(d.snr<=-127)?0:(d.snr>=5?4:(d.snr>=0?3:(d.snr>=-7?2:1)));\n"
  " E('hsig').innerHTML=svgSig(lvl);E('hwifi').innerHTML=d.wifi?svgWifi(1):'';E('hble').innerHTML=d.ble?svgBt():'';\n"
  " E('hbatt').innerHTML=(d.batt<0)?'':(svgBatt(d.batt,d.chg)+'<span class=bpct>'+d.batt+'%</span>');\n"
  " E('hclk').textContent=fmt(d.clk);\n"
  " var cb=document.querySelector('#tabs [data-t=chats]');if(cb)cb.textContent=d.unr>0?('Chats ('+d.unr+')'):'Chats'}\n"
  "function closeOv(){E('sheet').classList.remove('on');E('modal').classList.remove('on');E('scrim').classList.remove('on')}\n"
  "function sheet(title,items){var h='<h4>'+esc(title)+'</h4>';items.forEach(function(it,i){h+='<button class=\"si '+(it.cls||'')+'\" data-i='+i+'>'+esc(it.label)+'</button>'});\n"
  " E('sheet').innerHTML=h;each(E('sheet').querySelectorAll('.si'),function(b){b.onclick=function(){var it=items[+b.getAttribute('data-i')];closeOv();if(it.fn)it.fn()}});\n"
  " E('sheet').classList.add('on');E('scrim').classList.add('on')}\n"
  "function modal(title,html){E('mbox').innerHTML='<h3>'+esc(title)+'</h3>'+html+'<button id=mclose>Close</button>';E('modal').classList.add('on');E('scrim').classList.add('on');E('mclose').onclick=closeOv}\n"
  "function copy(t){try{navigator.clipboard.writeText(t)}catch(_){var a=document.createElement('textarea');a.value=t;document.body.appendChild(a);a.select();try{document.execCommand('copy')}catch(e){}a.remove()}}\n"
  "function longPress(el,fn){var tmr=null;function s(){tmr=setTimeout(function(){tmr=null;lpAt=Date.now();fn()},480)}function c(){if(tmr){clearTimeout(tmr);tmr=null}}\n"
  " el.addEventListener('touchstart',s,{passive:true});el.addEventListener('touchmove',c,{passive:true});el.addEventListener('touchend',c);\n"
  " el.addEventListener('mousedown',s);el.addEventListener('mouseup',c);el.addEventListener('mouseleave',c);el.addEventListener('contextmenu',function(e){e.preventDefault();lpAt=Date.now();fn()})}\n"
  "function clicked(fn){return function(){if(Date.now()-lpAt<650)return;fn()}}\n"
  "function contactSheet(c){sheet(c.n||'Contact',[\n"
  " {label:'Message',fn:function(){if(c.t>=0)wsSend('@m '+c.t);else wsSend('@oc '+c.i)}},\n"
  " {label:'Info',fn:function(){wsSend('@ic '+c.i)}},\n"
  " {label:c.fav?'Unfavorite':'Favorite',fn:function(){wsSend('@fv '+c.i)}},\n"
  " {label:'Reset path',fn:function(){wsSend('@pr '+c.i)}},\n"
  " {label:c.ign?'Unblock':'Block',fn:function(){wsSend('@bk '+c.i)}},\n"
  " {label:'Delete',cls:'dng',fn:function(){if(confirm('Delete '+(c.n||'contact')+'?'))wsSend('@xc '+c.i)}}])}\n"
  "function chatSheet(t){sheet(t.n||'Chat',[\n"
  " {label:'Mark as read',fn:function(){wsSend('@r '+t.i)}},\n"
  " {label:'Clear history',fn:function(){if(confirm('Clear history?'))wsSend('@d '+t.i)}},\n"
  " {label:'Delete chat',cls:'dng',fn:function(){if(confirm('Delete this chat?'))wsSend('@xt '+t.i)}}])}\n"
  "function msgSheet(m){sheet('Message',[{label:'Copy',fn:function(){copy(m.x)}},{label:'Info',fn:function(){msgInfo(m)}}])}\n"
  "function msgInfo(m){var pl=(m.pl==255)?'Direct':(m.pl+' hop'+(m.pl==1?'':'s'));\n"
  " var h='<div class=kv><b>Direction</b><span>'+(m.o?'Sent':'Received')+'</span></div><div class=kv><b>Time</b><span>'+(fmt(m.ts)||'--')+'</span></div><div class=kv><b>Path</b><span>'+pl+'</span></div>';\n"
  " if(!m.o)h+='<div class=kv><b>SNR</b><span>'+m.sn+' dB</span></div><div class=kv><b>RSSI</b><span>'+m.rs+' dBm</span></div>';\n"
  " if(m.o)h+='<div class=kv><b>Delivery</b><span>'+(({0:'--',1:'Sent',2:'Delivered',3:'Failed'})[m.ds]||'--')+'</span></div>';\n"
  " if(m.o&&m.rp>0)h+='<div class=kv><b>Repeats heard</b><span>'+m.rp+'</span></div>';\n"
  " modal('Message info',h)}\n"
  "function showContactInfo(d){var type=d.rp?'Repeater':(d.rm?'Room server':'Contact');var pl=(d.pl<0)?'Unknown':(d.pl==0?'Direct':(d.pl+' hop'+(d.pl==1?'':'s')));\n"
  " var h='<div class=kv><b>Type</b><span>'+type+'</span></div><div class=kv><b>Path</b><span>'+pl+'</span></div><div class=kv><b>Favorite</b><span>'+(d.fav?'Yes':'No')+'</span></div><div class=kv><b>Blocked</b><span>'+(d.ign?'Yes':'No')+'</span></div>';\n"
  " if(d.adv>1000000000)h+='<div class=kv><b>Last heard</b><span>'+fmtDate(d.adv)+'</span></div>';\n"
  " if(d.la||d.lo){h+='<div class=kv><b>Location</b><span>'+(d.la/1e6).toFixed(5)+', '+(d.lo/1e6).toFixed(5)+'</span></div>';var ds=fmtDist(cdist(d));if(ds)h+='<div class=kv><b>Distance</b><span>'+ds+'</span></div>'}\n"
  " h+='<div class=kv><b>ID</b><span>'+esc(d.k)+'</span></div>';modal(d.n||'Contact',h)}\n"
  "var prevUnread={},toastTmr=null,AC=null;\n"
  "function onThreads(nt){\n"
  " nt.forEach(function(t){var pu=prevUnread[t.i];if(pu!=null&&t.u>pu&&!(chat&&chat.i==t.i))notify(t.i,(t.ch?'# ':'')+t.n,t.last||'New message')});\n"
  " prevUnread={};nt.forEach(function(t){prevUnread[t.i]=t.u});threads=nt;renderThreads()}\n"
  "function notify(ti,title,body){showToast(ti,title,body);\n"
  " try{if('Notification' in window&&Notification.permission=='granted'&&document.hidden)new Notification(title,{body:body})}catch(e){}\n"
  " try{beep()}catch(e){}}\n"
  "function showToast(ti,title,body){var t=E('toast');t.innerHTML='<b></b><span></span>';t.querySelector('b').textContent=title;t.querySelector('span').textContent=body;\n"
  " t.onclick=function(){t.classList.remove('on');wsSend('@m '+ti)};t.classList.add('on');\n"
  " if(toastTmr)clearTimeout(toastTmr);toastTmr=setTimeout(function(){t.classList.remove('on')},4200)}\n"
  "function beep(){AC=AC||new (window.AudioContext||window.webkitAudioContext)();var o=AC.createOscillator(),g=AC.createGain();o.connect(g);g.connect(AC.destination);o.frequency.value=880;g.gain.value=0.05;o.start();g.gain.exponentialRampToValueAtTime(0.0001,AC.currentTime+0.18);o.stop(AC.currentTime+0.2)}\n"
  "document.addEventListener('click',function(){try{if('Notification' in window&&Notification.permission=='default')Notification.requestPermission()}catch(e){}},{once:true});\n"
  "var QCMDS=[{h:'CHAT'},{l:'list - contacts',c:'list'},{l:'channels',c:'channels'},{l:'to <name>',c:'to '},{l:'send <text>',c:'send '},{l:'public <text>',c:'public '},{l:'exit',c:'exit'},\n"
  " {h:'INFO'},{l:'help',c:'help'},{l:'ver - firmware version',c:'ver'},{l:'clock - RTC time',c:'clock'},{l:'status',c:'status'},{l:'get - radio params',c:'get'},\n"
  " {h:'RADIO / MESH'},{l:'advert',c:'advert'},{l:'advert.zerohop',c:'advert.zerohop'},{l:'set name <v>',c:'set name '},{l:'set freq <MHz>',c:'set freq '},{l:'set bw <kHz>',c:'set bw '},{l:'set sf <7-12>',c:'set sf '},{l:'set cr <5-8>',c:'set cr '},{l:'set tx <dBm>',c:'set tx '},\n"
  " {h:'CONNECTIVITY'},{l:'wifi status',c:'wifi status'},{l:'wifi on',c:'wifi on'},{l:'wifi off',c:'wifi off'},{l:'wifi scan',c:'wifi scan'},{l:'wifi apply',c:'wifi apply'},{l:'wifi clear',c:'wifi clear'},{l:'tcp status',c:'tcp status'},{l:'tcp on',c:'tcp on'},{l:'tcp off',c:'tcp off'},{l:'ble status',c:'ble status'},{l:'ble on',c:'ble on'},{l:'ble off',c:'ble off'},{l:'ota status',c:'ota status'},{l:'ota start',c:'ota start'},\n"
  " {h:'SYSTEM'},{l:'reboot',c:'reboot'},{l:'bootloader - download mode',c:'bootloader'}];\n"
  "function cmdPicker(){var h='<h4>Commands</h4>';QCMDS.forEach(function(x,i){if(x.h)h+='<div class=sec>'+esc(x.h)+'</div>';else h+='<button class=si data-i='+i+'>'+esc(x.l)+'</button>'});\n"
  " E('sheet').innerHTML=h;each(E('sheet').querySelectorAll('.si'),function(b){b.onclick=function(){var x=QCMDS[+b.getAttribute('data-i')];closeOv();showTab('term');E('i').value=x.c;E('i').focus()}});\n"
  " E('sheet').classList.add('on');E('scrim').classList.add('on')}\n"
  "var hist=[],hi=0,CMDS=['help','?','status','ver','clock','get','advert','advert.zerohop','reboot','bootloader','set ','tcp ','ble ','ota ','wifi ','list','contacts','channels','to ','send ','public ','exit','leave','ok','cancel','clear'];\n"
  "function tadd(t,cls){var s=document.createElement('span');if(cls)s.className=cls;s.textContent=t;E('o').appendChild(s);E('o').scrollTop=E('o').scrollHeight}\n"
  "function tsend(){var v=E('i').value;E('i').value='';if(!v.trim())return;if(v.trim()=='clear'){E('o').innerHTML='';return}\n"
  " tadd('\\n> '+v+'\\n','c');hist.push(v);hi=hist.length;if(!wsSend(v))tadd('(not connected)\\n','err')}\n"
  "function onData(s){var d;try{d=JSON.parse(s)}catch(_){return}\n"
  " if(d.t=='t'){onThreads(d.threads||[])}\n"
  " else if(d.t=='c'){contacts=d.contacts||[];channels=d.channels||[];discCount=d.dc||0;renderContacts()}\n"
  " else if(d.t=='m'){openChatView(d)}\n"
  " else if(d.t=='st'){renderStatus(d)}\n"
  " else if(d.t=='ci'){showContactInfo(d)}\n"
  " else if(d.t=='dc'){showDiscovered(d.nodes||[])}\n"
  " else if(d.t=='sg'){showSettings(d)}\n"
  " else if(d.t=='rx'){wsSend('@t');if(chat&&chat.i>=0)wsSend('@m '+chat.i);if(curTab=='contacts')wsSend('@c')}}\n"
  "function renderThreads(){var h='';if(!threads.length)h='<div class=empty>No chats yet.<br>Start one from Contacts.</div>';\n"
  " threads.sort(function(a,b){return (b.ts||0)-(a.ts||0)});\n"
  " threads.forEach(function(t){var bd=t.u>0?'<span class=badge>'+t.u+'</span>':'';\n"
  "  h+='<div class=row data-ti='+t.i+'><div class=rmain><div class=rname>'+(t.ch?'# ':'')+esc(t.n)+bd+'</div><div class=rsub>'+esc(t.last||'')+'</div></div><div class=rtime>'+fmt(t.ts)+'</div></div>'});\n"
  " E('tlist').innerHTML=h;each(E('tlist').querySelectorAll('.row'),function(r){var ti=+r.getAttribute('data-ti');\n"
  "  r.onclick=clicked(function(){wsSend('@m '+ti)});var t=null;for(var k=0;k<threads.length;k++)if(threads[k].i==ti)t=threads[k];longPress(r,function(){if(t)chatSheet(t)})})}\n"
  "function cdist(c){if(!(c.la||c.lo)||!(self.la||self.lo))return 1e18;var R=6371,rad=Math.PI/180,dLa=(c.la-self.la)/1e6*rad,dLo=(c.lo-self.lo)/1e6*rad,l1=self.la/1e6*rad,l2=c.la/1e6*rad;\n"
  " var a=Math.sin(dLa/2)*Math.sin(dLa/2)+Math.cos(l1)*Math.cos(l2)*Math.sin(dLo/2)*Math.sin(dLo/2);return R*2*Math.atan2(Math.sqrt(a),Math.sqrt(1-a))}\n"
  "function cVis(c){if(csearchq&&(c.n||'').toLowerCase().indexOf(csearchq)<0)return false;\n"
  " if(cfilter==1)return c.rp;if(cfilter==2)return !c.rp;if(cfilter==3)return c.fav;if(cfilter==4)return !!(c.la||c.lo);if(cfilter==5)return c.dir;return true}\n"
  "function renderContacts(){E('csort').textContent='Sort: '+SORTL[csort];E('cfilt').textContent=FILTL[cfilter];var h='';\n"
  " if(cfilter==0&&!csearchq){h+='<div class=row id=discrow><div class=rmain><div class=rname>Discovered nodes'+(discCount?' <span class=badge>'+discCount+'</span>':'')+'</div><div class=rsub>heard nearby - tap to add</div></div><div class=rtime>\\u203a</div></div>'}\n"
  " if(cfilter==0&&!csearchq&&channels.length){h+='<div class=sec>CHANNELS</div>';channels.forEach(function(c){h+='<div class=row data-h='+c.i+' data-t='+c.t+'><div class=rmain><div class=rname># '+esc(c.n)+'</div></div></div>'})}\n"
  " var cs=contacts.filter(cVis);cs.sort(function(a,b){if(csort==1)return (a.n||'').localeCompare(b.n||'');if(csort==2)return cdist(a)-cdist(b);return (b.adv||0)-(a.adv||0)});\n"
  " h+='<div class=sec>CONTACTS'+(cs.length?' ('+cs.length+')':'')+'</div>';if(!cs.length)h+='<div class=empty>No contacts.</div>';\n"
  " cs.forEach(function(c){var tag=c.rp?'<span class=\"tag trep\">RPT</span>':(c.rm?'<span class=\"tag troom\">ROOM</span>':'');var fav=c.fav?'<span class=\"tag tfav\">\\u2605</span>':'';var blk=c.ign?'<span class=tag style=color:#ff6b6b>\\u2298</span>':'';\n"
  "  var sub=[fmtDist(cdist(c)),age(c.adv)].filter(Boolean).join(' \\u00b7 ');if(c.dir)sub=sub?(sub+' \\u00b7 direct'):'direct';\n"
  "  h+='<div class=row data-c='+c.i+' data-t='+c.t+'><div class=rmain><div class=rname>'+esc(c.n)+tag+fav+blk+'</div>'+(sub?'<div class=rsub>'+sub+'</div>':'')+'</div></div>'});\n"
  " E('clist').innerHTML=h;var dr=E('discrow');if(dr)dr.onclick=function(){wsSend('@dc')};\n"
  " each(E('clist').querySelectorAll('.row'),function(r){\n"
  "  if(r.hasAttribute('data-c')){var ci=+r.getAttribute('data-c');var open=function(){var c=cById(ci);if(c)contactSheet(c)};r.onclick=clicked(open);longPress(r,open)}\n"
  "  else if(r.hasAttribute('data-h')){r.onclick=clicked(function(){var t=+r.getAttribute('data-t');if(t>=0)wsSend('@m '+t);else wsSend('@oh '+r.getAttribute('data-h'))})}})}\n"
  "function showDiscovered(nodes){if(!nodes.length){sheet('Discovered nodes',[{label:'No new nodes heard yet'}]);return}\n"
  " var items=nodes.map(function(nd){var tag=nd.rp?' [RPT]':(nd.rm?' [ROOM]':'');var meta=[fmtDist(cdist(nd)),age(nd.adv)].filter(Boolean).join(' \\u00b7 ');\n"
  "  return {label:(nd.n||'(unnamed)')+tag+(meta?'  ('+meta+')':''),fn:function(){wsSend('@ad '+nd.i);showToast(-1,'Added contact',nd.n||'')}}});\n"
  " sheet('Discovered - tap to add',items)}\n"
  "function showSettings(d){var g=E;\n"
  " var h='<div class=sset><label>NODE NAME</label><div class=srow><input type=text id=sfname><button class=sbtn id=sbname>Save</button></div>';\n"
  " h+='<label>RADIO (applies live)</label><div class=sgrid>';\n"
  " h+='<div class=f><label>Freq MHz</label><input type=number step=0.001 id=sffreq></div><div class=f><label>BW kHz</label><input type=number id=sfbw></div>';\n"
  " h+='<div class=f><label>SF</label><input type=number id=sfsf></div><div class=f><label>CR</label><input type=number id=sfcr></div>';\n"
  " h+='<div class=f><label>TX dBm</label><input type=number id=sftx></div><div class=f><label>&nbsp;</label><button class=sbtn id=sbradio style=width:100%>Apply</button></div></div>';\n"
  " h+='<label>CONNECTIVITY</label><div class=stog><b>Wi-Fi</b><div class=sw id=swifi><i></i></div></div>';\n"
  " if(d.blecap)h+='<div class=stog><b>Bluetooth</b><div class=sw id=sble><i></i></div></div>';\n"
  " h+='<div class=stog><b>TCP (phone app)</b><div class=sw id=stcp><i></i></div></div>';\n"
  " h+='<div class=srow2><button id=sadvert>Send advert</button><button id=sreboot class=dng>Reboot</button></div></div>';\n"
  " E('mbox').innerHTML='<h3>Settings</h3>'+h+'<button id=mclose>Close</button>';\n"
  " g('sfname').value=d.name||'';g('sffreq').value=d.freq;g('sfbw').value=d.bw;g('sfsf').value=d.sf;g('sfcr').value=d.cr;g('sftx').value=d.tx;\n"
  " function tg(id,on){var el=g(id);if(el){on?el.classList.add('on'):el.classList.remove('on')}}\n"
  " tg('swifi',d.wifi);tg('sble',d.ble);tg('stcp',d.tcp);\n"
  " g('sbname').onclick=function(){var v=g('sfname').value.trim();if(v){wsSend('set name '+v);showToast(-1,'Name saved',v)}};\n"
  " g('sbradio').onclick=function(){wsSend('@sr '+(+g('sffreq').value)+' '+(+g('sfbw').value)+' '+(+g('sfsf').value)+' '+(+g('sfcr').value)+' '+(+g('sftx').value));showToast(-1,'Radio applied','')};\n"
  " g('swifi').onclick=function(){var on=!g('swifi').classList.contains('on');tg('swifi',on);wsSend('wifi '+(on?'on':'off'))};\n"
  " if(g('sble'))g('sble').onclick=function(){var on=!g('sble').classList.contains('on');tg('sble',on);wsSend('ble '+(on?'on':'off'))};\n"
  " g('stcp').onclick=function(){var on=!g('stcp').classList.contains('on');tg('stcp',on);wsSend('tcp '+(on?'on':'off'))};\n"
  " g('sadvert').onclick=function(){wsSend('advert');showToast(-1,'Advert sent','')};\n"
  " g('sreboot').onclick=function(){if(confirm('Reboot the device now?'))wsSend('reboot')};\n"
  " g('mclose').onclick=closeOv;E('modal').classList.add('on');E('scrim').classList.add('on')}\n"
  "function openChatView(d){var box=E('cvmsgs');var wasBottom=(box.scrollHeight-box.scrollTop-box.clientHeight)<44;var same=chat&&d.i!=null&&d.i>=0&&chat.i==d.i;\n"
  " chat={i:(d.i!=null?d.i:-1),ch:!!d.ch,n:d.n||'',c:(d.c!=null?d.c:null),h:(d.h!=null?d.h:null)};\n"
  " showTab('chats');E('tlist').style.display='none';E('cview').style.display='flex';E('cvname').textContent=(chat.ch?'# ':'')+chat.n;\n"
  " var arr=(d.msgs||[]).slice().reverse();var h='';arr.forEach(function(m,i){var cls=m.o?'b out':'b in';var meta=fmt(m.ts);\n"
  "  if(m.o){meta+=' '+(({1:'\\u2713',2:'\\u2713\\u2713',3:'\\u2717'})[m.ds]||'');if(m.rp>0)meta+=' \\u21bb'+m.rp}\n"
  "  var nm=(!m.o&&chat.ch)?'<div class=bname>'+esc(m.s)+'</div>':'';\n"
  "  h+='<div class=\"'+cls+'\" data-mi='+i+'>'+nm+'<div class=btext>'+esc(m.x)+'</div><div class=bmeta>'+meta+'</div></div>'});\n"
  " E('cvmsgs').innerHTML=h;if(!same||wasBottom)E('cvmsgs').scrollTop=E('cvmsgs').scrollHeight;\n"
  " each(E('cvmsgs').querySelectorAll('.b'),function(b){var m=arr[+b.getAttribute('data-mi')];longPress(b,function(){if(m)msgSheet(m)})})}\n"
  "function closeChat(){chat=null;E('cview').style.display='none';E('tlist').style.display=''}\n"
  "function chatSend(){var v=E('cvi').value.trim();if(!v||!chat)return;E('cvi').value='';\n"
  " if(chat.i>=0)wsSend('@s '+chat.i+' '+v);else if(chat.c!=null)wsSend('@sc '+chat.c+' '+v);else if(chat.h!=null)wsSend('@sh '+chat.h+' '+v)}\n"
  "function conn(){WS=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/term');WS.binaryType='arraybuffer';\n"
  " WS.onopen=function(){stat('online','ok');wsSend('@st');if(curTab=='chats')wsSend('@t')};\n"
  " WS.onclose=function(){stat('reconnecting...','err');setTimeout(conn,1500)};WS.onerror=function(){stat('error','err')};\n"
  " WS.onmessage=function(e){var b=new Uint8Array(e.data);if(!b.length)return;var ty=b[0],p=dec.decode(b.subarray(1));if(ty==0)tadd(p);else if(ty==1)onData(p)}}\n"
  "each(document.querySelectorAll('#tabs button'),function(b){b.onclick=function(){showTab(b.getAttribute('data-t'))}});\n"
  "E('scrim').onclick=closeOv;E('modal').onclick=function(e){if(e.target==E('modal'))closeOv()};E('qcmd').onclick=cmdPicker;E('gear').onclick=function(){wsSend('@sg')};\n"
  "E('csort').onclick=function(){sheet('Sort by',SORTL.map(function(s,i){return {label:(i==csort?'\\u2713 ':'')+s,fn:function(){csort=i;renderContacts()}}}))};\n"
  "E('cfilt').onclick=function(){sheet('Filter',FILTL.map(function(f,i){return {label:(i==cfilter?'\\u2713 ':'')+f,fn:function(){cfilter=i;renderContacts()}}}))};\n"
  "E('csearch').addEventListener('input',function(){csearchq=E('csearch').value.trim().toLowerCase();renderContacts()});\n"
  "E('cvback').onclick=function(){closeChat();wsSend('@t')};E('cvsend').onclick=chatSend;\n"
  "E('cvi').addEventListener('keydown',function(e){if(e.key=='Enter'){chatSend();e.preventDefault()}});\n"
  "E('i').addEventListener('keydown',function(e){if(e.key=='Enter'){tsend();e.preventDefault()}\n"
  " else if(e.key=='ArrowUp'){if(hi>0){hi--;E('i').value=hist[hi];e.preventDefault()}}\n"
  " else if(e.key=='ArrowDown'){if(hi<hist.length-1){hi++;E('i').value=hist[hi]}else{hi=hist.length;E('i').value=''}e.preventDefault()}\n"
  " else if(e.key=='Tab'){var q=E('i').value.toLowerCase(),m=CMDS.filter(function(c){return c.indexOf(q)==0});\n"
  "  if(m.length==1)E('i').value=m[0];else if(m.length>1)tadd('\\n'+m.join('  ').trim()+'\\n','e');e.preventDefault()}});\n"
  "tadd('wadamesh terminal - node config + messaging. help = all commands.\\n\\n','e');\n"
  "conn();showTab('chats');\n"
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
#if defined(HAS_TDISPLAY_P4)
  // C6/AT transport: no lwIP fd to select() on (fd() is -1 by design). Writes go through the AT
  // worker whose CIPSEND exchange is itself bounded (the C6 buffers TX), so liveness is the only
  // cheap pre-check here.
  return client.connected();
#else
  int fd = client.fd();
  if (fd < 0) return false;
  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(fd, &wset);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  return select(fd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(fd, &wset);
#endif
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
    _clients[i].is_term = false;
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

void WebSocketCompanionServer::adoptClient(WiFiClient& incoming) {
  // Slot-insert an already-accepted connection. Used by our own accept loop below AND by the
  // T-Display P4's first-byte router (one shared AT listener dispatches companion vs web).
  WsClientsLock _lk(_client_mtx);
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
  _clients[slot].is_term = false;
  _clients[slot].meta_sent = false;
  _clients[slot].tx_len = 0;      // drop any stale pending frame from the previous occupant (tx_buf is reused)
  _clients[slot].tx_sent = 0;
}

void WebSocketCompanionServer::acceptNewClients() {
  if (!_listening) return;
  while (_server.hasClient()) {
    WiFiClient incoming = _server.accept();
    if (!incoming) continue;
    adoptClient(incoming);
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
          c->is_term   = (strncmp(c->handshake_buf, "GET /term", 9) == 0);   // web mesh terminal socket
          if (c->is_mirror || c->is_term) c->client.setNoDelay(true);   // low latency: small frames, no Nagle coalescing
          c->meta_sent = false;
          c->handshake_done = true;
          c->ws_state = WS_STATE_HEADER_0;
          c->comp_state = COMP_STATE_IDLE;
          return true;
        }
      }
      // Plain HTTP GET (no WS upgrade): serve the terminal page in terminal mode, else the mirror page.
      const char* page = g_web_mirror.terminalOn() ? WS_HTML_TERMINAL_PAGE : WS_HTTP_INFO_PAGE;
      (void)writeAllBytes(*cl, (const uint8_t*)page, strlen(page), TCP_WRITE_TIMEOUT_MS);
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
    if (c->is_mirror || c->is_term) continue;

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
    bool ok = _clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror && !_clients[i].is_term;
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
  return c->in_use && c->client.connected() && c->handshake_done && !c->is_mirror && !c->is_term;
}

int WebSocketCompanionServer::connectedCount() const {
  WsClientsLock _lk(_client_mtx);
  int n = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror && !_clients[i].is_term)
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
#if defined(HAS_TDISPLAY_P4)
  // AT transport: write() is a BLOCKING worker round-trip (there is no lwIP buffer to
  // fill non-blockingly), and we run with _client_mtx held — draining a whole band here
  // starved the main loop (tickHandshake / pollRecvFrame / mesh) and froze the device.
  // Send ONE CIPSEND-sized chunk per call; tx_sent resumes next pass. serviceMirror's
  // all-idle check then also self-limits to one in-flight band. A failed chunk means the
  // worker already waited out its SEND-OK window (seconds) — retrying a sick browser tab
  // would stall the whole AT link every pass, so drop the client immediately (the viewer
  // page auto-reconnects).
  size_t chunk = c->tx_len - c->tx_sent;
  if (chunk > 1436) chunk = 1436;   // == c6_at SOCK_PULL_MAX: exactly one AT round-trip
  size_t n = c->client.write(c->tx_buf + c->tx_sent, chunk);
  if (n == 0) { disconnectClient(idx); return; }
  c->tx_sent += (uint16_t)n;
  if (c->tx_sent >= c->tx_len) { c->tx_len = 0; c->tx_sent = 0; c->stall_ms = 0; }
#else
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
#endif
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
            } else if (ty == 0x05) {                          // exit remote mode
              m.requestExit();
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

// Parse a terminal client's masked WS frames; each complete text/binary frame is one
// command line -> m.pushTermCmd. Reuses this client's ws_* parse state (pollRecvFrame
// skips is_term clients, so it is exclusively ours).
void WebSocketCompanionServer::drainTermInput(int idx, WebMirror& m) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  int guard = 0;
  while (cl->available() && guard++ < 2048) {
    switch (c->ws_state) {
      case WS_STATE_HEADER_0: { uint8_t b = (uint8_t)cl->read(); c->ws_opcode = b & 0x0F; c->ws_state = WS_STATE_HEADER_1; break; }
      case WS_STATE_HEADER_1: {
        uint8_t b = (uint8_t)cl->read(); uint8_t l7 = b & 0x7F; c->ws_payload_read = 0; c->comp_rx_len = 0;
        if (l7 == 126 || l7 == 127) { c->ws_payload_len = l7; c->ws_state = WS_STATE_LEN_EXT; }
        else                        { c->ws_payload_len = l7; c->ws_state = WS_STATE_MASK; }
        break;
      }
      case WS_STATE_LEN_EXT: {
        if (c->ws_payload_len == 126) { if (cl->available() < 2) return; uint8_t hi = (uint8_t)cl->read(), lo = (uint8_t)cl->read(); c->ws_payload_len = ((uint16_t)hi << 8) | lo; }
        else                          { if (cl->available() < 8) return; c->ws_payload_len = 0; for (int i = 0; i < 8; i++) c->ws_payload_len = (c->ws_payload_len << 8) | (uint8_t)cl->read(); }
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_MASK; break;
      }
      case WS_STATE_MASK: {
        if (cl->available() < 4) return;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_PAYLOAD; break;
      }
      default: {  // WS_STATE_PAYLOAD
        uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
        if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len++] = b;
        c->ws_payload_read++;
        if (c->ws_payload_read >= c->ws_payload_len) {
          if ((c->ws_opcode == 0x01 || c->ws_opcode == 0x02) && c->comp_rx_len > 0) {   // text/binary = a command line
            uint16_t n = c->comp_rx_len;
            while (n > 0 && (c->comp_rx_buf[n - 1] == '\r' || c->comp_rx_buf[n - 1] == '\n')) n--;
            if (n > 0) m.pushTermCmd((const char*)c->comp_rx_buf, n);
          } else if (c->ws_opcode == 0x08) { disconnectClient(idx); return; }
          c->ws_state = WS_STATE_HEADER_0;
        }
        break;
      }
    }
  }
}

// Web mesh terminal: shuttle text both ways for /term clients (independent of the
// framebuffer mirror; a client is never both). Called from serviceMirror on the stream
// task with _client_mtx already held. writeBinaryFrame's per-client tx_buf keeps it
// non-blocking; the browser TextDecodes the reply bytes.
void WebSocketCompanionServer::serviceTerminalClients(WebMirror& m) {
  int tc = 0;
  bool all_idle = true;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_term) continue;
    tc++;
    drainClientTx(i);
    if (!c->in_use) continue;          // reaped mid-drain
    drainTermInput(i, m);
    if (c->tx_len != 0) all_idle = false;
  }
  m.noteTermClients(tc);
  if (tc == 0 || !all_idle || !_mirror_txbuf) return;   // wait until every client drained its previous chunk
  // Each frame is [type][payload]: type 0x01 = a complete JSON data message (Contacts/Chats),
  // 0x00 = a terminal text chunk. Prefer data (discrete + responsive) over the text stream.
  _mirror_txbuf[0] = 0x01;
  size_t n = m.popTermData(_mirror_txbuf + 1, WS_MIRROR_CLIENT_TXBUF - 8);
  if (n == 0) { _mirror_txbuf[0] = 0x00; n = m.popTermReply(_mirror_txbuf + 1, 2048); }
  if (n == 0) return;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_term) continue;
    writeBinaryFrame(i, _mirror_txbuf, n + 1);
  }
}

// Called every loop: refresh the client count for the producer gate, send each
// client its one-time screen-size meta, drain remote pointer input, and broadcast
// queued framebuffer bands. Strictly non-blocking (socketWritableNow-gated) so it
// never stalls the mesh loop (the beta_32 Wi-Fi-freeze discipline).
void WebSocketCompanionServer::serviceMirror(WebMirror& m) {
  WsClientsLock _lk(_client_mtx);          // held for the whole (quick, non-blocking) pass
  if (!_mirror_txbuf) {                    // shared scratch — also used by serviceTerminalClients for data frames
    _mirror_txbuf = (uint8_t*)heap_caps_malloc(WS_MIRROR_TXBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_mirror_txbuf) _mirror_txbuf = (uint8_t*)malloc(WS_MIRROR_TXBUF);
  }
  serviceTerminalClients(m);               // web mesh terminal + Contacts/Chats data (independent of the framebuffer)
  const int mc = mirrorClientCount();
  m.noteClients(mc);
  if (mc == 0 || !_mirror_txbuf) return;

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
