# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Deterministic checks of one eval sample (no model judges).

Argument specs match partially: dict keys recursively, "re:<regex>" for a string, "*" for any
value of a present key. A sample's trace is what the agent did, in order: every MCP call of the editor server (name
without the client's prefix, its arguments), every shell command, and the final reply. A
case's `checks` (cases.py) are:

    calls           [{tool, args?}]   each appears at least once (args: partial match)
    must_not_call   [{tool, args?}]   none appears
    order           [[a, b], ...]     a's first call comes before b's first call (both appear)
    max_calls       {tool|"*": n}     at most n calls of that tool ("*": every editor call)
    min_calls       {tool: n}         at least n calls of that tool
    last_call       {tool, args?}     the last editor call matches (the turn ends after it)
    shell_must      [regex]           some shell command matches
    shell_must_not  [regex]           no shell command matches
    reply_must      [regex]           the final reply matches (case-insensitive)
    reply_must_not  [regex]           the final reply does not match
    edits_must_not  [regex]           no Write/Edit/MultiEdit path matches
    max_errors      {tool|"*": n}     at most n of those calls returned isError
    file_must       {path: [regex]}   the project file (after the run) matches each
    file_must_not   {path: [regex]}   the project file matches none (a missing file fails file_must only)
    any_of          [checks, ...]     at least one of these check sets passes

grade() returns (passed, [failure messages]).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

MCP_PREFIX = "mcp__jasper__"
EDIT_TOOLS = ("Write", "Edit", "MultiEdit")


@dataclass
class Trace:
    calls: list[dict] = field(default_factory=list)      # {"tool", "args"}
    shell: list[str] = field(default_factory=list)
    other: list[dict] = field(default_factory=list)      # other harness tools: {"tool", "args"}
    reply: str = ""
    files: dict = field(default_factory=dict)            # project-relative path -> text, after the run
    turns: int = 0
    cost_usd: float = 0.0
    error: str = ""


def parse_stream(lines) -> Trace:
    """A Claude Code `--output-format stream-json` transcript."""
    trace = Trace()
    last_text = ""
    by_id: dict[str, dict] = {}
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        kind = ev.get("type")
        if kind == "assistant":
            texts = []
            for block in (ev.get("message") or {}).get("content") or []:
                if block.get("type") == "tool_use":
                    name, args = block.get("name", ""), block.get("input") or {}
                    if name.startswith(MCP_PREFIX):
                        call = {"tool": name[len(MCP_PREFIX):], "args": args, "is_error": False}
                        trace.calls.append(call)
                        by_id[block.get("id", "")] = call
                    elif name == "Bash":
                        trace.shell.append(str(args.get("command", "")))
                    else:
                        trace.other.append({"tool": name, "args": args})
                elif block.get("type") == "text":
                    texts.append(block.get("text", ""))
            if texts:
                last_text = "\n".join(texts)
        elif kind == "user":
            content = (ev.get("message") or {}).get("content")
            for block in content if isinstance(content, list) else []:
                if block.get("type") == "tool_result" and block.get("tool_use_id") in by_id:
                    by_id[block["tool_use_id"]]["is_error"] = bool(block.get("is_error"))
        elif kind == "result":
            trace.reply = ev.get("result") or last_text
            trace.turns = int(ev.get("num_turns") or 0)
            trace.cost_usd = float(ev.get("total_cost_usd") or 0.0)
            if ev.get("is_error") or ev.get("subtype") not in (None, "success"):
                trace.error = str(ev.get("subtype") or "error")
    if not trace.reply:
        trace.reply = last_text
    return trace


def args_match(expected, actual) -> bool:
    """Partial match: expected dict keys recursively; a string starting with "re:" is a regex."""
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(k in actual and args_match(v, actual[k]) for k, v in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(expected) <= len(actual) and all(
            args_match(e, a) for e, a in zip(expected, actual))
    if expected == "*":
        return True
    if isinstance(expected, str) and expected.startswith("re:"):
        return isinstance(actual, str) and re.search(expected[3:], actual) is not None
    return expected == actual


def call_matches(spec, call: dict) -> bool:
    if isinstance(spec, str):
        spec = {"tool": spec}
    return spec["tool"] == call["tool"] and args_match(spec.get("args") or {}, call["args"])


def first_index(spec, calls: list[dict]) -> int:
    for i, call in enumerate(calls):
        if call_matches(spec, call):
            return i
    return -1


def describe(spec) -> str:
    if isinstance(spec, str):
        return spec
    return spec["tool"] + (json.dumps(spec["args"], sort_keys=True) if spec.get("args") else "")


def check(checks: dict, trace: Trace) -> list[str]:
    fails: list[str] = []
    calls = trace.calls
    for spec in checks.get("calls", []):
        if first_index(spec, calls) < 0:
            fails.append(f"missing call {describe(spec)}")
    for spec in checks.get("must_not_call", []):
        if first_index(spec, calls) >= 0:
            fails.append(f"forbidden call {describe(spec)}")
    for a, b in checks.get("order", []):
        ia, ib = first_index(a, calls), first_index(b, calls)
        if ia < 0 or ib < 0 or ia > ib:
            fails.append(f"order {describe(a)} before {describe(b)} (got {ia}, {ib})")
    for tool, limit in (checks.get("max_calls") or {}).items():
        n = len(calls) if tool == "*" else sum(1 for c in calls if c["tool"] == tool)
        if n > limit:
            fails.append(f"{n} calls of {tool} (max {limit})")
    for tool, least in (checks.get("min_calls") or {}).items():
        n = sum(1 for c in calls if c["tool"] == tool)
        if n < least:
            fails.append(f"{n} calls of {tool} (min {least})")
    if checks.get("last_call"):
        if not calls or not call_matches(checks["last_call"], calls[-1]):
            got = describe({"tool": calls[-1]["tool"]}) if calls else "none"
            fails.append(f"last call is {got}, not {describe(checks['last_call'])}")
    for pattern in checks.get("shell_must", []):
        if not any(re.search(pattern, cmd) for cmd in trace.shell):
            fails.append(f"no shell command matches /{pattern}/")
    for pattern in checks.get("shell_must_not", []):
        hits = [cmd for cmd in trace.shell if re.search(pattern, cmd)]
        if hits:
            fails.append(f"shell /{pattern}/ matched: {hits[0][:120]}")
    for pattern in checks.get("reply_must", []):
        if not re.search(pattern, trace.reply, re.IGNORECASE | re.DOTALL):
            fails.append(f"reply lacks /{pattern}/")
    for pattern in checks.get("reply_must_not", []):
        if re.search(pattern, trace.reply, re.IGNORECASE | re.DOTALL):
            fails.append(f"reply has /{pattern}/")
    edited = [str(c["args"].get("file_path", "")) for c in trace.other if c["tool"] in EDIT_TOOLS]
    for pattern in checks.get("edits_must_not", []):
        hits = [path for path in edited if re.search(pattern, path)]
        if hits:
            fails.append(f"edited {hits[0]}")
    for tool, limit in (checks.get("max_errors") or {}).items():
        n = sum(1 for c in calls if c.get("is_error") and (tool == "*" or c["tool"] == tool))
        if n > limit:
            fails.append(f"{n} failed calls of {tool} (max {limit})")
    for path, patterns in (checks.get("file_must") or {}).items():
        text = trace.files.get(path)
        if text is None:
            fails.append(f"{path} missing")
            continue
        for pattern in patterns:
            if not re.search(pattern, text):
                fails.append(f"{path} lacks /{pattern}/")
    for path, patterns in (checks.get("file_must_not") or {}).items():
        for pattern in patterns:
            if path in trace.files and re.search(pattern, trace.files[path]):
                fails.append(f"{path} has /{pattern}/")
    alternatives = checks.get("any_of") or []
    if alternatives:
        results = [check(alt, trace) for alt in alternatives]
        if all(results):
            fails.append("none of any_of passed: " + " | ".join("; ".join(r) for r in results))
    return fails


def grade(case: dict, trace: Trace) -> tuple[bool, list[str]]:
    fails = check(case.get("checks") or {}, trace)
    if trace.error and trace.error != "error_max_turns" and not fails:
        # an API error or a timeout proves nothing either way; the turn cap ends a run after its
        # decision (the checks judge it; a missing final reply fails reply_must)
        fails.append(f"run ended with {trace.error}")
    return (not fails, fails)


def verdict(sample_passes: list[bool]) -> bool:
    """A case fails when half or more of its samples fail."""
    fails = sum(1 for p in sample_passes if not p)
    return fails * 2 < len(sample_passes)
