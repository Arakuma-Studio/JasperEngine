// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The collection policy follows OpenEngine's NavigationSourceCollector (Engine/Modules/
// PathfindingECS/Source/NavigationSourceCollector.cpp), Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License, as a reference; the code is our own. See NOTICE.md.

#include "NavBakeCollector.h"
#include "subsystem/RenderSystem.h"

#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "subsystem/FoliageSystem.h"
#include "component/MeshComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "terrain/TerrainHoles.h"
#include "component/Transform.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "texture/TextureData.h"
#include "util/MeshTriangles.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "component/CharacterController2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    constexpr float PI = 3.14159265358979323846f;
    // the largest terrain lattice a bake samples per side
    constexpr int MAX_TERRAIN_SAMPLES = 4097;
    // tessellation of round colliders
    constexpr int ROUND_SEGMENTS = 16;
    constexpr int ROUND_RINGS = 8;
    // 2D circles become 16-gons (spec R21)
    constexpr int CIRCLE_SIDES = 16;

    // ---------------------------------------------------------------- geometry helpers

    struct Mesh {
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;
    };

#ifdef JASPERENGINE_PHYSICS_3D
    void addBox(Mesh& mesh, const Vector3& half) {
        const uint32_t base = (uint32_t)mesh.vertices.size();
        for (int i = 0; i < 8; i++)
            mesh.vertices.push_back(Vector3((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z));
        // counter-clockwise from outside (y up)
        const uint32_t faces[12][3] = {
            {0, 1, 5}, {0, 5, 4},   // -y
            {2, 6, 7}, {2, 7, 3},   // +y
            {0, 4, 6}, {0, 6, 2},   // -x
            {1, 3, 7}, {1, 7, 5},   // +x
            {0, 2, 3}, {0, 3, 1},   // -z
            {4, 5, 7}, {4, 7, 6}    // +z
        };
        for (const auto& f : faces) {
            mesh.indices.push_back(base + f[0]);
            mesh.indices.push_back(base + f[1]);
            mesh.indices.push_back(base + f[2]);
        }
    }
#endif

    // a closed surface of revolution about y: rings from bottom to top (radius, y)
    void addRevolution(Mesh& mesh, const std::vector<std::pair<float, float>>& profile) {
        const uint32_t base = (uint32_t)mesh.vertices.size();
        const int rings = (int)profile.size();
        for (const auto& ring : profile) {
            for (int s = 0; s < ROUND_SEGMENTS; s++) {
                const float angle = 2.0f * PI * (float)s / (float)ROUND_SEGMENTS;
                mesh.vertices.push_back(Vector3(ring.first * std::cos(angle), ring.second, ring.first * std::sin(angle)));
            }
        }
        for (int r = 0; r + 1 < rings; r++) {
            for (int s = 0; s < ROUND_SEGMENTS; s++) {
                const uint32_t a = base + (uint32_t)(r * ROUND_SEGMENTS + s);
                const uint32_t b = base + (uint32_t)(r * ROUND_SEGMENTS + (s + 1) % ROUND_SEGMENTS);
                const uint32_t c = a + ROUND_SEGMENTS;
                const uint32_t d = b + ROUND_SEGMENTS;
                // outward: a -> c -> d faces out for angles growing from +x towards +z
                mesh.indices.insert(mesh.indices.end(), {a, c, d, a, d, b});
            }
        }
        // caps
        const uint32_t bottom = (uint32_t)mesh.vertices.size();
        mesh.vertices.push_back(Vector3(0.0f, profile.front().second, 0.0f));
        const uint32_t top = (uint32_t)mesh.vertices.size();
        mesh.vertices.push_back(Vector3(0.0f, profile.back().second, 0.0f));
        for (int s = 0; s < ROUND_SEGMENTS; s++) {
            const uint32_t a = base + (uint32_t)s;
            const uint32_t b = base + (uint32_t)((s + 1) % ROUND_SEGMENTS);
            mesh.indices.insert(mesh.indices.end(), {bottom, a, b});
            const uint32_t c = base + (uint32_t)((rings - 1) * ROUND_SEGMENTS + s);
            const uint32_t d = base + (uint32_t)((rings - 1) * ROUND_SEGMENTS + (s + 1) % ROUND_SEGMENTS);
            mesh.indices.insert(mesh.indices.end(), {top, d, c});
        }
    }

#ifdef JASPERENGINE_PHYSICS_3D
    void addSphere(Mesh& mesh, float radius) {
        std::vector<std::pair<float, float>> profile;
        for (int r = 1; r < ROUND_RINGS; r++) {
            const float phi = PI * (float)r / (float)ROUND_RINGS - PI * 0.5f;
            profile.emplace_back(radius * std::cos(phi), radius * std::sin(phi));
        }
        addRevolution(mesh, profile);
    }
#endif

    // two half spheres (radii bottom and top) joined over 2 * halfHeight
#ifdef JASPERENGINE_PHYSICS_3D
    void addCapsule(Mesh& mesh, float halfHeight, float bottomRadius, float topRadius) {
        std::vector<std::pair<float, float>> profile;
        const int half = ROUND_RINGS / 2;
        for (int r = 1; r <= half; r++) {
            const float phi = PI * 0.5f * (float)r / (float)half - PI * 0.5f;
            profile.emplace_back(bottomRadius * std::cos(phi), -halfHeight + bottomRadius * std::sin(phi));
        }
        for (int r = 0; r < half; r++) {
            const float phi = PI * 0.5f * (float)r / (float)half;
            profile.emplace_back(topRadius * std::cos(phi), halfHeight + topRadius * std::sin(phi));
        }
        addRevolution(mesh, profile);
    }
#endif

    void addCylinder(Mesh& mesh, float halfHeight, float radius) {
        addRevolution(mesh, {{radius, -halfHeight}, {radius, halfHeight}});
    }

    // ---- a 3D convex hull (QuickHull) of a point cloud, its faces wound outwards
#ifdef JASPERENGINE_PHYSICS_3D
    Mesh convexHull(const std::vector<Vector3>& points) {
        Mesh hull;
        if (points.size() < 4)
            return hull;
        Vector3 bmin = points[0], bmax = points[0];
        for (const Vector3& p : points) {
            bmin.makeFloor(p);
            bmax.makeCeil(p);
        }
        const float epsilon = std::max(1e-6f, 1e-5f * (bmax - bmin).length());

        // the initial tetrahedron: the farthest pair, the farthest from their line, from their plane
        int i0 = 0, i1 = 0;
        float best = -1.0f;
        for (int axis = 0; axis < 3; axis++) {
            int lo = 0, hi = 0;
            for (int i = 0; i < (int)points.size(); i++) {
                if (points[(size_t)i][(unsigned)axis] < points[(size_t)lo][(unsigned)axis]) lo = i;
                if (points[(size_t)i][(unsigned)axis] > points[(size_t)hi][(unsigned)axis]) hi = i;
            }
            const float d = points[(size_t)lo].distance(points[(size_t)hi]);
            if (d > best) {
                best = d;
                i0 = lo;
                i1 = hi;
            }
        }
        if (best <= epsilon)
            return hull;
        const Vector3 lineDir = (points[(size_t)i1] - points[(size_t)i0]).normalized();
        int i2 = -1;
        best = epsilon;
        for (int i = 0; i < (int)points.size(); i++) {
            const Vector3 v = points[(size_t)i] - points[(size_t)i0];
            const float d = (v - lineDir * v.dotProduct(lineDir)).length();
            if (d > best) {
                best = d;
                i2 = i;
            }
        }
        if (i2 < 0)
            return hull;
        const Vector3 planeNormal = (points[(size_t)i1] - points[(size_t)i0]).crossProduct(points[(size_t)i2] - points[(size_t)i0]).normalized();
        int i3 = -1;
        best = epsilon;
        for (int i = 0; i < (int)points.size(); i++) {
            const float d = std::fabs((points[(size_t)i] - points[(size_t)i0]).dotProduct(planeNormal));
            if (d > best) {
                best = d;
                i3 = i;
            }
        }
        if (i3 < 0)
            return hull;

        struct Face {
            int v[3];
            Vector3 normal;
            float offset = 0.0f;
            std::vector<int> outside;
            bool alive = true;
        };
        std::vector<Face> faces;
        auto makeFace = [&](int a, int b, int c) {
            Face face;
            face.v[0] = a;
            face.v[1] = b;
            face.v[2] = c;
            face.normal = (points[(size_t)b] - points[(size_t)a]).crossProduct(points[(size_t)c] - points[(size_t)a]).normalized();
            face.offset = face.normal.dotProduct(points[(size_t)a]);
            return face;
        };
        const Vector3 centroid = (points[(size_t)i0] + points[(size_t)i1] + points[(size_t)i2] + points[(size_t)i3]) * 0.25f;
        const int tetra[4][3] = {{i0, i1, i2}, {i0, i3, i1}, {i1, i3, i2}, {i2, i3, i0}};
        for (const auto& t : tetra) {
            Face face = makeFace(t[0], t[1], t[2]);
            if (face.normal.dotProduct(centroid) - face.offset > 0.0f)
                face = makeFace(t[0], t[2], t[1]);
            faces.push_back(face);
        }
        auto distance = [&](const Face& face, int p) { return face.normal.dotProduct(points[(size_t)p]) - face.offset; };
        for (int p = 0; p < (int)points.size(); p++) {
            if (p == i0 || p == i1 || p == i2 || p == i3)
                continue;
            for (Face& face : faces) {
                if (distance(face, p) > epsilon) {
                    face.outside.push_back(p);
                    break;
                }
            }
        }

        for (size_t guard = 0; guard < points.size() * 4; guard++) {
            int faceIndex = -1;
            for (int f = 0; f < (int)faces.size(); f++) {
                if (faces[(size_t)f].alive && !faces[(size_t)f].outside.empty()) {
                    faceIndex = f;
                    break;
                }
            }
            if (faceIndex < 0)
                break;
            const Face& seed = faces[(size_t)faceIndex];
            int eye = seed.outside[0];
            float far = distance(seed, eye);
            for (int p : seed.outside) {
                const float d = distance(seed, p);
                if (d > far) {
                    far = d;
                    eye = p;
                }
            }
            std::vector<int> visible;
            for (int f = 0; f < (int)faces.size(); f++) {
                if (faces[(size_t)f].alive && distance(faces[(size_t)f], eye) > epsilon)
                    visible.push_back(f);
            }
            std::unordered_set<uint64_t> edges;
            auto key = [](int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; };
            for (int f : visible) {
                for (int e = 0; e < 3; e++)
                    edges.insert(key(faces[(size_t)f].v[e], faces[(size_t)f].v[(e + 1) % 3]));
            }
            std::vector<int> orphans;
            std::vector<std::pair<int, int>> horizon;
            for (int f : visible) {
                Face& face = faces[(size_t)f];
                for (int e = 0; e < 3; e++) {
                    const int a = face.v[e], b = face.v[(e + 1) % 3];
                    if (!edges.count(key(b, a)))
                        horizon.emplace_back(a, b);
                }
                face.alive = false;
                for (int p : face.outside) {
                    if (p != eye)
                        orphans.push_back(p);
                }
                face.outside.clear();
            }
            const size_t first = faces.size();
            for (const auto& edge : horizon)
                faces.push_back(makeFace(edge.first, edge.second, eye));
            for (int p : orphans) {
                for (size_t f = first; f < faces.size(); f++) {
                    if (distance(faces[f], p) > epsilon) {
                        faces[f].outside.push_back(p);
                        break;
                    }
                }
            }
        }

        std::unordered_map<int, uint32_t> remap;
        for (const Face& face : faces) {
            if (!face.alive)
                continue;
            for (int k = 0; k < 3; k++) {
                auto found = remap.find(face.v[k]);
                if (found == remap.end()) {
                    found = remap.emplace(face.v[k], (uint32_t)hull.vertices.size()).first;
                    hull.vertices.push_back(points[(size_t)face.v[k]]);
                }
                hull.indices.push_back(found->second);
            }
        }
        return hull;
    }
#endif

    // ---- a 2D convex hull (Andrew's monotone chain), counter-clockwise
    std::vector<Vector2> convexHull2D(std::vector<Vector2> points) {
        std::sort(points.begin(), points.end(), [](const Vector2& a, const Vector2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        points.erase(std::unique(points.begin(), points.end(), [](const Vector2& a, const Vector2& b) { return a.x == b.x && a.y == b.y; }),
                     points.end());
        if (points.size() < 3)
            return points;
        auto cross = [](const Vector2& o, const Vector2& a, const Vector2& b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        };
        std::vector<Vector2> hull(points.size() * 2);
        size_t k = 0;
        for (size_t i = 0; i < points.size(); i++) {
            while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) k--;
            hull[k++] = points[i];
        }
        for (size_t i = points.size() - 1, t = k + 1; i > 0; i--) {
            while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) k--;
            hull[k++] = points[i - 1];
        }
        hull.resize(k - 1);
        return hull;
    }

    // a convex polygon pushed out by `offset` (mitred corners)
#ifdef JASPERENGINE_PHYSICS_2D
    std::vector<Vector2> offsetPolygon(const std::vector<Vector2>& polygon, float offset) {
        if (offset <= 0.0f || polygon.size() < 3)
            return polygon;
        std::vector<Vector2> out;
        const size_t n = polygon.size();
        for (size_t i = 0; i < n; i++) {
            const Vector2& prev = polygon[(i + n - 1) % n];
            const Vector2& p = polygon[i];
            const Vector2& next = polygon[(i + 1) % n];
            const Vector2 e0 = (p - prev).normalized(), e1 = (next - p).normalized();
            // outward normals of a counter-clockwise polygon
            const Vector2 n0(e0.y, -e0.x), n1(e1.y, -e1.x);
            Vector2 bisector = (n0 + n1);
            const float length = bisector.length();
            if (length < 1e-6f) {
                out.push_back(p + n0 * offset);
                continue;
            }
            bisector = bisector * (1.0f / length);
            const float cosHalf = std::max(0.25f, bisector.dotProduct(n0));
            out.push_back(p + bisector * (offset / cosHalf));
        }
        return out;
    }
#endif

    // a component's area (an int) as an area index
    uint8_t areaIndex(int area) {
        return (uint8_t)std::clamp(area, 0, AREA_COUNT - 1);
    }

    Matrix4 trsMatrix(const Vector3& position, const Quaternion& rotation, const Vector3& scale) {
        return Matrix4::translateMatrix(position) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    }

    // ---------------------------------------------------------------- the collection

    struct Policy {
        bool include = true;
        uint8_t area = AREA_WALKABLE;
    };

    class Collector {
    public:
        Collector(Scene* scene, Entity surface, const NavMeshSurfaceComponent& component, NavCollectStats& stats)
            : scene(scene), surfaceEntity(surface), surface(component), stats(stats) {
#ifdef JASPERENGINE_EDITOR
            renderSystem = scene->getSystem<RenderSystem>().get();
#endif
        }

        std::shared_ptr<NavBakeInput> run(std::string& error);

    private:
        Scene* scene;
        Entity surfaceEntity;
        const NavMeshSurfaceComponent& surface;
        NavCollectStats& stats;
        NavFrame frame;
        Matrix4 navFromWorld;
        std::shared_ptr<NavBakeInput> input;
        std::unordered_map<Entity, Policy> policies;
        // nav-space bounds of what was collected
        Vector3 boundsMin = Vector3(std::numeric_limits<float>::max());
        Vector3 boundsMax = Vector3(-std::numeric_limits<float>::max());
        // the volume box in nav space (Volume mode)
        Vector3 volumeMin, volumeMax;

        bool isDescendant(Entity entity, Entity ancestor) const;
        bool inScope(Entity entity) const;
        const Policy& policyOf(Entity entity);
        bool collectable(Entity entity, Policy& policy);
#ifdef JASPERENGINE_EDITOR
        // the editor's overlay flags (editor builds only: a game has no overlays)
        RenderSystem* renderSystem = nullptr;
#endif
        void grow(const Vector3& nav);
        void addTriangles(const Mesh& mesh, const Matrix4& toNav, uint8_t area);
        void addMeshComponent(Entity entity, const Matrix4& worldOf, uint8_t area);
        void collectRenderMeshes();
        void collectTerrains();
        void collectTrees();
        void addTerrainLattice(TerrainComponent& terrain, const Matrix4& world, uint8_t area, int samplesOverride);
        void collectColliders3D();
        void collectColliders2D();
        void collectVolumes();
        void collectLinks();
        void addVolume(const Matrix4& worldOf, const Vector3& center, const Vector3& size, uint8_t recastArea);
#ifdef JASPERENGINE_PHYSICS_2D
        void addFootprint(const std::vector<Vector2>& worldPolygon, uint8_t recastArea);
#endif
    };

    bool Collector::isDescendant(Entity entity, Entity ancestor) const {
        Entity current = entity;
        while (current != NULL_ENTITY) {
            if (current == ancestor)
                return true;
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        return false;
    }

    bool Collector::inScope(Entity entity) const {
        if (!scene->isEntityActiveInHierarchy(entity))
            return false;
        const int layer = scene->getEntityLayer(entity);
        if (layer >= 0 && layer < 32 && !(surface.includeLayers & (1u << layer)))
            return false;
        if (surface.collectObjects == NavCollectObjects::CHILDREN && !isDescendant(entity, surfaceEntity))
            return false;
        return true;
    }

    const Policy& Collector::policyOf(Entity entity) {
        auto found = policies.find(entity);
        if (found != policies.end())
            return found->second;
        // the chain from the root down to the entity: the nearest modifier decides inclusion and
        // area, a moving ancestor leaves its subtree out (spec R2)
        std::vector<Entity> chain;
        for (Entity current = entity; current != NULL_ENTITY;) {
            chain.push_back(current);
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        NavModifierInclusion inclusion = NavModifierInclusion::AUTO;
        bool moving = false;
        uint8_t area = areaIndex(surface.defaultArea);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Entity node = *it;
            const bool self = node == entity;
            if (const NavMeshModifierComponent* modifier = scene->findComponent<NavMeshModifierComponent>(node)) {
                if (modifier->enabled && !modifier->useVolume && (self || modifier->applyToChildren)) {
                    if (modifier->overrideInclusion != NavModifierInclusion::AUTO)
                        inclusion = modifier->overrideInclusion;
                    if (modifier->overrideArea)
                        area = areaIndex(modifier->area);
                }
            }
            if (scene->findComponent<NavMeshAgentComponent>(node) || scene->findComponent<NavMeshObstacleComponent>(node))
                moving = true;
#ifdef JASPERENGINE_PHYSICS_3D
            if (const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(node)) {
                if (body->type != BodyType::STATIC)
                    moving = true;
            }
            if (scene->findComponent<CharacterControllerComponent>(node))
                moving = true;
#endif
#ifdef JASPERENGINE_PHYSICS_2D
            if (const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(node)) {
                if (body->type != BodyType::STATIC)
                    moving = true;
            }
            if (scene->findComponent<CharacterController2DComponent>(node))
                moving = true;
#endif
        }
        Policy policy;
        policy.include = inclusion == NavModifierInclusion::INCLUDE ? true : (inclusion == NavModifierInclusion::EXCLUDE ? false : !moving);
        policy.area = area;
        return policies.emplace(entity, policy).first->second;
    }

    bool Collector::collectable(Entity entity, Policy& policy) {
        // the editor's own objects in the scene (gizmos, the navigation overlay's meshes) are not
        // the level: collecting the overlay of a bake would make the next check find it stale
#ifdef JASPERENGINE_EDITOR
        if (renderSystem && renderSystem->isEditorOverlay(entity))
            return false;
#endif
        if (!inScope(entity))
            return false;
        policy = policyOf(entity);
        if (!policy.include) {
            stats.excluded++;
            return false;
        }
        return true;
    }

    void Collector::grow(const Vector3& nav) {
        boundsMin.makeFloor(nav);
        boundsMax.makeCeil(nav);
    }

    void Collector::addTriangles(const Mesh& mesh, const Matrix4& toNav, uint8_t area) {
        if (mesh.indices.size() < 3)
            return;
        const int base = (int)(input->vertices.size() / 3);
        // a mirroring map turns the triangles inside out: wound back so they face out again
        const bool mirrored = MeshTriangles::linearDeterminant(toNav) < 0.0f;
        for (const Vector3& v : mesh.vertices) {
            const Vector3 nav = toNav * v;
            input->vertices.push_back(nav.x);
            input->vertices.push_back(nav.y);
            input->vertices.push_back(nav.z);
        }
        const uint8_t recastArea = recastAreaOf(area);
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
                continue;
            input->triangles.push_back(base + (int)a);
            input->triangles.push_back(base + (int)(mirrored ? c : b));
            input->triangles.push_back(base + (int)(mirrored ? b : c));
            input->triangleAreas.push_back(recastArea);
            for (uint32_t k : {a, b, c}) {
                const float* v = &input->vertices[((size_t)base + k) * 3];
                grow(Vector3(v[0], v[1], v[2]));
            }
        }
        stats.triangles += mesh.indices.size() / 3;
    }

    void Collector::addMeshComponent(Entity entity, const Matrix4& worldOf, uint8_t area) {
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh)
            return;
        Mesh local;
        MeshTriangles::appendMeshTriangles(mesh, Matrix4(), local.vertices, local.indices);
        addTriangles(local, navFromWorld * worldOf, area);
    }

    void Collector::collectRenderMeshes() {
        if (surface.useGeometry == NavUseGeometry::PHYSICS_COLLIDERS)
            return;
        std::vector<Entity> entities;
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++)
            entities.push_back(meshes->getEntity(i));
        std::sort(entities.begin(), entities.end());
        for (Entity entity : entities) {
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            // skinned meshes move; terrains and instanced meshes have their own paths
            if (!mesh.enabled || mesh.numSubmeshes == 0 || !mesh.bonesAABB.empty() || scene->findComponent<TerrainComponent>(entity))
                continue;
            Policy policy;
            if (!collectable(entity, policy))
                continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform)
                continue;
            if (InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity)) {
                // T5 R34: grass and tree prototypes never enter a bake; trees come by collectTrees
                if (instanced->foliage)
                    continue;
                Mesh local;
                MeshTriangles::appendMeshTriangles(&mesh, Matrix4(), local.vertices, local.indices);
                for (const InstanceData& instance : instanced->instances) {
                    if (!instance.visible)
                        continue;
                    addTriangles(local, navFromWorld * transform->modelMatrix * trsMatrix(instance.position, instance.rotation, instance.scale),
                                 policy.area);
                    stats.instances++;
                }
                continue;
            }
            addMeshComponent(entity, transform->modelMatrix, policy.area);
            stats.meshes++;
        }
    }

    void Collector::addTerrainLattice(TerrainComponent& terrain, const Matrix4& world, uint8_t area, int samplesOverride) {
        if (!(terrain.terrainSize > 0.0f))
            return;
        // the heightmap's pixels: the loaded texture's, else read again from its file (a terrain
        // whose CPU copy was released after upload)
        const unsigned char* pixels = nullptr;
        int width = 0, height = 0, channels = 0, bytesPerChannel = 1;
        TextureData reloaded;
        if (!terrain.heightMap.empty() && !terrain.heightMap.isFramebuffer()) {
            if (terrain.heightMap.hasData() && terrain.heightMap.getData().getData()) {
                TextureData& data = terrain.heightMap.getData();
                pixels = static_cast<const unsigned char*>(data.getData());
                width = data.getWidth();
                height = data.getHeight();
                channels = data.getChannels();
                bytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
            } else if (!terrain.heightMap.getPath().empty() && reloaded.loadTextureFromFile(terrain.heightMap.getPath().c_str())) {
                pixels = static_cast<const unsigned char*>(reloaded.getData());
                width = reloaded.getWidth();
                height = reloaded.getHeight();
                channels = reloaded.getChannels();
                bytesPerChannel = TextureData::getBytesPerChannel(reloaded.getColorFormat());
            } else {
                stats.warnings.push_back("a terrain's heightmap could not be read (" + terrain.heightMap.getPath() + "); it bakes flat");
            }
        }

        // a sample per heightmap texel, at most a sample per cell and MAX_TERRAIN_SAMPLES a side
        const NavBuildConfig& config = input->config;
        const Vector3 scale(std::fabs(world.get(0, 0)) + std::fabs(world.get(0, 1)) + std::fabs(world.get(0, 2)), 1.0f,
                            std::fabs(world.get(2, 0)) + std::fabs(world.get(2, 1)) + std::fabs(world.get(2, 2)));
        const float worldSize = terrain.terrainSize * std::max(scale.x, scale.z);
        int samples = samplesOverride > 0 ? samplesOverride : (pixels ? std::min(width, height) : 2);
        samples = std::min(samples, (int)std::floor(worldSize / config.cellSize) + 1);
        samples = std::clamp(samples, 2, MAX_TERRAIN_SAMPLES);

        NavTerrainInput lattice;
        lattice.toNav = navFromWorld * world;
        lattice.halfSize = terrain.terrainSize * 0.5f;
        lattice.spacing = terrain.terrainSize / (float)(samples - 1);
        lattice.samples = samples;
        lattice.recastArea = recastAreaOf(area);
        lattice.heights.resize((size_t)samples * (size_t)samples);
        for (int j = 0; j < samples; j++) {
            const float z = (j == samples - 1) ? lattice.halfSize : (-lattice.halfSize + lattice.spacing * (float)j);
            for (int i = 0; i < samples; i++) {
                const float x = (i == samples - 1) ? lattice.halfSize : (-lattice.halfSize + lattice.spacing * (float)i);
                lattice.heights[(size_t)j * (size_t)samples + (size_t)i] =
                    pixels ? terrain.maxHeight * MeshSystem::sampleTerrainHeight(pixels, width, height, channels, bytesPerChannel,
                                                                                 terrain.terrainSize, x, z)
                           : 0.0f;
            }
        }
        if (!terrain.holeMap.empty()) {
            // T3 R23, R24: the walkable area stays inside the drawn ground
            if (terrain::navHoleCells(terrainSurfaceView(terrain).holes, samples, lattice.holeCells) == 0)
                lattice.holeCells.clear();
        }
        lattice.computeBounds();
        grow(Vector3(lattice.bmin[0], lattice.bmin[1], lattice.bmin[2]));
        grow(Vector3(lattice.bmax[0], lattice.bmax[1], lattice.bmax[2]));
        input->terrains.push_back(std::move(lattice));
        stats.terrains++;
        // the pixels read again here are this collect's own (TextureData does not free them)
        reloaded.releaseImageData();
    }

    void Collector::collectTerrains() {
        // terrains are collected under every Use Geometry (spec R2)
        std::vector<Entity> entities;
        auto terrains = scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++)
            entities.push_back(terrains->getEntity(i));
        std::sort(entities.begin(), entities.end());
        for (Entity entity : entities) {
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            Policy policy;
            if (!terrain.enabled || !collectable(entity, policy))
                continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform)
                continue;
            addTerrainLattice(terrain, transform->modelMatrix, policy.area, 0);
        }
    }

    // T5 R34: a cylinder per visible tree with navObstacle, under every Use Geometry, following the
    // Terrain's NavMeshModifier policy
    void Collector::collectTrees() {
        std::shared_ptr<FoliageSystem> foliage = scene->getSystem<FoliageSystem>();
        if (!foliage)
            return;
        auto terrains = scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++) {
            const Entity entity = terrains->getEntity(i);
            Policy policy;
            if (!collectable(entity, policy))
                continue;
            foliage->forEachNavObstacle(entity, [&](const Vector3& base, float radius, float height) {
                Mesh mesh;
                addCylinder(mesh, height * 0.5f, radius);
                addTriangles(mesh, navFromWorld * Matrix4::translateMatrix(base + Vector3(0.0f, height * 0.5f, 0.0f)), policy.area);
                stats.instances++;
            });
        }
    }

    void Collector::collectColliders3D() {
#ifdef JASPERENGINE_PHYSICS_3D
        if (surface.useGeometry == NavUseGeometry::RENDER_MESHES)
            return;
        std::vector<Entity> entities;
        auto bodies = scene->getComponentArray<RigidbodyComponent>();
        for (size_t i = 0; i < bodies->size(); i++)
            entities.push_back(bodies->getEntity(i));
        std::sort(entities.begin(), entities.end());
        for (Entity entity : entities) {
            RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(entity);
            Policy policy;
            if (!body.enabled || body.sensor || !collectable(entity, policy))
                continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform)
                continue;
            const Vector3 scale = transform->worldScale;
            const Vector3 absScale(std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z));
            const Matrix4 bodyWorld = Matrix4::translateMatrix(transform->worldPosition) * transform->worldRotation.getRotationMatrix();
            for (size_t c = 0; c < body.colliders.size(); c++) {
                const Collider3D& collider = body.colliders[c];
                const Matrix4 colliderWorld = bodyWorld * Matrix4::translateMatrix(collider.position * scale) * collider.rotation.getRotationMatrix();
                Mesh mesh;
                Matrix4 meshToWorld = colliderWorld;
                switch (collider.type) {
                    case Collider3DType::BOX:
                        addBox(mesh, Vector3(collider.width * absScale.x, collider.height * absScale.y, collider.depth * absScale.z) * 0.5f);
                        break;
                    case Collider3DType::SPHERE:
                        addSphere(mesh, collider.radius * std::max(absScale.x, std::max(absScale.y, absScale.z)));
                        break;
                    case Collider3DType::CAPSULE: {
                        const float r = collider.radius * std::max(absScale.x, absScale.z);
                        addCapsule(mesh, collider.halfHeight * absScale.y, r, r);
                        break;
                    }
                    case Collider3DType::TAPERED_CAPSULE: {
                        const float rs = std::max(absScale.x, absScale.z);
                        addCapsule(mesh, collider.halfHeight * absScale.y, collider.bottomRadius * rs, collider.topRadius * rs);
                        break;
                    }
                    case Collider3DType::CYLINDER:
                        addCylinder(mesh, collider.halfHeight * absScale.y, collider.radius * std::max(absScale.x, absScale.z));
                        break;
                    case Collider3DType::CONVEX_HULL:
                    case Collider3DType::MESH: {
                        const bool raw = collider.source == Collider3DSource::RAW_VERTICES || collider.source == Collider3DSource::RAW_MESH;
                        Mesh source;
                        if (raw) {
                            for (size_t v = 0; v < collider.vertices.size(); v++)
                                source.vertices.push_back(collider.vertices[v] * scale);
                            for (size_t i = 0; i + 2 < collider.indices.size(); i += 3) {
                                source.indices.push_back(collider.indices[i]);
                                source.indices.push_back(collider.indices[i + 1]);
                                source.indices.push_back(collider.indices[i + 2]);
                            }
                            if (scale.x * scale.y * scale.z < 0.0f) {
                                for (size_t t = 0; t + 2 < source.indices.size(); t += 3)
                                    std::swap(source.indices[t + 1], source.indices[t + 2]);
                            }
                        } else {
                            // the mesh in the body's space (rotation-free, world-scaled), as the physics builds it
                            const Entity meshEntity = collider.sourceEntity == NULL_ENTITY ? entity : collider.sourceEntity;
                            for (Entity sourceEntity : MeshTriangles::meshSourceEntities(scene, meshEntity)) {
                                const Transform* meshTransform = scene->findComponent<Transform>(sourceEntity);
                                const Matrix4 toBody = (sourceEntity == entity || !meshTransform)
                                    ? Matrix4::scaleMatrix(scale)
                                    : transform->worldRotation.inverse().getRotationMatrix() * Matrix4::translateMatrix(-transform->worldPosition) *
                                          meshTransform->modelMatrix;
                                MeshTriangles::appendMeshTriangles(scene->findComponent<MeshComponent>(sourceEntity), toBody, source.vertices,
                                                                   source.indices);
                            }
                        }
                        if (collider.type == Collider3DType::CONVEX_HULL)
                            mesh = convexHull(source.vertices);
                        else
                            mesh = std::move(source);
                        if (mesh.indices.empty())
                            stats.warnings.push_back("a " + std::string(collider.type == Collider3DType::MESH ? "mesh" : "convex hull") +
                                                     " collider of '" + scene->getEntityName(entity) + "' has no geometry");
                        break;
                    }
                    case Collider3DType::HEIGHTFIELD: {
                        const Entity terrainEntity = collider.sourceEntity == NULL_ENTITY ? entity : collider.sourceEntity;
                        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
                        // the terrain itself is collected already when it is this GameObject's
                        if (terrain && terrainEntity != entity) {
                            addTerrainLattice(*terrain, colliderWorld * Matrix4::scaleMatrix(scale), policy.area,
                                              (int)collider.samplesSize);
                        }
                        break;
                    }
                }
                if (!mesh.indices.empty()) {
                    addTriangles(mesh, navFromWorld * meshToWorld, policy.area);
                    stats.colliders++;
                }
            }
        }
#endif
    }

#ifdef JASPERENGINE_PHYSICS_2D
    void Collector::addFootprint(const std::vector<Vector2>& worldPolygon, uint8_t recastArea) {
        if (worldPolygon.size() < 3)
            return;
        std::vector<Vector2> navPoints;
        for (const Vector2& p : worldPolygon) {
            const Vector3 nav = frame.toNav(Vector3(p.x, p.y, frame.origin.z));
            navPoints.push_back(Vector2(nav.x, nav.z));
        }
        const std::vector<Vector2> hull = convexHull2D(navPoints);
        if (hull.size() < 3)
            return;
        NavConvexVolume volume;
        for (const Vector2& p : hull) {
            volume.points.push_back(p.x);
            volume.points.push_back(p.y);
            grow(Vector3(p.x, 0.0f, p.y));
        }
        volume.minY = -std::numeric_limits<float>::max() * 0.5f;
        volume.maxY = std::numeric_limits<float>::max() * 0.5f;
        volume.recastArea = recastArea;
        input->volumes.push_back(std::move(volume));
    }
#endif

    void Collector::collectColliders2D() {
#ifdef JASPERENGINE_PHYSICS_2D
        std::vector<Entity> entities;
        auto bodies = scene->getComponentArray<Rigidbody2DComponent>();
        for (size_t i = 0; i < bodies->size(); i++)
            entities.push_back(bodies->getEntity(i));
        std::sort(entities.begin(), entities.end());
        // segments and chains become quads of width 2 x cell size (spec R21)
        const float halfWidth = input->config.cellSize;
        for (Entity entity : entities) {
            Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(entity);
            Policy policy;
            if (!body.enabled || !collectable(entity, policy))
                continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform)
                continue;
            const Vector2 scale(transform->worldScale.x, transform->worldScale.y);
            const float radiusScale = std::max(std::fabs(scale.x), std::fabs(scale.y));
            auto toWorld = [&](const Vector2& local) {
                const Vector3 rotated = transform->worldRotation * Vector3(local.x * scale.x, local.y * scale.y, 0.0f);
                return Vector2(transform->worldPosition.x + rotated.x, transform->worldPosition.y + rotated.y);
            };
            for (size_t c = 0; c < body.colliders.size(); c++) {
                const Collider2D& collider = body.colliders[c];
                if (collider.sensor)
                    continue;
                // a static collider's footprint is the null area
                const uint8_t area = RECAST_NULL_AREA;
                switch (collider.type) {
                    case Collider2DType::POLYGON: {
                        std::vector<Vector2> points;
                        if (collider.points.size() >= 3) {
                            for (size_t v = 0; v < collider.points.size(); v++)
                                points.push_back(collider.points[v]);
                        } else {
                            points = {collider.pointA, Vector2(collider.pointB.x, collider.pointA.y), collider.pointB,
                                      Vector2(collider.pointA.x, collider.pointB.y)};
                        }
                        std::vector<Vector2> world;
                        for (const Vector2& p : points)
                            world.push_back(toWorld(p));
                        addFootprint(offsetPolygon(convexHull2D(world), collider.radius * radiusScale), area);
                        break;
                    }
                    case Collider2DType::CIRCLE: {
                        std::vector<Vector2> world;
                        const Vector2 center = toWorld(collider.pointA);
                        // circumscribed: the 16-gon holds the circle
                        const float radius = collider.radius * radiusScale / std::cos(PI / (float)CIRCLE_SIDES);
                        for (int s = 0; s < CIRCLE_SIDES; s++) {
                            const float angle = 2.0f * PI * (float)s / (float)CIRCLE_SIDES;
                            world.push_back(Vector2(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)));
                        }
                        addFootprint(world, area);
                        break;
                    }
                    case Collider2DType::CAPSULE: {
                        std::vector<Vector2> world;
                        const Vector2 a = toWorld(collider.pointA), b = toWorld(collider.pointB);
                        const float radius = collider.radius * radiusScale / std::cos(PI / (float)CIRCLE_SIDES);
                        for (const Vector2& center : {a, b}) {
                            for (int s = 0; s < CIRCLE_SIDES; s++) {
                                const float angle = 2.0f * PI * (float)s / (float)CIRCLE_SIDES;
                                world.push_back(Vector2(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)));
                            }
                        }
                        addFootprint(world, area);
                        break;
                    }
                    case Collider2DType::SEGMENT:
                    case Collider2DType::CHAIN: {
                        std::vector<Vector2> points;
                        if (collider.type == Collider2DType::SEGMENT) {
                            points = {toWorld(collider.pointA), toWorld(collider.pointB)};
                        } else {
                            for (size_t v = 0; v < collider.points.size(); v++)
                                points.push_back(toWorld(collider.points[v]));
                            if (collider.loop && points.size() > 2)
                                points.push_back(points.front());
                        }
                        for (size_t i = 0; i + 1 < points.size(); i++) {
                            const Vector2 a = points[i], b = points[i + 1];
                            Vector2 d = b - a;
                            const float length = d.length();
                            if (length < 1e-6f)
                                continue;
                            d = d * (1.0f / length);
                            const Vector2 n(-d.y * halfWidth, d.x * halfWidth);
                            const Vector2 e = d * halfWidth;
                            addFootprint({a - e + n, b + e + n, b + e - n, a - e - n}, area);
                        }
                        break;
                    }
                }
                stats.colliders2D++;
            }
        }
#endif
    }

    void Collector::addVolume(const Matrix4& worldOf, const Vector3& center, const Vector3& size, uint8_t recastArea) {
        // the box's footprint in nav space (the hull of its corners) and its height range
        std::vector<Vector2> footprint;
        float minY = std::numeric_limits<float>::max(), maxY = -std::numeric_limits<float>::max();
        for (int c = 0; c < 8; c++) {
            const Vector3 local(center.x + ((c & 1) ? 0.5f : -0.5f) * size.x, center.y + ((c & 2) ? 0.5f : -0.5f) * size.y,
                                center.z + ((c & 4) ? 0.5f : -0.5f) * size.z);
            const Vector3 nav = frame.toNav(worldOf * local);
            footprint.push_back(Vector2(nav.x, nav.z));
            minY = std::min(minY, nav.y);
            maxY = std::max(maxY, nav.y);
        }
        const std::vector<Vector2> hull = convexHull2D(footprint);
        if (hull.size() < 3)
            return;
        NavConvexVolume volume;
        for (const Vector2& p : hull) {
            volume.points.push_back(p.x);
            volume.points.push_back(p.y);
        }
        if (frame.plane == NavPlane::XY) {
            minY = -std::numeric_limits<float>::max() * 0.5f;
            maxY = std::numeric_limits<float>::max() * 0.5f;
        }
        volume.minY = minY;
        volume.maxY = maxY;
        volume.recastArea = recastArea;
        input->volumes.push_back(std::move(volume));
        stats.volumes++;
    }

    void Collector::collectVolumes() {
        std::vector<Entity> entities;
        auto modifiers = scene->getComponentArray<NavMeshModifierComponent>();
        for (size_t i = 0; i < modifiers->size(); i++)
            entities.push_back(modifiers->getEntity(i));
        std::sort(entities.begin(), entities.end());
        // ascending area, Not Walkable last: the higher index wins, Not Walkable always (spec R5)
        std::vector<std::pair<int, Entity>> ordered;
        for (Entity entity : entities) {
            const NavMeshModifierComponent& modifier = scene->getComponent<NavMeshModifierComponent>(entity);
            if (!modifier.enabled || !modifier.useVolume || !inScope(entity))
                continue;
            const uint8_t area = areaIndex(modifier.overrideArea ? modifier.area : surface.defaultArea);
            ordered.emplace_back(area == AREA_NOT_WALKABLE ? AREA_COUNT : area, entity);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : ordered) {
            const NavMeshModifierComponent& modifier = scene->getComponent<NavMeshModifierComponent>(entry.second);
            const Transform* transform = scene->findComponent<Transform>(entry.second);
            if (!transform)
                continue;
            const uint8_t area = entry.first == AREA_COUNT ? AREA_NOT_WALKABLE : (uint8_t)entry.first;
            addVolume(transform->modelMatrix, modifier.volumeCenter, modifier.volumeSize,
                      area == AREA_NOT_WALKABLE ? RECAST_NULL_AREA : (uint8_t)(area + 1));
        }
    }

    void Collector::collectLinks() {
        std::vector<Entity> entities;
        auto links = scene->getComponentArray<NavMeshLinkComponent>();
        for (size_t i = 0; i < links->size(); i++)
            entities.push_back(links->getEntity(i));
        std::sort(entities.begin(), entities.end());
        for (Entity entity : entities) {
            const NavMeshLinkComponent& link = scene->getComponent<NavMeshLinkComponent>(entity);
            if (!link.enabled || link.agentType != surface.agentType || !scene->isEntityActiveInHierarchy(entity))
                continue;
            if (surface.collectObjects == NavCollectObjects::CHILDREN && !isDescendant(entity, surfaceEntity))
                continue;
            const size_t before = input->links.size();
            NavBakeCollector::linkConnections(scene, entity, frame, input->config.agentRadius, &input->linkAreaCosts, input->links);
            for (size_t i = before; i < input->links.size(); i++) {
                const NavLinkInput& connection = input->links[i];
                // Detour connects an end only within the start's tile or a neighbour of it
                const NavTileCoord a = input->tileAt(connection.start), b = input->tileAt(connection.end);
                if (std::abs(a.x - b.x) > 1 || std::abs(a.y - b.y) > 1)
                    stats.warnings.push_back("link '" + scene->getEntityName(entity) +
                                             "' spans more than one tile; its end only connects within the tile next to its start");
            }
            if (input->links.size() > before)
                stats.links++;
        }
        if (input->linkAreaCosts.size() > (size_t)(MAX_POLY_AREAS - FIRST_LINK_COST_AREA))
            stats.warnings.push_back("more than " + std::to_string(MAX_POLY_AREAS - FIRST_LINK_COST_AREA) +
                                     " distinct link cost modifiers: the extra links use their area's cost");
    }

    std::shared_ptr<NavBakeInput> Collector::run(std::string& error) {
        frame = NavBakeCollector::frameOf(scene, surfaceEntity);
        navFromWorld = frame.navFromWorld();
        input = std::make_shared<NavBakeInput>();
        input->config = NavBakeCollector::configOf(scene, surface);

        if (surface.collectObjects == NavCollectObjects::VOLUME) {
            const Vector3 half = surface.volumeSize * 0.5f;
            const Vector3 lo = surface.volumeCenter - half, hi = surface.volumeCenter + half;
            if (frame.plane == NavPlane::XY) {
                volumeMin = Vector3(lo.x, -input->config.cellSize, lo.y);
                volumeMax = Vector3(hi.x, input->config.cellSize, hi.y);
            } else {
                volumeMin = lo;
                volumeMax = hi;
            }
        }

        if (frame.plane == NavPlane::XY) {
            collectColliders2D();
            collectVolumes();
            // the ground spans the footprints and what is drawn (sprites, tilemaps, meshes) or the volume
            if (surface.collectObjects != NavCollectObjects::VOLUME) {
                std::vector<Entity> entities;
                auto meshes = scene->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++)
                    entities.push_back(meshes->getEntity(i));
                std::sort(entities.begin(), entities.end());
                for (Entity entity : entities) {
                    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
                    if (!mesh.enabled || !inScope(entity) || mesh.worldAABB.isNull() || mesh.worldAABB.isInfinite())
                        continue;
                    const Vector3 a = frame.toNav(mesh.worldAABB.getMinimum()), b = frame.toNav(mesh.worldAABB.getMaximum());
                    grow(Vector3(std::min(a.x, b.x), 0.0f, std::min(a.z, b.z)));
                    grow(Vector3(std::max(a.x, b.x), 0.0f, std::max(a.z, b.z)));
                }
            } else {
                boundsMin = volumeMin;
                boundsMax = volumeMax;
            }
            if (!(boundsMax.x > boundsMin.x) || !(boundsMax.z > boundsMin.z)) {
                error = "nothing to bake: a 2D surface needs a volume, colliders or sprites to span";
                return nullptr;
            }
            // the walkable ground: a quad of the default area at y = 0 over the bounds
            Mesh ground;
            ground.vertices = {Vector3(boundsMin.x, 0.0f, boundsMin.z), Vector3(boundsMin.x, 0.0f, boundsMax.z),
                               Vector3(boundsMax.x, 0.0f, boundsMax.z), Vector3(boundsMax.x, 0.0f, boundsMin.z)};
            ground.indices = {0, 1, 2, 0, 2, 3};
            addTriangles(ground, Matrix4(), areaIndex(surface.defaultArea));
            boundsMin.y = -input->config.cellSize;
            boundsMax.y = input->config.cellSize;
        } else {
            collectTerrains();
            collectTrees();
            collectRenderMeshes();
            collectColliders3D();
            collectVolumes();
            if (surface.collectObjects == NavCollectObjects::VOLUME) {
                boundsMin = volumeMin;
                boundsMax = volumeMax;
                // outside the volume nothing is walkable: six slabs of the null area around it
                const float big = 1e7f;
                auto slab = [&](float x0, float z0, float x1, float z1, float y0, float y1) {
                    NavConvexVolume volume;
                    volume.points = {x0, z0, x1, z0, x1, z1, x0, z1};
                    volume.minY = y0;
                    volume.maxY = y1;
                    volume.recastArea = RECAST_NULL_AREA;
                    input->volumes.push_back(std::move(volume));
                };
                slab(-big, -big, volumeMin.x, big, -big, big);
                slab(volumeMax.x, -big, big, big, -big, big);
                slab(volumeMin.x, -big, volumeMax.x, volumeMin.z, -big, big);
                slab(volumeMin.x, volumeMax.z, volumeMax.x, big, -big, big);
                slab(volumeMin.x, volumeMin.z, volumeMax.x, volumeMax.z, -big, volumeMin.y);
                slab(volumeMin.x, volumeMin.z, volumeMax.x, volumeMax.z, volumeMax.y, big);
            } else {
                // the agent stands up to its height over the highest walkable surface
                boundsMax.y += input->config.agentHeight;
            }
            if (input->triangles.empty() && input->terrains.empty()) {
                error = "nothing to bake: no static mesh, collider or terrain " +
                        std::string(surface.collectObjects == NavCollectObjects::VOLUME ? "in the volume" : "was collected");
                return nullptr;
            }
        }
        // links after the bounds are known (their tiles), clipped to the grid by finalize
        input->bmin[0] = boundsMin.x;
        input->bmin[1] = boundsMin.y;
        input->bmin[2] = boundsMin.z;
        input->bmax[0] = boundsMax.x;
        input->bmax[1] = boundsMax.y;
        input->bmax[2] = boundsMax.z;
        collectLinks();
        if (input->linkAreaCosts.size() > (size_t)(MAX_POLY_AREAS - FIRST_LINK_COST_AREA))
            input->linkAreaCosts.resize((size_t)(MAX_POLY_AREAS - FIRST_LINK_COST_AREA));

        // the 32-bit reference limit, before any tile is bucketed or built (spec R6)
        input->computeTileGrid();
        const std::string tooLarge = checkPolyRefBits(input->tileCount64(), input->config.maxPolysPerTile);
        if (!tooLarge.empty()) {
            error = tooLarge;
            return nullptr;
        }
        input->finalize();
        return input;
    }

}

NavFrame NavBakeCollector::frameOf(Scene* scene, Entity surface) {
    NavFrame frame;
    if (const Transform* transform = scene->findComponent<Transform>(surface)) {
        frame.origin = transform->worldPosition;
        frame.rotation = transform->worldRotation;
    }
    if (const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface))
        frame.plane = component->plane;
    return frame;
}

float NavBakeCollector::unitScaleOf(Scene* scene, const NavMeshSurfaceComponent& surface) {
    if (surface.plane != NavPlane::XY)
        return 1.0f;
#ifdef JASPERENGINE_PHYSICS_2D
    const float scale = scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
#else
    // without 2D physics nothing sets the scale (Lua's PhysicsSystem.pointsToMeterScale2D is a 2D
    // physics binding): its default
    const float scale = 0.0f;
#endif
    return scale > 0.0f ? scale : 64.0f;
}

NavBuildConfig NavBakeCollector::configOf(Scene* scene, const NavMeshSurfaceComponent& surface) {
    const AgentTypeSettings agent = getSettings().agentTypeOrDefault(surface.agentType);
    int tileSize = surface.overrideTileSize ? surface.tileSize : 64;
#ifdef __EMSCRIPTEN__
    // a web player bakes in tiles of at most 32 cells: every Recast pass of a tile then fits the
    // main-thread slice of a single-threaded build (spec R23), and threaded web builds bake the
    // same bytes. Baked assets keep the tile size they were baked with.
    tileSize = std::min(tileSize, WEB_MAX_TILE_CELLS);
#endif
    return NavBuildConfig::derive(agent, surface.plane, surface.overrideCellSize ? surface.cellSize : 0.0f, tileSize,
                                  surface.minRegionArea, surface.runtimeCarving, unitScaleOf(scene, surface));
}

void NavBakeCollector::linkConnections(Scene* scene, Entity entity, const NavFrame& frame, float agentRadius,
                                       std::vector<float>* costAreas, std::vector<NavLinkInput>& out) {
    const NavMeshLinkComponent* link = scene->findComponent<NavMeshLinkComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!link || !transform)
        return;
    const Vector3 start = frame.toNav(transform->modelMatrix * link->startPoint);
    const Vector3 end = frame.toNav(transform->modelMatrix * link->endPoint);
    const uint8_t area = areaIndex(link->area);
    if (area == AREA_NOT_WALKABLE)
        return;
    uint8_t polyArea = area;
    if (link->costModifier >= 0.0f && costAreas) {
        // a cost override gets a link cost area of its own (shared by equal costs)
        auto found = std::find(costAreas->begin(), costAreas->end(), link->costModifier);
        size_t index = (size_t)(found - costAreas->begin());
        if (found == costAreas->end()) {
            index = costAreas->size();
            costAreas->push_back(link->costModifier);
        }
        if (index < (size_t)(MAX_POLY_AREAS - FIRST_LINK_COST_AREA))
            polyArea = (uint8_t)(FIRST_LINK_COST_AREA + (int)index);
    }
    // a width places ceil(width / (2 r)) connections side by side across it (spec R7)
    const float radius = std::max(agentRadius, 1e-3f);
    const int count = link->width > 0.0f ? std::max(1, (int)std::ceil(link->width / (2.0f * radius))) : 1;
    Vector3 across(0.0f, 0.0f, 0.0f);
    const Vector3 along(end.x - start.x, 0.0f, end.z - start.z);
    if (along.length() > 1e-6f)
        across = Vector3(-along.z, 0.0f, along.x).normalized();
    for (int i = 0; i < count; i++) {
        const float offset = count > 1 ? link->width * (((float)i + 0.5f) / (float)count - 0.5f) : 0.0f;
        NavLinkInput connection;
        const Vector3 s = start + across * offset, e = end + across * offset;
        connection.start[0] = s.x; connection.start[1] = s.y; connection.start[2] = s.z;
        connection.end[0] = e.x; connection.end[1] = e.y; connection.end[2] = e.z;
        connection.radius = radius;
        connection.bidirectional = link->bidirectional;
        connection.area = area;
        connection.polyArea = polyArea;
        connection.userId = (uint32_t)entity;
        out.push_back(connection);
    }
}

std::shared_ptr<NavBakeInput> NavBakeCollector::collect(Scene* scene, Entity surface, std::string& error, NavCollectStats* stats) {
    NavCollectStats local;
    NavCollectStats& out = stats ? *stats : local;
    out = NavCollectStats();
    const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface);
    if (!component) {
        error = "the GameObject has no NavMeshSurface";
        return nullptr;
    }
    Collector collector(scene, surface, *component, out);
    return collector.run(error);
}
