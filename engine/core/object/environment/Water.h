// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_H
#define WATER_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "subsystem/WaterSystem.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace jasperengine{

    class Scene;
    struct Material;

    // A water surface: an enabled, active mesh with a submesh drawn with library:water
    // (engine/shaderlib/library/water.frag), with the values of that submesh's material (its
    // own or the manifest defaults). Made for a horizontal surface: it rests at the top of the
    // mesh's world bounds and covers their extent in XZ.
    struct JASPERENGINE_API WaterSurface{
        Entity entity = NULL_ENTITY;
        float level = 0.0f;                 // rest height (world Y), the top of the bounds
        Vector2 minXZ;                      // world bounds in XZ
        Vector2 maxXZ;
        bool waves = false;                 // WAVES, with the values below
        Vector2 waveDirection = Vector2(1.0f, 0.3f);
        float waveHeight = 0.0f;
        float waveLength = 1.0f;
        float waveSpeed = 0.0f;
        bool buoyancy = true;               // floats Rigidbodies with a Buoyancy
        float buoyancyDepth = 0.0f;         // how far below the surface they float, 0: no bottom
        Vector4 shallowColor;               // linear, as the material stores them
        Vector4 deepColor;

        bool covers(float x, float z) const;
        // the lowest point in the water: level - buoyancyDepth, minus infinity without a depth
        float getBottom() const;
        // the surface at a world XZ at a time of the shader clock (Water::getTime)
        float getHeight(float x, float z, float time) const;
        Vector3 getNormal(float x, float z, float time) const;
    };

    // The water surfaces of a scene for the physics and scripts (Lua: Water). They are the
    // meshes drawn with library:water; heights take its waves (WAVES) on the clock the
    // shader animates them by, so what floats rides the waves on screen. Where waters overlap
    // the one whose bottom (WaterSurface::getBottom) is highest wins: a pool with a floating
    // depth over a lake without one floats its own bodies.
    //
    // Engine hooks: applyBuoyancy (PhysicsSystem::fixedUpdate, before the 3D step) floats the
    // dynamic Rigidbodies with a Buoyancy (RigidbodyComponent::buoyancy > 0) whose centre of
    // mass is in a water whose buoyancy is on, by Jolt's ApplyBuoyancyImpulse at the surface
    // over it; updateUnderwater (RenderSystem::update, a running scene only) switches the
    // scene's library:underwater post-process passes on while the main camera is under a
    // water's surface and off above it (Scene::setPostProcessPassEnabled), giving them the
    // surface height (waterLevel) and, with followWater, the water's colours.
    class JASPERENGINE_API Water{
    public:
        static const char* const SHADER;            // "library:water"
        static const char* const UNDERWATER_SHADER; // "library:underwater"

        // The clock the shader library animates by (frame.time.x): Engine::getShaderTime, the
        // wall clock latched once a frame (or whole fixed frames under a fixed frame delta),
        // also running in the editor and while the game is paused or its time scaled
        static float getTime();

        // The waves of library:water (WAVES, library/water.glsl's waterWaves through its C++
        // twin util/WaterWaves.h) at a world XZ: x the height above the rest level, yz its slope
        // along world X and Z
        static Vector3 getWaves(float x, float z, float time, const Vector2& direction, float height, float waveLength, float speed);

        // every water surface of the scene (none without library:water meshes)
        static std::vector<WaterSurface> getSurfaces(Scene* scene);
        // the surface a material makes of a mesh (false: the material is not library:water)
        static bool readMaterial(const Material& material, WaterSurface& surface);
        // the water a point is in or over: of the surfaces covering its XZ whose bottom is at
        // or under it, the one whose bottom is highest (then the highest level); buoyant:
        // only waters whose buoyancy is on. Null for none.
        static const WaterSurface* findSurface(const std::vector<WaterSurface>& surfaces, const Vector3& point, bool buoyant = false);

        // The surface height at a world XZ, waves included (the topmost water there, as
        // findSurface from above); nil where no water covers it
        static std::optional<float> getHeight(Scene* scene, float x, float z);
        // The surface normal there (up without WAVES); nil where no water covers it
        static std::optional<Vector3> getNormal(Scene* scene, float x, float z);
        // Whether a world point is under the surface of the water it is in (findSurface)
        static bool isUnderwater(Scene* scene, const Vector3& point);

        // What the water is at a world point (docs/specs/S22-gameplay.md R31): Water Volumes
        // first, then these surfaces; with the frame's waves (Engine::getShaderTime). Usable in
        // edit mode.
        static WaterSample sample(Scene* scene, const Vector3& point);
        // its surface height at a world XZ; nil where no water is
        static std::optional<float> sampleHeight(Scene* scene, float x, float z);

        // Engine hooks (see above)
        static void applyBuoyancy(Scene* scene, float deltaTime);
        static void updateUnderwater(Scene* scene);

        // Shader library @texture defaults "ripples" and "noise" (RenderSystem): seamless
        // size x size RGBA8 pixels (R in the low byte). ripples: a tangent-space normal map of
        // sines on whole wave numbers (the map tiles); noise: grey value noise of four octaves
        // on periodic lattices, stretched to 0..1.
        static void makeRipplesTexture(uint32_t* pixels, int size);
        static void makeNoiseTexture(uint32_t* pixels, int size);
    };

}

#endif //WATER_H
