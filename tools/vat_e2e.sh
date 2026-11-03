#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# The Vertex Animation end-to-end proofs (docs/superpowers/specs/2026-10-27-vertex-animation-design.md
# section 8) in their two halves: the editor tests bake / import into a bundle directory
# (editor/tests/VatEndToEndTest.cpp: the real VatBakeSession and importers, with the CPU-measured
# error of every case), then the headless GPU tests load and render it against the skinned,
# rigid and morph sources and the importers' ground truth (engine/tests/htmlui/TestVatEndToEnd.cpp).
# The editor test binary has no GPU and the GPU test binary can't link the editor: the bundle is
# the hand-over. A GPU test that skips fails this script.
#
# usage: tools/vat_e2e.sh [--editor-tests <jasperengine-editor-tests>] [--htmlui-tests <jasperengine-htmlui-tests>]
#        defaults: build/jasperengine-editor-tests and build-engine/tests/htmlui/jasperengine-htmlui-tests
#        under the repository root (the root build and the standalone engine build with
#        -DJASPERENGINE_EDITOR_TESTS=ON / -DJASPERENGINE_HTMLUI_TESTS=ON). Without a DISPLAY the GPU
#        half runs under xvfb-run with software GL. The bundle goes to a fresh temporary directory
#        (kept with KEEP_BUNDLE=1; failed comparisons leave PNGs in its failures/).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
editor_tests="$root/build/jasperengine-editor-tests"
htmlui_tests="$root/build-engine/tests/htmlui/jasperengine-htmlui-tests"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --editor-tests) editor_tests="$2"; shift 2 ;;
        --htmlui-tests) htmlui_tests="$2"; shift 2 ;;
        -h|--help) sed -n '4,19p' "$0"; exit 0 ;;
        *) echo "vat_e2e: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
for binary in "$editor_tests" "$htmlui_tests"; do
    [[ -x "$binary" ]] || { echo "vat_e2e: $binary is not built" >&2; exit 2; }
done

bundle="$(mktemp -d -t jasperengine-vat-e2e-XXXXXX)"
[[ "${KEEP_BUNDLE:-0}" == 1 ]] || trap 'rm -rf "$bundle"' EXIT
export JASPER_VAT_E2E_DIR="$bundle"

echo "== editor half: bake and import into $bundle"
"$editor_tests" --gtest_filter='VatEndToEnd.*'

echo "== GPU half: render the bundle"
gpu=("$htmlui_tests" --gtest_filter='VatEndToEndTest.*')
if [[ -z "${DISPLAY:-}" ]]; then
    gpu=(env LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a "${gpu[@]}")
fi
log="$bundle/gpu.log"
set +e
"${gpu[@]}" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e
[[ $status == 0 ]] || exit "$status"
if grep -q '^\[  SKIPPED \]' "$log"; then
    echo "vat_e2e: GPU tests skipped (no bundle or no GPU)" >&2
    exit 1
fi
echo "== vat_e2e: both halves passed"
