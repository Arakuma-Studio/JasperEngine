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
//   * layers (anim layers spec 5.2, 7.1) are authored fields: `layers` empty = one "Base Layer"
//     (anim::playerLayers gives the effective list); layers[0] is the base layer, its weight is
//     always 1 and its blend Override (both ignored when authored). Entries reference their
//     layer by index (`AnimationPlayerClip::layer`, out of range: 0). Runtime layer weights,
//     masks and blends (Lua setLayerWeight...) live in AnimationPlayerRuntime::layers.
//   * masks are S21's Avatar Masks (.mask, AnimatorAssetPool; the weight rule AvatarMaskWeights.h).
//   * no controller: an Animator Controller plays on the Animator component (S21). Upstream's
//     `controller` / `controllerGuid` keys of this component are moved to an Animator by the
//     scene migration (editor/StreamMigrations.cpp); the AnimationPlayer object forwards
//     upstream's controller calls to the GameObject's Animator (object/AnimationPlayer.h).

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
        int layer = 0;          // index into the player's layers (anim layers spec 5.3)
        // humanoid playback (anim-retarget 7.1), each written only when true:
        bool retarget = false;  // play a generic clip as a humanoid clip, converted from its source model
        bool mirror = false;    // humanoid clips: left and right swapped
        bool footIK = false;    // humanoid clips: the feet pinned to the clip's foot goals

        bool operator==(const AnimationPlayerClip& other) const{
            return clip == other.clip && guid == other.guid && name == other.name && wrap == other.wrap && layer == other.layer &&
                   retarget == other.retarget && mirror == other.mirror && footIK == other.footIK;
        }
        bool operator!=(const AnimationPlayerClip& other) const{ return !(*this == other); }
    };

    // How a layer combines with the layers below it (Unity AnimatorLayerBlendingMode)
    enum class AnimationLayerBlend : uint8_t { Override, Additive };

    // Unity's PlayMode: what play / crossFade stop (Lua PlayMode.STOP_SAME_LAYER / STOP_ALL)
    enum class PlayMode : uint8_t { StopSameLayer = 0, StopAll = 1 };

    // One authored layer (anim layers spec 7.1)
    struct JASPERENGINE_API AnimationPlayerLayer{
        std::string name;               // unique in the player (case-sensitive); layers[0]: "Base Layer"
        float weight = 1.0f;            // [0, 1]; layers[0]: ignored (always 1)
        AnimationLayerBlend blend = AnimationLayerBlend::Override;  // layers[0]: always Override
        std::string mask;               // asset-relative .mask, "" = none
        std::string maskGuid;           // the mask's GUID (editor, AssetDatabase)

        bool operator==(const AnimationPlayerLayer& other) const{
            return name == other.name && weight == other.weight && blend == other.blend && mask == other.mask && maskGuid == other.maskGuid;
        }
        bool operator!=(const AnimationPlayerLayer& other) const{ return !(*this == other); }
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
        // S48 Pose Frame Rate (fps, 0 = smooth): every track holds its value on whole steps of
        // 1 / fps from its previous key, and the fades' weights step on the same ticks; events
        // fire at their true times
        float poseFrameRate = 0.0f;
        // empty = one "Base Layer" (anim::playerLayers)
        std::vector<AnimationPlayerLayer> layers;

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
        // upstream's controller callbacks: a state of the GameObject's Animator was entered /
        // exited (layer index, state name without the layer prefix; AnimatorSystem calls them
        // with the Animator's onStateEnter / onStateExit)
        FunctionSubscribe<void(int, const std::string&)> onStateEnter;
        FunctionSubscribe<void(int, const std::string&)> onStateExit;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onEvent); f(onFinish); f(onStateEnter); f(onStateExit); }
    };

}

#endif //ANIMATION_PLAYER_COMPONENT_H
