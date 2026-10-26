#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The size the cooked-texture runtime adds to a game (docs/specs/S14-texture-compression.md R21).

    python3 tools/textures/runtime_size.py --binary <linked game executable> [--json out.json]
    python3 tools/textures/runtime_size.py --wasm <web build dir> [--json out.json]

Native (budget 120 KB): the code and data symbols of a linked Release game (`nm -S`, what
stripping keeps) that are Zstandard's decoder, bcdec and the KTX2 reader, R21's list. The rest
of the texture runtime (CPU decodes, fallback plans, the cooked index, the Basis wrapper) is
listed apart. Run it on build/linux/engine/jasperengine-project after tools/build.sh linux.

Wasm (budget 250 KB): the code and data sections of the web build's objects (relocatable wasm)
of the Basis Universal transcoder, Zstandard's decoder and the texture runtime's sources. Object
sections count the inline and template code each object carries before the linker merges
copies, so this is an upper bound.
"""

import argparse
import json
import os
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

RUNTIME = ("Ktx2.cpp", "TextureDecode.cpp", "TextureFallback.cpp", "TextureFormat.cpp", "CookedTextures.cpp",
           "BasisTranscode.cpp")
ZSTD = re.compile(r"/zstd_decompress\.dir/")
BASIS = re.compile(r"/basisu_transcoder\.dir/")


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


def wasm_size(path):
    # the code and data sections of a wasm object (relocatable): its size minus the custom sections
    data = open(path, "rb").read()
    if data[:4] != b"\0asm":
        return 0
    pos, total = 8, 0
    while pos < len(data):
        section = data[pos]
        pos += 1
        length, shift = 0, 0
        while True:
            byte = data[pos]
            pos += 1
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        if section in (10, 11):     # code, data
            total += length
        pos += length
    return total


def wasm(build):
    groups = {"runtime": 0, "zstd_decompress": 0, "basisu_transcoder": 0}
    seen = set()
    for directory, _, names in os.walk(build):
        for file in names:
            if not file.endswith((".o", ".obj")):
                continue
            path = os.path.join(directory, file)
            name = file.rsplit(".", 1)[0]
            group = None
            if ZSTD.search(path):
                group = "zstd_decompress"
            elif BASIS.search(path):
                group = "basisu_transcoder"
            elif "/jasperengine.dir/" in path and name in RUNTIME:
                group = "runtime"
            if group and (group, name) not in seen:
                seen.add((group, name))
                groups[group] += wasm_size(path)
    return groups, sum(groups.values()), 250 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--binary", help="a linked native game executable")
    mode.add_argument("--wasm", help="a web build directory")
    ap.add_argument("--json")
    args = ap.parse_args()
    groups, counted, budget = native(args.binary) if args.binary else wasm(args.wasm)
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
