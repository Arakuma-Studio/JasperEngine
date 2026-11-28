// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Run against pristine Anime.js 4.5.0: node gen_svg_reference.mjs /path/to/anime/src.
// The small DOM shim supplies exact polyline geometry; the SVG modules under test are
// unmodified upstream code. Curved geometry is tested separately against analytic curves.

import {writeFileSync} from 'node:fs';
import {dirname, join} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

globalThis.window = {};
globalThis.document = {};
globalThis.NodeList = class {};
globalThis.HTMLCollection = class {};
globalThis.getComputedStyle = element => ({strokeLinecap: element.cap || 'butt', vectorEffect: 'none'});

globalThis.SVGElement = class {
  constructor(tag, points, attr = {}) {
    this.tagName = tag;
    this.points = points;
    this.attributes = {...attr};
    this.style = {};
  }
  getAttribute(name) { return this.attributes[name] ?? null; }
  setAttribute(name, value) { this.attributes[name] = value; }
  getCTM() { return {a: 2, b: 0, c: 0, d: 2, e: 0, f: 0}; }
  getTotalLength() {
    return this.points.slice(1).reduce((length, p, i) => length + Math.hypot(p[0] - this.points[i][0], p[1] - this.points[i][1]), 0);
  }
  getPointAtLength(length) {
    for (let i = 1; i < this.points.length; ++i) {
      const a = this.points[i - 1], b = this.points[i];
      const segment = Math.hypot(b[0] - a[0], b[1] - a[1]);
      if (length <= segment) return {x: a[0] + (b[0] - a[0]) * length / segment, y: a[1] + (b[1] - a[1]) * length / segment};
      length -= segment;
    }
    const last = this.points.at(-1);
    return {x: last[0], y: last[1]};
  }
};

const source = process.argv[2] || process.env.ANIME_SRC || '/tmp/htmlui-src/anime/src';
const load = path => import(pathToFileURL(join(source, path)).href);
const {createDrawable} = await load('svg/drawable.js');
const {morphTo} = await load('svg/morphto.js');
const {createMotionPath} = await load('svg/motionpath.js');

const drawable = new SVGElement('path', [[0, 0], [100, 0]]);
const proxy = createDrawable(drawable)[0];
const draws = ['0 0', '0 .25', '0 .5', '0 1', '.5 1', '.5 .5', '1 0'].map(draw => {
  proxy.setAttribute('draw', draw);
  return {draw, dash: drawable.getAttribute('stroke-dasharray'), offset: +drawable.getAttribute('stroke-dashoffset')};
});

const from = new SVGElement('path', [[0, 0], [100, 0]], {d: 'M0 0L100 0'});
const to = new SVGElement('path', [[0, 0], [0, 100], [100, 100]], {d: 'M0 0L0 100L100 100'});
const morph = morphTo(to, .02)(from, 0, 1);
const times = [0, .25, .5, .75, 1];
const motion = {};
for (const offset of [0, .25, -.25]) {
  const path = createMotionPath(to, offset);
  motion[offset] = times.map(time => Object.fromEntries(Object.entries(path).map(([name, factory]) => {
    const tween = factory({});
    return [name, tween.modifier(tween.to * time)];
  })));
}

const result = {version: '4.5.0', draws, morph, times, motion};
writeFileSync(join(dirname(fileURLToPath(import.meta.url)), 'svg-reference.json'), JSON.stringify(result, null, 2) + '\n');
