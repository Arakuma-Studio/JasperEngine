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

namespace {

    const char* const kTitleArtPlatforms[] = {"windows", "linux", "macos", "ios", "android", "web"};

}

const std::string& TitleArtPlatformTiers::of(const std::string& platform) const {
    static const std::string none;
    const std::string* tier = const_cast<TitleArtPlatformTiers*>(this)->find(platform);
    return tier ? *tier : none;
}

std::string* TitleArtPlatformTiers::find(const std::string& platform) {
    if (platform == "windows") return &windowsTier;
    if (platform == "linux") return &linuxTier;
    if (platform == "macos") return &macosTier;
    if (platform == "ios") return &iosTier;
    if (platform == "android") return &androidTier;
    if (platform == "web") return &webTier;
    return nullptr;
}

bool editor::isTitleArtRealtimeTier(const std::string& tier) {
    return tier == "high" || tier == "medium" || tier == "low";
}

YAML::Node TitleArtProjectSettings::encode() const {
    const TitleArtProjectSettings defaults;
    YAML::Node node(YAML::NodeType::Map);
    if (autoBake != defaults.autoBake) node["autoBake"] = autoBake;
    if (staleBakesOnExport != defaults.staleBakesOnExport) node["staleBakesOnExport"] = titleStaleBakesPolicyName(staleBakesOnExport);
    auto groups = [&node](const char* key, const TitleArtGroupValues& values, const TitleArtGroupValues& d, const double* editor, double editorDefault) {
        if (values == d && (!editor || *editor == editorDefault)) return;
        YAML::Node group(YAML::NodeType::Map);
        group["desktop"] = values.desktop;
        group["mobile"] = values.mobile;
        group["web"] = values.web;
        if (editor) group["editor"] = *editor;
        node[key] = group;
    };
    groups("frameBudgetMs", frameBudgetMs, defaults.frameBudgetMs, &editorFrameBudgetMs, defaults.editorFrameBudgetMs);
    groups("releaseDelaySeconds", releaseDelaySeconds, defaults.releaseDelaySeconds, nullptr, 0.0);
    groups("cacheBudgetMiB", cacheBudgetMiB, defaults.cacheBudgetMiB, nullptr, 0.0);
    if (referenceHeight != defaults.referenceHeight) node["referenceHeight"] = referenceHeight;
    if (simulateExportCharacters != defaults.simulateExportCharacters) node["simulateExportCharacters"] = simulateExportCharacters;
    // S86 R37: only the platforms whose tier is not the default
    YAML::Node tiers(YAML::NodeType::Map);
    for (const char* platform : kTitleArtPlatforms) {
        if (realtime3DQuality.of(platform) != defaults.realtime3DQuality.of(platform)) tiers[platform] = realtime3DQuality.of(platform);
    }
    if (tiers.size() > 0) node["realtime3DQuality"] = tiers;
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
    // S81's: a value that does not read keeps its default
    auto number = [&](const YAML::Node& value, double& out, double lo, double hi, const std::string& path) {
        if (!value) return;
        try {
            out = std::clamp(value.as<double>(), lo, hi);
        } catch (const std::exception&) {
            if (warnings) warnings->push_back("titleArt." + path + " is not a number");
        }
    };
    auto groups = [&](const char* key, TitleArtGroupValues& values, double lo, double hi, double* editor) {
        const YAML::Node group = node[key];
        if (!group || !group.IsMap()) return;
        number(group["desktop"], values.desktop, lo, hi, std::string(key) + ".desktop");
        number(group["mobile"], values.mobile, lo, hi, std::string(key) + ".mobile");
        number(group["web"], values.web, lo, hi, std::string(key) + ".web");
        if (editor) number(group["editor"], *editor, lo, hi, std::string(key) + ".editor");
    };
    groups("frameBudgetMs", out.frameBudgetMs, 0.25, 16.0, &out.editorFrameBudgetMs);
    groups("releaseDelaySeconds", out.releaseDelaySeconds, 0.0, 600.0, nullptr);
    groups("cacheBudgetMiB", out.cacheBudgetMiB, 1.0, 4096.0, nullptr);
    if (node["referenceHeight"]) {
        double height = out.referenceHeight;
        number(node["referenceHeight"], height, 0.0, 8192.0, "referenceHeight");
        out.referenceHeight = height > 0.0 && height < 120.0 ? 120 : (int)height;
    }
    try {
        if (node["simulateExportCharacters"]) out.simulateExportCharacters = node["simulateExportCharacters"].as<bool>();
    } catch (const std::exception&) {
        if (warnings) warnings->push_back("titleArt.simulateExportCharacters is not true or false");
    }
    if (const YAML::Node tiers = node["realtime3DQuality"]; tiers && tiers.IsMap()) {
        for (const char* platform : kTitleArtPlatforms) {
            const YAML::Node value = tiers[platform];
            if (!value) continue;
            const std::string tier = value.IsScalar() ? value.Scalar() : std::string();
            if (isTitleArtRealtimeTier(tier)) *out.realtime3DQuality.find(platform) = tier;
            else if (warnings) warnings->push_back("titleArt.realtime3DQuality." + std::string(platform) + " \"" + tier + "\" is not high, medium or low");
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
