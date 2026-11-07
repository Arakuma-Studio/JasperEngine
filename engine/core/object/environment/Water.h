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
#include "util/WaterWaves.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    struct Material;

    // A water surface: an enabled, active mesh with a submesh drawn with library:water or a
    // shader whose manifest says @role water (a fork of it; engine/shaderlib/library/water.frag),
    // with the values of that submesh's material (its own or its shader's manifest defaults).
    // Made for a horizontal surface: it rests at the top of the mesh's world bounds and covers
    // their extent in XZ.
    struct JASPERENGINE_API WaterSurface{
        Entity entity = NULL_ENTITY;
        float level = 0.0f;                 // rest height (world Y), the top of the bounds
        Vector2 minXZ;                      // world bounds in XZ
        Vector2 maxXZ;
        WaterWaveParams waves;              // WAVES (and GERSTNER), with their values
        bool buoyancy = true;               // floats Rigidbodies with a Buoyancy
        float buoyancyDepth = 0.0f;         // how far below the surface they float, 0: no bottom
        Vector4 shallowColor;               // linear, as the material stores them
        Vector4 deepColor;

        bool covers(float x, float z) const;
        // the lowest point in the water: level - buoyancyDepth, minus infinity without a depth
        float getBottom() const;
        // the surface at a world XZ at a time of the scene clock (Water::getTime)
        float getHeight(float x, float z, float time) const;
        Vector3 getNormal(float x, float z, float time) const;
        // the waves there: height above the level, slope, normal, the water's velocity
        WaterWaveSample getWaves(float x, float z, float time) const;
    };

    // The water surfaces of a scene for the physics and scripts (Lua: Water). They are the
    // meshes drawn with library:water (or a fork with @role water); heights take its waves
    // (WAVES, GERSTNER) on the clock the shader animates them by (the scene clock), so what
    // floats rides the waves on screen. Where waters overlap
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

        // Whether a shader reference is a water: library:water, or a shader whose manifest says
        // @role water (a fork of it). The name first; only a fork's manifest is looked up.
        static bool isWaterShader(const std::string& reference);
        // the same for the view from inside: library:underwater or @role underwater
        static bool isUnderwaterShader(const std::string& reference);

        // The clock library:water animates by (its engine-written sceneTime): the scene's game
        // clock (Scene::getSceneTime), which holds while the game is paused and follows the time
        // scale; the main scene's without one, 0 without either
        static float getTime(Scene* scene = nullptr);

        // The waves of library:water (WAVES, library/water.glsl's waterWaves through its C++
        // twin util/WaterWaves.h) at a world XZ: x the height above the rest level, yz its slope
        // along world X and Z
        static Vector3 getWaves(float x, float z, float time, const Vector2& direction, float height, float waveLength, float speed);

        // every water surface of the scene (none without library:water meshes)
        static std::vector<WaterSurface> getSurfaces(Scene* scene);
        // the surface a material makes of a mesh (false: the material is not a water)
        static bool readMaterial(const Material& material, WaterSurface& surface);
        // the waves of a water material: WAVES, GERSTNER and their values, set or its shader's
        // manifest defaults (false: the material is not a water)
        static bool readWaves(const Material& material, WaterWaveParams& waves);
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
