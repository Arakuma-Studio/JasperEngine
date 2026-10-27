#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The size the cooked-texture runtime adds to a game (docs/specs/S14-texture-compression.md R21).

    python3 tools/textures/runtime_size.py --binary <linked game executable> [--json out.json]
    python3 tools/textures/runtime_size.py --wasm-map <wasm-ld map> [--json out.json]

Native (budget 120 KB): the code and data symbols of a linked Release game (`nm -S`, what
stripping keeps) that are Zstandard's decoder, bcdec and the KTX2 reader, R21's list. The rest
of the texture runtime (CPU decodes, fallback plans, the cooked index, the Basis wrapper) is
listed apart. Run it on build/linux/engine/jasperengine-project after tools/build.sh linux.

Wasm (R21: 250 KB; checked against 205 KB, the measured 197.4 KB and a margin, spec S14 deviation
23): from the link map of a web Release build (relink it with -DCMAKE_EXE_LINKER_FLAGS=-Wl,--Map=FILE),
the code and initialised data the link kept of the trimmed Basis Universal transcoder, Zstandard's
decoder and the texture runtime (bcdec included).
"""

import argparse
import json
import re
import subprocess
import sys

KTX2_WRITER = re.compile(r"Ktx2::(write|makeDfd|makeUastcDfd|rewriteKeyValues)\b")
NATIVE_GROUPS = [
    ("zstd_decoder", re.compile(r"^(ZSTD_|HUF_|FSE_|ZSTDv|JE_ZSTD_|ERR_getErrorString|HIST_)")),
    ("bcdec", re.compile(r"^bcdec_")),
    ("ktx2_reader", re.compile(r"^jasperengine::(Ktx2::|Ktx2MemorySource::|Ktx2PrefixSource::|Ktx2ByteSource::|ktx2detail::)")),
    ("texture_runtime", re.compile(r"^jasperengine::(TextureDecode::|TextureFallback::|CookedTextures::|BasisTranscode::)")),
    ("basisu_transcoder", re.compile(r"^basist::")),
]
COUNTED_NATIVE = ("zstd_decoder", "bcdec", "ktx2_reader")



def native(binary):
    out = subprocess.run(["nm", "-S", "-C", "--size-sort", binary], capture_output=True, text=True, check=True).stdout
    groups = {name: 0 for name, _ in NATIVE_GROUPS}
    for line in out.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) < 4 or parts[2].lower() not in "tdrbv":
            continue
        name = parts[3]
        if KTX2_WRITER.search(name):
            continue
        for group, pattern in NATIVE_GROUPS:
            if pattern.search(name):
                groups[group] += int(parts[1], 16)
                break
    counted = sum(groups[g] for g in COUNTED_NATIVE)
    return groups, counted, 120 * 1024


WASM_ENTRY = re.compile(r"^\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\S.*?\.o\)?):\(([^)]*)")
WASM_RUNTIME = re.compile(r"\((Ktx2|TextureDecode|TextureFallback|TextureFormat|CookedTextures|BasisTranscode)\.cpp\.o\)")


def wasm(link_map):
    # wasm-ld --Map: every input section the link kept, with its size; .bss takes no file bytes
    groups = {"zstd_decoder": 0, "texture_runtime": 0, "basisu_transcoder": 0}
    for line in open(link_map, errors="replace"):
        m = WASM_ENTRY.match(line)
        if not m:
            continue
        offset, size, obj, section = m.group(2), int(m.group(3), 16), m.group(4), m.group(5)
        if offset == "0" and section.startswith(".bss"):
            continue
        if "basisu_transcoder.cpp.o" in obj:
            groups["basisu_transcoder"] += size
        elif "zstd_decompress" in obj:
            groups["zstd_decoder"] += size
        elif WASM_RUNTIME.search(obj) or "bcdec" in obj:
            groups["texture_runtime"] += size
    # R21 asks 250 KB; the check holds the measured 197.4 KB with a small margin (spec S14 deviation
    # 23), so a transcode path or table that creeps back in fails here
    return groups, sum(groups.values()), 205 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--binary", help="a linked native game executable")
    mode.add_argument("--wasm-map", help="the wasm-ld link map (-Wl,--Map=FILE) of a web Release build")
    ap.add_argument("--json")
    args = ap.parse_args()
    groups, counted, budget = native(args.binary) if args.binary else wasm(args.wasm_map)
    report = {"groups": groups, "counted": counted, "budget": budget, "ok": counted <= budget}
    for group, size in groups.items():
        print("%-18s %8.1f KB" % (group, size / 1024.0))
    print("%-18s %8.1f KB (budget %d KB)" % ("counted", counted / 1024.0, budget // 1024))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
