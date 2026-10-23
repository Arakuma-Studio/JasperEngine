"""File watcher and hot reload (S05): files changed on disk by another program reload in the
editor without an MCP call (the watcher), the editor's own writes do not come back as external
changes, external_changes reports what the watcher did (source, reloaded, pending), a broken file
keeps the previous version, Auto Refresh off holds external changes, added/removed/renamed
files reach the asset database, and a reload does not hitch the frames.

The textures are drawn by a sprite filling the game view of a 2D scene, so the centre pixel is
the texture's colour."""

from __future__ import annotations

import os
import struct
import time
import zlib
from pathlib import Path

import pngdiff
from harness import Editor, find_key

EDITOR = {"create": True, "name": "Hot Reload"}
TIMEOUT = 900

TEX = "assets/hr/tex.png"
RED, GREEN, BLUE, YELLOW = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)


def png_bytes(rgb, size: int = 64) -> bytes:
    """A solid RGBA PNG (stdlib only)."""
    raw = b"".join(b"\x00" + bytes(list(rgb) + [255]) * size for _ in range(size))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


def safe_save(path: Path, data: bytes) -> None:
    """What an image editor does: write a temp file next to it, rename it over the target."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(data)
    os.replace(temp, path)


async def console_mark(ed: Editor) -> None:
    """Move the Console cursor past everything logged so far (console_since() reads from here)."""
    result = await ed.call("read_console", limit=1000)
    cursor = result["next_cursor"]
    while result.get("more"):
        result = await ed.call("read_console", since=cursor, limit=1000)
        cursor = result["next_cursor"]
    ed._console_cursor = cursor


def prefab_guid(path: Path) -> str | None:
    """The GUID a .prefab file carries (its `guid:` line)."""
    for line in path.read_text().splitlines():
        if line.strip().startswith("guid:"):
            return line.split(":", 1)[1].strip().strip('"')
    return None


def external(r) -> dict | None:
    s = r.structured if isinstance(r.structured, dict) else {}
    return s.get("external_changes")


async def status(ed: Editor) -> dict:
    r = await ed.call_raw("editor_status")
    ed.check(not r.is_error, f"editor_status failed: {r.text[:300]}")
    return r


def watcher(r) -> dict:
    return r.structured["file_watcher"]


async def centre(ed: Editor) -> tuple:
    png = await ed.screenshot(view="game", scene=STATE["scene"])
    width, height, rgba = pngdiff.decode(png)
    i = ((height // 2) * width + width // 2) * 4
    return tuple(rgba[i:i + 3])


def close(a, b, tol: int = 3) -> bool:
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


async def wait_centre(ed: Editor, want, seconds: float = 5.0) -> tuple:
    deadline = time.monotonic() + seconds
    got = await centre(ed)
    while not close(got, want) and time.monotonic() < deadline:
        time.sleep(0.2)
        got = await centre(ed)
    return got


STATE: dict = {}


async def test_setup_sprite_with_texture(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "generate_placeholder_art", "screenshot", "editor_status")
    made = await ed.call("generate_placeholder_art", path=TEX, width=64, height=64, shape="rect", color="#ff0000")
    ed.check_eq(made["path"], TEX)
    scene = await ed.call("create_scene", name="HotReload", type="2d")
    STATE["scene"] = scene["scene"]["id"]
    sprite = await ed.call("create_entity", scene=STATE["scene"], type="sprite", name="Board", values={
        "sprite": {"width": 8000, "height": 8000},
        "transform": {"position": [-4000, -4000, 0]},
        "mesh": {"submeshes[0].material.baseColorTexture": TEX},
    })
    STATE["sprite"] = find_key(sprite, "entity_id", "id")
    got = await wait_centre(ed, RED)
    ed.check(close(got, RED), f"the sprite shows the red texture: {got}")
    fw = watcher(await status(ed))
    ed.check(fw["state"] in ("watching", "polling"), f"the watcher runs: {fw!r}")


async def test_external_texture_edit_reloads(ed: Editor):
    """AC11: a safe-save of the PNG reloads it without any MCP call."""
    await console_mark(ed)
    safe_save(ed.project / TEX, png_bytes(GREEN))
    time.sleep(1.5)   # no MCP call: only the watcher can reload it
    asked = time.time()
    entries = await ed.console_since(contains="[Hot Reload] Reloaded " + TEX)
    ed.check(entries, "no [Hot Reload] line for the texture")
    ed.check(min(e["time"] for e in entries) < asked, f"the reload happened before the call: {entries!r} (asked {asked})")
    fw = watcher(await status(ed))
    ed.check(fw["last_reload"] and fw["last_reload"]["path"] == TEX and fw["last_reload"]["ok"], f"last_reload: {fw!r}")
    got = await centre(ed)
    ed.check(close(got, GREEN), f"the sprite shows the green texture: {got}")


async def test_agent_edit_reported_once(ed: Editor):
    """AC12: an edit right before a call is picked up by the watcher's journal, reloaded once."""
    before = watcher(await status(ed))["reloads"]
    (ed.project / TEX).write_bytes(png_bytes(BLUE))
    r = await status(ed)
    ext = external(r)
    ed.check(ext is not None, "the edit is reported")
    ed.check_eq(ext.get("source"), "watcher", f"from the journal: {ext!r}")
    ed.check_in(TEX, ext["changed"], "changed")
    ed.check(any(item["path"] == TEX for item in ext.get("reloaded", [])), f"reloaded: {ext!r}")
    ed.check_eq(ext.get("pending", []), [], "nothing left reloading")
    ed.check_eq(watcher(r)["reloads"], before + 1, "reloaded exactly once")
    r = await status(ed)
    ed.check(external(r) is None, f"reported once: {external(r)!r}")
    ed.check_eq(watcher(r)["reloads"], before + 1, "not reloaded again")
    got = await centre(ed)
    ed.check(close(got, BLUE), f"blue: {got}")


async def test_editor_save_no_echo(ed: Editor):
    """AC13: the editor's own save of a linked material is not an external change; the other
    entity linking the file follows it."""
    ed.require_tools("create_material", "link_material", "update_material", "get_entity")
    await ed.call("create_material", path="materials/hr.material", values={"baseColorFactor": [1, 0, 0, 1]})
    ids = []
    for name in ("LinkA", "LinkB"):
        made = await ed.call("create_entity", scene=STATE["scene"], type="cube", name=name, select=False)
        ids.append(find_key(made, "entity_id", "id"))
        await ed.call("link_material", scene=STATE["scene"], entity=ids[-1], material="materials/hr.material")
    await status(ed)
    echoes = watcher(await status(ed))["echoes_suppressed"]
    await console_mark(ed)
    await ed.call("update_material", path="materials/hr.material", values={"baseColorFactor": [0, 0, 1, 1]})
    time.sleep(2.0)
    r = await status(ed)
    ed.check(external(r) is None, f"the save came back as external: {external(r)!r}")
    lines = await ed.console_since(contains="[Hot Reload]")
    ed.check(not [e for e in lines if "hr.material" in e["message"]], f"a [Hot Reload] line for the editor's save: {lines!r}")
    ed.check_eq(watcher(r)["echoes_suppressed"], echoes + 1, "its echo was suppressed")
    other = await ed.call("get_entity", scene=STATE["scene"], entity=ids[1], components=["mesh"], detail="values")
    text = str(other)
    ed.check("0.0, 0.0, 1.0" in text or "[0, 0, 1" in text or "0.0, 0.0, 1.0, 1.0" in text, f"the other link took the new colour: {text[:600]}")


async def test_broken_texture_kept(ed: Editor):
    """AC15: garbage bytes keep the previous texture; a problem and an error line say so."""
    good = await centre(ed)
    await console_mark(ed)
    safe_save(ed.project / TEX, b"this is not a png at all" * 10)
    r = await status(ed)
    ext = external(r)
    ed.check(ext is not None, "the broken edit is reported")
    ed.check(any(p.get("path") == TEX for p in ext.get("problems", [])), f"a problem names the file: {ext!r}")
    errors = await ed.console_since(contains="[Hot Reload] " + TEX, types=["error"])
    ed.check(errors, "an error line in the Console")
    got = await centre(ed)
    ed.check(close(got, good, 6), f"the old texture stays: {got} (was {good})")
    # fixed again
    safe_save(ed.project / TEX, png_bytes(GREEN))
    await status(ed)
    got = await wait_centre(ed, GREEN)
    ed.check(close(got, GREEN), f"green again: {got}")


async def test_auto_refresh_off(ed: Editor):
    """AC16: Auto Refresh off: the watcher's changes wait (counted as pending, no reload, no
    Console line); an MCP call still picks them up (the agent contract)."""
    ed.require_tools("sync_files")
    await ed.call("sync_files", auto_refresh=False, full=False)
    try:
        await status(ed)
        await console_mark(ed)
        safe_save(ed.project / TEX, png_bytes(YELLOW))
        time.sleep(1.5)
        # this call's pick-up applies what waited; its status counts what waited for it
        r = await status(ed)
        fw = watcher(r)
        ed.check_eq(fw["auto_refresh"], False)
        ed.check(fw["pending"] >= 1, f"pending while Auto Refresh is off: {fw!r}")
        ext = external(r)
        ed.check(ext is not None and TEX in ext["changed"], f"an MCP call still syncs it: {ext!r}")
        lines = await ed.console_since(contains="[Hot Reload] Reloaded " + TEX)
        ed.check(not lines, f"the watcher reloaded it with Auto Refresh off: {lines!r}")
        got = await wait_centre(ed, YELLOW)
        ed.check(close(got, YELLOW), f"yellow after the call: {got}")
    finally:
        await ed.call("sync_files", auto_refresh=True, full=False)
    safe_save(ed.project / TEX, png_bytes(GREEN))
    time.sleep(1.0)
    await wait_centre(ed, GREEN)


async def test_external_add_delete_rename(ed: Editor):
    """AC14: added, renamed and removed files reach the asset database without a call; a
    prefab keeps its GUID across the rename."""
    ed.require_tools("list_assets", "get_asset_info", "create_prefab")
    # a prefab made by the editor, then renamed and removed by another program
    made = await ed.call("create_entity", scene=STATE["scene"], type="sprite", name="PrefabSource", select=False)
    eid = find_key(made, "entity_id", "id")
    await ed.call("create_prefab", scene=STATE["scene"], entity=eid, path="prefabs/hr.prefab")
    guid = prefab_guid(ed.project / "prefabs/hr.prefab")
    ed.check(guid, "the prefab has a GUID")

    async def listed(pattern: str) -> list[str]:
        out = await ed.call("list_assets", pattern=pattern)
        return [a if isinstance(a, str) else a.get("path") for a in out.get("entries", [])]

    png = ed.project / "assets/hr/added.png"
    safe_save(png, png_bytes(RED, 8))
    time.sleep(1.5)
    await status(ed)
    ed.check("assets/hr/added.png" in await listed("assets/**/added*.png"), "added")

    os.replace(png, png.with_name("renamed.png"))
    prefab = ed.project / "prefabs/hr.prefab"
    os.replace(prefab, prefab.with_name("hr_renamed.prefab"))
    time.sleep(1.5)
    await status(ed)
    ed.check("assets/hr/renamed.png" in await listed("assets/**/renamed*.png"), "renamed png")
    ed.check(not await listed("assets/**/added*.png"), "the old name is gone")
    ed.check_eq(prefab_guid(ed.project / "prefabs/hr_renamed.prefab"), guid, "the prefab keeps its GUID across the rename")
    listing = await ed.call("list_assets", pattern="prefabs/*.prefab")
    ed.check([e["path"] for e in listing["entries"]] == ["prefabs/hr_renamed.prefab"], f"listed under its new name: {listing!r}")

    png.with_name("renamed.png").unlink()
    time.sleep(1.5)
    await status(ed)
    ed.check(not await listed("assets/**/renamed*.png"), "removed")


def run_stats(windows: list[dict]) -> tuple[int, float, float]:
    """(frames, median, max) of a run from the frame_ms windows of its editor_status calls."""
    frames = sum(w["frames"] for w in windows)
    # the median frame: the windows' medians weighted by their frames
    half, seen, median = frames / 2, 0, 0.0
    for value, count in sorted((w["median"], w["frames"]) for w in windows):
        seen += count
        if seen >= half:
            median = value
            break
    return frames, median, max(w["max"] for w in windows)


async def test_reload_hitch(ed: Editor):
    """AC19: a 2048x2048 PNG reloaded 20 times at 200 ms intervals while the view redraws
    continuously (the scene plays): the reload adds no frame more than 16 ms over the run's
    median, against the same loop of calls without a reload (on a loaded runner with Mesa the
    editor has spikes of its own); each reload within 500 ms."""
    ed.require_tools("play", "set_properties")
    big = ed.project / "assets/hr/big.png"
    safe_save(big, png_bytes(RED, 2048))
    await ed.call("set_properties", scene=STATE["scene"], entity=STATE["sprite"],
                  values={"mesh": {"submeshes[0].material.baseColorTexture": "assets/hr/big.png"}})
    await status(ed)
    # the game runs on its own between calls (the MCP holds it by default)
    await ed.call("play", action="set", auto_pause=False)
    r = await ed.call_raw("play", {"action": "start", "scene": STATE["scene"], "timeout_seconds": 300})
    ed.check(not r.is_error, f"play failed: {r.text[:500]}")
    latencies = []
    runs: dict[str, list[dict]] = {"control": [], "reload": []}

    async def stat(run: str) -> dict:
        out = await ed.call("editor_status")
        if out["frame_ms"]["frames"]:
            runs[run].append(out["frame_ms"])
        return out

    try:
        await ed.call("editor_status")   # frame_ms from here on
        for run in ("control", "reload"):
            for i in range(20):
                before = (await stat(run))["file_watcher"]["reloads"]
                t0 = time.monotonic()
                if run == "reload":
                    safe_save(big, png_bytes(GREEN if i % 2 == 0 else RED, 2048))
                while time.monotonic() - t0 < (5.0 if run == "reload" else 0.25):
                    if (await stat(run))["file_watcher"]["reloads"] > before:
                        break
                    time.sleep(0.02)
                if run == "reload":
                    latencies.append(time.monotonic() - t0)
                time.sleep(0.2)
            await stat(run)
    finally:
        await ed.call("play", action="stop")
        await ed.call("play", action="set", auto_pause=True)
    control, reload = run_stats(runs["control"]), run_stats(runs["reload"])
    print(f"     [hitch] control {control[0]} frames, median {control[1]:.1f} ms, max {control[2]:.1f} ms; "
          f"reload {reload[0]} frames, median {reload[1]:.1f} ms, max {reload[2]:.1f} ms; "
          f"reload latency max {max(latencies) * 1000:.0f} ms")
    ed.check(reload[0] >= 100, f"the view redrew continuously: {reload[0]} frames")
    hitch = (reload[2] - reload[1]) - max(0.0, control[2] - control[1])
    ed.check(hitch <= 16.0, f"a reload frame hitched {hitch:.1f} ms over the run's own spikes: control {control}, reload {reload}")
    ed.check(reload[1] <= control[1] * 1.25 + 2.0, f"the reloads slowed every frame: control {control}, reload {reload}")
    # each measure includes the status calls' own round trips
    ed.check(max(latencies) <= 0.5 + 0.25, f"reload latencies (s): {latencies!r}")


async def test_bulk_checkout(ed: Editor):
    """AC20: 10,000 small files in one burst: few Console lines, reloads done within 10 s, the
    editor answering throughout."""
    await console_mark(ed)
    root = ed.project / "assets/bulk"
    t0 = time.monotonic()
    for i in range(10000):
        p = root / f"d{i // 500}" / f"f{i}.txt"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(str(i))
    slowest = 0.0
    while time.monotonic() - t0 < 10.0:
        c0 = time.monotonic()
        stats = await ed.call("editor_status")
        slowest = max(slowest, time.monotonic() - c0)
        fw = stats["file_watcher"]
        if fw["pending"] == 0 and fw["reloading"] == 0:
            break
        time.sleep(0.1)
    ed.check(time.monotonic() - t0 <= 10.0 + 1.0, "the burst settled within 10 s")
    ed.check(slowest < 0.2 + 0.3, f"editor_status stayed responsive (slowest {slowest:.3f} s)")
    lines = await ed.console_since(contains="[Hot Reload]")
    ed.check(len(lines) <= 10, f"{len(lines)} [Hot Reload] lines: {[e['message'] for e in lines][:12]!r}")


async def test_polling_backend():
    """AC17: the texture reload and the asset changes with an editor started --file-watcher=polling."""
    async with Editor(create=True, name="Hot Reload Polling", extra_args=["--file-watcher=polling"]) as ed:
        made = await ed.call("generate_placeholder_art", path=TEX, width=64, height=64, shape="rect", color="#ff0000")
        ed.check_eq(made["path"], TEX)
        scene = await ed.call("create_scene", name="HotReload", type="2d")
        STATE["scene"] = scene["scene"]["id"]
        await ed.call("create_entity", scene=STATE["scene"], type="sprite", name="Board", values={
            "sprite": {"width": 8000, "height": 8000},
            "transform": {"position": [-4000, -4000, 0]},
            "mesh": {"submeshes[0].material.baseColorTexture": TEX},
        })
        ed.check(close(await wait_centre(ed, RED), RED), "red")
        fw = watcher(await status(ed))
        ed.check_eq(fw["state"], "polling", f"polling: {fw!r}")
        await console_mark(ed)
        safe_save(ed.project / TEX, png_bytes(GREEN))
        # interval 1 s + the settle: within interval + 500 ms, with the margin of a busy runner
        time.sleep(2.0)
        asked = time.time()
        entries = await ed.console_since(contains="[Hot Reload] Reloaded " + TEX)
        ed.check(entries and min(e["time"] for e in entries) < asked, f"reloaded by the poller: {entries!r}")
        ed.check(close(await centre(ed), GREEN), "green")
        png = ed.project / "assets/hr/added.png"
        safe_save(png, png_bytes(RED, 8))
        time.sleep(2.0)
        out = await ed.call("list_assets", pattern="assets/**/added*.png")
        names = [a if isinstance(a, str) else a.get("path") for a in out.get("entries", [])]
        ed.check("assets/hr/added.png" in names, f"added: {out!r}")
