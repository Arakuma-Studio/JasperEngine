# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S80 Deviation 58, S86 Deviation 23: the editor stays responsive while a new title's programs
compile. On llvmpipe a program's first draw holds the render thread for its LLVM compile; the
ELEMENT program drew whole in 4.5-5.3 s and a 3D layer's programs all drew in one frame, so the
first render of a title stalled the editor past the hang watchdog's 3 s. Now each element draws
with the variant of its material (one compile up to about 1 s) and every Title Art render lets one
new program draw a frame. A title with every heavy Material member, a 3D title with AO and a glint,
rendered through render_title and opened in the Title Designer, with Mesa's shader cache off (cold
compiles, as on a first run): no main-thread stall reaches 3 s. The watchdog reports stalls from
1 s here so the longest ones show in the log. Red control (by hand, Deviation 58): the same run
with the whole ELEMENT program logs a stall over 4 s.
"""

import re
import time

from harness import Editor

CI = True
# cold compiles: Mesa's disk cache would serve programs an earlier run compiled
EDITOR = {"create": True, "name": "Title Compile Stalls", "env": {"MESA_SHADER_CACHE_DISABLE": "true"}}
TIMEOUT = 900

# the editor's hang threshold (HangWatchdog.h WatchdogConfig)
HANG_MS = 3000

# the heavy Material members on one face: a pattern with relief, a relief texture, glass, satin and
# a per-character gradient (spans); the rims and two effects of a new title stay
HEAVY_FACE = {
    "style.face.fill.kind": "linear",
    "style.face.fill.span": "char",
    "style.face.bevel.enabled": True,
    "style.face.pattern.kind": "giraffe",
    "style.face.pattern.relief": 0.6,
    "style.face.texture.kind": "liquid",
    "style.face.texture.amount": 0.5,
    "style.face.glass.enabled": True,
    "style.face.satin.enabled": True,
}


def stalls(text: str) -> list[float]:
    """The main-thread stalls the watchdog saw recover, in ms."""
    out = []
    for m in re.finditer(r"JASPERENGINE HANG: main thread recovered after ([0-9.]+) ?(ms|s)", text):
        value = float(m.group(1))
        out.append(value if m.group(2) == "ms" else value * 1000.0)
    return out


async def render(ed: Editor, args: dict) -> None:
    r = await ed.call_raw("render_title", args)
    job = (r.structured or {}).get("job_id") if not r.is_error else None
    ed.check(not r.is_error, f"render_title: {r.text[:300]}")
    if not job:
        return
    deadline = time.monotonic() + TIMEOUT
    state = await ed.call("get_job", job_id=job)
    while state.get("status") == "running" and time.monotonic() < deadline:
        state = await ed.call("get_job", job_id=job)
    ed.check_eq(state.get("status"), "succeeded", f"the render job: {state}")


async def test_new_titles_compile_without_a_hang(ed: Editor):
    await ed.call("tool_groups", enable=["title_art", "diagnostics", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("set_hang_watchdog", enabled=True, threshold_ms=1000)
    start = len(ed.stderr_text())

    # a 2D title with every heavy member, through render_title
    heavy = "assets/stalls/heavy.titleart"
    await ed.call("edit_title", path=heavy, create={"text": "BIG WIN"}, bake="never")
    await ed.call("edit_title", path=heavy, ops=[{"op": "set", "layer": "Title", "values": HEAVY_FACE},
                                                {"op": "add_effect", "layer": "Title", "type": "outer_glow"},
                                                {"op": "add_effect", "layer": "Title", "type": "drop_shadow"}])
    await render(ed, {"path": heavy})

    # a 3D title with AO and a glint, through render_title
    gold = "assets/stalls/gold3d.titleart"
    await ed.call("edit_title", path=gold, create={"text": "Gold"}, bake="never")
    await ed.call("edit_title", path=gold, ops=[{"op": "set_dimension", "layer": "Title", "dimension": "3d"},
                                               {"op": "apply_material", "layer": "Title", "part": "face", "preset": "gold"},
                                               {"op": "set", "layer": "Title", "values": {"style_3d.ao.enabled": True,
                                                                                         "style_3d.glint.enabled": True}}])
    await render(ed, {"path": gold})

    # another new look in the Title Designer: its canvas renders frame by frame
    designed = "assets/stalls/designed.titleart"
    await ed.call("edit_title", path=designed, create={"text": "JACKPOT"}, bake="never")
    await ed.call("edit_title", path=designed, ops=[{"op": "set", "layer": "Title", "values": {
        "style.face.pattern.kind": "tiger", "style.face.pattern.relief": 0.4, "style.face.texture.kind": "flakes",
        "style.face.texture.amount": 0.4, "style.face.sheen.enabled": True, "style.face.satin.enabled": True}}])
    state = (await ed.call("focus_window", window="title_designer", title_designer={"path": designed, "zoom": "fit"})).get("state", {})
    deadline = time.monotonic() + TIMEOUT
    while (state.get("busy") or not state.get("last_render_ms")) and time.monotonic() < deadline:
        state = (await ed.call("focus_window", window="title_designer", title_designer={})).get("state", {})
    ed.check(not state.get("busy") and state.get("last_render_ms"), f"the Designer's canvas rendered: {state}")

    health = await ed.call("get_editor_health")
    ed.check(health["responsive"], f"responsive after the renders: {health}")
    log = ed.stderr_text()[start:]
    seen = stalls(log)
    print(f"main-thread stalls over 1 s while the titles compiled: {[round(s) for s in seen]} ms")
    for line in log.splitlines():
        if line.startswith("JASPERENGINE HANG:"):
            print(line)
    if seen:
        # where the main thread was (the newest report's stacks)
        reports = (await ed.call("get_crash_reports", limit=1, include_text=True)).get("reports", [])
        if reports:
            print((reports[0].get("text") or "")[:6000])
    ed.check(all(s < HANG_MS for s in seen), f"no stall reached the {HANG_MS} ms hang threshold: {seen}")
    await ed.call("set_hang_watchdog", enabled=True, threshold_ms=HANG_MS)
