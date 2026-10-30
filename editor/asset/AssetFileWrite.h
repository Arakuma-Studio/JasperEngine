// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The writes of the asset identity code (meta sidecars, in-file GUID lines, the asset
// index cache) and its renames, on S03's atomic writes (engine/core/io/AtomicFile.h): a
// temporary sibling `.<file>.<pid>-<n>.je-tmp` is written, synced for documents, then
// renamed over the target, so a crash leaves either the old or the new bytes. Renames retry
// while the file is briefly locked (antivirus, indexers).

#pragma once

#include <cstdint>
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

    // What a file was when it was read (taken before reading it)
    struct ReadStamp {
        bool exists = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        bool operator==(const ReadStamp&) const = default;
    };
    ReadStamp stampOf(const std::filesystem::path& path);

    // writeFile for an edit of the bytes read after `read` was taken (a GUID line put into a
    // clip or a prefab): when the file changed since (another program saved it meanwhile),
    // nothing is written and *superseded is set, so the caller reads it again and redoes its
    // edit instead of putting the old bytes back over that save
    bool rewriteFile(const std::filesystem::path& target, std::string_view bytes, Durability durability, const ReadStamp& read,
                     bool* superseded, std::string* error = nullptr);

    // Refuses to replace an existing destination
    bool renameFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error = nullptr);

    bool isTemporarySiblingName(std::string_view filename);

}
