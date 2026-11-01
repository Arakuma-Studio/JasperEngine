// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Project Hub's name and path rules (docs/specs/S27-editor-tools.md R20, R23, R34): template
// ids, relative paths of template and archive entries, the Git URLs an import accepts, and the
// folder names of new projects.

#include <filesystem>
#include <string>
#include <string_view>

namespace jasperengine::editor::hub {

    // OpenEngine's ProjectCatalog rules (HubPaths.cpp): a plain slug of at most 64 characters
    bool isValidCatalogId(const std::string& id);
    // a relative path inside a folder we own: no traversal, no absolute or drive form, no '\\',
    // no '-' first (a git flag lookalike)
    bool isSafeRelativePath(const std::string& path);

    // a name as a template id ("My Racing Game" -> "my-racing-game"); empty when nothing is left
    std::string slugify(std::string_view name);

    // An archive entry's name kept (R34): relative, no drive letter, no "..", no '\\'; false with
    // the reason otherwise. Directory entries end in '/'.
    bool isSafeArchiveEntry(std::string_view name, std::string& why);

    // The Git URLs an import accepts (R23): https://, ssh:// and scp-like user@host:path; never
    // anything starting with '-', ext:: or file://. False with the reason otherwise.
    bool isAcceptedGitUrl(std::string_view url, std::string& why);

    // A project folder name from a project name: what a file system holds everywhere (no
    // separators, no reserved characters, no trailing dot or space); empty when nothing is left
    std::string folderName(std::string_view projectName);

    // why a new project cannot go to `dir` ("" when it can: absent or an empty folder)
    std::string targetProblem(const std::filesystem::path& dir);

}
