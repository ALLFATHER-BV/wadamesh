// web/scripts/web-tour.mjs: the acceptance tour (web/scripts/tour.wada, the same file
// the native smoke test runs) replayed in real browsers against web/dist, through the
// page's own pointer and keyboard handlers. Then: reload persistence, a scroll fps
// probe, console errors, and a phone-sized run with touch.
//
//   node web/scripts/web-tour.mjs [--url http://localhost:8080/] [--browsers chromium,firefox,webkit]
//                                 [--shots web/docs] [--all-shots] [--quick]
// Needs Playwright (npm install --prefix web playwright; npx playwright install).
import { createRequire } from 'node:module';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(path.join(here, '..', 'package.json'));
const pw = require('playwright');

const arg = (name, dflt) => {
  const i = process.argv.indexOf(name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
};
const URL_ = arg('--url', 'http://localhost:8080/');
const BROWSERS = arg('--browsers', 'chromium').split(',');
const SHOTS = arg('--shots', path.join(here, '..', 'docs'));
const QUICK = process.argv.includes('--quick');
const ONLY = arg('--only', process.argv.includes('--geo-only') ? 'geo' : '');   // geo | store: just that check
const ALL_SHOTS = process.argv.includes('--all-shots');   // screenshots from every browser, not just Chromium
const tour = fs.readFileSync(path.join(here, 'tour.wada'), 'utf8').split('\n');
fs.mkdirSync(SHOTS, { recursive: true });

const W = 800, H = 480;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function canvasPoint(page, x, y) {
  const r = await page.locator('#screen').boundingBox();
  return [r.x + (x + 0.5) * r.width / W, r.y + (y + 0.5) * r.height / H];
}
async function canvasPng(page, file) {
  const data = await page.evaluate(() => document.getElementById('screen').toDataURL('image/png'));
  fs.writeFileSync(file, Buffer.from(data.split(',')[1], 'base64'));
}
const KEYMAP = { enter: 'Enter', esc: 'Escape', bs: 'Backspace', tab: 'Tab',
                 left: 'ArrowLeft', up: 'ArrowUp', down: 'ArrowDown', right: 'ArrowRight' };

async function runScript(page, lines, { prefix, touch, shots = true }) {
  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const [cmd, ...rest] = line.split(/\s+/);
    const n = rest.map(Number);
    if (cmd === 'wait') await sleep(n[0]);
    else if (cmd === 'tap' || cmd === 'hold') {
      const [px, py] = await canvasPoint(page, n[0], n[1]);
      if (touch && cmd === 'tap') { await page.touchscreen.tap(px, py); await sleep(120); continue; }
      await page.mouse.move(px, py);
      await page.mouse.down();
      await sleep(cmd === 'tap' ? 60 : (n[2] || 1300));
      await page.mouse.up();
      await sleep(120);
    } else if (cmd === 'drag') {
      const [x0, y0] = await canvasPoint(page, n[0], n[1]);
      const [x1, y1] = await canvasPoint(page, n[2], n[3]);
      const ms = n[4] || 300, steps = Math.max(4, Math.round(ms / 16));
      await page.mouse.move(x0, y0);
      await page.mouse.down();
      for (let i = 1; i <= steps; i++) {
        await page.mouse.move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        await sleep(ms / steps);
      }
      await sleep(n[5] || 40);   // optional: hold still before letting go (no scroll throw)
      await page.mouse.up();
      await sleep(120);
    } else if (cmd === 'wheel') {
      const [px, py] = await canvasPoint(page, n[0], n[1]);
      const r = await page.locator('#screen').boundingBox();
      await page.mouse.move(px, py);
      await page.mouse.wheel(0, n[2] * r.height / H);   // canvas pixels -> CSS pixels
      await sleep(80);
    } else if (cmd === 'type') {
      await page.keyboard.type(line.slice(5), { delay: 25 });
      await sleep(200);
    } else if (cmd === 'key') {
      await page.keyboard.press(KEYMAP[rest[0]] || rest[0]);
      await sleep(150);
    } else if (cmd === 'shot') {
      if (shots) await canvasPng(page, path.join(SHOTS, `${prefix}${rest[0]}.png`));
    } else if (cmd === 'quit') break;
  }
}

async function bootPage(page) {
  const t0 = Date.now();
  await page.goto(URL_, { waitUntil: 'load' });
  await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
  return Date.now() - t0;
}

async function scrollFps(page) {
  // Contacts list: drag up and down for ~3 s, count animation frames and presented frames.
  const s0 = await page.evaluate(() => ({ ...window.wadamesh.stats, now: performance.now() }));
  const [x, yTop] = await canvasPoint(page, 400, 140);
  const [, yBot] = await canvasPoint(page, 400, 400);
  for (let k = 0; k < 4; k++) {
    await page.mouse.move(x, yBot); await page.mouse.down();
    for (let i = 1; i <= 20; i++) { await page.mouse.move(x, yBot + (yTop - yBot) * i / 20); await sleep(16); }
    await page.mouse.up(); await sleep(200);
    await page.mouse.move(x, yTop); await page.mouse.down();
    for (let i = 1; i <= 20; i++) { await page.mouse.move(x, yTop + (yBot - yTop) * i / 20); await sleep(16); }
    await page.mouse.up(); await sleep(200);
  }
  const s1 = await page.evaluate(() => ({ ...window.wadamesh.stats, now: performance.now() }));
  const sec = (s1.now - s0.now) / 1000;
  return { seconds: +sec.toFixed(2), rafFps: +((s1.frames - s0.frames) / sec).toFixed(1),
           presentsPerSec: +((s1.presents - s0.presents) / sec).toFixed(1),
           mpixPerSec: +((s1.presentedPx - s0.presentedPx) / sec / 1e6).toFixed(2) };
}

async function historyHas(page, needle) {
  const hits = await page.evaluate((needle) => window.wadamesh.findInStorage(needle), needle);
  return hits.length ? hits : false;
}

// ---- location + map tiles ------------------------------------------------------------
// A fresh visitor in Ghent (Playwright's geolocation permission + coordinates): switch the
// GPS on in Settings > GPS, open the map. The map must ask for the tiles around Ghent
// (the centre came from the browser's location) and show them (non-uniform pixels), when
// the server can reach the tile server; without a network the tile half is skipped.
const GHENT = { latitude: 51.05, longitude: 3.72 };
function tileOf(lat, lon, z) {
  const n = 2 ** z, r = lat * Math.PI / 180;
  return [Math.floor((lon + 180) / 360 * n), Math.floor((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * n)];
}
async function mapPixelStats(page) {
  return page.evaluate(() => {
    const c = document.getElementById('screen');
    const d = c.getContext('2d').getImageData(0, 40, 740, 360).data;   // the map, without the bars and buttons
    const seen = new Set();
    let sum = 0, sum2 = 0, n = 0, dark = 0;
    for (let i = 0; i < d.length; i += 4 * 7) {
      const v = (d[i] * 299 + d[i + 1] * 587 + d[i + 2] * 114) / 1000;
      sum += v; sum2 += v * v; n++;
      if (v < 30) dark++;
      seen.add((d[i] >> 3) << 10 | (d[i + 1] >> 3) << 5 | (d[i + 2] >> 3));
    }
    const mean = sum / n;
    return { colours: seen.size, stddev: Math.sqrt(Math.max(0, sum2 / n - mean * mean)), mean,
             emptyShare: +(dark / n).toFixed(2) };   // still the map's dark background
  });
}
async function geoAndTiles(browser, r, shots) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 }, deviceScaleFactor: 1,
                                         geolocation: GHENT, permissions: ['geolocation'] });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => r.errors.push('[geo] ' + String(e)));
  const tiles = [];
  page.on('request', (q) => { const m = q.url().match(/\/tiles\/(\d+)\/(\d+)\/(\d+)\.jpg/); if (m) tiles.push(m.slice(1).map(Number)); });
  await bootPage(page);
  const geoAtBoot = await page.evaluate(() => window.wadamesh.geo());
  const secure = await page.evaluate(() => window.isSecureContext);   // http (not localhost): no location
  const net = await page.evaluate(() => fetch('tiles/3/4/2.jpg').then((x) => x.status, () => 0));
  await runScript(page, [
    'wait 5000', 'tap 60 283', 'wait 3000',            // onboarding, Home
    'tap 720 456', 'wait 1500', 'drag 200 420 200 40 400 400', 'wait 1500',   // Settings, to the end   // Settings, down to Connections
    'tap 100 302', 'wait 1500', 'tap 745 95', 'wait 3000',                 // GPS page, GPS on
    'shot 14-gps-fix', 'key esc', 'wait 1000', 'tap 560 456', 'wait 2000', // the map
  ], { prefix: 'web-', shots });
  let st = await mapPixelStats(page);
  // The UI paces its downloads (OSM etiquette), so the screen fills over some seconds.
  for (let i = 0; i < 40 && net === 200 && st.emptyShare > 0.1; i++) {
    await sleep(1000);
    if (i % 8 === 7) await runScript(page, ['wheel 400 240 1'], { shots: false });   // keep the screensaver away
    st = await mapPixelStats(page);
  }
  if (shots) await canvasPng(page, path.join(SHOTS, 'web-15-map-tiles.png'));
  const z = tiles.length ? Math.max(...tiles.map((t) => t[0])) : 0;
  const [cx, cy] = tileOf(GHENT.latitude, GHENT.longitude, z);
  r.geo = {
    promptedAtBoot: geoAtBoot.watching,    // must be false: nothing asks on page load
    watching: (await page.evaluate(() => window.wadamesh.geo())).watching,
    tileServer: net,
    tilesRequested: tiles.length,
    centredOnGhent: secure ? tiles.some((t) => t[0] === z && Math.abs(t[1] - cx) <= 1 && Math.abs(t[2] - cy) <= 1)
                           : 'skipped (no location on an insecure page)',
    map: st,
    tilesShown: net === 200 ? (st.colours >= 150 && st.stddev >= 12) : 'skipped (no tile server)',
  };
  await ctx.close();
}

// ---- the Store, Lua apps, backups ------------------------------------------------------
// A Store-only app (2048) installed through the same-origin Store proxy survives a
// reload and runs; a settings backup exported (a browser download) and imported into a
// fresh profile (the browser's file picker) carries a setting across; the page's own
// "Back up this browser" file restored into another fresh profile brings the app back.
const SETTINGS_END = ['tap 720 456', 'wait 1500', 'wheel 400 300 600', 'wait 800'];   // Settings, scrolled to the end
async function freshPage(browser, r, tag) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 }, acceptDownloads: true });
  const page = await ctx.newPage();
  const log = [];
  page.on('console', (m) => log.push(m.text()));
  page.on('pageerror', (e) => r.errors.push(`[${tag}] ` + String(e)));
  await bootPage(page);
  await runScript(page, ['wait 5000', 'tap 60 283', 'wait 3000'], { shots: false });   // the demo's welcome
  return { ctx, page, log };
}
async function exportBackup(page) {
  const [d] = await Promise.all([page.waitForEvent('download', { timeout: 30000 }),
    runScript(page, [...SETTINGS_END, 'tap 600 356', 'wait 1500', 'tap 400 126', 'wait 2500'], { shots: false })]);
  return { name: d.suggestedFilename(), text: fs.readFileSync(await d.path(), 'utf8') };
}
async function storeAndBackups(browser, r, shots) {
  const st = r.store = {};
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wada-store-'));
  try {
    // 1. Store: install 2048 (catalog row 7), reload, run it.
    let { ctx, page, log } = await freshPage(browser, r, 'store');
    const net = await page.evaluate(() => fetch('fw/apps/apps.json').then((x) => x.status, () => 0));
    st.storeServer = net === 200 ? 200 : 'skipped (no Store server)';
    if (net === 200) {
      await runScript(page, ['tap 695 360', 'wait 1500', 'tap 60 150', 'wait 5000', 'shot 16-store',
                             'tap 740 463', 'wait 6000', 'shot 17-store-installed', 'key esc', 'wait 1000'], { prefix: 'web-', shots });
      await page.evaluate(() => window.wadamesh.flush());
      st.installed = (await page.evaluate(() => window.wadamesh.findInStorage('wada'))).some((h) => /\/spiffs\/apps\/2048\.lua$/.test(h));
      await bootPage(page);   // reload
      await sleep(4000);
      log.length = 0;
      await runScript(page, ['tap 695 360', 'wait 1500', 'shot 18-drawer-2048', 'tap 398 150', 'wait 2500', 'shot 19-2048'], { prefix: 'web-', shots });
      st.runsAfterReload = log.some((t) => /\[LUAAPP\] 2048 open/.test(t));
      await bootPage(page);   // leave the game the simple way
      await sleep(4000);
    }
    // 2. Settings backup: Location privacy 1 km, export (a download).
    await runScript(page, [...SETTINGS_END, 'tap 100 302', 'wait 1500', 'tap 680 305', 'wait 800', 'key esc', 'wait 1000', 'key esc', 'wait 1000'], { shots: false });
    const b1 = await exportBackup(page);
    st.exportDownloaded = /\.json$/.test(b1.name) && /"gps_fuzz_m": 1000/.test(b1.text) && /"contacts": \[/.test(b1.text);
    st.exportHasNoPrivateKey = /"private_key": ""/.test(b1.text);
    fs.writeFileSync(path.join(tmp, b1.name), b1.text);
    // 3. The page's own backup (everything this backend keeps), for step 5.
    await page.click('#btn-more');
    const [tar] = await Promise.all([page.waitForEvent('download', { timeout: 20000 }), page.click('#btn-backup')]);
    const tarPath = path.join(tmp, tar.suggestedFilename());
    fs.copyFileSync(await tar.path(), tarPath);
    st.browserBackupDownloaded = /\.tar$/.test(tarPath) && fs.statSync(tarPath).size > 2048;
    await ctx.close();

    // 4. A fresh profile imports the settings backup through the file picker.
    ({ ctx, page, log } = await freshPage(browser, r, 'import'));
    const [chooser] = await Promise.all([page.waitForEvent('filechooser', { timeout: 30000 }),
      runScript(page, [...SETTINGS_END, 'tap 600 356', 'wait 1500', 'tap 400 391', 'wait 500'], { shots: false })]);
    await chooser.setFiles(path.join(tmp, b1.name));
    await sleep(1500);
    if (shots) await canvasPng(page, path.join(SHOTS, 'web-20-import-confirm.png'));
    await runScript(page, ['tap 550 391', 'wait 500'], { shots: false });   // the confirm's Import
    await page.waitForFunction(() => !window.wadamesh.running(), null, { timeout: 20000 }).catch(() => {});
    await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
    await sleep(4000);
    const b2 = await exportBackup(page);
    st.importCarriesSettings = /"gps_fuzz_m": 1000/.test(b2.text);
    await ctx.close();

    // 5. Another fresh profile restores the page backup: the installed app is back.
    ({ ctx, page, log } = await freshPage(browser, r, 'restore'));
    page.on('dialog', (d) => d.accept());
    await page.click('#btn-more');
    const [chooser2] = await Promise.all([page.waitForEvent('filechooser', { timeout: 20000 }), page.click('#btn-restore')]);
    await chooser2.setFiles(tarPath);
    await page.waitForEvent('load', { timeout: 30000 });
    await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
    await sleep(3000);
    const hits = await page.evaluate(() => window.wadamesh.findInStorage('wada'));
    if (net === 200) st.restoreBringsTheApp = hits.some((h) => /\/spiffs\/apps\/2048\.lua$/.test(h));
    st.restoreKeepsSettings = (await exportBackup(page)).text.includes('"gps_fuzz_m": 1000');
    await ctx.close();
  } catch (e) {
    r.errors.push('RUNNER store: ' + (e && e.stack || e));
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

const results = {};
for (const name of BROWSERS) {
  const r = results[name] = { errors: [], warnings: [] };
  const browser = await pw[name].launch();
  try {
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 }, deviceScaleFactor: 2 });
    const page = await ctx.newPage();
    page.on('console', (m) => {
      if (m.type() === 'error' && !/Cross-Origin-Opener-Policy header has been ignored/.test(m.text())) r.errors.push(m.text());
      else if (m.type() === 'warning') r.warnings.push(m.text().slice(0, 200));
    });
    page.on('pageerror', (e) => r.errors.push(String(e)));
    r.coi = null;
    if (ONLY) {
      await ctx.close();
      if (ONLY === 'geo') await geoAndTiles(browser, r, name === 'chromium' || ALL_SHOTS);
      if (ONLY === 'store') await storeAndBackups(browser, r, name === 'chromium' || ALL_SHOTS);
      continue;
    }
    r.firstLoadMs = await bootPage(page);
    r.coi = await page.evaluate(() => crossOriginIsolated);
    r.bootMs = await page.evaluate(() => window.wadamesh.stats.bootMs);
    const prefix = name === 'chromium' ? 'web-' : `web-${name}-`;
    if (name === 'chromium') await page.screenshot({ path: path.join(SHOTS, 'web-page.png') });
    const shots = name === 'chromium' || ALL_SHOTS;
    await runScript(page, QUICK ? tour.slice(0, 8) : tour, { prefix, shots });

    // persistence: the tour sent "Hello from the browser!" (06-sent); flush, reload, look again
    await page.evaluate(() => window.wadamesh.flush());
    await sleep(1500);
    r.savedBeforeReload = await historyHas(page, 'Hello from the browser');
    r.reloadMs = await bootPage(page);
    r.persisted = await historyHas(page, 'Hello from the browser');
    await sleep(5000);
    await runScript(page, ['shot 12-reload-home', 'tap 80 456', 'wait 1500', 'shot 12-reload-chats', 'tap 400 152', 'wait 2000', `shot 13-after-reload`],
                    { prefix, shots });
    // scroll fps on Contacts
    await runScript(page, ['tap 18 30', 'wait 1200', 'tap 240 456', 'wait 1500'], { prefix, shots: false });
    r.scroll = await scrollFps(page);
    await ctx.close();

    if (name === 'chromium') await geoAndTiles(browser, r, shots);
    if (name === 'chromium') await storeAndBackups(browser, r, shots);

    if (name === 'chromium' || name === 'webkit') {
      // a phone: portrait, touch
      const phone = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 3,
                                               isMobile: name === 'chromium', hasTouch: true });
      const p2 = await phone.newPage();
      p2.on('pageerror', (e) => r.errors.push('[phone] ' + String(e)));
      r.phoneLoadMs = await bootPage(p2);
      await sleep(4000);
      // The phone gets its own portrait canvas (its CSS width; web/build.sh mobiletest covers
      // phones in depth), so tap by label and by the canvas's own size here.
      const [pw2, ph2] = await p2.evaluate(() => window.wadamesh.size());
      r.phoneCanvas = `${pw2}x${ph2}`;
      const ptap = async (x, y) => {
        const b = await p2.locator('#screen').boundingBox();
        await p2.touchscreen.tap(b.x + (x + 0.5) * b.width / pw2, b.y + (y + 0.5) * b.height / ph2);
      };
      const skip = await p2.evaluate(() => window.wadamesh.findText('Skip'));
      if (skip) await ptap(skip[0], skip[1]);
      await sleep(2500);
      await ptap(pw2 * 0.1, ph2 - 16);   // Chats, the first tab
      await sleep(1500);
      if (shots) await p2.screenshot({ path: path.join(SHOTS, `${prefix}phone.png`) });
      r.phoneTapWorked = ph2 > pw2 && !!(await p2.evaluate(() => window.wadamesh.findText('Public')));
      await phone.close();
    }
  } catch (e) {
    r.errors.push('RUNNER: ' + (e && e.stack || e));
  } finally {
    await browser.close();
  }
}
console.log(JSON.stringify(results, null, 2));
const geoBad = (g) => g && (g.promptedAtBoot || (g.centredOnGhent !== 'skipped (no location on an insecure page)' &&
                                                  (!g.watching || !g.centredOnGhent)) || g.tilesShown === false);
const storeBad = (g) => g && Object.values(g).some((v) => v === false);
const bad = Object.values(results).some((r) => (!ONLY && !r.persisted) || geoBad(r.geo) || storeBad(r.store) || r.phoneTapWorked === false ||
                                               r.errors.some((e) => e.startsWith('RUNNER')));
process.exit(bad ? 1 : 0);
