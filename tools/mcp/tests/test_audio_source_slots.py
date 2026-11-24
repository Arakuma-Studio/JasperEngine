"""Several Audio Sources on one GameObject (docs/specs/S97-audio-gaps.md REQ-24 to REQ-28, AC24): a
second add_component AudioSource makes a slot, a hidden child entity returned by id; the scene tree
and find_entities list only the GameObject, get_entity lists its audio_sources, set_properties and
remove_component work on the slot's id, each source keeps its own effects (REQ-30), both play, and the
slot survives a save and reopen.
"""

from audio_refs import TRACE_ENV, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Source Slots", "env": TRACE_ENV}
CI = True

PATH = "scenes/Slots.scene"
STATE = {}


def names(nodes: list) -> list:
    out = []
    for node in nodes:
        out.append(node["name"])
        out.extend(names(node.get("children", [])))
    return out


def audio_props(entity: dict) -> dict:
    comps = entity["components"]
    return comps[next(k for k in comps if "audio" in k.lower())]


async def test_second_add_component_makes_a_slot(ed: Editor):
    write_tone(ed.project / "sounds/engine.wav", seconds=2.0)
    write_tone(ed.project / "sounds/horn.wav", seconds=2.0, frequency=600.0)
    await ed.call("sync_files")
    scene = (await ed.call("create_scene", name="Slots", type="3D", path=PATH))["scene"]["id"]
    STATE["scene"] = scene
    car = (await ed.call("create_entity", scene=scene, name="Car", type="empty"))["entity_id"]
    STATE["car"] = car
    loop = {"looping": True, "state": "playing"}
    first = await ed.call("add_component", scene=scene, entity=car, component="AudioSource",
                          values={"AudioSource": {"filename": "sounds/engine.wav", **loop}})
    ed.check("audio_source" not in first["added"][0], f"the first is the GameObject's own: {first}")
    second = await ed.call("add_component", scene=scene, entity=car, component="AudioSource",
                           values={"AudioSource": {"filename": "sounds/horn.wav", **loop}})
    added = second["added"][0]
    slot = added.get("audio_source")
    ed.check(isinstance(slot, int) and slot != car, f"a slot entity: {second}")
    ed.check_eq(added.get("slot"), 1, "slot 1")
    ed.check("set_properties" in second.get("next", ""), f"next names how to address it: {second}")
    STATE["slot"] = slot

    tree = await ed.call("get_scene_tree", scene=scene, format="json")
    listed = names(tree["nodes"])
    ed.check_eq(listed.count("Car"), 1, f"the GameObject: {listed}")
    ed.check(not any(n.startswith("Audio Source") for n in listed), f"no slot row: {listed}")
    found = await ed.call("find_entities", scene=scene, component="AudioSource")
    ed.check_eq([e["id"] for e in found["entities"]], [car], f"one entity with an Audio Source: {found}")

    entity = await ed.call("get_entity", scene=scene, entity=car)
    ed.check_eq(entity.get("audio_sources"), [{"entity": car, "slot": 0}, {"entity": slot, "slot": 1}], "audio_sources")
    own = audio_props(entity)
    ed.check_eq(own["filename"], "sounds/engine.wav", "the values of the first went to the GameObject")

    await ed.call("set_properties", scene=scene, entity=slot, values={"AudioSource": {"volume": 0.5}})
    horn = await ed.call("get_entity", scene=scene, entity=slot)
    ed.check_eq(audio_props(horn)["filename"], "sounds/horn.wav", "the second add's values went to the slot")
    ed.check_eq(audio_props(horn)["volume"], 0.5, "set_properties on the slot's id")
    ed.check_eq(audio_props(await ed.call("get_entity", scene=scene, entity=car))["volume"], 1, "the GameObject's own is untouched")

    # REQ-30: each source has its own effects
    await ed.call("set_properties", scene=scene, entity=slot,
                  values={"AudioSource": {"effects": [{"type": "lowpass", "params": {"cutoff": 500}}]}})
    ed.check_eq([e["type"] for e in audio_props(await ed.call("get_entity", scene=scene, entity=slot))["effects"]], ["lowpass"],
                "the slot's effect")
    ed.check_eq(audio_props(await ed.call("get_entity", scene=scene, entity=car)).get("effects") or [], [], "none on the GameObject's own")


async def test_slot_survives_save_and_reopen(ed: Editor):
    scene, car, slot = STATE["scene"], STATE["car"], STATE["slot"]
    await ed.call("save_scene", scene=scene)
    await ed.call("create_scene", name="Other", type="3D")
    await ed.call("close_scene", scene=scene, unsaved="cancel")
    reopened = await ed.call("open_scene", scene=PATH)
    scene = reopened["scene"]["id"]
    STATE["scene"] = scene
    entity = await ed.call("get_entity", scene=scene, entity=car)
    ed.check_eq(entity.get("audio_sources"), [{"entity": car, "slot": 0}, {"entity": slot, "slot": 1}], "after reopening")
    ed.check_eq(audio_props(await ed.call("get_entity", scene=scene, entity=slot))["volume"], 0.5, "its values kept")


async def test_both_sources_play(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=10)
        state = await ed.call("get_play_state")
        ed.check(state["audio"]["voices"] >= 2, f"both play: {state['audio']}")
    finally:
        await ed.call("play", action="stop")


async def test_remove_component_on_the_slot(ed: Editor):
    scene, car, slot = STATE["scene"], STATE["car"], STATE["slot"]
    await ed.call("remove_component", scene=scene, entity=slot, component="AudioSource")
    entity = await ed.call("get_entity", scene=scene, entity=car)
    ed.check("audio_sources" not in entity, f"the slot went with its entity: {entity.get('audio_sources')}")
    ed.check_eq(entity["child_count"], 0, "no child left behind")
    await ed.call("undo", scope="scene", scene=scene)
    entity = await ed.call("get_entity", scene=scene, entity=car)
    ed.check_eq(entity.get("audio_sources"), [{"entity": car, "slot": 0}, {"entity": slot, "slot": 1}], "undo brings it back")
    # delete_entities on the slot removes it too
    await ed.call("delete_entities", scene=scene, entity=slot)
    ed.check("audio_sources" not in await ed.call("get_entity", scene=scene, entity=car), "deleted")
