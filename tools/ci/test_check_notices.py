#!/usr/bin/env python3
"""Unit tests of tools/ci/check-notices.py (python3 tools/ci/test_check_notices.py; CI's unit job).

Each test builds a small repository layout (libs/, engine/libs/, NOTICE.md) in a temporary
directory, plus the real repository's check as the end-to-end case.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_notices", HERE / "check-notices.py")
check_notices = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check_notices  # (dataclasses look their module up)
spec.loader.exec_module(check_notices)

NOTICE = """# Notice

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| alpha | 1 | `libs/alpha/` (`LICENSE`) | MIT | a |
| beta | 2 | `engine/libs/beta/` (`COPYING`) | zlib | b |
| googletest | 1.17.0 | `libs/googletest/` (`LICENSE`) | BSD-3-Clause | tests only |
"""


class CheckNoticesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="check-notices-test-")
        self.repo = Path(self.directory.name)
        for path, text in (("libs/alpha/LICENSE", "MIT"), ("engine/libs/beta/COPYING", "zlib"),
                           ("engine/libs/beta/NOTICE", "notice"), ("libs/googletest/LICENSE", "BSD"),
                           ("engine/libs/shadercompiler/src/x.cc", "ours")):
            (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
            (self.repo / path).write_text(text)
        (self.repo / "NOTICE.md").write_text(NOTICE)
        self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"}})

    def write_game_notices(self, libraries):
        import json
        (self.repo / "engine/notices.json").write_text(json.dumps({"schemaVersion": 1, "libraries": libraries}))

    def tearDown(self):
        self.directory.cleanup()

    def test_complete_repository_passes(self):
        self.assertEqual(check_notices.check_repository(self.repo), [])

    def test_removed_row_fails(self):
        (self.repo / "NOTICE.md").write_text(NOTICE.replace("| beta | 2 | `engine/libs/beta/` (`COPYING`) | zlib | b |\n", ""))
        problems = check_notices.check_repository(self.repo)
        self.assertEqual(len(problems), 1)
        self.assertIn("engine/libs/beta: no NOTICE.md row", problems[0])

    def test_location_outside_a_table_does_not_count(self):
        (self.repo / "NOTICE.md").write_text(NOTICE.replace("| alpha | 1 | `libs/alpha/` (`LICENSE`) | MIT | a |\n", "")
                                             + "\nSee `libs/alpha/`.\n")
        self.assertIn("libs/alpha: no NOTICE.md row names `libs/alpha/`", check_notices.check_repository(self.repo))

    def test_missing_licence_file_fails(self):
        (self.repo / "libs/alpha/LICENSE").unlink()
        (self.repo / "libs/alpha/README.md").write_text("readme")
        self.assertIn("libs/alpha: no licence file (LICENSE, LICENCE, COPYING or UNLICENSE)",
                      check_notices.check_repository(self.repo))

    def test_licence_named_by_its_extension(self):
        # FFmpeg's COPYING.LGPLv2.1 is a licence file; any other extension is not
        (self.repo / "libs/alpha/LICENSE").unlink()
        (self.repo / "libs/alpha/COPYING.LGPLv2.1").write_text("LGPL")
        self.assertEqual(check_notices.check_repository(self.repo), [])
        (self.repo / "libs/alpha/COPYING.LGPLv2.1").rename(self.repo / "libs/alpha/COPYING.bak")
        self.assertIn("libs/alpha: no licence file (LICENSE, LICENCE, COPYING or UNLICENSE)",
                      check_notices.check_repository(self.repo))

    def test_new_library_without_anything_fails_three_times(self):
        (self.repo / "engine/libs/gamma").mkdir()
        problems = check_notices.check_repository(self.repo)
        self.assertEqual(len(problems), 3)
        self.assertTrue(all(p.startswith("engine/libs/gamma:") for p in problems))
        self.assertIn("no engine/notices.json entry", problems[2])

    # S30 AC12: an engine/libs directory missing from engine/notices.json fails
    def test_engine_library_without_game_notice_entry_fails(self):
        self.write_game_notices({})
        self.assertEqual(check_notices.check_repository(self.repo),
                         ["engine/libs/beta: no engine/notices.json entry (when: always, a player feature or editor)"])

    def test_game_notice_entries_are_checked(self):
        self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "sometimes"},
                                 "ghost": {"dir": "engine/libs/ghost", "when": "always"}})
        problems = check_notices.check_repository(self.repo)
        self.assertTrue(any("beta: when must be one of" in p for p in problems), problems)
        self.assertTrue(any("ghost: engine/libs/ghost does not exist" in p for p in problems), problems)

    def test_player_manifest_follows_its_features(self):
        (self.repo / "engine/libs/delta").mkdir()
        (self.repo / "engine/libs/delta/LICENSE").write_text("MIT")
        self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"},
                                 "delta": {"dir": "engine/libs/delta", "when": "physics3d"}})
        names = [name for _, name in check_notices.player_manifest(self.repo, [])]
        self.assertEqual(names, ["beta-LICENSE", "beta-NOTICE"])
        names = [name for _, name in check_notices.player_manifest(self.repo, ["physics3d"])]
        self.assertEqual(names, ["beta-LICENSE", "beta-NOTICE", "delta-LICENSE"])

    # S80 R40: ThorVG ships with the HTML UI or with Title Art, a list of player features
    def test_when_list_ships_with_any_of_its_features(self):
        (self.repo / "engine/libs/delta").mkdir()
        (self.repo / "engine/libs/delta/LICENSE").write_text("MIT")
        self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"},
                                 "delta": {"dir": "engine/libs/delta", "when": ["htmlui", "titleart"]}})
        self.assertEqual(check_notices.check_game_notices(self.repo), [])
        self.assertEqual(check_notices.player_libraries(self.repo, []), ["beta"])
        self.assertEqual(check_notices.player_libraries(self.repo, ["titleart"]), ["beta", "delta"])
        self.assertEqual(check_notices.player_libraries(self.repo, ["htmlui"]), ["beta", "delta"])
        self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"},
                                 "delta": {"dir": "engine/libs/delta", "when": ["htmlui", "sometimes"]}})
        self.assertTrue(any("delta: when must be one of" in p for p in check_notices.check_game_notices(self.repo)))

    # S81 R50: a library inside another's directory (HarfBuzz's editor-only subsetter) has its own
    # NOTICE.md location, licence file and engine/notices.json entry under its nested name
    def test_nested_library_needs_its_own_entries(self):
        nested = "engine/libs/beta/sub"
        (self.repo / nested).mkdir()
        (self.repo / nested / "COPYING").write_text("zlib")
        with mock.patch.dict(check_notices.NESTED_LIBRARIES, {nested: "beta-sub"}, clear=True):
            problems = check_notices.check_repository(self.repo)
            self.assertEqual(problems, ["engine/libs/beta/sub: no NOTICE.md row names `engine/libs/beta/sub/`",
                                        "engine/libs/beta/sub: no engine/notices.json entry (when: always, a player feature or editor)"])
            (self.repo / "NOTICE.md").write_text(NOTICE.replace("| zlib | b |", "; `engine/libs/beta/sub/` (`COPYING`) | zlib | b |"))
            self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"},
                                     "beta-sub": {"dir": nested, "when": "editor"}})
            self.assertEqual(check_notices.check_repository(self.repo), [])
            self.assertIn("beta-sub-LICENSE", [name for _, name in check_notices.install_manifest(self.repo)])
            self.assertEqual(check_notices.player_libraries(self.repo, ["titleart"]), ["beta"])
            self.write_game_notices({"beta": {"dir": "engine/libs/beta", "when": "always"},
                                     "sub": {"dir": nested, "when": "editor"}})
            self.assertTrue(any("sub: the name must be its directory's" in p for p in check_notices.check_repository(self.repo)))

    def test_third_party_notices_text(self):
        text = check_notices.third_party_notices(self.repo, [])
        self.assertIn("beta: beta, version 2, zlib (licenses/beta-LICENSE). Used for: b", text)
        self.assertNotIn("alpha", text)

    def test_first_party_directory_is_not_a_library(self):
        self.assertNotIn("engine/libs/shadercompiler", check_notices.vendored_libraries(self.repo))

    def test_manifest_names_and_skips_tests_only(self):
        names = [name for _, name in check_notices.install_manifest(self.repo)]
        self.assertEqual(names, ["alpha-LICENSE", "beta-LICENSE", "beta-NOTICE"])

    def test_licence_file_patterns(self):
        for name in ("LICENSE", "LICENSE.txt", "LICENSE.md", "LICENSE.TXT", "COPYING", "LICENSE-taffy.txt", "UNLICENSE"):
            self.assertTrue(check_notices.LICENCE_FILE.match(name), name)
        for name in ("license-checker.cfg", "README.md", "LICENSES"):
            self.assertFalse(check_notices.LICENCE_FILE.match(name), name)

    def test_installed_tree(self):
        install = self.repo / "install/bin"
        (install / "licenses").mkdir(parents=True)
        for name in ("NOTICE.md", "LICENSE"):
            (install / name).write_text("x")
        for _, name in check_notices.install_manifest(self.repo):
            (install / "licenses" / name).write_text("x")
        self.assertEqual(check_notices.check_installed(install, self.repo), [])
        (install / "licenses/beta-LICENSE").unlink()
        (install / "engine").mkdir()
        (install / "engine/CMakePresets.json").write_text("{}")
        problems = check_notices.check_installed(install, self.repo)
        self.assertEqual(len(problems), 2)
        self.assertIn("no licenses/beta-LICENSE", problems[0])
        self.assertIn("CMakePresets.json", problems[1])

    def test_this_repository_passes(self):
        self.assertEqual(check_notices.check_repository(check_notices.REPO), [])


if __name__ == "__main__":
    unittest.main()
