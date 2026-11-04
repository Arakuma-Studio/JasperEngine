// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_SYSTEM_H
#define WATER_SYSTEM_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "util/WaterWaves.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace jasperengine{

    class Scene;
    class PhysicsSystem;

    // What the water is at a point (Water.sample, PhysicsSystem::sampleWater)
    struct JASPERENGINE_API WaterSample{
        bool inWater = false;               // under the surface, inside a volume
        float surfaceHeight = -std::numeric_limits<float>::infinity();   // world Y over the point (-inf: no water there)
        Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);   // the surface's
        Vector3 flowVelocity;               // the volume's flow plus the surface's rise (0, dh/dt, 0)
        Entity volume = NULL_ENTITY;        // the Water Volume (or library:water mesh) it is in or over

        bool isInWater() const { return inWater; }
        float getSurfaceHeight() const { return surfaceHeight; }
        Vector3 getNormal() const { return normal; }
        Vector3 getFlowVelocity() const { return flowVelocity; }
        Entity getVolume() const { return volume; }
        bool hasWater() const { return volume != NULL_ENTITY; }
    };

    // The Water Volumes of a scene and the Buoyancy they give (docs/specs/S22-gameplay.md
    // R30-R32), owned by its PhysicsSystem: every fixed step, before the 3D world steps, the
    // columns of each Buoyancy push its dynamic Rigidbody up by Archimedes and drag it against
    // the water. Surfaces follow their library:water material's waves on the scene clock
    // (Water::getTime), evaluated by WaterWaves.h. A scene without Water Volumes or Buoyancies
    // costs nothing.
    class JASPERENGINE_API WaterSystem{
    private:
        struct Volume{
            Entity entity = NULL_ENTITY;
            float surface = 0.0f;           // world Y of the flat surface
            Matrix4 worldToLocal;
            float halfX = 0.0f;
            float halfZ = 0.0f;
            float depth = 0.0f;             // world units under the surface
            bool infinite = false;
            float density = 1000.0f;
            Vector3 flow;
            float linearDrag = 1.0f;
            float angularDrag = 0.5f;
            // the waves of its library:water material (WAVES, GERSTNER), none without
            WaterWaveParams waves;
            // the surface is the GameObject's spline river or lake (SplineSystem), not its box
            bool spline = false;
        };

        Scene* scene;
        PhysicsSystem* physics;
        // gathered for each use (the storage is kept)
        mutable std::vector<Volume> volumes;
        mutable float time = 0.0f;

        // the scene's enabled volumes of active GameObjects, at the shader clock now
        void gatherVolumes() const;
        // the volume whose surface over (x, z) is highest among those holding a point at y
        // (-inf: any depth); its surface height, its waves there (normal, the water's motion)
        // and its current
        const Volume* findVolume(float x, float y, float z, float& height, WaterWaveSample& wave, Vector3& flow) const;

    public:
        WaterSystem(Scene* scene, PhysicsSystem* physics);

        // PhysicsSystem::fixedUpdate, before the 3D world steps
        void applyBuoyancy(float dt);

        WaterSample sample(const Vector3& point) const;
        // the surface height at a world XZ (NaN where no water is)
        float sampleHeight(float x, float z) const;

        // how many times the waves were evaluated (the tests' proof that an empty scene costs nothing)
        static uint64_t getWaveEvaluations();
    };

}

#endif //WATER_SYSTEM_H
