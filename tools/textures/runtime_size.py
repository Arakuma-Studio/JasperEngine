#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The size the cooked-texture runtime adds to a game (docs/specs/S14-texture-compression.md R21):
the code and data of its objects in a Release build tree, as `size` (native ELF) or the object
files' code sections (wasm) count them.

    python3 tools/textures/runtime_size.py <build dir> [--wasm] [--json out.json]

Native counts Zstandard's decoder, the KTX2 reader/writer, the CPU decoders (bcdec), the
fallback plans and the cooked index (R21's "zstd decoder + bcdec + KTX2 reader"); the mip
chain and the preparation are listed apart (the raw path's, which replaced
SokolTexture::generateMipmaps). --wasm adds the Basis Universal transcoder. Budgets: native
<= 120 KB, wasm <= 250 KB.
"""

import argparse
import json
import os
import re
import subprocess
import sys

RUNTIME = ("Ktx2.cpp", "TextureDecode.cpp", "TextureFallback.cpp", "TextureFormat.cpp", "CookedTextures.cpp",
           "BasisTranscode.cpp")
RAW_PATH = ("MipChain.cpp", "TexturePrepare.cpp")
ZSTD = re.compile(r"/zstd_decompress\.dir/")
BASIS = re.compile(r"/basisu_transcoder\.dir/")


def objects(build):
    for directory, _, names in os.walk(build):
        for name in names:
            if name.endswith((".o", ".obj")):
                yield os.path.join(directory, name)


def native_size(path):
    out = subprocess.run(["size", "-A", path], capture_output=True, text=True, check=True).stdout
    total = 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith((".text", ".rodata", ".data")) and parts[1].isdigit():
            total += int(parts[1])
    return total


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build")
    ap.add_argument("--wasm", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()
    measure = wasm_size if args.wasm else native_size
    groups = {"runtime": 0, "zstd_decompress": 0, "raw_path": 0, "basisu_transcoder": 0}
    seen = set()
    for path in objects(args.build):
        name = os.path.basename(path).rsplit(".", 1)[0]
        group = None
        if ZSTD.search(path):
            group = "zstd_decompress"
        elif BASIS.search(path):
            group = "basisu_transcoder"
        elif "/jasperengine.dir/" in path and name in RUNTIME:
            group = "runtime"
        elif "/jasperengine.dir/" in path and name in RAW_PATH:
            group = "raw_path"
        if group and (group, name) not in seen:
            seen.add((group, name))
            groups[group] += measure(path)
    budget = 250 * 1024 if args.wasm else 120 * 1024
    counted = groups["runtime"] + groups["zstd_decompress"] + (groups["basisu_transcoder"] if args.wasm else 0)
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
