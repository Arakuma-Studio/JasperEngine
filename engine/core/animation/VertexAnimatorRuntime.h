// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.3, 3.4, 3.7)
// - Purpose: what VertexAnimatorSystem keeps per Vertex Animator besides the packed track pairs
//   of the component (VertexAnimatorComponent::state / instanceStates): the animator's own clock
//   and epoch, per-state speeds, holds (speed 0, stop), queues, finish flags and the callbacks
//   waiting for dispatch; plus the pure helpers the system and the tests share (event crossing,
//   seeded random starts, a track's local time).
// - Thread-safety: main thread (ECS), the helpers are pure.
// - Units as VertexAnimation.h: seconds, `now` = the animator's clock minus its epoch.
// - Invariants: VertexAnimatorRuntime::instances stays parallel to
//   VertexAnimatorComponent::instanceStates (and so to InstancedMeshComponent::instances);
//   eraseInstanceState keeps both when an instance is removed from the middle.

#ifndef VERTEX_ANIMATOR_RUNTIME_H
#define VERTEX_ANIMATOR_RUNTIME_H

#include "Export.h"
#include "animation/AnimationEvent.h"
#include "animation/VertexAnimation.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace jasperengine{

    struct VertexAnimatorComponent;

}

namespace jasperengine::vat{

    // A track held at a local time (speed 0, or stopped): the GPU form can't stop a clock (its
    // local time is (now - start) * speed), so a held track is written with speed 1 and a start
    // re-pinned to now - tau at every update
    struct JASPERENGINE_API VatTrackHold{
        bool held = false;
        double tau = 0.0;
    };

    // One playing state (the entity's own, or one instance's), beside its VatTrackPair
    struct JASPERENGINE_API VatStateExtra{
        float speed = 1.0f;             // this state's own multiplier (setInstanceSpeed, SetSpeed)
        VatTrackHold holdA;
        VatTrackHold holdB;
        bool stopped = false;           // stop(): held where it was, not playing
        bool freshA = false;            // the track just started: events exactly at its start fire
        bool freshB = false;
        bool finishNotified = false;    // A (once / clamp_forever) reported its end
        bool dirty = true;              // the packed pair changed: the GPU copy is written again
        std::vector<std::string> queue; // instances; the entity's is VertexAnimatorComponent::queued
    };

    // a callback waiting for the end of the system update (user code runs last)
    struct JASPERENGINE_API VatPendingCallback{
        double when = 0.0;              // clock time it happened (ordering)
        int instance = -1;              // -1: the entity
        bool finish = false;            // else an event
        std::string clip;               // finish: the clip that ended
        AnimationEvent event;
    };

    struct JASPERENGINE_API VertexAnimatorRuntime{
        double clock = 0.0;             // seconds this animator played (play mode, or edit-mode preview)
        int64_t epoch = 0;              // VAT_EPOCH_SECONDS periods taken off the clock: now = clock - epoch * period
        std::string path;               // the animation the asset is for
        float retryTimer = 0.0f;        // a failed load tries again every second
        bool failed = false;
        std::string loggedError;        // the last error logged (not logged again)
        bool waitingModel = false;      // the Model is still loading
        float appliedSpeed = 1.0f;      // the component speed the tracks were written with
        bool previewing = false;        // edit mode: Preview was on at the last update
        uint64_t clipMask = 0;          // clips in use when the bounds were set (bit per clip)
        bool boundsSet = false;
        bool anyDirty = true;           // an instance's state changed since the last GPU write
        bool watchDirty = true;         // `watched` needs a rebuild
        std::vector<int> watched;       // instances needing per-frame work without instanceEvents
                                        // (a queue, or a hold to re-pin)
        VatStateExtra entity;
        std::vector<VatStateExtra> instances;
        std::vector<VatPendingCallback> pending;
        std::set<std::string> warned;   // warnings given once (unknown clip, socket, instance)
    };

    // the runtime of an animator, made on first use
    JASPERENGINE_API VertexAnimatorRuntime& animatorRuntime(VertexAnimatorComponent& animator);

    // An instance removed from the middle of InstancedMeshComponent::instances: its state goes
    // with it, so the following instances keep theirs (Mesh::removeInstance calls it)
    JASPERENGINE_API void eraseInstanceState(VertexAnimatorComponent& animator, size_t index);
    // every instance removed (Mesh::clearInstances)
    JASPERENGINE_API void clearInstanceStates(VertexAnimatorComponent& animator);

    // ---- pure helpers ----

    // a track's local time (seconds of its clip, unwrapped) at `now`
    JASPERENGINE_API double vatTrackTau(const VatTrack& track, const VatTrackHold& hold, double now);
    // one cycle of a clip's local time: loop = length, ping_pong = 2 * length, others = length
    JASPERENGINE_API double vatClipPeriod(const VatClip& clip);

    // A seeded number in [0, 1): the same (seed, index) always gives the same value. Random
    // starts use seed = the entity id and index = instance + 1 (0 for the entity itself)
    JASPERENGINE_API double vatRandomUnit(uint64_t seed, int64_t index);

    // Events of `clip` crossed when its local time went from `from` to `to` (unwrapped seconds,
    // as vatTrackTau gives them; backwards when to < from), with the clip's wrap as vatSample
    // plays it: loop repeats every length, ping_pong goes there and back (period 2 * length),
    // once / clamp_forever clamp to [0, length]. Forward fires from < t <= to, backward
    // to <= t < from; `includeStart` also fires an event exactly at `from` (a track that just
    // started). Each event fires at most once per call however many cycles the interval spans
    // (a low frame rate never floods). Appends the event's index and where it was crossed (on
    // the unwrapped line, between from and to), in crossing order.
    struct JASPERENGINE_API VatCrossedEvent{
        size_t index = 0;               // into clip.events
        double position = 0.0;
    };
    JASPERENGINE_API void vatCrossedEvents(const VatClip& clip, double from, double to, bool includeStart,
                                           std::vector<VatCrossedEvent>& out);

}

#endif //VERTEX_ANIMATOR_RUNTIME_H
