#!/usr/bin/env node
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Stage 1 of the Pachislop port converter (S84 R19, §6.7.1): evaluates the presets of a Pachislop
// clone (by SafEight, used with the author's permission) with Pachislop's own code and writes
// <scratch>/pachislop-dump.json, the input of `jasperengine-editor title-library port-pachislop`.
//
// Host only: run on purpose, never in CI or Docker. The modules (esbuild, three, opentype.js,
// typescript, pinned in package.json and package-lock.json) are installed outside the repository:
//
//   mkdir -p /tmp/pachislop-port && cp package.json package-lock.json /tmp/pachislop-port/
//   (cd /tmp/pachislop-port && npm ci --ignore-scripts)
//   node dump.mjs --modules /tmp/pachislop-port/node_modules --pachislop <clone> --scratch <dir> [--check]
//   node dump.mjs --self-test
//   node dump.mjs --sanitise <dump.json>
//
// --modules is the node_modules directory (or the directory holding it); without it dump.mjs uses
// a node_modules next to itself (`npm ci` here; .gitignore keeps it out of git). --check compares
// the result with the committed dump/pachislop-<commit>.json and fails when they differ (AC12:
// run it twice into two scratch directories, then `cmp` the two dumps).
// --commit <sha> (default 1c32502) is the clone commit expected; its HEAD is read from .git
// without running git in the clone. The dump's meta.commit is that value.
//
// What it does, in order:
// - refuses a scratch directory inside this repository (this checkout and, for a worktree, the
//   main checkout), inside the clone, or holding it; and a modules directory inside the clone;
// - copies the clone's src/ into <scratch>/src (a fresh copy every run; symbolic links refused)
//   and entry.ts into <scratch>/entry.ts; the clone is only read, never built, installed or run
//   (its scripts, npm install and tests are not used);
// - bundles entry.ts against that copy with esbuild (platform node; import.meta.env.BASE_URL "/",
//   DEV false, PROD true, MODE "production"; three and opentype.js from --modules). EXPOSE below
//   lists module-private tables entry.ts reads: they are exported from the copy under `__dump_`
//   names at bundle time (the copied files on disk stay byte-identical to the clone's);
// - checks that every bundled input is the copy, entry.ts or the modules directory;
// - runs the bundle in a child Node with the permission model (reads and writes only the scratch
//   directory and this script; no child processes, workers or addons) under the shims of
//   installShims(): `document`, `window`, `navigator` and `fetch` throw when touched (read, set,
//   called, `in`); `typeof` still works, and three.js's `window.__THREE__` instance mark is
//   the one member let through, since three.js sets it on any `window` it finds; `Math.random`
//   and `crypto.randomUUID` are a seeded counter (uid() gives 00000001, 00000002 ...), and
//   `crypto.getRandomValues` throws, so every id and random choice is reproducible;
// - writes the dump: keys sorted (integer keys ascending, as JavaScript orders them), 2-space
//   indent, a final newline; a function, symbol, non-finite number, cycle or non-plain object
//   left in the data fails the run instead of being dropped;
// - sanitises it (S84 Deviation 34: no denied term is written in the repository): every string
//   value or member name holding a term of ../denied_terms.txt (matched as the library's
//   DeniedTerms.h does) becomes "sha256:" and the hex SHA-256 of its UTF-8, the form
//   pachislop-map.json keys such ids and texts by and the port recognises.
//
// --sanitise rewrites a dump in place with the sanitiser below (how the committed dump of 1c32502
// was made from its earlier, unsanitised form; a fresh run sanitises by itself).
//
// --self-test needs no clone: it runs the refusals (scratch inside the repository, inside a
// clone), an accepted scratch outside both, the shims in a fresh Node (a touched `document`
// throws; window.__THREE__ passes; navigator and fetch throw; the seeded counter repeats across
// processes) and the sanitiser, printing PASS / FAIL lines; it exits 1 when any fails.

import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const SELF = fileURLToPath(import.meta.url);
const HERE = path.dirname(SELF);
const DEFAULT_COMMIT = '1c32502';
const DUMP = 'pachislop-dump.json';
const BUNDLE = 'pachislop-dump.bundle.mjs';
const COPY_MARK = '.pachislop-port-copy';

/** Module-private tables of the copied src/ that entry.ts reads (as `__dump_<name>`). */
const EXPOSE = {
  'presets/colourways.ts': [
    'COLOURWAY_DEFS',
    'LIGHT',
    'RAMPS',
    'DARK_LUMA',
    'WHITE_CORE',
    'ON_RAMP',
    'CORE_SPAN',
    'PATTERN_GROUND',
    'SPECTRUM_FILL',
    'OWN_COLOURS',
    'LIGHT_EFFECTS',
  ],
  'presets/factory.ts': ['orbBody'],
  'model/defaults.ts': ['PRESET_VALUES'],
};

class Refusal extends Error {}

// ---------- shims (the bundle's banner; self-contained, it runs as source text) ----------

function installShims() {
  const trap = (name, target, allowed) => {
    const kept = {};
    const touched = (what) => {
      throw new Error(`pachislop-port shim: ${what}; the dump runs without a browser`);
    };
    const handler = {
      get: (_t, k) => (allowed.includes(k) ? kept[k] : touched(`${name}.${String(k)} read`)),
      set: (_t, k, v) => (allowed.includes(k) ? ((kept[k] = v), true) : touched(`${name}.${String(k)} set`)),
      has: (_t, k) => touched(`'${String(k)}' in ${name}`),
      deleteProperty: (_t, k) => touched(`delete ${name}.${String(k)}`),
      defineProperty: (_t, k) => touched(`${name}.${String(k)} defined`),
      ownKeys: () => touched(`${name}'s keys listed`),
      getOwnPropertyDescriptor: (_t, k) => touched(`${name}.${String(k)} described`),
      getPrototypeOf: () => touched(`${name}'s prototype read`),
      apply: () => touched(`${name}() called`),
      construct: () => touched(`new ${name}()`),
    };
    return new Proxy(target, handler);
  };
  const globals = [
    ['document', {}, []],
    // three.js marks the `window` it finds with its revision (typeof window !== 'undefined').
    ['window', {}, ['__THREE__']],
    ['navigator', {}, []],
    ['fetch', function fetch() {}, []],
  ];
  for (const [name, target, allowed] of globals)
    Object.defineProperty(globalThis, name, { value: trap(name, target, allowed), configurable: true, writable: false, enumerable: false });
  let draws = 0;
  // mulberry32 over a counter: a seeded, reproducible sequence in [0, 1).
  Math.random = () => {
    let t = (0x5eed84 + Math.imul(++draws, 0x6d2b79f5)) | 0;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  let ids = 0;
  const c = globalThis.crypto;
  Object.defineProperty(c, 'randomUUID', {
    configurable: true,
    value: () => `${(++ids).toString(16).padStart(8, '0')}-0000-4000-8000-000000000000`,
  });
  Object.defineProperty(c, 'getRandomValues', {
    configurable: true,
    value: () => {
      throw new Error('pachislop-port shim: crypto.getRandomValues called; the dump must be reproducible');
    },
  });
}

const SHIM_SOURCE = `(${installShims.toString()})();`;

// ---------- paths ----------

/** `p` resolved through the symbolic links of its nearest existing ancestor (it may not exist yet). */
function real(p) {
  const abs = path.resolve(p);
  let head = abs;
  const rest = [];
  while (!fs.existsSync(head)) {
    rest.unshift(path.basename(head));
    const up = path.dirname(head);
    if (up === head) break;
    head = up;
  }
  return path.join(fs.realpathSync(head), ...rest);
}

function inside(child, parent) {
  const rel = path.relative(parent, child);
  return rel === '' || (rel !== '..' && !rel.startsWith(`..${path.sep}`) && !path.isAbsolute(rel));
}

/** This checkout's root and, when it is a worktree, the main checkout's. */
function repoRoots() {
  let d = HERE;
  while (!fs.existsSync(path.join(d, '.git'))) {
    const up = path.dirname(d);
    if (up === d) throw new Error(`${SELF} is not inside a git checkout`);
    d = up;
  }
  const roots = [d];
  const dotgit = path.join(d, '.git');
  if (fs.statSync(dotgit).isFile()) {
    const m = /^gitdir:\s*(.+)$/m.exec(fs.readFileSync(dotgit, 'utf8'));
    const gitdir = m ? path.resolve(d, m[1].trim()) : '';
    const i = gitdir.lastIndexOf(`${path.sep}.git${path.sep}`);
    if (i >= 0) roots.push(gitdir.slice(0, i));
  }
  return roots.map(real);
}

/** The real scratch path, or a Refusal. */
function checkScratch(scratch, clone) {
  const s = real(scratch);
  for (const root of repoRoots()) if (inside(s, root)) throw new Refusal(`scratch directory ${s} is inside the repository ${root}`);
  if (s === path.parse(s).root || s === real(os.homedir())) throw new Refusal(`scratch directory ${s} is the filesystem root or the home directory`);
  if (clone !== undefined) {
    const c = real(clone);
    if (inside(s, c)) throw new Refusal(`scratch directory ${s} is inside the Pachislop clone ${c}`);
    if (inside(c, s)) throw new Refusal(`the Pachislop clone ${c} is inside the scratch directory ${s}`);
  }
  return s;
}

/** The node_modules directory to bundle from. */
function modulesDir(opt, clone) {
  const candidates = opt ? [path.join(opt, 'node_modules'), opt] : [path.join(HERE, 'node_modules')];
  const dir = candidates.find((d) => fs.existsSync(path.join(d, 'esbuild', 'package.json')));
  if (!dir) throw new Refusal(`no esbuild in ${candidates.join(' or ')}: install package-lock.json with npm ci (see the header of dump.mjs)`);
  const m = real(dir);
  if (inside(m, real(clone))) throw new Refusal(`modules directory ${m} is inside the Pachislop clone`);
  return m;
}

// ---------- the clone ----------

/** The clone's HEAD commit, read from its .git (git itself is not run in an untrusted clone). */
function cloneHead(clone) {
  let gitdir = path.join(clone, '.git');
  if (fs.statSync(gitdir).isFile()) gitdir = path.resolve(clone, /^gitdir:\s*(.+)$/m.exec(fs.readFileSync(gitdir, 'utf8'))[1].trim());
  const common = fs.existsSync(path.join(gitdir, 'commondir')) ? path.resolve(gitdir, fs.readFileSync(path.join(gitdir, 'commondir'), 'utf8').trim()) : gitdir;
  const head = fs.readFileSync(path.join(gitdir, 'HEAD'), 'utf8').trim();
  const ref = /^ref:\s*(\S+)$/.exec(head)?.[1];
  if (!ref) return head;
  for (const dir of [gitdir, common]) {
    const f = path.join(dir, ...ref.split('/'));
    if (fs.existsSync(f)) return fs.readFileSync(f, 'utf8').trim();
  }
  const packed = path.join(common, 'packed-refs');
  const line = fs.existsSync(packed) ? fs.readFileSync(packed, 'utf8').split('\n').find((l) => l.endsWith(` ${ref}`)) : undefined;
  if (!line) throw new Refusal(`cannot resolve ${ref} in ${common}`);
  return line.split(' ')[0];
}

/** Copies `from` into `to` (regular files and directories only). */
function copyTree(from, to) {
  fs.mkdirSync(to, { recursive: true });
  for (const name of fs.readdirSync(from).sort()) {
    const a = path.join(from, name);
    const b = path.join(to, name);
    const st = fs.lstatSync(a);
    if (st.isSymbolicLink()) throw new Refusal(`${a} is a symbolic link`);
    if (st.isDirectory()) copyTree(a, b);
    else if (st.isFile()) fs.copyFileSync(a, b);
    else throw new Refusal(`${a} is not a regular file`);
  }
}

// ---------- serialisation ----------

/** `v` as plain JSON data with sorted keys; anything JSON would drop or mangle fails. */
function canonical(v, at = '$', open = new Set()) {
  if (v === null || typeof v === 'string' || typeof v === 'boolean') return v;
  if (typeof v === 'number') {
    if (!Number.isFinite(v)) throw new Error(`${at}: ${v} is not a finite number`);
    return Object.is(v, -0) ? 0 : v;
  }
  if (typeof v !== 'object') throw new Error(`${at}: a ${typeof v} is not data`);
  if (open.has(v)) throw new Error(`${at}: a cycle`);
  open.add(v);
  let out;
  if (Array.isArray(v)) {
    out = Array.from(v, (x, i) => {
      if (x === undefined) throw new Error(`${at}[${i}]: undefined in an array`);
      return canonical(x, `${at}[${i}]`, open);
    });
  } else {
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) throw new Error(`${at}: a ${proto?.constructor?.name ?? 'non-plain object'} is not plain data`);
    if (Object.getOwnPropertySymbols(v).length) throw new Error(`${at}: symbol-keyed members`);
    out = {};
    for (const k of Object.keys(v).sort()) if (v[k] !== undefined) out[k] = canonical(v[k], `${at}.${k}`, open);
  }
  open.delete(v);
  return out;
}

const serialise = (v) => `${JSON.stringify(canonical(v), null, 2)}\n`;

// ---------- denied terms (S84 R38, Deviation 34) ----------

/** The terms of a denied_terms.txt text, as DeniedTerms.h parses them: lowercase (ASCII), unique, in order. */
function parseDeniedTerms(text) {
  const terms = [];
  for (const raw of text.split('\n')) {
    const t = raw.split('#')[0].trim().replace(/[A-Z]/g, (c) => c.toLowerCase());
    if (t && !terms.includes(t)) terms.push(t);
  }
  return terms;
}

// DeniedTerms.h's matching: a term with Japanese script is a substring; any other matches whole
// words over ASCII-folded text (every run of ASCII that is not a letter or digit is one space)
const JAPANESE = /[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff66-\uff9f]/u;
const foldWords = (s) => ` ${s.replace(/[A-Z]/g, (c) => c.toLowerCase()).replace(/[\x00-\x2f\x3a-\x40\x5b-\x60\x7b-\x7f]+/g, ' ').trim()} `;

function deniedMatcher(terms) {
  const compiled = terms.map((t) => (JAPANESE.test(t) ? { sub: t } : { word: foldWords(t) }));
  return (s) => {
    const lowered = s.replace(/[A-Z]/g, (c) => c.toLowerCase());
    const folded = foldWords(s);
    return compiled.some((t) => (t.sub ? lowered.includes(t.sub) : folded.includes(t.word)));
  };
}

const sourceKey = (s) => `sha256:${crypto.createHash('sha256').update(s, 'utf8').digest('hex')}`;

/** `v` with every string and member name that holds a denied term replaced by its sourceKey. */
function sanitiseDump(v, denied) {
  if (typeof v === 'string') return denied(v) ? sourceKey(v) : v;
  if (Array.isArray(v)) return v.map((x) => sanitiseDump(x, denied));
  if (v && typeof v === 'object') {
    const out = {};
    for (const k of Object.keys(v)) out[denied(k) ? sourceKey(k) : k] = sanitiseDump(v[k], denied);
    return out;
  }
  return v;
}

const deniedTermsFile = () => path.join(HERE, '..', 'denied_terms.txt');

/** Rewrites the dump `file` sanitised, as a run writes it. */
function sanitiseFile(file) {
  const denied = deniedMatcher(parseDeniedTerms(fs.readFileSync(deniedTermsFile(), 'utf8')));
  fs.writeFileSync(file, serialise(sanitiseDump(JSON.parse(fs.readFileSync(file, 'utf8')), denied)));
  console.log(`${file}: sanitised`);
}

// ---------- run ----------

function args(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--self-test' || a === '--check') out[a.slice(2)] = true;
    else if (['--pachislop', '--scratch', '--modules', '--commit', '--run-bundle', '--sanitise'].includes(a) && i + 1 < argv.length) out[a.slice(2)] = argv[++i];
    else throw new Refusal(`unknown argument ${a}`);
  }
  return out;
}

async function bundle(scratch, modules) {
  const esbuild = createRequire(path.join(modules, 'noop.js'))('esbuild');
  const srcDir = path.join(scratch, 'src');
  const expose = {
    name: 'pachislop-expose',
    setup(b) {
      b.onLoad({ filter: /\.ts$/ }, (a) => {
        const names = EXPOSE[path.relative(srcDir, a.path).split(path.sep).join('/')];
        if (!names) return undefined;
        const text = fs.readFileSync(a.path, 'utf8');
        return { contents: `${text}\nexport { ${names.map((n) => `${n} as __dump_${n}`).join(', ')} };\n`, loader: 'ts' };
      });
    },
  };
  const result = await esbuild.build({
    absWorkingDir: scratch,
    entryPoints: [path.join(scratch, 'entry.ts')],
    outfile: path.join(scratch, BUNDLE),
    bundle: true,
    platform: 'node',
    format: 'esm',
    target: 'node22',
    nodePaths: [modules],
    define: {
      'import.meta.env.BASE_URL': '"/"',
      'import.meta.env.DEV': 'false',
      'import.meta.env.PROD': 'true',
      'import.meta.env.MODE': '"production"',
    },
    banner: { js: SHIM_SOURCE },
    plugins: [expose],
    metafile: true,
    legalComments: 'none',
    logLevel: 'warning',
  });
  const stray = Object.keys(result.metafile.inputs)
    .map((p) => path.resolve(scratch, p))
    .filter((p) => !(inside(p, srcDir) || p === path.join(scratch, 'entry.ts') || inside(p, modules)));
  if (stray.length) throw new Error(`the bundle reached outside the copy and the modules: ${stray.join(', ')}`);
  return Object.keys(result.metafile.inputs).length;
}

/** Child mode: evaluate the bundle (shims installed by its banner) and write the dump. */
async function runBundle(bundleFile, scratch, commit) {
  const srcDir = path.join(scratch, 'src');
  const read = (rel) => {
    const f = path.resolve(srcDir, rel);
    if (!inside(f, srcDir)) throw new Error(`${rel} is outside the copied src`);
    return fs.readFileSync(f, 'utf8');
  };
  const m = await import(pathToFileURL(bundleFile).href);
  fs.writeFileSync(path.join(scratch, DUMP), serialise(m.buildDump(read, commit)));
}

async function dump(o) {
  if (!o.pachislop || !o.scratch) throw new Refusal('usage: node dump.mjs --pachislop <clone> --scratch <dir> [--modules <dir>] [--commit <sha>] [--check] | --self-test');
  const scratch = checkScratch(o.scratch, o.pachislop);
  const clone = real(o.pachislop);
  const modules = modulesDir(o.modules, clone);
  const commit = o.commit ?? DEFAULT_COMMIT;
  if (!fs.existsSync(path.join(clone, 'src', 'presets', 'factory.ts'))) throw new Refusal(`${clone} is not a Pachislop clone (no src/presets/factory.ts)`);
  const head = cloneHead(clone);
  if (!head.startsWith(commit)) throw new Refusal(`the clone is at ${head}, not ${commit} (--commit)`);

  fs.mkdirSync(scratch, { recursive: true });
  const srcDir = path.join(scratch, 'src');
  if (fs.existsSync(srcDir)) {
    if (!fs.existsSync(path.join(srcDir, COPY_MARK))) throw new Refusal(`${srcDir} exists and is not a copy dump.mjs made`);
    fs.rmSync(srcDir, { recursive: true });
  }
  for (const f of [DUMP, BUNDLE]) fs.rmSync(path.join(scratch, f), { force: true });
  copyTree(path.join(clone, 'src'), srcDir);
  fs.writeFileSync(path.join(srcDir, COPY_MARK), `${head}\n`);
  fs.copyFileSync(path.join(HERE, 'entry.ts'), path.join(scratch, 'entry.ts'));

  const inputs = await bundle(scratch, modules);
  const child = spawnSync(
    process.execPath,
    ['--permission', `--allow-fs-read=${scratch}`, `--allow-fs-read=${SELF}`, `--allow-fs-write=${scratch}`, SELF, '--run-bundle', path.join(scratch, BUNDLE), '--scratch', scratch, '--commit', commit],
    { stdio: 'inherit', env: { PATH: process.env.PATH ?? '', LANG: 'C.UTF-8', TZ: 'UTC' } },
  );
  if (child.status !== 0) throw new Error(`evaluating the bundle failed (exit ${child.status ?? child.signal})`);

  const out = path.join(scratch, DUMP);
  sanitiseFile(out);
  const bytes = fs.readFileSync(out);
  const sha = crypto.createHash('sha256').update(bytes).digest('hex');
  const d = JSON.parse(bytes.toString('utf8'));
  console.log(`${out}: ${bytes.length} bytes, sha256 ${sha}`);
  console.log(
    `  ${inputs} bundled inputs; ${d.looks.length} looks, ${d.templates.length} templates, ${d.effects.length} effects, ${d.generators.length} generators, ` +
      `${d.colourway_cases.length} colourway cases, ${d.lint_cases.length} lint cases`,
  );
  if (o.check) {
    const committed = path.join(HERE, 'dump', `pachislop-${commit}.json`);
    if (!fs.existsSync(committed) || !fs.readFileSync(committed).equals(bytes)) {
      console.error(`DIFFERS from ${committed}`);
      return 1;
    }
    console.log(`  equal to ${committed}`);
  }
  return 0;
}

// ---------- self-test ----------

function selfTest() {
  let failed = 0;
  const report = (ok, what) => {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`);
    if (!ok) failed++;
  };
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'pachislop-port-selftest-'));
  try {
    // A scratch directory inside the repository is refused by the command itself, before anything is written.
    const inRepo = path.join(repoRoots()[0], 'build', `pachislop-port-selftest-${process.pid}`);
    const r = spawnSync(process.execPath, [SELF, '--pachislop', path.join(tmp, 'no-clone'), '--scratch', inRepo], { encoding: 'utf8' });
    report(r.status === 2 && r.stderr.includes('is inside the repository') && !fs.existsSync(inRepo), `scratch inside the repository refused (exit ${r.status}: ${r.stderr.trim()})`);

    const fakeClone = path.join(tmp, 'clone');
    fs.mkdirSync(path.join(fakeClone, 'src'), { recursive: true });
    const inClone = path.join(fakeClone, 'scratch');
    const c = spawnSync(process.execPath, [SELF, '--pachislop', fakeClone, '--scratch', inClone], { encoding: 'utf8' });
    report(c.status === 2 && c.stderr.includes('is inside the Pachislop clone') && !fs.existsSync(inClone), `scratch inside the clone refused (exit ${c.status}: ${c.stderr.trim()})`);

    let accepted = '';
    try {
      accepted = checkScratch(path.join(tmp, 'scratch'), fakeClone);
    } catch (e) {
      accepted = `refused: ${e.message}`;
    }
    report(accepted === real(path.join(tmp, 'scratch')), `scratch outside the repository and the clone accepted (${accepted})`);

    // The shims, as the bundle's banner installs them, in a fresh Node.
    const probe = (body) => {
      const p = spawnSync(process.execPath, ['--input-type=module', '-e', `${SHIM_SOURCE}\n${body}`], { encoding: 'utf8' });
      return `${p.stdout}${p.stderr}`.trim();
    };
    const touch = (expr) => `try { ${expr}; console.log('NO THROW'); } catch (e) { console.log('THROW ' + e.message); }`;
    const doc = probe(touch('document.title'));
    report(doc.startsWith('THROW pachislop-port shim: document.title read'), `touching the document shim throws (${doc})`);
    const create = probe(touch("document.createElement('canvas')"));
    report(create.startsWith('THROW pachislop-port shim: document.createElement read'), `document.createElement throws (${create})`);
    const kinds = probe("console.log(typeof document, typeof window, typeof navigator, typeof fetch)");
    report(kinds === 'object object object function', `typeof on the shims does not throw (${kinds})`);
    const three = probe("window.__THREE__ = '186'; console.log(window.__THREE__)");
    report(three === '186', `window.__THREE__ (three.js's instance mark) is let through (${three})`);
    const others = [probe(touch('window.devicePixelRatio')), probe(touch('navigator.userAgent')), probe(touch("fetch('/reflections/city.jpg')")), probe(touch("'ontouchstart' in window"))];
    report(others.every((o) => o.startsWith('THROW pachislop-port shim:')), `window, navigator, fetch and 'in' throw (${others.join(' | ')})`);
    const seq = "console.log([Math.random(), Math.random(), crypto.randomUUID().slice(0, 8), crypto.randomUUID().slice(0, 8)].join(' '))";
    const a = probe(seq);
    const b = probe(seq);
    report(a === b && / 00000001 00000002$/.test(a), `Math.random and crypto.randomUUID are a seeded counter, equal in two processes (${a})`);
    const rv = probe(touch('crypto.getRandomValues(new Uint8Array(4))'));
    report(rv.startsWith('THROW pachislop-port shim: crypto.getRandomValues'), `crypto.getRandomValues throws (${rv})`);

    // The sanitiser: a denied id, string and member name become their source keys and allowed text
    // stays; a Latin term matches in capitals with hyphens for spaces, and not run together.
    const terms = parseDeniedTerms(fs.readFileSync(deniedTermsFile(), 'utf8'));
    const denied = deniedMatcher(terms);
    const latin = terms.find((t) => !JAPANESE.test(t) && t.includes(' '));
    const japanese = terms.find((t) => JAPANESE.test(t));
    const shouted = latin.toUpperCase().replace(/ /g, '-');
    const sample = sanitiseDump({ id: `f-${shouted}-box`, keep: '確定 BIG BONUS', [`${japanese}x`]: [`a${japanese}b`, 'fine'] }, denied);
    report(
      sample.id === sourceKey(`f-${shouted}-box`) && sample.keep === '確定 BIG BONUS' &&
        JSON.stringify(sample[sourceKey(`${japanese}x`)]) === JSON.stringify([sourceKey(`a${japanese}b`), 'fine']) &&
        !denied(`${latin.replace(/ /g, '')}s`),
      `denied strings become their sha256 source keys, the rest stays (${JSON.stringify(sample).slice(0, 120)})`,
    );
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  console.log(failed ? `${failed} self-test check(s) FAILED` : 'self-test passed');
  return failed ? 1 : 0;
}

// ---------- main ----------

try {
  const o = args(process.argv.slice(2));
  if (o['run-bundle']) await runBundle(o['run-bundle'], o.scratch, o.commit);
  else if (o.sanitise) sanitiseFile(o.sanitise);
  else process.exitCode = o['self-test'] ? selfTest() : await dump(o);
} catch (e) {
  if (e instanceof Refusal) {
    console.error(`dump.mjs: refused: ${e.message}`);
    process.exitCode = 2;
  } else {
    console.error(e instanceof Error ? e.stack : e);
    process.exitCode = 1;
  }
}
