// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayer.h"

#include "animation/AnimationPlayerRuntime.h"

#include <algorithm>

using namespace jasperengine;

// AnimationState (TODO(T1b): every accessor)

AnimationState::AnimationState(){
}

AnimationState::AnimationState(Scene* scene, Entity entity, const std::string& name):
        scene(scene), entity(entity), name(name){
}

bool AnimationState::isValid() const{
    return false;
}

std::string AnimationState::getName() const{
    return name;
}

std::string AnimationState::getClip() const{
    return "";
}

void AnimationState::setTime(float time){
    (void)time;
}

float AnimationState::getTime() const{
    return 0.0f;
}

void AnimationState::setNormalizedTime(float normalizedTime){
    (void)normalizedTime;
}

float AnimationState::getNormalizedTime() const{
    return 0.0f;
}

void AnimationState::setSpeed(float speed){
    (void)speed;
}

float AnimationState::getSpeed() const{
    return 1.0f;
}

void AnimationState::setWeight(float weight){
    (void)weight;
}

float AnimationState::getWeight() const{
    return 0.0f;
}

float AnimationState::getLength() const{
    return 0.0f;
}

void AnimationState::setWrap(const std::string& wrap){
    (void)wrap;
}

std::string AnimationState::getWrap() const{
    return "once";
}

void AnimationState::setEnabled(bool enabled){
    (void)enabled;
}

bool AnimationState::isEnabled() const{
    return false;
}

// AnimationPlayer

AnimationPlayer::AnimationPlayer(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<AnimationPlayerComponent>(entity)){
        scene->addComponent<AnimationPlayerComponent>(entity, {});
    }
}

AnimationPlayer::~AnimationPlayer(){
}

AnimationPlayer::AnimationPlayer(const AnimationPlayer& rhs) = default;
AnimationPlayer& AnimationPlayer::operator=(const AnimationPlayer& rhs) = default;
AnimationPlayer::AnimationPlayer(AnimationPlayer&& rhs) noexcept = default;
AnimationPlayer& AnimationPlayer::operator=(AnimationPlayer&& rhs) noexcept = default;

// TODO(T1b): the playback calls below

void AnimationPlayer::play(const std::string& name){
    (void)name;
}

void AnimationPlayer::play(const std::string& name, float fade){
    (void)name;
    (void)fade;
}

void AnimationPlayer::crossFade(const std::string& name){
    (void)name;
}

void AnimationPlayer::crossFade(const std::string& name, float fade){
    (void)name;
    (void)fade;
}

void AnimationPlayer::blend(const std::string& name, float weight){
    (void)name;
    (void)weight;
}

void AnimationPlayer::blend(const std::string& name, float weight, float fade){
    (void)name;
    (void)weight;
    (void)fade;
}

void AnimationPlayer::queue(const std::string& name){
    (void)name;
}

void AnimationPlayer::queue(const std::string& name, float fade){
    (void)name;
    (void)fade;
}

void AnimationPlayer::stop(){
}

void AnimationPlayer::stop(const std::string& name){
    (void)name;
}

void AnimationPlayer::rewind(){
}

void AnimationPlayer::rewind(const std::string& name){
    (void)name;
}

void AnimationPlayer::sample(){
}

bool AnimationPlayer::isPlaying() const{
    return false;
}

bool AnimationPlayer::isPlaying(const std::string& name) const{
    (void)name;
    return false;
}

AnimationState AnimationPlayer::getState(const std::string& name) const{
    return AnimationState(scene, entity, name);
}

AnimationStateInfo AnimationPlayer::getStateInfo(const std::string& name) const{
    (void)name;
    return AnimationStateInfo();
}

// authored fields (complete): a clip list change sets needsLoad

bool AnimationPlayer::addClip(const std::string& path){
    return addClip(path, "");
}

bool AnimationPlayer::addClip(const std::string& path, const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    AnimationPlayerClip entry;
    entry.clip = path;
    entry.name = name;
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    if (std::find(names.begin(), names.end(), anim::defaultStateName(entry)) != names.end()) return false;
    player.clips.push_back(entry);
    player.needsLoad = true;
    return true;
}

bool AnimationPlayer::removeClip(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    for (size_t i = 0; i < names.size(); i++){
        if (names[i] == name){
            player.clips.erase(player.clips.begin() + (long)i);
            player.needsLoad = true;
            return true;
        }
    }
    return false;
}

int AnimationPlayer::getClipCount() const{
    return (int)getComponent<AnimationPlayerComponent>().clips.size();
}

std::string AnimationPlayer::getClipName(int index) const{
    std::vector<std::string> names = anim::playerStateNames(getComponent<AnimationPlayerComponent>().clips);
    return index >= 0 && (size_t)index < names.size() ? names[(size_t)index] : std::string();
}

void AnimationPlayer::setSpeed(float speed){
    getComponent<AnimationPlayerComponent>().speed = speed;
}

float AnimationPlayer::getSpeed() const{
    return getComponent<AnimationPlayerComponent>().speed;
}

void AnimationPlayer::setDefaultClip(const std::string& name){
    getComponent<AnimationPlayerComponent>().defaultClip = name;
}

std::string AnimationPlayer::getDefaultClip() const{
    return getComponent<AnimationPlayerComponent>().defaultClip;
}

void AnimationPlayer::setDefaultFadeTime(float fade){
    getComponent<AnimationPlayerComponent>().defaultFadeTime = fade;
}

float AnimationPlayer::getDefaultFadeTime() const{
    return getComponent<AnimationPlayerComponent>().defaultFadeTime;
}

void AnimationPlayer::setPlayAutomatically(bool playAutomatically){
    getComponent<AnimationPlayerComponent>().playAutomatically = playAutomatically;
}

bool AnimationPlayer::isPlayAutomatically() const{
    return getComponent<AnimationPlayerComponent>().playAutomatically;
}

void AnimationPlayer::setEnabled(bool enabled){
    getComponent<AnimationPlayerComponent>().enabled = enabled;
}

bool AnimationPlayer::isEnabled() const{
    return getComponent<AnimationPlayerComponent>().enabled;
}

bool AnimationPlayer::isLoaded() const{
    return false;
}

FunctionSubscribe<void(const AnimationEvent&)>& AnimationPlayer::getOnEvent(){
    return getComponent<AnimationPlayerComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&)>& AnimationPlayer::getOnFinish(){
    return getComponent<AnimationPlayerComponent>().onFinish;
}
