// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEPREVIEW_H
#define PARTICLES_PARTICLEPREVIEW_H

// Scrubbing an emitter to a time (docs/specs/S20-particles.md R28, R29): seek restarts and runs
// ticks to the time in chunks of at most budgetMs per call; checkpoints (full copies of the
// state) every second of simulated time, at most 32 MB per emitter (the oldest dropped), let a
// backward or repeated scrub start from the nearest one instead of from 0. The editor's Scene
// view overlay, the Inspector preview, the animation window and control_particles seek use it.

#include "Export.h"
#include "particles/EmitterRuntime.h"

#include <cstddef>
#include <deque>

namespace jasperengine { namespace particles {

    class JASPERENGINE_API ParticlePreview {
    public:
        static constexpr double kCheckpointSeconds = 1.0;
        static constexpr size_t kCheckpointBytes = 32u * 1024u * 1024u;

        explicit ParticlePreview(size_t maxBytes = kCheckpointBytes): maxBytes(maxBytes) {}

        // forgets the checkpoints (the effect or its structure changed)
        void invalidate();
        // starts moving `runtime` to `time` (seconds since play)
        void seek(EmitterRuntime& runtime, const EmitterInput& input, double time);
        // runs the seek for at most budgetMs; true when the runtime is at the time
        bool update(EmitterRuntime& runtime, const EmitterInput& input, double budgetMs);
        bool pending() const { return state.pending; }
        double target() const { return targetTime; }

        // seconds simulated by the last seek so far (a seek from a checkpoint simulates less)
        double simulatedSeconds() const { return simulated; }
        size_t checkpointCount() const { return checkpoints.size(); }
        size_t checkpointBytes() const { return bytes; }

    private:
        struct Stored {
            uint64_t tick = 0;
            EmitterRuntime::Checkpoint state;
            size_t bytes = 0;
        };
        void keepCheckpoint(EmitterRuntime& runtime);

        std::deque<Stored> checkpoints;
        size_t maxBytes;
        size_t bytes = 0;
        SeekState state;
        double targetTime = 0.0;
        double simulated = 0.0;
        uint64_t startTick = 0;
    };

}}

#endif // PARTICLES_PARTICLEPREVIEW_H
