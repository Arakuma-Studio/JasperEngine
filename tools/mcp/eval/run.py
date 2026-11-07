# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Run the MCP text eval: real Claude Code, the editor's real tool text, canned results.

    python3 tools/mcp/eval/run.py --appdir /var/lib/orquester --account jasperinuwu[,jaspinggg] \\
        --surface ~/je-eval/surface [--model sonnet] [-k CASE_SUBSTRING] [--samples N] \\
        [--tool-search auto|off] [--patch PATCH.json] [--baseline OLD/results.json] [-j 4] \\
        --out ~/je-eval/runs/<name>

--surface is a directory dump_surface.py wrote from a built editor (surface.json and the
project/ every sample copies). --account takes Orquester-managed Claude accounts by label (or
the part before @) or id prefix: there is no default, and never the system login. The runner
points `claude` at the account's home (CLAUDE_CONFIG_DIR) with no API key in the environment,
loads none of the account's settings, CLAUDE.md, plugins or MCP servers (only the project's: a
new game's CLAUDE.md -> AGENTS.md, which the editor writes, is part of what the eval tests), and deletes the session files a
sample leaves in that home. Default: 2 samples per case, a third when they disagree;
--samples N runs exactly N. A case fails when half or more of its samples fail.
--tool-search off sends every tool schema up front (what Codex-style clients do); auto is
Claude Code's default (deferred MCP tools: names only until ToolSearch loads one).
--patch applies {"instructions": str, "tools": {name: {description?, inputSchema?}}} to the
surface, to try a wording before changing the editor.
"""

from __future__ import annotations

import argparse
import concurrent.futures as futures
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import grade  # noqa: E402
import world  # noqa: E402

SECRET_ENV = ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CLAUDE_CODE_OAUTH_TOKEN", "OPENAI_API_KEY")
HARNESS_TOOLS = ["mcp__jasper", "Bash", "Read", "Edit", "Write", "Glob", "Grep", "ToolSearch",
                 "ListMcpResourcesTool", "ReadMcpResourceTool", "TodoWrite"]
DISALLOWED = ["Agent", "Task", "WebFetch", "WebSearch", "NotebookEdit", "Skill", "SlashCommand"]


# -- accounts ----------------------------------------------------------------------------------
def load_accounts(appdir: Path, wanted: list[str]) -> list[dict]:
    index = appdir / "daemon" / "agent-accounts.json"
    if not index.is_file():
        sys.exit(f"{index} not found: pass the appdir Orquester (or box_lend_accounts) gave you")
    rows = [a for a in json.loads(index.read_text())["accounts"] if a.get("agent") == "claude"]
    picked = []
    for want in wanted:
        hits = [a for a in rows if a["id"].startswith(want) or a.get("label") == want
                or str(a.get("label", "")).split("@")[0] == want]
        if len(hits) != 1:
            sys.exit(f"account {want!r}: {len(hits)} matches among {[a.get('label') for a in rows]}")
        acc = hits[0]
        if acc.get("needsReauth"):
            sys.exit(f"account {acc.get('label')} needs reauth in Orquester")
        home = appdir / "daemon" / "agent-accounts" / "claude" / acc["id"] / "home"
        if not home.is_dir():
            sys.exit(f"{home} missing")
        picked.append({"id": acc["id"], "label": acc.get("label") or acc["id"][:8], "home": home})
    return picked


def slug(path: Path) -> str:
    return re.sub(r"[^A-Za-z0-9]", "-", str(path))


# -- surface -----------------------------------------------------------------------------------
def load_surface(surface_dir: Path, patch: Path | None) -> dict:
    surface = json.loads((surface_dir / "surface.json").read_text(encoding="utf-8"))
    if patch:
        p = json.loads(patch.read_text(encoding="utf-8"))
        if "instructions" in p:
            surface["instructions"] = p["instructions"]
        for t in surface["tools"]:
            for key, value in (p.get("tools") or {}).get(t["name"], {}).items():
                t[key] = value
    return surface


# -- history (prior turns) as a resumable Claude Code session ---------------------------------
def write_history(home: Path, cwd: Path, case: dict, model: str) -> str | None:
    turns = case.get("history") or []
    if not turns:
        return None
    sid = str(uuid.uuid4())
    folder = home / "projects" / slug(cwd)
    folder.mkdir(parents=True, exist_ok=True)
    parent = None
    lines = []
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")

    def push(kind: str, message: dict) -> None:
        nonlocal parent
        me = str(uuid.uuid4())
        lines.append({"parentUuid": parent, "isSidechain": False, "userType": "external", "cwd": str(cwd),
                      "sessionId": sid, "version": "2.1.0", "type": kind, "message": message,
                      "uuid": me, "timestamp": stamp})
        parent = me

    for turn in turns:
        if "user" in turn:
            push("user", {"role": "user", "content": turn["user"]})
            continue
        content = []
        if turn.get("assistant"):
            content.append({"type": "text", "text": turn["assistant"]})
        calls = turn.get("calls") or []
        ids = []
        for call in calls:
            tid = "toolu_" + uuid.uuid4().hex[:24]
            ids.append(tid)
            name = call["tool"] if call.get("native") else grade.MCP_PREFIX + call["tool"]
            content.append({"type": "tool_use", "id": tid, "name": name, "input": call.get("args") or {}})
        push("assistant", {"id": "msg_" + uuid.uuid4().hex[:24], "type": "message", "role": "assistant",
                           "model": model, "content": content, "stop_reason": "tool_use" if calls else "end_turn",
                           "usage": {"input_tokens": 0, "output_tokens": 0}})
        if calls:
            push("user", {"role": "user", "content": [
                {"type": "tool_result", "tool_use_id": tid, "content": call.get("result", ""),
                 "is_error": bool(call.get("is_error"))} for tid, call in zip(ids, calls)]})
    with (folder / f"{sid}.jsonl").open("w", encoding="utf-8") as f:
        for line in lines:
            f.write(json.dumps(line) + "\n")
    return sid


def resolve_mocks(case: dict, surface: dict) -> dict:
    """{"recorded": key, ...overrides} results become the result dump_surface.py recorded."""
    out = dict(case)
    out["mocks"] = []
    for mock in list(case.get("mocks") or []) + world.default_mocks(playing=bool(case.get("playing"))):
        mock = dict(mock)
        res = dict(mock.get("result") or {})
        if "recorded" in res:
            rec = surface.get("recorded", {}).get(res.pop("recorded"))
            if rec is None:
                sys.exit(f"{case['id']}: the surface recorded no {mock['result']['recorded']!r}")
            res = {"text": rec["text"], "structured": rec["structured"], "is_error": rec["is_error"], "real": True, **res}
        mock["result"] = res
        out["mocks"].append(mock)
    return out


# -- one sample --------------------------------------------------------------------------------
def run_sample(case: dict, n: int, account: dict, args, surface_file: Path, project: Path, out: Path) -> dict:
    sample = out / case["id"] / f"s{n}"
    if sample.exists():
        shutil.rmtree(sample)
    cwd = sample / "project"
    shutil.copytree(project, cwd)
    yaml = cwd / "project.yaml"
    if yaml.exists():  # the world's name for the project (world.py), on disk too
        yaml.write_text(re.sub(r"(?m)^(\s*name:\s*).*$", r"\1Space Dodge", yaml.read_text(), count=1))
    for rel, text in (case.get("files") or {}).items():
        (cwd / rel).parent.mkdir(parents=True, exist_ok=True)
        (cwd / rel).write_text(text, encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=cwd, check=True)
    subprocess.run(["git", "-c", "user.name=eval", "-c", "user.email=eval@localhost", "add", "-A"], cwd=cwd, check=True)
    subprocess.run(["git", "-c", "user.name=eval", "-c", "user.email=eval@localhost", "commit", "-qm", "project"],
                   cwd=cwd, check=True)
    case_file = sample / "case.json"
    case_file.write_text(json.dumps(resolve_mocks(case, json.loads(surface_file.read_text())), ensure_ascii=False),
                         encoding="utf-8")
    mcp = {"mcpServers": {"jasper": {"command": sys.executable, "args": [
        str(HERE / "stub_server.py"), "--surface", str(surface_file), "--case", str(case_file),
        "--log", str(sample / "calls.jsonl")]}}}
    (sample / "mcp.json").write_text(json.dumps(mcp), encoding="utf-8")
    hook = f"{sys.executable} {HERE / 'sim_hook.py'} --root {cwd} --case {case_file}"
    settings = {"hooks": {"PreToolUse": [{"matcher": "Bash|Read|Write|Edit|MultiEdit|Glob|Grep|NotebookEdit",
                                          "hooks": [{"type": "command", "command": hook}]}]}}
    (sample / "settings.json").write_text(json.dumps(settings), encoding="utf-8")

    env = {k: v for k, v in os.environ.items()
           if k not in SECRET_ENV and not k.startswith(("CLAUDE_CODE_", "CLAUDECODE", "MCP_", "ENABLE_TOOL_SEARCH"))}
    env["CLAUDE_CONFIG_DIR"] = str(account["home"])
    if args.tool_search == "off":
        env["ENABLE_TOOL_SEARCH"] = "false"
    env["DISABLE_AUTOUPDATER"] = "1"
    env["DISABLE_TELEMETRY"] = "1"
    sid = write_history(account["home"], cwd, case, args.model)
    cmd = ["claude", "-p", case["prompt"], "--model", args.model, "--output-format", "stream-json", "--verbose",
           "--mcp-config", str(sample / "mcp.json"), "--strict-mcp-config", "--setting-sources", "project",
           "--settings", str(sample / "settings.json"), "--disable-slash-commands",
           "--max-turns", str(case.get("max_turns", args.max_turns)),
           "--allowedTools", *HARNESS_TOOLS, "--disallowedTools", *DISALLOWED]
    cmd += ["--resume", sid] if sid else ["--no-session-persistence"]
    started = time.time()
    try:
        with (sample / "stream.jsonl").open("w", encoding="utf-8") as so, (sample / "stderr.log").open("w") as se:
            subprocess.run(cmd, cwd=cwd, env=env, stdout=so, stderr=se, stdin=subprocess.DEVNULL,
                           timeout=args.timeout, check=False)
        timed_out = False
    except subprocess.TimeoutExpired:
        timed_out = True
    finally:
        shutil.rmtree(account["home"] / "projects" / slug(cwd), ignore_errors=True)
    lines = (sample / "stream.jsonl").read_text(encoding="utf-8").splitlines()
    trace = grade.parse_stream(lines)
    if timed_out:
        trace.error = "timeout"
    trace.files = project_files(cwd)
    init = next((json.loads(l) for l in lines if '"subtype":"init"' in l.replace(" ", "")), {})
    passed, fails = grade.grade(case, trace)
    unmocked = []
    if (sample / "calls.jsonl").exists():
        unmocked = [json.loads(l)["tool"] for l in (sample / "calls.jsonl").read_text().splitlines()
                    if json.loads(l).get("unmocked")]
    row = {"case": case["id"], "kind": case["kind"], "sample": n, "account": account["label"], "model": args.model,
           "tool_search": args.tool_search, "passed": passed, "fails": fails,
           "calls": [c["tool"] for c in trace.calls], "shell": trace.shell[:10], "unmocked": unmocked,
           "turns": trace.turns, "cost_usd": round(trace.cost_usd, 4), "seconds": round(time.time() - started, 1),
           "isolation": isolation_problems(init), "reply": trace.reply[-600:]}
    (sample / "result.json").write_text(json.dumps(row, indent=1, ensure_ascii=False), encoding="utf-8")
    return row


def project_files(root: Path) -> dict:
    out = {}
    for path in root.rglob("*"):
        if ".git" in path.parts or not path.is_file() or path.stat().st_size > 512_000:
            continue
        try:
            out[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
    return out


def isolation_problems(init: dict) -> list[str]:
    """What of the account's own setup leaked into the sample (from the init event)."""
    problems = []
    servers = [s.get("name") for s in init.get("mcp_servers") or []]
    if servers != ["jasper"]:
        problems.append(f"mcp servers {servers}")
    plugins = [p.get("name") for p in init.get("plugins") or [] if p.get("path") != "builtin"]
    if plugins:
        problems.append(f"plugins {plugins}")
    if init.get("skills"):
        problems.append(f"skills {init['skills'][:5]}")
    for s in init.get("mcp_servers") or []:
        if s.get("name") == "jasper" and s.get("status") != "connected":
            problems.append(f"jasper {s.get('status')}")
    return problems


# -- driver ------------------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--appdir", default=os.environ.get("ORQUESTER_APPDIR"))
    ap.add_argument("--account", required=True, help="comma-separated labels or id prefixes")
    ap.add_argument("--surface", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--model", default="sonnet")
    ap.add_argument("-k", action="append", default=[], help="run cases whose id contains this")
    ap.add_argument("--kind", choices=["bad", "guard", "control"])
    ap.add_argument("--samples", type=int, default=0, help="exactly N samples (default 2, a 3rd on a split)")
    ap.add_argument("--tool-search", choices=["auto", "off"], default="auto")
    ap.add_argument("--patch", type=Path)
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--max-turns", type=int, default=14)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("--cases", type=Path, default=HERE / "cases.py", help="the cases module (default cases.py)")
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location("eval_cases", args.cases)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    CASES = module.CASES
    if not args.appdir:
        sys.exit("--appdir (or ORQUESTER_APPDIR) is required: the eval runs only on Orquester-managed accounts")
    repo = HERE.parents[2]
    if args.out.resolve().is_relative_to(repo):
        sys.exit("--out must be outside the repository")
    accounts = load_accounts(Path(args.appdir), [a for a in args.account.split(",") if a])
    cases = [c for c in CASES if (not args.k or any(k in c["id"] for k in args.k))
             and (not args.kind or c["kind"] == args.kind)]
    if not cases:
        sys.exit("no case matches")
    args.out.mkdir(parents=True, exist_ok=True)
    surface = load_surface(args.surface, args.patch)
    surface_file = args.out / "surface.json"
    surface_file.write_text(json.dumps(surface, ensure_ascii=False), encoding="utf-8")
    project = args.surface / "project"

    lock = threading.Lock()
    counter = [0]

    def next_account() -> dict:
        with lock:
            counter[0] += 1
            return accounts[counter[0] % len(accounts)]

    def one(case, n):
        row = run_sample(case, n, next_account(), args, surface_file, project, args.out)
        mark = "PASS" if row["passed"] else "FAIL"
        print(f"{mark} {case['id']} s{n} [{row['account'].split('@')[0]}] {row['seconds']}s "
              f"calls={row['calls'][:8]} {'; '.join(row['fails'])[:200]}"
              + (f" ISOLATION {row['isolation']}" if row["isolation"] else ""), flush=True)
        return row

    rows: list[dict] = []
    first = args.samples or 2
    with futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = [pool.submit(one, c, n) for c in cases for n in range(1, first + 1)]
        rows += [j.result() for j in jobs]
        if not args.samples:
            split = [c for c in cases if len({r["passed"] for r in rows if r["case"] == c["id"]}) > 1]
            rows += [j.result() for j in [pool.submit(one, c, 3) for c in split]]
    write_report(rows, cases, args)
    return 0


def summarize(rows: list[dict], cases: list[dict]) -> dict:
    out = {}
    for c in cases:
        mine = [r["passed"] for r in rows if r["case"] == c["id"]]
        out[c["id"]] = {"kind": c["kind"], "passes": sum(mine), "samples": len(mine), "passed": grade.verdict(mine)}
    return out


def write_report(rows: list[dict], cases: list[dict], args) -> None:
    summary = summarize(rows, cases)
    (args.out / "results.json").write_text(json.dumps({"model": args.model, "tool_search": args.tool_search,
                                                       "patch": str(args.patch or ""), "summary": summary,
                                                       "rows": rows}, indent=1, ensure_ascii=False))
    print()
    for kind in ("bad", "guard", "control"):
        ids = [k for k, v in summary.items() if v["kind"] == kind]
        if ids:
            ok = sum(1 for k in ids if summary[k]["passed"])
            print(f"{kind:8s} {ok}/{len(ids)} cases pass")
    total = sum(1 for v in summary.values() if v["passed"])
    print(f"total    {total}/{len(summary)}  samples {sum(r['passed'] for r in rows)}/{len(rows)}  "
          f"cost ${sum(r['cost_usd'] for r in rows):.2f}")
    for k, v in summary.items():
        print(f"  {'ok ' if v['passed'] else 'BAD'} {k} {v['passes']}/{v['samples']}")
    if args.baseline and args.baseline.exists():
        base = json.loads(args.baseline.read_text())["summary"]
        regress = [k for k, v in summary.items() if k in base and base[k]["passes"] == base[k]["samples"]
                   and not v["passed"]]
        print("regressions vs baseline (passed every baseline sample, fail now):", regress or "none")


if __name__ == "__main__":
    sys.exit(main())
