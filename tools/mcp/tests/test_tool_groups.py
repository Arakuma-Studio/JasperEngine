"""Tool groups (S31 R17-R19): tools/list hides a group's tools until a session enables them, and
every tool stays callable whatever is listed.

The test-only group "selftest" exists under JASPER_MCP_SELFTEST_GROUP=1 with one tool,
selftest_echo. It is absent from the default tools/list and still callable directly and inside
batch; tool_groups {enable: ["selftest"]} sends notifications/tools/list_changed (on stdio at
once, over HTTP inside the call's SSE response; without text/event-stream the result says to
re-list), after which that session's tools/list holds it. `mcp --tool-groups` and the settings
key mcp.tool_groups choose the groups sessions start with; an unknown name exits 2. The agent
guide names tool_groups and stays within 16,384 bytes (the headroom is tool_budget.py --check's).

This is the pattern every spec's group follows: one assertion that its tools are hidden by
default and listed once tool_groups enables the group (docs/mcp/conventions.md).
"""

from __future__ import annotations

CI = True

import asyncio
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, HttpSession, RawEditor, free_port, tool_meta, wait_for_port
import tool_budget

SELFTEST_ENV = {"JASPER_MCP_SELFTEST_GROUP": "1"}
EDITOR = {"create": True, "env": SELFTEST_ENV}
TOOL = "selftest_echo"
LIST_CHANGED = "notifications/tools/list_changed"


def names(tools) -> set[str]:
    return {t["name"] if isinstance(t, dict) else t.name for t in tools}


async def test_hidden_by_default(ed: Editor):
    default = names(await ed.tools(groups=None))
    ed.check(TOOL not in default, "a group tool is not in the default tools/list")
    ed.check("tool_groups" in default and "create_entity" in default, "core tools are listed")
    everything = await ed.tools(groups="all")
    ed.check(TOOL in names(everything), "_meta jasper/groups all lists it")
    ed.check_eq(tool_meta(ed.tool(TOOL)).get("jasper/group"), "selftest", "its _meta group")
    ed.check_eq(tool_meta(ed.tool("create_entity")).get("jasper/group"), "core", "a core tool's _meta group")
    ed.check(tool_meta(ed.tool("create_entity")).get("jasper/module"), "a core tool's _meta module")
    chosen = names(await ed.tools(groups=["selftest"]))
    ed.check(TOOL in chosen and "create_entity" in chosen, "a list of groups adds them to core")
    core = {t.name for t in everything if tool_meta(t).get("jasper/group") == "core"}
    ed.check_eq(chosen, core | {TOOL}, "core + selftest exactly")


async def test_callable_without_enabling(ed: Editor):
    r = await ed.call(TOOL, text="direct")
    ed.check_eq(r["text"], "direct", "called directly while hidden")
    b = await ed.batch([(TOOL, {"text": "in batch"}), ("editor_status", {})])
    ed.check_eq(b["succeeded"], 2, f"called inside batch while hidden: {b}")
    ed.check_eq(b["results"][0]["structured"]["text"], "in batch", "batch result")


async def test_tool_groups_report(ed: Editor):
    r = await ed.call("tool_groups")
    groups = {g["name"]: g for g in r["groups"]}
    ed.check_eq(list(groups)[0], "core", "core first")
    ed.check(groups["core"]["enabled"] and "tool_groups" in groups["core"]["tools"], f"core: {groups['core']}")
    ed.check(not groups["selftest"]["enabled"] and groups["selftest"]["tools"] == [TOOL], f"selftest: {groups['selftest']}")
    ed.check(groups["selftest"]["chars"] > 0 and groups["core"]["chars"] > 50000, "sizes are measured")
    for name in ("profiling", "splines", "navigation", "particles", "animation", "gameplay", "vcs", "rendering_fx",
                 "editor_tools", "assets_pipeline", "diagnostics", "editor_ux"):
        ed.check(name in groups and groups[name]["description"], f"the spec groups of S31 §6.4: {name}")
    msg = await ed.check_error("tool_groups", enable=["nope"], contains="Unknown tool group")
    ed.check("selftest" in msg and "profiling" in msg, f"the error lists the groups: {msg}")
    await ed.check_error("tool_groups", disable=["core"], contains="core")
    # the harness session itself: enabling twice is idempotent, disabling restores the listing
    r = await ed.call("tool_groups", enable=["selftest"])
    ed.check(r["list_changed"] is True, f"stdio receives the notification: {r}")
    r = await ed.call("tool_groups", enable=["selftest"])
    ed.check(r["list_changed"] is False, "nothing changed, nothing sent")
    ed.check(TOOL in names(await ed.tools(groups=None)), "listed once enabled")
    await ed.call("tool_groups", disable=["selftest"])
    ed.check(TOOL not in names(await ed.tools(groups=None)), "hidden again once disabled")


async def test_enable_on_stdio_notifies():
    async with RawEditor(create=True, env=SELFTEST_ENV) as raw:
        init = await raw.initialize()
        caps = init["result"]["capabilities"]
        if caps["tools"].get("listChanged") is not True:
            raw.fail(f"initialize advertises tools.listChanged: {caps}")
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if TOOL in names((await raw.response(2))["result"]["tools"]):
            raw.fail("hidden before tool_groups")
        raw.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                  "params": {"name": "tool_groups", "arguments": {"enable": ["selftest"]}}})
        resp = await raw.response(3)
        if resp["result"]["isError"]:
            raw.fail(f"tool_groups failed: {resp}")
        order = [m.get("method") or m.get("id") for m in raw.messages]
        if LIST_CHANGED not in order or order.index(LIST_CHANGED) > order.index(3):
            raw.fail(f"list_changed before the tool_groups response on stdio: {order}")
        raw.send({"jsonrpc": "2.0", "id": 4, "method": "tools/list"})
        if TOOL not in names((await raw.response(4))["result"]["tools"]):
            raw.fail("listed after tool_groups enabled it")
        # inside batch: the notification goes out through the batch's request
        before = order.count(LIST_CHANGED)
        raw.send({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "batch", "arguments": {
            "calls": [{"tool": "tool_groups", "arguments": {"disable": ["selftest"]}}]}}})
        await raw.response(5)
        after = [m.get("method") for m in raw.messages].count(LIST_CHANGED)
        if after != before + 1:
            raw.fail(f"tool_groups inside batch notifies too ({before} -> {after})")
        raw.send({"jsonrpc": "2.0", "id": 6, "method": "tools/list"})
        if TOOL in names((await raw.response(6))["result"]["tools"]):
            raw.fail("hidden again after the batch disabled it")


async def test_enable_over_http_sse():
    port = free_port()
    async with RawEditor(create=True, env=SELFTEST_ENV, extra_args=["--http-port", str(port)]) as raw:
        await raw.initialize()
        await wait_for_port(port)
        http = HttpSession(port, token=harness.HTTP_TOKEN)
        await asyncio.to_thread(http.initialize)
        other = HttpSession(port, token=harness.HTTP_TOKEN)
        await asyncio.to_thread(other.initialize)

        a = await asyncio.to_thread(http.request, "tools/list")
        if TOOL in names(a.response["result"]["tools"]):
            raw.fail("hidden in a new HTTP session")
        a = await asyncio.to_thread(http.request, "tools/call",
                                    {"name": "tool_groups", "arguments": {"enable": ["selftest"]}}, sse=True)
        if not a.sse:
            raw.fail(f"the call's answer is an SSE stream: {a.headers} {a.body[:300]}")
        if not a.notifications(LIST_CHANGED) or a.messages[0].get("method") != LIST_CHANGED:
            raw.fail(f"list_changed first inside the SSE stream: {a.messages}")
        if a.response["result"]["structuredContent"]["list_changed"] is not True:
            raw.fail(f"result says it was sent: {a.response}")
        a = await asyncio.to_thread(http.request, "tools/list")
        if TOOL not in names(a.response["result"]["tools"]):
            raw.fail("listed in that session afterwards")
        # per session: the other HTTP session and the stdio one still hide it
        a = await asyncio.to_thread(other.request, "tools/list")
        if TOOL in names(a.response["result"]["tools"]):
            raw.fail("another session's listing is unchanged")
        raw.send({"jsonrpc": "2.0", "id": 9, "method": "tools/list"})
        if TOOL in names((await raw.response(9))["result"]["tools"]):
            raw.fail("the stdio session's listing is unchanged")
        # a client that does not accept text/event-stream: plain JSON, told to re-list
        a = await asyncio.to_thread(other.request, "tools/call",
                                    {"name": "tool_groups", "arguments": {"enable": ["selftest"]}}, sse=False)
        result = a.response["result"]
        text = result["content"][0]["text"]
        if a.sse or result["structuredContent"]["list_changed"] is not False or "tools/list again" not in text:
            raw.fail(f"without SSE: no notification, the text says to re-list: {a.headers} {text}")
        a = await asyncio.to_thread(other.request, "tools/list")
        if TOOL not in names(a.response["result"]["tools"]):
            raw.fail("listed after re-listing")
        # a call that sends nothing before its answer stays plain JSON even when SSE is accepted
        a = await asyncio.to_thread(http.request, "tools/call", {"name": "editor_status", "arguments": {}}, sse=True)
        if a.sse or not a.response:
            raw.fail(f"no notification: application/json: {a.headers}")


async def test_tool_groups_flag():
    with tempfile.TemporaryDirectory(prefix="jasper-groups-") as tmp:
        proc = await asyncio.to_thread(
            subprocess.run, harness.editor_command() + ["mcp", "--tool-groups", "nope", "--create", str(Path(tmp) / "p")],
            capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
    if proc.returncode != 2 or "unknown tool group" not in proc.stderr:
        raise harness.TestFailure(f"--tool-groups nope exits 2 naming it: {proc.returncode} {proc.stderr[-500:]}")
    async with RawEditor(create=True, env=SELFTEST_ENV, extra_args=["--tool-groups", "selftest"]) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if TOOL not in names((await raw.response(2))["result"]["tools"]):
            raw.fail("--tool-groups selftest lists it from the start")
    async with RawEditor(create=True, env=SELFTEST_ENV, extra_args=["--tool-groups", "all"]) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if TOOL not in names((await raw.response(2))["result"]["tools"]):
            raw.fail("--tool-groups all lists it from the start")


async def test_settings_default():
    async with RawEditor(create=True, env=SELFTEST_ENV, settings={"mcp": {"tool_groups": "selftest"}}) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        if TOOL not in names((await raw.response(2))["result"]["tools"]):
            raw.fail("settings mcp.tool_groups lists it from the start")


async def test_instructions_name_tool_groups(ed: Editor):
    text = ed.instructions
    ed.check("`tool_groups`" in text and "## Tool groups" in text, "the guide names tool_groups")
    ed.check("selftest" in text, "the guide names the groups that have tools")
    size = len(text.encode())
    limit = tool_budget.load_budget()["guide"]["bytes"]
    ed.check(size <= limit, f"the guide is {size} bytes, over {limit} (tools/mcp/budget.json)")
    print(f"     [guide] {size} bytes")


# The groups of the specs merged so far: hidden by default, listed once enabled (the pattern of
# docs/mcp/conventions.md every spec's group follows)
SPEC_GROUPS = {
    "diagnostics": ["get_job_system_stats", "set_job_system_config", "get_memory_stats",   # S06
                    "get_log_config", "set_log_level", "wait_for_log", "list_timers"],     # S09
    "data_safety": ["get_load_report", "list_scene_backups", "discard_scene_backup"],      # S03
    "terrain": ["sculpt_terrain", "set_terrain_maps", "sample_terrain", "edit_terrain", "visualize_terrain"],                   # S31 (budget)
    "editor_ux": ["get_undo_history", "get_play_changes", "apply_play_changes", "discard_play_changes",  # S24
                  "find_ui_item",                                                   # BF1 (UI tests)
                  "get_inspector_layout",                                           # S08
                  "pick_at", "edit_scene_view_bookmarks", "set_scene_view_layout", "measure_scene_view",  # S26
                  "set_scene_view_filter",
                  "search_editor", "run_command", "list_shortcuts", "set_shortcut", "edit_layout_presets",  # S25
                  "edit_asset_labels", "edit_saved_searches", "edit_bookmarks"],
    "rendering_fx": ["get_shadow_stats", "set_shadow_debug",                               # S10
                     "list_atmosphere_presets", "apply_atmosphere_preset", "get_atmosphere_info",  # S12
                     "inspect_color_lut", "get_post_processing_status", "set_post_processing_debug"],  # S13
    "assets_pipeline": ["resolve_asset", "find_references", "get_asset_dependencies", "list_missing_assets",  # S04
                        "retarget_asset_references", "clear_asset_references",
                        "get_texture_import_settings", "set_texture_import_settings", "reimport_textures",  # S14
                        "get_texture_cook_report"],
    "device": ["get_device_info", "set_device_simulation"],                                # S56
}


async def test_spec_groups_hidden_until_enabled(ed: Editor):
    for group, tools in SPEC_GROUPS.items():
        ed.require_tools(*tools)
        ed.check(all(tool_meta(ed.tool(t)).get("jasper/group") == group for t in tools), f"{group}: _meta group")
        default = names(await ed.tools(groups=None))
        ed.check(not default & set(tools), f"{group} tools hidden by default: {sorted(default & set(tools))}")
        await ed.call("tool_groups", enable=[group])
        ed.check(set(tools) <= names(await ed.tools(groups=None)), f"{group} tools listed once enabled")
        await ed.call("tool_groups", disable=[group])
