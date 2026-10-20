// Chromium 153's line-break opportunities, measured: the data the fork's line breaker is tested against
// (engine/libs/rmlui/Utilities/linebreak-chromium-153*.json; gen_case_tables.py turns them into
// Tests/Source/UnitTests/LineBreakTestData.inl).
//
//   node linebreak-oracle.mjs <LineBreakTest.txt> <out.json>
//       the strings of Unicode's LineBreakTest.txt: per string, a break (true) or none (false) at each boundary between two
//       of its code points, null where it cannot be measured (the character after the boundary has no width)
//   node linebreak-oracle.mjs --ascii <out.json>
//       every pair of printable ASCII characters ('!' to '~'), alone and after 'x': a row per first character, a column per
//       second ('/' a break, 'x' none)
//
// Each string is laid out at width 0 (white-space: pre-wrap, every character kept), where every opportunity is a line
// break; a break lies before a character whose box starts a line below the last character with a box. A character without
// width fits a zero-width line, broken or not: the boundary before it is not measured. Run it in the shared lock, as
// render-reference.mjs: flock /tmp/htmlui-heavy.lock node linebreak-oracle.mjs ...
import { chromium } from 'playwright-core';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const BROWSER_REVISION = '1243';
const BROWSER_VERSION = '153.0.8010.12';

const args = process.argv.slice(2);
const ascii = args[0] === '--ascii';
const output = ascii ? args[1] : args[1];
if (!output || (!ascii && !args[0])) {
    console.error('usage: node linebreak-oracle.mjs <LineBreakTest.txt> <out.json> | --ascii <out.json>');
    process.exit(2);
}

const strings = [];
if (ascii) {
    for (const before of [[], [0x78]]) {
        for (let a = 0x21; a <= 0x7E; a++) {
            for (let b = 0x21; b <= 0x7E; b++)
                strings.push([...before, a, b]);
        }
    }
} else {
    for (const line of fs.readFileSync(args[0], 'utf8').split('\n')) {
        const body = line.split('#')[0].trim();
        if (body)
            strings.push(body.split(/\s+/).filter((t) => t !== '÷' && t !== '×').map((t) => parseInt(t, 16)));
    }
}

const executable = path.join(process.env.PLAYWRIGHT_BROWSERS_PATH || path.join(os.homedir(), '.cache', 'ms-playwright'),
    `chromium_headless_shell-${BROWSER_REVISION}`, 'chrome-headless-shell-linux64', 'chrome-headless-shell');
const browser = await chromium.launch({ headless: true, executablePath: executable });
if (browser.version() !== BROWSER_VERSION)
    throw new Error(`browser ${browser.version()} is not Chromium ${BROWSER_VERSION}`);
const page = await browser.newPage();
await page.setContent('<!DOCTYPE html><html lang="en"><head><style>div { width: 0; white-space: pre-wrap; font: 10px monospace; ' +
                      'line-height: 10px; margin-bottom: 4px; }</style></head><body></body></html>');
const measured = [];
const batch = 500;
for (let first = 0; first < strings.length; first += batch) {
    const chunk = strings.slice(first, first + batch);
    measured.push(...await page.evaluate((chunk) => {
        document.body.textContent = '';
        const divs = chunk.map((codes) => {
            const div = document.createElement('div');
            div.textContent = String.fromCodePoint(...codes);
            document.body.appendChild(div);
            return div;
        });
        return divs.map((div, i) => {
            const text = div.firstChild;
            const tops = [], widths = [];
            let offset = 0;
            for (const code of chunk[i]) {
                const length = code > 0xFFFF ? 2 : 1;
                const range = document.createRange();
                range.setStart(text, offset);
                range.setEnd(text, offset + length);
                const rects = range.getClientRects();
                tops.push(rects.length ? rects[0].top : null);
                widths.push(rects.length ? rects[0].width : 0);
                offset += length;
            }
            const result = [];
            let last = null;
            for (let k = 0; k < tops.length; k++) {
                if (k > 0)
                    result.push(widths[k] < 0.5 ? null : (tops[k] !== null && last !== null && tops[k] > last + 1));
                if (tops[k] !== null)
                    last = tops[k];
            }
            return result;
        });
    }, chunk));
}
await browser.close();

if (ascii) {
    const rows = (start, boundary) => {
        const result = [];
        for (let a = 0; a < 94; a++) {
            let row = '';
            for (let b = 0; b < 94; b++)
                row += measured[start + a * 94 + b][boundary] ? '/' : 'x';
            result.push(row);
        }
        return result;
    };
    fs.writeFileSync(output, JSON.stringify({
        comment: "Chromium 153's breaks between two printable ASCII characters ('!' to '~'), a row per first character, a column per " +
                 "second: alone ('isolated') and after 'x' ('after_x'). Measured by tools/htmlui/linebreak-oracle.mjs (--ascii).",
        isolated: rows(0, 0),
        after_x: rows(94 * 94, 1),
    }, null, 1));
} else {
    fs.writeFileSync(output, JSON.stringify(measured));
}
console.log(`linebreak-oracle: ${strings.length} strings`);
