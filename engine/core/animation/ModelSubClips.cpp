// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSubClips.h"

using namespace jasperengine;

bool anim::isModelFilePath(const std::string& path){
    // TODO(T1c)
    (void)path;
    return false;
}

bool anim::isSubClipReference(const std::string& reference){
    // TODO(T1c)
    (void)reference;
    return false;
}

bool anim::parseSubClipReference(const std::string& reference, SubClipReference& out){
    // TODO(T1c)
    (void)reference;
    out = SubClipReference();
    return false;
}

std::string anim::formatSubClipReference(const std::string& model, const std::string& clip){
    // TODO(T1c)
    (void)clip;
    return model;
}

std::vector<anim::ModelClipInfo> anim::listModelClips(const tinygltf::Model& model){
    // TODO(T1c)
    (void)model;
    return {};
}

int anim::findModelClip(const tinygltf::Model& model, const std::string& clip){
    // TODO(T1c)
    (void)model;
    (void)clip;
    return -1;
}

std::vector<std::string> anim::modelNodePaths(const tinygltf::Model& model){
    // TODO(T1c)
    (void)model;
    return {};
}

bool anim::buildSubClip(const tinygltf::Model& model, int animationIndex, ClipData& out,
                        std::vector<ClipWarning>& warnings, std::string* error){
    // TODO(T1c)
    (void)model;
    (void)animationIndex;
    (void)warnings;
    out = ClipData();
    if (error) *error = "Model sub-clips are not implemented yet.";
    return false;
}

bool anim::listModelFileClips(const std::string& modelPath, std::vector<ModelClipInfo>& out, std::string* error){
    // TODO(T1c)
    (void)modelPath;
    out.clear();
    if (error) *error = "Model sub-clips are not implemented yet.";
    return false;
}

bool anim::buildModelSubClip(const std::string& reference, ClipData& out,
                             std::vector<ClipWarning>& warnings, std::string* error){
    // TODO(T1c)
    (void)reference;
    (void)warnings;
    out = ClipData();
    if (error) *error = "Model sub-clips are not implemented yet.";
    return false;
}
