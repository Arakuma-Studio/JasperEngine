#!/usr/bin/env python3
"""Writes engine/core/script/binding/stubs/ParticleSystemModules.lua: the LuaLS classes of the
Particle System module views (ps.main, ps.emission, ... ps.renderer; docs/specs/S20-particles.md,
Upstream absorption). The Lua binding (script/binding/ParticleClassesLua.cpp) answers their
fields from S20's module table (engine/core/particles/ParticleModules.cpp), which the stub
generator (tools/lua_api) cannot read, so this script reads that table and writes the fragment
the stub generator joins.

Run it after changing a module field: python3 tools/gen_particle_stubs.py (--check exits 1 when
the fragment is stale).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "engine" / "core" / "particles" / "ParticleModules.cpp"
OUT = ROOT / "engine" / "core" / "script" / "binding" / "stubs" / "ParticleSystemModules.lua"

# the proxies ParticleSystem has (ParticleClassesLua.cpp): Lua name -> the module key
PROXIES = [
    ("main", "main"), ("emission", "emission"), ("shape", "shape"), ("velocityOverLifetime", "velocityOverLifetime"),
    ("limitVelocityOverLifetime", "limitVelocity"), ("inheritVelocity", None), ("forceOverLifetime", "forceOverLifetime"),
    ("noise", "noise"), ("colorOverLifetime", "colorOverLifetime"), ("colorBySpeed", "colorBySpeed"),
    ("sizeOverLifetime", "sizeOverLifetime"), ("sizeBySpeed", "sizeBySpeed"), ("rotationOverLifetime", "rotationOverLifetime"),
    ("rotationBySpeed", "rotationBySpeed"), ("textureSheetAnimation", "textureSheetAnimation"), ("collision", "collision"),
    ("trails", "trails"), ("customData", "customData"), ("externalForces", "externalForces"), ("renderer", "renderer"),
    ("pixelArt", "pixelArt"),
]

TYPES = {
    "Bool": "boolean", "Int": "integer", "UInt": "integer", "Mask": "integer", "Float": "number", "Enum": "string",
    "Vector2": "Vector2", "Vector3": "Vector3", "Vector4": "Vector4", "Color": "Vector4", "String": "string",
    "MinMaxCurve": "MinMaxCurve|number|{min: number, max: number}", "MinMaxGradient": "ParticleSystemGradient|MinMaxGradient|Vector4|Gradient",
}

# the effect's own fields Unity puts in Main (ParticleClassesLua.cpp pathOf)
MAIN_EFFECT_FIELDS = ["duration", "looping", "prewarm", "startDelay", "maxParticles", "simulationSpace", "simulationSpeed",
                      "scalingMode", "useUnscaledTime", "stopAction", "emitterVelocityMode"]

# fields kept only so old files load, which nothing reads: not offered to scripts' authors
NOT_LISTED = {("trails", "generateLightingData")}

# Unity's names a field also answers to: (proxy, alias, field)
ALIASES = [
    ("main", "startSizeX", "startSize"), ("main", "startRotationZ", "startRotation"), ("noise", "strengthX", "strength"),
    ("sizeOverLifetime", "x", "size"), ("sizeOverLifetime", "y", "sizeY"), ("sizeOverLifetime", "z", "sizeZ"),
    ("sizeBySpeed", "x", "size"), ("sizeBySpeed", "y", "sizeY"), ("sizeBySpeed", "z", "sizeZ"),
    ("rotationOverLifetime", "x", "angularVelocityX"), ("rotationOverLifetime", "y", "angularVelocityY"),
    ("rotationOverLifetime", "z", "angularVelocity"), ("rotationBySpeed", "x", "angularVelocityX"),
    ("rotationBySpeed", "y", "angularVelocityY"), ("rotationBySpeed", "z", "angularVelocity"),
    ("limitVelocityOverLifetime", "limitX", "limit"), ("textureSheetAnimation", "numTilesX", "tilesX"),
    ("textureSheetAnimation", "numTilesY", "tilesY"), ("velocityOverLifetime", "orbitalOffsetX", "offsetX"),
    ("velocityOverLifetime", "orbitalOffsetY", "offsetY"), ("velocityOverLifetime", "orbitalOffsetZ", "offsetZ"),
]

FIELD = re.compile(r'(?<![A-Za-z_])(field|enumField|arrayField<[^>]*>)\("(\w+)", "[^"]*", (?:FieldType::(\w+))?')


def parse() -> tuple[dict, list]:
    """module key -> [(name, type)] in the table's order; and the document's own fields."""
    text = MODULES.read_text(encoding="utf-8")
    modules: dict = {}
    document: list = []
    for body in re.split(r"\n    (?=(?:ModuleDesc|StructDesc) make\w+\(\)\{)", text)[1:]:
        head = re.match(r"(ModuleDesc|StructDesc) make(\w+)\(\)\{", body)
        if not head:
            continue
        body = body.split("\n    }\n", 1)[0]
        key = re.search(r'module<\w+>\(ModuleId::\w+, "(\w+)"', body) or re.search(r'd\.key = "(\w+)";', body)
        fields = []
        for m in FIELD.finditer(body):
            kind, name, ftype = m.groups()
            fields.append((name, "Enum" if kind == "enumField" else ("Array" if kind.startswith("arrayField") else ftype)))
        if head.group(2) == "Document":
            document = fields
        elif key:
            modules[key.group(1)] = fields
    return modules, document


def lua_type(kind: str) -> str:
    return TYPES.get(kind, "any")


def class_name(proxy: str) -> str:
    return "ParticleSystem" + proxy[0].upper() + proxy[1:] + "Module"


def main() -> int:
    modules, document = parse()
    if not modules or not document:
        sys.exit(f"gen_particle_stubs: no module table in {MODULES}")
    by_name = dict(document)
    lines = ["---@meta _", "",
             "-- Generated by tools/gen_particle_stubs.py from engine/core/particles/ParticleModules.cpp. Do not edit.", ""]
    for proxy, key in PROXIES:
        lines.append(f"---The {proxy} module of a Particle System (ps.{proxy}): a live view of its effect (an inline one;")
        lines.append("---an asset is copied inline first), its fields read and write it now with Unity's names (angles in")
        lines.append("---radians, as Unity's).")
        lines.append(f"---@class {class_name(proxy)}: ParticleSystemModule")
        if proxy == "inheritVelocity":
            fields = [("curve", "MinMaxCurve"), ("mode", "Enum")]
        else:
            fields = [f for f in modules.get(key, []) if f[1] != "Array" and (key, f[0]) not in NOT_LISTED]
            if key != "main" and key != "renderer":
                lines.append("---@field enabled boolean")
        if proxy == "main":
            fields = fields + [(n, by_name[n]) for n in MAIN_EFFECT_FIELDS if n in by_name] + [("randomSeed", "UInt")]
        known = dict(fields)
        for name, kind in fields:
            lines.append(f"---@field {name} {lua_type(kind)}")
            if kind == "MinMaxCurve":
                lines.append(f"---@field {name}Multiplier number")
        for alias_proxy, alias, target in ALIASES:
            if alias_proxy != proxy or target not in known:
                continue
            lines.append(f"---@field {alias} {lua_type(known[target])}")
            if known[target] == "MinMaxCurve":
                lines.append(f"---@field {alias}Multiplier number")
        if proxy == "emission":
            lines.append("---@field burstCount integer")
        lines.append("")
    # the ParticleSystem class's module fields are its binding stub's (ParticleSystem.lua: the lua_api
    # parser's PROPERTY_OVERRIDES type them as these classes); declared here too, LuaLS reports
    # every one as a duplicate-doc-field
    text = "\n".join(lines).rstrip("\n") + "\n"
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"{OUT} is stale: run tools/gen_particle_stubs.py")
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
