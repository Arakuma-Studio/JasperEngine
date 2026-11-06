"""The Lua Audio table, SoundHandle and AudioPlayOn (docs/specs/S37-sound-events.md R13; AC7): each
option of Audio.play, a handle going invalid at the end, fadeOut with pause, stopAll("SFX")'s count,
calls inside a contact callback deferred, the AudioSource additions, AudioPlayOn's values.
"""

import json

from audio_refs import TRACE_ENV, voice_lines, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Audio Lua", "env": TRACE_ENV}
CI = True
TIMEOUT = 900

EVENT = "events/steps.soundevent"
STATE = {}

WATCH = """---@class AudioInContact : ScriptInstance
local AudioInContact = { properties = {} }

function AudioInContact:init()
    RegisterEvent(self, self.scene:getPhysicsSystem().onContactAdded3D, "onAdded")
end

function AudioInContact:onAdded(a, b, contact)
    if not self.handle then
        self.before = AudioSystem.getVoiceCount()
        self.handle = Audio.play("sounds/long.wav")
        self.validInCallback = self.handle:isValid()
        self.countInCallback = self.handle.voiceCount
        self.stopped = Audio.stopAll("SFX")
    end
end

return AudioInContact
"""


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def lines_since(ed: Editor) -> list[dict]:
    return voice_lines(await ed.console_since(limit=500))


async def test_setup(ed: Editor):
    write_tone(ed.project / "sounds/a.wav", seconds=0.2)
    write_tone(ed.project / "sounds/long.wav", seconds=3.0)
    await ed.call("sync_files")
    await ed.call("write_file", path=EVENT, content=json.dumps({
        "format": "jasperengine.soundevent", "version": 1, "mode": "layer",
        "clips": [{"clip": {"path": "sounds/long.wav"}}, {"clip": {"path": "sounds/a.wav"}}]}, indent=2))
    await ed.call("audio_mixer", action="edit", ops=[{"op": "add_group", "name": "SFX"}, {"op": "add_group", "name": "UI"}])
    scene = (await ed.call("create_scene", name="Lua", type="3D", path="scenes/Lua.scene"))["scene"]["id"]
    STATE["scene"] = scene
    await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
    await ed.call("create_entity", scene=scene, name="Carrier", type="cube", position=[4, 5, 6])
    await ed.call("save_scene", scene=scene)


async def test_play_with_each_option(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await ed.console_since(limit=500)
        got = await lua(ed, """
            h = Audio.play('sounds/long.wav', {volume = 0.5, pitch = 2, group = 'SFX'})
            return {h:isValid(), h:isPlaying(), h.volume, h.pitch, h.voiceCount}""")
        ed.check_eq(got[0], [True, True, 0.5, 2, 1], "volume, pitch")
        line = (await lines_since(ed))[-1]
        ed.check_eq(line["owner"], "free", f"{line}")
        ed.check_eq(line["cause"], "lua", f"{line}")
        ed.check(line["gain"] == 0.5 and line["pitch"] == 2, f"{line}")

        got = await lua(ed, """
            local p = Audio.play('sounds/long.wav', {position = Vector3(7, 8, 9)})
            local pos = p.position
            return {pos.x, pos.y, pos.z}""")
        ed.check_eq(got[0], [7, 8, 9], "position")

        got = await lua(ed, """
            local c = object('Carrier', 'GameObject')
            follow = Audio.play('sounds/long.wav', {target = c})
            local pos = follow.position
            return {pos.x, pos.y, pos.z}""")
        ed.check_eq(got[0], [4, 5, 6], "target")
        got = await lua(ed, "local c = object('Carrier', 'GameObject'); c.position = Vector3(1, 1, 1); return true")
        await ed.call("wait", frames=2)
        got = await lua(ed, "local p = follow.position; return {p.x, p.y, p.z}")
        ed.check_eq(got[0], [1, 1, 1], "it follows")
        await lua(ed, "follow.position = Vector3(2, 2, 2); return true")
        await ed.call("wait", frames=1)
        got = await lua(ed, "local p = follow.position; return {p.x, p.y, p.z}")
        ed.check_eq(got[0], [2, 2, 2], "setting it stops following")

        got = await lua(ed, """
            d = Audio.play('sounds/a.wav', {delay = 0.1})
            return {d:isValid(), d.voiceCount}""")
        ed.check_eq(got[0], [True, 0], "delay: valid, not yet started")
        await ed.call("wait", frames=10)
        ed.check_eq((await lua(ed, "return d.voiceCount"))[0], 1, "started")

        got = await lua(ed, "local s = Audio.play('sounds/a.wav', {scene = getScene(), unscaled = true}); return s:isValid()")
        ed.check_eq(got[0], True, "scene, unscaled")

        ed.check_eq((await lua(ed, f"local l = Audio.play('{EVENT}'); return l.voiceCount"))[0], 2, "an event's layers")
        ed.check_eq((await lua(ed, "return Audio.play('nothing.wav'):isValid()"))[0], False, "an unknown name")
    finally:
        await ed.call("play", action="stop")


async def test_handle_invalid_after_the_end(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await lua(ed, "short = Audio.play('sounds/a.wav'); return true")
        await ed.call("wait", frames=30)
        got = await lua(ed, "local p = short.position; return {short:isValid(), short:isPlaying(), short.volume, short.pitch, short.pan, short.time, short.voiceCount, p.x}")
        ed.check_eq(got[0], [False, False, 0, 0, 0, 0, 0, 0], "getters of an invalid handle")
        await lua(ed, "short.volume = 0.5; short:stop(); short:pause(); return true")
    finally:
        await ed.call("play", action="stop")


async def test_fade_out_with_pause(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await lua(ed, "f = Audio.play('sounds/long.wav'); f:fadeOut(0.25, {pause = true}); return true")
        await ed.call("wait", frames=6)
        volume = (await lua(ed, "return f.volume"))[0]
        ed.check(0 < volume < 1, f"on its way: {volume}")
        await ed.call("wait", frames=20)
        got = await lua(ed, "return {f:isValid(), f:isPlaying(), f.volume}")
        ed.check_eq(got[0], [True, False, 1], "paused, its volume back")
        await lua(ed, "f:resume(); f:fadeTo(0.5, 0.1); return true")
        await ed.call("wait", frames=10)
        ed.check_eq((await lua(ed, "return {f:isPlaying(), f.volume}"))[0], [True, 0.5], "fadeTo")
        await lua(ed, "f:fadeOut(0.05); return true")
        await ed.call("wait", frames=6)
        ed.check_eq((await lua(ed, "return f:isValid()"))[0], False, "fadeOut stops")
    finally:
        await ed.call("play", action="stop")


async def test_stop_all_counts(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        got = await lua(ed, """
            Audio.play('sounds/long.wav', {group = 'SFX'})
            Audio.play('sounds/long.wav', {group = 'SFX'})
            keep = Audio.play('sounds/long.wav', {group = 'UI'})
            local stopped = Audio.stopAll('SFX')
            return {stopped, keep:isValid(), Audio.stopAll('Nope'), Audio.stopAll(), keep:isValid()}""")
        ed.check_eq(got[0], [2, True, 0, 1, False], "stopAll")
    finally:
        await ed.call("play", action="stop")


async def test_calls_inside_a_contact_callback_are_deferred(ed: Editor):
    scene = (await ed.call("create_scene", name="Contact", type="3D", path="scenes/Contact.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="cube", name="Floor", position=[0, -0.5, 0], scale=[30, 1, 30])
    await ed.call("edit_colliders", scene=scene, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=scene, type="cube", name="Box", position=[0, 1, 0])
    await ed.call("edit_colliders", scene=scene, entity="Box", body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=scene, type="empty", name="Watch")
    await ed.call("create_script", kind="lua", name="AudioInContact", content=WATCH, entity="Watch", scene=scene)
    await ed.call("save_scene", scene=scene)
    await ed.console_since(limit=500)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=90)
        got = (await lua(ed, """
            local w = script('Watch')
            return {w.validInCallback, w.countInCallback, w.stopped, w.handle:isValid(), w.handle.voiceCount}"""))[0]
        ed.check_eq(got, [True, 0, 0, True, 1], "valid at once, started at the frame's audio update")
        problems = await ed.console_since(types=["error"], limit=20)
        ed.check(not problems, f"no error: {problems}")
    finally:
        await ed.call("play", action="stop")


async def test_audio_source_additions(ed: Editor):
    guid = (await ed.call("resolve_asset", asset=EVENT))["guid"]
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        got = (await lua(ed, f"""
            local s = object('Source', 'AudioSource')
            s.event = '{guid}'
            local byGuid = s.event
            s.event = '{EVENT}'
            local byPath = s.event
            s.playOn = AudioPlayOn.UIClick + AudioPlayOn.Collision3D
            s.minImpactSpeed = 1.5
            s.fullVolumeImpactSpeed = 8
            s.pitchRandomSemitones = 99
            s.volumeRandomDb = 3
            return {{byGuid, byPath, s.playOn, s.minImpactSpeed, s.fullVolumeImpactSpeed, s.pitchRandomSemitones, s.volumeRandomDb}}"""))[0]
        ed.check_eq(got, [EVENT, EVENT, 17, 1.5, 8, 24, 3], "the AudioSource additions")
        names = (await lua(ed, "return {AudioPlayOn.UIClick, AudioPlayOn.UIPress, AudioPlayOn.UIHover, AudioPlayOn.Collision2D, AudioPlayOn.Collision3D, AudioPlayOn.Sensor2D, AudioPlayOn.Sensor3D}"))[0]
        ed.check_eq(names, [1, 2, 4, 8, 16, 32, 64], "AudioPlayOn")
    finally:
        await ed.call("play", action="stop")
