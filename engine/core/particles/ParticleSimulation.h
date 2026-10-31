// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_SIMULATION_H
#define PARTICLES_PARTICLE_SIMULATION_H

// The version 2 Particle System simulation (particles spec 6.3): pure (no scene, no render),
// driven by ActionSystem (Play, the editor preview, exported builds alike) and gtested
// (ParticleShapeTest, ParticleEmissionTest, ParticleSimulationTest). It reads the modules of a
// ParticleSystemComponent and moves its `state`. Positions and velocities are in the
// simulation space: the emitter's frame (Local: its world position and rotation, scaled as the
// scaling mode says) or the world.

#include "Export.h"
#include "component/ParticleSystemComponent.h"
#include "math/Matrix4.h"
#include "math/Rect.h"

#include <cstdint>

namespace jasperengine {

    // the emitter this step, in the world
    struct JASPERENGINE_API ParticleEmitterFrame {
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 worldScale = Vector3(1, 1, 1);   // the hierarchy's (lossy) scale
        Vector3 localScale = Vector3(1, 1, 1);   // the system's own Transform scale
        // the emitter's Rigidbody velocity (emitterVelocityMode Rigidbody), when it has one
        bool hasRigidbodyVelocity = false;
        Vector3 rigidbodyVelocity = Vector3(0, 0, 0);
    };

    struct JASPERENGINE_API ParticleEnvironment {
        Vector3 gravity3D = Vector3(0.0f, -9.81f, 0.0f);
        // the 2D world's gravity in the scene's units (Box2D gravity x points per meter)
        Vector3 gravity2D = Vector3(0.0f, -9.81f * 64.0f, 0.0f);
    };

    namespace particles {

        // the step size above which a step is split (spec 5.6) and the prewarm step
        constexpr float MAX_SUBSTEP = 0.1f;
        constexpr float PREWARM_STEP = 1.0f / 30.0f;
        // an emitter jump this long (world units) in one step does not emit over distance
        constexpr float TELEPORT_DISTANCE = 100.0f;

        // Unity's play(): a stopped system restarts at time 0 (a new seed when autoRandomSeed;
        // prewarmed when looping and prewarm), a paused one resumes, a playing one is unchanged
        JASPERENGINE_API void play(ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env);
        // StopEmitting: no new particles, the alive ones live out (isPlaying until the last dies);
        // StopEmittingAndClear: stopped and empty now. True when the system is stopped after it
        JASPERENGINE_API bool stop(ParticleSystemComponent& ps, ParticleSystemStopBehavior behavior);
        JASPERENGINE_API void pause(ParticleSystemComponent& ps);
        // removes the particles, the system keeps playing
        JASPERENGINE_API void clear(ParticleSystemComponent& ps);
        // stopped and empty, the clocks back to 0 (no stop action: the reset of a preview or a load)
        JASPERENGINE_API void reset(ParticleSystemComponent& ps);
        // `count` particles now with the shape and the start values (also while stopped: they
        // simulate, the system is alive without emitting); the number spawned
        JASPERENGINE_API int emit(ParticleSystemComponent& ps, int count, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env);

        // Advances the system by `dt` seconds of its own time (the caller applied the time scale
        // and main.simulationSpeed), split into MAX_SUBSTEP steps. Returns true when the system
        // finished in it: it was playing and now emits nothing and has no particle (the caller
        // runs the stop action and onStop).
        JASPERENGINE_API bool step(ParticleSystemComponent& ps, float dt, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env);

        // Unity's Simulate(t, restart): from a restart (or from now) advances t seconds in
        // PREWARM_STEP steps, then leaves the system paused there (as Unity does)
        JASPERENGINE_API void simulate(ParticleSystemComponent& ps, float t, bool restart, const ParticleEmitterFrame& emitter, const ParticleEnvironment& env);

        JASPERENGINE_API bool isAlive(const ParticleSystemComponent& ps);
        JASPERENGINE_API size_t particleCount(const ParticleSystemComponent& ps);

        // the matrix from the simulation space to the world (identity for World systems)
        JASPERENGINE_API Matrix4 simulationToWorld(const ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter);
        // the scale the emitter gives particle sizes (Hierarchy: the world scale, Local: the own
        // scale, Shape: none)
        JASPERENGINE_API Vector3 sizeScale(const ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter);
        // a vector of the simulation space in the world
        JASPERENGINE_API Vector3 simulationVectorToWorld(const ParticleSystemComponent& ps, const ParticleEmitterFrame& emitter, const Vector3& v);

        // the frame count of the texture sheet (1 when off) and a frame's UV rect (origin top
        // left, as sprite frames)
        JASPERENGINE_API int textureSheetFrameCount(const ParticleTextureSheetAnimationModule& sheet);
        JASPERENGINE_API Rect textureSheetRect(const ParticleTextureSheetAnimationModule& sheet, int frame, int row);
        // the row of particle `index` (SingleRow) and its frame in that row / the sheet
        JASPERENGINE_API int textureSheetRow(const ParticleSystemComponent& ps, size_t index);

        // the built-in soft particle texture (the shader library's @texture default "particle"):
        // `size` x `size` RGBA8 pixels, white, the alpha a smooth disc ((1 - r^2)^2)
        JASPERENGINE_API void makeParticleTexture(uint32_t* pixels, int size);

        // the quaternion of Euler angles in radians applied X, then Y, then Z (the engine's order)
        JASPERENGINE_API Quaternion eulerRadians(const Vector3& angles);

        // a per particle random number in [0, 1) for a module value (seed + id hashed)
        JASPERENGINE_API float particleRandom(uint32_t seed, uint32_t id);

        // the random ids of the module values (stable: a saved seed replays the same)
        enum RandomId : uint32_t {
            RANDOM_LIFETIME = 1, RANDOM_SPEED, RANDOM_SIZE, RANDOM_SIZE_Y, RANDOM_SIZE_Z,
            RANDOM_ROTATION, RANDOM_ROTATION_X, RANDOM_ROTATION_Y, RANDOM_FLIP_ROTATION, RANDOM_COLOR,
            RANDOM_GRAVITY, RANDOM_FORCE_X, RANDOM_FORCE_Y, RANDOM_FORCE_Z, RANDOM_INHERIT,
            RANDOM_VELOCITY_X, RANDOM_VELOCITY_Y, RANDOM_VELOCITY_Z, RANDOM_ORBITAL_X, RANDOM_ORBITAL_Y,
            RANDOM_ORBITAL_Z, RANDOM_OFFSET_X, RANDOM_OFFSET_Y, RANDOM_OFFSET_Z, RANDOM_RADIAL,
            RANDOM_SPEED_MODIFIER, RANDOM_LIMIT, RANDOM_LIMIT_X, RANDOM_LIMIT_Y, RANDOM_LIMIT_Z, RANDOM_DRAG,
            RANDOM_COLOR_LIFETIME, RANDOM_COLOR_SPEED, RANDOM_SIZE_LIFETIME, RANDOM_SIZE_LIFETIME_Y,
            RANDOM_SIZE_LIFETIME_Z, RANDOM_SIZE_SPEED, RANDOM_SIZE_SPEED_Y, RANDOM_SIZE_SPEED_Z,
            RANDOM_ROTATION_LIFETIME, RANDOM_ROTATION_LIFETIME_X, RANDOM_ROTATION_LIFETIME_Y,
            RANDOM_ROTATION_SPEED, RANDOM_ROTATION_SPEED_X, RANDOM_ROTATION_SPEED_Y, RANDOM_NOISE,
            RANDOM_NOISE_Y, RANDOM_NOISE_Z, RANDOM_NOISE_OFFSET, RANDOM_FRAME, RANDOM_START_FRAME, RANDOM_ROW,
            RANDOM_FLIP_U, RANDOM_FLIP_V, RANDOM_FLIP_X, RANDOM_FLIP_Y, RANDOM_FLIP_Z, RANDOM_SPRITE
        };

    }

}

#endif //PARTICLES_PARTICLE_SIMULATION_H
