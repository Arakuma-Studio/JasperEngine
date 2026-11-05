# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Unit tests of the eval's grader, stub server and sandbox hook (CI runs these; never the eval).

    python3 tools/mcp/eval/test_grade.py
"""

from __future__ import annotations

import ast
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import grade  # noqa: E402
import sim_hook  # noqa: E402
import stub_server  # noqa: E402
from cases import CASES, KINDS  # noqa: E402


def stream(*blocks, result="done", subtype="success"):
    lines = [json.dumps({"type": "system", "subtype": "init", "mcp_servers": [{"name": "jasper"}]})]
    for b in blocks:
        lines.append(json.dumps({"type": "assistant", "message": {"content": [b]}}))
    lines.append(json.dumps({"type": "result", "subtype": subtype, "result": result, "num_turns": 3}))
    return lines


def mcp(name, **args):
    return {"type": "tool_use", "name": "mcp__jasper__" + name, "input": args}


class GradeTest(unittest.TestCase):
    def setUp(self):
        self.trace = grade.parse_stream(stream(
            mcp("editor_status"), mcp("play", action="start", fixed_fps=60),
            {"type": "tool_use", "name": "Bash", "input": {"command": "grep -r speed scripts"}},
            {"type": "tool_use", "name": "Edit", "input": {"file_path": "/p/scenes/Main.scene"}},
            mcp("read_console", since=4), result="Playing; the Console is clean."))

    def test_no_case_was_merged_into_another(self):
        # a merge that drops the "}, {" between two cases still parses: the later keys silently win
        tree = ast.parse((Path(__file__).parent / "cases.py").read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Dict):
                keys = [key.value for key in node.keys if isinstance(key, ast.Constant)]
                self.assertEqual(len(keys), len(set(keys)), f"duplicate keys in the dict at line {node.lineno}")

    def test_parse(self):
        self.assertEqual([c["tool"] for c in self.trace.calls], ["editor_status", "play", "read_console"])
        self.assertEqual(self.trace.shell, ["grep -r speed scripts"])
        self.assertEqual(self.trace.reply, "Playing; the Console is clean.")

    def test_calls_and_partial_args(self):
        self.assertEqual(grade.check({"calls": [{"tool": "play", "args": {"action": "start"}}]}, self.trace), [])
        self.assertTrue(grade.check({"calls": [{"tool": "play", "args": {"action": "stop"}}]}, self.trace))
        self.assertEqual(grade.check({"calls": [{"tool": "play", "args": {"action": "re:^st"}}]}, self.trace), [])

    def test_must_not_order_max_last(self):
        self.assertTrue(grade.check({"must_not_call": ["play"]}, self.trace))
        self.assertEqual(grade.check({"order": [["editor_status", "play"]]}, self.trace), [])
        self.assertTrue(grade.check({"order": [["read_console", "play"]]}, self.trace))
        self.assertTrue(grade.check({"order": [["save_scene", "play"]]}, self.trace), "a missing call fails order")
        self.assertTrue(grade.check({"max_calls": {"*": 2}}, self.trace))
        self.assertEqual(grade.check({"max_calls": {"play": 1}}, self.trace), [])
        self.assertTrue(grade.check({"min_calls": {"play": 2}}, self.trace))
        self.assertEqual(grade.check({"last_call": {"tool": "read_console"}}, self.trace), [])
        self.assertTrue(grade.check({"last_call": {"tool": "play"}}, self.trace))

    def test_shell_reply_edits(self):
        self.assertEqual(grade.check({"shell_must": [r"grep"]}, self.trace), [])
        self.assertTrue(grade.check({"shell_must_not": [r"grep -r"]}, self.trace))
        self.assertEqual(grade.check({"reply_must": [r"console is clean"]}, self.trace), [])
        self.assertTrue(grade.check({"reply_must_not": [r"clean"]}, self.trace))
        self.assertTrue(grade.check({"edits_must_not": [r"\.scene$"]}, self.trace))

    def test_errors_and_files(self):
        lines = [json.dumps({"type": "assistant", "message": {"content": [
                     {"type": "tool_use", "id": "t1", "name": "mcp__jasper__set_properties", "input": {"entity": 1}}]}}),
                 json.dumps({"type": "user", "message": {"content": [
                     {"type": "tool_result", "tool_use_id": "t1", "is_error": True, "content": "Unknown property"}]}}),
                 json.dumps({"type": "result", "subtype": "success", "result": "ok"})]
        trace = grade.parse_stream(lines)
        self.assertTrue(grade.check({"max_errors": {"*": 0}}, trace))
        self.assertEqual(grade.check({"max_errors": {"play": 0}}, trace), [])
        self.assertEqual(grade.check({"calls": [{"tool": "set_properties", "args": {"entity": "*"}}]}, trace), [])
        trace.files = {"scripts/a.lua": "RegisterEngineEvent(self, 'onUpdate')"}
        self.assertEqual(grade.check({"file_must": {"scripts/a.lua": ["RegisterEngineEvent"]}}, trace), [])
        self.assertTrue(grade.check({"file_must": {"scripts/b.lua": ["x"]}}, trace))
        self.assertTrue(grade.check({"file_must_not": {"scripts/a.lua": ["onUpdate"]}}, trace))

    def test_any_of(self):
        ok = {"any_of": [{"calls": ["undo"]}, {"calls": ["play"]}]}
        bad = {"any_of": [{"calls": ["undo"]}, {"calls": ["redo"]}]}
        self.assertEqual(grade.check(ok, self.trace), [])
        self.assertTrue(grade.check(bad, self.trace))

    def test_errored_run_never_passes(self):
        trace = grade.parse_stream(stream(mcp("play", action="start"), subtype="error_during_execution"))
        passed, fails = grade.grade({"checks": {"calls": ["play"]}}, trace)
        self.assertFalse(passed)
        self.assertIn("error_during_execution", fails[0])
        capped = grade.parse_stream(stream(mcp("play", action="start"), subtype="error_max_turns"))
        self.assertTrue(grade.grade({"checks": {"calls": ["play"]}}, capped)[0], "the turn cap alone does not fail")

    def test_verdict(self):
        self.assertTrue(grade.verdict([True, True]))
        self.assertFalse(grade.verdict([True, False]))
        self.assertTrue(grade.verdict([True, False, True]))
        self.assertFalse(grade.verdict([False, False, True]))


SURFACE = {"instructions": "guide", "default_groups": ["core"], "tools": [
    {"name": "play", "inputSchema": {"type": "object", "properties": {"action": {"type": "string"}},
                                     "required": ["action"]}},
    {"name": "tool_groups", "inputSchema": {"type": "object", "properties": {"enable": {}, "disable": {}}}},
    {"name": "get_frame_times", "_meta": {"jasper/group": "profiling"},
     "inputSchema": {"type": "object", "properties": {}}}]}


class StubTest(unittest.TestCase):
    def stub(self, mocks=()):
        return stub_server.Stub(SURFACE, {"mocks": list(mocks)}, None)

    def test_unknown_argument_is_the_registry_message(self):
        res = self.stub().call("play", {"action": "start", "speed": 2})
        self.assertTrue(res["isError"])
        self.assertEqual(res["content"][0]["text"], 'Unknown argument "speed" for play. Arguments: action.')

    def test_required(self):
        self.assertEqual(self.stub().call("play", {})["content"][0]["text"], "action is required.")

    def test_mock_partial_match_and_once(self):
        s = self.stub([{"tool": "play", "args": {"action": "start"}, "once": True, "result": {"text": "started"}},
                       {"tool": "play", "result": {"text": "other", "structured": {"state": "playing"}, "real": True}}])
        self.assertEqual(s.call("play", {"action": "start"})["content"][0]["text"], "started")
        second = s.call("play", {"action": "start"})
        self.assertEqual(second["structuredContent"], {"state": "playing"}, "a recorded result keeps its structuredContent")
        hand = self.stub([{"tool": "play", "result": {"text": "t", "structured": {"x": 1}}}]).call("play", {"action": "x"})
        self.assertNotIn("structuredContent", hand, "a hand-written result sends its text")

    def test_mock_after_error(self):
        s = self.stub([{"tool": "play", "args": {"action": "bad"}, "result": {"text": "no", "is_error": True}},
                       {"tool": "play", "after_error": "play", "result": {"text": "after the refusal"}},
                       {"tool": "play", "result": {"text": "before it"}}])
        self.assertEqual(s.call("play", {"action": "start"})["content"][0]["text"], "before it")
        self.assertTrue(s.call("play", {"action": "bad"})["isError"])
        self.assertEqual(s.call("play", {"action": "start"})["content"][0]["text"], "after the refusal")

    def test_mock_regex_and_after(self):
        s = self.stub([{"tool": "play", "args": {"action": "re:^sta"}, "after": "tool_groups", "result": {"text": "late"}},
                       {"tool": "play", "args": {"action": "*"}, "result": {"text": "early"}}])
        self.assertEqual(s.call("play", {"action": "start"})["content"][0]["text"], "early")
        s.call("tool_groups", {})
        self.assertEqual(s.call("play", {"action": "start"})["content"][0]["text"], "late")

    def test_unmocked_is_logged(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "calls.jsonl"
            res = stub_server.Stub(SURFACE, {}, log).call("play", {"action": "stop"})
            self.assertTrue(res["isError"])
            self.assertTrue(json.loads(log.read_text())["unmocked"])

    def test_groups_list_and_notify(self):
        s = self.stub()
        self.assertEqual([t["name"] for t in s.listed()], ["play", "tool_groups"])
        out = io.StringIO()
        req = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                          "params": {"name": "tool_groups", "arguments": {"enable": ["profiling"]}}})
        stub_server.serve(s, io.StringIO(req + "\n"), out)
        msgs = [json.loads(l) for l in out.getvalue().splitlines()]
        self.assertEqual(msgs[1]["method"], "notifications/tools/list_changed")
        self.assertIn("get_frame_times", [t["name"] for t in s.listed()])


class HookTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "project"
        self.root.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def decide(self, tool, case=None, **args):
        return sim_hook.decide(self.root, case or {}, {"tool_name": tool, "tool_input": args})

    def test_read_only_runs_inside(self):
        self.assertIsNone(self.decide("Bash", command="grep -rn speed scripts | head"))
        self.assertIsNone(self.decide("Bash", command="git log --oneline"))

    def test_writes_and_outside_paths_refused(self):
        for cmd in ("rm -rf scripts", "sed -i s/a/b/ x.lua", "cat /etc/hostname", "ls; rm x", "echo hi > f",
                    "find . -delete", "jasperengine-editor call play"):
            out = self.decide("Bash", command=cmd)
            self.assertEqual(out["hookSpecificOutput"]["permissionDecision"], "deny", cmd)
        self.assertEqual(self.decide("Read", file_path="/etc/passwd")["hookSpecificOutput"]["permissionDecision"], "deny")
        self.assertIsNone(self.decide("Read", file_path=str(self.root / "project.yaml")))

    def test_canned_shell(self):
        out = self.decide("Bash", {"shell": [{"match": r"^jasperengine-editor", "output": "ok\n"}]},
                          command="jasperengine-editor check")
        new = out["hookSpecificOutput"]["updatedInput"]["command"]
        self.assertTrue(new.startswith("cat "))
        self.assertEqual(Path(new[4:].strip("'")).read_text(), "ok\n")


class CasesTest(unittest.TestCase):
    def test_cases_are_well_formed(self):
        ids = [c["id"] for c in CASES]
        self.assertEqual(len(ids), len(set(ids)))
        for c in CASES:
            self.assertIn(c["kind"], KINDS, c["id"])
            for key in ("prompt", "what_happened", "text_at_issue", "source", "checks"):
                self.assertIn(key, c, f"{c['id']} lacks {key}")
            known = {"calls", "must_not_call", "order", "max_calls", "last_call", "shell_must", "shell_must_not",
                     "reply_must", "reply_must_not", "edits_must_not", "any_of", "max_errors", "file_must", "min_calls",
                     "file_must_not"}
            self.assertLessEqual(set(c["checks"]), known, c["id"])
        self.assertGreaterEqual(sum(1 for c in CASES if c["kind"] == "control"), 5)


if __name__ == "__main__":
    unittest.main()
