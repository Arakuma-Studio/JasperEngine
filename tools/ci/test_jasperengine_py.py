#!/usr/bin/env python3
"""Unit tests of engine/jasperengine.py, the build script of exported source projects
(python3 tools/ci/test_jasperengine_py.py; CI's unit job).

Web builds configure CMake directly with Emscripten's toolchain file, as the editor does (no
emcmake: Emscripten 6 ships emcmake.exe on Windows, earlier versions emcmake.bat), and on Windows
pick a generator Emscripten can use: Ninja on PATH, else Visual Studio's own Ninja, else
mingw32-make. Fake SDK layouts in a temporary directory; Windows is simulated (sys.platform, PATH,
vswhere) so the choices are tested on any host.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "engine" / "jasperengine.py"
spec = importlib.util.spec_from_file_location("jasperengine_build", SCRIPT)
script = importlib.util.module_from_spec(spec)
spec.loader.exec_module(script)


def touch(path: Path, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


class WebBuildTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="jasperengine-py-test-")
        self.root = Path(self.directory.name)
        # an emsdk with Emscripten 6's Windows launchers only
        self.emsdk = self.root / "emsdk"
        self.emscripten = self.emsdk / "upstream" / "emscripten"
        touch(self.emscripten / "emcc.exe")
        touch(self.emscripten / "emcmake.exe")
        touch(self.emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake")
        touch(self.emsdk / "emsdk.py")
        self.node = touch(self.emsdk / "node" / "22.16.0_64bit" / "bin" / "node.exe")
        # Visual Studio with its C++ CMake tools: vswhere and the Ninja beside it
        self.program_files = self.root / "Program Files (x86)"
        touch(self.program_files / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
        self.vs = self.root / "VS"
        self.vs_ninja = self.vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe"
        self.project = self.root / "project"
        self.project.mkdir()
        self.build = self.root / "build"
        self.commands: list[list[str]] = []
        self.envs: list[dict] = []

    def tearDown(self):
        self.directory.cleanup()

    def fake_run(self, command, **kwargs):
        if str(command[0]).endswith("vswhere.exe"):
            stdout = str(self.vs) + "\n" if self.vs_ninja.is_file() else ""
            return subprocess.CompletedProcess(command, 0, stdout=stdout, stderr="")
        self.commands.append(list(command))
        self.envs.append(kwargs.get("env") or {})
        return subprocess.CompletedProcess(command, 0)

    def configure(self, programs_on_path=("cmake",), extra_args=(), windows=True, extra_env=None, platform="web", build=False):
        """Runs the script's build() (`--platform web --no-build` by default); returns (exit code, stdout+stderr)."""
        argv = ["jasperengine.py", "--platform", platform, "--project", str(self.project), "--output", str(self.build),
                "--build" if build else "--no-build", *extra_args]
        env = {"EMSDK": str(self.emsdk), "ProgramFiles(x86)": str(self.program_files), "PATH": ""}
        env.update(extra_env or {})

        def which(name):
            return "/usr/bin/" + name if name in programs_on_path else None

        out = io.StringIO()
        patches = [mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, env, clear=True),
                   mock.patch.object(script.shutil, "which", which), mock.patch.object(script.subprocess, "run", self.fake_run)]
        if windows:
            # the old script read os.name, this one sys.platform: Windows either way
            patches += [mock.patch.object(sys, "platform", "win32"), mock.patch.object(os, "name", "nt")]
        code = 0
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(out))
            try:
                script.build()
            except SystemExit as exit:
                code = exit.code if isinstance(exit.code, int) else 1
        return code, out.getvalue()

    def test_emscripten6_on_windows_with_visual_studios_ninja(self):
        touch(self.vs_ninja)
        code, out = self.configure()
        self.assertEqual(code, 0, out)
        self.assertEqual(len(self.commands), 1, out)
        cmake = self.commands[0]
        self.assertEqual(cmake[0], "cmake", "CMake directly, no emcmake launcher: %s" % cmake)
        self.assertNotIn("emcmake", " ".join(cmake))
        self.assertIn("-DCMAKE_TOOLCHAIN_FILE=" + str(self.emscripten / "cmake/Modules/Platform/Emscripten.cmake").replace("\\", "/"), cmake)
        self.assertIn("-DCMAKE_CROSSCOMPILING_EMULATOR=" + str(self.node).replace("\\", "/"), cmake)
        generator = cmake[cmake.index("-G") + 1]
        self.assertEqual(generator, "Ninja")
        self.assertIn("-DCMAKE_MAKE_PROGRAM=" + str(self.vs_ninja).replace("\\", "/"), cmake)
        # the SDK's environment, as emsdk_env sets it
        self.assertEqual(self.envs[0].get("EMSDK"), str(self.emsdk))
        self.assertTrue(self.envs[0]["PATH"].startswith(str(self.emsdk)), self.envs[0]["PATH"])

    def test_ninja_on_path_wins(self):
        touch(self.vs_ninja)
        code, out = self.configure(programs_on_path=("cmake", "ninja"))
        self.assertEqual(code, 0, out)
        cmake = self.commands[0]
        self.assertEqual(cmake[cmake.index("-G") + 1], "Ninja")
        self.assertFalse(any(arg.startswith("-DCMAKE_MAKE_PROGRAM=") for arg in cmake), cmake)

    def test_mingw_make_without_any_ninja(self):
        code, out = self.configure(programs_on_path=("cmake", "mingw32-make"))
        self.assertEqual(code, 0, out)
        cmake = self.commands[0]
        self.assertEqual(cmake[cmake.index("-G") + 1], "MinGW Makefiles")

    def test_no_generator_names_what_to_install(self):
        code, out = self.configure()
        self.assertEqual(code, 1)
        self.assertIn("Ninja", out)
        self.assertEqual(self.commands, [])

    def test_a_configured_directory_keeps_its_generator(self):
        touch(self.build / "CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n")
        code, out = self.configure()
        self.assertEqual(code, 0, out)
        self.assertNotIn("-G", self.commands[0])

    def test_dry_run_prints_and_runs_nothing(self):
        touch(self.vs_ninja)
        code, out = self.configure(extra_args=("--dry-run",))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.commands, [])
        self.assertIn("> cmake ", out)
        self.assertIn("-DCMAKE_TOOLCHAIN_FILE=", out)
        self.assertIn("with EMSDK=" + str(self.emsdk), out)

    def test_linux_web_build_uses_the_toolchain_file(self):
        (self.emscripten / "emcc.exe").unlink()
        touch(self.emscripten / "emcc")
        code, out = self.configure(programs_on_path=("cmake", "ninja"), windows=False)
        self.assertEqual(code, 0, out)
        cmake = self.commands[0]
        self.assertEqual(cmake[0], "cmake")
        self.assertTrue(any(arg.startswith("-DCMAKE_TOOLCHAIN_FILE=") for arg in cmake), cmake)
        self.assertEqual(cmake[cmake.index("-G") + 1], "Ninja")

    def test_cmake_generator_from_the_environment(self):
        touch(self.vs_ninja)
        code, out = self.configure(extra_env={"CMAKE_GENERATOR": "Ninja Multi-Config"})
        self.assertEqual(code, 0, out)
        cmake = self.commands[0]
        self.assertNotIn("-G", cmake, "CMake reads CMAKE_GENERATOR itself")
        self.assertFalse(any(arg.startswith("-DCMAKE_MAKE_PROGRAM=") for arg in cmake), cmake)

    def test_node_emsdk_env_first(self):
        chosen = touch(self.root / "chosen" / "node.exe")
        touch(self.vs_ninja)
        code, out = self.configure(extra_env={"EMSDK_NODE": str(chosen)})
        self.assertEqual(code, 0, out)
        self.assertIn("-DCMAKE_CROSSCOMPILING_EMULATOR=" + str(chosen).replace("\\", "/"), self.commands[0])

    def test_node_from_the_config_before_the_emsdk_newest(self):
        configured = touch(self.root / "configured" / "node.exe")
        touch(self.emsdk / ".emscripten", "NODE_JS = '%s'\n" % configured)
        touch(self.vs_ninja)
        code, out = self.configure()
        self.assertEqual(code, 0, out)
        self.assertIn("-DCMAKE_CROSSCOMPILING_EMULATOR=" + str(configured).replace("\\", "/"), self.commands[0])

    def test_visual_studio_builds_one_project_at_a_time(self):
        # red before platform-build: --parallel <jobs> is MSBuild's /m (projects at once) beside
        # cl's /MP<jobs> in each, up to jobs x jobs compilers; the configure passed no job cap
        touch(self.build / "CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n")
        code, out = self.configure(platform="windows", build=True, extra_args=("--jobs", "12"))
        self.assertEqual(code, 0, out)
        configure, build = self.commands
        self.assertIn("-DJASPERENGINE_PARALLEL_BUILD_JOBS=12", configure)
        self.assertEqual(build[build.index("--parallel") + 1], "1", build)
        # Ninja schedules files itself: the cap is its -j
        self.commands.clear()
        touch(self.build / "CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n")
        code, out = self.configure(platform="windows", build=True, extra_args=("--jobs", "12"))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.commands[1][self.commands[1].index("--parallel") + 1], "12", self.commands[1])

    def test_no_emscripten(self):
        for name in ("emcc.exe",):
            (self.emscripten / name).unlink()
        code, out = self.configure(programs_on_path=("cmake", "ninja"))
        self.assertEqual(code, 1)
        self.assertIn("Emscripten not found", out)


if __name__ == "__main__":
    unittest.main()
