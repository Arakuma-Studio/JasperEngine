// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimatorSystem.h"

#include "Scene.h"
#include "Log.h"
#include "animation/AnimationEvent.h"
#include "animation/VertexAnimatorRuntime.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pool/VertexAnimationPool.h"
#include "script/LuaBinding.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    // how often a failed animator tries to load again
    constexpr float FAILED_RETRY_SECONDS = 1.0f;
    // transitions (fade ends, finishes, queued clips) handled in one update at most
    constexpr int MAX_TRANSITIONS = 16;

    std::string normalizePath(const std::string& path){
        std::string normal = std::filesystem::path(path).lexically_normal().generic_string();
        while (normal.compare(0, 2, "./") == 0) normal.erase(0, 2);
        return normal;
    }

    // the .vat.glb an asset plays on, as a Model filename (asset-relative, like `animation`)
    std::string expectedMesh(const std::string& animation, const VatData& data){
        const std::filesystem::path dir = std::filesystem::path(animation).parent_path();
        return normalizePath((dir / data.mesh).generic_string());
    }

    std::string fileName(const std::string& path){
        return std::filesystem::path(path).filename().generic_string();
    }

    // everything one animator's update needs
    struct Animating{
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        VertexAnimatorComponent* animator = nullptr;
        VertexAnimatorRuntime* runtime = nullptr;
        const VertexAnimation* asset = nullptr;
        const VatData* data = nullptr;
        float speed = 1.0f;             // the component's, finite
        bool fire = false;              // play mode: callbacks are queued
        bool instanced = false;         // has an Instanced Mesh: the instances are what is drawn

        const VatClip& clip(int index) const{ return data->clips[(size_t)index]; }
        bool validClip(int index) const{ return index >= 0 && index < (int)data->clips.size(); }
    };

    std::string label(const Animating& a){
        return a.scene->getEntityName(a.entity);
    }

    void warnOnce(const Animating& a, const std::string& key, const std::string& message){
        if (a.runtime->warned.insert(key).second){
            Log::warn("Vertex Animator '%s': %s", label(a).c_str(), message.c_str());
        }
    }

    int findClip(const Animating& a, const std::string& name){
        const int index = a.data->findClip(name);
        if (index < 0){
            std::string names;
            for (const VatClip& clip : a.data->clips) names += (names.empty() ? "" : ", ") + clip.name;
            warnOnce(a, "clip|" + name, "no clip '" + name + "' in " + a.animator->animation +
                     (names.empty() ? std::string(" (it has none)") : " (clips: " + names + ")"));
        }
        return index;
    }

    // the clip playAutomatically starts: defaultClip, else the first
    int defaultClip(const Animating& a){
        if (a.data->clips.empty()) return -1;
        if (a.animator->defaultClip.empty()) return 0;
        const int index = findClip(a, a.animator->defaultClip);
        return index >= 0 ? index : 0;
    }

    float effectiveSpeed(const Animating& a, const VatStateExtra& extra, int clip){
        if (!a.validClip(clip)) return 0.0f;
        const float speed = a.speed * extra.speed * a.clip(clip).speed;
        return std::isfinite(speed) ? speed : 0.0f;
    }

    // where a clip starts: its end when played backwards (not for loops, where both are the same)
    double startTau(const Animating& a, int clip, float speed){
        if (!a.validClip(clip)) return 0.0;
        const VatClip& c = a.clip(clip);
        return (speed < 0.0f && c.wrap != ClipWrapMode::Loop) ? (double)c.length() : 0.0;
    }

    void touch(const Animating& a, VatStateExtra& extra){
        extra.dirty = true;
        a.runtime->anyDirty = true;
    }

    // a track at local time `tau` from `now`, with `speed` (0: held there, packed as speed 0 with
    // the local time as its start: vat::vatLocalTime, nothing to rewrite while it holds)
    void setTrack(VatTrack& track, VatTrackHold& hold, int clip, double tau, float speed, double now){
        track.clip = clip;
        if (speed == 0.0f){
            hold.held = true;
            hold.tau = tau;
            track.speed = 0.0f;
            track.start = (float)tau;
        }else{
            hold.held = false;
            hold.tau = 0.0;
            track.speed = speed;
            track.start = (float)(now - tau / (double)speed);
        }
    }

    bool hasB(const VatTrackPair& pair){
        return pair.b.clip >= 0;
    }

    bool blendMode(const VatTrackPair& pair){
        return hasB(pair) && pair.blend >= 0.0f;
    }

    // in blend mode B's time follows A's phase: its track mirrors A's
    void mirrorBlendB(VatTrackPair& pair){
        if (blendMode(pair)){
            pair.b.start = pair.a.start;
            pair.b.speed = pair.a.speed;
        }
    }

    // a finished crossfade: B is the current track
    void settle(const Animating& a, VatTrackPair& pair, VatStateExtra& extra){
        pair.a = pair.b;
        extra.holdA = extra.holdB;
        extra.freshA = extra.freshB;
        pair.b = VatTrack();
        extra.holdB = VatTrackHold();
        extra.freshB = false;
        pair.fadeStart = 0.0f;
        pair.fadeEnd = 0.0f;
        pair.blend = -1.0f;
        extra.finishNotified = false;
        touch(a, extra);
    }

    bool dominantIsB(const VatTrackPair& pair, double now){
        return hasB(pair) && vatWeightB(pair, (float)now) >= 0.5f;
    }

    // the one track a crossfade, stop or seek continues from: the dominant one
    void collapse(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, double now){
        if (!hasB(pair)) return;
        if (dominantIsB(pair, now) && !blendMode(pair)){
            settle(a, pair, extra);
        }else if (dominantIsB(pair, now)){
            // blend: B at A's phase becomes a track of its own
            const VatClip& clipA = a.clip(pair.a.clip);
            const VatClip& clipB = a.clip(pair.b.clip);
            const VatFrameSample sample = vatSample(clipA, (float)vatTrackTau(pair.a, extra.holdA, now));
            const double tauB = (double)sample.normalized * (double)clipB.length();
            const int clip = pair.b.clip;
            pair.b = VatTrack();
            pair.blend = -1.0f;
            setTrack(pair.a, extra.holdA, clip, tauB, extra.stopped ? 0.0f : effectiveSpeed(a, extra, clip), now);
            extra.finishNotified = false;
        }else{
            pair.b = VatTrack();
            extra.holdB = VatTrackHold();
            extra.freshB = false;
            pair.fadeStart = 0.0f;
            pair.fadeEnd = 0.0f;
            pair.blend = -1.0f;
        }
        touch(a, extra);
    }

    // every track written again with the current speeds, keeping its local time
    void respeed(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, double now){
        if (pair.a.clip >= 0){
            const double tau = vatTrackTau(pair.a, extra.holdA, now);
            setTrack(pair.a, extra.holdA, pair.a.clip, tau, extra.stopped ? 0.0f : effectiveSpeed(a, extra, pair.a.clip), now);
        }
        if (hasB(pair) && !blendMode(pair)){
            const double tau = vatTrackTau(pair.b, extra.holdB, now);
            setTrack(pair.b, extra.holdB, pair.b.clip, tau, extra.stopped ? 0.0f : effectiveSpeed(a, extra, pair.b.clip), now);
        }
        mirrorBlendB(pair);
        touch(a, extra);
    }

    // plays `clip` at once from `tau` (NaN: its start)
    void playNow(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue,
                 int clip, double now, double tau = NAN){
        const float speed = effectiveSpeed(a, extra, clip);
        pair = VatTrackPair();
        extra.holdA = VatTrackHold();
        extra.holdB = VatTrackHold();
        extra.stopped = false;
        extra.finishNotified = false;
        extra.freshB = false;
        setTrack(pair.a, extra.holdA, clip, std::isnan(tau) ? startTau(a, clip, speed) : tau, speed, now);
        extra.freshA = true;
        queue.clear();
        touch(a, extra);
    }

    bool isFinished(const Animating& a, const VatTrackPair& pair, const VatStateExtra& extra, double now){
        if (!a.validClip(pair.a.clip) || hasB(pair)) return false;
        const VatClip& clip = a.clip(pair.a.clip);
        if (clip.wrap != ClipWrapMode::Once && clip.wrap != ClipWrapMode::ClampForever) return false;
        return extra.finishNotified || vatSample(clip, (float)vatTrackTau(pair.a, extra.holdA, now)).finished;
    }

    void crossFade(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue,
                   int clip, float fade, double now){
        if (!(fade > 0.0f) || pair.a.clip < 0 || extra.stopped){
            playNow(a, pair, extra, queue, clip, now);
            return;
        }
        collapse(a, pair, extra, now);
        queue.clear();
        // already playing it: nothing to fade to (Unity); an ended once clip fades into its start
        if (pair.a.clip == clip && !isFinished(a, pair, extra, now)) return;
        const float speed = effectiveSpeed(a, extra, clip);
        setTrack(pair.b, extra.holdB, clip, startTau(a, clip, speed), speed, now);
        extra.freshB = true;
        pair.fadeStart = (float)now;
        pair.fadeEnd = (float)(now + fade);
        pair.blend = -1.0f;
        touch(a, extra);
    }

    void blendClips(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue,
                    int clipA, int clipB, float weight, double now){
        weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
        const bool same = pair.a.clip == clipA && !extra.stopped &&
                          (!hasB(pair) || (blendMode(pair) && pair.b.clip == clipB));
        if (!same){
            // A keeps its phase when it is already playing
            if (pair.a.clip == clipA && !extra.stopped){
                collapse(a, pair, extra, now);
            }
            if (pair.a.clip != clipA || extra.stopped){
                playNow(a, pair, extra, queue, clipA, now);
            }
        }
        queue.clear();
        pair.b.clip = clipB;
        pair.blend = weight;
        pair.fadeStart = 0.0f;
        pair.fadeEnd = 0.0f;
        extra.holdB = VatTrackHold();
        extra.freshB = false;
        mirrorBlendB(pair);
        touch(a, extra);
    }

    void queueClip(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue,
                   int clip, double now){
        // nothing playing: plays now
        if (pair.a.clip < 0 || extra.stopped || isFinished(a, pair, extra, now)){
            playNow(a, pair, extra, queue, clip, now);
            return;
        }
        queue.push_back(a.clip(clip).name);
        a.runtime->watchDirty = true;
    }

    void stopState(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue, double now){
        collapse(a, pair, extra, now);
        queue.clear();
        if (pair.a.clip >= 0){
            setTrack(pair.a, extra.holdA, pair.a.clip, vatTrackTau(pair.a, extra.holdA, now), 0.0f, now);
        }
        extra.stopped = true;
        extra.freshA = false;
        touch(a, extra);
    }

    void setStateTime(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, double time, double now){
        // the current clip's time: the one faded to ends its fade (a blend sets A, B follows)
        if (hasB(pair) && !blendMode(pair)) settle(a, pair, extra);
        if (pair.a.clip < 0 || !std::isfinite(time)) return;
        const float speed = extra.stopped ? 0.0f : effectiveSpeed(a, extra, pair.a.clip);
        setTrack(pair.a, extra.holdA, pair.a.clip, time, speed, now);
        extra.finishNotified = false;
        touch(a, extra);
    }

    // a seeded start time in the current clip (randomStart, randomizeTimes); a blend keeps its
    // pair (A gets the time, B follows its phase), a crossfade continues from the dominant clip
    void randomizeState(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, uint64_t seed, int64_t index, double now){
        if (!blendMode(pair)) collapse(a, pair, extra, now);
        if (pair.a.clip < 0) return;
        const double tau = vatRandomUnit(seed, index) * vatClipPeriod(a.clip(pair.a.clip));
        const float speed = extra.stopped ? 0.0f : effectiveSpeed(a, extra, pair.a.clip);
        setTrack(pair.a, extra.holdA, pair.a.clip, tau, speed, now);
        mirrorBlendB(pair);
        extra.finishNotified = false;
        touch(a, extra);
    }

    // a state as playAutomatically starts it: the default clip, at a seeded time with randomStart
    void startState(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, std::vector<std::string>& queue,
                    int64_t index, double now){
        pair = VatTrackPair();
        extra = VatStateExtra();
        queue.clear();
        int clip = a.animator->playAutomatically ? defaultClip(a) : -1;
        // an instance's own start (Create Crowd): its clip and its speed
        if (index > 0){
            const size_t instance = (size_t)(index - 1);
            const std::vector<std::string>& clips = a.animator->instanceClips;
            if (clip >= 0 && instance < clips.size() && !clips[instance].empty()){
                const int own = findClip(a, clips[instance]);
                if (own >= 0) clip = own;
            }
            const std::vector<float>& speeds = a.animator->instanceSpeeds;
            if (instance < speeds.size() && std::isfinite(speeds[instance])){
                extra.speed = speeds[instance];
            }
        }
        if (clip >= 0){
            playNow(a, pair, extra, queue, clip, now);
            if (a.animator->randomStart){
                randomizeState(a, pair, extra, (uint64_t)a.entity, index, now);
                // a random start is mid-clip: nothing is crossed at it
                extra.freshA = false;
            }
        }
        touch(a, extra);
    }

    // ---- events and finishes ----

    void pendEvent(const Animating& a, int instance, const VatClip& clip, const ClipEventData& data, double when){
        VatPendingCallback callback;
        callback.when = when;
        callback.instance = instance;
        callback.clip = clip.name;
        callback.event.functionName = data.function;
        callback.event.args = data.args;
        callback.event.state = clip.name;
        callback.event.time = data.time;
        a.runtime->pending.push_back(std::move(callback));
    }

    bool firesFor(const Animating& a, int instance){
        // a crowd's own (undrawn) state fires nothing; its instances only with instanceEvents
        return a.fire && (instance < 0 ? !a.instanced : a.animator->instanceEvents);
    }

    // events of one track over the clock interval [t0, t1]
    void trackEvents(const Animating& a, int instance, const VatTrack& track, const VatTrackHold& hold, bool& fresh,
                     double t0, double t1){
        const bool includeStart = fresh;
        fresh = false;
        if (!a.validClip(track.clip) || hold.held) return;
        const VatClip& clip = a.clip(track.clip);
        std::vector<VatCrossedEvent> crossed;
        vatCrossedEvents(clip, vatTrackTau(track, hold, t0), vatTrackTau(track, hold, t1), includeStart, crossed);
        for (const VatCrossedEvent& event : crossed){
            const double when = (double)track.start + event.position / (double)track.speed;
            pendEvent(a, instance, clip, clip.events[event.index], std::clamp(when, t0, t1));
        }
    }

    // the events of the interval: of the dominant track only (weight >= 0.5); a crossfade hands
    // over at its midpoint
    void stateEvents(const Animating& a, int instance, VatTrackPair& pair, VatStateExtra& extra, double t0, double t1){
        // nothing fires (edit mode, an instance without instanceEvents): a start still waiting
        // for its events keeps them for when it does (Play after edit mode starts at 0)
        if (!firesFor(a, instance)) return;
        if (!hasB(pair)){
            trackEvents(a, instance, pair.a, extra.holdA, extra.freshA, t0, t1);
            return;
        }
        if (blendMode(pair)){
            if (pair.blend < 0.5f){
                trackEvents(a, instance, pair.a, extra.holdA, extra.freshA, t0, t1);
                return;
            }
            // B follows A's phase: A's local time scaled to B's length
            extra.freshA = false;
            if (!a.validClip(pair.a.clip) || !a.validClip(pair.b.clip) || extra.holdA.held) return;
            const VatClip& clipA = a.clip(pair.a.clip);
            const VatClip& clipB = a.clip(pair.b.clip);
            const double lengthA = clipA.length();
            if (!(lengthA > 0.0)) return;
            const double scale = (double)clipB.length() / lengthA;
            const double from = vatTrackTau(pair.a, extra.holdA, t0);
            const double to = vatTrackTau(pair.a, extra.holdA, t1);
            std::vector<VatCrossedEvent> crossed;
            vatCrossedEvents(clipB, from * scale, to * scale, false, crossed);
            for (const VatCrossedEvent& event : crossed){
                const double position = event.position / scale;
                const double when = to != from ? t0 + (t1 - t0) * (position - from) / (to - from) : t0;
                pendEvent(a, instance, clipB, clipB.events[event.index], std::clamp(when, t0, t1));
            }
            return;
        }
        const double handover = (double)pair.fadeStart + 0.5 * std::max(0.0, (double)pair.fadeEnd - (double)pair.fadeStart);
        if (t0 < handover){
            trackEvents(a, instance, pair.a, extra.holdA, extra.freshA, t0, std::min(t1, handover));
        }else{
            extra.freshA = false;
        }
        // B is past its start by then: nothing exactly at the handover is a start
        extra.freshB = false;
        if (t1 > handover){
            trackEvents(a, instance, pair.b, extra.holdB, extra.freshB, std::max(t0, handover), t1);
        }
    }

    void pendFinish(const Animating& a, int instance, const std::string& clip, double when){
        if (!firesFor(a, instance)) return;
        VatPendingCallback callback;
        callback.when = when;
        callback.instance = instance;
        callback.finish = true;
        callback.clip = clip;
        a.runtime->pending.push_back(std::move(callback));
    }

    // A state from `prev` to `now`: events, crossfade completion, finishes, queued clips
    void advanceState(const Animating& a, int instance, VatTrackPair& pair, VatStateExtra& extra,
                      std::vector<std::string>& queue, double prev, double now){
        double t = prev;
        for (int step = 0; step <= MAX_TRANSITIONS; step++){
            enum class Next { None, Settle, End } next = Next::None;
            double at = now;

            if (hasB(pair) && !blendMode(pair)){
                if ((double)pair.fadeEnd <= t){
                    settle(a, pair, extra);
                    continue;
                }
                if ((double)pair.fadeEnd <= now){
                    at = pair.fadeEnd;
                    next = Next::Settle;
                }
            }else if (!hasB(pair) && a.validClip(pair.a.clip) && !extra.stopped && !extra.holdA.held){
                const VatClip& clip = a.clip(pair.a.clip);
                const double speed = pair.a.speed;
                const double from = vatTrackTau(pair.a, extra.holdA, t);
                const double to = vatTrackTau(pair.a, extra.holdA, now);
                if (clip.wrap == ClipWrapMode::Once || clip.wrap == ClipWrapMode::ClampForever){
                    const double length = clip.length();
                    const double end = speed > 0.0 ? length : 0.0;
                    bool reached = false;
                    if (length <= 0.0){
                        // one sample: done as soon as it played
                        reached = !extra.finishNotified;
                    }else if (speed > 0.0){
                        reached = from < end && to >= end;
                    }else{
                        reached = from > end && to <= end;
                    }
                    if (reached){
                        const double when = (double)pair.a.start + end / speed;
                        at = std::clamp(when, t, now);
                        next = Next::End;
                    }else if (!queue.empty() && (extra.finishNotified || (speed > 0.0 ? from >= end : from <= end))){
                        // finished before the queue was filled: the next one starts now
                        at = t;
                        next = Next::End;
                    }
                }else if (!queue.empty()){
                    // the next one waits for the end of the current cycle
                    const double period = vatClipPeriod(clip);
                    if (period > 0.0){
                        const double boundary = speed > 0.0 ? (std::floor(from / period) + 1.0) * period
                                                            : (std::ceil(from / period) - 1.0) * period;
                        if (speed > 0.0 ? to >= boundary : to <= boundary){
                            at = std::clamp((double)pair.a.start + boundary / speed, t, now);
                            next = Next::End;
                        }
                    }else{
                        at = t;
                        next = Next::End;
                    }
                }
            }

            stateEvents(a, instance, pair, extra, t, at);
            t = at;
            if (next == Next::None) break;
            if (next == Next::Settle){
                settle(a, pair, extra);
                continue;
            }
            // the end of A (once / clamp_forever), or of its cycle with something queued
            const VatClip& ended = a.clip(pair.a.clip);
            const bool endsPlaying = ended.wrap == ClipWrapMode::Once || ended.wrap == ClipWrapMode::ClampForever;
            if (endsPlaying && !extra.finishNotified){
                extra.finishNotified = true;
                pendFinish(a, instance, ended.name, at);
            }
            if (queue.empty()) break;
            const std::string name = queue.front();
            queue.erase(queue.begin());
            const int clip = findClip(a, name);
            a.runtime->watchDirty = true;
            if (clip < 0) continue;
            std::vector<std::string> rest;
            rest.swap(queue);
            playNow(a, pair, extra, queue, clip, at);
            queue.swap(rest);
        }
    }

    // the clock moved back by `shift` (an epoch): every time is rewritten once, loops and
    // ended clips brought near the clock so their floats keep their precision
    void rebaseState(const Animating& a, VatTrackPair& pair, VatStateExtra& extra, double shift, double now){
        // a crossfade that ended before the shift is settled first
        if (hasB(pair) && !blendMode(pair) && (double)pair.fadeEnd - shift <= now){
            settle(a, pair, extra);
        }
        auto rebaseTrack = [&](VatTrack& track, VatTrackHold& hold){
            // a held track's start is its local time, not a clock time: it stays
            if (!a.validClip(track.clip) || hold.held || track.speed == 0.0f) return;
            // the local time now, from the old clock (now + shift)
            double tau = (now + shift - (double)track.start) * (double)track.speed;
            const VatClip& clip = a.clip(track.clip);
            const double length = clip.length();
            if (clip.wrap == ClipWrapMode::Loop || clip.wrap == ClipWrapMode::PingPong){
                const double period = vatClipPeriod(clip);
                if (period > 0.0) tau -= std::floor(tau / period) * period;
            }else{
                // past either end it shows the end: kept within a second of it
                tau = std::clamp(tau, -1.0, length + 1.0);
            }
            track.start = (float)(now - tau / (double)track.speed);
        };
        rebaseTrack(pair.a, extra.holdA);
        if (hasB(pair) && !blendMode(pair)){
            rebaseTrack(pair.b, extra.holdB);
            pair.fadeStart = (float)((double)pair.fadeStart - shift);
            pair.fadeEnd = (float)((double)pair.fadeEnd - shift);
        }
        mirrorBlendB(pair);
        touch(a, extra);
    }

    // ---- requests ----

    struct Target{
        VatTrackPair* pair;
        VatStateExtra* extra;
        std::vector<std::string>* queue;
        int64_t index;                  // seed index: 0 the entity, instance + 1
    };

    void applyRequest(const Animating& a, const VertexAnimatorRequest& request, double now){
        VertexAnimatorComponent& animator = *a.animator;
        VertexAnimatorRuntime& runtime = *a.runtime;
        std::vector<Target> targets;
        const bool entity = request.instance < 0 || request.all;
        if (entity){
            targets.push_back({&animator.state, &runtime.entity, &animator.queued, 0});
        }
        if (request.all){
            for (size_t i = 0; i < animator.instanceStates.size(); i++){
                targets.push_back({&animator.instanceStates[i], &runtime.instances[i], &runtime.instances[i].queue, (int64_t)i + 1});
            }
        }else if (request.instance >= 0){
            if ((size_t)request.instance >= animator.instanceStates.size()){
                warnOnce(a, "instance|" + std::to_string(request.instance),
                         "no instance " + std::to_string(request.instance) + " (it has " +
                         std::to_string(animator.instanceStates.size()) + "; instances count from 0)");
                return;
            }
            const size_t i = (size_t)request.instance;
            targets.push_back({&animator.instanceStates[i], &runtime.instances[i], &runtime.instances[i].queue, (int64_t)i + 1});
        }
        if (request.instance >= 0 || request.all) runtime.watchDirty = true;

        using Kind = VertexAnimatorRequest::Kind;
        int clip = -1;
        int clipB = -1;
        if (request.kind == Kind::Play || request.kind == Kind::CrossFade || request.kind == Kind::Queue || request.kind == Kind::Blend){
            clip = findClip(a, request.clip);
            if (clip < 0) return;
        }
        if (request.kind == Kind::Blend){
            clipB = findClip(a, request.clipB);
            if (clipB < 0) return;
        }

        for (const Target& target : targets){
            VatTrackPair& pair = *target.pair;
            VatStateExtra& extra = *target.extra;
            std::vector<std::string>& queue = *target.queue;
            switch (request.kind){
                case Kind::Play:
                    if (request.value > 0.0f) crossFade(a, pair, extra, queue, clip, request.value, now);
                    else playNow(a, pair, extra, queue, clip, now);
                    break;
                case Kind::CrossFade:
                    crossFade(a, pair, extra, queue, clip, request.value, now);
                    break;
                case Kind::Blend:
                    blendClips(a, pair, extra, queue, clip, clipB, request.value, now);
                    break;
                case Kind::Queue:
                    queueClip(a, pair, extra, queue, clip, now);
                    break;
                case Kind::Stop:
                    stopState(a, pair, extra, queue, now);
                    break;
                case Kind::SetTime:
                    setStateTime(a, pair, extra, request.value, now);
                    break;
                case Kind::SetSpeed:
                    extra.speed = std::isfinite(request.value) ? request.value : 0.0f;
                    respeed(a, pair, extra, now);
                    break;
                case Kind::RandomizeTimes:
                    randomizeState(a, pair, extra, (uint64_t)(int64_t)request.value, target.index, now);
                    break;
            }
        }
    }

    // ---- GPU state, bounds, attachments ----

    bool samePacked(const Vector4& a0, const Vector4& a1, const Vector4& b0, const Vector4& b1){
        return a0 == b0 && a1 == b1;
    }

    // true when the entity's state changed on the GPU (also when it was written directly, by the
    // editor or a test, without a request)
    bool writeGpu(const Animating& a, InstancedMeshComponent* instanced){
        VertexAnimatorComponent& animator = *a.animator;
        VertexAnimatorRuntime& runtime = *a.runtime;
        Vector4 v0, v1;
        vatPackInstance(animator.state, v0, v1);
        const bool entityChanged = !samePacked(v0, v1, animator.gpuState0, animator.gpuState1);
        if (entityChanged){
            animator.gpuState0 = v0;
            animator.gpuState1 = v1;
        }
        runtime.entity.dirty = false;

        if (!runtime.anyDirty) return entityChanged;
        runtime.anyDirty = false;
        if (!instanced) return entityChanged;
        bool changed = false;
        const size_t count = std::min(instanced->instances.size(), animator.instanceStates.size());
        for (size_t i = 0; i < count; i++){
            VatStateExtra& extra = runtime.instances[i];
            if (!extra.dirty) continue;
            extra.dirty = false;
            InstanceData& instance = instanced->instances[i];
            vatPackInstance(animator.instanceStates[i], v0, v1);
            if (!samePacked(v0, v1, instance.vat0, instance.vat1)){
                instance.vat0 = v0;
                instance.vat1 = v1;
                changed = true;
            }
        }
        if (changed){
            instanced->needUpdateInstances = true;
        }
        return entityChanged;
    }

    // the clips in use (bit per clip) and whether a drawn state is at rest (A without a clip):
    // the instances' when instanced, else the entity's
    uint64_t clipMask(const Animating& a, const InstancedMeshComponent* instanced, bool& rest){
        uint64_t mask = 0;
        rest = false;
        auto add = [&mask, &rest, &a](const VatTrackPair& pair){
            if (a.validClip(pair.a.clip) && pair.a.clip < 64) mask |= (uint64_t)1 << pair.a.clip;
            if (a.validClip(pair.b.clip) && pair.b.clip < 64) mask |= (uint64_t)1 << pair.b.clip;
            if (!a.validClip(pair.a.clip)) rest = true;
        };
        if (instanced){
            for (const VatTrackPair& pair : a.animator->instanceStates) add(pair);
        }else{
            add(a.animator->state);
        }
        return mask;
    }

    void setAnimatedBounds(Scene* scene, Entity entity, const AABB& bounds){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || mesh->animatedBounds == bounds) return;
        mesh->animatedBounds = bounds;
        mesh->needUpdateAABB = true;
        if (InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity)){
            instanced->needUpdateInstances = true;
        }
    }

    // the mesh's own bounds when a drawn state is at rest (the clips' bounds don't hold the rest pose)
    AABB restBoundsOf(const Animating& a, bool rest){
        const MeshComponent* mesh = rest ? a.scene->findComponent<MeshComponent>(a.entity) : nullptr;
        return mesh ? mesh->verticesAABB : AABB();
    }

    // the bounds of the clips in use, and of the rest pose when a state is at rest
    void updateBounds(const Animating& a, const InstancedMeshComponent* instanced){
        VertexAnimatorRuntime& runtime = *a.runtime;
        bool rest = false;
        const uint64_t mask = clipMask(a, instanced, rest);
        const AABB restBounds = restBoundsOf(a, rest);
        if (runtime.boundsSet && mask == runtime.clipMask && rest == runtime.anyRest && restBounds == runtime.restBounds) return;
        runtime.clipMask = mask;
        runtime.anyRest = rest;
        runtime.restBounds = restBounds;
        runtime.boundsSet = true;

        AABB bounds;
        for (size_t c = 0; c < a.data->clips.size() && c < 64; c++){
            const VatBounds& clip = a.data->clips[c].bounds;
            if ((mask & ((uint64_t)1 << c)) && clip.valid){
                bounds.merge(AABB(clip.min, clip.max));
            }
        }
        bounds.merge(restBounds);
        // clips without bounds (and no mesh bounds): the asset's (every frame)
        if (bounds.isNull() && a.data->bounds.valid) bounds = AABB(a.data->bounds.min, a.data->bounds.max);
        setAnimatedBounds(a.scene, a.entity, bounds);
    }

    // the rest state on the GPU (a disabled or removed animator): the mesh draws at rest at once,
    // its instances too (the loaded VAT variant still reads them until the mesh reloads)
    void writeRest(Scene* scene, Entity entity, VertexAnimatorComponent* animator){
        if (animator){
            animator->gpuActive = false;
            animator->gpuState0 = VertexAnimatorComponent().gpuState0;
            animator->gpuState1 = VertexAnimatorComponent().gpuState1;
        }
        if (InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity)){
            const InstanceData rest;
            bool changed = false;
            for (InstanceData& instance : instanced->instances){
                if (instance.vat0 != rest.vat0 || instance.vat1 != rest.vat1){
                    instance.vat0 = rest.vat0;
                    instance.vat1 = rest.vat1;
                    changed = true;
                }
            }
            if (changed) instanced->needUpdateInstances = true;
        }
    }

    // every state written to the GPU again at the next write (compared with what is there)
    void touchAll(VertexAnimatorRuntime& runtime){
        runtime.entity.dirty = true;
        for (VatStateExtra& extra : runtime.instances) extra.dirty = true;
        runtime.anyDirty = true;
    }

    Animating animating(Scene* scene, Entity entity, VertexAnimatorComponent& animator, bool fire){
        Animating a;
        a.scene = scene;
        a.entity = entity;
        a.animator = &animator;
        a.runtime = animator.runtime.get();
        a.asset = animator.asset.get();
        a.data = &animator.asset->getData();
        a.speed = std::isfinite(animator.speed) ? animator.speed : 0.0f;
        a.fire = fire;
        a.instanced = scene->findComponent<InstancedMeshComponent>(entity) != nullptr;
        return a;
    }

    void moveAttachments(const Animating& a, double now){
        VertexAnimatorComponent& animator = *a.animator;
        if (animator.attachments.empty()) return;
        const Transform* owner = a.scene->findComponent<Transform>(a.entity);
        for (const VertexAnimatorAttachment& attachment : animator.attachments){
            if (attachment.entity == NULL_ENTITY || attachment.entity == a.entity || !a.scene->isEntityCreated(attachment.entity)) continue;
            Transform* transform = a.scene->findComponent<Transform>(attachment.entity);
            if (!transform) continue;
            const int socket = a.data->findSocket(attachment.socket);
            if (socket < 0){
                warnOnce(a, "socket|" + attachment.socket, "no socket '" + attachment.socket + "' in " + animator.animation +
                         " (bake it with sockets, or pick one of the asset's)");
                continue;
            }
            Vector3 position;
            Quaternion rotation;
            if (!a.asset->sampleSocket(socket, animator.state, (float)now, position, rotation)) continue;

            if (transform->parent != a.entity && owner){
                // not a child: the socket in the world, then in the attachment's parent space
                position = owner->modelMatrix * position;
                rotation = owner->worldRotation * rotation;
                const Transform* parent = transform->parent != NULL_ENTITY ? a.scene->findComponent<Transform>(transform->parent) : nullptr;
                if (parent){
                    position = parent->modelMatrix.inverse() * position;
                    rotation = parent->worldRotation.inverse() * rotation;
                }
            }
            if (transform->position != position || transform->rotation != rotation){
                transform->position = position;
                transform->rotation = rotation;
                transform->needUpdate = true;
            }
        }
    }

    // ---- the animator's life ----

    // back to before its first update (the asset stays)
    void resetPlayback(VertexAnimatorComponent& animator){
        animator.state = VatTrackPair();
        for (VatTrackPair& pair : animator.instanceStates) pair = VatTrackPair();
        animator.queued.clear();
        animator.requests.clear();
        animator.started = false;
        animator.lastNow = 0.0f;
        animator.gpuNow = 0.0f;
        if (animator.runtime){
            VertexAnimatorRuntime& runtime = *animator.runtime;
            runtime.clock = 0.0;
            runtime.epoch = 0;
            runtime.lastNow = 0.0;
            runtime.entity = VatStateExtra();
            for (VatStateExtra& extra : runtime.instances) extra = VatStateExtra();
            runtime.pending.clear();
            // a reset from a callback: the rest of the list being dispatched goes too
            runtime.dispatching.clear();
            runtime.dispatchNext = 0;
            runtime.appliedSpeed = animator.speed;
            runtime.anyDirty = true;
            runtime.watchDirty = true;
            runtime.boundsSet = false;
        }
    }

    // instanceStates (and the runtime's) as many as the instances; new ones start like the
    // animator did
    void syncInstances(const Animating& a, const InstancedMeshComponent* instanced, double now){
        VertexAnimatorComponent& animator = *a.animator;
        VertexAnimatorRuntime& runtime = *a.runtime;
        const size_t count = instanced ? instanced->instances.size() : 0;
        if (runtime.instances.size() != animator.instanceStates.size()){
            runtime.instances.resize(animator.instanceStates.size());
        }
        if (animator.instanceStates.size() == count) return;
        const size_t old = animator.instanceStates.size();
        animator.instanceStates.resize(count);
        runtime.instances.resize(count);
        for (size_t i = old; i < count; i++){
            if (animator.started){
                startState(a, animator.instanceStates[i], runtime.instances[i], runtime.instances[i].queue, (int64_t)i + 1, now);
            }else{
                touch(a, runtime.instances[i]);
            }
        }
        runtime.watchDirty = true;
        runtime.anyDirty = true;
    }

    // a reloaded asset (a rebake): every state plays the clip of the same name, or the default
    void remapClips(const Animating& a, const VatData& old, double now){
        auto remap = [&](VatTrack& track){
            if (track.clip < 0) return;
            const int index = track.clip < (int)old.clips.size() ? a.data->findClip(old.clips[(size_t)track.clip].name) : -1;
            track.clip = index >= 0 ? index : defaultClip(a);
        };
        auto remapState = [&](VatTrackPair& pair, VatStateExtra& extra){
            remap(pair.a);
            remap(pair.b);
            if (pair.a.clip < 0 && pair.b.clip >= 0){
                pair.a = pair.b;
                pair.b = VatTrack();
            }
            if (pair.a.clip < 0) pair = VatTrackPair();
            respeed(a, pair, extra, now);
        };
        remapState(a.animator->state, a.runtime->entity);
        for (size_t i = 0; i < a.animator->instanceStates.size() && i < a.runtime->instances.size(); i++){
            remapState(a.animator->instanceStates[i], a.runtime->instances[i]);
        }
        a.runtime->boundsSet = false;
    }

    void failLoad(Scene* scene, Entity entity, VertexAnimatorComponent& animator, VertexAnimatorRuntime& runtime, const std::string& message){
        animator.asset.reset();
        animator.loadError = message;
        animator.gpuActive = false;
        runtime.failed = true;
        runtime.retryTimer = 0.0f;
        if (message != runtime.loggedError){
            Log::error("Vertex Animator '%s': %s", scene->getEntityName(entity).c_str(), message.c_str());
            runtime.loggedError = message;
        }
    }

    // the asset of `animation`, loaded or reloaded as needed; null while there is none
    bool ensureLoaded(Scene* scene, Entity entity, VertexAnimatorComponent& animator, VertexAnimatorRuntime& runtime,
                      double dt, double now){
        if (animator.animation.empty()){
            animator.asset.reset();
            animator.needsLoad = false;
            animator.loadError.clear();
            return false;
        }
        if (!animator.needsLoad && animator.asset &&
                VertexAnimationPool::getGeneration(animator.animation) != animator.loadedGeneration){
            animator.needsLoad = true;
        }
        if (!animator.needsLoad && runtime.failed){
            runtime.retryTimer += (float)dt;
            if (runtime.retryTimer >= FAILED_RETRY_SECONDS){
                animator.needsLoad = true;
            }
        }
        if (animator.needsLoad){
            animator.needsLoad = false;
            std::string error;
            std::vector<ClipWarning> warnings;
            std::shared_ptr<const VertexAnimation> loaded = VertexAnimationPool::getOrLoad(animator.animation, &error, &warnings);
            if (!loaded){
                failLoad(scene, entity, animator, runtime, animator.animation + ": " + (error.empty() ? std::string("could not be loaded") : error));
                return false;
            }
            for (const ClipWarning& warning : warnings){
                Log::warn("Vertex Animator '%s': %s: %s: %s", scene->getEntityName(entity).c_str(), animator.animation.c_str(),
                          warning.path.c_str(), warning.message.c_str());
            }
            std::shared_ptr<const VertexAnimation> old = animator.asset;
            animator.asset = loaded;
            animator.loadedGeneration = VertexAnimationPool::getGeneration(animator.animation);
            animator.loadError.clear();
            runtime.failed = false;
            runtime.retryTimer = 0.0f;
            runtime.loggedError.clear();
            runtime.boundsSet = false;
            if (old && old != loaded && animator.started){
                Animating a;
                a.scene = scene;
                a.entity = entity;
                a.animator = &animator;
                a.runtime = &runtime;
                a.asset = loaded.get();
                a.data = &loaded->getData();
                a.speed = std::isfinite(animator.speed) ? animator.speed : 0.0f;
                remapClips(a, old->getData(), now);
            }
        }
        return animator.asset != nullptr;
    }

    // The mesh the asset was baked for: the GameObject's Model is "<.vat's folder>/<data.mesh>".
    // False with `wait` while the Model loads, else with the mesh_mismatch text
    bool meshMatches(Scene* scene, Entity entity, const VertexAnimatorComponent& animator, const VatData& data,
                     bool& wait, std::string& error){
        wait = false;
        const std::string expected = expectedMesh(animator.animation, data);
        const std::string fix = ": set the Model to " + expected + " (Inspector: Use " + fileName(expected) + ")";
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (!model || !scene->findComponent<MeshComponent>(entity)){
            error = "mesh_mismatch: '" + scene->getEntityName(entity) + "' has no Model; " + animator.animation +
                    " plays on its baked mesh " + expected + fix;
            return false;
        }
        if (model->needUpdateModel){
            wait = true;
            return false;
        }
        const std::string actual = normalizePath(model->filename);
        if (actual != expected){
            error = "mesh_mismatch: the Model of '" + scene->getEntityName(entity) + "' is " +
                    (actual.empty() ? std::string("empty") : actual) + ", but " + animator.animation +
                    " plays on its baked mesh " + expected + fix;
            return false;
        }
        return true;
    }

}

VertexAnimatorSystem::VertexAnimatorSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<VertexAnimatorComponent>());
}

void VertexAnimatorSystem::updateAnimator(Entity entity, VertexAnimatorComponent& animator, double dt){
    VertexAnimatorRuntime& runtime = animatorRuntime(animator);

    if (!animator.enabled){
        // disabled: the mesh draws at rest at once (its instances too: written once), nothing moves
        animator.gpuActive = false;
        if (!runtime.restWritten){
            writeRest(scene, entity, &animator);
            runtime.restWritten = true;
        }
        if (runtime.boundsSet){
            setAnimatedBounds(scene, entity, AABB());
            runtime.boundsSet = false;
        }
        return;
    }
    if (runtime.restWritten){
        // enabled again: every state goes back to the GPU
        runtime.restWritten = false;
        touchAll(runtime);
    }

    // edit mode: Preview turned off puts it back where it started (deterministic Scene view)
    if (paused && runtime.previewing && !animator.preview){
        resetPlayback(animator);
    }
    runtime.previewing = paused && animator.preview;

    if (runtime.path != animator.animation){
        // another animation: starts over, its clock too (what scripts asked for already stays)
        std::vector<VertexAnimatorRequest> requests;
        requests.swap(animator.requests);
        resetPlayback(animator);
        animator.requests.swap(requests);
        animator.asset.reset();
        animator.loadError.clear();
        runtime.path = animator.animation;
        runtime.failed = false;
        runtime.loggedError.clear();
        animator.needsLoad = true;
    }

    // the clock: play mode, or a preview in edit mode. The previous update's `now` in double:
    // events cross (prev, now], each exactly once
    double prev = runtime.lastNow;
    if ((!paused || animator.preview) && std::isfinite(dt) && dt > 0.0){
        runtime.clock += dt;
    }
    double now = runtime.clock - (double)runtime.epoch * VAT_EPOCH_SECONDS;
    bool rebase = false;
    double shift = 0.0;
    if (now >= VAT_EPOCH_SECONDS){
        const int64_t periods = (int64_t)std::floor(now / VAT_EPOCH_SECONDS);
        runtime.epoch += periods;
        shift = (double)periods * VAT_EPOCH_SECONDS;
        now -= shift;
        prev -= shift;
        rebase = true;
    }

    if (!ensureLoaded(scene, entity, animator, runtime, dt, now)){
        if (rebase){
            // no clip table to bring them near the clock: the times only move with it
            auto shiftState = [shift](VatTrackPair& pair){
                // (a held track's start is its local time: it stays)
                if (pair.a.speed != 0.0f) pair.a.start = (float)((double)pair.a.start - shift);
                if (pair.b.speed != 0.0f) pair.b.start = (float)((double)pair.b.start - shift);
                pair.fadeStart = (float)((double)pair.fadeStart - shift);
                pair.fadeEnd = (float)((double)pair.fadeEnd - shift);
            };
            shiftState(animator.state);
            for (VatTrackPair& pair : animator.instanceStates) shiftState(pair);
        }
        animator.gpuActive = false;
        runtime.lastNow = now;
        animator.lastNow = (float)now;
        animator.gpuNow = (float)now;
        if (runtime.boundsSet){
            setAnimatedBounds(scene, entity, AABB());
            runtime.boundsSet = false;
        }
        return;
    }

    Animating a = animating(scene, entity, animator, !paused);

    // the mesh it plays on
    bool wait = false;
    std::string meshError;
    const bool matches = meshMatches(scene, entity, animator, *a.data, wait, meshError);
    if (!matches && !wait){
        if (animator.loadError != meshError){
            animator.loadError = meshError;
            if (runtime.loggedError != meshError){
                Log::warn("Vertex Animator '%s': %s", scene->getEntityName(entity).c_str(), meshError.c_str());
                runtime.loggedError = meshError;
            }
        }
    }else if (matches && animator.loadError.compare(0, 14, "mesh_mismatch:") == 0){
        animator.loadError.clear();
        runtime.loggedError.clear();
    }
    animator.gpuActive = matches;
    animator.gpuMode = a.data->mode;

    InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
    // the instances changed elsewhere (a whole InstanceData written, an editor undo): their
    // vat0/vat1 are compared and written again where they differ
    if (instanced && instanced->needUpdateInstances){
        touchAll(runtime);
    }

    if (rebase){
        rebaseState(a, animator.state, runtime.entity, shift, now);
        for (size_t i = 0; i < animator.instanceStates.size() && i < runtime.instances.size(); i++){
            rebaseState(a, animator.instanceStates[i], runtime.instances[i], shift, now);
        }
    }

    syncInstances(a, instanced, prev);

    if (!animator.started){
        startState(a, animator.state, runtime.entity, animator.queued, 0, prev);
        for (size_t i = 0; i < animator.instanceStates.size(); i++){
            startState(a, animator.instanceStates[i], runtime.instances[i], runtime.instances[i].queue, (int64_t)i + 1, prev);
        }
        animator.started = true;
        runtime.appliedSpeed = a.speed;
        runtime.watchDirty = true;
    }

    // a new component speed: every track keeps its phase
    if (runtime.appliedSpeed != a.speed){
        respeed(a, animator.state, runtime.entity, prev);
        for (size_t i = 0; i < animator.instanceStates.size(); i++){
            respeed(a, animator.instanceStates[i], runtime.instances[i], prev);
        }
        runtime.appliedSpeed = a.speed;
        runtime.watchDirty = true;
    }

    // what scripts and the editor asked since the last update
    if (!animator.requests.empty()){
        std::vector<VertexAnimatorRequest> requests;
        requests.swap(animator.requests);
        for (const VertexAnimatorRequest& request : requests){
            applyRequest(a, request, prev);
        }
    }

    advanceState(a, -1, animator.state, runtime.entity, animator.queued, prev, now);

    // instances: every one when its events are wanted, else only those with work to do
    if (animator.instanceEvents){
        for (size_t i = 0; i < animator.instanceStates.size(); i++){
            advanceState(a, (int)i, animator.instanceStates[i], runtime.instances[i], runtime.instances[i].queue, prev, now);
        }
        runtime.watchDirty = true;
    }else{
        if (runtime.watchDirty){
            runtime.watched.clear();
            for (size_t i = 0; i < runtime.instances.size(); i++){
                const VatStateExtra& extra = runtime.instances[i];
                if (!extra.queue.empty()){
                    runtime.watched.push_back((int)i);
                }
            }
            runtime.watchDirty = false;
        }
        for (int i : runtime.watched){
            if ((size_t)i >= animator.instanceStates.size()) continue;
            advanceState(a, i, animator.instanceStates[(size_t)i], runtime.instances[(size_t)i], runtime.instances[(size_t)i].queue, prev, now);
        }
    }

    bool changed = runtime.anyDirty || runtime.entity.dirty;
    if (writeGpu(a, instanced)) changed = true;
    runtime.lastNow = now;
    animator.lastNow = (float)now;
    animator.gpuNow = (float)now;

    if (matches){
        // (a state at rest: the mesh's bounds may arrive after it, when its model loads)
        if (changed || !runtime.boundsSet || (runtime.anyRest && restBoundsOf(a, true) != runtime.restBounds)){
            updateBounds(a, instanced);
        }
        moveAttachments(a, now);
    }else if (runtime.boundsSet){
        setAnimatedBounds(scene, entity, AABB());
        runtime.boundsSet = false;
    }

    if (!runtime.pending.empty() &&
            std::find(withCallbacks.begin(), withCallbacks.end(), entity) == withCallbacks.end()){
        withCallbacks.push_back(entity);
    }
}

void VertexAnimatorSystem::applyRequests(Entity entity){
    if (updating || !scene->isEntityCreated(entity)) return;
    VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(entity);
    if (!animator || animator->requests.empty() || !animator->asset || !animator->started || animator->needsLoad ||
            !animator->runtime || !animator->enabled){
        return;
    }
    VertexAnimatorRuntime& runtime = *animator->runtime;
    Animating a = animating(scene, entity, *animator, false);
    InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
    // instances added since the last update first: a request may name them
    syncInstances(a, instanced, runtime.lastNow);
    std::vector<VertexAnimatorRequest> requests;
    requests.swap(animator->requests);
    for (const VertexAnimatorRequest& request : requests){
        applyRequest(a, request, runtime.lastNow);
    }
    writeGpu(a, instanced);
    if (animator->gpuActive){
        updateBounds(a, instanced);
        moveAttachments(a, runtime.lastNow);
    }
}

void VertexAnimatorSystem::refreshInstances(){
    auto animators = scene->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
        const Entity entity = animators->getEntity(i);
        InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
        if (!instanced || !instanced->needUpdateInstances || !animator.enabled || !animator.asset || !animator.started ||
                animator.needsLoad || !animator.runtime || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        VertexAnimatorRuntime& runtime = *animator.runtime;
        Animating a = animating(scene, entity, animator, false);
        syncInstances(a, instanced, runtime.lastNow);
        touchAll(runtime);
        writeGpu(a, instanced);
        if (animator.gpuActive) updateBounds(a, instanced);
    }
}

bool VertexAnimatorSystem::dispatchCallbacks(){
    if (withCallbacks.empty()) return false;
    std::vector<Entity> entities;
    entities.swap(withCallbacks);
    bool called = false;

    for (Entity entity : entities){
        VertexAnimatorComponent* animator = scene->isEntityCreated(entity) ? scene->findComponent<VertexAnimatorComponent>(entity) : nullptr;
        if (!animator || !animator->runtime || !animator->runtime->dispatching.empty()) continue;
        // held: a callback can remove the component
        std::shared_ptr<VertexAnimatorRuntime> runtime = animator->runtime;
        // dispatched from the runtime, so an instance removed by a callback re-indexes (or drops)
        // the callbacks not called yet (eraseInstanceState)
        runtime->dispatching.swap(runtime->pending);
        runtime->dispatchNext = 0;
        std::stable_sort(runtime->dispatching.begin(), runtime->dispatching.end(), [](const VatPendingCallback& a, const VatPendingCallback& b){
            return a.when < b.when;
        });

        while (runtime->dispatchNext < runtime->dispatching.size()){
            // a copy: the list may change under the call
            const VatPendingCallback callback = runtime->dispatching[runtime->dispatchNext++];
            // callbacks may add components (arrays move) or destroy the GameObject
            animator = scene->isEntityCreated(entity) ? scene->findComponent<VertexAnimatorComponent>(entity) : nullptr;
            if (!animator || animator->runtime != runtime) break;
            called = true;
            if (callback.instance >= 0){
                if (callback.finish){
                    animator->onInstanceFinish.call(callback.instance, callback.clip);
                }else{
                    const AnimationEvent event = callback.event;
                    animator->onInstanceEvent.call(callback.instance, event);
                }
                continue;
            }
            if (callback.finish){
                const std::string clip = callback.clip;
                animator->onFinish.call(clip);
                continue;
            }
            const bool subscribed = animator->onEvent.hasSubscribers();
            const AnimationEvent event = callback.event;
            animator->onEvent.call(event);
            // then the method of that name on the GameObject's Lua scripts (as Animation Player events)
            const bool scripted = LuaBinding::callLuaScriptMethod(scene, entity, event.functionName, event.args);
            if (!subscribed && !scripted){
                const std::string key = runtime->path + "|" + callback.clip + "|" + event.functionName;
                if (warnedEventHandlers.insert(key).second){
                    Log::warn("Vertex Animator '%s': event '%s' of clip '%s' has no handler (no onEvent subscriber, no script method)",
                              scene->getEntityName(entity).c_str(), event.functionName.c_str(), callback.clip.c_str());
                }
            }
        }
        runtime->dispatching.clear();
        runtime->dispatchNext = 0;
    }
    return called;
}

void VertexAnimatorSystem::resetAnimators(){
    auto animators = scene->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
        resetPlayback(animator);
        animator.preview = false;
        if (animator.runtime) animator.runtime->previewing = false;
        // play's scripts and C++ libraries are gone after it: their callbacks with them
        animator.onEvent.clear();
        animator.onFinish.clear();
        animator.onInstanceEvent.clear();
        animator.onInstanceFinish.clear();
    }
    withCallbacks.clear();
    warnedEventHandlers.clear();
}

bool VertexAnimatorSystem::stopPreviews(){
    bool any = false;
    auto animators = scene->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
        const bool previewed = animator.preview || (animator.runtime && animator.runtime->previewing);
        if (!previewed) continue;
        any = true;
        animator.preview = false;
        resetPlayback(animator);
        if (animator.runtime) animator.runtime->previewing = false;
    }
    if (any){
        // back at the start right away (GPU state, attachments) before a save or play reads them
        update(0.0);
    }
    return any;
}

void VertexAnimatorSystem::load(){
}

void VertexAnimatorSystem::draw(){
}

void VertexAnimatorSystem::destroy(){
}

void VertexAnimatorSystem::update(double dt){
    updating = true;
    auto animators = scene->getComponentArray<VertexAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
        const Entity entity = animators->getEntity(i);
        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        updateAnimator(entity, animator, dt);
    }
    updating = false;

    // user code last: it may add or remove components (and move the array under the loop). An
    // update run from a callback leaves its callbacks for the next one
    if (!paused){
        if (!dispatching){
            dispatching = true;
            const bool called = dispatchCallbacks();
            dispatching = false;
            // instances the callbacks wrote whole or added: their states again, before the draw
            if (called) refreshInstances();
        }
    }else{
        for (Entity entity : withCallbacks){
            VertexAnimatorComponent* animator = scene->isEntityCreated(entity) ? scene->findComponent<VertexAnimatorComponent>(entity) : nullptr;
            if (animator && animator->runtime) animator->runtime->pending.clear();
        }
        withCallbacks.clear();
    }
}

void VertexAnimatorSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<VertexAnimatorComponent>()){
        // the mesh is a plain mesh again: its own bounds
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            if (!mesh->animatedBounds.isNull()){
                mesh->animatedBounds = AABB();
                mesh->needUpdateAABB = true;
            }
        }
        writeRest(scene, entity, nullptr);
    }
}
