// Minimal static server with COOP/COEP for Godot web exports.
// Usage: node serve_dir.mjs <dir> <port>
import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join, extname, resolve } from 'path';

const dir = resolve(process.argv[2]);
const port = parseInt(process.argv[3] || '8971');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.pck': 'application/octet-stream', '.png': 'image/png' };

createServer((req, res) => {
  let p = req.url.split('?')[0];
  if (p === '/') p = '/index.html';
  const f = join(dir, p);
  if (!existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, {
    'Content-Type': MIME[extname(f)] || 'application/octet-stream',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
  });
  res.end(readFileSync(f));
}).listen(port, '127.0.0.1', () => console.log(`serving ${dir} on ${port}`));
