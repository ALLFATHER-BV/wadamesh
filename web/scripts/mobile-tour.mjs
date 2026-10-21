// web/scripts/mobile-tour.mjs: wadamesh on phones and tablets (Playwright device emulation).
//   node web/scripts/mobile-tour.mjs [--url http://localhost:8189/] [--shots web/docs]
//                                    [--devices "iPhone 14,Pixel 7,iPad (gen 7)"]
//   (web/build.sh mobiletest serves web/dist and runs it)
// iPhone 14 (WebKit), Pixel 7 (Chromium), iPad (WebKit): the canvas fills the space below
// the bar at the device's own size (portrait upright); tap into a chat, type through the
// phone keyboard path (the page's hidden field: UTF-8 with an emoji), send; turn to
// landscape and back with the data intact; Connect shows what that device can use.
// Taps go by label text (window.wadamesh.findText) or by canvas coordinates relative to
// the canvas size, so the tour runs at whatever size each device gets.
// Screenshots: web/docs/mobile-<device>-*.png
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(path.join(here, '..', 'package.json'));
const pw = require('playwright');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d; };
const URL_ = arg('--url', 'http://localhost:8080/');
const SHOTS = arg('--shots', path.join(here, '..', 'docs'));
const DEVICES = arg('--devices', 'iPhone 14,Pixel 7,iPad (gen 7)').split(',');
const ENGINE = { 'Pixel 7': 'chromium' };   // the rest: WebKit (Safari)
const TEXT = 'Grüße vom Handy 👋';
fs.mkdirSync(SHOTS, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = {};
let failures = 0;

async function booted(page) {
  await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
}
async function canvasSize(page) { return page.evaluate(() => window.wadamesh.size()); }
// canvas pixels -> page pixels
async function toPage(page, x, y) {
  const r = await page.locator('#screen').boundingBox();
  const [w, h] = await canvasSize(page);
  return [r.x + (x + 0.5) * r.width / w, r.y + (y + 0.5) * r.height / h];
}
async function tapAt(page, x, y) { const [px, py] = await toPage(page, x, y); await page.touchscreen.tap(px, py); await sleep(300); }
async function tapText(page, text) {
  const p = await page.evaluate((t) => window.wadamesh.findText(t), text);
  if (!p) throw new Error(`no label "${text}" on screen`);
  await tapAt(page, p[0], p[1]);
}
async function hasText(page, text) { return !!(await page.evaluate((t) => window.wadamesh.findText(t), text)); }
// How the canvas sits in the window: its size, how much of the space below the bar it
// covers, the bar (one row), crisp scaling.
async function fit(page) {
  return page.evaluate(() => {
    const c = document.getElementById('screen').getBoundingClientRect();
    const bar = document.querySelector('.bar').getBoundingClientRect();
    const vw = window.innerWidth, vh = window.innerHeight;
    const ix = Math.max(0, Math.min(c.right, vw) - Math.max(c.left, 0));
    const iy = Math.max(0, Math.min(c.bottom, vh) - Math.max(c.top, bar.bottom));
    const btn = document.getElementById('btn-connect').getBoundingClientRect();
    return { canvas: window.wadamesh.size().join('x'), css: `${Math.round(c.width)}x${Math.round(c.height)}`,
             viewport: `${vw}x${vh}`, dpr: devicePixelRatio, bar: Math.round(bar.height),
             barOneRow: bar.height < 64 && Math.abs((btn.top + btn.bottom) / 2 - (bar.top + bar.bottom) / 2) < 8,
             cover: +((ix * iy) / (vw * (vh - bar.bottom))).toFixed(3),
             crisp: document.getElementById('screen').classList.contains('crisp') };
  });
}
// The tab bar: the first tab (Chats) sits at the bottom left of every layout.
async function openChats(page) { const [w, h] = await canvasSize(page); await tapAt(page, w * 0.1, h - 16); await sleep(1500); }

for (const devName of DEVICES) {
  const engine = ENGINE[devName] || 'webkit';
  const tag = devName.toLowerCase().replace(/\(gen \d+\)/, '').replace(/[^a-z0-9]+/g, '');
  const r = results[devName] = { engine, errors: [] };
  const check = (k, ok, extra) => {
    r[k] = extra === undefined ? !!ok : { ok: !!ok, ...extra };
    if (!ok) { failures++; console.error('FAIL', devName, k, JSON.stringify(extra || '')); }
  };
  const browser = await pw[engine].launch();
  try {
    const dev = pw.devices[devName];
    if (!dev) throw new Error('unknown Playwright device ' + devName);
    const ctx = await browser.newContext({ ...dev });
    const page = await ctx.newPage();
    page.on('pageerror', (e) => r.errors.push(String(e)));
    // CSS-pixel screenshots keep web/docs small; the chat one is at the device's own pixels
    // (shows the crisp whole-pixel scaling)
    const snap = (n, full) => page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-${n}.png`), scale: full ? 'device' : 'css' });
    await page.goto(URL_, { waitUntil: 'load' });
    await booted(page);
    await sleep(4500);
    // 1. portrait: the canvas fills the space below the one-row bar
    const f1 = await fit(page);
    const [w, h] = await canvasSize(page);
    check('portrait canvas fills the screen', h > w && f1.cover > 0.9 && f1.barOneRow, f1);
    await snap('01-welcome');
    await tapText(page, 'Skip');
    await sleep(3000);
    await snap('02-home');
    // 2. a chat, the composer, the phone keyboard (the page's hidden field)
    await openChats(page);
    await tapText(page, 'Sanne');
    await sleep(1800);
    await snap('03-chat', true);
    await tapAt(page, w * 0.45, h - 28);   // the composer, at the bottom of the chat
    await sleep(800);
    check('a tap on the composer opens the phone keyboard (hidden field focused)',
          await page.evaluate(() => document.activeElement && document.activeElement.id === 'ime'));
    await page.keyboard.type(TEXT, { delay: 40 });
    await sleep(600);
    check('the UI keeps its own keys down', await page.evaluate(() => window.wadamesh._m()._wadaWebOskShown() === 0));
    const typed = await page.evaluate(() => window.wadamesh.editText());
    check('the field got the typed text, emoji included (UTF-8)', typed === TEXT, { typed });
    await snap('04-typing');
    await page.keyboard.press('Enter');
    await sleep(2500);
    check('Enter sends: the bubble shows the text', await hasText(page, TEXT), {});
    check('the field is empty again', (await page.evaluate(() => window.wadamesh.editText())) === '');
    await snap('05-sent');
    await page.evaluate(() => window.wadamesh.flush());
    check('the message is saved (UTF-8)', (await page.evaluate((t) => window.wadamesh.findInStorage(t), TEXT)).length > 0);
    if (engine === 'chromium') {
      // A composing keyboard (Gboard, any IME): the word grows while composing, then the
      // keyboard commits its (autocorrected) choice; Backspace deletes, also past the start.
      await tapAt(page, w * 0.45, h - 28);
      await sleep(600);
      const cdp = await ctx.newCDPSession(page);
      for (const t of ['w', 'we', 'wel', 'welk']) {
        await cdp.send('Input.imeSetComposition', { text: t, selectionStart: t.length, selectionEnd: t.length });
        await sleep(120);
      }
      const composing = await page.evaluate(() => window.wadamesh.editText());
      await cdp.send('Input.insertText', { text: 'welkom ' });
      await sleep(300);
      const committed = await page.evaluate(() => window.wadamesh.editText());
      for (let i = 0; i < 9; i++) { await page.keyboard.press('Backspace'); await sleep(60); }
      await sleep(300);
      const cleared = await page.evaluate(() => window.wadamesh.editText());
      check('composition (IME) is followed, committed and deleted', composing === 'welk' && committed === 'welkom ' && cleared === '',
            { composing, committed, cleared });
      await page.evaluate(() => document.activeElement && document.activeElement.blur());
      await sleep(900);
    }
    // 3. turn to landscape: the UI restarts in the landscape layout
    await page.evaluate(() => document.activeElement && document.activeElement.blur());
    await sleep(500);
    await page.setViewportSize({ width: dev.viewport.height, height: dev.viewport.width });
    await page.waitForEvent('load', { timeout: 20000 }).catch(() => {});
    await booted(page);
    await sleep(3500);
    const f2 = await fit(page);
    const [lw, lh] = await canvasSize(page);
    check('landscape after turning, filling the screen', lw > lh && f2.cover > 0.9 && f2.barOneRow, f2);
    await openChats(page);
    await snap('06-landscape');
    // (the preview may already be Sanne's answer: the demo sometimes replies)
    await tapText(page, 'Sanne');
    await sleep(1800);
    check('the chat still has the message after the turn', await hasText(page, TEXT));
    await snap('06b-landscape-chat');
    // 4. and back
    await page.setViewportSize(dev.viewport);
    await page.waitForEvent('load', { timeout: 20000 }).catch(() => {});
    await booted(page);
    await sleep(3500);
    const f3 = await fit(page);
    check('portrait again, the same size as before', f3.canvas === f1.canvas && f3.cover > 0.9, f3);
    await openChats(page);
    await tapText(page, 'Sanne');
    await sleep(1800);
    check('the sent message is still in the chat after two turns', await hasText(page, TEXT));
    await snap('07-portrait-again');
    // 5. Connect: what this device can use
    await page.click('#btn-connect');
    await page.waitForSelector('#connect-dlg[open]');
    const kinds = await page.evaluate(() => [...document.querySelectorAll('#connect-dlg [data-kind]')].filter((b) => !b.hidden).map((b) => b.dataset.kind));
    const note = await page.textContent('#connect-note');
    if (engine === 'chromium') check('Android Chrome offers Bluetooth', kinds.includes('ble'), { kinds });
    else check('iOS offers Wi-Fi only, with a one-line reason',
               !kinds.includes('ble') && !kinds.includes('web-serial') && kinds.includes('ws') && /iPhone and iPad/.test(note), { kinds, note });
    await snap('08-connect');
    // 6. the home-screen app: manifest + icons reachable
    const man = await page.evaluate(async () => {
      const href = document.querySelector('link[rel=manifest]').href;
      const m = await (await fetch(href)).json();
      const icons = await Promise.all(m.icons.map(async (i) => (await fetch(new URL(i.src, href))).ok));
      return { display: m.display, icons: icons.every(Boolean), maskable: m.icons.some((i) => i.purpose === 'maskable') };
    });
    check('web app manifest (standalone, icons)', man.display === 'standalone' && man.icons && man.maskable, man);
    check('no page errors', r.errors.length === 0, { errors: r.errors.slice(0, 3) });
    r.portrait = f1; r.landscape = f2;
    await ctx.close();
  } catch (e) {
    failures++;
    r.errors.push('RUNNER: ' + (e && e.stack || e));
    console.error('FAIL', devName, e && e.message);
  } finally {
    await browser.close();
  }
}
console.log(JSON.stringify(results, null, 2));
process.exit(failures ? 1 : 0);
