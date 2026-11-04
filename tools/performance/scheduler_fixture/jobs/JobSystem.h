// Serial adapter only. Integration tests exercise the real worker/task implementation.
#pragma once

#include <cstddef>
#include <utility>

namespace jasperengine::jobs {

enum class Lane { High };
enum class GateClass { Default };
struct SubmitOptions { Lane lane; GateClass gate; const char* label; };
struct ParallelForOptions {
    static constexpr int kAllWorkers = -1;
    size_t grain;
    int workers;
    Lane lane;
    const char* label;
};

struct SerialSystem { int getWorkerCount() const { return 0; } };
inline bool hasSystem() { return false; }
inline SerialSystem system() { return {}; }

template<class T> class Task {
public:
    void wait() const {}
    void get() const {}
};

template<class F> Task<void> submit(F&& fn, SubmitOptions) {
    fn();
    return {};
}

template<class F> void parallelFor(size_t count, F&& fn, ParallelForOptions) {
    for (size_t i = 0; i < count; i++) fn(i);
}

}
