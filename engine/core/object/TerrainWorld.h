// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_H
#define TERRAIN_WORLD_H

#include "object/EntityHandle.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine{

    class TerrainWorldRuntime;
    enum class TerrainWorldReadyLevel : uint8_t;

    // What TerrainWorld:getSurfaceAt answers for a point (T4 R19, R24), in world units
    struct JASPERENGINE_API TerrainWorldSurface{
        float height = 0.0f;
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
        bool hole = false;
        // a tile of the world covers the point
        bool present = false;
        // from the tile's own samples; false when it came from the overview (the tile is not in memory)
        bool exact = false;
        // "absent", "overview" or "full"
        std::string state = "absent";
    };

    // T4 R47: a Terrain World GameObject (docs/scripting/terrain-worlds.md): Lua TerrainWorld(scene,
    // entity) or go:getTerrainWorld(). X and Z are world positions; a World only translates, so its
    // own are its Transform's position subtracted. A world whose data cannot start answers as one
    // with nothing loaded, and what needs its streaming (focuses, waits) returns 0 or false.
    class JASPERENGINE_API TerrainWorld: public EntityHandle{
    public:
        TerrainWorld(Scene* scene, Entity entity);
        virtual ~TerrainWorld();

        TerrainWorld(const TerrainWorld& rhs);
        TerrainWorld& operator=(const TerrainWorld& rhs);
        TerrainWorld(TerrainWorld&& rhs) noexcept;
        TerrainWorld& operator=(TerrainWorld&& rhs) noexcept;

        // the GameObject has a Terrain World component
        bool isValid() const;
        // the world's streaming, started now when it has not run yet; null with the reason
        TerrainWorldRuntime* getRuntime(std::string* error = nullptr) const;
        // what is subtracted from a world position to give the runtime's local one
        Vector3 getOrigin() const;

        // R15: a focus streams the tiles round it (radiusScale times the near band); its id, 0
        // when the world already has 8 (the camera's and the component's count)
        int addFocus(const Vector3& position, float radiusScale = 1.0f, const Vector3& velocity = Vector3(0.0f, 0.0f, 0.0f));
        // false when no focus has that id
        bool setFocus(int id, const Vector3& position, const Vector3& velocity = Vector3(0.0f, 0.0f, 0.0f));
        bool removeFocus(int id);

        // R19, R24: the game never waits for a tile here, it answers from the overview (exact
        // false); the editor reads a cold tile now
        TerrainWorldSurface getSurfaceAt(float x, float z) const;
        // R14's state of the tile under the point: "absent", "cold", "queued", "loading",
        // "cpu_ready", "uploading", "resident" or "error"
        std::string getTileState(float x, float z) const;
        // R18
        bool isAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level) const;
        // R18: called once in the update phase when the area is ready (at once if it is);
        // the area streams meanwhile. Its id, 0 when the world does not run.
        int onAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level, std::function<void()> callback);
        // R22: a coarse collider under the point now, before a teleport; false off the world
        bool ensureCollisionAt(float x, float z);
        // R10: the overview is in, so every present tile draws
        bool isReady() const;

        // R41's settings; 0 is the platform's default
        void setPoolSize(int poolSize);
        int getPoolSize() const;
        void setLodDistance(float lodDistance);
        float getLodDistance() const;
        // MiB a frame
        void setUploadBudget(float uploadBudget);
        float getUploadBudget() const;
        // metres round each focus with fine collision
        void setCollisionRadius(float collisionRadius);
        float getCollisionRadius() const;
    };

}

#endif //TERRAIN_WORLD_H
