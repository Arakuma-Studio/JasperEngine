"""What a project ships for noise (T2 R43-R45; AC35)."""

from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Noise Export Test"}
TIMEOUT = 600


async def test_detection_decides(ed: Editor):
    """A .noisegraph alone needs no noise module; a script naming Noise does."""
    root = Path(ed.project)
    (root / "graphs").mkdir(exist_ok=True)
    (root / "graphs" / "Baked.noisegraph").write_text('{"formatVersion":1,"kind":"noise","kindVersion":1,"nextId":1,"nodes":[],"edges":[]}')
    usage = await ed.call("get_module_usage") if "get_module_usage" in [t.name for t in await ed.tools()] else None
    if usage is not None:
        ed.check("noise" not in usage.get("needed", usage.get("modules", [])), f"a graph file is no evidence: {usage}")
    (root / "Uses.lua").write_text("local v = Noise.perlin(1, 2)\nprint(v)\n")
    if usage is not None:
        used = await ed.call("get_module_usage")
        ed.check("noise" in used.get("needed", used.get("modules", [])), f"a script is: {used}")
