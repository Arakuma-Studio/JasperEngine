# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S80 Deviation 61: a Title Art render does not wait on the main thread for a program still being
built. JASPER_TEST_TITLE_PROGRAM_BUILD=delay:<n> (editor/App.cpp) builds every Title Art program only
once n main-loop iterations passed since it was first asked for. A render that waited on the main
thread never saw one built (no iteration ran while it waited) and failed after 60 s ("a title art
program was not built within 60 s", the red run); now the render reports not ready, the frames go
on, and the tools that need the image wait for it: view_asset, check_title's pixel checks and
list_title_library's sheet return what a quick host returns. `fail`: a program that cannot be built
is still an error, at once.

A program built for one test is built for the next: the first test runs on the file's editor, each
other on an editor of its own."""

import asyncio

from harness import Editor

DELAYED = {"JASPER_TEST_TITLE_PROGRAM_BUILD": "delay:30"}

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "Title Programs", "env": DELAYED}
PATH = "assets/titles/hero.titleart"
LOOK = "casino/outline-gold"


async def new_title(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    # no bake: view_asset then renders (it shows a fresh output as baked)
    await ed.call("edit_title", path=PATH, create={"look": LOOK, "text": "HERO"}, bake="never")


async def finished(ed: Editor, reply: dict) -> dict:
    """The job's result when the call handed back its job_id."""
    if reply.get("status") != "running":
        return reply
    for _ in range(3000):
        job = await ed.call("get_job", job_id=reply["job_id"])
        if job["status"] != "running":
            ed.check_eq(job["status"], "succeeded", str(job)[:400])
            return job["result"]
        await asyncio.sleep(0.2)
    ed.fail(f"job {reply['job_id']} still running")


async def test_view_asset_waits_for_a_program_being_built(ed: Editor):
    await new_title(ed)
    r = await ed.call_raw("view_asset", {"path": PATH})
    ed.check(not r.is_error, f"view_asset renders once the programs are built: {r.text[:300]}")
    ed.check_eq(len(r.images), 1, "one image")
    ed.check("a render" in (r.structured or {}).get("shows", ""), f"it is a render: {r.text[:200]}")


async def test_check_title_pixel_checks_wait_for_programs(ed: Editor):
    async with Editor(create=True, name="Title Programs Check", env=DELAYED) as slow:
        await new_title(slow)
        report = await finished(slow, await slow.call("check_title", path=PATH, backgrounds=["black", "white"], render_scale=0.25))
        checks = {c["id"]: c for c in report["checks"]}
        ed.check("fallback_parity" in checks, f"the pixel checks ran: {sorted(checks)}")
        for cid, check in checks.items():
            message = str(check.get("message", ""))
            ed.check("not built" not in message and "not_ready" not in message, f"{cid}: no render gave up on a program: {message[:200]}")
        ed.check(checks["fallback_parity"]["status"] != "FAIL", f"fallback_parity: {checks['fallback_parity']}")


async def test_library_sheet_waits_for_programs(ed: Editor):
    async with Editor(create=True, name="Title Programs Sheet", env=DELAYED) as slow:
        await slow.call("set_project_settings", values={"title_art.auto_bake": False})
        r = await slow.call_raw("list_title_library", {"kind": "looks", "sheet": {"ids": [LOOK]}})
        ed.check(not r.is_error, r.text[:300])
        result = await finished(slow, r.structured)
        ed.check_eq(len(result["rows"]), 1, f"one cell: {result}")
        ed.check("error" not in result["rows"][0], f"the cell rendered: {result['rows'][0]}")


async def test_a_program_that_cannot_be_built_is_an_error(ed: Editor):
    async with Editor(create=True, name="Title Programs Fail", env={"JASPER_TEST_TITLE_PROGRAM_BUILD": "fail"}) as broken:
        await new_title(broken)
        message = await broken.check_error("view_asset", {"path": PATH}, contains="could not be built")
        ed.check("not built within" not in message, f"a failure, not a wait: {message[:300]}")
