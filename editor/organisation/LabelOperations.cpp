// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LabelOperations.h"

#include "Project.h"
#include "organisation/OrganisationCommands.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    enum class Kind { Rename, Remove, Color };

    LabelOperations::Result run(Project* project, Kind kind, const std::string& rawLabel, const std::string& argument) {
        LabelOperations::Result result;
        AssetLabels& store = project->getAssetLabels();
        store.waitUntilReady();
        std::string label;
        std::string why;
        if (!AssetLabels::normalizeLabel(rawLabel, label, &why)) {
            result.error = "label: " + why;
            return result;
        }
        std::string newName = label;
        if (kind == Kind::Rename && !AssetLabels::normalizeLabel(argument, newName, &why)) {
            result.error = "new name: " + why;
            return result;
        }
        if (kind == Kind::Color && !AssetLabels::isColor(argument)) {
            result.error = "a colour is \"#RRGGBB\"";
            return result;
        }

        SetAssetLabelsCmd::Changes before, after;
        if (kind != Kind::Color) {
            for (const auto& [guid, labels] : store.all()) {
                if (std::none_of(labels.begin(), labels.end(), [&](const std::string& l) { return AssetLabels::sameLabel(l, label); })) continue;
                std::vector<std::string> changed;
                for (const std::string& existing : labels) {
                    if (!AssetLabels::sameLabel(existing, label)) changed.push_back(existing);
                    else if (kind == Kind::Rename) changed.push_back(newName);
                }
                before.push_back({guid, labels});
                after.push_back({guid, changed});
            }
        }
        const std::vector<LabelColor> entriesBefore = store.entries();
        std::vector<LabelColor> entriesAfter;
        bool hadEntry = false;
        for (const LabelColor& entry : entriesBefore) {
            if (!AssetLabels::sameLabel(entry.name, label)) {
                entriesAfter.push_back(entry);
                continue;
            }
            hadEntry = true;
            if (kind == Kind::Rename) entriesAfter.push_back({newName, entry.color});
            if (kind == Kind::Color) entriesAfter.push_back({entry.name, argument});
        }
        if (kind == Kind::Color && !hadEntry) entriesAfter.push_back({store.canonical(label), argument});
        if (kind == Kind::Rename && !hadEntry && store.colorOf(label) != AssetLabels::paletteColorFor(newName)) {
            // the renamed label keeps the colour it showed
            entriesAfter.push_back({newName, store.colorOf(label)});
        }
        if (before.empty() && !hadEntry && kind != Kind::Color) {
            result.error = "No asset has the label " + label + " and Project Settings does not list it.";
            return result;
        }

        const char* name = kind == Kind::Rename ? "Rename Label" : (kind == Kind::Remove ? "Delete Label" : "Set Label Color");
        auto* command = new SetAssetLabelsCmd(project, name, before, after);
        command->setEntries(entriesBefore, entriesAfter);
        command->setNoMerge();
        CommandHistory* history = project->getProjectCommandHistory();
        if (!history->addCommand(command)) {
            result.error = "The labels could not be written (see the Console).";
            return result;
        }
        if (const UndoEntry* top = history->top()) {
            if (const auto* applied = dynamic_cast<const SetAssetLabelsCmd*>(top->cmd.get())) result.filesWritten = applied->filesWritten();
        }
        result.ok = true;
        result.assets = after.size();
        return result;
    }

}

LabelOperations::Result LabelOperations::rename(Project* project, const std::string& label, const std::string& newName) {
    return run(project, Kind::Rename, label, newName);
}

LabelOperations::Result LabelOperations::remove(Project* project, const std::string& label) {
    return run(project, Kind::Remove, label, {});
}

LabelOperations::Result LabelOperations::setColor(Project* project, const std::string& label, const std::string& color) {
    return run(project, Kind::Color, label, color);
}
