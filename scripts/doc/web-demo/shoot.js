// Automated screenshots of the wadamesh web control panel demo, to doc-shots/web_*.png.
// Start the demo server first:  python3 scripts/doc/web-demo/serve.py 8791
// Then:  NODE_PATH=<playwright node_modules> node scripts/doc/web-demo/shoot.js [port] [outdir]
const { chromium } = require('playwright');
const assert = require('assert');

const PORT = process.argv[2] || '8791';
const OUT  = process.argv[3] || 'doc-shots';
const URL  = `http://127.0.0.1:${PORT}/`;
const path = require('path');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
  const launchOptions = process.env.PLAYWRIGHT_EXECUTABLE_PATH
    ? { executablePath: process.env.PLAYWRIGHT_EXECUTABLE_PATH }
    : {};
  const browser = await chromium.launch(launchOptions);
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

  // Mention autocomplete: recent-first filtering, keyboard selection, escaped
  // labels, and replacement around a caret in the middle of existing text.
  await page.fill('#cvi', '@sa');
  await page.waitForSelector('#mnames.on button', { timeout: 5000 });
  assert.deepStrictEqual(await page.locator('#mnames button').allTextContents(), ['@Sanne', '@Sam <Ops>']);
  assert.strictEqual(await page.locator('#mnames ops').count(), 0);
  await page.press('#cvi', 'ArrowDown');
  await page.press('#cvi', 'Enter');
  assert.strictEqual(await page.inputValue('#cvi'), '@Sam <Ops> ');

  await page.fill('#cvi', 'hello @sa, later');
  await page.locator('#cvi').evaluate((el) => {
    el.setSelectionRange(9, 9);
    el.dispatchEvent(new Event('input', { bubbles: true }));
  });
  await page.waitForSelector('#mnames.on button', { timeout: 5000 });
  await page.press('#cvi', 'Tab');
  assert.strictEqual(await page.inputValue('#cvi'), 'hello @Sanne, later');

  await page.fill('#cvi', '@r');
  await page.waitForSelector('#mnames.on button', { timeout: 5000 });
  assert.strictEqual(await page.locator('#mnames button').first().textContent(), '@R&D "QA"');
  await shot('mentions');
  await page.fill('#cvi', '');
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
