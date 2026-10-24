// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MORPHTRACKS_H
#define MORPHTRACKS_H

#include "KeyframeTracks.h"

namespace jasperengine{
    class JASPERENGINE_API MorphTracks: public KeyframeTracks{

    public:
        MorphTracks(Scene* scene);
        MorphTracks(Scene* scene, Entity entity);
        MorphTracks(Scene* scene, std::vector<float> times, std::vector<std::vector<float>> values);

        void setValues(std::vector<std::vector<float>> values);
    };
}

#endif //MORPHTRACKS_H