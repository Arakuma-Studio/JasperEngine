#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# The GPU database of the device tiers (docs/device-tiers.md, spec S56 R1): The Forge's gpu.data
# tables, converted to engine/core/device/gpu_tiers.json and re-mapped to JasperEngine's tiers,
# plus our own rules. The engine embeds the JSON (GpuTiersData.cpp, written from it here).
#
#   python3 -I tools/device/convert_forge_gpu_data.py --forge /path/to/The-Forge   # convert, write both
#   python3 -I tools/device/convert_forge_gpu_data.py --embed                      # the JSON edited by hand: rewrite the .cpp
#   python3 -I tools/device/convert_forge_gpu_data.py --check [--forge ...]        # exit 1 when a file is stale
#
# The Forge's files are data (Apache-2.0, The Forge Interactive Inc.): this script only reads them
# as text, it never runs anything of theirs. The data is copied, the parser is our own.
#
# What the Forge rows become:
# - Their presets are calibrated for their visibility-buffer renderer: they are an order, not our
#   tiers. REMAP turns each platform's presets into Low/Medium/High/Ultra; OVERRIDES then moves
#   single GPUs where our forward renderer disagrees (each with its reason).
# - Android's ids are Vulkan ids, which JasperEngine's GLES3 never sees: rules key on the renderer
#   name (GL_RENDERER, MTLDevice.name, the DXGI or Vulkan device name, the WebGL unmasked renderer).
#   The PCI ids are kept: a WebGL renderer string from ANGLE carries one ("(0x00002487)").
# - Rows with an UNDEFINED preset are left out; one name with several presets keeps the lowest.
#
# Names are matched normalized (normalize() below, GpuTierDatabase::normalize in C++): lower case,
# "(tm)" and "(r)" dropped, every other character than a-z and 0-9 a space, spaces collapsed. A name
# rule matches when the renderer contains it between word boundaries (the longest one wins); a
# family rule's pattern is normalized text with \d (a digit), [...] (a class with ranges) and . (any
# character), matched from a word start to a word end.

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JSON_PATH = os.path.join(ROOT, "engine", "core", "device", "gpu_tiers.json")
CPP_PATH = os.path.join(ROOT, "engine", "core", "device", "GpuTiersData.cpp")

FORGE_FILES = [
    ("android", "Common_3/OS/Android/android_gpu.data"),
    ("apple", "Common_3/OS/Darwin/apple_gpu.data"),
    ("pc", "Common_3/OS/Windows/pc_gpu.data"),
    # the Steam Deck's one row: a Linux PC
    ("pc", "Common_3/OS/Linux/steamdeck_gpu.data"),
]
FORGE_SOURCE = ("The Forge (https://github.com/ConfettiFX/The-Forge, Apache-2.0, commit cd50468): "
                "Common_3/OS/{Android/android_gpu.data,Darwin/apple_gpu.data,Windows/pc_gpu.data,Linux/steamdeck_gpu.data}")

TIERS = ["low", "medium", "high", "ultra"]

# Forge preset -> our tier, per platform. Android phones in their table top out at "medium" (an
# Adreno 740): the order is kept and shifted up; Apple and PC keep their top end.
REMAP = {
    "android": {"office": "low", "verylow": "low", "low": "medium", "medium": "high", "high": "high", "ultra": "ultra"},
    "apple": {"office": "low", "verylow": "low", "low": "medium", "medium": "high", "high": "high", "ultra": "ultra"},
    "pc": {"office": "low", "verylow": "low", "low": "medium", "medium": "high", "high": "high", "ultra": "ultra"},
}

# (platform, normalized name) -> (tier, why): single GPUs our renderer rates differently
OVERRIDES = {
    ("android", "adreno 630"): ("medium", "a 2018 flagship (Snapdragon 845): runs the forward renderer at 0.85"),
    ("android", "adreno 640"): ("medium", "a 2019 flagship (Snapdragon 855)"),
    ("android", "adreno 642"): ("medium", "Snapdragon 778G class"),
    ("android", "adreno 642l"): ("medium", "Snapdragon 778G class"),
    ("android", "adreno 643"): ("medium", "Snapdragon 778G+ class"),
    ("android", "adreno 644"): ("medium", "Snapdragon 7 Gen 1 class"),
    ("android", "adreno 710"): ("medium", "Snapdragon 7 Gen 1 class"),
    ("apple", "apple a14 gpu"): ("high", "iPhone 12: Metal forward at full scale"),
    ("apple", "apple a15 gpu"): ("high", "iPhone 13"),
    ("apple", "apple m1"): ("high", "a desktop-class Mac or iPad Pro"),
    ("apple", "apple m1 gpu"): ("high", "a desktop-class Mac or iPad Pro"),
    ("apple", "apple m1 pro"): ("high", "a desktop-class Mac"),
    ("apple", "apple m1 max"): ("high", "a desktop-class Mac"),
}

# Our own rules, after the converted names: families (in this order, the first match wins), then a
# vendor's default. "any" applies on every platform.
FAMILIES = [
    # software rasterizers: always Low
    ("any", "software", "swiftshader", "low"),
    ("any", "software", "llvmpipe", "low"),
    ("any", "software", "lavapipe", "low"),
    ("any", "software", "softpipe", "low"),
    ("any", "software", "microsoft basic render", "low"),
    # Qualcomm Adreno, by generation and bin
    ("any", "qualcomm", r"adreno [2-4]\d\d", "low"),
    ("any", "qualcomm", r"adreno 5\d\d", "low"),
    ("any", "qualcomm", r"adreno 6[0-2]\d", "low"),
    ("any", "qualcomm", r"adreno 6[3-9]\d", "medium"),
    ("any", "qualcomm", r"adreno 7[0-2]\d", "medium"),
    ("any", "qualcomm", r"adreno 7[3-9]\d", "high"),
    ("any", "qualcomm", r"adreno [89]\d\d", "high"),
    # ARM Mali and Immortalis
    ("any", "arm", r"immortalis", "high"),
    ("any", "arm", r"mali [t4]\d\d\d", "low"),
    ("any", "arm", r"mali [t4]\d\d", "low"),
    ("any", "arm", r"mali g[3-5]\d", "low"),
    ("any", "arm", r"mali g[3-5]\d\d", "low"),
    ("any", "arm", r"mali g7[1-6]", "low"),
    ("any", "arm", r"mali g7[7-9]", "medium"),
    ("any", "arm", r"mali g6\d", "medium"),
    ("any", "arm", r"mali g6\d\d", "medium"),
    ("any", "arm", r"mali g7[01]0", "medium"),
    ("any", "arm", r"mali g7[1-9]\d", "high"),
    ("any", "arm", r"mali g[89]\d\d", "high"),
    ("any", "arm", r"mali g1", "high"),
    # Imagination, Samsung, Huawei
    ("any", "imgtec", r"powervr", "low"),
    ("any", "samsung", r"xclipse", "high"),
    ("any", "huawei", r"maleoon", "medium"),
    # Apple silicon newer than the table
    ("any", "apple", r"apple a1[0-1]", "low"),
    ("any", "apple", r"apple a1[2-3]", "medium"),
    ("any", "apple", r"apple a1[4-9]", "high"),
    ("any", "apple", r"apple a[2-9]\d", "high"),
    ("any", "apple", r"apple m\d", "high"),
    # desktop families the table does not name
    ("any", "nvidia", r"geforce rtx [2-9]0[89]0", "ultra"),
    ("any", "nvidia", r"geforce rtx", "high"),
    ("any", "nvidia", r"geforce gtx 1[06][5-8]0", "high"),
    ("any", "nvidia", r"geforce gtx", "medium"),
    ("any", "amd", r"radeon rx [5-9]\d\d\d", "high"),
    ("any", "amd", r"radeon rx", "medium"),
    ("any", "amd", r"radeon \d\d\dm", "medium"),
    ("any", "amd", r"radeon graphics", "medium"),
    ("any", "amd", r"radeon vega", "medium"),
    ("any", "intel", r"intel arc", "high"),
    ("any", "intel", r"intel iris xe", "medium"),
    ("any", "intel", r"intel uhd graphics", "medium"),
    ("any", "intel", r"intel hd graphics", "low"),
]

VENDOR_DEFAULTS = [
    ("pc", "nvidia", "high"),
    ("pc", "amd", "high"),
    ("pc", "intel", "medium"),
    ("apple", "apple", "high"),
    ("apple", "amd", "high"),
    ("apple", "intel", "medium"),
]

# the vendors: their PCI id and the words that name them in a renderer string
VENDORS = [
    ("qualcomm", "0x5143", ["adreno", "qualcomm", "turnip"]),
    ("arm", "0x13b5", ["mali", "immortalis"]),
    ("imgtec", "0x1010", ["powervr", "imagination", "img"]),
    ("apple", "0x106b", ["apple"]),
    ("nvidia", "0x10de", ["nvidia", "geforce", "quadro", "tegra"]),
    ("amd", "0x1002", ["amd", "radeon", "ati"]),
    ("intel", "0x8086", ["intel"]),
    ("samsung", "0x144d", ["xclipse", "samsung"]),
    ("huawei", "0x19e5", ["maleoon", "huawei"]),
    ("software", "", ["swiftshader", "llvmpipe", "lavapipe", "softpipe", "microsoft"]),
]
VENDOR_BY_ID = {vid: name for name, vid, _ in VENDORS if vid}

# The acceptance table (spec S56 AC1): the C++ test (engine/tests/core/device/GpuTierTest.cpp) checks
# the same strings; here they check the data before it is written.
EXAMPLES = [
    ("android", "Adreno (TM) 530", "low"),
    ("android", "Adreno (TM) 650", "medium"),
    ("android", "Adreno (TM) 740", "high"),
    ("android", "Mali-G52", "low"),
    ("android", "Mali-G78", "medium"),
    ("android", "PowerVR Rogue GE8320", "low"),
    ("apple", "Apple A12 GPU", "medium"),
    ("apple", "Apple A15 GPU", "high"),
    ("apple", "Apple M1", "high"),
    ("pc", "Mesa Intel(R) UHD Graphics 620 (KBL GT2)", "medium"),
    ("pc", "NVIDIA GeForce RTX 3060/PCIe/SSE2", "high"),
    ("web", "ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)", "low"),
]


def normalize(text):
    out = text.lower()
    for token in ("(tm)", "(r)"):
        out = out.replace(token, "")
    out = "".join(c if ("a" <= c <= "z" or "0" <= c <= "9") else " " for c in out)
    return " ".join(out.split())


def check_pattern(pattern):
    """A family pattern: normalized literal text with \\d, [...] and '.'; returns an error or None."""
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\":
            if pattern[i + 1:i + 2] != "d":
                return "only \\d is an escape"
            i += 2
        elif c == "[":
            end = pattern.find("]", i)
            if end < 0:
                return "unclosed ["
            i = end + 1
        elif c == "." or c == " " or ("a" <= c <= "z") or ("0" <= c <= "9"):
            i += 1
        else:
            return "'%s' is not normalized text" % c
    if pattern != pattern.strip() or "  " in pattern:
        return "spaces at an end or doubled"
    return None


def match_at(pattern, text, pos):
    """The end of the pattern matched at pos, or -1 (the C++ matcher does the same)."""
    i = 0
    while i < len(pattern):
        if pos >= len(text):
            return -1
        c = pattern[i]
        ch = text[pos]
        if c == "\\":
            if not ch.isdigit():
                return -1
            i += 2
        elif c == "[":
            end = pattern.index("]", i)
            body = pattern[i + 1:end]
            ok = False
            k = 0
            while k < len(body):
                if k + 2 < len(body) and body[k + 1] == "-":
                    ok = ok or body[k] <= ch <= body[k + 2]
                    k += 3
                else:
                    ok = ok or body[k] == ch
                    k += 1
            if not ok:
                return -1
            i = end + 1
        elif c == ".":
            i += 1
        else:
            if c != ch:
                return -1
            i += 1
        pos += 1
    return pos


def family_matches(pattern, text):
    for start in range(len(text)):
        if start > 0 and text[start - 1] != " ":
            continue
        end = match_at(pattern, text, start)
        if end >= 0 and (end == len(text) or text[end] == " "):
            return True
    return False


def name_matches(name, text):
    return (" " + name + " ") in (" " + text + " ")


def platforms_for(platform):
    return {"android", "apple", "pc", "any"} if platform == "web" else {platform, "any"}


def classify(db, platform, renderer):
    """(tier, kind) the way GpuTierDatabase::match does, without device ids."""
    text = normalize(renderer)
    allowed = platforms_for(platform)
    best = None
    for rule in db["rules"]:
        if rule["platform"] in allowed and rule["kind"] == "name" and name_matches(rule["name"], text):
            if best is None or len(rule["name"]) > len(best["name"]):
                best = rule
    if best:
        return best["tier"], "name"
    for rule in db["rules"]:
        if rule["platform"] in allowed and rule["kind"] == "family" and family_matches(rule["pattern"], text):
            return rule["tier"], "family"
    words = set(text.split())
    vendor = next((name for name, _, tokens in db_vendors(db) if words & set(tokens)), "")
    for rule in db["rules"]:
        if rule["platform"] in {platform, "any"} and rule["kind"] == "vendor" and rule["vendor"] == vendor:
            return rule["tier"], "vendor"
    return None, "unknown"


def db_vendors(db):
    return [(v["name"], v["id"], v["tokens"]) for v in db["vendors"]]


def parse_forge(path, platform):
    """Rows of one gpu.data file: (vendor id, device id, preset, name)."""
    rows = []
    in_list = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("BEGIN_GPU_LIST"):
                in_list = True
                continue
            if line.startswith("END_GPU_LIST"):
                in_list = False
                continue
            if not in_list or not line or line.startswith("#"):
                continue
            fields = [part.strip() for part in line.split(";")]
            if len(fields) < 5 or not fields[0].lower().startswith("0x"):
                continue
            vendor_id = "0x%04x" % int(fields[0], 16)
            device_id = "0x%x" % int(fields[1], 16)
            preset = fields[2].lower()
            name = fields[4].lstrip("#").strip()
            if preset not in REMAP[platform]:
                continue   # UNDEFINED
            rows.append((vendor_id, device_id, preset, name))
    return rows


def convert(forge_root):
    by_key = {}
    for platform, rel in FORGE_FILES:
        path = os.path.join(forge_root, rel)
        if not os.path.isfile(path):
            sys.exit("missing %s (give --forge the root of a The-Forge checkout)" % path)
        for vendor_id, device_id, preset, name in parse_forge(path, platform):
            key = (platform, normalize(name))
            if not key[1]:
                continue
            tier = REMAP[platform][preset]
            entry = by_key.setdefault(key, {"vendor": VENDOR_BY_ID.get(vendor_id, vendor_id), "devices": [], "tier": tier,
                                            "presets": set()})
            if device_id not in entry["devices"]:
                entry["devices"].append(device_id)
            entry["presets"].add(preset)
            if TIERS.index(tier) < TIERS.index(entry["tier"]):
                entry["tier"] = tier

    rules = []
    for (platform, name), entry in sorted(by_key.items()):
        rule = {"platform": platform, "kind": "name", "vendor": entry["vendor"], "name": name}
        # (Android's are Vulkan ids: no renderer string JasperEngine reads there carries one)
        if platform != "android":
            rule["devices"] = sorted(entry["devices"])
        rule["tier"] = entry["tier"]
        rule["from"] = "forge " + "/".join(sorted(entry["presets"]))
        override = OVERRIDES.get((platform, name))
        if override:
            rule["tier"] = override[0]
            rule["from"] += "; je: " + override[1]
        rules.append(rule)
    missing = sorted(set(OVERRIDES) - set(by_key))
    if missing:
        sys.exit("OVERRIDES names a GPU the Forge files do not: %s" % missing)
    for platform, vendor, pattern, tier in FAMILIES:
        error = check_pattern(pattern)
        if error:
            sys.exit("family pattern %r: %s" % (pattern, error))
        rules.append({"platform": platform, "kind": "family", "vendor": vendor, "pattern": pattern, "tier": tier, "from": "je"})
    for platform, vendor, tier in VENDOR_DEFAULTS:
        rules.append({"platform": platform, "kind": "vendor", "vendor": vendor, "tier": tier, "from": "je"})
    for rule in rules:
        assert rule["tier"] in TIERS, rule
    return {
        "version": 1,
        "generator": "tools/device/convert_forge_gpu_data.py",
        "source": FORGE_SOURCE,
        "tiers": TIERS,
        "vendors": [{"name": name, "id": vid, "tokens": tokens} for name, vid, tokens in VENDORS],
        "rules": rules,
    }


def dump_json(db):
    # one rule per line: reviewable diffs
    head = {k: v for k, v in db.items() if k not in ("vendors", "rules")}
    lines = ["{"]
    for key, value in head.items():
        lines.append("  %s: %s," % (json.dumps(key), json.dumps(value)))
    lines.append('  "vendors": [')
    lines.append(",\n".join("    " + json.dumps(v, separators=(",", ":")) for v in db["vendors"]))
    lines.append("  ],")
    lines.append('  "rules": [')
    lines.append(",\n".join("    " + json.dumps(r, separators=(",", ":")) for r in db["rules"]))
    lines.append("  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def dump_cpp(text):
    # MSVC refuses string literals above 16,380 bytes: chunks of at most 8,000
    chunks = []
    rest = text
    while rest:
        cut = len(rest) if len(rest) <= 8000 else rest.rfind("\n", 0, 8000) + 1
        if cut <= 0:
            cut = 8000
        chunks.append(rest[:cut])
        rest = rest[cut:]
    out = ["// (c) Eduardo Doria and contributors",
           "// SPDX-License-Identifier: MIT",
           "// Generated by tools/device/convert_forge_gpu_data.py from gpu_tiers.json: do not edit (run it with --embed).",
           "// The data is The Forge's gpu.data (Apache-2.0), re-mapped; see the script.",
           "",
           '#include "device/GpuTierDatabase.h"',
           "",
           "namespace {",
           "    const char* const kGpuTierChunks[] = {"]
    for chunk in chunks:
        assert ")JEGPU\"" not in chunk
        out.append('R"JEGPU(' + chunk + ')JEGPU",')
    out += ["    };",
            "}",
            "",
            "std::string jasperengine::GpuTierDatabase::embeddedJson(){",
            "    std::string json;",
            "    json.reserve(%d);" % len(text.encode("utf-8")),
            "    for (const char* chunk : kGpuTierChunks)",
            "        json += chunk;",
            "    return json;",
            "}",
            ""]
    return "\n".join(out)


def self_check(db):
    failures = []
    for platform, renderer, expected in EXAMPLES:
        tier, kind = classify(db, platform, renderer)
        if tier != expected:
            failures.append("%s on %s: %s (%s), expected %s" % (renderer, platform, tier, kind, expected))
    return failures


def read(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--forge", help="the root of a The-Forge checkout (its Common_3/OS/*/*_gpu.data)")
    parser.add_argument("--embed", action="store_true", help="rewrite GpuTiersData.cpp from gpu_tiers.json")
    parser.add_argument("--check", action="store_true", help="write nothing; exit 1 when a file is stale")
    args = parser.parse_args()

    if args.forge:
        db = convert(args.forge)
        json_text = dump_json(db)
    else:
        json_text = read(JSON_PATH)
        if json_text is None:
            sys.exit("no %s: run with --forge" % JSON_PATH)
        db = json.loads(json_text)
    failures = self_check(db)
    if failures:
        print("the database misclassifies the acceptance table:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    cpp_text = dump_cpp(json_text)

    stale = []
    if args.forge and read(JSON_PATH) != json_text:
        stale.append(JSON_PATH)
    if read(CPP_PATH) != cpp_text:
        stale.append(CPP_PATH)
    if args.check:
        for path in stale:
            print("stale: %s (run tools/device/convert_forge_gpu_data.py %s)" % (os.path.relpath(path, ROOT),
                  "--forge <The-Forge>" if path == JSON_PATH else "--embed"), file=sys.stderr)
        return 1 if stale else 0
    if not args.forge and not args.embed:
        parser.error("give --forge, --embed or --check")
    if args.forge:
        write(JSON_PATH, json_text)
    write(CPP_PATH, cpp_text)
    names = sum(1 for r in db["rules"] if r["kind"] == "name")
    print("%d rules (%d names), %d bytes of JSON" % (len(db["rules"]), names, len(json_text.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
