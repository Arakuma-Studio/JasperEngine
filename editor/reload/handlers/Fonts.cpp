// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Fonts (S05 R11): the FontPool entries loaded from a changed font load again on a worker
// and replace theirs; the texts using them rebuild their glyph atlas. HTML UI fonts reload
// with their documents (the UI rule).

#include "ReloadHandlers.h"

#include "Project.h"
#include "component/TextComponent.h"
#include "pool/FontPool.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadFonts(Context& ctx, const std::vector<fs::path>& files) {
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        std::vector<std::pair<std::string, FontSource>> sources;
        for (const std::string& id : FontPool::findIdsUsingFile(watch::pathToUtf8(path))) {
            FontSource source;
            if (FontPool::getSource(id, source)) sources.emplace_back(id, source);
        }
        if (sources.empty()) continue;
        ctx.service.startJob("font", path, [sources, project, path]() -> HotReloadService::AdoptFn {
            std::vector<std::pair<std::string, std::shared_ptr<STBText>>> loaded;
            for (const auto& [id, source] : sources) {
                std::shared_ptr<STBText> font = FontPool::decode(source);
                if (!font) throw std::runtime_error("the font could not be read");
                loaded.emplace_back(id, font);
            }
            return [loaded, project, path](std::string&) {
                for (const auto& [id, font] : loaded) FontPool::replace(id, font);
                // the texts reading it build their atlas again from the new font
                for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                    Scene* scene = sceneProject->scene;
                    if (!scene || !scene->isComponentRegistered<TextComponent>()) continue;
                    auto texts = scene->getComponentArray<TextComponent>();
                    bool any = false;
                    for (size_t i = 0; i < texts->size(); i++) {
                        TextComponent& text = texts->getComponentFromIndex(i);
                        bool uses = false;
                        for (size_t f = 0; f < text.font.size(); f++) {
                            if (!text.font[f].empty() && project->resolveAssetPath(text.font[f]).lexically_normal() == path) uses = true;
                        }
                        if (!uses) continue;
                        text.needReloadAtlas = true;
                        text.needUpdateText = true;
                        any = true;
                    }
                    if (any) sceneProject->needUpdateRender = true;
                }
                return true;
            };
        });
    }
}
