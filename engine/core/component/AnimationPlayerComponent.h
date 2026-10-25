// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.2, 3.6, 9.R8/R14/R18/R24/R25/R40/R51)
// - Purpose: Unity's legacy Animation component ("Animation Player", Lua AnimationPlayer): a list
//   of clips (.anim files or model sub-clips) the root plays with fades, crossfades, blends,
//   queues and wrap modes, binding tracks by path relative to its entity.
// - The runtime (anim::AnimationPlayerRuntime) is driven by ActionSystem's players; the editor
//   plumbing (Catalog, Stream, Factory...) covers the authored fields.
// - Thread-safety: an ECS component (main thread). Decoding a scene off the main thread fills
//   the authored fields only: nothing loads or resolves until the system update.
// - Invariants:
//   * only the authored fields are serialized, cloned as values and emitted on export; the
//     runtime (`runtime`, `needsLoad`, `started`) is reset on clone/decode and the callbacks are
//     cleared (EntityCloner, Engine script reset, Scene::removeSubscriptionsByTag).
//   * clip paths are asset-relative like ModelComponent::filename (".anim" or "<model>#<clip>");
//     `guid` is editor-only (AssetDatabase: resolved first, repairs `clip`), empty for sub-clips.
//   * state names: anim::defaultStateName / anim::playerStateNames (AnimationPlayerRuntime.h):
//     `name`, else the file stem / sub-clip name; duplicates get a runtime-only " 2", " 3" alias
//     (never written back). Everyone (runtime, Inspector, controller, MCP) uses those two.

#ifndef ANIMATION_PLAYER_COMPONENT_H
#define ANIMATION_PLAYER_COMPONENT_H

#include "Export.h"
#include "animation/ClipTypes.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace anim{
        class AnimationPlayerRuntime;
    }
    // animation/AnimationEvent.h (callers of onEvent include it)
    struct AnimationEvent;

    // One entry of the player's clip list (spec 9.R14)
    struct JASPERENGINE_API AnimationPlayerClip{
        std::string clip;       // asset-relative .anim path or "<model>#<clip>"
        std::string guid;       // the .anim's GUID (editor); empty for sub-clips
        std::string name;       // state name; empty = file stem / sub-clip name
        ClipWrapMode wrap = ClipWrapMode::Default;  // Default = the clip's own

        bool operator==(const AnimationPlayerClip& other) const{
            return clip == other.clip && guid == other.guid && name == other.name && wrap == other.wrap;
        }
        bool operator!=(const AnimationPlayerClip& other) const{ return !(*this == other); }
    };

    struct JASPERENGINE_API AnimationPlayerComponent{
        // component checkbox: disabled = time stops and nothing is written (like an inactive
        // GameObject)
        bool enabled = true;

        std::vector<AnimationPlayerClip> clips;
        // the state played automatically; empty = the first clip
        std::string defaultClip;
        // the default clip plays when play mode / the game starts (or the player is spawned)
        bool playAutomatically = true;
        // multiplies every state's speed
        float speed = 1.0f;
        // used by play(name, fade) / crossFade(name) without a fade and Model:playAnimation
        // (0.25 s: between the engine's entity-animation 0.15 and Unity's 0.3)
        float defaultFadeTime = 0.25f;

        // runtime only (not serialised)
        bool needsLoad = true;      // the clip list changed: the next update (re)loads and rebinds
        // the initial playing state was decided: playAutomatically applied, or a script played
        // or stopped before the first update
        bool started = false;
        std::shared_ptr<anim::AnimationPlayerRuntime> runtime;

        // a clip event fired (after the frame's pose was written; spec 3.5, 9.R16)
        FunctionSubscribe<void(const AnimationEvent&)> onEvent;
        // a once state ended, or a clamp_forever state first reached its end (the state name)
        FunctionSubscribe<void(const std::string&)> onFinish;
    };

}

#endif //ANIMATION_PLAYER_COMPONENT_H
