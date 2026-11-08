// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Test seams of AnimationClipPool and ModelSubClips, compiled only into test builds: their
// definitions exist when the engine is configured with JASPERENGINE_ANIM_TESTS or
// JASPERENGINE_EDITOR_TESTS (engine/CMakeLists.txt defines JASPERENGINE_ANIM_TEST_HOOKS for the
// two sources). A shipped engine has neither the functions nor the hooks' per-load cost; calling
// them there fails to link.
// - beforeInsert runs after a load finished and before its result is cached, without the pool
//   lock: a test invalidates there to check that an in-flight load is not cached.
// - listModelClips / buildSubClip replace anim::listModelFileClips / anim::buildModelSubClip, so
//   the sub-clip branch (alias tables, generations, reimport with reordered animations) can be
//   tested without model files.
// - singleThreaded stands for a build without threads (web without pthreads) in pollOrLoad.
// Set them while no other thread uses the pool; an empty struct restores the real functions.

#ifndef ANIMATION_CLIP_POOL_TESTING_H
#define ANIMATION_CLIP_POOL_TESTING_H

#include "Export.h"
#include "animation/ModelSubClips.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    struct AnimationClipPoolTestHooks{
        std::function<void(const std::string& key)> beforeInsert;
        std::function<bool(const std::string& modelPath, std::vector<anim::ModelClipInfo>& out, std::string* error)> listModelClips;
        std::function<bool(const std::string& reference, ClipData& out, std::vector<ClipWarning>& warnings, std::string* error)> buildSubClip;
        // pollOrLoad's "no threads" (else the job system's)
        std::function<bool()> singleThreaded;
    };

    JASPERENGINE_API void setAnimationClipPoolTestHooks(const AnimationClipPoolTestHooks& hooks);

    // how many times ModelSubClips parsed a model file itself (models not in the ModelPool)
    JASPERENGINE_API size_t getModelFileParseCount();

}

#endif //ANIMATION_CLIP_POOL_TESTING_H
