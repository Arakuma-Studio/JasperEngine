// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureLabels.h"

#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "util/FileUtils.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TextureSlotLabel jasperengine::editor::textureSlotLabel(const Texture& texture, const std::function<std::string(const std::string&)>& pathShown) {
    TextureSlotLabel label;
    auto shown = [&](const std::string& path) { return pathShown ? pathShown(path) : path; };
    auto fileName = [](const std::string& path) {
        // a legacy "<path>?svgScale=N" names the same file
        return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(TextureData::parseSvgScalePath(path)).filename());
    };
    const std::string first = texture.getPath(0);
    if (!first.empty()) {
        if (texture.getNumFaces() == 6) {
            label.text = fileName(first) + " (Cubemap)";
            for (size_t face = 0; face < 6; face++) {
                if (face > 0) label.tooltip += "\n";
                label.tooltip += shown(texture.getPath(face));
            }
        } else {
            label.text = fileName(first);
            label.tooltip = shown(first);
        }
        return label;
    }
    if (!texture.getId().empty()) {
        label.text = texture.getId();
        label.tooltip = texture.getId();
        return label;
    }
    label.text = "< Not set >";
    return label;
}
