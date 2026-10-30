"""S26 R35, AC17, AC18 (docs/specs/S26-scene-view.md): MeshRaycast, the triangle-exact raycast
without physics, from Lua in play mode and through MCP (raycast_meshes), against a static mesh, a
skinned mesh (posed on the CPU: its bent tip is hit where it is drawn) and an instanced mesh;
raycastAll's order, the layer mask, ignore, the two distance semantics (a Ray's fraction, world
units with origin/direction/maxDistance), and prepare() building ahead of the raycasts.

test_web_latency runs the WebAssembly build of tools/test-projects/mesh-raycast in headless
Chrome (the S02 web gate, tools/web/gates/mesh-raycast.json): 10,000 rays against 1,000 meshes of
about 1,000 triangles, p99 at most 4 ms. It needs that web build and Chrome: the full suite
(tools/build.sh mcp-tests with MCP_SUITE=all) builds it first (WEB_PROJECT=mesh-raycast tools/build.sh
web) and the linux image has chrome-headless-shell; elsewhere it is skipped without them.
"""

from __future__ import annotations

import shutil
import subprocess
import sys

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Mesh Raycast"}
TIMEOUT = 900

S: dict = {}

GLTF = harness.REPO / "tools" / "fixtures" / "shadows" / "assets" / "skinned_morph.gltf"


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


async def lua(ed: Editor, code: str):
    """The chunk's one return value (eval_lua answers {values: [...]})"""
    values = (await ed.call("eval_lua", code=code))["values"]
    return values[0] if len(values) == 1 else values


async def setup(ed: Editor):
    if "scene" in S:
        return S
    made = await ed.call("create_scene", name="Rays", type="3D", path="scenes/Rays")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    (ed.project / "assets" / "models").mkdir(parents=True, exist_ok=True)
    shutil.copy(GLTF, ed.project / "assets" / "models" / "skinned_morph.gltf")
    await ed.call("sync_files")
    S["scene"] = sid
    S["static"] = (await ed.call("create_entity", scene=sid, type="cube", name="Static", position=[3, 0, 0], select=False))["entity_id"]
    S["second"] = (await ed.call("create_entity", scene=sid, type="cube", name="Second", position=[3, 0, -3], select=False))["entity_id"]
    S["layered"] = (await ed.call("create_entity", scene=sid, type="cube", name="Layered", position=[-3, 0, 0], select=False))["entity_id"]
    await ed.call("set_entity_state", scene=sid, entity="Layered", layer=9)
    S["inst"] = (await ed.call("create_entity", scene=sid, type="cube", name="Inst", position=[0, -5, 0], select=False))["entity_id"]
    S["model"] = (await ed.call("create_entity", scene=sid, type="model", name="Strip", model="assets/models/skinned_morph.gltf",
                                select=False))["entity_id"]
    await ed.call("create_entity", scene=sid, type="camera", name="Cam", position=[0, 1, 10], select=False)
    await ed.screenshot("scene", scene=sid, max_width=256)
    await ed.call("save_scene", scene=sid)
    return S


async def test_raycast_meshes_tool(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    # world units: the near face of Static at z = 0.5, 9.5 from the origin
    got = await ed.call("raycast_meshes", scene=sid, origin=[3, 0, 10], direction=[0, 0, -1])
    ed.check(got["hits"] and got["hits"][0]["entity"] == s["static"], f"Static first: {got['hits']}")
    ed.check(close(got["hits"][0]["distance"], 9.5, 1e-4), f"world distance: {got['hits'][0]}")
    ed.check(close(got["hits"][0]["normal"], [0, 0, 1], 1e-4), "the normal faces the ray")
    # all: nearest first, the cube behind it too
    got = await ed.call("raycast_meshes", scene=sid, origin=[3, 0, 10], direction=[0, 0, -1], all=True)
    order = [h["entity"] for h in got["hits"]]
    ed.check(order[:2] == [s["static"], s["second"]], f"raycastAll nearest first: {order}")
    ed.check(all(a["distance"] <= b["distance"] for a, b in zip(got["hits"], got["hits"][1:])), "sorted")
    # max_distance and the layer mask
    got = await ed.call("raycast_meshes", scene=sid, origin=[3, 0, 10], direction=[0, 0, -1], max_distance=5)
    ed.check(got["hits"] == [], "nothing within 5")
    got = await ed.call("raycast_meshes", scene=sid, origin=[-3, 0, 10], direction=[0, 0, -1])
    ed.check(got["hits"] and got["hits"][0]["entity"] == s["layered"], "on layer 9 by default")
    got = await ed.call("raycast_meshes", scene=sid, origin=[-3, 0, 10], direction=[0, 0, -1], layer_mask=1)
    ed.check(got["hits"] == [], "layer 9 masked out")
    # backfaces: from inside the cube
    got = await ed.call("raycast_meshes", scene=sid, origin=[3, 0, 0], direction=[0, 0, -1])
    ed.check(not got["hits"] or got["hits"][0]["entity"] != s["static"], "from inside: its faces are back faces")
    got = await ed.call("raycast_meshes", scene=sid, origin=[3, 0, 0], direction=[0, 0, -1], backfaces=True)
    ed.check(got["hits"] and got["hits"][0]["entity"] == s["static"] and close(got["hits"][0]["distance"], 0.5, 1e-4),
             f"hitBackfaces hits its far wall: {got['hits'][:1]}")
    ed.check("built" in got["stats"] and "inline_waits" in got["stats"], f"stats: {got['stats']}")


async def test_lua_raycasts_in_play(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=10)
        # fraction of the Ray against world units of the origin/direction/maxDistance overload
        got = await lua(ed, """
            local a = MeshRaycast.raycast(scene, Ray(Vector3(3, 0, 10), Vector3(0, 0, -20)))
            local b = MeshRaycast.raycast(scene, Vector3(3, 0, 10), Vector3(0, 0, -1), 100)
            return {hitA = a.hit, fraction = a.distance, hitB = b.hit, world = b.distance, name = scene:getEntityName(b.entity),
                    kindMesh = b.kind == MeshRaycast.MESH, px = b.point.x, nz = b.normal.z, tri = b.triangle}
        """)
        ed.check(got["hitA"] and got["hitB"], f"both overloads hit: {got}")
        ed.check(close(got["fraction"], 9.5 / 20, 1e-4), f"a Ray's distance is the fraction of its direction: {got}")
        ed.check(close(got["world"], 9.5, 1e-4), f"world units with maxDistance: {got}")
        ed.check(got["name"] == "Static" and got["kindMesh"] and close(got["px"], 3, 1e-4) and close(got["nz"], 1, 1e-4), f"{got}")
        # raycastAll order, the layer mask and ignore
        got = await lua(ed, """
            local all = MeshRaycast.raycastAll(scene, Vector3(3, 0, 10), Vector3(0, 0, -1), 100)
            local names = {}
            for i, h in ipairs(all) do names[i] = scene:getEntityName(h.entity) end
            local masked = MeshRaycast.raycast(scene, Vector3(-3, 0, 10), Vector3(0, 0, -1), 100, 1)
            local static = entity("Static")
            local ignored = MeshRaycast.raycast(scene, Vector3(3, 0, 10), Vector3(0, 0, -1), 100, LayerMask.EVERYTHING or 0xFFFFFFFF,
                                                {ignore = {static}})
            return {names = names, masked = masked.hit, ignored = scene:getEntityName(ignored.entity)}
        """)
        ed.check(got["names"][:2] == ["Static", "Second"], f"raycastAll nearest first: {got}")
        ed.check(got["masked"] is False, "layer mask")
        ed.check(got["ignored"] == "Second", f"ignore skips Static: {got}")
        # an instanced mesh: instance 0 at (0, 3, 0) of Inst (at y = -5): world (0, -2, 0)
        got = await lua(ed, """
            local inst = object("Inst", "Mesh")
            inst:addInstance(Vector3(0, 3, 0))
            inst:addInstance(Vector3(4, 3, 0))
            return true
        """)
        await ed.call("wait", frames=3)
        got = await lua(ed, """
            local h = MeshRaycast.raycast(scene, Vector3(4, -2, 10), Vector3(0, 0, -1), 100)
            return {hit = h.hit, name = scene:getEntityName(h.entity), instance = h.instance, kind = h.kind == MeshRaycast.INSTANCE}
        """)
        ed.check(got["hit"] and got["name"] == "Inst" and got["instance"] == 1 and got["kind"], f"instance 1 of Inst: {got}")
        # the skinned strip: bend its tip 90 degrees about z; the bind-pose top is empty then and the
        # bent tip is hit where it is drawn
        got = await lua(ed, """
            local h = MeshRaycast.raycast(scene, Vector3(0, 1.9, 5), Vector3(0, 0, -1), 100)
            return {hit = h.hit, name = h.hit and scene:getEntityName(h.entity) or ""}
        """)
        ed.check(got["hit"], f"the bind-pose strip at (0, 1.9): {got}")
        await lua(ed, """
            local tip = object("Tip")
            tip.rotation = Quaternion(90, Vector3(0, 0, 1))
            return true
        """)
        await ed.call("wait", frames=4)
        got = await lua(ed, """
            local old = MeshRaycast.raycast(scene, Vector3(0, 1.9, 5), Vector3(0, 0, -1), 100)
            local bent = MeshRaycast.raycast(scene, Vector3(-0.8, 1.0, 5), Vector3(0, 0, -1), 100)
            return {old = old.hit, bent = bent.hit, name = bent.hit and scene:getEntityName(bent.entity) or ""}
        """)
        ed.check(got["old"] is False, f"the bind-pose top is empty once bent: {got}")
        ed.check(got["bent"], f"the bent tip is hit where it is drawn: {got}")
        # prepare() builds ahead: the raycasts after it wait for nothing
        got = await lua(ed, """
            MeshRaycast.prepare(scene)
            return true
        """)
        await ed.call("wait", frames=30)
        before = (await ed.call("raycast_meshes", scene=s["scene"], origin=[100, 100, 100], direction=[0, 0, 1]))["stats"]
        await lua(ed, "return MeshRaycast.raycastAll(scene, Vector3(0, 1, 10), Vector3(0, 0, -1), 100)[1] ~= nil")
        after = (await ed.call("raycast_meshes", scene=s["scene"], origin=[100, 100, 100], direction=[0, 0, 1]))["stats"]
        ed.check(after["inline_waits"] == before["inline_waits"], f"no inline build after prepare(): {before} -> {after}")
    finally:
        await ed.call("play", action="stop")


async def test_raycast_entity_lua(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=5)
        got = await lua(ed, """
            local ray = Ray(Vector3(3, 0, 10), Vector3(0, 0, -20))
            local only = MeshRaycast.raycastEntity(scene, entity("Second"), ray)
            local none = MeshRaycast.raycastEntity(scene, entity("Layered"), ray)
            return {only = only.hit, name = scene:getEntityName(only.entity), fraction = only.distance, none = none.hit}
        """)
        ed.check(got["only"] and got["name"] == "Second" and close(got["fraction"], 12.5 / 20, 1e-4), f"raycastEntity: {got}")
        ed.check(got["none"] is False, "another GameObject is not tested")
    finally:
        await ed.call("play", action="stop")


async def test_web_latency():
    root = harness.REPO / "build" / "web-mesh-raycast"
    gate = harness.REPO / "tools" / "web" / "gates" / "mesh-raycast.json"
    if not (root / "jasperengine-project.html").exists():
        harness.skip(f"no web build at {root} (WEB_PROJECT=mesh-raycast tools/build.sh web)")
    chrome = shutil.which("chrome-headless-shell") or shutil.which("google-chrome") or shutil.which("chromium")
    if not chrome:
        harness.skip("no Chrome for the web gate")
    run = subprocess.run([sys.executable, str(harness.REPO / "tools" / "web" / "browser_gate.py"), "--root", str(root),
                          "--page", "jasperengine-project.html", "--until-ready", "--baseline", str(gate), "--timeout", "240"],
                         capture_output=True, text=True, timeout=600)
    print(run.stdout[-3000:])
    if run.returncode != 0:
        raise AssertionError(f"web gate failed ({run.returncode}):\n{run.stdout[-3000:]}\n{run.stderr[-2000:]}")
