# 3D titles: geometry and bake figures

The budgets of `docs/specs/S86-title-3d.md` R46 and R47 (AC24) are gated by `tools/perf/check_bench.py`
with `tools/perf/bench-thresholds.json` (`je-bench-titlemesh`, `je-bench-titlebake`): they are the
reference build server A's. The figures below are **this host's**: a container on a shared 8-thread
AMD EPYC (Genoa) CPU, RelWithDebInfo, llvmpipe for the GPU. They were measured on 2026-11-08 at
`ta/s86-3d` 1b800f3d5 (before the light-unit fix, which changes no geometry or pass count).

How to run them:

```sh
# geometry (one thread for the build, the job system's workers for the rest)
build/linux/core-tests/tests/core/je-bench-titlemesh --json titlemesh.json
# bake (needs a GPU context: the core test binary)
JASPER_TITLE3D_BENCH_JSON=titlebake.json jasperengine-core-tests --gtest_filter='Title3DBakeBench*'
python3 tools/perf/check_bench.py titlemesh.json titlebake.json
```

## je-bench-titlemesh (R46)

"BIG BONUS" in Dela Gothic One at 200 px with the Gold 3D structure (a gold letter with a round
bevel, a chamfered gold ring, a dark step ring, a black hull plate), medians of cold builds (a new
mesher each run).

| Metric | This host | Budget (server A) | |
|---|---|---|---|
| `big_bonus_gold_ms` | 75.8 | 40 | over |
| `big_bonus_triangles` | 33,444 | 55,000 | within |
| `big_bonus_vertex_ratio` | 0.84 | 0.65 | over (deviation 20: faceted bevels split their rings' vertices) |
| `kanji_gold_ms` (激熱, Dela Gothic One: Yuji Syuku is not a test font) | 39.6 | 300 | within |
| `inflate_3_kanji_detail4_ms` | 391.7 | 400 | within |
| `retype_last_letter_ms` | 60.7 | 15 | over: the shape cache is per layer source, so a retyped letter rebuilds every island |
| `material_edit_mesh_builds` | 0 | 0 | within |

## je-bench-titlebake (R47)

`Title3DBakeBench.GoldBigBonusAndAColdEnvironment`: the same title in a 1920 x 1080 canvas at k = 3 with
the full glint, document to readback, the environment warm; and the cold procedural environment at
1024 (the first render minus the warm one: it includes the programs' first draws).

| Metric | This host (llvmpipe) | Budget (server A GPU) |
|---|---|---|
| `gold_big_bonus_bake_ms` | 1,989 | 1,500 |
| `cold_environment_1024_ms` | 18,174 | 400 |

A software rasteriser runs the environment builder's 1024-px cube with 128-sample GGX levels on the
CPU: that is the 21 s stall MCP renders showed before the build was spread over frames (spec
deviation 22). These are not GPU figures; the budgets stay to be checked on server A.
