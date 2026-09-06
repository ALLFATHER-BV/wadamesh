#pragma once

#include "WebFileTransferConfig.h"

#if WADA_WEB_FILE_TRANSFER

static const char WS_HTTP_FILES_PAGE[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/html; charset=utf-8\r\n"
  "Cache-Control: no-store\r\n"
  "Content-Security-Policy: default-src 'self'; connect-src 'self' ws: wss:; style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n"
  "Connection: close\r\n"
  "\r\n"
R"FILEPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WADAMESH File Transfer</title>
<style>
:root{color-scheme:dark;--bg:#0b0d0e;--panel:#15191a;--line:#303738;--text:#edf2f1;--muted:#929b99;--accent:#15b6a6;--danger:#ef6a67}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(150deg,#0b0d0e,#111817);color:var(--text);font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
main{width:min(560px,100%);margin:0 auto;padding:24px 16px 40px}header{display:flex;align-items:center;gap:11px;margin-bottom:24px}header b{font-size:20px;letter-spacing:0}header i{display:block;width:9px;height:9px;border-radius:50%;background:var(--accent);box-shadow:0 0 12px var(--accent)}
section{border:1px solid var(--line);background:var(--panel);border-radius:8px;padding:16px;margin:12px 0}label{display:block;color:var(--muted);font-size:12px;margin-bottom:7px}input,button{font:inherit;border-radius:6px;min-height:42px}input[type=password]{width:100%;padding:9px 11px;background:#0d1112;color:var(--text);border:1px solid var(--line);font-size:18px;letter-spacing:3px}button{border:1px solid var(--line);background:#202627;color:var(--text);padding:8px 14px;cursor:pointer}button.primary{background:var(--accent);border-color:var(--accent);color:#061412;font-weight:700}button.danger{color:var(--danger)}button:disabled{opacity:.4;cursor:not-allowed}.row{display:flex;gap:8px;margin-top:10px}.row>*{flex:1}.drop{display:block;border:1px dashed #53605e;border-radius:8px;padding:24px 12px;text-align:center;color:var(--muted);cursor:pointer}.drop.ready{border-color:var(--accent);color:var(--text)}#pick{position:absolute;opacity:0;pointer-events:none}#name{margin-top:10px;overflow-wrap:anywhere}progress{width:100%;height:10px;margin-top:14px;accent-color:var(--accent)}#status{min-height:20px;margin-top:10px;color:var(--muted);font-size:13px}#status.ok{color:var(--accent)}#status.err{color:var(--danger)}small{display:block;color:var(--muted);line-height:1.5;margin-top:12px}.file{display:flex;align-items:center;gap:10px;padding:9px 0;border-bottom:1px solid var(--line)}.file:last-child{border-bottom:0}.file span{flex:1;min-width:0;overflow-wrap:anywhere}.file small{margin:2px 0 0}.empty{color:var(--muted);padding:10px 0}
</style>
</head>
<body><main>
<header><i></i><b>WADAMESH FILE TRANSFER</b></header>
<section>
  <label for="code">Session code shown on the device</label>
  <input id="code" type="password" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" autocomplete="one-time-code">
  <button id="connect" class="primary" style="width:100%;margin-top:10px">Connect</button>
</section>
<section>
  <label id="drop" class="drop" for="pick">Choose or drop a file</label>
  <input id="pick" type="file">
  <div id="name">No file selected</div>
  <progress id="progress" max="100" value="0"></progress>
  <div class="row"><button id="upload" class="primary" disabled>Upload</button><button id="cancel" class="danger" disabled>Cancel</button></div>
  <div id="status">Enter the session code to connect.</div>
  <small>Files are uploaded to the device's SD card in <b>/transfer</b>. The transfer session ends when you leave the File Transfer app on the device.</small>
</section>
<section>
  <div class="row" style="margin-top:0"><b>Device files</b><button id="refresh" disabled>Refresh</button></div>
  <div id="files" class="empty">Connect to list screenshots and transferred files.</div>
</section>
</main>
<script>
const E=id=>document.getElementById(id),dec=new TextDecoder(),CHUNK=2048,MAX=512*1024*1024,MAX_DOWNLOAD=64*1024*1024;
let ws=null,file=null,name='',offset=0,crc=0xffffffff,state='idle',pendingEnd=false,expectedAck=0,authed=false,cancelTimer=0,cancelText='',cancelKind='';
let entries=[],downloadParts=[],downloadSize=0,downloadOffset=0,downloadName='';
const table=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=(c&1)?(0xedb88320^(c>>>1)):(c>>>1);table[n]=c>>>0}
function crcAdd(c,a){for(let i=0;i<a.length;i++)c=table[(c^a[i])&255]^(c>>>8);return c>>>0}
function status(text,kind=''){E('status').textContent=text;E('status').className=kind}
function controls(){const ready=ws&&ws.readyState===1&&authed&&state==='ready';E('upload').disabled=!(ready&&file);E('refresh').disabled=!ready;E('cancel').disabled=state!=='uploading'&&state!=='downloading'}
function resetUpload(){if(cancelTimer){clearTimeout(cancelTimer);cancelTimer=0}offset=0;crc=0xffffffff;pendingEnd=false;expectedAck=0;cancelText=cancelKind='';downloadParts=[];downloadSize=downloadOffset=0;E('progress').value=0;state=authed?'ready':'idle';controls()}
function safeName(raw){return raw.replace(/[^A-Za-z0-9._-]/g,'_').replace(/^\.+/,'').slice(0,64)}
function choose(f){if(!f)return;if(f.size<=0||f.size>MAX){status('File must be between 1 byte and 512 MB.','err');return}file=f;name=safeName(f.name)||'upload.bin';E('name').textContent=(name===f.name?name:(f.name+' → '+name))+'  ('+f.size.toLocaleString()+' bytes)';E('drop').classList.add('ready');controls()}
function connect(){const code=E('code').value.trim();if(!/^\d{6}$/.test(code)){status('Enter the six-digit code.','err');return}if(ws)ws.close();authed=false;state='connecting';controls();const socket=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/files');ws=socket;socket.binaryType='arraybuffer';socket.onopen=()=>{if(ws===socket){status('Authenticating…');socket.send('AUTH '+code)}};socket.onerror=()=>{if(ws===socket)status('Connection error.','err')};socket.onclose=()=>{if(ws!==socket)return;ws=null;authed=false;resetUpload();status('Disconnected. Reopen File Transfer on the device if the session ended.','err')};socket.onmessage=e=>{if(ws===socket)onMessage(e)}}
function onMessage(ev){const bytes=typeof ev.data==='string'?null:new Uint8Array(ev.data);if(bytes&&bytes[0]===1){if(state!=='cancelling')downloadChunk(bytes);return}const text=typeof ev.data==='string'?ev.data:dec.decode(bytes);if(state==='cancelling'&&text!=='CANCELLED')return;if(text==='AUTH OK'){authed=true;state='ready';status('Connected.','ok');controls();refreshList();return}if(text==='READY'){state='uploading';status('Uploading…');controls();sendNext();return}if(text.startsWith('ACK ')){const next=Number(text.slice(4));if(state!=='uploading'||!Number.isSafeInteger(next)||next!==expectedAck||next>file.size){fail('Invalid acknowledgement.');return}offset=next;expectedAck=0;E('progress').value=Math.floor(offset*100/file.size);sendNext();return}if(text.startsWith('DONE ')){const completed=Number(text.slice(5));if(state!=='uploading'||!pendingEnd||completed!==file.size||offset!==file.size){fail('Invalid completion response.');return}E('progress').value=100;state='ready';controls();status('Upload complete: '+name,'ok');refreshList();return}if(text.startsWith('ENTRY ')){const m=text.match(/^ENTRY (\d+) (.+)$/);if(!m){fail('Invalid file list entry.');return}entries.push({size:Number(m[1]),path:m[2]});ws.send('LIST NEXT');return}if(text==='LIST DONE'){state='ready';renderFiles();controls();status(entries.length?'File list updated.':'No screenshots or transferred files yet.','ok');return}if(text.startsWith('FILE ')){const m=text.match(/^FILE (\d+) (.+)$/);if(!m){fail('Invalid download metadata.');return}downloadSize=Number(m[1]);if(!Number.isSafeInteger(downloadSize)||downloadSize<=0||downloadSize>MAX_DOWNLOAD){fail('Downloads are limited to 64 MB in this browser tool.');return}downloadName=m[2];downloadOffset=0;downloadParts=[];state='downloading';E('progress').value=0;controls();status('Downloading '+downloadName+'…');ws.send('READ 0');return}if(text==='CANCELLED'){const message=cancelText||'Transfer cancelled.',kind=cancelKind;resetUpload();status(message,kind);return}if(text.startsWith('ERR ')){fail(text.slice(4));return}}
async function sendNext(){if(state!=='uploading'||!file)return;if(offset>=file.size){if(!pendingEnd){pendingEnd=true;ws.send('END '+((crc^0xffffffff)>>>0).toString(16).padStart(8,'0'))}return}const start=offset;const bytes=new Uint8Array(await file.slice(start,Math.min(start+CHUNK,file.size)).arrayBuffer());if(state!=='uploading'||start!==offset)return;crc=crcAdd(crc,bytes);const frame=new Uint8Array(bytes.length+4);new DataView(frame.buffer).setUint32(0,start,true);frame.set(bytes,4);expectedAck=start+bytes.length;ws.send(frame)}
function upload(){if(!file||state!=='ready')return;offset=0;crc=0xffffffff;pendingEnd=false;expectedAck=0;E('progress').value=0;state='starting';controls();status('Preparing '+name+'…');ws.send('BEGIN '+file.size+' '+name)}
function requestCancel(text='Transfer cancelled.',kind=''){if(state==='cancelling')return;if(ws&&ws.readyState===1&&authed&&state!=='ready'&&state!=='idle'&&state!=='connecting'){cancelText=text;cancelKind=kind;state='cancelling';controls();status('Cancelling…');ws.send('CANCEL');cancelTimer=setTimeout(()=>{if(state!=='cancelling')return;const socket=ws;resetUpload();status(text,kind);if(socket&&socket.readyState<2)socket.close()},5000);return}resetUpload();status(text,kind)}
function cancel(){requestCancel()}
function fail(text){if(state==='starting'||state==='uploading'||state==='downloading'||state==='listing'){requestCancel(text,'err');return}resetUpload();status(text,'err')}
function refreshList(){if(!authed||state!=='ready')return;entries=[];state='listing';controls();status('Reading file list…');ws.send('LIST')}
function renderFiles(){const box=E('files');box.textContent='';box.className=entries.length?'':'empty';if(!entries.length){box.textContent='No files found.';return}entries.forEach(item=>{const row=document.createElement('div');row.className='file';const info=document.createElement('span');const title=document.createElement('b');title.textContent=item.path;const size=document.createElement('small');size.textContent=item.size.toLocaleString()+' bytes';info.append(title,size);const button=document.createElement('button');button.textContent='Download';button.onclick=()=>startDownload(item.path);row.append(info,button);box.append(row)})}
function startDownload(path){if(!authed||state!=='ready')return;state='starting';controls();status('Opening '+path+'…');ws.send('GET '+path)}
function downloadChunk(bytes){if(state!=='downloading'||bytes.length<6){fail('Unexpected download data.');return}const at=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength).getUint32(1,true),part=bytes.slice(5);if(at!==downloadOffset||downloadOffset+part.length>downloadSize){fail('Invalid download offset.');return}downloadParts.push(part);downloadOffset+=part.length;E('progress').value=Math.floor(downloadOffset*100/downloadSize);if(downloadOffset<downloadSize){ws.send('READ '+downloadOffset);return}if(downloadOffset!==downloadSize){fail('Download size mismatch.');return}const href=URL.createObjectURL(new Blob(downloadParts)),a=document.createElement('a');a.href=href;a.download=downloadName;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(href),1000);state='ready';downloadParts=[];controls();status('Downloaded '+downloadName,'ok')}
E('connect').onclick=connect;E('upload').onclick=upload;E('cancel').onclick=cancel;E('refresh').onclick=refreshList;E('pick').onchange=e=>choose(e.target.files[0]);
const drop=E('drop');drop.ondragover=e=>{e.preventDefault();drop.classList.add('ready')};drop.ondragleave=()=>{if(!file)drop.classList.remove('ready')};drop.ondrop=e=>{e.preventDefault();choose(e.dataTransfer.files[0])};
</script></body></html>)FILEPAGE";

static const char WS_HTTP_FILES_DISABLED[] =
  "HTTP/1.1 503 Service Unavailable\r\n"
  "Content-Type: text/plain; charset=utf-8\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n\r\n"
  "File Transfer is not enabled on the device.\n";

#endif  // WADA_WEB_FILE_TRANSFER