// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A directory as a release records it (docs/specs/S30-shipping.md R23, R24, D11): every file
// with its sha256 and size, every symlink with its target, by '/'-separated relative path; with
// the problems that make a tree unshippable (names that collide when case-folded, symlinks that
// point outside it or are absolute, any symlink where the target OS has none). Portions adapted
// from OpenEngine Tools/Scripts/publish_game.py (MIT), tree_manifest().

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::publish {

    enum class TargetOs { Windows, Linux, MacOS, Android, IOS, Web };

    struct TreeEntry {
        std::string sha256;
        uint64_t size = 0;
        std::string symlink;   // a symlink's target (relative), the entry has no hash then
    };

    struct TreeManifestResult {
        std::map<std::string, TreeEntry> entries;
        std::vector<std::string> problems;
        bool ok() const { return problems.empty(); }
    };

    TreeManifestResult treeManifest(const std::filesystem::path& root, TargetOs os);

}
