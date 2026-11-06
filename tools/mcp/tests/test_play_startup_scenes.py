"""Play's startup reads the scenes on the main thread, and agents' edits wait for it.

Project::runPlayStartup used to save the scenes, write their generated sources, make the runtime
copies and take the Play snapshot on its worker thread while the main thread kept changing them
(MCP edits, the other scenes' views making gizmos, a scene opened or created). That part now runs as
one main-thread task (Project::preparePlayStartup); the worker only builds shaders and C++ scripts.
While Play is starting (its scene LOADING) MCP refuses edits and project switches, with a message.
This plays a two-scene project (a child scene that starts active, both with Lua scripts, the child
open in its own tab) several times, stopping during the startup too.
"""

from __future__ import annotations

from harness import Editor, find_key

EDITOR = {"create": True, "name": "Play Startup Scenes"}
CI = True
TIMEOUT = 600

S: dict = {}


def probe_script(tag: str) -> str:
    return f"""local Probe = {{}}
function Probe:init()
    Log.print("{tag}-INIT")
end
return Probe
"""


async def batch_results(ed: Editor, calls: list[tuple[str, dict]]) -> list[dict]:
    return (await ed.batch(calls, stop_on_error=False))["results"]


async def test_edits_refused_while_play_starts(ed: Editor):
    child = (await ed.call("create_scene", name="StartupChild", type="3D"))["scene"]["id"]
    made = await ed.call("create_entity", scene=child, name="ChildProbe", type="cube", position=[0, 1, 0],
                         components=["Rigidbody"])
    await ed.call("create_script", kind="lua", name="ChildProbe", content=probe_script("CHILD"), scene=child,
                  entity=made["entity_id"])
    await ed.call("create_entity", scene=child, name="ChildLight", type="point_light", position=[0, 3, 0])
    await ed.call("save_scene", scene=child, path="StartupChild.scene")

    main = (await ed.call("create_scene", name="StartupMain", type="3D"))["scene"]["id"]
    made = await ed.call("create_entity", scene=main, name="MainProbe", type="cube", position=[2, 1, 0])
    await ed.call("create_script", kind="lua", name="MainProbe", content=probe_script("MAIN"), scene=main,
                  entity=made["entity_id"])
    await ed.call("set_child_scene", scene=main, child=child, start_active=True)
    await ed.call("save_scene", scene=main, path="StartupMain.scene")
    S.update(main=main, child=child)

    main, child = S["main"], S["child"]
    results = await batch_results(ed, [
        ("play", {"action": "start", "scene": main, "wait": False}),
        # in the same frame: the scene is LOADING
        ("create_entity", {"scene": child, "name": "TooEarly", "type": "cube"}),
        ("set_transform", {"scene": main, "entity": "MainProbe", "position": [5, 5, 5]}),
        ("open_project", {"path": str(ed.scratch.root / "no-such-project")}),
        ("eval_lua", {"code": "return 1", "allow_edit_mode": True}),
        # these only drive the game: not refused for the startup (they answer about it themselves)
        ("wait", {"frames": 1}),
        ("send_input", {"events": [{"type": "key", "key": "space"}]}),
        ("get_play_state", {}),
    ])
    try:
        ed.check(results[0]["ok"], f"play started: {results[0]}")
        job = find_key(results[0], "job_id") or results[0]["text"].split("job ")[1].split(")")[0]
        for item in results[1:3] + [results[4]]:
            ed.check(not item["ok"] and "is refused while Play mode is starting" in item["text"],
                     f"{item['tool']} refused while Play starts: {item}")
            ed.check(item["text"].startswith("This call") and f'get_job with job_id "{job}"' in item["text"],
                     f"{item['tool']}: neutral wording, the play job to poll: {item['text']}")
        # (a project switch is refused while any scene plays or starts)
        ed.check(not results[3]["ok"] and ("Play mode is starting" in results[3]["text"] or "playing" in results[3]["text"]),
                 f"open_project refused while Play starts: {results[3]}")
        for item in results[5:7]:
            ed.check("refused while Play mode is starting" not in item["text"], f"{item['tool']} not refused: {item}")
        ed.check(results[7]["ok"] and "loading" in results[7]["text"].lower(), f"still starting: {results[7]}")
        # joining the startup waits for it
        started = await ed.call("play", action="start", scene=main)
        ed.check_eq(started["state"], "playing", f"started: {started}")
    finally:
        await ed.call("play", action="stop")
    await ed.check_error("get_entity", {"scene": child, "entity": "TooEarly"})
    probe = await ed.call("get_entity", scene=main, entity="MainProbe")
    position = [round(v, 4) for v in find_key(probe, "position")]
    ed.check_eq(position, [2, 1, 0], "the refused move did nothing")
    # edits work again once stopped
    await ed.call("create_entity", scene=child, name="AfterStop", type="cube")
    await ed.call("delete_entities", scene=child, entity="AfterStop")


async def test_two_scene_play_repeats(ed: Editor):
    main, child = S["main"], S["child"]
    for cycle in range(4):
        # the child's own tab in front on odd cycles: its Scene view draws (and makes gizmos) while
        # the played scene starts
        await ed.call("focus_window", window="scene", scene=child if cycle % 2 else main)
        cursor = (await ed.call("read_console", limit=1))["next_cursor"]
        if cycle == 2:
            # stopped during the startup
            await ed.call("play", action="start", scene=main, wait=False)
            await ed.call("play", action="stop")
            continue
        await ed.call("play", action="start", scene=main, auto_pause=True, fixed_fps=60)
        try:
            await ed.call("wait", frames=2)
            logs = [e["message"] for e in (await ed.call("read_console", since=cursor))["entries"]]
            for tag in ("MAIN-INIT", "CHILD-INIT"):
                ed.check(any(tag in m for m in logs), f"cycle {cycle}: {tag} logged: {logs[-20:]}")
            listed = await ed.call_raw("list_runtime_entities", {})
            ed.check(not listed.is_error and "MainProbe" in listed.text, f"cycle {cycle}: it runs: {listed.text[:600]}")
        finally:
            await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
        ed.check_eq((await ed.call("get_play_state"))["state"], "stopped")
    await ed.check_alive()
    for scene, name in ((main, "MainProbe"), (child, "ChildProbe"), (child, "ChildLight")):
        await ed.call("get_entity", scene=scene, entity=name)
