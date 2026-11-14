"""An Audio Source's own effects (docs/specs/S97-audio-gaps.md REQ-1, REQ-5, REQ-41, REQ-42; AC7):
add_component "Audio Low Pass Filter" gives an empty GameObject an Audio Source holding one lowpass
and undo removes both in one step, a second filter alias goes on the same source, set_properties
refuses an effect type or parameter a source has not with the valid ones in the error, and in Play
the Lua AudioSourceEffect sets and moves a parameter the component then holds.
"""

from audio_refs import write_tone
import harness
from harness import Editor

EDITOR = {"create": True, "name": "Audio Source Effects"}
CI = True
TIMEOUT = 600


def audio_of(entity: dict) -> dict | None:
    comps = entity["components"]
    key = next((k for k in comps if k.lower() in ("audiosource", "audiosourcecomponent")), None)
    return comps[key] if key else None


async def test_add_component_filter_aliases(ed: Editor):
    await ed.call("create_entity", name="Radio", type="empty")
    got = await ed.call("get_entity", entity="Radio")
    ed.check(audio_of(got) is None, "an empty GameObject")

    added = await ed.call("add_component", entity="Radio", component="Audio Low Pass Filter")
    ed.check_eq([a.get("effect") for a in added["added"]], ["lowpass"], f"{added}")
    audio = audio_of(await ed.call("get_entity", entity="Radio"))
    ed.check(audio is not None, "the Audio Source came first")
    ed.check_eq([e["type"] for e in audio["effects"]], ["lowpass"], f"{audio.get('effects')}")

    await ed.call("undo")
    ed.check(audio_of(await ed.call("get_entity", entity="Radio")) is None, "one undo removes the effect and the source")
    await ed.call("redo")
    await ed.call("add_component", entity="Radio", component=["AudioEchoFilter"])
    audio = audio_of(await ed.call("get_entity", entity="Radio"))
    ed.check_eq([e["type"] for e in audio["effects"]], ["lowpass", "echo"], "a second filter goes on the same source")


async def test_set_properties_refuses_what_a_source_cannot_hold(ed: Editor):
    error = await ed.call("set_properties", entity="Radio", expect_error=True,
                          values={"audiosource": {"effects": [{"type": "muffle"}]}})
    ed.check("lowpass" in error and "highpass" in error and "muffle" in error, f"the valid types: {error}")
    error = await ed.call("set_properties", entity="Radio", expect_error=True,
                          values={"audiosource": {"effects": [{"type": "limiter"}]}})
    ed.check("audio_mixer" in error, f"a group-only effect points at the mixer: {error}")
    error = await ed.call("set_properties", entity="Radio", expect_error=True,
                          values={"audiosource": {"effects": [{"type": "lowpass", "params": {"frequency": 500}}]}})
    ed.check("cutoff" in error and "resonance" in error, f"the valid parameters: {error}")
    error = await ed.call("set_properties", entity="Radio", expect_error=True,
                          values={"audiosource": {"effects": [{"type": "lowpass"}] * 5}})
    ed.check("4" in error, f"up to 4: {error}")

    await ed.call("set_properties", entity="Radio",
                  values={"audiosource": {"effects": [{"type": "lowpass", "params": {"cutoff": 500}}]}})
    audio = audio_of(await ed.call("get_entity", entity="Radio"))
    ed.check_eq(audio["effects"][0]["params"]["cutoff"], 500, f"{audio['effects']}")


async def test_lua_moves_a_parameter_in_play(ed: Editor):
    write_tone(ed.project / "sounds/tone.wav", seconds=2.0)
    await ed.call("sync_files")
    await ed.call("set_properties", entity="Radio",
                  values={"audiosource": {"filename": "sounds/tone.wav", "looping": True, "state": "playing"}})
    await ed.call("play", action="start", auto_pause=False)
    try:
        await ed.call("wait", seconds=0.2)
        got = (await ed.call("eval_lua", code="""
            local src = object('Radio', 'AudioSource')
            local lp = src:getEffect('lowpass')
            moved = lp:setParameter('cutoff', 2000, 0.3)
            return {lp.type, lp.index, src:getEffectCount(), lp:hasParameter('resonance'), lp:hasParameter('decay'), moved}""")
        )["values"][0]
        ed.check_eq(got, ["lowpass", 1, 1, True, False, True], "AudioSourceEffect")
        await ed.call("wait", seconds=0.6)
        got = (await ed.call("eval_lua", code="""
            return object('Radio', 'AudioSource'):getEffect(1):getParameter('cutoff')"""))["values"][0]
        ed.check(abs(got - 2000) < 1, f"the timed change ended at 2000 Hz: {got}")
        runtime = await ed.call("get_runtime_entity", entity="Radio", include_script_state=False)
        # the list whole, or flattened into its elements' paths
        effects = harness.find_key(runtime, "effects")
        params = effects[0]["params"] if isinstance(effects, list) and effects else harness.find_key(runtime, "effects[0].params")
        cutoff = params.get("cutoff") if isinstance(params, dict) else harness.find_key(runtime, "effects[0].params.cutoff")
        ed.check(isinstance(cutoff, (int, float)) and abs(cutoff - 2000) < 1,
                 f"the component holds the value heard: {harness.find_key(runtime, 'components')}")
    finally:
        await ed.call("play", action="stop")
