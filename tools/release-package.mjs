#!/usr/bin/env node
// Writes a BrickOS release package: a zip whose contents mirror the root of the SD card.
// Includes the native GitHub updater, its Settings extension and deferred boot activation.
//
//   node tools/release-package.mjs [--out DIR]
//
// It collects the built input daemon, OSD, LED daemon, and menu library from bin/, the on-device setup,
// the component helpers, the start and launch templates, the boot script, every boot logo in
// assets/bootlogos/, every splash screen in assets/splashscreens/, every loading-screen preset in
// assets/loading-screens/, the Settings icons in data/icons/settings/, and the MainUI wallpapers in
// data/wallpapers/ with earlier.sha256, the list of their earlier versions. It also collects the
// Uninstall BrickOS app as uninstall/: its program from bin/, its app files from
// data/Apps/BrickOSUninstall/, its boot-time uninstall script, and the MainUI rollback helper, which
// the setup installs into /mnt/SDCARD/Apps/BrickOSUninstall/. The BrickOS MainUI goes in mainui/:
// its executable, its install helper, and its start and wrapper templates. GameSwitcher goes in
// gameswitcher/: its app, session supervisor and session library from bin/gameswitcher/, its entry
// script, its install helper, and its app files from data/Apps/GameSwitcher/ in app/, which the
// setup installs into /mnt/SDCARD/Apps/GameSwitcher/. Nothing is packaged under Apps/ or trimui/,
// and no acceptance receipt is packaged. Everything except the boot script and the marker goes under
// BrickOS/package/<build>/, whose manifest.txt lists each packaged file as
// "SHA256 SIZE PATH", the path relative to that folder and the lines sorted by path. The build
// identity is the first 12 hex digits of the manifest's SHA-256, so unchanged inputs give the
// same identity; the zip has fixed entry times, so they also give the same bytes. The marker
// BrickOS/enabled holds the build identity and is the one file outside the manifest. Nothing
// is taken from the stock firmware, and no stock version or hash is named.
import { createHash } from 'node:crypto';
import { lstat, mkdir, readdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import zlib from 'node:zlib';
import { checkGenerated, componentManifest, compareManifests } from './component-versions.mjs';

export const PACKAGE_DIR = 'BrickOS/package';
export const BOOT_SCRIPT = 'System/starts/brickos-00-setup.sh';
export const MARKER = 'BrickOS/enabled';
// The boot script as the manifest names it, relative to the package folder.
const BOOT_SCRIPT_FROM_PACKAGE = `../../../${BOOT_SCRIPT}`;
const VISUAL_BYTES_MAX = 16 * 1024 * 1024;
const SAFE_NAME = /^[A-Za-z0-9][A-Za-z0-9._ -]*$/;
// 1980-01-01 00:00, the earliest DOS time, for every entry.
const DOS_TIME = 0;
const DOS_DATE = (0 << 9) | (1 << 5) | 1;

// The packaged scripts and components: package path, source relative to the repository, kind.
export const PACKAGED_FILES = [
  ['updater/brickos-update', 'bin/brickos-update', 'elf'],
  ['updater/Updates.json', 'data/updater/Updates.json', 'data'],
  ['updater/icon.png', 'data/updater/icon.png', 'data'],
  ['updater/menu.sh', 'scripts/update-menu.sh', 'script'],
  ['updater/boot.sh', 'scripts/update-boot.sh', 'script'],
  ['setup.sh', 'scripts/brickos-setup.sh', 'script'],
  ['helpers/inputd-install.sh', 'scripts/inputd-install.sh', 'script'],
  ['helpers/osdd-install.sh', 'scripts/osdd-install.sh', 'script'],
  ['helpers/ledd-install.sh', 'scripts/ledd-install.sh', 'script'],
  ['helpers/tmenu-install.sh', 'scripts/tmenu-install.sh', 'script'],
  ['templates/brickos-inputd-start.sh', 'scripts/brickos-inputd-start.sh', 'script'],
  ['templates/brickos-inputd-launch.sh', 'scripts/brickos-inputd-launch.sh', 'script'],
  ['templates/brickos-osdd-start.sh', 'scripts/brickos-osdd-start.sh', 'script'],
  ['templates/brickos-osdd-launch.sh', 'scripts/brickos-osdd-launch.sh', 'script'],
  ['templates/brickos-ledd-start.sh', 'scripts/brickos-ledd-start.sh', 'script'],
  ['templates/brickos-ledd-launch.sh', 'scripts/brickos-ledd-launch.sh', 'script'],
  [BOOT_SCRIPT_FROM_PACKAGE, 'scripts/brickos-00-setup.sh', 'script'],
  ['bin/trimui_inputd', 'bin/trimui_inputd', 'elf'],
  ['bin/trimui_osdd', 'bin/trimui_osdd', 'elf'],
  ['bin/brickos-ledd', 'bin/brickos-ledd', 'elf'],
  ['bin/libtmenu.so', 'bin/tmenu/libtmenu.so', 'elf'],
  ['uninstall/brickos-uninstall', 'bin/brickos-uninstall', 'elf'],
  ['uninstall/launch.sh', 'data/Apps/BrickOSUninstall/launch.sh', 'script'],
  ['uninstall/uninstall.sh', 'scripts/brickos-uninstall-boot.sh', 'script'],
  ['uninstall/mainui-install.sh', 'scripts/mainui-install.sh', 'script'],
  ['uninstall/config.json', 'data/Apps/BrickOSUninstall/config.json', 'data'],
  ['uninstall/icon.png', 'data/Apps/BrickOSUninstall/icon.png', 'data'],
  ['mainui/MainUI', 'bin/MainUI', 'elf'],
  ['mainui/mainui-install.sh', 'scripts/mainui-install.sh', 'script'],
  ['mainui/brickos-mainui-start.sh', 'scripts/brickos-mainui-start.sh', 'script'],
  ['mainui/brickos-mainui-wrapper.sh', 'scripts/brickos-mainui-wrapper.sh', 'script'],
  ['gameswitcher/GameSwitcher', 'bin/gameswitcher/GameSwitcher', 'elf'],
  ['gameswitcher/gs-session', 'bin/gameswitcher/gs-session', 'elf'],
  ['gameswitcher/libtmenu.so', 'bin/gameswitcher/libtmenu.so', 'elf'],
  ['gameswitcher/entry.sh', 'scripts/gameswitcher-entry.sh', 'script'],
  ['gameswitcher/gameswitcher-setup.sh', 'scripts/gameswitcher-setup.sh', 'script'],
  ['gameswitcher/app/config.json', 'data/Apps/GameSwitcher/config.json', 'data'],
  ['gameswitcher/app/launch.sh', 'data/Apps/GameSwitcher/launch.sh', 'script'],
  ['gameswitcher/app/icon.png', 'data/Apps/GameSwitcher/icon.png', 'data'],
];
// Folders whose images are packaged: package folder, source folder, image format.
export const PACKAGED_FOLDERS = [
  ['bootlogos', 'assets/bootlogos', 'bmp'],
  ['splashscreens', 'assets/splashscreens', 'png'],
  ['icons/settings', 'data/icons/settings', 'png'],
  ['wallpapers', 'data/wallpapers', 'png'],
];
// The wallpapers folder's list of the earlier versions BrickOS shipped, in sha256sum form: the
// setup replaces a wallpaper only when the card holds one of them.
export const WALLPAPERS_EARLIER = 'earlier.sha256';

// Folders of loading-screen presets: package folder, source folder. Each subfolder is one set,
// packaged whole under <package folder>/<preset>/; a missing source folder gives no presets.
export const PACKAGED_SETS = [
  ['loading-screens', 'assets/loading-screens'],
];
export const LOADING_BACKGROUND = 'app_loading_bg.png';
const LOADING_FRAMES_MAX = 99;

const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

function little32(bytes, offset) { return bytes.readUInt32LE(offset); }

// The same checks the frontend applies before listing a boot logo or splash screen.
export function imageFormat(bytes) {
  if (bytes.length === 0 || bytes.length > VISUAL_BYTES_MAX) return null;
  if (bytes.length >= 54 && bytes[0] === 0x42 && bytes[1] === 0x4d) {
    const dib = little32(bytes, 14), pixelOffset = little32(bytes, 10);
    const width = bytes.readInt32LE(18), height = bytes.readInt32LE(22);
    const planes = bytes.readUInt16LE(26), bits = bytes.readUInt16LE(28), compression = little32(bytes, 30);
    const rows = Math.abs(height);
    if (dib < 40 || width <= 0 || height === 0 || width > 4096 || rows > 4096 || planes !== 1 ||
        (bits !== 24 && bits !== 32) || (compression !== 0 && !(compression === 3 && bits === 32))) return null;
    let headerEnd = 14 + dib;
    if (compression === 3) {
      if ((dib !== 40 && dib < 52) || bytes.length < 66) return null;
      const red = little32(bytes, 54), green = little32(bytes, 58), blue = little32(bytes, 62);
      if (!red || !green || !blue || (red & green) || (red & blue) || (green & blue)) return null;
      headerEnd = Math.max(headerEnd, 66);
    }
    if (pixelOffset < headerEnd || pixelOffset > bytes.length) return null;
    const rowBytes = Math.ceil((width * bits) / 32) * 4;
    return rowBytes * rows <= bytes.length - pixelOffset ? 'bmp' : null;
  }
  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  if (bytes.length >= 24 && bytes.subarray(0, 8).equals(signature) && bytes.toString('ascii', 12, 16) === 'IHDR') {
    const width = bytes.readUInt32BE(16), height = bytes.readUInt32BE(20);
    return width && height && width <= 4096 && height <= 4096 ? 'png' : null;
  }
  return null;
}

// The rules MainUI applies before listing a loading-screen preset: a 1024x768 PNG background and
// PNG frames from app_loading_01.png, each at most 1024x768 and 16 MiB, up to the first missing
// number. read(name) gives a file's bytes, or null when the preset has no file of that name.
// Returns the set's files in order; other files in the preset are never read. Throws on a broken set.
export async function loadingScreenSet(read) {
  const pngSize = bytes => (imageFormat(bytes) === 'png' ? { width: bytes.readUInt32BE(16), height: bytes.readUInt32BE(20) } : null);
  const background = await read(LOADING_BACKGROUND);
  const size = background && pngSize(background);
  if (!size) throw new Error(`${LOADING_BACKGROUND} is missing or not a valid PNG`);
  if (size.width !== 1024 || size.height !== 768) throw new Error(`${LOADING_BACKGROUND} is not 1024x768`);
  const files = [{ name: LOADING_BACKGROUND, bytes: background }];
  for (let frame = 1; frame <= LOADING_FRAMES_MAX; ++frame) {
    const name = `app_loading_${String(frame).padStart(2, '0')}.png`;
    const bytes = await read(name);
    if (!bytes) break;
    const frameSize = pngSize(bytes);
    if (!frameSize) throw new Error(`${name} is not a valid PNG`);
    if (frameSize.width > 1024 || frameSize.height > 768) throw new Error(`${name} is larger than 1024x768`);
    files.push({ name, bytes });
  }
  if (files.length === 1) throw new Error('app_loading_01.png is missing');
  return files;
}

// Reads a preset's files by name: null when absent, an error for anything but a regular file.
const presetReader = folder => async name => {
  let info;
  try {
    info = await lstat(join(folder, name));
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
  if (!info.isFile()) throw new Error(`${name} is not a regular file`);
  return readFile(join(folder, name));
};

export function assertAarch64(bytes, name) {
  if (bytes.length < 64 || bytes.toString('latin1', 0, 4) !== '\x7fELF' || bytes[4] !== 2 || bytes[5] !== 1 ||
      bytes.readUInt16LE(18) !== 183) throw new Error(`${name} is not a Linux AArch64 ELF64 file`);
}

// Reads every input and checks it: components are AArch64 ELFs, scripts have LF endings,
// images and JSON data are valid, and names are plain. Returns [{ path, bytes, mode }] with
// package paths.
export async function collectFiles({ root = process.cwd(), sources = {} } = {}) {
  const versions = checkGenerated(root);
  const files = [];
  for (const [path, source, kind] of PACKAGED_FILES) {
    const location = resolve(root, sources[source] || source);
    let bytes;
    try { bytes = await readFile(location); } catch { throw new Error(`missing ${source}; run the build first`); }
    if (kind === 'elf') assertAarch64(bytes, source);
    if (kind === 'script') {
      if (bytes.includes(13)) throw new Error(`${source} has CRLF line endings`);
      if (!bytes.toString('latin1').startsWith('#!/bin/sh\n')) throw new Error(`${source} does not start with #!/bin/sh`);
    }
    if (kind === 'data' && path.endsWith('.png') && imageFormat(bytes) !== 'png') throw new Error(`${source} is not a valid PNG`);
    if (kind === 'data' && path.endsWith('.json')) {
      try { JSON.parse(bytes.toString('utf8')); } catch { throw new Error(`${source} is not valid JSON`); }
    }
    files.push({ path, bytes, mode: kind === 'script' || kind === 'elf' ? 0o755 : 0o644 });
  }
  for (const [folder, source, format] of PACKAGED_FOLDERS) {
    const location = resolve(root, sources[source] || source);
    const names = (await readdir(location, { withFileTypes: true })).filter(entry => entry.isFile()).map(entry => entry.name);
    if (folder === 'wallpapers') {
      if (!names.includes(WALLPAPERS_EARLIER)) throw new Error(`${source}/${WALLPAPERS_EARLIER} is missing`);
      const bytes = await readFile(join(location, WALLPAPERS_EARLIER));
      const text = bytes.toString('utf8');
      if (text && (!text.endsWith('\n') || text.includes('\r')))
        throw new Error(`${source}/${WALLPAPERS_EARLIER} must be LF lines ending in a newline`);
      for (const line of text ? text.slice(0, -1).split('\n') : []) {
        const match = /^[0-9a-f]{64} {2}(.+\.png)$/.exec(line);
        if (!match || !SAFE_NAME.test(match[1])) throw new Error(`${source}/${WALLPAPERS_EARLIER} has a bad line: ${line}`);
      }
      files.push({ path: `${folder}/${WALLPAPERS_EARLIER}`, bytes, mode: 0o644 });
    }
    for (const name of names.sort()) {
      // The Settings icon and wallpaper folders also hold the prompts their images were made from.
      if ((folder === 'icons/settings' || folder === 'wallpapers') && !name.toLowerCase().endsWith('.png')) continue;
      if (!name.toLowerCase().endsWith(`.${format}`)) throw new Error(`${source}/${name} is not a .${format} file`);
      if (!SAFE_NAME.test(name)) throw new Error(`${source}/${name} has an unsupported file name`);
      const bytes = await readFile(join(location, name));
      if (imageFormat(bytes) !== format) throw new Error(`${source}/${name} is not a valid ${format.toUpperCase()}`);
      files.push({ path: `${folder}/${name}`, bytes, mode: 0o644 });
    }
  }
  for (const [folder, source] of PACKAGED_SETS) {
    const location = resolve(root, sources[source] || source);
    let entries;
    try {
      entries = await readdir(location, { withFileTypes: true });
    } catch (error) {
      if (error.code === 'ENOENT') continue;
      throw error;
    }
    // Like MainUI, loose files and hidden entries beside the presets are not presets.
    const presets = entries.filter(entry => entry.isDirectory() && !entry.name.startsWith('.')).map(entry => entry.name);
    for (const name of presets.sort()) {
      if (!SAFE_NAME.test(name)) throw new Error(`${source}/${name} has an unsupported preset name`);
      let set;
      try {
        set = await loadingScreenSet(presetReader(join(location, name)));
      } catch (error) {
        throw new Error(`loading-screen preset ${name} in ${source}: ${error.message}`);
      }
      for (const file of set) files.push({ path: `${folder}/${name}/${file.name}`, bytes: file.bytes, mode: 0o644 });
    }
  }
  const metadata = componentManifest(files, versions);
  files.push({ path: 'versions.json', bytes: Buffer.from(JSON.stringify(metadata, null, 2) + '\n'), mode: 0o644 });
  return files;
}

const byPath = (a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);

export function buildManifest(files) {
  return [...files].sort(byPath).map(file => `${sha256(file.bytes)} ${file.bytes.length} ${file.path}\n`).join('');
}

export function buildIdentity(manifest) { return sha256(Buffer.from(manifest)).slice(0, 12); }

// The zip entries in the SD-root layout, folders included, in a fixed order.
export function packageEntries(files) {
  const manifest = buildManifest(files);
  const build = buildIdentity(manifest);
  const base = `${PACKAGE_DIR}/${build}`;
  const entries = files.map(file => ({
    name: file.path === BOOT_SCRIPT_FROM_PACKAGE ? BOOT_SCRIPT : `${base}/${file.path}`, data: file.bytes, mode: file.mode }));
  entries.push({ name: `${base}/manifest.txt`, data: Buffer.from(manifest), mode: 0o644 });
  entries.push({ name: MARKER, data: Buffer.from(`${build}\n`), mode: 0o644 });
  const folders = new Set();
  for (const entry of entries) {
    const parts = entry.name.split('/');
    for (let i = 1; i < parts.length; ++i) folders.add(`${parts.slice(0, i).join('/')}/`);
  }
  for (const entry of entries) {
    if (entry.name.startsWith('trimui/') || entry.name.startsWith('Apps/') || entry.name.includes('/usr/trimui') ||
        entry.name.includes('..'))
      throw new Error(`refusing to package ${entry.name}`);
  }
  const all = [...[...folders].map(name => ({ name, data: Buffer.alloc(0), mode: 0o755, folder: true })), ...entries];
  all.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return { build, manifest, entries: all };
}

function crc32(bytes) {
  if (zlib.crc32) return zlib.crc32(bytes) >>> 0;
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// A plain zip writer: deflate when it helps, Unix modes, and fixed times.
export function createZip(entries) {
  const locals = [], centrals = [];
  let offset = 0;
  for (const entry of entries) {
    const name = Buffer.from(entry.name, 'utf8');
    const crc = crc32(entry.data);
    const deflated = entry.data.length ? zlib.deflateRawSync(entry.data, { level: 9 }) : entry.data;
    const method = deflated.length < entry.data.length ? 8 : 0;
    const body = method === 8 ? deflated : entry.data;
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50, 0);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(0x0800, 6); // UTF-8 names
    header.writeUInt16LE(method, 8);
    header.writeUInt16LE(DOS_TIME, 10);
    header.writeUInt16LE(DOS_DATE, 12);
    header.writeUInt32LE(crc, 14);
    header.writeUInt32LE(body.length, 18);
    header.writeUInt32LE(entry.data.length, 22);
    header.writeUInt16LE(name.length, 26);
    header.writeUInt16LE(0, 28);
    locals.push(header, name, body);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE((3 << 8) | 20, 4); // made by Unix
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(method, 10);
    central.writeUInt16LE(DOS_TIME, 12);
    central.writeUInt16LE(DOS_DATE, 14);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(body.length, 20);
    central.writeUInt32LE(entry.data.length, 24);
    central.writeUInt16LE(name.length, 28);
    const mode = (entry.folder ? 0o040000 : 0o100000) | entry.mode;
    central.writeUInt32LE(((mode << 16) | (entry.folder ? 0x10 : 0)) >>> 0, 38);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, name);
    offset += header.length + name.length + body.length;
  }
  const directory = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, directory, end]);
}

// Reads back a zip written by createZip, checking each entry's CRC.
export function readZip(zip) {
  const end = zip.length - 22;
  if (zip.readUInt32LE(end) !== 0x06054b50) throw new Error('not a zip written by this tool');
  const count = zip.readUInt16LE(end + 10);
  let position = zip.readUInt32LE(end + 16);
  const entries = [];
  for (let i = 0; i < count; ++i) {
    if (zip.readUInt32LE(position) !== 0x02014b50) throw new Error('broken central directory');
    const method = zip.readUInt16LE(position + 10);
    const time = zip.readUInt16LE(position + 12), date = zip.readUInt16LE(position + 14);
    const crc = zip.readUInt32LE(position + 16), size = zip.readUInt32LE(position + 20);
    const nameLength = zip.readUInt16LE(position + 28), extraLength = zip.readUInt16LE(position + 30);
    const commentLength = zip.readUInt16LE(position + 32);
    const mode = zip.readUInt32LE(position + 38) >>> 16;
    const local = zip.readUInt32LE(position + 42);
    const name = zip.toString('utf8', position + 46, position + 46 + nameLength);
    const start = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
    const body = zip.subarray(start, start + size);
    const data = method === 8 ? zlib.inflateRawSync(body) : Buffer.from(body);
    if (crc32(data) !== crc) throw new Error(`CRC mismatch for ${name}`);
    entries.push({ name, data, time, date, mode, folder: name.endsWith('/') });
    position += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

// Checks a package's entries as the setup does: the manifest names the build, every manifest
// line matches its file, every file is in the manifest, and the marker names the build.
export function verifyEntries(entries) {
  const files = new Map(entries.filter(entry => !entry.folder).map(entry => [entry.name, entry.data]));
  const manifests = [...files.keys()].filter(name => /^BrickOS\/package\/[0-9a-f]{12}\/manifest\.txt$/.test(name));
  if (manifests.length !== 1) throw new Error(`expected one package manifest, found ${manifests.length}`);
  const base = manifests[0].slice(0, -'/manifest.txt'.length);
  const build = base.slice(PACKAGE_DIR.length + 1);
  const manifest = files.get(manifests[0]).toString('utf8');
  if (buildIdentity(manifest) !== build) throw new Error('the manifest does not match the build identity');
  if ((files.get(MARKER) || '').toString() !== `${build}\n`) throw new Error('the marker does not name the build');
  const listed = new Set([manifests[0], MARKER]);
  for (const line of manifest.trimEnd().split('\n')) {
    const match = line.match(/^([0-9a-f]{64}) (\d+) (.+)$/);
    if (!match) throw new Error(`bad manifest line: ${line}`);
    const name = match[3] === BOOT_SCRIPT_FROM_PACKAGE ? BOOT_SCRIPT : `${base}/${match[3]}`;
    const data = files.get(name);
    if (!data) throw new Error(`${name} is in the manifest but not in the package`);
    if (data.length !== Number(match[2]) || sha256(data) !== match[1]) throw new Error(`${name} does not match the manifest`);
    listed.add(name);
  }
  for (const name of files.keys()) if (!listed.has(name)) throw new Error(`${name} is not in the manifest`);
  return { build, manifest };
}

// Builds the package and writes it to OUT/brickos-<build>.zip, only once it verifies.
export async function createPackage({ root = process.cwd(), out = 'dist', sources, previous } = {}) {
  const files = await collectFiles({ root, sources });
  const versions = JSON.parse(files.find(file => file.path === 'versions.json').bytes);
  if (previous) {
    const bytes = await readFile(resolve(root, previous));
    let old;
    if (previous.endsWith('.zip')) {
      const entries = readZip(bytes); verifyEntries(entries);
      const entry = entries.find(item => /^BrickOS\/package\/[0-9a-f]{12}\/versions\.json$/.test(item.name));
      if (!entry) throw new Error('The previous package has no component versions');
      old = JSON.parse(entry.data);
    } else old = JSON.parse(bytes);
    compareManifests(old, versions);
  }
  const { build, manifest, entries } = packageEntries(files);
  const zip = createZip(entries);
  verifyEntries(readZip(zip));
  const directory = resolve(root, out);
  const target = join(directory, `brickos-${build}.zip`);
  const temporary = `${target}.${process.pid}.tmp`;
  await mkdir(directory, { recursive: true });
  try {
    await writeFile(temporary, zip);
    await rename(temporary, target);
  } finally {
    await rm(temporary, { force: true });
  }
  return { build, path: target, manifest, release: versions.release, versions, bytes: zip.length, files: files.length };
}

async function main() {
  const args = process.argv.slice(2);
  const options = { out: 'dist' }, seen = new Set();
  for (let i = 0; i < args.length; ++i) {
    const flag = args[i];
    if (!['--out', '--previous'].includes(flag) || seen.has(flag) || !args[i + 1] || args[i + 1].startsWith('--'))
      throw new Error('usage: release-package.mjs [--out DIR] [--previous VERSIONED.zip|versions.json]');
    seen.add(flag); options[flag === '--out' ? 'out' : 'previous'] = args[++i];
  }
  const result = await createPackage(options);
  console.log(`package ${result.path}`);
  console.log(`build ${result.build}`);
  console.log(`release ${result.release}`);
  console.log(`files ${result.files}, ${result.bytes} bytes`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(`release package failed: ${error.message}`); process.exitCode = 1; });
}
