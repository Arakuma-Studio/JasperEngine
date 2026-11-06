"""Sound Events in a capture (docs/specs/S37-sound-events.md R14, AC8; docs/specs/S35-agent-audio.md R12): a start names its
event, path, variation and instance, a steal names the instance that stole, a rejection its reason and cause; report counts the
starts by event; and two runs are the same bytes, though the editor's instance counter runs on between Plays.
"""

import hashlib
import json

from audio_refs import capture_sessions, read_jsonl, write_tone
from harness import Editor

EDITOR = {"create": True, "name": "Capture Events"}
CI = True
TIMEOUT = 900

STATE = {}

DRIVER = """---@class EventDriver : ScriptInstance
local EventDriver = { properties = {} }

function EventDriver:init()
    self.frame = 0
    RegisterEngineEvent(self, "onUpdate")
end

function EventDriver:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 5 or self.frame == 10 then Audio.play("events/limited.soundevent") end
    if self.frame == 15 or self.frame == 16 then Audio.play("events/cool.soundevent") end
end

return EventDriver
"""


async def write_event(ed: Editor, name: str, **values) -> None:
    body = {"format": "jasperengine.soundevent", "version": 1, "mode": "sequence",
            "clips": [{"clip": {"path": f"sounds/c{i}.wav"}} for i in range(2)], **values}
    await ed.call("write_file", path=f"events/{name}.soundevent", content=json.dumps(body, indent=2))


async def capture(ed: Editor):
    before = capture_sessions(ed.project)
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60, auto_pause=True)
    await ed.call("wait", frames=40)
    await ed.call("play", action="stop")
    made = [p for p in capture_sessions(ed.project) if p not in before]
    ed.check_eq(len(made), 1, "one session")
    return made[0]


async def test_setup(ed: Editor):
    for i in range(2):
        write_tone(ed.project / f"sounds/c{i}.wav", seconds=1.0, frequency=500.0 + 200.0 * i, rate=48000)
    await ed.call("sync_files")
    await write_event(ed, "limited", max_instances=1, steal="oldest")
    await write_event(ed, "cool", cooldown_ms=1000)
    await ed.call("sync_files")
    STATE["scene"] = (await ed.call("create_scene", name="Events", type="2D", path="scenes/Events.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=STATE["scene"], type="empty", name="Driver")
    await ed.call("create_script", kind="lua", name="EventDriver", content=DRIVER, entity="Driver", scene=STATE["scene"])
    await ed.call("save_scene", scene=STATE["scene"])


async def test_lines_carry_the_event_fields(ed: Editor):
    session = await capture(ed)
    lines = read_jsonl(session / "events.jsonl")[1:]
    starts = [l for l in lines if l["kind"] == "voice_start" and l.get("event_path") == "events/limited.soundevent"]
    ed.check_eq(len(starts), 2, f"{lines}")
    for start in starts:
        ed.check(isinstance(start.get("event"), str) and len(start["event"]) >= 16, f"the event's GUID: {start}")
        ed.check_eq((start["owner"], start["cause"]), ("free", "lua"), f"{start}")
        ed.check("variation" in start, f"{start}")
    ed.check_eq([s["instance"] for s in starts], [1, 2], "the capture numbers the plays from 1")
    stolen = [l for l in lines if l["kind"] == "voice_stop" and l.get("reason") == "stolen_instance"]
    ed.check_eq(len(stolen), 1, f"{lines}")
    ed.check_eq((stolen[0]["voice"], stolen[0]["by_instance"]), (starts[0]["voice"], 2), f"the play that stole it: {stolen[0]}")
    rejected = [l for l in lines if l["kind"] == "voice_reject"]
    ed.check_eq(len(rejected), 1, f"{lines}")
    ed.check_eq((rejected[0]["reason"], rejected[0]["cause"], rejected[0]["event_path"]), ("cooldown", "lua", "events/cool.soundevent"), f"{rejected[0]}")
    await ed.call("tool_groups", enable=["audio"])
    report = await ed.call("audio_session", action="report", image=False)
    by_event = {e["event_path"]: e["starts"] for e in report["voices"]["by_event"]}
    ed.check_eq(by_event, {"events/limited.soundevent": 2, "events/cool.soundevent": 1}, "report counts the starts by event")
    STATE["first"] = session


async def test_two_runs_are_the_same_bytes(ed: Editor):
    second = await capture(ed)
    first = STATE["first"]
    for a, b in zip((first / "events.jsonl").read_text().splitlines(), (second / "events.jsonl").read_text().splitlines()):
        ed.check_eq(a, b, "the same line on both runs")
    for name in ("master.wav", "events.jsonl", "levels.jsonl"):
        ed.check_eq(hashlib.sha1((first / name).read_bytes()).hexdigest(), hashlib.sha1((second / name).read_bytes()).hexdigest(), f"{name}")
