"""A file another program writes right after a tool call is an external change (S70): reported by the
next call even 0 ms later and even with the stamps of the tool's own write, and the tool's own writes
are never reported. Both ways of finding changes: the watcher's journal and, with the watcher off, the
stat scan.

Deterministic. The editor runs with JASPER_TEST_SETTLE_DELAY_MS=300, which makes the settle after a
writing call (McpAutoSync::afterCall) wait 300 ms first: when the result goes out before the settle,
the external write below always lands before that settle; when it goes out after, the delay only
slows the result. A coarse clock's stamp (a write right after the call stamped inside it, or in the
tick of the tool's write) is set with os.utime.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from harness import Editor, RawEditor, find_key, over_budget, skip, symlinks_permitted
from pngdiff import encode as encode_png

EDITOR = {"create": True, "name": "AutoSyncRace", "env": {"JASPER_TEST_SETTLE_DELAY_MS": "300"}}
TIMEOUT = 1200
CI = True

SCAN_ENV = {"JASPER_TEST_SETTLE_DELAY_MS": "300", "JASPER_FILE_WATCHER": "off"}
SETTLE_DELAY_S = 0.3
BUSY_LUA = "local t = os.clock() while os.clock() - t < 0.8 do end return true"
# how long past a stamp's mtime a racy stamp ends (RACY_NS), less a margin
RACY_S = 1.8


def external(r) -> dict | None:
    s = r.structured if isinstance(r.structured, dict) else {}
    return s.get("external_changes")


def changed(r) -> list:
    ext = external(r)
    return list(ext.get("changed", [])) if ext else []


def stamp(path: Path, ns: int) -> None:
    os.utime(path, ns=(ns, ns))


async def status_call(ed: Editor):
    r = await ed.call_raw("editor_status")
    ed.check(not r.is_error, f"editor_status failed: {r.text[:300]}")
    return r


async def write_file(ed: Editor, path: str, content: str):
    r = await ed.call_raw("write_file", {"path": path, "content": content})
    ed.check(not r.is_error, f"write_file {path}: {r.text[:300]}")
    ed.check(path not in changed(r), f"write_file's own write reported: {external(r)!r}")
    return r


async def quiet(ed: Editor):
    """What earlier steps left is reported now: the next report is this step's."""
    await status_call(ed)
    r = await status_call(ed)
    ed.check(external(r) is None, f"reported with nothing changed: {external(r)!r}")


async def new_file_stamped_inside_the_call(ed: Editor, tag: str, source: str):
    """A new file written 0 ms after a writing call, stamped (as a coarse clock can) inside the call."""
    await quiet(ed)
    await write_file(ed, f"scripts/{tag}Tool.lua", "return {}\n")
    rel = f"scripts/{tag}Inside.lua"
    (ed.project / rel).write_text("return {}\n")
    stamp(ed.project / rel, (ed.project / f"scripts/{tag}Tool.lua").stat().st_mtime_ns)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and rel in ext.get("added", []), f"the file stamped inside the call is not reported: {ext!r}")
    ed.check_eq(ext["changed"], [rel], "the tool's own file is not")
    ed.check_eq(ext["source"], source, "found by")
    r = await status_call(ed)
    ed.check(external(r) is None, f"reported twice: {external(r)!r}")


async def same_tick_rewrite_of_the_tools_file(ed: Editor, tag: str):
    """The file a tool just wrote, rewritten by another program with other content of the same size
    and the same mtime: a modification (the stat cannot tell, the content does)."""
    await quiet(ed)
    rel = f"scripts/{tag}Rewrite.lua"
    await write_file(ed, rel, "return { value = 1 }\n")
    path = ed.project / rel
    before = path.stat()
    path.write_text("return { value = 2 }\n")
    stamp(path, before.st_mtime_ns)
    ed.check_eq(path.stat().st_size, before.st_size, "same size")
    r = await status_call(ed)
    ed.check_in(rel, changed(r), "the same-tick rewrite is reported")
    r = await status_call(ed)
    ed.check(external(r) is None, f"reported twice: {external(r)!r}")


async def same_tick_new_file_in_the_tools_folder(ed: Editor, tag: str):
    """A tool writes into a folder; another program adds a file there in the same tick: the new file
    has the tool's stamp and the folder's stays what the tool left. Added, and the tool's file is not."""
    await write_file(ed, f"scripts/{tag}Folder/Seed.lua", "return {}\n")
    await quiet(ed)
    await write_file(ed, f"scripts/{tag}Folder/One.lua", "return {}\n")
    folder = ed.project / f"scripts/{tag}Folder"
    after_tool = folder.stat()
    tool_stamp = (folder / "One.lua").stat().st_mtime_ns
    (folder / "Two.lua").write_text("return {}\n")
    stamp(folder / "Two.lua", tool_stamp)
    stamp(folder, after_tool.st_mtime_ns)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and ext.get("added") == [f"scripts/{tag}Folder/Two.lua"],
             f"the file added in the same tick is not reported (or the tool's is): {ext!r}")
    r = await status_call(ed)
    ed.check(external(r) is None, f"reported twice: {external(r)!r}")


async def no_pause_rounds(ed: Editor, tag: str):
    """Ten rounds of write_file and, with no pause, another file: each external file is reported once,
    no file the tool wrote ever is. Real stamps, nothing forged."""
    await quiet(ed)
    folder = f"scripts/{tag}Rounds"
    (ed.project / folder).mkdir(parents=True, exist_ok=True)
    await quiet(ed)
    seen: list = []
    for i in range(10):
        r = await write_file(ed, f"{folder}/Tool{i}.lua", f"return {{ round = {i} }}\n")
        seen += changed(r)
        (ed.project / f"{folder}/Ext{i}.lua").write_text(f"return {{ external = {i} }}\n")
    seen += changed(await status_call(ed))
    seen += changed(await status_call(ed))
    ed.check_eq(sorted(p for p in seen if "/Ext" in p), sorted(f"{folder}/Ext{i}.lua" for i in range(10)),
                "each external file reported exactly once")
    ed.check_eq([p for p in seen if "/Tool" in p], [], "no file write_file wrote was reported")


async def through_a_linked_folder(ed: Editor, tag: str, source: str):
    """A folder linked in from outside the project (S68) is a folder of the project: the changes behind
    the link are found under its name, with the same stamps as anywhere. A tool can't write through such
    a link (McpRefs::writeEscape), so the stamps here are of another program's earlier write."""
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    outside = Path(tempfile.mkdtemp(prefix="jasper-autosync-link-"))
    link = ed.project / f"scripts/{tag}Lib"
    link.parent.mkdir(parents=True, exist_ok=True)
    try:
        (outside / "Seed.lua").write_text("return {}\n")
        os.symlink(outside, link)
        ed.check_in(f"scripts/{tag}Lib/Seed.lua", changed(await status_call(ed)), "the linked folder's file is found")
        await quiet(ed)

        # a new file 0 ms after a writing call, with that call's stamp
        anchor = f"scripts/{tag}Anchor.lua"
        await write_file(ed, anchor, "return {}\n")
        (outside / "Inside.lua").write_text("return {}\n")
        stamp(outside / "Inside.lua", (ed.project / anchor).stat().st_mtime_ns)
        ext = external(await status_call(ed))
        ed.check(ext is not None and ext["changed"] == [f"scripts/{tag}Lib/Inside.lua"], f"found behind the link: {ext!r}")
        ed.check_eq(ext["source"], source, "found by")

        # a rewrite with the stamp a pick-up recorded: the content tells
        rewrite = outside / "Rewrite.lua"
        rewrite.write_text("return { value = 1 }\n")
        ed.check_in(f"scripts/{tag}Lib/Rewrite.lua", changed(await status_call(ed)), "the first write")
        before = rewrite.stat()
        rewrite.write_text("return { value = 2 }\n")
        stamp(rewrite, before.st_mtime_ns)
        ed.check_in(f"scripts/{tag}Lib/Rewrite.lua", changed(await status_call(ed)), "the same-stamp rewrite behind the link")

        # a new file with that stamp, the linked folder's own stamp unchanged
        (outside / "Three.lua").write_text("return {}\n")
        ed.check_in(f"scripts/{tag}Lib/Three.lua", changed(await status_call(ed)), "the folder's stamp is recorded")
        folder = outside.stat()
        three = (outside / "Three.lua").stat()
        (outside / "Four.lua").write_text("return {}\n")
        stamp(outside / "Four.lua", three.st_mtime_ns)
        stamp(outside, folder.st_mtime_ns)
        ext = external(await status_call(ed))
        ed.check(ext is not None and ext.get("added") == [f"scripts/{tag}Lib/Four.lua"], f"the same-tick new file behind the link: {ext!r}")
        r = await status_call(ed)
        ed.check(external(r) is None, f"reported twice: {external(r)!r}")
    finally:
        link.unlink(missing_ok=True)
        shutil.rmtree(outside, ignore_errors=True)
        await status_call(ed)


# ------------------------------------------------------------------ the watcher's journal


async def test_new_file_stamped_inside_the_call(ed: Editor):
    ed.require_tools("write_file", "editor_status")
    await new_file_stamped_inside_the_call(ed, "Journal", "watcher")


async def test_same_tick_rewrite_of_the_tools_file(ed: Editor):
    await same_tick_rewrite_of_the_tools_file(ed, "Journal")


async def test_same_tick_new_file_in_the_tools_folder(ed: Editor):
    await same_tick_new_file_in_the_tools_folder(ed, "Journal")


async def test_no_pause_rounds(ed: Editor):
    await no_pause_rounds(ed, "Journal")


async def test_through_a_linked_folder(ed: Editor):
    await through_a_linked_folder(ed, "Journal", "watcher")


# ------------------------------------------------------------------ the stat scan


async def test_watcher_off_the_scan_finds_the_same():
    async with Editor(create=True, name="AutoSyncRaceScan", label="scan", env=SCAN_ENV) as ed:
        status = await ed.call("editor_status")
        ed.check_eq(status["file_watcher"]["state"], "off", "the watcher is off")
        await new_file_stamped_inside_the_call(ed, "Scan", "scan")
        await same_tick_rewrite_of_the_tools_file(ed, "Scan")
        await same_tick_new_file_in_the_tools_folder(ed, "Scan")
        await no_pause_rounds(ed, "Scan")
        await through_a_linked_folder(ed, "Scan", "scan")


# ------------------------------------------------------------------ when the result goes out


async def test_writing_results_wait_for_their_settle_and_read_only_ones_do_not(ed: Editor):
    await quiet(ed)
    t0 = time.monotonic()
    await write_file(ed, "scripts/Timed.lua", "return {}\n")
    ed.check(time.monotonic() - t0 >= SETTLE_DELAY_S, "the result of a synchronous writing call went out before its settle")
    fastest = 1e9
    for _ in range(6):
        t0 = time.monotonic()
        await status_call(ed)
        fastest = min(fastest, time.monotonic() - t0)
    slow = over_budget("read-only call, its settle delay not paid", {"fastest": fastest}, SETTLE_DELAY_S)
    ed.check(not slow, f"a read-only call waited for a settle: {slow}")


async def test_deferred_completion_settles_before_its_result(ed: Editor):
    """A call that completes in a waitUntil / afterFrames callback: settled at the end of tick."""
    await quiet(ed)
    await ed.screenshot(view="game", save_path="assets/deferred-shot.png")
    shot = ed.project / "assets/deferred-shot.png"
    ed.check(shot.is_file(), "the screenshot was saved")
    rel = "scripts/AfterDeferred.lua"
    (ed.project / rel).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / rel).write_text("return {}\n")
    stamp(ed.project / rel, shot.stat().st_mtime_ns)
    ext = external(await status_call(ed))
    ed.check(ext is not None and ext["changed"] == [rel], f"the external file is reported, the screenshot is not: {ext!r}")


async def test_completion_outside_a_handler_settles_before_its_result():
    """A detached call (commit_files: the version control service answers it from its own update):
    settled by a main-thread task before its result."""
    async with Editor(create=True, name="AutoSyncRaceDetached", label="detached", env=EDITOR["env"]) as ed:
        await status_call(ed)
        env = dict(os.environ, HOME=str(ed.scratch.home), GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0", LC_ALL="C")
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            env.pop(name, None)

        def git(*args: str) -> None:
            done = subprocess.run(["git", *args], cwd=ed.project, env=env, capture_output=True, text=True, timeout=60)
            ed.check(done.returncode == 0, f"git {' '.join(args)}: {done.stderr}")

        git("init", "-q")
        git("config", "user.name", "Agent")
        git("config", "user.email", "agent@example.com")
        git("config", "commit.gpgsign", "false")
        (ed.project / ".gitignore").write_text(".jasperengine/\n")
        (ed.project / "notes.txt").write_text("one\n")
        git("add", "-A")
        git("commit", "-q", "-m", "initial")
        status = await ed.call("get_vcs_status", refresh=True)
        ed.check_eq(status.get("state"), "active", f"after git init: {status}")
        (ed.project / "notes.txt").write_text("two\n")
        await quiet(ed)

        wall = time.monotonic()
        committed = await ed.call("commit_files", paths=["notes.txt"], message="Notes")
        ed.check(committed.get("revision"), f"{committed}")
        ed.check(time.monotonic() - wall >= SETTLE_DELAY_S, "the detached call's result went out before its settle")
        rel = "scripts/AfterDetached.lua"
        (ed.project / rel).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / rel).write_text("return {}\n")
        # the coarse clock's stamp from inside the call: an instant before its result
        stamp(ed.project / rel, time.time_ns() - 1_000_000)
        ext = external(await status_call(ed))
        ed.check(ext is not None and ext["changed"] == [rel], f"the external file is reported: {ext!r}")


async def test_editor_closes_with_a_writing_call_waiting():
    """A writing call waiting for its job when the client goes away: stop() fails it and runs the settle
    queue, and the editor exits cleanly (a client that is gone gets no answer)."""
    async with RawEditor(create=True, name="AutoSyncRaceStop", env=EDITOR["env"]) as raw:
        await raw.initialize()

        def call(id: int, name: str, **arguments) -> None:
            raw.send({"jsonrpc": "2.0", "id": id, "method": "tools/call", "params": {"name": name, "arguments": arguments}})

        call(21, "save_scene", path="Stop.scene")
        if (await raw.response(21)).get("result", {}).get("isError", True):
            raw.fail("save_scene failed")
        example = raw.scratch.project / "assets/stop_example.png"
        example.parent.mkdir(parents=True, exist_ok=True)
        example.write_bytes(encode_png(64, 64, os.urandom(64 * 64 * 4)))
        call(22, "synthesize_texture", examples=["assets/stop_example.png"], size=1024, quality="high", seed=4,
             companions="none", output="assets/stop_out.png", timeout_seconds=600)
        await asyncio.sleep(0.5)
        if any(m.get("id") == 22 for m in raw.messages):
            raw.fail(f"the call ended by itself: {raw.messages[-1]}")
        code = await raw.close_stdin_and_wait(120)
        if code != 0:
            raw.fail(f"the editor must exit cleanly, got {code!r}")


# ------------------------------------------------------------------ dialogs (S69)


async def open_dialog(ed: Editor) -> dict:
    found = (await ed.call("editor_status")).get("dialog")
    ed.check(found, "the dialog never opened")
    return found


async def test_a_call_a_dialog_refuses_settles_too(ed: Editor):
    """A writing call refused because a modal is open has run through execute: it waits for its settle
    like any other, and what is written once it has the refusal is external."""
    await quiet(ed)
    await ed.call("run_command", id="help.about")
    opened = await open_dialog(ed)
    try:
        wall = time.monotonic()
        r = await ed.call_raw("write_file", {"path": "scripts/RefusedByDialog.lua", "content": "return {}\n"})
        ed.check(r.is_error and opened["title"] in r.text, f"refused: {r.text[:300]}")
        ed.check(time.monotonic() - wall >= SETTLE_DELAY_S, "the refusal went out before its settle")
        ed.check(not (ed.project / "scripts/RefusedByDialog.lua").exists(), "nothing written")
        rel = "scripts/AfterRefusal.lua"
        (ed.project / rel).write_text("return {}\n")
        stamp(ed.project / rel, time.time_ns() - 1_000_000)
        ext = external(await status_call(ed))
        ed.check(ext is not None and ext["changed"] == [rel], f"reported: {ext!r}")
    finally:
        await ed.call("respond_dialog", choice="ok")


async def test_a_writing_call_that_opens_a_file_picker_answers_at_once(ed: Editor):
    """The picker holds the main thread until it is answered, and its client has this result to wait for:
    the result can't wait for the settle, which runs once the thread is back. Then the tool's window is
    closed as usual: what is written after that is external."""
    await quiet(ed)
    r = await ed.call_raw("run_command", {"id": "assets.import"}, timeout=60)
    ed.check(not r.is_error, f"run_command: {r.text[:300]}")
    ed.check_eq(r.structured["dialog"]["kind"], "file_picker", "the tool says so")
    refused = await ed.call_raw("create_entity", {"name": "DuringPicker", "type": "cube"}, timeout=60)
    ed.check(refused.is_error and "respond_dialog" in refused.text, f"answered without the main thread: {refused.text[:300]}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq((await ed.call("editor_status")).get("dialog"), None, "cancelled")
    await quiet(ed)
    await write_file(ed, "scripts/AfterPicker.lua", "return {}\n")
    rel = "scripts/AfterPickerExternal.lua"
    (ed.project / rel).write_text("return {}\n")
    stamp(ed.project / rel, (ed.project / "scripts/AfterPicker.lua").stat().st_mtime_ns)
    ext = external(await status_call(ed))
    ed.check(ext is not None and ext["changed"] == [rel], f"reported, the tool's own not: {ext!r}")


# ------------------------------------------------------------------ R3, R4 and what they cost


async def test_change_stamped_before_the_call_stays_external(ed: Editor):
    """Found by the settle but stamped before the call started (`cp -p`, a checkout that keeps times,
    landing while the call ran): not the tool's. Reported once, by this call or the next."""
    await quiet(ed)
    rel = "scripts/OldStamp.lua"
    running = asyncio.create_task(ed.call_raw("eval_lua", {"code": BUSY_LUA, "allow_edit_mode": True}))
    await asyncio.sleep(0.2)
    path = ed.project / rel
    path.write_text("return {}\n")
    stamp(path, time.time_ns() - 3_600_000_000_000)
    r = await running
    ed.check(not r.is_error, f"eval_lua: {r.text[:300]}")
    seen = changed(r) + changed(await status_call(ed)) + changed(await status_call(ed))
    ed.check_eq(seen, [rel], "reported exactly once")


async def test_rewrite_with_the_same_stamp_reloads(ed: Editor):
    """The same-stamp rewrite is applied, not only reported: its stamp is racy for the auto-sync (2 s)
    but no longer for the hot reload's applied stamps (100 ms), which call the file current."""
    script = 'return { properties = { { name = "%s", type = "int", default = 1 } } }\n'
    for attempt in range(6):
        name = f"SameStamp{attempt}"
        entity = (await ed.call("create_entity", name=name + "Host", type="cube"))["entity_id"]
        rel = (await ed.call("create_script", kind="lua", name=name, content=script % "alpha", entity=entity))["path"]
        path = ed.project / rel
        await quiet(ed)
        path.write_text(script % "bravo")
        mtime = time.time_ns() - 400_000_000
        stamp(path, mtime)
        ed.check_in(rel, changed(await status_call(ed)), "the first rewrite is reported")
        if (time.time_ns() - mtime) / 1e9 >= RACY_S:
            continue    # so loaded that the pick-up saw an old stamp: nothing to conclude
        path.write_text(script % "carol")
        stamp(path, mtime)
        ed.check_in(rel, changed(await status_call(ed)), "the same-stamp rewrite is reported")
        listed = (await ed.call("list_scripts", entity=entity))["scripts"][0]
        ed.check_eq([p["name"] for p in listed["properties"]], ["carol"], "the content the rewrite left is the one loaded")
        return
    ed.fail("every attempt was too slow to conclude anything")


async def test_a_large_fresh_file_costs_no_full_hash(ed: Editor):
    """A 48 MB tracked file just written: the scans around the next writing calls read a fixed part of
    it, and a same-size rewrite in the same tick is still found. Oracle: the scans' own CPU time
    (editor_status.auto_sync)."""
    eid = find_key(await ed.call("create_entity", name="LargeFresh", type="cube"), "entity_id", "id")
    await quiet(ed)
    big = ed.project / "assets/big_fresh.png"
    big.parent.mkdir(parents=True, exist_ok=True)
    payload = bytearray(os.urandom(48 << 20))
    big.write_bytes(payload)
    worst = {"scan_cpu_ms": 0.0, "settle_cpu_ms": 0.0}
    seen: list = []
    for i in range(4):
        r = await ed.call_raw("set_properties", {"entity": eid, "values": {"transform": {"position": [i, 0, 0]}}})
        ed.check(not r.is_error, r.text[:300])
        seen += changed(r)
        stats = (await ed.call("editor_status"))["auto_sync"]
        for key in worst:
            worst[key] = max(worst[key], stats[key])
    print(f"[autosync race] worst scan/settle CPU around a fresh 48 MB file: {worst}")
    ed.check_in("assets/big_fresh.png", seen, "the external file is reported")
    # the thread's CPU time, which a loaded host does not inflate: not a wall-clock budget
    ed.check(worst["scan_cpu_ms"] < 20 and worst["settle_cpu_ms"] < 20, f"the scans hashed the large file: {worst} ms of CPU")

    # stamped again so it is racy now, whatever the calls above took
    now = time.time_ns()
    stamp(big, now)
    seen = changed(await status_call(ed))
    ed.check_in("assets/big_fresh.png", seen, "the new stamp is a change")
    payload[10] ^= 0xff
    big.write_bytes(payload)
    stamp(big, now)
    ed.check_in("assets/big_fresh.png", changed(await status_call(ed)), "the same-stamp rewrite of the large file is reported")
