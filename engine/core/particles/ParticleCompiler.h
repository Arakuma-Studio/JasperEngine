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
#include "math/Vector4.h"
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
        // a parent sub-emitter's Follow Parent: this effect's particles keep moving with theirs
        // (and turning with them, and scaling with their size)
        bool followsParent = false;
        bool followsRotation = false;
        bool followsScale = false;
    };

    class JASPERENGINE_API ParticleCompiler {
    public:
        // validates and compiles; on errors `effect.valid` is false and the program is empty
        static std::shared_ptr<CompiledEffect> compile(const ParticleEffectDocument& document, const CompileEnvironment& environment = {});
        static void compile(const ParticleEffectDocument& document, const CompileEnvironment& environment, CompiledEffect& effect);
        // the compiles since the process started (any thread; wraps at 2^32)
        static uint32_t compileCount();

        // Exposed parameter `parameter` (its index in the document's list) to `value` (as
        // ParticleModules::setExposed takes it) in the effect's document and program, without
        // compiling (S91 R10): its curve or colour value's params are written where they sit.
        // False, and the effect unchanged, when it cannot be done that way: a field that is not a
        // curve or colour value, a value that changes what the compiler decides (a mode, a zero, a
        // LUT's size), or a use of it the compiler reads otherwise. Compile it then.
        static bool applyExposed(CompiledEffect& effect, size_t parameter, const Vector4& value);

        // the LUT entries a curve bakes into: 65 and doubling, until linear interpolation
        // between them stays within `tolerance` of the curve (at most 1025)
        static uint32_t lutSize(const Curve& curve, float tolerance);
        // 256 linear values of the RGBA8 sRGB channel values (gamma 2.2)
        static const float* srgbDecodeTable();
        // The emitter shape's outline as line segments (point pairs) in the emitter's space,
        // with the shape's position, rotation and scale: the Scene view's gizmo
        static void shapeOutline(const ShapeModule& shape, std::vector<Vector3>& segments);
    };

}}

#endif // PARTICLES_PARTICLECOMPILER_H
