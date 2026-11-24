"""tools/ci/runner_dispatch.py: the overflow runners' availability signal and watchdog (docs/testing.md
"Overflow runners"). Cron runs it every minute on an always-on host whose `gh` is logged in as a
repository admin:

    python3 -I tools/ci/runner_dispatch.py --state-dir DIR [--hosts FILE] [--log FILE] [--repo R] [--dry-run]

Each run:

- state: the overflow runners (OVERFLOW in plan_lanes.py: online, busy, or claimed by a job queued for
  them; online when GitHub says so or --hosts' ssh probe finds its listener running), their free disk
  (the same probe: under the runner's min_free_gb it is unfit),
  and per OS the jobs on GitHub's runners, running and queued: saturated when a job waits while the OS's
  concurrency limit is used up (HOSTED_LIMITS, TOTAL_LIMIT) or has waited SATURATED_WAIT. Written to the
  data branch STATE_BRANCH (state.json, one parentless commit, force-pushed), which the `changes` job
  fetches as it plans (plan_lanes.route: older than its STATE_MAX_AGE means no overflow runner);
- planner: SELF_HOSTED_PLANNER, the runner ci.yml's `changes` runs on: the Linux overflow runner while
  hosted Linux is saturated and it is online and fit (a run reads `vars` as it is created, as `changes`
  is queued, so this one is current where it is read);
- watchdog: a run with a job the plan routed onto an overflow runner (a je-* label) whose job has waited
  QUEUED_LIMIT for a runner that is offline or gone, or runs on one offline for OFFLINE_LIMIT, or failed
  because its runner went away (LOST, also while the rest of the run goes on), is cancelled and, once
  stopped, rerun whole: a rerun is planned on
  GitHub's runners only. A merge-queue run is not rerun (the queue removed its group) but its pull
  request is put back in the queue. Each run at most once, at most MAX_ACTIONS an hour; runs it did not
  route are never touched; every action goes to --log.

--hosts: JSON {"<label>": {"ssh": "<user@host>", "shell": "posix|powershell", "min_free_gb": N}}, kept on
the host (it names the machines). --dry-run reads and prints, and writes and changes nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plan_lanes import OVERFLOW, STATE_BRANCH  # noqa: E402

REPO = "Arakuma-Studio/JasperEngine"
# GitHub Team: 60 concurrent hosted jobs, 5 of them macOS
HOSTED_LIMITS = {"macos": 5}
TOTAL_LIMIT = 60
SATURATED_WAIT = 300        # a hosted job queued this long (s): its OS's runners are taken, whatever the count
PLANNER_WAIT = 300          # a `changes` job queued this long (s) for a busy overflow runner is rerouted
# A merge-queue run is cancelled only when its job truly cannot finish: a cancel fails its Check, the queue
# removes the pull request and every group behind it restarts (run 45310250691, #153's group, 2026-11-24:
# about 2 h lost for a job that would have waited). So never for an idle runner or a runner that went
# offline mid-job (GitHub fails that job itself, and LOST then puts the pull request back), and for a job
# queued for an offline runner only once that runner has been gone QUEUE_OFFLINE_LIMIT
QUEUE_OFFLINE_LIMIT = 1800
QUEUED_LIMIT = 600          # a job queued for an offline overflow runner this long (s) is rerouted
OFFLINE_LIMIT = 180         # a running job's overflow runner offline this long (s): rerouted
LOOKBACK = 6 * 3600         # finished runs looked at for a job its runner's loss failed
MAX_ACTIONS = 6             # watchdog actions an hour
MIN_FREE_GB = 60
# what GitHub says when a job's runner went away
LOST = re.compile(r"lost communication with the server|received a shutdown signal", re.I)
QUEUE_REF = re.compile(r"^gh-readonly-queue/[^/]+/pr-(\d+)-[0-9a-f]+$")


def epoch(stamp: str | None) -> float | None:
    if not stamp:
        return None
    try:
        return datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def os_of(labels: list[str]) -> tuple[str, str]:
    """(OS, overflow label or "") of a job's runs-on labels; ("", "") for a runner this does not count."""
    for label in labels:
        if label in OVERFLOW:
            return OVERFLOW[label][0], label
    for label in labels:
        for prefix, name in (("ubuntu-", "linux"), ("windows-", "windows"), ("macos-", "macos")):
            if label.startswith(prefix):
                return name, ""
    return "", ""


# ---------------------------------------------------------------------------------------- state

def compute_state(runners: list[dict], jobs: list[dict], probes: dict, now: float) -> dict:
    """The runner state from the repository's runners, the jobs of its queued and running runs, and the
    probes ({label: {"alive", "free_gb", "ok", "why"}}: online when GitHub or the probe says the runner
    is; a label without a probe is fit)."""
    hosted = {o: {"running": 0, "queued": 0, "waited": 0} for o in ("linux", "windows", "macos")}
    claimed: set[str] = set()
    for job in jobs:
        os_, label = os_of(job.get("labels") or [])
        if not os_:
            continue
        status = job.get("status")
        if label:
            if status in ("queued", "waiting", "pending", "requested"):
                claimed.add(label)
            continue
        if status == "in_progress":
            hosted[os_]["running"] += 1
        elif status in ("queued", "waiting", "pending", "requested"):
            hosted[os_]["queued"] += 1
            created = epoch(job.get("created_at"))
            if created is not None:
                hosted[os_]["waited"] = max(hosted[os_]["waited"], int(now - created))
    total = sum(h["running"] for h in hosted.values())
    for os_, h in hosted.items():
        full = h["running"] >= HOSTED_LIMITS.get(os_, TOTAL_LIMIT) or total >= TOTAL_LIMIT
        h["saturated"] = h["queued"] > 0 and (full or h["waited"] >= SATURATED_WAIT)
    out = {}
    for label in OVERFLOW:
        r = next((r for r in runners if label in [x.get("name") for x in r.get("labels", [])]), None)
        probe = probes.get(label) or {}
        entry = {"online": bool(r and (r.get("status") == "online" or probe.get("alive"))),
                 "busy": bool(r and r.get("busy")) or label in claimed, "fit": probe.get("ok", True)}
        if not entry["fit"]:
            entry["why"] = probe.get("why", "")
        if "free_gb" in probe:
            entry["free_gb"] = probe["free_gb"]
        out[label] = entry
    return {"t": int(now), "os": hosted, "runners": out}


PLANNER_VARIABLE = "SELF_HOSTED_PLANNER"
PLANNER = "je-linux"


def planner(state: dict, disabled: str) -> str:
    """The runner `changes` runs on (ci.yml reads PLANNER_VARIABLE as a run is created): the Linux overflow
    runner while hosted Linux is saturated and it is online, fit and idle (no job running or queued for it:
    runs created while it plans go hosted, so planners never queue behind each other or a build), else ""."""
    off = set(disabled.replace(",", " ").split())
    r = state["runners"].get(PLANNER) or {}
    ok = (state["os"]["linux"]["saturated"] and r.get("online") and r.get("fit") and not r.get("busy")
          and not off & {"all", PLANNER})
    return PLANNER if ok else ""


def reserve(state: dict, want: str) -> None:
    """The planner's runner takes no lane while it plans: every run's `changes` would queue behind them."""
    if want:
        state["runners"][want].update(fit=False, why="reserved for `changes` (SELF_HOSTED_PLANNER)")


def probe(label: str, host: dict, run=subprocess.run) -> dict:
    """Over ssh: whether the machine's runner listener runs, and its runner folder's free disk, enough or
    not. GitHub's runner status alone is not enough: it reads offline for runners that take jobs."""
    if host.get("shell") == "powershell":
        command = ("powershell -NoProfile -Command \"if (Get-Process Runner.Listener -ErrorAction SilentlyContinue) "
                   "{'alive'} else {'dead'}; [math]::Floor((Get-PSDrive C).Free / 1GB)\"")
    else:
        command = ("(pgrep -f Runner.Listener > /dev/null && echo alive || echo dead); "
                   "df -Pk ~/je-runner | awk 'NR == 2 {print int($4 / 1048576)}'")
    try:
        r = run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host["ssh"], command],
                capture_output=True, text=True, timeout=30)
        lines = r.stdout.split()
        alive, free = lines[-2] == "alive", int(lines[-1])
    except (subprocess.SubprocessError, OSError, ValueError, IndexError, KeyError):
        return {"alive": False, "ok": False, "why": "not reachable over ssh"}
    need = int(host.get("min_free_gb", MIN_FREE_GB))
    return {"alive": alive, "free_gb": free, "ok": free >= need, **({} if free >= need else {"why": f"{free} GB free, under {need}"})}


# ---------------------------------------------------------------------------------------- watchdog

def stuck(run: dict, jobs: list[dict], runners: dict, offline_since: dict, now: float) -> str:
    """Why a run the plan routed onto an overflow runner waits on one that went away, or ""."""
    for job in jobs:
        _os, label = os_of(job.get("labels") or [])
        if not label:
            continue
        runner = runners.get(label) or {}
        online = runner.get("online", False)
        status = job.get("status")
        queue = run.get("event") == "merge_group"
        if status in ("queued", "waiting", "pending", "requested") and not online:
            waited = now - (epoch(job.get("created_at")) or now)
            gone = now - offline_since.get(label, now)
            if waited >= QUEUED_LIMIT and (not queue or gone >= QUEUE_OFFLINE_LIMIT):
                return f"{job.get('name')} queued {waited / 60:.0f} min for {label}, which is offline"
        # the planner behind a busy runner: every lane of its run waits on it. Not a merge-queue run's (a
        # cancel fails its group): the planner is named only while its runner is idle (planner)
        if (status in ("queued", "waiting", "pending", "requested") and online and runner.get("busy")
                and job.get("name") == "changes" and run.get("event") != "merge_group"):
            waited = now - (epoch(job.get("created_at")) or now)
            if waited >= PLANNER_WAIT:
                return f"changes queued {waited / 60:.0f} min for {label}, which is busy"
        if status == "in_progress" and not online and job.get("runner_name") == label and not queue:
            gone = now - offline_since.get(label, now)
            if gone >= OFFLINE_LIMIT:
                return f"{job.get('name')} running on {label}, offline for {gone / 60:.0f} min"
    return ""


def lost(jobs: list[dict], annotations) -> str:
    """Why a finished run's job on an overflow runner failed because the runner went away, or ""."""
    for job in jobs:
        _os, label = os_of(job.get("labels") or [])
        if label and job.get("conclusion") in ("failure", "cancelled"):
            for message in annotations(job["id"]):
                if LOST.search(message or ""):
                    return f"{job.get('name')} on {label}: {message.strip()[:160]}"
    return ""


class Watchdog:
    """The actions on routed runs: cancel, then rerun (or, a merge-queue run, put its pull request back
    at the front of the queue). Two kinds: "lost" (its overflow runner went away: the rerun is in the
    state's hosted_only, so it plans on GitHub's runners) and "idle" (it waits for a hosted runner while
    an overflow runner of its OS is idle: the rerun plans again). Each kind at most once a run, and no
    "idle" after a "lost": never a loop. memory: the state directory's record (acted runs, action times)."""

    def __init__(self, gh, memory: dict, now: float, log, dry_run: bool = False):
        self.gh, self.memory, self.now, self.log, self.dry_run = gh, memory, now, log, dry_run
        memory.setdefault("acted", {})
        memory["actions"] = [t for t in memory.get("actions", []) if now - t < 3600]
        memory["acted"] = {k: v for k, v in memory["acted"].items() if now - v.get("t", now) < 7 * 86400}

    def kinds(self, key: str) -> list[str]:
        seen = self.memory["acted"].get(key)
        return list(seen.get("kinds", ["lost"])) if seen else []

    def held(self) -> list[str]:
        """The runs moved off a runner that went away in the last day: their reruns plan hosted only."""
        return sorted(k for k, v in self.memory["acted"].items() if "lost" in v.get("kinds", ["lost"])
                      and self.now - v.get("t", 0) < 86400)

    def allowed(self) -> bool:
        return len(self.memory["actions"]) < MAX_ACTIONS

    def act(self, what: str, run: dict, why: str, call) -> bool:
        line = f"{what} run {run['id']} ({run.get('event')}, {run.get('head_branch')}, attempt {run.get('run_attempt')}): {why}"
        if self.dry_run:
            self.log(f"dry run: would {line}")
            return True
        ok = call()
        self.log(f"{'' if ok else 'FAILED to '}{line}")
        self.memory["actions"].append(self.now)
        return ok

    def record(self, key: str, stage: str, why: str, kinds: list[str]) -> None:
        self.memory["acted"][key] = {"t": self.now, "stage": stage, "why": why, "kinds": kinds}

    def handle(self, run: dict, why: str, finished: bool, kind: str = "lost") -> bool:
        """Cancel a run (kind: why), or rerun it once finished. True when it acted."""
        key = str(run["id"])
        seen = self.memory["acted"].get(key)
        kinds = self.kinds(key)
        if seen and seen.get("stage") == "cancelled":
            return False  # stopping: finish() reruns it
        if kind in kinds or (kind == "idle" and "lost" in kinds):
            return False
        if not self.allowed():
            if not self.memory.get("limited") or self.now - self.memory["limited"] > 3600:
                self.log(f"rate limit ({MAX_ACTIONS} actions an hour): run {run['id']} left: {why}")
                self.memory["limited"] = self.now
            return False
        if not finished:
            if self.act("cancel", run, why, lambda: self.gh.cancel(run["id"])):
                self.record(key, "cancelled", why, kinds + [kind])
                return True
            return False
        self.record(key, "finishing", why, kinds + [kind])
        self.finish(run, why)
        return True

    def finish(self, run: dict, why: str) -> None:
        """Rerun a stopped run, or put its pull request back in the queue: one try (a failure is logged
        and left), within the hourly limit."""
        key = str(run["id"])
        kinds = self.kinds(key)
        if not self.allowed():
            return  # (still pending: the next hour's budget takes it)
        if run.get("event") == "merge_group":
            m = QUEUE_REF.match(run.get("head_branch") or "")
            if not m:
                self.log(f"run {run['id']}: a merge-queue run without a pull request in {run.get('head_branch')}: left")
                self.record(key, "left", why, kinds)
                return
            pr = int(m.group(1))
            queued = self.gh.queue_state(pr)
            if queued:
                self.log(f"run {run['id']}: #{pr} is {queued} already: nothing to do ({why})")
                self.record(key, "requeued", why, kinds)
                return
            ok = self.act("re-enqueue the pull request of", run, f"#{pr} back at the front of the merge queue: {why}",
                          lambda: self.gh.enqueue(pr))
            self.record(key, "requeued" if ok else "left", why, kinds)
            return
        # (a pull request's newer push has a run of its own: a rerun of this one would cancel it)
        newest = self.gh.newest_run(run) if run.get("event") == "pull_request" else run["id"]
        if newest != run["id"]:
            self.log(f"run {run['id']}: superseded by run {newest} of {run.get('head_branch')}: not rerun ({why})")
            self.record(key, "left", why, kinds)
        else:
            ok = self.act("rerun", run, why, lambda: self.gh.rerun(run["id"]))
            self.record(key, "rerun" if ok else "left", why, kinds)

    def pending(self) -> list[str]:
        """Runs it cancelled and has not rerun yet."""
        return [k for k, v in self.memory["acted"].items() if v.get("stage") == "cancelled"]


IDLE_REROUTE_WAIT = 900     # a hosted-label job queued this long (s) while an overflow runner of its OS is idle


def trusted_run(run: dict) -> bool:
    """A run an overflow runner may take (as plan_lanes.trusted): never a fork's pull request."""
    if run.get("event") in ("push", "merge_group", "workflow_dispatch"):
        return True
    head = (run.get("head_repository") or {}).get("full_name")
    return run.get("event") == "pull_request" and bool(head) and head == (run.get("repository") or {}).get("full_name")


def job_key(name: str) -> str:
    """plan_lanes' name of a job ("windows (engine-d3d11)": windows-engine-d3d11)."""
    m = re.fullmatch(r"([a-z0-9-]+) \(([^)]+)\)", name or "")
    return f"{m.group(1)}-{m.group(2)}" if m else (name or "")


def waiting_for_idle(run: dict, jobs: list[dict], runners: dict, disabled: str, now: float, taken: set[str]) -> tuple[str, str]:
    """(why, label) when a trusted run's job has waited IDLE_REROUTE_WAIT for a hosted runner while an
    overflow runner that would take it (OVERFLOW) is online, idle and fit; else ("", "")."""
    # (never a merge-queue run: its cancel fails the group and restarts every group behind it)
    if not trusted_run(run) or run.get("event") == "merge_group":
        return "", ""
    off = set(disabled.replace(",", " ").split())
    for label, (os_, order) in OVERFLOW.items():
        r = runners.get(label) or {}
        if label in taken or label in off or "all" in off or not (r.get("online") and not r.get("busy") and r.get("fit", True)):
            continue
        for job in jobs:
            job_os, overflow = os_of(job.get("labels") or [])
            if overflow or job_os != os_ or job.get("status") != "queued" or job_key(job.get("name")) not in order:
                continue
            waited = now - (epoch(job.get("created_at")) or now)
            if waited >= IDLE_REROUTE_WAIT:
                return f"{job.get('name')} waited {waited / 60:.0f} min for a hosted {os_} runner while {label} is idle", label
    return "", ""


# ---------------------------------------------------------------------------------------- GitHub

class Gh:
    def __init__(self, repo: str):
        self.repo = repo
        self.calls = 0

    def api(self, path: str, *args: str):
        self.calls += 1
        r = subprocess.run(["gh", "api", *args, path], capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(f"gh api {path}: {(r.stderr or r.stdout).strip()[:200]}")
        return json.loads(r.stdout) if r.stdout.strip() else None

    def runners(self) -> list[dict]:
        return self.api(f"repos/{self.repo}/actions/runners?per_page=100")["runners"]

    def runs(self, status: str, per_page: int = 100) -> list[dict]:
        return self.api(f"repos/{self.repo}/actions/runs?status={status}&per_page={per_page}")["workflow_runs"]

    def run(self, run_id) -> dict:
        return self.api(f"repos/{self.repo}/actions/runs/{run_id}")

    def jobs(self, run_id) -> list[dict]:
        return self.api(f"repos/{self.repo}/actions/runs/{run_id}/jobs?per_page=100&filter=latest")["jobs"]

    def newest_run(self, run: dict):
        """The id of the newest run of the same workflow, branch and event."""
        runs = self.api(f"repos/{self.repo}/actions/workflows/{run['workflow_id']}/runs?branch={run['head_branch']}"
                        f"&event={run['event']}&per_page=1")["workflow_runs"]
        return runs[0]["id"] if runs else run["id"]

    def annotations(self, job_id) -> list[str]:
        return [a.get("message", "") for a in self.api(f"repos/{self.repo}/check-runs/{job_id}/annotations") or []]

    def _cli(self, *args: str) -> bool:
        self.calls += 1
        return subprocess.run(["gh", *args, "--repo", self.repo], capture_output=True, text=True, timeout=120).returncode == 0

    def set_state(self, body: str) -> bool:
        """state.json as the only commit of STATE_BRANCH (its old commits go unreferenced)."""
        base = f"repos/{self.repo}/git"
        tree = self.post(f"{base}/trees", {"tree": [{"path": "state.json", "mode": "100644", "type": "blob", "content": body}]})
        commit = self.post(f"{base}/commits", {"message": "overflow runner state (tools/ci/runner_dispatch.py)",
                                               "tree": tree["sha"], "parents": []})
        try:
            self.post(f"{base}/refs/heads/{STATE_BRANCH}", {"sha": commit["sha"], "force": True}, "PATCH")
        except RuntimeError:
            self.post(f"{base}/refs", {"ref": f"refs/heads/{STATE_BRANCH}", "sha": commit["sha"]})
        return True

    def variable(self, name: str) -> str:
        self.calls += 1
        r = subprocess.run(["gh", "variable", "get", name, "--repo", self.repo], capture_output=True, text=True, timeout=60)
        return r.stdout.strip() if r.returncode == 0 else ""

    def set_variable(self, name: str, value: str) -> bool:
        if value:
            return self._cli("variable", "set", name, "--body", value)
        return self._cli("variable", "delete", name)

    def post(self, path: str, payload: dict, method: str = "POST"):
        self.calls += 1
        r = subprocess.run(["gh", "api", "-X", method, path, "--input", "-"], input=json.dumps(payload),
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(f"gh api -X {method} {path}: {(r.stderr or r.stdout).strip()[:200]}")
        return json.loads(r.stdout)

    def cancel(self, run_id) -> bool:
        return self._cli("run", "cancel", str(run_id))

    def rerun(self, run_id) -> bool:
        return self._cli("run", "rerun", str(run_id))

    def queue_state(self, pr: int) -> str:
        """Why the pull request needs no enqueue: "in the merge queue" or "closed", else ""."""
        self.calls += 1
        owner, name = self.repo.split("/")
        r = subprocess.run(["gh", "api", "graphql", "-f", "query=query($o:String!,$n:String!,$p:Int!){repository(owner:$o,name:$n)"
                            "{pullRequest(number:$p){state mergeQueueEntry{state}}}}", "-F", f"o={owner}", "-F", f"n={name}",
                            "-F", f"p={pr}", "--jq", ".data.repository.pullRequest | [.state, (.mergeQueueEntry.state // \"\")] | @tsv"],
                           capture_output=True, text=True, timeout=60)
        state, _, entry = r.stdout.strip().partition("\t")
        if r.returncode != 0 or not state:
            return ""
        return "in the merge queue" if entry else ("" if state == "OPEN" else state.lower())

    def enqueue(self, pr: int) -> bool:
        """Back in the merge queue, at its front (it was building there when the watchdog stopped it)."""
        self.calls += 2
        owner, name = self.repo.split("/")
        q = subprocess.run(["gh", "api", "graphql", "-f", "query=query($o:String!,$n:String!,$p:Int!){repository(owner:$o,name:$n)"
                            "{pullRequest(number:$p){id}}}", "-F", f"o={owner}", "-F", f"n={name}", "-F", f"p={pr}",
                            "--jq", ".data.repository.pullRequest.id"], capture_output=True, text=True, timeout=60)
        if q.returncode != 0 or not q.stdout.strip():
            return False
        r = subprocess.run(["gh", "api", "graphql", "-f", "query=mutation($id:ID!){enqueuePullRequest(input:{pullRequestId:$id,"
                            "jump:true}){mergeQueueEntry{position}}}", "-F", f"id={q.stdout.strip()}"],
                           capture_output=True, text=True, timeout=60)
        return r.returncode == 0


def cached_jobs(gh, run: dict, cache: dict) -> list[dict]:
    """A run's jobs, fetched again only when the run changed (its updated_at or attempt)."""
    key = str(run["id"])
    stamp = f"{run.get('updated_at')}#{run.get('run_attempt')}"
    hit = cache.get(key)
    if hit and hit.get("stamp") == stamp:
        return hit["jobs"]
    jobs = [{k: j.get(k) for k in ("id", "name", "status", "conclusion", "labels", "runner_name", "created_at")}
            for j in gh.jobs(run["id"])]
    cache[key] = {"stamp": stamp, "jobs": jobs}
    return jobs


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--state-dir", type=Path, required=True, help="this script's memory between runs")
    ap.add_argument("--hosts", type=Path, help="the machines to probe for free disk (kept off the repository)")
    ap.add_argument("--log", type=Path, help="append every action here (default: stdout)")
    ap.add_argument("--dry-run", action="store_true", help="read and print; set, cancel and rerun nothing")
    args = ap.parse_args(argv)

    now = time.time()
    stamp = datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    def log(line: str) -> None:
        text = f"{stamp} {line}\n"
        if args.log and not args.dry_run:
            with args.log.open("a") as f:
                f.write(text)
        else:
            sys.stdout.write(text)

    args.state_dir.mkdir(parents=True, exist_ok=True)
    memory_path = args.state_dir / "memory.json"
    try:
        memory = json.loads(memory_path.read_text())
    except (OSError, ValueError):
        memory = {}
    gh = Gh(args.repo)
    try:
        runners = gh.runners()
        active = {r["id"]: r for status in ("queued", "in_progress") for r in gh.runs(status)}
        cache = {k: v for k, v in memory.get("jobs", {}).items() if int(k) in active}
        with ThreadPoolExecutor(8) as pool:
            jobs = dict(zip(active, pool.map(lambda run: cached_jobs(gh, run, cache), active.values())))
    except (RuntimeError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as e:
        log(f"no state: {e}")   # the variable ages past STATE_MAX_AGE: the plans route hosted only
        return 1
    hosts = {}
    if args.hosts:
        try:
            hosts = json.loads(args.hosts.read_text())
        except (OSError, ValueError) as e:
            log(f"--hosts unreadable ({e}): disks not probed")
    labels = [label for label in hosts if label in OVERFLOW]
    with ThreadPoolExecutor(4) as pool:
        probes = dict(zip(labels, pool.map(lambda label: probe(label, hosts[label]), labels)))
    state = compute_state(runners, [j for js in jobs.values() for j in js], probes, now)
    dog = Watchdog(gh, memory, now, log, args.dry_run)
    # the reruns of runs moved off a runner that went away plan on GitHub's runners (plan_lanes.route)
    state["hosted_only"] = dog.held()
    disabled = ""
    try:
        disabled = gh.variable("SELF_HOSTED_DISABLED")
        want = planner(state, disabled)
    except (RuntimeError, subprocess.SubprocessError) as e:
        want = memory.get("planner", "")
        log(f"{PLANNER_VARIABLE} not recomputed: {e}")
    reserve(state, want)
    body = json.dumps(state, separators=(",", ":"), sort_keys=True)
    if args.dry_run:
        print(f"{STATE_BRANCH}: {body}")
    else:
        try:
            gh.set_state(body)
        except RuntimeError as e:
            log(f"the state was not published to {STATE_BRANCH}: {e}")

    # the planner's runner: written only when it changes
    try:
        if want != memory.get("planner", "") or not memory.get("planner_set"):
            if args.dry_run:
                print(f"{PLANNER_VARIABLE}={want}")
            elif gh.set_variable(PLANNER_VARIABLE, want) or not want:
                log(f"{PLANNER_VARIABLE}={want or '(none: hosted)'}")
                memory.update(planner=want, planner_set=True)
    except (RuntimeError, subprocess.SubprocessError) as e:
        log(f"{PLANNER_VARIABLE} not updated: {e}")

    # the watchdog
    offline_since = memory.get("offline_since", {})
    for label, entry in state["runners"].items():
        if entry["online"]:
            offline_since.pop(label, None)
        else:
            offline_since.setdefault(label, now)
    # a finished job's annotations do not change: each is read once
    notes = {k: v for k, v in memory.get("annotations", {}).items() if now - v["t"] < LOOKBACK}

    def annotations(job_id) -> list[str]:
        key = str(job_id)
        if key not in notes:
            notes[key] = {"t": now, "messages": gh.annotations(job_id)}
        return notes[key]["messages"]

    for rid, run in active.items():
        # stuck, or a job its runner's loss failed while the rest of the run goes on
        try:
            why = stuck(run, jobs[rid], state["runners"], offline_since, now) or lost(jobs[rid], annotations)
        except RuntimeError as e:
            log(f"run {rid}: {e}")
            continue
        if why:
            dog.handle(run, why, finished=False)
    # a trusted run waiting for a hosted runner while an overflow runner of its OS is idle: rerun, so its
    # plan routes it there (the merge queue's first: it is the gate). One run per idle runner a minute
    taken: set[str] = set()
    for rid, run in sorted(active.items(), key=lambda kv: (kv[1].get("event") != "merge_group", kv[1].get("created_at", ""))):
        why, label = waiting_for_idle(run, jobs[rid], state["runners"], disabled, now, taken)
        if why and dog.handle(run, why, finished=False, kind="idle"):
            taken.add(label)
    for key in dog.pending():
        try:
            run = active.get(int(key)) or gh.run(key)
        except RuntimeError as e:
            log(f"run {key}: {e}")
            continue
        if run.get("status") == "completed":
            dog.finish(run, memory["acted"][key]["why"])
    # a finished run whose overflow runner went away mid-job (GitHub failed the job)
    finished = memory.get("finished", {})
    try:
        fresh = [run for run in gh.runs("completed", per_page=30)
                 if str(run["id"]) not in finished and str(run["id"]) not in memory["acted"]
                 and now - (epoch(run.get("updated_at")) or 0) <= LOOKBACK]
        failed = [run for run in fresh if run.get("conclusion") in ("failure", "cancelled")]
        with ThreadPoolExecutor(8) as pool:
            reasons = list(pool.map(lambda run: lost(cached_jobs(gh, run, {}), annotations), failed))
        for run in fresh:
            finished[str(run["id"])] = now
        for run, why in zip(failed, reasons):
            if why:
                dog.handle(run, why, finished=True)
    except (RuntimeError, ValueError, KeyError) as e:
        log(f"finished runs not read: {e}")
    memory.update(jobs=cache, offline_since=offline_since, annotations=notes,
                  finished={k: t for k, t in finished.items() if now - t < LOOKBACK})
    if not args.dry_run:
        tmp = memory_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(memory))
        os.replace(tmp, memory_path)
    else:
        print(f"api calls: {gh.calls}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
