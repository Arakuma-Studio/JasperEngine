// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef KEYFRAMETRACKS_H
#define KEYFRAMETRACKS_H

#include "action/Action.h"
#include "action/Ease.h"

namespace jasperengine{
    // The key times and per-segment easings every track type shares
    class JASPERENGINE_API KeyframeTracks: public Action{

    public:
        KeyframeTracks(Scene* scene);
        KeyframeTracks(Scene* scene, Entity entity);

        void setTimes(std::vector<float> times);
        void setEasings(std::vector<EaseType> easings);
        void setEasing(unsigned int segment, EaseType ease);

        // starts over after the last key
        void setLoop(bool loop);
        bool isLoop() const;

        // values are offsets from the target's pose when the track starts (transform tracks)
        void setRelative(bool relative);
        bool isRelative() const;
    };
}

#endif //KEYFRAMETRACKS_H
