// Shared by every console app. The browser keeps a random id in localStorage and opens
// each socket with "HELLO <id> <label>". A trusted or already-approved browser gets
// "AUTH OK" at once; a new one gets "WAIT" while the device asks its user to allow it.
// "CODE <id> nnnnnn" (the 6-digit code shown on the device) is the fallback.
var WA=0,WD=0,WR=function(){},WK='wmc_id';
function wId(){var i=null;try{i=localStorage.getItem(WK);}catch(e){}
if(!i||!/^[0-9a-f]{32}$/.test(i)){var a=new Uint8Array(16);crypto.getRandomValues(a);i='';
for(var n=0;n<16;n++)i+=('0'+a[n].toString(16)).slice(-2);try{localStorage.setItem(WK,i);}catch(e){}}return i;}
function wLabel(){var u=navigator.userAgent,b=/Edg\//.test(u)?'Edge':/Firefox\//.test(u)?'Firefox':/Chrome\//.test(u)?'Chrome':/Safari\//.test(u)?'Safari':'Browser',
o=/iPhone|iPad/.test(u)?'iOS':/Android/.test(u)?'Android':/Mac OS X/.test(u)?'Mac':/Windows/.test(u)?'Windows':/Linux/.test(u)?'Linux':'';
return (b+(o?' on '+o:'')).slice(0,23);}
function wShow(m,wait){var d=document;d.getElementById('wov').style.display='block';
d.getElementById('wtitle').textContent=wait?'Approve this browser on the device':'Enter the 6-digit code shown on the device';
d.getElementById('wsub').style.display=wait?'block':'none';d.getElementById('wmsg').textContent=m||'';
if(!wait)d.getElementById('wcode').focus();}
function wOpen(s){WA=0;s.send('HELLO '+wId()+' '+wLabel());
document.getElementById('wgo').onclick=function(){var c=document.getElementById('wcode').value.trim();if(/^\d{6}$/.test(c))s.send('CODE '+wId()+' '+c);};
document.getElementById('wcode').onkeydown=function(e){if(e.key==='Enter')document.getElementById('wgo').onclick();};}
function wGate(e){if(WA)return 0;
var t=typeof e.data==='string'?e.data:new TextDecoder().decode(e.data);
if(t==='AUTH OK'){WA=1;WD=0;document.getElementById('wov').style.display='none';WR();return 1;}
if(t==='WAIT'){wShow('',1);return 1;}
if(t==='ERR code'){wShow('',0);return 1;}
if(t.indexOf('ERR')===0){if(t.indexOf('ERR denied')===0)WD=1;wShow(t.slice(4),0);return 1;}
return 1;}
// Reconnect a dropped socket, unless the device user denied this browser: then stay
// put until the page is reloaded, rather than asking the device again every few seconds.
function wRetry(f){if(WD){wShow('Denied on the device. Reload the page to ask again.',0);return;}setTimeout(f,1500);}
