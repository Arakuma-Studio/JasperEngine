// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_EDIT_H
#define VOXEL_EDIT_H

// R26-R31: one stateless operation set behind the brush, MCP, Lua and generators. A call validates
// everything before any write, works in a transaction, and either commits all of it or nothing.
// Deterministic (R28): no libm but sqrt and floor, sources built with no contraction, and block order
// is irrelevant because each block's output depends only on the transaction's earlier ops.

#include "Export.h"
#include "VoxelStore.h"
#include "VoxelTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace jasperengine::voxel {

    enum class EditKind : uint8_t { Dig, Add, Smooth, Paint };
    enum class ShapeKind : uint8_t { Sphere, Box, Capsule, Mesh, Stamp };

    enum class EditError : uint8_t {
        None,
        OutOfBounds,
        InvalidShape,
        InvalidMaterial,
        MissingAsset,
        BudgetExceeded,
        NotLoaded
    };

    // R27's codes, as the tools and Lua name them
    JASPERENGINE_API const char* editErrorCode(EditError error);

    // A signed distance grid: a stamp asset's level 0, or a voxelized mesh (R36). Immutable once built.
    struct StampField {
        Int3 size;
        double voxelSize = 1.0;            // metres per sample
        std::vector<int16_t> sdf;          // x fastest
        double originOffset[3] = {0.0, 0.0, 0.0};   // metres from the field's sample 0 to the shape's origin
        bool nonWatertight = false;        // R36
        int triangleCount = 0;             // R30: a mesh's cost is triangles times samples
    };

    struct Quatd {
        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
    };

    // Positions and lengths are metres in the volume's frame
    struct Shape {
        ShapeKind kind = ShapeKind::Sphere;
        Vec3d center;
        double radius = 1.0;
        Vec3d halfExtents{1.0, 1.0, 1.0};
        Quatd rotation;
        Vec3d from, to;
        std::shared_ptr<const StampField> field;   // Mesh and Stamp; null means the asset is missing
        double scale = 1.0;                        // Stamp and Mesh, uniform
    };

    struct EditOp {
        EditKind kind = EditKind::Dig;
        Shape shape;
        double blend = 0.0;       // metres; smooth-CSG radius, polynomial
        int material = -1;        // -1: writes no material
        double weight = 1.0;      // absolute, 0-1
        double falloff = 0.0;     // 0-1, the shape's edge softness for paint and smooth
        double amount = 0.5;      // smooth, 0-1
        int iterations = 1;       // smooth, 1-8
    };

    struct EditOptions {
        bool dryRun = false;
        bool hash = false;               // content_hash_after; it may resolve cold blocks
        int64_t maxVoxels = 0;           // R30's per-call budget, 0 = none
        int slotCount = MAX_SLOTS;       // the volume's slots: a material beyond them is invalid
        int debrisCount = 0;             // R31: up to this many points on the changed solid samples
        uint64_t debrisSeed = 0;         // the edit's index
        bool captureDelta = false;       // R29: keep old and new slots per touched block
    };

    struct EditResult {
        bool ok = true;
        EditError error = EditError::None;
        IntBox changedVoxels;
        int64_t voxelsChanged = 0;
        int64_t surfaceVoxelsChanged = 0;     // sign flipped
        int blocksTouched = 0;
        int blocksAllocated = 0;
        int blocksCollapsed = 0;
        int64_t memoryDelta = 0;
        int64_t clampedVoxels = 0;
        double removedVolume = 0.0;           // cubic metres
        double addedVolume = 0.0;
        std::array<double, MAX_SLOTS> removedByMaterial{};
        int holesCut = 0;                     // filled by the terrain integration (R40)
        int holesFilled = 0;
        bool entranceNarrow = false;          // T3 R27's warning (R40)
        bool hasHash = false;
        uint64_t contentHashAfter = 0;
        std::vector<EditResult> perOp;        // only for a call with several ops
        std::vector<Vec3d> debrisPoints;      // metres
        std::vector<SlotChange> delta;        // captureDelta and not dryRun
    };

    JASPERENGINE_API EditResult applyEdit(Store& store, std::span<const EditOp> ops, const EditOptions& options = {});

    // Undo and redo (R29): swaps the references the delta holds. O(touched blocks) and byte-exact.
    JASPERENGINE_API void applyDelta(Store& store, const std::vector<SlotChange>& delta, bool undo);

    // The box a shape reaches, in samples, without the band
    JASPERENGINE_API IntBox shapeSampleBox(const VolumeDesc& desc, const Shape& shape);

}

#endif
