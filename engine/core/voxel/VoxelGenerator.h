// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_GENERATOR_H
#define VOXEL_GENERATOR_H

// R33-R36: what a volume's untouched blocks are. Pure functions of a block's sample positions, so
// a coarse block never needs its fine blocks and anything here runs on a mesh job.

#include "Export.h"
#include "VoxelEdit.h"
#include "VoxelStore.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::voxel {

    // R35: adapted from T1's surface query, so this header needs no terrain type (D1, D5). Metres in
    // the volume's frame; thread-safe.
    struct TerrainLayerSample {
        int slot = 0;        // the volume's slot of the strongest layer
        int second = 0;      // of the second
        int secondWeight = 0;// 0-255
    };
    class TerrainSource {
    public:
        virtual ~TerrainSource() = default;
        virtual float height(float x, float z) const = 0;
        // dh/dx, dh/dz
        virtual void gradient(float x, float z, float& dhdx, float& dhdz) const = 0;
        virtual TerrainLayerSample layers(float x, float z) const = 0;
    };

    // The base with nothing in it
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeEmptyBase();
    // Solid under a height in metres, a true distance, one material
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeSolidGroundBase(double heightMetres, double voxelSize, int slot);

    struct TerrainBaseOptions {
        double voxelSize = 1.0;
        double overlapDepth = 0.02;   // metres: the seed sits this far under the terrain (R41)
    };
    // R35: (y - h + overlapDepth) * c, c = 1 / sqrt(1 + |grad h|^2)
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeTerrainBase(std::shared_ptr<const TerrainSource> terrain, const TerrainBaseOptions& options);
    // The slope factor of R35 and R39
    JASPERENGINE_API double slopeFactor(double dhdx, double dhdz);

    // R35: mix(other, terrain, b), b going from 1 at the footprint edge to 0 at blendWidth inward
    // (smoothstep), so a cave fades to exact terrain before the box ends
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeBlendedBase(std::shared_ptr<const BaseSource> other, std::shared_ptr<const BaseSource> terrain,
                                                                       const Int3& dimensions, double blendWidthVoxels);

    // A stamp field as a base (R36: "a stamp or a base")
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeFieldBase(std::shared_ptr<const StampField> field, double voxelSize, int slot);

    // A seeded chain of capsules dug into another base (R37's cave_entrance)
    struct CarveCapsule {
        Vec3d from, to;       // voxels
        double radius = 1.0;  // voxels
    };
    JASPERENGINE_API std::shared_ptr<const BaseSource> makeCarvedBase(std::shared_ptr<const BaseSource> other, std::vector<CarveCapsule> capsules);

    // The canonical JSON of what R33 stores (V10), and the source hash that says whether it went stale
    struct GeneratorDesc {
        std::string kind = "empty";    // empty, solid_ground, noise_graph, terrain, mesh, preset
        std::string sourceGuid;
        std::string sourcePath;
        uint64_t sourceHash = 0;
        uint64_t seed = 0;
        double height = 0.0;
        double graphScale = 1.0;
        double blendWidth = 0.0;
        double overlapDepth = 0.0;
        std::string preset;
        std::string otherKind;         // blended with the terrain: what it blends
        std::string otherGuid;
        std::string otherPath;
    };
    JASPERENGINE_API std::string generatorToJson(const GeneratorDesc& desc);
    JASPERENGINE_API bool generatorFromJson(const std::string& json, GeneratorDesc& desc);

}

#endif
