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

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* pathsName(NavOverlaySettings::Paths paths) {
        switch (paths) {
            case NavOverlaySettings::Paths::None: return "none";
            case NavOverlaySettings::Paths::Selected: return "selected";
            case NavOverlaySettings::Paths::All: return "all";
        }
        return "selected";
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
        if (entry.second.edges) entry.second.edges->setVisible(false);
    }
    for (auto& entry : grids) {
        for (Mesh* mesh : entry.second.meshes) mesh->setVisible(false);
        if (entry.second.lines) entry.second.lines->setVisible(false);
    }
    if (lines) lines->setVisible(false);
}

void NavigationOverlay::update(SceneProject& sceneProject, Camera* camera, const std::set<Entity>& selected,
                               const NavOverlaySettings& settings) {
    drawnTriangles = 0;
    drawnLines = 0;
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
    auto surfaceArray = scene->getComponentArray<NavMeshSurfaceComponent>();
    std::vector<Entity> surfaceEntities;
    for (size_t i = 0; i < surfaceArray->size(); i++) surfaceEntities.push_back(surfaceArray->getEntity(i));
    std::sort(surfaceEntities.begin(), surfaceEntities.end());
    for (Entity entity : surfaceEntities) {
        currentSurfaces.insert(entity);
        SurfaceDraw& draw = surfaces[entity];
        updateSurface(entity, draw, sceneProject, camera, settings, budget);
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
    // the tiles in the view: a rebuild when they, the navmesh or the area colours changed
    CameraComponent& cameraComponent = camera->getComponent<CameraComponent>();
    std::vector<uint32_t> key;
    nav::NavDebugGeometry geometry;
    uint32_t tileIndex = 0;
    system->getDebugGeometry(entity, [&](const AABB& box) {
        const bool visible = inFrustum(cameraComponent, box);
        if (visible) key.push_back(tileIndex);
        tileIndex++;
        return visible;
    }, triangleBudget, settings.showHeightMesh, geometry);
    const uint64_t version = nav::getSettingsVersion();
    const bool same = draw.revision == mesh->getRevision() && draw.visibleKey == key && draw.detail == settings.showHeightMesh &&
                      draw.settingsVersion == version;
    triangleBudget -= std::min(triangleBudget, geometry.triangleAreas.size());
    drawnTriangles += geometry.triangleAreas.size();
    if (!same) {
        draw.revision = mesh->getRevision();
        draw.visibleKey = key;
        draw.detail = settings.showHeightMesh;
        draw.settingsVersion = version;
        std::vector<Vector4> colors;
        colors.reserve(geometry.triangleAreas.size());
        for (uint8_t area : geometry.triangleAreas) colors.push_back(areaColor(area, 0.45f));
        const float lift = surface.plane == nav::NavPlane::XY ? 0.0f : 0.02f;
        fillMeshes(draw.meshes, geometry.triangles, colors, lift);

        const size_t boundary = geometry.boundaryEdges.size() / 6;
        const size_t inner = geometry.polygonEdges.size() / 6;
        const size_t tiles = settings.showTileBounds ? geometry.tileBounds.size() / 24 : 0;
        const unsigned int maxLines = (unsigned int)std::max<size_t>(1, boundary + inner + tiles * 12);
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
        for (size_t t = 0; t < tiles; t++) {
            const float* c = &geometry.tileBounds[t * 24];
            auto corner = [&](int i) { return Vector3(c[i * 3], c[i * 3 + 1], c[i * 3 + 2]); };
            const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) draw.edges->addLine(corner(e[0]), corner(e[1]), Vector4(1.0f, 1.0f, 1.0f, 0.25f));
        }
        draw.edges->updateLines();
    }
    for (Mesh* m : draw.meshes) m->setVisible(true);
    if (draw.edges) {
        draw.edges->setVisible(true);
        drawnLines += draw.edges->getComponent<LineRendererComponent>().lines.size();
    }
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
    if (draw.revision != map->getRevision() || draw.settingsVersion != version || moved || draw.meshes.empty()) {
        draw.revision = map->getRevision();
        draw.settingsVersion = version;
        std::copy(pose, pose + 7, draw.pose);
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
                const Vector4 color = areaColor(map->getArea(cell), map->getOverride(cell) ? 0.6f : 0.4f);
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
                    lines->addLine(previous, corner, Vector4(1.0f, 0.85f, 0.2f, 1.0f));
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
            const Vector4 color = link.enabled ? areaColor(link.area, 1.0f) : Vector4(0.6f, 0.6f, 0.6f, 0.8f);
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
        if (!transform || !modifier.useVolume || !selected.count(entity)) continue;
        reserve(12);
        const Vector4 color = modifier.overrideArea ? areaColor(modifier.area, 1.0f) : Vector4(0.8f, 0.8f, 0.8f, 1.0f);
        addBox(lines, transform->modelMatrix, modifier.volumeCenter, modifier.volumeSize, color);
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
