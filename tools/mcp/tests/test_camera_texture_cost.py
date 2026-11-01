"""Binding camera textures does not walk the Scene view's own line renderers.

CameraTextureLink::resolve(scene) visits every component of the scene's registry, the editor's own
included, and builds the property map of each one that may hold a reference. The Scene view's
overlay line renderers (grid, selection and gizmo lines) list four properties per line, so a
whole-scene bind cost tens of milliseconds in a 3D scene:
- a prefab apply reimports every instance, and bound the whole scene per instance (300 instances:
  about 27 s, against the 2.5 s of instantiating them); it binds the reimported branch only;
- removing, adding, pasting or deleting a camera binds the whole scene; it costs about what the
  same step costs without a camera, since line renderers hold no references.

Oracles: the main thread's CPU time (/proc), the scene and prefab files (undo and redo exact).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from harness import Editor, skip
from scene_history import SCENE_INPUTS, first_difference

TIMEOUT = 2400
CI = True
EDITOR = {"project": str(SCENE_INPUTS)}


def main_thread_ticks(root: Path) -> tuple:
    """(pid, utime+stime ticks of the main thread, which runs the tool calls) of the editor in `root`"""
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            cwd = os.readlink(f"/proc/{name}/cwd")
            exe = open(f"/proc/{name}/cmdline", "rb").read().split(b"\0")[0].decode()
            stat = open(f"/proc/{name}/task/{name}/stat").read()
        except OSError:
            continue
        if cwd.startswith(str(root)) and "jasperengine-editor" in exe:
            fields = stat[stat.rindex(")") + 2:].split()
            return int(name), int(fields[11]) + int(fields[12])
    return None, 0


async def test_camera_commands_do_not_walk_the_scene_view_lines(ed: Editor):
    """A 3D scene drawn in the Scene view, 20 cameras and 20 GameObjects with an Audio Source. Each
    step below, one call per GameObject in a batch, binds the camera textures of the whole scene when
    a camera comes or goes: removing a component, its undo (adding it back), pasting a copy and
    deleting. Main-thread CPU of each on the cameras against the same on the others: what a camera
    adds to a step is under 35 ms (the CPU clock ticks in 10 ms)"""
    if not sys.platform.startswith("linux") or ed.scratch is None:
        skip("needs /proc and a stdio editor")
    n = 20
    hz = os.sysconf("SC_CLK_TCK")
    sid = (await ed.call("create_scene", name="Cameras", type="3D", path="Cameras.scene"))["scene"]["id"]
    calls = [{"tool": "create_entity", "arguments": {"scene": sid, "type": "camera", "name": f"C{i}", "select": False, "position": [i, 0, 0]}} for i in range(n)]
    calls += [{"tool": "create_entity", "arguments": {"scene": sid, "type": "empty", "components": ["audiosource"], "name": f"A{i}", "select": False,
                                                      "position": [i, 2, 0]}} for i in range(n)]
    made = await ed.call("batch", calls=calls, timeout=900)
    ed.check_eq(made.get("failed"), 0, f"batch: {[r for r in made['results'] if not r.get('ok')]}")
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid, "max_width": 320})
    ed.check(not shot.is_error, f"the Scene view draws: {shot.text[:300]}")
    pid, _ = main_thread_ticks(ed.scratch.root)
    if pid is None:
        skip("editor process not found")

    results = []

    async def cpu(calls: list) -> float:
        _, before = main_thread_ticks(ed.scratch.root)
        r = await ed.call("batch", calls=calls, timeout=900)
        _, after = main_thread_ticks(ed.scratch.root)
        ed.check_eq(r.get("failed"), 0, f"batch: {[x for x in r['results'] if not x.get('ok')][:3]}")
        results[:] = r["results"]
        return (after - before) / hz

    async def undo_cpu(steps: int) -> float:
        _, before = main_thread_ticks(ed.scratch.root)
        await ed.call("undo", scene=sid, steps=steps, timeout=900)
        _, after = main_thread_ticks(ed.scratch.root)
        return (after - before) / hz

    def each(tool: str, prefix: str, **args) -> list:
        return [{"tool": tool, "arguments": dict({"scene": sid, "entity": f"{prefix}{i}"}, **args)} for i in range(n)]

    measured = {}
    for _ in range(2):
        cameras = await cpu(each("remove_component", "C", component="camera"))
        sources = await cpu(each("remove_component", "A", component="audiosource"))
        measured.setdefault("remove component", []).append((cameras, sources))
        # back one per step, the sources' first (the newest): adding a Camera binds the scene
        sources = await undo_cpu(n)
        cameras = await undo_cpu(n)
        measured.setdefault("undo (add it back)", []).append((cameras, sources))

    pasted, took = {}, {}
    for prefix in ("C", "A"):
        await ed.call("copy_entities", scene=sid, entity=f"{prefix}0")
        took[prefix] = await cpu([{"tool": "paste_entities", "arguments": {"scene": sid}}] * n)
        pasted[prefix] = [r["structured"]["created"][0]["id"] for r in results]
    measured["paste"] = [(took["C"], took["A"])]
    for prefix in ("C", "A"):
        took[prefix] = await cpu([{"tool": "delete_entities", "arguments": {"scene": sid, "entity": e}} for e in pasted[prefix]])
    measured["delete"] = [(took["C"], took["A"])]

    extra = {}
    for step, pairs in measured.items():
        extra[step] = [round((c - o) * 1000 / n, 1) for c, o in pairs]
        print(f"[cameras] {step}: " + "; ".join(f"cameras {c * 1000 / n:.1f} ms, others {o * 1000 / n:.1f} ms a step" for c, o in pairs))
    print(f"[cameras] what a camera adds to a step, ms: {extra}")
    slow = {step: values for step, values in extra.items() if min(values) >= 35.0}
    ed.check_eq(slow, {}, "a camera step costs about what the same step costs without a camera")


async def test_prefab_apply_costs_about_an_instantiation_per_instance(ed: Editor):
    """300 instances of the scene-inputs figure model (a model prefab, 3 submeshes). A castShadows
    override of one instance applied to the prefab reimports every instance. Main-thread CPU of the
    apply and of its undo against instantiating the same 300 instances (instantiate_prefab binds the
    new branch only): both ratios under 3. Undo and redo give back the instances and the scene byte
    for byte (and redo the prefab), and every instance follows the prefab"""
    if not sys.platform.startswith("linux") or ed.scratch is None:
        skip("needs /proc and a stdio editor")
    hz = os.sysconf("SC_CLK_TCK")
    rel, prefab_rel = "Figures.scene", "Figure.prefab"
    sid = (await ed.call("create_scene", name="Figures", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="model", name="Fig", model="assets/models/figure.gltf", select=False)
    await ed.call("create_prefab", scene=sid, entity="Fig", path=prefab_rel)
    await ed.call("screenshot", view="scene", scene=sid, max_width=320)
    n = 300

    _, before = main_thread_ticks(ed.scratch.root)
    calls = [{"tool": "instantiate_prefab", "arguments": {"scene": sid, "prefab": prefab_rel, "position": [i % 20, 0, i // 20], "name": f"F{i}"}}
             for i in range(1, n)]
    for k in range(0, len(calls), 100):
        r = await ed.call("batch", calls=calls[k:k + 100], timeout=1800)
        ed.check_eq(r.get("failed"), 0, f"batch: {str(r)[:300]}")
    _, after = main_thread_ticks(ed.scratch.root)
    instantiate = (after - before) / hz

    def files() -> tuple:
        return (ed.project / rel).read_bytes(), (ed.project / prefab_rel).read_bytes()

    async def saved() -> tuple:
        await ed.call("wait", frames=3)
        await ed.call("save_scene", scene=sid)
        return files()

    async def cpu(tool: str, **args) -> float:
        _, before = main_thread_ticks(ed.scratch.root)
        await ed.call(tool, timeout=1800, **args)
        _, after = main_thread_ticks(ed.scratch.root)
        return (after - before) / hz

    async def mesh_shadows(names) -> list:
        return [(await ed.call("get_entity", scene=sid, entity=name, components=["mesh"], properties=["castShadows"]))["components"]["mesh"].get("castShadows")
                for name in names]

    names = ["F1", f"F{n - 1}", "Fig"]
    await ed.call("set_properties", scene=sid, entity="F1", values={"Mesh": {"castShadows": False}})
    start = await saved()
    ed.check_eq(await mesh_shadows(names), [False, True, True], "only F1 overrides castShadows before the apply")
    apply = await cpu("prefab_overrides", action="apply", scene=sid, entity="F1", component="mesh", properties=["castShadows"])
    ed.check_eq(await mesh_shadows(names), [False] * 3, "the instances follow the prefab")
    applied = await saved()
    ed.check(b"castShadows: false" in applied[1], "the override went into the prefab")
    undo = await cpu("undo", scene=sid)
    ed.check_eq(await mesh_shadows(names), [False, True, True], "undo: the override and the prefab back")
    undone = await saved()
    ed.check(undone[0] == start[0], f"undo gives back the scene byte for byte: {first_difference(start[0], undone[0])}")
    await cpu("redo", scene=sid)
    ed.check_eq(await mesh_shadows(names), [False] * 3, "redo")
    redone = await saved()
    ed.check(redone == applied, f"redo gives back the applied scene and prefab byte for byte: "
                                f"{first_difference(applied[0], redone[0])}\n{first_difference(applied[1], redone[1])}")
    print(f"[apply] {n} instances: apply {apply:.2f} s, undo {undo:.2f} s, instantiating them {instantiate:.2f} s main-thread CPU")
    ratios = [round(apply / max(instantiate, 1 / hz), 2), round(undo / max(instantiate, 1 / hz), 2)]
    print(f"[apply] against instantiating the instances: apply x{ratios[0]}, undo x{ratios[1]}")
    ed.check(max(ratios) < 3.0, f"the apply and its undo cost about as much as instantiating the instances: {ratios}")
