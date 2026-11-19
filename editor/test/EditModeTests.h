// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Edit Mode cases (docs/specs/project-test-runner.md R15): each file in a sandboxed editor
// state (S23's: the math classes and an Editor table), where Editor.call runs only the tools
// annotated read-only; any other tool raises the read-only error, a wait raises. No Play session
// starts; the cases run synchronously.

#include "test/TestTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    namespace mcp { class McpCall; }
}

namespace jasperengine::editor::gametests {

    inline constexpr const char* EDIT_MODE_READ_ONLY = "Edit Mode tests are read-only: %s edits the project; test it in a Play Mode case";

    // The cases, in order; Editor.call's tools run nested in `parent` (the run_tests call)
    std::vector<gametest::CaseResult> runEditModeCases(const std::shared_ptr<mcp::McpCall>& parent, Project* project,
                                                       const std::filesystem::path& root, const gametest::Settings& settings,
                                                       const gametest::Target& target, const std::vector<gametest::CaseSpec>& cases);

}
