// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clip actions that write clip files and change a scene, each ONE scene undo step (a
// CreateClipCmd, animation clips spec 9.R4): Create New Clip, Convert to Clip, Extract to .anim
// (switching the model's player to the file), Use Animation Player (R15), clips dropped or added
// onto an object, and the legacy-layout fix of ClipValidation (R3). Shared by the Inspector and
// App hooks (installed here: convertToClip, extractSubClip, useAnimationPlayer,
// addToAnimationPlayer, createNewClip, dropClips), the Pose window's Bake to Animation Clip
// (PoseBaker) and MCP. Every one that makes an Animation Player apply reloads legacy-layout
// models under it in the node hierarchy in the same step
// (PlayerModelLayout::ensureNodeHierarchyForPlayer). Main thread, edit mode only.

#pragma once

#include "animation/ClipConverters.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Command;
    class Project;

    struct ClipActionResult{
        std::string clip;                   // the .anim written (project-relative); empty: none
        Entity player = NULL_ENTITY;        // the GameObject with the Animation Player
        bool addedPlayer = false;           // false: an existing player was used
        std::vector<size_t> entries;        // the player entry of each clip, in the order given
        size_t appended = 0;                // entries appended (clips already listed are reused)
        size_t reloadedModels = 0;          // models reloaded in the node hierarchy
        size_t removedEntities = 0;         // entity animations / converted tracks removed
        bool projectStep = false;           // a pure clip edit: the project history, no scene step
        ClipConvertReport convert;          // Convert to Clip: what was and wasn't converted
        std::vector<ClipWarning> warnings;
        std::string status;                 // one line for the status bar / Console / tools
    };

    class ClipSceneActions{
    public:
        // a new clip at `clipPath` (project-relative .anim; fails when it exists) on `root`'s
        // player (added when it has none)
        static bool createNewClip(Project* project, uint32_t sceneId, Entity root, const std::string& clipPath,
                                  ClipActionResult& result, std::string& error);

        // Convert to Clip: the entity Animation's keyframe tracks into `clipPath`, played by the
        // player of `root` (NULL_ENTITY: the Animation's target); `removeConvertedTracks` removes
        // the converted track entities from the Animation
        static bool convertToClip(Project* project, uint32_t sceneId, Entity animation, Entity root, const std::string& clipPath,
                                  bool removeConvertedTracks, ClipActionResult& result, std::string& error);

        // Extract to .anim: the model sub-clip `subClip` ("<model>#<clip>") into `clipPath`. With
        // `model` and `usePlayer`, the model's player entries of that sub-clip switch to the file
        // (the state keeps its name; a player is added with the file when it has none) in one
        // scene step; otherwise the file alone, a project step (into an existing file: a commit
        // that keeps its GUID). `edited` (optional): the clip to write instead of the plain
        // extract (MCP's extract with the call's edits applied)
        static bool extractSubClip(Project* project, uint32_t sceneId, Entity model, const std::string& subClip,
                                   const std::string& clipPath, bool usePlayer, ClipActionResult& result, std::string& error,
                                   const ClipData* edited = nullptr);

        // Convert to Humanoid Clip (anim-retarget 9): `source` (a .anim, project- or asset-relative,
        // or "<model>#<clip>") converted on the rig of `model` (asset-relative; "" = its model,
        // source or preview model) into the new file `clipPath`. With `player` and `entry` (an
        // index of its clip list) the entry switches to the file in the same scene step (Use in
        // this player); otherwise the file alone, a project step.
        static bool convertToHumanoid(Project* project, uint32_t sceneId, Entity player, size_t entry, const std::string& source,
                                      const std::string& model, const std::string& clipPath, ClipActionResult& result, std::string& error);
        // "<name> (Humanoid).anim" next to the source (a model clip: in the model's folder), project-relative
        static std::string humanoidClipPath(Project* project, const std::string& source);

        // Use Animation Player (R15): the model's entity animations and their tracks removed,
        // ModelComponent.animationEntities = false, a player listing every "<model>#<name>"
        static bool useAnimationPlayer(Project* project, uint32_t sceneId, Entity model, ClipActionResult& result, std::string& error);

        // Clips onto an object (drop, "Add to Animation Player"): `clips` are .anim paths
        // (absolute, project- or asset-relative), "<model>#<clip>" references, or model files
        // (every sub-clip of them); `target` gets a player (or reuses its own) listing them
        static bool addClipsToPlayer(Project* project, uint32_t sceneId, Entity target, const std::vector<std::string>& clips,
                                     ClipActionResult& result, std::string& error);

        // optimize_animation with `root` (S77): the reduced clips written and `player`'s Animation Player
        // entries of their sources switched to them (the states keep their names), one scene step
        struct OptimizedClip{
            std::string source;             // what an entry plays now, in any form findClipUsers takes
            std::filesystem::path file;     // absolute
            std::string text;
        };
        static bool writeOptimizedClips(Project* project, uint32_t sceneId, Entity player, const std::vector<OptimizedClip>& clips,
                                        ClipActionResult& result, std::string& error);

        // ClipValidation's legacy-layout fix: the models on or under `root` that its player needs
        // in the node hierarchy reloaded, one scene step
        static bool reloadModelsForPlayer(Project* project, uint32_t sceneId, Entity root, ClipActionResult& result, std::string& error);

        // The scene commands that give `root` an Animation Player (or use its own) listing
        // `entries` after its own clips; an entry already listed (same clip) is reused.
        // `result` gets player, addedPlayer, entries and appended.
        static void appendPlayerCommands(Project* project, uint32_t sceneId, Entity root, const std::vector<AnimationPlayerClip>& entries,
                                         std::vector<Command*>& commands, ClipActionResult& result);

        // One CreateClipCmd on the scene history: `files` (project-relative path, content) and
        // `commands`; `playerRoot` the entity whose player applies (layout reload). False +
        // error when it failed (nothing changed).
        static bool runStep(Project* project, uint32_t sceneId, const std::vector<std::pair<std::string, std::string>>& files,
                            std::vector<Command*> commands, Entity playerRoot, ClipActionResult& result, std::string& error);

        // the text of a new clip file: canonical, with a GUID no asset has (or `keepGuidOf`'s
        // GUID when that file exists: a rewrite keeps its identity)
        static std::string newClipText(Project* project, ClipData data, const std::string& keepGuidOf = "");

        // Before a step reads or changes what a preview may show (every action above, their
        // CreateClipCmd's execute / undo, the layout reload's ModelLoadCmd): the previews of
        // `roots` of `sceneId` (or of an entity above or under one) restore their snapshots
        // first (9.R7): the clip and entity modes of the Animation window, the Inspector's
        // action preview, and a posed character (PoseEditSession::suspendActiveFor).
        // NULL_ENTITY roots are ignored; none left: nothing stops.
        static void stopPreviewsFor(uint32_t sceneId, std::vector<Entity> roots);

        // The save dialog of a new .anim (Create New Clip, Convert to Clip, Extract to .anim from
        // the Inspector and the Animation window): starts in `directory` (project-relative; a
        // path outside the project starts at its root) with `name`.anim ("New Animation" when
        // empty). The chosen path with ".anim" added unless it has it (any case); empty when
        // cancelled.
        static std::string askNewClipPath(Project* project, const std::string& directory, const std::string& name);
        // The directory a sub-clip's Extract starts in: its model's folder (project-relative,
        // empty when outside the project)
        static std::string subClipDirectory(Project* project, const std::string& subClip);
    };

}
