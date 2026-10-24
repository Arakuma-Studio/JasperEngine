// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Test seams of AnimationClipPool (engine tests only; never set in the editor or a game):
// - beforeInsert runs after a load finished and before its result is cached, without the pool
//   lock: a test invalidates there to check that an in-flight load is not cached.
// - listModelClips / buildSubClip replace anim::listModelFileClips / anim::buildModelSubClip, so
//   the sub-clip branch (alias tables, generations, reimport with reordered animations) can be
//   tested without model files.
// Set them while no other thread uses the pool; an empty struct restores the real functions.

#ifndef ANIMATION_CLIP_POOL_TESTING_H
#define ANIMATION_CLIP_POOL_TESTING_H

#include "Export.h"
#include "animation/ModelSubClips.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    struct AnimationClipPoolTestHooks{
        std::function<void(const std::string& key)> beforeInsert;
        std::function<bool(const std::string& modelPath, std::vector<anim::ModelClipInfo>& out, std::string* error)> listModelClips;
        std::function<bool(const std::string& reference, ClipData& out, std::vector<ClipWarning>& warnings, std::string* error)> buildSubClip;
    };

    JASPERENGINE_API void setAnimationClipPoolTestHooks(const AnimationClipPoolTestHooks& hooks);

}

#endif //ANIMATION_CLIP_POOL_TESTING_H
