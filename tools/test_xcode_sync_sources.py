"""Tests of tools/xcode_sync_sources.py (python3 -I tools/test_xcode_sync_sources.py): the CMake
evaluation (platform branches, includes, globs, filters, flags after add_library, unknown variables
and commands), the project.pbxproj parser and its structure check, the comparison, and a sync of a
small project that a second run leaves alone. The last case evaluates the repository's own CMake."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xcode_sync_sources as xs  # noqa: E402

CMAKE = """\
option(JASPERENGINE_EARLY "before the evaluated range: never read" ON)
set(JASPERENGINE_SRCS
    core/A.cpp
    core/Gone.cpp  # a comment with a ) in it
)
if(EMSCRIPTEN)
    list(APPEND JASPERENGINE_SRCS platform/web/W.cpp)
elseif(APPLE)
    list(APPEND JASPERENGINE_SRCS platform/apple/P.mm platform/apple/Q.mm)
else()
    set(LINUX_ONLY core/Never.cpp)
    list(APPEND JASPERENGINE_SRCS core/Linux.cpp)
endif()
# assigned by this code, so unset it reads as empty
if(LINUX_ONLY)
    list(APPEND JASPERENGINE_SRCS ${LINUX_ONLY})
endif()
include(core/mod/ModSources.cmake)
if(JASPERENGINE_MOD AND NOT (MSVC OR CMAKE_SYSTEM_NAME STREQUAL "Android"))
    list(APPEND JASPERENGINE_SRCS ${MOD_SRCS})
endif()
foreach(n 1 2)
    list(APPEND JASPERENGINE_SRCS core/Loop${n}.cpp)
endforeach()
list(FILTER JASPERENGINE_SRCS EXCLUDE REGEX "^core/Gone[.]cpp$")
function(helper out)
    this_is_never_run()
endfunction()
add_library(jasperengine ${JASPERENGINE_LIB_TYPE} ${JASPERENGINE_SRCS})
target_link_libraries(jasperengine PRIVATE whatever)
if(SOMETHING_NOBODY_SET)
    target_link_libraries(jasperengine PRIVATE skipped)
endif()
if(MSVC)
    set(FLAGS /fp:precise)
else()
    set(FLAGS -ffp-contract=off)
endif()
set_source_files_properties(core/A.cpp PROPERTIES COMPILE_OPTIONS "${FLAGS}")
"""

MOD_CMAKE = """\
file(GLOB MOD_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
set_source_files_properties(${MOD_SRCS} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-math-errno")
"""

VARIABLES = {"APPLE": "1", "EMSCRIPTEN": None, "MSVC": None, "CMAKE_SYSTEM_NAME": "Darwin",
             "JASPERENGINE_LIB_TYPE": "STATIC", "JASPERENGINE_MOD": "ON"}

# a project with the two engine targets: A.cpp compiled by both (with the flag only in macos), Gone.cpp
# (CMake drops it) in macos, P.mm twice in ios; core/mod/, the loop's sources and Q.mm missing
PBXPROJ = """\
// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {
	};
	objectVersion = 54;
	objects = {

/* Begin PBXBuildFile section */
		B00000000000000000000001 /* A.cpp in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000001 /* A.cpp */; settings = {COMPILER_FLAGS = "-ffp-contract=off"; }; };
		B00000000000000000000002 /* A.cpp in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000001 /* A.cpp */; };
		B00000000000000000000003 /* Gone.cpp in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000002 /* Gone.cpp */; };
		B00000000000000000000004 /* P.mm in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000003 /* P.mm */; };
		B00000000000000000000005 /* P.mm in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000003 /* P.mm */; };
		B00000000000000000000006 /* P.mm in Sources */ = {isa = PBXBuildFile; fileRef = F00000000000000000000004 /* P.mm */; };
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		F00000000000000000000001 /* A.cpp */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.cpp; path = A.cpp; sourceTree = "<group>"; };
		F00000000000000000000002 /* Gone.cpp */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.cpp; path = Gone.cpp; sourceTree = "<group>"; };
		F00000000000000000000003 /* P.mm */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.objcpp; path = platform/apple/P.mm; sourceTree = "<group>"; };
		F00000000000000000000004 /* P.mm */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.objcpp; path = ../../platform/apple/P.mm; sourceTree = SOURCE_ROOT; };
/* End PBXFileReference section */

/* Begin PBXGroup section */
		A00000000000000000000001 = {
			isa = PBXGroup;
			children = (
				A00000000000000000000002 /* engine */,
				F00000000000000000000004 /* P.mm */,
			);
			sourceTree = "<group>";
		};
		A00000000000000000000002 /* engine */ = {
			isa = PBXGroup;
			children = (
				A00000000000000000000003 /* core */,
				F00000000000000000000003 /* P.mm */,
			);
			path = ../..;
			sourceTree = "<group>";
		};
		A00000000000000000000003 /* core */ = {
			isa = PBXGroup;
			children = (
				F00000000000000000000001 /* A.cpp */,
				F00000000000000000000002 /* Gone.cpp */,
			);
			path = core;
			sourceTree = "<group>";
		};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		C00000000000000000000001 /* jasperengine-macos */ = {
			isa = PBXNativeTarget;
			buildPhases = (
				D00000000000000000000001 /* Sources */,
			);
			name = "jasperengine-macos";
		};
		C00000000000000000000002 /* jasperengine-ios */ = {
			isa = PBXNativeTarget;
			buildPhases = (
				D00000000000000000000002 /* Sources */,
			);
			name = "jasperengine-ios";
		};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		E00000000000000000000001 /* Project object */ = {
			isa = PBXProject;
			mainGroup = A00000000000000000000001;
			projectDirPath = "";
			targets = (
				C00000000000000000000001 /* jasperengine-macos */,
				C00000000000000000000002 /* jasperengine-ios */,
			);
		};
/* End PBXProject section */

/* Begin PBXSourcesBuildPhase section */
		D00000000000000000000001 /* Sources */ = {
			isa = PBXSourcesBuildPhase;
			files = (
				B00000000000000000000001 /* A.cpp in Sources */,
				B00000000000000000000003 /* Gone.cpp in Sources */,
				B00000000000000000000004 /* P.mm in Sources */,
			);
		};
		D00000000000000000000002 /* Sources */ = {
			isa = PBXSourcesBuildPhase;
			files = (
				B00000000000000000000002 /* A.cpp in Sources */,
				B00000000000000000000005 /* P.mm in Sources */,
				B00000000000000000000006 /* P.mm in Sources */,
			);
		};
/* End PBXSourcesBuildPhase section */
	};
	rootObject = E00000000000000000000001 /* Project object */;
}
"""

TARGETS = [("jasperengine-macos", {"CMAKE_SYSTEM_NAME": "Darwin"}), ("jasperengine-ios", {"CMAKE_SYSTEM_NAME": "iOS"})]
WANTED = ["core/A.cpp", "platform/apple/P.mm", "platform/apple/Q.mm", "core/mod/M1.cpp", "core/mod/M2.cpp", "core/Loop1.cpp", "core/Loop2.cpp"]


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.engine = Path(self.tmp.name) / "engine"
        for rel in ("core/A.cpp", "core/Gone.cpp", "core/Loop1.cpp", "core/Loop2.cpp", "platform/apple/P.mm", "platform/apple/Q.mm",
                    "core/mod/M1.cpp", "core/mod/M2.cpp", "core/mod/notes.txt"):
            (self.engine / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.engine / rel).write_text("")
        (self.engine / "CMakeLists.txt").write_text(CMAKE)
        (self.engine / "core/mod/ModSources.cmake").write_text(MOD_CMAKE)
        self.project_dir = str(self.engine / "workspaces" / "xcode")
        os.makedirs(self.project_dir)

    def tearDown(self):
        self.tmp.cleanup()

    def sources(self, cmake=None, **overrides):
        if cmake is not None:
            (self.engine / "CMakeLists.txt").write_text(cmake)
        variables = dict(VARIABLES, **overrides)
        return xs.cmake_target_sources(str(self.engine), variables, "jasperengine", first="JASPERENGINE_SRCS")


class CMakeEvaluation(Fixture):
    def test_apple_branch_include_glob_filter_and_flags(self):
        sources, contracted = self.sources()
        self.assertEqual(sources, WANTED)
        # the flag set after add_library (in an else branch) and the module's, a list of flags
        self.assertEqual(contracted, {"core/A.cpp", "core/mod/M1.cpp", "core/mod/M2.cpp"})

    def test_other_platform_and_module_off(self):
        sources, _ = self.sources(APPLE=None, EMSCRIPTEN="1", JASPERENGINE_MOD="OFF")
        self.assertEqual(sources, ["core/A.cpp", "platform/web/W.cpp", "core/Loop1.cpp", "core/Loop2.cpp"])

    def test_a_variable_nobody_gave_stops_it(self):
        variables = dict(VARIABLES)
        del variables["JASPERENGINE_MOD"]
        with self.assertRaisesRegex(xs.SyncError, "JASPERENGINE_MOD is read but not set"):
            xs.cmake_target_sources(str(self.engine), variables, "jasperengine", first="JASPERENGINE_SRCS")
        with self.assertRaisesRegex(xs.SyncError, r"CMakeLists.txt:\d+: UNSET_DIR is read but not set"):
            self.sources(CMAKE.replace("core/A.cpp\n", "${UNSET_DIR}/A.cpp\n", 1))

    def test_an_unmodelled_command_stops_it(self):
        with self.assertRaisesRegex(xs.SyncError, "string\\(\\) is not modelled"):
            self.sources(CMAKE.replace("include(core/mod", "string(TOUPPER x y)\ninclude(core/mod"))
        with self.assertRaisesRegex(xs.SyncError, "target_sources\\(jasperengine\\) adds a source"):
            self.sources(CMAKE + "target_sources(jasperengine PRIVATE core/Late.cpp)\n")
        with self.assertRaisesRegex(xs.SyncError, "CMake stops here: no"):
            self.sources(CMAKE.replace("foreach(n 1 2)", 'message(FATAL_ERROR "no")\nforeach(n 1 2)'))

    def test_a_block_with_a_flag_after_add_library_is_evaluated(self):
        # (a block holding only target commands is skipped unread, SOMETHING_NOBODY_SET above)
        with self.assertRaisesRegex(xs.SyncError, "OTHER_SWITCH is read but not set"):
            self.sources(CMAKE + "if(OTHER_SWITCH)\n    set_source_files_properties(core/A.cpp PROPERTIES COMPILE_OPTIONS -O0)\nendif()\n")
        _, contracted = self.sources(CMAKE + "set_source_files_properties(core/A.cpp PROPERTIES COMPILE_OPTIONS -Os)\n")
        self.assertNotIn("core/A.cpp", contracted)


class Pbxproj(unittest.TestCase):
    def test_parses_and_is_consistent(self):
        root = xs.parse_pbxproj(PBXPROJ)
        self.assertEqual(len(root["objects"]), 18)
        self.assertEqual(root["objects"]["B00000000000000000000001"]["settings"]["COMPILER_FLAGS"], "-ffp-contract=off")
        self.assertEqual(xs.structure_errors(root), [])

    def test_syntax_errors(self):
        for broken, message in ((PBXPROJ.replace("\t\t};\n/* End PBXGroup", "\n/* End PBXGroup", 1), "expected"),
                                (PBXPROJ.replace("path = core;", "path = core"), "expected ';'"),
                                (PBXPROJ.replace("B00000000000000000000002 /* A.cpp in Sources */ = ",
                                                 "B00000000000000000000001 /* A.cpp in Sources */ = "), "defined twice"),
                                (PBXPROJ[:-3], "end of file")):
            with self.assertRaisesRegex(xs.SyncError, message):
                xs.parse_pbxproj(broken)

    def test_structure_errors(self):
        dangling = PBXPROJ.replace("fileRef = F00000000000000000000002", "fileRef = F0000000000000000000000F")
        self.assertEqual(xs.structure_errors(xs.parse_pbxproj(dangling)),
                         ["B00000000000000000000003.fileRef names F0000000000000000000000F, which is no object"])
        orphan = PBXPROJ.replace("\t\t\t\tB00000000000000000000003 /* Gone.cpp in Sources */,\n", "")
        self.assertEqual(xs.structure_errors(xs.parse_pbxproj(orphan)),
                         ["build file B00000000000000000000003 is in 0 build phases"])
        two_groups = PBXPROJ.replace("\t\t\t\tF00000000000000000000004 /* P.mm */,\n",
                                     "\t\t\t\tF00000000000000000000004 /* P.mm */,\n\t\t\t\tF00000000000000000000001 /* A.cpp */,\n")
        self.assertEqual(xs.structure_errors(xs.parse_pbxproj(two_groups)), ["F00000000000000000000001 is in 2 groups"])


class CompareAndSync(Fixture):
    def analyse(self, text):
        return xs.analyse(text, self.project_dir, str(self.engine), TARGETS, VARIABLES, [])

    def test_target_sources_resolve_group_and_source_root_paths(self):
        _, listed = xs.target_sources(xs.parse_pbxproj(PBXPROJ), self.project_dir, str(self.engine), "jasperengine-ios")
        self.assertEqual(listed, [("core/A.cpp", "B00000000000000000000002", ""),
                                  ("platform/apple/P.mm", "B00000000000000000000005", ""),
                                  ("platform/apple/P.mm", "B00000000000000000000006", "")])

    def test_compare(self):
        _, results = self.analyse(PBXPROJ)
        macos, ios = results[0][5], results[1][5]
        missing = ["core/Loop1.cpp", "core/Loop2.cpp", "core/mod/M1.cpp", "core/mod/M2.cpp", "platform/apple/Q.mm"]
        self.assertEqual(macos, {"missing": missing, "extra": ["core/Gone.cpp"], "duplicate": [],
                                 "flag_missing": [], "flag_extra": []})
        self.assertEqual(ios, {"missing": missing, "extra": [], "duplicate": ["platform/apple/P.mm"],
                               "flag_missing": ["core/A.cpp"], "flag_extra": []})
        lines = xs.report(results)
        self.assertIn("  core/Gone.cpp: compiled by jasperengine-macos, not by CMake", lines)
        self.assertIn("  core/A.cpp: without -ffp-contract=off in jasperengine-ios (CMake gives it)", lines)

    def test_sync_makes_them_agree_and_a_second_run_changes_nothing(self):
        _, results = self.analyse(PBXPROJ)
        text = xs.sync(PBXPROJ, self.project_dir, str(self.engine), results)
        root, again = self.analyse(text)
        self.assertEqual(xs.structure_errors(root), [])
        self.assertEqual(xs.report(again), [])
        self.assertEqual(xs.sync(text, self.project_dir, str(self.engine), again), text)
        objects = root["objects"]
        # core/mod got a group under core, one reference shared by both targets' build files
        mod = [o for o in objects.values() if o.get("isa") == "PBXGroup" and o.get("path") == "mod"]
        self.assertEqual(len(mod), 1)
        self.assertIn(mod[0], [objects[c] for c in objects["A00000000000000000000003"]["children"]])
        self.assertEqual(len(mod[0]["children"]), 2)
        m1 = [b for b in objects.values() if b.get("isa") == "PBXBuildFile" and b.get("fileRef") == mod[0]["children"][0]]
        self.assertEqual([b["settings"]["COMPILER_FLAGS"] for b in m1], ["-ffp-contract=off"] * 2)
        # Gone.cpp's build file is gone, its reference stays; the ios P.mm keeps one build file
        self.assertNotIn("B00000000000000000000003", objects)
        self.assertIn("F00000000000000000000002", objects)
        self.assertEqual(sum(1 for b in objects["D00000000000000000000002"]["files"]
                             if objects[b]["fileRef"] in ("F00000000000000000000003", "F00000000000000000000004")), 1)
        self.assertEqual(objects["B00000000000000000000002"]["settings"]["COMPILER_FLAGS"], "-ffp-contract=off")
        # Q.mm joins P.mm in the engine group (platform/apple has no group of its own), as Objective-C++
        q = [(oid, o) for oid, o in objects.items() if o.get("isa") == "PBXFileReference" and o["path"].endswith("Q.mm")]
        self.assertEqual([(o["path"], o["lastKnownFileType"]) for _, o in q], [("platform/apple/Q.mm", "sourcecode.cpp.objcpp")])
        self.assertIn(q[0][0], objects["A00000000000000000000002"]["children"])
        # the edits keep Xcode's layout: one object per line in the flat sections
        self.assertRegex(text, r"\n\t\t4A534E53[0-9A-F]{16} /\* M1.cpp \*/ = \{isa = PBXFileReference; [^\n]*\};\n")


class Repository(unittest.TestCase):
    def test_apple_choices_of_the_engine_cmake(self):
        for target, platform in xs.TARGETS:
            sources, contracted = xs.apple_sources(xs.ENGINE, dict(xs.APPLE, **xs.MODULES, **platform), xs.ENGINE_LIBRARIES)
            for wanted in ("platform/apple/HttpBackendApple.mm", "platform/apple/NativePlatformApple.mm",
                           "core/subsystem/TitleArtRuntimeNone.cpp", "core/tilemap/TileCells.cpp",
                           "core/object/sound/SoundHandle.cpp", "libs/recastnavigation/Detour/Source/DetourNavMesh.cpp",
                           "libs/msdfgen/core/msdfgen.cpp"):
                self.assertIn(wanted, sources, target)
            for unwanted in ("core/net/HttpBackendCurl.cpp", "core/native/NativePlatformNone.cpp",
                             "platform/linux/FFmpegBackend.cpp", "platform/win/HttpBackendWin.cpp"):
                self.assertNotIn(unwanted, sources, target)
            self.assertFalse([s for s in sources if s.startswith("core/titleart/")], target)
            self.assertIn("core/pixelart/PaletteSwap.cpp", contracted)
            self.assertIn("core/animation/AnimatorPose.cpp", contracted)
            self.assertNotIn("core/tilemap/TileCells.cpp", contracted)


if __name__ == "__main__":
    unittest.main()
