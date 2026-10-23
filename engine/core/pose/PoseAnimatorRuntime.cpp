// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseAnimatorRuntime.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    float positiveMod(float value, float length){
        float r = std::fmod(value, length);
        if (r < 0.0f) r += length;
        // fmod of a value just below a multiple of length can round up to length itself
        if (r >= length) r = 0.0f;
        return r;
    }

    float smootherstep01(float x){
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    }

}

bool pose::advancePlayback(float& time, bool& playing, float dt, float speed, float length, bool loop){
    if (!playing) return false;
    if (length <= 0.0f){
        // a static pose: a one-shot is over at once
        time = 0.0f;
        if (!loop){
            playing = false;
            return true;
        }
        return false;
    }

    time += dt * speed;
    if (loop){
        time = positiveMod(time, length);
        return false;
    }
    if (speed >= 0.0f && time >= length){
        time = length;
        playing = false;
        return true;
    }
    if (speed < 0.0f && time <= 0.0f){
        time = 0.0f;
        playing = false;
        return true;
    }
    return false;
}

float pose::playbackTime(float time, float length, bool loop){
    if (length <= 0.0f) return 0.0f;
    if (loop) return positiveMod(time, length);
    return std::clamp(time, 0.0f, length);
}

Quaternion pose::slerpShortest(const Quaternion& a, const Quaternion& b, float t){
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    double dot = (double)a.w * b.w + (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
    double bw = b.w, bx = b.x, by = b.y, bz = b.z;
    if (dot < 0.0){
        dot = -dot; bw = -bw; bx = -bx; by = -by; bz = -bz;
    }
    double k0, k1;
    if (dot > 0.9995){
        // nearly equal: lerp, normalised below
        k0 = 1.0 - t;
        k1 = t;
    }else{
        const double theta = std::acos(std::min(dot, 1.0));
        const double s = std::sin(theta);
        k0 = std::sin((1.0 - t) * theta) / s;
        k1 = std::sin(t * theta) / s;
    }
    double w = k0 * a.w + k1 * bw, x = k0 * a.x + k1 * bx, y = k0 * a.y + k1 * by, z = k0 * a.z + k1 * bz;
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n <= 1e-12) return a;
    return Quaternion((float)(w / n), (float)(x / n), (float)(y / n), (float)(z / n));
}

void pose::sampleFrames(const std::vector<HumanPose>& frames, float length, bool loop, bool stepped,
                        float time, HumanPose& out){
    if (frames.empty()) return;
    const size_t last = frames.size() - 1;
    if (last == 0 || length <= 0.0f){
        out = frames[0];
        return;
    }

    const float t = playbackTime(time, length, loop);
    const double u = (double)t / (double)length * (double)last;
    size_t f0 = (size_t)std::floor(u + 1e-6);
    if (f0 >= last){
        out = frames[last];
        return;
    }
    const float a = (float)(u - (double)f0);
    if (stepped || a <= 1e-6f){
        out = frames[f0];
        return;
    }

    const HumanPose& p0 = frames[f0];
    const HumanPose& p1 = frames[f0 + 1];
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        out.rotations[b] = slerpShortest(p0.rotations[b], p1.rotations[b], a);
    }
    out.hipsPosition = p0.hipsPosition + (p1.hipsPosition - p0.hipsPosition) * a;
}

Quaternion PoseBlendBone::blendRotation(const Quaternion& current, const Quaternion& pose, float weight){
    // anything but our own last value is what the clips (or the rest pose) put under us
    if (!rotationActive || current != writtenRotation){
        baseRotation = current;
    }
    rotationActive = true;
    writtenRotation = slerpShortest(baseRotation, pose, std::clamp(weight, 0.0f, 1.0f));
    return writtenRotation;
}

Vector3 PoseBlendBone::blendPosition(const Vector3& current, const Vector3& pose, float weight){
    if (!positionActive || current != writtenPosition){
        basePosition = current;
    }
    positionActive = true;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    if (w <= 0.0f) writtenPosition = basePosition;
    else if (w >= 1.0f) writtenPosition = pose;
    else writtenPosition = basePosition + (pose - basePosition) * w;
    return writtenPosition;
}

int PoseAnimatorRuntime::findPose(const std::string& name) const{
    if (!dataLoaded) return -1;
    for (size_t i = 0; i < data.poses.size(); i++){
        if (data.poses[i].name == name) return (int)i;
    }
    return -1;
}

void PoseAnimatorRuntime::samplePlayback(float time, HumanPose& out) const{
    sampleFrames(frames, length, data.settings.loop, stepped(), time, out);
}

bool PoseAnimatorRuntime::startTransition(const PoseGoTo& request){
    if (!isLoaded() || request.key < 0 || request.key >= (int)keys.size()) return false;

    Transition tr = data.settings.transition;
    if (request.duration >= 0.0f) tr.duration = request.duration;
    if (request.overshoot >= 0.0f) tr.overshoot = request.overshoot;
    if (request.anticipation >= 0.0f) tr.anticipation = request.anticipation;
    tr.duration = std::clamp(tr.duration, 0.02f, 1.0f);
    tr.overshoot = std::clamp(tr.overshoot, 0.0f, 0.9f);
    tr.anticipation = std::clamp(tr.anticipation, 0.0f, 0.5f);

    transitionFrom = hasDisplayed ? displayed : keys[0];
    transition = tr;
    transitionKey = request.key;
    transitionTime = 0.0f;
    transitionActive = true;
    return true;
}

void PoseAnimatorRuntime::sampleTransition(float dt, HumanPose& out){
    if (!transitionActive || transitionKey < 0 || transitionKey >= (int)keys.size()) return;
    transitionTime += std::max(dt, 0.0f);

    const HumanPose& to = keys[transitionKey];
    const float e = smootherstep01(transitionTime / std::max(transition.duration, 1e-4f));
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        out.rotations[b] = slerpShortest(transitionFrom.rotations[b], to.rotations[b], e);
    }
    out.hipsPosition = transitionFrom.hipsPosition + (to.hipsPosition - transitionFrom.hipsPosition) * e;
}
