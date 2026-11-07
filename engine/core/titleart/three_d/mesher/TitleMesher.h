// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R13-R15, §6.2, §6.7)
// - Purpose: one 3D layer's mesh from its source and style at a tier: outlines flattened and
//   normalised (cached by the outline inputs: the shape cache), solids and the indexed mesh
//   (cached by the geometry key: geometry fields only, so materials, lighting, transforms, skew
//   and rig values never rebuild), inflate fields cached by (outline hash, step).
// - The geometry key covers the source outlines, which characters are rigged, the geometry
//   members of style_3d, the material blocks, the tier, draft, the wrap stream and the output
//   scale as the mesher sees it (effective bevel segments, the inflate grid's power of two, the
//   orb grid).
// - Thread-safety: build() may run on any thread (the renderer's background job); the caches lock.

#pragma once

#include "Export.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/mesher/InflateMesher.h"
#include "titleart/three_d/mesher/TitleSolids.h"

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>

namespace jasperengine::titleart::three_d {

    struct TitleMeshRequest {
        TitleSource3D source;
        Title3D style;
        std::map<uint32_t, CharBlock3D> blocks;
        TitleTier tier = TitleTier::Bake;
        double outputScale = 1.0;       // output px per layer unit
        bool draft = false;
        bool wrap = false;              // a material of the layer projects `wrap`
        uint32_t testFlags = 0;
    };

    JASPERENGINE_API uint64_t titleGeometryKey(const TitleMeshRequest& request);
    // the outline part of the key (the shape cache)
    JASPERENGINE_API uint64_t titleShapeKey(const TitleSource3D& source, int curveSegments);

    class JASPERENGINE_API TitleMesher {
    public:
        explicit TitleMesher(size_t meshCapacity = 16, size_t shapeCapacity = 32);

        std::shared_ptr<const TitleMesh> build(const TitleMeshRequest& request);
        // a cached mesh of `geometryKey`, null when none
        std::shared_ptr<const TitleMesh> cached(uint64_t geometryKey);

        uint64_t meshBuilds() const { return meshCount.load(); }
        uint64_t shapeBuilds() const { return shapeCount.load(); }
        InflateFieldCache& fields() { return inflate; }
        void clear();

    private:
        std::mutex lock;
        std::list<std::pair<uint64_t, std::shared_ptr<const TitleMesh>>> meshes;
        std::list<std::pair<uint64_t, std::shared_ptr<const TitleShapes3D>>> shapes;
        size_t meshCapacity, shapeCapacity;
        InflateFieldCache inflate;
        std::atomic<uint64_t> meshCount{0}, shapeCount{0};
    };

}
