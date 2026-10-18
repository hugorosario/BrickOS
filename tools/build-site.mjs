// A dependency-free static build: only the site and explicitly referenced captures are deployed.
import {readFile, writeFile, mkdir, copyFile, lstat} from 'node:fs/promises';
import {resolve, dirname, join, relative, isAbsolute} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
if (args.length && (args.length !== 2 || args[0] !== '--out')) throw new Error('Usage: node tools/build-site.mjs [--out DIR]');
const out = resolve(root, args[1] || 'dist/site');
const source = join(root, 'site');
const outputLocation = relative(join(root, 'dist'), out);
if (!outputLocation || outputLocation.startsWith('..') || isAbsolute(outputLocation)) throw new Error('Choose a dedicated output folder inside dist/');
const config = JSON.parse(await readFile(join(root, 'versions.json'), 'utf8'));
if (!/^\d+\.\d+\.\d+$/.test(config.release)) throw new Error('Expected a release version');
const siteUrl = new URL(process.env.SITE_URL || 'https://hugorosario.github.io/BrickOS/');
if (!['http:', 'https:'].includes(siteUrl.protocol)) throw new Error('SITE_URL must use HTTP or HTTPS');
siteUrl.protocol = 'https:';
if (siteUrl.username || siteUrl.password || siteUrl.search || siteUrl.hash) throw new Error('SITE_URL must be a public base URL without credentials, query or hash');
if (!siteUrl.pathname.endsWith('/')) siteUrl.pathname += '/';
const render = text => text.replaceAll('{{VERSION}}', config.release).replaceAll('{{SITE_URL}}', siteUrl.href);
let html = render(await readFile(join(source,'index.html'),'utf8'));
// Change asset URLs with their contents so returning browsers receive the current styles/scripts.
for (const file of ['styles.css', 'app.js']) {
  const revision = createHash('sha256').update(await readFile(join(source, file))).digest('hex').slice(0, 12);
  html = html.replaceAll(`"${file}"`, `"${file}?v=${revision}"`);
}
if (/\{\{[A-Z_]+\}\}/.test(html)) throw new Error('Unresolved website placeholder');
const images = [...new Set([...html.matchAll(/(?:screenshots\/|data-image=")([a-z0-9-]+\.png)/g)].map(m=>m[1]))];
if (!images.length || images.filter(n=>n.startsWith('stockpp-')).length <= images.length/2) throw new Error('Use a majority of Stock++ screenshots');
await mkdir(out,{recursive:true});
for (const name of ['styles.css','app.js','logo.png']) await copyFile(join(source,name),join(out,name));
await writeFile(join(out,'404.html'),render(await readFile(join(source,'404.html'),'utf8')));
await writeFile(join(out,'index.html'),html);
await writeFile(join(out,'.nojekyll'),'');
await mkdir(join(out,'screenshots'),{recursive:true});
for (const name of images) {
  const path=join(root,'docs/screenshots',name);
  if (!(await lstat(path)).isFile()) throw new Error(`Not a regular screenshot: ${name}`);
  const bytes=await readFile(path);
  if(bytes.readUInt32BE(16)!==1024||bytes.readUInt32BE(20)!==768) throw new Error(`Unexpected screenshot size: ${name}`);
  await copyFile(path,join(out,'screenshots',name));
}
console.log(`Built BrickOS v${config.release}: ${images.length} original device captures → ${out}`);
