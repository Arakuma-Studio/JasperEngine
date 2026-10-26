// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEKERNELCONTEXT_H
#define PARTICLES_PARTICLEKERNELCONTEXT_H

// What a kernel sees (docs/specs/S20-particles.md D3): channel pointers and a range, never an
// index list. Ranges start at 0 or on a chunk boundary (a multiple of 4); lanes past the end
// are unused slots a kernel may overwrite. Spawning goes through a staging area that starts at
// 0, so spawn kernels and the spawn pre-advance follow the same rule.

#include "particles/ParticleProgram.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine { namespace particles {

    inline constexpr uint32_t kChunkSize = 4096;       // particles per update work unit
    inline constexpr uint32_t kSpawnBatch = 1024;      // particles per spawn staging batch

    // A force field the External Forces module samples, in simulation space (ParticleWorld
    // gathers them from ForceFieldComponents, each emitter keeps those that overlap it)
    enum class ForceFieldShape : uint8_t { Sphere, Box, Cylinder, Infinite };
    enum class ForceFieldType : uint8_t { Directional, Radial, Vortex, Turbulence, Drag };

    struct ForceFieldSample {
        ForceFieldShape shape = ForceFieldShape::Sphere;
        ForceFieldType type = ForceFieldType::Directional;
        uint8_t pad[2] = {0, 0};
        uint32_t layers = 1;
        // simulation space to the field's unit space (the shape is the unit sphere, cube or
        // cylinder there), column-major 3x4
        float toField[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        float center[3] = {0, 0, 0};
        float axis[3] = {0, 1, 0};         // vortex axis / wind direction (unit, simulation space)
        float strength = 1.0f;             // this tick's (a pulsing wind's strength already applied)
        float turbulence = 0.0f;           // Directional: noise added to the wind
        float frequency = 1.0f;            // Turbulence, and the Directional turbulence
        float falloff[17] = {};            // strength over the normalized distance 0..1 (17 samples)
        float aabbMin[3] = {0, 0, 0};
        float aabbMax[3] = {0, 0, 0};
        uint32_t seed = 0;
    };

    // A hit a collision provider reports (simulation space)
    struct CollisionHit {
        float position[3] = {0, 0, 0};
        float normal[3] = {0, 1, 0};
    };

    // The queries a collision provider answers. Terrain is answered inline in the kernels (it
    // must be thread-safe); rays are batched per chunk and cast by the collision stage.
    struct CollisionProvider {
        // the terrain surface under a point (simulation space): a point on it and its normal;
        // false when no terrain covers the point
        bool (*terrain)(const void* context, const float point[3], float surface[3], float normal[3]) = nullptr;
        // a ray (or swept sphere of `radius`) from `from` to `to`: the first hit
        bool (*ray)(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, CollisionHit& hit) = nullptr;
        const void* context = nullptr;
    };

    // A contact plane found by a world query, shared by the particles of a grid cell (world
    // collision, R8): normal and d (dot(n, p) + d = 0), and the tick it was found (0: none)
    struct CachedPlane {
        float plane[4] = {0, 1, 0, 0};
        uint32_t tick = 0;
        uint32_t key = 0;
    };
    inline constexpr uint32_t kPlaneGridSize = 4096;
    // a particle's cached plane after a probe found its path clear: never hit (dot(n, p) + d > 0)
    inline constexpr float kEmptyPlaneD = 1.0e30f;

    struct RayRequest {
        uint32_t particle = 0;
        float from[3] = {0, 0, 0};
        float to[3] = {0, 0, 0};
        float radius = 0.0f;
    };

    struct CollisionEvent {
        float position[3] = {0, 0, 0};
        float normal[3] = {0, 1, 0};
        float velocity[3] = {0, 0, 0};
        uint32_t particleId = 0;      // the spawn index
        uint32_t index = 0;           // the particle's slot when it hit
        bool killed = false;
    };

    // Per-thread scratch of the kernels (one chunk's worth)
    struct ChunkScratch {
        alignas(16) float ax[kChunkSize];
        alignas(16) float ay[kChunkSize];
        alignas(16) float az[kChunkSize];
        alignas(16) float vx[kChunkSize];   // animated velocity
        alignas(16) float vy[kChunkSize];
        alignas(16) float vz[kChunkSize];
        alignas(16) float dt[kChunkSize];   // per-particle dt (spawn pre-advance)
        bool hasAccel = false;
        bool hasAnimated = false;
    };

    // What one chunk's update produced
    struct ChunkOutput {
        uint32_t deadCount = 0;
        uint32_t* dead = nullptr;          // kChunkSize entries: absolute indices, ascending
        uint32_t rayCount = 0;
        RayRequest* rays = nullptr;        // kChunkSize entries
        uint32_t eventCount = 0;
        CollisionEvent* events = nullptr;  // up to maxEvents
        uint32_t maxEvents = 0;
        float boundsMin[3] = {3.0e38f, 3.0e38f, 3.0e38f};
        float boundsMax[3] = {-3.0e38f, -3.0e38f, -3.0e38f};
    };

    struct KernelContext {
        float* ch[ChCount] = {};             // channel bases (nullptr: absent)
        const ParticleProgram* program = nullptr;
        const unsigned char* pool = nullptr;  // the program's parameter pool
        uint32_t seed = 0;
        uint32_t tick = 0;                    // ticks since the start (per-tick randomness)
        float dt = 0.0f;                      // the tick's step
        const float* dtLanes = nullptr;       // per-particle steps (pre-advance), indexed from the range's 0
        float emitterTime = 0.0f;             // the emitter's time over its duration (0 to 1)
        float time = 0.0f;                    // seconds simulated (noise scroll, pulses)
        float gravity[3] = {0, 0, 0};         // in simulation space
        // emitter space to simulation space (identity in Local simulation space), 3x4 column-major
        float emitterToSim[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        // world directions to simulation space (identity in World simulation space), 3x3
        float worldToSimDir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        // emitter directions to simulation space (the emitter's rotation in World space)
        float emitterToSimDir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        // the same rotation as a quaternion (x, y, z, w): mesh particles' orientation
        float emitterToSimQuat[4] = {0, 0, 0, 1};
        float sizeScale = 1.0f;               // the scaling mode's factor on sizes
        const ForceFieldSample* fields = nullptr;
        uint32_t fieldCount = 0;
        const CollisionProvider* collision = nullptr;
        float collisionPlanes[kMaxCollisionPlanes][4] = {};   // the program's planes and the component's
        uint32_t collisionPlaneCount = 0;
        // world collision: the planes of the grid cells (read-only during the update)
        const CachedPlane* planeGrid = nullptr;
        float planeCellInv = 1.0f;
        ChunkScratch* scratch = nullptr;
        ChunkOutput* out = nullptr;
        // spawn: the staged particles' emitter transform blend (0 previous tick, 1 this one)
        const float* spawnFraction = nullptr;
        float prevEmitterToSim[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        float prevEmitterToSimDir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float emitterVelocity[3] = {0, 0, 0};  // simulation space, units per second
        bool preAdvance = false;               // the update ops run over just-spawned particles
    };

    // The kernels of one back end (ParticleKernels.h instantiates them per back end)
    struct KernelTable {
        const char* backend = "";
        void (*spawn)(const KernelContext& ctx, const Op& op, uint32_t begin, uint32_t end) = nullptr;
        void (*update)(const KernelContext& ctx, const Op& op, uint32_t begin, uint32_t end) = nullptr;
    };

    // the widest back end of the build (or Scalar when particles.forceScalar / JASPERENGINE_SIMD_DISABLE)
    JASPERENGINE_API const KernelTable& nativeKernels();
    JASPERENGINE_API const KernelTable& scalarKernels();
    // the table the runtime uses now: native unless forced to scalar
    JASPERENGINE_API const KernelTable& activeKernels();
    JASPERENGINE_API void setForceScalarKernels(bool forceScalar);
    JASPERENGINE_API bool forceScalarKernels();

}}

#endif // PARTICLES_PARTICLEKERNELCONTEXT_H
