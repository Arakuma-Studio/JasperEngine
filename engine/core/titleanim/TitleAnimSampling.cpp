// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimSampling.h"

#include "animation/AnimationClipIO.h"
#include "animation/ClipTypes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    bool passSplitDisabled = false;

    double clampTo(double v, double lo, double hi){
        return v < lo ? lo : (v > hi ? hi : v);
    }

    double titleAnimationFraction(double v){
        return v - std::floor(v);
    }

    // one pass of the unwrapped time line: [start, end] in seconds since the start, and how a
    // local time maps into it
    struct Pass{
        int index = 0;
        double start = 0.0, end = 0.0;
        bool backward = false;          // ping-pong's odd passes run D -> loopStart
        bool startIncluded = true, endIncluded = true;
    };

    Pass passAt(int k, double duration, double loopStart, AnimWrap wrap){
        Pass p;
        p.index = k;
        if (k == 0){
            p.start = 0.0;
            p.end = duration;
            return p;
        }
        const double span = std::max(1e-9, duration - loopStart);
        p.start = duration + (double)(k - 1) * span;
        p.end = p.start + span;
        if (wrap == AnimWrap::Loop){
            // local loopStart is the pass's first instant, local D is the next pass's start
            p.startIncluded = true;
            p.endIncluded = false;
        }else{
            // the turning points belong to the pass that reaches them
            p.backward = (k % 2) == 1;
            p.startIncluded = false;
            p.endIncluded = true;
        }
        return p;
    }

    // the pass an instant is in when moving forward from it (at its start or inside: [start, end))
    int passForward(double u, double duration, double loopStart, AnimWrap wrap){
        if (wrap == AnimWrap::Once || u < duration) return 0;
        const double span = std::max(1e-9, duration - loopStart);
        return 1 + (int)std::floor((u - duration) / span);
    }

    // ... and when moving backward from it (at its end or inside: (start, end])
    int passBackward(double u, double duration, double loopStart, AnimWrap wrap){
        if (wrap == AnimWrap::Once || u <= duration) return 0;
        const double span = std::max(1e-9, duration - loopStart);
        return (int)std::ceil((u - duration) / span);
    }

    // where a marker at local time `m` sits in pass `p`; false when it is not in the pass
    bool positionIn(const Pass& p, double m, double duration, double loopStart, double& x){
        if (p.index == 0){
            if (m < 0.0 || m > duration) return false;
            x = m;
            return true;
        }
        if (m < loopStart || m > duration) return false;
        x = p.backward ? p.start + (duration - m) : p.start + (m - loopStart);
        if (x == p.start && !p.startIncluded) return false;
        if (x == p.end && !p.endIncluded) return false;
        return true;
    }

    void collectNaive(const CompiledTitleAnimation& animation, AnimWrap wrap, double from, double to, bool includeStart,
                      std::vector<CrossedMarker>& out){
        // AC6's red control: the wrapped local times, both ends inclusive across the wrap (a
        // marker on the boundary fires at the end of one update and at the start of the next)
        const double a = wrapAnimationTime(from, animation.duration, animation.loopStart, wrap);
        const double b = wrapAnimationTime(to, animation.duration, animation.loopStart, wrap);
        const double d = animation.duration, l = wrap == AnimWrap::Once ? 0.0 : animation.loopStart;
        for (size_t i = 0; i < animation.markers.size(); i++){
            const double m = animation.markers[i].time;
            bool hit = false;
            (void)includeStart;
            if (b >= a) hit = m >= a && m <= b;
            else hit = (m >= a && m <= d) || (m >= l && m <= b);
            if (to > from && hit) out.push_back({i, to});
        }
    }

}

// ==================================================================== wrap

double titleanim::wrapAnimationTime(double time, double duration, double loopStart, AnimWrap wrap){
    if (!(time > 0.0) || !(duration > 0.0)) return 0.0;
    if (time <= duration || wrap == AnimWrap::Once) return std::min(time, duration);
    const double l = clampTo(loopStart, 0.0, duration);
    const double span = duration - l;
    if (span <= 1e-9) return duration;
    const double u = time - duration;
    if (wrap == AnimWrap::Loop) return l + std::fmod(u, span);
    const double k = std::floor(u / span);
    const double r = u - k * span;
    return std::fmod(k, 2.0) == 0.0 ? duration - r : l + r;
}

double titleanim::wrapAnimationTime(double time, const CompiledTitleAnimation& animation){
    return wrapAnimationTime(time, animation.duration, animation.loopStart, animation.wrap);
}

// ==================================================================== compiling

size_t titleanim::compileNumericKeys(const std::vector<ClipKey>& keys, size_t width, CompiledTitleTrack& out){
    // D2: the keys go through the .anim normalize as one track of `width` floats, so times are
    // quantized, computed tangent modes get their tangents, keys without tangents in a track that
    // has some get linear ones and tangents win over eases exactly as a clip's
    ClipData data;
    ClipTrackData track;
    track.channel = ClipChannel::Morph;
    track.keys = keys;
    for (ClipKey& key : track.keys){
        key.value.resize(width, 0.0f);
        if (key.hasTangents()){
            key.inTangent.resize(width, 0.0f);
            key.outTangent.resize(width, 0.0f);
        }else{
            key.inTangent.clear();
            key.outTangent.clear();
        }
    }
    data.tracks.push_back(track);
    std::vector<ClipWarning> warnings;
    anim::normalize(data, &warnings);
    std::vector<ClipKey> normalized = data.tracks.empty() ? std::vector<ClipKey>() : data.tracks[0].keys;
    size_t dropped = 0;
    for (size_t k = 0; k < normalized.size() && k < keys.size(); k++){
        if (keys[k].ease != EaseType::LINEAR && normalized[k].ease == EaseType::LINEAR) dropped++;
    }
    bool tangents = false;
    for (const ClipKey& key : normalized) tangents = tangents || key.hasTangents();
    const size_t n = normalized.size();
    out.width = (uint8_t)width;
    out.times.resize(n);
    out.easings.resize(n);
    out.values.resize(n * width);
    out.inTangents.assign(tangents ? n * width : 0, 0.0f);
    out.outTangents.assign(tangents ? n * width : 0, 0.0f);
    for (size_t k = 0; k < n; k++){
        const ClipKey& key = normalized[k];
        out.times[k] = key.time;
        out.easings[k] = key.ease == EaseType::CUSTOM || key.ease == EaseType::CURVE ? EaseType::LINEAR : key.ease;
        for (size_t i = 0; i < width; i++){
            out.values[k * width + i] = i < key.value.size() ? key.value[i] : 0.0f;
            if (tangents && key.hasTangents()){
                out.inTangents[k * width + i] = i < key.inTangent.size() ? key.inTangent[i] : 0.0f;
                out.outTangents[k * width + i] = i < key.outTangent.size() ? key.outTangent[i] : 0.0f;
            }
        }
    }
    out.stepValues.clear();
    return dropped;
}

void titleanim::compileSteppedKeys(const std::vector<TitleKey>& keys, CompiledTitleTrack& out){
    const size_t n = keys.size();
    out.value = TrackValue::Stepped;
    out.width = 1;
    out.times.resize(n);
    out.easings.assign(n, EaseType::STEP);
    out.values.resize(n);
    out.inTangents.clear();
    out.outTangents.clear();
    out.stepValues.resize(n);
    for (size_t k = 0; k < n; k++){
        out.times[k] = keys[k].key.time;
        out.values[k] = (float)k;
        out.stepValues[k] = keys[k].stepped;
    }
}

// ==================================================================== stagger

uint64_t SplitMix64::next(){
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double SplitMix64::unit(){
    return (double)(next() >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t titleanim::mixSeeds(uint64_t a, uint64_t b){
    SplitMix64 r(a ^ (b * 0xD1B54A32D192ED03ull + 0x8CB92BA72F3D8DD7ull));
    return r.next();
}

void titleanim::staggerRanks(StaggerOrder order, size_t n, uint64_t documentSeed, uint32_t staggerSeed, std::vector<uint32_t>& ranks){
    ranks.assign(n, 0);
    if (n == 0) return;
    switch (order){
        case StaggerOrder::Forward:
            for (size_t i = 0; i < n; i++) ranks[i] = (uint32_t)i;
            break;
        case StaggerOrder::Reverse:
            for (size_t i = 0; i < n; i++) ranks[i] = (uint32_t)(n - 1 - i);
            break;
        case StaggerOrder::CenterOut:
        case StaggerOrder::EdgesIn:{
            // twice the distance from the centre, so an even count's two middle units are rank 0
            uint32_t largest = 0;
            for (size_t i = 0; i < n; i++){
                const long twice = std::labs((long)(2 * i) - (long)(n - 1));
                ranks[i] = (uint32_t)(twice / 2);
                largest = std::max(largest, ranks[i]);
            }
            if (order == StaggerOrder::EdgesIn)
                for (uint32_t& r : ranks) r = largest - r;
            break;
        }
        case StaggerOrder::Random:{
            std::vector<uint32_t> order(n);
            for (size_t i = 0; i < n; i++) order[i] = (uint32_t)i;
            SplitMix64 random(mixSeeds(documentSeed, staggerSeed));
            for (size_t i = n - 1; i > 0; i--){
                const size_t j = (size_t)(random.next() % (uint64_t)(i + 1));
                std::swap(order[i], order[j]);
            }
            for (size_t k = 0; k < n; k++) ranks[order[k]] = (uint32_t)k;
            break;
        }
    }
}

void titleanim::staggerDelays(const TitleStagger& stagger, size_t n, uint64_t documentSeed, std::vector<float>& delays){
    std::vector<uint32_t> ranks;
    staggerRanks(stagger.order, n, documentSeed, stagger.seed, ranks);
    delays.assign(n, 0.0f);
    if (n <= 1) return;
    uint32_t largest = 0;
    for (uint32_t r : ranks) largest = std::max(largest, r);
    for (size_t i = 0; i < n; i++){
        if (stagger.over >= 0.0f){
            const float u = largest > 0 ? (float)ranks[i] / (float)largest : 0.0f;
            delays[i] = stagger.over * anim::applyEase(stagger.ease, u);
        }else{
            delays[i] = (float)ranks[i] * std::max(0.0f, stagger.each);
        }
    }
}

// ==================================================================== sampling

void titleanim::sampleTrackValue(const CompiledTitleTrack& track, double time, double spanStart, double spanLength, double* out,
                                 anim::KeyCursor* cursor){
    const size_t width = std::max<size_t>(1, track.width);
    switch (track.source){
        case TrackSource::Keys:{
            const size_t n = track.times.size();
            if (n == 0){
                for (size_t i = 0; i < width; i++) out[i] = i < track.rest.size() ? track.rest[i] : 0.0;
                return;
            }
            const float t = (float)time;
            const size_t last = n - 1;
            if (std::isnan(t) || t <= track.times[0]){
                for (size_t i = 0; i < width; i++) out[i] = track.values[i];
                if (cursor) cursor->index = 0;
                return;
            }
            if (t >= track.times[last]){
                for (size_t i = 0; i < width; i++) out[i] = track.values[last * width + i];
                if (cursor) cursor->index = last;
                return;
            }
            anim::KeyCursor local;
            const anim::KeyLocation at = anim::locateKey(track.times.data(), n, track.easings.data(), n, t, cursor ? cursor : &local);
            const size_t previous = at.index > 0 ? at.index - 1 : 0;
            const size_t next = at.index;
            const bool cubic = at.index > 0 && track.hasTangents();
            float row[3] = {0.0f, 0.0f, 0.0f};
            anim::sampleFloatRow(at, track.values.data() + previous * width, track.values.data() + next * width, std::min<size_t>(width, 3),
                                 cubic ? track.outTangents.data() + previous * width : nullptr,
                                 cubic ? track.inTangents.data() + next * width : nullptr, row);
            for (size_t i = 0; i < width; i++) out[i] = row[i];
            return;
        }
        case TrackSource::Drive:{
            for (size_t i = 0; i < width; i++){
                const double from = i < track.driveFrom.size() ? track.driveFrom[i] : 0.0;
                const double to = i < track.driveTo.size() ? track.driveTo[i] : from;
                double v = from;
                if (track.cycles > 0.0f){
                    const double span = std::max(1e-6, spanLength);
                    const double f = titleAnimationFraction((double)track.cycles * (time - spanStart) / span);
                    // wrap: from -> to each cycle; else from -> to -> from (no jump at all)
                    const double u = track.driveWrap ? f : (f < 0.5 ? 2.0 * f : 2.0 * (1.0 - f));
                    v = from + (to - from) * (double)anim::applyEase(track.driveEase, (float)u);
                }else{
                    v = from + (double)track.rate * time;
                    if (track.driveWrap && to != from){
                        const double range = to - from;
                        v = from + range * titleAnimationFraction((v - from) / range);
                    }
                }
                out[i] = v;
            }
            return;
        }
        case TrackSource::Reseed:{
            out[0] = std::floor(time * (double)track.reseedRate + (double)track.reseedOffset);
            for (size_t i = 1; i < width; i++) out[i] = 0.0;
            return;
        }
    }
}

void titleanim::combineWithRest(TrackCombine combine, const double* sample, const float* rest, size_t width, double* out){
    for (size_t i = 0; i < width; i++){
        const double r = rest ? (double)rest[i] : 0.0;
        switch (combine){
            case TrackCombine::Replace: out[i] = sample[i]; break;
            case TrackCombine::Add: out[i] = r + sample[i]; break;
            case TrackCombine::Multiply: out[i] = r * sample[i]; break;
        }
    }
}

void titleanim::sampleAnimation(const CompiledTitleAnimation& animation, double time, TitleAnimPose& out){
    out.time = time;
    if (out.values.size() < animation.poseSize) return;
    const double spanStart = animation.spanStart();
    const double spanLength = animation.duration - spanStart;
    double raw[3];
    for (const CompiledTitleTrack& track : animation.tracks){
        const size_t width = std::max<size_t>(1, track.width);
        const bool restUsable = track.rest.size() >= width;
        for (uint32_t u = 0; u < track.units; u++){
            const double t = track.unitDelays.empty() ? time : time - (double)track.unitDelays[u];
            sampleTrackValue(track, t, spanStart, spanLength, raw);
            double* dst = out.values.data() + track.poseOffset + (size_t)u * width;
            if (track.value == TrackValue::Stepped){
                dst[0] = raw[0];
                continue;
            }
            combineWithRest(track.combine, raw, restUsable ? track.rest.data() : nullptr, width, dst);
            for (size_t i = 0; i < width; i++) dst[i] = clampTo(dst[i], track.min, track.max);
        }
    }
}

// ==================================================================== markers

void titleanim::setMarkerPassSplitDisabledForTesting(bool disabled){
    passSplitDisabled = disabled;
}

void titleanim::collectCrossedMarkers(const CompiledTitleAnimation& animation, double from, double to, bool includeStart,
                                      std::vector<CrossedMarker>& out){
    collectCrossedMarkers(animation, animation.wrap, from, to, includeStart, out);
}

void titleanim::collectCrossedMarkers(const CompiledTitleAnimation& animation, AnimWrap wrap, double from, double to, bool includeStart,
                                      std::vector<CrossedMarker>& out){
    if (animation.markers.empty() || !(animation.duration > 0.0) || std::isnan(from) || std::isnan(to)) return;
    if (from == to && !includeStart) return;
    if (passSplitDisabled){
        collectNaive(animation, wrap, from, to, includeStart, out);
        return;
    }
    const double d = animation.duration;
    const double l = wrap == AnimWrap::Once ? 0.0 : clampTo(animation.loopStart, 0.0, d);
    const bool forward = to >= from;
    const double lo = std::min(from, to), hi = std::max(from, to);
    const int startPass = forward ? passForward(from, d, l, wrap) : passBackward(from, d, l, wrap);
    // candidates (membership is exact per marker below)
    const int firstPass = std::max(0, passForward(lo, d, l, wrap) - 1);
    int lastPass = passForward(hi, d, l, wrap) + 1;
    // each marker fires at most once per call: past this many passes nothing new can cross
    lastPass = std::min(lastPass, firstPass + (int)animation.markers.size() + 2);
    if (wrap == AnimWrap::Once) lastPass = 0;

    // each marker fires at most once per call (collectCrossedEvents' rule): its first crossing in
    // firing order, kept per marker in a fixed table (no allocation: R35)
    struct Hit{ double x = 0.0; int pass = 0; bool set = false; };
    Hit best[TITLE_ANIM_MAX_MARKERS];
    const size_t count = std::min(animation.markers.size(), TITLE_ANIM_MAX_MARKERS);
    auto earlier = [forward](double x, int pass, const Hit& h){
        if (x != h.x) return forward ? x < h.x : x > h.x;
        return forward ? pass < h.pass : pass > h.pass;
    };
    for (int k = firstPass; k <= lastPass; k++){
        const Pass p = passAt(k, d, l, wrap);
        for (size_t i = 0; i < count; i++){
            double x = 0.0;
            if (!positionIn(p, animation.markers[i].time, d, l, x)) continue;
            bool crossed = forward ? (x > from && x <= to) : (x >= to && x < from);
            // a start fires what sits exactly at it, in the pass it starts in
            if (!crossed && includeStart && x == from && k == startPass) crossed = true;
            if (crossed && (!best[i].set || earlier(x, k, best[i]))){
                best[i].x = x;
                best[i].pass = k;
                best[i].set = true;
            }
        }
    }
    uint8_t order[TITLE_ANIM_MAX_MARKERS];
    size_t n = 0;
    for (size_t i = 0; i < count; i++) if (best[i].set) order[n++] = (uint8_t)i;
    // insertion sort by firing order (ties: the marker's index)
    for (size_t a = 1; a < n; a++){
        const uint8_t v = order[a];
        size_t b = a;
        while (b > 0){
            const Hit& prev = best[order[b - 1]];
            const bool before = earlier(best[v].x, best[v].pass, prev) ||
                                (best[v].x == prev.x && best[v].pass == prev.pass && v < order[b - 1]);
            if (!before) break;
            order[b] = order[b - 1];
            b--;
        }
        order[b] = v;
    }
    for (size_t a = 0; a < n; a++) out.push_back({order[a], best[order[a]].x});
}

// ==================================================================== the title's draw values

void titleanim::accumulateTitlePose(const CompiledTitleAnimation& animation, const TitleAnimPose& pose, bool forceRelative,
                                    const double* liveRest, TitlePoseSummary& s){
    if (pose.values.size() < animation.poseSize) return;
    for (const CompiledTitleTrack& track : animation.tracks){
        if (track.target != TrackTarget::Title || track.mute || track.orphan || track.titleField == TitleField::None) continue;
        const double* v = pose.values.data() + track.poseOffset;
        const bool relative = forceRelative || track.combine != TrackCombine::Replace;
        const VirtualFieldInfo info = titleFieldInfo(track.titleField);
        const bool multiply = info.relativeCombine == TrackCombine::Multiply;
        auto blend = [&](double& dst, double value, size_t component){
            (void)component;
            if (!relative) dst = value;
            else if (multiply) dst *= value;
            else dst += value;
        };
        const int c = track.component;
        auto component = [&](double* dst, size_t width){
            if (c >= 0){
                if ((size_t)c < width) blend(dst[c], v[0], (size_t)c);
                return;
            }
            for (size_t i = 0; i < width; i++) blend(dst[i], v[i], i);
        };
        switch (track.titleField){
            case TitleField::Position: component(s.position, 2); s.transformed = true; break;
            case TitleField::Rotation: blend(s.rotation, v[0], 0); s.transformed = true; break;
            case TitleField::Scale: blend(s.scale, v[0], 0); s.transformed = true; break;
            case TitleField::Stretch: component(s.stretch, 2); s.transformed = true; break;
            case TitleField::Pivot: component(s.pivot, 2); s.transformed = true; break;
            case TitleField::Opacity: blend(s.opacity, v[0], 0); s.transformed = true; break;
            case TitleField::FlashColor: component(s.flashColor, 3); s.transformed = true; break;
            case TitleField::FlashAmount: blend(s.flashAmount, v[0], 0); s.transformed = true; break;
            case TitleField::TintColor:{
                const size_t f = (size_t)TitleField::TintColor;
                if (!s.liveSet[f]){
                    // the instance's colour is the rest of a relative colour track
                    for (size_t i = 0; i < 3; i++) s.tintColor[i] = liveRest ? liveRest[f] : 1.0;
                }
                component(s.tintColor, 3);
                s.liveSet[f] = true;
                break;
            }
            default:{
                const size_t f = (size_t)track.titleField;
                if (!s.liveSet[f]) s.live[f] = liveRest ? liveRest[f] : (double)info.rest[0];
                blend(s.live[f], v[0], 0);
                s.liveSet[f] = true;
                break;
            }
        }
    }
}

TrackPivot titleanim::titlePivotOf(const CompiledTitleAnimation& animation){
    for (const CompiledTitleTrack& t : animation.tracks){
        if (t.target != TrackTarget::Title || t.mute || t.orphan) continue;
        if ((t.titleField == TitleField::Scale || t.titleField == TitleField::Stretch || t.titleField == TitleField::Rotation) &&
                t.pivot != TrackPivot::Origin && t.pivot != TrackPivot::Baseline)
            return t.pivot;
    }
    return TrackPivot::Origin;
}

FrameAffine FrameAffine::inverse() const{
    const double det = a * e - b * d;
    FrameAffine r;
    if (std::fabs(det) < 1e-12){
        // a collapsed title (scale 0): every point maps outside the source
        r.a = 0.0; r.b = 0.0; r.c = -1.0; r.d = 0.0; r.e = 0.0; r.f = -1.0;
        return r;
    }
    const double inv = 1.0 / det;
    r.a = e * inv;
    r.b = -b * inv;
    r.d = -d * inv;
    r.e = a * inv;
    r.c = -(r.a * c + r.b * f);
    r.f = -(r.d * c + r.e * f);
    return r;
}

FrameAffine FrameAffine::then(const FrameAffine& n) const{
    FrameAffine r;
    r.a = n.a * a + n.b * d;
    r.b = n.a * b + n.b * e;
    r.c = n.a * c + n.b * f + n.c;
    r.d = n.d * a + n.e * d;
    r.e = n.d * b + n.e * e;
    r.f = n.d * c + n.e * f + n.f;
    return r;
}

void FrameAffine::apply(double x, double y, double& ox, double& oy) const{
    ox = a * x + b * y + c;
    oy = d * x + e * y + f;
}

bool FrameAffine::identity() const{
    return a == 1.0 && b == 0.0 && c == 0.0 && d == 0.0 && e == 1.0 && f == 0.0;
}

FrameAffine titleanim::titleFrameAffine(const TitlePoseSummary& s, const TitleFrameGeometry& g, TrackPivot pivot){
    const double w = std::max(1e-6, g.frameWidth), h = std::max(1e-6, g.frameHeight);
    // the pivot in frame pixels (y down)
    const double inkL = g.inkX - g.frameX, inkT = g.inkY - g.frameY;
    const bool ink = g.inkWidth > 0.0 && g.inkHeight > 0.0;
    double px = w * 0.5, py = h * 0.5;
    switch (pivot){
        case TrackPivot::Center: if (ink){ px = inkL + g.inkWidth * 0.5; py = inkT + g.inkHeight * 0.5; } break;
        case TrackPivot::Bottom: if (ink){ px = inkL + g.inkWidth * 0.5; py = inkT + g.inkHeight; } break;
        case TrackPivot::Top: if (ink){ px = inkL + g.inkWidth * 0.5; py = inkT; } break;
        case TrackPivot::Left: if (ink){ px = inkL; py = inkT + g.inkHeight * 0.5; } break;
        case TrackPivot::Right: if (ink){ px = inkL + g.inkWidth; py = inkT + g.inkHeight * 0.5; } break;
        default: break;
    }
    px += s.pivot[0];
    py -= s.pivot[1];
    const double radians = s.rotation * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(radians), sn = std::sin(radians);
    const double sx = s.scale * s.stretch[0], sy = s.scale * s.stretch[1];
    // pixels: p' = P + T + R S (p - P), clockwise on screen with y down
    const double m00 = cs * sx, m01 = -sn * sy, m10 = sn * sx, m11 = cs * sy;
    const double tx = px + s.position[0] - (m00 * px + m01 * py);
    const double ty = py - s.position[1] - (m10 * px + m11 * py);
    FrameAffine r;
    r.a = m00;
    r.b = m01 * h / w;
    r.c = tx / w;
    r.d = m10 * w / h;
    r.e = m11;
    r.f = ty / h;
    return r;
}

FrameAffine titleanim::flipbookFrameAffine(const TitleFrameGeometry& still, double x, double y, double width, double height){
    const double w = std::max(1e-6, still.frameWidth), h = std::max(1e-6, still.frameHeight);
    FrameAffine r;
    r.a = width / w;
    r.b = 0.0;
    r.c = (x - still.frameX) / w;
    r.d = 0.0;
    r.e = height / h;
    r.f = (y - still.frameY) / h;
    return r;
}
