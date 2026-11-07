#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Imports Material Maker's node library as data into editor/procmat/nodes/ (S90 R17 to R20).

    python3 -I tools/procmat/import_mm_nodes.py --source <scratch clone> [--out DIR] [--check]

Run once, by a developer, on a scratch clone. The clone is untrusted: this script only parses its
files as data and runs nothing from it. It exits non-zero on anything it does not understand: an
attribution comment outside ATTRIBUTIONS, a float hash, a macro outside the closed set, a field
that executes or fetches (R3). `--check` writes nothing and fails if the output would change.

What it writes (all deterministic, sorted JSON; `format` is FORMAT_VERSION):

  _functions.json   id -> {name, kind, code, deps[], notices[]}: every GLSL declaration a node can
                    use, shared by id (R5). Text differing under one name gets a suffixed name,
                    so a generated shader never depends on which node's text came first.
  _types.json       the port types: GLSL type, parameter list, conversions.
  _library.json     the add-node menu: tree path, display name, aliases, descriptor, preset values.
  _manifest.json    source commit and what was left out, with the reason.
  <id>.json         one descriptor per node type, the input of the C++ expander:

    id, type_version, kind ("shader" | "graph"), name, description, short_description,
    source {file, commit}, notices [{license, holder, year, url, node}]
    shader body:  functions[]  ids in _functions.json, the expander emits them in dependency order
                  instance[], code[]  lines of GLSL with macros, emitted once / per variant
                  inputs[]     {name, type, default, function, label, ...}
                  outputs[]    {type, expr, label, ...}
                  parameters[] {name, type, label, default, min, max, step, values[], first, last}
    graph body:   nodes[] {id, type | shader | graph, parameters, seed, ports, version, generic_size}
                  a parameter value is a number, or a string in the closed grammar of
                  PARAMETER_EXPRESSION, which the importer has checked
                  connections[] {from, from_port, to, to_port}, inputs[], outputs[]
                  parameters[] as above plus links[] {node, param} or configurations{}

Macros are Material Maker's, resolved by the expander at expansion time: $uv, $(uv), $seed, $time,
$name, $(name), $(name_uv), $node_id, $?1..$?4, $<parameter>, $<input>(uv), $<input>.size,
$<input>.texture, $<input>.variation(uv, seed, variation), and #, #count, #for..#end in generic
nodes. Three things Material Maker does at run time are already done here, so the expander has no
regular expression over GLSL: instance functions carry their `_seed_variation_` parameters, global
functions are resolved by id, and identifiers are renamed by token.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path

FORMAT_VERSION = 1
MM_COMMIT = "7d6daae"
NODE_DIR = "addons/material_maker/nodes"
FUNCTIONS_FILE = "addons/material_maker/shader_functions.tres"
LIBRARY_FILE = "material_maker/library/base.json"
ALIASES_FILE = "material_maker/library/aliases.json"

# R18: never imported as they stand; a dependant goes with them
DENY_LIST = {
    "skewed_bricks": "CC BY-SA 3.0 code (get_line_intersection); may return after a rewrite from scratch",
    "skewed_uneven_bricks": "uses skewed_bricks' CC BY-SA 3.0 code",
    "shard_fbm": "adapted from a Shadertoy with no stated licence; also a float hash",
    "voronoi_triangle": "based on a Shadertoy with no stated licence",
    "splines": "cites a Shadertoy with no stated licence",
    "hbao": "cites a gist and a repository with no stated licence",
    "sdf3d_fbm": "cites an article with no stated licence for the code",
    "sdf3d_fbm2": "cites an article with no stated licence for the code",
}

# nodes Material Maker implements in GDScript; the evaluator provides the first group (R20, R5)
HOST_NODES = {"buffer", "iterate_buffer", "switch", "image"}
UI_ONLY_NODES = {"ios", "remote", "comment"}
LEFT_OUT_HOST_NODES = {
    "text": "needs Godot's text renderer",
    "sdf": "the Easy SDF builder is 58 GDScript files",
    "meshmap": "Mesh Map nodes are out of scope",
    "portal": "wireless connections are not part of S90",
    "custom": "user-written GLSL is out of scope",
    "remote": "a graph parameter panel, part of graph nodes only",
    "comment": "not a node of the evaluator",
    "reroute": "a canvas convenience, not a node of the evaluator",
    "export": "Material Maker's export system is not taken",
    "autoexport": "Material Maker's export system is not taken",
    "debug": "a Material Maker debugging aid",
    "comment_line": "not a node of the evaluator",
}

# R19: the nodes under their own notices, as their comments state them
_IQ = "Inigo Quilez"
_GG = "Garrett Gunnell"
_MM = "Michele Morrone"
OWN_NOTICES = {
    "sdarrow": ("MIT", _IQ, "2021", "https://www.shadertoy.com/view/slj3Dd"),
    "sdellipse": ("MIT", _IQ, "2015", ""),
    "sdstairs": ("MIT", _IQ, "2022", "https://www.shadertoy.com/view/7tKSWt"),
    "sdstar": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/3tSGDy"),
    "sdtunnel": ("MIT", _IQ, "2021", "https://www.shadertoy.com/view/flSSDy"),
    "sdf3d_box_frame": ("MIT", _IQ, "2020", "https://www.shadertoy.com/view/3ljcRh"),
    "sdf3d_cut_sphere": ("MIT", _IQ, "2021", "https://www.shadertoy.com/view/stKSzc"),
    "sdf3d_ellipsoid": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/tdS3DG"),
    "sdf3d_link": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/wlXSD7"),
    "sdf3d_octahedron": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/wsSGDG"),
    "sdf3d_pyramid": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/Ws3SDl"),
    "sdf3d_solid_angle": ("MIT", _IQ, "2019", "https://www.shadertoy.com/view/wtjSDW"),
    "voronoi": ("MIT", _IQ, "2013", "https://www.shadertoy.com/view/ldl3W8"),
    "voronoi2": ("MIT", _IQ, "2013", "https://www.shadertoy.com/view/ldl3W8"),
    "tex3d_voronoi": ("MIT", _IQ, "2013", "https://www.shadertoy.com/view/ldl3W8"),
    "fbm2": ("MIT", _IQ, "", "https://www.shadertoy.com/view/Xd23Dh"),
    "fbm3": ("MIT", _IQ, "", "https://www.shadertoy.com/view/Xd23Dh"),
    "fbm4": ("MIT", _IQ, "", "https://www.shadertoy.com/view/Xd23Dh https://www.shadertoy.com/view/clGyWm"),
    "anisotropic_kuwahara": ("MIT", _GG, "2022", "https://github.com/GarrettGunnell/Post-Processing/"),
    "classic_kuwahara": ("MIT", _GG, "2022", "https://github.com/GarrettGunnell/Post-Processing/"),
    "generalized_kuwahara": ("MIT", _GG, "", "https://github.com/GarrettGunnell/Post-Processing/"),
    "denoiser": ("BSD-2-Clause", _MM, "2018-2019", "https://github.com/BrutPitt/glslSmartDeNoise/"),
    "denoiser_shader": ("BSD-2-Clause", _MM, "2018-2019", "https://github.com/BrutPitt/glslSmartDeNoise/"),
    "tex3d_fbm_3": ("MIT", "David Hoskins", "2014", "https://www.shadertoy.com/view/4djSRW"),
    "symmetric_nearest_neighbor": ("MIT", "fiveko.com", "2017", "https://github.com/fiveko/fivekogfx/blob/master/src/modules/symmetricnn.js"),
}

_MIT = "The MIT License"
# Every comment matching ATTRIBUTION_TRIGGER must be listed here for its node, verbatim. New or
# changed text fails the import, because licences live in these comments (R17, R18).
ATTRIBUTIONS = {
    "anisotropic_kuwahara": {"The MIT License - Copyright (c) 2022 Garrett Gunnell",
                             "Adapted from https://github.com/GarrettGunnell/Post-Processing/"},
    "bricks_uneven4": {"We can then calculate the brick rects based on where we find the cuts in the surrounding cells."},
    "classic_kuwahara": {"The MIT License, Copyright (c) 2022 Garrett Gunnell",
                         "Adapated based on https://github.com/GarrettGunnell/Post-Processing/blob/871a7ffcc44174f237b4c0127db51026c438d7a4/Assets/Kuwahara%20Filter/Kuwahara.shader"},
    "convert_colorspace": {"Matrix coefficients taken from https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.709_conversion"},
    "denoiser": {"Copyright (c) 2018-2019 Michele Morrone", "All rights reserved.", "https://michelemorrone.eu - https://BrutPitt.com",
                 "https://github.com/BrutPitt/glslSmartDeNoise/",
                 "This software is distributed under the terms of the BSD 2-Clause license",
                 "Adapted for Material Maker by wojtekpil"},
    "denoiser_shader": {"Copyright (c) 2018-2019 Michele Morrone", "All rights reserved.", "https://michelemorrone.eu - https://BrutPitt.com",
                        "https://github.com/BrutPitt/glslSmartDeNoise/",
                        "This software is distributed under the terms of the BSD 2-Clause license"},
    "fbm2": {"Rotate by shift", "MIT License Inigo Quilez - https://www.shadertoy.com/view/Xd23Dh"},
    "fbm3": {"Rotate by shift", "MIT License Inigo Quilez - https://www.shadertoy.com/view/Xd23Dh"},
    "fbm4": {"Rotate by shift", "MIT License Inigo Quilez - https://www.shadertoy.com/view/Xd23Dh",
             "MIT License Inigo Quilez - https://www.shadertoy.com/view/clGyWm"},
    "gaussian_blur_x_hq": {"range based on 68–95–99.7 rule"},
    "gaussian_blur_y_hq": {"range based on 68–95–99.7 rule"},
    "generalized_kuwahara": {"MIT License - Garrett Gunnell",
                             "Based on https://github.com/GarrettGunnell/Post-Processing/blob/main/Assets/Kuwahara%20Filter/GeneralizedKuwahara.shader"},
    "runes": {"Dots (degenerate lines) are not cross-GPU safe without adding 0.001 - divide by 0 error."},
    "sdarrow": {"The MIT License, Copyright © 2021 Inigo Quilez", "https://www.shadertoy.com/view/slj3Dd"},
    "sdellipse": {"The MIT License, Copyright © 2015 Inigo Quilez"},
    "sdf3d_box_frame": {_MIT, "Copyright © 2020 Inigo Quilez", "source: https://www.shadertoy.com/view/3ljcRh"},
    "sdf3d_cut_sphere": {_MIT, "Copyright © 2021 Inigo Quilez", "source: https://www.shadertoy.com/view/stKSzc"},
    "sdf3d_ellipsoid": {"The MIT License, Copyright © 2019 Inigo Quilez", "source: https://www.shadertoy.com/view/tdS3DG"},
    "sdf3d_link": {_MIT, "Copyright © 2019 Inigo Quilez", "source: https://www.shadertoy.com/view/wlXSD7"},
    "sdf3d_octahedron": {"The MIT License, Copyright © 2019 Inigo Quilez", "https://www.shadertoy.com/view/wsSGDG"},
    "sdf3d_pyramid": {"The MIT License, Copyright © 2019 Inigo Quilez", "https://www.shadertoy.com/view/Ws3SDl"},
    "sdf3d_solid_angle": {_MIT, "Copyright © 2019 Inigo Quilez", "source: https://www.shadertoy.com/view/wtjSDW"},
    "sdstairs": {"The MIT License, Copyright © 2022 Inigo Quilez", "https://www.shadertoy.com/view/7tKSWt"},
    "sdstar": {"The MIT License, Copyright © 2019 Inigo Quilez", "https://www.shadertoy.com/view/3tSGDy"},
    "sdtunnel": {"The MIT License, Copyright © 2021 Inigo Quilez, https://www.shadertoy.com/view/flSSDy"},
    "symmetric_nearest_neighbor": {"Based on https://github.com/fiveko/fivekogfx/blob/master/src/modules/symmetricnn.js",
                                   _MIT, "Copyright © 2017 fiveko.com"},
    "tex3d_fbm_3": {_MIT, "Copyright © 2014 David Hoskins", "https://www.shadertoy.com/view/4djSRW (hash33)"},
    "tex3d_voronoi": {"Based on https://www.shadertoy.com/view/ldl3W8", _MIT, "Copyright © 2013 Inigo Quilez"},
    "voronoi": {"Based on https://www.shadertoy.com/view/ldl3W8", _MIT, "Copyright © 2013 Inigo Quilez"},
    "voronoi2": {"Based on https://www.shadertoy.com/view/ldl3W8", _MIT, "Copyright © 2013 Inigo Quilez"},
}
ATTRIBUTION_TRIGGER = re.compile(
    r"copyright|©|\(c\)|licen[cs]e|spdx|shadertoy|https?:|www\.|adapa?t|based on|ported|port of|source:|author|credit"
    r"|\bby\b|stolen|thanks|courtesy|taken from|inspired|derived|quilez|\bgpl\b|\blgpl\b|\bmit\b|\bbsd\b|apache"
    r"|cc[- ]by|public domain|all rights|permission is|warrant", re.IGNORECASE)
DOC_TRIGGER = re.compile(r"copyright|©|licen[cs]e|shadertoy|cc[- ]by", re.IGNORECASE)

# Our own replacement of the rand functions (R10): a hash of the coordinates quantized to 1/65536,
# so every tier that rounds a coordinate to the same integer gets the same bits. The mixing is
# MurmurHash3's (Austin Appleby, public domain). The output keeps 24 bits, which a float holds
# exactly: no device rounds it and it cannot reach 1.0. A float-to-int conversion out of range is
# undefined in GLSL, hence the clamp; coordinates past it are not a real input to these nodes.
HASH_FUNCTIONS = """
int jp_quantize(float x) {
	return int(floor(clamp(x, -32767.0, 32767.0) * 65536.0 + 0.5));
}

uint jp_hash_mix(uint h, int value) {
	uint k = uint(value) * 0xcc9e2d51u;
	k = ((k << 15u) | (k >> 17u)) * 0x1b873593u;
	h ^= k;
	h = (h << 13u) | (h >> 19u);
	return h * 5u + 0xe6546b64u;
}

float jp_hash_float(uint h, uint stream) {
	h ^= stream * 0x9e3779b9u;
	h ^= h >> 16u;
	h *= 0x85ebca6bu;
	h ^= h >> 13u;
	h *= 0xc2b2ae35u;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0 / 16777216.0);
}

uint jp_hash_vec2(vec2 x) {
	return jp_hash_mix(jp_hash_mix(0x2f8b15c7u, jp_quantize(x.x)), jp_quantize(x.y));
}

uint jp_hash_vec4(vec4 x) {
	return jp_hash_mix(jp_hash_mix(jp_hash_vec2(x.xy), jp_quantize(x.z)), jp_quantize(x.w));
}

// NaN has no defined 8-bit or filtered value, and each tier makes up its own: a pass writes zero
// instead, so a node's 0/0 is the same image everywhere (R12, R13)
vec4 jp_finite(vec4 c) {
	return vec4(isnan(c.x) ? 0.0 : c.x, isnan(c.y) ? 0.0 : c.y, isnan(c.z) ? 0.0 : c.z, isnan(c.w) ? 0.0 : c.w);
}

// The (34x + 1)x permutation of simplex noise, on integers: in floats it stops being exact at x = 702,
// a scale fbm reaches, and from there a GPU that fuses x - a*b gives another gradient (R10)
int jp_mod289(int i) {
	i -= 289 * (i / 289);
	return i < 0 ? i + 289 : i;
}

float jp_permute289(float x) {
	int i = jp_mod289(int(floor(clamp(x, -2147483000.0, 2147483000.0) + 0.5)));
	return float(jp_mod289((i * 34 + 1) * i));
}

float rand(vec2 x) {
	return jp_hash_float(jp_hash_vec2(x), 1u);
}

vec2 rand2(vec2 x) {
	uint h = jp_hash_vec2(x);
	return vec2(jp_hash_float(h, 1u), jp_hash_float(h, 2u));
}

vec3 rand3(vec2 x) {
	uint h = jp_hash_vec2(x);
	return vec3(jp_hash_float(h, 1u), jp_hash_float(h, 2u), jp_hash_float(h, 3u));
}

vec3 rand3_4d(vec4 x) {
	uint h = jp_hash_vec4(x);
	return vec3(jp_hash_float(h, 1u), jp_hash_float(h, 2u), jp_hash_float(h, 3u));
}
"""
REPLACED_COMMON_FUNCTIONS = {"rand", "rand2", "rand3", "rand3_4d"}
# Material Maker's host template declares these for every shader
HOST_CONSTANTS = "const float PI = 3.14159265359;\nconst float TAU = 2.0*PI;\n"
# What a float hash looks like: fract of a trigonometric function, a large constant with a fractional
# part, or a permutation by floats. Whoever adds a node with one is told by the import, not the GPU.
FLOAT_HASH = re.compile(r"fract\s*\(\s*(?:[\d.]+\s*\*\s*)?(sin|cos)\s*\(|fract\s*\([^;]*\b(sin|cos)\s*\([^;]*\*\s*\d{4,}"
                        r"|\b\d{5,}\.\d*[1-9]|floor\([^;]*\)\s*\*\s*289\.0")
# Float permutations in nodes (R10), and what replaces them. Their results are the same below x = 702.
PERMUTE_REPLACEMENTS = {name: f"float {name}(float x) {{\n\treturn jp_permute289(x);\n}}"
                        for name in ("fbm_2d_permute", "old_fbm_2d_permute")}
FLOAT_MOD289 = {"fbm_2d_mod289", "old_fbm_2d_mod289"}

COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
MACRO = re.compile(r"\$\(?[\w?]*\)?")
IDENTIFIER = re.compile(r"\$\(?[\w?]*\)?|(?<![\w.])[A-Za-z_]\w*")
MACRO_NAMES = {"uv", "name", "name_uv", "seed", "time", "node_id", "?1", "?2", "?3", "?4"}
FORBIDDEN_TEXT = ("$(expr:", "website:", "process_option_")
# A parameter of a node inside a graph may be an expression instead of a number. This is what the
# library writes (`0.005/$n_scale`, `$uv.x`, `3.14 / 2.0`), and all a data file may put in GLSL (R3).
PARAMETER_EXPRESSION = re.compile(r"(?:\$\w+(?:\.[xy])?|\d+(?:\.\d*)?|[-+*/(), ])+\Z")
MODEL_FIELDS = {"code", "global", "includes", "inputs", "instance", "longdesc", "name", "outputs", "parameters", "shortdesc"}
# fields of a data file that execute, fetch or only concern Godot (R3): dropped with the node
REFUSED_MODEL_FIELDS = {"custom", "exports", "preview_shader", "template", "files"}


class ImportFailure(Exception):
    pass


def strip_trailing_commas(text: str) -> str:
    """Removes the commas before a closing bracket, outside strings (smooth_curvature2, io_types)."""
    out = []
    in_string = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\":
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            out.append(c)
        elif c == ",":
            j = i + 1
            while j < len(text) and text[j] in " \t\r\n":
                j += 1
            if j >= len(text) or text[j] not in "}]":
                out.append(c)
        else:
            out.append(c)
        i += 1
    return "".join(out)


def load_data(path: Path):
    return json.loads(strip_trailing_commas(path.read_text(encoding="utf-8")))


def text_of(value) -> str:
    """Material Maker stores code as a string or as an array of lines."""
    if isinstance(value, list):
        return "\n".join(value)
    return value or ""


def lines_of(text: str) -> list[str]:
    return text.split("\n") if text.strip() else []


def code_strings(node, doc_keys=("shortdesc", "longdesc", "label", "name", "display_name", "icon_data", "text")):
    """Every string of a data file that can hold GLSL; doc fields come out with a None path."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key in doc_keys:
                if isinstance(value, str):
                    yield None, value
            else:
                yield from code_strings(value, doc_keys)
    elif isinstance(node, list):
        if node and all(isinstance(item, str) for item in node):
            yield "code", "\n".join(node)
        else:
            for item in node:
                yield from code_strings(item, doc_keys)
    elif isinstance(node, str):
        yield "code", node


def comment_lines(text: str):
    for comment in COMMENT.findall(text):
        body = comment[2:-2] if comment.startswith("/*") else comment[2:]
        for line in body.split("\n"):
            line = line.strip()
            if line:
                yield line


def check_attributions(node_id: str, data) -> None:
    allowed = ATTRIBUTIONS.get(node_id, set())
    found = set()
    for kind, text in code_strings(data):
        if kind is None:
            if DOC_TRIGGER.search(text):
                raise ImportFailure(f"{node_id}: unrecognised attribution in a description: {text[:120]!r}")
            continue
        for line in comment_lines(text):
            if ATTRIBUTION_TRIGGER.search(line):
                if line not in allowed:
                    raise ImportFailure(f"{node_id}: unrecognised attribution comment: {line!r}")
                found.add(line)
    missing = allowed - found
    if missing:
        raise ImportFailure(f"{node_id}: the known attribution comment is gone: {sorted(missing)[0]!r}")


def strip_comments(text: str) -> str:
    return COMMENT.sub(" ", text)


class Declaration:
    def __init__(self, name: str, kind: str, signature: str, text: str, origin: str):
        self.name, self.kind, self.signature, self.text, self.origin = name, kind, signature, text, origin
        self.normal = re.sub(r"\s+", " ", strip_comments(text)).strip()

    @property
    def key(self):
        return self.name, self.signature


def split_declarations(text: str, origin: str) -> list[Declaration]:
    """Splits a `global` text at the top level, with a declaration's leading comments kept on it."""
    decls = []
    i = 0
    n = len(text)
    start = 0
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end == -1 else end + 1
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
        elif c == "#":
            end = i
            while True:
                end = text.find("\n", end)
                if end == -1:
                    end = n
                    break
                if text[end - 1] != "\\":
                    break
                end += 1
            decls.append(_classify(text[start:end], origin))
            i = start = end
        else:
            depth = 0
            while i < n:
                if text.startswith("//", i):
                    end = text.find("\n", i)
                    i = n if end == -1 else end
                    continue
                if text.startswith("/*", i):
                    i = text.index("*/", i) + 2
                    continue
                c = text[i]
                if c in "({":
                    depth += 1
                elif c in ")}":
                    depth -= 1
                    if depth == 0 and c == "}":
                        j = i + 1
                        while j < n and text[j] in " \t\r\n":
                            j += 1
                        if j < n and text[j] == ";":
                            i = j
                        break
                elif c == ";" and depth == 0:
                    break
                i += 1
            i += 1
            decls.append(_classify(text[start:i], origin))
            start = i
    tail = strip_comments(text[start:]).strip()
    if tail:
        raise ImportFailure(f"{origin}: cannot read the end of its global code: {tail[:80]!r}")
    return [d for d in decls if d is not None]


def _classify(text: str, origin: str):
    code = strip_comments(text).strip()
    if code.startswith("#"):
        m = re.match(r"#\s*define\s+(\w+)", code)
        if not m:
            raise ImportFailure(f"{origin}: unsupported preprocessor line in its global code: {code[:80]!r}")
        return Declaration(m.group(1), "define", "", text, origin)
    m = re.match(r"struct\s+(\w+)", code)
    if m:
        return Declaration(m.group(1), "struct", "", text, origin)
    paren = code.find("(")
    assign = code.find("=")
    if paren != -1 and (assign == -1 or paren < assign):
        m = re.match(r"(?:[A-Za-z_]\w*\s+)*?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\(", code)
        if not m:
            raise ImportFailure(f"{origin}: cannot read a declaration in its global code: {code[:80]!r}")
        close = _matching(code, paren)
        params = []
        for param in code[paren + 1:close].split(","):
            words = [w for w in re.sub(r"\[.*?\]", " ", param).split()
                     if w not in ("in", "out", "inout", "const", "highp", "mediump", "lowp")]
            if words and words != ["void"]:
                params.append(words[0])
        rest = code[close + 1:].strip()
        if rest == ";":
            return None  # a prototype: the expander emits in dependency order
        return Declaration(m.group(2), "function", ",".join(params), text, origin)
    m = re.match(r"(?:const\s+|uniform\s+)?(?:[A-Za-z_]\w*\s+)+?([A-Za-z_]\w*)\s*(\[[^\]]*\]\s*)?(=|;|,)", code)
    if not m:
        raise ImportFailure(f"{origin}: cannot read a declaration in its global code: {code[:80]!r}")
    return Declaration(m.group(1), "const", "", text, origin)


def _matching(text: str, start: int) -> int:
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    raise ImportFailure(f"unbalanced parentheses in {text[start:start + 60]!r}")


def identifiers(text: str):
    """The identifiers of GLSL with macros, skipping the macros and the comments."""
    for token in IDENTIFIER.findall(strip_comments(text)):
        if not token.startswith("$"):
            yield token


def rename_tokens(text: str, renames: dict[str, str]) -> str:
    if not renames:
        return text

    def replace(match):
        token = match.group(0)
        return renames.get(token, token)

    parts = []
    last = 0
    for comment in COMMENT.finditer(text):
        parts.append(IDENTIFIER.sub(replace, text[last:comment.start()]))
        parts.append(comment.group(0))
        last = comment.end()
    parts.append(IDENTIFIER.sub(replace, text[last:]))
    return "".join(parts)


# Defaults naming a macro the model does not have: Material Maker expands them to an error text
# and only works because the graph always connects the input
INPUT_DEFAULT_FIXES = {("normal2height/normal2height_base", "h"): "0.5"}
# An enum value naming a function its node does not have, in Material Maker too: `blend` is the old
# node, used inside graphs, and blend2 has the function. The value is the last, so no index moves.
ENUM_VALUE_REMOVALS = {("blend", "blend_type"): {"linear_light"}}

# Formulas whose result is not defined, so each device draws its own (S90 R20, the per-node image test):
# each gets a rule that is the same on every tier. One (old, new[, count]) per change, and each old text must be
# there exactly once (or count times), so a new Material Maker version that rewrites the node fails here instead of
# silently keeping the old fix. The spec's Deviations list them.
# a dilation keeps the largest neighbour, and two neighbours equal up to rounding (any gradient has them)
# were picked by each device's rounding: the next one has to be larger by more than 8-bit output can show
_MORPHOLOGY_PICK = ("\t\tif (c > v) {", "\t\tif (c > v + 0.0009765625) {", 2)
_MORPHOLOGY_LOOP = [
    # a float counter stepped by 1/s against a float bound: whether the last step runs depends on rounding
    ("\tfloat {a} = floor($d/e)*e;", "\tint n = int(floor($d/e));"),
    ("\tfor (float {v} = 0.0; {v} < {a}; {v} += e) {{", "\tfor (int i = 0; i < n; i++) {{\n\t\tfloat {v} = float(i)*e;"),
]
TEXT_FIXES = {
    # sign() of width - height, zero up to rounding on square bricks (Running Bond 2): ties are 0.5
    "bricks": [("0.5*(sign($(name_uv)_rect.z-$(name_uv)_rect.x-$(name_uv)_rect.w+$(name_uv)_rect.y)+1.0)",
                "(abs($(name_uv)_rect.z-$(name_uv)_rect.x-$(name_uv)_rect.w+$(name_uv)_rect.y) < 1e-4 ? 0.5 : "
                "step(0.0, $(name_uv)_rect.z-$(name_uv)_rect.x-$(name_uv)_rect.w+$(name_uv)_rect.y))")],
    # atan(0, 0) on a flat input: no slope is angle 0. On the cut, a slope straight down -x is pi or -pi by the
    # sign of a zero dh.y, which each device's arithmetic leaves its own way (lavapipe's differs): it is pi
    "height_to_angle": [("\treturn atan(dh.y, dh.x)/3.141592;",
                         "\treturn dot(dh, dh) < 1e-12 ? 0.0 : (dh.y == 0.0 && dh.x < 0.0 ? 1.0 : atan(dh.y, dh.x)/3.141592);")],
    "morphology3/shape_2": [(o.format(a="dx", v="x"), n.format(a="dx", v="x")) for o, n in _MORPHOLOGY_LOOP] + [_MORPHOLOGY_PICK],
    "morphology3/shape_3": [(o.format(a="dx", v="x"), n.format(a="dx", v="x")) for o, n in _MORPHOLOGY_LOOP] +
                           [("if (v < input_dist.x && ", "if (v + 0.0009765625 < input_dist.x && ", 4)],
    "morphology3/shape_4": [(o.format(a="dy", v="y"), n.format(a="dy", v="y")) for o, n in _MORPHOLOGY_LOOP] + [_MORPHOLOGY_PICK],
    # an iteration count kept in a half-float channel as 0.01 steps: its rounding made a size that is no power
    # of two, so texel edges fell where each device rounds its own way. The count is whole, so it is rounded
    "auto_tones/14423": [("\tsize = size / pow(2.0, (iter * 100.0) );", "\tsize = size / exp2(floor(iter * 100.0 + 0.5));")],
}
# The same for the shared functions a node's `global` declares
GLOBAL_TEXT_FIXES = {
    # the low half of a packed value lands in an RGBA16F Buffer, and each device rounds a float to a half its
    # own way: it is quantized to 1/2048 first, which a half holds exactly
    "pack_1x32_to_2x16": [("vec2(mod(s.x, 1.0/pack_size), mod(s.y, 1.0/pack_size))*pack_size);",
                           "floor(vec2(mod(s.x, 1.0/pack_size), mod(s.y, 1.0/pack_size))*pack_size*2048.0+0.5)/2048.0);")],
}


def global_text_of(owner: str, model: dict) -> str:
    """A node's `global` declarations, with their fixes: they are read for the node and for its includers."""
    text = text_of(model.get("global"))
    for old, new in GLOBAL_TEXT_FIXES.get(owner, []):
        if text.count(old) != 1:
            raise ImportFailure(f"{owner}: a formula fix expects its text once, found it {text.count(old)} times: {old!r}")
        text = text.replace(old, new)
    return text


def apply_text_fixes(owner: str, instance: str, code: str, outputs: list[dict]) -> tuple[str, str]:
    for fix in TEXT_FIXES.get(owner, []):
        old, new, expected = fix if len(fix) == 3 else (*fix, 1)
        found = instance.count(old) + code.count(old) + sum(output["expr"].count(old) for output in outputs)
        if found != expected:
            raise ImportFailure(f"{owner}: a formula fix expects its text {expected} time(s), found it {found} times: {old!r}")
        instance, code = instance.replace(old, new), code.replace(old, new)
        for output in outputs:
            output["expr"] = output["expr"].replace(old, new)
    return instance, code



def typed_default(default: str, port_type: str) -> str:
    """A bare number as the default of a vector port compiles in no GLSL: those nodes only work
    in Material Maker while connected. The compile sweep found them, the type is known here."""
    glsl_type = PORT_TYPES.get(port_type, {}).get("type", "float")
    if glsl_type != "float" and re.fullmatch(r"-?\d+(\.\d*)?", default.strip()):
        return f"{glsl_type}({default.strip()})"
    return default


# Material Maker adds these parameters at load (gen_shader.gd, find/fix_instance_functions)
INSTANCE_FUNCTION = re.compile(r"(\w+)\s+(\w*\$(?:\(name\)\w*|name))\s*\((.*)\)\s*\{")
VARIATION_ARGS = ", _seed_variation_, _controlled_variation_"
VARIATION_PARAMS = ", float _seed_variation_, vec4 _controlled_variation_"


def add_variation_definitions(instance: str):
    functions = []

    def replace(match):
        if match.group(1) == "return":
            return match.group(0)
        functions.append(match.group(2))
        return f"{match.group(1)} {match.group(2)}({match.group(3)}{VARIATION_PARAMS}) {{"

    return INSTANCE_FUNCTION.sub(replace, instance), functions


def add_variation_calls(code: str, functions: list[str]) -> str:
    for name in functions:
        location = 0
        lowered = name.lower()
        while True:
            location = code.lower().find(lowered, location)
            if location == -1:
                break
            if location > 0 and (code[location - 1].isalnum() or code[location - 1] == "_"):
                location += len(name)
                continue
            p = location + len(name)
            while p < len(code) and code[p] in " \t\r\n":
                p += 1
            if p >= len(code) or code[p] != "(":
                location = p
                continue
            close = _matching(code, p)
            q = close + 1
            while q < len(code) and code[q] in " \t\r\n":
                q += 1
            if q >= len(code) or code[q] != "{":
                code = code[:close] + VARIATION_ARGS + code[close:]
                close += len(VARIATION_ARGS)
            location = close
    return code


def check_parameter_values(owner: str, parameters: dict, named: set[str]) -> None:
    """Graph parameters reach GLSL as text: refuse any that is not a plain expression (R3)."""
    allowed = MACRO_NAMES | named | {"rnd", "rndi"}
    for name, value in parameters.items():
        if not isinstance(value, str):
            continue
        if not PARAMETER_EXPRESSION.match(value):
            raise ImportFailure(f"{owner}: parameter {name} is not an expression: {value[:60]!r}")
        for macro in re.findall(r"\$(\w+)", value):
            if macro not in allowed:
                raise ImportFailure(f"{owner}: parameter {name} uses an unknown macro ${macro}")


class Body:
    """One shader model: a library node or a shader inside a graph, before its names are final."""

    def __init__(self, owner: str, model: dict):
        unknown = set(model) - MODEL_FIELDS - REFUSED_MODEL_FIELDS
        if unknown:
            raise ImportFailure(f"{owner}: unknown shader model field {sorted(unknown)[0]!r}")
        self.owner = owner
        self.includes = [i for i in model.get("includes") or [] if i]
        self.global_text = global_text_of(owner, model)
        instance = text_of(model.get("instance"))
        code = text_of(model.get("code"))
        self.outputs = []
        for output in model.get("outputs", []):
            types = [k for k in output if k in PORT_TYPES]
            if len(types) != 1:
                raise ImportFailure(f"{owner}: an output needs exactly one port type, found {types}")
            self.outputs.append({"type": types[0], "expr": text_of(output[types[0]]),
                                 "label": output.get("shortdesc", ""), "description": output.get("longdesc", ""),
                                 "group_size": output.get("group_size", 0)})
        instance, code = apply_text_fixes(owner, instance, code, self.outputs)
        self.parameters = [dict(p) for p in model.get("parameters", [])]
        for parameter in self.parameters:
            refused = ENUM_VALUE_REMOVALS.get((owner, parameter["name"]))
            if refused:
                parameter["values"] = [v for v in parameter["values"] if v["value"] not in refused]
        self.inputs = [dict(i) for i in model.get("inputs", [])]
        for port in self.inputs:
            if (owner, port["name"]) in INPUT_DEFAULT_FIXES:
                port["default"] = INPUT_DEFAULT_FIXES[owner, port["name"]]
            port["default"] = typed_default(str(port.get("default", "")), port["type"])
        self.instance, functions = add_variation_definitions(instance)
        if functions:
            self.instance = add_variation_calls(self.instance, functions)
            code = add_variation_calls(code, functions)
            for output in self.outputs:
                output["expr"] = add_variation_calls(output["expr"], functions)
            for parameter in self.parameters:
                for value in parameter.get("values", []):
                    value["value"] = add_variation_calls(str(value["value"]), functions)
        self.code = code
        self.name = model.get("name", "")
        self.short_description = model.get("shortdesc", "")
        self.description = model.get("longdesc", "")

    def texts(self):
        yield self.instance
        yield self.code
        for output in self.outputs:
            yield output["expr"]
        for port in self.inputs:
            yield str(port.get("default", ""))
        for parameter in self.parameters:
            for value in parameter.get("values", []):
                yield str(value["value"])

    def references(self):
        """Identifiers the body uses, with names built from an enum's value (`blend_$mode`)."""
        names = set()
        for text in self.texts():
            names.update(identifiers(text))
            names.update(_composed_names(text, self.parameters))
        return names

    def check_macros(self, extra_names: set[str]) -> None:
        names = set(MACRO_NAMES) | extra_names
        for parameter in self.parameters:
            names.add(parameter["name"])
            if parameter["type"] in ("pixels", "lattice"):
                names.add(parameter["name"] + "_size")
        for port in self.inputs:
            names.add(port["name"])
        names = {n.replace("#", "1") for n in names}
        for text in self.texts():
            if any(forbidden in text for forbidden in FORBIDDEN_TEXT):
                raise ImportFailure(f"{self.owner}: refused text in {text[:80]!r}")
            for macro in MACRO.findall(text.replace("#", "1")):
                name = macro[1:].strip("()")
                if name not in names:
                    raise ImportFailure(f"{self.owner}: unknown macro {macro!r}")
        if "$" in self.global_text:
            raise ImportFailure(f"{self.owner}: a macro in the global code cannot be resolved by id")


# A function name built from macros: `blend_$blend_type#`, `$direction_$colorspace`, `tile_$tile`
GLUED_NAME = re.compile(r"(?<![\w$)])(?:\w+(?=\$)|\$\(\w+#?\)(?=\w|\$)|\$\w+#?(?=\$))(?:\w+|\$\(\w+#?\)|\$\w+#?)*")


def _composed_names(text: str, parameters: list[dict]) -> set[str]:
    """Every name such a glued token can expand to, over its enum values and booleans."""
    choices = {}
    for parameter in parameters:
        name = parameter["name"].rstrip("#")
        if parameter["type"] == "enum":
            choices[name] = [str(v["value"]) for v in parameter.get("values", [])]
        elif parameter["type"] == "boolean":
            choices[name] = ["true", "false"]
    result = set()
    for match in GLUED_NAME.finditer(strip_comments(text)):
        names = [""]
        for part in re.findall(r"\$\(\w+#?\)|\$\w+#?|\w+", match.group(0)):
            if part.startswith("$"):
                # a greedy word macro may hold the rest: `$a_b` is the macro a_b, as the expander reads it
                options = choices.get(part[1:].strip("()").rstrip("#"))
                if options is None:
                    names = []
                    break
                names = [n + o for n in names for o in options]
            else:
                names = [n + part for n in names]
        result.update(n for n in names if re.fullmatch(r"[A-Za-z_]\w*", n))
    return result


PORT_TYPES: dict = {}


class Importer:
    def __init__(self, source: Path, commit: str):
        self.source = source
        self.commit = commit
        self.mmg = {}
        self.descriptors = {}
        self.excluded = {}
        self.bodies: list[Body] = []
        self.functions = {}
        self.body_functions = {}
        self.body_renames = {}
        self.needed_notices = {}

    # -- reading ------------------------------------------------------------------------------

    def read(self) -> None:
        global PORT_TYPES
        node_dir = self.source / NODE_DIR
        self.types = load_data(node_dir / "io_types.mmt")
        PORT_TYPES = {t["name"]: t for t in self.types}
        for path in sorted(node_dir.glob("*.mmg")):
            node_id = path.stem
            if node_id.startswith("website"):
                raise ImportFailure(f"{node_id}: a website node is never imported")
            self.mmg[node_id] = load_data(path)
        self.library = load_data(self.source / LIBRARY_FILE)["lib"]
        self.aliases = load_data(self.source / ALIASES_FILE)
        self.common = self._read_common()
        for node_id, data in self.mmg.items():
            if node_id not in DENY_LIST:
                check_attributions(node_id, data)
        for entry in self.library:
            if entry.get("type") == "graph" or "shader_model" in entry:
                check_attributions(entry["tree_item"], {k: v for k, v in entry.items() if k != "icon_data"})
        for node_id in OWN_NOTICES:
            if node_id not in ATTRIBUTIONS:
                raise ImportFailure(f"{node_id}: its notice has no attribution line")

    def _read_common(self) -> list[Declaration]:
        raw = (self.source / FUNCTIONS_FILE).read_text(encoding="utf-8")
        m = re.search(r'^text = "(.*)"\s*\Z', raw, re.DOTALL | re.MULTILINE)
        if not m:
            raise ImportFailure(f"{FUNCTIONS_FILE}: no text field")
        text = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
        check_attributions("shader_functions", text)
        decls = split_declarations(HOST_CONSTANTS, "host")
        for decl in split_declarations(text, "shader_functions"):
            if decl.name not in REPLACED_COMMON_FUNCTIONS:
                if "rand" in decl.name:
                    raise ImportFailure(f"shader_functions: {decl.name} is a random function with no R10 replacement")
                decls.append(decl)
        missing = REPLACED_COMMON_FUNCTIONS - {d.name for d in split_declarations(text, "shader_functions")}
        if missing:
            raise ImportFailure(f"shader_functions: expected {sorted(missing)}")
        return split_declarations(HASH_FUNCTIONS, "jasperengine") + decls

    # -- selecting ----------------------------------------------------------------------------

    def select(self) -> None:
        self.entries = []
        roots = {}
        for entry in self.library:
            tree = entry["tree_item"]
            kind = entry.get("type")
            if tree.split("/")[0] == "3D":
                self.excluded[f"entry:{tree}"] = "the 3D category is out of scope (R20)"
            elif kind in HOST_NODES:
                self.entries.append(self._entry(entry, kind, host=True))
            elif kind in LEFT_OUT_HOST_NODES:
                self.excluded[f"entry:{tree}"] = LEFT_OUT_HOST_NODES[kind]
            elif kind == "graph":
                node_id = "graph_" + re.sub(r"[^a-z0-9]+", "_", tree.lower()).strip("_")
                if node_id in self.mmg:
                    raise ImportFailure(f"{tree}: id {node_id} is taken")
                self.mmg[node_id] = {**{k: v for k, v in entry.items() if k != "icon_data"},
                                     "label": entry.get("display_name", entry.get("label", ""))}
                roots[node_id] = entry
            elif kind in self.mmg:
                roots[kind] = entry
            else:
                raise ImportFailure(f"{tree}: unknown node type {kind!r}")
        self.keep = {}
        for node_id in sorted(set(roots) | {e.get("type") for e in self.library if e.get("type") in self.mmg
                                            and e["tree_item"].split("/")[0] != "3D"}):
            self._closure(node_id)
        for entry in self.library:
            tree = entry["tree_item"]
            if tree.split("/")[0] == "3D" or entry.get("type") in HOST_NODES or entry.get("type") in LEFT_OUT_HOST_NODES:
                continue
            node_id = entry["type"] if entry["type"] != "graph" else "graph_" + re.sub(r"[^a-z0-9]+", "_", tree.lower()).strip("_")
            if self.keep.get(node_id) is True:
                self.entries.append(self._entry(entry, node_id))
            else:
                self.excluded[f"entry:{tree}"] = self.excluded.get(node_id, "left out")

    def _entry(self, entry: dict, descriptor: str, host: bool = False) -> dict:
        tree = entry["tree_item"]
        result = {"tree_item": tree, "display_name": entry.get("display_name", tree.split("/")[-1]), "type": descriptor}
        if host:
            result["host"] = True
        aliases = self.aliases.get(tree)
        if aliases:
            result["aliases"] = aliases.split(",")
        presets = {k: v for k, v in (entry.get("parameters") or {}).items() if k != "image"} if entry.get("type") != "graph" else {}
        if presets:
            # strings other than expressions (a host path for Simple/Image) are not data we keep
            check_parameter_values(tree, presets, set())
            result["parameters"] = presets
        if entry.get("generic_size"):
            result["generic_size"] = int(entry["generic_size"])
        return result

    def _closure(self, node_id: str) -> bool:
        """Whether a node and everything it needs can be imported; records why not."""
        if node_id in self.keep:
            return self.keep[node_id]
        self.keep[node_id] = True  # graphs cannot cycle; this only ends a repeated visit
        reason = None
        if node_id in DENY_LIST:
            reason = "deny-list: " + DENY_LIST[node_id]
        elif node_id not in self.mmg:
            reason = "unknown node"
        else:
            data = self.mmg[node_id]
            kind = data.get("type")
            if kind == "material_export":
                reason = "a Material Maker export target"
            elif "shader_model" in data and any(f in data["shader_model"] for f in ("custom", "exports", "preview_shader")):
                reason = "a host-specific material model"
            else:
                reason = self._closure_children(node_id, data)
        if reason:
            self.keep[node_id] = False
            self.excluded[node_id] = reason
            return False
        return True

    def _closure_children(self, node_id: str, data: dict):
        model = data.get("shader_model")
        if model:
            return self._include_problem(model)
        if "nodes" not in data:
            return f"unsupported node shape of type {data.get('type')!r}"
        return self._graph_children(data)

    def _include_problem(self, model: dict, seen=None):
        """Why a model's includes cannot come along, following them: an include brings its own."""
        seen = set() if seen is None else seen
        for include in filter(None, model.get("includes") or []):
            if include in seen:
                continue
            seen.add(include)
            if include in DENY_LIST:
                return f"includes {include}, deny-list: {DENY_LIST[include]}"
            if include not in self.mmg:
                return f"includes the unknown node {include}"
            problem = self._include_problem(self.mmg[include].get("shader_model") or {}, seen)
            if problem:
                return problem
        return None

    def _graph_children(self, graph: dict):
        for child in graph.get("nodes", []):
            kind = child.get("type")
            if kind in UI_ONLY_NODES or kind in HOST_NODES:
                continue
            if "shader_model" in child:
                model = child["shader_model"]
                if any(f in model for f in ("custom", "exports", "preview_shader")):
                    return f"contains a host-specific model in {child.get('name')}"
                problem = self._include_problem(model)
                if problem:
                    return problem
                continue
            if "nodes" in child:
                reason = self._graph_children(child)
                if reason:
                    return reason
                continue
            if kind in LEFT_OUT_HOST_NODES:
                return f"contains a {kind} node: {LEFT_OUT_HOST_NODES[kind]}"
            if not self._closure(kind):
                return f"depends on {kind}: {self.excluded[kind]}"
        return None

    # -- declarations -------------------------------------------------------------------------

    def _scope(self, body: Body) -> dict:
        """What a body can see, name -> {signature: declaration}: common code, includes, its global."""
        scope: dict = {}
        for decl in self.common:
            scope.setdefault(decl.name, {})[decl.signature] = decl
        visited = set()

        def add_include(node_id):
            if node_id in visited:
                return
            visited.add(node_id)
            model = self.mmg[node_id].get("shader_model") or {}
            for nested in model.get("includes") or []:
                if nested:
                    add_include(nested)
            for decl in self._declarations(global_text_of(node_id, model), node_id):
                scope.setdefault(decl.name, {})[decl.signature] = decl

        for include in body.includes:
            add_include(include)
        for decl in self._declarations(body.global_text, body.owner):
            scope.setdefault(decl.name, {})[decl.signature] = decl
        return scope

    @staticmethod
    def _declarations(text: str, origin: str) -> list[Declaration]:
        declarations = []
        for decl in split_declarations(text, origin):
            if decl.name in PERMUTE_REPLACEMENTS:
                decl = Declaration(decl.name, decl.kind, decl.signature, PERMUTE_REPLACEMENTS[decl.name], origin)
            if decl.name not in FLOAT_MOD289:
                declarations.append(decl)
        return declarations

    def resolve_functions(self) -> None:
        """Gives each distinct declaration one id, so a body names what it uses, not its text."""
        scopes = [(body, self._scope(body)) for body in self.bodies]
        # first pass: a content hash per name in each scope, following the dependencies
        hashes = []
        for body, scope in scopes:
            memo: dict = {}
            needed = set(body.references())
            name_hash = {}

            def visit(name, stack=()):
                if name in memo:
                    return memo[name]
                if name in stack:
                    return ""
                parts = []
                for signature in sorted(scope[name]):
                    decl = scope[name][signature]
                    deps = sorted(t for t in set(identifiers(decl.normal)) if t in scope and t != name)
                    parts.append(signature + "\0" + decl.normal + "\0" +
                                 "\0".join(dep + ":" + visit(dep, stack + (name,)) for dep in deps))
                memo[name] = hashlib.sha256("\1".join(parts).encode()).hexdigest()
                return memo[name]

            frontier = [n for n in needed if n in scope]
            seen = set()
            while frontier:
                name = frontier.pop()
                if name in seen:
                    continue
                seen.add(name)
                name_hash[name] = visit(name)
                for decl in scope[name].values():
                    frontier.extend(t for t in identifiers(decl.normal) if t in scope and t not in seen)
            hashes.append(name_hash)
        # second pass: one final name per distinct content
        variants: dict = {}
        for (body, scope), name_hash in zip(scopes, hashes):
            for name, digest in name_hash.items():
                variants.setdefault(name, {}).setdefault(digest, []).append(body.owner)
        final_names = {}
        for name, by_hash in variants.items():
            if len(by_hash) == 1:
                final_names[name, next(iter(by_hash))] = name
            else:
                # the first owner in sorted order names the variant: stable while the input is
                taken = set()
                for digest, owners in sorted(by_hash.items(), key=lambda item: sorted(item[1])[0]):
                    owner = re.sub(r"\W+", "_", sorted(owners)[0])
                    candidate = f"{name}_of_{owner}"  # two underscores are reserved in GLSL
                    if candidate in taken:
                        raise ImportFailure(f"{name}: two variants under {owner}")
                    taken.add(candidate)
                    final_names[name, digest] = candidate
        # what the expander's pass wrapper itself calls
        for decl in self.common:
            if decl.name == "jp_finite":
                self.functions[decl.name] = {"name": decl.name, "kind": decl.kind, "code": lines_of(decl.text.strip("\n")), "deps": [], "origin": decl.origin}
        # third pass: emit
        for (body, scope), name_hash in zip(scopes, hashes):
            renames = {n: final_names[n, h] for n, h in name_hash.items() if final_names[n, h] != n}
            ids = []
            for text in body.texts():
                for composed in _composed_names(text, body.parameters):
                    if composed in renames:
                        raise ImportFailure(f"{body.owner}: {composed} needs a new name but is built from a macro")
            for name in body.references():
                if name in name_hash:
                    ids.extend(self._emit(name, scope, renames, final_names, name_hash))
            self.body_functions[id(body)] = sorted(set(ids))
            self.body_renames[id(body)] = renames

    def _emit(self, name: str, scope: dict, renames: dict, final_names: dict, name_hash: dict) -> list[str]:
        final = final_names[name, name_hash[name]]
        overloaded = len(scope[name]) > 1
        ids = []
        for signature, decl in sorted(scope[name].items()):
            function_id = f"{final}({signature})" if overloaded else final
            ids.append(function_id)
            if function_id in self.functions:
                self._merge_copy(self.functions[function_id], decl, renames)
                continue
            deps = set()
            for token in sorted(set(identifiers(decl.normal))):
                if token in scope and token != name:
                    deps.update(self._emit(token, scope, renames, final_names, name_hash))
                elif token == name and overloaded:
                    # an overload may call another: the expander keeps their source order
                    deps.update(f"{final}({s})" for s in scope[name] if s < signature)
            self.functions[function_id] = {
                "name": final, "kind": decl.kind, "code": lines_of(rename_tokens(decl.text.strip("\n"), renames)),
                "deps": sorted(deps), "origin": decl.origin,
            }
            if self._notice_of(decl.origin):
                self.functions[function_id]["notices"] = [self._notice_of(decl.origin)]
        return ids

    def _merge_copy(self, function: dict, decl: Declaration, renames: dict) -> None:
        """The same code from another node: which copy sorts first must not lose a notice or its comment."""
        notice = self._notice_of(decl.origin)
        if not notice:
            return
        if notice not in function.setdefault("notices", []):
            function["notices"].append(notice)
        code = lines_of(rename_tokens(decl.text.strip("\n"), renames))
        if len("\n".join(code)) > len("\n".join(function["code"])):
            function["code"] = code

    def _notice_of(self, node_id: str):
        notice = OWN_NOTICES.get(node_id)
        if not notice:
            return None
        return {"license": notice[0], "holder": notice[1], "year": notice[2], "url": notice[3], "node": node_id}

    # -- descriptors --------------------------------------------------------------------------

    def collect_bodies(self) -> None:
        self.node_bodies = {}
        for node_id, keep in sorted(self.keep.items()):
            if not keep:
                continue
            data = self.mmg[node_id]
            model = data.get("shader_model")
            if model:
                body = Body(node_id, model)
                self.bodies.append(body)
                self.node_bodies[node_id] = body
            else:
                self.node_bodies[node_id] = self._graph_bodies(node_id, data)

    def _graph_bodies(self, owner: str, graph: dict) -> dict:
        inner = {}
        for child in graph.get("nodes", []):
            child_name = child.get("name")
            if "shader_model" in child:
                body = Body(f"{owner}/{child_name}", child["shader_model"])
                self.bodies.append(body)
                inner[child_name] = body
            elif "nodes" in child:
                inner[child_name] = self._graph_bodies(f"{owner}/{child_name}", child)
        return inner

    def check_bodies(self) -> None:
        for body in self.bodies:
            named = set()
            if "/" in body.owner:
                named = self._named_parameters(body.owner)
            body.check_macros(named)
            for text in list(body.texts()):
                if FLOAT_HASH.search(strip_comments(text)):
                    raise ImportFailure(f"{body.owner}: float hash, R10 needs an integer hash")

    def _named_parameters(self, owner: str) -> set[str]:
        graph = self.mmg[owner.split("/")[0]]
        names = set()
        for child in graph.get("nodes", []):
            if child.get("type") == "remote":
                for widget in child.get("widgets", []):
                    if widget.get("type") == "named_parameter":
                        names.add(widget["name"])
        return names

    def check_functions(self) -> None:
        for function_id, function in self.functions.items():
            if FLOAT_HASH.search(strip_comments("\n".join(function["code"]))):
                raise ImportFailure(f"{function['origin']}: float hash in {function_id}, R10 needs an integer hash")

    def _shader_body(self, body: Body) -> dict:
        renames = self.body_renames[id(body)]
        inputs = []
        for port in body.inputs:
            item = {"name": port["name"], "type": port["type"], "label": port.get("label", ""),
                    "default": rename_tokens(str(port.get("default", "")), renames)}
            if port.get("function"):
                item["function"] = True
            for field, target in (("shortdesc", "short_description"), ("longdesc", "description"), ("group_size", "group_size")):
                if port.get(field):
                    item[target] = port[field]
            inputs.append(item)
        parameters = []
        for parameter in body.parameters:
            item = {k: parameter[k] for k in ("name", "type", "default", "min", "max", "step", "first", "last") if k in parameter}
            item["label"] = parameter.get("label", "")
            if "values" in parameter:
                item["values"] = [{"name": v["name"], "value": rename_tokens(str(v["value"]), renames)} for v in parameter["values"]]
            for field, target in (("shortdesc", "short_description"), ("longdesc", "description")):
                if parameter.get(field):
                    item[target] = parameter[field]
            parameters.append(item)
        outputs = []
        for output in body.outputs:
            item = {"type": output["type"], "expr": rename_tokens(output["expr"], renames)}
            for field, target in (("label", "label"), ("description", "description"), ("group_size", "group_size")):
                if output[field]:
                    item[target] = output[field]
            outputs.append(item)
        return {"functions": self.body_functions[id(body)], "instance": lines_of(rename_tokens(body.instance, renames)),
                "code": lines_of(rename_tokens(body.code, renames)), "inputs": inputs, "outputs": outputs,
                "parameters": parameters}

    def _graph_body(self, owner: str, graph: dict, bodies: dict) -> dict:
        nodes = []
        ports = {"gen_inputs": [], "gen_outputs": []}
        parameters = []
        for child in graph.get("nodes", []):
            kind = child.get("type")
            name = child.get("name")
            if kind == "comment":
                continue
            if kind == "ios":
                if name in ports:
                    ports[name] = [{"name": p.get("name", ""), "type": p.get("type", "rgba"), "label": p.get("shortdesc", p.get("name", "")),
                                    "description": p.get("longdesc", ""), "group_size": p.get("group_size", 0)}
                                   for p in child.get("ports", [])]
                    continue
            if kind == "remote":
                if name == "gen_parameters":
                    parameters = self._widgets(owner, graph, child)
                    continue
                raise ImportFailure(f"{owner}/{name}: a remote node other than gen_parameters")
            check_parameter_values(f"{owner}/{name}", child.get("parameters", {}), self._graph_named(graph))
            item = {"id": name, "parameters": child.get("parameters", {})}
            if "shader_model" in child:
                item["shader"] = self._shader_body(bodies[name])
            elif "nodes" in child:
                item["graph"] = self._graph_body(f"{owner}/{name}", child, bodies[name])
            else:
                item["type"] = kind
            if kind == "ios":
                item["ports"] = child.get("ports", [])
            if "seed_int" in child:
                item["seed"] = child["seed_int"] / 4294967296.0
            elif "seed" in child:
                item["seed"] = child["seed"]
            elif "seed_value" in child:
                item["seed"] = child["seed_value"]
            if child.get("seed_locked"):
                item["seed_locked"] = True
            for field in ("version", "generic_size"):
                if field in child:
                    item[field] = child[field]
            nodes.append(item)
        return {"nodes": nodes, "connections": graph.get("connections", []), "inputs": ports["gen_inputs"],
                "outputs": ports["gen_outputs"], "parameters": parameters}

    @staticmethod
    def _graph_named(graph: dict) -> set[str]:
        return {widget["name"] for child in graph.get("nodes", []) if child.get("type") == "remote"
                for widget in child.get("widgets", []) if widget.get("type") == "named_parameter"}

    def _widgets(self, owner: str, graph: dict, remote: dict) -> list[dict]:
        values = remote.get("parameters", {})
        result = []
        for widget in remote.get("widgets", []):
            name = widget["name"]
            item = {"name": name, "label": widget.get("label", name)}
            for field, target in (("shortdesc", "short_description"), ("longdesc", "description")):
                if widget.get(field):
                    item[target] = widget[field]
            kind = widget.get("type")
            if kind == "linked_control":
                links = [{"node": w["node"], "param": w["widget"]} for w in widget.get("linked_widgets", [])]
                item["links"] = links
                definition = self._linked_definition(graph, links)
                item.update({k: v for k, v in definition.items() if k not in ("name", "label", "short_description", "description")})
            elif kind == "config_control":
                configurations = sorted(widget.get("configurations", {}))
                item["type"] = "boolean" if configurations == ["False", "True"] else "enum"
                if item["type"] == "enum":
                    item["values"] = [{"name": c, "value": c} for c in configurations]
                item["configurations"] = {c: [{"node": w["node"], "param": w["widget"], "value": w.get("value")}
                                              for w in widget["configurations"][c]] for c in configurations}
                for links in item["configurations"].values():
                    check_parameter_values(owner, {link["param"]: link["value"] for link in links}, self._graph_named(graph))
            elif kind == "named_parameter":
                item.update({"type": "float", "named": True, "min": widget["min"], "max": widget["max"],
                             "step": widget["step"], "default": widget["default"]})
            else:
                raise ImportFailure(f"{owner}: unknown widget type {kind!r}")
            if name in values:
                item["default"] = values[name]
            result.append(item)
        return result

    def _linked_definition(self, graph: dict, links: list[dict]) -> dict:
        if not links:
            return {}
        target = next((c for c in graph["nodes"] if c.get("name") == links[0]["node"]), None)
        if target is None:
            return {}
        kind = target.get("type")
        if kind == "buffer":
            # a Buffer's size, or one of its flags ("32 Bits"): a flag is no size
            return {"type": "size", "first": 4, "last": 13} if links[0]["param"] == "size" else {"type": "boolean"}
        if kind == "iterate_buffer":
            if links[0]["param"] == "size":
                return {"type": "size", "first": 4, "last": 13}
            if links[0]["param"] == "iterations":
                return {"type": "float", "min": 1, "max": 50, "step": 1}
            return {"type": "boolean"}
        if kind == "remote":
            return {}
        if kind == "switch":
            return {"type": "float", "min": 0, "max": 4, "step": 1}
        model = target.get("shader_model") or self.mmg.get(kind, {}).get("shader_model")
        if model is None:
            nested = target if "nodes" in target else self.mmg.get(kind, {})
            for child in nested.get("nodes", []):
                if child.get("type") == "remote":
                    for widget in child.get("widgets", []):
                        if widget["name"] == links[0]["param"]:
                            return self._linked_definition(nested, [{"node": w["node"], "param": w["widget"]} for w in widget.get("linked_widgets", [])])
            return {}
        for parameter in model.get("parameters", []):
            if parameter["name"] == links[0]["param"]:
                return {k: parameter[k] for k in ("type", "min", "max", "step", "first", "last", "values", "default") if k in parameter}
        return {}

    def _inherited_notices(self, node_id: str, seen: set[str]) -> list[dict]:
        """A node's own notice plus those of what it inlines: a shipped shader carries that code."""
        if node_id in seen:
            return []
        seen.add(node_id)
        notices = []
        if self._notice_of(node_id):
            notices.append(self._notice_of(node_id))
        data = self.mmg.get(node_id)
        if not data:
            return notices

        def walk(graph):
            for child in graph.get("nodes", []):
                if "nodes" in child:
                    walk(child)
                elif child.get("type") in self.mmg:
                    notices.extend(self._inherited_notices(child["type"], seen))

        walk(data)
        return notices

    def _function_notices(self, ids, seen=None) -> list[dict]:
        seen = set() if seen is None else seen
        notices = []
        for function_id in ids:
            if function_id in seen:
                continue
            seen.add(function_id)
            function = self.functions[function_id]
            notices.extend(function.get("notices", []))
            notices.extend(self._function_notices(function["deps"], seen))
        return notices

    def _all_function_ids(self, body_dict: dict):
        yield from body_dict.get("functions", [])
        for child in body_dict.get("nodes", []):
            if "shader" in child:
                yield from self._all_function_ids(child["shader"])
            if "graph" in child:
                yield from self._all_function_ids(child["graph"])

    def build_descriptors(self) -> None:
        for node_id, keep in sorted(self.keep.items()):
            if not keep:
                continue
            data = self.mmg[node_id]
            model = data.get("shader_model")
            if model:
                body = self._shader_body(self.node_bodies[node_id])
                kind = "shader"
                name = model.get("name") or data.get("label") or node_id
                short, long_ = model.get("shortdesc", ""), model.get("longdesc", "")
            else:
                body = self._graph_body(node_id, data, self.node_bodies[node_id])
                kind = "graph"
                name = data.get("label") or data.get("shortdesc") or node_id
                short, long_ = data.get("shortdesc", ""), data.get("longdesc", "")
            notices = {json.dumps(n, sort_keys=True): n for n in
                       self._inherited_notices(node_id, set()) + self._function_notices(self._all_function_ids(body))}
            descriptor = {"format": FORMAT_VERSION, "id": node_id, "type_version": 1, "kind": kind, "name": name,
                          "short_description": short, "description": long_,
                          "source": {"file": f"{NODE_DIR}/{node_id}.mmg" if not node_id.startswith("graph_") else LIBRARY_FILE, "commit": self.commit},
                          "notices": [notices[k] for k in sorted(notices)], **body}
            self.descriptors[node_id] = descriptor

    def write(self, out: Path) -> None:
        out.mkdir(parents=True, exist_ok=True)
        for path in out.glob("*.json"):
            path.unlink()

        def dump(name, value):
            (out / name).write_text(json.dumps(value, indent=1, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")

        for node_id, descriptor in self.descriptors.items():
            dump(f"{node_id}.json", descriptor)
        dump("_functions.json", {"format": FORMAT_VERSION, "functions": self.functions})
        types = [{k: t[k] for k in ("name", "label", "type", "paramdefs", "params", "convert", "color", "slot_type") if k in t}
                 for t in self.types]
        dump("_types.json", {"format": FORMAT_VERSION, "types": types})
        dump("_library.json", {"format": FORMAT_VERSION, "entries": sorted(self.entries, key=lambda e: e["tree_item"])})
        dump("_manifest.json", {"format": FORMAT_VERSION, "source_commit": self.commit, "descriptors": len(self.descriptors),
                                "functions": len(self.functions), "entries": len(self.entries),
                                "excluded": dict(sorted(self.excluded.items())),
                                "not_reachable": sorted(n for n in self.mmg if n not in self.keep)})

    def run(self, out: Path) -> None:
        self.read()
        self.select()
        self.collect_bodies()
        self.check_bodies()
        self.resolve_functions()
        self.check_functions()
        self.build_descriptors()
        forbidden = [n for n in self.descriptors if n in DENY_LIST]
        if forbidden:
            raise ImportFailure(f"deny-listed node in the output: {forbidden[0]}")
        self.write(out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, type=Path, help="scratch clone of Material Maker")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "editor/procmat/nodes")
    parser.add_argument("--commit", default=MM_COMMIT, help="the clone's commit, recorded in every descriptor")
    parser.add_argument("--check", action="store_true", help="fail if the output would differ from --out")
    args = parser.parse_args(argv)
    importer = Importer(args.source, args.commit)
    try:
        if args.check:
            with tempfile.TemporaryDirectory() as scratch:
                importer.run(Path(scratch))
                wanted = {p.name: p.read_bytes() for p in Path(scratch).glob("*.json")}
                actual = {p.name: p.read_bytes() for p in args.out.glob("*.json")}
                if wanted != actual:
                    differing = sorted(n for n in wanted.keys() | actual.keys() if wanted.get(n) != actual.get(n))
                    raise ImportFailure(f"{len(differing)} file(s) differ, first {differing[0]}")
        else:
            importer.run(args.out)
    except ImportFailure as failure:
        print(f"import_mm_nodes: {failure}", file=sys.stderr)
        return 1
    print(f"import_mm_nodes: {len(importer.descriptors)} descriptors, {len(importer.functions)} functions, "
          f"{len(importer.entries)} library entries, {len(importer.excluded)} left out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
