// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSubClips.h"

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace jasperengine;

namespace{

    std::string lower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    const char* const modelExtensions[] = {".gltf", ".glb", ".fbx"};

}

bool anim::isModelFilePath(const std::string& path){
    std::string text = lower(path);
    for (const char* extension : modelExtensions){
        size_t length = std::strlen(extension);
        if (text.size() >= length && text.compare(text.size() - length, length, extension) == 0) return true;
    }
    return false;
}

bool anim::isSubClipReference(const std::string& reference){
    SubClipReference parsed;
    return parseSubClipReference(reference, parsed);
}

bool anim::parseSubClipReference(const std::string& reference, SubClipReference& out){
    out = SubClipReference();
    std::string text = lower(reference);
    size_t split = std::string::npos;
    for (const char* extension : modelExtensions){
        std::string marker = std::string(extension) + "#";
        size_t at = text.find(marker);
        if (at != std::string::npos) split = std::min(split, at + marker.size() - 1);
    }
    if (split == std::string::npos || split + 1 >= reference.size()) return false;
    out.model = reference.substr(0, split);
    out.clip = reference.substr(split + 1);
    return true;
}

std::string anim::formatSubClipReference(const std::string& model, const std::string& clip){
    return model + "#" + clip;
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
