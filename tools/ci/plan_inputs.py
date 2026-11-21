"""tools/ci/plan_inputs.py: CI's plan inputs on the data branch `ci-plan-inputs`, where no cache
eviction reaches them (docs/testing.md "CI", "Plan inputs").

    python3 tools/ci/plan_inputs.py fetch --out build/ci-inputs [--repo .] [--remote origin]
    python3 tools/ci/plan_inputs.py publish --commit <sha> --run <id> [--set NAME=FILE ...]
                                            [--append shadow=FILE] [--keep 500] [--repo .] [--remote origin]
    python3 tools/ci/plan_inputs.py compact-deps <in.deps> <out.deps>

The branch holds one commit, replaced on every publish (no history to grow):

    manifest.json               NAME -> {commit, run, created}: the main commit each input was made at
    coverage-map.json.gz        tools/mcp/coverage_map.py's map (the nightly)
    deps/<tree>.deps.gz         tools/ci/dump_deps.sh's dumps, compacted (push to main)
    shadow.jsonl                tools/ci/shadow.py's records, the newest --keep lines (the nightly)

fetch writes <out>/coverage-map.json, <out>/deps/<tree>.deps, <out>/shadow.jsonl and
<out>/manifest.json for what the branch has; no branch, or a missing input, writes nothing for it
and exits 0 (the plan widens, as with no input at all). publish needs push rights (contents: write)
and retries a push another job won the race for, on top of that job's commit. Standard library and
git only.
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BRANCH = "ci-plan-inputs"
LOCAL_REF = f"refs/plan-inputs/{BRANCH}"
# what may be stored, and as which file
SETS = {"coverage-map": "coverage-map.json.gz", "deps/core-tests": "deps/core-tests.deps.gz",
        "deps/editor-tests": "deps/editor-tests.deps.gz", "deps/htmlui-tests": "deps/htmlui-tests.deps.gz"}
APPENDS = {"shadow": "shadow.jsonl"}
ATTEMPTS = 5
IDENTITY = {"GIT_AUTHOR_NAME": "github-actions[bot]", "GIT_COMMITTER_NAME": "github-actions[bot]",
            "GIT_AUTHOR_EMAIL": "41898282+github-actions[bot]@users.noreply.github.com",
            "GIT_COMMITTER_EMAIL": "41898282+github-actions[bot]@users.noreply.github.com"}
README = f"""# {BRANCH}

Data, not code: the inputs CI's `changes` job plans pull requests with, kept here because GitHub's
cache evicts. Written only by `tools/ci/plan_inputs.py publish` from workflows on `main`; one
commit, replaced each time. See docs/testing.md, "CI".
"""
VENDORED_PREFIXES = ("/src/engine/libs/", "/src/editor/libs/", "/src/libs/")


def compact_deps(text: str) -> str:
    """`ninja -t deps` cut to what check-changed reads: every object with its dependencies inside the
    repository (the image's /src; the test units' select suites, the others' lead nearest_consumers
    to them), and a vendored library's object with its source only."""
    out: list[str] = []
    header = None
    first = False
    keep = False
    for line in text.splitlines():
        if not line.strip():
            header = None
            continue
        if not line.startswith((" ", "\t")):
            header, first, keep = line.split(":", 1)[0].strip(), True, False
            continue
        if header is None:
            continue
        path = line.strip()
        if first:
            first = False
            if not path.startswith("/src/"):
                continue
            out.append(f"{header}:")
            out.append(f"    {path}")
            keep = not path.startswith(VENDORED_PREFIXES)
            continue
        if keep and path.startswith("/src/"):
            out.append(f"    {path}")
    return "\n".join(out) + "\n" if out else ""


def git(repo: Path, *args: str, data: bytes | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=str(repo), input=data, capture_output=True,
                          env=dict(os.environ, **(env or {})))


def fetch_branch(repo: Path, remote: str) -> str | None:
    """The branch's commit, fetched into LOCAL_REF; None when there is no such branch."""
    r = git(repo, "fetch", "--quiet", "--no-tags", remote, f"+refs/heads/{BRANCH}:{LOCAL_REF}")
    if r.returncode != 0:
        return None
    r = git(repo, "rev-parse", "--verify", "--quiet", f"{LOCAL_REF}^{{commit}}")
    return r.stdout.decode().strip() or None


def read_file(repo: Path, commit: str | None, path: str) -> bytes | None:
    if not commit:
        return None
    r = git(repo, "show", f"{commit}:{path}")
    return r.stdout if r.returncode == 0 else None


def push(repo: Path, commit: str, lease: str | None, remote: str = "origin") -> bool:
    """Replace the branch with commit, if it is still at lease (None: if it does not exist)."""
    r = git(repo, "push", "--quiet", f"--force-with-lease=refs/heads/{BRANCH}:{lease or ''}", remote,
            f"{commit}:refs/heads/{BRANCH}")
    if r.returncode != 0:
        print(f"plan_inputs: push refused: {r.stderr.decode(errors='replace').strip()[-300:]}")
    return r.returncode == 0


def build_commit(repo: Path, old: str | None, files: dict[str, bytes], message: str) -> str:
    """A parentless commit of old's tree with files written over it."""
    with tempfile.TemporaryDirectory(prefix="plan-inputs-index-") as tmp:
        # (a workflow's commits are the bot's; by hand, whoever runs it)
        env = dict(IDENTITY if os.environ.get("GITHUB_ACTIONS") else {}, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        if old:
            r = git(repo, "read-tree", old, env=env)
            if r.returncode != 0:
                raise RuntimeError(f"git read-tree {old}: {r.stderr.decode(errors='replace')}")
        for path, data in sorted(files.items()):
            blob = git(repo, "hash-object", "-w", "--stdin", data=data).stdout.decode().strip()
            r = git(repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=env)
            if r.returncode != 0:
                raise RuntimeError(f"git update-index {path}: {r.stderr.decode(errors='replace')}")
        tree = git(repo, "write-tree", env=env).stdout.decode().strip()
        r = git(repo, "commit-tree", tree, "-m", message, env=env)
        if r.returncode != 0:
            raise RuntimeError(f"git commit-tree: {r.stderr.decode(errors='replace')}")
        return r.stdout.decode().strip()


def publish(repo: Path, sets: dict[str, Path], appends: dict[str, Path], keep: int, commit: str, run: str,
            remote: str = "origin") -> int:
    stamp = {"commit": commit, "run": run, "created": int(time.time())}
    for attempt in range(1, ATTEMPTS + 1):
        old = fetch_branch(repo, remote)
        manifest = json.loads(read_file(repo, old, "manifest.json") or b"{}")
        files: dict[str, bytes] = {"README.md": README.encode()}
        for name, source in sets.items():
            data = source.read_bytes()
            if name.startswith("deps/"):
                data = compact_deps(data.decode(errors="replace")).encode()
            files[SETS[name]] = gzip.compress(data, 9, mtime=0)
            manifest[name] = stamp
        for name, source in appends.items():
            lines = (read_file(repo, old, APPENDS[name]) or b"").decode(errors="replace").splitlines()
            lines += [line for line in source.read_text(errors="replace").splitlines() if line.strip()]
            files[APPENDS[name]] = ("\n".join(lines[-keep:]) + "\n").encode()
            manifest[name] = stamp
        files["manifest.json"] = (json.dumps(manifest, indent=1, sort_keys=True) + "\n").encode()
        new = build_commit(repo, old, files, f"plan inputs: {', '.join([*sets, *appends])} of {commit[:12]} (run {run})")
        if push(repo, new, old, remote):
            print(f"plan_inputs: {BRANCH} is {new[:12]}: {', '.join(f'{n} ({len(files[SETS.get(n) or APPENDS[n]])} bytes)' for n in [*sets, *appends])}")
            return 0
        time.sleep(min(2 ** attempt, 20) if attempt < ATTEMPTS and os.environ.get("CI") else 0)
    print(f"plan_inputs: {BRANCH} not published after {ATTEMPTS} attempts", file=sys.stderr)
    return 1


def fetch(repo: Path, out: Path, remote: str = "origin") -> int:
    commit = fetch_branch(repo, remote)
    if not commit:
        print(f"plan_inputs: no {BRANCH} branch on {remote}: no plan inputs (the plan widens)")
        return 0
    manifest = json.loads(read_file(repo, commit, "manifest.json") or b"{}")
    out.mkdir(parents=True, exist_ok=True)
    got = []
    for name, stored in [*SETS.items(), *APPENDS.items()]:
        data = read_file(repo, commit, stored)
        if data is None:
            continue
        if stored.endswith(".gz"):
            data = gzip.decompress(data)
        target = out / (stored[:-3] if stored.endswith(".gz") else stored)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        made = manifest.get(name, {})
        got.append(f"{name} of {str(made.get('commit', '?'))[:12]} (run {made.get('run', '?')})")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(f"plan_inputs: {BRANCH} {commit[:12]}: " + ("; ".join(got) or "nothing"))
    return 0


def named_files(values: list[str], allowed: dict[str, str], option: str, ap: argparse.ArgumentParser) -> dict[str, Path]:
    found = {}
    for value in values:
        name, sep, path = value.partition("=")
        if not sep or name not in allowed:
            ap.error(f"{option} {value}: NAME is one of {', '.join(allowed)}")
        if not Path(path).is_file():
            ap.error(f"{option} {value}: no file {path}")
        found[name] = Path(path)
    return found


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--out", required=True, type=Path)
    p = sub.add_parser("publish")
    p.add_argument("--commit", required=True, help="the main commit the inputs were made at")
    p.add_argument("--run", required=True, help="the workflow run that made them")
    p.add_argument("--set", action="append", default=[], metavar="NAME=FILE", help=f"one of {', '.join(SETS)}")
    p.add_argument("--append", action="append", default=[], metavar="NAME=FILE", help=f"one of {', '.join(APPENDS)}")
    p.add_argument("--keep", type=int, default=500, help="lines an appended file keeps (the newest)")
    for s in (f, p):
        s.add_argument("--repo", type=Path, default=Path("."))
        s.add_argument("--remote", default="origin")
    c = sub.add_parser("compact-deps")
    c.add_argument("source", type=Path)
    c.add_argument("target", type=Path)
    args = ap.parse_args(argv)
    if args.command == "compact-deps":
        args.target.write_text(compact_deps(args.source.read_text(errors="replace")))
        return 0
    if args.command == "fetch":
        return fetch(args.repo.resolve(), args.out.resolve(), args.remote)
    sets = named_files(args.set, SETS, "--set", p)
    appends = named_files(args.append, APPENDS, "--append", p)
    if not sets and not appends:
        p.error("nothing to publish: --set or --append")
    return publish(args.repo.resolve(), sets, appends, max(1, args.keep), args.commit, args.run, args.remote)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
