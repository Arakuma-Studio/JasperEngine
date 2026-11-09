// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Art Cleanup's entries outside its window (docs/specs/S46-pixel-art-cleanup.md R32, R43):
// what a file says about being a cleanup output, cached by its stamp and its source's, so the
// Project window and the Inspector can ask every refresh without reading or hashing the same files
// again. Header-only, the SpriteNormalsEntry.h pattern.

#pragma once

#include "pixelart/cleanup/CleanupSession.h"
#include "util/FileUtils.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>

namespace jasperengine::editor::cleanupentry {

    namespace fs = std::filesystem;

    struct OutputStatus {
        bool isOutput = false;      // it carries a cleanup recipe
        bool sourceChanged = false;
        bool sourceMissing = false;
        std::string source;         // as recorded
    };

    inline OutputStatus outputStatus(const fs::path& projectDir, const fs::path& output) {
        struct Entry {
            fs::file_time_type outputTime{}, sourceTime{};
            uintmax_t outputSize = 0, sourceSize = 0;
            fs::path source;
            OutputStatus status;
        };
        static std::unordered_map<std::string, Entry> cache;
        std::error_code ec;
        const fs::file_time_type outputTime = fs::last_write_time(output, ec);
        if (ec) return OutputStatus();
        const uintmax_t outputSize = fs::file_size(output, ec);
        if (ec) return OutputStatus();
        const std::string key = FileUtils::pathToGenericUtf8(output.lexically_normal());
        auto it = cache.find(key);
        if (it != cache.end() && it->second.outputTime == outputTime && it->second.outputSize == outputSize) {
            std::error_code sourceError;
            const fs::file_time_type sourceTime = fs::last_write_time(it->second.source, sourceError);
            const uintmax_t sourceSize = sourceError ? 0 : fs::file_size(it->second.source, sourceError);
            if (!sourceError && sourceTime == it->second.sourceTime && sourceSize == it->second.sourceSize) return it->second.status;
            if (sourceError && it->second.status.sourceMissing) return it->second.status;
        }
        Entry entry;
        entry.outputTime = outputTime;
        entry.outputSize = outputSize;
        const pixelart::cleanup::Staleness staleness = pixelart::cleanup::checkStaleness(projectDir, output);
        using State = pixelart::cleanup::Staleness::State;
        if (staleness.state == State::Fresh || staleness.state == State::Changed || staleness.state == State::Missing) {
            entry.status.isOutput = true;
            entry.status.source = staleness.sourcePath;
            entry.status.sourceChanged = staleness.state == State::Changed;
            entry.status.sourceMissing = staleness.state == State::Missing;
            entry.source = staleness.sourceAbsolute;
            std::error_code sourceError;
            entry.sourceTime = fs::last_write_time(entry.source, sourceError);
            entry.sourceSize = sourceError ? 0 : fs::file_size(entry.source, sourceError);
        }
        if (cache.size() > 20000) cache.clear();
        cache[key] = entry;
        return entry.status;
    }

    inline std::string sourceFileName(const OutputStatus& status) {
        return FileUtils::pathToUtf8(fs::u8path(status.source).filename());
    }

    // The Project window's tooltip text for a cleanup output
    inline std::string generatedText(const OutputStatus& status) {
        std::string text = "Cleaned from " + sourceFileName(status) + " (Pixel Art Cleanup)";
        if (status.sourceChanged) text += "\nSource changed: right-click > Re-run Pixel Art Cleanup";
        if (status.sourceMissing) text += "\nThe source " + sourceFileName(status) + " is missing";
        return text;
    }

}
