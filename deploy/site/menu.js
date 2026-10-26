// The phone menu: one burger button that opens a list of every page and link on the site,
// with names. On a phone the icon row (home page) and the small top-bar links had become
// too many icons without labels. One list for every page, so a new page is one row here.
// Above 760 px nothing changes: the button is hidden and the pages keep their own bars.
(function () {
  'use strict';
  var PAGES = [
    ['index.html', 'Home', 'What WADAMESH is, the boards, questions', 'M3 11l9-8 9 8M5 10v10h14V10'],
    ['index.html#install', 'Install', 'Flash a board from the browser', 'M12 3v12M7 11l5 5 5-5M5 21h14'],
    ['docs.html', 'User guide', 'Every screen, explained', 'M4 4h11l5 5v11H4zM15 4v5h5M8 13h8M8 17h6'],
    ['webclient.html', 'Web client', 'WADAMESH in your browser', 'M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zM2 12h20M12 2a15 15 0 0 1 0 20M12 2a15 15 0 0 0 0 20'],
    ['apps.html', 'App Store', 'The Lua apps for the device', 'M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM17 14v6M14 17h6'],
    ['sdk.html', 'Lua SDK', 'Write your own app', 'M8 6l-6 6 6 6M16 6l6 6-6 6'],
    ['changelog.html', 'Changelog', 'What changed in every build', 'M4 6h16M4 12h16M4 18h10'],
    ['beta.html', 'Beta test matrix', 'Which boards were vouched for', 'M4 4h16v16H4zM8 12l3 3 5-6'],
    ['tanmatsu.html', 'Tanmatsu', 'WADAMESH on the Tanmatsu', 'M5 3h14v18H5zM9 17h6M8 7h8v6H8z'],
    ['index.html#tak', 'WADAMESH-TAK', 'The Cascadia Mesh build for teams', 'M9 11a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM3 20a6 6 0 0 1 12 0M17 12a2.5 2.5 0 1 0 0-5M15.5 14.2A5 5 0 0 1 21 19']
  ];
  var LINKS = [
    ['https://files.wadamesh.com', 'USB Files', 'Your device’s storage over the cable'],
    ['https://t413.com/p/projects/MapTileDl/', 'Map tile downloader', 'Offline maps for the SD card'],
    ['https://discord.gg/T8X2UaQ7Sm', 'Discord', 'Questions, ideas and help'],
    ['index.html#videos', 'Videos', 'Walkthroughs from the community'],
    ['https://github.com/ALLFATHER-BV/wadamesh/issues', 'Report a bug', 'On GitHub'],
    ['https://github.com/ALLFATHER-BV/wadamesh', 'Source code', 'GPL-3.0, on GitHub']
  ];

  function svg(d) {
    return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="' + d + '"/></svg>';
  }

  var css =
    '.wm-burger,.wm-menu{display:none}' +
    '@media (max-width:760px){' +
    '.wm-burger{display:inline-flex;align-items:center;gap:7px;height:38px;padding:0 13px;border:0;border-radius:100px;cursor:pointer;' +
      'background:var(--solid,#111);color:var(--ink,#fff);font:inherit;font-size:.86rem;font-weight:650;box-shadow:var(--shadow,none)}' +
    '.wm-burger svg{width:18px;height:18px}' +
    '.wm-burger.fixed{position:fixed;top:calc(12px + env(safe-area-inset-top,0px));right:14px;z-index:60}' +
    'html.wm-has-menu .fab-wrap{display:none!important}' +
    'html.wm-has-menu body>header{padding-top:calc(60px + env(safe-area-inset-top,0px))}' +
    'html.wm-has-menu .top .home{display:none!important}' +
    'html.wm-has-menu body>main>.wm-theme{display:none!important}' +
    'html.wm-has-menu body>main:has(>.wm-theme){padding-top:calc(52px + env(safe-area-inset-top,0px))}' +
    '.wm-menu{position:fixed;inset:0;z-index:70;display:block;visibility:hidden;pointer-events:none}' +
    '.wm-menu.open{visibility:visible;pointer-events:auto}' +
    '.wm-menu .scrim{position:absolute;inset:0;background:rgba(0,0,0,.5);opacity:0;transition:opacity .18s}' +
    '.wm-menu.open .scrim{opacity:1}' +
    '.wm-menu .sheet{position:absolute;top:0;right:0;bottom:0;width:min(340px,88vw);overflow-y:auto;overscroll-behavior:contain;' +
      'background:var(--solid,#0b0e10);color:var(--ink,#fff);box-shadow:var(--shadow,none);transform:translateX(100%);transition:transform .2s ease;' +
      'padding:calc(14px + env(safe-area-inset-top,0px)) 14px calc(18px + env(safe-area-inset-bottom,0px))}' +
    '.wm-menu.open .sheet{transform:none}' +
    '.wm-menu .hd{display:flex;align-items:center;gap:10px;margin-bottom:10px}' +
    '.wm-menu .hd b{font-weight:800;letter-spacing:.04em;flex:1}' +
    '.wm-menu .hd b span{color:var(--teal,#2dd4bf)}' +
    '.wm-menu .x{width:38px;height:38px;border:0;border-radius:50%;background:var(--surface,rgba(255,255,255,.06));color:inherit;cursor:pointer;display:grid;place-items:center}' +
    '.wm-menu .x svg{width:18px;height:18px}' +
    '.wm-menu h3{font-size:.72rem;letter-spacing:.08em;text-transform:uppercase;color:var(--mut,#9aa);margin:16px 6px 6px;font-weight:700}' +
    '.wm-menu a{display:flex;align-items:center;gap:12px;padding:10px;border-radius:12px;color:inherit;text-decoration:none}' +
    '.wm-menu a:hover,.wm-menu a:focus-visible{background:var(--surface,rgba(255,255,255,.06));outline:none}' +
    '.wm-menu a[aria-current="page"]{background:var(--bg2,rgba(255,255,255,.06))}' +
    '.wm-menu a .ic{width:34px;height:34px;flex:none;border-radius:10px;display:grid;place-items:center;background:var(--surface,rgba(255,255,255,.06));color:var(--teal,#2dd4bf)}' +
    '.wm-menu a .ic svg{width:18px;height:18px}' +
    '.wm-menu a b{display:block;font-weight:650;font-size:.95rem}' +
    '.wm-menu a small{display:block;color:var(--mut,#9aa);font-size:.8rem;line-height:1.35}' +
    '.wm-menu .ext{margin-left:auto;color:var(--mut,#9aa);font-size:.8rem}' +
    '.wm-menu .theme{display:flex;align-items:center;gap:12px;width:100%;padding:10px;margin-top:10px;border:0;border-radius:12px;background:var(--surface,rgba(255,255,255,.06));color:inherit;font:inherit;font-weight:650;cursor:pointer}' +
    '.wm-menu .theme svg{width:18px;height:18px}' +
    '.wm-menu .theme .i-sun{display:none}' +
    ':root[data-theme="light"] .wm-menu .theme .i-sun{display:block}:root[data-theme="light"] .wm-menu .theme .i-moon{display:none}' +
    '}' +
    '@media (prefers-reduced-motion:reduce){.wm-menu .sheet,.wm-menu .scrim{transition:none}}';

  function here(href) {
    var path = location.pathname.replace(/\/$/, '/index.html').split('/').pop() || 'index.html';
    return href.indexOf('#') < 0 && href === path;
  }

  function build() {
    if (document.querySelector('.wm-burger')) return;
    var st = document.createElement('style');
    st.textContent = css;
    document.head.appendChild(st);

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'wm-burger';
    btn.setAttribute('aria-label', 'Menu');
    btn.setAttribute('aria-expanded', 'false');
    btn.setAttribute('aria-controls', 'wm-menu');
    btn.innerHTML = svg('M4 7h16M4 12h16M4 17h16') + '<span>Menu</span>';

    var top = document.querySelector('.top');
    if (top) top.appendChild(btn);
    else { btn.classList.add('fixed'); document.body.appendChild(btn); }

    var rows = PAGES.map(function (p) {
      return '<a href="' + p[0] + '"' + (here(p[0]) ? ' aria-current="page"' : '') + '><span class="ic">' + svg(p[3]) +
        '</span><span><b>' + p[1] + '</b><small>' + p[2] + '</small></span></a>';
    }).join('');
    var links = LINKS.map(function (l) {
      var ext = /^https?:/.test(l[0]);
      return '<a href="' + l[0] + '"' + (ext ? ' target="_blank" rel="noopener"' : '') + '><span><b>' + l[1] + '</b><small>' + l[2] +
        '</small></span>' + (ext ? '<span class="ext" aria-hidden="true">↗</span>' : '') + '</a>';
    }).join('');

    var menu = document.createElement('div');
    menu.className = 'wm-menu';
    menu.id = 'wm-menu';
    menu.setAttribute('role', 'dialog');
    menu.setAttribute('aria-modal', 'true');
    menu.setAttribute('aria-label', 'Site menu');
    menu.innerHTML =
      '<div class="scrim" data-close></div><nav class="sheet">' +
      '<div class="hd"><b>WADA<span>MESH</span></b><button type="button" class="x" data-close aria-label="Close the menu">' + svg('M6 6l12 12M18 6L6 18') + '</button></div>' +
      '<h3>Pages</h3>' + rows + '<h3>Tools and community</h3>' + links +
      '<button type="button" class="theme" data-theme-toggle>' +
      '<svg class="i-moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/></svg>' +
      '<svg class="i-sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>' +
      '<span>Light or dark</span></button></nav>';
    document.body.appendChild(menu);
    document.documentElement.classList.add('wm-has-menu');

    var lastFocus = null;
    function open() {
      lastFocus = document.activeElement;
      menu.classList.add('open');
      btn.setAttribute('aria-expanded', 'true');
      document.documentElement.style.overflow = 'hidden';
      var first = menu.querySelector('a');
      if (first) first.focus({ preventScroll: true });
    }
    function close() {
      menu.classList.remove('open');
      btn.setAttribute('aria-expanded', 'false');
      document.documentElement.style.overflow = '';
      if (lastFocus && lastFocus.focus) lastFocus.focus({ preventScroll: true });
    }
    btn.addEventListener('click', open);
    menu.addEventListener('click', function (e) {
      var t = e.target;
      if (t.closest('[data-close]')) { close(); return; }
      var a = t.closest('a');
      // A link to a section of this same page only scrolls: close the sheet for it.
      if (a && a.getAttribute('href').indexOf('#') >= 0 && here(a.getAttribute('href').split('#')[0])) close();
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && menu.classList.contains('open')) close();
    });
    window.matchMedia('(min-width:761px)').addEventListener('change', function (m) { if (m.matches) close(); });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build);
  else build();
})();
