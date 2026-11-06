// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOCLOCK_H
#define AUDIOCLOCK_H

#include "Export.h"

#include <cstdint>

namespace jasperengine {

    // Turns game time into sample frames with no rounding piling up: each advance gives what
    // brings the cursor to round(all the time so far x rate) (144 fps at 44.1 kHz is 306.25 frames
    // a frame: rounded each time, 48 ms a minute behind the game).
    class JASPERENGINE_API AudioClock {
    public:
        void reset(unsigned int rate);
        // the frames to mix now; a zero or negative time mixes none
        uint32_t advance(double seconds);
        // what advance(seconds) would give now, the clock unchanged
        uint32_t peek(double seconds) const;
        uint64_t cursor() const;
        double seconds() const;

    private:
        unsigned int sampleRate = 0;
        double accumulated = 0.0;
        uint64_t frames = 0;
    };

}

#endif //AUDIOCLOCK_H
