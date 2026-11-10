// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelSceneOverlay.h"

#include "VoxelBrushTool.h"
#include "VoxelOverlay.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/LineRenderer.h"
#include "object/Mesh.h"
#include "object/TerrainEdit.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainSurface.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VoxelSystem.h"
#include "util/ScopedDefaultEntityPool.h"
#include "voxel/TransvoxelMesher.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // Enough for a screenful of chunks in an editor view. Past it the finest blocks win, since the
    // map of visible blocks starts with them, and the panel's stats still say the true totals.
    constexpr size_t MAX_OVERLAY_LINES = 180000;
    // 16-bit indices, three unshared vertices per triangle
    constexpr size_t TINT_TRIANGLES_PER_MESH = 21845;
    constexpr size_t MAX_TINT_TRIANGLES = 12 * TINT_TRIANGLES_PER_MESH;
    constexpr float TINT_ALPHA = 0.55f;
    constexpr int COLLISION_RADIUS_BLOCKS = 4;
    constexpr double REDRAW_SECONDS = 0.1;

    uint64_t overlayMix(uint64_t hash, uint64_t value) {
        return voxel::fnv1a(&value, sizeof(value), hash);
    }

    Vector3 overlayWorldOf(const VoxelSystem& system, Entity owner, const voxel::Vec3d& local) {
        return system.localToWorld(owner, local);
    }

    voxel::Vec3d overlayLocalOf(const int32_t* position, float voxelSize) {
        return voxel::Vec3d{voxel::fixedToLocal(position[0], voxelSize), voxel::fixedToLocal(position[1], voxelSize), voxel::fixedToLocal(position[2], voxelSize)};
    }

    // the slot with the largest weight at a vertex (R13: sorted, so the first with weight)
    int dominantSlot(const voxel::MeshVertex& vertex) {
        int best = 0;
        for (int i = 1; i < 4; i++) {
            if (vertex.weights[i] > vertex.weights[best]) best = i;
        }
        return (vertex.slots >> (4 * best)) & 0xF;
    }

    class VoxelOverlayLines {
    public:
        explicit VoxelOverlayLines(Scene* scene): scene(scene) {
            ScopedDefaultEntityPool sys(*scene, EntityPool::System);
            lines = new LineRenderer(scene);
            scene->getSystem<RenderSystem>()->setEditorOverlay(lines->getEntity(), true);
        }
        ~VoxelOverlayLines() {
            if (isGone()) lines->setEntityOwned(false);
            delete lines;
        }
        VoxelOverlayLines(const VoxelOverlayLines&) = delete;
        VoxelOverlayLines& operator=(const VoxelOverlayLines&) = delete;

        // the buffer grows to what is drawn; `limit` keeps a whole mountain from becoming one
        void begin(size_t limit) {
            max = limit;
            lines->clearLines();
            count = 0;
        }
        void line(const Vector3& a, const Vector3& b, const Vector4& colour) {
            if (count >= max) return;
            lines->addLine(a, b, colour);
            count++;
        }
        void show(bool visible) { lines->setVisible(visible && count > 0); }
        size_t size() const { return count; }
        // Stop can sweep an entity the view made while playing: it is not ours to touch then
        bool isGone() const {
            const Entity entity = lines->getEntity();
            return !scene->isEntityCreated(entity) || !scene->findComponent<Transform>(entity) || !scene->getSystem<RenderSystem>()->isEditorOverlay(entity);
        }

    private:
        Scene* scene;
        LineRenderer* lines;
        size_t max = 100;
        size_t count = 0;
    };

    class VoxelOverlayTint {
    public:
        explicit VoxelOverlayTint(Scene* scene): scene(scene) {}
        ~VoxelOverlayTint() {
            for (Mesh* mesh : meshes) delete mesh;
        }
        VoxelOverlayTint(const VoxelOverlayTint&) = delete;
        VoxelOverlayTint& operator=(const VoxelOverlayTint&) = delete;

        // triangles: 3 world points and 3 colours each
        void fill(const std::vector<Vector3>& points, const std::vector<Vector4>& colours) {
            const size_t count = points.size() / 3;
            const size_t needed = (count + TINT_TRIANGLES_PER_MESH - 1) / TINT_TRIANGLES_PER_MESH;
            while (meshes.size() < needed) meshes.push_back(newMesh());
            while (meshes.size() > needed) {
                delete meshes.back();
                meshes.pop_back();
            }
            auto meshSystem = scene->getSystem<MeshSystem>();
            for (size_t m = 0; m < needed; m++) {
                MeshComponent& mesh = meshes[m]->getComponent<MeshComponent>();
                const size_t first = m * TINT_TRIANGLES_PER_MESH;
                const size_t n = std::min(TINT_TRIANGLES_PER_MESH, count - first);
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
                    const Vector3& a = points[(first + t) * 3];
                    const Vector3& b = points[(first + t) * 3 + 1];
                    const Vector3& c = points[(first + t) * 3 + 2];
                    Vector3 up = (b - a).crossProduct(c - a);
                    up = up.length() > 1e-8f ? up.normalized() : Vector3(0, 1, 0);
                    for (int corner = 0; corner < 3; corner++) {
                        // a little off the surface: it must not fight the chunk it tints
                        mesh.buffer.addVector3(position, points[(first + t) * 3 + static_cast<size_t>(corner)] + up * 0.01f);
                        mesh.buffer.addVector2(texcoord, Vector2(0.0f, 0.0f));
                        mesh.buffer.addVector3(normal, up);
                        mesh.buffer.addVector4(color, colours[(first + t) * 3 + static_cast<size_t>(corner)]);
                        indices.push_back(static_cast<uint16_t>(indices.size()));
                    }
                }
                mesh.indices.clear();
                mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), reinterpret_cast<char*>(indices.data()), sizeof(uint16_t));
                meshSystem->calculateMeshAABB(mesh);
                if (mesh.loaded) mesh.needReload = true;
                meshes[m]->setVisible(true);
            }
        }
        void hide() {
            for (Mesh* mesh : meshes) mesh->setVisible(false);
        }

    private:
        Mesh* newMesh() {
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

        Scene* scene;
        std::vector<Mesh*> meshes;
    };

    struct VoxelSceneOverlayState {
        std::unique_ptr<VoxelOverlayLines> decal;
        std::unique_ptr<VoxelOverlayLines> plane;
        std::unique_ptr<VoxelOverlayLines> overlay;
        std::unique_ptr<VoxelOverlayTint> tint;
        uint64_t decalKey = 0;
        uint64_t planeKey = 0;
        uint64_t overlayKey = 0;
        size_t overlayLines = 0;
        size_t overlayTriangles = 0;
        double sinceRedraw = REDRAW_SECONDS;
    };

    std::unordered_map<Scene*, VoxelSceneOverlayState>& overlayStates() {
        static std::unordered_map<Scene*, VoxelSceneOverlayState> map;
        return map;
    }

    struct VoxelSlicePlaneState {
        bool on = false;
        uint32_t sceneId = 0;
        Entity owner = NULL_ENTITY;
        VoxelSliceAxis axis = VoxelSliceAxis::Y;
        int position = 0;
        uint64_t revision = 1;
    };
    VoxelSlicePlaneState slicePlane;

    void addBoxLines(VoxelOverlayLines& lines, const VoxelSystem& system, Entity owner, const voxel::Vec3d& low, const voxel::Vec3d& high, const Vector4& colour) {
        const auto corner = [&](int i) {
            return overlayWorldOf(system, owner, voxel::Vec3d{(i & 1) ? high.x : low.x, (i & 2) ? high.y : low.y, (i & 4) ? high.z : low.z});
        };
        const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) lines.line(corner(edge[0]), corner(edge[1]), colour);
    }

    void drawVoxelDecal(Scene* scene, VoxelSceneOverlayState& state) {
        VoxelBrushTool& tool = VoxelBrushTool::get();
        const bool mine = tool.getProject() && tool.isEditingScene(scene);
        if (!mine || !tool.decalVisible() || tool.decalAlpha() <= 0.0f) {
            if (state.decal) state.decal->show(false);
            state.decalKey = 0;
            return;
        }
        if (state.decal && state.decalKey == tool.decalRevision()) return;
        std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
        if (!system) return;
        if (!state.decal) state.decal = std::make_unique<VoxelOverlayLines>(scene);
        const VoxelDecal& decal = tool.decal();
        const Vector4 colour(decal.colour.r, decal.colour.g, decal.colour.b, tool.decalAlpha());
        // a thinner look for the wire, so the ring on the ground reads first
        const Vector4 wire(decal.colour.r, decal.colour.g, decal.colour.b, tool.decalAlpha() * 0.55f);
        state.decal->begin((decal.ring.size() + decal.wire.size()) / 2 + 8);
        for (size_t i = 0; i + 1 < decal.ring.size(); i += 2) {
            state.decal->line(overlayWorldOf(*system, tool.getTargetEntity(), decal.ring[i]), overlayWorldOf(*system, tool.getTargetEntity(), decal.ring[i + 1]), colour);
        }
        for (size_t i = 0; i + 1 < decal.wire.size(); i += 2) {
            state.decal->line(overlayWorldOf(*system, tool.getTargetEntity(), decal.wire[i]), overlayWorldOf(*system, tool.getTargetEntity(), decal.wire[i + 1]), wire);
        }
        state.decal->show(true);
        state.decalKey = tool.decalRevision();
    }

    void drawVoxelPlane(Scene* scene, uint32_t sceneId, VoxelSceneOverlayState& state) {
        if (!slicePlane.on || slicePlane.sceneId != sceneId) {
            if (state.plane) state.plane->show(false);
            state.planeKey = 0;
            return;
        }
        std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
        voxel::Store* store = system ? system->getStore(slicePlane.owner) : nullptr;
        if (!store) {
            if (state.plane) state.plane->show(false);
            return;
        }
        if (state.plane && state.planeKey == slicePlane.revision) return;
        if (!state.plane) state.plane = std::make_unique<VoxelOverlayLines>(scene);
        const voxel::VolumeDesc& desc = store->desc();
        const double voxelSize = desc.voxelSize;
        const voxel::Vec3d high{desc.dimensions.x * voxelSize, desc.dimensions.y * voxelSize, desc.dimensions.z * voxelSize};
        const double at = slicePlane.position * voxelSize;
        const int axis = static_cast<int>(slicePlane.axis);
        // the plane's outline, and a light grid of block lines so it reads as a sheet
        const Vector4 outline(1.0f, 0.77f, 0.22f, 1.0f);
        const Vector4 grid(1.0f, 0.77f, 0.22f, 0.3f);
        state.plane->begin(512);
        const auto point = [&](double u, double v) {
            voxel::Vec3d p;
            switch (axis) {
                case 0: p = voxel::Vec3d{at, u, v}; break;
                case 1: p = voxel::Vec3d{u, at, v}; break;
                default: p = voxel::Vec3d{u, v, at}; break;
            }
            return overlayWorldOf(*system, slicePlane.owner, p);
        };
        const double maxU = axis == 0 ? high.y : high.x;
        const double maxV = axis == 2 ? high.y : high.z;
        state.plane->line(point(0.0, 0.0), point(maxU, 0.0), outline);
        state.plane->line(point(maxU, 0.0), point(maxU, maxV), outline);
        state.plane->line(point(maxU, maxV), point(0.0, maxV), outline);
        state.plane->line(point(0.0, maxV), point(0.0, 0.0), outline);
        const double step = voxel::BLOCK_SIZE * voxelSize;
        for (double u = step; u < maxU; u += step) state.plane->line(point(u, 0.0), point(u, maxV), grid);
        for (double v = step; v < maxV; v += step) state.plane->line(point(0.0, v), point(maxU, v), grid);
        state.plane->show(true);
        state.planeKey = slicePlane.revision;
    }

    // What the overlays drew from: a change here is a redraw
    uint64_t overlayKeyOf(VoxelSystem& system, Entity owner, VoxelOverlay overlay) {
        uint64_t hash = overlayMix(voxel::FNV_OFFSET, static_cast<uint64_t>(overlay));
        hash = overlayMix(hash, owner);
        const voxel::Store* store = system.getStore(owner);
        hash = overlayMix(hash, store ? store->version() : 0);
        for (const auto& item : system.getVisibleBlocks(owner)) {
            hash = overlayMix(hash, item.second.windowVersion);
            hash = overlayMix(hash, item.second.sideMask);
            hash = overlayMix(hash, static_cast<uint64_t>(item.first.level) | (static_cast<uint64_t>(item.first.x) << 8) | (static_cast<uint64_t>(item.first.y) << 24) | (static_cast<uint64_t>(item.first.z) << 40));
        }
        const Matrix4 model = system.modelMatrixOf(owner);
        hash = voxel::fnv1a(&model, sizeof(model), hash);
        return hash;
    }

    void drawMeshLines(VoxelOverlayLines& lines, VoxelSystem& system, Entity owner, const voxel::MeshOut& mesh, float voxelSize, const Vector4& colour) {
        for (size_t i = 0; i + 2 < mesh.indices.size() && lines.size() < MAX_OVERLAY_LINES; i += 3) {
            const Vector3 a = overlayWorldOf(system, owner, overlayLocalOf(mesh.vertices[mesh.indices[i]].position, voxelSize));
            const Vector3 b = overlayWorldOf(system, owner, overlayLocalOf(mesh.vertices[mesh.indices[i + 1]].position, voxelSize));
            const Vector3 c = overlayWorldOf(system, owner, overlayLocalOf(mesh.vertices[mesh.indices[i + 2]].position, voxelSize));
            lines.line(a, b, colour);
            lines.line(b, c, colour);
            lines.line(c, a, colour);
        }
    }

    void drawVoxelOverlayOf(Scene* scene, VoxelSceneOverlayState& state, VoxelSystem& system, Entity owner, VoxelOverlay overlay, std::vector<Vector3>& tintPoints, std::vector<Vector4>& tintColours) {
        voxel::Store* store = system.getStore(owner);
        if (!store) return;
        const float voxelSize = store->desc().voxelSize;
        const std::map<voxel::BlockKey, voxel::MeshId> blocks = system.getVisibleBlocks(owner);
        const std::vector<std::shared_ptr<const voxel::MeshOut>> meshes = system.getVisibleMeshes(owner);
        // the system keeps both by the same key order; a set activating this frame can leave them apart
        const bool paired = blocks.size() == meshes.size();

        switch (overlay) {
            case VoxelOverlay::None:
                break;
            case VoxelOverlay::Wireframe:
                for (const auto& mesh : meshes) {
                    if (mesh) drawMeshLines(*state.overlay, system, owner, *mesh, voxelSize, Vector4(0.1f, 1.0f, 0.9f, 1.0f));
                }
                break;
            case VoxelOverlay::Blocks:
                for (const auto& item : blocks) {
                    const VoxelSceneOverlay::Colour colour = VoxelSceneOverlay::levelColour(item.first.level);
                    const double span = static_cast<double>(voxel::BLOCK_SIZE << item.first.level) * voxelSize;
                    const voxel::Vec3d low{item.first.x * span, item.first.y * span, item.first.z * span};
                    addBoxLines(*state.overlay, system, owner, low, voxel::Vec3d{low.x + span, low.y + span, low.z + span}, Vector4(colour.r, colour.g, colour.b, 1.0f));
                }
                break;
            case VoxelOverlay::LodColours:
            case VoxelOverlay::DominantMaterial: {
                auto block = blocks.begin();
                for (const auto& mesh : meshes) {
                    const int level = paired && block != blocks.end() ? block->first.level : 0;
                    if (block != blocks.end()) ++block;
                    if (!mesh) continue;
                    const VoxelSceneOverlay::Colour levelTint = VoxelSceneOverlay::levelColour(level);
                    for (size_t i = 0; i + 2 < mesh->indices.size() && tintPoints.size() / 3 < MAX_TINT_TRIANGLES; i += 3) {
                        for (int corner = 0; corner < 3; corner++) {
                            const voxel::MeshVertex& vertex = mesh->vertices[mesh->indices[i + static_cast<size_t>(corner)]];
                            tintPoints.push_back(overlayWorldOf(system, owner, overlayLocalOf(vertex.position, voxelSize)));
                            if (overlay == VoxelOverlay::LodColours) {
                                tintColours.emplace_back(levelTint.r, levelTint.g, levelTint.b, TINT_ALPHA);
                            } else {
                                const VoxelSlotColor slot = voxelSlotColor(dominantSlot(vertex));
                                tintColours.emplace_back(slot.r / 255.0f, slot.g / 255.0f, slot.b / 255.0f, TINT_ALPHA);
                            }
                        }
                    }
                }
                break;
            }
            case VoxelOverlay::Collision: {
                // the level-0 collision geometry around the view's pivot, what a body nearby collides with
                Vector3 centre = scene->findComponent<Transform>(owner) ? scene->getComponent<Transform>(owner).worldPosition : Vector3(0, 0, 0);
                if (const Entity camera = scene->getCamera(); camera != NULL_ENTITY) {
                    if (const CameraComponent* component = scene->findComponent<CameraComponent>(camera)) centre = component->worldTarget;
                }
                const float radius = static_cast<float>(COLLISION_RADIUS_BLOCKS * voxel::BLOCK_SIZE) * voxelSize;
                const AABB bounds(centre.x - radius, centre.y - radius, centre.z - radius, centre.x + radius, centre.y + radius, centre.z + radius);
                std::vector<Vector3> vertices;
                std::vector<uint32_t> indices;
                system.appendNavTriangles(bounds, vertices, indices);
                const Vector4 colour(0.35f, 1.0f, 0.35f, 1.0f);
                for (size_t i = 0; i + 2 < indices.size() && state.overlay->size() < MAX_OVERLAY_LINES; i += 3) {
                    const Vector3& a = vertices[indices[i]];
                    const Vector3& b = vertices[indices[i + 1]];
                    const Vector3& c = vertices[indices[i + 2]];
                    state.overlay->line(a, b, colour);
                    state.overlay->line(b, c, colour);
                    state.overlay->line(c, a, colour);
                }
                break;
            }
            case VoxelOverlay::Holes: {
                // the attached terrain's hole map in place (T3 R30): each run of hole texels, on its surface
                const VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(owner);
                if (!component || component->terrain == NULL_ENTITY) break;
                TerrainComponent* terrain = scene->findComponent<TerrainComponent>(component->terrain);
                const Transform* terrainTransform = scene->findComponent<Transform>(component->terrain);
                if (!terrain || !terrainTransform) break;
                TerrainComponentStore holes(scene, component->terrain, *terrain);
                const terraincore::MapId holeMap{terraincore::MapKind::Hole, 0};
                const terraincore::MapFormat format = holes.format(holeMap);
                if (!format.exists) break;
                const int width = format.bounds.w, height = format.bounds.h;
                std::vector<float> texels(static_cast<size_t>(width) * static_cast<size_t>(height));
                if (!holes.read(holeMap, format.bounds, texels)) break;
                TerrainSurfaceSnapshot snapshot;
                snapshotTerrainSurface(*terrain, snapshot);
                const float half = terrain->terrainSize / static_cast<float>(width) * 0.5f;
                const Vector4 colour(1.0f, 0.2f, 0.9f, 1.0f);
                const auto at = [&](float x, float z) {
                    return terrainTransform->modelMatrix * Vector3(x, terraincore::sampleHeight(snapshot.view, x, z) + 0.05f, z);
                };
                for (int y = 0; y < height && state.overlay->size() < MAX_OVERLAY_LINES; y++) {
                    for (int x = 0; x < width; x++) {
                        if (texels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] != terraincore::HOLE_VALUE) continue;
                        const int first = x;
                        while (x + 1 < width && texels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x) + 1] == terraincore::HOLE_VALUE) x++;
                        const Vector2 low = holes.worldOfTexel(holeMap, first, y);
                        const Vector2 high = holes.worldOfTexel(holeMap, x, y);
                        const float x0 = low.x - half, x1 = high.x + half, z0 = low.y - half, z1 = low.y + half;
                        state.overlay->line(at(x0, z0), at(x1, z0), colour);
                        state.overlay->line(at(x1, z0), at(x1, z1), colour);
                        state.overlay->line(at(x1, z1), at(x0, z1), colour);
                        state.overlay->line(at(x0, z1), at(x0, z0), colour);
                    }
                }
                break;
            }
        }
    }

    void drawVoxelOverlays(Scene* scene, VoxelSceneOverlayState& state, double dt) {
        std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
        auto array = scene->getComponentArray<VoxelVolumeComponent>();
        uint64_t key = voxelOverlayRevision();
        bool any = false;
        if (system) {
            for (size_t i = 0; i < array->size(); i++) {
                const Entity owner = array->getEntity(i);
                const VoxelOverlay overlay = getVoxelOverlay(owner);
                if (overlay == VoxelOverlay::None) continue;
                any = true;
                key = overlayMix(key, overlayKeyOf(*system, owner, overlay));
            }
        }
        if (!any) {
            if (state.overlay) state.overlay->show(false);
            if (state.tint) state.tint->hide();
            state.overlayKey = 0;
            state.overlayLines = 0;
            state.overlayTriangles = 0;
            return;
        }
        state.sinceRedraw += dt;
        if (key == state.overlayKey || state.sinceRedraw < REDRAW_SECONDS) return;
        state.sinceRedraw = 0.0;
        state.overlayKey = key;
        if (!state.overlay) state.overlay = std::make_unique<VoxelOverlayLines>(scene);
        state.overlay->begin(MAX_OVERLAY_LINES);
        std::vector<Vector3> points;
        std::vector<Vector4> colours;
        for (size_t i = 0; i < array->size(); i++) {
            const Entity owner = array->getEntity(i);
            const VoxelOverlay overlay = getVoxelOverlay(owner);
            if (overlay != VoxelOverlay::None) drawVoxelOverlayOf(scene, state, *system, owner, overlay, points, colours);
        }
        state.overlay->show(true);
        state.overlayLines = state.overlay->size();
        state.overlayTriangles = points.size() / 3;
        if (points.empty()) {
            if (state.tint) state.tint->hide();
        } else {
            if (!state.tint) state.tint = std::make_unique<VoxelOverlayTint>(scene);
            state.tint->fill(points, colours);
        }
    }

}

void editor::setVoxelSlicePlane(uint32_t sceneId, Entity owner, VoxelSliceAxis axis, int position) {
    if (slicePlane.on && slicePlane.sceneId == sceneId && slicePlane.owner == owner && slicePlane.axis == axis && slicePlane.position == position) return;
    slicePlane.on = true;
    slicePlane.sceneId = sceneId;
    slicePlane.owner = owner;
    slicePlane.axis = axis;
    slicePlane.position = position;
    slicePlane.revision++;
}

void editor::clearVoxelSlicePlane() {
    if (!slicePlane.on) return;
    slicePlane.on = false;
    slicePlane.revision++;
}

VoxelSceneOverlay::Colour VoxelSceneOverlay::levelColour(int level) {
    switch (level) {
        case 0: return {0.2f, 0.9f, 0.3f};
        case 1: return {0.95f, 0.9f, 0.2f};
        case 2: return {1.0f, 0.55f, 0.1f};
        default: return {0.95f, 0.2f, 0.2f};
    }
}

void VoxelSceneOverlay::frame(Project& project) {
    if (project.isAnyScenePlaying()) {
        for (auto& item : overlayStates()) {
            if (item.second.decal) item.second.decal->show(false);
            if (item.second.plane) item.second.plane->show(false);
            if (item.second.overlay) item.second.overlay->show(false);
            if (item.second.tint) item.second.tint->hide();
        }
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;

    const bool overlaysOn = anyVoxelOverlay();
    VoxelBrushTool& tool = VoxelBrushTool::get();
    for (SceneProject& sceneProject : project.getScenes()) {
        Scene* scene = sceneProject.scene;
        if (!scene || !sceneProject.sceneRender) continue;
        const auto found = overlayStates().find(scene);
        // nothing on and nothing made: this scene pays one lookup (R58)
        if (found == overlayStates().end() && !overlaysOn && !slicePlane.on && !tool.isArmed()) continue;
        VoxelSceneOverlayState& state = overlayStates()[scene];
        drawVoxelDecal(scene, state);
        drawVoxelPlane(scene, sceneProject.id, state);
        drawVoxelOverlays(scene, state, dt);
    }
}

void VoxelSceneOverlay::release(Scene* scene) {
    overlayStates().erase(scene);
}

size_t VoxelSceneOverlay::overlayLineCount(Scene* scene) {
    const auto found = overlayStates().find(scene);
    return found == overlayStates().end() ? 0 : found->second.overlayLines;
}

size_t VoxelSceneOverlay::overlayTriangleCount(Scene* scene) {
    const auto found = overlayStates().find(scene);
    return found == overlayStates().end() ? 0 : found->second.overlayTriangles;
}
