// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sheet page images uploaded for ImGui: the Sprite Atlas inspector's pages and the Sprite
// inspector's frame picker (docs/specs/S45-sprite-atlas.md R29, R30)

#pragma once

#include "pixelart/PixelImage.h"
#include "texture/Texture.h"

#include <filesystem>
#include <map>
#include <string>

namespace jasperengine::editor {

    class Project;

    // Image files uploaded once (by path and modification time) for ImGui, nearest-filtered
    class SheetPreviewTextures {
    public:
        struct Image {
            Texture texture;
            int width = 0, height = 0;
            std::filesystem::file_time_type time;
            bool valid() const { return width > 0 && !texture.empty(); }
        };
        ~SheetPreviewTextures();
        // a file (absolute, or relative to the assets folder); null when it does not read
        Image* get(Project* project, const std::string& path);
        Image* fromImage(const std::string& key, const pixelart::PixelImage& image);
        void clear();

    private:
        std::map<std::string, Image> images;
    };

}
