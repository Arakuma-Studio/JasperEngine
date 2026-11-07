// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneDrawModes.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "object/LineRenderer.h"
#include "render/LodFade.h"
#include "subsystem/RenderSystem.h"
#include "profiler/GpuMemory.h"
#include "Theme.h"

#include <cmath>

#include <algorithm>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{
    struct DrawModeName{
        ViewDrawMode mode;
        const char* key;
        const char* label;
    };
    const DrawModeName kDrawModes[SCENE_DRAW_MODE_COUNT] = {
        {ViewDrawMode::SHADED, "shaded", "Shaded"},
        {ViewDrawMode::WIREFRAME, "wireframe", "Wireframe"},
        {ViewDrawMode::SHADED_WIREFRAME, "shaded_wireframe", "Shaded Wireframe"},
        {ViewDrawMode::LOD_LEVEL, "lod_level", "LOD Level"},
        {ViewDrawMode::OVERDRAW, "overdraw", "Overdraw"},
    };

    const Vector4 kWireColor(0.05f, 0.05f, 0.05f, 1.0f);
}

const char* editor::drawModeKey(ViewDrawMode mode){
    for (const DrawModeName& name : kDrawModes)
        if (name.mode == mode) return name.key;
    return "shaded";
}

bool editor::drawModeFromKey(std::string_view key, ViewDrawMode& mode){
    for (const DrawModeName& name : kDrawModes){
        if (key == name.key){
            mode = name.mode;
            return true;
        }
    }
    return false;
}

const char* editor::drawModeLabel(ViewDrawMode mode){
    for (const DrawModeName& name : kDrawModes)
        if (name.mode == mode) return name.label;
    return "Shaded";
}

bool editor::drawModeHasWires(ViewDrawMode mode){
    return mode == ViewDrawMode::WIREFRAME || mode == ViewDrawMode::SHADED_WIREFRAME;
}

const SceneWireframe::Edges* SceneWireframe::edgesOf(MeshComponent& mesh, Entity entity, unsigned int submesh, unsigned int level){
    const uint64_t lodKey = mesh.submeshes[submesh].lodKey;
    Edges& edges = cache[{entity, submesh, level}];
    if (!edges.positions.empty() && edges.lodKey == lodKey)
        return &edges;
    edges = Edges();
    edges.lodKey = lodKey;
    std::vector<uint32_t> indices;
    if (!RenderSystem::readSubmeshLevel(mesh, submesh, level, edges.positions, indices))
        return nullptr;
    // each edge once, whichever triangles share it
    std::set<std::pair<uint32_t, uint32_t>> unique;
    for (size_t t = 0; t + 2 < indices.size(); t += 3){
        for (int k = 0; k < 3; k++){
            uint32_t a = indices[t + k], b = indices[t + (k + 1) % 3];
            if (a > b) std::swap(a, b);
            if (a != b) unique.insert({a, b});
        }
    }
    edges.edges.assign(unique.begin(), unique.end());
    return &edges;
}

void SceneWireframe::update(Scene* scene, LineRenderer* lines, CameraComponent& camera, const Vector3& eye, const std::function<unsigned int(Entity)>& levelOf){
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    lines->clearLines();
    edgesDrawn = 0;
    truncated = false;
    if (!render)
        return;

    // the visible meshes, nearest first
    struct Visible{
        Entity entity;
        float distance;
    };
    std::vector<Visible> visible;
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        const Entity entity = meshes->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!mesh.loaded || !transform || !transform->visible || render->isEditorOverlay(entity) || mesh.worldAABB.isNull())
            continue;
        if (!render->isInsideCamera(camera.farClip, camera.frustumPlanes, mesh.worldAABB))
            continue;
        visible.push_back({entity, mesh.worldAABB.distance(eye)});
    }
    std::sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b){ return a.distance < b.distance; });

    std::set<std::tuple<Entity, unsigned int, unsigned int>> used;
    for (const Visible& item : visible){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(item.entity);
        const Transform& transform = scene->getComponent<Transform>(item.entity);
        const unsigned int level = levelOf(item.entity);
        if (level == LOD_CULLED)
            continue;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const Edges* edges = edgesOf(mesh, item.entity, s, level);
            if (!edges)
                continue;
            used.insert({item.entity, s, level});
            if (edgesDrawn + edges->edges.size() > MAX_EDGES){
                truncated = true;
                break;
            }
            for (const auto& [a, b] : edges->edges){
                lines->addLine(transform.modelMatrix * edges->positions[a], transform.modelMatrix * edges->positions[b], kWireColor);
            }
            edgesDrawn += edges->edges.size();
        }
        if (truncated)
            break;
    }
    // the edge lists of what is not drawn any more go
    for (auto it = cache.begin(); it != cache.end();){
        if (!used.count(it->first)) it = cache.erase(it);
        else ++it;
    }
}

void SceneWireframe::release(){
    cache.clear();
    edgesDrawn = 0;
    truncated = false;
}

void editor::drawLodLegend(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageMax){
    const float margin = Theme::dpi(8.0f);
    const float swatch = ImGui::GetTextLineHeight();
    const ImVec2 pad = Theme::dpi(ImVec2(8.0f, 6.0f));
    float width = pad.x;
    char labels[MAX_MESH_LODS][16];
    for (unsigned int l = 0; l < MAX_MESH_LODS; l++){
        std::snprintf(labels[l], sizeof(labels[l]), "LOD %u", l);
        width += swatch + Theme::dpi(4.0f) + ImGui::CalcTextSize(labels[l]).x + pad.x;
    }
    const ImVec2 size(width, swatch + pad.y * 2.0f);
    const ImVec2 min(imageMin.x + margin, imageMax.y - margin - size.y);
    drawList->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), IM_COL32(20, 20, 20, 210), Theme::dpi(3.0f));
    float x = min.x + pad.x;
    for (unsigned int l = 0; l < MAX_MESH_LODS; l++){
        float rgb[3];
        lodLevelColor(l, rgb);
        // the colours are linear: shown as the lit view shows them
        auto channel = [](float c){ return (int)std::clamp(std::pow(c, 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f); };
        drawList->AddRectFilled(ImVec2(x, min.y + pad.y), ImVec2(x + swatch, min.y + pad.y + swatch), IM_COL32(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]), 255));
        x += swatch + Theme::dpi(4.0f);
        drawList->AddText(ImVec2(x, min.y + pad.y), IM_COL32(220, 220, 220, 255), labels[l]);
        x += ImGui::CalcTextSize(labels[l]).x + pad.x;
    }
}

void editor::drawStatsBox(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageMax, const std::vector<std::string>& lines){
    float boxWidth = 0.0f;
    for (const std::string& line : lines) boxWidth = std::max(boxWidth, ImGui::CalcTextSize(line.c_str()).x);
    const ImVec2 pad = Theme::dpi(ImVec2(10.0f, 8.0f));
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 boxSize(boxWidth + pad.x * 2.0f, lineHeight * (float)lines.size() + pad.y * 2.0f - ImGui::GetStyle().ItemSpacing.y);
    const float margin = Theme::dpi(8.0f);
    ImVec2 boxMin(imageMax.x - boxSize.x - margin, imageMin.y + margin);
    boxMin.x = std::max(boxMin.x, imageMin.x + margin);
    drawList->AddRectFilled(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), IM_COL32(20, 20, 20, 210), Theme::dpi(3.0f));
    for (size_t i = 0; i < lines.size(); i++){
        const ImU32 color = i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255);
        drawList->AddText(ImVec2(boxMin.x + pad.x, boxMin.y + pad.y + lineHeight * (float)i), color, lines[i].c_str());
    }
}

void editor::appendMeshStatsLines(std::vector<std::string>& lines, uint64_t vertices, const LodFrameStats& lod, bool lodEnabled){
    char line[160];
    std::snprintf(line, sizeof(line), "Verts: %llu", (unsigned long long)vertices);
    lines.push_back(line);
    if (lodEnabled){
        int used = std::snprintf(line, sizeof(line), "LOD");
        for (unsigned int l = 0; l < MAX_MESH_LODS && used > 0 && (size_t)used < sizeof(line); l++)
            used += std::snprintf(line + used, sizeof(line) - (size_t)used, "%s %u: %u", l == 0 ? "" : ",", l, lod.levels[l]);
        lines.push_back(line);
        std::snprintf(line, sizeof(line), "Fading: %u (%u draws)  Culled by size: %u", lod.fading, lod.fadeDraws, lod.culledBySize);
        lines.push_back(line);
    }
    const gpumem::Report memory = gpumem::report(false, false);
    std::snprintf(line, sizeof(line), "Mesh memory: %.1f MB", (double)memory.categoryBytes[(size_t)gpumem::Category::Meshes] / (1024.0 * 1024.0));
    lines.push_back(line);
}
