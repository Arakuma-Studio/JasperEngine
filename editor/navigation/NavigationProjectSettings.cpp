// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationProjectSettings.h"

#include "Factory.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    template <typename T>
    T readOr(const YAML::Node& node, const char* key, T fallback) {
        const YAML::Node value = node[key];
        if (!value.IsDefined() || value.IsNull()) return fallback;
        try {
            return value.as<T>();
        } catch (const YAML::Exception&) {
            return fallback;
        }
    }

    float readFinite(const YAML::Node& node, const char* key, float fallback) {
        const float value = readOr<float>(node, key, fallback);
        return std::isfinite(value) ? value : fallback;
    }

    YAML::Node encodeColor(const Vector4& color) {
        YAML::Node node;
        node.SetStyle(YAML::EmitterStyle::Flow);
        node.push_back(color.x);
        node.push_back(color.y);
        node.push_back(color.z);
        node.push_back(color.w);
        return node;
    }

    Vector4 decodeColor(const YAML::Node& node, const Vector4& fallback) {
        // a missing key is an invalid node: asking it anything else throws
        if (!node.IsDefined() || !node.IsSequence() || node.size() < 3) return fallback;
        try {
            float c[4] = {node[0].as<float>(), node[1].as<float>(), node[2].as<float>(), node.size() > 3 ? node[3].as<float>() : 1.0f};
            for (float& value : c) {
                if (!std::isfinite(value)) return fallback;
                value = std::clamp(value, 0.0f, 1.0f);
            }
            return Vector4(c[0], c[1], c[2], c[3]);
        } catch (const YAML::Exception&) {
            return fallback;
        }
    }

    std::string quoted(const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

}

YAML::Node editor::encodeNavigationProjectSettings(const NavigationProjectSettings& project) {
    const nav::NavigationSettings defaults;
    const nav::NavigationSettings& settings = project.settings;
    YAML::Node node;
    if (!project.includeInBuilds) node["includeInBuilds"] = false;
    if (settings.crowdMaxAgents != defaults.crowdMaxAgents) node["crowdMaxAgents"] = settings.crowdMaxAgents;
    if (settings.webBakeBudgetMs != defaults.webBakeBudgetMs) node["webBakeBudgetMs"] = settings.webBakeBudgetMs;
    if (settings.maxGridSolvesPerTick != defaults.maxGridSolvesPerTick) node["maxGridSolvesPerTick"] = settings.maxGridSolvesPerTick;
    if (settings.carveTilesPerTick != defaults.carveTilesPerTick) node["carveTilesPerTick"] = settings.carveTilesPerTick;
    if (settings.agentTypes != defaults.agentTypes) {
        YAML::Node agents;
        for (const nav::AgentTypeSettings& agent : settings.agentTypes) {
            YAML::Node row;
            row["id"] = agent.id;
            row["name"] = agent.name;
            row["radius"] = agent.radius;
            row["height"] = agent.height;
            row["stepHeight"] = agent.stepHeight;
            row["maxSlope"] = agent.maxSlope;
            agents.push_back(row);
        }
        node["agentTypes"] = agents;
    }
    YAML::Node areas;
    for (int i = 0; i < nav::AREA_COUNT; i++) {
        const nav::AreaSettings& area = settings.areas[(size_t)i];
        const nav::AreaSettings& fallback = defaults.areas[(size_t)i];
        if (area == fallback) continue;
        YAML::Node row;
        row["index"] = i;
        if (area.name != fallback.name) row["name"] = area.name;
        if (area.cost != fallback.cost) row["cost"] = area.cost;
        if (area.color != fallback.color) row["color"] = encodeColor(area.color);
        areas.push_back(row);
    }
    if (areas.size() > 0) node["areas"] = areas;
    return node;
}

NavigationProjectSettings editor::decodeNavigationProjectSettings(const YAML::Node& node) {
    NavigationProjectSettings project;
    if (!node.IsDefined() || !node.IsMap()) return project;
    nav::NavigationSettings& settings = project.settings;
    project.includeInBuilds = readOr<bool>(node, "includeInBuilds", true);
    settings.crowdMaxAgents = std::clamp(readOr<int>(node, "crowdMaxAgents", settings.crowdMaxAgents), 1, 4096);
    settings.webBakeBudgetMs = std::clamp(readFinite(node, "webBakeBudgetMs", settings.webBakeBudgetMs), 0.5f, 50.0f);
    settings.maxGridSolvesPerTick = std::clamp(readOr<int>(node, "maxGridSolvesPerTick", settings.maxGridSolvesPerTick), 1, 4096);
    // 0: Auto (S74 R14; projects before it have no key)
    settings.carveTilesPerTick = std::clamp(readOr<int>(node, "carveTilesPerTick", settings.carveTilesPerTick), 0, 64);
    const YAML::Node agents = node["agentTypes"];
    if (agents.IsDefined() && agents.IsSequence() && agents.size() > 0) {
        settings.agentTypes.clear();
        for (const YAML::Node& row : agents) {
            if (!row.IsDefined() || !row.IsMap()) continue;
            nav::AgentTypeSettings agent;
            agent.id = readOr<int>(row, "id", settings.nextAgentTypeId());
            agent.name = readOr<std::string>(row, "name", agent.name);
            agent.radius = readFinite(row, "radius", agent.radius);
            agent.height = readFinite(row, "height", agent.height);
            agent.stepHeight = readFinite(row, "stepHeight", agent.stepHeight);
            agent.maxSlope = readFinite(row, "maxSlope", agent.maxSlope);
            settings.agentTypes.push_back(agent);
        }
    }
    const YAML::Node areas = node["areas"];
    if (areas.IsDefined() && areas.IsSequence()) {
        for (const YAML::Node& row : areas) {
            if (!row.IsDefined() || !row.IsMap()) continue;
            const int index = readOr<int>(row, "index", -1);
            if (index < 0 || index >= nav::AREA_COUNT) continue;
            nav::AreaSettings& area = settings.areas[(size_t)index];
            area.name = readOr<std::string>(row, "name", area.name);
            area.cost = readFinite(row, "cost", area.cost);
            area.color = decodeColor(row["color"], area.color);
        }
    }
    settings.sanitize();
    return project;
}

std::string editor::generateNavigationSettingsCode(const nav::NavigationSettings& settings, const std::string& indent) {
    if (settings == nav::NavigationSettings()) return "";
    const std::string in = indent + "    ";
    std::string code = indent + "{\n";
    code += in + "nav::NavigationSettings navigation;\n";
    code += in + "navigation.crowdMaxAgents = " + std::to_string(settings.crowdMaxAgents) + ";\n";
    code += in + "navigation.webBakeBudgetMs = " + Factory::formatFloat(settings.webBakeBudgetMs) + ";\n";
    code += in + "navigation.maxGridSolvesPerTick = " + std::to_string(settings.maxGridSolvesPerTick) + ";\n";
    if (settings.carveTilesPerTick != 0) code += in + "navigation.carveTilesPerTick = " + std::to_string(settings.carveTilesPerTick) + ";\n";
    code += in + "navigation.agentTypes.clear();\n";
    for (const nav::AgentTypeSettings& agent : settings.agentTypes) {
        code += in + "navigation.agentTypes.push_back({" + std::to_string(agent.id) + ", " + quoted(agent.name) + ", " +
                Factory::formatFloat(agent.radius) + ", " + Factory::formatFloat(agent.height) + ", " +
                Factory::formatFloat(agent.stepHeight) + ", " + Factory::formatFloat(agent.maxSlope) + "});\n";
    }
    const nav::NavigationSettings defaults;
    for (int i = 0; i < nav::AREA_COUNT; i++) {
        const nav::AreaSettings& area = settings.areas[(size_t)i];
        if (area == defaults.areas[(size_t)i]) continue;
        code += in + "navigation.areas[" + std::to_string(i) + "] = {" + quoted(area.name) + ", " + Factory::formatFloat(area.cost) +
                ", Vector4(" + Factory::formatFloat(area.color.x) + ", " + Factory::formatFloat(area.color.y) + ", " +
                Factory::formatFloat(area.color.z) + ", " + Factory::formatFloat(area.color.w) + ")};\n";
    }
    code += in + "nav::setSettings(navigation);\n";
    code += indent + "}\n";
    return code;
}
