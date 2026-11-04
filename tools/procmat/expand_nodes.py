#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Expands the imported node descriptors into fragment shaders, for the compile sweep of S90 M0.

    python3 tools/procmat/expand_nodes.py --nodes editor/procmat/nodes --out DIR
                                          [--chains] [--float-hash]

Writes DIR/<shader>.frag and DIR/manifest.json, which `procmat-sweep` compiles and interprets. One
shader per output and per pass (R5): a node with three outputs is three shaders, and a graph with a
Buffer is one shader per side of it. Variants exercise what the C++ expander must get right:

  default     default parameters, inputs unconnected
  wired       every input connected to a small source of its port type, through its function if
              the input has one
  <p>=<v>     one enum value or boolean flipped, wired: these inline as text, so each is its own
              compile
  generic2    a generic node with two groups

  --chains adds passes of 10 to 240 nodes, for R6's limits and R25's budgets. --float-hash puts Material
  Maker's float rand functions back, the red control of R10: same corpus, compare with `crosstier`.
  It exits 1 when a node cannot be expanded.

This is Material Maker's macro semantics (gen_shader.gd) in Python, kept deliberately close to
it so the later C++ expander (R5) has a reference to compare against. It is a development tool.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

WORD_LETTERS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_?")
# Material Maker's rand functions, kept only as the control R10 is measured against
FLOAT_RAND = {
    "rand": "float rand(vec2 x) {\n return fract(cos(mod(dot(x, vec2(13.9898, 8.141)), 3.14)) * 43758.5);\n}",
    "rand2": "vec2 rand2(vec2 x) {\n return fract(cos(mod(vec2(dot(x, vec2(13.9898, 8.141)),dot(x, vec2(3.4562, 17.398))), vec2(3.14, 3.14))) * 43758.5);\n}",
    "rand3": "vec3 rand3(vec2 x) {\n return fract(cos(mod(vec3(dot(x, vec2(13.9898, 8.141)),dot(x, vec2(3.4562, 17.398)),dot(x, vec2(13.254, 5.867))), vec3(3.14, 3.14, 3.14))) * 43758.5);\n}",
    "rand3_4d": "vec3 rand3_4d(vec4 x) {\n return fract(cos(mod(vec3(dot(x, vec4(13.9898, 8.141, 3.4562, 17.398)),dot(x + 0.1234, vec4(3.4562, 17.398, 13.254, 5.867)),dot(x + 0.5678, vec4(11.1357, 23.234, 95.346, 9.123))), vec3(3.14))) * 43758.5);\n}",
}


class ExpansionError(Exception):
    pass


def matching(text: str, start: int, opener: str = "(", closer: str = ")") -> int:
    depth = 0
    for i in range(start, len(text)):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
    raise ExpansionError(f"unbalanced text: {text[start:start + 60]!r}")


def split_arguments(text: str) -> list[str]:
    args, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(text[start:i].strip())
            start = i + 1
    args.append(text[start:].strip())
    return args


def parenthesized(uv: str) -> str:
    """Material Maker substitutes coordinates as text, so `$uv.x` with `a+b` binds to b alone. It
    goes unnoticed when the types still agree, and fails to compile when they do not."""
    if re.fullmatch(r"[\w.\[\]]+", uv):
        return uv
    if uv[0] == "(" and matching(uv, 0) == len(uv) - 1:
        return uv
    return f"({uv})"


class Context:
    def __init__(self, parent=None):
        self.variants = {}
        self.parent = parent

    def has_variant(self, generator) -> bool:
        return generator in self.variants or (self.parent is not None and self.parent.has_variant(generator))

    def touch(self, generator) -> None:
        self.variants.setdefault(generator, [])
        if self.parent is not None:
            self.parent.touch(generator)

    def get_variant(self, generator, variant) -> int:
        result = -1
        if generator in self.variants:
            if variant in self.variants[generator]:
                result = self.variants[generator].index(variant)
            else:
                self.variants[generator].append(variant)
        else:
            self.variants[generator] = [variant]
        self.touch(generator)
        return result


class Code:
    def __init__(self):
        self.functions = set()
        self.defs = ""
        self.code = ""
        self.output_values = {}
        self.output_type = ""
        self.alias = None

    def merge(self, other) -> None:
        self.functions |= other.functions
        self.defs += other.defs


class Pass:
    """The state of one generated shader: its uniform block, textures and function set."""

    def __init__(self):
        self.values = []  # the block's floats, four to a vec4
        self.slots = {}
        self.textures = []  # [{name, source}]

    def float_slot(self, key, value: float) -> str:
        if key not in self.slots:
            index = len(self.values)
            self.values.append(float(value))
            self.slots[key] = f"pm.v[{index // 4}].{'xyzw'[index % 4]}"
        return self.slots[key]

    def vec4_slot(self, key, value) -> str:
        if key not in self.slots:
            while len(self.values) % 4:
                self.values.append(0.0)
            index = len(self.values)
            self.values.extend(float(v) for v in value)
            self.slots[key] = f"pm.v[{index // 4}]"
        return self.slots[key]

    def texture(self, name: str, generator, port: int) -> str:
        for item in self.textures:
            if item["name"] == name:
                break
        else:
            self.textures.append({"name": name, "generator": generator, "port": port})
        return f"sampler2D(t_{name}, s_{name})"


class Generator:
    def __init__(self, expander, name: str):
        self.expander = expander
        self.index = expander.next_index()
        self.genname = f"o{self.index}"
        self.name = name
        self.sources = {}  # input index -> (generator, output port)

    def get_source(self, index: int):
        return self.sources.get(index)

    def get_output_attributes(self, _port):
        return None

    def output_type(self, port: int) -> str:
        raise NotImplementedError

    def shader_code(self, uv: str, port: int, context: Context) -> Code:
        result = self._shader_code(uv, port, context)
        convert = self.expander.types.get(result.output_type, {}).get("convert", [])
        for conversion in convert:
            if conversion["type"] not in result.output_values:
                result.output_values[conversion["type"]] = conversion["expr"].replace("$(value)", result.output_values[result.output_type])
        return result

    def _shader_code(self, uv, port, context):
        raise NotImplementedError


def default_code() -> Code:
    result = Code()
    result.output_type = "rgba"
    result.output_values = {"f": "0.0", "rgb": "vec3(0.0)", "rgba": "vec4(0.0, 0.0, 0.0, 0.0)"}
    return result


class TextureSource(Generator):
    """A Buffer as its consumers see it: a texture sample, where inlining stops."""

    def __init__(self, expander, name, parameters, pass_kind="buffer"):
        super().__init__(expander, name)
        self.parameters = parameters
        self.pass_kind = pass_kind
        self.filter = parameters.get("filter", True)
        self.size = 2.0 ** float(parameters.get("size", 10))
        self.loop_texture = False

    def is_greyscale(self, port) -> bool:
        source = self.get_source(0)
        return source is not None and source[0].output_type(source[1]) == "f"

    def output_type(self, port):
        return "f" if self.is_greyscale(port) else "rgba"

    def texture_name(self, port) -> str:
        return f"{self.genname}_{'loop' if self.pass_kind == 'iterate' and port == 1 else 'tex'}"

    def get_output_attributes(self, port):
        current = self.expander.current_pass
        return {"texture": current.texture(self.texture_name(port), self, port),
                "texture_size": current.float_slot(f"{self.genname}_size", self.size)}

    def _shader_code(self, uv, port, context):
        current = self.expander.current_pass
        result = Code()
        grey = self.is_greyscale(port)
        result.output_type = "f" if grey else "rgba"
        sampler = current.texture(self.texture_name(port), self, port)
        variant = context.get_variant(self, uv)
        kind = "float" if grey else "vec4"
        select = ".r" if grey else ""
        if variant == -1:
            variant = context.get_variant(self, uv)
            size = current.float_slot(f"{self.genname}_size", self.size)
            adjusted = f"fract({uv})" if self.filter else f"((floor(fract({uv}) * {size})+vec2(0.5))/{size})"
            result.code = f"{kind} {self.genname}_{variant} = textureLod({sampler}, {adjusted}, 0.0){select};\n"
        result.output_values[result.output_type] = f"{self.genname}_{variant}"
        return result


class Switch(Generator):
    def __init__(self, expander, name, parameters):
        super().__init__(expander, name)
        self.parameters = parameters

    def _forward(self, port):
        outputs = int(self.parameters.get("outputs", 2))
        return self.get_source(port + int(self.parameters.get("source", 0)) * outputs)

    def output_type(self, port):
        source = self._forward(port)
        return source[0].output_type(source[1]) if source else "rgba"

    def _shader_code(self, uv, port, context):
        source = self._forward(port)
        return source[0]._shader_code(uv, source[1], context) if source else default_code()


class Probe(Generator):
    """A small source of a given port type: what a wired variant connects to an input."""

    EXPRESSIONS = {
        "f": "fract($uv.x*7.0)", "rgb": "vec3($uv, 0.5)", "rgba": "vec4($uv, 0.5, 1.0)",
        "sdf2d": "(length($uv-vec2(0.5))-0.25)", "fill": "vec4(0.25, 0.25, 0.5, 0.5)",
        "sdf3d": "(length($uv)-0.5)", "sdf3dc": "vec2(length($uv)-0.5, 0.0)",
        "tex3d": "vec3(fract(($uv).xyz))", "tex3d_gs": "fract(($uv).x)",
    }

    def __init__(self, expander, kind):
        super().__init__(expander, f"probe_{kind}")
        self.kind = kind

    def output_type(self, port):
        return self.kind

    def _shader_code(self, uv, port, context):
        result = Code()
        result.output_type = self.kind
        result.output_values[self.kind] = self.EXPRESSIONS[self.kind].replace("$uv", uv)
        return result


class Shader(Generator):
    def __init__(self, expander, name, body, parameters, seed=0.0, generic_size=1):
        super().__init__(expander, name)
        self.generic_size = int(generic_size)
        self.seed = float(seed)
        self.named_parameters = {}
        self.functions = list(body.get("functions", []))
        self.instance = "\n".join(body.get("instance", []))
        self.code = "\n".join(body.get("code", []))
        self.inputs = [dict(i) for i in body.get("inputs", [])]
        self.outputs = [dict(o) for o in body.get("outputs", [])]
        self.parameter_defs = [dict(p) for p in body.get("parameters", [])]
        if self._is_generic():
            self._expand_generic()
        self.parameters = {}
        for definition in self.parameter_defs:
            value = parameters.get(definition["name"], definition.get("default"))
            self.parameters[definition["name"]] = value

    def _is_generic(self):
        return any("#" in p["name"] for p in self.parameter_defs) or any("#" in i["name"] for i in self.inputs)

    def _expand_generic_code(self, code):
        result = ""
        code = code.replace("#count", str(self.generic_size))
        while code:
            position = code.find("#for")
            if position == -1:
                result += code
                break
            result += code[:position]
            code = code[position + 4:]
            end = code.find("#end")
            if end == -1:
                generic, code = code, ""
            else:
                generic, code = code[:end], code[end + 4:]
            for i in range(self.generic_size):
                result += generic.replace("#", str(i + 1))
        return result

    def _expand_list(self, items, field):
        first = last = -1
        for i, item in enumerate(items):
            has_mark = "#" in item[field]
            if has_mark and first == -1:
                first = i
            elif not has_mark and first != -1 and last == -1:
                last = i
        if first == -1:
            return items
        if last == -1:
            last = len(items)
        result = items[:first]
        for g in range(self.generic_size):
            for item in items[first:last]:
                copy = dict(item)
                copy[field] = copy[field].replace("#", str(g + 1))
                result.append(copy)
        return result + items[last:]

    def _expand_generic(self):
        self.inputs = self._expand_list(self.inputs, "name")
        self.parameter_defs = self._expand_list(self.parameter_defs, "name")
        self.outputs = self._expand_list(self.outputs, "expr")
        self.code = self._expand_generic_code(self.code)
        self.instance = self._expand_generic_code(self.instance)

    def input_index(self, name):
        for i, item in enumerate(self.inputs):
            if item["name"] == name:
                return i
        return -1

    def output_type(self, port):
        if port >= len(self.outputs):
            raise ExpansionError(f"{self.name}: no output {port}")
        return self.outputs[port]["type"]

    # -- parameters --

    def _process_parameters(self, rv: Code, variables: dict, declare: bool):
        current = self.expander.current_pass
        for definition in self.parameter_defs:
            name = definition["name"]
            kind = definition["type"]
            value = self.parameters.get(name)
            prefix = f"{self.genname}_{name}"
            if kind == "float":
                if isinstance(value, str):
                    variables[name] = "(" + self._random_macros(value) + ")"
                else:
                    variables[name] = current.float_slot(prefix, value if value is not None else 0.0)
            elif kind == "color":
                value = value or {"r": 1, "g": 1, "b": 1, "a": 1}
                variables[name] = current.vec4_slot(prefix, [value["r"], value["g"], value["b"], value.get("a", 1.0)])
            elif kind == "enum":
                values = definition.get("values", [])
                index = int(value) if isinstance(value, (int, float)) and 0 <= value < len(values) else 0
                variables[name] = values[index]["value"] if values else ""
            elif kind == "boolean":
                variables[name] = "true" if value else "false"
            elif kind == "size":
                variables[name] = f"{2.0 ** float(value):.1f}"
            elif kind == "gradient":
                variables[name] = self._gradient(prefix, value, rv, declare)
            elif kind == "curve":
                variables[name] = self._curve(prefix, value, rv, declare)
            elif kind in ("polygon", "polyline"):
                points = value["points"]
                slots = [current.vec4_slot((prefix, i), [p["x"], p["y"], 0.0, 0.0]) + ".xy" for i, p in enumerate(points)]
                variables[name] = f"vec2[{len(points)}]({', '.join(slots)})"
            elif kind == "lattice":
                points = value["points"]
                slots = [current.vec4_slot((prefix, i), [p["x"], p["y"], 0.0, 0.0]) + ".xy" for i, p in enumerate(points)]
                variables[name] = f"vec2[{len(points)}]({', '.join(slots)})"
                variables[name + "_size"] = f"ivec2({int(value['size']['x'])}, {int(value['size']['y'])})"
            elif kind == "pixels":
                variables[name], variables[name + "_size"] = self._pixels(prefix, value, rv, declare)
            else:
                raise ExpansionError(f"{self.name}: unsupported parameter type {kind}")

    def _random_macros(self, text: str) -> str:
        for keyword, function in (("rnd", "param_rnd"), ("rndi", "param_rndi")):
            needle = f"${keyword}("
            while needle in text:
                position = text.index(needle)
                end = matching(text, position + len(needle) - 1)
                inner = text[position + len(needle):end]
                self.expander.note_function(self, function)
                text = text[:position] + f"{function}({inner}, $seed+{math.sin(position) * 0.5 + 0.5:f})" + text[end + 1:]
        return text

    def _gradient(self, prefix, value, rv, declare):
        current = self.expander.current_pass
        value = value or {"points": [{"pos": 0, "r": 0, "g": 0, "b": 0, "a": 1}, {"pos": 1, "r": 1, "g": 1, "b": 1, "a": 1}], "interpolation": 1}
        points = sorted(value["points"] if isinstance(value, dict) else value, key=lambda p: p["pos"])
        interpolation = int(value.get("interpolation", 1)) if isinstance(value, dict) else 1
        pos = [current.float_slot((prefix, "p", i), p["pos"]) for i, p in enumerate(points)]
        col = [current.vec4_slot((prefix, "c", i), [p["r"], p["g"], p["b"], p.get("a", 1.0)]) for i, p in enumerate(points)]
        function = f"{prefix}_gradient_fct"
        if declare:
            count = len(points)
            body = f"vec4 {function}(float x) {{\n"
            if interpolation == 0:
                body += f"  if (x < {pos[1] if count > 1 else '0.0'}) return {col[0]};\n"
                for i in range(1, count - 1):
                    body += f"  if (x < {pos[i + 1]}) return {col[i]};\n"
                body += f"  return {col[-1]};\n"
            else:
                blend = "(" if interpolation == 1 else "0.5-0.5*cos(3.14159265359*"
                body += f"  if (x < {pos[0]}) return {col[0]};\n"
                for i in range(count - 1):
                    t = f"(x-{pos[i]})/({pos[i + 1]}-{pos[i]})"
                    if interpolation == 3:
                        b = f"mix({col[i]}, {col[i + 1]}, {t})"
                        if i > 0 and i < count - 2:
                            a = f"mix({col[i - 1]}, {col[i]}, (x-{pos[i - 1]})/({pos[i]}-{pos[i - 1]}))"
                            c = f"mix({col[i + 1]}, {col[i + 2]}, (x-{pos[i + 1]})/({pos[i + 2]}-{pos[i + 1]}))"
                            body += f"  if (x < {pos[i + 1]}) return 0.5*({b} + mix({a}, {c}, 0.5-0.5*cos(3.14159265359*{t})));\n"
                        elif i > 0:
                            a = f"mix({col[i - 1]}, {col[i]}, (x-{pos[i - 1]})/({pos[i]}-{pos[i - 1]}))"
                            body += f"  if (x < {pos[i + 1]}) return mix({a}, {b}, 0.5+0.5*{t});\n"
                        elif i < count - 2:
                            c = f"mix({col[i + 1]}, {col[i + 2]}, (x-{pos[i + 1]})/({pos[i + 2]}-{pos[i + 1]}))"
                            body += f"  if (x < {pos[i + 1]}) return mix({c}, {b}, 1.0-0.5*{t});\n"
                        else:
                            body += f"  if (x < {pos[i + 1]}) return {b};\n"
                    else:
                        body += f"  if (x < {pos[i + 1]}) return mix({col[i]}, {col[i + 1]}, {blend}{t}));\n"
                body += f"  return {col[-1]};\n"
            rv.defs += body + "}\n"
        return function

    def _curve(self, prefix, value, rv, declare):
        current = self.expander.current_pass
        value = value or {"points": [{"x": 0, "y": 0, "ls": 0, "rs": 1}, {"x": 1, "y": 1, "ls": 1, "rs": 0}]}
        points = value["points"]
        function = f"{prefix}_curve_fct"
        slots = [current.vec4_slot((prefix, i), [p["x"], p["y"], p["ls"], p["rs"]]) for i, p in enumerate(points)]
        if declare:
            body = f"float {function}(float x) {{\n"
            for i in range(len(points) - 1):
                if i < len(points) - 2:
                    body += f"if (x <= {slots[i + 1]}.x) "
                body += "{\n"
                body += f"float dx = x - {slots[i]}.x;\nfloat d = {slots[i + 1]}.x - {slots[i]}.x;\nfloat t = dx/d;\n"
                body += "float omt = (1.0 - t);\nfloat omt2 = omt * omt;\nfloat omt3 = omt2 * omt;\nfloat t2 = t * t;\nfloat t3 = t2 * t;\nd /= 3.0;\n"
                body += f"float y1 = {slots[i]}.y;\nfloat yac = {slots[i]}.y + d*{slots[i]}.w;\nfloat ybc = {slots[i + 1]}.y - d*{slots[i + 1]}.z;\nfloat y2 = {slots[i + 1]}.y;\n"
                body += "return y1*omt3 + yac*omt2*t*3.0 + ybc*omt*t2*3.0 + y2*t3;\n}\n"
            rv.defs += body + "}\n"
        return function

    def _pixels(self, prefix, value, rv, declare):
        current = self.expander.current_pass
        width, height, bpp = int(value["w"]), int(value["h"]), int(value["bpp"])
        palette = [current.vec4_slot((prefix, i), [c["r"], c["g"], c["b"], c.get("a", 1.0)]) for i, c in enumerate(value["pl"])]
        words = [(int(p) + 2 ** 31) % 2 ** 32 - 2 ** 31 for p in value["px"]]
        function = f"{prefix}_pixels_fct"
        if declare:
            per_word = 32 // bpp
            colors = ", ".join(palette)
            rv.defs += (f"const int {prefix}_pixels[{len(words)}] = int[{len(words)}]({', '.join(str(w) for w in words)});\n"
                        f"const ivec2 {prefix}_size = ivec2({width}, {height});\n"
                        f"vec4 {function}(ivec2 v) {{\n\tvec4 palette[{len(palette)}] = vec4[{len(palette)}]({colors});\n"
                        f"\tif (v.x < 0 || v.x >= {width} || v.y < 0 || v.y >= {height}) {{\n\t\treturn palette[0];\n\t}} else {{\n"
                        f"\t\tint i = v.x+{width}*v.y;\n\t\tint c = {(1 << bpp) - 1} & ({prefix}_pixels[i/{per_word}] >> ({bpp}*(i%{per_word})));\n"
                        f"\t\treturn palette[c];\n\t}}\n}}\n")
        return function, f"{prefix}_size"

    # -- inputs --

    def _generate_input_function(self, index, definition, rv, context):
        source = self.get_source(index)
        if source is None:
            return
        local = Context(context)
        port_type = self.expander.types[definition["type"]]
        source_rv = source[0].shader_code(port_type["params"], source[1], local)
        rv.merge(source_rv)
        rv.defs += (f"{port_type['type']} {self.genname}_input_{definition['name']}({port_type['paramdefs']}, "
                    "float _seed_variation_, vec4 _controlled_variation_) {\n")
        rv.defs += f"{source_rv.code}\nreturn {source_rv.output_values[definition['type']]};\n}}\n"

    def _replace_input(self, index, suffix, parameters, variables, rv, context):
        definition = self.inputs[index]
        source = self.get_source(index)
        if source is None:
            old_uv = variables["uv"]
            # .variation() has more arguments than coordinates, and only those reach a default
            coordinates = split_arguments(parameters)[0] if suffix == "variation" else parameters
            variables["uv"] = parenthesized(coordinates)
            replaced = self._replace_variables(str(definition.get("default", "")), variables, rv, context)
            variables["uv"] = old_uv
            return replaced
        if suffix:
            attributes = source[0].get_output_attributes(source[1])
            attribute = "texture_size" if suffix == "size" else suffix
            if attributes and attribute in attributes:
                return attributes[attribute]
            if suffix in ("texture", "size"):
                raise ExpansionError(f"{self.name}: input {definition['name']} needs a texture for .{suffix}")
        if definition.get("function"):
            function = f"{self.genname}_input_{definition['name']}"
            if suffix == "variation":
                arguments = split_arguments(parameters)
                tail = ")"
                if len(arguments) == 1:
                    tail = ", _seed_variation_, _controlled_variation_)"
                elif len(arguments) == 2:
                    tail = ", _controlled_variation_)"
                return function + "(" + ", ".join(arguments) + tail
            return f"{function}({parameters}, _seed_variation_, _controlled_variation_)"
        source_rv = source[0].shader_code(parameters, source[1], context)
        rv.merge(source_rv)
        rv.code += source_rv.code
        rv.alias = source_rv
        return source_rv.output_values[definition["type"]]

    def _replace_variables(self, string, variables, rv, context):
        string_end = ""
        while True:
            dollar = string.rfind("$")
            if dollar == -1:
                break
            if dollar + 1 < len(string) and string[dollar + 1] == "(":
                variable_end = matching(string, dollar + 1) + 1
                variable = string[dollar + 2:variable_end - 1]
            else:
                variable_end = dollar + 1
                while variable_end < len(string) and string[variable_end] in WORD_LETTERS:
                    variable_end += 1
                variable = string[dollar + 1:variable_end]
            string_end = string[variable_end:] + string_end
            string = string[:dollar]
            if variable not in variables:
                raise ExpansionError(f"{self.name}: {variable} not found")
            replacement = variables[variable]
            if isinstance(replacement, str):
                string += replacement
            else:
                replace_all = dollar == 0
                string_end = string_end.strip()
                parameters = ""
                suffix = ""
                if string_end[:1] == ".":
                    string_end = string_end[1:].strip()
                    end = 0
                    while end < len(string_end) and string_end[end] in WORD_LETTERS:
                        end += 1
                    suffix = string_end[:end]
                    string_end = string_end[end:].strip()
                if string_end[:1] == "(":
                    close = matching(string_end, 0)
                    if close < len(string_end) - 1:
                        replace_all = False
                    parameters = string_end[:close + 1]
                    if parameters[1] == "(" and matching(parameters, 1) == len(parameters) - 2:
                        parameters = parameters[1:-1]
                    string_end = string_end[close + 1:]
                    parameters = parameters[1:-1] if parameters[0] == "(" and matching(parameters, 0) == len(parameters) - 1 else parameters
                else:
                    replace_all = False
                string += self._replace_input(replacement["index"], suffix, parameters, variables, rv, context)
                if not replace_all:
                    rv.alias = None
        return string + string_end

    def _shader_code(self, uv, port, context):
        rv = Code()
        current = self.expander.current_pass
        declare = not context.has_variant(self)
        output = self.outputs[port]
        variables = {"uv": parenthesized(uv), "name": self.genname,
                     "?1": "_controlled_variation_.x", "?2": "_controlled_variation_.y",
                     "?3": "_controlled_variation_.z", "?4": "_controlled_variation_.w",
                     "node_id": str(self.index)}
        variables["seed"] = f"({current.float_slot(self.genname + '_seed', self.seed)}+fract(_seed_variation_))"
        for name, (key, value) in self.named_parameters.items():
            variables[name] = current.float_slot(key, value)
        if "$time" in self.instance + self.code + output["expr"]:
            variables["time"] = current.float_slot("elapsed_time", 0.0)
        self._process_parameters(rv, variables, declare)
        for i, definition in enumerate(self.inputs):
            if declare and definition.get("function"):
                self._generate_input_function(i, definition, rv, context)
            variables[definition["name"]] = {"index": i}
        if declare:
            rv.functions |= set(self.functions)
            rv.functions |= self.expander.extra_functions.get(self, set())
            if self.instance.strip():
                rv.defs += self._replace_variables(self.instance, variables, rv, context) + "\n\n"
        if self.code.strip() and "@NOCODE" not in output["expr"]:
            variant_index = context.get_variant(self, uv)
            variables["name_uv"] = f"{self.genname}_{context.get_variant(self, uv)}"
            if variant_index == -1:
                code = self._replace_variables(self.code, variables, rv, context)
                if code:
                    rv.code += code + "\n"
        else:
            variables.setdefault("name_uv", f"{self.genname}_nouv")
        variant_string = f"{uv},{port}"
        variant_index = context.get_variant(self, variant_string)
        assign = False
        if variant_index == -1:
            variant_index = context.get_variant(self, variant_string)
            assign = True
        kind = output["type"]
        keep_type = "@KEEPTYPE" in output["expr"]
        text = output["expr"].replace("@NOCODE", "").replace("@KEEPTYPE", "").strip()
        expression = self._replace_variables(text, variables, rv, context)
        rv.functions |= self.expander.extra_functions.get(self, set())
        if keep_type and rv.alias is not None:
            rv.output_values = rv.alias.output_values
            rv.output_type = rv.alias.output_type
            return rv
        variable = f"{self.genname}_{port}_{variant_index}_{kind}"
        if assign:
            rv.code += f"{self.expander.types[kind]['type']} {variable} = {expression};\n"
        rv.output_values[kind] = variable
        rv.output_type = kind
        return rv


class Expander:
    def __init__(self, nodes_dir: Path):
        self.nodes_dir = nodes_dir
        self.descriptors = {}
        for path in sorted(nodes_dir.glob("*.json")):
            if not path.name.startswith("_"):
                data = json.loads(path.read_text(encoding="utf-8"))
                self.descriptors[data["id"]] = data
        self.functions = json.loads((nodes_dir / "_functions.json").read_text(encoding="utf-8"))["functions"]
        self.types = {t["name"]: t for t in json.loads((nodes_dir / "_types.json").read_text(encoding="utf-8"))["types"]}
        self.extra_functions = {}
        self.counter = 0
        self.current_pass = None

    def next_index(self) -> int:
        self.counter += 1
        return self.counter

    def note_function(self, generator, function) -> None:
        self.extra_functions.setdefault(generator, set()).add(function)

    def instantiate(self, descriptor_id: str, parameters=None, seed=0.0, generic_size=1):
        """Builds the generator tree of a node; returns a function port -> (generator, port)."""
        descriptor = self.descriptors[descriptor_id]
        if descriptor["kind"] == "shader":
            shader = Shader(self, descriptor_id, descriptor, parameters or {}, seed, generic_size)
            return shader, [(shader, i) for i in range(len(shader.outputs))], [(shader, i) for i in range(len(shader.inputs))]
        return self.instantiate_graph(descriptor_id, descriptor, parameters or {})

    def instantiate_graph(self, owner, graph, parameters):
        values = {}
        for definition in graph.get("parameters", []):
            value = parameters.get(definition["name"], definition.get("default"))
            for link in definition.get("links", []):
                values.setdefault(link["node"], {})[link["param"]] = value
            configurations = definition.get("configurations")
            if configurations:
                keys = sorted(configurations)
                index = 1 if value is True else 0 if value is False else int(value or 0)
                for link in configurations[keys[min(index, len(keys) - 1)]]:
                    decoded = link["value"]
                    values.setdefault(link["node"], {})[link["param"]] = decoded
        named = {definition["name"]: (f"{owner}_{definition['name']}", parameters.get(definition["name"], definition.get("default", 0.0)))
                 for definition in graph.get("parameters", []) if definition.get("named")}
        children = {}
        for node in graph["nodes"]:
            name = node["id"]
            merged = {**node.get("parameters", {}), **values.get(name, {})}
            seed = node.get("seed", 0.0)
            generic = node.get("generic_size", 1)
            if "shader" in node:
                children[name] = self._child_entry(Shader(self, f"{owner}/{name}", node["shader"], merged, seed, generic))
            elif "graph" in node:
                children[name] = self.instantiate_graph(f"{owner}/{name}", node["graph"], merged)
            elif node.get("type") in ("buffer", "iterate_buffer"):
                kind = "iterate" if node["type"] == "iterate_buffer" else "buffer"
                source = TextureSource(self, f"{owner}/{name}", merged, kind)
                children[name] = self._child_entry(source)
            elif node.get("type") == "switch":
                children[name] = self._child_entry(Switch(self, f"{owner}/{name}", merged))
            elif node.get("type") == "ios":
                raise ExpansionError(f"{owner}: a free-standing ios node {name}")
            else:
                children[name] = self.instantiate(node["type"], merged, seed, generic)
            if name in children and isinstance(children[name][0], Shader):
                children[name][0].named_parameters = named
        # the interface of the graph: ports wired below
        outputs = [None] * len(graph.get("outputs", []))
        input_targets = {}
        for connection in graph.get("connections", []):
            source_name, target_name = connection["from"], connection["to"]
            from_port, to_port = int(connection["from_port"]), int(connection["to_port"])
            if target_name == "gen_outputs":
                if source_name == "gen_inputs":
                    continue
                if source_name in children and from_port < len(children[source_name][1]):
                    outputs[to_port] = children[source_name][1][from_port]
                continue
            if target_name not in children:
                raise ExpansionError(f"{owner}: connection to unknown node {target_name}")
            targets = children[target_name][2]
            if source_name == "gen_inputs":
                if to_port < len(targets):
                    input_targets.setdefault(from_port, []).append(targets[to_port])
                continue
            if source_name not in children:
                raise ExpansionError(f"{owner}: connection from unknown node {source_name}")
            if to_port < len(targets) and from_port < len(children[source_name][1]):
                self.connect(targets[to_port], children[source_name][1][from_port])
        for connection in graph.get("connections", []):
            if connection["from"] == "gen_inputs" and connection["to"] == "gen_outputs":
                outputs[int(connection["to_port"])] = ("input", int(connection["from_port"]))
        inputs = []
        for index in range(len(graph.get("inputs", []))):
            inputs.append(("group", input_targets.get(index, [])))
        return None, outputs, inputs

    @staticmethod
    def _child_entry(generator):
        if isinstance(generator, TextureSource):
            return generator, [(generator, i) for i in range(2)], [(generator, i) for i in range(2)]
        if isinstance(generator, Switch):
            ports = [(generator, i) for i in range(int(generator.parameters.get("outputs", 2)))]
            inputs = int(generator.parameters.get("outputs", 2)) * int(generator.parameters.get("choices", 2))
            return generator, ports, [(generator, i) for i in range(inputs)]
        return generator, [(generator, i) for i in range(len(generator.outputs))], [(generator, i) for i in range(len(generator.inputs))]

    def connect(self, target, generator_port) -> None:
        if target[0] == "group":
            for inner in target[1]:
                self.connect(inner, generator_port)
        else:
            target[0].sources[target[1]] = generator_port

    def function_closure(self, ids) -> list[str]:
        emitted, order = set(), []

        def visit(function_id):
            if function_id in emitted:
                return
            emitted.add(function_id)
            for dep in sorted(self.functions[function_id]["deps"]):
                visit(dep)
            order.append(function_id)

        for function_id in sorted(ids):
            visit(function_id)
        return order

    def shader_text(self, root: tuple, current: Pass) -> str:
        self.current_pass = current
        generator, port = root
        kind = generator.output_type(port)
        # a 2D view of a 3D output: the coordinate the port's functions take, at z = 0
        coordinate = self.types[kind]["params"]
        coordinate_type = self.types[kind]["paramdefs"].split()[0]
        setup = "" if coordinate == "uv" else f"\t{coordinate_type} {coordinate} = {coordinate_type}(uv, {'0.0' if coordinate_type == 'vec3' else '0.0, 0.0'});"
        rv = generator.shader_code(coordinate, port, Context())
        color = {"f": "vec4(vec3(@), 1.0)", "rgb": "vec4(@, 1.0)", "rgba": "@", "sdf2d": "vec4(vec3(@), 1.0)",
                 "fill": "@", "sdf3d": "vec4(vec3(@), 1.0)", "sdf3dc": "vec4(vec3(@.x), 1.0)",
                 "tex3d": "vec4(@, 1.0)", "tex3d_gs": "vec4(vec3(@), 1.0)"}[rv.output_type].replace("@", rv.output_values[rv.output_type])
        functions = "\n".join("\n".join(self.functions[f]["code"]) for f in self.function_closure(rv.functions | {"jp_finite"}))
        lines = ["#version 450", "in vec2 v_uv;", "out vec4 g_finalColor;"]
        for item in current.textures:
            lines.append(f"uniform texture2D t_{item['name']};")
            lines.append(f"uniform sampler s_{item['name']};")
        if current.values:
            lines.append(f"uniform u_fs_procmat {{\n\tvec4 v[{(len(current.values) + 3) // 4}];\n}} pm;")
        return "\n".join(lines + [functions, rv.defs, "void main() {", "\tfloat _seed_variation_ = 0.0;",
                                  "\tvec4 _controlled_variation_ = vec4(0.0);", "\tvec2 uv = v_uv;", setup, rv.code,
                                  f"\tg_finalColor = jp_finite({color});", "}", ""]) if kind else ""


def slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.=-]+", "_", text)


def expand_variant(expander: Expander, node_id: str, variant: str, parameters: dict, generic: int, wire: bool, sink) -> int:
    descriptor = expander.descriptors[node_id]
    expander.counter = 0
    expander.extra_functions = {}
    _, outputs, inputs = expander.instantiate(node_id, parameters, 0.0, generic)
    needs_texture = uses_texture_attribute(descriptor)
    for index, target in enumerate(inputs):
        kind = port_type(descriptor, index, generic)
        if wire or kind is None or (index in needs_texture):
            kind = kind or "rgba"
            if index in needs_texture:
                source = TextureSource(expander, f"{node_id}/probe{index}", {"size": 10})
                source.sources[0] = (Probe(expander, "rgba"), 0)
                expander.connect(target, (source, 0))
            else:
                expander.connect(target, (Probe(expander, kind), 0))
    count = 0
    for port, output in enumerate(outputs):
        if output is None or output[0] == "input":
            continue
        count += expand_passes(expander, node_id, variant, port, output, sink)
    return count


def uses_texture_attribute(descriptor) -> set[int]:
    result = set()
    text = json.dumps({k: descriptor.get(k) for k in ("instance", "code", "outputs")})
    for i, port in enumerate(descriptor.get("inputs", [])):
        if f"${port['name']}.texture" in text or f"${port['name']}.size" in text:
            result.add(i)
    return result


def port_type(descriptor, index, generic):
    inputs = descriptor.get("inputs", [])
    expanded = []
    groups = [p for p in inputs if "#" in p.get("name", "")]
    for port in inputs:
        if "#" in port.get("name", ""):
            if port is groups[0]:
                for _ in range(generic):
                    expanded.extend(groups)
        else:
            expanded.append(port)
    return expanded[index]["type"] if index < len(expanded) else None


def expand_passes(expander, node_id, variant, port, root, sink) -> int:
    """A root output plus the Buffers it reaches, each one pass with its own uniforms."""
    done = set()
    queue = [("out", root)]
    count = 0
    while queue:
        label, target = queue.pop()
        key = (id(target[0]), target[1])
        if key in done:
            continue
        done.add(key)
        current = Pass()
        text = expander.shader_text(target, current)
        name = slug(f"{node_id}.{variant}.o{port}.{label}")
        sink(name, node_id, variant, port, label, text, current)
        count += 1
        for item in current.textures:
            texture = item["generator"]
            source = texture.get_source(0)
            if source is not None:
                queue.append((f"p{texture.index}in", source))
            loop = texture.get_source(1) if texture.pass_kind == "iterate" else None
            if loop is not None:
                queue.append((f"p{texture.index}loop", loop))
    return count


def variants_of(descriptor):
    yield "default", {}, 1, False
    has_inputs = bool(descriptor.get("inputs"))
    if has_inputs:
        yield "wired", {}, 1, True
    for parameter in descriptor.get("parameters", []):
        name = parameter["name"]
        kind = parameter.get("type")
        if kind == "enum":
            default = parameter.get("default", 0)
            for i, value in enumerate(parameter.get("values", [])):
                if i != default:
                    yield f"{name}={i}", {name: i}, 1, has_inputs
        elif kind == "boolean":
            yield f"{name}={'false' if parameter.get('default') else 'true'}", {name: not parameter.get("default")}, 1, has_inputs
    if any("#" in p.get("name", "") for p in descriptor.get("inputs", []) + descriptor.get("parameters", [])):
        yield "generic2", {}, 2, has_inputs


def expand_chains(expander: Expander, sink) -> None:
    """Noise and blend nodes in one pass: a plausible graph of N nodes with no Buffer to cut it."""
    for size in (10, 30, 60, 120, 240):
        expander.counter = 0
        expander.extra_functions = {}
        generator = expander.instantiate("perlin", {}, 0.1)[0]
        previous = (generator, 0)
        count, step = 1, 0
        while count < size:
            noise = expander.instantiate(("fbm4", "voronoi2", "perlin", "bricks3")[step % 4], {}, 0.01 * step)[0]
            blend = expander.instantiate("blend2", {}, 0.0, 1)[0]
            blend.sources[blend.input_index("b")] = previous
            blend.sources[blend.input_index("l1")] = (noise, 0)
            previous = (blend, 0)
            count += 2
            step += 1
        current = Pass()
        sink(f"chain{size}", f"chain{size}", "default", 0, "out", expander.shader_text(previous, current), current)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nodes", type=Path, default=Path(__file__).resolve().parents[2] / "editor/procmat/nodes")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--chains", action="store_true", help="also write the N-node passes")
    parser.add_argument("--float-hash", action="store_true", help="Material Maker's rand functions, R10's red control")
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    expander = Expander(args.nodes)
    if args.float_hash:
        for name, code in FLOAT_RAND.items():
            if name in expander.functions:
                expander.functions[name].update(code=code.split("\n"), deps=[])
    manifest = []
    failures = []

    def sink_for(kind):
        def sink(name, node, variant, port, label, text, current):
            (args.out / f"{name}.frag").write_text(text, encoding="utf-8")
            textures = [{"name": t["name"], "binding": i, "size": t["generator"].size} for i, t in enumerate(current.textures)]
            manifest.append({"shader": name, "node": node, "kind": kind, "variant": variant, "output": port, "pass": label,
                             "bytes": len(text.encode()), "uniforms": current.values, "textures": textures})
        return sink

    for node_id, descriptor in sorted(expander.descriptors.items()):
        sink = sink_for(descriptor["kind"])
        for variant, parameters, generic, wire in variants_of(descriptor):
            try:
                expand_variant(expander, node_id, variant, parameters, generic, wire, sink)
            except ExpansionError as error:
                failures.append({"node": node_id, "variant": variant, "error": str(error)})
            except (KeyError, IndexError, TypeError, ValueError) as error:
                failures.append({"node": node_id, "variant": variant, "error": f"{type(error).__name__}: {error}"})
    if args.chains:
        expand_chains(expander, sink_for("graph"))
    (args.out / "manifest.json").write_text(json.dumps({"shaders": manifest, "expansion_failures": failures}, indent=0), encoding="utf-8")
    print(f"expand_nodes: {len(manifest)} shaders from {len(expander.descriptors)} nodes, {len(failures)} expansion failures")
    for failure in failures[:40]:
        print(f"  {failure['node']} [{failure['variant']}]: {failure['error']}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
