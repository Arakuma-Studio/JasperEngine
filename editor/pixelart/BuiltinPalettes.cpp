// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// palettes_builtin.h is generated from editor/resources/palettes by CMakeLists.txt; this is its
// only includer.

#include "BuiltinPalettes.h"

#include "Log.h"

#include "palettes_builtin.h"

#include <algorithm>

using namespace jasperengine;

namespace jasperengine::editor::pixelart::builtinio {
namespace {

    std::string_view stripPrefix(std::string_view id) {
        const std::string_view prefix = kBuiltinPrefix;
        if (id.size() >= prefix.size() && id.compare(0, prefix.size(), prefix) == 0) id.remove_prefix(prefix.size());
        return id;
    }

    std::string_view idOf(const BuiltinPaletteEmbeddedFile& file) {
        std::string_view name = file.name;
        const std::string_view extension = ".palette";
        if (name.size() > extension.size() && name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
            name.remove_suffix(extension.size());
        }
        return name;
    }

}
}

const std::vector<editor::pixelart::BuiltinPalette>& editor::pixelart::builtinPalettes() {
    static const std::vector<BuiltinPalette> palettes = []() {
        std::vector<BuiltinPalette> list;
        for (const BuiltinPaletteEmbeddedFile& file : builtin_palette_embedded_files) {
            if (!file.name) continue;   // the terminator
            BuiltinPalette entry;
            entry.id = std::string(builtinio::idOf(file));
            std::vector<jasperengine::pixelart::Diagnostic> diagnostics;
            const std::string_view text(reinterpret_cast<const char*>(file.data), file.size);
            if (!jasperengine::pixelart::PaletteCodec::read(text, entry.palette, diagnostics)) {
                // the unit tests read every built-in; a broken one is left out rather than shown
                for (const jasperengine::pixelart::Diagnostic& diagnostic : diagnostics) {
                    Log::error("[Palette] built-in %s: %s: %s", file.name, diagnostic.field.c_str(), diagnostic.message.c_str());
                }
                continue;
            }
            list.push_back(std::move(entry));
        }
        std::sort(list.begin(), list.end(), [](const BuiltinPalette& a, const BuiltinPalette& b) { return a.id < b.id; });
        return list;
    }();
    return palettes;
}

const editor::pixelart::BuiltinPalette* editor::pixelart::findBuiltinPalette(std::string_view id) {
    const std::string_view wanted = builtinio::stripPrefix(id);
    const std::vector<BuiltinPalette>& palettes = builtinPalettes();
    auto it = std::lower_bound(palettes.begin(), palettes.end(), wanted,
                               [](const BuiltinPalette& palette, std::string_view key) { return std::string_view(palette.id) < key; });
    return it != palettes.end() && it->id == wanted ? &*it : nullptr;
}

std::string_view editor::pixelart::builtinPaletteText(std::string_view id) {
    const std::string_view wanted = builtinio::stripPrefix(id);
    for (const BuiltinPaletteEmbeddedFile& file : builtin_palette_embedded_files) {
        if (file.name && builtinio::idOf(file) == wanted) return std::string_view(reinterpret_cast<const char*>(file.data), file.size);
    }
    return {};
}
