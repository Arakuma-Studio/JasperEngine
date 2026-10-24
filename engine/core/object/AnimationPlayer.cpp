// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayer.h"

#include "animation/AnimationPlayerRuntime.h"

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

anim::AnimationStateInfo AnimationPlayer::getStateInfo(const std::string& name) const{
    (void)name;
    return anim::AnimationStateInfo();
}

bool AnimationPlayer::addClip(const std::string& path){
    (void)path;
    return false;
}

bool AnimationPlayer::addClip(const std::string& path, const std::string& name){
    (void)path;
    (void)name;
    return false;
}

bool AnimationPlayer::removeClip(const std::string& name){
    (void)name;
    return false;
}

int AnimationPlayer::getClipCount() const{
    return 0;
}

std::string AnimationPlayer::getClipName(int index) const{
    (void)index;
    return "";
}

void AnimationPlayer::setSpeed(float speed){
    (void)speed;
}

float AnimationPlayer::getSpeed() const{
    return 1.0f;
}

void AnimationPlayer::setDefaultClip(const std::string& name){
    (void)name;
}

std::string AnimationPlayer::getDefaultClip() const{
    return "";
}

void AnimationPlayer::setDefaultFadeTime(float fade){
    (void)fade;
}

float AnimationPlayer::getDefaultFadeTime() const{
    return 0.25f;
}

void AnimationPlayer::setPlayAutomatically(bool playAutomatically){
    (void)playAutomatically;
}

bool AnimationPlayer::isPlayAutomatically() const{
    return true;
}

void AnimationPlayer::setEnabled(bool enabled){
    (void)enabled;
}

bool AnimationPlayer::isEnabled() const{
    return true;
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
