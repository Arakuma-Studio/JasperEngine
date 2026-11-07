// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationOverlay.h"

#include "Project.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/Transform.h"
#include "navigation/NavGridMap.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeInput.h"
#include "navigation/NavFrame.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavMesh.h"
#include "object/Camera.h"
#include "object/LineRenderer.h"
#include "object/Mesh.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/NavigationSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/ScopedDefaultEntityPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the alpha of what is drawn but not in effect: a deactivated link, an area override not applied
    constexpr float DEACTIVATED_ALPHA = 0.35f;

    const char* pathsName(NavOverlaySettings::Paths paths) {
        switch (paths) {
            case NavOverlaySettings::Paths::None: return "none";
            case NavOverlaySettings::Paths::Selected: return "selected";
            case NavOverlaySettings::Paths::All: return "all";
        }
        return "selected";
    }

    const char* colorByName(NavOverlaySettings::ColorBy colorBy) {
        switch (colorBy) {
            case NavOverlaySettings::ColorBy::Areas: return "areas";
            case NavOverlaySettings::ColorBy::Islands: return "islands";
            case NavOverlaySettings::ColorBy::Tiles: return "tiles";
        }
        return "areas";
    }

    // a distinct, stable tint per id (islands, tiles): golden-ratio steps around the hue circle
    // (in double: a float product of a large id loses its fraction)
    Vector4 idColor(uint32_t id, float alpha) {
        const float hue = (float)std::fmod((double)id * 0.6180339887498949, 1.0) * 6.0f;
        const float s = 0.65f, v = 0.95f;
        const int sector = (int)hue % 6;
        const float f = hue - std::floor(hue);
        const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
        switch (sector) {
            case 0: return Vector4(v, t, p, alpha);
            case 1: return Vector4(q, v, p, alpha);
            case 2: return Vector4(p, v, t, alpha);
            case 3: return Vector4(p, q, v, alpha);
            case 4: return Vector4(t, p, v, alpha);
            default: return Vector4(v, p, q, alpha);
        }
    }

    // the overlays' tint of an area (translucent fill)
    Vector4 areaColor(int area, float alpha) {
        const nav::NavigationSettings& settings = nav::getSettings();
        const Vector4 c = settings.areas[(size_t)std::clamp(area, 0, nav::AREA_COUNT - 1)].color;
        return Vector4(c.x, c.y, c.z, alpha);
    }

    bool inFrustum(CameraComponent& camera, const AABB& box) {
        if (box.isNull()) return false;
        const Vector3 centre = box.getCenter();
        const Vector3 half = box.getHalfSize();
        for (int plane = 0; plane < 6; plane++) {
            if (plane == FRUSTUM_PLANE_FAR && camera.farClip == 0) continue;
            if (camera.frustumPlanes[plane].getSide(centre, half) == Plane::Side::NEGATIVE_SIDE) return false;
        }
        return true;
    }

    void addBox(LineRenderer* lines, const Matrix4& model, const Vector3& center, const Vector3& size, const Vector4& color) {
        const Vector3 h = Vector3(std::fabs(size.x), std::fabs(size.y), std::fabs(size.z)) * 0.5f;
        Vector3 p[8];
        for (int c = 0; c < 8; c++)
            p[c] = model * (center + Vector3((c & 1) ? h.x : -h.x, (c & 2) ? h.y : -h.y, (c & 4) ? h.z : -h.z));
        const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : edges) lines->addLine(p[e[0]], p[e[1]], color);
    }

    void addCylinder(LineRenderer* lines, const Vector3& base, const Vector3& up, float radius, float height, const Vector4& color, int segments = 16) {
        const Vector3 n = up.length() > 1e-6f ? up.normalized() : Vector3(0, 1, 0);
        Vector3 right = std::fabs(n.y) > 0.9f ? Vector3(1, 0, 0).crossProduct(n) : Vector3(0, 1, 0).crossProduct(n);
        right = right.normalized();
        const Vector3 forward = n.crossProduct(right).normalized();
        for (int i = 0; i < segments; i++) {
            const float a0 = 6.2831853f * (float)i / (float)segments;
            const float a1 = 6.2831853f * (float)(i + 1) / (float)segments;
            const Vector3 o0 = (right * std::cos(a0) + forward * std::sin(a0)) * radius;
            const Vector3 o1 = (right * std::cos(a1) + forward * std::sin(a1)) * radius;
            lines->addLine(base + o0, base + o1, color);
            lines->addLine(base + n * height + o0, base + n * height + o1, color);
            if (i % 4 == 0) lines->addLine(base + o0, base + n * height + o0, color);
        }
    }

    // a link's arc from start to end (a jump), with an arrow head at the end (and the start when two-way)
    void addArc(LineRenderer* lines, const Vector3& a, const Vector3& b, const Vector3& up, bool bidirectional, const Vector4& color) {
        const float length = (b - a).length();
        const float lift = std::max(0.25f, length * 0.25f);
        Vector3 previous = a;
        const int segments = 12;
        for (int i = 1; i <= segments; i++) {
            const float t = (float)i / (float)segments;
            const Vector3 p = a + (b - a) * t + up * (lift * 4.0f * t * (1.0f - t));
            lines->addLine(previous, p, color);
            previous = p;
        }
        auto arrow = [&](const Vector3& tip, const Vector3& from) {
            Vector3 dir = tip - from;
            if (dir.length() < 1e-5f) return;
            dir = dir.normalized();
            Vector3 side = dir.crossProduct(up);
            if (side.length() < 1e-5f) side = Vector3(1, 0, 0);
            side = side.normalized();
            const float s = std::max(0.1f, length * 0.05f);
            lines->addLine(tip, tip - dir * s + side * s * 0.6f, color);
            lines->addLine(tip, tip - dir * s - side * s * 0.6f, color);
        };
        arrow(b, a + (b - a) * 0.9f + up * (lift * 0.36f));
        if (bidirectional) arrow(a, a + (b - a) * 0.1f + up * (lift * 0.36f));
    }

}

void NavigationOverlay::gridCellColors(const nav::NavGridMap& map, const std::array<Vector4, nav::AREA_COUNT>& areaColors,
                                       const std::vector<int>& reserved, std::vector<Vector4>& out) {
    const int cells = map.getCellCount();
    out.resize((size_t)cells);
    for (int i = 0; i < cells; i++) {
        const nav::GridCell cell = map.cellOf(i);
        const Vector4& c = areaColors[(size_t)std::clamp((int)map.getAreaAt(i), 0, nav::AREA_COUNT - 1)];
        out[(size_t)i] = Vector4(c.x, c.y, c.z, map.getOverride(cell) ? OVERRIDE_CELL_ALPHA : CELL_ALPHA);
    }
    const Vector4& path = AGENT_PATH_COLOR;
    for (int i : reserved) {
        if (i < 0 || i >= cells) continue;
        // from the area's colour each time: a cell two agents reserve is tinted once
        const nav::GridCell cell = map.cellOf(i);
        const Vector4& c = areaColors[(size_t)std::clamp((int)map.getAreaAt(i), 0, nav::AREA_COUNT - 1)];
        out[(size_t)i] = Vector4((c.x + path.x) * 0.5f, (c.y + path.y) * 0.5f, (c.z + path.z) * 0.5f,
                                 map.getOverride(cell) ? OVERRIDE_CELL_ALPHA : CELL_ALPHA);
    }
}

YAML::Node editor::encodeNavOverlaySettings(const NavOverlaySettings& settings) {
    const NavOverlaySettings defaults;
    YAML::Node node;
    if (settings.enabled != defaults.enabled) node["enabled"] = settings.enabled;
    if (settings.showNavMesh != defaults.showNavMesh) node["showNavMesh"] = settings.showNavMesh;
    if (settings.showTileBounds != defaults.showTileBounds) node["showTileBounds"] = settings.showTileBounds;
    if (settings.showHeightMesh != defaults.showHeightMesh) node["showHeightMesh"] = settings.showHeightMesh;
    if (settings.showGrid != defaults.showGrid) node["showGrid"] = settings.showGrid;
    if (settings.showPaths != defaults.showPaths) node["showPaths"] = pathsName(settings.showPaths);
    if (settings.showAvoidance != defaults.showAvoidance) node["showAvoidance"] = settings.showAvoidance;
    if (settings.showLinks != defaults.showLinks) node["showLinks"] = settings.showLinks;
    if (settings.showObstacles != defaults.showObstacles) node["showObstacles"] = settings.showObstacles;
    if (settings.showAreaOverrides != defaults.showAreaOverrides) node["showAreaOverrides"] = settings.showAreaOverrides;
    if (settings.colorBy != defaults.colorBy) node["colorBy"] = colorByName(settings.colorBy);
    if (settings.showSteepSlopes != defaults.showSteepSlopes) node["showSteepSlopes"] = settings.showSteepSlopes;
    if (settings.showTileLabels != defaults.showTileLabels) node["showTileLabels"] = settings.showTileLabels;
    return node;
}

void editor::decodeNavOverlaySettings(const YAML::Node& node, NavOverlaySettings& settings) {
    settings = NavOverlaySettings();
    if (!node.IsDefined() || !node.IsMap()) return;
    auto readBool = [&](const char* key, bool& value) {
        try {
            if (node[key]) value = node[key].as<bool>();
        } catch (const YAML::Exception&) {
        }
    };
    readBool("enabled", settings.enabled);
    readBool("showNavMesh", settings.showNavMesh);
    readBool("showTileBounds", settings.showTileBounds);
    readBool("showHeightMesh", settings.showHeightMesh);
    readBool("showGrid", settings.showGrid);
    readBool("showAvoidance", settings.showAvoidance);
    readBool("showLinks", settings.showLinks);
    readBool("showObstacles", settings.showObstacles);
    readBool("showAreaOverrides", settings.showAreaOverrides);
    readBool("showSteepSlopes", settings.showSteepSlopes);
    readBool("showTileLabels", settings.showTileLabels);
    try {
        if (node["colorBy"]) {
            const std::string colorBy = node["colorBy"].as<std::string>();
            if (colorBy == "islands") settings.colorBy = NavOverlaySettings::ColorBy::Islands;
            else if (colorBy == "tiles") settings.colorBy = NavOverlaySettings::ColorBy::Tiles;
        }
    } catch (const YAML::Exception&) {
    }
    try {
        if (node["showPaths"]) {
            const std::string paths = node["showPaths"].as<std::string>();
            if (paths == "none") settings.showPaths = NavOverlaySettings::Paths::None;
            else if (paths == "all") settings.showPaths = NavOverlaySettings::Paths::All;
            else settings.showPaths = NavOverlaySettings::Paths::Selected;
        }
    } catch (const YAML::Exception&) {
    }
}

NavigationOverlay::NavigationOverlay(Scene* scene): scene(scene) {
}

NavigationOverlay::~NavigationOverlay() {
    for (auto& entry : surfaces) destroy(entry.second);
    for (auto& entry : grids) destroy(entry.second);
    delete lines;
}

void NavigationOverlay::destroy(SurfaceDraw& draw) {
    for (Mesh* mesh : draw.meshes) delete mesh;
    draw.meshes.clear();
    for (Mesh* mesh : draw.slopes) delete mesh;
    draw.slopes.clear();
    delete draw.edges;
    draw.edges = nullptr;
}

void NavigationOverlay::destroy(GridDraw& draw) {
    for (Mesh* mesh : draw.meshes) delete mesh;
    draw.meshes.clear();
    delete draw.lines;
    draw.lines = nullptr;
}

Mesh* NavigationOverlay::newMesh() {
    ScopedDefaultEntityPool sys(*scene, EntityPool::System);
    Mesh* mesh = new Mesh(scene);
    scene->getSystem<RenderSystem>()->setEditorOverlay(mesh->getEntity(), true);
    mesh->setTransparent(true);
    mesh->setReceiveLights(false);
    mesh->setCastShadows(false);
    mesh->setReceiveShadows(false);
    mesh->setFaceCulling(false);
    return mesh;
}

LineRenderer* NavigationOverlay::newLines(unsigned int maxLines) {
    ScopedDefaultEntityPool sys(*scene, EntityPool::System);
    LineRenderer* renderer = new LineRenderer(scene);
    scene->getSystem<RenderSystem>()->setEditorOverlay(renderer->getEntity(), true);
    renderer->setMaxLines(maxLines);
    return renderer;
}

// Triangles (world, 3 points each) into meshes of at most 21845 triangles (16-bit indices),
// lifted along +Y (XZ) a little so they do not fight the ground
void NavigationOverlay::fillMeshes(std::vector<Mesh*>& meshes, const std::vector<float>& triangles, const std::vector<Vector4>& colors,
                                   float lift) {
    const size_t count = triangles.size() / 9;
    const size_t perMesh = 21845;
    const size_t needed = (count + perMesh - 1) / perMesh;
    while (meshes.size() < needed) meshes.push_back(newMesh());
    while (meshes.size() > needed) {
        delete meshes.back();
        meshes.pop_back();
    }
    auto meshSystem = scene->getSystem<MeshSystem>();
    for (size_t m = 0; m < needed; m++) {
        MeshComponent& mesh = meshes[m]->getComponent<MeshComponent>();
        const size_t first = m * perMesh;
        const size_t n = std::min(perMesh, count - first);
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
        std::vector<uint16_t> indices;
        indices.reserve(n * 3);
        for (size_t t = 0; t < n; t++) {
            const float* p = &triangles[(first + t) * 9];
            const Vector3 a(p[0], p[1], p[2]), b(p[3], p[4], p[5]), c(p[6], p[7], p[8]);
            Vector3 up = (b - a).crossProduct(c - a);
            up = up.length() > 1e-8f ? up.normalized() : Vector3(0, 1, 0);
            if (up.y < 0.0f) up = up * -1.0f;
            for (const Vector3& v : {a, b, c}) {
                mesh.buffer.addVector3(position, v + up * lift);
                mesh.buffer.addVector2(texcoord, Vector2(0.0f, 0.0f));
                mesh.buffer.addVector3(normal, up);
                mesh.buffer.addVector4(color, colors[first + t]);
                indices.push_back((uint16_t)indices.size());
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)indices.data(), sizeof(uint16_t));
        meshSystem->calculateMeshAABB(mesh);
        if (mesh.loaded) mesh.needReload = true;
        meshes[m]->setVisible(true);
    }
}

void NavigationOverlay::hide() {
    for (auto& entry : surfaces) {
        for (Mesh* mesh : entry.second.meshes) mesh->setVisible(false);
        for (Mesh* mesh : entry.second.slopes) mesh->setVisible(false);
        if (entry.second.edges) entry.second.edges->setVisible(false);
    }
    tileLabels.clear();
    for (auto& entry : grids) {
        for (Mesh* mesh : entry.second.meshes) mesh->setVisible(false);
        if (entry.second.lines) entry.second.lines->setVisible(false);
    }
    if (lines) lines->setVisible(false);
}

size_t NavigationOverlay::getObjectCount() const {
    size_t count = lines ? 1 : 0;
    for (const auto& entry : surfaces) count += entry.second.meshes.size() + entry.second.slopes.size() + (entry.second.edges ? 1 : 0);
    for (const auto& entry : grids) count += entry.second.meshes.size() + (entry.second.lines ? 1 : 0);
    return count;
}

std::vector<Vector4> NavigationOverlay::triangleColors(const nav::NavDebugGeometry& geometry, NavOverlaySettings::ColorBy colorBy,
                                                       int startIsland) {
    const float alpha = 0.45f;
    const Vector4 grey(0.45f, 0.45f, 0.45f, alpha);
    std::vector<Vector4> colors;
    colors.reserve(geometry.triangleAreas.size());
    for (size_t t = 0; t < geometry.triangleAreas.size(); t++) {
        if (colorBy == NavOverlaySettings::ColorBy::Islands && t < geometry.triangleIslands.size()) {
            const int island = geometry.triangleIslands[t];
            const bool unreachable = island < 0 || (startIsland >= 0 && island != startIsland);
            colors.push_back(unreachable ? grey : idColor((uint32_t)island, alpha));
        } else if (colorBy == NavOverlaySettings::ColorBy::Tiles && t < geometry.triangleTiles.size()) {
            const size_t tile = (size_t)geometry.triangleTiles[t] * 3;
            const uint32_t x = tile + 2 < geometry.tileCoords.size() ? (uint32_t)geometry.tileCoords[tile] : 0;
            const uint32_t y = tile + 2 < geometry.tileCoords.size() ? (uint32_t)geometry.tileCoords[tile + 1] : 0;
            const uint32_t layer = tile + 2 < geometry.tileCoords.size() ? (uint32_t)geometry.tileCoords[tile + 2] : 0;
            colors.push_back(idColor(x * 73856093u ^ y * 19349663u ^ layer * 83492791u, alpha));
        } else {
            colors.push_back(areaColor(geometry.triangleAreas[t], alpha));
        }
    }
    return colors;
}

std::string NavigationOverlay::tileLabelText(const nav::NavTileReport& report) {
    char text[96];
    const std::string ms = report.milliseconds < 0.0 ? std::string("\xE2\x80\x93") : [&]() {
        char value[32];
        std::snprintf(value, sizeof(value), "%.1f", report.milliseconds);
        return std::string(value);
    }();
    std::snprintf(text, sizeof(text), "%s ms \xC2\xB7 %d polys \xC2\xB7 %.1f KB", ms.c_str(), report.polys, (double)report.bytes / 1024.0);
    return text;
}

void NavigationOverlay::update(SceneProject& sceneProject, Camera* camera, const std::set<Entity>& selected,
                               const NavOverlaySettings& settings) {
    drawnTriangles = 0;
    drawnLines = 0;
    drawnSlopeTriangles = 0;
    tileLabels.clear();
    if (!settings.enabled || !sceneProject.scene) {
        hide();
        return;
    }
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    if (!system) {
        hide();
        return;
    }

    // surfaces, by entity (the triangle budget goes to the first ones)
    std::set<Entity> currentSurfaces;
    size_t budget = MAX_TRIANGLES;
    size_t slopeBudget = MAX_TRIANGLES;
    auto surfaceArray = scene->getComponentArray<NavMeshSurfaceComponent>();
    std::vector<Entity> surfaceEntities;
    for (size_t i = 0; i < surfaceArray->size(); i++) surfaceEntities.push_back(surfaceArray->getEntity(i));
    std::sort(surfaceEntities.begin(), surfaceEntities.end());
    for (Entity entity : surfaceEntities) {
        currentSurfaces.insert(entity);
        SurfaceDraw& draw = surfaces[entity];
        updateSurface(entity, draw, sceneProject, camera, settings, budget);
        updateSlopes(entity, draw, sceneProject, settings, slopeBudget);
    }
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        if (!currentSurfaces.count(it->first)) {
            destroy(it->second);
            it = surfaces.erase(it);
        } else {
            ++it;
        }
    }

    std::set<Entity> currentGrids;
    auto gridArray = scene->getComponentArray<NavGridComponent>();
    for (size_t i = 0; i < gridArray->size(); i++) {
        const Entity entity = gridArray->getEntity(i);
        currentGrids.insert(entity);
        updateGrid(entity, grids[entity], sceneProject, settings);
    }
    for (auto it = grids.begin(); it != grids.end();) {
        if (!currentGrids.count(it->first)) {
            destroy(it->second);
            it = grids.erase(it);
        } else {
            ++it;
        }
    }

    drawGizmos(sceneProject, selected, settings);
}

void NavigationOverlay::updateSurface(Entity entity, SurfaceDraw& draw, SceneProject& sceneProject, Camera* camera,
                                      const NavOverlaySettings& settings, size_t& triangleBudget) {
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entity);
    const nav::NavMesh* mesh = system->getNavMesh(entity);
    if (!settings.showNavMesh || !surface.enabled || !mesh) {
        for (Mesh* m : draw.meshes) m->setVisible(false);
        if (draw.edges) draw.edges->setVisible(false);
        return;
    }
    // the tiles in the view: a rebuild when they, the navmesh, the area colours or what tints
    // the triangles changed
    CameraComponent& cameraComponent = camera->getComponent<CameraComponent>();
    std::vector<uint32_t> key;
    nav::NavDebugGeometry geometry;
    uint32_t tileIndex = 0;
    const uint32_t extras = settings.colorBy == NavOverlaySettings::ColorBy::Islands ? nav::DEBUG_ISLANDS
                          : settings.colorBy == NavOverlaySettings::ColorBy::Tiles ? nav::DEBUG_TILES : 0u;
    system->getDebugGeometry(entity, [&](const AABB& box) {
        const bool visible = inFrustum(cameraComponent, box);
        if (visible) key.push_back(tileIndex);
        tileIndex++;
        return visible;
    }, triangleBudget, settings.showHeightMesh, geometry, extras);
    // Color By Islands with a query start on this surface: the islands it cannot reach are grey
    int startIsland = -1;
    if (settings.colorBy == NavOverlaySettings::ColorBy::Islands) {
        if (const NavQueryDraw* query = NavigationEditor::get().getQueryDraw(sceneProject.id); query && query->hasFrom) {
            const nav::NavLocation start = system->locateOn(entity, query->from);
            if (start.onMesh) startIsland = start.island;
        }
    }
    // the tiles whose inputs changed since the bake, outlined while tile bounds or labels show
    const bool outlines = settings.showTileBounds || settings.showTileLabels;
    std::vector<std::pair<int, int>> dirty;
    const NavigationSystem::InputReport* inputs = outlines ? NavigationEditor::get().inputReport(sceneProject, entity) : nullptr;
    if (inputs) {
        for (const nav::NavTileCoord& coord : inputs->dirty) dirty.push_back({coord.x, coord.y});
    }
    const uint64_t version = nav::getSettingsVersion();
    const bool same = draw.revision == mesh->getRevision() && draw.visibleKey == key && draw.detail == settings.showHeightMesh &&
                      draw.settingsVersion == version && draw.colorBy == settings.colorBy && draw.startIsland == startIsland &&
                      draw.tileBounds == settings.showTileBounds && draw.dirtyOutlines == outlines && draw.dirty == dirty;
    triangleBudget -= std::min(triangleBudget, geometry.triangleAreas.size());
    drawnTriangles += geometry.triangleAreas.size();
    const size_t drawnTiles = geometry.tileBounds.size() / 24;
    auto tileCoord = [&](size_t t) {
        return std::pair<int, int>(geometry.tileCoords[t * 3], geometry.tileCoords[t * 3 + 1]);
    };
    if (!same) {
        draw.revision = mesh->getRevision();
        draw.visibleKey = key;
        draw.detail = settings.showHeightMesh;
        draw.settingsVersion = version;
        draw.colorBy = settings.colorBy;
        draw.startIsland = startIsland;
        draw.tileBounds = settings.showTileBounds;
        draw.dirtyOutlines = outlines;
        draw.dirty = dirty;
        const float lift = surface.plane == nav::NavPlane::XY ? 0.0f : 0.02f;
        fillMeshes(draw.meshes, geometry.triangles, triangleColors(geometry, settings.colorBy, startIsland), lift);

        // the dirty tiles drawn here get their box; the others (new ground, a tile not built) a
        // flat square at the navmesh's lowest tile
        std::vector<size_t> dirtyDrawn;
        std::vector<std::pair<int, int>> dirtyMissing;
        if (outlines) {
            for (const std::pair<int, int>& coord : dirty) {
                bool found = false;
                for (size_t t = 0; t < drawnTiles && t * 3 + 1 < geometry.tileCoords.size(); t++) {
                    if (tileCoord(t) == coord) {
                        dirtyDrawn.push_back(t);
                        found = true;
                    }
                }
                if (!found) dirtyMissing.push_back(coord);
            }
        }
        const size_t boundary = geometry.boundaryEdges.size() / 6;
        const size_t inner = geometry.polygonEdges.size() / 6;
        const size_t tiles = settings.showTileBounds ? drawnTiles : 0;
        const unsigned int maxLines =
            (unsigned int)std::max<size_t>(1, boundary + inner + (tiles + dirtyDrawn.size()) * 12 + dirtyMissing.size() * 4);
        if (!draw.edges) draw.edges = newLines(maxLines);
        draw.edges->setMaxLines(maxLines);
        draw.edges->clearLines();
        const Vector3 up = surface.plane == nav::NavPlane::XY ? Vector3(0, 0, 0) : Vector3(0, lift * 1.5f, 0);
        auto addSegments = [&](const std::vector<float>& points, const Vector4& color) {
            for (size_t i = 0; i + 5 < points.size(); i += 6) {
                draw.edges->addLine(Vector3(points[i], points[i + 1], points[i + 2]) + up,
                                    Vector3(points[i + 3], points[i + 4], points[i + 5]) + up, color);
            }
        };
        addSegments(geometry.polygonEdges, Vector4(0.0f, 0.2f, 0.25f, 0.35f));
        addSegments(geometry.boundaryEdges, Vector4(0.0f, 0.1f, 0.15f, 0.9f));
        auto addTileBox = [&](size_t t, const Vector4& color) {
            const float* c = &geometry.tileBounds[t * 24];
            auto corner = [&](int i) { return Vector3(c[i * 3], c[i * 3 + 1], c[i * 3 + 2]); };
            const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) draw.edges->addLine(corner(e[0]), corner(e[1]), color);
        };
        for (size_t t = 0; t < tiles; t++) addTileBox(t, Vector4(1.0f, 1.0f, 1.0f, 0.25f));
        const Vector4 orange(1.0f, 0.55f, 0.1f, 1.0f);
        for (size_t t : dirtyDrawn) addTileBox(t, orange);
        if (!dirtyMissing.empty() && inputs && inputs->tileSize > 0) {
            const nav::NavFrame frame = nav::NavBakeCollector::frameOf(scene, entity);
            const float size = (float)inputs->tileSize * inputs->cellSize;
            float floor = 0.0f;
            for (size_t t = 0; t < drawnTiles; t++) {
                const float y = frame.toNav(Vector3(geometry.tileBounds[t * 24], geometry.tileBounds[t * 24 + 1], geometry.tileBounds[t * 24 + 2])).y;
                floor = t == 0 ? y : std::min(floor, y);
            }
            for (const std::pair<int, int>& coord : dirtyMissing) {
                const float x0 = (float)coord.first * size, z0 = (float)coord.second * size;
                const Vector3 corners[4] = {frame.toWorld(Vector3(x0, floor, z0)), frame.toWorld(Vector3(x0 + size, floor, z0)),
                                            frame.toWorld(Vector3(x0 + size, floor, z0 + size)), frame.toWorld(Vector3(x0, floor, z0 + size))};
                for (int i = 0; i < 4; i++) draw.edges->addLine(corners[i], corners[(i + 1) % 4], orange);
            }
        }
        draw.edges->updateLines();
    }
    for (Mesh* m : draw.meshes) m->setVisible(true);
    if (draw.edges) {
        draw.edges->setVisible(true);
        drawnLines += draw.edges->getComponent<LineRendererComponent>().lines.size();
    }

    // the visible tiles' last builds, at the top of each tile's box
    if (settings.showTileLabels) {
        if (draw.reportsRevision != mesh->getRevision() || draw.reports.empty()) {
            draw.reports = system->getTileReports(entity);
            draw.reportsRevision = mesh->getRevision();
        }
        for (size_t t = 0; t < drawnTiles && t * 3 + 2 < geometry.tileCoords.size() && tileLabels.size() < MAX_TILE_LABELS; t++) {
            const int x = geometry.tileCoords[t * 3], y = geometry.tileCoords[t * 3 + 1], layer = geometry.tileCoords[t * 3 + 2];
            auto report = std::find_if(draw.reports.begin(), draw.reports.end(), [&](const nav::NavTileReport& r) {
                return r.x == x && r.y == y && r.layer == layer;
            });
            if (report == draw.reports.end()) continue;
            NavTileLabel label;
            label.surface = entity;
            label.x = x;
            label.y = y;
            label.text = tileLabelText(*report);
            // the bottom face's centre (corners with y min: not c & 2), on the ground it labels
            const float* c = &geometry.tileBounds[t * 24];
            Vector3 bottom(0, 0, 0);
            for (int i : {0, 1, 4, 5}) bottom += Vector3(c[i * 3], c[i * 3 + 1], c[i * 3 + 2]);
            label.world = bottom * 0.25f;
            label.dirty = std::find(dirty.begin(), dirty.end(), std::pair<int, int>(x, y)) != dirty.end();
            tileLabels.push_back(std::move(label));
        }
    }
}

// Show Steep Slopes (S75 R27): the triangles the bake collected (meshes and terrains) that are
// steeper than the agent type's Max Slope, tinted red, from the editor's cached input. A 2D
// surface is flat: nothing to show.
void NavigationOverlay::updateSlopes(Entity entity, SurfaceDraw& draw, SceneProject& sceneProject, const NavOverlaySettings& settings,
                                     size_t& triangleBudget) {
    const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entity);
    std::shared_ptr<const nav::NavBakeInput> input;
    if (settings.showSteepSlopes && surface.enabled && surface.plane == nav::NavPlane::XZ)
        input = NavigationEditor::get().keptInput(sceneProject, entity);
    if (!input) {
        for (Mesh* m : draw.slopes) m->setVisible(false);
        return;
    }
    // the input's identity: its configuration and every tile's hash (a recollection that found
    // the same scene keeps the triangles)
    uint64_t slopeKey = input->config.hash() ^ 0x9E3779B97F4A7C15ull;
    for (uint64_t hash : input->tileHashes) slopeKey = (slopeKey ^ hash) * 1099511628211ull;
    slopeKey ^= (uint64_t)nav::getSettingsVersion() << 1;
    if (slopeKey != draw.slopeKey) {
        draw.slopeKey = slopeKey;
        const nav::NavFrame frame = nav::NavBakeCollector::frameOf(scene, entity);
        const float limit = std::cos(input->config.agentMaxSlope * 3.14159265f / 180.0f);
        std::vector<float> triangles;
        std::vector<Vector4> colors;
        const Vector4 color(1.0f, 0.15f, 0.1f, 0.55f);
        auto add = [&](const Vector3& a, const Vector3& b, const Vector3& c) {
            if (colors.size() >= triangleBudget) return false;
            Vector3 n = (b - a).crossProduct(c - a);
            const float length = n.length();
            if (length < 1e-8f) return true;
            // nav space is Y up: steeper than the limit when the normal leans further from it
            if (std::fabs(n.y / length) >= limit) return true;
            for (const Vector3& v : {a, b, c}) {
                const Vector3 w = frame.toWorld(v);
                triangles.push_back(w.x);
                triangles.push_back(w.y);
                triangles.push_back(w.z);
            }
            colors.push_back(color);
            return true;
        };
        auto vertex = [&](int index) {
            const float* v = &input->vertices[(size_t)index * 3];
            return Vector3(v[0], v[1], v[2]);
        };
        bool room = true;
        for (size_t t = 0; room && t + 2 < input->triangles.size(); t += 3)
            room = add(vertex(input->triangles[t]), vertex(input->triangles[t + 1]), vertex(input->triangles[t + 2]));
        for (const nav::NavTerrainInput& terrain : input->terrains) {
            for (int j = 0; room && j + 1 < terrain.samples; j++) {
                for (int i = 0; room && i + 1 < terrain.samples; i++) {
                    const Vector3 p00 = terrain.toNav * terrain.localAt(i, j), p10 = terrain.toNav * terrain.localAt(i + 1, j);
                    const Vector3 p01 = terrain.toNav * terrain.localAt(i, j + 1), p11 = terrain.toNav * terrain.localAt(i + 1, j + 1);
                    room = add(p00, p01, p10) && add(p10, p01, p11);
                }
            }
        }
        fillMeshes(draw.slopes, triangles, colors, 0.03f);
        draw.slopeTriangles = colors.size();
    }
    triangleBudget -= std::min(triangleBudget, draw.slopeTriangles);
    drawnSlopeTriangles += draw.slopeTriangles;
    for (Mesh* m : draw.slopes) m->setVisible(true);
}

void NavigationOverlay::updateGrid(Entity entity, GridDraw& draw, SceneProject& sceneProject, const NavOverlaySettings& settings) {
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    const NavGridComponent& grid = scene->getComponent<NavGridComponent>(entity);
    nav::NavGridMap* map = settings.showGrid && grid.enabled ? system->getGridMap(entity) : nullptr;
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!map || !transform) {
        for (Mesh* m : draw.meshes) m->setVisible(false);
        if (draw.lines) draw.lines->setVisible(false);
        return;
    }
    const float pose[7] = {transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z, transform->worldRotation.w,
                           transform->worldRotation.x, transform->worldRotation.y, transform->worldRotation.z};
    const uint64_t version = nav::getSettingsVersion();
    const bool moved = !std::equal(pose, pose + 7, draw.pose);
    // in play, the cells agents reserved this fixed tick (S74 R22; ascending, each once)
    reservedScratch.clear();
    if (sceneProject.playState != ScenePlayState::STOPPED) system->getGridReservedCells(entity, reservedScratch);
    if (draw.revision != map->getRevision() || draw.settingsVersion != version || moved || draw.meshes.empty() ||
        draw.reserved != reservedScratch) {
        draw.revision = map->getRevision();
        draw.settingsVersion = version;
        draw.reserved = reservedScratch;
        std::copy(pose, pose + 7, draw.pose);
        std::array<Vector4, nav::AREA_COUNT> areaColors;
        for (int a = 0; a < nav::AREA_COUNT; a++) areaColors[(size_t)a] = nav::getSettings().areas[(size_t)a].color;
        gridCellColors(*map, areaColors, draw.reserved, cellColors);
        std::vector<float> triangles;
        std::vector<Vector4> colors;
        const int cells = map->getCellCount();
        // at most the overlay's triangle cap: one in `stride` rows and columns beyond it
        const int corners = map->getLayout() == nav::GridLayout::Hex ? 6 : 4;
        const int perCell = corners - 2;
        const int stride = std::max(1, (int)std::ceil(std::sqrt((double)cells * perCell / (double)MAX_TRIANGLES)));
        Vector2 outline[6];
        for (int y = 0; y < map->getHeight(); y += stride) {
            for (int x = 0; x < map->getWidth(); x += stride) {
                const nav::GridCell cell{x, y};
                const int n = map->cellCorners(cell, outline);
                const float height = map->getHeight(cell);
                Vector3 world[6];
                for (int i = 0; i < n; i++) world[i] = system->gridLocalToWorld(entity, outline[i], height);
                const Vector4& color = cellColors[(size_t)map->index(cell)];
                for (int i = 1; i + 1 < n; i++) {
                    for (const Vector3& v : {world[0], world[i], world[i + 1]}) {
                        triangles.push_back(v.x);
                        triangles.push_back(v.y);
                        triangles.push_back(v.z);
                    }
                    colors.push_back(color);
                }
            }
        }
        fillMeshes(draw.meshes, triangles, colors, grid.plane == nav::NavPlane::XY ? 0.0f : 0.03f);
        // cell outlines for grids small enough to read
        const bool outlines = cells <= 128 * 128;
        const unsigned int maxLines = outlines ? (unsigned int)(cells * corners) : 4;
        if (!draw.lines) draw.lines = newLines(maxLines);
        draw.lines->setMaxLines(maxLines);
        draw.lines->clearLines();
        const Vector4 lineColor(0.0f, 0.0f, 0.0f, 0.35f);
        if (outlines) {
            for (int y = 0; y < map->getHeight(); y++) {
                for (int x = 0; x < map->getWidth(); x++) {
                    const nav::GridCell cell{x, y};
                    const int n = map->cellCorners(cell, outline);
                    const float height = map->getHeight(cell);
                    for (int i = 0; i < n; i++) {
                        draw.lines->addLine(system->gridLocalToWorld(entity, outline[i], height),
                                            system->gridLocalToWorld(entity, outline[(i + 1) % n], height), lineColor);
                    }
                }
            }
        } else {
            const Vector2 size = map->getSize();
            const Vector2 c[4] = {Vector2(0, 0), Vector2(size.x, 0), Vector2(size.x, size.y), Vector2(0, size.y)};
            for (int i = 0; i < 4; i++)
                draw.lines->addLine(system->gridLocalToWorld(entity, c[i]), system->gridLocalToWorld(entity, c[(i + 1) % 4]), lineColor);
        }
        draw.lines->updateLines();
    }
    for (Mesh* m : draw.meshes) m->setVisible(true);
    if (draw.lines) draw.lines->setVisible(true);
}

void NavigationOverlay::drawGizmos(SceneProject& sceneProject, const std::set<Entity>& selected, const NavOverlaySettings& settings) {
    NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
    if (!lines) lines = newLines(4096);
    lines->clearLines();
    size_t capacity = 4096;
    auto reserve = [&](size_t more) {
        // the renderer's capacity follows what this update draws
        const size_t used = lines->getComponent<LineRendererComponent>().lines.size();
        if (used + more > capacity) {
            capacity = std::max(capacity * 2, used + more);
            lines->setMaxLines((unsigned int)capacity);
        }
    };
    const bool playing = sceneProject.playState != ScenePlayState::STOPPED;

    // agents: paths (selected / all), avoidance radius and velocity, the selected one's cylinder
    auto agents = scene->getComponentArray<NavMeshAgentComponent>();
    for (size_t i = 0; i < agents->size(); i++) {
        const Entity entity = agents->getEntity(i);
        const NavMeshAgentComponent& agent = agents->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) continue;
        const bool isSelected = selected.count(entity) > 0;
        const Vector3 base = transform->worldPosition - Vector3(0, agent.baseOffset, 0);
        if (isSelected) {
            reserve(64);
            addCylinder(lines, base, Vector3(0, 1, 0), agent.radius, agent.height, Vector4(0.2f, 0.9f, 0.4f, 0.9f));
        }
        if (playing && (settings.showPaths == NavOverlaySettings::Paths::All ||
                        (settings.showPaths == NavOverlaySettings::Paths::Selected && isSelected))) {
            std::vector<Vector3> corners;
            if (system->getAgentPath(entity, corners) && !corners.empty()) {
                reserve(corners.size() + 1);
                Vector3 previous = base;
                for (const Vector3& corner : corners) {
                    lines->addLine(previous, corner, AGENT_PATH_COLOR);
                    previous = corner;
                }
            }
        }
        if (playing && settings.showAvoidance) {
            reserve(20);
            addCylinder(lines, base, Vector3(0, 1, 0), agent.radius, 0.0f, Vector4(0.9f, 0.3f, 0.9f, 0.7f), 16);
            lines->addLine(base, base + agent.velocity, Vector4(0.9f, 0.3f, 0.9f, 1.0f));
        }
    }

    // links: an arc from start to end, the selected one's endpoints marked (its handles)
    if (settings.showLinks) {
        auto links = scene->getComponentArray<NavMeshLinkComponent>();
        for (size_t i = 0; i < links->size(); i++) {
            const Entity entity = links->getEntity(i);
            const NavMeshLinkComponent& link = links->getComponentFromIndex(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) continue;
            const Vector3 a = transform->modelMatrix * link.startPoint;
            const Vector3 b = transform->modelMatrix * link.endPoint;
            Vector4 color = link.enabled ? areaColor(link.area, 1.0f) : Vector4(0.6f, 0.6f, 0.6f, 0.8f);
            // S76: a deactivated link is faint, one an agent is crossing is white (R41)
            if (!link.activated)
                color.w *= DEACTIVATED_ALPHA;
            else if (playing && system && system->isLinkOccupied(entity))
                color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            reserve(40);
            const Vector3 up = transform->worldRotation * Vector3(0, 1, 0);
            addArc(lines, a, b, up, link.bidirectional, color);
            if (selected.count(entity)) {
                const float r = std::max(0.15f, link.width * 0.5f);
                addCylinder(lines, a, up, r, 0.0f, Vector4(1, 1, 1, 1), 12);
                addCylinder(lines, b, up, r, 0.0f, Vector4(1, 1, 1, 1), 12);
                if (link.width > 0.0f) {
                    Vector3 side = (b - a).crossProduct(up);
                    if (side.length() > 1e-5f) {
                        side = side.normalized() * (link.width * 0.5f);
                        lines->addLine(a - side, a + side, color);
                        lines->addLine(b - side, b + side, color);
                    }
                }
            }
        }
    }

    // obstacles: their box or capsule
    if (settings.showObstacles) {
        auto obstacles = scene->getComponentArray<NavMeshObstacleComponent>();
        for (size_t i = 0; i < obstacles->size(); i++) {
            const Entity entity = obstacles->getEntity(i);
            const NavMeshObstacleComponent& obstacle = obstacles->getComponentFromIndex(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) continue;
            const float alpha = selected.count(entity) ? 1.0f : 0.5f;
            const Vector4 color = obstacle.carve ? Vector4(1.0f, 0.4f, 0.2f, alpha) : Vector4(0.9f, 0.8f, 0.2f, alpha);
            reserve(64);
            if (obstacle.shape == NavObstacleShape::BOX) {
                addBox(lines, transform->modelMatrix, obstacle.center, obstacle.size, color);
            } else {
                const Vector3 base = transform->modelMatrix * (obstacle.center - Vector3(0, obstacle.height * 0.5f, 0));
                const float scale = std::max(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.z));
                addCylinder(lines, base, transform->worldRotation * Vector3(0, 1, 0), obstacle.radius * scale,
                            obstacle.height * std::fabs(transform->worldScale.y), color);
            }
        }
    }

    // the selected surfaces' and modifiers' volumes
    auto surfaces = scene->getComponentArray<NavMeshSurfaceComponent>();
    for (size_t i = 0; i < surfaces->size(); i++) {
        const Entity entity = surfaces->getEntity(i);
        const NavMeshSurfaceComponent& surface = surfaces->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !selected.count(entity) || surface.collectObjects != NavCollectObjects::VOLUME) continue;
        reserve(12);
        addBox(lines, transform->modelMatrix, surface.volumeCenter, surface.volumeSize, Vector4(0.3f, 0.8f, 1.0f, 1.0f));
    }
    auto modifiers = scene->getComponentArray<NavMeshModifierComponent>();
    for (size_t i = 0; i < modifiers->size(); i++) {
        const Entity entity = modifiers->getEntity(i);
        const NavMeshModifierComponent& modifier = modifiers->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        // a runtime volume is drawn with the area overrides
        if (!transform || !modifier.useVolume || !selected.count(entity) || (modifier.applyAtRuntime && settings.showAreaOverrides)) continue;
        reserve(12);
        const Vector4 color = modifier.overrideArea ? areaColor(modifier.area, 1.0f) : Vector4(0.8f, 0.8f, 0.8f, 1.0f);
        addBox(lines, transform->modelMatrix, modifier.volumeCenter, modifier.volumeSize, color);
    }

    // ---- S76 areas: runtime modifier volumes always, script volumes in play, in their area's colour:
    // solid while applied, faint while not (R41)
    areaOverrideDraws.clear();
    if (settings.showAreaOverrides && system) {
        for (const NavigationSystem::AreaOverrideStatus& status : system->getAreaOverrides()) {
            if (status.handle != 0 && !playing) continue;
            AreaOverrideDraw draw;
            draw.modifier = status.modifier;
            draw.handle = status.handle;
            draw.color = areaColor(status.area, status.applied ? 1.0f : DEACTIVATED_ALPHA);
            reserve(12);
            const size_t before = lines->getComponent<LineRendererComponent>().lines.size();
            addBox(lines, status.box, Vector3(), Vector3(1.0f, 1.0f, 1.0f), draw.color);
            draw.lines = lines->getComponent<LineRendererComponent>().lines.size() - before;
            areaOverrideDraws.push_back(draw);
        }
    }

    // the query of the Path Tester or visualize_navigation: start green, end red, a Complete
    // path magenta, a Partial one orange with a red gap from where it stops to the end asked
    if (const NavQueryDraw* query = NavigationEditor::get().getQueryDraw(sceneProject.id)) {
        const Vector3 up = sceneProject.sceneType == SceneType::SCENE_3D ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
        const std::vector<Vector3>& corners = query->path.corners;
        const bool partial = query->path.status == nav::PathStatus::Partial;
        reserve(corners.size() + 80);
        const Vector4 pathColor = partial ? Vector4(1.0f, 0.55f, 0.1f, 1.0f) : Vector4(1.0f, 0.2f, 0.8f, 1.0f);
        for (size_t i = 1; i < corners.size(); i++) lines->addLine(corners[i - 1], corners[i], pathColor);
        if (partial && query->hasTo && !corners.empty()) lines->addLine(corners.back(), query->to, Vector4(1.0f, 0.1f, 0.1f, 1.0f));
        auto marker = [&](const Vector3& at, const Vector4& color) {
            addCylinder(lines, at, up, 0.25f, 0.0f, color, 16);
            lines->addLine(at, at + up * 1.0f, color);
        };
        if (query->hasFrom) marker(query->from, Vector4(0.2f, 0.95f, 0.3f, 1.0f));
        if (query->hasTo) marker(query->to, Vector4(1.0f, 0.15f, 0.15f, 1.0f));
    }

    lines->updateLines();
    const size_t count = lines->getComponent<LineRendererComponent>().lines.size();
    lines->setVisible(count > 0);
    drawnLines += count;
}
