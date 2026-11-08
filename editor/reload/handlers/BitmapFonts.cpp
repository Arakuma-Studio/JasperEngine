// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// Bitmap fonts (S51 AC23): the pooled fonts and chains that read a changed file (the
// .bitmapfont, a .fnt, a sheet or page PNG, a fallback font) are dropped; each Text using one
// sees its generation move on its next update and loads again (UISystem::updateBitmapText).

#include "ReloadHandlers.h"

#include "Project.h"
#include "pixelart/BitmapFontPool.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadBitmapFonts(Context& ctx, const std::vector<fs::path>& files) {
    bool any = false;
    for (const fs::path& path : files) {
        if (!::jasperengine::pixelart::BitmapFontPool::invalidate(watch::pathToUtf8(path)).empty()) any = true;
    }
    if (!any) return;
    for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages()) {
        if (sceneProject->scene) sceneProject->needUpdateRender = true;
    }
}
