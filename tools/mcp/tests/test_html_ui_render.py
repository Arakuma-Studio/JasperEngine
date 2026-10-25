"""HTML UI shaders exported through MCP for every supported graphics backend.

The renderer's documented gallery is owned by TestRenderP1Golden; the editor Scene-view
pipeline is owned by test_html_ui_editor. This test verifies the export-facing contract.
"""

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

TIMEOUT = 1200


async def test_shaders_cross_compile(ed: Editor):
    """Every HTML UI variant for every graphics API generate_shaders accepts."""
    # by spec, not by ShaderKey number (the ShaderType values of the HTML UI's types moved by one
    # when origin/main's BLOOM came before them)
    posts = ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv")
    specs = ["htmlui", "htmlui:Tex", "htmluigradient"] + [f"htmluipost:{name}" for name in posts]
    out = ed.scratch.root / "htmlui-shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends="all", format="binary",
                      shaders=specs, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"generate_shaders: {r}")
    files = [p.name for p in out.rglob("*") if p.is_file()]
    # every variant for GLSL 410, GLSL 300 es, HLSL 5, MSL 2.1 (macOS, iOS) and SPIR-V
    languages = ("glsl410", "glsl300es", "hlsl5", "msl21macos", "msl21ios", "spirv10")
    stems = ["htmlui", "htmlui_Tex", "htmluigradient"] + [f"htmluipost_{name}" for name in ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv")]
    for stem in stems:
        for language in languages:
            ed.check(f"{stem}_{language}.sdat" in files, f"{stem}_{language}.sdat not generated: {sorted(files)}")
