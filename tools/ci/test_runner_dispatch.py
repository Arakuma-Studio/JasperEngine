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
        # only while idle: a run created while it plans (or builds) plans hosted
        self.assertEqual(rd.planner(rd.compute_state([runner("je-mac"), runner("je-linux", busy=True), runner("je-win")],
                                                     busy, {}, NOW), ""), "")
        self.assertEqual(rd.planner(rd.compute_state(RUNNERS, busy + [job("je-linux", ago=10, name="changes")], {}, NOW), ""), "")
        self.assertEqual(rd.planner(s, "all"), "")
        self.assertEqual(rd.planner(rd.compute_state(RUNNERS, busy[:10], {}, NOW), ""), "")
        self.assertEqual(rd.planner(rd.compute_state([runner("je-linux", "offline")], busy, {}, NOW), ""), "")
        self.assertEqual(rd.planner(rd.compute_state(RUNNERS, busy, {"je-linux": {"alive": True, "ok": False}}, NOW), ""), "")
        # while it plans it takes no lane (plan_lanes.route: unfit)
        rd.reserve(s, "je-linux")
        self.assertEqual((s["runners"]["je-linux"]["fit"], s["runners"]["je-mac"]["fit"]), (False, True))
        rd.reserve(s, "")


class Stuck(unittest.TestCase):
    def test_queued_for_an_offline_runner(self):
        offline = {"je-mac": {"online": False}}
        self.assertIn("queued 10 min for je-mac", rd.stuck({}, [job("je-mac", ago=rd.QUEUED_LIMIT)], offline, {}, NOW))
        self.assertEqual(rd.stuck({}, [job("je-mac", ago=rd.QUEUED_LIMIT - 1)], offline, {}, NOW), "")
        # online: it is waiting its turn on the machine, not lost
        self.assertEqual(rd.stuck({}, [job("je-mac", ago=3600)], {"je-mac": {"online": True}}, {}, NOW), "")
        # gone (unregistered)
        self.assertIn("offline", rd.stuck({}, [job("je-win", ago=3600)], {}, {}, NOW))

    def test_a_planner_behind_a_busy_runner_goes_hosted(self):
        busy = {"je-linux": {"online": True, "busy": True}}
        planner = job("je-linux", ago=rd.PLANNER_WAIT, name="changes")
        self.assertIn("changes queued 5 min for je-linux, which is busy", rd.stuck({"event": "pull_request"}, [planner], busy, {}, NOW))
        self.assertIn("busy", rd.stuck({"event": "push"}, [planner], busy, {}, NOW))
        # not sooner, not a merge-queue run's (a cancel fails its group), not a lane job (it waits its turn)
        self.assertEqual(rd.stuck({"event": "pull_request"}, [job("je-linux", ago=rd.PLANNER_WAIT - 30, name="changes")], busy, {}, NOW), "")
        self.assertEqual(rd.stuck({"event": "merge_group"}, [planner], busy, {}, NOW), "")
        self.assertEqual(rd.stuck({"event": "pull_request"}, [job("je-linux", ago=3600, name="linux")], busy, {}, NOW), "")

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


class Idle(unittest.TestCase):
    RUNNERS = {"je-mac": {"online": True, "busy": False, "fit": True}, "je-win": {"online": True, "busy": False, "fit": True},
               "je-linux": {"online": True, "busy": False, "fit": False}}

    def make_run(self, event="push", head="Arakuma-Studio/JasperEngine"):
        return {"id": 5, "event": event, "repository": {"full_name": "Arakuma-Studio/JasperEngine"},
                "head_repository": {"full_name": head}}

    def test_a_job_waiting_for_hosted_while_its_overflow_runner_is_idle(self):
        jobs = [job("macos-latest", ago=rd.IDLE_REROUTE_WAIT, name="macos")]
        why, label = rd.waiting_for_idle(self.make_run(), jobs, self.RUNNERS, "", NOW, set())
        self.assertEqual(label, "je-mac")
        self.assertIn("macos waited 15 min for a hosted macos runner while je-mac is idle", why)
        jobs = [job("windows-latest", ago=3600, name="windows (engine-d3d11)")]
        self.assertEqual(rd.waiting_for_idle(self.make_run(), jobs, self.RUNNERS, "", NOW, set())[1], "je-win")

    def test_no_reroute_when_it_cannot_take_it_or_should_not(self):
        mac = [job("macos-latest", ago=3600, name="macos")]
        cases = (
            (self.make_run(), [job("macos-latest", ago=rd.IDLE_REROUTE_WAIT - 60, name="macos")], self.RUNNERS, ""),   # not long
            (self.make_run(), [job("windows-latest", ago=3600, name="windows (editor)")], self.RUNNERS, ""),          # not its job
            (self.make_run(), [job("ubuntu-latest", ago=3600, name="linux")], self.RUNNERS, ""),                      # je-linux unfit
            (self.make_run(), mac, dict(self.RUNNERS, **{"je-mac": {"online": True, "busy": True, "fit": True}}), ""),
            (self.make_run(), mac, dict(self.RUNNERS, **{"je-mac": {"online": False, "busy": False, "fit": True}}), ""),
            (self.make_run(), mac, self.RUNNERS, "je-mac"),
            (self.make_run(), mac, self.RUNNERS, "all"),
            (self.make_run(), [job("macos-latest", "in_progress", ago=3600, name="macos")], self.RUNNERS, ""),
            (self.make_run(), [job("je-mac", ago=3600, name="macos")], self.RUNNERS, ""),                           # routed already
            (self.make_run("pull_request", "someone/JasperEngine"), mac, self.RUNNERS, ""),                          # a fork
            (self.make_run("pull_request", ""), mac, self.RUNNERS, ""),
            (self.make_run("schedule"), mac, self.RUNNERS, ""),
        )
        for run, jobs, runners, disabled in cases:
            self.assertEqual(rd.waiting_for_idle(run, jobs, runners, disabled, NOW, set()), ("", ""), (run["event"], jobs[0]["name"], disabled))
        # one run per idle runner a minute
        self.assertEqual(rd.waiting_for_idle(self.make_run(), mac, self.RUNNERS, "", NOW, {"je-mac"}), ("", ""))
        # a same-repository pull request may
        self.assertEqual(rd.waiting_for_idle(self.make_run("pull_request"), mac, self.RUNNERS, "", NOW, set())[1], "je-mac")

    def test_a_merge_queue_run_is_cancelled_only_when_its_job_cannot_finish(self):
        # (run 45310250691, #153's group: an idle reroute cancelled it, failed its Check, removed the pull
        # request and restarted every group behind it, about 2 h)
        mac = [job("macos-latest", ago=3600, name="macos")]
        self.assertEqual(rd.waiting_for_idle(self.make_run("merge_group"), mac, self.RUNNERS, "", NOW, set()), ("", ""))
        queue = {"event": "merge_group"}
        offline = {"je-mac": {"online": False}}
        queued = [job("je-mac", ago=3600, name="macos")]
        # queued for an offline runner: only once it has been gone QUEUE_OFFLINE_LIMIT (it may come back)
        self.assertEqual(rd.stuck(queue, queued, offline, {"je-mac": NOW - rd.QUEUE_OFFLINE_LIMIT + 60}, NOW), "")
        self.assertIn("offline", rd.stuck(queue, queued, offline, {"je-mac": NOW - rd.QUEUE_OFFLINE_LIMIT}, NOW))
        self.assertIn("offline", rd.stuck({"event": "push"}, queued, offline, {"je-mac": NOW - 60}, NOW))
        # running on a runner that went away: GitHub fails the job itself; the lost path then re-enqueues
        running = [job("je-mac", "in_progress", runner="je-mac", name="macos")]
        self.assertEqual(rd.stuck(queue, running, offline, {"je-mac": NOW - 7200}, NOW), "")
        self.assertIn("offline for", rd.stuck({"event": "push"}, running, offline, {"je-mac": NOW - rd.OFFLINE_LIMIT}, NOW))
        notes = {7: ["The self-hosted runner: je-mac lost communication with the server."]}
        self.assertIn("lost communication", rd.lost([job("je-mac", "completed", conclusion="failure", id_=7)], notes.get))


class FakeGh:
    def __init__(self, newest=None, queued=None, enqueue_ok=True):
        self.calls = []
        self.newest = newest
        self.queued = queued or {}
        self.enqueue_ok = enqueue_ok

    def cancel(self, run_id):
        self.calls.append(("cancel", run_id))
        return True

    def rerun(self, run_id):
        self.calls.append(("rerun", run_id))
        return True

    def enqueue(self, pr):
        self.calls.append(("enqueue", pr))
        return self.enqueue_ok

    def queue_state(self, pr):
        return self.queued.get(pr, "")

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
        self.assertIn("#152 back at the front of the merge queue", self.lines[-1])

    def test_an_idle_runner_reroute_then_a_lost_runner_reroute_then_nothing(self):
        # (each kind once; the lost one is held hosted-only, and no idle reroute after it: never a loop)
        gh = FakeGh()
        dog = self.dog(gh)
        self.assertTrue(dog.handle(run_(), "macos waited 20 min while je-mac is idle", finished=False, kind="idle"))
        self.assertFalse(dog.handle(run_(), "again", finished=False, kind="idle"))
        dog.finish(run_(), "macos waited")
        self.assertEqual(dog.held(), [])                     # its rerun plans again
        self.assertFalse(dog.handle(run_(), "idle again", finished=False, kind="idle"))
        self.assertTrue(dog.handle(run_(), "macos queued 10 min for je-mac, which is offline", finished=False))
        dog.finish(run_(), "offline")
        self.assertEqual(dog.held(), ["11"])                 # its rerun stays hosted
        for kind in ("idle", "lost"):
            self.assertFalse(dog.handle(run_(), "more", finished=True, kind=kind))
        self.assertEqual(gh.calls, [("cancel", 11), ("rerun", 11), ("cancel", 11), ("rerun", 11)])
        # an older record without kinds counts as a lost one
        self.memory["acted"]["12"] = {"t": NOW, "stage": "rerun", "why": "old"}
        self.assertIn("12", dog.held())
        self.assertFalse(dog.handle(run_(12), "idle", finished=False, kind="idle"))

    def test_a_pull_request_already_queued_or_a_failed_enqueue_is_tried_once(self):
        # (run 45269664917: #160 was back in the queue; the enqueue failed every minute, 121 times)
        r = run_(13, "merge_group", "gh-readonly-queue/main/pr-160-7005c661391004d44c6dfe3d97110ec9726f5b12")
        gh = FakeGh(queued={160: "in the merge queue"})
        dog = self.dog(gh)
        dog.handle(r, "why", finished=True)
        self.assertEqual(gh.calls, [])
        self.assertIn("#160 is in the merge queue already", self.lines[-1])
        self.assertEqual(dog.pending(), [])
        gh = FakeGh(enqueue_ok=False)
        dog = self.dog(gh)
        dog.handle(run_(14, "merge_group", "gh-readonly-queue/main/pr-161-dc432e199312b393e6170477ae0d2e849d9aa50e"), "why", finished=False)
        for _ in range(3):
            for key in dog.pending():
                dog.finish(run_(int(key), "merge_group", "gh-readonly-queue/main/pr-161-dc432e199312b393e6170477ae0d2e849d9aa50e"), "why")
        self.assertEqual(gh.calls, [("cancel", 14), ("enqueue", 161)])
        self.assertTrue(any("FAILED to re-enqueue" in line for line in self.lines))

    def test_finishing_respects_the_hourly_limit(self):
        gh = FakeGh()
        dog = self.dog(gh)
        for i in range(rd.MAX_ACTIONS):
            dog.handle(run_(300 + i), "why", finished=False)
        self.assertEqual(len(dog.pending()), rd.MAX_ACTIONS)
        for key in dog.pending():
            dog.finish(run_(int(key)), "why")
        self.assertEqual(len(gh.calls), rd.MAX_ACTIONS)          # the cancels: no rerun past the limit
        self.assertEqual(len(dog.pending()), rd.MAX_ACTIONS)      # still pending, for the next hour
        later = self.dog(gh, NOW + 3601)
        for key in later.pending():
            later.finish(run_(int(key)), "why")
        self.assertEqual([c[0] for c in gh.calls].count("rerun"), rd.MAX_ACTIONS)

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
