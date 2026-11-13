// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R4-R8, D5, D18)
// - Purpose: the editable value forms of the Animator's three assets: an Animator Controller
//   (`.controller`), an Avatar Mask (`.mask`) and an Animation Montage (`.montage`). Parsed and
//   written by AnimatorAssetIO, compiled for the runtime by CompiledController / the asset pool,
//   edited by the editor's documents, the Animator window and MCP (value semantics: copy, change,
//   commit), like ClipData for clips.
// - Thread-safety: plain values.
// - Names and paths:
//   * a state machine's states and sub-state machines share one name space (unique, no '/', not
//     "Exit", "Entry" or "Any State"); parameters and layers are unique by name.
//   * a transition's `to` is a path relative to the machine that owns the transition (its state's
//     machine; for entry / any-state transitions the machine that lists them; for a sub-state
//     machine's exit transitions its parent): "Run", "Combat/Attack" (into a sub-state machine,
//     the last segment a state or a sub-state machine), "Exit" (the owning machine's exit), or
//     "/Locomotion/Walk" from the layer's root machine.
//   * a state's full path is the layer name, its machines and its name joined by '.'
//     ("Base Layer.Combat.Attack"), Unity's form.
//   * clip references are upstream's ({path, guid}: an asset-relative .anim path or
//     "<model>#<clip>", the .anim's GUID or empty).

#ifndef CONTROLLER_DEF_H
#define CONTROLLER_DEF_H

#include "Export.h"
#include "animation/AnimationEvent.h"
#include "animation/AnimatorTypes.h"
#include "math/Vector2.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::anim{

    // unknown members of a file's object and their compact JSON text, in file order: written back
    // (every object of a controller and of a mask keeps them, so no edit drops what it does not know)
    using AnimatorUnknownKeys = std::vector<std::pair<std::string, std::string>>;

    // an asset reference in one of the files: the path hint and the GUID (editor)
    struct JASPERENGINE_API AnimatorAssetRef{
        std::string path;
        std::string guid;

        bool empty() const { return path.empty() && guid.empty(); }
        bool operator==(const AnimatorAssetRef& other) const { return path == other.path && guid == other.guid; }
        bool operator!=(const AnimatorAssetRef& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API ParameterDef{
        std::string name;
        AnimatorParameterType type = AnimatorParameterType::Float;
        // Float: the value; Int: an integer value; Bool / Trigger: 0 or 1
        float defaultValue = 0.0f;
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API ConditionDef{
        std::string parameter;
        AnimatorConditionMode mode = AnimatorConditionMode::If;
        float threshold = 0.0f;     // Greater / Less / Equals / NotEqual
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API TransitionDef{
        std::string to;
        std::vector<ConditionDef> conditions;
        bool hasExitTime = false;
        float exitTime = 0.75f;             // normalised time of the source state
        bool fixedDuration = true;          // duration in seconds, else in source-state lengths
        float duration = 0.25f;
        float offset = 0.0f;                // normalised start time of the destination
        TransitionInterruptionSource interruptionSource = TransitionInterruptionSource::None;
        bool orderedInterruption = true;
        bool canTransitionToSelf = false;   // any-state transitions only
        bool mute = false;                  // never taken
        AnimatorUnknownKeys unknownKeys;
    };

    struct BlendTreeDef;

    struct JASPERENGINE_API MotionDef{
        // empty clip and no tree: no motion (the layer shows its base / rest pose)
        AnimatorAssetRef clip;
        std::vector<BlendTreeDef> tree;     // 0 or 1 element (a value-semantic optional of an incomplete type)

        // defined below, where BlendTreeDef is complete (MSVC defines a dllexport class's implicit
        // members where the class ends, which would need it complete here)
        MotionDef();
        MotionDef(const MotionDef& other);
        MotionDef(MotionDef&& other) noexcept;
        MotionDef& operator=(const MotionDef& other);
        MotionDef& operator=(MotionDef&& other) noexcept;
        ~MotionDef();

        bool isTree() const;
        bool empty() const;
    };

    struct JASPERENGINE_API BlendChildDef{
        MotionDef motion;
        float threshold = 0.0f;             // Simple1D
        Vector2 position = Vector2(0, 0);   // 2D types
        float timeScale = 1.0f;
        std::string directParameter;        // Direct: the Float parameter that is its weight
        bool mirror = false;                // flips its state's Mirror (humanoid playback)
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API BlendTreeDef{
        BlendTreeType type = BlendTreeType::Simple1D;
        std::string parameter;              // 1D and 2D x
        std::string parameterY;             // 2D y
        std::vector<BlendChildDef> children;
        AnimatorUnknownKeys unknownKeys;
    };

    inline MotionDef::MotionDef() = default;
    inline MotionDef::MotionDef(const MotionDef& other) = default;
    inline MotionDef::MotionDef(MotionDef&& other) noexcept = default;
    inline MotionDef& MotionDef::operator=(const MotionDef& other) = default;
    inline MotionDef& MotionDef::operator=(MotionDef&& other) noexcept = default;
    inline MotionDef::~MotionDef() = default;
    inline bool MotionDef::isTree() const { return !tree.empty(); }
    inline bool MotionDef::empty() const { return clip.empty() && tree.empty(); }

    struct JASPERENGINE_API StateDef{
        std::string name;
        Vector2 position = Vector2(0, 0);   // graph position (editor)
        MotionDef motion;
        float speed = 1.0f;
        std::string speedParameter;         // multiplies speed when set (Float)
        float cycleOffset = 0.0f;           // normalised
        std::string cycleOffsetParameter;   // replaces cycleOffset when set (Float)
        std::string timeParameter;          // when set (Float), the normalised time is this parameter
        AnimatorStateLoop loop = AnimatorStateLoop::Inherit;
        std::string tag;
        AdditiveReference additiveReference = AdditiveReference::FirstFrame;   // additive layers
        float additiveTime = 0.0f;          // seconds, AdditiveReference::Time
        // humanoid playback (the one retarget path, §12): left and right swapped; the legs reach
        // the clip's foot goals; retargeted from the clip's source model whatever the Animator's
        // Retarget (upstream's state flags). Each plays the state's clips through the humanoid
        // path when the Animator has a humanoid avatar.
        bool mirror = false;
        bool footIK = false;
        bool retarget = false;
        std::vector<TransitionDef> transitions;
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API StateMachineDef{
        std::string name;                   // the layer's root machine: unnamed
        Vector2 position = Vector2(0, 0);   // its node in the parent's graph
        std::string defaultState;           // relative path; empty: the first state
        std::vector<TransitionDef> entry;   // conditional entries (no duration, no exit time)
        std::vector<TransitionDef> anyState;
        std::vector<StateDef> states;
        std::vector<StateMachineDef> subMachines;
        // taken in the parent machine when this machine is left through its Exit
        std::vector<TransitionDef> exitTransitions;
        // graph positions of the special nodes (editor)
        Vector2 entryPosition = Vector2(50, 120);
        Vector2 anyStatePosition = Vector2(50, 40);
        Vector2 exitPosition = Vector2(800, 120);
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API LayerDef{
        std::string name;
        float weight = 1.0f;                // the first layer always counts as 1
        AnimatorLayerBlending blending = AnimatorLayerBlending::Override;
        AnimatorAssetRef mask;              // a .mask; empty: every bone
        std::string slot;                   // the montage slot this layer plays (empty: none)
        StateMachineDef machine;
        AnimatorUnknownKeys unknownKeys;
    };

    struct JASPERENGINE_API ControllerDef{
        std::string guid;
        std::vector<ParameterDef> parameters;
        std::vector<LayerDef> layers;
        // unknown top-level keys and their compact JSON text, in file order: written back
        AnimatorUnknownKeys unknownKeys;

        int findParameter(const std::string& name) const;
        int findLayer(const std::string& name) const;
    };

    // ---- avatar mask ----

    // The weight of a GameObject under the masked root is decided by AvatarMaskWeights.h: its path
    // entry (or an ancestor's with `children`), else its humanoid body part, else `defaultWeight`.
    struct JASPERENGINE_API MaskPathDef{
        std::string path;                   // relative to the Animator's GameObject (ClipBinding grammar; "" = itself)
        float weight = 1.0f;                // 0..1
        bool children = false;              // the entry also covers the path's descendants

        bool operator==(const MaskPathDef& other) const{
            return path == other.path && weight == other.weight && children == other.children;
        }
        bool operator!=(const MaskPathDef& other) const{ return !(*this == other); }
    };

    struct JASPERENGINE_API AvatarMaskDef{
        std::string guid;
        // the weight of what neither a path nor a body part covers (an empty mask passes everything)
        float defaultWeight = 1.0f;
        // per body part 0..1; unset parts take defaultWeight
        std::optional<float> bodyParts[AVATAR_BODY_PART_COUNT];
        // the root part also covers the masked GameObject and the transforms between it and the
        // hips (upstream's rule; otherwise they take defaultWeight). The root part always weighs
        // root motion.
        bool rootHierarchy = false;
        std::vector<MaskPathDef> paths;
        // editor only: the model or prefab the mask Inspector builds its tree and figure from
        std::string previewModel;
        AnimatorUnknownKeys unknownKeys;

        // the part's own weight, else defaultWeight
        float partWeight(AvatarBodyPart part) const;
        bool hasPart(AvatarBodyPart part) const { return bodyParts[(int)part].has_value(); }
        // a body part is set: the weights need the root's humanoid
        bool usesHumanoid() const{
            for (const std::optional<float>& part : bodyParts){
                if (part) return true;
            }
            return false;
        }
        // index into paths, -1 when none
        int findPath(const std::string& path) const;
    };

    // ---- montage ----

    struct JASPERENGINE_API MontageBlendDef{
        float time = 0.25f;                 // seconds
        std::string ease = "linear";        // an upstream ease name (AnimationClipIO easeName)
    };

    struct JASPERENGINE_API MontageSectionDef{
        std::string name;
        float start = 0.0f;                 // seconds in the clip; a section ends where the next starts
        std::string next;                   // the section played after it; empty: the montage ends
    };

    struct JASPERENGINE_API MontageEventDef{
        float time = 0.0f;
        std::string function;
        std::vector<AnimationEventArg> args;
    };

    struct JASPERENGINE_API MontageDef{
        std::string guid;
        AnimatorAssetRef clip;
        std::string slot = "Default";
        float rate = 1.0f;
        MontageBlendDef blendIn;
        MontageBlendDef blendOut;
        // sorted by start; the first starts at 0 (normalize adds "Default" when none is given)
        std::vector<MontageSectionDef> sections;
        std::vector<MontageEventDef> events;
        AnimatorUnknownKeys unknownKeys;

        int findSection(const std::string& name) const;
    };

}

#endif //CONTROLLER_DEF_H
