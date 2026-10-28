#!/usr/bin/env python3
# Keeps the hand-kept known-key tables of editor/StreamSchema.cpp honest (docs/data-safety.md,
# S03 AC12): every key a decoder in editor/Stream*.cpp reads from its YAML node must be listed
# for that decoder's component (knownFieldKeys) or nested structure (knownNestedKeys).
# A key a decoder reads but the table lacks would make a valid file report "unknown field"
# (noise in the load report, and Dropped for nested structures, which gates saves).
#
#   python3 tools/check_stream_keys.py            exit 1 and a list when a key is missing
#   python3 tools/check_stream_keys.py --print    the keys each decoder reads, as C++ initializers
#
# A decoder's keys: every node["literal"] on its YAML parameter, the literals passed to a
# lambda of its body that indexes that parameter (given("renderMode")), and the keys of the
# helpers it hands the parameter to (decodeComponentShaderLibrary(node, ...)). S08 replaces the
# tables with reflection.

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [os.path.join(ROOT, "editor", "Stream.cpp"), os.path.join(ROOT, "editor", "StreamPrefab.cpp"),
           os.path.join(ROOT, "editor", "StreamNavigation.cpp")]
SCHEMA = os.path.join(ROOT, "editor", "StreamSchema.cpp")

# Nested structures with a decoder of their own, checked inside components (knownNestedKeys)
NESTED = {
    "decodeSubmesh": "Submesh",
    "decodeMaterial": "Material",
    "decodeMaterialPass": "MaterialPass",
    "decodeStencil": "Stencil",
    "decodeTexture": "Texture",
}


def functions(text):
    """name -> (parameter name, body) for each `... decodeX(const YAML::Node& param, ...) {` definition"""
    found = {}
    pattern = re.compile(r"^[^\n;]*?\b(?:editor::)?(?:Stream::)?(decode\w+)\(\s*const YAML::Node&\s*(\w+)[^)]*\)\s*\{", re.M)
    for match in pattern.finditer(text):
        name, param = match.group(1), match.group(2)
        start = match.end() - 1
        depth = 0
        for index in range(start, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    found.setdefault(name, (param, text[start:index + 1]))
                    break
    return found


def direct_keys(param, body):
    keys = set(re.findall(r"\b" + re.escape(param) + r'\["([A-Za-z0-9_]+)"\]', body))
    # lambdas indexing the parameter with their argument: given("key")
    for lam in re.finditer(r"auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(\s*const char\s*\*\s*(\w+)\s*\)\s*\{(.*?)\n\s*\};", body, re.S):
        lname, arg, lbody = lam.groups()
        if re.search(r"\b" + re.escape(param) + r"\[\s*" + re.escape(arg) + r"\s*\]", lbody):
            keys |= set(re.findall(r"\b" + re.escape(lname) + r'\("([A-Za-z0-9_]+)"\)', body))
    return keys


def helper_calls(param, body, decoders):
    return {name for name in re.findall(r"\b(decode\w+)\(\s*" + re.escape(param) + r"\s*[,)]", body) if name in decoders}


def decoder_keys(decoders):
    cache = {}

    def keys_of(name, stack=()):
        if name in cache:
            return cache[name]
        param, body = decoders[name]
        keys = direct_keys(param, body)
        for helper in helper_calls(param, body, decoders):
            if helper not in stack and helper != name:
                keys |= keys_of(helper, stack + (name,))
        cache[name] = keys
        return keys

    return {name: keys_of(name) for name in decoders}


def component_decoders(text):
    """ComponentType -> decoder name, from the blocks of decodeComponents"""
    mapping = {}
    for block in re.finditer(r"StreamLoad::decodeComponent\(ComponentType::(\w+),(.*?)\n    \}\);", text, re.S):
        call = re.search(r"\b(decode\w+Component|decodeTransform)\(componentNode", block.group(2))
        if call:
            mapping[block.group(1)] = call.group(1)
    return mapping


def schema_tables(text):
    tables = {}
    for match in re.finditer(r"\{\s*(ComponentType::\w+|NestedKind::\w+)\s*,\s*\{([^}]*)\}\s*\}", text):
        tables[match.group(1).split("::")[1]] = set(re.findall(r'"([^"]+)"', match.group(2)))
    return tables


def main():
    text = "\n".join(open(path, encoding="utf-8").read() for path in SOURCES)
    decoders = functions(text)
    keys = decoder_keys(decoders)
    components = component_decoders(text)
    if not components:
        print("check_stream_keys: no component blocks found in decodeComponents", file=sys.stderr)
        return 2

    wanted = {component: keys[decoder] for component, decoder in components.items()}
    for decoder, kind in NESTED.items():
        if decoder in keys:
            wanted[kind] = keys[decoder]

    if "--print" in sys.argv:
        for name in sorted(wanted):
            prefix = "NestedKind" if name in NESTED.values() else "ComponentType"
            listed = ", ".join('"%s"' % key for key in sorted(wanted[name]))
            print("        {%s::%s, {%s}}," % (prefix, name, listed))
        return 0

    tables = schema_tables(open(SCHEMA, encoding="utf-8").read())
    problems = []
    for name in sorted(wanted):
        table = tables.get(name)
        if table is None:
            problems.append("%s: no known-key table in editor/StreamSchema.cpp" % name)
            continue
        missing = sorted(wanted[name] - table)
        if missing:
            problems.append("%s: read by its decoder but not in the table: %s" % (name, ", ".join(missing)))
    for problem in problems:
        print("check_stream_keys: " + problem, file=sys.stderr)
    if problems:
        print("Add the keys to editor/StreamSchema.cpp (knownFieldKeys / knownNestedKeys).", file=sys.stderr)
        return 1
    print("check_stream_keys: %d tables cover every key their decoders read" % len(wanted))
    return 0


if __name__ == "__main__":
    sys.exit(main())
