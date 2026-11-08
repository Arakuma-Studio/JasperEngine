// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteAtlasCook.h"
#include "SpriteAtlasBuilder.h"

#include <algorithm>
#include <string>

namespace jasperengine::editor::sprite {

    TextureCookInputs atlasPageCookInputs(const AtlasBuildResult& result, size_t page) {
        TextureCookInputs in;
        const std::string key = "image#" + std::to_string(page);
        for (const AtlasBuildResult::ExportedFile& f : result.exportedFiles(false)) {
            if (f.key == key) in.assetPath = f.exportPath;
        }
        in.projectRelative = result.atlasPath + " (" + key + ")";
        in.absolute = result.pageFile(page);
        in.settings.type = TextureImportType::Sprite;
        in.settings.inferredFrom = result.atlasPath;
        if (result.compression == "lossless") {
            const AtlasPageRow& row = result.pages[page];
            in.settings.quality = TextureCookQuality::None;
            in.settings.mipmaps = false;
            in.settings.alphaIsTransparency = false;
            in.settings.preserveCoverage = false;
            in.settings.maxSize = std::max(row.width, row.height);
        }
        in.hasSettings = true;
        in.inference.type = TextureImportType::Sprite;
        in.inference.reason = result.atlasPath;
        return in;
    }

}
