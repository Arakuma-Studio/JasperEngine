// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "registry/SortingLayers.h"

#include <vector>

namespace jasperengine::editor{

    class Project;

    // Project Settings > Tags and Layers > Sorting Layers and Graphics > Transparency Sort Mode / Axis
    // (spec S52 R30): the editor's scenes draw with them at once, and undo restores them. project.yaml
    // is written either way.
    struct SortingSettings{
        std::vector<SortingLayerInfo> layers;
        TransparencySortMode mode = TransparencySortMode::DEFAULT;
        Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);

        static SortingSettings current();
        bool operator==(const SortingSettings& o) const { return layers == o.layers && mode == o.mode && axis == o.axis; }
        bool operator!=(const SortingSettings& o) const { return !(*this == o); }
    };

    class SortingLayersCmd: public Command{
    private:
        Project* project;
        SortingSettings oldSettings;
        SortingSettings newSettings;
        std::string description;

        void apply(const SortingSettings& settings);

    public:
        SortingLayersCmd(Project* project, const SortingSettings& newSettings, std::string description = "Change Sorting Layers");

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        std::string describe() const override { return description; }
        const char* typeName() const override { return "SortingLayersCmd"; }
    };

}
