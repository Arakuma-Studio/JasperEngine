// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOTAP_H
#define AUDIOTAP_H

#include "Export.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine {

    // The deterministic driver's output: its clients (a capture, the Recorder) read the same
    // chunks, so what they write is the same samples and no client mixes on its own. On the main
    // thread, in subscription order, after each chunk the pump mixed.
    class JASPERENGINE_API AudioTap {
    public:
        // frames x the output's channels of interleaved samples; firstFrame: the first one's place
        // on the driver's clock
        using Fn = std::function<void(const float* interleaved, uint32_t frames, uint64_t firstFrame)>;

        static int subscribe(std::string name, Fn fn);
        static void unsubscribe(int id);
        static void publish(const float* interleaved, uint32_t frames, uint64_t firstFrame);
    };

}

#endif //AUDIOTAP_H
