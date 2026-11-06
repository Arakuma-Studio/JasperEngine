"""Self-test of tools/mcp/build_cache.py's shared cache (no engine build: a fake build script).

    python3 -I tools/mcp/test_build_cache.py        # in the Linux image (tools/build.sh exec linux ...)

Checks: two runs needing one key at once make one build and both get it; a later run reuses it
without a build; the build is published whole (the readers never see a partial entry); a failed
build fails the run's later requests without a rebuild and is retried in the next run; another key
is another entry; the size limit evicts the least recently used entry that no run holds, never one
a live run holds; a dead build's staging directory is cleaned up; the run's players directory
links to the entry; the local cache (no shared one) still builds and reuses as before.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
checks = 0


def ok(cond, msg):
    global checks
    checks += 1
    if not cond:
        raise SystemExit(f"BUILD CACHE SELFTEST FAIL: {msg}")
    print(f"ok  {msg}", flush=True)


FAKE_BUILD = textwrap.dedent("""\
    #!/usr/bin/env bash
    # fake build_linux_player.sh: --jobs N --engine-version V backend flavour tree out -- args
    set -euo pipefail
    while [[ "$1" == --* ]]; do shift 2; done
    backend=$1 flavour=$2 tree=$3 out=$4
    echo "build $backend-$flavour $$" >> "$FAKE_BUILD_COUNT"
    [[ -n "${FAKE_BUILD_FAIL:-}" ]] && { echo "failing on purpose"; exit 3; }
    mkdir -p "$tree" "$out/linux-x64-$backend-$flavour"
    if [[ -n "${FAKE_BUILD_GATE:-}" ]]; then
        echo ready > "$FAKE_BUILD_GATE.started"
        read -r release < "$FAKE_BUILD_GATE.release"
    fi
    head -c "${FAKE_BUILD_BYTES:-1048576}" /dev/urandom > "$out/linux-x64-$backend-$flavour/jasperengine-player"
    echo '{"id": "linux-x64-'$backend-$flavour'"}' > "$out/linux-x64-$backend-$flavour/player-info.json"
""")

DRIVER = textwrap.dedent("""\
    import sys
    sys.path.insert(0, sys.argv[1])
    import build_cache
    from pathlib import Path
    build_cache.REPO = Path(sys.argv[2])
    build_cache.BUILD_SCRIPT = Path(sys.argv[3])
    try:
        print("RUNTIME", build_cache.ensure_player(sys.argv[4], sys.argv[5]), flush=True)
    except build_cache.BuildCacheError as e:
        print("ERROR", str(e).splitlines()[0], flush=True)
    if sys.argv[6] == "hold":
        sys.stdin.read(1)   # a run that keeps using the runtime until released
""")


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="jasper-build-cache-test-"))
    children = []
    try:
        repo = root / "repo"
        for d in ("engine", "tools/players", "tools/performance", "tools/ci", "tools/mcp", "docker"):
            (repo / d).mkdir(parents=True)
        (repo / "engine" / "a.cpp").write_text("int a;\n")
        for f in ("tools/ci/check-notices.py", "NOTICE.md", "LICENSE", "docker/linux.Dockerfile"):
            (repo / f).write_text("x\n")
        shutil.copy(HERE / "build_cache.py", repo / "tools/mcp/build_cache.py")
        script = root / "fake_build.sh"
        script.write_text(FAKE_BUILD)
        script.chmod(0o755)
        editor = root / "fake_editor.sh"
        editor.write_text('#!/bin/sh\necho \'{"editor_version": "selftest"}\'\n')
        editor.chmod(0o755)
        driver = root / "driver.py"
        driver.write_text(DRIVER)
        count = root / "builds.txt"
        shared = root / "shared"
        env = dict(os.environ, JASPER_EDITOR=str(editor), JASPER_MCP_BUILD_JOBS="1", FAKE_BUILD_COUNT=str(count),
                   JASPER_MCP_SHARED_BUILD_CACHE=str(shared), JASPER_MCP_BUILD_CACHE=str(root / "wt1-cache"),
                   JASPER_MCP_SESSION="run1")
        env.pop("CCACHE_DIR", None)

        def start(backend="glcore", flavour="release", hold="", **extra):
            proc = subprocess.Popen([sys.executable, "-I", str(driver), str(HERE), str(repo), str(script), backend,
                                     flavour, hold], env={**env, **extra}, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, start_new_session=True)
            children.append(proc)
            return proc

        def until(proc, event):
            lines = []
            for line in proc.stdout:
                lines.append(line)
                if event in line:
                    return "".join(lines)
            raise AssertionError(f"worker exited before {event}: {''.join(lines)}")

        def builds() -> int:
            return len(count.read_text().splitlines()) if count.exists() else 0

        def runtime_of(out: str) -> str:
            lines = [l for l in out.splitlines() if l.startswith(("RUNTIME", "ERROR"))]
            line = lines[-1] if lines else out[-500:]
            if line.startswith("RUNTIME "):
                runtime = Path(line[8:])
                if not all((runtime / name).is_file() for name in ("jasperengine-player", "player-info.json")):
                    return f"ERROR incomplete runtime: {runtime}"
            return line

        # two runs (two worktrees) at once: one build, both get it
        gate = root / "build-gate"
        for event in ("started", "release"):
            os.mkfifo(str(gate) + "." + event)
        a = start(FAKE_BUILD_GATE=str(gate))
        with open(str(gate) + ".started") as started:
            started.read()
        b = start(JASPER_MCP_BUILD_CACHE=str(root / "wt2-cache"), JASPER_MCP_SESSION="run2")
        waiting = until(b, "waiting for the build another run")
        # The first build is held and the second is waiting for it: only staging is visible.
        group = shared / "v1" / "linux-x64-glcore-release"
        ok(not [p for p in group.iterdir() if p.is_dir() and not p.name.startswith(".")],
           "no entry is visible while its build runs (published whole)")
        with open(str(gate) + ".release", "w") as release:
            release.write("release\n")
        out_a, out_b = a.communicate()[0], waiting + b.communicate()[0]
        ok(runtime_of(out_a).startswith("RUNTIME") and runtime_of(out_b).startswith("RUNTIME"),
           f"both runs get the runtime: {runtime_of(out_a)} | {runtime_of(out_b)}")
        ok(builds() == 1, f"two runs needing one key at once build it once (builds: {builds()})")
        ok("waiting for the build another run" in out_b, "the second run waited for the first one's build")
        entries = [p for p in group.iterdir() if p.is_dir() and not p.name.startswith(".")]
        ok(len(entries) == 1 and (entries[0] / "key.json").is_file() and
           (entries[0] / "players/linux-x64-glcore-release/player-info.json").is_file(), "one complete entry")
        link = root / "wt2-cache" / "linux-x64-glcore-release" / "players"
        ok(link.is_symlink() and Path(os.readlink(link)) == entries[0] / "players",
           "the run's players directory links to the entry")
        ok(not list(group.glob(".new-*")), "no staging directory left")

        # a later run: reused, no build
        c = start(JASPER_MCP_BUILD_CACHE=str(root / "wt3-cache"), JASPER_MCP_SESSION="run3")
        out_c = c.communicate()[0]
        ok(runtime_of(out_c).startswith("RUNTIME") and builds() == 1 and "reused from the shared cache" in out_c,
           "a later run reuses the entry without a build")

        # a failed build: the run's later requests fail at once, the next run tries again
        d = start(flavour="development", FAKE_BUILD_FAIL="1")
        out_d = d.communicate()[0]
        n = builds()
        e = start(flavour="development", FAKE_BUILD_FAIL="1")
        out_e = e.communicate()[0]
        ok(runtime_of(out_d).startswith("ERROR") and "failed earlier in this run" in runtime_of(out_e) and builds() == n,
           "a failed build fails the run's later requests without a rebuild")
        f = start(flavour="development", JASPER_MCP_SESSION="run9")
        out_f = f.communicate()[0]
        ok(runtime_of(out_f).startswith("RUNTIME") and builds() == n + 1, "the next run builds it again (and it works)")

        # another key: another entry; the size limit evicts the least recently used entry nobody
        # holds, never the one a live run holds
        holder = start(hold="hold", JASPER_MCP_SESSION="run4")   # holds glcore-release
        until(holder, "RUNTIME")
        held = [p for p in group.iterdir() if p.is_dir() and not p.name.startswith(".")][0]
        os.utime(held, (time.time() - 3600, time.time() - 3600))   # ... and makes it the least recently used
        g = start(CFLAGS="-DOTHER", FAKE_BUILD_BYTES=str(3 << 20), JASPER_MCP_BUILD_CACHE_MAX_GB=str(4.5 / 1024),
                  JASPER_MCP_SESSION="run5")
        out_g = g.communicate()[0]
        names = sorted(p.parent.name + "/" + p.name[:8] for p in shared.glob("v1/*/*") if p.is_dir()
                       and not p.name.startswith("."))
        ok(len([p for p in group.iterdir() if p.is_dir() and not p.name.startswith(".")]) == 2 and held.is_dir(),
           f"another key is another entry, and the held (least recently used) entry stays ({names})")
        ok("evicted linux-x64-glcore-development" in out_g,
           f"the size limit evicted the least recently used entry nobody holds: {out_g[-600:]}")
        holder.communicate("release")

        # a build that died: its staging directory goes at the next eviction
        stale = group / ".new-0123456789abcdef-host-1"
        (stale / "players").mkdir(parents=True)
        sys.path.insert(0, str(HERE))
        os.environ.update(JASPER_MCP_SHARED_BUILD_CACHE=str(shared))
        import build_cache  # noqa: E402
        build_cache.evict(shared / "v1")
        ok(not stale.exists(), "a dead build's staging directory is removed")

        # the local cache (no shared one): builds once, reuses after
        local_env = {"JASPER_MCP_SHARED_BUILD_CACHE": "", "JASPER_MCP_BUILD_CACHE": str(root / "local"),
                     "JASPER_MCP_SESSION": "run6"}
        n = builds()
        h = start(**local_env)
        out_h = h.communicate()[0]
        i = start(**local_env)
        out_i = i.communicate()[0]
        local_players = root / "local" / "linux-x64-glcore-release" / "players"
        ok(runtime_of(out_h).startswith("RUNTIME") and runtime_of(out_i).startswith("RUNTIME") and builds() == n + 1
           and local_players.is_dir() and not local_players.is_symlink(), "the local cache builds once and reuses")
        print(f"build cache selftest: {checks} checks passed")
        return 0
    finally:
        for proc in children:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
            proc.wait()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
