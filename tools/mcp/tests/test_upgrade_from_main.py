"""A game made with an origin/main editor survives the upgrade to this one (docs/upgrading-from-main.md).

The fixture tools/test-projects/upgrade-from-main (tools/ci/upgrade_from_main/generate.sh) is a sample
game an origin/main editor built through its MCP tools, with the behaviour transcripts it printed.
This runs the editor part of tools/ci/upgrade_from_main/check.sh (check.py): the first open migrates
every scene and prefab with nothing but migrations reported and saves them in the current formats,
every asset gets a GUID, every reference resolves, a second open rewrites nothing, and the autopilot
play prints the old editor's transcript. check.sh adds the input run, the screenshots and the
exported Linux and web players.
"""

from __future__ import annotations

import sys
from pathlib import Path

import harness

CI = True
TIMEOUT = 3600

sys.path.insert(0, str(harness.REPO / "tools" / "ci" / "upgrade_from_main"))
import check  # noqa: E402

FIXTURE = harness.REPO / "tools" / "test-projects" / "upgrade-from-main"


async def test_fixture_upgrades():
    out = Path(harness.tempfile.mkdtemp(prefix="jasper-upgrade-"))
    report = await check.run(Path(harness.editor_command()[0]), FIXTURE, out, modes=("autopilot",), cpp=False)
    if report.failures:
        raise AssertionError(f"{len(report.failures)} upgrade check(s) failed (kept in {out}):\n"
                             + "\n".join(report.failures[:20]))
    harness.shutil.rmtree(out, ignore_errors=True)
