#!/usr/bin/env bash
# The UI effects' shaders on every backend (phase-5 spec 5.6): the editor's `shaders` command
# compiles every built-in effect (uieffect/all: a paint effect in both forms, a filter effect and
# the internal bright pass in the filter form, a stateful effect's step and scatter forms too) for GLSL 410, GLSL 300 es (the web), HLSL 5, MSL 2.1
# (macOS, iOS) and SPIR-V. It must exit 0 and write one .sdat per variant per backend; then
# `ui-effects --validate` (both forms compiled for glcore, each manifest against the shader's
# reflection) must pass.
#
#   tools/htmlui/check_effect_shaders.sh [--editor <jasperengine-editor>] [--keep]
#
# The editor defaults to build/linux/editor-install/bin/jasperengine-editor of this checkout,
# else of the main checkout (a worktree's). The output goes to a temporary directory, removed
# afterwards unless --keep. Runs on the host (no display needed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EDITOR=""
KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --editor) EDITOR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$EDITOR" ]]; then
    EDITOR="$ROOT/build/linux/editor-install/bin/jasperengine-editor"
    if [[ ! -x "$EDITOR" ]]; then
        MAIN="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | xargs -r dirname)"
        [[ -n "$MAIN" ]] && EDITOR="$MAIN/build/linux/editor-install/bin/jasperengine-editor"
    fi
fi
if [[ ! -x "$EDITOR" ]]; then
    echo "no editor at $EDITOR: build it (tools/build.sh linux) or pass --editor" >&2
    exit 2
fi

OUT="$(mktemp -d "${TMPDIR:-/tmp}/jasper-effect-shaders.XXXXXX")"
cleanup() { [[ $KEEP == 1 ]] && echo "kept: $OUT" || rm -rf "$OUT"; }
trap cleanup EXIT

# the expected variants: every engine/shaderlib/effects/*.frag, by its kind (the internal ones,
# named with a leading '_', have the filter form only); a stateful effect (phase 6: `// @state`)
# adds the step form (Stp) of each draw form, and the scatter form (Sct) with a scatter pass
EFFECTS="$ROOT/engine/shaderlib/effects"
expected=()
for frag in "$EFFECTS"/*.frag; do
    name="$(basename "$frag" .frag)"
    base="uieffect_fx_${name//-/_}"
    kind="$(sed -n 's|^// @kind[[:space:]]*\([a-z]*\).*|\1|p' "$frag" | head -1)"
    forms=()
    if [[ "$name" != _* && "$kind" == paint ]]; then
        forms+=("")
    fi
    forms+=("Flt")
    for form in "${forms[@]}"; do
        suffix="${form:+_$form}"
        expected+=("${base}${suffix}")
        if grep -q '^// @state' "$frag"; then
            expected+=("${base}_${form}Stp")
            if grep -qE '^// @(step|frame) .* scatter ' "$frag"; then
                expected+=("${base}_${form}Sct")
            fi
        fi
    done
done
LANGS=(glsl410 glsl300es hlsl5 msl21macos msl21ios spirv10)

echo "== shaders --backend all --shader uieffect/all (${#expected[@]} variants x ${#LANGS[@]} backends)"
if ! "$EDITOR" shaders --out "$OUT" --format binary --backend all --shader uieffect/all; then
    echo "FAIL: the shaders command failed" >&2
    exit 1
fi

missing=0
for lang in "${LANGS[@]}"; do
    for base in "${expected[@]}"; do
        if [[ ! -s "$OUT/${base}_${lang}.sdat" ]]; then
            echo "missing: ${base}_${lang}.sdat" >&2
            missing=$((missing + 1))
        fi
    done
done
written="$(find "$OUT" -name 'uieffect_*.sdat' | wc -l)"
if [[ $missing != 0 || $written != $(( ${#expected[@]} * ${#LANGS[@]} )) ]]; then
    echo "FAIL: $missing variant file(s) missing, $written written, $(( ${#expected[@]} * ${#LANGS[@]} )) expected" >&2
    exit 1
fi
echo "ok: $written variant files (${expected[*]})"

echo "== ui-effects --validate"
if ! "$EDITOR" ui-effects --validate; then
    echo "FAIL: ui-effects --validate" >&2
    exit 1
fi
echo "PASS"
