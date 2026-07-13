// Serve an exported Godot web build, run it in Chrome, capture the
// [BENCHMARK_RESULT] console line and a screenshot mid-benchmark.
// Usage: node bench_browser.mjs <export_dir> <out_prefix>
import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join, extname, resolve } from 'path';

const exportDir = resolve(process.argv[2]);
const outPrefix = process.argv[3];
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.pck': 'application/octet-stream', '.png': 'image/png' };

const server = createServer((req, res) => {
  let p = req.url.split('?')[0];
  if (p === '/') p = '/index.html';
  const f = join(exportDir, p);
  if (!existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, {
    'Content-Type': MIME[extname(f)] || 'application/octet-stream',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
  });
  res.end(readFileSync(f));
});

server.listen(0, '127.0.0.1', async () => {
  const port = server.address().port;
  const pw = await import('playwright');
  const which = process.env.BENCH_BROWSER || 'chrome';
  // Headed: headless cannot capture WebGPU canvas content.
  const browser = which === 'firefox'
    ? await pw.firefox.launch({
        headless: false,
        firefoxUserPrefs: { 'dom.webgpu.enabled': true, 'gfx.webgpu.force-enabled': true },
      })
    : await pw.chromium.launch({
        channel: 'chrome',
        headless: false,
        args: ['--window-size=1300,760', '--window-position=100,100'],
      });
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });

  let result = null;
  let screenshotTaken = false;
  page.on('console', async (msg) => {
    const t = msg.text();
    if (t.includes('BENCHMARK_RESULT')) {
      result = t;
      console.log('RESULT:', t);
    }
    if (t.includes('[BENCHMARK]')) console.log(t);
    if (msg.type() === 'error' || msg.type() === 'warning' || t.toLowerCase().includes('error') || t.toLowerCase().includes('warn')) console.log('CONSOLE[' + msg.type() + ']:', t.substring(0, 500));
  });

  await page.goto(`http://127.0.0.1:${port}/`);

  // Screenshot mid-benchmark (after warmup, while FPS overlay is live).
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    await page.waitForTimeout(1000);
    if (!screenshotTaken && Date.now() > deadline - 60000 + (parseInt(process.argv[4] || '9000'))) {
      try {
        await page.screenshot({ path: `${outPrefix}.png`, timeout: 5000 });
        console.log('screenshot saved');
      } catch (e) {
        console.log('in-page screenshot failed (use window capture)');
      }
      screenshotTaken = true;
    }
    if (result && screenshotTaken) break;
  }
  if (!result) console.log('WARNING: no BENCHMARK_RESULT captured');
  await browser.close();
  server.close();
});
