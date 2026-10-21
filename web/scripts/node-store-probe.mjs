// Store install while connected to a (mock) node, on either engine: --nt forces the
// no-threads build. Prints the [STORE]/[companion] log and whether the app landed.
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(path.join(here, '..', 'package.json'));
const pw = require('playwright');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const URL_ = arg('--url', 'http://localhost:8188/'), WS = arg('--ws', 'localhost:18766');
const NT = process.argv.includes('--nt'), ENGINE = arg('--browser', 'chromium');
const W = 800, H = 480, sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await pw[ENGINE].launch();
const ctx = await browser.newContext({ viewport: { width: 1100, height: 760 } });
const page = await ctx.newPage();
const log = [];
page.on('console', (m) => log.push(`${(performance.now() / 1000).toFixed(1)} ${m.type()} ${m.text()}`));
page.on('pageerror', (e) => log.push('PAGEERROR ' + e));
async function tap(x, y) {
  const r = await page.locator('#screen').boundingBox();
  await page.mouse.move(r.x + (x + 0.5) * r.width / W, r.y + (y + 0.5) * r.height / H);
  await page.mouse.down(); await sleep(80); await page.mouse.up(); await sleep(150);
}
const booted = () => page.waitForFunction(() => window.wadamesh && window.wadamesh.running(), null, { timeout: 60000 });
await page.goto(URL_ + (NT ? '?nt' : ''), { waitUntil: 'load' });
await booted(); await sleep(4000);
await tap(60, 283); await sleep(2000);
await page.click('#btn-connect');
await page.fill('#ws-host', WS);
await page.click('#connect-dlg [data-kind="ws"]');
await page.waitForLoadState('load'); await booted(); await sleep(6000);
console.log('badge:', await page.locator('#mode').textContent(), 'bundle nt:', await page.evaluate(() => [...document.scripts].some((s) => /nt\/wadamesh/.test(s.src))));
await tap(695, 360); await sleep(1500); await tap(60, 150); await sleep(5000);
const TRACE_CPU = process.argv.includes('--cpu');
if (TRACE_CPU) await browser.startTracing(page, { categories: ['disabled-by-default-v8.cpu_profiler', 'v8'] });
const APP = arg('--app', '');
if (APP) {
  for (let i = 0; i < 12; i++) {   // scroll the list until the app's row shows
    const p = await page.evaluate((t) => window.wadamesh.findText(t), APP);
    if (p && p[1] > 90 && p[1] < 440) { console.log('row', APP, p); await tap(740, p[1] + 10); break; }
    const r = await page.locator('#screen').boundingBox();
    await page.mouse.move(r.x + 400 * r.width / W, r.y + 400 * r.height / H); await page.mouse.down();
    for (let k = 1; k <= 10; k++) { await page.mouse.move(r.x + 400 * r.width / W, r.y + (400 - k * 22) * r.height / H); await sleep(16); }
    await sleep(60); await page.mouse.up(); await sleep(800);
  }
} else await tap(740, 463);
if (process.argv.includes('--watch')) {
  for (let k = 0; k < 12; k++) {
    await sleep(2000);
    const r = await Promise.race([page.evaluate(() => JSON.stringify({ f: window.wadamesh.stats.frames, p: window.wadamesh.stats.presents })), sleep(1500).then(() => 'JS BLOCKED')]);
    console.log('t+' + (2 * (k + 1)) + 's', r);
    if (r === 'JS BLOCKED') {
      await sleep(4000);
      if (TRACE_CPU) {
        const buf = await browser.stopTracing();
        const ev = JSON.parse(buf.toString()).traceEvents || [];
        const nodes = new Map(), counts = new Map();
        for (const e of ev) {
          if (e.name !== 'ProfileChunk') continue;
          const d = e.args.data.cpuProfile || {};
          for (const n of d.nodes || []) nodes.set(n.id, n);
          for (const sId of d.samples || []) counts.set(sId, (counts.get(sId) || 0) + 1);
        }
        const byName = new Map();
        for (const [id, c] of counts) { const n = nodes.get(id); const k = n ? (n.callFrame.functionName || '(anon)') + ' ' + (n.callFrame.url || '').split('/').pop() : '?' + id; byName.set(k, (byName.get(k) || 0) + c); }
        console.log('HOT:'); [...byName.entries()].sort((a, b) => b[1] - a[1]).slice(0, 30).forEach(([k, c]) => console.log('  ' + c + ' ' + k));
        // parents of the hottest node
        const parentOf2 = new Map(); for (const m of nodes.values()) for (const c of (m.children || [])) parentOf2.set(c, m.id);
        for (const m of nodes.values()) if (m.parent) parentOf2.set(m.id, m.parent);
        const tops = [...counts.entries()].filter(([id]) => { const m = nodes.get(id); return m && !/idle|program|garbage/.test(m.callFrame.functionName); }).sort((a, b) => b[1] - a[1]).slice(0, 3);
        for (const [tid, c] of tops) { const chain = []; let id = tid; for (let k = 0; k < 60 && id; k++) { const m = nodes.get(id); chain.push(m ? m.callFrame.functionName || '(anon)' : '?'); id = parentOf2.get(id); } console.log('STACK ' + c + ': ' + chain.join(' <- ')); }
        const top = null;
        if (top) { const chain = []; let n = nodes.get(top[0]); const parentOf = new Map(); for (const m of nodes.values()) if (m.parent) parentOf.set(m.id, m.parent);
          let id = top[0]; for (let k = 0; k < 40 && id; k++) { const m = nodes.get(id); chain.push(m ? m.callFrame.functionName || '(anon)' : '?'); id = parentOf.get(id); }
          console.log('HOTTEST STACK: ' + chain.join(' <- ')); }
      }
      console.log(log.slice(-12).join('\n')); process.exit(0);
    }
  }
  console.log(log.slice(-15).join('\n'));
  await browser.close(); process.exit(0);
}
if (process.argv.includes('--pause')) {
  await sleep(8000);
  const cdp = await ctx.newCDPSession(page);
  await cdp.send('Profiler.enable');
  await cdp.send('Profiler.start');
  await sleep(5000);
  const { profile } = await cdp.send('Profiler.stop');
  const self = new Map();
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const dt = profile.timeDeltas; let i = 0;
  for (const sid of profile.samples) { const n = byId.get(sid); const k = n.callFrame.functionName || '(anon)'; self.set(k, (self.get(k) || 0) + (dt[i++] || 0)); }
  console.log('HOT:\n' + [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => '  ' + (v / 1000).toFixed(0) + ' ms ' + k).join('\n'));
  console.log(log.slice(-25).join('\n'));
  await cdp.send('Debugger.enable');
  const paused = new Promise((r) => cdp.once('Debugger.paused', r));
  await cdp.send('Debugger.pause');
  const ev = await Promise.race([paused, sleep(10000).then(() => null)]);
  console.log('PAUSED STACK:\n' + (ev ? ev.callFrames.slice(0, 40).map((f) => '  ' + f.functionName + ' @' + f.url.split('/').pop()).join('\n') : '(no pause)'));
  await cdp.send('Debugger.resume').catch(() => {});
  await browser.close(); process.exit(0);
}
await page.screenshot({ path: '/tmp/store-before.png' });
await sleep(15000);
await page.screenshot({ path: '/tmp/store-after.png' });
await page.evaluate(() => window.wadamesh.flush());
const sizes = await page.evaluate(() => { const M = window.wadamesh; try { return JSON.stringify(M.fileSizes ? M.fileSizes() : null); } catch (e) { return String(e); } });
const hits = await page.evaluate(() => window.wadamesh.findInStorage('wada'));
console.log('installed:', hits.filter((h) => /apps\//.test(h)));
console.log(log.filter((l) => /STORE|LUA|companion|error|PAGEERROR|freertos|abort/i.test(l)).slice(-40).join('\n'));
await browser.close();
