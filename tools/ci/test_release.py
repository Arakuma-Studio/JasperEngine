#!/usr/bin/env python3
"""Tests of tools/ci/release.sh (python3 -I tools/ci/test_release.py), against a stand-in gh on PATH
that keeps its releases in a scratch directory: a release is a draft until every asset is on it at
its size, a dry run stays a draft, a re-run takes the draft or re-uploads to a release of the same
commit and refuses one of another, nightlies past the newest 14 go with their tags, and the
version, channel and commit checks.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "release.sh"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
VERSION = "nightly-20261112"

# releases.txt: "<tag> <draft> <commit>" lines; assets.txt: "<name> <size>" of the one release in play
FAKE_GH = r"""#!/usr/bin/env bash
S="$FAKE_STATE"
echo "gh $*" >> "$S/calls.log"
case "$1 $2" in
"release list")
    while read -r tag draft commit; do [[ "$tag" =~ ^nightly-[0-9]{8}$ ]] && echo "$tag $draft"; done < "$S/releases.txt"; true ;;
"release view")
    line="$(grep "^$3 " "$S/releases.txt")" || { echo "release not found" >&2; exit 1; }
    if [[ "$*" == *assets* ]]; then cat "$S/assets.txt" 2>/dev/null; true; else echo "$line" | awk '{print $2}'; fi ;;
"release create")
    commit=""; args=("$@"); for i in "${!args[@]}"; do [[ "${args[$i]}" == --target ]] && commit="${args[$((i+1))]}"; done
    echo "$3 true $commit" >> "$S/releases.txt" ;;
"release edit")
    if [[ "$*" == *--draft=false* ]]; then sed -i "s/^$3 true /$3 false /" "$S/releases.txt"; fi ;;
"release upload")
    seen=0
    for a in "$@"; do
        [[ $seen == 1 && "$a" != "${FAKE_SKIP:-none}" ]] && echo "$a $(stat -c %s "$a")" >> "$S/assets.txt"
        [[ "$a" == -- ]] && seen=1
        true
    done ;;
"release delete")
    sed -i "/^$3 /d" "$S/releases.txt" ;;
"api repos/o/r/releases/generate-notes")
    echo "## What's Changed" ;;
api*)
    awk -v t="${2##*/}" '$1 == t { print $3 }' "$S/releases.txt" ;;
*) echo "fake gh: unhandled $*" >&2; exit 3 ;;
esac
"""

EDITORS = ("windows-x64.zip", "linux-x86_64.AppImage", "linux-x86_64.AppImage.zsync", "linux-x86_64.zip",
           "macos-universal.zip", "macos-universal.dmg")


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="release-test-"))
        (self.tmp / "bin").mkdir()
        gh = self.tmp / "bin" / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(0o755)
        self.state = self.tmp / "state"
        self.state.mkdir()
        (self.state / "releases.txt").write_text("")
        self.assets = self.tmp / "assets"
        self.assets.mkdir()
        for name in EDITORS:
            (self.assets / f"JasperEngine-{VERSION}-{name}").write_text(name)
        (self.assets / f"jasperengine-player-linux-x64-glcore-release-{VERSION}.zip").write_text("player")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_script(self, *args: str, **env: str) -> subprocess.CompletedProcess:
        environ = dict(os.environ, PATH=f"{self.tmp / 'bin'}:{os.environ['PATH']}", FAKE_STATE=str(self.state),
                       GITHUB_REPOSITORY="o/r", **env)
        return subprocess.run(["bash", str(SCRIPT), *args], capture_output=True, text=True, env=environ, timeout=60)

    def releases(self) -> dict[str, tuple[str, str]]:
        lines = (self.state / "releases.txt").read_text().split("\n")
        return {tag: (draft, commit) for tag, draft, commit in (line.split() for line in lines if line.strip())}

    def calls(self) -> str:
        return (self.state / "calls.log").read_text()

    def test_publish_drafts_uploads_every_asset_then_publishes(self):
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.releases()[VERSION], ("false", COMMIT))
        sums = (self.assets / "SHA256SUMS").read_text()
        self.assertEqual(len(sums.splitlines()), len(EDITORS) + 1)
        self.assertNotIn("/", sums)
        uploaded = {line.split()[0] for line in (self.state / "assets.txt").read_text().splitlines()}
        self.assertEqual(uploaded, {p.name for p in self.assets.iterdir()})
        calls = self.calls()
        self.assertIn(f"release create {VERSION} --repo o/r --draft --target {COMMIT}", calls)
        self.assertIn("--prerelease", calls)
        self.assertIn("--draft=false --prerelease --latest=false", calls)
        self.assertLess(calls.index("release upload"), calls.index("--draft=false"))

    def test_a_missing_upload_leaves_the_draft(self):
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets), FAKE_SKIP="SHA256SUMS")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("SHA256SUMS", p.stderr)
        self.assertEqual(self.releases()[VERSION], ("true", COMMIT))

    def test_a_missing_editor_package_publishes_nothing(self):
        (self.assets / f"JasperEngine-{VERSION}-macos-universal.dmg").unlink()
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("macos-universal.dmg", p.stderr)
        self.assertNotIn(VERSION, self.releases())

    def test_dry_run_stays_a_draft_and_a_rerun_takes_it(self):
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets), "--dry-run")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.releases()[VERSION], ("true", COMMIT))
        self.assertNotIn("--draft=false", self.calls())
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.calls().count("release create"), 1)
        self.assertEqual(self.releases()[VERSION], ("false", COMMIT))

    def test_a_published_release_of_another_commit_is_refused(self):
        (self.state / "releases.txt").write_text(f"{VERSION} false {'f' * 40}\n")
        p = self.run_script("publish", "nightly", VERSION, COMMIT, str(self.assets))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("already published", p.stderr)
        self.assertNotIn("release upload", self.calls())

    def test_stable_is_latest(self):
        for name in EDITORS:
            (self.assets / f"JasperEngine-{VERSION}-{name}").rename(self.assets / f"JasperEngine-v1.2.3-{name}")
        (self.assets / f"jasperengine-player-linux-x64-glcore-release-{VERSION}.zip").rename(
            self.assets / "jasperengine-player-linux-x64-glcore-release-v1.2.3.zip")
        p = self.run_script("publish", "stable", "v1.2.3", COMMIT, str(self.assets))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("--draft=false --latest", self.calls())
        self.assertNotIn("--prerelease", self.calls())

    def test_newest_nightly_and_prune(self):
        lines = [f"nightly-202610{d:02d} false {str(d) * 40}"[:60] for d in range(1, 17)]
        lines.append("nightly-20261020 true x")
        lines.append("v0.7.1 false y")
        (self.state / "releases.txt").write_text("\n".join(lines) + "\n")
        p = self.run_script("newest-nightly")
        self.assertEqual(p.stdout.split()[0], "nightly-20261016")
        p = self.run_script("prune", "14", VERSION)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        left = self.releases()
        self.assertEqual(sorted(t for t in left if t.startswith("nightly-")), [f"nightly-202610{d:02d}" for d in range(3, 17)])
        self.assertIn("v0.7.1", left)
        self.assertIn("release delete nightly-20261001 --repo o/r --cleanup-tag --yes", self.calls())

    def test_check(self):
        for args, code in ((("nightly", VERSION, COMMIT), 0), (("stable", "v1.2.3", COMMIT), 0),
                           (("stable", "v1.2", COMMIT), 2), (("stable", VERSION, COMMIT), 2),
                           (("nightly", "nightly-2026111", COMMIT), 2), (("nightly", VERSION, "abc1234"), 2),
                           (("beta", "v1.2.3", COMMIT), 2)):
            with self.subTest(args=args):
                self.assertEqual(self.run_script("check", *args).returncode, code)


if __name__ == "__main__":
    unittest.main()
