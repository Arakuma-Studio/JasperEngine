// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .palette files (S43 R3, S05): the palette library reads the file again and raises its
// generation, so the Palette Swap textures (RenderSystem::updateShaderPalettes) and the Palette
// Quantize table made from it are built again on the next frame, and its ramp strips get the
// new colours. A palette nothing has read yet is left for its first use.

#include "ReloadHandlers.h"

#include "Project.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteRampTexture.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadPalettes(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        const bool read = jasperengine::pixelart::PaletteLibrary::version(assetPath) != 0;
        const bool strips = !jasperengine::pixelart::paletteRampTextureIds(assetPath).empty();
        if (!read && !strips) continue;
        if (read) jasperengine::pixelart::PaletteLibrary::reload(assetPath);
        jasperengine::pixelart::reloadPaletteRampTextures(assetPath);
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages())
            if (sceneProject->scene) sceneProject->needUpdateRender = true;
        ctx.service.recordDone("palette", path, 0.0);
    }
}
