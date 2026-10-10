// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "htmlui/compat/ImportManifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    namespace compat = jasperengine::htmlui::compat;

    // Writes assets/htmlui-imports/manifest.json (phase-2 §3.13.2), the file the runtime's
    // compat::ImportManifest reads: version 1, entries sorted by URL, written atomically
    // (temporary file + rename). Every change is a read-modify-write of the file on disk under
    // one process-wide lock, so the auto-import worker, the CLI and MCP never lose an entry.
    // Entries are never removed automatically (remove() is the Inspector's "Remove unused").
    class ImportManifestWriter {
    public:
        explicit ImportManifestWriter(std::filesystem::path assetsDir);

        std::filesystem::path path() const;                 // <assets>/htmlui-imports/manifest.json
        // the manifest on disk; empty when the file is missing or not a version-1 manifest
        compat::ImportManifest read() const;

        // adds the entries (an entry with the same normalized URL is replaced)
        bool add(const std::vector<compat::ImportEntry>& entries, std::string* error = nullptr);
        // removes the entries of these URLs (normalized before the lookup)
        bool remove(const std::vector<std::string>& urls, std::string* error = nullptr);

        static std::string toJson(const compat::ImportManifest& manifest);

        // Writes `content` to `file` through a temporary file in the same directory and a
        // rename, creating the parent directories (every file the importers write)
        static bool writeFileAtomic(const std::filesystem::path& file, const std::string& content, std::string* error = nullptr);
        // ISO-8601 UTC, seconds ("2026-10-07T12:00:00Z")
        static std::string nowIso8601();

    private:
        bool write(const compat::ImportManifest& manifest, std::string* error);

        std::filesystem::path assetsDir;
    };

}
