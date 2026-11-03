// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The three-way merge of a scene or prefab (docs/specs/S29-version-control.md R17), property by
// property over SceneYamlModels: a property one side changed takes that side; both sides with
// equal values, either; with different values it is a conflict (ours stays, unless a resolution
// says theirs). A GameObject both sides added under one id with different content keeps ours;
// theirs is renumbered above max(nextEntity, every id) and the references to it in known fields
// (the reflection tables' Entity and EntityReference fields, EntityReference script properties,
// mainCamera, prefabInstance.memberIds, a prefab's root) follow. Deleted on one side and modified
// on the other is a conflict. A prefab's nextEntity takes the max. The output is in Stream's
// canonical key order, so loading and saving it again changes nothing.

#include "vcs/scene/SceneYamlModel.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::vcs::scene {

    struct MergeConflict {
        uint64_t object = 0;                // the GameObject (kSceneSettingsId for the document)
        std::string name;
        std::string path;                   // "transform.position"; "" for the GameObject itself
        std::string reason;                 // "changed on both sides", "deleted in theirs, modified in ours"...
        std::string base;                   // YAML flow text
        std::string ours;
        std::string theirs;

        // what a resolution names it by: "<object>/<path>"
        std::string key() const;
    };

    enum class MergeSide : uint8_t { Ours, Theirs };

    struct MergeResult {
        bool ok = false;                    // false: an input did not parse (error says which)
        std::string error;
        SceneYamlModel merged;
        std::vector<MergeConflict> conflicts;   // the ones no resolution decided
        std::map<uint64_t, uint64_t> renumbered;   // theirs' id -> its id in the result
        std::string text;                   // the merged document as a save writes it
    };

    // resolutions: conflict key -> the side it takes (the Conflicts window's Ours/Theirs)
    MergeResult mergeScenes(const SceneYamlModel& base, const SceneYamlModel& ours, const SceneYamlModel& theirs,
                            const std::map<std::string, MergeSide>& resolutions = {});
    // The three texts parsed first (an empty base: both sides added the file)
    MergeResult mergeSceneTexts(const std::string& base, const std::string& ours, const std::string& theirs,
                                const std::map<std::string, MergeSide>& resolutions = {});

    // The conflicts as `.jasperengine/merge/<path>.conflicts.yaml` holds them (R18)
    std::string conflictsText(const std::string& file, const std::vector<MergeConflict>& conflicts);

}
