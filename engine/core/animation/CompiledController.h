// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R4-R6, D5, D6)
// - Purpose: the immutable runtime form of an Animator Controller, built once from a
//   ControllerDef and shared by every Animator that uses it (AnimatorAssetPool): flat arrays of
//   parameters, layers, machines, states, transitions, conditions and blend trees with every name
//   resolved to an index, the distinct clips it plays, the gradient-band pair data of the 2D
//   trees, and the diagnostics editors and MCP report (unknown parameters, missing targets,
//   unreachable states, transitions that can never fire).
// - Indices: state, machine, transition, condition and tree indices are global to the
//   controller; a layer owns ranges of them. -1 means none / unresolved (a condition on an
//   unknown parameter never holds; a transition to an unknown target is never taken).
// - Thread-safety: never changes after compile (any thread reads it).

#ifndef COMPILED_CONTROLLER_H
#define COMPILED_CONTROLLER_H

#include "Export.h"
#include "animation/ClipTypes.h"
#include "animation/ControllerDef.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::anim{

    struct CompiledCondition{
        int parameter = -1;
        AnimatorConditionMode mode = AnimatorConditionMode::If;
        float threshold = 0.0f;
    };

    enum class TransitionTargetKind : uint8_t { State, Machine, Exit, None };

    struct CompiledTransition{
        TransitionTargetKind kind = TransitionTargetKind::None;
        int target = -1;                // state or machine index; Exit: the machine left
        uint32_t conditionBegin = 0;
        uint32_t conditionCount = 0;
        bool hasExitTime = false;
        float exitTime = 0.75f;
        bool fixedDuration = true;
        float duration = 0.25f;
        float offset = 0.0f;
        TransitionInterruptionSource interruptionSource = TransitionInterruptionSource::None;
        bool orderedInterruption = true;
        bool canTransitionToSelf = false;
        bool mute = false;
        // a state's transition: the state and its index in the state's list; -1 for entry,
        // any-state and exit transitions
        int ownerState = -1;
        int indexInOwner = -1;
        std::string to;                 // as written (diagnostics, MCP)
    };

    struct CompiledMotion{
        enum class Kind : uint8_t { None, Clip, Tree };
        Kind kind = Kind::None;
        int index = -1;                 // clip slot or tree
    };

    struct CompiledTreeChild{
        CompiledMotion motion;
        float threshold = 0.0f;
        float x = 0.0f, y = 0.0f;
        float timeScale = 1.0f;
        int directParameter = -1;
        bool mirror = false;            // flips the state's Mirror
    };

    struct CompiledTree{
        BlendTreeType type = BlendTreeType::Simple1D;
        int parameterX = -1;
        int parameterY = -1;
        uint32_t childBegin = 0;
        uint32_t childCount = 0;
        // gradient band (freeform types): childCount * childCount records of 4 floats from pairBegin
        uint32_t pairBegin = 0;
        // simple directional: the child at the origin, -1 when none
        int centre = -1;
        // leaves of the whole subtree (clip motions), for the evaluation scratch
        uint32_t leafCount = 0;
    };

    struct CompiledState{
        std::string name;
        std::string fullPath;           // "Base Layer.Combat.Attack"
        std::string tag;
        int layer = 0;
        int machine = -1;
        CompiledMotion motion;
        float speed = 1.0f;
        int speedParameter = -1;
        float cycleOffset = 0.0f;
        int cycleOffsetParameter = -1;
        int timeParameter = -1;
        AnimatorStateLoop loop = AnimatorStateLoop::Inherit;
        AdditiveReference additiveReference = AdditiveReference::FirstFrame;
        float additiveTime = 0.0f;
        bool mirror = false;            // humanoid playback (§12)
        bool footIK = false;
        bool retarget = false;
        uint32_t transitionBegin = 0;
        uint32_t transitionCount = 0;
        uint32_t nameHash = 0;          // of name and of fullPath (Unity's Animator.StringToHash rule: FNV-1a here)
        uint32_t fullPathHash = 0;
    };

    struct CompiledMachine{
        std::string name;
        std::string path;               // "Base Layer.Combat"
        int layer = 0;
        int parent = -1;
        TransitionTargetKind defaultKind = TransitionTargetKind::None;
        int defaultTarget = -1;         // its default: a state or a sub-machine
        uint32_t entryBegin = 0, entryCount = 0;
        uint32_t exitBegin = 0, exitCount = 0;      // exitTransitions: evaluated in the parent
    };

    struct CompiledLayer{
        std::string name;
        float weight = 1.0f;
        AnimatorLayerBlending blending = AnimatorLayerBlending::Override;
        AnimatorAssetRef mask;
        std::string slot;
        int rootMachine = -1;
        uint32_t stateBegin = 0, stateCount = 0;
        // every any-state transition of the layer's machines, root first then depth first
        uint32_t anyBegin = 0, anyCount = 0;
    };

    struct CompiledController{
        std::string guid;
        std::vector<ParameterDef> parameters;
        std::vector<CompiledLayer> layers;
        std::vector<CompiledMachine> machines;
        std::vector<CompiledState> states;
        std::vector<CompiledTransition> transitions;
        std::vector<CompiledCondition> conditions;
        std::vector<CompiledTree> trees;
        std::vector<CompiledTreeChild> children;
        std::vector<float> pairs;
        // the distinct clips (by path), in first-use order
        std::vector<AnimatorAssetRef> clips;
        // per clip slot: it plays through the humanoid path whenever the Animator has a humanoid
        // avatar (a state's Retarget, Mirror or Foot IK, a child's Mirror); a clip used both ways
        // has a slot of each kind
        std::vector<uint8_t> clipHumanoid;
        // problems found while compiling: path = where in the controller ("layers[0]...", the
        // JSON path), message
        std::vector<ClipWarning> diagnostics;

        int findParameter(const std::string& name) const;
        int findLayer(const std::string& name) const;
        // a state by "Layer.Machine.State", or by name within `layer` (-1: every layer, the first
        // match); -1 when none
        int findState(const std::string& nameOrPath, int layer = -1) const;
        // the deepest blend-tree nesting of any motion
        int maxTreeDepth = 0;
        // the most leaves a single motion has (the evaluation scratch size)
        uint32_t maxMotionLeaves = 1;
    };

    // Compiles `def` (normalized). Never null.
    JASPERENGINE_API std::shared_ptr<const CompiledController> compileController(const ControllerDef& def);

    // FNV-1a 32 of a state name or full path (MCP and Lua accept names; hashes are for C++)
    JASPERENGINE_API uint32_t animatorNameHash(const std::string& text);

}

#endif //COMPILED_CONTROLLER_H
