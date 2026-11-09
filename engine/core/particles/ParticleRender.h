// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The appearance per view (billboard inputs, velocity stretch, frame blending, distance
// thinning) follows OpenEngine's ParticleExtraction (Engine/Modules/Particles/Source/Rendering/
// ParticleExtraction.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License (see NOTICE.md); the 32-byte vertex-attribute instance replaces its 128-byte SSBO record.

#ifndef PARTICLES_PARTICLERENDER_H
#define PARTICLES_PARTICLERENDER_H

// Render extraction (docs/specs/S20-particles.md D2 step 3, D6, D7, R22): per visible
// (emitter, view), the interpolated position and the appearance modules at the interpolated
// age, distance thinning, an optional 16-bit radix sort, and packing into the 32-byte instance
// the particle shaders read as per-instance vertex attributes. Trails and ribbons pack into
// strips of 28-byte vertices. No GPU calls: the caller uploads.

#include "Export.h"
#include "memory/Arena.h"
#include "particles/EmitterRuntime.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace jobs { class JobSystem; } }

namespace jasperengine { namespace particles {

    // FLOAT3 position (simulation space), UBYTE4N colour (rgb sRGB-encoded with gamma 2.2,
    // linearised by the shader), HALF2 size, USHORT2N (rotation / 2pi, frame / frameCount with
    // the blend in the fraction), HALF4 (velocity xyz, emissive; Mesh: the size z in w)
    struct ParticleInstance {
        float position[3];
        uint32_t color;
        uint32_t size;
        uint32_t rotationFrame;
        uint32_t velocityXY;
        uint32_t velocityZW;
    };
    static_assert(sizeof(ParticleInstance) == 32, "the particle instance is 32 bytes (D6)");

    // Mesh mode's second stream: the rotation quaternion, SHORT4N
    struct ParticleMeshInstance {
        int16_t rotation[4];
    };
    static_assert(sizeof(ParticleMeshInstance) == 8, "the mesh instance stream is 8 bytes");

    // Custom Data's stream: HALF4
    struct ParticleCustomInstance {
        uint16_t data[4];
    };

    // The Pixel render mode's stream (S49 R10): UBYTE4
    struct ParticlePixelInstance {
        uint8_t rampStep;   // the primary colour's ramp step (255: not from a ramp)
        uint8_t age;        // normalized age x 255 (Puff's cap)
        uint8_t phase;      // the particle's random phase (Wobble, Flicker)
        uint8_t flags;      // kPixelFlag*
    };
    static_assert(sizeof(ParticlePixelInstance) == 4, "the pixel stream is 4 bytes a particle");
    inline constexpr uint8_t kPixelFlagRest = 1;    // at rest (Settle Speed)

    // the tiles a Tile-mode trail's u covers (its 16-bit u holds u / kTrailTileRange)
    inline constexpr float kTrailTileRange = 64.0f;

    // A trail vertex: FLOAT3 centre, HALF4 (tangent xyz, half width), UBYTE4N colour (sRGB),
    // USHORT2N (u along the trail, side 0 or 1); the shader turns it to face the camera
    struct TrailVertex {
        float center[3];
        uint16_t tangentWidth[4];
        uint32_t color;
        uint16_t uvSide[2];
    };
    static_assert(sizeof(TrailVertex) == 28, "the trail vertex is 28 bytes (D6)");

    struct RenderView {
        float position[3] = {0, 0, 0};   // the camera, in the emitter's simulation space
        float forward[3] = {0, 0, -1};   // its view direction
        float up[3] = {0, 1, 0};         // its up, in simulation space (Mesh Facing alignment)
        bool sort = false;               // the program sorts and the blend needs it (D7)
        float keep = 1.0f;               // distance thinning: the share drawn
        // Mesh particles' Render Alignment: the rotation (x, y, z, w, simulation space) a View,
        // World or Local alignment puts in front of each particle's own (the camera's, the
        // world's or the emitter's axes, seen from the simulation space)
        float alignQuat[4] = {0, 0, 0, 1};
        // one base pixel of the drawing grid along the screen's right and up, in simulation
        // space (the Pixel Art module's Wobble, S49 R7)
        float pixelRight[3] = {1, 0, 0};
        float pixelUp[3] = {0, 1, 0};
    };

    struct PackResult {
        uint32_t count = 0;
        ParticleInstance* instances = nullptr;
        ParticleMeshInstance* mesh = nullptr;
        ParticleCustomInstance* custom = nullptr;
        ParticlePixelInstance* pixel = nullptr;   // Render Mode Pixel
        // Mesh mode: the records of each mesh, one after the other (one draw each)
        uint32_t meshFirst[kMaxRendererMeshes] = {};
        uint32_t meshCount[kMaxRendererMeshes] = {};
    };

    struct TrailPack {
        uint32_t strips = 0;             // drawn strips
        uint32_t pointsPerStrip = 0;
        TrailVertex* vertices = nullptr; // strips * pointsPerStrip * 2
        bool worldSpace = false;         // the vertices are in world space (else simulation space)
    };

    // Hold Frames (S49 R5): the records of the last step boundary, drawn again until the emitter's
    // time crosses the next step of 1 / rate (the pack runs only then: the CPU saving)
    struct JASPERENGINE_API HeldPack {
        double step = -1.0;
        // what the records were packed from: a reset, clear or seek of the emitter, or another
        // effect, packs again whatever the step
        uint64_t generation = 0;
        const void* effect = nullptr;
        std::vector<ParticleInstance> instances;
        std::vector<ParticlePixelInstance> pixels;
        // the records to draw now (pointing into this struct)
        PackResult get(const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs, float rate);
        void clear(){ step = -1.0; generation = 0; effect = nullptr; instances.clear(); pixels.clear(); }
    };

    // one HeldPack per view that draws the emitter (each camera packs its own sorting and facing)
    struct JASPERENGINE_API HeldPacks {
        static constexpr size_t kMaxViews = 8;
        struct Entry {
            uint64_t view = 0;
            uint64_t lastUse = 0;
            HeldPack pack;
        };
        std::vector<Entry> entries;
        uint64_t uses = 0;
        // the view's pack (made on first use; past kMaxViews the least recently used is reused)
        HeldPack& of(uint64_t view);
        void clear(){ entries.clear(); }
    };

    class JASPERENGINE_API ParticleRenderPrep {
    public:
        // packs the live particles of `emitter` for one view into memory from `arena`; in
        // parallel chunks when there are many. Zero allocations from the heap.
        static PackResult pack(const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs = nullptr);
        // the same with the scalar or the native kernels (tests compare them)
        static PackResult packWith(bool scalar, const EmitterRuntime& emitter, const RenderView& view, memory::Arena& arena, jobs::JobSystem* jobs);
        // how many packs ran since the process started (Hold Frames' measure, S49 AC4)
        static uint64_t packCount();
        // per-particle trails and ribbons
        static TrailPack packTrails(const EmitterRuntime& emitter, memory::Arena& arena);
        // the points of each ribbon's strip when there are several (one strip holds every live
        // particle): the effect's most per ribbon, so the strips' triangles are fixed at load
        static uint32_t ribbonStripPoints(const ParticleProgram& program);

        // the 16-bit LSD radix sort of `count` indices by key (two 8-bit passes), stable; the
        // first pass carries each key next to its index, so the second reads them in order
        static void radixSort16(const uint16_t* keys, uint32_t* indices, uint32_t* scratch, uint16_t* keyScratch, uint32_t count);
        // the same of the indices 0..count-1, written to out (the draw order of a fresh pack)
        static void radixSort16Sequence(const uint16_t* keys, uint32_t* out, uint32_t* scratch, uint16_t* keyScratch, uint32_t count);

        // IEEE half from float (round to nearest even), and back
        static uint16_t toHalf(float value);
        static float fromHalf(uint16_t value);
    };

}}

#endif // PARTICLES_PARTICLERENDER_H
