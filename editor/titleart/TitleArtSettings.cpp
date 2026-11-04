// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtSettingsCmd.h"

#include "Project.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;

const char* editor::titleStaleBakesPolicyName(TitleStaleBakesPolicy policy) {
    switch (policy) {
        case TitleStaleBakesPolicy::Rebake: return "rebake";
        case TitleStaleBakesPolicy::Warn: return "warn";
        case TitleStaleBakesPolicy::Fail: return "fail";
    }
    return "rebake";
}

const char* editor::titleStaleBakesPolicyLabel(TitleStaleBakesPolicy policy) {
    switch (policy) {
        case TitleStaleBakesPolicy::Rebake: return "Rebake";
        case TitleStaleBakesPolicy::Warn: return "Warn";
        case TitleStaleBakesPolicy::Fail: return "Fail";
    }
    return "Rebake";
}

bool editor::titleStaleBakesPolicyFromName(const std::string& name, TitleStaleBakesPolicy& out) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (TitleStaleBakesPolicy p : {TitleStaleBakesPolicy::Rebake, TitleStaleBakesPolicy::Warn, TitleStaleBakesPolicy::Fail}) {
        if (lower == titleStaleBakesPolicyName(p)) {
            out = p;
            return true;
        }
    }
    return false;
}

YAML::Node TitleArtProjectSettings::encode() const {
    const TitleArtProjectSettings defaults;
    YAML::Node node(YAML::NodeType::Map);
    if (autoBake != defaults.autoBake) node["autoBake"] = autoBake;
    if (staleBakesOnExport != defaults.staleBakesOnExport) node["staleBakesOnExport"] = titleStaleBakesPolicyName(staleBakesOnExport);
    return node;
}

TitleArtProjectSettings TitleArtProjectSettings::decode(const YAML::Node& node, std::vector<std::string>* warnings) {
    TitleArtProjectSettings out;
    if (!node || !node.IsMap()) return out;
    try {
        if (node["autoBake"]) out.autoBake = node["autoBake"].as<bool>();
    } catch (const std::exception&) {
        if (warnings) warnings->push_back("titleArt.autoBake is not true or false");
    }
    if (node["staleBakesOnExport"]) {
        const std::string value = node["staleBakesOnExport"].IsScalar() ? node["staleBakesOnExport"].Scalar() : std::string();
        if (!titleStaleBakesPolicyFromName(value, out.staleBakesOnExport) && warnings) {
            warnings->push_back("titleArt.staleBakesOnExport \"" + value + "\" is not rebake, warn or fail");
        }
    }
    return out;
}

TitleArtSettingsCmd::TitleArtSettingsCmd(Project* project, const TitleArtProjectSettings& before, const TitleArtProjectSettings& after)
    : project(project), before(before), after(after) {
}

bool TitleArtSettingsCmd::apply(const TitleArtProjectSettings& settings) {
    project->setTitleArtSettings(settings);
    if (project->getProjectPath().empty()) return true;
    return project->saveProjectFile();
}

bool TitleArtSettingsCmd::execute() {
    return apply(after);
}

void TitleArtSettingsCmd::undo() {
    apply(before);
}

std::vector<Command::Touched> TitleArtSettingsCmd::touchedPaths(bool) const {
    if (project->getProjectPath().empty()) return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
