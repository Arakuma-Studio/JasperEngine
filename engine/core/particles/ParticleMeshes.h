// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEMESHES_H
#define PARTICLES_PARTICLEMESHES_H

// The meshes particle effects use (Shape > Mesh, Renderer > Meshes): a primitive made here, a
// model asset's mesh (glTF or OBJ, read through the engine's files) or a GameObject's
// MeshComponent. Vertices come interleaved (position, normal, uv: 8 floats) with 32-bit
// triangles; the compiler samples their positions and normals, RenderSystem draws them.

#include "Export.h"
#include "particles/ParticleCompiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    struct MeshComponent;

    namespace particles {

        struct ParticleMeshData {
            static constexpr uint32_t kFloatsPerVertex = 8;
            std::vector<float> vertices;     // x, y, z, nx, ny, nz, u, v
            std::vector<uint32_t> indices;   // triangles
            uint32_t vertexCount() const { return (uint32_t)(vertices.size() / kFloatsPerVertex); }
        };

        class JASPERENGINE_API ParticleMeshes {
        public:
            // a primitive of unit size around the origin: Quad and Plane 1 x 1 (Quad in XY facing
            // +Z, Plane in XZ facing +Y), Cube 1, Sphere / Cylinder / Capsule / Torus 1 across
            static void primitive(PrimitiveMesh primitive, ParticleMeshData& out);
            // the Renderer's Ring: a flat ring in XY facing +Z between the two radii over `arcDegrees`
            // (from +X towards +Y), `segments` around the whole circle; U along the arc, V from the
            // outer edge (0) to the inner (1). An inner radius of 0 makes a disc.
            static void ring(float outerRadius, float innerRadius, float arcDegrees, int segments, ParticleMeshData& out);
            // mesh `index` of a model file (glTF: its primitives in order; OBJ: its shapes)
            static bool model(const std::string& path, int index, ParticleMeshData& out, std::string* error = nullptr);
            // submesh `index` of a GameObject's mesh (its loaded vertices)
            static bool meshComponent(MeshComponent& mesh, int index, ParticleMeshData& out);

            // the positions, normals and triangles the compiler samples (Shape > Mesh)
            static void geometry(const ParticleMeshData& mesh, MeshGeometry& out);
        };

    }

}

#endif // PARTICLES_PARTICLEMESHES_H
