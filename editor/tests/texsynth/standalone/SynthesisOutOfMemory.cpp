// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A process of its own (jasperengine-texsynth-oom, built next to the editor tests and run by
// TexSynthOutOfMemory.IsAFailedStatusNotATerminate): a synthesis whose allocations fail under an
// address-space limit ends with Status::Failed "out of memory", never std::terminate. The test
// binary can't run it itself: S02's TestMain runs the engine there (many threads, each with its
// malloc arena), so the free memory the limit leaves can't be told from the process size.
// It links the engine, whose global operator new is ThorVG's (tvgInitializer.cpp): that one
// throwing std::bad_alloc is part of what this checks (it returned null, a SIGSEGV here).
// Exit 0: the expected failed status; 1: anything else (printed).

#include "texsynth/TextureSynthesis.h"
#include "object/ui/Image.h"

#include <execinfo.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

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

    // the main thread's stack grown now: under the limit, a stack page past what is mapped can't
    // be added (a SIGSEGV wherever the synthesis first reaches it, not a bad_alloc)
    [[gnu::noinline]] void growStack() {
        volatile char pad[1 << 20];
        for (size_t i = 0; i < sizeof(pad); i += 4096) pad[i] = 0;
    }

    // a crash prints where (the test only sees the signal)
    void onCrash(int sig, siginfo_t* info, void*) {
        char line[64];
        const int n = std::snprintf(line, sizeof(line), "signal %d at %p\n", sig, info->si_addr);
        if (n > 0) (void)!write(STDERR_FILENO, line, static_cast<size_t>(n));
        void* frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        signal(sig, SIG_DFL);
        raise(sig);
    }

}

int main() {
    struct sigaction action = {};
    action.sa_sigaction = onCrash;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    Request request;
    request.mode = Mode::Guided;
    request.quality = Quality::Draft;
    request.examples.push_back(Example::fromImage(noise(2048, 2048, 4, 1)));
    request.targetGuide = ImageInput::fromImage(noise(64, 64, 3, 2));

    growStack();
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
