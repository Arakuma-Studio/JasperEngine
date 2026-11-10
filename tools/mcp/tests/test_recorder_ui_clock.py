"""CSS and both anime clocks advance once per recorded frame, including cold UI loads."""

from pathlib import Path

from harness import Editor
import pngdiff

CI = True
EDITOR = {"create": True, "name": "RecorderUiClock"}
TIMEOUT = 900

HTML = """<html><head><style>
body { position: absolute; inset: 0; margin: 0; background: #000; }
.marker { position: absolute; left: 4px; width: 8px; height: 8px; }
#css { top: 8px; background: #f00; animation: slide 3s linear both; }
#ui { top: 40px; background: #0f0; }
#game { top: 72px; background: #00f; }
#steps { top: 104px; background: #ff0; animation: slide 3s steps(90, jump-end) both; }
@keyframes slide { from { left: 4px; } to { left: 94px; } }
</style></head><body><div id="css" class="marker"></div>
<div id="ui" class="marker"></div><div id="game" class="marker"></div>
<div id="steps" class="marker"></div></body></html>
"""

SCRIPT = """---@class RecordedUiClock : ScriptInstance
local RecordedUiClock = { properties = {} }
function RecordedUiClock:init()
    local doc = UIDocument(self.scene, self.entity)
    recordedUiClockDocument = doc
    recordedUiClockWentBack = false
    self.css = doc:getElementById('css')
    RegisterEngineEvent(self, 'onUpdate')
    anime.animate(doc:getElementById('ui'), { translateX = 90, duration = 3000, ease = 'linear' })
    anime.animate(doc:getElementById('game'), { translateX = 90, duration = 3000, ease = 'linear', clock = 'game' })
end
function RecordedUiClock:onUpdate()
    local x = tonumber((self.css:getStyle('left'):gsub('px', '')))
    if x and self.previousCss and x < self.previousCss - 0.01 then recordedUiClockWentBack = true end
    self.previousCss = x
end
return RecordedUiClock
"""


def centre(png: bytes, channel: int, first_row: int) -> float:
    width, height, rgba = pngdiff.decode(png)
    xs = []
    for y in range(first_row, min(height, first_row + 8)):
        for x in range(width):
            p = (y * width + x) * 4
            matches = (rgba[p] > 200 and rgba[p + 1] > 200 and rgba[p + 2] < 50) if channel == 3 else (
                rgba[p + channel] > 200 and all(rgba[p + c] < 50 for c in range(3) if c != channel))
            if matches:
                xs.append(x)
    return sum(xs) / len(xs) if xs else -1.0


async def test_recorded_css_and_anime_clocks_repeat_at_exact_steps(ed: Editor):
    await ed.call("set_project_settings", values={"graphics.canvas_width": 128, "graphics.canvas_height": 128})
    scene = (await ed.call("create_scene", name="RecordedUi", type="UI", path="scenes/RecordedUi"))["scene"]["id"]
    await ed.call("write_file", path="assets/ui/recorded.html", content=HTML)
    entity = (await ed.call("create_entity", scene=scene, type="ui_document", name="Animated"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=entity, values={"uidocument": {"filename": "assets/ui/recorded.html"}})
    await ed.call("create_script", kind="lua", name="RecordedUiClock", content=SCRIPT, scene=scene, entity=entity)
    await ed.call("save_scene", scene=scene)
    recordings = []
    for take in range(2):
        # Different edit-time progress must have no effect on a fresh recording's CSS origin.
        await ed.call("screenshot", view="game")
        out = ed.scratch.root / f"ui-take-{take}"
        result = await ed.call("record_game", frames=16, fps=30, format="png", width=128, height=128,
                               output=str(out), exit_play=True, timeout_seconds=600)
        ed.check_eq(result["frames"], 16, "every requested frame was recorded")
        files = [Path(path) for path in result["files"]]
        ed.check_eq(len(files), 16)
        frames = [path.read_bytes() for path in files]
        recordings.append(frames)
        for channel, top in ((0, 8), (1, 40), (2, 72), (3, 104)):
            first = centre(frames[0], channel, top)
            ed.check(first >= 0, f"clock {channel} marker exists")
            ed.check(abs(first - 7.5) <= 0.6, f"clock {channel} starts at its initial value in frame zero: {first}")
            for index in (3, 9, 15):
                position = centre(frames[index], channel, top)
                ed.check(abs(position - first - index) <= 0.6,
                         f"clock {channel}, frame {index}: moved {position - first}, expected {index}")
    ed.check_eq(sum(a == b for a, b in zip(*recordings)), 16,
                "CSS and both anime clocks produce byte-identical repeated recordings")

    # Establish a nonzero UI epoch before creating an unrelated animation. A fresh take
    # must not reset that process-wide clock and leave this context behind its old origin.
    await ed.call("play", action="start", scene=scene, fixed_fps=30)
    await ed.call("wait", frames=60)
    await ed.call("play", action="stop")
    unrelated = (await ed.call("create_scene", name="UnrelatedUi", type="UI", path="scenes/UnrelatedUi"))["scene"]["id"]
    await ed.call("write_file", path="assets/ui/unrelated.html", content="""<html><head><style>
        #probe { position:absolute; top:0; left:100px; width:8px; height:8px;
                 animation: epoch 3600s linear both; }
        @keyframes epoch { from { left:100px; } to { left:3600100px; } }
        </style></head><body><div id="probe"></div></body></html>""")
    probe = (await ed.call("create_entity", scene=unrelated, type="ui_document", name="Unrelated"))["entity_id"]
    await ed.call("set_properties", scene=unrelated, entity=probe,
                  values={"uidocument": {"filename": "assets/ui/unrelated.html"}})
    await ed.call("save_scene", scene=unrelated)

    async def probe_x():
        result = await ed.call("inspect_ui", scene=unrelated, entity=probe, selector="#probe", depth=0)
        return result["elements"][0]["box"][0]

    before = await probe_x()
    for _ in range(60):
        if before >= 102:
            break
        await ed.call("wait", frames=1)
        before = await probe_x()
    ed.check(before >= 102, f"the unrelated animation advanced beyond its initial state: {before}")
    await ed.call("select_scene", scene=scene)
    await ed.call("record_game", frames=16, fps=30, format="png", width=128, height=128,
                  output=str(ed.scratch.root / "ui-preserves-epoch"), exit_play=True, timeout_seconds=600)
    await ed.call("select_scene", scene=unrelated)
    await ed.call("wait", frames=2)
    after = await probe_x()
    ed.check(after >= before, f"an unrelated UI context keeps its clock origin: {before} -> {after}")
    await ed.call("select_scene", scene=scene)

    # Ending a mid-Play recording must not rewind a running CSS animation.
    await ed.call("play", action="start", scene=scene, fixed_fps=30)
    await ed.call("eval_lua", code="recordedUiClockWentBack = false")
    try:
        out = ed.scratch.root / "ui-mid-play"
        await ed.call("record_game", frames=16, fps=30, format="png", width=128, height=128,
                      output=str(out), exit_play=False, timeout_seconds=600)
        await ed.call("wait", frames=3)
        state = await ed.call("eval_lua", code="return recordedUiClockWentBack")
        ed.check_eq(state["values"], [False], "CSS time never moves backward when a recording ends without exiting Play")

        # Paused gameplay keeps its CSS UI clock running, including while it records.
        await ed.call("play", action="pause")
        await ed.call("eval_lua", code="recordedUiClockDocument:getElementById('css'):setStyle('animation-name', 'none')")
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="recordedUiClockDocument:getElementById('css'):setStyle('animation-name', 'slide')")
        out = ed.scratch.root / "ui-paused-play"
        result = await ed.call("record_game", frames=16, fps=30, format="png", width=128, height=128,
                               output=str(out), exit_play=False, timeout_seconds=600)
        frames = [Path(path).read_bytes() for path in result["files"]]
        ed.check(centre(frames[-1], 0, 8) - centre(frames[0], 0, 8) >= 14.0,
                 "paused Play records a moving CSS UI rather than a frozen clock")
    finally:
        await ed.call("play", action="stop")
