// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// "Revert to Baseline" of a scene diff row, after OpenEngine's InspectorVcsController
// (Apps/Editor/Source/VersionControl/Ui/InspectorVcsController.cpp: Revert Property to Baseline;
// MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md), for whole
// components and GameObjects too.

#pragma once

#include "command/Command.h"
#include "vcs/scene/SceneDiff.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // One undo step "Revert <path> to Baseline" (docs/specs/S29-version-control.md R16), made of
    // the editor's own commands, so each keeps its usual effects (prefab overrides, the Hierarchy,
    // Play Mode Changes):
    //   a property: the baseline's component decoded into a temporary one (reflection) and the
    //     one field set through ReflectedValueCmd; a value no table field holds (a script's
    //     properties) is spliced into the component and pasted (ComponentValuesCmd). A prefab
    //     instance member's property without an override in the baseline goes back to the
    //     prefab's value through PrefabOverrideCmd, as the Inspector's Revert does;
    //   a component: removed, added back or its values pasted (ComponentValuesCmd);
    //   a GameObject: an added one deleted, a removed one made again from the baseline's YAML
    //     (its id kept when free, its parent and sibling index restored), a moved one put back.
    class RevertToBaselineCmd : public Command {
    public:
        // The command for a diff row (component and property null: the whole GameObject);
        // null with *why when it cannot be reverted (the GameObject is not in the scene, a value
        // no command edits)
        static RevertToBaselineCmd* build(Project* project, uint32_t sceneId, const vcs::scene::SceneYamlModel& baseline,
                                          const vcs::scene::ObjectDiff& object, const vcs::scene::ComponentDiff* component,
                                          const vcs::scene::PropertyDiff* property, std::string* why);

        bool execute() override;
        void undo() override;
        bool canUndo() const override;
        bool canRedo() const override;

        // "Revert transform.position to Baseline", "Revert Enemy to Baseline"
        std::string describe() const override { return label; }
        const char* typeName() const override { return "RevertToBaselineCmd"; }
        size_t memoryBytes() const override;

    private:
        RevertToBaselineCmd(std::string label, std::vector<std::unique_ptr<Command>> steps);

        std::string label;
        std::vector<std::unique_ptr<Command>> steps;
    };

}
