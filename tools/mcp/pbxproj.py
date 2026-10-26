"""The exported Xcode workspace's project.pbxproj for the MCP tests: an old-style (OpenStep)
property list parser (no dependencies, no Xcode needed), the objects that name no object, and
lookups by target name. Used by test_build.py.
"""

from __future__ import annotations

import re

_TOKEN = re.compile(r'\s*(?:(//[^\n]*)|(/\*.*?\*/)|"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_$./:+-]+)|([{}()=;,]))', re.S)
OBJECT_ID = re.compile(r"[0-9A-F]{24}")


def parse(text: str) -> dict:
    """The root dictionary; ValueError on anything Xcode would not read."""
    tokens: list[tuple[str, str]] = []
    pos = 0
    while True:
        m = _TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip():
                raise ValueError(f"bad token on line {text.count(chr(10), 0, pos) + 1}: {text[pos:pos + 40]!r}")
            break
        pos = m.end()
        if m.group(1) is not None or m.group(2) is not None:
            continue
        if m.group(3) is not None:
            tokens.append(("s", m.group(3)))
        elif m.group(4) is not None:
            tokens.append(("s", m.group(4)))
        else:
            tokens.append(("p", m.group(5)))
    i = 0

    def expect(punct: str, what: str) -> None:
        nonlocal i
        if i >= len(tokens) or tokens[i] != ("p", punct):
            raise ValueError(f"missing {punct!r} {what}")
        i += 1

    def value():
        nonlocal i
        if i >= len(tokens):
            raise ValueError("unexpected end")
        kind, tok = tokens[i]
        i += 1
        if kind == "s":
            return tok
        if tok == "{":
            d = {}
            while i < len(tokens) and tokens[i] != ("p", "}"):
                key = value()
                if not isinstance(key, str):
                    raise ValueError(f"bad key {key!r}")
                if key in d:
                    raise ValueError(f"duplicate key {key!r}")
                expect("=", f"after {key!r}")
                d[key] = value()
                expect(";", f"after the value of {key!r}")
            expect("}", "closing a dictionary")
            return d
        if tok == "(":
            items = []
            while i < len(tokens) and tokens[i] != ("p", ")"):
                items.append(value())
                if i < len(tokens) and tokens[i] == ("p", ","):
                    i += 1
                elif i >= len(tokens) or tokens[i] != ("p", ")"):
                    raise ValueError("missing ',' in a list")
            expect(")", "closing a list")
            return items
        raise ValueError(f"unexpected {tok!r}")

    root = value()
    if i != len(tokens):
        raise ValueError("text after the root dictionary")
    if not isinstance(root, dict) or not isinstance(root.get("objects"), dict):
        raise ValueError("no objects dictionary")
    return root


def dangling_references(root: dict) -> list[tuple[str, str, str]]:
    """(object, key, id) of every reference to an object the file does not define."""
    objects = root["objects"]
    missing = []
    for oid, obj in objects.items():
        for key in ("children", "files", "buildPhases", "dependencies", "buildConfigurations", "targets"):
            refs = obj.get(key)
            for ref in refs if isinstance(refs, list) else []:
                if ref not in objects:
                    missing.append((oid, key, ref))
        for key in ("fileRef", "buildConfigurationList", "target", "targetProxy", "mainGroup", "productRefGroup"):
            ref = obj.get(key)
            if isinstance(ref, str) and OBJECT_ID.fullmatch(ref) and ref not in objects:
                missing.append((oid, key, ref))
    return missing


def target(root: dict, name: str) -> dict:
    """The PBXNativeTarget called name (KeyError when there is none)."""
    for obj in root["objects"].values():
        if obj.get("isa") == "PBXNativeTarget" and obj.get("name") == name:
            return obj
    raise KeyError(name)


def target_sources(root: dict, name: str) -> list[str]:
    """Paths (as their file references hold them) the target's Sources phase compiles."""
    objects = root["objects"]
    paths = []
    for phase_id in target(root, name)["buildPhases"]:
        phase = objects[phase_id]
        if phase.get("isa") != "PBXSourcesBuildPhase":
            continue
        for build_id in phase.get("files", []):
            ref = objects[objects[build_id]["fileRef"]]
            paths.append(ref.get("path", ""))
    return paths


def target_settings(root: dict, name: str) -> list[dict]:
    """The buildSettings of each configuration of the target."""
    objects = root["objects"]
    configurations = objects[target(root, name)["buildConfigurationList"]]["buildConfigurations"]
    return [objects[c]["buildSettings"] for c in configurations]


def project_settings(root: dict) -> list[dict]:
    """The buildSettings of each project-level configuration."""
    objects = root["objects"]
    project = objects[root["rootObject"]]
    return [objects[c]["buildSettings"] for c in objects[project["buildConfigurationList"]]["buildConfigurations"]]


def group_paths(root: dict, group_path: str) -> dict[str, str]:
    """path -> isa of everything under the group whose path is group_path, as nested paths ("a/b.cpp")."""
    objects = root["objects"]
    group = next(obj for obj in objects.values() if obj.get("isa") == "PBXGroup" and obj.get("path") == group_path)
    found: dict[str, str] = {}

    def walk(g: dict, prefix: str) -> None:
        for child_id in g.get("children", []):
            child = objects[child_id]
            path = prefix + child.get("path", child.get("name", ""))
            found[path] = child["isa"]
            if child["isa"] == "PBXGroup":
                walk(child, path + "/")

    walk(group, "")
    return found
