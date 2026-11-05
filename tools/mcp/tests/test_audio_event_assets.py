"""Sound Events as assets (docs/specs/S37-sound-events.md R2, R3; AC2): moving a clip relinks the
event's path and its undo puts it back, deleting it shows the event in the Missing list, the
dependency query lists the event's clips, and a renamed event keeps its GUID and its scene references.
"""

import json

from audio_refs import write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Event Assets"}
CI = True

EVENT = "events/steps.soundevent"


def event_json(ed: Editor, path: str = EVENT) -> dict:
    return json.loads((ed.project / path).read_text())


async def guid_of(ed: Editor, path: str) -> str:
    return (await ed.call("resolve_asset", asset=path))["guid"]


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/a.wav")
    write_tone(ed.project / "sounds/b.wav")
    # by name: a full sync this soon after the start can compare against a baseline that already holds them
    await ed.call("sync_files", paths=["sounds/a.wav", "sounds/b.wav"])
    a = await guid_of(ed, "sounds/a.wav")
    b = await guid_of(ed, "sounds/b.wav")
    body = {"format": "jasperengine.soundevent", "version": 1, "mode": "shuffle", "clips": [
        {"clip": {"guid": a, "path": "sounds/a.wav"}},
        {"clip": {"guid": b, "path": "sounds/b.wav"}, "weight": 2}]}
    await ed.call("write_file", path=EVENT, content=json.dumps(body, indent=2))
    await ed.call("sync_files")
    ed.check((ed.project / (EVENT + ".meta")).is_file(), "the event got its .meta")
    meta = (ed.project / (EVENT + ".meta")).read_text()
    guid = await guid_of(ed, EVENT)
    ed.check(guid in meta, f"the GUID is the .meta's: {meta}")
    ed.check(guid not in (ed.project / EVENT).read_text(), "the file body holds no GUID")


async def test_dependencies_list_the_clips(ed: Editor):
    deps = await ed.call("get_asset_dependencies", asset=EVENT)
    paths = sorted(d["target"]["path"] for d in deps["dependencies"])
    ed.check_eq(paths, ["sounds/a.wav", "sounds/b.wav"], f"{deps}")
    fields = sorted(d["field"] for d in deps["dependencies"])
    ed.check_eq(fields, ["clips[0].clip", "clips[1].clip"], f"{deps}")
    refs = await ed.call("find_references", asset="sounds/a.wav")
    ed.check(EVENT in json.dumps(refs), f"the clip's references list the event: {refs}")


async def test_moving_a_clip_relinks_and_its_undo_puts_it_back(ed: Editor):
    guid = await guid_of(ed, "sounds/a.wav")
    await ed.call("create_folder", path="moved")
    await ed.call("move_asset", path="sounds/a.wav", to="moved/a2.wav")
    clip = event_json(ed)["clips"][0]["clip"]
    ed.check_eq(clip["path"], "moved/a2.wav", "the path follows the GUID")
    ed.check_eq(clip["guid"], guid, "the GUID stays")
    await ed.call("undo", scope="project")
    ed.check((ed.project / "sounds/a.wav").exists(), "the move was undone")
    ed.check_eq(event_json(ed)["clips"][0]["clip"]["path"], "sounds/a.wav", "one undo step puts the path back")


async def test_a_deleted_clip_is_missing(ed: Editor):
    await ed.call("delete_asset", paths=["sounds/b.wav"])
    listing = await ed.call("list_missing_assets")
    ed.check(EVENT in json.dumps(listing) and "sounds/b.wav" in json.dumps(listing), f"the event names the missing clip: {listing}")
    await ed.call("undo", scope="project")
    ed.check((ed.project / "sounds/b.wav").exists(), "the delete was undone")
    listing = await ed.call("list_missing_assets")
    ed.check("sounds/b.wav" not in json.dumps(listing), f"found again: {listing}")


async def test_renaming_the_event_keeps_guid_and_scene_reference(ed: Editor):
    scene = (await ed.call("create_scene", name="Refs", type="3D"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, name="Steps", type="audio_source")
    guid = await guid_of(ed, EVENT)
    await ed.call("set_properties", scene=scene, entity="Steps", values={"audiosource": {"event": EVENT}})
    await ed.call("save_scene", scene=scene, path="scenes/Refs.scene")
    before = (ed.project / "scenes/Refs.scene").read_text()
    ed.check(guid in before, f"the scene holds the event's GUID: {before}")

    await ed.call("move_asset", path=EVENT, to="events/walk.soundevent")
    ed.check_eq(await guid_of(ed, "events/walk.soundevent"), guid, "the GUID moved with it")
    got = await ed.call("get_entity", scene=scene, entity="Steps", components=["audiosource"], properties=["event"])
    ed.check("events/walk.soundevent" in json.dumps(got), f"the open scene follows: {got}")
    await ed.call("undo", scope="project")
    ed.check_eq(await guid_of(ed, EVENT), guid, "and back")
