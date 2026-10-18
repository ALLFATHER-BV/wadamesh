// web/scripts/mobile-tour.mjs: wadamesh on phones (Playwright device emulation).
//   node web/scripts/mobile-tour.mjs [--url http://localhost:8080/] [--shots web/docs]
// iPhone 14 (WebKit) and Pixel 7 (Chromium): a portrait canvas that fills the screen;
// tap into a chat, type through the phone keyboard path (the page's hidden field:
// UTF-8 with an emoji), send; turn the phone to landscape and back with the data
// intact; Connect shows what that phone can use. Screenshots: web/docs/mobile-*.png
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
fs.mkdirSync(SHOTS, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = {};
let failures = 0;

async function booted(page) {
  await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
}
// canvas pixels -> page pixels, for the current canvas size
async function at(page, fx, fy) {
  const r = await page.locator('#screen').boundingBox();
  const [w, h] = await page.evaluate(() => [document.getElementById('screen').width, document.getElementById('screen').height]);
  return [r.x + (fx <= 1 ? fx * w : fx) * r.width / w, r.y + (fy <= 1 ? fy * h : fy) * r.height / h, w, h];
}
async function tap(page, fx, fy) { const [x, y] = await at(page, fx, fy); await page.touchscreen.tap(x, y); await sleep(250); }
async function canvasSize(page) { return page.evaluate(() => [document.getElementById('screen').width, document.getElementById('screen').height]); }

for (const [devName, engine] of [['iPhone 14', 'webkit'], ['Pixel 7', 'chromium']]) {
  const tag = devName.toLowerCase().replace(/\s+/g, '');
  const r = results[devName] = { errors: [] };
  const check = (k, ok, extra) => { r[k] = extra === undefined ? !!ok : { ok: !!ok, ...extra }; if (!ok) { failures++; console.error('FAIL', devName, k, extra || ''); } };
  const browser = await pw[engine].launch();
  try {
    const dev = pw.devices[devName];
    const ctx = await browser.newContext({ ...dev });
    const page = await ctx.newPage();
    page.on('pageerror', (e) => r.errors.push(String(e)));
    await page.goto(URL_, { waitUntil: 'load' });
    await booted(page);
    await sleep(5000);
    const [w, h] = await canvasSize(page);
    const fill = await page.evaluate(() => { const b = document.getElementById('screen').getBoundingClientRect(); return [Math.round(b.width), window.innerWidth]; });
    check('portrait canvas', h > w && fill[0] >= fill[1] - 2, { canvas: `${w}x${h}`, cssWidth: fill[0], viewport: fill[1] });
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-01-welcome.png`) });
    // the welcome's Skip: the left button of the pair under the text (calibrated on 480 wide)
    await tap(page, 60 / 480, 533 / 960);
    await sleep(3000);
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-02-home.png`) });
    // Chats (first tab), the first conversation, the composer at the bottom
    await tap(page, 0.1, 0.975);
    await sleep(1500);
    await tap(page, 0.5, 0.17);
    await sleep(1800);
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-03-chat.png`) });
    await tap(page, 0.45, 0.955);
    await sleep(800);
    const imeFocused = await page.evaluate(() => document.activeElement && document.activeElement.id === 'ime');
    check('a tap on the composer opens the phone keyboard (hidden field focused)', imeFocused);
    await page.keyboard.type('Grüße vom Handy 👋', { delay: 40 });
    await sleep(600);
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-04-typing.png`) });
    check('the UI keeps its own keys down', await page.evaluate(() => window.wadamesh._m()._wadaWebOskShown() === 0));
    await page.keyboard.press('Enter');
    await sleep(3000);
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-05-sent.png`) });
    await page.evaluate(() => window.wadamesh.flush());
    // turn to landscape: the UI restarts in the landscape layout, the chat is still there
    await page.evaluate(() => document.activeElement && document.activeElement.blur());
    await page.setViewportSize({ width: dev.viewport.height, height: dev.viewport.width });
    await page.waitForEvent('load', { timeout: 20000 }).catch(() => {});
    await booted(page);
    await sleep(3500);
    const [lw, lh] = await canvasSize(page);
    check('landscape after turning', lw > lh, { canvas: `${lw}x${lh}` });
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-06-landscape.png`) });
    await page.setViewportSize(dev.viewport);
    await page.waitForEvent('load', { timeout: 20000 }).catch(() => {});
    await booted(page);
    await sleep(3500);
    const [pw2, ph2] = await canvasSize(page);
    check('portrait again', ph2 > pw2, { canvas: `${pw2}x${ph2}` });
    check('the typed text (UTF-8) was sent, stored, and is still there after two turns',
          (await page.evaluate(() => window.wadamesh.findInStorage(   // the stored UTF-8, read back as Latin-1
            String.fromCharCode(...new TextEncoder().encode('Grüße vom Handy'))))).length > 0);
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-07-portrait-again.png`) });
    // Connect: what this phone can use
    await page.click('#btn-connect');
    await page.waitForSelector('#connect-dlg[open]');
    const kinds = await page.evaluate(() => [...document.querySelectorAll('#connect-dlg [data-kind]')].filter((b) => !b.hidden).map((b) => b.dataset.kind));
    r.chooser = kinds;
    if (engine === 'chromium') check('Android Chrome offers Bluetooth', kinds.includes('ble'), { kinds });
    else check('iOS offers Wi-Fi only, with a note', !kinds.includes('ble') && kinds.includes('ws') &&
               /iPhone and iPad/.test(await page.textContent('#connect-note')), { kinds });
    await page.screenshot({ path: path.join(SHOTS, `mobile-${tag}-08-connect.png`) });
    check('no page errors', r.errors.length === 0, { errors: r.errors.slice(0, 3) });
    await ctx.close();
  } catch (e) {
    failures++;
    r.errors.push('RUNNER: ' + (e && e.stack || e));
  } finally {
    await browser.close();
  }
}
console.log(JSON.stringify(results, null, 2));
process.exit(failures ? 1 : 0);
