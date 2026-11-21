// SVG -> PNG（2x），用 playwright 自带的 Chromium：node render_png.js a.svg b.svg ...
const { chromium } = require('playwright');
const fs = require('fs');
(async () => {
  const b = await chromium.launch();
  for (const f of process.argv.slice(2)) {
    const svg = fs.readFileSync(f, 'utf8');
    const m = svg.match(/width="(\d+)" height="(\d+)"/);
    const p = await b.newPage({ viewport: { width: +m[1], height: +m[2] }, deviceScaleFactor: 2 });
    await p.setContent(`<html><body style="margin:0">${svg}</body></html>`);
    await p.screenshot({ path: f.replace(/\.svg$/, '.png'), clip: { x: 0, y: 0, width: +m[1], height: +m[2] } });
    await p.close();
    console.log(f.replace(/\.svg$/, '.png'));
  }
  await b.close();
})();
