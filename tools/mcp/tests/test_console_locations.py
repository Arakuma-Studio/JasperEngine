"""Console click-to-source (S23 R27, R28, R32; AC18).

A Lua runtime error during Play: read_console's entry has locations[0] = the script's project
path and line, in_project true; code_editor open at that location opens the built-in editor at
the line. With Preferences > External Tools > External Script Editor = Custom, code_editor
target "preferred" runs the command with {file}:{line} as ONE argument even when the path has
spaces. (The C++ compile-error location is checked in test_script_status.py.)
"""

import asyncio
import os
import stat

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Console Locations CI"}

TIMEOUT = 600

FAULTY = """local Faulty = {}

function Faulty:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Faulty:onUpdate()
    self.frames = self.frames + 1
    if self.frames == 3 then
        local missing = nil
        missing.field = 1
    end
end

return Faulty
"""

ERROR_LINE = 12


async def test_lua_runtime_error_location_opens(ed: Editor):
    made = await ed.call("create_scene", name="Faulty", type="3D", path="scenes/Faulty")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Faulty")
    script = await ed.call("create_script", kind="lua", name="Faulty", content=FAULTY, scene=scene, entity=ent["entity_id"])
    path = script["path"]
    await ed.call("save_scene", scene=scene)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=10)
    await ed.call("play", action="stop")

    console = await ed.call("read_console", since=cursor, limit=500, types=["error"])
    errors = [e for e in console["entries"] if "Faulty" in e.get("message", "")]
    ed.check(errors, f"the runtime error is in the Console: {console['entries'][:5]}")
    first = errors[0]
    ed.check(first.get("locations"), f"the entry has locations: {first}")
    loc = first["locations"][0]
    ed.check_eq(loc["file"], path, f"locations[0] is the script's project path: {first['locations']}")
    ed.check_eq(loc["line"], ERROR_LINE, "and its line")
    ed.check_eq(loc["in_project"], True, "in_project")

    opened = await ed.call("code_editor", action="open", path=loc["file"], line=loc["line"])
    ed.check_eq(opened["opened_in"], "builtin", "the built-in code editor")
    buffers = {b["path"]: b for b in opened["buffers"]}
    ed.check(path in buffers, f"the file is open: {opened['buffers']}")
    for _ in range(20):
        status = await ed.call("code_editor", action="status")
        buffer = next((b for b in status["buffers"] if b["path"] == path), None)
        if buffer and buffer["cursor_line"] == ERROR_LINE:
            break
        await ed.call("wait", frames=2)
    ed.check(buffer and buffer["cursor_line"] == ERROR_LINE, f"the cursor is on the error line: {buffer}")
    await ed.call("code_editor", action="close", path=path, discard_unsaved=True)


async def test_custom_external_editor_gets_whole_paths(ed: Editor):
    folder = ed.scratch.root / "ext editor"
    folder.mkdir(parents=True, exist_ok=True)
    out = folder / "args.txt"
    recorder = folder / "record.sh"
    recorder.write_text('#!/bin/sh\nfor a in "$@"; do printf "%s\\n" "$a"; done > "$(dirname "$0")/args.txt"\n')
    recorder.chmod(recorder.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    template = f'"{recorder}" "{{file}}:{{line}}" {{project}}'
    status = await ed.call("script_status", external_script_editor="custom", external_script_editor_command=template)
    ed.check_eq(status["settings"]["external_script_editor"], "custom", "the preference is set")
    try:
        target = "scripts/With Space.lua"
        await ed.call("write_file", path=target, content="return {}\n")
        opened = await ed.call("code_editor", action="open", path=target, line=1, target="preferred")
        ed.check_eq(opened["opened_in"], "custom", "the external editor")
        for _ in range(50):
            if out.exists() and out.read_text().strip():
                break
            await asyncio.sleep(0.1)
        ed.check(out.exists(), "the custom command ran")
        args = out.read_text().splitlines()
        ed.check_eq(len(args), 2, f"two arguments: {args}")
        ed.check(args[0].endswith("With Space.lua:1") and os.path.isabs(args[0].rsplit(":", 1)[0]),
                 f"{{file}}:{{line}} is one argument with the absolute path: {args}")
        ed.check_eq(os.path.realpath(args[1]), os.path.realpath(str(ed.project)), "{project} is the project folder")
    finally:
        await ed.call("script_status", external_script_editor="builtin", external_script_editor_command="")
