// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCALETRACKS_H
#define SCALETRACKS_H

#include "KeyframeTracks.h"

namespace jasperengine{
    class JASPERENGINE_API ScaleTracks: public KeyframeTracks{

    public:
        ScaleTracks(Scene* scene);
        ScaleTracks(Scene* scene, Entity entity);
        ScaleTracks(Scene* scene, std::vector<float> times, std::vector<Vector3> values);

        void setValues(std::vector<Vector3> values);
    };
}

#endif //SCALETRACKS_H