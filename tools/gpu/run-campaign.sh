#!/usr/bin/env bash
# tools/gpu/run-campaign.sh <part...>  — the real-GPU measurements of docs/perf/gpu-campaign.md, inside the
# build image on a GPU host, with tools/gpu/xorg-headless.sh running:
#   JIMG_GPU=1 rb.sh H --img 'tools/gpu/run-campaign.sh s32 s12'
# Parts: s32 s12 s10 s11 s13 s20 s15 s16 vat (default: all). RUNS (default 5). Results and logs go to
# build/gpu/<part>-<backend>.{json,log}. GL editor build/linux/editor-install, Vulkan
# build/linux/editor-vulkan-install (tools/build.sh linux, GRAPHIC_BACKEND=vulkan).
set -uo pipefail
cd "$(dirname "$0")/../.."
. tools/gpu/env.sh
export JASPER_XVFB=none
RUNS="${RUNS:-5}"
OUT=build/gpu
mkdir -p "$OUT"
GL="$PWD/build/linux/editor-install/bin/jasperengine-editor"
VK="$PWD/build/linux/editor-vulkan-install/bin/jasperengine-editor"   # absolute: the harness runs editors in scratch dirs
PY=(dbus-run-session -- uv run -q --no-project --with-requirements tools/mcp/requirements.txt python)
parts=("$@"); [[ ${#parts[@]} -gt 0 ]] || parts=(s32 s15 s12 s10 s11 s13 s20 s16 vat)

run() {  # run <name> <command...>
    local name="$1"; shift
    echo "[campaign] $name: $*"
    "$@" > "$OUT/$name.log" 2>&1
    echo "[campaign] $name: exit $?"
    grep -E '^\| |^run |device|Traceback|Error' "$OUT/$name.log" | tail -40
}

for part in "${parts[@]}"; do
    case "$part" in
      s32)
        B=(tools/perf/async_shaders_bench.py --runs "$RUNS")
        run s32-gl-worker-warm "${PY[@]}" "${B[@]}" --mode worker --warm-draws on --json "$OUT/s32-gl-worker-warm.json"
        run s32-gl-worker "${PY[@]}" "${B[@]}" --mode worker --warm-draws off --json "$OUT/s32-gl-worker.json"
        run s32-gl-parallel "${PY[@]}" "${B[@]}" --mode parallel --json "$OUT/s32-gl-parallel.json"
        run s32-gl-sync "${PY[@]}" "${B[@]}" --mode sync --json "$OUT/s32-gl-sync.json"
        run s32-gl-control "${PY[@]}" "${B[@]}" --mode worker --warm-draws on --control --json "$OUT/s32-gl-control.json"
        run s32-vk "${PY[@]}" "${B[@]}" --editor "$VK" --json "$OUT/s32-vk.json"
        run s32-vk-sync "${PY[@]}" "${B[@]}" --editor "$VK" --mode sync --json "$OUT/s32-vk-sync.json"
        ;;
      s12)
        run s12-gl "${PY[@]}" tools/performance/atmosphere_bench.py --gpu --runs "$RUNS" --json "$OUT/s12-gl.json"
        JASPER_EDITOR="$VK" run s12-vk "${PY[@]}" tools/performance/atmosphere_bench.py --gpu --runs "$RUNS" --json "$OUT/s12-vk.json"
        ;;
      s10)
        run s10-gl "${PY[@]}" tools/perf/gpu_campaign.py shadows --runs "$RUNS" --json "$OUT/s10-gl.json"
        run s10-vk "${PY[@]}" tools/perf/gpu_campaign.py shadows --runs "$RUNS" --tiers high,ultra --editor "$VK" --json "$OUT/s10-vk.json"
        ;;
      s11)
        [[ -f build/pbr-extended/project.yaml ]] || run s11-generate "${PY[@]}" tools/fixtures/pbr-extended/generate.py build/pbr-extended --benchmarks-only
        run s11-gl "${PY[@]}" tools/perf/gpu_campaign.py pbr --runs "$RUNS" --json "$OUT/s11-gl.json"
        ;;
      s13)
        run s13-gl "${PY[@]}" tools/perf/gpu_campaign.py post --runs "$RUNS" --json "$OUT/s13-gl.json"
        run s13-vk "${PY[@]}" tools/perf/gpu_campaign.py post --runs "$RUNS" --editor "$VK" --json "$OUT/s13-vk.json"
        ;;
      s20)
        run s20-gl "${PY[@]}" tools/perf/gpu_campaign.py particles --runs "$RUNS" --json "$OUT/s20-gl.json"
        run s20-vk "${PY[@]}" tools/perf/gpu_campaign.py particles --runs "$RUNS" --editor "$VK" --json "$OUT/s20-vk.json"
        ;;
      s15)
        # the pre-S15 editor (f93897204 + the benchmark focus fix c3acfbd56) in build/s15base-install, and
        # that commit's tools/fixtures/post-processing in build/s15-fixture (both editors read it)
        run s15-gl dbus-run-session -- python3 tools/perf/profiler_overhead.py --editor "$GL" --baseline build/s15base-install/bin/jasperengine-editor \
            --project build/s15-fixture --runs "${S15_ROUNDS:-30}" --no-check --json "$OUT/s15-gl.json"
        ;;
      s16)
        # HtmlUIRenderPerf with the frame's GPU time (R24: B2 at 1280x720, S16 against its baseline);
        # results also in build/linux/htmlui-render-bench.json
        for i in $(seq 1 "$RUNS"); do
            JASPER_HTMLUI_GPU_TIMES=1 run "s16-gl-$i" build/linux/htmlui-tests-glcore/tests/htmlui/jasperengine-htmlui-tests \
                --gtest_filter='HtmlUIRenderPerf.B2*:HtmlUIRenderPerf.B6*:HtmlUIRenderPerf.B3*'
            cp build/linux/htmlui-render-bench.json "$OUT/s16-gl-$i.json" 2>/dev/null || true
        done
        grep -h "GPU" "$OUT"/s16-gl-*.log
        ;;
      vat)
        run vat-gl python3 tools/performance/vat_crowd.py --editor "$GL" --editor-tests build/linux/editor-tests/jasperengine-editor-tests \
            --count 1000,2000,5000 --gpu-passes --frames 300 --warmup 120 --output "$OUT/vat-gl"
        run vat-vk python3 tools/performance/vat_crowd.py --editor "$VK" --editor-tests build/linux/editor-tests/jasperengine-editor-tests \
            --bundle "$OUT/vat-gl/bundle" --count 1000,2000,5000 --gpu-passes --frames 300 --warmup 120 --output "$OUT/vat-vk"
        ;;
      *) echo "unknown part $part" >&2 ;;
    esac
done
