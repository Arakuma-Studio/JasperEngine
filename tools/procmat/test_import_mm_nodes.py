#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""AC11 of docs/specs/S90-procedural-material.md: the node importer's licence and data rules.

    python3 tools/procmat/test_import_mm_nodes.py

Uses a small synthetic source tree, so it needs no clone: the real library is untrusted and is not
test data (R18 names pentagram.ptex in particular). With MM_CLONE set to a clone of Material Maker
at the recorded commit, it also checks that the committed output is what the tool writes.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NODES = REPO / "editor/procmat/nodes"

spec = importlib.util.spec_from_file_location("import_mm_nodes", Path(__file__).with_name("import_mm_nodes.py"))
importer = importlib.util.module_from_spec(spec)
sys.modules["import_mm_nodes"] = importer
spec.loader.exec_module(importer)

COMMON = """[resource]
text = "float dot2(vec2 x) {
\treturn dot(x, x);
}

float rand(vec2 x) {
    return fract(cos(mod(dot(x, vec2(13.9898, 8.141)), 3.14)) * 43758.5);
}

vec2 rand2(vec2 x) {
    return fract(cos(mod(vec2(dot(x, vec2(13.9898, 8.141)), dot(x, vec2(3.4562, 17.398))), vec2(3.14, 3.14))) * 43758.5);
}

vec3 rand3(vec2 x) {
    return fract(cos(mod(vec3(dot(x, vec2(13.9898, 8.141)), dot(x, vec2(3.4562, 17.398)), dot(x, vec2(13.254, 5.867))), vec3(3.14, 3.14, 3.14))) * 43758.5);
}

vec3 rand3_4d(vec4 x) {
    return fract(cos(mod(vec3(dot(x, vec4(1.0)), dot(x, vec4(2.0)), dot(x, vec4(3.0))), vec3(3.14))) * 43758.5);
}

float param_rnd(float minimum, float maximum, float seed) {
\treturn minimum+(maximum-minimum)*rand(vec2(seed));
}
"
"""

# io_types.mmt really has trailing commas, as smooth_curvature2.mmg does
TYPES = """[
 { "name":"f", "label":"Grayscale", "type":"float", "paramdefs":"vec2 uv", "params":"uv", "slot_type":0,
   "convert":[ { "type":"rgba", "expr":"vec4(vec3($(value)), 1.0)" } ], "color":"#8D8D8D", },
 { "name":"rgba", "label":"RGBA", "type":"vec4", "paramdefs":"vec2 uv", "params":"uv", "slot_type":0, "color":"#335DC7", },
]"""


def shader(name="Node", global_code="", instance="", code="", output="0.0", parameters=(), inputs=(), **extra):
    model = {"name": name, "global": global_code, "instance": instance, "code": code, "inputs": list(inputs),
             "outputs": [{"f": output, "type": "f"}], "parameters": list(parameters), **extra}
    return {"name": name.lower(), "type": "shader", "parameters": {}, "seed_int": 0, "shader_model": model}


class Source:
    """A throwaway tree shaped like the clone, with one library entry per node."""

    def __init__(self, root: Path):
        self.root = root
        self.nodes = root / importer.NODE_DIR
        self.nodes.mkdir(parents=True)
        (root / importer.FUNCTIONS_FILE).parent.mkdir(parents=True, exist_ok=True)
        (root / importer.FUNCTIONS_FILE).write_text(COMMON, encoding="utf-8")
        (self.nodes / "io_types.mmt").write_text(TYPES, encoding="utf-8")
        (root / importer.LIBRARY_FILE).parent.mkdir(parents=True)
        (root / importer.ALIASES_FILE).write_text('{ "Noise/plain": "alias,other" }', encoding="utf-8")
        self.entries = []

    def add(self, node_id: str, data, tree=None, raw: str | None = None):
        path = self.nodes / f"{node_id}.mmg"
        path.write_text(raw if raw is not None else json.dumps(data), encoding="utf-8")
        if tree is not False:
            self.entries.append({"tree_item": tree or f"Noise/{node_id}", "type": node_id, "display_name": node_id})

    def finish(self):
        (self.root / importer.LIBRARY_FILE).write_text(json.dumps({"lib": self.entries, "name": "base"}), encoding="utf-8")

    def run(self, out: Path):
        self.finish()
        run = importer.Importer(self.root, "abc1234")
        run.run(out)
        return run


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.source = Source(self.root / "clone")
        self.out = self.root / "out"

    def descriptor(self, node_id: str):
        return json.loads((self.out / f"{node_id}.json").read_text(encoding="utf-8"))

    def functions(self):
        return json.loads((self.out / "_functions.json").read_text(encoding="utf-8"))["functions"]

    def assertRefuses(self, pattern: str):
        with self.assertRaisesRegex(importer.ImportFailure, pattern):
            self.source.run(self.out)
        self.assertFalse(list(self.out.glob("*.json")) if self.out.exists() else [], "a failed import wrote output")

    # -- AC11: attribution comments --

    def test_unrecognised_attribution_comment_fails(self):
        self.source.add("plain", shader(global_code="// Based on https://www.shadertoy.com/view/abcd\nfloat f(float x) { return x; }\n"))
        self.assertRefuses(r"plain: unrecognised attribution comment: .*shadertoy")

    def test_each_kind_of_attribution_comment_is_noticed(self):
        for number, comment in enumerate(("Copyright 2019 Someone", "© Someone", "Licensed under the GPL", "CC BY-NC-SA", "SPDX-License-Identifier: GPL-3.0",
                                          "adapted from elsewhere", "ported from Foo", "stolen from Bar", "/* from www.example.com */", "All rights reserved")):
            with self.subTest(comment=comment):
                source = Source(self.root / f"clone{number}")
                text = comment if comment.startswith("/*") else "// " + comment
                source.add("plain", shader(instance=text + "\n"))
                source.finish()
                with self.assertRaisesRegex(importer.ImportFailure, "unrecognised attribution"):
                    importer.Importer(source.root, "abc1234").run(self.out)

    def test_attribution_inside_a_graph_is_noticed(self):
        inner = shader(global_code="// Copyright Someone\nfloat f(float x) { return x; }\n")
        inner["name"] = "inner"
        graph = {"type": "graph", "name": "g", "label": "G", "nodes": [inner], "connections": [], "parameters": {}}
        self.source.add("wrapped", graph)
        self.assertRefuses(r"wrapped: unrecognised attribution comment")

    def test_attribution_in_a_description_is_noticed(self):
        self.source.add("plain", shader(shortdesc="Ported shader, Copyright Someone"))
        self.assertRefuses(r"plain: unrecognised attribution in a description")

    def test_a_changed_known_comment_fails(self):
        # the table is exact: the licence text changing upstream must stop the import
        self.source.add("sdstar", shader(global_code="// The MIT License, Copyright © 2020 Someone Else\nfloat f(float x) { return x; }\n"))
        self.assertRefuses(r"sdstar: unrecognised attribution comment")

    def test_a_known_comment_disappearing_fails(self):
        self.source.add("sdstar", shader(global_code="float f(float x) { return x; }\n"))
        self.assertRefuses(r"sdstar: the known attribution comment is gone")

    # -- AC11: the deny-list --

    def test_deny_listed_nodes_never_appear(self):
        self.source.add("plain", shader())
        for node_id in importer.DENY_LIST:
            self.source.add(node_id, shader(global_code="// (CC BY-SA 3.0 License)\nfloat forbidden_code(float x) { return x; }\n"))
        self.source.run(self.out)
        written = {p.stem for p in self.out.glob("*.json")}
        self.assertIn("plain", written)
        self.assertFalse(written & set(importer.DENY_LIST))
        manifest = json.loads((self.out / "_manifest.json").read_text(encoding="utf-8"))
        for node_id in importer.DENY_LIST:
            self.assertIn("deny-list", manifest["excluded"][node_id])
        self.assertNotIn("forbidden_code", (self.out / "_functions.json").read_text(encoding="utf-8"))

    def test_a_node_including_a_denied_node_goes_with_it(self):
        self.source.add("skewed_bricks", shader(global_code="float skew(float x) { return x; }\n"))
        self.source.add("user", shader(includes=["skewed_bricks"], output="skew(0.0)"))
        self.source.run(self.out)
        self.assertFalse((self.out / "user.json").exists())
        manifest = json.loads((self.out / "_manifest.json").read_text(encoding="utf-8"))
        self.assertIn("skewed_bricks", manifest["excluded"]["user"])

    def test_a_graph_containing_a_denied_node_goes_with_it(self):
        self.source.add("shard_fbm", shader())
        graph = {"type": "graph", "name": "g", "label": "G", "connections": [], "parameters": {},
                 "nodes": [{"name": "n", "type": "shard_fbm", "parameters": {}}]}
        self.source.add("wrapped", graph)
        self.source.run(self.out)
        self.assertFalse((self.out / "wrapped.json").exists())

    def test_a_denied_node_is_followed_through_nested_includes(self):
        self.source.add("skewed_bricks", shader(global_code="float forbidden_code(float x) { return x; }\n"), tree=False)
        self.source.add("middle", shader(includes=["skewed_bricks"]), tree=False)
        self.source.add("user", shader(includes=["middle"], output="forbidden_code(0.0)"))
        inner = shader(includes=["middle"], output="forbidden_code(0.0)")
        inner["name"] = "inner"
        self.source.add("wrapped", {"type": "graph", "name": "g", "label": "G", "nodes": [inner], "connections": [], "parameters": {}})
        self.source.add("plain", shader())
        self.source.run(self.out)
        self.assertFalse((self.out / "user.json").exists())
        self.assertFalse((self.out / "wrapped.json").exists())
        self.assertNotIn("forbidden_code", (self.out / "_functions.json").read_text(encoding="utf-8"))

    def test_the_3d_category_is_left_out(self):
        self.source.add("solid", shader(), tree="3D/SDF/solid")
        self.source.run(self.out)
        self.assertFalse((self.out / "solid.json").exists())

    # -- R19: notices --

    def test_notice_kept_in_the_descriptor_and_passed_to_includers(self):
        self.source.add("sdstar", shader(global_code="// The MIT License, Copyright © 2019 Inigo Quilez\n// https://www.shadertoy.com/view/3tSGDy\nfloat star(float x) { return x; }\n", output="star(0.0)"))
        self.source.add("user", shader(includes=["sdstar"], output="star(1.0)"))
        self.source.run(self.out)
        for node_id in ("sdstar", "user"):
            notices = self.descriptor(node_id)["notices"]
            self.assertEqual([(n["license"], n["holder"]) for n in notices], [("MIT", "Inigo Quilez")], node_id)
        # the comment stays with the code it covers
        self.assertIn("Inigo Quilez", "\n".join(self.functions()["star"]["code"]))

    def test_a_copy_without_the_comment_keeps_the_notice(self):
        code = "float star(float x) { return x; }\n"
        self.source.add("aaa_copy", shader(global_code=code, output="star(0.0)"))
        self.source.add("sdstar", shader(global_code="// The MIT License, Copyright © 2019 Inigo Quilez\n// https://www.shadertoy.com/view/3tSGDy\n" + code, output="star(0.0)"))
        self.source.run(self.out)
        function = self.functions()["star"]
        self.assertEqual([n["node"] for n in function["notices"]], ["sdstar"])
        self.assertIn("Inigo Quilez", "\n".join(function["code"]))
        self.assertEqual([n["holder"] for n in self.descriptor("aaa_copy")["notices"]], ["Inigo Quilez"])

    def test_the_shared_functions_are_scanned_for_attributions(self):
        path = self.source.root / importer.FUNCTIONS_FILE
        path.write_text(path.read_text(encoding="utf-8").replace("float dot2", "// Copyright Someone\nfloat dot2"), encoding="utf-8")
        self.source.add("plain", shader())
        self.assertRefuses(r"shader_functions: unrecognised attribution")

    # -- R17, R3: what is data and what is refused --

    def graph_with(self, value):
        inner = shader(parameters=[{"name": "x", "type": "float", "default": 1, "min": 0, "max": 1, "step": 0.1, "label": ""}], output="$x")
        inner.update(name="inner", parameters={"x": value})
        remote = {"name": "gen_parameters", "type": "remote", "parameters": {},
                  "widgets": [{"name": "scale", "label": "Scale", "type": "named_parameter", "min": 0, "max": 1, "step": 0.1, "default": 0.5}]}
        return {"type": "graph", "name": "g", "label": "G", "nodes": [inner, remote], "connections": [], "parameters": {}}

    def test_a_graph_parameter_may_be_a_plain_expression(self):
        for value in ("0.005/$scale", "$uv.x", "3.14 / 2.0", "1.0+$scale", "$rnd(0.0, 1.0)"):
            with self.subTest(value=value):
                source = Source(self.root / f"good{abs(hash(value))}")
                source.add("wrapped", self.graph_with(value))
                out = self.root / f"good{abs(hash(value))}_out"
                source.run(out)
                self.assertTrue((out / "wrapped.json").exists())

    def test_a_graph_parameter_that_is_not_an_expression_is_refused(self):
        for number, value in enumerate(("1.0); return vec4(0.0); //", "$(expr:1+1)", "$nothing", "sin(1.0)", "foo")):
            with self.subTest(value=value):
                source = Source(self.root / f"bad{number}")
                source.add("wrapped", self.graph_with(value))
                with self.assertRaisesRegex(importer.ImportFailure, "wrapped/inner: parameter x"):
                    source.run(self.root / f"bad{number}_out")

    def test_a_host_path_preset_is_not_kept(self):
        self.source.add("plain", shader())
        self.source.entries.append({"tree_item": "Simple/Image", "type": "image", "display_name": "Image",
                                    "parameters": {"image": "%MATERIAL_MAKER_PATH%/examples/mm_icon.png", "filter": True}})
        self.source.run(self.out)
        self.assertNotIn("MATERIAL_MAKER_PATH", (self.out / "_library.json").read_text(encoding="utf-8"))

    def test_a_preset_that_is_not_an_expression_is_refused(self):
        self.source.add("plain", shader())
        self.source.entries[0]["parameters"] = {"x": "1.0); return vec4(0.0); //"}
        self.assertRefuses(r"parameter x is not an expression")

    def test_trailing_commas_are_tolerated_and_strings_keep_theirs(self):
        data = shader(output='vec4(1.0, 1.0, 1.0, 1.0).x')
        raw = json.dumps(data, indent=1)
        self.assertEqual(raw[-2:], "\n}")
        self.source.add("commas", None, raw=raw[:-2] + ",\n}")
        self.source.run(self.out)
        self.assertIn("1.0, 1.0", self.descriptor("commas")["outputs"][0]["expr"])
        self.assertEqual(importer.strip_trailing_commas('{"a": ",}", "b": [1, 2, ], }'), '{"a": ",}", "b": [1, 2 ] }')

    def test_fields_that_execute_are_refused(self):
        self.source.add("exporter", shader(custom="OS.execute()"))
        self.source.run(self.out)
        self.assertFalse((self.out / "exporter.json").exists())
        self.assertNotIn("OS.execute", "".join(p.read_text(encoding="utf-8") for p in self.out.glob("*.json")))

    def test_an_expression_macro_is_refused(self):
        self.source.add("plain", shader(output="$(expr:1+1)"))
        self.assertRefuses(r"plain: refused text")

    def test_an_unknown_macro_is_refused(self):
        self.source.add("plain", shader(output="$nothing"))
        self.assertRefuses(r"plain: unknown macro")

    def test_a_website_node_is_refused(self):
        self.source.add("website_1", shader(), tree=False)
        self.assertRefuses(r"website node")

    def test_an_unknown_model_field_is_refused(self):
        self.source.add("plain", shader(surprise="x"))
        self.assertRefuses(r"plain: unknown shader model field")

    # -- R10 --

    def test_rand_functions_are_the_integer_hash(self):
        self.source.add("plain", shader(output="rand(vec2(1.0)) + rand2(vec2(1.0)).x + rand3(vec2(1.0)).x + rand3_4d(vec4(1.0)).x"))
        self.source.run(self.out)
        functions = self.functions()
        for name in ("rand", "rand2", "rand3", "rand3_4d"):
            text = "\n".join(functions[name]["code"])
            self.assertNotRegex(text, r"\b(sin|cos)\b|43758", name)
            self.assertIn("jp_hash", text, name)
        self.assertIn("* 65536.0", "\n".join(functions["jp_quantize"]["code"]))

    def test_a_node_with_its_own_float_hash_fails(self):
        self.source.add("plain", shader(global_code="float h(vec2 p) { return fract(sin(dot(p, vec2(12.9, 78.2))) * 43758.5453); }\n", output="h(vec2(0.0))"))
        self.assertRefuses(r"float hash")

    def test_other_shapes_of_a_float_hash_fail(self):
        for number, code in enumerate(("float h(float x) { return fract(1234.5 * sin(x)); }\n",
                                       "float h(float x) { float s = sin(x) * 43758.5453; return fract(s); }\n",
                                       "float h(float x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n")):
            with self.subTest(code=code):
                source = Source(self.root / f"hash{number}")
                source.add("plain", shader(global_code=code, output="h(0.0)"))
                with self.assertRaisesRegex(importer.ImportFailure, "float hash"):
                    source.run(self.root / f"hash{number}_out")

    def test_the_float_permutation_becomes_the_integer_one(self):
        code = ("float fbm_2d_mod289(float x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n"
                "float fbm_2d_permute(float x) { return fbm_2d_mod289(((x * 34.0) + 1.0) * x); }\n")
        self.source.add("plain", shader(global_code=code, output="fbm_2d_permute(1.0)"))
        self.source.run(self.out)
        functions = self.functions()
        self.assertNotIn("fbm_2d_mod289", functions)
        self.assertEqual(functions["fbm_2d_permute"]["deps"], ["jp_permute289"])
        self.assertNotIn("289.0", "\n".join(functions["jp_permute289"]["code"]))

    def test_a_pass_writes_no_nan_with_isnan(self):
        self.source.add("plain", shader())
        self.source.run(self.out)
        self.assertIn("isnan(c.x)", "\n".join(self.functions()["jp_finite"]["code"]))

    # -- R5: functions by id --

    def test_shared_functions_have_one_id_and_differing_ones_two(self):
        same = "float same(float x) { return x; }\n"
        self.source.add("a", shader(global_code=same + "float other(float x) { return same(x) + 1.0; }\n", output="other(0.0)"))
        self.source.add("b", shader(global_code=same + "float other(float x) { return same(x) + 2.0; }\n", output="other(1.0)"))
        self.source.run(self.out)
        functions = self.functions()
        self.assertIn("same", functions)
        self.assertIn("other_of_a", functions)
        self.assertIn("other_of_b", functions)
        self.assertEqual(self.descriptor("a")["outputs"][0]["expr"], "other_of_a(0.0)")
        self.assertEqual(self.descriptor("b")["outputs"][0]["expr"], "other_of_b(1.0)")
        self.assertEqual(functions["other_of_a"]["deps"], ["same"])

    def test_a_name_built_from_an_enum_resolves(self):
        enum = {"name": "mode", "type": "enum", "default": 0, "label": "", "values": [{"name": "A", "value": "one"}, {"name": "B", "value": "two"}]}
        self.source.add("plain", shader(global_code="float blend_one(float x) { return x; }\nfloat blend_two(float x) { return x; }\nfloat unused(float x) { return x; }\n",
                                        output="blend_$mode(0.0)", parameters=[enum]))
        self.source.run(self.out)
        self.assertEqual(self.descriptor("plain")["functions"], ["blend_one", "blend_two"])
        self.assertNotIn("unused", self.functions())

    def test_instance_functions_carry_their_variation_parameters(self):
        self.source.add("plain", shader(instance="float $(name)_f(vec2 uv) {\n\treturn uv.x;\n}\n", output="$(name)_f($uv)"))
        self.source.run(self.out)
        descriptor = self.descriptor("plain")
        self.assertEqual(descriptor["instance"][0], "float $(name)_f(vec2 uv, float _seed_variation_, vec4 _controlled_variation_) {")
        self.assertEqual(descriptor["outputs"][0]["expr"], "$(name)_f($uv, _seed_variation_, _controlled_variation_)")

    def test_two_runs_write_the_same_bytes_and_check_says_so(self):
        self.source.add("plain", shader())
        self.source.run(self.out)
        first = {p.name: p.read_bytes() for p in self.out.glob("*.json")}
        self.assertEqual(importer.main(["--source", str(self.source.root), "--out", str(self.out), "--commit", "abc1234", "--check"]), 0)
        self.assertEqual(first, {p.name: p.read_bytes() for p in self.out.glob("*.json")})
        (self.out / "plain.json").write_text("{}", encoding="utf-8")
        self.assertEqual(importer.main(["--source", str(self.source.root), "--out", str(self.out), "--commit", "abc1234", "--check"]), 1)


class CommittedOutputTest(unittest.TestCase):
    """The library in the repository holds to the same rules, without the clone."""

    def texts(self):
        return {p: p.read_text(encoding="utf-8") for p in NODES.glob("*.json")}

    def test_no_deny_listed_node_or_its_code(self):
        written = {p.stem for p in NODES.glob("*.json")}
        self.assertFalse(written & set(importer.DENY_LIST))
        # text only those files hold: their attributions and their distinctive code
        markers = re.compile(r"softSurfer|Nexii|CC BY-SA|ENDESGA|dlKyWw|ss3fW4|4lcBWn|scanberg|dotModded|fbmsdf|get_line_intersection|shard_noise")
        for path, text in self.texts().items():
            if path.name == "_manifest.json":
                continue
            self.assertIsNone(markers.search(text), path.name)

    def test_no_float_hash_survives(self):
        for path, text in self.texts().items():
            self.assertNotRegex(text, r"43758|fract\(\s*(sin|cos)\(", path.name)

    def test_every_notice_is_in_notice_md(self):
        notice = (REPO / "NOTICE.md").read_text(encoding="utf-8")
        self.assertIn("Material Maker", notice)
        holders = set()
        for path in NODES.glob("*.json"):
            if not path.name.startswith("_"):
                notices = json.loads(path.read_text(encoding="utf-8"))["notices"]
                holders.update(n["holder"] for n in notices)
                for item in notices:
                    self.assertIn(f"`{item['node']}`", notice, path.name)
        self.assertTrue(holders)
        for holder in holders:
            self.assertIn(holder, notice)
        self.assertIn("BSD 2-Clause", notice)

    def test_the_interpreter_test_holds_the_real_hash(self):
        # IntegerHashGivesTheExactBits checks a copy: it must be this copy, or it tests nothing
        fixture = re.sub(r"\s+", " ", (Path(__file__).parent / "interpreter_fixtures.glsl").read_text(encoding="utf-8"))
        for function in ("jp_quantize", "jp_hash_mix", "jp_hash_float"):
            code = re.search(rf"\w+ {function}\(.*?\n}}\n", importer.HASH_FUNCTIONS, re.DOTALL).group(0)
            self.assertIn(re.sub(r"\s+", " ", code).strip(), fixture, function)

    def test_the_expander_fails_on_a_node_it_cannot_expand(self):
        expander_spec = importlib.util.spec_from_file_location("expand_nodes", Path(__file__).with_name("expand_nodes.py"))
        expander = importlib.util.module_from_spec(expander_spec)
        sys.modules["expand_nodes"] = expander
        expander_spec.loader.exec_module(expander)
        with tempfile.TemporaryDirectory() as scratch:
            nodes = Path(scratch) / "nodes"
            nodes.mkdir()
            for name in ("_functions.json", "_types.json"):
                (nodes / name).write_text((NODES / name).read_text(encoding="utf-8"), encoding="utf-8")
            broken = {"id": "broken", "kind": "shader", "functions": [], "instance": [], "code": [], "inputs": [], "parameters": [],
                      "outputs": [{"type": "f", "expr": "$nothing"}]}
            (nodes / "broken.json").write_text(json.dumps(broken), encoding="utf-8")
            self.assertEqual(expander.main(["--nodes", str(nodes), "--out", str(Path(scratch) / "out")]), 1)
            (nodes / "broken.json").unlink()
            self.assertEqual(expander.main(["--nodes", str(nodes), "--out", str(Path(scratch) / "out")]), 0)

    def test_the_cpp_expanders_reference_is_current(self):
        # ProcMatExpanderTest holds the C++ expander to this file: stale, it would hold it to an old library
        expander_spec = importlib.util.spec_from_file_location("expand_nodes", Path(__file__).with_name("expand_nodes.py"))
        expander = importlib.util.module_from_spec(expander_spec)
        sys.modules["expand_nodes"] = expander
        expander_spec.loader.exec_module(expander)
        reference = REPO / "editor/tests/procmat/ExpanderReference.txt"
        self.assertEqual(expander.main(["--nodes", str(NODES), "--check-reference", str(reference)]), 0)

    def test_pentagram_is_not_test_data(self):
        for path in Path(__file__).parent.rglob("*"):
            self.assertNotIn("pentagram", path.name)

    @unittest.skipUnless(os.environ.get("MM_CLONE"), "MM_CLONE names a clone of Material Maker")
    def test_output_matches_the_tool(self):
        self.assertEqual(importer.main(["--source", os.environ["MM_CLONE"], "--out", str(NODES), "--check"]), 0)


if __name__ == "__main__":
    unittest.main()
