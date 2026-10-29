// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The headless scene commands of version control (docs/specs/S29-version-control.md R18, R34),
// run before the backend starts (no window, no engine):
//
//   jasperengine-editor merge-scene <base> <ours> <theirs> <path>
//       git's merge driver (merge.jasperengine-scene.driver, R22): the result goes to <ours>;
//       exit 0 merged, 1 conflicts (ours kept for them, recorded in
//       <project>/.jasperengine/merge/<path>.conflicts.yaml), 2 an input does not parse (<ours>
//       untouched)
//   jasperengine-editor scene-diff <old> <new> [--json]
//       the semantic diff (usable as `git difftool -x`); exit 0, 2 when a file does not parse

#include <filesystem>
#include <string>

namespace jasperengine::editor::vcs::scene {

    int runMergeSceneCommand(int argc, char** argv, const char* executableName);
    int runSceneDiffCommand(int argc, char** argv, const char* executableName);

    // Where merge-scene records the conflicts of `path` (relative to `workTree`, the driver's
    // working directory): below the project holding it (the nearest folder up with a
    // project.yaml), else below the working tree
    std::filesystem::path conflictsFileFor(const std::filesystem::path& workTree, const std::string& path);

}
