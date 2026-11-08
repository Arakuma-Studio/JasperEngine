#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/check_gradle_warnings.py (python3 tools/ci/test_check_gradle_warnings.py).

The logs are Gradle's own wording with --warning-mode all: a DSL deprecation in one of our build
files, an obsolete gradle.properties option the Android plugin reports, a third-party plugin's
deprecation (a note, not a failure) and a clean configuration.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_gradle_warnings", HERE / "check_gradle_warnings.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

DSL = """> Configure project :app
Build file '/src/engine/workspaces/androidstudio/app/build.gradle': line 48
Properties should be assigned using the 'propName = value' syntax. Setting a property via the Gradle-generated 'propName value' or 'propName(value)' syntax in Groovy DSL has been deprecated. This is scheduled to be removed in Gradle 10. Use assignment ('multiDexEnabled = <value>') instead.
        at build_7c2$_run_closure2$_closure5.doCall$original(/src/engine/workspaces/androidstudio/app/build.gradle:48)
        (Run with --stacktrace to get the full stack trace of this deprecation warning.)
"""

GENERATED = """Build file '/home/me/MyGame/export/android/workspaces/androidstudio/nativeplugins/build.gradle': line 21
Properties should be assigned using the 'propName = value' syntax. Setting a property via the Gradle-generated 'propName value' or 'propName(value)' syntax in Groovy DSL has been deprecated.
"""

OPTION = """WARNING: The option setting 'android.nonFinalResIds=false' is deprecated.
The current default is 'true'.
It will be removed in version 10.0 of the Android Gradle plugin.
"""

THIRD_PARTY = """Script file '/root/.gradle/caches/some-plugin/init.gradle': line 3
The Project.getConvention() method has been deprecated. This is scheduled to be removed in Gradle 10.
"""

CLEAN = """> Task :help

Welcome to Gradle 9.6.0.

BUILD SUCCESSFUL in 41s
"""


class CheckGradleWarningsTest(unittest.TestCase):
    def test_dsl_deprecation_in_our_build_file_fails(self):
        failures, notes = check.problems(DSL)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("app/build.gradle:48", failures[0])
        self.assertEqual(notes, [])

    def test_generated_module_is_ours(self):
        failures, _ = check.problems(GENERATED)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("nativeplugins/build.gradle:21", failures[0])

    def test_obsolete_option_fails(self):
        failures, _ = check.problems(OPTION)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("android.nonFinalResIds", failures[0])

    def test_third_party_deprecation_is_a_note(self):
        failures, notes = check.problems(THIRD_PARTY)
        self.assertEqual(failures, [])
        self.assertEqual(len(notes), 1, notes)

    def test_clean_log_passes(self):
        self.assertEqual(check.problems(CLEAN), ([], []))

    def test_main_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "bad.log"
            bad.write_text(DSL + OPTION)
            good = Path(directory) / "good.log"
            good.write_text(CLEAN + THIRD_PARTY)
            self.assertEqual(check.main(["check_gradle_warnings.py", str(bad)]), 1)
            self.assertEqual(check.main(["check_gradle_warnings.py", str(good)]), 0)
            self.assertEqual(check.main(["check_gradle_warnings.py"]), 2)


if __name__ == "__main__":
    unittest.main()
