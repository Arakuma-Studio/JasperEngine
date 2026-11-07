// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/TerrainOverlay.h"

#include "Project.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/GameObject.h"
#include "object/Mesh.h"
#include "object/TerrainEdit.h"
#include "subsystem/FoliageSystem.h"
#include "subsystem/MeshSystem.h"
#include "terrain/FoliageRules.h"
#include "window/TerrainEditWindow.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    constexpr int OVERLAY_GRID = 96;

    struct OverlayState {
        std::unique_ptr<Mesh> mesh;
        editor::TerrainOverlay::Kind kind = editor::TerrainOverlay::Kind::None;
        float maxValue = 0.0f;
        size_t triangles = 0;
    };

    std::map<uint32_t, OverlayState>& overlays() {
        static std::map<uint32_t, OverlayState> states;
        return states;
    }

    // blue, through green and yellow, to red: the same ramp as the height overlays read
    Vector4 heat(float t, float opacity) {
        t = std::clamp(t, 0.0f, 1.0f);
        const float r = std::clamp(t * 2.0f - 0.5f, 0.0f, 1.0f);
        const float g = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        const float b = std::clamp(1.0f - t * 2.0f, 0.0f, 1.0f);
        return Vector4(r, g, b, t > 0.0f ? opacity : opacity * 0.15f);
    }

}

void editor::TerrainOverlay::show(SceneProject* sceneProject, Entity entity, Kind kind, int layer, float opacity) {
    if (!sceneProject || !sceneProject->scene) return;
    Scene* scene = sceneProject->scene;
    OverlayState& state = overlays()[sceneProject->id];
    if (kind == Kind::None) {
        state.mesh.reset();
        state.kind = Kind::None;
        state.maxValue = 0.0f;
        state.triangles = 0;
        return;
    }
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    if (!terrain) return;
    const Matrix4 model = GameObject(scene, entity).getModelMatrix();

    // the values, per grid cell, per square unit
    std::vector<float> values(static_cast<size_t>(OVERLAY_GRID) * OVERLAY_GRID, 0.0f);
    const float size = terrain->terrainSize;
    const float half = size * 0.5f;
    const float cell = size / static_cast<float>(OVERLAY_GRID);
    if (kind == Kind::Density) {
        if (layer >= 0 && layer < static_cast<int>(terrain->foliageLayers.size())) {
            const TerrainFoliageLayer& detail = terrain->foliageLayers[layer];
            TerrainComponentStore store(scene, entity, *terrain);
            const MapId map{MapKind::Density, layer};
            const MapFormat format = store.format(map);
            if (format.exists) {
                std::vector<float> densities(static_cast<size_t>(format.bounds.area()));
                if (store.read(map, format.bounds, densities)) {
                    for (int y = 0; y < OVERLAY_GRID; y++) {
                        for (int x = 0; x < OVERLAY_GRID; x++) {
                            const int tx = std::min(format.bounds.w - 1, (x * format.bounds.w) / OVERLAY_GRID);
                            const int ty = std::min(format.bounds.h - 1, (y * format.bounds.h) / OVERLAY_GRID);
                            values[static_cast<size_t>(y) * OVERLAY_GRID + x] = densities[static_cast<size_t>(ty) * format.bounds.w + tx] * detail.density;
                        }
                    }
                }
            }
        }
    } else if (std::shared_ptr<const TreeStore> trees = static_cast<const FoliageSystem*>(scene->getSystem<FoliageSystem>().get())->getTreeStore(entity)) {
        for (const TreeRecord& record : trees->records()) {
            const int x = std::clamp(static_cast<int>((foliageTreeLocalX(record, size) + half) / cell), 0, OVERLAY_GRID - 1);
            const int y = std::clamp(static_cast<int>((foliageTreeLocalZ(record, size) + half) / cell), 0, OVERLAY_GRID - 1);
            values[static_cast<size_t>(y) * OVERLAY_GRID + x] += 1.0f / (cell * cell);
        }
    }
    float top = 0.0f;
    for (float value : values) top = std::max(top, value);
    state.maxValue = top;

    if (!state.mesh) {
        ScopedDefaultEntityPool system(*scene, EntityPool::System);
        state.mesh = std::make_unique<Mesh>(scene);
        TerrainEditWindow::markEditorOverlay(scene, state.mesh->getEntity());
        state.mesh->setTransparent(true);
        state.mesh->setReceiveLights(false);
        state.mesh->setCastShadows(false);
        state.mesh->setReceiveShadows(false);
        state.mesh->setFaceCulling(false);
    }
    state.kind = kind;

    MeshComponent& mesh = state.mesh->getComponent<MeshComponent>();
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;
    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);
    Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);

    // on the surface, a little above it, in world space (terrain-local through its transform)
    const float lift = std::max(terrain->maxHeight * 0.002f, 0.05f);
    const auto corner = [&](int x, int y) {
        const float lx = -half + static_cast<float>(x) * cell;
        const float lz = -half + static_cast<float>(y) * cell;
        return model * Vector3(lx, querySurface(*terrain, lx, lz).height + lift, lz);
    };
    const float scaleTop = top > 0.0f ? 1.0f / top : 0.0f;
    std::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(OVERLAY_GRID) * OVERLAY_GRID * 6);
    for (int y = 0; y < OVERLAY_GRID; y++) {
        for (int x = 0; x < OVERLAY_GRID; x++) {
            const Vector4 tint = heat(values[static_cast<size_t>(y) * OVERLAY_GRID + x] * scaleTop, opacity);
            const Vector3 a = corner(x, y), b = corner(x, y + 1), c = corner(x + 1, y + 1), d = corner(x + 1, y);
            const uint16_t first = static_cast<uint16_t>(indices.empty() ? 0 : (x + y * OVERLAY_GRID) * 4);
            for (const Vector3& v : {a, b, c, d}) {
                mesh.buffer.addVector3(position, v);
                mesh.buffer.addVector2(texcoord, Vector2(0.0f, 0.0f));
                mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
                mesh.buffer.addVector4(color, tint);
            }
            indices.insert(indices.end(), {first, static_cast<uint16_t>(first + 1), static_cast<uint16_t>(first + 2),
                                           first, static_cast<uint16_t>(first + 2), static_cast<uint16_t>(first + 3)});
        }
    }
    mesh.indices.clear();
    mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), reinterpret_cast<char*>(indices.data()), sizeof(uint16_t));
    scene->getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    if (mesh.loaded) mesh.needReload = true;
    state.mesh->setVisible(true);
    state.triangles = indices.size() / 3;
    sceneProject->needUpdateRender = true;
}

editor::TerrainOverlay::Kind editor::TerrainOverlay::shownKind(uint32_t sceneId) {
    const auto it = overlays().find(sceneId);
    return it != overlays().end() ? it->second.kind : Kind::None;
}

float editor::TerrainOverlay::maxValue(uint32_t sceneId) {
    const auto it = overlays().find(sceneId);
    return it != overlays().end() ? it->second.maxValue : 0.0f;
}

size_t editor::TerrainOverlay::triangleCount(uint32_t sceneId) {
    const auto it = overlays().find(sceneId);
    return it != overlays().end() ? it->second.triangles : 0;
}

void editor::TerrainOverlay::forget(uint32_t sceneId) {
    overlays().erase(sceneId);
}
