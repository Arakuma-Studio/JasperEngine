// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The writes of the asset identity code (meta sidecars, in-file GUID lines, the asset
// index cache) and its renames, on S03's atomic writes (engine/core/io/AtomicFile.h): a
// temporary sibling `.<file>.<pid>-<n>.je-tmp` is written, synced for documents, then
// renamed over the target, so a crash leaves either the old or the new bytes. Renames retry
// while the file is briefly locked (antivirus, indexers).

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace jasperengine::editor::assetio {

    enum class Durability { Cache, Document };

    // Unchanged content is not written (the file and its mtime stay as they are) and counts
    // as success; *written tells which happened
    bool writeFile(const std::filesystem::path& target, std::string_view bytes, Durability durability,
                   std::string* error = nullptr, bool* written = nullptr);

    bool readFile(const std::filesystem::path& path, std::string& bytes);

    // Refuses to replace an existing destination
    bool renameFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error = nullptr);

    bool isTemporarySiblingName(std::string_view filename);

}
