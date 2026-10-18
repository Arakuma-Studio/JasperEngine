#!/usr/bin/env node
// Chromium reference renders of the HTML UI golden pages (phase-2 spec 5.3).
//
//   cd tools/htmlui && npm ci
//   node render-reference.mjs --all [--verify]        every page of fixtures/compat/pages
//   node render-reference.mjs --page main-menu [--page hud ...]
//   node render-reference.mjs --all --check           render and compare with the committed files
//
// Run on purpose on a developer host, never in CI or Docker: playwright-core 1.63.0 drives the
// cached Chrome headless shell revision 1243 (Chromium 153.0.8010.12, ~/.cache/ms-playwright;
// `npx playwright-core install chromium-headless-shell` installs it). Another browser is refused.
//
// Determinism and parity with the engine:
// - viewport 1280x720, deviceScaleFactor 1, light scheme, no reduced motion;
// - JavaScript is off (the engine runs none; <noscript> renders on both sides);
// - fontconfig sees only the engine's built-in face (Roboto, extracted from
//   engine/core/util/DefaultFont.h) and maps every generic family (serif, sans-serif,
//   monospace, system-ui, ...) to it, which is what the engine's font registry does: the
//   reference does not depend on the host's fonts;
// - remote URLs listed in the page manifest (pages/htmlui-imports/manifest.json) are served
//   from its local files, every other remote request is aborted (offline);
// - data-lucide elements are expanded from the engine's Lucide pack with compat's algorithm;
// - Playwright's default --hide-scrollbars is dropped: element scrollbars (Chromium's classic
//   15px ones, or ::-webkit-scrollbar styles) are part of the reference, as the engine draws
//   them; only the root viewport's scrollbar is hidden (the engine's root does not scroll; in
//   fixture mode it is kept when the page makes the viewport scroll, as the engine's root then does);
// - CSS transitions are disabled, animations finished (end state), states applied from
//   <meta name="htmlui-ref-hover|htmlui-ref-focus|htmlui-ref-active" content="#selector">.
//
// Outputs per page in engine/tests/htmlui/golden/compat/reference/: <page>.png (transparent
// background), <page>.notext.png (glyphs hidden, layout unchanged), <page>.layout.json
// (border boxes of every element with an id), plus VERSION.
//
// Fixture mode (phase-3 spec 5.2, 5.3), the references of the browser-parity tests:
//
//   node render-reference.mjs --fixtures <spec> [--fixtures <spec> ...] [--boxes-only] [--verify] [--check] [--jobs <n>]
//     <spec>: all | p3 | <area> | <area>/<name> | taffy | taffy/<suite> | taffy/<suite>/<name>
//   node render-reference.mjs --self-test     the testkit fixtures twice: byte-identical to each other
//                                             and to the committed references (reproducibility)
//
// - p3 fixtures: engine/tests/htmlui/fixtures/p3/<area>/<name>.html with a <name>.json sidecar
//   {viewport: [w, h], steps: [...], boxes, tolerance, samples, comment}; the steps (hover, focus, press,
//   release, addClass, removeClass, setStyle, setAttr, scroll, advance, capture, seekVideo) run in order, every
//   capture step (or one at the end when there is none) writes golden/browser/<area>/<name>[.<suffix>].png
//   and the boxes of every element with an id into <name>.boxes.json; with "noText": true the steps run a
//   second time in a fresh browser with the text made transparent (NO_TEXT_CSS, as page mode's no-text render)
//   into <name>[.<suffix>].notext.png. JavaScript stays off (the steps
//   run in the page's isolated evaluation), transitions are on, and every animation and transition is
//   paused when it starts and seeked to the time `advance` steps accumulated (frame-exact captures).
// - Taffy fixtures (but those of excluded.txt): engine/tests/htmlui/fixtures/taffy/<suite>/<name>.html at 1024x768, boxes only: the
//   border box of #test-root (in the viewport) and of every element under it (DOM order, by child-index
//   path; relative to #test-root), into golden/browser/taffy/<suite>.boxes.json (one file per suite).
// - Same browser, flags, fonts (fontconfig on the built-in Roboto; fixtures load theirs from
//   engine/tests/htmlui/fonts with @font-face) and scrollbars as page mode; every p3 fixture gets a
//   fresh browser, Taffy fixtures (layout only) share one with a fresh context each.
// - --verify renders twice: box JSON must be identical, images identical or within the raster noise
//   below (reported); --check compares with the committed files. golden/browser/VERSION is written.

import { chromium } from 'playwright-core';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';

const TOOL_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(TOOL_DIR, '..', '..');
const PAGES_DIR = path.join(REPO, 'engine/tests/htmlui/fixtures/compat/pages');
const OUT_DIR = path.join(REPO, 'engine/tests/htmlui/golden/compat/reference');
const DEFAULT_FONT_HEADER = path.join(REPO, 'engine/core/util/DefaultFont.h');
const P3_DIR = path.join(REPO, 'engine/tests/htmlui/fixtures/p3');
const TAFFY_DIR = path.join(REPO, 'engine/tests/htmlui/fixtures/taffy');
const BROWSER_DIR = path.join(REPO, 'engine/tests/htmlui/golden/browser');
const TAFFY_SUITES = ['grid', 'flex', 'block', 'blockflex', 'blockgrid', 'gridflex', 'leaf', 'float'];
const TAFFY_VIEWPORT = { width: 1024, height: 768 };
const LUCIDE_PACK = path.join(REPO, 'engine/libs/lucide/lucide.pack');

const PLAYWRIGHT_VERSION = '1.63.0';
const BROWSER_REVISION = '1243';
const BROWSER_VERSION = '153.0.8010.12';
const VIEWPORT = { width: 1280, height: 720 };

// the spec's launch flags (5.3); --hide-scrollbars is Playwright's default and removed on purpose
const LAUNCH_ARGS = [
    '--font-render-hinting=none',
    '--disable-font-subpixel-positioning',
    '--disable-lcd-text',
    '--force-color-profile=srgb',
    '--disable-gpu',
    // determinism: one raster thread (the tiles of a page are rasterized in a fixed order)
    '--num-raster-threads=1',
    '--disable-checker-imaging',
];

const NO_TRANSITIONS_CSS = '*,*::before,*::after{transition:none!important}';
const ROOT_SCROLLBAR_CSS = 'html::-webkit-scrollbar{display:none!important}';
const NO_TEXT_CSS = '*,*::before,*::after{-webkit-text-fill-color:transparent!important;text-shadow:none!important;' +
    '-webkit-text-stroke-color:transparent!important;text-decoration-color:transparent!important;caret-color:transparent!important}';

const LUCIDE_ROOT = {
    xmlns: 'http://www.w3.org/2000/svg', width: '24', height: '24', viewBox: '0 0 24 24', fill: 'none',
    stroke: 'currentColor', 'stroke-width': '2', 'stroke-linecap': 'round', 'stroke-linejoin': 'round',
};

function usage(message){
    if (message)
        console.error(`render-reference: ${message}`);
    console.error('usage: node render-reference.mjs (--all | --page <name> ...) [--verify] [--check] [--out <dir>] [--lucide-dir <lucide checkout>]\n' +
                  '       node render-reference.mjs --fixtures <all|p3|area|area/name|taffy|taffy/suite|taffy/suite/name> ... [--boxes-only] [--verify] [--check] [--out <dir>] [--jobs <n>]\n' +
                  '       node render-reference.mjs --self-test');
    process.exit(2);
}

function parseArgs(argv){
    const options = { pages: [], all: false, verify: false, check: false, out: null, lucideDir: null, fixtures: [], boxesOnly: false, jobs: 4,
                      selfTest: false, identical: false };
    for (let i = 0; i < argv.length; i++){
        const arg = argv[i];
        const value = () => { if (i + 1 >= argv.length) usage(`${arg} needs a value`); return argv[++i]; };
        if (arg === '--all') options.all = true;
        else if (arg === '--page') options.pages.push(value().replace(/\.html$/, ''));
        else if (arg === '--verify') options.verify = true;
        else if (arg === '--check') options.check = true;
        else if (arg === '--out') options.out = path.resolve(value());
        else if (arg === '--lucide-dir') options.lucideDir = path.resolve(value());
        else if (arg === '--fixtures') options.fixtures.push(value().replace(/\.(html|json)$/, '').replace(/\/+$/, ''));
        else if (arg === '--boxes-only') options.boxesOnly = true;
        else if (arg === '--self-test') options.selfTest = true;
        else if (arg === '--jobs') options.jobs = Math.max(1, parseInt(value(), 10) || 1);
        else if (arg === '--help' || arg === '-h') usage();
        else usage(`unknown argument ${arg}`);
    }
    if (options.selfTest){
        // the harness's reproducibility check (spec 4, p3-testkit): the testkit fixtures twice, and
        // against the committed references, byte for byte
        if (options.fixtures.length || options.all || options.pages.length || options.boxesOnly)
            usage('--self-test takes no other fixture or page option');
        options.fixtures = ['testkit'];
        options.verify = true;
        options.check = true;
        options.identical = true;
    }
    if (options.fixtures.length && (options.all || options.pages.length))
        usage('--fixtures and the page options (--all, --page) are separate modes');
    if (!options.fixtures.length && options.boxesOnly)
        usage('--boxes-only applies to --fixtures');
    if (!options.all && options.pages.length === 0 && options.fixtures.length === 0)
        usage('name the pages (--page), use --all, or name fixtures (--fixtures)');
    options.out ??= options.fixtures.length ? BROWSER_DIR : OUT_DIR;
    return options;
}

function listPages(){
    return fs.readdirSync(PAGES_DIR).filter((name) => name.endsWith('.html')).map((name) => name.slice(0, -5)).sort();
}

// --- fonts: the engine's built-in face, the only one fontconfig sees ----------------------

function extractDefaultFont(directory){
    const source = fs.readFileSync(DEFAULT_FONT_HEADER, 'utf8');
    const start = source.indexOf('roboto_regular_ttf[] = {');
    if (start < 0)
        throw new Error(`no roboto_regular_ttf array in ${DEFAULT_FONT_HEADER}`);
    const end = source.indexOf('};', start);
    const bytes = source.slice(start, end).match(/0x[0-9a-fA-F]{2}/g).map((hex) => parseInt(hex, 16));
    const lengthMatch = source.match(/roboto_regular_ttf_len\s*=\s*(\d+)/);
    if (!lengthMatch || Number(lengthMatch[1]) !== bytes.length)
        throw new Error('DefaultFont.h: the array length does not match roboto_regular_ttf_len');
    fs.writeFileSync(path.join(directory, 'Roboto-Regular-builtin.ttf'), Buffer.from(bytes));
}

function writeFontconfig(directory){
    const fonts = path.join(directory, 'fonts');
    fs.mkdirSync(fonts, { recursive: true });
    extractDefaultFont(fonts);
    const generics = ['serif', 'sans-serif', 'monospace', 'system-ui', 'ui-sans-serif', 'ui-serif', 'ui-monospace',
                      'cursive', 'fantasy', 'Arial', 'Helvetica', 'Times New Roman', 'Courier New'];
    const aliases = generics.map((family) =>
        `  <alias binding="same"><family>${family}</family><prefer><family>Roboto</family></prefer></alias>`).join('\n');
    const config = `<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>${fonts}</dir>
  <cachedir>${path.join(directory, 'cache')}</cachedir>
${aliases}
</fontconfig>
`;
    const file = path.join(directory, 'fonts.conf');
    fs.writeFileSync(file, config);
    return file;
}

// --- the import manifest: remote URLs served from local files ------------------------------

function normalizeUrl(url){
    // ImportManifest::normalizeUrl: lowercase scheme and host, http -> https, no fragment, query kept
    let text = url.trim();
    if (text.startsWith('//'))
        text = 'https:' + text;
    const parsed = new URL(text);
    if (parsed.protocol === 'http:')
        parsed.protocol = 'https:';
    parsed.hash = '';
    return parsed.toString();
}

function loadManifest(){
    const file = path.join(PAGES_DIR, 'htmlui-imports', 'manifest.json');
    if (!fs.existsSync(file))
        return { entries: [] };
    const manifest = JSON.parse(fs.readFileSync(file, 'utf8'));
    for (const entry of manifest.entries ?? [])
        entry.url = normalizeUrl(entry.url);
    return manifest;
}

const MIME = { '.css': 'text/css', '.ttf': 'font/ttf', '.otf': 'font/otf', '.png': 'image/png', '.jpg': 'image/jpeg',
               '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.svg': 'image/svg+xml', '.json': 'application/json' };

// A manifest URL maps to its local file; URLs next to it (the fonts a CSS references
// relatively) map to the files next to the local file.
function resolveRemote(manifest, url){
    let normalized;
    try { normalized = normalizeUrl(url); } catch { return null; }
    for (const entry of manifest.entries){
        if (entry.url === normalized)
            return path.join(PAGES_DIR, entry.path);
    }
    for (const entry of manifest.entries){
        const base = entry.url.slice(0, entry.url.lastIndexOf('/', entry.url.indexOf('?') >= 0 ? entry.url.indexOf('?') : undefined) + 1);
        const target = normalized.split('?')[0];
        if (target.startsWith(base)){
            const local = path.join(PAGES_DIR, path.dirname(entry.path), decodeURIComponent(target.slice(base.length)));
            if (fs.existsSync(local))
                return local;
        }
    }
    return null;
}

// --- Lucide: compat's expansion (spec 3.7.3 step 7) from the engine's pack -------------------

class LucideIcons {
    constructor(lucideDir){
        this.icons = new Map();
        this.aliases = new Map();
        if (lucideDir)
            this.loadCheckout(lucideDir);
        else if (fs.existsSync(LUCIDE_PACK))
            this.loadPack(fs.readFileSync(LUCIDE_PACK));
        this.source = lucideDir ? `checkout ${path.basename(lucideDir)}` :
            (fs.existsSync(LUCIDE_PACK) ? `engine/libs/lucide/lucide.pack (Lucide ${this.release})` : 'none');
    }

    // engine/libs/lucide/lucide.pack, written by tools/htmlui/build_lucide_pack.py (little
    // endian): "LUCP", u32 format version, u32 payload size, zlib payload {u32 icon count,
    // u32 alias count, u16+bytes release tag, icons {u16+name, u32 offset, u32 length},
    // aliases {u16+name, u32 icon index}, u32 markup size, markup}
    loadPack(data){
        if (data.toString('latin1', 0, 4) !== 'LUCP')
            throw new Error('lucide.pack: bad magic');
        const version = data.readUInt32LE(4);
        if (version !== 1)
            throw new Error(`lucide.pack: format version ${version} is not 1`);
        const payload = zlib.inflateSync(data.subarray(12));
        if (payload.length !== data.readUInt32LE(8))
            throw new Error('lucide.pack: payload size mismatch');
        let at = 0;
        const u16 = () => { const v = payload.readUInt16LE(at); at += 2; return v; };
        const u32 = () => { const v = payload.readUInt32LE(at); at += 4; return v; };
        const string = () => { const length = u16(); const text = payload.toString('utf8', at, at + length); at += length; return text; };
        const iconCount = u32();
        const aliasCount = u32();
        this.release = string();
        const entries = [];
        for (let i = 0; i < iconCount; i++)
            entries.push({ name: string(), offset: u32(), length: u32() });
        const aliases = [];
        for (let i = 0; i < aliasCount; i++)
            aliases.push({ alias: string(), index: u32() });
        const markupSize = u32();
        const markup = at;
        if (markup + markupSize !== payload.length)
            throw new Error('lucide.pack: markup size mismatch');
        for (const entry of entries)
            this.icons.set(entry.name, payload.toString('utf8', markup + entry.offset, markup + entry.offset + entry.length));
        for (const { alias, index } of aliases)
            this.aliases.set(alias, entries[index].name);
    }

    // a Lucide checkout (icons/*.svg + icons/*.json), for developing before the pack exists
    loadCheckout(directory){
        const icons = path.join(directory, 'icons');
        for (const file of fs.readdirSync(icons)){
            if (file.endsWith('.svg')){
                const svg = fs.readFileSync(path.join(icons, file), 'utf8');
                const inner = svg.slice(svg.indexOf('>', svg.indexOf('<svg')) + 1, svg.lastIndexOf('</svg>'));
                this.icons.set(file.slice(0, -4), inner.replace(/\s*\n\s*/g, '').trim());
            }else if (file.endsWith('.json')){
                const meta = JSON.parse(fs.readFileSync(path.join(icons, file), 'utf8'));
                for (const alias of meta.aliases ?? [])
                    this.aliases.set(typeof alias === 'string' ? alias : alias.name, file.slice(0, -5));
            }
        }
    }

    static kebab(name){
        return name.replace(/([a-z0-9])([A-Z])/g, '$1-$2').replace(/([A-Z])([A-Z][a-z])/g, '$1-$2').replace(/([a-zA-Z])([0-9])/g, '$1-$2').toLowerCase();
    }

    find(name){
        const key = LucideIcons.kebab(name);
        if (this.icons.has(key))
            return { name: key, shapes: this.icons.get(key) };
        if (this.aliases.has(key))
            return { name: this.aliases.get(key), shapes: this.icons.get(this.aliases.get(key)) };
        return null;
    }
}

// --- page metadata ---------------------------------------------------------------------------

function metaContent(html, name){
    const pattern = new RegExp(`<meta\\s+name=["']${name}["']\\s+content=["']([^"']*)["']`, 'i');
    const match = html.match(pattern);
    return match ? match[1].trim() : null;
}

// --- rendering ---------------------------------------------------------------------------------

function headlessShellPath(){
    const browsers = process.env.PLAYWRIGHT_BROWSERS_PATH || path.join(os.homedir(), '.cache', 'ms-playwright');
    return path.join(browsers, `chromium_headless_shell-${BROWSER_REVISION}`, 'chrome-headless-shell-linux64', 'chrome-headless-shell');
}

async function launch(fontconfigFile){
    if (process.platform !== 'linux')
        throw new Error('the references are rendered on Linux (fontconfig maps the generic families)');
    const executable = headlessShellPath();
    if (!fs.existsSync(executable))
        throw new Error(`${executable} is missing: npx playwright-core install chromium-headless-shell`);
    const browser = await chromium.launch({
        headless: true,
        executablePath: executable,
        args: LAUNCH_ARGS,
        ignoreDefaultArgs: ['--hide-scrollbars'],
        env: { ...process.env, FONTCONFIG_FILE: fontconfigFile, FONTCONFIG_PATH: path.dirname(fontconfigFile) },
    });
    if (browser.version() !== BROWSER_VERSION){
        await browser.close();
        throw new Error(`browser ${browser.version()} is not Chromium ${BROWSER_VERSION}`);
    }
    return browser;
}

async function injectStyle(page, css){
    await page.evaluate((text) => {
        const style = document.createElement('style');
        style.setAttribute('data-htmlui-reference', '');
        style.textContent = text;
        (document.head ?? document.documentElement).appendChild(style);
    }, css);
}

async function expandLucide(page, lucide){
    const names = await page.evaluate(() => [...document.querySelectorAll('[data-lucide]')].map((e) => e.getAttribute('data-lucide')));
    if (names.length === 0)
        return [];
    const icons = {};
    const missing = [];
    for (const name of names){
        const icon = lucide.find(name);
        if (icon)
            icons[name] = icon;
        else
            missing.push(name);
    }
    if (missing.length)
        throw new Error(`Lucide icons not found (${lucide.source}): ${missing.join(', ')}`);
    await page.evaluate(({ icons, root }) => {
        // lucide.createIcons(): replaceElement with the default attributes, the element's
        // attributes over them, classes "lucide lucide-<name> <element classes>", aria-hidden
        // unless the element has an aria-* attribute or a role
        for (const element of [...document.querySelectorAll('[data-lucide]')]){
            const icon = icons[element.getAttribute('data-lucide')];
            const attributes = { ...root, 'data-lucide': icon.name };
            let accessible = false;
            for (const attribute of element.attributes){
                if (attribute.name === 'class')
                    continue;
                if (attribute.name.startsWith('aria-') || attribute.name === 'role')
                    accessible = true;
                attributes[attribute.name] = attribute.value;
            }
            attributes['data-lucide'] = element.getAttribute('data-lucide');
            if (!accessible)
                attributes['aria-hidden'] = 'true';
            const classes = ['lucide', `lucide-${icon.name}`, ...element.classList];
            attributes.class = [...new Set(classes)].join(' ');
            const holder = document.createElement('div');
            holder.innerHTML = `<svg>${icon.shapes}</svg>`;
            const svg = holder.firstChild;
            for (const [name, value] of Object.entries(attributes))
                svg.setAttribute(name, value);
            element.replaceWith(svg);
        }
    }, { icons, root: LUCIDE_ROOT });
    return names;
}

// Chromium first paints scaled images with a fast filter and repaints them with the
// high-quality one a moment later: let that happen before a capture
const SETTLE_MS = 400;

// One capture of a page from a fresh load: `extraCss` is injected with the transition and
// root-scrollbar rules, before any state is applied. Returns the PNG, the layout and notes.
async function capturePage(browser, name, html, manifest, lucide, extraCss, withLayout){
    const htmlPath = path.join(PAGES_DIR, `${name}.html`);
    const context = await browser.newContext({
        viewport: VIEWPORT,
        deviceScaleFactor: 1,
        colorScheme: 'light',
        reducedMotion: 'no-preference',
        javaScriptEnabled: false,
        locale: 'en-US',
        timezoneId: 'UTC',
    });
    const blocked = new Set();
    await context.route(/^(https?:)?\/\//, async (route) => {
        const local = resolveRemote(manifest, route.request().url());
        if (!local || !fs.existsSync(local)){
            blocked.add(route.request().url());
            return route.abort('blockedbyclient');
        }
        return route.fulfill({
            status: 200,
            body: fs.readFileSync(local),
            headers: { 'content-type': MIME[path.extname(local).toLowerCase()] ?? 'application/octet-stream', 'access-control-allow-origin': '*' },
        });
    });

    try {
        const page = await context.newPage();
        await page.goto(pathToFileURL(htmlPath).href, { waitUntil: 'load' });
        await injectStyle(page, NO_TRANSITIONS_CSS + ROOT_SCROLLBAR_CSS + extraCss);
        const icons = await expandLucide(page, lucide);
        await page.evaluate(async () => {
            await document.fonts.ready;
            // decoded images, or a screenshot can catch one half drawn
            await Promise.all([...document.images].map((image) => image.decode().catch(() => {})));
            for (const animation of document.getAnimations()){
                const timing = animation.effect?.getComputedTiming?.();
                if (timing && Number.isFinite(timing.endTime))
                    animation.finish();
                else{
                    animation.pause();
                    animation.currentTime = 0;
                }
            }
        });
        const failedFonts = await page.evaluate(() => [...document.fonts].filter((f) => f.status === 'error').map((f) => f.family));
        if (failedFonts.length)
            throw new Error(`${name}: fonts failed to load: ${failedFonts.join(', ')}`);

        // states
        const focus = metaContent(html, 'htmlui-ref-focus');
        const hover = metaContent(html, 'htmlui-ref-hover');
        const active = metaContent(html, 'htmlui-ref-active');
        if (focus){
            await page.keyboard.press('Tab');   // keyboard modality: :focus-visible applies
            await page.focus(focus);
        }
        if (hover)
            await page.hover(hover);
        if (active){
            await page.hover(active);
            await page.mouse.down();
        }

        await page.waitForTimeout(SETTLE_MS);
        const png = await page.screenshot({ omitBackground: true, caret: 'hide' });
        const layout = withLayout ? await page.evaluate(() => {
            const round = (value) => Math.round(value * 100) / 100;
            // under a CSS transform the client rect is the transformed bounding box, which the
            // engine's layout boxes do not include: such elements are compared by the images only
            const transformed = (element) => {
                for (let e = element; e && e.nodeType === 1; e = e.parentElement){
                    const style = getComputedStyle(e);
                    if ((style.transform !== 'none' && style.transform !== 'matrix(1, 0, 0, 1, 0, 0)') ||
                        style.translate !== 'none' || style.rotate !== 'none' || style.scale !== 'none')
                        return true;
                }
                return false;
            };
            const elements = [];
            for (const element of document.querySelectorAll('[id]')){
                const box = element.getBoundingClientRect();
                const entry = { id: element.id, tag: element.localName, x: round(box.x), y: round(box.y), w: round(box.width), h: round(box.height) };
                const tolerance = element.getAttribute('data-ref-tol');
                if (tolerance)
                    entry.tol = tolerance;
                if (transformed(element))
                    entry.transformed = true;
                elements.push(entry);
            }
            return elements;
        }) : null;
        if (active)
            await page.mouse.up();

        const states = {};
        if (hover) states.hover = hover;
        if (focus) states.focus = focus;
        if (active) states.active = active;
        return { png, layout, states, icons, blocked: [...blocked] };
    } finally {
        await context.close();
    }
}

// one page: <page>.png, <page>.notext.png, <page>.layout.json
async function renderPage(launchBrowser, name, manifest, lucide){
    const html = fs.readFileSync(path.join(PAGES_DIR, `${name}.html`), 'utf8');
    // a fresh browser per capture: a page captured after another one in the same browser can
    // differ in a few pixels (warm caches, image filter upgrades)
    const capture = async (extraCss, withLayout) => {
        const browser = await launchBrowser();
        try {
            return await capturePage(browser, name, html, manifest, lucide, extraCss, withLayout);
        } finally {
            await browser.close();
        }
    };
    const full = await capture('', true);
    const noText = await capture(NO_TEXT_CSS, false);

    const document = {
        page: `${name}.html`,
        viewport: [VIEWPORT.width, VIEWPORT.height],
        browser: `Chromium ${BROWSER_VERSION}`,
        states: full.states,
        requires: metaContent(html, 'htmlui-test-requires') ?? '',
        lucide: full.icons.length ? [...new Set(full.icons)].sort() : [],
        elements: full.layout,
    };
    const files = {};
    files[`${name}.png`] = full.png;
    files[`${name}.notext.png`] = noText.png;
    files[`${name}.layout.json`] = Buffer.from(JSON.stringify(document, null, 1) + '\n');
    return { files, blocked: full.blocked };
}

// --- PNG comparison (Playwright writes 8-bit RGBA, non-interlaced) ------------------------

function decodePng(buffer){
    let at = 8;
    let width = 0, height = 0, colorType = 0;
    const chunks = [];
    while (at < buffer.length){
        const length = buffer.readUInt32BE(at);
        const type = buffer.toString('latin1', at + 4, at + 8);
        const data = buffer.subarray(at + 8, at + 8 + length);
        if (type === 'IHDR'){
            width = data.readUInt32BE(0);
            height = data.readUInt32BE(4);
            if (data[8] !== 8 || data[12] !== 0)
                throw new Error('only 8-bit non-interlaced PNGs are compared');
            colorType = data[9];
        }else if (type === 'IDAT'){
            chunks.push(data);
        }
        at += 12 + length;
    }
    const channels = { 6: 4, 2: 3 }[colorType];
    if (!channels)
        throw new Error(`PNG color type ${colorType} is not compared`);
    const raw = zlib.inflateSync(Buffer.concat(chunks));
    const stride = width * channels;
    const pixels = Buffer.alloc(width * height * 4);
    let previous = Buffer.alloc(stride);
    for (let y = 0; y < height; y++){
        const filter = raw[y * (stride + 1)];
        const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
        for (let i = 0; i < stride; i++){
            const a = i >= channels ? line[i - channels] : 0;
            const b = previous[i];
            const c = i >= channels ? previous[i - channels] : 0;
            let predictor = 0;
            if (filter === 1) predictor = a;
            else if (filter === 2) predictor = b;
            else if (filter === 3) predictor = (a + b) >> 1;
            else if (filter === 4){
                const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                predictor = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
            }
            line[i] = (line[i] + predictor) & 255;
        }
        for (let x = 0; x < width; x++){
            for (let k = 0; k < 4; k++)
                pixels[(y * width + x) * 4 + k] = k < channels ? line[x * channels + k] : 255;
        }
        previous = line;
    }
    return { width, height, pixels };
}

// Chromium's software raster is not bit-exact across browser processes: anti-aliased pixels
// of blurred rounded shapes (box-shadow glows on rounded boxes) vary by up to ~32/255 on a
// few dozen pixels. Two renders "agree" when they are byte-identical or differ only by that
// noise, far below the comparator's 48 threshold (BrowserCompare).
const NOISE_MAX_DIFF = 40;
const NOISE_MAX_FRACTION = 0.0005;

function compareRenders(fileName, a, b){
    if (a.equals(b))
        return { same: true, identical: true, text: 'identical' };
    if (!fileName.endsWith('.png'))
        return { same: false, identical: false, text: 'differs' };
    const x = decodePng(a), y = decodePng(b);
    if (x.width !== y.width || x.height !== y.height)
        return { same: false, identical: false, text: `size ${x.width}x${x.height} vs ${y.width}x${y.height}` };
    let count = 0, max = 0;
    for (let i = 0; i < x.pixels.length; i += 4){
        let d = 0;
        for (let k = 0; k < 4; k++)
            d = Math.max(d, Math.abs(x.pixels[i + k] - y.pixels[i + k]));
        if (d > 0){
            count++;
            max = Math.max(max, d);
        }
    }
    const fraction = count / (x.width * x.height);
    const same = max <= NOISE_MAX_DIFF && fraction <= NOISE_MAX_FRACTION;
    return { same, identical: false, text: `${count} pixels differ (${(fraction * 100).toFixed(4)} %), largest difference ${max}` };
}

function sha(buffer){
    return crypto.createHash('sha256').update(buffer).digest('hex').slice(0, 16);
}

// ===== fixture mode: the references of the phase-3 browser-parity tests (spec 5.2, 5.3) ========

const STEP_KINDS = ['hover', 'focus', 'press', 'release', 'addClass', 'removeClass', 'setStyle', 'setAttr', 'scroll', 'advance', 'capture',
                    'seekVideo'];

function listAreas(){
    if (!fs.existsSync(P3_DIR))
        return [];
    return fs.readdirSync(P3_DIR, { withFileTypes: true }).filter((e) => e.isDirectory()).map((e) => e.name).sort();
}

// the fixtures of an area: <name>.html with a <name>.json sidecar
function listP3(area){
    const directory = path.join(P3_DIR, area);
    return fs.readdirSync(directory).filter((f) => f.endsWith('.html') && fs.existsSync(path.join(directory, f.slice(0, -5) + '.json')))
        .map((f) => f.slice(0, -5)).sort();
}

// the fixtures of excluded.txt (Taffy's invalid ones, rtl, writing-mode, scripts) are not compared,
// so they have no reference
function taffyExcluded(){
    const file = path.join(TAFFY_DIR, 'excluded.txt');
    if (!fs.existsSync(file))
        return new Set();
    return new Set(fs.readFileSync(file, 'utf8').split('\n').map((line) => line.split('#')[0].trim().split(/\s+/)[0]).filter(Boolean));
}

function listTaffy(suite){
    const excluded = taffyExcluded();
    return fs.readdirSync(path.join(TAFFY_DIR, suite)).filter((f) => f.endsWith('.html')).map((f) => f.slice(0, -5))
        .filter((name) => !excluded.has(`${suite}/${name}`)).sort();
}

// --fixtures specs -> {p3: [{area, name}], taffy: Map(suite -> [names] | null for the whole suite)}
function resolveFixtures(specs){
    const p3 = new Map();
    const taffy = new Map();
    const addArea = (area) => { for (const name of listP3(area)) p3.set(`${area}/${name}`, { area, name }); };
    const addSuite = (suite, name) => {
        if (!TAFFY_SUITES.includes(suite))
            throw new Error(`no Taffy suite ${suite} (${TAFFY_SUITES.join(', ')})`);
        if (name === undefined){
            taffy.set(suite, null);
            return;
        }
        if (!fs.existsSync(path.join(TAFFY_DIR, suite, `${name}.html`)))
            throw new Error(`no Taffy fixture ${suite}/${name}`);
        if (taffyExcluded().has(`${suite}/${name}`))
            throw new Error(`taffy/${suite}/${name} is excluded (excluded.txt): it is not compared and has no reference`);
        if (taffy.get(suite) !== null)
            taffy.set(suite, [...(taffy.get(suite) ?? []), name]);
    };
    for (const spec of specs){
        const parts = spec.split('/');
        if (spec === 'all' || spec === 'p3')
            listAreas().forEach(addArea);
        if (spec === 'all' || spec === 'taffy')
            TAFFY_SUITES.forEach((suite) => addSuite(suite));
        if (spec === 'all' || spec === 'p3' || spec === 'taffy')
            continue;
        if (parts[0] === 'taffy'){
            if (parts.length > 3)
                throw new Error(`bad fixture spec ${spec}`);
            addSuite(parts[1], parts[2]);
        }else if (parts.length === 1 && listAreas().includes(spec)){
            addArea(spec);
        }else if (parts.length === 2){
            if (!fs.existsSync(path.join(P3_DIR, parts[0], `${parts[1]}.json`)))
                throw new Error(`no fixture ${spec} (fixtures/p3/${spec}.html + .json)`);
            p3.set(spec, { area: parts[0], name: parts[1] });
        }else if (parts.length === 1){
            const found = listAreas().filter((area) => fs.existsSync(path.join(P3_DIR, area, `${spec}.json`)));
            if (found.length !== 1)
                throw new Error(found.length ? `fixture ${spec} is in several areas (${found.join(', ')}): name the area` : `no fixture or area ${spec}`);
            p3.set(`${found[0]}/${spec}`, { area: found[0], name: spec });
        }else{
            throw new Error(`bad fixture spec ${spec}`);
        }
    }
    return { p3: [...p3.values()], taffy };
}

// The sidecar (spec 5.2): viewport, steps, boxes, noText, tolerance (with a comment saying why), selfTest
// (fixtures of the testkit area only: {expect: "mismatch", referenceCss}: the CSS is injected in the
// reference alone, so the engine must be reported as different).
function loadSidecar(area, name){
    const file = path.join(P3_DIR, area, `${name}.json`);
    let sidecar;
    try { sidecar = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (error){ throw new Error(`${file}: ${error.message}`); }
    const where = `fixtures/p3/${area}/${name}.json`;
    const viewport = sidecar.viewport;
    if (!Array.isArray(viewport) || viewport.length !== 2 || !viewport.every((v) => Number.isInteger(v) && v > 0 && v <= 4096))
        throw new Error(`${where}: "viewport" must be [width, height]`);
    const steps = sidecar.steps ?? [];
    if (!Array.isArray(steps))
        throw new Error(`${where}: "steps" must be a list`);
    const target = (value) => typeof value === 'string' && /^#[A-Za-z_][\w-]*$/.test(value);
    steps.forEach((step, index) => {
        const keys = Object.keys(step).filter((key) => key !== 'keyboard');
        const kind = keys[0];
        const value = step[kind];
        const bad = (why) => { throw new Error(`${where}: step ${index + 1} ${JSON.stringify(step)}: ${why}`); };
        if (keys.length !== 1 || !STEP_KINDS.includes(kind))
            bad(`one of ${STEP_KINDS.join(', ')}`);
        if ('keyboard' in step && kind !== 'focus')
            bad('"keyboard" goes with "focus"');
        if (['hover', 'focus', 'press'].includes(kind) && !target(value))
            bad('the target is "#id"');
        if (kind === 'release' && value !== true && !target(value))
            bad('"release": true or "#id"');
        if (['addClass', 'removeClass'].includes(kind) && !(Array.isArray(value) && value.length === 2 && target(value[0]) && typeof value[1] === 'string'))
            bad('["#id", "class"]');
        if (kind === 'setStyle' && !(Array.isArray(value) && value.length === 3 && target(value[0]) && value.slice(1).every((v) => typeof v === 'string')))
            bad('["#id", "property", "value"] ("" removes)');
        if (kind === 'setAttr' && !(Array.isArray(value) && value.length === 3 && target(value[0]) && typeof value[1] === 'string' &&
                                    (typeof value[2] === 'string' || value[2] === null)))
            bad('["#id", "name", "value" or null (removes)]');
        if (kind === 'scroll' && !(Array.isArray(value) && value.length === 3 && target(value[0]) && Number.isFinite(value[1]) && Number.isFinite(value[2])))
            bad('["#id", x, y]');
        if (kind === 'advance' && !(Number.isFinite(value) && value >= 0))
            bad('seconds >= 0');
        if (kind === 'seekVideo' && !(Array.isArray(value) && value.length === 2 && target(value[0]) && Number.isFinite(value[1]) && value[1] >= 0))
            bad('["#id", seconds]');
        if (kind === 'capture' && !(typeof value === 'string' && /^[\w-]*$/.test(value)))
            bad('a file-name suffix ([A-Za-z0-9_-]*)');
    });
    const captures = steps.filter((step) => 'capture' in step).map((step) => step.capture);
    if (new Set(captures).size !== captures.length)
        throw new Error(`${where}: capture suffixes must differ`);
    if (sidecar.tolerance !== undefined && !(typeof sidecar.comment === 'string' && sidecar.comment.trim()))
        throw new Error(`${where}: a "tolerance" override needs a "comment" explaining the rasterization difference (spec 5.2)`);
    // "samples" (1 or 4) is the engine's MSAA for the fixture (Chromium has its own antialiasing)
    if (sidecar.samples !== undefined && !((sidecar.samples === 1 || sidecar.samples === 4) && typeof sidecar.comment === 'string' && sidecar.comment.trim()))
        throw new Error(`${where}: "samples" is 1 or 4, with a "comment" saying why`);
    if (sidecar.selfTest !== undefined && area !== 'testkit')
        throw new Error(`${where}: "selfTest" is for the harness's own fixtures (fixtures/p3/testkit)`);
    if (sidecar.noText !== undefined && sidecar.noText !== true && sidecar.noText !== false)
        throw new Error(`${where}: "noText" must be true or false`);
    return { viewport, steps, boxes: sidecar.boxes === true, noText: sidecar.noText === true, selfTest: sidecar.selfTest ?? null };
}

// in the page (JavaScript off: Playwright evaluates in its own world, where state persists):
// adopt every animation and transition that is not yet, paused at its start, seeked from then on
async function adoptAnimations(page){
    await page.evaluate(() => {
        const state = window.__htmluiFixture ??= { time: 0, animations: [] };
        for (const animation of document.getAnimations()){
            if (state.animations.some((known) => known.animation === animation))
                continue;
            animation.pause();
            animation.currentTime = 0;
            state.animations.push({ animation, local: 0 });
        }
    });
}

// <video> (phase 6): the frame of `seconds` on screen, as the engine's FixtureRunner seeks in its
// frame-exact mode: currentTime set, `seeked`, the element at HAVE_CURRENT_DATA. (Not
// requestVideoFrameCallback: the headless shell presents no frame until a screenshot asks for one.)
async function seekVideo(page, label, selector, seconds){
    const problem = await page.evaluate(async ([selector, seconds]) => {
        const video = document.querySelector(selector);
        if (!(video instanceof HTMLVideoElement))
            return `${selector} is not a <video>`;
        const within = (promise, what) => Promise.race([promise, new Promise((resolve) => setTimeout(() => resolve(what), 10000))]);
        if (video.readyState < 1){
            const late = await within(new Promise((resolve) => video.addEventListener('loadedmetadata', () => resolve(null), { once: true })),
                                      'no metadata after 10 s');
            if (late)
                return `${selector}: ${late}`;
        }
        const late = await within(new Promise((resolve) => {
            video.addEventListener('seeked', () => resolve(null), { once: true });
            video.currentTime = seconds;
        }), `no seeked event 10 s after currentTime = ${seconds}`);
        if (late)
            return `${selector}: ${late}`;
        const deadline = Date.now() + 10000;
        while (video.readyState < 2 && Date.now() < deadline)
            await new Promise((resolve) => setTimeout(resolve, 20));
        return video.readyState < 2 ? `${selector}: no frame after the seek` : null;
    }, [selector, seconds]);
    if (problem)
        throw new Error(`${label}: ${problem}`);
}

// every <video> that loads (not preload="none") has its frame, and every poster is decoded, before a capture
async function waitForVideos(page, label){
    const stuck = await page.evaluate(async () => {
        const all = [...document.querySelectorAll('video')];
        const videos = all.filter((v) => v.preload !== 'none' || v.currentTime > 0);
        const deadline = Date.now() + 10000;
        while (videos.some((v) => v.readyState < 2 && !v.error) && Date.now() < deadline)
            await new Promise((resolve) => setTimeout(resolve, 20));
        const posters = await Promise.all(all.filter((v) => v.poster).map(async (v) => {
            const image = new Image();
            image.src = v.poster;
            try { await image.decode(); return null; } catch (e) { return (v.id ? '#' + v.id : v.poster) + ' (poster)'; }
        }));
        return videos.filter((v) => v.readyState < 2).map((v) => (v.id ? '#' + v.id : v.currentSrc) + (v.error ? ` (error ${v.error.code})` : ''))
            .concat(posters.filter((p) => p));
    });
    if (stuck.length)
        throw new Error(`${label}: no frame for ${stuck.join(', ')}`);
}

async function advanceAnimations(page, seconds){
    await page.evaluate((dt) => {
        const state = window.__htmluiFixture;
        state.time += dt;
        // A CSS animation whose animation-play-state is paused holds its time (pause() above makes
        // the browser ignore the property, so the seek follows it instead; phase-3 spec 3.13.5).
        const cssPaused = (animation) => {
            if (typeof animation.animationName !== 'string' || !animation.effect || !animation.effect.target)
                return false;
            const style = getComputedStyle(animation.effect.target);
            const names = style.animationName.split(',').map((name) => name.trim());
            const states = style.animationPlayState.split(',').map((value) => value.trim());
            const index = names.indexOf(animation.animationName);
            return index >= 0 && states[index % states.length] === 'paused';
        };
        // only the live ones: seeking a cancelled transition would revive it
        const live = new Set(document.getAnimations());
        for (const entry of state.animations){
            if (!live.has(entry.animation))
                continue;
            if (!cssPaused(entry.animation))
                entry.local += dt;
            entry.animation.currentTime = entry.local * 1000;
        }
    }, seconds);
}

// the centre of an element's border box, rounded (the engine moves its pointer to the same pixel)
async function centreOf(page, selector){
    const centre = await page.evaluate((s) => {
        const element = document.querySelector(s);
        if (!element)
            return null;
        const r = element.getBoundingClientRect();
        return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) };
    }, selector);
    if (!centre)
        throw new Error(`no element ${selector}`);
    return centre;
}

// the border box of every element with an id, in document order (phase-2 layout format)
async function idBoxes(page){
    return page.evaluate(() => {
        const transformed = (element) => {
            for (let e = element; e && e.nodeType === 1; e = e.parentElement){
                const style = getComputedStyle(e);
                if ((style.transform !== 'none' && style.transform !== 'matrix(1, 0, 0, 1, 0, 0)') ||
                    style.translate !== 'none' || style.rotate !== 'none' || style.scale !== 'none')
                    return true;
            }
            return false;
        };
        // the root element (<html>) has no box of its own in the engine (its id names the document for steps)
        return [...document.querySelectorAll('[id]')].filter((element) => element !== document.documentElement).map((element) => {
            const box = element.getBoundingClientRect();
            const entry = { id: element.id, tag: element.localName, x: box.x, y: box.y, w: box.width, h: box.height };
            if (transformed(element))
                entry.transformed = true;
            return entry;
        });
    });
}

// The engine's document scrolls (and shows its scrollbars) only when the page makes the viewport a scroll
// container: html's overflow, or body's when html's is visible (CSS Overflow 3 3.3), auto or scroll. A browser's
// viewport scrolls by default; its scrollbar is hidden then (ROOT_SCROLLBAR_CSS), kept otherwise.
async function viewportScrolls(page){
    return page.evaluate(() => {
        const root = getComputedStyle(document.documentElement);
        let overflow = [root.overflowX, root.overflowY];
        if (overflow[0] === 'visible' && overflow[1] === 'visible' && document.body){
            const body = getComputedStyle(document.body);
            overflow = [body.overflowX, body.overflowY];
        }
        return overflow.some((value) => value === 'auto' || value === 'scroll');
    });
}

async function waitForFonts(page, label){
    await page.evaluate(async () => {
        await document.fonts.ready;
        await Promise.all([...document.images].map((image) => image.decode().catch(() => {})));
    });
    const failed = await page.evaluate(() => [...document.fonts].filter((f) => f.status === 'error').map((f) => f.family));
    if (failed.length)
        throw new Error(`${label}: fonts failed to load: ${failed.join(', ')}`);
}

async function newFixtureContext(browser, viewport, blocked){
    const context = await browser.newContext({
        viewport: { width: viewport[0], height: viewport[1] },
        deviceScaleFactor: 1,
        colorScheme: 'light',
        reducedMotion: 'no-preference',
        javaScriptEnabled: false,
        locale: 'en-US',
        timezoneId: 'UTC',
    });
    // fixtures are offline: everything they load is next to them
    await context.route(/^(https?:)?\/\//, (route) => { blocked.add(route.request().url()); return route.abort('blockedbyclient'); });
    return context;
}

// one p3 fixture in a fresh browser: {files: {name: Buffer}, blocked}
async function renderP3Fixture(launchBrowser, { area, name }, boxesOnly){
    const sidecar = loadSidecar(area, name);
    const label = `${area}/${name}`;
    const blocked = new Set();
    const captures = await runP3Fixture(launchBrowser, sidecar, area, name, label, boxesOnly, '', blocked);
    // "noText": the same run again with the text made transparent (the phase-2 golden pages' no-text
    // render): <name>[.<capture>].notext.png, gated strictly by BrowserParityTest
    const noText = (sidecar.noText && !boxesOnly) ? await runP3Fixture(launchBrowser, sidecar, area, name, label, false, NO_TEXT_CSS, blocked) : [];

    const files = {};
    for (const { suffix, png } of captures){
        if (png)
            files[`${area}/${name}${suffix ? '.' + suffix : ''}.png`] = png;
    }
    for (const { suffix, png } of noText)
        files[`${area}/${name}${suffix ? '.' + suffix : ''}.notext.png`] = png;
    const document = {
        fixture: `${area}/${name}.html`,
        viewport: sidecar.viewport,
        browser: `Chromium ${BROWSER_VERSION}`,
        captures: Object.fromEntries(captures.map(({ suffix, boxes }) => [suffix, boxes])),
    };
    files[`${area}/${name}.boxes.json`] = Buffer.from(JSON.stringify(document, null, 1) + '\n');
    return { files, blocked: [...blocked] };
}

// One run of a fixture's steps in a fresh browser; extraCss is injected with the root scrollbar rule (the
// no-text pass: from the start, so no step sees the text style change).
async function runP3Fixture(launchBrowser, sidecar, area, name, label, boxesOnly, extraCss, blocked){
    const browser = await launchBrowser();
    try {
        const context = await newFixtureContext(browser, sidecar.viewport, blocked);
        const page = await context.newPage();
        await page.goto(pathToFileURL(path.join(P3_DIR, area, `${name}.html`)).href, { waitUntil: 'load' });
        await injectStyle(page, (await viewportScrolls(page) ? '' : ROOT_SCROLLBAR_CSS) + (sidecar.selfTest?.referenceCss ?? '') + extraCss);
        await waitForFonts(page, label);
        await adoptAnimations(page);

        const captures = [];
        const capture = async (suffix) => {
            await waitForVideos(page, label);
            await page.waitForTimeout(SETTLE_MS);
            // a step may use a face (or an image) the page did not draw before: loaded, and none failed
            await waitForFonts(page, label);
            const png = boxesOnly ? null : await page.screenshot({ omitBackground: true, caret: 'hide' });
            captures.push({ suffix, png, boxes: await idBoxes(page) });
        };
        for (const step of sidecar.steps){
            const [kind] = Object.keys(step).filter((key) => key !== 'keyboard');
            const value = step[kind];
            if (kind === 'hover'){
                const { x, y } = await centreOf(page, value);
                await page.mouse.move(x, y);
            }else if (kind === 'focus' && step.keyboard){
                // keyboard navigation (:focus-visible), as the engine's FixtureRunner does
                let reached = false;
                for (let i = 0; i < 64 && !reached; i++){
                    await page.keyboard.press('Tab');
                    reached = await page.evaluate((s) => document.activeElement === document.querySelector(s), value);
                }
                if (!reached)
                    throw new Error(`${label}: Tab navigation never reached ${value}`);
            }else if (kind === 'focus' || kind === 'press'){
                const { x, y } = await centreOf(page, value);
                await page.mouse.move(x, y);
                await page.mouse.down();
                if (kind === 'focus')
                    await page.mouse.up();
            }else if (kind === 'release'){
                if (value !== true){
                    const { x, y } = await centreOf(page, value);
                    await page.mouse.move(x, y);
                }
                await page.mouse.up();
            }else if (kind === 'advance'){
                await advanceAnimations(page, value);
            }else if (kind === 'seekVideo'){
                await seekVideo(page, label, value[0], value[1]);
            }else if (kind === 'capture'){
                await capture(value);
            }else{
                const missing = await page.evaluate(({ kind, value }) => {
                    const element = document.querySelector(value[0]);
                    if (!element)
                        return value[0];
                    if (kind === 'addClass') element.classList.add(value[1]);
                    else if (kind === 'removeClass') element.classList.remove(value[1]);
                    else if (kind === 'setStyle') element.style.setProperty(value[1], value[2]);
                    else if (kind === 'setAttr' && value[2] === null) element.removeAttribute(value[1]);
                    else if (kind === 'setAttr') element.setAttribute(value[1], value[2]);
                    else if (kind === 'scroll'){ element.scrollLeft = value[1]; element.scrollTop = value[2]; }
                    return null;
                }, { kind, value });
                if (missing)
                    throw new Error(`${label}: no element ${missing}`);
            }
            if (kind !== 'advance' && kind !== 'capture')
                await adoptAnimations(page);
        }
        if (!sidecar.steps.some((step) => 'capture' in step))
            await capture('');
        await context.close();
        return captures;
    } finally {
        await browser.close();
    }
}

// one Taffy fixture: [[path, tag, x, y, w, h], ...] for #test-root ("", in the viewport) and every
// element under it (path = child indices among element children, "0.2"; relative to #test-root's
// border box)
async function taffyBoxes(browser, suite, name){
    const blocked = new Set();
    const context = await newFixtureContext(browser, [TAFFY_VIEWPORT.width, TAFFY_VIEWPORT.height], blocked);
    try {
        const page = await context.newPage();
        await page.goto(pathToFileURL(path.join(TAFFY_DIR, suite, `${name}.html`)).href, { waitUntil: 'load' });
        await injectStyle(page, ROOT_SCROLLBAR_CSS);
        await waitForFonts(page, `taffy/${suite}/${name}`);
        const boxes = await page.evaluate(() => {
            const root = document.getElementById('test-root');
            if (!root)
                return null;
            const origin = root.getBoundingClientRect();
            const out = [];
            const walk = (element, at) => {
                const r = element.getBoundingClientRect();
                // #test-root in the viewport (its insets and margins are tested too), the rest relative to it
                const o = element === root ? { x: 0, y: 0 } : origin;
                out.push([at, element.localName, r.x - o.x, r.y - o.y, r.width, r.height]);
                [...element.children].forEach((child, index) => walk(child, at === '' ? String(index) : `${at}.${index}`));
            };
            walk(root, '');
            return out;
        });
        if (!boxes)
            throw new Error(`taffy/${suite}/${name}: no #test-root`);
        if (blocked.size)
            throw new Error(`taffy/${suite}/${name}: requested ${[...blocked].join(', ')}`);
        return boxes;
    } finally {
        await context.close();
    }
}

// every fixture of `names` with `jobs` contexts at a time in one browser: Map(name -> boxes)
async function taffySuiteBoxes(launchBrowser, suite, names, jobs){
    const browser = await launchBrowser();
    try {
        const result = new Map();
        let next = 0;
        const worker = async () => {
            while (next < names.length){
                const name = names[next++];
                result.set(name, await taffyBoxes(browser, suite, name));
            }
        };
        await Promise.all(Array.from({ length: Math.min(jobs, names.length) }, worker));
        return result;
    } finally {
        await browser.close();
    }
}

// golden/browser/taffy/<suite>.boxes.json: one fixture per line, sorted
function taffySuiteJson(suite, fixtures){
    const names = [...fixtures.keys()].sort();
    const lines = names.map((name, i) => `  ${JSON.stringify(name)}: ${JSON.stringify(fixtures.get(name))}${i + 1 < names.length ? ',' : ''}`);
    return `{\n "suite": ${JSON.stringify(suite)},\n "viewport": [${TAFFY_VIEWPORT.width}, ${TAFFY_VIEWPORT.height}],\n` +
        ` "browser": "Chromium ${BROWSER_VERSION}",\n "fixtures": {\n${lines.join('\n')}\n }\n}\n`;
}

function readTaffySuite(file){
    if (!fs.existsSync(file))
        return new Map();
    return new Map(Object.entries(JSON.parse(fs.readFileSync(file, 'utf8')).fixtures));
}

function writeVersion(directory){
    fs.mkdirSync(directory, { recursive: true });
    fs.writeFileSync(path.join(directory, 'VERSION'),
        `Chromium ${BROWSER_VERSION} (chrome-headless-shell revision ${BROWSER_REVISION}), playwright-core ${PLAYWRIGHT_VERSION}\n` +
        `Fonts: the fixtures' @font-face files (engine/tests/htmlui/fonts); generic families: the engine's built-in Roboto through fontconfig\n` +
        `Taffy fixtures: viewport ${TAFFY_VIEWPORT.width}x${TAFFY_VIEWPORT.height}, boxes only\n` +
        `Written by tools/htmlui/render-reference.mjs --fixtures; do not edit.\n`);
}

async function fixturesMain(options){
    const { p3, taffy } = resolveFixtures(options.fixtures);
    if (!p3.length && !taffy.size)
        throw new Error('the --fixtures specs name no fixture');
    const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'htmlui-reference-'));
    let failed = false;
    try {
        const fontconfig = writeFontconfig(temp);
        const launchBrowser = () => launch(fontconfig);

        // p3 fixtures
        for (const fixture of p3){
            const first = await renderP3Fixture(launchBrowser, fixture, options.boxesOnly);
            if (options.verify){
                const second = await renderP3Fixture(launchBrowser, fixture, options.boxesOnly);
                for (const [file, data] of Object.entries(first.files)){
                    const result = compareRenders(file, data, second.files[file]);
                    // box JSON must be exact; images may carry Chromium's cross-process raster noise
                    // (--self-test: not even that)
                    const same = file.endsWith('.json') || options.identical ? result.identical : result.same;
                    if (!result.identical)
                        console.log(`  ${file}: two runs ${same ? 'agree within the raster noise' : 'DIFFER'}: ${result.text}`);
                    failed ||= !same;
                }
            }
            for (const [file, data] of Object.entries(first.files)){
                const target = path.join(options.out, file);
                if (options.check){
                    const result = fs.existsSync(target) ? compareRenders(file, fs.readFileSync(target), data) : { same: false, identical: false, text: 'missing' };
                    const same = file.endsWith('.json') || options.identical ? result.identical : result.same;
                    console.log(`${same ? 'same   ' : 'CHANGED'} ${file}${result.identical ? '' : ` (${result.text})`}`);
                    failed ||= !same;
                }else{
                    fs.mkdirSync(path.dirname(target), { recursive: true });
                    fs.writeFileSync(target, data);
                    console.log(`wrote ${path.relative(REPO, target)} (${sha(data)})`);
                }
            }
            for (const url of first.blocked)
                console.log(`  ${fixture.area}/${fixture.name}: blocked ${url} (fixtures are offline)`);
        }

        // Taffy suites
        if (taffy.size && !options.boxesOnly)
            console.log('Taffy fixtures are compared by their boxes only (--boxes-only implied)');
        for (const [suite, selected] of taffy){
            const names = selected ?? listTaffy(suite);
            const started = Date.now();
            const boxes = await taffySuiteBoxes(launchBrowser, suite, names, options.jobs);
            console.log(`taffy/${suite}: ${names.length} fixtures in ${((Date.now() - started) / 1000).toFixed(1)} s`);
            if (options.verify){
                const again = await taffySuiteBoxes(launchBrowser, suite, names, options.jobs);
                const differing = names.filter((name) => JSON.stringify(boxes.get(name)) !== JSON.stringify(again.get(name)));
                for (const name of differing)
                    console.log(`  taffy/${suite}/${name}: two runs DIFFER: ${JSON.stringify(boxes.get(name))} vs ${JSON.stringify(again.get(name))}`);
                console.log(`taffy/${suite}: two runs ${differing.length ? `differ in ${differing.length} fixtures` : 'identical'}`);
                failed ||= differing.length > 0;
            }
            const file = path.join(options.out, 'taffy', `${suite}.boxes.json`);
            const committed = readTaffySuite(file);
            if (options.check){
                const changed = names.filter((name) => JSON.stringify(committed.get(name)) !== JSON.stringify(boxes.get(name)));
                for (const name of changed)
                    console.log(`CHANGED taffy/${suite}/${name}${committed.has(name) ? '' : ' (missing)'}`);
                console.log(`${changed.length ? 'CHANGED' : 'same   '} taffy/${suite}.boxes.json (${names.length - changed.length} of ${names.length} same)`);
                failed ||= changed.length > 0;
            }else{
                // a whole suite replaces the file; single fixtures update their entries
                const merged = selected ? committed : new Map();
                for (const [name, value] of boxes)
                    merged.set(name, value);
                const data = Buffer.from(taffySuiteJson(suite, merged));
                fs.mkdirSync(path.dirname(file), { recursive: true });
                fs.writeFileSync(file, data);
                console.log(`wrote ${path.relative(REPO, file)} (${merged.size} fixtures, ${sha(data)})`);
            }
        }
        if (!options.check)
            writeVersion(options.out);
        if (options.selfTest)
            console.log(failed ? 'self-test FAILED: two renders, or a render and the committed reference, differ'
                               : `self-test passed: ${p3.length} testkit fixtures rendered twice, byte-identical to each other and to the committed references`);
        if (failed)
            process.exitCode = 1;
    } finally {
        fs.rmSync(temp, { recursive: true, force: true });
    }
}

async function main(){
    const options = parseArgs(process.argv.slice(2));
    const installed = JSON.parse(fs.readFileSync(path.join(TOOL_DIR, 'node_modules/playwright-core/package.json'), 'utf8')).version;
    if (installed !== PLAYWRIGHT_VERSION)
        throw new Error(`playwright-core ${installed} is installed; run npm ci (needs ${PLAYWRIGHT_VERSION})`);
    if (options.fixtures.length)
        return fixturesMain(options);

    const pages = options.all ? listPages() : options.pages;
    for (const name of pages){
        if (!fs.existsSync(path.join(PAGES_DIR, `${name}.html`)))
            throw new Error(`no page ${name}.html in ${PAGES_DIR}`);
    }

    const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'htmlui-reference-'));
    try {
        const fontconfig = writeFontconfig(temp);
        const manifest = loadManifest();
        const lucide = new LucideIcons(options.lucideDir);
        const render = (name) => renderPage(() => launch(fontconfig), name, manifest, lucide);
        let failed = false;
        fs.mkdirSync(options.out, { recursive: true });
        for (const name of pages){
            const first = await render(name);
            if (options.verify){
                const second = await render(name);
                for (const [file, data] of Object.entries(first.files)){
                    const result = compareRenders(file, data, second.files[file]);
                    if (!result.identical)
                        console.log(`  ${file}: two runs ${result.same ? 'agree within the raster noise' : 'DIFFER'}: ${result.text}`);
                    failed ||= !result.same;
                }
            }
            for (const [file, data] of Object.entries(first.files)){
                const target = path.join(options.out, file);
                if (options.check){
                    const result = fs.existsSync(target) ? compareRenders(file, fs.readFileSync(target), data) : { same: false, text: 'missing' };
                    console.log(`${result.same ? 'same   ' : 'CHANGED'} ${file}${result.identical ? '' : ` (${result.text})`}`);
                    failed ||= !result.same;
                }else{
                    fs.writeFileSync(target, data);
                    console.log(`wrote ${path.relative(REPO, target)} (${sha(data)})`);
                }
            }
            for (const url of first.blocked)
                console.log(`  ${name}: blocked ${url} (not in the manifest)`);
        }
        if (!options.check){
            fs.writeFileSync(path.join(options.out, 'VERSION'),
                `Chromium ${BROWSER_VERSION} (chrome-headless-shell revision ${BROWSER_REVISION}), playwright-core ${PLAYWRIGHT_VERSION}\n` +
                `Generic font families: the engine's built-in Roboto (engine/core/util/DefaultFont.h) through fontconfig\n` +
                `Lucide: ${lucide.source}\n` +
                `Written by tools/htmlui/render-reference.mjs; do not edit.\n`);
        }
        if (failed)
            process.exitCode = 1;
    } finally {
        fs.rmSync(temp, { recursive: true, force: true });
    }
}

main().catch((error) => {
    console.error(`render-reference: ${error.message}`);
    process.exit(1);
});
