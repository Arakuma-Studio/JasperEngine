"""S06 job system and memory: get_job_system_stats, set_job_system_config, get_memory_stats,
the loading count during an asset storm, and the Lua Jobs table.

Starts from a brand-new project; the textures are written by the test (PNG, stdlib only).
"""

import asyncio
import os
import struct
import time
import zlib

from harness import Editor, find_key

CI = True
EDITOR = {"create": True, "name": "Jobs Memory Test"}
TIMEOUT = 900

TEXTURES = 40
SIZE = 512


def png(width: int, height: int, seed: int) -> bytes:
    """An RGBA PNG with a pattern that does not compress to nothing (decoding takes time)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            v = (x * 31 + y * 17 + seed * 97 + ((x * y) >> 3)) & 0xFF
            rows += bytes((v, (v * 7 + seed) & 0xFF, (x ^ y ^ seed) & 0xFF, 255))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")


def default_workers() -> int:
    # thread::workerCountFor(Desktop, cores) without JE_JOB_WORKERS
    if os.environ.get("JE_JOB_WORKERS"):
        return int(os.environ["JE_JOB_WORKERS"])
    return max(1, min((os.cpu_count() or 2) - 1, 15))


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code, allow_edit_mode=True))["values"]


async def test_job_system_stats_schema(ed: Editor):
    """get_job_system_stats returns the R29 payload; workers is the Auto policy's count."""
    s = await ed.call("get_job_system_stats", window_frames=30)
    for key in ("workers", "single_thread", "lanes", "gate", "channels", "io", "per_worker", "totals", "asset_jobs"):
        ed.check_in(key, s, f"stats has {key}")
    ed.check_eq(s["workers"], default_workers(), "workers follows the Auto policy")
    ed.check_eq(s["single_thread"], False, "the editor runs workers")
    ed.check_eq(sorted(s["lanes"].keys()), ["Background", "High", "Normal"], "three lanes")
    for lane in s["lanes"].values():
        ed.check(set(lane.keys()) >= {"queued", "running"}, f"lane fields: {lane}")
    ed.check(set(s["gate"].keys()) >= {"max", "taken", "heavy_max", "heavy_taken", "reserve"}, f"gate: {s['gate']}")
    ed.check_eq(s["gate"]["max"], max(1, s["workers"] - s["gate"]["reserve"]), "gate max = workers - reserve")
    ed.check_eq(s["gate"]["heavy_max"], max(1, s["gate"]["max"] - 2), "Heavy leaves two slots")
    names = {c["name"]: c for c in s["channels"]}
    for name, cap in (("process", 2), ("io-write", 1), ("net", 2)):
        ed.check_in(name, names, "built-in channel")
        ed.check_eq(names[name]["cap"], cap, f"{name} cap")
    ed.check_eq(sorted(s["io"]["queued_by_priority"].keys()), ["Critical", "High", "Low", "Normal"], "I/O priorities")
    ed.check_eq(len(s["per_worker"]), s["workers"], "one entry per worker")
    for worker in s["per_worker"]:
        ed.check(0.0 <= worker["busy_pct"] <= 100.0, f"busy_pct in range: {worker}")
    ed.check(set(s["totals"].keys()) >= {"executed", "stolen", "cancelled", "failed"}, f"totals: {s['totals']}")


async def test_reserved_frame_workers_live_and_undoable(ed: Editor):
    """set_job_system_config changes the gate at once; undo (scope project) restores it."""
    before = await ed.call("get_job_system_stats", window_frames=1)
    r = await ed.call("set_job_system_config", reserved_frame_workers=3)
    workers = before["workers"]
    ed.check_eq(r["gate"]["reserve"], 3, "the reserve applied")
    ed.check_eq(r["gate"]["max"], max(1, workers - 3), "gate.max changed live")
    settings = await ed.call("get_project_settings", keys=["player.threading.reserved_frame_workers"])
    ed.check_eq(settings["settings"]["player.threading.reserved_frame_workers"], 3, "saved in the project settings")
    await ed.call("undo", scope="project")
    after = await ed.call("get_job_system_stats", window_frames=1)
    ed.check_eq(after["gate"]["max"], before["gate"]["max"], "undo restores gate.max")
    ed.check_eq(after["gate"]["reserve"], before["gate"]["reserve"], "undo restores the reserve")


async def setup_textures(ed: Editor) -> list[str]:
    folder = ed.project / "assets" / "storm"
    folder.mkdir(parents=True, exist_ok=True)
    paths = []
    for k in range(TEXTURES):
        (folder / f"storm{k:02d}.png").write_bytes(png(SIZE, SIZE, k))
        paths.append(f"assets/storm/storm{k:02d}.png")
    await ed.call("sync_files")
    return paths


async def test_loading_count_never_idle_mid_decode(ed: Editor):
    """40 textures load asynchronously: Engine.getQueuedResourceCount() never reads 0 while a
    load is in progress, and get_memory_stats reports the GPU textures afterwards."""
    paths = await setup_textures(ed)
    # their cooks first (S14: the editor cooks new textures on the Background lane, and a raw load
    # asks for one): during the storm every Background body is then an asset load, of the cooks.
    # Uncompressed (RGBA8 + Zstandard): their loads decode enough for the probe to see them
    await ed.call("set_texture_import_settings", paths=paths, settings={"format": "RGBA8"}, platform="desktop")
    cooked = await ed.call("reimport_textures", paths=paths, platforms=["desktop"], wait=True, timeout_s=600)
    ed.check(not cooked["timed_out"], "the storm textures cooked")
    made = await ed.call("create_scene", name="Storm", type="3D", path="scenes/Storm")
    scene = made["scene"]["id"]
    calls = []
    for k, path in enumerate(paths):
        calls.append(("create_entity", {"scene": scene, "type": "box", "name": f"Box{k}",
                                        "position": [k % 8 * 1.5, 0, k // 8 * 1.5]}))
    created = await ed.batch(calls)
    ids = [find_key(r, "entity_id", "id") for r in created["results"]]
    ed.check(all(i is not None for i in ids), f"entity ids: {created}")
    probe = ("local before = Jobs.getRunningCount(JobLane.Background) "
             "local queued = Engine.getQueuedResourceCount() "
             "local active = ResourceProgress.getActiveBuildCount() "
             "local after = Jobs.getRunningCount(JobLane.Background) "
             "return {queued, active, before, after}")
    # let the editor's own Background work (the Project window's thumbnails of the new files)
    # finish first: during the storm every Background body is then an asset job
    deadline = time.monotonic() + 120
    quiet = 0
    while time.monotonic() < deadline and quiet < 10:
        queued, active, before, after = (await lua(ed, probe))[0]
        quiet = quiet + 1 if queued == 0 and active == 0 and before == 0 and after == 0 else 0
        await asyncio.sleep(0.05)
    ed.check_eq(quiet, 10, "the editor went idle before the storm")

    # every texture is requested at once: an asset storm on the Background lane. One asset job
    # at a time (the gate's max): the 40 decodes take tens of probes, where 11 workers finish
    # them within a frame of a loaded builder, before the first probe can see them. The first
    # probe is the batch's last call, right after the requests.
    await lua(ed, "Engine.setMaxResourceLoadingThreads(1) return true")
    try:
        storm = await ed.batch([("set_properties", {"scene": scene, "entity": eid,
                                                    "values": {"mesh": {"submeshes[0].material.baseColorTexture": path}}})
                                for eid, path in zip(ids, paths)]
                               + [("eval_lua", {"code": probe, "allow_edit_mode": True})])
        ed.check_eq(storm.get("failed", 0), 0, f"the storm's calls: {str(storm)[:600]}")
        first = find_key(storm["results"][-1], "values")
        ed.check(first, f"the probe in the batch: {str(storm['results'][-1])[:300]}")
        observe = await observe_storm(ed, probe, first[0])
    finally:
        await lua(ed, "Engine.setMaxResourceLoadingThreads(0) return true")
    samples, busy, background, violations = observe
    ed.check(not violations, f"getQueuedResourceCount read 0 while loads were running: {violations[:5]}")
    ed.check(busy > 0, f"the storm was observed loading ({samples} samples)")
    print(f"storm: {samples} samples, {busy} loading, {background} with Background bodies running")

    stats = await ed.call("get_memory_stats", prefix="Texture")
    gpu = [r for r in stats["records"] if r["residency"] == "Gpu"]
    ed.check(len(gpu) >= 1, f"a Gpu texture record: {stats['records']}")
    ed.check(sum(r["current"] for r in gpu) >= TEXTURES * SIZE * SIZE * 4 // 2, f"the textures are counted: {gpu}")
    ed.check(stats["totals"]["Gpu"] > 0, "Gpu total")


async def observe_storm(ed: Editor, probe: str, first: list) -> tuple:
    """Samples the loading counts until the storm is over: (samples, busy, background, violations)."""
    deadline = time.monotonic() + 120
    samples = 0
    busy = 0
    background = 0
    violations = []
    sample = first
    while time.monotonic() < deadline:
        if sample is None:
            sample = (await lua(ed, probe))[0]
        queued, active, before, after = sample
        sample = None
        samples += 1
        busy += queued > 0
        background += before > 0
        # an asset load in progress (its build started and not finished, or its body running on
        # a Background worker across the read) is counted
        if queued == 0 and (active > 0 or (before > 0 and after > 0)):
            violations.append((queued, active, before, after))
        if queued == 0 and active == 0 and before == 0 and samples > 5:
            break
        await asyncio.sleep(0.01)
    return samples, busy, background, violations


async def test_lua_jobs_table(ed: Editor):
    """The read-only Jobs table, Engine.setMaxResourceLoadingThreads and the deprecated
    ThreadPoolManager shim (works, warns once)."""
    workers = (await lua(ed, "return Jobs.getWorkerCount()"))[0]
    ed.check(workers > 0, f"workers on desktop: {workers}")
    ed.check_eq((await lua(ed, "return Jobs.isSingleThreaded()"))[0], False, "not single-threaded")
    queued = (await lua(ed, "return Jobs.getQueuedCount(JobLane.Background)"))[0]
    ed.check(queued >= 0, "queued count")

    await lua(ed, "Engine.setMaxResourceLoadingThreads(1) return true")
    s = await ed.call("get_job_system_stats", window_frames=1)
    ed.check_eq(s["gate"]["max"], 1, "setMaxResourceLoadingThreads sets the gate max, live")
    await lua(ed, "Engine.setMaxResourceLoadingThreads(0) return true")
    s = await ed.call("get_job_system_stats", window_frames=1)
    ed.check_eq(s["gate"]["max"], max(1, s["workers"] - s["gate"]["reserve"]), "0 returns to the reserve")

    shim = ("local a = ThreadPoolManager.getInstance():getQueueSize() "
            "ThreadPoolManager.initialize(4) ThreadPoolManager.shutdown() "
            "local b = ThreadPoolManager.getInstance():getQueueSize() return a, b")
    sizes = await lua(ed, shim)
    ed.check(all(isinstance(v, int) and v >= 0 for v in sizes), f"getQueueSize works: {sizes}")
    await lua(ed, shim)
    warnings = await ed.console_since(None, types=["warning"], contains="ThreadPoolManager is deprecated", limit=100)
    ed.check_eq(len(warnings), 1, f"the shim warns exactly once: {warnings}")


def editor_threads(ed: Editor) -> tuple[str, list[str]]:
    """(main thread's name, every thread's name) of this test's editor process, found by its
    project path on the command line; ("", []) when it is not visible."""
    project = str(ed.project).encode()
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as f:
                cmdline = f.read()
        except OSError:
            continue
        if b"jasperengine-editor" not in cmdline or project not in cmdline:
            continue
        main, names = "", []
        for task in os.listdir(f"/proc/{entry}/task"):
            try:
                with open(f"/proc/{entry}/task/{task}/comm") as f:
                    name = f.read().strip()
            except OSError:
                continue
            names.append(name)
            if task == entry:
                main = name
        return main, names
    return "", []


async def test_physics_runs_on_the_job_workers(ed: Editor):
    """3D physics steps on the engine's workers. A thread pool of Jolt's own would show up
    as threads named like the main thread that created them (Linux threads inherit the name)."""
    main, before = editor_threads(ed)
    if not before:
        from harness import skip
        skip("the editor process is not visible in /proc (remote editor)")
    ed.check_eq(main, "je-main", "the main thread is named")
    ed.check_eq(sum(n.startswith("je-worker-") for n in before), default_workers(), f"named workers: {before}")

    made = await ed.call("create_scene", name="Bodies", type="3D", path="scenes/Bodies")
    scene = made["scene"]["id"]
    created = await ed.batch([("create_entity", {"scene": scene, "type": "box", "name": f"Body{k}",
                                                  "position": [k % 10 * 1.2, 2 + k // 100 * 1.2, k // 10 % 10 * 1.2]})
                              for k in range(200)])
    ids = [find_key(r, "entity_id", "id") for r in created["results"]]
    await ed.batch([("edit_colliders", {"scene": scene, "entity": eid, "body_type": "dynamic", "add": [{"shape": "box"}]})
                    for eid in ids])
    await ed.call("play", action="start")
    try:
        await ed.call("wait", seconds=2)
        _, during = editor_threads(ed)
    finally:
        await ed.call("play", action="stop")
    ed.check(not any("jolt" in n.lower() for n in during), f"no Jolt-named threads: {during}")
    ed.check_eq(during.count(main), before.count(main), f"Play started threads named like the main thread: {during}")
    ed.check_eq(sum(n.startswith("je-worker-") for n in during), default_workers(), "the same workers")


async def pipeline_descs(ed: Editor) -> dict:
    stats = await ed.call("get_memory_stats", prefix="Render.PipelineDescs")
    records = [r for r in stats["records"] if r["name"] == "Render.PipelineDescs"]
    ed.check_eq(len(records), 1, f"the record: {stats['records']}")
    return records[0]


async def test_pipeline_desc_record(ed: Editor):
    """mesh-memory: render objects share their base pipeline descriptions (Render.PipelineDescs):
    63 more sprites equal to a first one, drawn, add none."""
    sid = (await ed.call("create_scene", name="Descs", type="2D"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="sprite", name="Desc0", select=False)
    await ed.call("wait", frames=10)
    one = await pipeline_descs(ed)
    ed.check(one["count"] > 0 and one["current"] > 0, f"descriptions are counted: {one}")
    for i in range(1, 64):
        await ed.call("create_entity", scene=sid, type="sprite", name=f"Desc{i}", position=[i * 0.1, 0, 0], select=False)
    await ed.call("wait", frames=10)
    many = await pipeline_descs(ed)
    ed.check(many["count"] <= one["count"] + 2, f"equal sprites share them: {one['count']} for one, {many['count']} for 64")


def type_entry(stats, name):
    for entry in stats["types"]:
        if entry["name"].lower() == name:
            return entry
    return None


async def test_mesh_component_memory(ed: Editor):
    """A sprite's MeshComponent costs what it holds (mesh-memory spec): a few KB, not 200 KB of
    fixed submesh, bone and buffer arrays."""
    made = []
    for i in range(64):
        made.append((await ed.call("create_entity", type="sprite", name=f"MemorySprite{i}", select=False))["entity_id"])
    try:
        stats = await ed.call("get_ecs_stats")
        mesh = type_entry(stats, "mesh")
        ed.check(mesh is not None, f"a MeshComponent array: {[t['name'] for t in stats['types']]}")
        per = mesh["bytes"] / mesh["count"]
        ed.check(per <= 4096, f"MeshComponent storage per component: {per:.0f} bytes")
        ed.check(mesh["element_bytes"] <= 2048, f"sizeof(MeshComponent): {mesh['element_bytes']}")
        ed.check(0 < mesh["heap_bytes"] / mesh["count"] <= 10240, f"arrays per mesh: {mesh['heap_bytes'] / mesh['count']:.0f}")
        ed.check_eq(stats["bytes"]["heap"], sum(t.get("heap_bytes", 0) for t in stats["types"]), "heap total")
    finally:
        await ed.call("delete_entities", entities=made)
