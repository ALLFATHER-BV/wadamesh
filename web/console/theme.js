// Theme choice shared by every console page: "light", "dark", or none to follow the OS.
// Inlined in <head> so the saved choice applies before first paint. The tabs are
// same-origin frames, so a change made in one reaches the rest through the storage event.
var WT='wmc_theme';
function wTheme(t){var r=document.documentElement;if(t==='light'||t==='dark')r.setAttribute('data-theme',t);else r.removeAttribute('data-theme');}
try{wTheme(localStorage.getItem(WT));}catch(e){}
addEventListener('storage',function(e){if(e.key===WT)wTheme(e.newValue);});
