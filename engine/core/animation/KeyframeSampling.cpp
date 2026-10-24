// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "KeyframeSampling.h"

#include "math/Interpolation.h"

#include <cfloat>
#include <algorithm>
#include <cmath>

using namespace jasperengine;

float anim::applyEase(EaseType ease, float t){
    // the Ease functions themselves (Ease::getFunction would wrap them in a std::function)
    switch (ease){
        case EaseType::LINEAR: return Ease::linear(t);
        case EaseType::QUAD_IN: return Ease::easeInQuad(t);
        case EaseType::QUAD_OUT: return Ease::easeOutQuad(t);
        case EaseType::QUAD_IN_OUT: return Ease::easeInOutQuad(t);
        case EaseType::CUBIC_IN: return Ease::easeInCubic(t);
        case EaseType::CUBIC_OUT: return Ease::easeOutCubic(t);
        case EaseType::CUBIC_IN_OUT: return Ease::easeInOutCubic(t);
        case EaseType::QUART_IN: return Ease::easeInQuart(t);
        case EaseType::QUART_OUT: return Ease::easeOutQuart(t);
        case EaseType::QUART_IN_OUT: return Ease::easeInOutQuart(t);
        case EaseType::QUINT_IN: return Ease::easeInQuint(t);
        case EaseType::QUINT_OUT: return Ease::easeOutQuint(t);
        case EaseType::QUINT_IN_OUT: return Ease::easeInOutQuint(t);
        case EaseType::SINE_IN: return Ease::easeInSine(t);
        case EaseType::SINE_OUT: return Ease::easeOutSine(t);
        case EaseType::SINE_IN_OUT: return Ease::easeInOutSine(t);
        case EaseType::EXPO_IN: return Ease::easeInExpo(t);
        case EaseType::EXPO_OUT: return Ease::easeOutExpo(t);
        case EaseType::EXPO_IN_OUT: return Ease::easeInOutExpo(t);
        case EaseType::CIRC_IN: return Ease::easeInCirc(t);
        case EaseType::CIRC_OUT: return Ease::easeOutCirc(t);
        case EaseType::CIRC_IN_OUT: return Ease::easeInOutCirc(t);
        case EaseType::ELASTIC_IN: return Ease::easeInElastic(t);
        case EaseType::ELASTIC_OUT: return Ease::easeOutElastic(t);
        case EaseType::ELASTIC_IN_OUT: return Ease::easeInOutElastic(t);
        case EaseType::BACK_IN: return Ease::easeInBack(t);
        case EaseType::BACK_OUT: return Ease::easeOutBack(t);
        case EaseType::BACK_IN_OUT: return Ease::easeInOutBack(t);
        case EaseType::BOUNCE_IN: return Ease::easeInBounce(t);
        case EaseType::BOUNCE_OUT: return Ease::easeOutBounce(t);
        case EaseType::BOUNCE_IN_OUT: return Ease::easeInOutBounce(t);
        case EaseType::STEP: return Ease::stepped(t);
        case EaseType::CUSTOM: return t;
    }
    return t;
}

anim::KeyLocation anim::locateKey(const float* times, size_t keyCount, const EaseType* easings, size_t easingCount,
                                  float time, KeyCursor* cursor){
    KeyLocation at;
    if (!times || keyCount == 0) return at;

    // the first key whose time is >= `time`, the last one past the end
    size_t index = 0;
    if (cursor){
        // sorted times (clips): walk a few keys from the hint (playback moves little per
        // frame), else binary search; either way the hint only changes the cost
        if (std::isnan(time)){
            // what the linear scan gives (no time compares as later)
            index = 0;
        }else{
            const size_t walk = 4;
            index = cursor->index < keyCount ? cursor->index : keyCount - 1;
            size_t steps = 0;
            while (steps < walk && index > 0 && times[index - 1] >= time){ index--; steps++; }
            while (steps < walk && index < keyCount - 1 && times[index] < time){ index++; steps++; }
            bool settled = (index == 0 || times[index - 1] < time) && (index == keyCount - 1 || times[index] >= time);
            if (!settled){
                index = std::lower_bound(times, times + keyCount - 1, time) - times;
            }
        }
        cursor->index = index;
    }else{
        // ActionSystem's linear scan (entity tracks' times are not guaranteed sorted)
        while (index < keyCount - 1 && times[index] < time) index++;
    }
    at.index = index;

    float previousTime = 0;
    float nextTime = times[index];
    if (index > 0){
        previousTime = times[index - 1];
        at.segmentDuration = nextTime - previousTime;
    }
    if (nextTime > previousTime){
        at.interpolation = (time - previousTime) / (nextTime - previousTime);
    }
    if (at.interpolation > 1){
        at.interpolation = 1;
    }

    // easings[i] shapes the segment from key i to key i+1
    if (index > 0){
        size_t segment = index - 1;
        if (easings && segment < easingCount && easings[segment] != EaseType::LINEAR){
            at.interpolation = applyEase(easings[segment], at.interpolation);
        }
    }
    return at;
}

Vector3 anim::sampleVector3(const KeyLocation& at, const Vector3* values, const Vector3* inTangents, const Vector3* outTangents){
    Vector3 previous = values[(at.index > 0) ? at.index - 1 : 0];

    if (at.index > 0 && inTangents && outTangents){
        return Interpolation::cubicSpline(at.interpolation, at.segmentDuration,
                previous, outTangents[at.index - 1],
                values[at.index], inTangents[at.index]);
    }

    return previous + at.interpolation * (values[at.index] - previous);
}

Quaternion anim::sampleQuaternion(const KeyLocation& at, const Quaternion* values, const Quaternion* inTangents, const Quaternion* outTangents){
    Quaternion previous = values[(at.index > 0) ? at.index - 1 : 0];

    Quaternion value;
    if (at.index > 0 && inTangents && outTangents){
        value = Interpolation::cubicSpline(at.interpolation, at.segmentDuration,
                previous, outTangents[at.index - 1],
                values[at.index], inTangents[at.index]);
        // the interpolated quaternion must be normalized; the all-zero result (v_k == -v_k+1)
        // would spread NaN, so it takes the nearer key instead
        if (value.norm() > FLT_EPSILON){
            value.normalize();
        }else{
            value = (at.interpolation < 0.5f) ? previous : values[at.index];
        }
    }else{
        value = Quaternion::slerp(at.interpolation, previous, values[at.index]);
    }
    return value;
}

void anim::sampleFloatRow(const KeyLocation& at, const float* previous, const float* next, size_t width,
                          const float* previousOut, const float* nextIn, float* out){
    bool cubic = at.index > 0 && previousOut && nextIn;
    for (size_t i = 0; i < width; i++){
        if (cubic){
            out[i] = Interpolation::cubicSpline(at.interpolation, at.segmentDuration,
                    previous[i], previousOut[i], next[i], nextIn[i]);
        }else{
            out[i] = previous[i] + at.interpolation * (next[i] - previous[i]);
        }
    }
}

void anim::sampleVector3Row(const KeyLocation& at, const Vector3* previous, const Vector3* next, size_t width,
                            const Vector3* previousOut, const Vector3* nextIn, Vector3* out){
    bool cubic = at.index > 0 && previousOut && nextIn;
    for (size_t i = 0; i < width; i++){
        if (cubic){
            out[i] = Interpolation::cubicSpline(at.interpolation, at.segmentDuration,
                    previous[i], previousOut[i], next[i], nextIn[i]);
        }else{
            out[i] = previous[i] + at.interpolation * (next[i] - previous[i]);
        }
    }
}

float anim::wrapClipTime(float time, float length, ClipWrapMode wrap){
    // TODO(T1b)
    (void)length;
    (void)wrap;
    return time;
}

float anim::trackTime(const AnimationClip::Track& track, float clipTime){
    // a looping track starts over after its last key, keeping the overshoot (KeyframeTracks)
    if (track.loop && track.duration > 0 && clipTime >= track.duration){
        return std::fmod(clipTime, track.duration);
    }
    return clipTime;
}

bool anim::sampleTrack(const AnimationClip::Track& track, float clipTime, TrackSample& out, KeyCursor* cursor){
    out.channel = track.channel;
    out.valid = false;
    if (!track.isSampled() || !track.times){
        out.weights.clear();
        out.offsets.clear();
        return false;
    }

    float time = trackTime(track, clipTime);
    size_t last = track.keyCount - 1;

    // At or before the first key and at or after the last one the sample is exactly that key:
    // `base` shifts the arrays so that a location at index 0 reads it
    size_t base = 0;
    KeyLocation at;
    // without a hint the search is binary (clip times are sorted), never a scan from key 0
    KeyCursor local;
    if (!cursor) cursor = &local;
    if (std::isnan(time) || time <= track.times[0]){
        cursor->index = 0;
    }else if (time >= track.times[last]){
        base = last;
        cursor->index = last;
    }else{
        at = locateKey(track.times, track.keyCount, track.easings, track.keyCount, time, cursor);
    }
    // key rows of the segment (at index 0 both are the exact key)
    size_t previous = base + (at.index > 0 ? at.index - 1 : 0);
    size_t next = base + at.index;
    bool cubic = at.index > 0 && track.hasTangents();

    size_t width = track.width;
    switch (track.channel){
        case ClipChannel::Position:
        case ClipChannel::Scale:
            out.vector = sampleVector3(at, track.vectors + base,
                                       cubic ? track.inVectors + base : nullptr,
                                       cubic ? track.outVectors + base : nullptr);
            break;
        case ClipChannel::Rotation:
            out.rotation = sampleQuaternion(at, track.rotations + base,
                                            cubic ? track.inRotations + base : nullptr,
                                            cubic ? track.outRotations + base : nullptr);
            break;
        case ClipChannel::Morph:
            out.weights.resize(width);
            sampleFloatRow(at, track.weights + previous * width, track.weights + next * width, width,
                           cubic ? track.outWeights + previous * width : nullptr,
                           cubic ? track.inWeights + next * width : nullptr,
                           out.weights.data());
            break;
        case ClipChannel::Lattice:
            out.offsets.resize(width);
            sampleVector3Row(at, track.vectors + previous * width, track.vectors + next * width, width,
                             cubic ? track.outVectors + previous * width : nullptr,
                             cubic ? track.inVectors + next * width : nullptr,
                             out.offsets.data());
            break;
        case ClipChannel::Property:
            return false;
    }
    if (track.channel != ClipChannel::Morph) out.weights.clear();
    if (track.channel != ClipChannel::Lattice) out.offsets.clear();
    out.valid = true;
    return true;
}

void anim::sampleClipTracks(const AnimationClip& clip, float clipTime, std::vector<TrackSample>& out){
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    out.resize(tracks.size());
    for (size_t i = 0; i < tracks.size(); i++){
        sampleTrack(tracks[i], clipTime, out[i]);
    }
}

void anim::collectCrossedEvents(const AnimationClip& clip, ClipWrapMode wrap, float fromTime, float toTime,
                                bool includeStart, std::vector<size_t>& out){
    // TODO(T1b)
    (void)clip;
    (void)wrap;
    (void)fromTime;
    (void)toTime;
    (void)includeStart;
    (void)out;
}
