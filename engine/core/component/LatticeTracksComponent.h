// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_TRACKS_COMPONENT_H
#define LATTICE_TRACKS_COMPONENT_H

#include "Engine.h"
#include "math/Vector3.h"
#include <vector>

namespace jasperengine{

    // Keyframed lattice offsets (spec §7). Sparse: values[key][n] is the offset of point
    // points[n] (flat index). Hermite tangents mirror values like MorphTracksComponent;
    // empty on linear tracks.
    struct JASPERENGINE_API LatticeTracksComponent{
        std::vector<int> points;
        std::vector<std::vector<Vector3>> values;
        std::vector<std::vector<Vector3>> inTangents;
        std::vector<std::vector<Vector3>> outTangents;
    };

}

#endif //LATTICE_TRACKS_COMPONENT_H
