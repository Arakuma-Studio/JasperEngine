// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_SAMPLE_H
#define VOXEL_SAMPLE_H

// R32 and R69: what you can ask a store. All main thread; cold and base blocks resolve synchronously.

#include "Export.h"
#include "VoxelStore.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::voxel {

    struct RaycastHit {
        bool hit = false;
        Vec3d point;      // metres
        Vec3d normal{0.0, 1.0, 0.0};
        double distance = 0.0;
        int slot = 0;
    };

    // R32: an SDF sphere trace over level 0, at most one voxel per step, uniform blocks skipped, the
    // crossing refined by bisection to 1/64 voxel. Origin and direction in metres in the volume's frame.
    JASPERENGINE_API RaycastHit raycast(Store& store, const Vec3d& origin, const Vec3d& direction, double maxDistance);

    // R32: trilinear signed distance in metres; outside the volume, +band
    JASPERENGINE_API double valueAt(Store& store, const Vec3d& point);

    struct MaterialSample {
        int slot = 0;
        int second = 0;
        int weight = 0;    // of the second, 0-255
    };
    JASPERENGINE_API MaterialSample materialAtPoint(Store& store, const Vec3d& point);

    // R69: FNV-1a 64 over the decoded level-0 samples in canonical order, independent of how a block is
    // held. Per block, then over the per-block hashes, so uniform blocks cost a lookup.
    using BlockReader = std::function<BlockContent(const BlockKey&)>;
    JASPERENGINE_API uint64_t contentHash(const Store& store, const BlockReader& reader = {});
    JASPERENGINE_API uint64_t blockContentHash(const BlockContent& content);

    struct AirComponent {
        int64_t voxels = 0;
        IntBox bounds;
        bool openToSky = false;   // touches the volume's top layer, or the air above it
    };

    struct ConnectivityResult {
        std::vector<AirComponent> components;
        bool connected = false;        // from and to asked, both in air and in one component
        bool fromInAir = false;
        bool toInAir = false;
        int fromComponent = -1;
        int toComponent = -1;
    };

    // R69: a 6-connected air flood over level 0 with uniform-block acceleration. from and to are in voxels.
    JASPERENGINE_API ConnectivityResult connectivity(Store& store, const Int3* from = nullptr, const Int3* to = nullptr);

    struct BoxStats {
        int64_t samples = 0;
        double minDistance = 0.0, maxDistance = 0.0;     // voxels
        double solidFraction = 0.0, airFraction = 0.0;
        int64_t surfaceSamples = 0;
        uint16_t slotsSeen = 0;                          // bit per slot
    };
    JASPERENGINE_API BoxStats boxStats(Store& store, const IntBox& box);

    struct VolumeBounds {
        IntBox solid;     // samples
        IntBox surface;   // samples with a neighbour of the other sign
    };
    JASPERENGINE_API VolumeBounds solidBounds(Store& store);

}

#endif
