"""Stop's cost on 600 boxes (docs/specs/S71-mesh-update-flags.md).

Stop decodes the Play snapshot over the live scene. A Mesh decoded over a live one paid
Catalog::getChangedUpdateFlags, about 1 ms a call before S71 (the property maps of its 16 submesh
slots), and every Mesh was decoded and reloaded on every Stop, even one that Play only moved or
never touched. Oracle: the Stop's own main-thread CPU time (play stop's stop_cpu_ms: its call and
its teardown, not the editor frames drawn meanwhile), against the Stop that rebuilds the 600
GameObjects a Play destroyed (no update flags, no comparison) in the same editor, so a machine's
load cancels.
"""

from __future__ import annotations

from harness import Editor, over_budget

TIMEOUT = 2400
CI = True
EDITOR = {"create": True}
# A Stop's CPU time still varies by a fifth from one Stop to the next on a busy machine (its cores'
# other threads): the ratios are of the totals of this many rounds
ROUNDS = 15


async def test_stop_on_600_boxes_costs_what_it_should(ed: Editor):
    """Stop's main-thread CPU against rebuilding the boxes (docs/specs/S71-mesh-update-flags.md, Deviations).
    Each ratio is of the totals of ROUNDS rounds, its Stops side by side: under the limits, or the cost came
    back. Whether a moved Mesh is decoded does not show in CPU (the comparison costs what its decode
    costs): UndoProjectTest.StopKeepsAMeshAPlayOnlyMoved checks that"""
    n = 600
    rel = "Mesh Flags.scene"
    sid = (await ed.call("create_scene", name="Mesh Flags", type="3D", path=rel))["scene"]["id"]
    calls = [{"tool": "create_entity", "arguments": {"scene": sid, "type": "box", "name": f"B{i}", "select": False, "position": [i % 30, 0, i // 30]}}
             for i in range(1, n + 1)]
    for k in range(0, len(calls), 200):
        made = await ed.call_raw("batch", {"calls": calls[k:k + 200]}, timeout=900)
        ed.check(not made.is_error, f"batch: {made.text[:300]}")
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)
    saved = (ed.project / rel).read_bytes()
    change = {"mesh": "Mesh(scene, e).castShadows = false",
              "destroy": "scene:destroyEntity(e)",
              "move": "local m = Mesh(scene, e) m.position = Vector3(i, 5, -i)"}

    async def stop_cpu(what: str) -> float:
        await ed.call("open_scene", scene=sid)
        started = await ed.call_raw("play", {"action": "start", "scene": sid, "wait": True, "fixed_fps": 60, "auto_pause": True,
                                             "timeout_seconds": 900}, timeout=960)
        ed.check(not started.is_error, f"play: {started.text[:500]}")
        out = await ed.call("eval_lua", code=f"local n = 0 for i = 1, {n} do local e = scene:findEntity('B' .. i) "
                                             f"if e ~= 0 then {change[what]} n = n + 1 end end return n")
        ed.check_eq(out.get("values"), [n], f"every box changed while playing ({what})")
        await ed.call("wait", frames=2)
        stopped = await ed.call("play", action="stop", fixed_fps=0, auto_pause=False, time_scale=1)
        ed.check_eq(stopped["state"], "stopped", f"stop: {stopped}")
        ed.check(stopped.get("stop_cpu_ms", 0) > 0, f"the Stop's CPU time: {stopped}")
        return stopped["stop_cpu_ms"] / 1000.0

    async def check_restored(what: str) -> None:
        for name, position in (("B1", [1, 0, 0]), (f"B{n}", [n % 30, 0, n // 30])):
            got = (await ed.call("get_entity", scene=sid, entity=name, components=["transform", "mesh"]))["components"]
            ed.check_eq([round(float(v), 4) for v in got["transform"]["position"]], position, f"{name}'s position after Stop ({what})")
            ed.check_eq(got["mesh"].get("castShadows"), True, f"{name}'s castShadows after Stop ({what})")

    totals = dict.fromkeys(change, 0.0)
    for _ in range(ROUNDS):
        cost = {what: await stop_cpu(what) for what in change}
        await check_restored("every kind of Play")
        for what in change:
            totals[what] += cost[what]
        print(f"[S71] Stop main-thread CPU on {n} boxes: {cost['mesh']:.2f} s after changing each Mesh, "
              f"{cost['move']:.2f} s after moving each, {cost['destroy']:.2f} s rebuilding them")
    rebuilt = max(totals["destroy"], 0.001)
    changed = round(totals["mesh"] / rebuilt, 2)
    moved = round(totals["move"] / rebuilt, 2)
    print(f"[S71] against rebuilding the boxes: changed Meshes {changed}, moved {moved}")

    # After the last Stop: nothing differs, and the scene saves as before Play once a frame has
    # derived the bounds of the restored Transforms again
    scene = next(entry for entry in (await ed.call("list_scenes"))["scenes"] if entry["id"] == sid)
    ed.check(not scene["modified"], "not modified after Stop")
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)
    ed.check((ed.project / rel).read_bytes() == saved, "the scene saves after Stop as before Play (the Meshes' worldAABB included)")

    # budgets of CPU time: a busy machine's other threads move these ratios too, so the benchmark run enforces them
    over = over_budget("S71 Stop decoding changed Meshes, against rebuilding the boxes", {"changed": changed}, 3.2, "x")
    over |= over_budget("S71 Stop after a Play that only moved the boxes, against rebuilding them", {"moved": moved}, 2.5, "x")
    ed.check(not over, f"Stop costs what it should against rebuilding the boxes: {over}")
