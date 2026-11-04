// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "window/ConsoleWindow.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <stdexcept>

using jasperengine::editor::LogData;

// Both variants use the production LogData/deque and the unmodified selectedLog body.
// The window's renderer, logger and filtering are outside this lookup measurement.
PRODUCTION

namespace {

    void require(bool condition) {
        if (!condition) throw std::runtime_error("Console selection regression");
    }

    void check(before::Selection& a, after::Selection& b, uint64_t id) {
        a.selectedLogId = b.selectedLogId = id;
        const LogData* x = a.selectedLog();
        const LogData* y = b.selectedLog();
        require((x == nullptr) == (y == nullptr));
        if (x) {
            require(x->id == y->id && x->message == y->message);
        }
    }

    void correctness() {
        before::Selection a;
        after::Selection b;
        check(a, b, 0);
        for (int count : {0, 1, 2, 17, 10000}) {
            a.logs.clear();
            b.logs.clear();
            for (int i = 0; i < count; i++) {
                LogData log;
                log.id = 100 + 3 * static_cast<uint64_t>(i);
                log.message = std::to_string(i);
                a.logs.push_back(log);
                b.logs.push_back(log);
            }
            for (bool selected : {false, true}) {
                a.hasSelection = b.hasSelection = selected;
                for (uint64_t id : {uint64_t(0), uint64_t(1), uint64_t(99), uint64_t(100),
                                    uint64_t(101), uint64_t(103), uint64_t(30100),
                                    std::numeric_limits<uint64_t>::max()}) check(a, b, id);
                for (int i = 0; i < count; i++) {
                    check(a, b, 100 + 3 * static_cast<uint64_t>(i));
                    check(a, b, 101 + 3 * static_cast<uint64_t>(i));
                }
            }
            for (int i = 0; i < count / 2; i++) {
                a.logs.pop_front();
                b.logs.pop_front();
            }
            check(a, b, 100);
            if (!a.logs.empty()) check(a, b, a.logs.front().id);
        }
        a.logs.clear();
        b.logs.clear();
        for (uint64_t id : {std::numeric_limits<uint64_t>::max() - 1,
                            std::numeric_limits<uint64_t>::max()}) {
            LogData log;
            log.id = id;
            a.logs.push_back(log);
            b.logs.push_back(log);
            check(a, b, id);
        }
        std::puts("Correctness: empty/single/full deque, selection off, all ids and gaps, eviction, uint64 bounds passed");
    }

    volatile uint64_t selectionSink = 0;

    template<class Selection>
    double measure(Selection& selection, uint64_t id) {
        selection.selectedLogId = id;
        constexpr int batch = 1024;
        int iterations = 0;
        const auto start = std::clock();
        const LogData* (Selection::*volatile lookup)() const = &Selection::selectedLog;
        double elapsed;
        do {
            for (int i = 0; i < batch; i++) {
                const LogData* log = (selection.*lookup)();
                selectionSink = log ? log->id : 0;
            }
            iterations += batch;
            elapsed = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
        } while (elapsed < 0.03);
        return elapsed * 1.0e9 / iterations;
    }

}

int main(int argc, char**) {
    correctness();
    if (argc > 1) return 0;
    std::puts("count,case,pair,before_cpu_ns,after_cpu_ns,speedup");
    for (int count : {100, 1000, 10000}) {
        before::Selection a;
        after::Selection b;
        for (int i = 0; i < count; i++) {
            LogData log;
            log.id = 100 + 3 * static_cast<uint64_t>(i);
            a.logs.push_back(log);
            b.logs.push_back(log);
        }
        a.hasSelection = b.hasSelection = true;
        const std::pair<const char*, uint64_t> cases[] = {
            {"oldest", a.logs.front().id}, {"middle", a.logs[count / 2].id},
            {"newest", a.logs.back().id}, {"evicted", a.logs.front().id - 1},
            {"gap", a.logs[count / 2].id + 1}, {"future", a.logs.back().id + 1}
        };
        for (const auto& [name, id] : cases) {
            for (int pair = 0; pair < 9; pair++) {
                double x, y;
                if (pair % 2 == 0) {
                    x = measure(a, id);
                    y = measure(b, id);
                } else {
                    y = measure(b, id);
                    x = measure(a, id);
                }
                std::printf("%d,%s,%d,%.3f,%.3f,%.4f\n", count, name, pair, x, y, x / y);
            }
        }
    }
}
