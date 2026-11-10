// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/import/ImportJsonReader.h"

#include <cctype>
#include <filesystem>

namespace jasperengine::editor::tilemapimport::detail {

    std::string joinMapPath(const std::string& baseDir, const std::string& relative) {
        std::filesystem::path path(relative);
        if (!path.is_absolute() && !baseDir.empty()) path = std::filesystem::path(baseDir) / path;
        return path.lexically_normal().generic_string();
    }

    std::string normalizeColor(const std::string& text) {
        std::string hex = text;
        if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
        if (hex.size() != 6 && hex.size() != 8) return "#ffffff";
        for (char c : hex) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return "#ffffff";
        }
        std::string rgb = hex.size() == 8 ? hex.substr(2) : hex;
        for (char& c : rgb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return "#" + rgb;
    }

}
