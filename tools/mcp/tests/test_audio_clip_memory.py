"""S97 AC15 (REQ-15, REQ-33): get_play_state's audio.clips shows the clips in memory while playing.

A Play with a 2 MB clip (Auto: streamed) and two small decoded ones: the three are counted, the stream
holds 0 bytes, `largest` is sorted by bytes, a decoded clip holds frames x 4 bytes (mono float), and a
destroySound through eval_lua drops its clip from the count.

Oracles: the frame counts of the WAVs this test writes, and SoundPool's streaming threshold (1 MiB).
"""

from audio_refs import write_tone
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Audio Clip Memory"}
TIMEOUT = 600
RATE = 44100


async def test_play_state_clips(ed: Editor):
    write_tone(ed.project / "sounds/big.wav", seconds=24.0)    # 2.1 MB of 16-bit mono: streamed
    write_tone(ed.project / "sounds/a.wav", seconds=0.5)
    write_tone(ed.project / "sounds/b.wav", seconds=1.0)
    await ed.call("sync_files")
    scene = (await ed.call("create_scene", name="Clips", type="2D", path="scenes/Clips.scene"))["scene"]["id"]
    for name, clip in (("Music", "sounds/big.wav"), ("A", "sounds/a.wav"), ("B", "sounds/b.wav")):
        await ed.call("create_entity", scene=scene, name=name, type="audio_source")
        await ed.call("set_properties", scene=scene, entity=name, values={"audiosource": {"filename": clip}})
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=5)
        clips = (await ed.call("get_play_state"))["audio"]["clips"]
        ed.check_eq(clips["count"], 3, f"the scene's three clips: {clips}")
        largest = clips["largest"]
        ed.check_eq([c["bytes"] for c in largest], sorted((c["bytes"] for c in largest), reverse=True), f"by bytes: {largest}")
        by_path = {c["path"]: c for c in largest}
        ed.check_eq((by_path["sounds/big.wav"]["load_type"], by_path["sounds/big.wav"]["bytes"]), ("streaming", 0),
                    "a stream holds no samples")
        ed.check_eq(by_path["sounds/b.wav"]["bytes"], int(1.0 * RATE) * 4, "decoded: frames x 4 bytes")
        ed.check_eq(by_path["sounds/a.wav"]["holders"], 1, "its source holds it")
        ed.check_eq(clips["bytes"], (int(0.5 * RATE) + int(1.0 * RATE)) * 4, "the total")
        ed.check_eq((clips["unused_bytes"], clips["loading"]), (0, 0), "all held, none loading")

        await ed.call("eval_lua", code="object('A', 'AudioSource'):destroySound(); return true")
        clips = (await ed.call("get_play_state"))["audio"]["clips"]
        ed.check_eq(clips["count"], 2, f"destroySound freed a.wav: {clips}")
        ed.check("sounds/a.wav" not in {c["path"] for c in clips["largest"]}, f"{clips}")
    finally:
        await ed.call("play", action="stop")
