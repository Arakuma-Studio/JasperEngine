// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimator.h"

#include "Scene.h"
#include "animation/VertexAnimatorRuntime.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "Log.h"

#include <cmath>

using namespace jasperengine;

namespace {

    // a negative index would address the GameObject itself
    bool validInstance(int instance){
        if (instance >= 0) return true;
        Log::warn("VertexAnimator: instance %d does not exist (instances count from 0)", instance);
        return false;
    }

    // the clip a state shows as current: the one faded to during a crossfade, A otherwise
    int currentClipOf(const vat::VatTrackPair& pair){
        if (pair.b.clip >= 0 && pair.blend < 0.0f) return pair.b.clip;
        return pair.a.clip;
    }

    const vat::VatTrack& currentTrackOf(const vat::VatTrackPair& pair){
        if (pair.b.clip >= 0 && pair.blend < 0.0f) return pair.b;
        return pair.a;
    }

    // the phase (0..1) of the current clip at `now`
    float normalizedOf(const vat::VertexAnimation& asset, const vat::VatTrackPair& pair, float now){
        const vat::VatTrack& track = currentTrackOf(pair);
        if (track.clip < 0 || track.clip >= (int)asset.getData().clips.size()) return 0.0f;
        return vat::vatSample(asset.getData().clips[(size_t)track.clip], vat::vatLocalTime(track, now)).normalized;
    }

}

VertexAnimator::VertexAnimator(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<VertexAnimatorComponent>(entity)){
        scene->addComponent<VertexAnimatorComponent>(entity, {});
    }
}

VertexAnimator::~VertexAnimator(){
}

VertexAnimator::VertexAnimator(const VertexAnimator& rhs) = default;
VertexAnimator& VertexAnimator::operator=(const VertexAnimator& rhs) = default;
VertexAnimator::VertexAnimator(VertexAnimator&& rhs) noexcept = default;
VertexAnimator& VertexAnimator::operator=(VertexAnimator&& rhs) noexcept = default;

void VertexAnimator::request(VertexAnimatorRequest request){
    getComponent<VertexAnimatorComponent>().requests.push_back(std::move(request));
    // applied now when the asset is there, so the next read sees it
    if (std::shared_ptr<VertexAnimatorSystem> system = scene->getSystem<VertexAnimatorSystem>()){
        system->applyRequests(entity);
    }
}

const vat::VatTrackPair* VertexAnimator::findState(int instance) const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (instance < 0) return &animator.state;
    if ((size_t)instance < animator.instanceStates.size()) return &animator.instanceStates[(size_t)instance];
    return nullptr;
}

// ---- the GameObject

void VertexAnimator::play(const std::string& name){
    play(name, 0.0f);
}

void VertexAnimator::play(const std::string& name, float fade){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Play;
    r.clip = name;
    r.value = std::isfinite(fade) ? fade : 0.0f;
    request(r);
}

void VertexAnimator::crossFade(const std::string& name){
    crossFade(name, getComponent<VertexAnimatorComponent>().defaultFadeTime);
}

void VertexAnimator::crossFade(const std::string& name, float fade){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::CrossFade;
    r.clip = name;
    r.value = std::isfinite(fade) ? fade : 0.0f;
    request(r);
}

void VertexAnimator::blend(const std::string& a, const std::string& b, float weight){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Blend;
    r.clip = a;
    r.clipB = b;
    r.value = weight;
    request(r);
}

void VertexAnimator::queue(const std::string& name){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Queue;
    r.clip = name;
    request(r);
}

void VertexAnimator::stop(){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Stop;
    request(r);
}

bool VertexAnimator::isPlaying() const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset){
        // not loaded yet: what it was asked to play
        for (auto it = animator.requests.rbegin(); it != animator.requests.rend(); ++it){
            if (it->instance >= 0 && !it->all) continue;
            return it->kind == VertexAnimatorRequest::Kind::Play || it->kind == VertexAnimatorRequest::Kind::CrossFade ||
                   it->kind == VertexAnimatorRequest::Kind::Queue || it->kind == VertexAnimatorRequest::Kind::Blend;
        }
        return false;
    }
    const vat::VatTrackPair& pair = animator.state;
    if (pair.a.clip < 0 || (animator.runtime && animator.runtime->entity.stopped)) return false;
    const int clip = currentClipOf(pair);
    const std::vector<vat::VatClip>& clips = animator.asset->getData().clips;
    if (clip < 0 || clip >= (int)clips.size()) return false;
    const vat::VatTrack& track = currentTrackOf(pair);
    return !vat::vatSample(clips[(size_t)clip], vat::vatLocalTime(track, animator.lastNow)).finished;
}

bool VertexAnimator::isPlaying(const std::string& name) const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset){
        for (auto it = animator.requests.rbegin(); it != animator.requests.rend(); ++it){
            if (it->instance >= 0 && !it->all) continue;
            if (it->kind == VertexAnimatorRequest::Kind::Stop) return false;
            if (it->clip == name || it->clipB == name) return true;
        }
        return false;
    }
    if (!isPlaying()) return false;
    const int index = animator.asset->findClip(name);
    if (index < 0) return false;
    const vat::VatTrackPair& pair = animator.state;
    return pair.a.clip == index || pair.b.clip == index;
}

void VertexAnimator::setTime(float time){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::SetTime;
    r.value = time;
    request(r);
}

float VertexAnimator::getTime() const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset) return 0.0f;
    const int clip = currentClipOf(animator.state);
    if (clip < 0 || clip >= (int)animator.asset->getData().clips.size()) return 0.0f;
    return normalizedOf(*animator.asset, animator.state, animator.lastNow) * animator.asset->clipLength(clip);
}

float VertexAnimator::getNormalizedTime() const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset) return 0.0f;
    return normalizedOf(*animator.asset, animator.state, animator.lastNow);
}

void VertexAnimator::setSpeed(float speed){
    // the system rewrites every track keeping its phase when it sees the new value
    getComponent<VertexAnimatorComponent>().speed = std::isfinite(speed) ? speed : 0.0f;
}

float VertexAnimator::getSpeed() const{
    return getComponent<VertexAnimatorComponent>().speed;
}

std::string VertexAnimator::getCurrentClip() const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset) return "";
    const int clip = currentClipOf(animator.state);
    const std::vector<vat::VatClip>& clips = animator.asset->getData().clips;
    return clip >= 0 && clip < (int)clips.size() ? clips[(size_t)clip].name : std::string();
}

std::vector<std::string> VertexAnimator::getClipNames() const{
    std::vector<std::string> names;
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (animator.asset){
        for (const vat::VatClip& clip : animator.asset->getData().clips) names.push_back(clip.name);
    }
    return names;
}

float VertexAnimator::getClipLength(const std::string& name) const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset) return 0.0f;
    const int index = animator.asset->findClip(name);
    return index >= 0 ? animator.asset->clipLength(index) : 0.0f;
}

VertexAnimatorSocketPose VertexAnimator::getSocketPose(const std::string& name) const{
    return getSocketPose(name, -1);
}

VertexAnimatorSocketPose VertexAnimator::getSocketPose(const std::string& name, int instance) const{
    VertexAnimatorSocketPose pose;
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    const vat::VatTrackPair* pair = findState(instance);
    if (!animator.asset || !pair) return pose;
    const int socket = animator.asset->getData().findSocket(name);
    if (socket < 0) return pose;
    pose.valid = animator.asset->sampleSocket(socket, *pair, animator.lastNow, pose.position, pose.rotation);
    return pose;
}

// ---- instances

void VertexAnimator::playInstance(int instance, const std::string& name){
    playInstance(instance, name, 0.0f);
}

void VertexAnimator::playInstance(int instance, const std::string& name, float fade){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Play;
    if (!validInstance(instance)) return;
    r.instance = instance;
    r.clip = name;
    r.value = std::isfinite(fade) ? fade : 0.0f;
    request(r);
}

void VertexAnimator::crossFadeInstance(int instance, const std::string& name, float fade){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::CrossFade;
    if (!validInstance(instance)) return;
    r.instance = instance;
    r.clip = name;
    r.value = std::isfinite(fade) ? fade : 0.0f;
    request(r);
}

void VertexAnimator::setInstanceSpeed(int instance, float speed){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::SetSpeed;
    if (!validInstance(instance)) return;
    r.instance = instance;
    r.value = speed;
    request(r);
}

void VertexAnimator::setInstanceTime(int instance, float time){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::SetTime;
    if (!validInstance(instance)) return;
    r.instance = instance;
    r.value = time;
    request(r);
}

std::string VertexAnimator::getInstanceClip(int instance) const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset || instance < 0) return "";
    const vat::VatTrackPair* pair = findState(instance);
    if (!pair) return "";
    const int clip = currentClipOf(*pair);
    const std::vector<vat::VatClip>& clips = animator.asset->getData().clips;
    return clip >= 0 && clip < (int)clips.size() ? clips[(size_t)clip].name : std::string();
}

float VertexAnimator::getInstanceNormalizedTime(int instance) const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (!animator.asset || instance < 0) return 0.0f;
    const vat::VatTrackPair* pair = findState(instance);
    return pair ? normalizedOf(*animator.asset, *pair, animator.lastNow) : 0.0f;
}

void VertexAnimator::playAll(const std::string& name){
    playAll(name, 0.0f);
}

void VertexAnimator::playAll(const std::string& name, float fade){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::Play;
    r.all = true;
    r.clip = name;
    r.value = std::isfinite(fade) ? fade : 0.0f;
    request(r);
}

void VertexAnimator::randomizeTimes(int seed){
    VertexAnimatorRequest r;
    r.kind = VertexAnimatorRequest::Kind::RandomizeTimes;
    r.all = true;
    r.value = (float)seed;
    request(r);
}

int VertexAnimator::getInstanceCount() const{
    return (int)getComponent<VertexAnimatorComponent>().instanceStates.size();
}

// ---- settings

void VertexAnimator::setAnimation(const std::string& path){
    VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    if (animator.animation != path){
        animator.animation = path;
        animator.guid.clear();
        animator.needsLoad = true;
    }
}

std::string VertexAnimator::getAnimation() const{
    return getComponent<VertexAnimatorComponent>().animation;
}

void VertexAnimator::setDefaultClip(const std::string& name){
    getComponent<VertexAnimatorComponent>().defaultClip = name;
}

std::string VertexAnimator::getDefaultClip() const{
    return getComponent<VertexAnimatorComponent>().defaultClip;
}

void VertexAnimator::setPlayAutomatically(bool playAutomatically){
    getComponent<VertexAnimatorComponent>().playAutomatically = playAutomatically;
}

bool VertexAnimator::isPlayAutomatically() const{
    return getComponent<VertexAnimatorComponent>().playAutomatically;
}

void VertexAnimator::setRandomStart(bool randomStart){
    getComponent<VertexAnimatorComponent>().randomStart = randomStart;
}

bool VertexAnimator::isRandomStart() const{
    return getComponent<VertexAnimatorComponent>().randomStart;
}

void VertexAnimator::setDefaultFadeTime(float fade){
    getComponent<VertexAnimatorComponent>().defaultFadeTime = std::isfinite(fade) && fade > 0.0f ? fade : 0.0f;
}

float VertexAnimator::getDefaultFadeTime() const{
    return getComponent<VertexAnimatorComponent>().defaultFadeTime;
}

void VertexAnimator::setInstanceEvents(bool instanceEvents){
    getComponent<VertexAnimatorComponent>().instanceEvents = instanceEvents;
}

bool VertexAnimator::isInstanceEvents() const{
    return getComponent<VertexAnimatorComponent>().instanceEvents;
}

void VertexAnimator::setPreview(bool preview){
    getComponent<VertexAnimatorComponent>().preview = preview;
}

bool VertexAnimator::isPreview() const{
    return getComponent<VertexAnimatorComponent>().preview;
}

void VertexAnimator::setEnabled(bool enabled){
    getComponent<VertexAnimatorComponent>().enabled = enabled;
}

bool VertexAnimator::isEnabled() const{
    return getComponent<VertexAnimatorComponent>().enabled;
}

bool VertexAnimator::isLoaded() const{
    const VertexAnimatorComponent& animator = getComponent<VertexAnimatorComponent>();
    return animator.asset != nullptr && !animator.needsLoad && animator.gpuActive;
}

std::string VertexAnimator::getLoadError() const{
    return getComponent<VertexAnimatorComponent>().loadError;
}

FunctionSubscribe<void(const AnimationEvent&)>& VertexAnimator::getOnEvent(){
    return getComponent<VertexAnimatorComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&)>& VertexAnimator::getOnFinish(){
    return getComponent<VertexAnimatorComponent>().onFinish;
}

FunctionSubscribe<void(int, const AnimationEvent&)>& VertexAnimator::getOnInstanceEvent(){
    return getComponent<VertexAnimatorComponent>().onInstanceEvent;
}

FunctionSubscribe<void(int, const std::string&)>& VertexAnimator::getOnInstanceFinish(){
    return getComponent<VertexAnimatorComponent>().onInstanceFinish;
}
