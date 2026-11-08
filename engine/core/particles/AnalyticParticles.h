// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The closed-form motion under gravity and linear drag (fly, flyV) and the seamless loop of
// stateless particles (cycle) follow pixelfx's src/core/particles.js, MIT License, Copyright (c)
// 2026 Patch. See NOTICE.md.

#ifndef PARTICLES_ANALYTICPARTICLES_H
#define PARTICLES_ANALYTICPARTICLES_H

// Analytic simulation (docs/specs/S49-pixel-particles.md R15-R19, D2, D3): an effect compiled to
// a fixed number of slots, each holding at time t the newest particle born into it, computed
// from the slot and t alone: no state on the GPU or the CPU, exact seeks and loops. The GPU
// evaluates it per instance in includes/particles_analytic_vs.glsl from AnalyticBlock (the
// members of u_vs_particlesParams the 'Ana' variant declares, byte for byte); evaluate() is its
// CPU twin, the same float32 formulas in the same order, for the bounds, counts, picking and
// tests. The randoms are S20's (hashRandom with the compiled streams' keys), so an Analytic
// particle draws the numbers its Simulated twin would.

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "particles/ParticleEffect.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace particles {

    class ParticleProgram;

    inline constexpr uint32_t kAnalyticMaxSlots = 65536;
    inline constexpr int kAnalyticMaxBursts = 4;
    inline constexpr int kAnalyticCurveSamples = 32;
    inline constexpr int kAnalyticColorEntries = 32;

    // The GPU block (std140: every member a vec4 or an array of them). Field comments name the
    // GLSL member.
    struct alignas(16) AnalyticBlock {
        float slots[4];        // analyticSlots: x rate slots, y burst slots, z N_loop (0: not looping), w copies (looping)
        float timing[4];       // analyticTiming: x rate (per second), y duration, z lifetime min, w max
        float frame[4];        // analyticFrame: x the time (the loop's phase when looping), y the loop count, z the emission's end, w flags (1 born-check off)
        // analyticKeys: 12 random keys of 32 bits in 16-bit halves (group g: row 2g the low halves,
        // 2g + 1 the high), exact as floats: the GL backends flatten a block only when every
        // member is of one type, and an unflattened block is limited to 16 members. Groups:
        // 0 lifetime, speed, size, rotation; 1 colour, shape, flip, phase; 2 wobble x, wobble y,
        // burst probability, burst count
        float keys[6][4];
        float start[4];        // analyticStart: speed min, max, size min, max
        float start2[4];       // analyticStart2: rotation min, max (radians), flip share, angular velocity (radians/s)
        float color0[4];       // analyticColor0: Start Color (sRGB, alpha) or the range's first
        float color1[4];       // analyticColor1: the range's second
        float motion[4];       // analyticMotion: xyz g (simulation space), w drag k (1/s)
        float velocity[4];     // analyticVelocity: xyz the constant Velocity over Lifetime, w colour modes (start + 4 x lifetime)
        float shape[4];        // analyticShape: kind, radius, inner ratio, arc (radians)
        float shape2[4];       // analyticShape2: tan(angle), length, donut radius, emit from
        float shape3[4];       // analyticShape3: box half extents, randomize direction
        float shape4[4];       // analyticShape4: box inner ratios, randomize position
        float shapeXform[3][4];  // analyticShapeXform: rows of the shape's 3x4 transform
        float shapeRot[3][4];    // analyticShapeRot: rows of its rotation, w of row 0: spherize direction
        float sheet[4];        // analyticSheet: x kind (0 none, 1 by lifetime, 2 by FPS), y frames, z cycles or FPS, w start frame
        float wobble[4];       // analyticWobble: amplitude x, y (base px), frequency x, y (Hz)
        float pixel[4];        // analyticPixel: flicker below, flicker rate, random wobble phase (1), fade curve (1)
        float ramp[4];         // analyticRamp: x colour table entries, y bias, z reverse (1), w size curve (1)
        float lifetimeRemap[4];  // analyticRemap: Color over Lifetime's driver scale, offset, z the table stepped (1), w its source (0 none, 1 Start Color, 2 Color over Lifetime)
        float extra[4];          // analyticExtra: x the Renderer's emission intensity
        float bursts[kAnalyticMaxBursts][4];      // analyticBursts: time, interval, cycles (0: for ever), probability
        float burstSlots[kAnalyticMaxBursts][4];  // analyticBurstSlots: count min, max, first slot, copies x loop copies (copies in w)
        float sizeCurve[kAnalyticCurveSamples / 4][4];   // analyticSizeCurve: 32 samples over normalized age
        float fadeCurve[kAnalyticCurveSamples / 4][4];   // analyticFadeCurve: the Pixel Art module's Fade over Lifetime
        float colorTable[kAnalyticColorEntries][4];      // analyticColorTable: sRGB + alpha (a ramp's exact bytes / 255, or a gradient's samples)
    };
    static_assert(sizeof(AnalyticBlock) % 16 == 0, "std140");

    inline uint32_t analyticKey(const AnalyticBlock& b, int group, int i){
        return (uint32_t)b.keys[group * 2][i] | ((uint32_t)b.keys[group * 2 + 1][i] << 16);
    }

    inline void setAnalyticKey(AnalyticBlock& b, int group, int i, uint32_t key){
        b.keys[group * 2][i] = (float)(key & 0xffffu);
        b.keys[group * 2 + 1][i] = (float)(key >> 16);
    }

    struct AnalyticProgram {
        bool valid = false;
        uint32_t slotCount = 0;         // rate + burst slots: the instances of its draw
        uint32_t rateSlots = 0, burstSlots = 0;
        uint32_t loopCount = 0;         // N_loop (0: not looping)
        uint32_t copies = 1;
        float rate = 0.0f;              // snapped for an exact loop
        float duration = 0.0f;
        bool looping = false;
        bool prewarm = false;
        float holdFrameRate = 0.0f;
        float maxSize = 1.0f;
        // what setMotion folds into g and the start velocity: the gravity modifier, the constant
        // Force over Lifetime and Velocity over Lifetime (local, or world turned into the simulation space)
        float gravityModifier = 0.0f;
        Vector3 force = Vector3(0.0f, 0.0f, 0.0f);
        bool forceWorld = false;
        Vector3 velocity = Vector3(0.0f, 0.0f, 0.0f);
        bool velocityWorld = false;
        // the bounds of every particle it can draw, simulation space (min, max)
        float boundsMin[3] = {0, 0, 0};
        float boundsMax[3] = {0, 0, 0};
        AnalyticBlock block{};
    };

    // one slot at one time (the vertex program's values)
    struct AnalyticParticle {
        bool alive = false;
        uint32_t index = 0;             // its random identity (n mod N_loop)
        float age = 0.0f, life = 1.0f;
        Vector3 position;               // simulation space, before the wobble
        Vector3 velocity;
        float rotation = 0.0f;          // radians, clockwise as seen
        Vector2 size;
        uint32_t color = 0xffffffffu;   // RGBA8, sRGB rgb (the record's encoding)
        uint8_t rampStep = 255;
        Vector2 wobble;                 // base pixels
        float frame01 = 0.0f;
    };

    class JASPERENGINE_API AnalyticParticles {
    public:
        // The R15 diagnostics of a document set to Analytic: every present, enabled module (or
        // setting) it cannot compute is an error analytic_unsupported at modules.<m>; a Moving
        // World emitter is the runtime's warning. True without errors.
        static bool validate(const ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics);
        // compiles a valid Analytic document's program for one seed; false (analytic_capacity
        // when over kAnalyticMaxSlots) when it cannot
        static bool compile(const ParticleEffectDocument& document, const ParticleProgram& program, uint32_t seed,
                            AnalyticProgram& out, std::vector<Diagnostic>& diagnostics);
        // g (the world gravity times the modifier, plus the force) and the constant velocity in the
        // simulation space; worldToSim the world-to-simulation rotation (column-major 3x3)
        static void setMotion(AnalyticProgram& program, const float worldGravity[3], const float worldToSim[9]);
        // the time fed to the GPU (R17): Hold Frames' step, then the loop's phase and count
        static void setTime(AnalyticProgram& program, double emitterTime, double emissionEnd);
        // the GPU's twin: slot `slot` at the time setTime() last set
        static AnalyticParticle evaluate(const AnalyticProgram& program, uint32_t slot);
        // the same at `emitterTime` (no emission end), leaving the program's time as it was
        static AnalyticParticle evaluateAt(const AnalyticProgram& program, uint32_t slot, double emitterTime);
        // the live slots at setTime()'s time (O(slots))
        static uint32_t aliveCount(const AnalyticProgram& program);
        // pixelfx's fly / flyV in float32: the position and velocity after t under g and drag k
        static Vector3 flyPosition(const Vector3& p0, const Vector3& v0, const Vector3& g, float k, float t);
        static Vector3 flyVelocity(const Vector3& v0, const Vector3& g, float k, float t);
        // the rate an exact loop of `duration` takes (N_loop = rate x duration, a whole number)
        static float loopRate(float rate, float duration);
    };

}}

#endif // PARTICLES_ANALYTICPARTICLES_H
