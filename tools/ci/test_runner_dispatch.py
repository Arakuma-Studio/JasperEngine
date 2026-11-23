"""Tests of tools/ci/runner_dispatch.py (python3 -I tools/ci/test_runner_dispatch.py; stdlib only).

The state it writes (hosted saturation, the overflow runners' availability), what the watchdog calls
stuck, and its actions on a fake GitHub: cancel then rerun, the merge queue's re-enqueue, one action per
run, the rate limit, runs it did not route left alone, a dry run that changes nothing.
"""

from __future__ import annotations

import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runner_dispatch as rd  # noqa: E402

NOW = 1_800_000_000.0


def at(seconds_ago: float) -> str:
    return datetime.fromtimestamp(NOW - seconds_ago, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def job(label, status="queued", ago=0, runner=None, conclusion=None, name="j", id_=1):
    return {"id": id_, "name": name, "status": status, "conclusion": conclusion, "labels": [label],
            "runner_name": runner, "created_at": at(ago)}


def runner(name, status="online", busy=False):
    return {"name": name, "status": status, "busy": busy,
            "labels": [{"name": "self-hosted"}, {"name": "macOS"}, {"name": name}]}


RUNNERS = [runner("je-mac"), runner("je-linux"), runner("je-win")]


class State(unittest.TestCase):
    def test_macos_is_saturated_when_its_five_run_and_one_more_waits(self):
        running = [job("macos-latest", "in_progress", id_=i) for i in range(5)]
        s = rd.compute_state(RUNNERS, running, {}, NOW)
        self.assertFalse(s["os"]["macos"]["saturated"])           # all five busy, none waiting
        s = rd.compute_state(RUNNERS, running + [job("macos-latest", ago=20)], {}, NOW)
        self.assertEqual({k: s["os"]["macos"][k] for k in ("running", "queued", "saturated")},
                         {"running": 5, "queued": 1, "saturated": True})
        s = rd.compute_state(RUNNERS, running[:4] + [job("macos-latest", ago=20)], {}, NOW)
        self.assertFalse(s["os"]["macos"]["saturated"])           # a free one takes it
        self.assertEqual(s["t"], int(NOW))

    def test_linux_and_windows_by_the_total_or_a_long_wait(self):
        busy = [job("ubuntu-latest", "in_progress", id_=i) for i in range(50)] + \
               [job("windows-latest", "in_progress", id_=i) for i in range(10)]
        s = rd.compute_state(RUNNERS, busy + [job("windows-latest", ago=5), job("ubuntu-slim", ago=5)], {}, NOW)
        self.assertTrue(s["os"]["windows"]["saturated"] and s["os"]["linux"]["saturated"])
        s = rd.compute_state(RUNNERS, busy[:10] + [job("ubuntu-latest", ago=30)], {}, NOW)
        self.assertFalse(s["os"]["linux"]["saturated"])
        s = rd.compute_state(RUNNERS, busy[:10] + [job("ubuntu-latest", ago=rd.SATURATED_WAIT)], {}, NOW)
        self.assertTrue(s["os"]["linux"]["saturated"])

    def test_the_overflow_runners(self):
        jobs = [job("je-linux", "queued", ago=5), job("je-mac", "in_progress", runner="je-mac"),
                job("self-hosted", "in_progress")]                 # another runner's: not counted
        s = rd.compute_state([runner("je-mac", busy=True), runner("je-linux"), runner("je-win", "offline")], jobs,
                             {"je-win": {"alive": False, "ok": False, "why": "12 GB free, under 60", "free_gb": 12}}, NOW)
        self.assertEqual(s["runners"]["je-mac"], {"online": True, "busy": True, "fit": True})
        # a job queued for it claims it: the next plan must not count it idle
        self.assertEqual(s["runners"]["je-linux"], {"online": True, "busy": True, "fit": True})
        self.assertEqual(s["runners"]["je-win"], {"online": False, "busy": False, "fit": False, "why": "12 GB free, under 60",
                                                  "free_gb": 12})
        # an overflow runner's job is not a hosted one
        self.assertEqual((s["os"]["macos"]["running"], s["os"]["linux"]["queued"]), (0, 0))
        # an unregistered runner is offline
        self.assertFalse(rd.compute_state([], [], {}, NOW)["runners"]["je-mac"]["online"])

    def test_the_disk_probe(self):
        class R:
            def __init__(self, out):
                self.stdout = out

        self.assertEqual(rd.probe("je-linux", {"ssh": "u@h", "min_free_gb": 100}, lambda *a, **k: R("alive\n14\n")),
                         {"alive": True, "free_gb": 14, "ok": False, "why": "14 GB free, under 100"})
        self.assertEqual(rd.probe("je-mac", {"ssh": "u@h"}, lambda *a, **k: R("dead\n458\n")),
                         {"alive": False, "free_gb": 458, "ok": True})
        self.assertEqual(rd.probe("je-mac", {"ssh": "u@h"}, lambda *a, **k: R("")),
                         {"alive": False, "ok": False, "why": "not reachable over ssh"})

    def test_online_when_github_or_the_probe_says_so(self):
        # GitHub reads offline for a listener that takes jobs; a machine asleep fails both
        for status, alive, online in (("online", False, True), ("offline", True, True), ("offline", False, False)):
            s = rd.compute_state([runner("je-mac", status)], [], {"je-mac": {"alive": alive, "ok": True}}, NOW)
            self.assertEqual(s["runners"]["je-mac"]["online"], online, (status, alive))
        # (a registered runner only: a probe alone does not make one)
        self.assertFalse(rd.compute_state([], [], {"je-mac": {"alive": True}}, NOW)["runners"]["je-mac"]["online"])


class Planner(unittest.TestCase):
    def test_the_planner_overflows_only_while_hosted_linux_is_saturated(self):
        busy = [job("ubuntu-latest", "in_progress", id_=i) for i in range(60)] + [job("ubuntu-latest", ago=30)]
        s = rd.compute_state(RUNNERS, busy, {}, NOW)
        self.assertEqual(rd.planner(s, ""), "je-linux")
        self.assertEqual(rd.planner(s, "je-linux"), "")
        self.assertEqual(rd.planner(s, "all"), "")
        self.assertEqual(rd.planner(rd.compute_state(RUNNERS, busy[:10], {}, NOW), ""), "")
        self.assertEqual(rd.planner(rd.compute_state([runner("je-linux", "offline")], busy, {}, NOW), ""), "")
        self.assertEqual(rd.planner(rd.compute_state(RUNNERS, busy, {"je-linux": {"alive": True, "ok": False}}, NOW), ""), "")


class Stuck(unittest.TestCase):
    def test_queued_for_an_offline_runner(self):
        offline = {"je-mac": {"online": False}}
        self.assertIn("queued 10 min for je-mac", rd.stuck({}, [job("je-mac", ago=rd.QUEUED_LIMIT)], offline, {}, NOW))
        self.assertEqual(rd.stuck({}, [job("je-mac", ago=rd.QUEUED_LIMIT - 1)], offline, {}, NOW), "")
        # online: it is waiting its turn on the machine, not lost
        self.assertEqual(rd.stuck({}, [job("je-mac", ago=3600)], {"je-mac": {"online": True}}, {}, NOW), "")
        # gone (unregistered)
        self.assertIn("offline", rd.stuck({}, [job("je-win", ago=3600)], {}, {}, NOW))

    def test_running_on_a_runner_that_went_away(self):
        j = job("je-mac", "in_progress", runner="je-mac")
        offline = {"je-mac": {"online": False}}
        self.assertEqual(rd.stuck({}, [j], offline, {"je-mac": NOW - 60}, NOW), "")
        self.assertIn("offline for 3 min", rd.stuck({}, [j], offline, {"je-mac": NOW - rd.OFFLINE_LIMIT}, NOW))

    def test_runs_it_did_not_route_are_never_stuck(self):
        jobs = [job("macos-latest", ago=99999), job("self-hosted", ago=99999), job("ubuntu-latest", "in_progress")]
        self.assertEqual(rd.stuck({}, jobs, {}, {}, NOW), "")

    def test_a_job_its_runner_lost(self):
        notes = {7: ["The self-hosted runner: je-mac lost communication with the server."], 8: ["Process completed with exit code 1."]}
        self.assertIn("lost communication", rd.lost([job("je-mac", "completed", conclusion="failure", id_=7)], notes.get))
        self.assertEqual(rd.lost([job("je-mac", "completed", conclusion="failure", id_=8)], notes.get), "")
        self.assertEqual(rd.lost([job("macos-latest", "completed", conclusion="failure", id_=7)], notes.get), "")


class FakeGh:
    def __init__(self, newest=None):
        self.calls = []
        self.newest = newest

    def cancel(self, run_id):
        self.calls.append(("cancel", run_id))
        return True

    def rerun(self, run_id):
        self.calls.append(("rerun", run_id))
        return True

    def enqueue(self, pr):
        self.calls.append(("enqueue", pr))
        return True

    def newest_run(self, run):
        return self.newest or run["id"]


def run_(id_=11, event="pull_request", branch="feature/x"):
    return {"id": id_, "event": event, "head_branch": branch, "run_attempt": 1, "workflow_id": 5}


class Watchdog(unittest.TestCase):
    def setUp(self):
        self.lines = []
        self.memory = {}

    def dog(self, gh, now=NOW, dry_run=False):
        return rd.Watchdog(gh, self.memory, now, self.lines.append, dry_run)

    def test_cancel_then_rerun_once(self):
        gh = FakeGh()
        dog = self.dog(gh)
        dog.handle(run_(), "macos queued 10 min for je-mac, which is offline", finished=False)
        dog.handle(run_(), "again", finished=False)               # still stopping: nothing more
        self.assertEqual(gh.calls, [("cancel", 11)])
        self.assertEqual(dog.pending(), ["11"])
        dog.finish(run_(), "macos queued 10 min for je-mac")
        self.assertEqual(gh.calls, [("cancel", 11), ("rerun", 11)])
        self.assertEqual(dog.pending(), [])
        # never again for this run (its rerun is planned hosted only: plan_lanes.route)
        later = self.dog(gh, NOW + 600)
        later.handle(run_(), "stuck again", finished=False)
        later.handle(run_(), "lost again", finished=True)
        self.assertEqual(len(gh.calls), 2)
        self.assertTrue(all("run 11 (pull_request, feature/x" in line for line in self.lines), self.lines)

    def test_a_merge_queue_run_puts_its_pull_request_back(self):
        gh = FakeGh()
        dog = self.dog(gh)
        r = run_(12, "merge_group", "gh-readonly-queue/main/pr-152-6ab5c9ed522250422380bfa5678df0b3462272f5")
        dog.handle(r, "why", finished=False)
        dog.finish(r, "why")
        self.assertEqual(gh.calls, [("cancel", 12), ("enqueue", 152)])
        self.assertIn("#152 back in the merge queue", self.lines[-1])

    def test_a_superseded_pull_request_run_is_not_rerun(self):
        # a rerun would cancel the newer push's run (cancel-in-progress)
        gh = FakeGh(newest=99)
        self.dog(gh).handle(run_(), "lost", finished=True)
        self.assertEqual(gh.calls, [])
        self.assertIn("superseded by run 99", self.lines[-1])

    def test_the_rate_limit(self):
        gh = FakeGh()
        dog = self.dog(gh)
        for i in range(rd.MAX_ACTIONS + 3):
            dog.handle(run_(100 + i), "lost", finished=True)
        self.assertEqual(len(gh.calls), rd.MAX_ACTIONS)
        self.assertEqual(sum("rate limit" in line for line in self.lines), 1)
        # an hour on, the budget is back
        self.dog(gh, NOW + 3601).handle(run_(200), "lost", finished=True)
        self.assertEqual(gh.calls[-1], ("rerun", 200))

    def test_a_dry_run_changes_nothing(self):
        gh = FakeGh()
        dog = self.dog(gh, dry_run=True)
        dog.handle(run_(), "why", finished=False)
        dog.finish(run_(), "why")
        self.assertEqual(gh.calls, [])
        self.assertTrue(self.lines and all(line.startswith("dry run: would ") for line in self.lines), self.lines)


if __name__ == "__main__":
    unittest.main(verbosity=2)
