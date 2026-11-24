#!/usr/bin/env python3
"""Tests of CI's line-tables-only debug information (python3 -I tools/ci/test_debug_info.py).

engine/cmake/JasperDebugInfo.cmake: scratch CMake trees that include it, configured with the host's
GCC and Clang, whose compile commands must end on the right -g level per build type (the last -g
level on a command line wins). tools/build.sh: a fake docker records what a unit-tests build hands
the image, which must carry the switch to the CMake configure and to the container's environment.
Needs cmake and g++ on PATH (the lint lane's runner has both, and clang++).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "engine/cmake/JasperDebugInfo.cmake"

PROJECT = """\
cmake_minimum_required(VERSION 3.20)
project(debuginfo C CXX)
include("{module}")
add_library(plain STATIC plain.cpp)
# a shipped game's split symbols (engine/CMakeLists.txt, JASPERENGINE_SPLIT_SYMBOLS)
add_library(split STATIC split.c)
target_compile_options(split PRIVATE ${{JASPERENGINE_DEBUG_FLAG}})
"""
G_FLAG = re.compile(r"^-g(\d|line-tables-only)?$")


def debug_flags(command: str) -> list[str]:
    return [word for word in command.split() if G_FLAG.match(word)]


class ModuleTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="debuginfo-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        src = self.dir / "src"
        src.mkdir()
        (src / "CMakeLists.txt").write_text(PROJECT.format(module=MODULE.as_posix()))
        (src / "plain.cpp").write_text("int plain() { return 1; }\n")
        (src / "split.c").write_text("int split(void) { return 2; }\n")
        self.runs = 0

    def configure(self, cxx: str, build_type: str, *defines: str, env_value: str | None = None):
        """(returncode, output, {source name: its -g flags in order})"""
        self.runs += 1
        tree = self.dir / f"b{self.runs}"
        env = {k: v for k, v in os.environ.items() if k != "JASPERENGINE_DEBUG_INFO"}
        if env_value is not None:
            env["JASPERENGINE_DEBUG_INFO"] = env_value
        cc = {"g++": "gcc", "clang++": "clang"}[cxx]
        r = subprocess.run(["cmake", "-S", str(self.dir / "src"), "-B", str(tree), f"-DCMAKE_BUILD_TYPE={build_type}",
                            f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
                            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *defines],
                           capture_output=True, text=True, env=env)
        flags = {}
        commands = tree / "compile_commands.json"
        if r.returncode == 0:
            for entry in json.loads(commands.read_text()):
                flags[Path(entry["file"]).name] = debug_flags(entry["command"])
        return r.returncode, r.stdout + r.stderr, flags

    def compilers(self):
        found = [c for c in ("g++", "clang++") if shutil.which(c) and shutil.which({"g++": "gcc", "clang++": "clang"}[c])]
        self.assertIn("g++", found, "the test needs g++ and gcc on PATH")
        return found

    def test_lines_ends_every_debug_build_on_line_tables(self):
        for cxx in self.compilers():
            lines = "-g1" if cxx == "g++" else "-gline-tables-only"
            for build_type in ("Debug", "RelWithDebInfo"):
                with self.subTest(cxx=cxx, build_type=build_type):
                    code, out, flags = self.configure(cxx, build_type, "-DJASPERENGINE_DEBUG_INFO=lines")
                    self.assertEqual(code, 0, out)
                    self.assertIn(f"Debug information: line tables only ({lines})", out)
                    self.assertEqual(flags["plain.cpp"][-1:], [lines], flags)
                    self.assertEqual(flags["split.c"][-1:], [lines], flags)

    def test_lines_adds_nothing_to_a_build_without_debug_information(self):
        for cxx in self.compilers():
            with self.subTest(cxx=cxx):
                code, out, flags = self.configure(cxx, "Release", "-DJASPERENGINE_DEBUG_INFO=lines")
                self.assertEqual(code, 0, out)
                self.assertEqual(flags["plain.cpp"], [], flags)
                # the split symbols' own flag, at line tables
                self.assertEqual(flags["split.c"], ["-g1" if cxx == "g++" else "-gline-tables-only"], flags)

    def test_full_is_the_default_and_changes_nothing(self):
        code, out, flags = self.configure("g++", "RelWithDebInfo")
        self.assertEqual(code, 0, out)
        self.assertNotIn("line tables only", out)
        self.assertEqual(flags["plain.cpp"], ["-g"], flags)
        self.assertEqual(flags["split.c"], ["-g", "-g"], flags)
        code, out, flags = self.configure("g++", "Release")
        self.assertEqual((code, flags["plain.cpp"], flags["split.c"]), (0, [], ["-g"]), out)

    def test_the_environment_gives_the_default_and_a_define_wins(self):
        code, out, flags = self.configure("g++", "RelWithDebInfo", env_value="lines")
        self.assertEqual(code, 0, out)
        self.assertEqual(flags["plain.cpp"][-1:], ["-g1"], flags)
        code, out, flags = self.configure("g++", "RelWithDebInfo", "-DJASPERENGINE_DEBUG_INFO=full", env_value="lines")
        self.assertEqual(code, 0, out)
        self.assertEqual(flags["plain.cpp"], ["-g"], flags)

    def test_an_unknown_value_fails_the_configure(self):
        code, out, _ = self.configure("g++", "Debug", "-DJASPERENGINE_DEBUG_INFO=minimal")
        self.assertNotEqual(code, 0)
        self.assertIn("JASPERENGINE_DEBUG_INFO is full or lines, got 'minimal'", out)


FAKE_DOCKER = """\
#!/usr/bin/env bash
# a file per call: its arguments, NUL-terminated
printf '%s\\0' "$@" > "$(mktemp "$FAKE_DOCKER_LOG/call.XXXXXX")"
"""


class BuildShTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="debuginfo-buildsh-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        docker = bin_dir / "docker"
        docker.write_text(FAKE_DOCKER)
        docker.chmod(0o755)
        self.bin = bin_dir

    def build(self, value: str | None):
        """(returncode, output, [argv of each docker run])"""
        log = Path(tempfile.mkdtemp(dir=self.dir))
        env = {k: v for k, v in os.environ.items() if k not in ("JASPERENGINE_DEBUG_INFO", "SANITIZE", "UNITY", "CXX_STANDARD")}
        env.update(PATH=f"{self.bin}{os.pathsep}{env['PATH']}", FAKE_DOCKER_LOG=str(log), JASPER_DOCKER_IMAGE="fake-image",
                   RUN="0", UNIT_SUITE="core")
        if value is not None:
            env["JASPERENGINE_DEBUG_INFO"] = value
        r = subprocess.run([str(ROOT / "tools/build.sh"), "unit-tests"], capture_output=True, text=True, env=env,
                           stdin=subprocess.DEVNULL)
        runs = []
        for call in sorted(log.iterdir(), key=lambda p: p.stat().st_mtime_ns):
            argv = call.read_text().split("\0")[:-1]
            if argv[:1] == ["run"]:
                runs.append(argv)
        return r.returncode, r.stdout + r.stderr, runs

    def test_the_switch_reaches_the_configure_and_the_container(self):
        _, out, runs = self.build("lines")
        self.assertTrue(runs, out)
        argv = runs[0]
        self.assertIn("JASPERENGINE_DEBUG_INFO=lines", argv, "the container's environment: games the editor builds")
        self.assertIn("-DJASPERENGINE_DEBUG_INFO=lines", argv[-1], "the tree's configure")

    def test_without_it_the_configure_gets_full(self):
        _, out, runs = self.build(None)
        self.assertTrue(runs, out)
        argv = runs[0]
        self.assertFalse([a for a in argv if a.startswith("JASPERENGINE_DEBUG_INFO=")], argv)
        self.assertIn("-DJASPERENGINE_DEBUG_INFO=full", argv[-1])

    def test_an_unknown_value_stops_before_the_build(self):
        code, out, runs = self.build("lines;rm")
        self.assertEqual(code, 2, out)
        self.assertIn("JASPERENGINE_DEBUG_INFO is full or lines", out)
        self.assertEqual(runs, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
