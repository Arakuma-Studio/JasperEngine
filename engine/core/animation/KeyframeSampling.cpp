// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "KeyframeSampling.h"

using namespace jasperengine;

float anim::applyEase(EaseType ease, float t){
    // TODO(T1a)
    (void)ease;
    return t;
}

anim::KeyLocation anim::locateKey(const float* times, size_t keyCount, const EaseType* easings, size_t easingCount,
                                  float time, KeyCursor* cursor){
    // TODO(T1a): ActionSystem::keyframeUpdate's rule
    (void)times;
    (void)keyCount;
    (void)easings;
    (void)easingCount;
    (void)time;
    (void)cursor;
    return KeyLocation();
}

Vector3 anim::sampleVector3(const KeyLocation& at, const Vector3* values, const Vector3* inTangents, const Vector3* outTangents){
    // TODO(T1a)
    (void)at;
    (void)values;
    (void)inTangents;
    (void)outTangents;
    return Vector3(0, 0, 0);
}

Quaternion anim::sampleQuaternion(const KeyLocation& at, const Quaternion* values, const Quaternion* inTangents, const Quaternion* outTangents){
    // TODO(T1a)
    (void)at;
    (void)values;
    (void)inTangents;
    (void)outTangents;
    return Quaternion();
}

void anim::sampleFloatRow(const KeyLocation& at, const float* previous, const float* next, size_t width,
                          const float* previousOut, const float* nextIn, float* out){
    // TODO(T1a)
    (void)at;
    (void)previous;
    (void)next;
    (void)previousOut;
    (void)nextIn;
    for (size_t i = 0; i < width; i++) out[i] = 0.0f;
}

void anim::sampleVector3Row(const KeyLocation& at, const Vector3* previous, const Vector3* next, size_t width,
                            const Vector3* previousOut, const Vector3* nextIn, Vector3* out){
    // TODO(T1a)
    (void)at;
    (void)previous;
    (void)next;
    (void)previousOut;
    (void)nextIn;
    for (size_t i = 0; i < width; i++) out[i] = Vector3(0, 0, 0);
}

float anim::wrapClipTime(float time, float length, ClipWrapMode wrap){
    // TODO(T1b)
    (void)length;
    (void)wrap;
    return time;
}

float anim::trackTime(const AnimationClip::Track& track, float clipTime){
    // TODO(T1a)
    (void)track;
    return clipTime;
}

bool anim::sampleTrack(const AnimationClip::Track& track, float clipTime, TrackSample& out, KeyCursor* cursor){
    // TODO(T1a)
    (void)clipTime;
    (void)cursor;
    out = TrackSample();
    out.channel = track.channel;
    return false;
}

void anim::sampleClipTracks(const AnimationClip& clip, float clipTime, std::vector<TrackSample>& out){
    // TODO(T1a)
    out.clear();
    out.resize(clip.getTracks().size());
    for (size_t i = 0; i < out.size(); i++){
        sampleTrack(clip.getTracks()[i], clipTime, out[i]);
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
