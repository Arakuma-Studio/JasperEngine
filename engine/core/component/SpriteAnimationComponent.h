// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITEANIMATION_COMPONENT_H
#define SPRITEANIMATION_COMPONENT_H

#include "Engine.h"
#include "util/GrowArray.h"

namespace jasperengine{

    struct JASPERENGINE_API SpriteAnimationComponent{
        bool loop = true;

        unsigned int framesTimeSize = 0;
        GrowArray<int> framesTime;

        unsigned int framesSize = 0;
        GrowArray<int> frames;
        // what its arrays hold outside the component (ComponentArray::arrayHeapBytes)
        size_t arrayHeapBytes() const { return framesTime.storageBytes() + frames.storageBytes(); }

        int frameIndex = 0;
        int frameTimeIndex = 0;

        unsigned int spriteFrameCount = 0;
    };
    
}

#endif //SPRITEANIMATION_COMPONENT_H