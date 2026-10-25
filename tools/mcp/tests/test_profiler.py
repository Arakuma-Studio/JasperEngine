"""Profiling and render diagnostics (spec S15, docs/mcp/guide/profiling.md): get_render_stats and
jasper://stats, GPU pass times of the frame (AC5), the VRAM report (AC8, AC17), the pass output
preview (AC10), frame-time spikes (AC11), profiler captures as .jetrace.json and their CPU trees
(AC12), the Lua Profiler table (AC13) and the CLI's capture (R38).

GPU timing on the build servers' software devices: llvmpipe answers GL_TIME_ELAPSED queries and
lavapipe writes timestamps, so every pass is timed on both (a Vulkan editor: JASPER_EDITOR)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_runtime_regressions import decode_png  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Profiler"}
TIMEOUT = 900

DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}

PROFILER_TOOLS = ["get_render_stats", "get_frame_times", "profiler_capture", "get_cpu_profile", "get_gpu_passes",
                  "capture_pass_output", "get_vram_report"]

S: dict = {}


async def red_scene(ed: Editor) -> int:
    """A 3D scene (its sun casts shadows) with a red unlit plane filling the Scene view's centre."""
    if "scene" in S:
        return S["scene"]
    made = await ed.call("create_scene", name="ProfilerRed", type="3D")
    scene = made["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="plane", name="Red", scale=[20, 1, 20], select=False,
                  values={"mesh": {"receiveLights": False, "submeshes[0].material.baseColorFactor": [1, 0, 0, 1]}})
    await ed.call("create_entity", scene=scene, type="box", name="Caster", position=[0, 1, 0], select=False)
    await ed.call("set_scene_camera", scene=scene, position=[0, 6, 0.01], target=[0, 0, 0], display=DISPLAY)
    await ed.call("wait", frames=10)
    S["scene"] = scene
    return scene


async def test_tools_listed(ed: Editor):
    names = {t.name for t in ed.tool_list}
    for name in PROFILER_TOOLS:
        ed.check(name in names, f"{name} is registered")
    for tool in ed.tool_list:
        if tool.name in PROFILER_TOOLS:
            ed.check(tool.annotations and tool.annotations.read_only_hint, f"{tool.name} is read-only")


async def test_render_stats(ed: Editor):
    await red_scene(ed)
    stats = await ed.call("get_render_stats")
    for key in ("draw_calls", "instances", "triangles", "passes", "pipelines_applied", "bindings_applied", "uniform_bytes",
                "image_upload_bytes", "buffer_upload_bytes", "alive", "pipeline_cache", "cpu_ms", "fps", "gpu"):
        ed.check(key in stats, f"get_render_stats has {key}")
    ed.check(stats["drawn"], "the frame drew")
    ed.check(stats["draw_calls"] >= 2 and stats["passes"] >= 1, f"draws and passes counted: {stats['draw_calls']}, {stats['passes']}")
    ed.check(stats["alive"]["images"] > 0, "alive images")
    ed.check("available" in stats["gpu"] and "semantics" in stats["gpu"], "the GPU timer's state")
    # the resource is the same view
    contents = await ed.session.read_resource("jasper://stats")
    resource = json.loads(contents.contents[0].text)
    ed.check(resource["passes"] >= 1 and "gpu" in resource, "jasper://stats")


async def test_gpu_passes(ed: Editor):
    await red_scene(ed)
    r = await ed.call("get_gpu_passes")
    ed.check_eq(r["availability"]["state"], "available", f"GPU timing: {r['availability']}")
    passes = [p for p in r["passes"] if not p.get("skipped")]
    ed.check(len(passes) >= 2, f"the frame has its passes: {[p['label'] for p in r['passes']]}")
    for p in passes:
        ed.check(p["valid"] and p["gpu_ms"] is not None and p["gpu_ms"] >= 0, f"pass {p['index']} {p['label']} timed: {p}")
    total = sum(p["gpu_ms"] for p in passes)
    ed.check(total > 0, f"the passes took GPU time: {total}")
    ed.check(r["frame_gpu_ms"] is not None and total <= r["frame_gpu_ms"] * 1.05 + 1e-6,
             f"the passes fit in the frame: {total} vs {r['frame_gpu_ms']} ({r['semantics']})")
    ed.check(r["semantics"] in ("elapsed", "pipeline_point"), f"semantics {r['semantics']}")
    for p in passes:
        ed.check(isinstance(p["targets"], list) and "actions" in p and "draws" in p and "cpu_ms" in p, "pass fields")
    # the draw list, on request
    d = await ed.call("get_gpu_passes", include_draws=True, max_draws=50)
    ed.check(0 < len(d["draws"]) <= 50, f"draw list: {len(d.get('draws', []))}")
    ed.check(all("pipeline" in x and "elements" in x for x in d["draws"]), "draw fields")
    # the game frame's GPU time reached the frame-time ring and get_render_stats
    stats = await ed.call("get_render_stats")
    ed.check(stats["gpu"]["frame_ms"] is not None, f"get_render_stats gpu.frame_ms after timing: {stats['gpu']}")


async def test_vram_report(ed: Editor):
    await red_scene(ed)
    r = await ed.call("get_vram_report", top=500)
    ed.check(r["estimated"] is True and r["total_bytes"] > 0, "estimated bytes")
    by_key = {g["key"]: g for g in r["groups"]}
    other = by_key.get("other", {"bytes": 0})["bytes"]
    ed.check(other <= r["total_bytes"] * 0.05, f"Other is {other} of {r['total_bytes']} bytes (≤ 5%)")
    shadow = [a for a in r["allocations"] if a["category"] == "shadow_maps"]
    ed.check(any(a["owner"] == "CSM atlas" for a in shadow), f"Shadow Maps holds the CSM atlas: {[a['owner'] for a in shadow]}")
    owners = await ed.call("get_vram_report", group_by="owner", top=10)
    ed.check(owners["groups"] and owners["groups"][0]["bytes"] >= owners["groups"][-1]["bytes"], "owners largest first")
    filtered = await ed.call("get_vram_report", filter="shadow")
    ed.check(all("shadow" in (a["owner"] + a["label"] + a["category"]).lower() for a in filtered["allocations"]), "filter")
    # Vulkan (lavapipe, JASPER_EDITOR=build/linux/editor-vulkan-install/...) has timestamps: pipeline_point
    semantics = (await ed.call("get_render_stats"))["gpu"]["semantics"]
    if semantics == "pipeline_point":
        ed.check(r["driver"] and r["driver"]["used_bytes"] > 0, f"lavapipe reports its memory: {r['driver']}")
    else:
        # Mesa's llvmpipe exposes GL_NVX_gpu_memory_info (its total, not what is in use): null or that
        ed.check(r["driver"] is None or r["driver"]["source"] == "GL_NVX_gpu_memory_info", f"the GL driver's figure: {r['driver']}")


async def test_capture_pass_output(ed: Editor):
    await red_scene(ed)
    r = await ed.call("get_gpu_passes")
    # the scene's opaque colour pass: one of the passes into a colour target (the Scene view also
    # draws the editor's own layers) shows the red plane at its centre
    colour = [p for p in r["passes"] if any(t["attachment"] == "color0" for t in p["targets"])]
    ed.check(colour, "a pass with a colour target")
    centres = []
    for p in sorted(colour, key=lambda p: -p["draws"]):
        shot = await ed.call_raw("capture_pass_output", {"pass": p["index"], "attachment": "color0", "mode": "color"})
        ed.check(not shot.is_error and shot.images, f"an image of pass {p['index']} {p['label']}: {shot.text}")
        w, h, n, rows = decode_png(shot.images[0][0])
        centre = list(rows[h // 2][(w // 2) * n:(w // 2) * n + 3])
        centres.append((p["index"], p["label"], p["draws"], centre))
        if abs(centre[0] - 255) <= 3 and centre[1] <= 3 and centre[2] <= 3:
            break
    else:
        ed.fail(f"no colour pass shows the red plane at its centre: {centres}")
    depth_pass = [p for p in r["passes"] if any(t["attachment"] == "depth" and t.get("samples", 1) == 1 for t in p["targets"])]
    if depth_pass:
        target = max(depth_pass, key=lambda p: p["draws"])
        d = await ed.call_raw("capture_pass_output", {"pass": target["index"], "attachment": "depth"})
        ed.check(not d.is_error and d.images, f"a depth image: {d.text}")
        w, h, n, rows = decode_png(d.images[0][0])
        values = {rows[y][x * n] for y in range(0, h, max(1, h // 16)) for x in range(0, w, max(1, w // 16))}
        ed.check(len(values) > 1, "the depth image is not uniform")
    swap = [p for p in r["passes"] if any(t["attachment"] == "swapchain" for t in p["targets"])]
    if swap:
        s = await ed.call("capture_pass_output", **{"pass": swap[0]["index"]})
        ed.check(s.get("preview_unavailable") is True and "swapchain" in s["reason"], f"swapchain: {s}")
    await ed.check_error("capture_pass_output", {"pass": 100000})


async def test_profiler_capture(ed: Editor):
    await red_scene(ed)
    path = ed.project / "captures" / "run.jetrace.json"
    r = await ed.call("profiler_capture", frames=60, save_path="captures/run.jetrace.json", timeout=600)
    ed.check_eq(r["frames"], 60, "60 frames")
    ed.check(r["path"] and Path(r["path"]).exists(), f"written: {r.get('path')}")
    ed.check("overhead_note" in r and "categories" in r and r["worst_frame"] > 0, "summary")
    trace = json.loads(path.read_text())
    ed.check_eq(trace["jasper"]["version"], 1, "jasper.version")
    main = [t["id"] for t in trace["jasper"]["threads"] if t["main"]]
    ed.check(main, "a main thread")
    frames = {e["args"]["frame"] for e in trace["traceEvents"] if e.get("ph") == "X" and e.get("tid") == main[0]}
    ed.check(len(frames) == 60, f"every frame has main-thread scopes: {len(frames)}")
    ed.check(any(e.get("ph") == "M" and e.get("name") == "thread_name" for e in trace["traceEvents"]), "thread names")
    ed.check(any(e.get("ph") == "C" for e in trace["traceEvents"]), "counters")
    ed.check(len(trace["jasper"]["frames"]) == 60 and trace["jasper"]["passes"], "frames and GPU passes in jasper")
    tree = await ed.call("get_cpu_profile", capture_id=r["capture_id"], frame="worst")
    ed.check(tree["rows"] and tree["rows"][0]["depth"] == 0, f"a tree: {tree['rows'][:3]}")
    flat = await ed.call("get_cpu_profile", capture_id=r["capture_id"], view="flat", max_rows=5)
    ed.check(len(flat["rows"]) <= 5 and flat["rows"][0]["total_ms"] >= flat["rows"][-1]["total_ms"], "flat by total")
    threads = await ed.call("get_cpu_profile", capture_id=r["capture_id"], view="threads")
    ed.check(any(t["thread"] == "Main Thread" for t in threads["rows"]), f"threads: {threads['rows']}")
    gpu = await ed.call("get_gpu_passes", capture_id=r["capture_id"])
    ed.check(gpu["passes"] and gpu["frames_with_gpu"] > 30, f"the capture's pass times: {gpu.get('frames_with_gpu')}")
    resource = await ed.session.read_resource(f"jasper://profiler/capture/{r['capture_id']}")
    ed.check(json.loads(resource.contents[0].text)["jasper"]["version"] == 1, "jasper://profiler/capture/{id}")
    # the Profiler window loads the file back (its Load button, without the file dialog)
    ui = await ed.call("focus_window", window="profiler", profiler={"load": str(path)})
    ed.check(ui["shown"], "the Profiler window")
    ed.check_eq(ui["profiler"]["frames"], 60, "the Profiler shows the capture's 60 frames")
    ed.check(ui["profiler"]["target"] != 0, "its target is the loaded capture")


async def test_frame_times_spikes(ed: Editor):
    """A script that busy-waits every 30th frame (AC11: 50 ms; here 200 ms, so a slow software-rendered
    frame of a loaded build server is never taken for a spike)."""
    scene = await red_scene(ed)
    await ed.call("play", action="start", scene=scene)
    await ed.call("eval_lua", code="""
        local function spike()
            local t = os.clock()
            while os.clock() - t < 0.2 do end
            Timer.afterFrames(30, spike)
        end
        Timer.afterFrames(30, spike)
        return true""")
    await ed.call("wait", frames=130)
    r = await ed.call("get_frame_times", window=120, min_frame_ms=150, monitors=["FPS", "Draw calls"])
    await ed.call("play", action="stop")
    ed.check(r["spike_count"] >= 3, f"spikes: {r['spike_count']} {r.get('spikes')}")
    ed.check(r["spike_interval_frames"] is not None and abs(r["spike_interval_frames"] - 30) <= 1,
             f"spike interval {r['spike_interval_frames']}: {r.get('spikes')}")
    ed.check(r["summary"]["frame_ms"]["max"] >= 150, "the max is a spike")
    ed.check(len(r["monitors"]["FPS"]["series"]) > 0, "the FPS monitor's series")
    await ed.check_error("get_frame_times", {"monitors": ["No such monitor"]}, contains="Monitors:")


async def test_lua_profiler(ed: Editor):
    scene = await red_scene(ed)
    stats = (await ed.call("eval_lua", code="return Profiler.frameStats()", allow_edit_mode=True))["values"][0]
    ed.check(stats["drawCalls"] > 0 and stats["fps"] >= 0 and "cpuMs" in stats, f"frameStats: {stats}")
    await ed.call("play", action="start", scene=scene)
    await ed.call("eval_lua", code="""
        local function work()
            Profiler.sample("work", function() local x = 0; for i = 1, 1000 do x = x + i end; return x end)
            Timer.nextFrame(work)
        end
        Timer.nextFrame(work)
        return true""")
    r = await ed.call("profiler_capture", frames=10, gpu=False)
    tree = await ed.call("get_cpu_profile", capture_id=r["capture_id"], frame="median", filter="work")
    work = [row for row in tree["rows"] if row["name"] == "work"]
    ed.check(work and work[0]["calls"] == 1, f"one 'work' sample a frame: {tree['rows']}")
    ed.check("Lua" in work[0]["path"].split("/"), f"under Lua: {work[0]['path']}")
    # an unbalanced endSample is ignored and logged once
    await ed.call("clear_console")
    await ed.call("eval_lua", code="Profiler.endSample(); Profiler.endSample(); return Profiler.enabled")
    logged = await ed.call("read_console", contains="endSample")
    await ed.call("play", action="stop")
    ed.check_eq(len(logged["entries"]), 1, f"logged once: {logged['entries']}")
