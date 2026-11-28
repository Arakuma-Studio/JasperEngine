"""Self-test of tools/mcp/call_profile.py on an editor call log (stdlib only: python3 -I tools/mcp/test_call_profile.py).

On a fixture log in the editor's line format (editor/mcp/McpCallLog.h) with a rotated <file>.1 and a
line that is not a call: the per-tool calls, error rate, nearest-rank p50 and p95 ms and mean result
bytes, the order (most calls first, then by name), the printed table, and the exit codes of an empty
or missing log. The tests' directory mode is unchanged and not covered here.
"""

from __future__ import annotations

import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import call_profile  # noqa: E402


def line(tool, ms, ok=True, kind=None, size=100, group="core", client="claude-code"):
    error_kind = "null" if kind is None else f'"{kind}"'
    return (f'{{"ts":"2026-11-21T10:00:00.000Z","tool":"{tool}","group":"{group}","ms":{ms},'
            f'"ok":{"true" if ok else "false"},"error_kind":{error_kind},"result_bytes":{size},"client":"{client}"}}\n')


# the rotated part: older calls
ROTATED = (line("editor_status", 2.0, size=300) + line("screenshot", 120.0, size=90000) +
           line("set_properties", 8.0, ok=False, kind="error", size=210))
# the current file: a torn line from a crash and a blank line are skipped, never a failure
CURRENT = (line("editor_status", 1.0, size=300) + line("editor_status", 4.0, size=300) +
           line("editor_status", 3.0, size=320) + line("set_properties", 5.0, size=180) +
           line("set_properties", 12.5, ok=False, kind="busy", size=150) + line("screenshot", 80.0, size=110000) +
           line("set_properties", 6.0, size=170) + '{"ts":"2026-11-21T10:00:01.000Z","tool":"sce' + "\n\n" +
           line("create_entity", 9.0, group="core", size=400))


class CallLogSummary(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="call-profile-"))
        self.log = self.dir / "mcp-calls.jsonl"
        self.log.write_text(CURRENT)
        (self.dir / "mcp-calls.jsonl.1").write_text(ROTATED)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_load_reads_the_rotated_part_first_and_skips_what_is_not_a_call(self):
        rows, bad = call_profile.load_call_log(self.log)
        self.assertEqual(len(rows), 11)
        self.assertEqual(bad, 1)
        self.assertEqual([r["tool"] for r in rows[:3]], ["editor_status", "screenshot", "set_properties"])

    def test_summary_per_tool(self):
        rows, _ = call_profile.load_call_log(self.log)
        summary = call_profile.summarize_calls(rows)
        self.assertEqual([s["tool"] for s in summary], ["editor_status", "set_properties", "screenshot", "create_entity"])
        status, props, shot, create = summary
        self.assertEqual((status["calls"], status["errors"], status["error_rate"]), (4, 0, 0.0))
        # ms 1, 2, 3, 4: nearest rank p50 is the 2nd value, p95 the 4th
        self.assertEqual((status["p50_ms"], status["p95_ms"]), (2.0, 4.0))
        self.assertEqual(status["mean_result_bytes"], 305.0)
        # ms 5, 6, 8, 12.5 with two errors (an error kind of each sort counts)
        self.assertEqual((props["calls"], props["errors"], props["error_rate"]), (4, 2, 0.5))
        self.assertEqual((props["p50_ms"], props["p95_ms"]), (6.0, 12.5))
        self.assertEqual(props["mean_result_bytes"], 177.5)
        self.assertEqual((shot["calls"], shot["p50_ms"], shot["p95_ms"], shot["mean_result_bytes"]), (2, 80.0, 120.0, 100000.0))
        self.assertEqual((create["calls"], create["p50_ms"], create["p95_ms"]), (1, 9.0, 9.0))

    def test_printed_table(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(call_profile.main([str(self.log)]), 0)
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0], "11 calls of 4 tools, 18.2% errors; 1 line(s) skipped")
        self.assertEqual(lines[1].split(), ["calls", "errors", "p50", "ms", "p95", "ms", "mean", "bytes", "tool"])
        self.assertEqual(lines[2].split(), ["4", "0.0%", "2.0", "4.0", "305", "editor_status"])
        self.assertEqual(lines[3].split(), ["4", "50.0%", "6.0", "12.5", "178", "set_properties"])
        self.assertEqual(lines[4].split(), ["2", "0.0%", "80.0", "120.0", "100000", "screenshot"])
        self.assertEqual(lines[5].split(), ["1", "0.0%", "9.0", "9.0", "400", "create_entity"])
        self.assertEqual(len(lines), 6)

    def test_empty_and_missing_logs_fail(self):
        empty = self.dir / "empty.jsonl"
        empty.write_text("")
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(call_profile.main([str(empty)]), 1)
            self.assertEqual(call_profile.main([str(self.dir / "absent.jsonl")]), 2)
        self.assertIn("no calls", err.getvalue())
        self.assertIn("no such file", err.getvalue())


if __name__ == "__main__":
    unittest.main()
