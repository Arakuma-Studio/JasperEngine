// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The small YAML files S25 keeps per user (shortcuts.yaml and layouts/ in the config directory)
// and per user and project (.jasperengine/user/layout.yaml, organisation.yaml): read whole, and
// written with S03's atomic write under the file's own lock (<file>.lock), as settings.yaml is,
// because two editors may share them.

#include <filesystem>
#include <string>

namespace jasperengine::editor::userfiles {

    // False (text empty) when the file does not exist or cannot be read
    bool read(const std::filesystem::path& file, std::string& text);

    // Atomic, under the lock (lockFile, or <file>.lock when empty); unchanged content is not
    // rewritten. False with *error (the file keeps its previous bytes)
    bool write(const std::filesystem::path& file, const std::string& text, std::string* error = nullptr,
               const std::filesystem::path& lockFile = {});

    // Deletes the file (a layout preset) under the same lock: false with *error when it exists
    // and cannot be removed
    bool remove(const std::filesystem::path& file, std::string* error = nullptr, const std::filesystem::path& lockFile = {});

}
