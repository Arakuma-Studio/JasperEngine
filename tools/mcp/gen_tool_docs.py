"""Generate docs/mcp/TOOLS.md from the editor's tools/list.

    uv run --no-project --with mcp --with pyyaml python tools/mcp/gen_tool_docs.py
        [--out docs/mcp/TOOLS.md] [--project DIR | --fixture] [--url URL] [--fake]

Starts the editor through the harness (a new empty project by default, so no fixture is
needed; --project/--fixture to use a project copy instead), lists every tool and writes them
grouped by module with each tool's description and a parameter table. A tool's module is
taken from its `_meta` ("jasper/module" or "module") when the server sets it, else from the
editor/mcp/tools/*.cpp file that registers it.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from harness import REPO, Editor  # noqa: E402

# Section per registering source file (editor/mcp/tools/<File>.cpp), in this order
MODULES: list[tuple[str, str]] = [
    ("CoreTools", "Core"),
    ("ProjectTools", "Project"),
    ("SceneTools", "Scenes"),
    ("EntityTools", "GameObjects and selection"),
    ("ComponentTools", "Components and properties"),
    ("PrefabTools", "Prefabs"),
    ("PlayTools", "Play mode and input"),
    ("ViewTools", "Views and screenshots"),
    ("ConsoleTools", "Console"),
    ("ScriptTools", "Scripts"),
    ("AssetTools", "Assets, materials and shaders"),
    ("SynthesisTools", "Texture synthesis"),
    ("DocsTools", "Docs"),
    ("BuildTools", "Build and export"),
    ("WorldTools", "World: terrain, tilemaps, sprites, animation, particles, physics, UI, models"),
    ("PlacementTools", "Physics placement"),
    ("HtmlUiTools", "HTML/CSS UI"),
    ("UIEffectTools", "HTML/CSS UI"),
]
GROUPS = [title for _, title in MODULES]

# `tool.name = "x";` (also `tool.name = undo ? "undo" : "redo";`) and WorldTools' `add("x", ...)`
_NAME_LINE = re.compile(r'tool\.name\s*=([^;]*);')
_ADD_CALL = re.compile(r'\badd\(\s*"([a-z0-9_]+)"\s*,')


def source_modules() -> dict[str, str]:
    """Tool name -> section, from the tool registrations in editor/mcp/tools/*.cpp."""
    out: dict[str, str] = {}
    tools_dir = REPO / "editor" / "mcp" / "tools"
    for stem, title in MODULES:
        path = tools_dir / f"{stem}.cpp"
        if not path.exists():
            continue
        text = path.read_text(errors="replace")
        names = [n for line in _NAME_LINE.findall(text) for n in re.findall(r'"([a-z0-9_]+)"', line)]
        for name in names + _ADD_CALL.findall(text):
            out.setdefault(name, title)
    return out


SOURCE_MODULES: dict[str, str] = {}


def module_of(tool) -> str:
    meta = getattr(tool, "meta", None) or {}
    for key in ("jasper/module", "module"):
        if isinstance(meta, dict) and meta.get(key):
            return str(meta[key])
    return SOURCE_MODULES.get(tool.name, "Other")


def type_of(schema: dict) -> str:
    if not isinstance(schema, dict):
        return ""
    if "enum" in schema:
        return " | ".join(f"`{json.dumps(v)}`" for v in schema["enum"])
    if "const" in schema:
        return f"`{json.dumps(schema['const'])}`"
    for key in ("anyOf", "oneOf"):
        if key in schema:
            return " or ".join(filter(None, (type_of(s) for s in schema[key])))
    t = schema.get("type")
    if isinstance(t, list):
        return " or ".join(t)
    if t == "array":
        inner = type_of(schema.get("items", {}))
        n = ""
        if "minItems" in schema and schema.get("minItems") == schema.get("maxItems"):
            n = f"[{schema['minItems']}]"
        return f"array{n} of {inner}" if inner else "array"
    if t == "object" and schema.get("properties"):
        return "object {" + ", ".join(schema["properties"].keys()) + "}"
    if t in ("number", "integer") and ("minimum" in schema or "maximum" in schema):
        return f"{t} ({schema.get('minimum', '')}..{schema.get('maximum', '')})"
    if "$ref" in schema:
        return schema["$ref"].split("/")[-1]
    return t or ""


def cell(text: str) -> str:
    return (text or "").replace("|", "\\|").replace("\n", " ").strip()


def render_tool(t) -> str:
    out = [f"### `{t.name}`", ""]
    if getattr(t, "title", None):
        out += [f"*{t.title}*", ""]
    ann = getattr(t, "annotations", None)
    flags = []
    if ann:
        for attr, label in (("read_only_hint", "read-only"), ("destructive_hint", "destructive"),
                            ("idempotent_hint", "idempotent"), ("open_world_hint", "open-world")):
            if getattr(ann, attr, None):
                flags.append(label)
    if flags:
        out += ["Hints: " + ", ".join(flags), ""]
    out += [(t.description or "(no description)").strip(), ""]
    schema = t.input_schema or {}
    props = schema.get("properties") or {}
    required = set(schema.get("required") or [])
    if props:
        out += ["| Parameter | Type | Required | Description |", "|---|---|---|---|"]
        for name, ps in props.items():
            desc = ps.get("description", "") if isinstance(ps, dict) else ""
            if isinstance(ps, dict) and "default" in ps:
                desc = (desc + f" Default: `{json.dumps(ps['default'])}`.").strip()
            out.append(f"| `{name}` | {cell(type_of(ps))} | {'yes' if name in required else ''} | {cell(desc)} |")
        out.append("")
    else:
        out += ["No parameters.", ""]
    if getattr(t, "output_schema", None):
        keys = list((t.output_schema.get("properties") or {}).keys())
        if keys:
            out += ["Returns (structured): " + ", ".join(f"`{k}`" for k in keys), ""]
    return "\n".join(out)


def render(tools, server_info, version) -> str:
    groups: dict[str, list] = {}
    for t in sorted(tools, key=lambda t: t.name):
        groups.setdefault(module_of(t), []).append(t)
    order = GROUPS + ["Other"]
    names = sorted(groups, key=lambda g: order.index(g) if g in order else len(order))
    server = f"{server_info.name} {server_info.version}" if server_info else "unknown server"
    lines = [
        "# JasperEngine MCP tool reference",
        "",
        f"Generated by `tools/mcp/gen_tool_docs.py` from `tools/list` of {server} "
        f"(protocol {version}). Do not edit by hand: regenerate after changing tools.",
        "",
        f"{len(tools)} tools. See [README.md](README.md) for setup and the addressing rules "
        "(scene, entity, component, property, asset references).",
        "",
        "Conventions shared by every tool (explained once in the server instructions, not repeated "
        "per tool): `scene` is a scene id, name or project-relative .scene path, omitted = the selected "
        "scene (a Prefab Mode stage id works too); `entity` is a GameObject id, a path from the scene "
        "root (`Player/Gun`, `Enemy#2`) or a unique name; `entities` (array) applies a tool to several "
        "GameObjects; asset paths are project-relative. Booleans without a listed default are false. Unknown arguments "
        "are rejected with the list of valid ones. Every scene edit is one undo step.",
        "",
    ]
    for g in names:
        anchor = "".join(c for c in g.lower().replace(" ", "-") if c.isalnum() or c == "-")
        lines.append(f"- [{g}](#{anchor}) ({len(groups[g])}): " + ", ".join(f"`{t.name}`" for t in groups[g]))
    lines.append("")
    for g in names:
        lines += [f"## {g}", ""]
        for t in groups[g]:
            lines += [render_tool(t), ""]
    return "\n".join(lines).rstrip() + "\n"


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(REPO / "docs" / "mcp" / "TOOLS.md"))
    ap.add_argument("--project", help="copy of this project instead of a new empty one")
    ap.add_argument("--fixture", action="store_true", help="use the default fixture project")
    ap.add_argument("--url", help="read tools/list from a running editor over HTTP")
    ap.add_argument("--fake", action="store_true", help="use tools/mcp/fake_server.py (harness self-test)")
    args = ap.parse_args()
    SOURCE_MODULES.update(source_modules())

    if args.url:
        ed = Editor(url=args.url)
    elif args.project or args.fixture:
        ed = Editor(project=args.project, fake=args.fake or None)
    else:
        ed = Editor(create=True, name="ToolDocs", fake=args.fake or None)
    async with ed:
        tools = await ed.tools()
        info = ed.init_result.server_info if ed.init_result else None
        version = ed.init_result.protocol_version if ed.init_result else "?"
    Path(args.out).write_text(render(tools, info, version))
    print(f"wrote {args.out}: {len(tools)} tools")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
