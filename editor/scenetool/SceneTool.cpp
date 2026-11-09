// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "scenetool/SceneTool.h"

#include "render/SceneRender.h"

#include <map>

using namespace jasperengine;

namespace jasperengine::editor::scenetools_detail {

    struct ViewEntry {
        std::unique_ptr<SceneTool> tool;
        const Scene* scene = nullptr;
        ImVec2 canvasMin = ImVec2(0.0f, 0.0f);
        ImVec2 engineScale = ImVec2(1.0f, 1.0f);
    };

    std::map<uint32_t, ViewEntry>& views() {
        static std::map<uint32_t, ViewEntry> entries;
        return entries;
    }

}

void editor::SceneTools::activate(uint32_t sceneId, std::unique_ptr<SceneTool> tool) {
    scenetools_detail::ViewEntry& entry = scenetools_detail::views()[sceneId];
    if (entry.tool) entry.tool->cancel();
    entry.tool = std::move(tool);
}

editor::SceneTool* editor::SceneTools::active(uint32_t sceneId) {
    auto& entries = scenetools_detail::views();
    auto it = entries.find(sceneId);
    return it == entries.end() ? nullptr : it->second.tool.get();
}

void editor::SceneTools::deactivate(uint32_t sceneId) {
    auto& entries = scenetools_detail::views();
    auto it = entries.find(sceneId);
    if (it == entries.end()) return;
    // the entry stays until the cancel returned: a tool may look at its own view while reverting
    if (it->second.tool) it->second.tool->cancel();
    entries.erase(it);
}

void editor::SceneTools::setCanvas(uint32_t sceneId, ImVec2 canvasMin, ImVec2 engineScale) {
    scenetools_detail::ViewEntry& entry = scenetools_detail::views()[sceneId];
    entry.canvasMin = canvasMin;
    entry.engineScale = engineScale;
}

bool editor::SceneTools::worldToScreen(uint32_t sceneId, SceneRender& render, const Vector3& world, ImVec2& screen) {
    auto& entries = scenetools_detail::views();
    auto it = entries.find(sceneId);
    if (it == entries.end()) return false;
    Vector2 engine;
    if (!render.worldToScreen(world, engine)) return false;
    screen = ImVec2(it->second.canvasMin.x + engine.x / it->second.engineScale.x,
                    it->second.canvasMin.y + engine.y / it->second.engineScale.y);
    return true;
}

void editor::SceneTools::setScene(uint32_t sceneId, const Scene* scene) {
    scenetools_detail::views()[sceneId].scene = scene;
}

bool editor::SceneTools::activeOn(const Scene* scene) {
    if (!scene) return false;
    for (const auto& [id, entry] : scenetools_detail::views()) {
        if (entry.tool && entry.scene == scene) return true;
    }
    return false;
}
