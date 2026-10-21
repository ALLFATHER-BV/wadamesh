// web/scripts/node-tour.mjs: the page connected to a node, in a real browser. A mock
// node (web/mesh/test/mock_ws_node.py) stands in for a MeshCore companion's WebSocket
// server; the page connects to it through Connect > Wi-Fi, exactly as to a device.
//
//   node web/scripts/node-tour.mjs [--url http://localhost:8187/] [--ws localhost:18765]
//                                  [--shots web/docs]
// Checks: the chooser, contacts / channels / waiting messages from the node, a DM that
// gets Delivered, a live message, reload reconnects (same data dir), Disconnect returns
// to the demo with the demo's own data. Screenshots: web/docs/node-*.png
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(path.join(here, '..', 'package.json'));
const pw = require('playwright');
const arg = (name, dflt) => {
  const i = process.argv.indexOf(name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
};
const URL_ = arg('--url', 'http://localhost:8187/');
const WS = arg('--ws', 'localhost:18765');
const SHOTS = arg('--shots', path.join(here, '..', 'docs'));
fs.mkdirSync(SHOTS, { recursive: true });
const W = 800, H = 480;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = {};
let failures = 0;
const check = (name, ok, extra) => {
  results[name] = extra === undefined ? !!ok : { ok: !!ok, ...extra };
  if (!ok) { failures++; console.error('FAIL', name, extra || ''); } else console.log('ok  ', name);
};

async function tap(page, x, y) {
  const r = await page.locator('#screen').boundingBox();
  await page.mouse.move(r.x + (x + 0.5) * r.width / W, r.y + (y + 0.5) * r.height / H);
  await page.mouse.down(); await sleep(80); await page.mouse.up(); await sleep(150);
}
async function shot(page, name) { await page.screenshot({ path: path.join(SHOTS, `node-${name}.png`) }); }
const badge = (page) => page.locator('#mode').textContent();
async function booted(page) {
  await page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
}
async function waitBadge(page, re, ms = 20000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (re.test(await badge(page))) return true; await sleep(250); }
  return false;
}
const inStorage = (page, needle) => page.evaluate((n) => window.wadamesh.findInStorage(n), needle);

const browser = await pw.chromium.launch();
// The visitor is in Ghent and lets the page use the location (step 6: the privacy rule).
const GHENT = { latitude: 51.05, longitude: 3.72 };
const ctx = await browser.newContext({ viewport: { width: 1100, height: 760 }, geolocation: GHENT,
                                       permissions: ['geolocation'] });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push(String(e)));
// (an http page that is not localhost: the browser notes it ignores COOP; that is the point)
page.on('console', (m) => { if (m.type() === 'error' && !/Cross-Origin-Opener-Policy header has been ignored/.test(m.text())) errors.push(m.text()); });

// 1. The demo, then the chooser.
await page.goto(URL_, { waitUntil: 'load' });
await booted(page);
await sleep(4000);
await tap(page, 60, 283);   // the demo's welcome: Skip
await sleep(2500);
check('demo badge', /demo/i.test(await badge(page)), { badge: await badge(page) });
await shot(page, '01-demo');
await page.click('#btn-connect');
await page.waitForSelector('#connect-dlg[open]');
const kinds = await page.evaluate(() => [...document.querySelectorAll('#connect-dlg [data-kind]')]
  .filter((b) => !b.hidden).map((b) => b.dataset.kind));
check('chooser offers the transports this browser has', kinds.includes('ws'), { kinds });
await shot(page, '02-chooser');

// 2. Connect over Wi-Fi (the mock's WebSocket server).
await page.fill('#ws-host', WS);
await page.click('#connect-dlg [data-kind="ws"]');
await page.waitForLoadState('load');
await booted(page);
check('connected', await waitBadge(page, /^Connected: Mock Node over Wi-Fi$/), { badge: await badge(page) });
check('node data dir', (await page.evaluate(() => window.wadamesh.dataDir)).startsWith('/data/node-'));
await sleep(2500);
await shot(page, '03-connected-home');
await tap(page, 80, 456);   // Chats
await sleep(1500);
await shot(page, '04-chats');
await tap(page, 240, 456);  // Contacts
await sleep(1500);
await shot(page, '05-contacts');

// 3. A DM to Alice: Delivered, and her reply.
await tap(page, 80, 456);
await sleep(1200);
await tap(page, 400, 152);  // Alice (second row: #wadatest has the newest message)
await sleep(1500);
await tap(page, 380, 447);
await sleep(600);
await page.keyboard.type('hello from the browser', { delay: 20 });
await page.keyboard.press('Enter');
await sleep(5000);
await shot(page, '06-dm-delivered');
await page.evaluate(() => window.wadamesh.flush());
check('DM reply from the node stored', (await inStorage(page, 'mock reply: got it')).length > 0);

// 4. The live channel post the mock sends 8 s after connecting.
await page.keyboard.press('Escape');
await sleep(1500);
await shot(page, '07-live');
await page.evaluate(() => window.wadamesh.flush());
check('live message received', (await inStorage(page, 'live hello from the mock')).length > 0);
check('waiting message synced', (await inStorage(page, 'hello from the mock (waiting)')).length > 0);
const nodeDir = await page.evaluate(() => window.wadamesh.dataDir);

// 5. Reload: reconnects by itself, into the node's own data dir (now keyed by the node).
await page.reload({ waitUntil: 'load' });
await booted(page);
check('reconnects after reload', await waitBadge(page, /^Connected: Mock Node/), { badge: await badge(page) });
const dir2 = await page.evaluate(() => window.wadamesh.dataDir);
check('data dir keyed by the node', /^\/data\/node-[0-9a-f]{8}$/.test(dir2), { before: nodeDir, after: dir2 });
await sleep(2000);
await tap(page, 80, 456);
await sleep(1500);
await shot(page, '08-after-reload');
const hits = await inStorage(page, 'hello from the browser');
check('history kept across the reload', hits.some((h) => h.startsWith(dir2)), { hits });

// 6. Location privacy: with the GPS on, this computer's position reaches the node ONLY
// while the node's own "share position in adverts" is on (WEB_BUILD.md "Location").
const mockState = () => fetch(`http://${WS}/latlon`).then((r) => r.json());
if (!(await page.evaluate(() => window.isSecureContext))) {
  results['location privacy'] = 'skipped (an insecure http page has no location)';
} else {
for (const t of ['tap 720 456', 'wait 1500']) if (t.startsWith('tap')) await tap(page, ...t.split(' ').slice(1).map(Number)); else await sleep(1500);
{
  const r = await page.locator('#screen').boundingBox();
  const at = (x, y) => [r.x + (x + 0.5) * r.width / W, r.y + (y + 0.5) * r.height / H];
  await page.mouse.move(...at(200, 420)); await page.mouse.down();
  for (let i = 1; i <= 24; i++) { await page.mouse.move(...at(200, 420 - 380 * i / 24)); await sleep(16); }
  await sleep(400); await page.mouse.up(); await sleep(1500);   // Settings, scrolled to the end
}
await tap(page, 100, 302);   // GPS
await sleep(1500);
await tap(page, 745, 95);    // GPS on
await sleep(9000);
await shot(page, '10-gps-on');
check('share off: the position stays in the browser', (await mockState()).latlon.length === 0, await mockState());
await fetch(`http://${WS}/share/1`);   // the node now shares its position (as if set in the phone app)
await page.reload({ waitUntil: 'load' });
await booted(page);
await waitBadge(page, /^Connected: Mock Node/);
await sleep(9000);
const shared = (await mockState()).latlon;
check('share on: the node gets this computer\'s position',
      shared.length >= 1 && Math.abs(shared[0][0] - GHENT.latitude) < 0.001 && Math.abs(shared[0][1] - GHENT.longitude) < 0.001,
      { latlon: shared });
await fetch(`http://${WS}/share/0`);
}

// 7. Disconnect: back to the demo, whose data never saw the node's.
await page.click('#btn-connect');
await page.waitForLoadState('load');
await booted(page);
await sleep(2500);
check('demo again', /demo/i.test(await badge(page)), { badge: await badge(page) });
const demoHits = (await inStorage(page, 'mock reply: got it')).filter((h) => h.startsWith('/data/demo'));
check('demo data has nothing from the node', demoHits.length === 0, { demoHits });
await shot(page, '09-demo-again');

check('no page errors', errors.length === 0, { errors: errors.slice(0, 5) });
console.log(JSON.stringify(results, null, 2));
await browser.close();
process.exit(failures ? 1 : 0);
