// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A process of its own (jasperengine-texsynth-oom, built next to the editor tests and run by
// TexSynthOutOfMemory.IsAFailedStatusNotATerminate): a synthesis whose allocations fail under an
// address-space limit ends with Status::Failed "out of memory", never std::terminate. The test
// binary can't run it itself: S02's TestMain runs the engine there (many threads, each with its
// malloc arena), so the free memory the limit leaves can't be told from the process size.
// Exit 0: the expected failed status; 1: anything else (printed).

#include "texsynth/TextureSynthesis.h"

#include <sys/resource.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

using namespace jasperengine::editor::texsynth;

namespace {

    Image noise(int w, int h, int channels, uint32_t seed) {
        Image image = Image::make(w, h, channels, 8);
        std::mt19937 rng(seed);
        for (auto& byte : image.data) byte = static_cast<uint8_t>(rng());
        return image;
    }

}

int main() {
    Request request;
    request.mode = Mode::Guided;
    request.quality = Quality::Draft;
    request.examples.push_back(Example::fromImage(noise(2048, 2048, 4, 1)));
    request.targetGuide = ImageInput::fromImage(noise(64, 64, 3, 2));

    long pages = 0;
    std::ifstream("/proc/self/statm") >> pages;
    const rlim_t limit = static_cast<rlim_t>(pages) * 4096 + (40u << 20);
    rlimit rl;
    rl.rlim_cur = limit;
    rl.rlim_max = limit;
    setrlimit(RLIMIT_AS, &rl);

    const Result result = synthesize(request);
    const bool failed = result.status == Status::Failed && !result.errors.empty() &&
                        result.errors[0].message.find("out of memory") != std::string::npos;
    if (!failed) {
        std::fprintf(stderr, "status %d: %s\n", static_cast<int>(result.status),
                     result.errors.empty() ? "no error" : result.errors[0].message.c_str());
    }
    return failed ? 0 : 1;
}
