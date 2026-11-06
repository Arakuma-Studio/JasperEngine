#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Unit tests of tools/check_shader_lod.py (python3 tools/ci/test_check_shader_lod.py).

Small shader sources for each rule, plus the real engine shader library as the end-to-end case.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_shader_lod", HERE.parent / "check_shader_lod.py")
check_shader_lod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check_shader_lod
spec.loader.exec_module(check_shader_lod)


def scan(**files):
    return check_shader_lod.scan({f"{name}.glsl": text for name, text in files.items()})


class CheckShaderLod(unittest.TestCase):
    def test_a_direct_sample_after_a_continue_is_flagged(self):
        findings = scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += texture(sampler2D(u_tex, u_smp), v_uv).r;
    }
    return sum;
}
""")
        self.assertEqual(len(findings), 1)
        self.assertIn("a.glsl:6: texture()", findings[0])

    def test_a_sample_through_a_function_is_flagged(self):
        findings = scan(lib="""
float lookup(vec2 uv){
    return texture(sampler2D(u_tex, u_smp), uv).r;
}
""", main="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += lookup(v_uv);
    }
    return sum;
}
""")
        self.assertEqual(len(findings), 1)
        self.assertIn("main.glsl:6: lookup()", findings[0])

    def test_explicit_lod_and_a_marked_line_pass(self):
        self.assertEqual(scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += textureLod(sampler2D(u_tex, u_smp), v_uv, 0.0).r;
        sum += dFdx(v_uv).x; // lod-safe: the test's marked line
    }
    return sum;
}
"""), [])

    def test_a_marked_function_definition_passes_its_callers(self):
        self.assertEqual(scan(a="""
float lookup(vec2 uv){ // lod-safe: its derivatives are passed in
    return texture(sampler2D(u_tex, u_smp), uv).r;
}
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += lookup(v_uv);
    }
    return sum;
}
"""), [])

    def test_a_uniform_loop_passes(self):
        self.assertEqual(scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (i >= u_count) break;
        sum += texture(sampler2D(u_tex, u_smp), v_uv + vec2(float(i))).r;
    }
    return sum;
}
"""), [])

    def test_a_commented_call_is_not_one(self):
        self.assertEqual(scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        // sum += texture(sampler2D(u_tex, u_smp), v_uv).r;
    }
    return sum;
}
"""), [])

    def test_offset_and_projective_samples_are_flagged(self):
        findings = scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += textureOffset(sampler2D(u_tex, u_smp), v_uv, ivec2(1, 0)).r;
        sum += textureProj(sampler2D(u_tex, u_smp), v_proj).r;
    }
    return sum;
}
""")
        self.assertEqual(len(findings), 2)
        self.assertIn("a.glsl:6: textureOffset()", findings[0])
        self.assertIn("a.glsl:7: textureProj()", findings[1])

    def test_a_sample_through_a_macro_is_flagged(self):
        findings = scan(lib="""
#define HEX_NORMAL(tex, smp, uv) \\
    texture(sampler2D(tex, smp), uv).rgb
#define FLAT_DX dFdx(v_position)
""", main="""
vec3 shade(){
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i)) continue;
        sum += HEX_NORMAL(u_tex, u_smp, v_uv);
        sum += FLAT_DX;
    }
    return sum;
}
""")
        self.assertEqual(len(findings), 2)
        self.assertIn("main.glsl:6: HEX_NORMAL()", findings[0])
        self.assertIn("main.glsl:7: FLAT_DX()", findings[1])

    def test_a_loop_without_braces_is_flagged(self):
        findings = scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++)
        sum += texture(sampler2D(u_tex, u_smp), v_uv + vec2(float(i))).r;
    for (int i = 0; i < 4; i++)
        if (v_value.x > float(i)) continue; else sum += dFdx(v_uv).x;
    return sum;
}
""")
        self.assertEqual(len(findings), 1)
        self.assertIn("a.glsl:7: dFdx()", findings[0])

    def test_a_per_fragment_branch_in_a_loop_is_flagged(self):
        findings = scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (v_value.x > float(i))
            sum += texture(sampler2D(u_tex, u_smp), v_uv).r;
        else {
            sum += fwidth(v_uv.x);
        }
    }
    for (int i = 0; i < 4; i++){
        if (gl_FragCoord.x > float(i)) break;
        sum += texture(sampler2D(u_tex, u_smp), v_uv).r;
    }
    return sum;
}
""")
        self.assertEqual(len(findings), 3)
        self.assertIn("a.glsl:6: texture()", findings[0])
        self.assertIn("a.glsl:8: fwidth()", findings[1])
        self.assertIn("a.glsl:13: texture()", findings[2])

    def test_a_uniform_branch_in_a_loop_passes(self):
        self.assertEqual(scan(a="""
float shade(){
    float sum = 0.0;
    for (int i = 0; i < 4; i++){
        if (i < u_count)
            sum += texture(sampler2D(u_tex, u_smp), v_uv).r;
    }
    return sum;
}
"""), [])

    def test_the_engine_shader_library_passes(self):
        self.assertEqual(check_shader_lod.main([]), 0)


if __name__ == "__main__":
    unittest.main()
