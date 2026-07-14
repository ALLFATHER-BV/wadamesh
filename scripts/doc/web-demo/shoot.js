// Automated screenshots of the wadamesh web control panel demo, to doc-shots/web_*.png.
// Start the demo server first:  python3 scripts/doc/web-demo/serve.py 8791
// Then:  NODE_PATH=<playwright node_modules> node scripts/doc/web-demo/shoot.js [port] [outdir]
const { chromium } = require('playwright');

const PORT = process.argv[2] || '8791';
const OUT  = process.argv[3] || 'doc-shots';
const URL  = `http://127.0.0.1:${PORT}/`;
const path = require('path');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({
    viewport: { width: 390, height: 780 },
    deviceScaleFactor: 2,           // crisp @2x for docs
  });
  const shot = async (name) => {
    const p = path.join(OUT, `web_${name}.png`);
    await page.screenshot({ path: p });
    console.log('  wrote', p);
  };

  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForSelector('#tlist .row', { timeout: 8000 });
  await sleep(600);

  // 1. Chats tab (default)
  await shot('chats');

  // 2. An open chat (first thread) with bubbles + delivery meta
  await page.click('#tlist .row');
  await page.waitForSelector('#cview', { state: 'visible', timeout: 5000 });
  await page.waitForSelector('#cvmsgs .b', { timeout: 5000 });
  await sleep(500);
  await shot('chat');
  await page.click('#cvback'); await sleep(300);

  // 3. Contacts tab
  await page.click('#tabs button[data-t=contacts]');
  await page.waitForSelector('#clist .row', { timeout: 5000 });
  await sleep(400);
  await shot('contacts');

  // 4. Contact action sheet (tap a contact row)
  await page.click('#clist .row[data-c]');
  await page.waitForSelector('#sheet.on', { timeout: 5000 });
  await sleep(400);
  await shot('contact_sheet');
  await page.click('#scrim'); await sleep(400);

  // 5. Discovered nodes sheet
  await page.click('#discrow');
  await page.waitForSelector('#sheet.on', { timeout: 5000 });
  await sleep(400);
  await shot('discovered');
  await page.click('#scrim'); await sleep(400);

  // 6. Terminal tab with some output
  await page.click('#tabs button[data-t=term]');
  await page.waitForSelector('#i', { timeout: 5000 });
  for (const cmd of ['status', 'ver']) {
    await page.fill('#i', cmd);
    await page.press('#i', 'Enter');
    await sleep(500);
  }
  await sleep(400);
  await shot('terminal');

  // 7. Settings modal (gear)
  await page.click('#gear');
  await page.waitForSelector('#modal.on', { timeout: 5000 });
  await sleep(400);
  await shot('settings');

  await browser.close();
  console.log('done.');
})().catch(e => { console.error('FAILED:', e); process.exit(1); });
