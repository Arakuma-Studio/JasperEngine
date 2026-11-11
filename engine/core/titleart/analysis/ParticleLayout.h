// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Ported from Pachislop src/generators/particles.ts and src/generators/inkSplatter.ts by SafEight, used with the author's permission.

// Contract (docs/specs/S83-title-effects-generators.md §6.7.1, §6.7.2, R15, R25, D6)
// - Purpose: the CPU half of the Particles family. Particles is a sampler of states, never a
//   simulation (D6): layoutParticles turns the params into one instance record per particle
//   (Pachislop's buildParticles with its emitters: the analytic shapes and the followed layer's
//   emit map), and particleAt evaluates a record at an animation time in closed form, the CPU
//   twin of titleart/gen/particles_motion.glsl. layoutInkSplatter places Ink Splatter's drops
//   on a content grid.
// - Stability (R25): particle i draws its placement from stream(seed, i, 0), its look from
//   stream(seed, i, 1) (eight values in a fixed order) and its motion phase from
//   stream(seed, i, 2), each value drawn before any condition. So changing `count` keeps the
//   records of the lower indices bit-identical, and on the layer emitter a flipped mask cell
//   moves only the particles whose tries land on it (the tries are uniform over the snapped
//   box, not a list of cells). Ink drops are keyed by their try index the same way.
// - Records: positions, sizes and streaks in drawing-space px (canvas px, +y up); colours
//   premultiplied linear. A record holds the particle at time 1 (Pachislop's still) and the
//   displacement it travelled to get there, so time 1 with no gravity, spin or fade reproduces
//   Pachislop's image exactly.
// - Grids: the layer emitter reads the followed layer's content on Pachislop's lattice (its ink
//   box +- 8 canvas px snapped to 64, cells sized from that area: particleGridSpec, from a
//   coarse frame grid's ink box), packed as Color (analysis/ReadGrid.h).
// - Thread-safety: pure functions of their arguments.

#pragma once

#include "Export.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/render/TitleEffectContext.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace jasperengine::titleart::analysis {

    // Pachislop's SHAPE_ID order (the shader's shape ids)
    enum class ParticleShape : uint8_t { Dot, Glint4, Glint6, Ember, Square, Confetti, Petal, Star, Heart, Bokeh, Rain };
    JASPERENGINE_API ParticleShape particleShapeOf(std::string_view name);     // unknown: Dot
    // opaque shapes drawn with normal blending far to near; every other shape is light
    JASPERENGINE_API bool particleShapeSolid(ParticleShape shape);
    // shapes whose rotation is their motion (a streak)
    JASPERENGINE_API bool particleShapeStreaked(ParticleShape shape);
    JASPERENGINE_API bool particleShapeGlint(ParticleShape shape);

    constexpr int PARTICLES_MAX = 5000;
    constexpr int PARTICLES_MOBILE_MAX = 2000;      // §6.7.1: the mobile tier draws the first 2,000

    enum class ParticleColorMode : uint8_t { Solid, Radial, Palette, Sample };

    // the look params read once (Pachislop readLook)
    struct ParticleLook {
        ParticleShape shape = ParticleShape::Dot;
        double glintMix = 0.0;          // 0..1 share of dots / squares drawn as big 4-point glints
        int count = 0;
        double size = 6.0;              // diameter, drawing-space px
        double sizeRandom = 0.0, depth = 0.0;
        double streak = 0.0;            // ember / rain streak length, px
        double ring = 0.0;
        double rotation = 0.0;          // rotation random, radians either way
        double tumble = 0.0, sheen = 0.0;
        ParticleColorMode colorMode = ParticleColorMode::Radial;
        TitleColor color;               // linear
        TitleGradient gradient;
        double opacity = 1.0, opacityRandom = 0.0;
        int seed = 1;
    };
    // `seed` is the element seed (gen::seedOf: the param plus the document's seed - 1)
    JASPERENGINE_API ParticleLook readParticleLook(const ParamValues& p, double seed);

    struct ParticleRecord {
        uint32_t index = 0;             // the particle's number (its streams)
        float x = 0, y = 0;             // centre at time 1, drawing-space px
        float mx = 0, my = 0;           // the displacement travelled by time 1 (born at x - mx, y - my)
        float w = 0, h = 0;             // full quad size, padded for defocus
        float rot = 0;                  // radians counter-clockwise (+y up)
        float color[4] = {0, 0, 0, 0};  // premultiplied linear
        float blur = 0;                 // defocus, in particle radii
        float aspect = 1;
        float phase = 0;                // tumble angle (foil sheen phase)
        float z = 0;                    // 0 far .. 1 near: solids' draw order
        float side = 0;                 // a free uniform: the half in front of / behind a followed layer
        float motionPhase = 0;          // 0..1: the loop offset, twinkle phase and spin rate
        bool glint = false;             // a mixed-in 4-point glint
    };

    // where the analytic emitters live: the canvas and document space -> drawing space (the
    // Full Frame emitter spans the canvas wherever the layer is)
    struct ParticleSpace {
        double canvasWidth = 0.0, canvasHeight = 0.0;
        TitleAffine documentToSpace;
    };

    // Where particles may be born on a followed layer's grid (Pachislop EmitMap)
    struct EmitMap {
        const Grid* grid = nullptr;     // Color packing, cells in canvas px
        std::vector<uint8_t> mask;      // 1 = candidate cell
        double box[4] = {0, 0, 0, 0};   // the candidates' box, canvas px, snapped to 64 (x0, y0, x1, y1)
        std::vector<float> nx, ny;      // unit outward normal per cell
        double cx = 0.0, cy = 0.0;      // the ink's centroid
        double radius = 0.0;            // half the ink box's diagonal
    };
    // the ink (alpha >= 0.5) of `grid`: its fill, or (edge) its edge band bandPx deep; false
    // when it has no ink. `out` keeps a pointer to `grid`
    JASPERENGINE_API bool buildEmitMap(const Grid& grid, bool edge, double bandPx, EmitMap& out);

    struct MapSample {
        double x = 0.0, y = 0.0;        // canvas px
        int cell = -1;
    };
    // a point on the map's candidates from `stream` (uniform tries over the box, up to 400);
    // false when every try missed
    JASPERENGINE_API bool sampleEmitMap(const EmitMap& map, gen::Rng& stream, MapSample& out);

    // Pachislop's per-(seed, particle, purpose) stream: 0 placement, 1 look, 2 motion
    JASPERENGINE_API gen::Rng particleStream(int seed, int index, int salt);

    // every particle's record at `look.count` (records of particles that place nowhere or end
    // invisible are left out; indices stay theirs). `map` feeds the `layer` emitter (null: it
    // places nothing)
    JASPERENGINE_API std::vector<ParticleRecord> layoutParticles(const ParamValues& p, const ParticleLook& look, const ParticleSpace& space,
                                                                 const EmitMap* map);

    // ---- closed-form animation (§6.7.1 "Animation") ----

    struct ParticleMotion {
        double time = 1.0;              // 0..1; 1 = Pachislop's still
        bool loop = false;              // each particle's time = fract(time + its phase)
        double gravity = 0.0;           // px fallen by time 1 (down)
        double spin = 0.0;              // degrees turned by time 1, clockwise (x the particle's rate)
        double twinkle = 0.0;           // 0..1: glints flash with their phase
        double fadeIn = 0.0, fadeOut = 0.0;     // shares of each particle's time its alpha ramps over
    };
    JASPERENGINE_API ParticleMotion readParticleMotion(const ParamValues& p);

    // flashes of a twinkling glint over time 0..1 (whole cycles, so a loop repeats seamlessly)
    constexpr double PARTICLE_TWINKLE_CYCLES = 3.0;

    struct ParticleState {
        double x = 0.0, y = 0.0, rot = 0.0;
        double colorScale = 1.0;        // multiplies the premultiplied colour (fades, twinkle)
    };
    // the record at the motion's time: what titleart/gen/particles_motion.glsl computes per quad
    JASPERENGINE_API ParticleState particleAt(const ParticleRecord& r, const ParticleMotion& m, bool spins, bool glint);

    // ---- grids of the followed layer ----

    // the box (canvas px, +y up) of the cells of `grid` whose alpha is above `threshold`; false
    // when there are none
    JASPERENGINE_API bool gridInkBox(const Grid& grid, float threshold, double box[4]);
    // Pachislop's lattice for the layer emitter around an ink box (contentGrid: the box +- 8
    // canvas px snapped outward to 64, cells max(3, ceil(sqrt(area / 50000))) canvas px, at
    // most 8 taps a side, Color packing)
    JASPERENGINE_API GridSpec particleGridSpec(const double inkBox[4]);

    // ---- Ink Splatter ----

    // one ink primitive: a round cone from a (radius ra) to b (radius rb), canvas px, +y up
    struct InkBlot {
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0, ra = 0.0, rb = 0.0;
        double seed = 0.0;              // 0..1: the ragged rim's noise offset
    };
    // the drops, satellites and streaks thrown off the ink (alpha >= 0.5) of an 8 px content
    // grid (Pachislop inkSplatter render); empty when there is no ink or no drop
    JASPERENGINE_API std::vector<InkBlot> layoutInkSplatter(const Grid& grid, const ParamValues& p, double seed);

}
