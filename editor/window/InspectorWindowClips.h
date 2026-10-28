// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pure helpers of the Inspector's animation clip sections (InspectorWindowClips.cpp: the
// Animation Player section, the .anim / model asset section, the Model and AnimationComponent
// clip buttons; animation clips spec 4.2, 9.R7/R8/R30/R45), kept out of InspectorWindow so the
// editor tests can check them without ImGui.
// Owner: T2c. Thread-safety: main thread (topLevelEntities, actionPreviewEntities and
// acquirePreviewOwners read a registry / the PreviewOwner registry), the rest is pure.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/ClipValidation.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
    class Scene;
}

namespace jasperengine::editor{
    class AnimationClipDocument;
    class Project;
}

namespace jasperengine::editor::inspectorclips{

    // The Inspector's clip documents by the reference it draws (a player entry's clip, an asset
    // path): per frame a map lookup; a reference that failed to open is tried again after
    // `retrySeconds`. A cached document is returned only while it is still the file the
    // reference named: a rename or move takes the document object along
    // (AnimationClipDocument::filesMoved), and the old path then names another file or none.
    class ClipDocumentCache{
    public:
        std::shared_ptr<AnimationClipDocument> find(Project* project, const std::string& clip, double now, double retrySeconds,
                                                    std::string* error);
        void clear() { entries.clear(); }
        size_t size() const { return entries.size(); }

    private:
        struct Entry{
            std::weak_ptr<AnimationClipDocument> document;
            std::string path;               // the document's path() when cached
            std::string error;
            double retryAt = 0.0;
        };
        std::map<std::string, Entry> entries;
    };

    // "<model>#<clip>"
    bool isSubClip(const std::string& clip);
    // what a clip field shows: "walk.anim", "Walk" (a sub-clip's name); "" for an empty entry
    std::string clipFieldText(const std::string& clip);
    // the tooltip of an entry whose clip is gone (R8: a deleted file keeps the reference)
    std::string missingClipTooltip(const std::string& clip);
    // a sub-clip part ("Walk", or an index "2") names one of the model's sub-clips
    bool subClipNamed(const std::vector<std::string>& subClipNames, const std::string& clip);
    // "Default", "Once", "Loop", "Ping Pong", "Clamp Forever"
    const char* wrapLabel(ClipWrapMode wrap);
    // "Position", "Rotation", "Scale", "Blend Shapes", "Lattice", "Property": the window's row names
    // too (clipui::channelLabel)
    const char* channelLabel(ClipChannel channel);
    // "1.25 s"
    std::string formatSeconds(float seconds);
    // "1 user in open scenes", "3 users in open scenes, 1 in loaded prefabs" (R45: only loaded
    // scenes and prefabs are counted)
    std::string usersText(size_t sceneUsers, size_t prefabUsers);

    // `text` fitting `maxWidth` as measured by `width`: unchanged when it fits, else with "..." in
    // the middle keeping more of the end (a file name keeps its extension), or at the start
    using TextWidth = std::function<float(const std::string&)>;
    std::string elideMiddle(const std::string& text, float maxWidth, const TextWidth& width);
    std::string elideStart(const std::string& text, float maxWidth, const TextWidth& width);

    // a report with at least one checked track or event (else the foldout says "Unchecked")
    bool reportChecked(const ClipValidationReport& report);
    // the Bindings foldout line of one clip: "12/14 bound", "Unchecked", "legacy model layout"
    std::string bindingsSummary(const ClipValidationReport& report);
    // a track that is neither bound nor unchecked (a WarningText row)
    bool trackHasProblem(const TrackReport& track);
    // the problems a report counts in the Bindings header: tracks, missing handlers, the legacy layout
    size_t problemCount(const ClipValidationReport& report);
    // its short row, the leaf names only ("Spne : Position: missing (did you mean Spine?)"); the
    // validation's full message is the row's tooltip
    std::string trackProblemText(const TrackReport& track);
    // an event whose handler is missing: "Event footstep at 0.50 s: no script on the root defines footstep()"
    std::string eventProblemText(const EventReport& event, float time);
    // "1, \"left\", true"
    std::string eventArgsText(const std::vector<AnimationEventArg>& args);

    // The label of row `index` of the Model's entity animations table: the sub-clip name the
    // loader's entity stands for when it names the same animation ("Wave" -> "Wave 2" for the
    // second of two "Wave" animations), else the entity name (empty: `fallback`)
    std::string modelAnimationRowLabel(const std::string& entityName, const std::vector<std::string>& subClipNames,
                                       size_t index, const std::string& fallback);

    // The entities of `entities` with no ancestor in the list (each is a PreviewOwner root the
    // Inspector's action preview acquires: owning a root also covers its descendants). Order kept,
    // duplicates and destroyed entities dropped.
    std::vector<Entity> topLevelEntities(EntityRegistry* registry, const std::vector<Entity>& entities);
    // What the Inspector's action preview writes: the action, its target, an animation's actions
    // and their targets (the walk of its snapshot), in walk order
    std::vector<Entity> actionPreviewEntities(Scene* scene, Entity action);
    // R7: one PreviewOwner per top-level entity of actionPreviewEntities, acquired before the
    // preview snapshots the scene (the owners in the way stop and restore first); `stop` ends
    // the Inspector preview (restore only, never acquire). The tokens, all > 0.
    std::vector<int> acquirePreviewOwners(uint32_t sceneId, Scene* scene, Entity action, std::function<void()> stop);

    // The inspected-object rule (9.R30) for programmatic selection changes: the Project window
    // sets an asset on a click; a later change of the scene selection (MCP, undo) shows the
    // selection again. User selection clicks clear the asset at their own sites.
    class InspectedObjectTracker{
    public:
        struct Result{
            bool clearAsset = false;    // the asset was set before and the selection changed
            bool changed = false;       // the selection or the asset changed (stale error lines go)
        };
        Result update(const std::filesystem::path& asset, uint32_t sceneId, const std::vector<Entity>& selection);
    private:
        bool known = false;
        uint32_t sceneId = 0;
        std::vector<Entity> selection;
        std::filesystem::path assetSeen;
    };

}
