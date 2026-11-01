// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.3, 9.R3/R23/R39)
// - Purpose: the binding and handler report of a clip against an (optional) bound root, shared
//   by the Animation window (missing rows), the Inspector (Bindings foldout, asset warnings),
//   MCP (edit/get_animation `bindings`) and the CLI (`check --anims`).
// - Owner: T0 (API); T2d implements it.
// - Thread-safety: main thread (reads a scene); validate() with a null registry is pure.
// - Invariants: never mutates the clip or the scene; one TrackReport per clip track and one
//   EventReport per event, in clip order; `nearest` holds at most ClipValidationOptions::
//   maxNearest existing paths ordered by edit distance; a ModelComponent in the legacy layout
//   under the root gives one LegacyModelLayout report instead of N missing paths (R3); a root
//   with a C++ script skips the missing-handler check (R23); an event named like a script lifecycle
//   method or engine event (init, onDestroy, onUpdate...) adds one warning per name (it calls that
//   method again).

#pragma once

#include "animation/AnimationClip.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor{

    class Project;
    struct SceneProject;

    enum class TrackBindingStatus{
        Ok,
        MissingPath,            // no entity at the path (`nearest` suggests)
        DuplicateSiblingName,   // resolved, but a segment without "#N" names several siblings
        MissingComponent,       // no Transform / no morph targets / no LatticeComponent / no property target
        LatticeIndexOutOfRange, // points outside the bound cage (`badPoints`)
        MorphIndexOutOfRange,   // more weights than the mesh has morph targets
        LegacyModelLayout,      // under a model loaded without its node hierarchy (R3)
        UnaddressableName,      // a name on the way has '/' or ends in "#<digits>"
        NotSampled,             // (unused since property tracks play: kept for the tools' names)
        UnknownProperty,        // a property track of a (component, property) that is not animatable
        Unchecked               // no root given
    };

    enum class EventHandlerStatus{
        Found,                  // a script on the root defines the function
        NotFound,
        Unchecked               // no root, or the root has a C++ script
    };

    struct TrackReport{
        size_t track = 0;
        std::string path;
        ClipChannel channel = ClipChannel::Position;
        // property tracks: "component.property" ("ui.color.a", "script:Torch.glow"); the binding's
        // reason when it does not bind ("no 'light' component", "did you mean 'intensity'?")
        std::string property;
        std::string reason;
        TrackBindingStatus status = TrackBindingStatus::Unchecked;
        Entity entity = NULL_ENTITY;            // the bound entity when resolved
        std::vector<std::string> nearest;       // MissingPath: existing paths, nearest first
        std::vector<int> badPoints;             // LatticeIndexOutOfRange
        std::string message;                    // one line for the window, Inspector and tools
    };

    struct EventReport{
        size_t event = 0;
        std::string function;
        EventHandlerStatus status = EventHandlerStatus::Unchecked;
        std::string message;
    };

    struct ClipValidationReport{
        Entity root = NULL_ENTITY;
        size_t resolved = 0;                    // tracks whose status is Ok or DuplicateSiblingName
        size_t total = 0;                       // sampled tracks
        bool legacyModelLayout = false;
        // the models on or under the root loaded in the legacy layout (R3): the fix is
        // ClipSceneActions::reloadModelsForPlayer (PlayerModelLayout::ensureNodeHierarchyForPlayer
        // as one scene step); tracks that can't bind because of it are LegacyModelLayout
        std::vector<Entity> legacyModels;
        std::vector<TrackReport> tracks;
        std::vector<EventReport> events;
        std::vector<ClipWarning> warnings;      // other problems (unaddressable names in the root...)

        // every track resolved and every handler found (or unchecked)
        bool ok() const;
    };

    struct ClipValidationOptions{
        size_t maxNearest = 3;
        // Lua methods the root's scripts define (validateInScene fills it)
        std::vector<std::string> scriptFunctions;
        // the root has a C++ script: handlers are not checked
        bool hasCppScript = false;
        bool checkHandlers = true;
    };

    class ClipValidation{
    public:
        // `registry` null or `root` NULL_ENTITY: structure only (every track Unchecked)
        static ClipValidationReport validate(const ClipData& data, EntityRegistry* registry, Entity root,
                                             const ClipValidationOptions& options = ClipValidationOptions());
        // the same with the handler list taken from the root's scripts in `scene`
        static ClipValidationReport validateInScene(Project* project, SceneProject* scene, const ClipData& data, Entity root);

        // the methods the Lua scripts on `entity` define (event function combo, handler check)
        static std::vector<std::string> scriptFunctions(Project* project, SceneProject* scene, Entity entity);
        // existing paths under `root` nearest to `path` by edit distance, at most `max`
        static std::vector<std::string> nearestPaths(EntityRegistry* registry, Entity root, const std::string& path, size_t max = 3);
        // "Arm/Hand : Position", "Panel : ui.color" (window rows, tool texts)
        static std::string trackLabel(const ClipTrackData& track);
        // The side a bone name says, 'L' or 'R' (0 when none or both), from the last path segment's
        // words: split at separators (. _ - space :) and camelCase boundaries, a word "Left" /
        // "Right" or a lone "L" / "R" ("mixamorig:LeftLeg", "thigh.L", "Hand_R", "l_foot", "LArm");
        // never inside a word ("Bright", "upright", "Lefty")
        static char boneSide(const std::string& path);
        // a method the engine calls on Lua scripts by itself (init, onDestroy) or an engine event
        // name (onUpdate...): an event of that function calls it again (validate warns)
        static bool isScriptLifecycleMethod(const std::string& function);
    };

}
