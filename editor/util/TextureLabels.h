// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What an Inspector texture slot shows for a texture (project-window spec 6.10): the file's name,
// never the pool id ("ship.png", not "sprites/ship.png|alphaborder"); its path(s) in the tooltip

#include <functional>
#include <string>

namespace jasperengine {
    class Texture;
}

namespace jasperengine::editor {

    struct TextureSlotLabel {
        std::string text;
        std::string tooltip;
    };

    // `pathShown` turns a stored path into what the tooltip shows (the project-relative path);
    // null: the stored path. A texture with no file (generated, pooled) shows its id; none: "< Not set >"
    TextureSlotLabel textureSlotLabel(const Texture& texture, const std::function<std::string(const std::string&)>& pathShown = nullptr);

}
