// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pure text and selection helpers of the Inspector's animation clip sections
// (InspectorWindowClips.cpp: the Animation Player section, the .anim / model asset section, the
// Model and AnimationComponent clip buttons; animation clips spec 4.2, 9.R7/R30/R45). Kept out of
// InspectorWindow so the editor tests can check them without ImGui.
// Owner: T2c. Thread-safety: main thread (topLevelEntities reads a registry), the rest is pure.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/ClipValidation.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor::inspectorclips{

    // "<model>#<clip>"
    bool isSubClip(const std::string& clip);
    // what a clip field shows: "walk.anim", "Walk" (a sub-clip's name); "" for an empty entry
    std::string clipFieldText(const std::string& clip);
    // the field of an entry whose file is gone (R8: delete keeps the reference): "Missing (walk.anim)"
    std::string missingClipText(const std::string& clip);
    // "Default", "Once", "Loop", "Ping Pong", "Clamp Forever"
    const char* wrapLabel(ClipWrapMode wrap);
    // "1.25 s"
    std::string formatSeconds(float seconds);
    // "1 user in open scenes", "3 users in open scenes" (R45: only loaded scenes are counted)
    std::string usersText(size_t count);

    // a report with at least one checked track or event (else the foldout says "Unchecked")
    bool reportChecked(const ClipValidationReport& report);
    // the Bindings foldout line of one clip: "12/14 bound", "Unchecked", "legacy model layout"
    std::string bindingsSummary(const ClipValidationReport& report);
    // a track that is neither bound nor unchecked (a WarningText row)
    bool trackHasProblem(const TrackReport& track);
    // its row: "Arm/Hand : Position: no GameObject at this path (did you mean Arm/Hand2?)"
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

}
