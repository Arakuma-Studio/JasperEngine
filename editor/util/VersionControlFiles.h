// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <set>
#include <string>

// The project's generated .gitignore and .gitattributes (Project Settings > Editor > Version
// Control Files). The editor owns the lines from the first-line marker down to the end marker;
// the lines below the end marker are the user's and are kept as they are.
namespace jasperengine::editor::vcsfiles {

    enum class FileKind { Ignore, Attributes };

    // the first line of a file the editor refreshes; a file without it is the user's
    extern const char* const startMarker;
    // the line after the editor's block
    extern const char* const endMarker;

    // the editor's block of each file, from the start marker, without the end marker
    std::string ignoreBlock();
    std::string attributesBlock(bool lfs, bool lockScenes);

    // Every line the editor wrote into the file in any version (each Git LFS and Lockable Scenes
    // variant, earlier templates), whitespace-normalised: what a file written before the end
    // marker existed is cleaned of
    const std::set<std::string>& knownGeneratedLines(FileKind kind);

    enum class Refresh { Write, LeaveAlone };

    // The file's next text in out. existing: its text, nullopt when there is none. LeaveAlone
    // for a file without the start marker (out untouched).
    Refresh refresh(const std::optional<std::string>& existing, const std::string& block, FileKind kind, std::string& out);

}
