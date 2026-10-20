#!/usr/bin/env node
// The short per-release changelog in CHANGELOG.md: one "## <version>" entry per release, newest
// first, with one to six plain-text bullets. The device shows an entry as it is, so it holds no
// Markdown beyond the bullet.
//
//   node tools/changelog.mjs check            validate CHANGELOG.md
//   node tools/changelog.mjs notes <version>  print the entry as the release-notes block
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseVersion, compareVersions } from './component-versions.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
export const FILE = 'CHANGELOG.md';
// Invisible on GitHub; the updater shows the bullets between them.
export const OPEN = '<!-- brickos-changelog -->';
export const CLOSE = '<!-- /brickos-changelog -->';
export const MAX_BULLETS = 6;
export const MAX_BULLET_LENGTH = 160;

// The Markdown a bullet must not use, each with what the check calls it.
const MARKUP = [
  [/[*`]/, 'Markdown emphasis or code'],
  [/(^|[\s(])_[^_\s]|[^_\s]_($|[\s.,;:!?)])/, 'Markdown emphasis'],
  [/[[\]]/, 'a Markdown link'],
  [/<[A-Za-z/!]/, 'HTML'],
];

// Reads the file's text into entries and a list of problems; an empty list means valid.
export function parseChangelog(text) {
  const entries = [], problems = [];
  let entry = null, title = false;
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  lines.forEach((line, index) => {
    const at = `line ${index + 1}`;
    const heading = line.match(/^## (.*)$/);
    if (heading) {
      const version = heading[1].trim();
      try { parseVersion(version); }
      catch { problems.push(`${at}: "${version}" is not a version (write 0.1.5, without a v)`); }
      entry = { version, bullets: [] };
      entries.push(entry);
      return;
    }
    if (!line.trim()) return;
    if (!entry) {
      if (line === '# Changelog' && !title) title = true;
      else problems.push(`${at}: only a "# Changelog" title may come before the first entry`);
      return;
    }
    if (!line.startsWith('- ')) {
      problems.push(`${entry.version}: ${at} is not a "- " bullet`);
      return;
    }
    entry.bullets.push(line);
  });
  const seen = new Set();
  entries.forEach((e, i) => {
    if (seen.has(e.version)) problems.push(`${e.version}: the version appears twice`);
    seen.add(e.version);
    const previous = entries[i - 1];
    try {
      if (previous && compareVersions(previous.version, e.version) <= 0)
        problems.push(`${e.version}: comes after ${previous.version}; entries go newest first`);
    } catch {}
    if (!e.bullets.length || e.bullets.length > MAX_BULLETS)
      problems.push(`${e.version}: has ${e.bullets.length} bullets; write 1 to ${MAX_BULLETS}`);
    e.bullets.forEach((bullet, b) => {
      const body = bullet.slice(2), name = `${e.version}: bullet ${b + 1}`;
      if (!body.trim()) problems.push(`${name} is empty`);
      if ([...body].length > MAX_BULLET_LENGTH)
        problems.push(`${name} has ${[...body].length} characters; keep it to ${MAX_BULLET_LENGTH}`);
      for (const [pattern, what] of MARKUP)
        if (pattern.test(body)) problems.push(`${name} uses ${what}: ${body}`);
    });
  });
  return { entries, problems };
}

function read(root, file = resolve(root, FILE)) {
  return readFileSync(file, 'utf8');
}

// The valid file's entry for version; throws with every problem otherwise. file defaults to the
// root's CHANGELOG.md.
export function changelogEntry(version, root = ROOT, file) {
  const { entries, problems } = parseChangelog(read(root, file));
  if (problems.length) throw new Error(`${FILE} is not valid:\n${problems.join('\n')}`);
  const entry = entries.find(e => e.version === version);
  if (!entry) throw new Error(`${FILE} has no entry for ${version}; add "## ${version}" with its bullets`);
  return entry;
}

// The entry as the block for the release's GitHub notes.
export function notesBlock(entry) {
  return [OPEN, ...entry.bullets, CLOSE].join('\n') + '\n';
}

export function main(args = process.argv.slice(2), root = ROOT, out = process.stdout) {
  const [command, version] = args;
  if (command === 'check') {
    const { entries, problems } = parseChangelog(read(root));
    if (problems.length) throw new Error(`${FILE} is not valid:\n${problems.join('\n')}`);
    out.write(`${FILE} is valid: ${entries.length} entries, newest ${entries[0]?.version ?? 'none'}.\n`);
    return;
  }
  if (command === 'notes' && version) {
    out.write(notesBlock(changelogEntry(version, root)));
    return;
  }
  throw new Error('usage: node tools/changelog.mjs check | notes <version>');
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { main(); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
