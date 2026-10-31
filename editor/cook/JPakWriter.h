// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Writes .jpak v1 content packs (engine/core/io/JPak.h, docs/specs/S30-shipping.md D6): a
// directory's files in path order, each blob at a 16-byte boundary with its FNV-1a 64, through
// an atomic publish. Paths follow the legal-path rule and must stay unique when case-folded.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::cook {

    struct JPakStats {
        uint32_t entries = 0;
        uint64_t bytes = 0;
    };

    // every regular file below `root` except the `exclude`d ones (root-relative, '/'-separated)
    bool writeJPak(const std::filesystem::path& root, const std::filesystem::path& output, const std::vector<std::string>& exclude,
                   JPakStats& stats, std::string& error);

}
