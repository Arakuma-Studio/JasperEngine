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

## RTX 3080 Ti (server H, `rb.sh H`, GL through the headless Xorg) against llvmpipe, 2026-11-08

`ta/w3-3d-runtime`, RelWithDebInfo core tests, the same tests as above plus `Title3DBakeBench.RealtimeTiers`
(one Realtime re-render of "BIG BONUS" Gold 3D at 280 px, about 30 % of a 1080p frame, a colour change so
every pass runs with the mesh and the environment cached; median of 5, wall clock including a synchronous
readback of the result).

| Metric | RTX 3080 Ti | llvmpipe (server A) | Budget |
|---|---|---|---|
| `gold_big_bonus_bake_ms` | 41.9 | 1,989 (earlier run) | 1,500 (server A GPU) |
| `cold_environment_1024_ms` | 1,796 | 18,174 (earlier run) | 400 |
| Realtime High, one re-render | 35.7 ms (70 passes) | 598.7 ms | §6.7: RTX 3080 Ti <= 4 ms GPU, <= 80 passes |
| Realtime Medium | 34.9 ms (41 passes) | 200.0 ms | <= 48 passes |
| Realtime Low | 35.0 ms (7 passes) | 505.4 ms | <= 7 passes |

The Realtime figures on the RTX are flat across tiers: they are dominated by the readback and the job's
CPU side, not by GPU time, so they do not show §6.7's GPU budgets (which need S15's GPU timings); the pass
counts do meet §6.7. The cold environment build at 1024 still takes 1.8 s on the RTX: the build is spread
over the render's calls by a time-adapted budget (spec deviation 22), so most of it is likely the share schedule (inferred, not profiled),
not the GPU; it stays over its 400 ms budget.
