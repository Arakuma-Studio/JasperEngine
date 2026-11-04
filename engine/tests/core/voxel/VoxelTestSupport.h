// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_TEST_SUPPORT_H
#define VOXEL_TEST_SUPPORT_H

// Bases and helpers the voxel tests share (T6)

#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelSample.h"
#include "voxel/VoxelStore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

namespace voxeltest {

    using namespace jasperengine::voxel;

    // Any function of position in voxels as a base
    class FunctionBase : public BaseSource {
    public:
        using Fn = std::function<double(double, double, double)>;
        using MaterialFn = std::function<uint16_t(int, int, int)>;
        explicit FunctionBase(Fn fn, MaterialFn materialFn = {}): fn(std::move(fn)), materialFn(std::move(materialFn)) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int px = origin.x + x * spacing, py = origin.y + y * spacing, pz = origin.z + z * spacing;
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encodeSdf(fn(px, py, pz));
                        material[index] = materialFn ? materialFn(px, py, pz) : 0;
                    }
                }
            }
        }

    private:
        Fn fn;
        MaterialFn materialFn;
    };

    inline VolumeDesc desc(int x, int y, int z, int levels = 1, BoundsMode bounds = BoundsMode::Closed, float voxelSize = 1.0f) {
        VolumeDesc out;
        out.dimensions = Int3{x, y, z};
        out.levels = levels;
        out.bounds = bounds;
        out.voxelSize = voxelSize;
        return out;
    }

    inline std::shared_ptr<BaseSource> sphereBase(double cx, double cy, double cz, double radius) {
        return std::make_shared<FunctionBase>([=](double x, double y, double z) {
            const double dx = x - cx, dy = y - cy, dz = z - cz;
            return std::sqrt(dx * dx + dy * dy + dz * dz) - radius;
        });
    }

    // A rolling field with real crossings on every level
    inline std::shared_ptr<BaseSource> wavyBase(double height) {
        return std::make_shared<FunctionBase>([=](double x, double y, double z) {
            return y - height - 3.0 * std::sin(x * 0.13) * std::cos(z * 0.11) - 1.5 * std::sin(x * 0.31 + z * 0.27);
        });
    }

    inline std::shared_ptr<const MeshOut> meshOf(Store& store, const BlockKey& key, uint8_t mask = 0, MeshMode mode = MeshMode::Full, const Coverage* coverage = nullptr) {
        auto out = std::make_shared<MeshOut>();
        const auto view = store.readView(key, mask);
        MeshRequest request;
        request.key = key;
        request.sideMask = mask;
        request.mode = mode;
        request.coverage = coverage;
        meshBlock(*view, request, *out);
        store.publish(view->takeResolved());
        return out;
    }

    inline Shape sphereShape(double x, double y, double z, double radius) {
        Shape shape;
        shape.kind = ShapeKind::Sphere;
        shape.center = Vec3d{x, y, z};
        shape.radius = radius;
        return shape;
    }

    inline EditOp digOp(const Shape& shape) {
        EditOp op;
        op.kind = EditKind::Dig;
        op.shape = shape;
        return op;
    }

    inline EditOp addOp(const Shape& shape, int material = -1) {
        EditOp op;
        op.kind = EditKind::Add;
        op.shape = shape;
        op.material = material;
        return op;
    }

    // Sorted triangles as position triples, whatever the vertex order: what "the same mesh" means
    inline std::vector<std::array<int32_t, 9>> sortedTriangles(const MeshOut& mesh) {
        std::vector<std::array<int32_t, 9>> out;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            std::array<std::array<int32_t, 3>, 3> corners;
            for (int c = 0; c < 3; c++) {
                const MeshVertex& v = mesh.vertices[mesh.indices[i + static_cast<size_t>(c)]];
                corners[static_cast<size_t>(c)] = {v.position[0], v.position[1], v.position[2]};
            }
            // Rotate the smallest corner first, keeping the winding
            size_t first = 0;
            for (size_t c = 1; c < 3; c++) {
                if (corners[c] < corners[first]) first = c;
            }
            std::array<int32_t, 9> triangle;
            for (size_t c = 0; c < 3; c++) {
                const auto& corner = corners[(first + c) % 3];
                triangle[c * 3] = corner[0];
                triangle[c * 3 + 1] = corner[1];
                triangle[c * 3 + 2] = corner[2];
            }
            out.push_back(triangle);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

}

#endif
