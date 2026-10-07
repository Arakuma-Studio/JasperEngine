// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Generates reference.json, the values the engine's Anime.js port (engine/core/htmlui/anim/)
// is compared with by AnimeReferenceTests.cpp: every easing at 101 points, springs, stagger
// outputs, timeline offsets, value decomposition, utils, and frame-by-frame recordings of
// animations, timers and timelines on plain objects driven by Anime's own engine.
//
// Runs on the host with node (v20) against the pristine Anime.js 4.5.0 sources:
//   node engine/tests/htmlui/anime/gen_reference.mjs [/tmp/htmlui-src/anime/src]
// (ANIME_SRC overrides the default source directory too). The output is checked in.
//
// Two adjustments make the recordings match the engine's semantics (docs/html-ui/phase-1 §3.11):
// - Anime's resume() moves a timer's start 12 ms back ("advance by at least one frame on the
//   next tick"). The engine's timers start at the last UI-clock tick instead, which already
//   gives the first frame one frame of progress, so the recordings undo those 12 ms.
// - The engine is driven frame by frame with a fake clock (Date.now) instead of
//   requestAnimationFrame/setImmediate.

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = process.argv[2] || process.env.ANIME_SRC || '/tmp/htmlui-src/anime/src';

// the fake clock must be in place before Anime captures Date.now
let fakeNow = 1000;
Date.now = () => fakeNow;

const anime = await import(pathToFileURL(join(src, 'index.js')).href);
const { decomposeRawValue, createDecomposedValueTargetObject } = await import(pathToFileURL(join(src, 'core/values.js')).href);
const { convertColorStringValuesToRgbaArray } = await import(pathToFileURL(join(src, 'core/colors.js')).href);
const { parseTimelinePosition } = await import(pathToFileURL(join(src, 'timeline/position.js')).href);
const { minValue } = await import(pathToFileURL(join(src, 'core/consts.js')).href);

const { engine, eases, cubicBezier, steps, linear, spring, Spring, stagger, utils,
        createTimeline, createTimer, animate, Timer } = anime;

engine.useDefaultMainLoop = false;

// undo resume()'s 12 ms head start (see the header)
const originalResume = Timer.prototype.resume;
Timer.prototype.resume = function () {
  const wasPaused = this.paused;
  const result = originalResume.call(this);
  if (wasPaused && !(this.duration <= minValue && !this._hasChildren)) this._startTime += 12;
  return result;
};

const samples = fn => Array.from({ length: 101 }, (_, i) => fn(i / 100));

// ---------------------------------------------------------------------------------------------
// Easings

const easings = {};
for (const name of Object.keys(eases)) {
  const ease = eases[name];
  const parametric = ['in', 'out', 'inOut', 'outIn'].includes(name) || name.includes('Back') || name.includes('Elastic');
  easings[name] = samples(parametric ? ease() : ease);
}
const parametricEases = [
  ['in(3)', eases.in(3)], ['out(3)', eases.out(3)], ['inOut(1.5)', eases.inOut(1.5)], ['outIn(2)', eases.outIn(2)],
  ['in(0.5)', eases.in(0.5)],
  ['inBack(3)', eases.inBack(3)], ['outBack(1.2)', eases.outBack(1.2)], ['inOutBack(2.5)', eases.inOutBack(2.5)],
  ['outInBack(1)', eases.outInBack(1)],
  ['inElastic(2, .5)', eases.inElastic(2, .5)], ['outElastic(1, .6)', eases.outElastic(1, .6)],
  ['inOutElastic(1.5, .4)', eases.inOutElastic(1.5, .4)], ['outInElastic(3, 1)', eases.outInElastic(3, 1)],
  ['outElastic(0.5, 3)', eases.outElastic(0.5, 3)],
  ['cubicBezier(.17,.67,.83,.67)', cubicBezier(.17, .67, .83, .67)],
  ['cubicBezier(0.68,-0.55,0.265,1.55)', cubicBezier(0.68, -0.55, 0.265, 1.55)],
  ['cubicBezier(0.25,0.1,0.25,1)', cubicBezier(0.25, 0.1, 0.25, 1)],
  ['cubicBezier(.5,.5,.5,.5)', cubicBezier(.5, .5, .5, .5)],
  ['steps(5)', steps(5)], ['steps(4, true)', steps(4, true)], ['steps(1)', steps(1)],
  ['linear(0, 0.25, 1)', linear(0, 0.25, 1)],
  ['linear(0, 0.5 25%, 0.75 50%, 1)', linear(0, '0.5 25%', '0.75 50%', 1)],
  ['linear(1, 0)', linear(1, 0)],
];
for (const [name, fn] of parametricEases) easings[name] = samples(fn);

// ---------------------------------------------------------------------------------------------
// Springs

const springParams = [
  {},
  { bounce: 0.5, duration: 400 },
  { bounce: 0, duration: 300 },
  { bounce: -0.5, duration: 800 },
  { bounce: 0.9 },
  { duration: 1500 },
  { mass: 1, stiffness: 100, damping: 10, velocity: 0 },
  { mass: 2, stiffness: 300, damping: 40, velocity: 5 },
  { stiffness: 50, damping: 1 },
  { stiffness: 200, damping: 30 },
  { mass: 1, stiffness: 100, damping: 20 },
];
const springs = springParams.map(params => {
  const s = spring(params);
  const easing = s.ease;
  const values = samples(t => easing(t));
  return {
    params,
    settlingDuration: s.settlingDuration, solverDuration: s.solverDuration,
    bounce: s.bounce, duration: s.duration, stiffness: s.stiffness, damping: s.damping,
    mass: s.mass, velocity: s.velocity, samples: values,
  };
});

// ---------------------------------------------------------------------------------------------
// Stagger

const staggerCases = [
  { value: 100, params: {}, total: 5 },
  { value: 100, params: { from: 'center' }, total: 5 },
  { value: 100, params: { from: 'center' }, total: 6 },
  { value: 100, params: { from: 'last' }, total: 5 },
  { value: 50, params: { from: 2 }, total: 7 },
  { value: 100, params: { start: 500 }, total: 4 },
  { value: [0, 360], params: {}, total: 5 },
  { value: [100, 0], params: { from: 'center' }, total: 7 },
  { value: '20px', params: {}, total: 4 },
  { value: ['-1rem', '1rem'], params: {}, total: 5 },
  { value: 100, params: { reversed: true }, total: 5 },
  { value: 100, params: { ease: 'inQuad' }, total: 6 },
  { value: 100, params: { ease: 'outElastic(1, .5)', from: 'center' }, total: 9 },
  { value: 50, params: { grid: [4, 3] }, total: 12 },
  { value: 50, params: { grid: [4, 3], from: 'center' }, total: 12 },
  { value: 50, params: { grid: [4, 3], from: 5 }, total: 12 },
  { value: 50, params: { grid: [4, 3], from: 'center', axis: 'x' }, total: 12 },
  { value: 50, params: { grid: [4, 3], from: 'last', axis: 'y' }, total: 12 },
  { value: 50, params: { grid: [4, 3], from: [0.25, 1] }, total: 12 },
  { value: 50, params: { grid: [3, 3], from: 'center', reversed: true }, total: 9 },
  { value: 50, params: { grid: [3, 3], axis: 'x', reversed: true }, total: 9 },
  { value: 50, params: { grid: [4, 3, 2], from: 'center' }, total: 24 },
  { value: 100, params: { total: 10 }, total: 4 },
  { value: 100, params: { from: 'random', seed: 42 }, total: 8 },
  { value: 100, params: { jitter: 10, seed: true }, total: 6 },
  { value: 100, params: { jitter: [0, 30], seed: 7, from: 'center' }, total: 7 },
];
const staggers = staggerCases.map(c => {
  const fn = stagger(c.value, c.params);
  const targets = Array.from({ length: c.total }, () => ({}));
  const outputs = targets.map((t, i) => fn(t, i, targets));
  return { ...c, outputs };
});

// ---------------------------------------------------------------------------------------------
// Timeline offsets: timers (and animations) added at every position form

const timelineCases = [
  { name: 'sequence', ops: [['add', 500], ['add', 300], ['add', 200]] },
  { name: 'absolute', ops: [['add', 500], ['add', 300, 100], ['add', 200, '250'], ['add', 100]] },
  { name: 'relative', ops: [['add', 500], ['add', 300, '+=100'], ['add', 200, '-=150'], ['add', 100, '*=2'], ['add', 50, '-=5000']] },
  { name: 'previous', ops: [['add', 500], ['add', 300, '<'], ['add', 200, '<<'], ['add', 100, '<+=50'], ['add', 100, '<<-=20'], ['add', 100, '<<+=10']] },
  { name: 'labels', ops: [['add', 500], ['label', 'a'], ['add', 300], ['label', 'b', 100], ['add', 200, 'a'],
                          ['add', 100, 'b+=50'], ['add', 100, 'a-=200'], ['add', 100, 'missing'], ['label', 'c', '<<'], ['add', 50, 'c*=2']] },
  { name: 'delays', ops: [['addDelay', 500, 200], ['add', 300], ['addDelay', 100, 50, '-=100'], ['add', 100, '<<']] },
  { name: 'stagger', ops: [['add', 400], ['addStagger', 200, 4, [100, {}]], ['add', 100]] },
  { name: 'staggerStart', ops: [['add', 400], ['addStagger', 200, 3, [150, { start: 50, from: 'last' }]], ['add', 100, '<<']] },
  { name: 'setAndCall', ops: [['add', 500], ['set', 200], ['call', '-=300'], ['add', 100, '<'], ['set']] },
];

const timelines = timelineCases.map(c => {
  const tl = createTimeline({ autoplay: false, defaults: { ease: 'linear' } });
  const offsets = [];
  for (const op of c.ops) {
    const [kind, ...args] = op;
    if (kind === 'add') {
      tl.add({ duration: args[0] }, args[1]);
    } else if (kind === 'addDelay') {
      tl.add({ duration: args[0], delay: args[1] }, args[2]);
    } else if (kind === 'addStagger') {
      const targets = Array.from({ length: args[1] }, () => ({ x: 0 }));
      tl.add(targets, { x: 10, duration: args[0] }, stagger(...args[2]));
    } else if (kind === 'set') {
      tl.set({ x: 0 }, { x: 1 }, args[0]);
    } else if (kind === 'call') {
      tl.call(() => {}, args[0]);
    } else if (kind === 'label') {
      tl.label(args[0], args[1]);
    }
  }
  anime.forEachChildren(tl, child => offsets.push({ offset: child._offset, delay: child._delay, duration: child.duration }));
  return { name: c.name, ops: c.ops, offsets, duration: tl.duration, iterationDuration: tl.iterationDuration, labels: { ...tl.labels } };
});

// direct parseTimelinePosition results on a timeline with children and labels
const positionTl = createTimeline({ autoplay: false });
positionTl.add({ duration: 1000 }).add({ duration: 400 }, 200).label('mid', 500);
const positionInputs = [undefined, 0, 250, '300', '+=100', '-=100', '*=2', '<', '<<', '<+=10', '<<-=10',
                        'mid', 'mid+=50', 'mid-=50', 'mid*=2', 'nope', 'nope+=10'];
const positions = positionInputs.map(p => ({ position: p === undefined ? null : p, value: parseTimelinePosition(positionTl, p) }));

// ---------------------------------------------------------------------------------------------
// Values

const decomposeInputs = [0, 12.5, '12.5', '-3e2', '100px', '-0.5em', '45deg', '1turn', '50%', '+=20', '-=1.5', '*=2', '+=10px',
  '#f00', '#ff000080', '#0f08', 'rgb(255, 128, 0)', 'rgba(10,20,30,0.25)', 'hsl(120, 50%, 50%)', 'hsla(240, 100%, 25%, .5)',
  'translate(10px, -20.5px) scale(1.5)', 'rgb(255, 0, 0) 0px 0px 6px 0px', 'calc(100% - 20px)', 'none', 'auto', ''];
const decomposed = decomposeInputs.map(v => {
  const o = decomposeRawValue(v, createDecomposedValueTargetObject());
  return { input: v, t: o.t, n: o.n, u: o.u, o: o.o, d: o.d, s: o.s };
});
const colorInputs = ['#abc', '#abcd', '#aabbcc', '#aabbccdd', 'rgb(1,2,3)', 'rgba(1, 2, 3, .5)', 'hsl(0, 100%, 50%)',
  'hsl(200, 30%, 70%)', 'hsla(330, 80%, 40%, 0.75)', 'hsl(90, 0%, 40%)'];
const colors = colorInputs.map(c => ({ input: c, rgba: convertColorStringValuesToRgbaArray(c) }));

// ---------------------------------------------------------------------------------------------
// Utils

const seeded = utils.createSeededRandom(1234);
const utilsReference = {
  wrap: [[5, 0, 3], [-1, 0, 3], [370, 0, 360], [2.5, 1, 2]].map(a => ({ args: a, value: utils.wrap(...a) })),
  mapRange: [[5, 0, 10, 0, 100], [2, 1, 3, 10, -10]].map(a => ({ args: a, value: utils.mapRange(...a) })),
  snap: [[17, 5], [17.4, 0.5], [17, [0, 10, 20]], [3, 0]].map(a => ({ args: a, value: utils.snap(...a) })),
  round: [[1.23456, 2], [1.5, 0], [-1.5, 0], [123.456, -1], [2.675, 2]].map(a => ({ args: a, value: utils.round(...a) })),
  clamp: [[5, 0, 3], [-1, 0, 3], [2, 0, 3]].map(a => ({ args: a, value: utils.clamp(...a) })),
  lerp: [[0, 100, 0.25], [10, 20, 1], [10, 20, 0]].map(a => ({ args: a, value: utils.lerp(...a) })),
  damp: [[0, 100, 16, 0.5], [0, 100, 16, 0], [0, 100, 16, 1]].map(a => ({ args: a, value: utils.damp(...a) })),
  degToRad: [[180], [45]].map(a => ({ args: a, value: utils.degToRad(...a) })),
  radToDeg: [[Math.PI], [1]].map(a => ({ args: a, value: utils.radToDeg(...a) })),
  roundPad: [[1.5, 3], [10, 2], [2.345, 1]].map(a => ({ args: a, value: utils.roundPad(...a) })),
  padStart: [[5, 3, '0'], ['ab', 5, 'xy']].map(a => ({ args: a, value: utils.padStart(...a) })),
  padEnd: [[5, 3, '0'], ['ab', 5, 'xy']].map(a => ({ args: a, value: utils.padEnd(...a) })),
  seededRandom: Array.from({ length: 10 }, (_, i) => seeded(0, 100, i % 3)),
};

// ---------------------------------------------------------------------------------------------
// Recordings: animations, timers and timelines on plain objects, driven frame by frame

// JSON markers for what JSON cannot hold (the C++ test maps them the same way):
//   {"$ease": ["cubicBezier", .1, .2, .3, 1]}  {"$ease": ["steps", 4]}  {"$ease": ["linear", 0, "0.5 25%", 1]}
//   {"$spring": {...}}  {"$stagger": [value, params]}  {"$fn": ["index", k]} = (t, i) => i * k
//   {"$modifier": ["round", n]}
const resolveMarkers = value => {
  if (Array.isArray(value)) return value.map(resolveMarkers);
  if (value && typeof value === 'object') {
    if ('$ease' in value) {
      const [name, ...args] = value.$ease;
      return name === 'cubicBezier' ? cubicBezier(...args) : name === 'steps' ? steps(...args) : linear(...args);
    }
    if ('$spring' in value) return spring(value.$spring);
    if ('$stagger' in value) return stagger(value.$stagger[0], resolveMarkers(value.$stagger[1] || {}));
    if ('$fn' in value) {
      const [kind, k] = value.$fn;
      if (kind === 'index') return (t, i) => i * k;
    }
    if ('$modifier' in value) return v => utils.round(v, value.$modifier[1]);
    const out = {};
    for (const key of Object.keys(value)) out[key] = resolveMarkers(value[key]);
    return out;
  }
  return value;
};

const callbackNames = ['onBegin', 'onBeforeUpdate', 'onUpdate', 'onRender', 'onLoop', 'onPause', 'onComplete'];

// a recording: steps run before the frame with that index; every frame advances the fake
// clock by dt, ticks the engine and records the targets' values and the callbacks fired
const recordingCases = [
  { name: 'basic', frames: 30, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 500, ease: 'linear' } }] },
  { name: 'defaultEaseDelay', frames: 30, targets: { a: { x: 10, y: -5 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 110, y: 5, delay: 100, duration: 400 } }] },
  { name: 'loopDelay', frames: 70, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: [0, 100], loop: 2, duration: 300, ease: 'inOutQuad', loopDelay: 100 } }] },
  { name: 'alternate', frames: 45, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, loop: 3, alternate: true, duration: 200, ease: 'outQuad' } }] },
  { name: 'reversed', frames: 20, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, reversed: true, duration: 300, ease: 'linear' } }] },
  { name: 'infiniteLoop', frames: 40, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, loop: true, alternate: true, duration: 150, ease: 'linear' } },
            { frame: 35, op: 'call', id: 'A', method: 'pause' }] },
  { name: 'keyframeValues', frames: 55, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: [0, 50, 20, 100], duration: 900, ease: 'linear' } }] },
  { name: 'keyframeObjects', frames: 50, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      x: [{ to: 50, duration: 200, ease: 'inQuad' }, { to: 0, delay: 100 }, { to: 100, ease: 'linear', duration: 300 }], duration: 600 } }] },
  { name: 'durationKeyframes', frames: 55, targets: { a: { x: 0, y: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      keyframes: [{ x: 100, y: 50 }, { x: 0, duration: 300 }, { y: 0 }], duration: 900, ease: 'linear' } }] },
  { name: 'percentKeyframes', frames: 50, targets: { a: { x: 0, y: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      keyframes: { '0%': { x: 0, y: 10 }, '25%': { x: 100, ease: 'inQuad' }, '60%': { y: 80 }, '100%': { x: 0, y: 0 } }, duration: 800, ease: 'linear' } }] },
  { name: 'fromToRelative', frames: 30, targets: { a: { x: 20, y: 20, z: 20, w: 20 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      x: { from: -50, to: 50 }, y: '+=50', z: '*=3', w: ['-=10', '+=10'], duration: 400, ease: 'linear' } }] },
  { name: 'unitsAndAngles', frames: 30, targets: { a: { w: '10px', r: '0deg', e: '1em', s: 2 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      w: '100px', r: '1turn', e: 3, s: '4rem', duration: 400, ease: 'inOutSine' } }] },
  { name: 'colors', frames: 30, targets: { a: { c: '#ff0000', d: 'rgb(0, 0, 0)', e: 'hsl(120, 100%, 50%)', f: '#0f08' } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      c: 'rgba(0, 0, 255, 0.5)', d: '#ffffff', e: 'hsla(240, 100%, 50%, .25)', f: '#f00', duration: 400, ease: 'linear' } }] },
  { name: 'complex', frames: 30, targets: { a: { s: 'translate(0px, 10px) scale(1)', t: '0 0 0px #000' } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      s: 'translate(100px, 50px) scale(2.5)', t: '10 20 5px #000', duration: 400, ease: 'outCubic' } }] },
  { name: 'playbackRateFrameRate', frames: 30, targets: { a: { x: 0 }, b: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 800, ease: 'linear', playbackRate: 2 } },
            { frame: 0, op: 'animate', id: 'B', targets: ['b'], params: { x: 100, duration: 500, ease: 'linear', frameRate: 10 } }] },
  { name: 'playbackEase', frames: 30, targets: { a: { x: 0, y: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, y: [{ to: 50 }, { to: 0 }], duration: 500, playbackEase: 'inOutSine' } }] },
  { name: 'easeForms', frames: 25, targets: { a: { b: 0, s: 0, l: 0, p: 0, e: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: {
      b: { to: 100, ease: { $ease: ['cubicBezier', 0.68, -0.55, 0.265, 1.55] } },
      s: { to: 100, ease: { $ease: ['steps', 4] } },
      l: { to: 100, ease: { $ease: ['linear', 0, '0.8 25%', 1] } },
      p: { to: 100, ease: 'outElastic(1, .6)' },
      e: { to: 100, ease: 'inOutBack(2)' },
      duration: 400 } }] },
  { name: 'spring', frames: 70, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, ease: { $spring: { bounce: 0.5, duration: 400 } } } }] },
  { name: 'staggerDelay', frames: 40, targets: { a: { x: 0 }, b: { x: 0 }, c: { x: 0 }, d: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a', 'b', 'c', 'd'], params: {
      x: { $stagger: [[10, 40]] }, delay: { $stagger: [100, { from: 'center' }] }, duration: { $fn: ['index', 100] }, ease: 'linear' } }] },
  { name: 'modifier', frames: 20, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 10, modifier: { $modifier: ['round', 0] }, duration: 300, ease: 'linear' } }] },
  { name: 'replace', frames: 50, targets: { a: { x: 0, y: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, y: 100, duration: 800, ease: 'linear' } },
            { frame: 15, op: 'animate', id: 'B', targets: ['a'], params: { x: -100, duration: 400, ease: 'linear' } }] },
  { name: 'replaceWhole', frames: 50, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 800, ease: 'linear' } },
            { frame: 10, op: 'animate', id: 'B', targets: ['a'], params: { x: -100, duration: 400, ease: 'linear' } }] },
  { name: 'compositionNone', frames: 50, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 800, ease: 'linear', composition: 'none' } },
            { frame: 10, op: 'animate', id: 'B', targets: ['a'], params: { x: -100, duration: 400, ease: 'linear', composition: 'none' } }] },
  { name: 'blend', frames: 50, targets: { a: { x: 0, c: '#000000' } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, c: '#ff0000', duration: 600, ease: 'linear', composition: 'blend' } },
            { frame: 10, op: 'animate', id: 'B', targets: ['a'], params: { x: 300, c: '#0000ff', duration: 400, ease: 'outQuad', composition: 'blend' } }] },
  { name: 'controls', frames: 90, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 1000, ease: 'linear' } },
            { frame: 10, op: 'call', id: 'A', method: 'pause' },
            { frame: 15, op: 'call', id: 'A', method: 'resume' },
            { frame: 20, op: 'call', id: 'A', method: 'seek', args: [700] },
            { frame: 25, op: 'call', id: 'A', method: 'reverse' },
            { frame: 35, op: 'call', id: 'A', method: 'play' },
            { frame: 45, op: 'call', id: 'A', method: 'restart' },
            { frame: 50, op: 'call', id: 'A', method: 'stretch', args: [500] },
            { frame: 55, op: 'call', id: 'A', method: 'alternate' },
            { frame: 60, op: 'call', id: 'A', method: 'complete' },
            { frame: 65, op: 'call', id: 'A', method: 'restart' },
            { frame: 70, op: 'call', id: 'A', method: 'revert' },
            { frame: 75, op: 'call', id: 'A', method: 'restart' },
            { frame: 80, op: 'call', id: 'A', method: 'cancel' }] },
  { name: 'setProgress', frames: 30, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['a'], params: { x: 100, duration: 1000, ease: 'linear', autoplay: false } },
            { frame: 5, op: 'setProp', id: 'A', prop: 'progress', value: 0.5 },
            { frame: 10, op: 'setProp', id: 'A', prop: 'currentTime', value: 250 },
            { frame: 15, op: 'call', id: 'A', method: 'play' },
            { frame: 20, op: 'setProp', id: 'A', prop: 'speed', value: 3 }] },
  { name: 'timer', frames: 50, targets: {},
    steps: [{ frame: 0, op: 'timer', id: 'T', params: { duration: 300, loop: 2, delay: 50, loopDelay: 40 } }] },
  { name: 'timerInfinite', frames: 20, targets: {},
    steps: [{ frame: 0, op: 'timer', id: 'T', params: { playbackRate: 0.5 } },
            { frame: 15, op: 'call', id: 'T', method: 'pause' }] },
  { name: 'timeline', frames: 70, targets: { a: { x: 0, y: 0, z: 0 } },
    steps: [{ frame: 0, op: 'timeline', id: 'TL', params: { defaults: { duration: 200, ease: 'linear' }, loop: 1, alternate: true }, children: [
      ['add', 'C1', ['a'], { x: 100 }],
      ['label', 'L'],
      ['add', 'C2', ['a'], { y: 50 }, '-=100'],
      ['set', 'S', ['a'], { z: 5 }, 'L+=50'],
      ['call', 'CB', '<<'],
      ['add', 'C3', ['a'], { x: 0, ease: 'outQuad' }, '<'],
      ['timer', 'T', { duration: 100 }, '+=50'],
    ] }] },
  { name: 'timelineStagger', frames: 40, targets: { a: { x: 0 }, b: { x: 0 }, c: { x: 0 } },
    steps: [{ frame: 0, op: 'timeline', id: 'TL', params: { defaults: { ease: 'inOutQuad' } }, children: [
      ['add', 'C', ['a', 'b', 'c'], { x: 100, duration: 300 }, { $stagger: [100] }],
      ['add', 'D', ['a', 'b', 'c'], { x: 0, duration: 200 }, { $stagger: [50, { from: 'last', start: 100 }] }],
    ] }] },
  { name: 'timelineChain', frames: 50, targets: { a: { x: 0 } },
    steps: [{ frame: 0, op: 'timeline', id: 'TL', params: { playbackEase: 'inQuad', defaults: { ease: 'linear' } }, children: [
      ['add', 'C1', ['a'], { x: 100, duration: 300 }],
      ['add', 'C2', ['a'], { x: '+=50', duration: 300 }],
      ['add', 'C3', ['a'], { x: { from: 0 }, duration: 300 }, 100],
    ] },
            { frame: 30, op: 'call', id: 'TL', method: 'seek', args: [200] },
            { frame: 35, op: 'call', id: 'TL', method: 'reverse' }] },
  { name: 'timelineSync', frames: 40, targets: { a: { x: 0 }, b: { y: 0 } },
    steps: [{ frame: 0, op: 'animate', id: 'A', targets: ['b'], params: { y: 100, duration: 400, ease: 'linear', autoplay: false } },
            { frame: 0, op: 'timeline', id: 'TL', params: { defaults: { ease: 'linear' } }, children: [
      ['add', 'C1', ['a'], { x: 100, duration: 300 }],
      ['sync', 'A', 100],
    ] }] },
];

const recordings = recordingCases.map(c => {
  // fresh state
  fakeNow += 10000;
  engine.update();
  const targets = {};
  for (const name of Object.keys(c.targets)) targets[name] = { ...c.targets[name] };
  const objects = {};
  const log = [];
  const withCallbacks = (id, params) => {
    const p = { ...resolveMarkers(params) };
    for (const cb of callbackNames) p[cb] = () => log.push(`${id}.${cb}`);
    return p;
  };
  const frames = [];
  const dt = c.dt || 20;
  for (let frame = 0; frame < c.frames; frame++) {
    for (const step of c.steps.filter(s => s.frame === frame)) {
      if (step.op === 'animate') {
        const anim = animate(step.targets.map(t => targets[t]), withCallbacks(step.id, step.params));
        anim.then(() => log.push(`${step.id}.then`));
        objects[step.id] = anim;
      } else if (step.op === 'timer') {
        objects[step.id] = createTimer(withCallbacks(step.id, step.params));
      } else if (step.op === 'timeline') {
        const tl = createTimeline(withCallbacks(step.id, step.params));
        for (const child of step.children) {
          const [kind, id, ...args] = child;
          if (kind === 'add') tl.add(args[0].map(t => targets[t]), withCallbacks(id, args[1]), resolveMarkers(args[2]));
          else if (kind === 'set') tl.set(args[0].map(t => targets[t]), resolveMarkers(args[1]), args[2]);
          else if (kind === 'label') tl.label(id, args[0]);
          else if (kind === 'call') tl.call(() => log.push(`${id}.call`), args[0]);
          else if (kind === 'timer') tl.add(withCallbacks(id, args[0]), args[1]);
          else if (kind === 'sync') tl.sync(objects[id], args[0]);
        }
        objects[step.id] = tl;
      } else if (step.op === 'call') {
        objects[step.id][step.method](...(step.args || []));
      } else if (step.op === 'setProp') {
        objects[step.id][step.prop] = step.value;
      }
    }
    fakeNow += dt;
    engine.update();
    const values = {};
    for (const name of Object.keys(targets)) values[name] = { ...targets[name] };
    const state = {};
    for (const id of Object.keys(objects)) {
      const o = objects[id];
      state[id] = { currentTime: o.currentTime, progress: o.progress, iteration: o.currentIteration,
                    paused: o.paused, began: o.began, completed: o.completed, reversed: o.reversed };
    }
    frames.push({ values, callbacks: log.splice(0), state });
  }
  for (const id of Object.keys(objects)) objects[id].cancel();
  return { ...c, dt, recorded: frames };
});

const reference = {
  generator: 'engine/tests/htmlui/anime/gen_reference.mjs',
  anime: 'Anime.js 4.5.0',
  easings, springs, staggers, timelines, positions, decomposed, colors, utils: utilsReference, recordings,
};

writeFileSync(join(here, 'reference.json'), JSON.stringify(reference, null, 1) + '\n');
console.log(`reference.json: ${Object.keys(easings).length} easings, ${springs.length} springs, ${staggers.length} staggers, ` +
            `${timelines.length} timelines, ${recordings.length} recordings`);
process.exit(0);
