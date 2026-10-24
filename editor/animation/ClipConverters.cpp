// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipConverters.h"

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T2d): everything below

bool ClipConverters::convertEntityAnimation(Scene* scene, Entity animation, Entity root, ClipData& out,
                                            ClipConvertReport& report, std::string* error){
    (void)scene;
    (void)animation;
    (void)root;
    out = ClipData();
    report = ClipConvertReport();
    if (error) *error = "Convert to Clip is not implemented yet.";
    return false;
}

bool ClipConverters::extractModelSubClip(const std::string& reference, ClipData& out,
                                         std::vector<ClipWarning>& warnings, std::string* error){
    (void)reference;
    (void)warnings;
    out = ClipData();
    if (error) *error = "Extract to .anim is not implemented yet.";
    return false;
}

bool ClipConverters::planUseAnimationPlayer(Scene* scene, Entity model, UseAnimationPlayerPlan& out, std::string* error){
    (void)scene;
    out = UseAnimationPlayerPlan();
    out.model = model;
    if (error) *error = "Use Animation Player is not implemented yet.";
    return false;
}

bool ClipConverters::bakeChannels(Scene* scene, Entity root, const std::vector<BakedChannel>& channels, float length, bool loop,
                                  ClipData& out, std::string* error){
    (void)scene;
    (void)root;
    (void)channels;
    (void)length;
    (void)loop;
    out = ClipData();
    if (error) *error = "Bake to Clip is not implemented yet.";
    return false;
}

std::string ClipConverters::uniqueClipPath(Project* project, const std::string& directory, const std::string& stem){
    (void)project;
    return directory + "/" + stem + ".anim";
}

std::string ClipConverters::defaultClipDirectory(Project* project, uint32_t sceneId, Entity root){
    (void)project;
    (void)sceneId;
    (void)root;
    return "assets";
}
