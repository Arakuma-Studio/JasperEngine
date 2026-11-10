// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The heartbeat arming, the shutdown window, the re-report interval, the "recovered after" line
// and the job-occupancy section are adapted from OpenEngine's MainThreadHangWatchdog
// (Apps/Editor/Source/Diagnostics/MainThreadHangWatchdog.cpp), Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License. See NOTICE.md. Busy scopes, context slots, the
// Lua hooks and the report files are our own.

#include "HangWatchdog.h"

#include "CrashHandler.h"
#include "jobs/JobStats.h"
#include "jobs/detail/TimedWait.h"
#include "thread/ThreadUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::diag;

namespace je_hang {}
using namespace je_hang;

namespace je_hang {

    constexpr size_t kMaxScopes = 16;
    constexpr size_t kLabelBytes = 96;
    constexpr size_t kContextBytes = 128;
    constexpr size_t kOccupancySlots = 64;
    constexpr uint64_t kUnlimited = ~0ull;
    constexpr const char* kShutdownPhase = "shutdown";

    struct ScopeSlot {
        char label[kLabelBytes] = {0};
        uint32_t budgetMs = 0;
        uint64_t startNs = 0;
    };

    struct ContextValue {
        std::atomic<uint32_t> seq{0};
        char value[kContextBytes] = {0};
    };

    struct Shared {
        // the main thread's side: one writer each
        std::atomic<uint64_t> beats{0};
        std::atomic<const char*> phase{nullptr};
        std::atomic<uint32_t> scopeDepth{0};
        ScopeSlot scopes[kMaxScopes];
        std::atomic<uint32_t> scopeSeq{0};
        std::atomic<uint64_t> frame{0};
        std::atomic<uint32_t> lastFrameUs{0};
        ContextValue context[static_cast<size_t>(ContextSlot::Count)];

        // set by start/stop and the settings
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> shutdownWindow{false};
        std::atomic<bool> readJobs{true};
        std::atomic<uint32_t> thresholdMs{3000};
        std::atomic<uint32_t> shutdownMs{15000};
        std::atomic<uint32_t> breakLuaMs{0};
        std::thread::id watched;

        // the watchdog thread's view, published for health()
        std::atomic<bool> armed{false};
        std::atomic<bool> inStall{false};
        std::atomic<uint64_t> lastBeatNs{0};
        // the beat count the watchdog last saw: health() treats a later beat as alive
        std::atomic<uint64_t> polledBeats{0};
        std::atomic<uint32_t> reports{0};

        // the watchdog thread's own
        std::mutex mutex;                  // config, hooks, notices; never taken by heartbeat
        std::condition_variable wake;
        WatchdogConfig config;
        watchdog::LuaHooks luaHooks;
        std::deque<HangNotice> notices;
        std::thread thread;
        uint64_t seenBeats = 0;
        uint64_t stallStartNs = 0;
        uint64_t lastReportNs = 0;
        uint32_t stallIndex = 0;
        std::string stallLabel;
        bool luaLocated = false;
        bool luaInterrupted = false;
        jobs::ThreadOccupancy occupancy[kOccupancySlots];
    };

    Shared& shared() {
        // never destroyed: a late heartbeat at exit stays harmless
        static Shared* instance = new Shared();
        return *instance;
    }

    uint64_t steadyNs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t nowNs(const Shared& s) {
        return s.config.clock ? s.config.clock() : steadyNs();
    }

    bool onWatchedThread(const Shared& s) {
        return s.running.load(std::memory_order_acquire) && std::this_thread::get_id() == s.watched;
    }

    bool isLuaPhase(const char* phase) {
        return phase && std::strncmp(phase, "lua", 3) == 0;
    }

    uint32_t envMs(const char* name, uint32_t fallback) {
        const char* value = std::getenv(name);
        if (!value || !*value) return fallback;
        const long parsed = std::strtol(value, nullptr, 10);
        return parsed > 0 ? static_cast<uint32_t>(parsed) : fallback;
    }

    std::string readContext(const Shared& s, ContextSlot slot) {
        const ContextValue& c = s.context[static_cast<size_t>(slot)];
        for (int attempt = 0; attempt < 4; attempt++) {
            const uint32_t before = c.seq.load(std::memory_order_acquire);
            if (before & 1u) continue;
            char copy[kContextBytes];
            std::memcpy(copy, c.value, sizeof(copy));
            if (c.seq.load(std::memory_order_acquire) == before) {
                copy[kContextBytes - 1] = '\0';
                return copy;
            }
        }
        return {};
    }

    struct ScopeView {
        std::string label;
        uint32_t budgetMs;
        uint64_t startNs;
    };

    std::vector<ScopeView> readScopes(const Shared& s) {
        std::vector<ScopeView> out;
        for (int attempt = 0; attempt < 4; attempt++) {
            out.clear();
            const uint32_t before = s.scopeSeq.load(std::memory_order_acquire);
            const uint32_t depth = std::min<uint32_t>(s.scopeDepth.load(std::memory_order_acquire), kMaxScopes);
            for (uint32_t i = 0; i < depth; i++) {
                char label[kLabelBytes];
                std::memcpy(label, s.scopes[i].label, sizeof(label));
                label[kLabelBytes - 1] = '\0';
                out.push_back({label, s.scopes[i].budgetMs, s.scopes[i].startNs});
            }
            if (s.scopeSeq.load(std::memory_order_acquire) == before) break;
        }
        return out;
    }

    // max(threshold, the active scopes' budgets); an unlimited scope never reports
    uint64_t effectiveThresholdMs(const Shared& s, const std::vector<ScopeView>& scopes) {
        uint64_t threshold = s.shutdownWindow.load(std::memory_order_relaxed)
            ? s.shutdownMs.load(std::memory_order_relaxed) : s.thresholdMs.load(std::memory_order_relaxed);
        for (const ScopeView& scope : scopes) {
            if (scope.budgetMs == 0) return kUnlimited;
            threshold = std::max<uint64_t>(threshold, scope.budgetMs);
        }
        return threshold;
    }

    std::string scopeList(const std::vector<ScopeView>& scopes) {
        if (scopes.empty()) return "none";
        std::string out;
        for (const ScopeView& scope : scopes) {
            if (!out.empty()) out += " > ";
            out += scope.label;
        }
        return out;
    }

    std::string seconds(uint64_t ns) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f s", static_cast<double>(ns) / 1e9);
        return text;
    }

    std::string describeJobs(Shared& s, uint64_t now) {
        if (!s.readJobs.load(std::memory_order_relaxed)) return "job system: not read (shutting down)\n";
        const size_t count = jobs::snapshotOccupancy(s.occupancy, kOccupancySlots);
        if (count == 0) return "job system: unavailable\n";
        std::string out = "job system:\n";
        for (size_t i = 0; i < count; i++) {
            const jobs::ThreadOccupancy& thread = s.occupancy[i];
            const char* kind = "other";
            switch (thread.kind) {
                case jobs::ThreadKind::Main: kind = "main"; break;
                case jobs::ThreadKind::Worker: kind = "worker"; break;
                case jobs::ThreadKind::Blocking: kind = "blocking"; break;
                case jobs::ThreadKind::IO: kind = "io"; break;
                case jobs::ThreadKind::Other: kind = "other"; break;
            }
            out += "  ";
            out += (thread.name && *thread.name) ? thread.name : "?";
            out += " (";
            out += kind;
            out += "): ";
            if (!thread.runningJob) {
                out += "idle\n";
                continue;
            }
            out += thread.runningJob;
            if (thread.sinceNs != 0) {
                // the job system stamps with the steady clock; a test clock has no relation to it
                const uint64_t steadyNow = s.config.clock ? steadyNs() : now;
                out += " for " + seconds(steadyNow > thread.sinceNs ? steadyNow - thread.sinceNs : 0);
            }
            out += "\n";
        }
        return out;
    }

    // Asks the Lua hook where the script is, waiting a little for the main thread to answer
    void locateLua(Shared& s, std::unique_lock<std::mutex>& lock) {
        if (s.luaLocated || !s.luaHooks.locate) return;
        s.luaLocated = true;
        std::function<void()> locate = s.luaHooks.locate;
        lock.unlock();
        locate();
        for (int i = 0; i < 10 && readContext(s, ContextSlot::Lua).empty(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        lock.lock();
    }

    void report(Shared& s, std::unique_lock<std::mutex>& lock, uint64_t now, uint64_t stalledNs, uint64_t thresholdMs,
                const std::vector<ScopeView>& scopes) {
        const char* phase = s.phase.load(std::memory_order_relaxed);
        if (isLuaPhase(phase)) locateLua(s, lock);
        const uint32_t index = s.reports.fetch_add(1, std::memory_order_relaxed) + 1;
        if (s.stallIndex == 0) s.stallIndex = index;

        HangReport out;
        out.index = index;
        out.stalledMs = stalledNs / 1000000;
        out.thresholdMs = thresholdMs;
        out.phase = phase ? phase : "unknown";
        out.shutdown = s.shutdownWindow.load(std::memory_order_relaxed);
        for (const ScopeView& scope : scopes) out.busyScopes.push_back(scope.label);

        std::string text = "JasperEngine hang report v1\n";
        text += std::string("product: ") + (s.config.product ? s.config.product : "JasperEngine") + "\n";
        text += "version: " + (s.config.version.empty() ? std::string("unknown") : s.config.version) + "\n";
        text += "build_id: " + (crash::buildId().empty() ? std::string("none") : crash::buildId()) + "\n";
        text += std::string("platform: ") + crash::platformName() + "\n";
        text += "session: " + crash::sessionId() + "\n";
        text += "hang: " + std::to_string(index) + "\n";
        text += "stalled_ms: " + std::to_string(out.stalledMs) + "\n";
        text += "threshold_ms: " + (thresholdMs == kUnlimited ? std::string("unlimited") : std::to_string(thresholdMs)) + "\n";
        text += "phase: " + out.phase + "\n";
        text += "busy_scopes: " + scopeList(scopes) + "\n";
        for (const ScopeView& scope : scopes) {
            text += "busy_scope: " + scope.label + " (budget " +
                (scope.budgetMs == 0 ? std::string("unlimited") : std::to_string(scope.budgetMs) + " ms") + ", for " +
                seconds(now > scope.startNs ? now - scope.startNs : 0) + ")\n";
        }
        text += std::string("shutdown: ") + (out.shutdown ? "true" : "false") + "\n";
        text += "frame: " + std::to_string(s.frame.load(std::memory_order_relaxed)) + "\n";
        char frameMs[32];
        std::snprintf(frameMs, sizeof(frameMs), "%.2f", s.lastFrameUs.load(std::memory_order_relaxed) / 1000.0);
        text += std::string("last_frame_ms: ") + frameMs + "\n";
        const std::string play = readContext(s, ContextSlot::PlayState);
        const std::string scene = readContext(s, ContextSlot::Scene);
        const std::string lua = readContext(s, ContextSlot::Lua);
        if (!play.empty()) text += "play_state: " + play + "\n";
        if (!scene.empty()) text += "scene: " + scene + "\n";
        if (isLuaPhase(phase) && !lua.empty()) text += "lua: " + lua + "\n";

        char stack[8192];
        const char* why = nullptr;
        const size_t stackBytes = watchdog::detail::captureMainThreadStack(stack, sizeof(stack), &why);
        if (stackBytes > 0) {
            text += "stack:\n";
            text.append(stack, stackBytes);
            if (text.back() != '\n') text += '\n';
        } else {
            text += std::string("stack: ") + (why ? why : "unavailable") + "\n";
        }
        text += describeJobs(s, now);
        out.text = text;

        char name[32];
        std::snprintf(name, sizeof(name), "hang-%03u.txt", index);
        out.file = crash::writeSessionFile(name, text);

        std::fprintf(stderr, "JASPERENGINE HANG: main thread not responding for %llu ms (phase: %s, busy: %s)%s%s\n",
                     static_cast<unsigned long long>(out.stalledMs), out.phase.c_str(), scopeList(scopes).c_str(),
                     out.file.empty() ? "" : " report=", out.file.empty() ? "" : out.file.string().c_str());
        std::fflush(stderr);
        s.stallLabel = scopes.empty() ? out.phase : scopes.back().label;

        if (s.config.onReport) {
            std::function<void(const HangReport&)> callback = s.config.onReport;
            lock.unlock();
            callback(out);
            lock.lock();
        }
    }

    void recovered(Shared& s, std::unique_lock<std::mutex>& lock, uint64_t now) {
        const uint64_t stalledNs = now > s.stallStartNs ? now - s.stallStartNs : 0;
        HangReport out;
        out.index = s.stallIndex;
        out.recovered = true;
        out.stalledMs = stalledNs / 1000000;
        out.phase = s.stallLabel;
        std::fprintf(stderr, "JASPERENGINE HANG: main thread recovered after %s (%s)\n", seconds(stalledNs).c_str(),
                     s.stallLabel.c_str());
        std::fflush(stderr);
        char name[32];
        std::snprintf(name, sizeof(name), "hang-%03u.txt", s.stallIndex);
        const std::filesystem::path folder = crash::sessionFolder();
        if (!folder.empty()) out.file = folder / name;
        s.notices.push_back({out.stalledMs, s.stallLabel, out.file});
        while (s.notices.size() > 16) s.notices.pop_front();
        if (s.config.onReport) {
            std::function<void(const HangReport&)> callback = s.config.onReport;
            lock.unlock();
            callback(out);
            lock.lock();
        }
    }

    // One tick: what the thread does every pollMs (the lock is held but around callbacks)
    void tick(Shared& s, std::unique_lock<std::mutex>& lock) {
        const uint64_t now = nowNs(s);
        const uint64_t beats = s.beats.load(std::memory_order_relaxed);
        s.polledBeats.store(beats, std::memory_order_relaxed);
        if (s.paused.load(std::memory_order_relaxed)) {
            s.seenBeats = beats;
            s.armed.store(false, std::memory_order_relaxed);
            s.inStall.store(false, std::memory_order_relaxed);
            return;
        }
        if (beats != s.seenBeats) {
            s.seenBeats = beats;
            s.lastBeatNs.store(now, std::memory_order_relaxed);
            s.armed.store(true, std::memory_order_relaxed);
            if (s.inStall.load(std::memory_order_relaxed)) {
                s.inStall.store(false, std::memory_order_relaxed);
                if (s.stallIndex != 0) recovered(s, lock, now);
            }
            s.stallIndex = 0;
            s.luaLocated = false;
            s.luaInterrupted = false;
            return;
        }
        if (!s.armed.load(std::memory_order_relaxed)) return;

        const uint64_t lastBeat = s.lastBeatNs.load(std::memory_order_relaxed);
        const uint64_t stalledNs = now > lastBeat ? now - lastBeat : 0;
        const std::vector<ScopeView> scopes = readScopes(s);
        const uint64_t threshold = effectiveThresholdMs(s, scopes);

        // runaway Lua in editor Play: interrupt once the stall passes the limit (busy or not)
        const char* phase = s.phase.load(std::memory_order_relaxed);
        const uint32_t breakMs = s.breakLuaMs.load(std::memory_order_relaxed);
        if (breakMs > 0 && isLuaPhase(phase) && !s.luaInterrupted && stalledNs / 1000000 >= breakMs && s.luaHooks.interrupt) {
            s.luaInterrupted = true;
            std::function<void(uint64_t)> interrupt = s.luaHooks.interrupt;
            lock.unlock();
            interrupt(stalledNs / 1000000);
            lock.lock();
        }

        if (threshold == kUnlimited || stalledNs / 1000000 <= threshold) return;
        if (!s.inStall.load(std::memory_order_relaxed)) {
            s.inStall.store(true, std::memory_order_relaxed);
            s.stallStartNs = lastBeat;
            s.lastReportNs = 0;
        }
        if (s.lastReportNs != 0 && now - s.lastReportNs < static_cast<uint64_t>(s.config.reReportMs) * 1000000) return;
        s.lastReportNs = now;
        report(s, lock, now, stalledNs, threshold, scopes);
    }

    void loop() {
        Shared& s = shared();
        std::unique_lock<std::mutex> lock(s.mutex);
        while (s.running.load(std::memory_order_relaxed)) {
            jobs::detail::timedWait(s.wake, lock, std::chrono::milliseconds(s.config.pollMs));
            if (!s.running.load(std::memory_order_relaxed)) break;
            tick(s, lock);
        }
    }

}

void diag::heartbeat() {
    Shared& s = shared();
    s.beats.store(s.beats.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

uint64_t diag::heartbeats() {
    return shared().beats.load(std::memory_order_relaxed);
}

void diag::watchdog::start(const WatchdogConfig& input) {
    Shared& s = shared();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.running.load(std::memory_order_relaxed)) return;
    WatchdogConfig config = input;
    if (const char* value = std::getenv("JASPERENGINE_HANG_WATCHDOG"); value && *value) {
        config.enabled = std::strcmp(value, "0") != 0;
    }
    if (!config.enabled) return;
    config.thresholdMs = envMs("JASPERENGINE_HANG_WATCHDOG_MS", config.thresholdMs);
    config.shutdownMs = envMs("JASPERENGINE_HANG_WATCHDOG_SHUTDOWN_MS", config.shutdownMs);
    s.config = config;
    s.thresholdMs.store(config.thresholdMs, std::memory_order_relaxed);
    s.shutdownMs.store(config.shutdownMs, std::memory_order_relaxed);
    s.shutdownWindow.store(false, std::memory_order_relaxed);
    s.readJobs.store(true, std::memory_order_relaxed);
    s.paused.store(false, std::memory_order_relaxed);
    s.armed.store(false, std::memory_order_relaxed);
    s.inStall.store(false, std::memory_order_relaxed);
    s.reports.store(0, std::memory_order_relaxed);
    s.phase.store(nullptr, std::memory_order_relaxed);
    s.scopeDepth.store(0, std::memory_order_relaxed);
    s.seenBeats = s.beats.load(std::memory_order_relaxed);
    s.polledBeats.store(s.seenBeats, std::memory_order_relaxed);
    s.stallIndex = 0;
    s.notices.clear();
    watchdog::detail::initializeMainThreadCapture();
    s.watched = std::this_thread::get_id();
    s.running.store(true, std::memory_order_release);
    if (config.pollMs > 0) {
        s.thread = thread::spawnNamed("Hang Watchdog", loop, thread::Priority::BelowNormal);
    }
}

void diag::watchdog::beginShutdownWatch() {
    Shared& s = shared();
    if (!s.running.load(std::memory_order_relaxed)) return;
    s.readJobs.store(false, std::memory_order_relaxed);
    heartbeat();
    s.phase.store(kShutdownPhase, std::memory_order_relaxed);
    s.shutdownWindow.store(true, std::memory_order_relaxed);
}

void diag::watchdog::stop() {
    Shared& s = shared();
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running.exchange(false, std::memory_order_acq_rel)) return;
        worker = std::move(s.thread);
        s.wake.notify_all();
    }
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.luaHooks = {};
    s.config.onReport = nullptr;
    s.config.clock = nullptr;
}

bool diag::watchdog::isRunning() {
    return shared().running.load(std::memory_order_acquire);
}

HealthSnapshot diag::watchdog::health() {
    Shared& s = shared();
    HealthSnapshot out;
    out.enabled = s.running.load(std::memory_order_acquire);
    out.shutdownWindow = s.shutdownWindow.load(std::memory_order_relaxed);
    out.thresholdMs = out.shutdownWindow ? s.shutdownMs.load(std::memory_order_relaxed) : s.thresholdMs.load(std::memory_order_relaxed);
    out.reports = s.reports.load(std::memory_order_relaxed);
    const char* phase = s.phase.load(std::memory_order_relaxed);
    out.phase = phase ? phase : "";
    const std::vector<ScopeView> scopes = readScopes(s);
    for (const ScopeView& scope : scopes) out.busyScopes.push_back(scope.label);
    if (!out.enabled || !s.armed.load(std::memory_order_relaxed) || s.paused.load(std::memory_order_relaxed)) return out;
    // the main thread beat since the last poll: alive, whatever the stall the watchdog last saw
    if (s.beats.load(std::memory_order_relaxed) != s.polledBeats.load(std::memory_order_relaxed)) return out;
    // never the config's clock: health is asked from any thread, the clock is the watchdog's
    const uint64_t now = s.config.clock ? 0 : steadyNs();
    const uint64_t lastBeat = s.lastBeatNs.load(std::memory_order_relaxed);
    out.stalledMs = now > lastBeat ? (now - lastBeat) / 1000000 : 0;
    const uint64_t threshold = effectiveThresholdMs(s, scopes);
    out.responsive = threshold == kUnlimited || out.stalledMs <= threshold;
    return out;
}

void diag::watchdog::setThresholdMs(uint32_t thresholdMs) {
    if (thresholdMs > 0) shared().thresholdMs.store(thresholdMs, std::memory_order_relaxed);
}

void diag::watchdog::setPaused(bool paused) {
    shared().paused.store(paused, std::memory_order_relaxed);
}

void diag::watchdog::setFrameInfo(uint64_t frame, float lastFrameMs) {
    Shared& s = shared();
    s.frame.store(frame, std::memory_order_relaxed);
    s.lastFrameUs.store(lastFrameMs > 0.0f ? static_cast<uint32_t>(lastFrameMs * 1000.0f) : 0u, std::memory_order_relaxed);
}

void diag::watchdog::setContext(ContextSlot slot, std::string_view value) {
    if (slot >= ContextSlot::Count) return;
    ContextValue& c = shared().context[static_cast<size_t>(slot)];
    const uint32_t seq = c.seq.load(std::memory_order_relaxed);
    c.seq.store(seq + 1, std::memory_order_release);
    const size_t n = std::min(value.size(), kContextBytes - 1);
    std::memcpy(c.value, value.data(), n);
    c.value[n] = '\0';
    c.seq.store(seq + 2, std::memory_order_release);
}

std::vector<HangNotice> diag::watchdog::takeNotices() {
    Shared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<HangNotice> out(s.notices.begin(), s.notices.end());
    s.notices.clear();
    return out;
}

void diag::watchdog::setLuaHooks(LuaHooks hooks) {
    Shared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.luaHooks = std::move(hooks);
}

void diag::watchdog::setBreakLuaAfterMs(uint32_t milliseconds) {
    shared().breakLuaMs.store(milliseconds, std::memory_order_relaxed);
}

uint32_t diag::watchdog::breakLuaAfterMs() {
    return shared().breakLuaMs.load(std::memory_order_relaxed);
}

void diag::watchdog::poll() {
    Shared& s = shared();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.running.load(std::memory_order_relaxed)) tick(s, lock);
}

diag::BusyScope::BusyScope(std::string_view label, uint32_t budgetMs) {
    Shared& s = shared();
    if (!onWatchedThread(s)) return;
    entered = true;
    const uint32_t depth = s.scopeDepth.load(std::memory_order_relaxed);
    if (depth < kMaxScopes) {
        s.scopeSeq.fetch_add(1, std::memory_order_acq_rel);
        ScopeSlot& slot = s.scopes[depth];
        const size_t n = std::min(label.size(), kLabelBytes - 1);
        std::memcpy(slot.label, label.data(), n);
        slot.label[n] = '\0';
        slot.budgetMs = budgetMs;
        slot.startNs = s.config.clock ? s.config.clock() : steadyNs();
        s.scopeSeq.fetch_add(1, std::memory_order_acq_rel);
    }
    s.scopeDepth.store(depth + 1, std::memory_order_release);
}

diag::BusyScope::~BusyScope() {
    if (!entered) return;
    Shared& s = shared();
    const uint32_t depth = s.scopeDepth.load(std::memory_order_relaxed);
    if (depth > 0) s.scopeDepth.store(depth - 1, std::memory_order_release);
    // the stall a long scope covered starts again from here
    heartbeat();
}

diag::PhaseScope::PhaseScope(const char* staticLabel) {
    Shared& s = shared();
    if (!onWatchedThread(s)) return;
    entered = true;
    previous = s.phase.exchange(staticLabel, std::memory_order_relaxed);
}

diag::PhaseScope::~PhaseScope() {
    if (!entered) return;
    shared().phase.store(previous, std::memory_order_relaxed);
}
