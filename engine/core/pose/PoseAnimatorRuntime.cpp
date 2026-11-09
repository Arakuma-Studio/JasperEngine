// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseAnimatorRuntime.h"

#include "pose/PoseMath.h"
#include "pose/PoseRig.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"
#include "animation/KeyframeSampling.h"

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

    // the x (in durations) after which the ease stays within 1e-4 of 1
    float settleX(const Transition& transition){
        if (transition.curve != TransitionCurve::Spring) return 1.0f;
        const float step = 0.02f;
        float last = 1.0f;
        for (float x = 1.0f; x <= 60.0f; x += step){
            if (std::abs(ease(x, transition) - 1.0f) >= 1e-4f) last = x;
        }
        return last + step;
    }

    // the transition of bone `b` (overlap delay, looseness)
    void boneTransition(const PoseAnimationSettings& settings, const Transition& base, int b, float& delay, Transition& out){
        const HumanBoneInfo& info = humanBoneInfo(boneAt(b));
        const float w = settings.weights[static_cast<int>(info.group)];
        delay = settings.overlap * w * info.depth;
        const float l = std::clamp(settings.looseness * w * info.depth, 0.0f, 1.0f);
        out = l > 0.0f ? loosen(base, l) : base;
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

PoseBuildCache& PoseBuildCache::shared(){
    static PoseBuildCache cache;
    return cache;
}

std::shared_ptr<const PoseAnimationBuild> PoseBuildCache::build(const PoseAnimationData& data, const PoseRig& rig, std::string* error){
    if (data.poses.empty()){
        if (error) *error = "the animation has no poses";
        return nullptr;
    }
    auto result = std::make_shared<PoseAnimationBuild>();
    result->keys = realiseKeys(data, rig, nullptr);
    const SamplerInput input = makeSamplerInput(data, result->keys, &rig);
    result->length = (float)input.timeline.length;
    result->frames = pose::frames(input, data.settings.frameRate);
    if (result->frames.empty()){
        if (error) *error = "the animation has no frames";
        return nullptr;
    }
    return result;
}

void PoseBuildCache::pruneLocked(){
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry){
        return entry.rig.expired() || entry.build.expired();
    }), entries.end());
}

std::shared_ptr<const PoseAnimationBuild> PoseBuildCache::findLocked(const std::string& path, const std::string& source,
                                                                     const std::shared_ptr<const PoseRig>& rig) const{
    for (const Entry& entry : entries){
        if (entry.path != path || entry.source != source || entry.rig.lock() != rig) continue;
        if (std::shared_ptr<const PoseAnimationBuild> found = entry.build.lock()) return found;
    }
    return nullptr;
}

std::shared_ptr<const PoseAnimationBuild> PoseBuildCache::get(const std::string& path, const std::string& source,
                                                              const std::shared_ptr<const PoseRig>& rig, const PoseAnimationData& data,
                                                              std::string* error){
    if (!rig){
        if (error) *error = "the rig is not valid";
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        pruneLocked();
        if (std::shared_ptr<const PoseAnimationBuild> found = findLocked(path, source, rig)) return found;
    }
    // built without the lock: realising and sampling take a while
    std::shared_ptr<const PoseAnimationBuild> made = build(data, *rig, error);
    if (!made) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    // another thread built the same meanwhile: theirs is shared
    if (std::shared_ptr<const PoseAnimationBuild> found = findLocked(path, source, rig)) return found;
    entries.push_back({path, source, rig, made});
    return made;
}

void PoseBuildCache::prune(){
    std::lock_guard<std::mutex> lock(mutex);
    pruneLocked();
}

void PoseBuildCache::clear(){
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}



const std::vector<HumanPose>& PoseAnimatorRuntime::keys() const{
    static const std::vector<HumanPose> none;
    return built ? built->keys : none;
}

const std::vector<HumanPose>& PoseAnimatorRuntime::frames() const{
    static const std::vector<HumanPose> none;
    return built ? built->frames : none;
}

bool PoseAnimatorRuntime::build(std::shared_ptr<const PoseRig> rigToUse, std::string* error, PoseBuildCache* cache){
    keyBoundaries.clear();
    if (!rigToUse || !rigToUse->valid()){
        if (error) *error = "the rig is not valid";
        return false;
    }
    std::shared_ptr<const PoseAnimationBuild> made = cache && !source.empty() ? cache->get(path, source, rigToUse, data, error)
                                                                              : PoseBuildCache::build(data, *rigToUse, error);
    if (!made) return false;
    rig = rigToUse;
    built = made;
    length = made->length;
    hasDisplayed = false;
    transitionActive = false;
    return true;
}

int PoseAnimatorRuntime::findPose(const std::string& name) const{
    if (!dataLoaded) return -1;
    for (size_t i = 0; i < data.poses.size(); i++){
        if (data.poses[i].name == name) return (int)i;
    }
    return -1;
}

void PoseAnimatorRuntime::samplePlayback(float time, HumanPose& out, float poseFrameRate) const{
    if (poseFrameRate > 0.0f && std::isfinite(poseFrameRate) && length > 0.0f){
        if (keyBoundaries.empty()){
            const PoseTimeline timeline = buildTimeline(data);
            keyBoundaries.push_back(0.0f);
            for (size_t s = 0; s < timeline.segmentCount(); s++){
                keyBoundaries.push_back((float)timeline.start[s]);
                keyBoundaries.push_back((float)(timeline.start[s] + std::max(0.0, (double)timeline.transitions[s].duration)));
            }
            std::sort(keyBoundaries.begin(), keyBoundaries.end());
        }
        const float t = playbackTime(time, length, data.settings.loop);
        sampleFrames(frames(), length, data.settings.loop, stepped(),
                     anim::holdPoseTime(keyBoundaries.data(), keyBoundaries.size(), t, poseFrameRate), out);
        return;
    }
    sampleFrames(frames(), length, data.settings.loop, stepped(), time, out);
}

bool PoseAnimatorRuntime::startTransition(const PoseGoTo& request){
    if (!isLoaded() || request.key < 0 || request.key >= (int)keys().size()) return false;

    Transition tr = data.settings.transition;
    if (request.duration >= 0.0f) tr.duration = request.duration;
    if (request.overshoot >= 0.0f) tr.overshoot = request.overshoot;
    if (request.anticipation >= 0.0f) tr.anticipation = request.anticipation;
    tr.duration = std::clamp(tr.duration, 0.02f, 1.0f);
    tr.overshoot = std::clamp(tr.overshoot, 0.0f, 0.9f);
    tr.anticipation = std::clamp(tr.anticipation, 0.0f, 0.5f);

    transitionFrom = hasDisplayed ? displayed : frames()[0];
    // the frames' ground rule (4.8): a start on the floor (up to 1e-3 humanScale above it)
    // counts as grounded
    transitionFromGrounded = !rig->valid() ||
        rig->lowestPointPrecise(transitionFrom, rig->skin()) - rig->floorHeight() <= 1e-3 * rig->humanScale();
    transition = tr;
    transitionKey = request.key;
    transitionTime = 0.0f;
    transitionActive = true;
    transitionArrived = false;
    transitionPaused = false;
    transitionSettle = transitionSettleTime();
    return true;
}

void PoseAnimatorRuntime::keepGoTo(const PoseAnimatorRuntime& previous){
    if (previous.pending.key >= 0){
        pending = previous.pending;
        pendingArrived = previous.pendingArrived;
    }else if (previous.transitionActive && previous.transitionKey >= 0){
        pending.key = previous.transitionKey;
        pending.duration = previous.transition.duration;
        pending.overshoot = previous.transition.overshoot;
        pending.anticipation = previous.transition.anticipation;
        pendingArrived = previous.transitionArrived;
    }
}

bool PoseAnimatorRuntime::startPending(){
    const bool started = pending.key >= 0 && startTransition(pending);
    if (started && pendingArrived){
        transitionArrived = true;
    }
    pending = PoseGoTo();
    pendingArrived = false;
    return started;
}

float PoseAnimatorRuntime::transitionSettleTime() const{
    // Left/right pairs and bones with zero looseness share the same spring.
    // Its expensive settling scan depends only on overshoot and anticipation;
    // duration and overlap are applied below. Keep the cache within this call
    // so edited settings never reuse an earlier transition's result.
    struct SpringSettle {
        float overshoot;
        float anticipation;
        float x;
    };
    std::array<SpringSettle, HUMAN_BONE_COUNT> springs;
    size_t springCount = 0;
    float settle = 0.0f;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        float delay = 0.0f;
        Transition tr;
        boneTransition(data.settings, transition, b, delay, tr);
        float x = 1.0f;
        if (tr.curve == TransitionCurve::Spring){
            size_t i = 0;
            while (i < springCount && !(springs[i].overshoot == tr.overshoot &&
                                       springs[i].anticipation == tr.anticipation)){
                i++;
            }
            if (i == springCount){
                springs[springCount++] = {tr.overshoot, tr.anticipation, settleX(tr)};
            }
            x = springs[i].x;
        }
        settle = std::max(settle, delay + x * std::max(tr.duration, 1e-4f));
    }
    return settle;
}

void PoseAnimatorRuntime::transitionPose(float time, HumanPose& out) const{
    if (transitionKey < 0 || transitionKey >= (int)keys().size()) return;
    const HumanPose& to = keys()[transitionKey];
    float t = std::max(time, 0.0f);
    if (data.settings.stepRate > 0.0f){
        t = std::floor(t * data.settings.stepRate + 1e-4f) / data.settings.stepRate;
    }else if (poseFrameRate > 0.0f && std::isfinite(poseFrameRate)){
        // S48 Pose Frame Rate: the blend steps on the same ticks as the poses
        t = anim::holdStepTime(t, poseFrameRate);
    }
    float hipsEase = 1.0f;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        float delay = 0.0f;
        Transition tr;
        boneTransition(data.settings, transition, b, delay, tr);
        const float e = ease((t - delay) / std::max(tr.duration, 1e-4f), tr);
        out.rotations[b] = slerpUnclamped(transitionFrom.rotations[b], to.rotations[b], e);
        if (b == boneIndex(HumanBone::Hips)){
            out.hipsPosition = transitionFrom.hipsPosition + (to.hipsPosition - transitionFrom.hipsPosition) * e;
            hipsEase = e;
        }
    }
    // as the frames: the joint limits, then the ground correction (a grounded start and key stay
    // on the floor; a launch or landing fades with the hips' ease and is only pushed out of it)
    if (rig && rig->valid()){
        if (data.settings.humanize.jointLimits){
            clampPoseToLimits(*rig, out);
        }
        const bool toGrounded = transitionKey < (int)data.poses.size() && data.poses[transitionKey].grounded;
        groundToWeight(*rig, out, transitionGroundWeight(transitionFromGrounded, toGrounded, hipsEase));
    }
}

void PoseAnimatorRuntime::sampleTransition(float dt, HumanPose& out){
    if (!transitionActive || transitionKey < 0 || transitionKey >= (int)keys().size()) return;
    if (transitionArrived){
        out = keys()[transitionKey];
        return;
    }
    transitionTime += std::max(dt, 0.0f);
    if (transitionTime >= transitionSettle){
        transitionArrived = true;
        out = keys()[transitionKey];
        return;
    }
    transitionPose(transitionTime, out);
}
