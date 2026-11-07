#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Regenerates the S53 loader fixtures (README.md) into <out dir>, outside the repository first:
#   make.sh <gltfpack 1.3> <node_modules with @gltf-transform/{core,extensions,functions} 4.x,
#           draco3dgltf, meshoptimizer> <out dir>
# Treat the tools as untrusted: they only ever read the sources written here.
set -euo pipefail
GLTFPACK="$1"; MODULES="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
python3 -I "$HERE/make_sources.py" "$OUT"
python3 -I "$HERE/make_handmade.py" "$OUT"
cd "$OUT"
"$GLTFPACK" -i source.glb -o meshopt_c.glb -c
"$GLTFPACK" -i source.glb -o meshopt_cc.glb -cc
"$GLTFPACK" -i source.glb -o meshopt_kn.glb -c -kn
"$GLTFPACK" -i source.glb -o meshopt_vpf.glb -c -vpf
"$GLTFPACK" -i source.glb -o meshopt_noq.glb -c -noq
"$GLTFPACK" -i source.glb -o meshopt_khr.glb -cz -ce khr
"$GLTFPACK" -i source.glb -o meshopt_cf.glb -cf
"$GLTFPACK" -i source.glb -o meshopt_c.gltf -c
"$GLTFPACK" -i source.glb -o quantized.glb
"$GLTFPACK" -i instances_source.glb -o instancing.glb -c -mi
"$GLTFPACK" -i source.glb -o basisu.glb -tc
"$GLTFPACK" -i source.glb -o webp.glb -tw
node "$HERE/gt_variants.mjs" "$MODULES" "$OUT/source.glb" "$OUT"
