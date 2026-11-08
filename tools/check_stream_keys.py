#!/usr/bin/env python3
# Keeps the hand-kept known-key tables honest (docs/data-safety.md, S03 AC12): every key a
# hand-written decoder in editor/Stream*.cpp reads from its YAML node must be listed for its
# component (the extraKeys of the component's reflection table, editor/reflect/components: a
# component with a codec of its own lists the keys it reads there) or nested structure
# (knownNestedKeys of editor/StreamSchema.cpp). Components the generic codec decodes need no
# list: their table is their decoder (docs/specs/S08-reflection.md).
# A key a decoder reads but the table lacks would make a valid file report "unknown field"
# (noise in the load report, and Dropped for nested structures, which gates saves).
#
#   python3 tools/check_stream_keys.py            exit 1 and a list when a key is missing
#   python3 tools/check_stream_keys.py --print    the keys each decoder reads, as C++ initializers
#
# A decoder's keys: every node["literal"] on its YAML parameter, the literals passed to a
# lambda of its body that indexes that parameter (given("renderMode")), and the keys of the
# helpers it hands the parameter to (decodeComponentShaderLibrary(node, ...)).

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [os.path.join(ROOT, "editor", name) for name in ("Stream.cpp", "StreamComponents.cpp", "StreamPrefab.cpp", "StreamNavigation.cpp", "StreamUpstream.cpp")]
SCHEMA = os.path.join(ROOT, "editor", "StreamSchema.cpp")
REFLECT = os.path.join(ROOT, "editor", "reflect", "components")

# Nested structures with a decoder of their own, checked inside components (knownNestedKeys)
NESTED = {
    "decodeSubmesh": "Submesh",
    "decodeMaterial": "Material",
    "decodeMaterialPass": "MaterialPass",
    "decodeStencil": "Stencil",
    "decodeTexture": "Texture",
}

# Settings structures: their decoders report no unknown keys, so StreamSchema.cpp has no table for them. Listed
# here so that a new key is a conscious one, with a default (T2 R60, R70).
SETTINGS = {
    "decodeTerrainEditorSettings": ("TerrainEditorSettings", {
        "brushMode", "brushShape", "brushFalloff", "brushSize", "brushStrength", "brushRotation", "brushMaskPath",
        "flattenHeight", "terraceSteps", "noiseSize", "heightMapResolution", "blendMapResolution", "densityMapResolution",
        "normalizeBlendPaint", "heightMapStartAtMiddle", "flattenPickOnStroke", "paintUseMask", "paintMinSlope", "paintMaxSlope",
        "paintMinHeight", "paintMaxHeight", "placeAssetPath", "placeInstanced", "placeSpacing", "placeMinScale", "placeMaxScale",
        "placeRotationJitter", "placeAlignToNormal", "generateGraph", "generateSeed",
        "filterAllTools", "paintRules", "paintRulesSeed", "slopeFilterInvert", "heightFilterInvert", "placeAs",
    }),
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


def component_tables():
    """ComponentType -> (decoder name, known keys) of each component with a codec of its own, from
    its ComponentDesc (.codec and .extraKeys) in editor/reflect/components"""
    found = {}
    for name in sorted(os.listdir(REFLECT)):
        if not name.endswith(".cpp"):
            continue
        text = open(os.path.join(REFLECT, name), encoding="utf-8").read()
        arrays = {m.group(1): set(re.findall(r'"([^"]+)"', m.group(2)))
                  for m in re.finditer(r"constexpr const char\*\s+(\w+)\[\]\s*=\s*\{([^}]*)\}", text)}
        for block in re.finditer(r"const ComponentDesc k\w+Desc\{(.*?)\n\};", text, re.S):
            body = block.group(1)
            component = re.search(r"\.type = ComponentType::(\w+)", body).group(1)
            if ".codec = " not in body:
                continue
            extra = re.search(r"\.extraKeys = (\w+)", body)
            decoder = "decodeTransform" if component == "Transform" else "decode" + component
            found[component] = (decoder, arrays.get(extra.group(1), set()) if extra else set())
    return found


def schema_tables(text):
    tables = {}
    for match in re.finditer(r"\{\s*(ComponentType::\w+|NestedKind::\w+)\s*,\s*\{([^}]*)\}\s*\}", text):
        tables[match.group(1).split("::")[1]] = set(re.findall(r'"([^"]+)"', match.group(2)))
    return tables


def main():
    text = "\n".join(open(path, encoding="utf-8").read() for path in SOURCES)
    decoders = functions(text)
    keys = decoder_keys(decoders)
    components = component_tables()
    if not components:
        print("check_stream_keys: no component with a codec of its own in editor/reflect/components", file=sys.stderr)
        return 2

    wanted = {component: keys[decoder] for component, (decoder, _) in components.items() if decoder in keys}
    for component, (decoder, _) in components.items():
        if decoder not in keys:
            print("check_stream_keys: %s: no decoder %s in editor/Stream*.cpp" % (component, decoder), file=sys.stderr)
            return 2
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
    for component, (_, known) in components.items():
        tables[component] = known
    for decoder, (name, known) in SETTINGS.items():
        if decoder in keys:
            wanted[name] = keys[decoder]
            tables[name] = known
    problems = []
    for name in sorted(wanted):
        table = tables.get(name)
        if table is None:
            problems.append("%s: no known-key table" % name)
            continue
        missing = sorted(wanted[name] - table)
        if missing:
            problems.append("%s: read by its decoder but not in the table: %s" % (name, ", ".join(missing)))
    for problem in problems:
        print("check_stream_keys: " + problem, file=sys.stderr)
    if problems:
        print("Add the keys to the component's extraKeys (editor/reflect/components) or to knownNestedKeys "
              "(editor/StreamSchema.cpp).", file=sys.stderr)
        return 1
    print("check_stream_keys: %d tables cover every key their decoders read" % len(wanted))
    return 0


if __name__ == "__main__":
    sys.exit(main())
