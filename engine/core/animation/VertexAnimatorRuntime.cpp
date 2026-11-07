// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimatorRuntime.h"

#include "math/HashRandom.h"

#include "component/VertexAnimatorComponent.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    // "exactly at the start" tolerance for includeStart (float starts written from double math)
    constexpr double START_EPSILON = 1e-6;

    // where `te` (one copy of an event, repeating every `period`) is first crossed going from
    // `from` towards `to`; false when it is not crossed in the interval
    bool firstCrossing(double te, double period, double from, double to, bool includeStart, double& at){
        if (period <= 0.0){
            if (includeStart && std::abs(te - from) <= START_EPSILON){
                at = from;
                return true;
            }
            return false;
        }
        if (to >= from){
            // the copy at or before `from` (c <= from < c + period, rounding corrected)
            double c = te + std::floor((from - te) / period) * period;
            if (c > from) c -= period;
            if (c + period <= from) c += period;
            if (includeStart && (std::abs(c - from) <= START_EPSILON || std::abs(c + period - from) <= START_EPSILON)){
                at = from;
                return true;
            }
            const double next = c + period;
            if (next <= to){
                at = next;
                return true;
            }
            return false;
        }
        // the copy at or after `from` (c - period < from <= c)
        double c = te + std::ceil((from - te) / period) * period;
        if (c < from) c += period;
        if (c - period >= from) c -= period;
        if (includeStart && (std::abs(c - from) <= START_EPSILON || std::abs(c - period - from) <= START_EPSILON)){
            at = from;
            return true;
        }
        const double next = c - period;
        if (next >= to){
            at = next;
            return true;
        }
        return false;
    }

}

VertexAnimatorRuntime& vat::animatorRuntime(VertexAnimatorComponent& animator){
    if (!animator.runtime){
        animator.runtime = std::make_shared<VertexAnimatorRuntime>();
    }
    return *animator.runtime;
}

void vat::eraseInstanceState(VertexAnimatorComponent& animator, size_t index){
    if (index < animator.instanceStates.size()){
        animator.instanceStates.erase(animator.instanceStates.begin() + (std::ptrdiff_t)index);
    }
    if (animator.runtime){
        VertexAnimatorRuntime& runtime = *animator.runtime;
        if (index < runtime.instances.size()){
            runtime.instances.erase(runtime.instances.begin() + (std::ptrdiff_t)index);
        }
        // callbacks of the removed one go; the later ones move down with their instances (those
        // waiting, and those not yet called of the list being dispatched)
        auto reindex = [index](std::vector<VatPendingCallback>& callbacks, size_t from){
            std::vector<VatPendingCallback> kept(std::make_move_iterator(callbacks.begin()),
                                                 std::make_move_iterator(callbacks.begin() + (std::ptrdiff_t)from));
            for (size_t i = from; i < callbacks.size(); i++){
                VatPendingCallback& callback = callbacks[i];
                if (callback.instance == (int)index) continue;
                if (callback.instance > (int)index) callback.instance--;
                kept.push_back(std::move(callback));
            }
            callbacks.swap(kept);
        };
        reindex(runtime.pending, 0);
        reindex(runtime.dispatching, std::min(runtime.dispatchNext, runtime.dispatching.size()));
        runtime.watchDirty = true;
        runtime.anyDirty = true;
    }
}

void vat::clearInstanceStates(VertexAnimatorComponent& animator){
    animator.instanceStates.clear();
    if (animator.runtime){
        VertexAnimatorRuntime& runtime = *animator.runtime;
        runtime.instances.clear();
        auto isInstance = [](const VatPendingCallback& callback){ return callback.instance >= 0; };
        runtime.pending.erase(std::remove_if(runtime.pending.begin(), runtime.pending.end(), isInstance),
                              runtime.pending.end());
        const size_t next = std::min(runtime.dispatchNext, runtime.dispatching.size());
        runtime.dispatching.erase(std::remove_if(runtime.dispatching.begin() + (std::ptrdiff_t)next, runtime.dispatching.end(), isInstance),
                                  runtime.dispatching.end());
        runtime.watched.clear();
        runtime.watchDirty = true;
        runtime.anyDirty = true;
    }
}

double vat::vatTrackTau(const VatTrack& track, const VatTrackHold& hold, double now){
    if (hold.held) return hold.tau;
    if (track.speed == 0.0f) return (double)track.start;
    return (now - (double)track.start) * (double)track.speed;
}

double vat::vatClipPeriod(const VatClip& clip){
    const double length = clip.length();
    return clip.wrap == ClipWrapMode::PingPong ? 2.0 * length : length;
}

double vat::vatRandomUnit(uint64_t seed, int64_t index){
    return hashUnit(seed, index);
}

void vat::vatCrossedEvents(const VatClip& clip, double from, double to, bool includeStart, std::vector<VatCrossedEvent>& out){
    if (clip.events.empty() || !std::isfinite(from) || !std::isfinite(to)) return;
    if (from == to && !includeStart) return;

    const double length = std::max(0.0, (double)clip.length());
    const bool forward = to >= from;
    const size_t first = out.size();

    for (size_t i = 0; i < clip.events.size(); i++){
        const double time = std::min(std::max((double)clip.events[i].time, 0.0), length);
        double at = 0.0;
        bool crossed = false;

        if (clip.wrap == ClipWrapMode::Loop){
            // an event at the length is the one at 0 of the next cycle
            const double te = length > 0.0 ? std::fmod(time, length) : 0.0;
            crossed = firstCrossing(te, length, from, to, includeStart, at);
        }else if (clip.wrap == ClipWrapMode::PingPong){
            // there (te) and back (2L - te): the first crossing of either copy
            const double period = 2.0 * length;
            double there = 0.0, back = 0.0;
            const bool hitThere = firstCrossing(time, period, from, to, includeStart, there);
            const bool hitBack = time > 0.0 && time < length &&
                                 firstCrossing(period - time, period, from, to, includeStart, back);
            if (hitThere && hitBack){
                at = forward ? std::min(there, back) : std::max(there, back);
                crossed = true;
            }else if (hitThere || hitBack){
                at = hitThere ? there : back;
                crossed = true;
            }
        }else{
            // once / clamp_forever: the time stays in [0, length]
            const double a = std::min(std::max(from, 0.0), length);
            const double b = std::min(std::max(to, 0.0), length);
            if (includeStart && std::abs(time - a) <= START_EPSILON){
                // the clamped start maps back onto the unwrapped line where the track was
                at = from;
                crossed = true;
            }else if (forward ? (time > a && time <= b) : (time < a && time >= b)){
                // inside [0, length] the local time is the clip time
                at = time;
                crossed = true;
            }
        }

        if (crossed){
            VatCrossedEvent event;
            event.index = i;
            event.position = at;
            out.push_back(event);
        }
    }

    std::stable_sort(out.begin() + (std::ptrdiff_t)first, out.end(), [forward](const VatCrossedEvent& a, const VatCrossedEvent& b){
        return forward ? a.position < b.position : a.position > b.position;
    });
}
