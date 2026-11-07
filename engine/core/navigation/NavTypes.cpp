// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavTypes.h"
#include "NavHash.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    // the fixed rows and the user rows' defaults (sRGB)
    AreaSettings defaultArea(int index) {
        AreaSettings area;
        switch (index) {
            case AREA_WALKABLE:
                area.name = "Walkable";
                area.cost = 1.0f;
                area.color = Vector4(0.0f, 0.75f, 1.0f, 1.0f);
                break;
            case AREA_NOT_WALKABLE:
                area.name = "Not Walkable";
                area.cost = 1.0f;
                area.color = Vector4(0.9f, 0.2f, 0.2f, 1.0f);
                break;
            case AREA_JUMP:
                area.name = "Jump";
                area.cost = 2.0f;
                area.color = Vector4(0.95f, 0.75f, 0.1f, 1.0f);
                break;
            default: {
                area.name = "Area " + std::to_string(index);
                area.cost = 1.0f;
                // a hue wheel for the user areas, so neighbouring indices differ
                const float hue = std::fmod(0.13f + 0.618034f * (float)index, 1.0f) * 6.0f;
                const int sector = (int)hue;
                const float f = hue - (float)sector;
                const float s = 0.65f, v = 0.9f;
                const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
                float r = v, g = t, b = p;
                switch (sector % 6) {
                    case 0: r = v; g = t; b = p; break;
                    case 1: r = q; g = v; b = p; break;
                    case 2: r = p; g = v; b = t; break;
                    case 3: r = p; g = q; b = v; break;
                    case 4: r = t; g = p; b = v; break;
                    default: r = v; g = p; b = q; break;
                }
                area.color = Vector4(r, g, b, 1.0f);
                break;
            }
        }
        return area;
    }

    std::mutex& settingsMutex() {
        static std::mutex mutex;
        return mutex;
    }

    NavigationSettings& settingsInstance() {
        static NavigationSettings settings = NavigationSettings::defaults();
        return settings;
    }

    std::atomic<uint64_t> settingsVersion{1};

}

NavigationSettings::NavigationSettings() {
    for (int i = 0; i < AREA_COUNT; i++)
        areas[(size_t)i] = defaultArea(i);
    agentTypes.push_back(AgentTypeSettings());
}

NavigationSettings NavigationSettings::defaults() {
    return NavigationSettings();
}

const AgentTypeSettings* NavigationSettings::findAgentType(int id) const {
    for (const AgentTypeSettings& type : agentTypes) {
        if (type.id == id)
            return &type;
    }
    return nullptr;
}

const AgentTypeSettings* NavigationSettings::findAgentType(const std::string& name) const {
    for (const AgentTypeSettings& type : agentTypes) {
        if (type.name == name)
            return &type;
    }
    return nullptr;
}

AgentTypeSettings NavigationSettings::agentTypeOrDefault(int id) const {
    if (const AgentTypeSettings* type = findAgentType(id))
        return *type;
    if (!agentTypes.empty())
        return agentTypes.front();
    return AgentTypeSettings();
}

int NavigationSettings::nextAgentTypeId() const {
    int id = 0;
    while (findAgentType(id))
        id++;
    return id;
}

int NavigationSettings::findArea(const std::string& name) const {
    for (int i = 0; i < AREA_COUNT; i++) {
        if (areas[(size_t)i].name == name)
            return i;
    }
    return -1;
}

void NavigationSettings::sanitize() {
    for (int i = 0; i < AREA_COUNT; i++) {
        AreaSettings& area = areas[(size_t)i];
        if (!(area.cost >= 1.0f) || !std::isfinite(area.cost))
            area.cost = 1.0f;
        if (area.name.empty())
            area.name = defaultArea(i).name;
    }
    // the fixed rows keep their names (and Not Walkable has no cost)
    for (int i = 0; i < FIRST_USER_AREA; i++)
        areas[(size_t)i].name = defaultArea(i).name;
    areas[AREA_NOT_WALKABLE].cost = 1.0f;

    if (agentTypes.empty())
        agentTypes.push_back(AgentTypeSettings());
    std::set<int> ids;
    for (AgentTypeSettings& type : agentTypes) {
        if (type.id < 0 || !ids.insert(type.id).second) {
            int id = 0;
            while (ids.count(id))
                id++;
            type.id = id;
            ids.insert(id);
        }
        if (type.name.empty())
            type.name = "Agent " + std::to_string(type.id);
        if (!(type.radius > 0.0f) || !std::isfinite(type.radius)) type.radius = 0.5f;
        if (!(type.height > 0.0f) || !std::isfinite(type.height)) type.height = 2.0f;
        if (!(type.stepHeight >= 0.0f) || !std::isfinite(type.stepHeight)) type.stepHeight = 0.4f;
        if (!std::isfinite(type.maxSlope)) type.maxSlope = 45.0f;
        type.maxSlope = std::clamp(type.maxSlope, 0.0f, 89.9f);
    }
    crowdMaxAgents = std::clamp(crowdMaxAgents, 1, 4096);
    if (!(webBakeBudgetMs > 0.0f) || !std::isfinite(webBakeBudgetMs))
        webBakeBudgetMs = 4.0f;
    webBakeBudgetMs = std::clamp(webBakeBudgetMs, 0.5f, 100.0f);
    maxGridSolvesPerTick = std::clamp(maxGridSolvesPerTick, 1, 100000);
}

bool NavigationSettings::operator==(const NavigationSettings& other) const {
    return areas == other.areas && agentTypes == other.agentTypes && crowdMaxAgents == other.crowdMaxAgents &&
           webBakeBudgetMs == other.webBakeBudgetMs && maxGridSolvesPerTick == other.maxGridSolvesPerTick;
}

const NavigationSettings& nav::getSettings() {
    return settingsInstance();
}

void nav::setSettings(const NavigationSettings& settings) {
    NavigationSettings sanitized = settings;
    sanitized.sanitize();
    std::lock_guard<std::mutex> lock(settingsMutex());
    if (settingsInstance() == sanitized)
        return;
    settingsInstance() = sanitized;
    settingsVersion.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t nav::getSettingsVersion() {
    return settingsVersion.load(std::memory_order_acquire);
}

uint64_t nav::hashAreaTable(const NavigationSettings& settings) {
    NavHasher hasher;
    for (const AreaSettings& area : settings.areas) {
        hasher.string(area.name);
        hasher.f32(area.cost);
        hasher.f32(area.color.x);
        hasher.f32(area.color.y);
        hasher.f32(area.color.z);
        hasher.f32(area.color.w);
    }
    return hasher.value();
}

const char* nav::pathStatusName(PathStatus status) {
    switch (status) {
        case PathStatus::Complete: return "complete";
        case PathStatus::Partial: return "partial";
        case PathStatus::Invalid: return "invalid";
    }
    return "invalid";
}

// ---------------------------------------------------------------- S54: path failures

namespace {

    struct FailureText {
        PathFailure failure;
        const char* name;
        const char* description;
    };

    // in enum order (NavFailureTest.NamesAreTheContract)
    constexpr FailureText FAILURES[] = {
        {PathFailure::None, "none", "The path reaches the end."},
        {PathFailure::NoSurface, "no_surface", "No enabled NavMesh Surface of this agent type is in the scene."},
        {PathFailure::NotBaked, "not_baked", "The NavMesh Surfaces of this agent type have no navmesh: bake them."},
        {PathFailure::StartOffMesh, "start_off_mesh", "The start is not on the navmesh: no polygon is near it."},
        {PathFailure::EndOffMesh, "end_off_mesh", "The end is not on the navmesh: no polygon is near it."},
        {PathFailure::StartFiltered, "start_filtered", "The start is on an area the area mask leaves out."},
        {PathFailure::EndFiltered, "end_filtered", "The end is on an area the area mask leaves out."},
        {PathFailure::Disconnected, "disconnected",
         "The start and the end are on parts of the navmesh that do not connect (or only through one-way links)."},
        {PathFailure::BlockedByArea, "blocked_by_area", "The start and the end connect only through areas the area mask leaves out."},
        {PathFailure::OutOfNodes, "out_of_nodes",
         "The search ran out of nodes before reaching the end: the path is longer than 2048 polygons."},
    };

}

const char* nav::pathFailureName(PathFailure failure) {
    for (const FailureText& text : FAILURES) {
        if (text.failure == failure)
            return text.name;
    }
    return "none";
}

const char* nav::pathFailureDescription(PathFailure failure) {
    for (const FailureText& text : FAILURES) {
        if (text.failure == failure)
            return text.description;
    }
    return FAILURES[0].description;
}

bool nav::pathFailureFromName(const std::string& name, PathFailure& out) {
    for (const FailureText& text : FAILURES) {
        if (name == text.name) {
            out = text.failure;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- S55 queries: area costs

float nav::clampAreaCost(float cost) {
    if (cost >= 1.0f && std::isfinite(cost))
        return cost;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        Log::warn("Navigation: an area cost below 1 (%g) is used as 1 (A*'s estimate needs costs >= 1)", (double)cost);
    return 1.0f;
}

void QueryFilter::setAreaCost(int area, float cost) {
    if (area < 0 || area >= AREA_COUNT)
        return;
    areaCosts[(size_t)area] = clampAreaCost(cost);
    costMask = (uint16_t)(costMask | (1u << area));
}

float QueryFilter::getAreaCost(int area) const {
    if (area < 0 || area >= AREA_COUNT)
        return 0.0f;
    if (costMask & (1u << area))
        return areaCosts[(size_t)area];
    return getSettings().areas[(size_t)area].cost;
}

void QueryFilter::resetAreaCost(int area) {
    if (area < 0 || area >= AREA_COUNT)
        return;
    costMask = (uint16_t)(costMask & ~(1u << area));
    areaCosts[(size_t)area] = 0.0f;
}
