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
//   them; only the root viewport's scrollbar is hidden (the engine's root does not scroll);
// - CSS transitions are disabled, animations finished (end state), states applied from
//   <meta name="htmlui-ref-hover|htmlui-ref-focus|htmlui-ref-active" content="#selector">.
//
// Outputs per page in engine/tests/htmlui/golden/compat/reference/: <page>.png (transparent
// background), <page>.notext.png (glyphs hidden, layout unchanged), <page>.layout.json
// (border boxes of every element with an id), plus VERSION.

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
    console.error('usage: node render-reference.mjs (--all | --page <name> ...) [--verify] [--check] [--out <dir>] [--lucide-dir <lucide checkout>]');
    process.exit(2);
}

function parseArgs(argv){
    const options = { pages: [], all: false, verify: false, check: false, out: OUT_DIR, lucideDir: null };
    for (let i = 0; i < argv.length; i++){
        const arg = argv[i];
        const value = () => { if (i + 1 >= argv.length) usage(`${arg} needs a value`); return argv[++i]; };
        if (arg === '--all') options.all = true;
        else if (arg === '--page') options.pages.push(value().replace(/\.html$/, ''));
        else if (arg === '--verify') options.verify = true;
        else if (arg === '--check') options.check = true;
        else if (arg === '--out') options.out = path.resolve(value());
        else if (arg === '--lucide-dir') options.lucideDir = path.resolve(value());
        else if (arg === '--help' || arg === '-h') usage();
        else usage(`unknown argument ${arg}`);
    }
    if (!options.all && options.pages.length === 0)
        usage('name the pages (--page) or use --all');
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

async function main(){
    const options = parseArgs(process.argv.slice(2));
    const installed = JSON.parse(fs.readFileSync(path.join(TOOL_DIR, 'node_modules/playwright-core/package.json'), 'utf8')).version;
    if (installed !== PLAYWRIGHT_VERSION)
        throw new Error(`playwright-core ${installed} is installed; run npm ci (needs ${PLAYWRIGHT_VERSION})`);

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
