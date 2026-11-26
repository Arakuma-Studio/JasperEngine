// Stand-in for the project test runner switch. CMake builds generate the real one from
// JASPERENGINE_TEST_RUNNER (engine/CMakeLists.txt), which takes precedence over this file.
// The Xcode workspace has no configure step and compiles no runner sources
// (tools/xcode_sync_sources.py), so its players have the runner off, as release players do.

#pragma once

#define JASPERENGINE_TEST_RUNNER 0
