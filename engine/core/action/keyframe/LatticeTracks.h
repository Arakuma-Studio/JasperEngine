// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICETRACKS_H
#define LATTICETRACKS_H

// LatticeTracks action (spec §7), mirrors MorphTracks. Keyframes the offsets of a sparse
// set of lattice points: values[key][n] is the offset of point points[n] (flat index
// i + rx * (j + ry * k)). The target must have a LatticeComponent; points out of range for
// its resolution are ignored, points no running track animates keep their offsets.

#include "KeyframeTracks.h"

namespace jasperengine{
    class JASPERENGINE_API LatticeTracks: public KeyframeTracks{

    public:
        LatticeTracks(Scene* scene);
        LatticeTracks(Scene* scene, Entity entity);
        LatticeTracks(Scene* scene, std::vector<float> times, std::vector<int> points, std::vector<std::vector<Vector3>> values);

        void setPoints(std::vector<int> points);
        void setValues(std::vector<std::vector<Vector3>> values);
    };
}

#endif //LATTICETRACKS_H
