// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLECOMPILER_H
#define PARTICLES_PARTICLECOMPILER_H

// Compiles a particle effect document into a ParticleProgram (docs/specs/S20-particles.md D2,
// D5, D10): the channels its enabled modules need, its spawn and update op lists, the render
// extraction's parameters, and every curve and gradient baked into a lookup table. Follows
// OpenEngine's ParticleStackCompiler (Engine/Modules/Particles/Source/ParticleStackCompiler.cpp),
// without its phases.

#include "Export.h"
#include "particles/ParticleProgram.h"

#include <functional>
#include <memory>
#include <vector>

namespace jasperengine { namespace particles {

    // CPU geometry of a mesh (Shape > Mesh): positions and normals (x, y, z each), triangles
    struct MeshGeometry {
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<uint32_t> indices;
    };

    struct CompileEnvironment {
        // the geometry of a mesh source (a model asset, a primitive, the GameObject's mesh);
        // false when it cannot be read (Shape > Mesh then emits from the emitter's origin)
        std::function<bool(const MeshSource& source, MeshGeometry& out)> mesh;
        // a parent sub-emitter hands its particles' rotation to this effect's
        bool inheritsRotation = false;
    };

    class JASPERENGINE_API ParticleCompiler {
    public:
        // validates and compiles; on errors `effect.valid` is false and the program is empty
        static std::shared_ptr<CompiledEffect> compile(const ParticleEffectDocument& document, const CompileEnvironment& environment = {});
        static void compile(const ParticleEffectDocument& document, const CompileEnvironment& environment, CompiledEffect& effect);

        // the LUT entries a curve bakes into: 65 and doubling, until linear interpolation
        // between them stays within `tolerance` of the curve (at most 1025)
        static uint32_t lutSize(const Curve& curve, float tolerance);
        // 256 linear values of the RGBA8 sRGB channel values (gamma 2.2)
        static const float* srgbDecodeTable();
    };

}}

#endif // PARTICLES_PARTICLECOMPILER_H
