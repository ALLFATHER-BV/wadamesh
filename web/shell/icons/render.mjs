// web/shell/icons/render.mjs: app-icon.svg -> the PNGs the manifest and iOS use.
//   node web/shell/icons/render.mjs      (needs Playwright, like the web tests)
// The PNGs are committed; run this again only when the SVG changes.
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const pw = createRequire(path.join(here, '..', '..', 'package.json'))('playwright');
const svg = fs.readFileSync(path.join(here, 'app-icon.svg'), 'utf8');
const b = await pw.chromium.launch();
for (const [name, px] of [['icon-180.png', 180], ['icon-192.png', 192], ['icon-512.png', 512]]) {
  const p = await b.newPage({ viewport: { width: px, height: px } });
  await p.setContent(`<body style="margin:0">${svg.replace('<svg ', `<svg width="${px}" height="${px}" `)}</body>`);
  await p.screenshot({ path: path.join(here, name), clip: { x: 0, y: 0, width: px, height: px } });
  await p.close();
}
await b.close();
