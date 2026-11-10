// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The dump of Pachislop's presets (S84 R19), evaluated by Pachislop's own code: Pachislop by
// SafEight, used with the author's permission. dump.mjs copies the clone's src/ next to this file
// in the scratch directory and bundles it from there, so the `./src/...` imports resolve only in
// that copy. `__dump_*` names are module-private tables dump.mjs exports from the copy (EXPOSE in
// dump.mjs); nothing in the copy is edited.
//
// buildDump() returns Pachislop's objects as they are; dump.mjs serialises them and refuses any
// function, symbol or non-plain value left in them. Only the colourway tables' thunks (Paint.fill,
// Paint.m3d, side3d) are evaluated here, and the registries' hooks dropped with their names listed.
// Every id comes from the seeded uid() shim, so two runs give the same object.

import { applyPresetToProject } from './src/presets/apply';
import {
  __dump_CORE_SPAN,
  __dump_COLOURWAY_DEFS,
  __dump_DARK_LUMA,
  __dump_LIGHT,
  __dump_LIGHT_EFFECTS,
  __dump_ON_RAMP,
  __dump_OWN_COLOURS,
  __dump_PATTERN_GROUND,
  __dump_RAMPS,
  __dump_SPECTRUM_FILL,
  __dump_WHITE_CORE,
  applyColourway,
  applyColourwayToProject,
  COLOURWAY_INFO,
  tintOrb,
} from './src/presets/colourways';
import { __dump_orbBody, FACTORY_STYLES, fx } from './src/presets/factory';
import { lintStyle } from './src/presets/lint';
import { TEMPLATES } from './src/presets/templates';
import { COLOURWAYS, type Colourway, type StylePreset } from './src/presets/types';
import { listEffects } from './src/effects/registry';
import { listGenerators } from './src/generators/registry';
import { companionsOf, ownGroupOf } from './src/model/companions';
import {
  __dump_PRESET_VALUES,
  createProject,
  createTitle2DLayer,
  createTitle3DLayer,
  erode2D,
  GRADIENTS,
  linearFill,
  material2D,
  material3D,
  rim2D,
  shapeSource,
  solidFill,
} from './src/model/defaults';
import type { Layer, MaterialPreset, Project, Title2DLayer, Title3DLayer } from './src/model/types';
import { english, formatMsg } from './src/store/msg';

/** Reads `src/<rel>` of the scratch copy (dump.mjs supplies it; the bundle itself touches no files). */
export type ReadSource = (rel: string) => string;

type Json = null | boolean | number | string | Json[] | { [k: string]: Json };
type TitleLayer = Title2DLayer | Title3DLayer;

const TEXT_2D = '確定';
const TEXT_3D = 'BIG';

/** The files that define looks (FACTORY_STYLES); templates.ts only uses them. */
const LOOK_FILES = ['factory.ts', 'factory3dMore.ts', 'factory3dSetA.ts', 'factory3dSetB.ts'];

/** The ten looks besides the eleven outlines whose colourways are dumped: 2D and 3D, metals, patterns, glows, plates, orbs, companions. */
const COLOURWAY_CASE_LOOKS = [
  'f-gold-classic',
  'f-chrome',
  'f-neon',
  'f-fire-gekiatsu',
  'f-pat-zebra',
  'f-plate-red',
  'f-3d-gold',
  'f-3d-candy',
  'f-orb-candy-blue',
  'f-fx-jackpot',
];

// ---------- plain data ----------

/** `v` with its zero-argument functions replaced by their results (Paint.fill, Paint.m3d, side3d); a function with arguments is refused. */
function evaluated(v: unknown, path: string): Json {
  if (typeof v === 'function') {
    if (v.length !== 0) throw new Error(`${path} is a function of ${v.length} arguments`);
    return evaluated((v as () => unknown)(), `${path}()`);
  }
  if (Array.isArray(v)) return v.map((x, i) => evaluated(x, `${path}[${i}]`));
  if (v !== null && typeof v === 'object') {
    const out: Record<string, Json> = {};
    for (const [k, x] of Object.entries(v)) if (x !== undefined) out[k] = evaluated(x, `${path}.${k}`);
    return out;
  }
  return v as Json;
}

/** `def` without its functions (render hooks, reach, apply...); their names go to `functions`. */
function definition(def: object): Json {
  const out: Record<string, Json> = {};
  const functions: string[] = [];
  for (const [k, v] of Object.entries(def)) {
    if (typeof v === 'function') functions.push(k);
    else if (v !== undefined) out[k] = v as Json;
  }
  out.functions = functions.sort();
  return out;
}

// ---------- source lines ----------

function lineOf(text: string, index: number): number {
  let n = 1;
  for (let i = 0; i < index; i++) if (text.charCodeAt(i) === 10) n++;
  return n;
}

const escapeRe = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/**
 * Where look `id` is defined in the look files: the first `id: '<id>'` (match "id"), else the
 * first `'<id>'` literal, an argument of a look builder (`glassLook('f-glass-red', ...)`, match
 * "literal"), else the one template literal whose static parts match the id
 * (`f-orb-${finish}-${colourway}`, match "template"). Null when none or several template
 * literals match.
 */
function lookSource(read: ReadSource, id: string): Json {
  const texts = LOOK_FILES.map((f) => ({ file: `src/presets/${f}`, text: read(`presets/${f}`) }));
  for (const re of [new RegExp(`\\bid:\\s*'${escapeRe(id)}'`), new RegExp(`'${escapeRe(id)}'`)])
    for (const { file, text } of texts) {
      const m = re.exec(text);
      if (m) return { file, line: lineOf(text, m.index), match: re.source.startsWith('\\bid') ? 'id' : 'literal' };
    }
  const hits: { file: string; line: number }[] = [];
  for (const { file, text } of texts)
    for (const m of text.matchAll(/`([^`\n]*\$\{[^`\n]*)`/g)) {
      const parts = m[1].split(/\$\{[^}]*\}/);
      if (parts.every((p) => p === '')) continue;
      if (new RegExp(`^${parts.map(escapeRe).join('[^]+')}$`).test(id)) hits.push({ file, line: lineOf(text, m.index) });
    }
  return hits.length === 1 ? { ...hits[0], match: 'template' } : null;
}

/** Line of `<key>: {` or `<key>: [` in `file` after the line that declares `table`. */
function tableLine(read: ReadSource, file: string, table: string, key: string): number {
  const text = read(file);
  const start = new RegExp(`\\bconst ${escapeRe(table)}\\b`).exec(text);
  if (!start) throw new Error(`No table ${table} in ${file}`);
  const re = new RegExp(`^\\s+${escapeRe(key)}: [\\[{]`, 'm');
  const m = re.exec(text.slice(start.index));
  if (!m) throw new Error(`No entry ${key} of ${table} in ${file}`);
  return lineOf(text, start.index + m.index);
}

// ---------- layers ----------

const isOrbLook = (look: StylePreset) => look.id.startsWith('f-orb-');

/**
 * A new title layer of the look's kind: text 確定 (2D) or BIG (3D); 保留 orb looks go on an orb
 * shape, the only source Pachislop gives them (applyColourway tints orbs only there).
 */
function freshLayer(look: StylePreset): TitleLayer {
  if (look.kind === 'title2d') return createTitle2DLayer(TEXT_2D);
  const l = createTitle3DLayer(TEXT_3D);
  return isOrbLook(look) ? { ...l, name: 'Orb', source: shapeSource('orb', l.source.size) } : l;
}

/** A project holding one new layer with `look` applied (applyPresetToProject: applyPreset, then its companions and group effects). */
function lookProject(look: StylePreset): { project: Project; id: string } {
  const layer = freshLayer(look);
  const project = applyPresetToProject({ ...createProject(), layers: [layer] }, layer.id, look, { applyFont: false });
  return { project, id: layer.id };
}

const layerOf = (p: Project, id: string): Layer => {
  const l = p.layers.find((x) => x.id === id);
  if (!l) throw new Error(`No layer ${id}`);
  return l;
};

// ---------- sections ----------

function looks(read: ReadSource): Json {
  const seen = new Set<string>();
  return FACTORY_STYLES.map((preset, index) => {
    if (seen.has(preset.id)) throw new Error(`Duplicate look id ${preset.id}`);
    seen.add(preset.id);
    const source = lookSource(read, preset.id);
    return { index, ...(source ? { source } : {}), preset: preset as unknown as Json };
  });
}

function colourways(read: ReadSource): Json {
  const file = 'presets/colourways.ts';
  const defs = __dump_COLOURWAY_DEFS as Record<Colourway, object>;
  const finishes = ['candy', 'glass', 'metal'] as const;
  return {
    order: [...COLOURWAYS],
    COLOURWAY_DEFS: Object.fromEntries(
      COLOURWAYS.map((c) => [c, { line: tableLine(read, file, 'COLOURWAY_DEFS', c), def: evaluated(defs[c], `COLOURWAY_DEFS.${c}`) }]),
    ),
    COLOURWAY_INFO: COLOURWAY_INFO as unknown as Json,
    LIGHT: Object.fromEntries(COLOURWAYS.map((c) => [c, { line: tableLine(read, file, 'LIGHT', c), ramp: __dump_LIGHT[c] }])),
    RAMPS: __dump_RAMPS as unknown as Json,
    DARK_LUMA: __dump_DARK_LUMA,
    WHITE_CORE: __dump_WHITE_CORE,
    ON_RAMP: __dump_ON_RAMP,
    CORE_SPAN: __dump_CORE_SPAN,
    PATTERN_GROUND: __dump_PATTERN_GROUND,
    SPECTRUM_FILL: __dump_SPECTRUM_FILL as unknown as Json,
    OWN_COLOURS: Object.keys(__dump_OWN_COLOURS),
    LIGHT_EFFECTS: Object.keys(__dump_LIGHT_EFFECTS),
    GRADIENTS: GRADIENTS as unknown as Json,
    // tintOrb on the body of each 保留 finish (factory.ts orbBody), as orbLook uses it.
    orb_bodies: Object.fromEntries(finishes.map((f) => [f, __dump_orbBody(f) as unknown as Json])),
    tint_orb: Object.fromEntries(
      finishes.map((f) => [f, Object.fromEntries(COLOURWAYS.map((c) => [c, tintOrb(__dump_orbBody(f), c) as unknown as Json]))]),
    ),
  };
}

function templates(read: ReadSource): Json {
  const text = read('presets/templates.ts');
  return TEMPLATES.map((t) => {
    const m = new RegExp(`\\bid:\\s*'${escapeRe(t.id)}'`).exec(text);
    return { id: t.id, name: t.name, ...(m ? { line: lineOf(text, m.index) } : {}), project: t.build() as unknown as Json };
  });
}

function materials3d(read: ReadSource): Json {
  const values = __dump_PRESET_VALUES as Record<Exclude<MaterialPreset, 'custom'>, object>;
  const presets = Object.keys(values) as Exclude<MaterialPreset, 'custom'>[];
  return {
    custom: material3D('custom') as unknown as Json,
    presets: presets.map((p) => ({
      preset: p,
      line: tableLine(read, 'model/defaults.ts', 'PRESET_VALUES', p),
      values: values[p] as Json,
      material: material3D(p) as unknown as Json,
    })),
  };
}

function colourwayCases(): Json {
  const ids = [...COLOURWAYS.map((c) => `f-outline-${c}`), ...COLOURWAY_CASE_LOOKS];
  const out: Json[] = [];
  for (const id of ids) {
    const look = FACTORY_STYLES.find((s) => s.id === id);
    if (!look) throw new Error(`No look ${id}`);
    // One layer per look, recoloured in each colourway: the eleven `layer_before`s are the same layer.
    const { project, id: layerId } = lookProject(look);
    const before = layerOf(project, layerId);
    const companions = companionsOf(project.layers, layerId);
    const group = ownGroupOf(project.layers, layerId);
    for (const colourway of COLOURWAYS) {
      const after = applyColourway(before, colourway);
      const c: Record<string, Json> = {
        look: id,
        colourway,
        source_kind: before.kind === 'title2d' || before.kind === 'title3d' ? before.source.kind : null,
        layer_before: before as unknown as Json,
        layer_after: after as unknown as Json,
      };
      if (companions.length || group) {
        // The rest of the look as the Library recolours it (applyColourwayToProject): its companions and its group's effects.
        const recoloured = applyColourwayToProject(project, layerId, colourway);
        c.companions_before = companions as unknown as Json;
        c.companions_after = companionsOf(recoloured.layers, layerId) as unknown as Json;
        if (group) {
          c.group_effects_before = group.effects as unknown as Json;
          c.group_effects_after = (ownGroupOf(recoloured.layers, layerId)?.effects ?? []) as unknown as Json;
        }
      }
      out.push(c);
    }
  }
  return out;
}

// ---------- lint ----------

/** The eight checks of lint.ts: its seven RULES, the no-op rule as its two settings (Hollow letters, かすれ). */
const LINT_CHECKS: { rule: string; ids: RegExp }[] = [
  { rule: 'contrast', ids: /^contrast-\d+$/ },
  { rule: 'hues', ids: /^hues$/ },
  { rule: 'thin', ids: /^thin$/ },
  { rule: 'thick', ids: /^thick$/ },
  { rule: 'shadow', ids: /^shadow$/ },
  { rule: 'span', ids: /^span$/ },
  { rule: 'noop-knockout', ids: /^knockout-(rims|face)$/ },
  { rule: 'noop-erode', ids: /^erode-zero$/ },
];

const solid = (color: string) => material2D(solidFill(color));
const plain2D = (text = TEXT_2D): Title2DLayer => createTitle2DLayer(text);
const withStyle = (l: Title2DLayer, s: Partial<Title2DLayer['style']>): Title2DLayer => ({ ...l, style: { ...l.style, ...s } });
/** A white face with one black keyline of `width` % of the size (well contrasted, total in 5..16 %). */
const keylined = (width = 7, face = '#ffffff') => withStyle(plain2D(), { face: solid(face), rims: [rim2D(width, solid('#0a0604'))] });

/**
 * One bad layer per check (`red-<rule>`, exactly that check fires) and its fixed twin
 * (`good-<rule>`, nothing fires); entry asserts both below.
 */
function lintControls(): { rule: string; red: TitleLayer; good: TitleLayer }[] {
  const rainbowFace = (span: 'layer' | 'line') => material2D(linearFill(GRADIENTS.rainbowFill, 90, span));
  return [
    // White face, a near-white rim: ΔL* far below 12. Twin: a black keyline.
    { rule: 'contrast', red: withStyle(plain2D(), { rims: [rim2D(7, solid('#e6e6e6'))] }), good: keylined() },
    // Yellow face, dark blue rim, red rim: three hue families. Twin: the outer rim white.
    {
      rule: 'hues',
      red: withStyle(plain2D(), { face: solid('#ffe000'), rims: [rim2D(3, solid('#0020a0')), rim2D(4, solid('#ff2020'))] }),
      good: withStyle(plain2D(), { face: solid('#ffe000'), rims: [rim2D(3, solid('#0020a0')), rim2D(4, solid('#ffffff'))] }),
    },
    // Rims total 2 % (< 5 %). Twin: 7 %.
    { rule: 'thin', red: keylined(2), good: keylined(7) },
    // Rims total 20 % (> 16 %). Twin: 7 %.
    { rule: 'thick', red: keylined(20), good: keylined(7) },
    // Opaque black drop shadow with almost no blur. Twin: 0.6 opacity, a soft blur, tinted dark.
    {
      rule: 'shadow',
      red: { ...keylined(), effects: [fx('dropShadow', { opacity: 1, size: 2, color: '#000000' })] },
      good: { ...keylined(), effects: [fx('dropShadow', { opacity: 0.6, size: 14, color: '#3a1800' })] },
    },
    // Two lines, a rainbow face spanning the whole block. Twin: spanning each line.
    {
      rule: 'span',
      red: withStyle(plain2D(`${TEXT_2D}\n${TEXT_2D}`), { face: rainbowFace('layer'), rims: [rim2D(7, solid('#ffffff'))] }),
      good: withStyle(plain2D(`${TEXT_2D}\n${TEXT_2D}`), { face: rainbowFace('line'), rims: [rim2D(7, solid('#ffffff'))] }),
    },
    // Hollow letters without rims. Twin: hollow letters over a keyline, the face fill transparent.
    {
      rule: 'noop-knockout',
      red: withStyle(plain2D(), { knockout: true }),
      good: withStyle(keylined(), { knockout: true, face: { ...solid('#ffffff'), opacity: 0 } }),
    },
    // かすれ on at amount 0. Twin: its starting amount and edge.
    { rule: 'noop-erode', red: withStyle(keylined(), { erode: erode2D({ enabled: true, amount: 0 }) }), good: withStyle(keylined(), { erode: erode2D({ enabled: true }) }) },
  ];
}

/** lintStyle's issues as data: the message and rule as Pachislop stores them, their English text, the fix's label (its function dropped). */
function findings(l: TitleLayer): Json {
  return lintStyle(l).map((i) => ({
    id: i.id,
    message: i.message as unknown as Json,
    message_en: formatMsg(i.message, english),
    rule: i.rule as unknown as Json,
    rule_en: formatMsg(i.rule, english),
    ...(i.fix ? { fix_label: i.fix.label } : {}),
  }));
}

function lintCases(): Json {
  const out: Json[] = [];
  for (const look of FACTORY_STYLES) {
    const { project, id } = lookProject(look);
    const layer = layerOf(project, id) as TitleLayer;
    out.push({ case: look.id, layer: layer as unknown as Json, findings: findings(layer) });
  }
  for (const { rule, red, good } of lintControls()) {
    const check = LINT_CHECKS.find((c) => c.rule === rule)!;
    const bad = lintStyle(red);
    if (bad.length !== 1 || !check.ids.test(bad[0].id)) throw new Error(`red-${rule} fires ${JSON.stringify(bad.map((i) => i.id))}, not exactly ${check.ids}`);
    const fixed = lintStyle(good);
    if (fixed.length) throw new Error(`good-${rule} fires ${JSON.stringify(fixed.map((i) => i.id))}`);
    out.push({ case: `red-${rule}`, rule, layer: red as unknown as Json, findings: findings(red) });
    out.push({ case: `good-${rule}`, rule, layer: good as unknown as Json, findings: findings(good) });
  }
  if (LINT_CHECKS.length !== 8) throw new Error('lint.ts has eight checks');
  return out;
}

// ---------- the dump ----------

export function buildDump(read: ReadSource, commit: string): Json {
  return {
    meta: {
      commit,
      generated_by: 'tools/titleart/pachislop-port/dump.mjs',
      texts: { title2d: TEXT_2D, title3d: TEXT_3D },
      lint_checks: LINT_CHECKS.map((c) => c.rule),
    },
    looks: looks(read),
    colourways: colourways(read),
    templates: templates(read),
    effects: listEffects().map(definition),
    generators: listGenerators().map(definition),
    materials3d: materials3d(read),
    colourway_cases: colourwayCases(),
    lint_cases: lintCases(),
  };
}
