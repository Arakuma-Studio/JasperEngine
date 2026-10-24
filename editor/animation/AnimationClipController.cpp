// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipController.h"

#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T3a): everything below. The stubs bind nothing, open nothing and never preview.

namespace{
    const char* notImplemented = "The Animation window's clip mode is not implemented yet.";

    bool fail(std::string* error){
        if (error) *error = notImplemented;
        return false;
    }
}

AnimationClipController::AnimationClipController(Project* project): project(project){
}

AnimationClipController::~AnimationClipController(){
}

namespace{
    std::map<Project*, std::unique_ptr<AnimationClipController>>& controllers(){
        static std::map<Project*, std::unique_ptr<AnimationClipController>> map;
        return map;
    }
}

AnimationClipController& AnimationClipController::shared(Project* project){
    std::unique_ptr<AnimationClipController>& controller = controllers()[project];
    if (!controller) controller.reset(new AnimationClipController(project));
    return *controller;
}

void AnimationClipController::release(Project* project){
    auto it = controllers().find(project);
    if (it == controllers().end()) return;
    it->second->stopPreview();
    it->second->closeClip();
    controllers().erase(it);
}

bool AnimationClipController::bindRoot(uint32_t sceneId, Entity root, std::string* error){
    (void)sceneId;
    (void)root;
    return fail(error);
}

uint32_t AnimationClipController::getSceneId() const{
    return sceneId;
}

Entity AnimationClipController::getRoot() const{
    return root;
}

bool AnimationClipController::hasPlayer() const{
    return false;
}

std::vector<std::string> AnimationClipController::getStateNames() const{
    return {};
}

bool AnimationClipController::openClip(const std::string& path, std::string* error){
    (void)path;
    return fail(error);
}

bool AnimationClipController::openState(const std::string& name, std::string* error){
    (void)name;
    return fail(error);
}

void AnimationClipController::closeClip(){
}

std::shared_ptr<AnimationClipDocument> AnimationClipController::getDocument() const{
    return document;
}

const std::string& AnimationClipController::getClipPath() const{
    return clipPath;
}

const std::string& AnimationClipController::getStateName() const{
    return stateName;
}

bool AnimationClipController::isReadOnly() const{
    return false;
}

std::string AnimationClipController::getPreviewModel() const{
    return "";
}

ClipValidationReport AnimationClipController::getBindings() const{
    return ClipValidationReport();
}

bool AnimationClipController::startPreview(std::string* error){
    return fail(error);
}

void AnimationClipController::stopPreview(){
}

bool AnimationClipController::isPreviewing() const{
    return previewing;
}

void AnimationClipController::play(){
}

void AnimationClipController::pause(){
}

bool AnimationClipController::isPlaying() const{
    return playing;
}

void AnimationClipController::setPlaybackSpeed(float speed){
    playbackSpeed = speed;
}

float AnimationClipController::getPlaybackSpeed() const{
    return playbackSpeed;
}

void AnimationClipController::seek(float time){
    (void)time;
}

float AnimationClipController::getTime() const{
    return time;
}

float AnimationClipController::getLength() const{
    return 0.0f;
}

void AnimationClipController::update(double dt){
    (void)dt;
}

std::vector<AnimationClipController::BoundTransform> AnimationClipController::sampledBoundTransforms(size_t max) const{
    (void)max;
    return {};
}

bool AnimationClipController::addProperty(const std::string& path, ClipChannel channel, size_t* newTrack, std::string* error){
    (void)path;
    (void)channel;
    (void)newTrack;
    return fail(error);
}

bool AnimationClipController::keyTracks(const std::vector<size_t>& tracks, float time, const std::string& mergeKey, std::string* error){
    (void)tracks;
    (void)time;
    (void)mergeKey;
    return fail(error);
}

bool AnimationClipController::keyAll(float time, std::string* error){
    (void)time;
    return fail(error);
}

bool AnimationClipController::keyModified(float time, std::string* error){
    (void)time;
    return fail(error);
}

bool AnimationClipController::setKeyValue(const KeyRef& key, const std::vector<float>& value, const std::string& mergeKey, std::string* error){
    (void)key;
    (void)value;
    (void)mergeKey;
    return fail(error);
}

bool AnimationClipController::moveKeys(const std::vector<KeyRef>& keys, float deltaTime, const std::string& mergeKey,
                                       std::vector<KeyRef>* moved, std::string* error){
    (void)keys;
    (void)deltaTime;
    (void)mergeKey;
    (void)moved;
    return fail(error);
}

bool AnimationClipController::beginKeyDrag(const std::vector<KeyRef>& keys, std::string* error){
    (void)keys;
    return fail(error);
}

bool AnimationClipController::updateKeyDrag(float totalDeltaTime, const std::vector<float>* valueDelta,
                                            std::vector<KeyRef>* moved, std::string* error){
    (void)totalDeltaTime;
    (void)valueDelta;
    (void)moved;
    return fail(error);
}

void AnimationClipController::endKeyDrag(){
    dragging = false;
    dragKeys.clear();
}

bool AnimationClipController::isDraggingKeys() const{
    return dragging;
}

bool AnimationClipController::findKey(const ClipData& data, const std::string& path, ClipChannel channel, float time, KeyRef& out){
    // complete: a pure lookup
    int track = data.findTrack(path, channel);
    if (track < 0) return false;
    const std::vector<ClipKey>& keys = data.tracks[(size_t)track].keys;
    for (size_t k = 0; k < keys.size(); k++){
        float difference = keys[k].time - time;
        if (difference <= anim::ANIM_TIME_EPSILON && difference >= -anim::ANIM_TIME_EPSILON){
            out.track = (size_t)track;
            out.key = k;
            return true;
        }
    }
    return false;
}

bool AnimationClipController::deleteKeys(const std::vector<KeyRef>& keys, std::string* error){
    (void)keys;
    return fail(error);
}

bool AnimationClipController::setEase(const std::vector<KeyRef>& keys, EaseType ease, std::string* error){
    (void)keys;
    (void)ease;
    return fail(error);
}

bool AnimationClipController::removeTrack(size_t track, std::string* error){
    (void)track;
    return fail(error);
}

bool AnimationClipController::retargetTrack(size_t track, const std::string& newPath, std::string* error){
    (void)track;
    (void)newPath;
    return fail(error);
}

bool AnimationClipController::commit(const ClipData& data, const std::string& undoName, const std::string& mergeKey, std::string* error){
    (void)data;
    (void)undoName;
    (void)mergeKey;
    return fail(error);
}

void AnimationClipController::setRecording(bool recording){
    (void)recording;
}

bool AnimationClipController::isRecording() const{
    return recording;
}

void AnimationClipController::stopTransientPreviews(){
}

bool AnimationClipController::ownsUndo(bool sceneViewFocused) const{
    (void)sceneViewFocused;
    return false;
}

void AnimationClipController::setLastActionWasClipEdit(bool clipEdit){
    lastActionWasClipEdit = clipEdit;
}

void AnimationClipController::sceneClosed(uint32_t sceneId){
    (void)sceneId;
}
