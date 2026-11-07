// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_INTERACTION_SYSTEM_H
#define WATER_INTERACTION_SYSTEM_H

#include "Export.h"
#include "ecs/SubSystem.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "util/WaterExclusion.h"
#include "util/WaterWakeMap.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    struct WaterExcluderComponent;
    struct WaterWakeComponent;

    // How objects interact with the water surfaces (docs/superpowers/specs/2026-11-04-water-boats-design.md):
    // Water Excluders keep water out (their volumes, hulls and hull maps, gathered each frame,
    // edit mode included) and Water Wakes leave foam and waves in the scene's wake map (a running
    // scene only). RenderSystem binds what it gathers to the water draws (WATER_INTERACTION_PARAMS,
    // includes/water_interaction.glsl); the physics, the Water queries and the underwater switch
    // ask findExcluder. A scene with neither component costs two array-size checks a frame.
    class JASPERENGINE_API WaterInteractionSystem : public SubSystem {
    public:
        // an active excluder this frame, as the shader tests it
        struct Excluder{
            Entity entity = NULL_ENTITY;
            WaterExcluderShape shape = WaterExcluderShape::BOX;
            bool hullMap = false;       // tested through its hull map (else its box or sphere)
            AABB localBox;
            Matrix4 modelMatrix;
            Matrix4 worldToUnit;
            AABB worldBounds;
            int hullLayer = -1;         // in the hull map array, -1 without one
        };

        struct HullInfo{
            WaterHullStatus status = WaterHullStatus::NONE;
            size_t faces = 0;
            size_t sourceVertices = 0;
            AABB localBounds;
        };

        struct EmitterInfo{
            bool emitting = false;      // on a water and moving this step
            float speed = 0.0f;         // smoothed, m/s over world XZ
            Entity water = NULL_ENTITY; // the water body it is on (its key entity)
        };

        struct WakeMapInfo{
            bool active = false;        // holds a wake (drawn)
            int size = 0;               // texels a side
            float area = 0.0f;          // metres a side
            Vector2 corner;             // world XZ of its corner
        };

    private:
        struct HullCache{
            uint64_t signature = 0;
            WaterHullStatus status = WaterHullStatus::NONE;
            WaterHull hull;
            std::vector<uint8_t> map;
            int layer = -1;
            bool seen = false;
        };

        struct Emitter{
            WaterWakeTrack track;
            Entity water = NULL_ENTITY;
            bool emitting = false;
            bool seen = false;
        };

        std::unordered_map<Entity, HullCache> hulls;
        std::vector<Excluder> frameExcluders;
        bool excludersGathered = false;
        // the hull map array's layers (an entity each) and their version
        std::vector<Entity> hullLayers;
        uint64_t hullMapsVersion = 0;

        std::unordered_map<Entity, Emitter> emitters;
        WaterWakeMap wakeMap;
        bool wakeActive = false;
        bool wakeDirty = false;
        float wakeFade = 8.0f;
        // A wake's water body tag (1..255) by the body's key, stable while the map holds texels of
        // it. A body is a spline river or lake (its GameObject) or the library:water meshes at one
        // rest level whose areas touch (an ocean of tiles is one); its key is its smallest entity.
        std::unordered_map<Entity, uint8_t> bodyTags;
        // the drawn water meshes' body keys this step
        std::unordered_map<Entity, Entity> meshBodies;
        bool bodyTagsWarned = false;

        uint64_t hullBuilds = 0;
        uint64_t wakeSteps = 0;
        uint64_t wakeUploads = 0;

        void gatherExcluders();
        bool updateHull(Entity entity, HullCache& cache);
        void releaseExcluders();
        void releaseWakes();
        void stepWakes(float dt);
        uint8_t tagOf(Entity key);
        bool isActive(Entity entity, bool componentEnabled) const;

    public:
        WaterInteractionSystem(Scene* scene);

        // the excluders active this frame (gathered on first use in a frame)
        const std::vector<Excluder>& getExcluders();
        // An active excluder holding the world point (the current poses), ignoring the excluders a
        // floating body owns; NULL_ENTITY for none
        Entity findExcluder(const Vector3& point, Entity floatingBody = NULL_ENTITY);
        // the nearest GameObject with a Rigidbody at or above the excluder's: the body it belongs to
        static Entity getExcluderOwner(Scene* scene, Entity excluder);
        // the local bounds of the meshes a hull is built from (the GameObject's mesh, or its model's
        // mesh nodes): Fit to Mesh and a Water Wake's automatic footprint; null without
        static AABB getMeshSourceLocalBounds(Scene* scene, Entity entity);
        // whether an excluder holds a world point (inactive ones too: Lua WaterExcluder:contains)
        bool contains(Entity excluder, const Vector3& point);

        HullInfo getHullInfo(Entity excluder) const;
        // its hull's points and edges in local space (the Scene view gizmo); null without a hull
        const WaterHull* getHull(Entity excluder) const;
        // the layers of the hull map array: one RGBA8 map per entry, rebuilt when the version moves
        const std::vector<Entity>& getHullLayers() const { return hullLayers; }
        const std::vector<uint8_t>* getHullMap(Entity excluder) const;
        uint64_t getHullMapsVersion() const { return hullMapsVersion; }

        EmitterInfo getEmitterInfo(Entity emitter) const;
        WakeMapInfo getWakeMapInfo() const;
        const WaterWakeMap& getWakeMap() const { return wakeMap; }
        bool isWakeActive() const { return wakeActive; }
        // the map changed since it was uploaded; RenderSystem::draw uploads it once a frame
        bool isWakeDirty() const { return wakeDirty; }
        void markWakeUploaded(){ wakeDirty = false; wakeUploads++; }
        // the tag of the water body a drawn water mesh belongs to (0: none this step)
        uint8_t getBodyTag(Entity waterMesh) const;
        // the wake's foam and crest at a world XZ on the water body there (0 without)
        bool sampleWake(float x, float z, Entity waterEntity, float& foam, float& crest) const;

        // the side of the wake map for a scene setting on this platform (AUTO: 256, 128 on mobile and web)
        static int getWakeMapSize(int resolution);

        // for tests: hulls built, wake steps taken and wake maps uploaded since the system started
        uint64_t getHullBuildCount() const { return hullBuilds; }
        uint64_t getWakeStepCount() const { return wakeSteps; }
        uint64_t getWakeUploadCount() const { return wakeUploads; }

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //WATER_INTERACTION_SYSTEM_H
