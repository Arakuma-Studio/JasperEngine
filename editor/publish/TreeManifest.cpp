// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine Tools/Scripts/publish_game.py (MIT):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors

#include "TreeManifest.h"

#include "util/Sha256.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor::publish;
namespace fs = std::filesystem;

namespace {

    std::string fold(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

}

TreeManifestResult publish::treeManifest(const fs::path& root, TargetOs os) {
    TreeManifestResult out;
    std::map<std::string, std::string> folded;
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::none, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path path = it->path();
        const std::string relative = fs::relative(path, root, ec).generic_string();
        const std::string key = fold(relative);
        if (auto clash = folded.find(key); clash != folded.end()) {
            out.problems.push_back("case-colliding paths: " + clash->second + " and " + relative);
        } else {
            folded.emplace(key, relative);
        }
        if (it->is_symlink(ec)) {
            it.disable_recursion_pending();
            const fs::path target = fs::read_symlink(path, ec);
            if (os == TargetOs::Windows || os == TargetOs::Web) {
                out.problems.push_back("symlink " + relative + ": a " + std::string(os == TargetOs::Web ? "web" : "Windows") +
                                       " build may hold no symlinks");
                continue;
            }
            if (target.is_absolute()) {
                out.problems.push_back("absolute symlink " + relative + " -> " + target.string());
                continue;
            }
            const fs::path resolved = fs::weakly_canonical(path.parent_path() / target, ec);
            const fs::path inside = resolved.lexically_relative(canonicalRoot);
            if (inside.empty() || *inside.begin() == "..") {
                out.problems.push_back("symlink " + relative + " -> " + target.generic_string() + " points outside the build");
                continue;
            }
            out.entries[relative] = {"", 0, target.generic_string()};
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        TreeEntry entry;
        std::string error;
        if (!sha256File(path, entry.sha256, &error, &entry.size)) {
            out.problems.push_back(error);
            continue;
        }
        out.entries[relative] = entry;
    }
    if (ec) out.problems.push_back("cannot list " + root.string() + ": " + ec.message());
    return out;
}
