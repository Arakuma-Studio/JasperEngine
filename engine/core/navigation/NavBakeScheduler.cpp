// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Tile jobs that own their input snapshot follow OpenEngine's NavigationMeshBake.cpp
// (Engine/Modules/PathfindingECS/Source/NavigationMeshBake.cpp), Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License. See NOTICE.md.

#include "NavBakeScheduler.h"

#include "jobs/JobSystem.h"
#include "profiler/Profile.h"

#include <algorithm>
#include <chrono>

using namespace jasperengine;
using namespace jasperengine::nav;

struct NavBake::Shared {
    std::shared_ptr<const NavBakeInput> input;
    std::vector<uint32_t> tiles;
    // results[i] is tiles[i]'s
    std::vector<NavTileBuildResult> results;
    std::atomic<int> done{0};
    std::atomic<bool> cancelled{false};
    jobs::CancelToken token;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::atomic<double> elapsedMs{0.0};
};

NavBake::NavBake(std::shared_ptr<const NavBakeInput> bakeInput, std::vector<uint32_t> tiles, const NavBakeOptions& options)
    : input(std::move(bakeInput)), shared(std::make_shared<Shared>()) {
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    shared->input = input;
    shared->tiles = tiles;
    shared->results.resize(tiles.size());

    jobs::JobSystem& system = options.system ? *options.system : jobs::system();
    timeSliced = options.mode == NavBakeOptions::Mode::TimeSliced ||
                 (options.mode == NavBakeOptions::Mode::Auto && system.isSingleThreaded());
    if (timeSliced) {
        queue = std::move(tiles);
        return;
    }

    jobs::SubmitOptions submit;
    submit.lane = options.lane;
    submit.gate = options.lane == jobs::Lane::Background ? jobs::GateClass::Heavy : jobs::GateClass::Default;
    submit.name = "nav-bake-tile";
    submit.cancel = &shared->token;
    tasks.reserve(shared->tiles.size());
    for (size_t i = 0; i < shared->tiles.size(); i++) {
        std::shared_ptr<Shared> state = shared;
        tasks.push_back(system.submit([state, i]() {
            JE_PROFILE_SCOPE("Nav.Bake");
            NavMeshBuilder::TileJob job(state->input, state->tiles[i]);
            while (!job.step()) {
                if (jobs::cancelRequested())
                    return;
            }
            state->results[i] = std::move(job.result());
            if (state->done.fetch_add(1, std::memory_order_acq_rel) + 1 == (int)state->tiles.size()) {
                state->elapsedMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state->started).count(),
                                       std::memory_order_release);
            }
        }, submit));
    }
}

NavBake::~NavBake() {
    cancel();
}

void NavBake::pump(double budgetMs) {
    if (!timeSliced || shared->cancelled.load(std::memory_order_acquire))
        return;
    JE_PROFILE_SCOPE("Nav.Bake");
    const auto begin = std::chrono::steady_clock::now();
    do {
        if (!active) {
            if (next >= queue.size())
                break;
            active = std::make_unique<NavMeshBuilder::TileJob>(input, queue[next]);
        }
        const char* stage = active->stageName();
        const auto stepStart = std::chrono::steady_clock::now();
        const bool finished = active->step();
        const double stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
        if (stepMs > slowestStepMs) {
            slowestStepMs = stepMs;
            slowestStep = std::string(stage) + ", tile " + std::to_string(queue[next]);
        }
        if (finished) {
            // queue and tiles are the same sorted list: next is the result's slot
            shared->results[next] = std::move(active->result());
            active.reset();
            next++;
            shared->done.fetch_add(1, std::memory_order_acq_rel);
            if (next == queue.size())
                shared->elapsedMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shared->started).count(),
                                        std::memory_order_release);
        }
    } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() < budgetMs);
}

bool NavBake::isDone() const {
    if (timeSliced)
        return shared->cancelled.load(std::memory_order_acquire) || next >= queue.size();
    for (const jobs::TaskRef& task : tasks) {
        if (!task.isReady())
            return false;
    }
    return true;
}

void NavBake::wait() {
    for (const jobs::TaskRef& task : tasks)
        task.wait();
}

int NavBake::getTilesDone() const {
    return shared->done.load(std::memory_order_acquire);
}

int NavBake::getTilesTotal() const {
    return (int)shared->tiles.size();
}

float NavBake::getProgress() const {
    const int total = getTilesTotal();
    return total > 0 ? (float)getTilesDone() / (float)total : 1.0f;
}

void NavBake::cancel() {
    if (shared->cancelled.exchange(true, std::memory_order_acq_rel))
        return;
    shared->token.cancel();
    active.reset();
}

bool NavBake::isCancelled() const {
    return shared->cancelled.load(std::memory_order_acquire);
}

std::vector<NavTileBuildResult>& NavBake::getResults() {
    return shared->results;
}

double NavBake::getElapsedMs() const {
    if (isDone() && !isCancelled())
        return shared->elapsedMs.load(std::memory_order_acquire);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shared->started).count();
}
