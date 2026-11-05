// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The file operations of an export: one walker for every tree it copies, copies and writes that
// report their failure, and the folders it must never ship (IDE output, earlier exports).
// docs/superpowers/specs/2026-11-04-export-pipeline-design.md 6.1

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace jasperengine::editor::exportfiles {

    namespace fs = std::filesystem;

    struct WalkResult {
        bool ok = true;
        std::string error;
        // folders that could not be opened for permission reasons, skipped
        std::vector<fs::path> unreadable;
    };

    enum class DirAction { Descend, Skip };

    using DirFilter = std::function<DirAction(const fs::directory_entry& entry, const fs::path& rel)>;
    // false stops the walk (the visitor keeps its own error)
    using Visitor = std::function<bool(const fs::directory_entry& entry, const fs::path& rel)>;

    // permission_denied: the folder is skipped and reported; any other error stops the walk
    bool isSkippableOpenError(const std::error_code& ec);

    // Every regular file below `root` (and every directory, which `filter` decides on before it is
    // entered), depth first. A link to a file is visited as that file; a link to a folder is not
    // entered (the project's linked folders are walked by Exporter::walkTree, S68). `rel` is
    // relative to root; an entry with an empty relative path is skipped. A root that is not a
    // directory fails.
    WalkResult walk(const fs::path& root, const DirFilter& filter, const Visitor& visit);

    // copy_file (overwriting) with the parent folders made; false with "Failed to copy <from> to
    // <to>: <reason>" in error
    bool copyFile(const fs::path& from, const fs::path& to, std::string& error);
    // FileUtils::writeFile (unchanged content is left alone) with the parent folders made; false
    // with "Failed to write <path>" in error
    bool writeFile(const fs::path& path, const std::string& content, std::string& error);

    // What Android Studio, Gradle, Xcode and Finder leave in a workspace
    bool isIdeOutput(const fs::path& path);

    // The file that makes `dir` an export's folder ("" when none): jasperengine.py (a source
    // export), content-manifest.json (every player build), <app>.wasm or the prebuilt web player,
    // <app> or <app>.exe (a regular file), <app>.app (a macOS bundle)
    std::string exportFolderMarker(const fs::path& dir, const std::string& appName);

    // Mirrors `src` into `dst`: unchanged files keep their time (an incremental build over the
    // tree recompiles nothing), what `src` no longer has is removed. With keepIdeOutput, IDE
    // output is neither copied from src nor removed from dst (the user's IDE state).
    bool mirrorTree(const fs::path& src, const fs::path& dst, bool keepIdeOutput, std::string& error);

}
