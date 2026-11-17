// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The fixed meshes of the vertexcolor tests (docs/specs/V1-vertex-colors.md AC1): a two-triangle
// quad, a cube with split normals (24 GPU vertices, 8 logical), a smooth UV sphere with its seam and
// poles duplicated, and a flat grid. All face +Y / outwards.

#include "vertexcolor/VertexColorInternal.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorOps.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor::test {

    inline MeshView built(MeshView mesh) {
        std::string error;
        EXPECT_TRUE(mesh.build(&error)) << error;
        return mesh;
    }

    // (0,0,0) (1,0,0) (1,0,1) (0,0,1), two triangles facing +Y
    inline MeshView quad() {
        MeshView mesh;
        mesh.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1), Vector3(0, 0, 1)};
        mesh.normals.assign(4, Vector3(0, 1, 0));
        mesh.uv0 = {Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)};
        mesh.indices = {0, 3, 1, 1, 3, 2};
        return built(mesh);
    }

    // a unit cube centred on the origin, four GPU vertices per face (split normals)
    inline MeshView cube() {
        MeshView mesh;
        struct Face {
            Vector3 n, u, v;
        };
        const Face faces[6] = {
            {Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0)},  {Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 1, 0)},
            {Vector3(0, 1, 0), Vector3(1, 0, 0), Vector3(0, 0, -1)},  {Vector3(0, -1, 0), Vector3(1, 0, 0), Vector3(0, 0, 1)},
            {Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)},   {Vector3(0, 0, -1), Vector3(-1, 0, 0), Vector3(0, 1, 0)},
        };
        for (const Face& f : faces) {
            const uint32_t first = (uint32_t)mesh.positions.size();
            const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& c : s) {
                mesh.positions.push_back(f.n * 0.5f + f.u * (0.5f * c[0]) + f.v * (0.5f * c[1]));
                mesh.normals.push_back(f.n);
                mesh.uv0.push_back(Vector2(0.5f + 0.5f * c[0], 0.5f + 0.5f * c[1]));
            }
            // u x v = n, so (0, 1, 2) winds outwards
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
        }
        return built(mesh);
    }

    // rings x segments UV sphere of radius 1; the u = 0/1 seam and the poles are duplicated
    inline MeshView sphere(int rings = 6, int segments = 8) {
        MeshView mesh;
        // the module's own series, not libm: the golden hashes must not move with the C library
        const double pi = detail::kPi;
        for (int r = 0; r <= rings; r++) {
            for (int s = 0; s <= segments; s++) {
                const double theta = pi * r / rings;
                const double phi = 2.0 * pi * (s % segments) / segments;
                // the poles and the seam are written bit-identical, as an exporter does
                const float st = r == 0 || r == rings ? 0.0f : (float)detail::detSin(theta);
                const float ct = r == 0 ? 1.0f : (r == rings ? -1.0f : (float)detail::detCos(theta));
                const Vector3 p(st * (float)detail::detCos(phi), ct, st * (float)detail::detSin(phi));
                mesh.positions.push_back(p);
                mesh.normals.push_back(p);
                mesh.uv0.push_back(Vector2((float)s / segments, (float)r / rings));
            }
        }
        auto at = [&](int r, int s) { return (uint32_t)(r * (segments + 1) + s); };
        for (int r = 0; r < rings; r++) {
            for (int s = 0; s < segments; s++) {
                const uint32_t a = at(r, s), b = at(r + 1, s), c = at(r + 1, s + 1), d = at(r, s + 1);
                if (r != rings - 1) mesh.indices.insert(mesh.indices.end(), {a, c, b});
                if (r != 0) mesh.indices.insert(mesh.indices.end(), {a, d, c});
            }
        }
        return built(mesh);
    }

    // an n x n grid of vertices on y = 0 spaced `step` from the origin, facing +Y
    inline MeshView grid(int n = 11, float step = 0.1f) {
        MeshView mesh;
        for (int z = 0; z < n; z++) {
            for (int x = 0; x < n; x++) {
                mesh.positions.push_back(Vector3(x * step, 0.0f, z * step));
                mesh.normals.push_back(Vector3(0, 1, 0));
                mesh.uv0.push_back(Vector2((float)x / (n - 1), (float)z / (n - 1)));
            }
        }
        for (int z = 0; z + 1 < n; z++) {
            for (int x = 0; x + 1 < n; x++) {
                const uint32_t a = (uint32_t)(z * n + x), b = a + 1, c = a + (uint32_t)n + 1, d = a + (uint32_t)n;
                mesh.indices.insert(mesh.indices.end(), {a, d, b, b, d, c});
            }
        }
        return built(mesh);
    }

    inline uint32_t gridVertex(int x, int z, int n = 11) {
        return (uint32_t)(z * n + x);
    }

    // local space, white start, RGBA16F unless said: exact enough to compare maths, not bytes
    inline ApplyOptions localOptions(Format format = Format::RGBA16F) {
        ApplyOptions options;
        options.world = false;
        options.initial = InitialValue::White;
        options.format = format;
        return options;
    }

    inline Op fillOp(const Vector4& value, Target target = Target()) {
        Op op;
        op.type = OpType::Fill;
        op.value = value;
        op.target = target;
        return op;
    }

    inline Target idsTarget(TargetKind kind, std::vector<uint32_t> ids) {
        Target target;
        target.kind = kind;
        target.ids = std::move(ids);
        return target;
    }

    inline OpsResult apply(TargetData& data, const MeshView& mesh, std::vector<Op> ops, const ApplyOptions& options, bool dryRun = false) {
        return applyOps(data, mesh, std::span<const Op>(ops.data(), ops.size()), options, dryRun);
    }

    inline std::string hex(uint64_t value) {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)value);
        return buffer;
    }

    inline Vector4 half(const Vector4& v) {
        return quantize(v, Format::RGBA16F);
    }

}
