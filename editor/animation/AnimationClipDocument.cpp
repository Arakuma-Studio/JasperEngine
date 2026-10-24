// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipDocument.h"

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T2b): everything below (PoseAnimationDocument is the model)

std::vector<ClipUser> jasperengine::editor::findClipUsers(Project* project, const std::string& clip){
    (void)project;
    (void)clip;
    return {};
}

std::shared_ptr<AnimationClipDocument> AnimationClipDocument::open(Project* project, const std::string& path, std::string* error){
    (void)project;
    (void)path;
    if (error) *error = "Animation clip documents are not implemented yet.";
    return nullptr;
}

std::shared_ptr<AnimationClipDocument> AnimationClipDocument::create(Project* project, const std::string& path, const ClipData& initial,
                                                                     const std::string& undoName, std::string* error){
    (void)project;
    (void)path;
    (void)initial;
    (void)undoName;
    if (error) *error = "Animation clip documents are not implemented yet.";
    return nullptr;
}

std::vector<std::shared_ptr<AnimationClipDocument>> AnimationClipDocument::openDocuments(){
    return {};
}

void AnimationClipDocument::filesChanged(const std::vector<std::filesystem::path>& paths){
    (void)paths;
}

void AnimationClipDocument::checkDiskChanges(){
}

void AnimationClipDocument::closeAll(){
}

void AnimationClipDocument::setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook){
    (void)hook;
}

std::string AnimationClipDocument::fileText(const ClipData& data){
    return anim::writeClip(data);
}

std::shared_ptr<const AnimationClip> AnimationClipDocument::clip(){
    if (!built || builtRevision != rev){
        built = AnimationClip::build(current);
        builtRevision = rev;
    }
    return built;
}

bool AnimationClipDocument::commit(const ClipData& newData, const std::string& undoName, const std::string& mergeKey,
                                   std::string* error, std::vector<ClipWarning>* warnings){
    (void)newData;
    (void)undoName;
    (void)mergeKey;
    (void)warnings;
    if (error) *error = "Animation clip documents are not implemented yet.";
    return false;
}

void AnimationClipDocument::reloadFromDisk(){
}

int AnimationClipDocument::addListener(Listener listener){
    int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void AnimationClipDocument::removeListener(int id){
    listeners.erase(id);
}
