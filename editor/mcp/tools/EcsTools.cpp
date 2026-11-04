// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S07: the ECS of a scene: get_ecs_stats (entities, component arrays, memory, change tick),
// get_system_schedule (phases, waves, timings, the last wave trace), set_system_options
// (Play-mode debug switches) and query_entities (typed queries, changed since a tick)
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Catalog.h"
#include "Project.h"
#include "Scene.h"
#include "util/EntityPath.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

// The editor scene, or with runtime: true the scene the running game uses
SceneProject* ecsScene(const CallPtr& call){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const Json ref = args.contains("scene") ? args["scene"] : Json();
    std::string error;
    SceneProject* scene = args.value("runtime", false)
        ? w3::resolveRuntimeScene(project, ref, error)
        : McpRefs::resolveScene(project, ref, error);
    if (!scene){
        call->fail(error);
        return nullptr;
    }
    if (!scene->scene){
        call->fail(McpRefs::notOpenError(project, *scene));
        return nullptr;
    }
    return scene;
}

// The tool name of a registered type ("mesh"), its registration name for types the editor
// does not list
std::string typeName(const EntityRegistry* registry, ComponentId id){
    const std::string& name = registry->getComponentName(id);
    try {
        return McpRefs::componentName(Catalog::getComponentType(name));
    } catch (const std::exception&) {
        return name.empty() ? "type" + std::to_string(id) : name;
    }
}

Json typeNames(const EntityRegistry* registry, const Signature& types){
    Json names = Json::array();
    types.forEachSet([&](size_t id){ names.push_back(typeName(registry, (ComponentId)id)); });
    return names;
}

const char* orderName(ComponentOrder order){
    switch (order){
        case ComponentOrder::Hierarchy: return "hierarchy";
        case ComponentOrder::Manual: return "manual";
        case ComponentOrder::Unordered: return "unordered";
        case ComponentOrder::Insertion: break;
    }
    return "insertion";
}

Json resourceNames(uint32_t resources){
    Json names = Json::array();
    const std::pair<uint32_t, const char*> all[] = {
        {SystemResource::MainThread, "main_thread"}, {SystemResource::Lua, "lua"}, {SystemResource::GPU, "gpu"},
        {SystemResource::Audio, "audio"}, {SystemResource::Physics, "physics"}, {SystemResource::Structural, "structural"},
        {SystemResource::Exclusive, "exclusive"}
    };
    for (const auto& [bit, name] : all){
        if (resources & bit) names.push_back(name);
    }
    return names;
}

// component names of an argument → registry ids; false (call failed) on a bad name
bool componentIds(const CallPtr& call, SceneProject* scene, const Json& refs, std::vector<ComponentId>& ids){
    if (refs.is_null()) return true;
    if (!refs.is_array()){
        call->fail("Expected an array of component names (list_component_types).");
        return false;
    }
    for (const Json& ref : refs){
        ComponentType type;
        std::string error;
        if (!McpRefs::resolveComponent(ref, type, error)){
            call->fail(error);
            return false;
        }
        try {
            ids.push_back(Catalog::getComponentId(scene->scene, type));
        } catch (const std::exception&) {
            call->fail("The scene has no " + McpRefs::componentName(type) + " components (the type is not in this build).");
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ get_ecs_stats

void getEcsStatsTool(const CallPtr& call){
    SceneProject* sp = ecsScene(call);
    if (!sp) return;
    Scene* scene = sp->scene;

    Json types = Json::array();
    size_t holes = 0, dirty = 0, componentBytes = 0, overheadBytes = 0, heapBytes = 0, components = 0;
    for (size_t id = 0; id < scene->getComponentTypeCount(); id++){
        ComponentArrayBase* array = scene->getComponentArray((ComponentId)id);
        holes += array->getHoleCount();
        dirty += array->isOrderDirty() ? 1 : 0;
        componentBytes += array->componentBytes();
        overheadBytes += array->overheadBytes();
        components += array->size();
        if (array->size() == 0 && array->getHoleCount() == 0) continue;
        // what the components hold in arrays of their own (a mesh's submeshes, a sprite's frames)
        const size_t heap = array->arrayHeapBytes();
        heapBytes += heap;
        types.push_back({{"name", typeName(scene, (ComponentId)id)}, {"count", array->size()}, {"holes", array->getHoleCount()},
                         {"order", orderName(array->getOrder())}, {"order_dirty", array->isOrderDirty()},
                         {"bytes", array->componentBytes() + array->overheadBytes()}, {"element_bytes", array->componentSize()},
                         {"heap_bytes", heap}});
    }
    const size_t entities = scene->getEntityCount();
    const size_t systemEntities = scene->getSystemEntityCount();
    const size_t metadataBytes = scene->getMetadataBytes();
    const Tick tick = scene->changeTick();
    Json data = {
        {"scene", McpRefs::sceneJson(call->ctx().project(), *sp)},
        {"entities", {{"total", entities}, {"user", entities - systemEntities}, {"system", systemEntities}}},
        {"component_types", {{"registered", scene->getComponentTypeCount()}, {"max", kMaxComponentTypes}}},
        {"components", components},
        {"types", types},
        {"holes", holes},
        {"dirty_arrays", dirty},
        {"bytes", {{"components", componentBytes}, {"overhead", overheadBytes}, {"metadata", metadataBytes},
                   {"heap", heapBytes}, {"total", componentBytes + overheadBytes + metadataBytes + heapBytes}}},
        {"change_tick", tick}
    };
    call->ok(std::to_string(entities) + " entities (" + std::to_string(systemEntities) + " system), " +
             std::to_string(components) + " components in " + std::to_string(types.size()) + " of " +
             std::to_string(scene->getComponentTypeCount()) + " registered types (max " + std::to_string(kMaxComponentTypes) +
             "), " + std::to_string(holes) + " pending holes, " + std::to_string(dirty) + " arrays waiting to re-sort, " +
             std::to_string((componentBytes + overheadBytes + metadataBytes + heapBytes) / 1024) + " KiB (" +
             std::to_string(heapBytes / 1024) + " KiB in component arrays). change_tick " + std::to_string(tick) +
             " (pass it to query_entities changed_since_tick).", data);
}

// ------------------------------------------------------------------ get_system_schedule

void getSystemScheduleTool(const CallPtr& call){
    SceneProject* sp = ecsScene(call);
    if (!sp) return;
    Scene* scene = sp->scene;
    SystemScheduler& scheduler = scene->getScheduler();

    Json phases = Json::array();
    std::string text;
    for (SystemPhase phase : {SystemPhase::FixedUpdate, SystemPhase::Update, SystemPhase::PostUpdate, SystemPhase::Draw}){
        Json waves = Json::array();
        const auto& plan = scheduler.getWaves(phase);
        for (const auto& wave : plan){
            Json systems = Json::array();
            for (const ScheduledSystem* system : wave){
                const SystemDescriptor& d = system->getDescriptor();
                const SystemTiming& timing = system->getTiming();
                systems.push_back({
                    {"name", d.name}, {"reads", typeNames(scene, d.reads)}, {"writes", typeNames(scene, d.writes)},
                    {"resources", resourceNames(d.resources)}, {"depends_on", d.dependsOn},
                    {"every_n_frames", system->getEveryNFrames()}, {"enabled", system->isEnabled()},
                    {"exclusive", system->isExclusive()},
                    {"timing", {{"last_ms", timing.lastMs()}, {"avg_ms", timing.averageMs()}, {"max_ms", timing.maxMs()},
                                {"calls", timing.callCount()}}}
                });
                if (text.size() < 6000){
                    char line[256];
                    std::snprintf(line, sizeof(line), "\n%s wave %u: %s%s avg %.3f ms, max %.3f ms, %llu calls", systemPhaseName(phase),
                                  system->getWave(), d.name.c_str(), system->isEnabled() ? "" : " (disabled)",
                                  timing.averageMs(), timing.maxMs(), (unsigned long long)timing.callCount());
                    text += line;
                }
            }
            waves.push_back(systems);
        }
        phases.push_back({{"phase", systemPhaseName(phase)}, {"waves", waves}});
    }

    Json trace = Json::array();
    for (const WaveTraceEntry& entry : scheduler.getLastTrace()){
        trace.push_back({{"system", *entry.system}, {"phase", systemPhaseName(entry.phase)}, {"wave", entry.wave},
                         {"start_us", entry.startUs}, {"end_us", entry.endUs}});
    }
    Json data = {{"scene", McpRefs::sceneJson(call->ctx().project(), *sp)}, {"phases", phases}, {"last_trace", trace},
                 {"serial", scheduler.isSerial()}, {"systems", scheduler.getSystems().size()}};
    call->ok("Schedule of \"" + sp->name + "\" (" + std::to_string(scheduler.getSystems().size()) + " phase entries; " +
             (scheduler.isSerial() ? "waves run serially" : "waves run on job workers") + "):" + text, data);
}

// ------------------------------------------------------------------ set_system_options

void setSystemOptionsTool(const CallPtr& call){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp || !sp->scene){
        call->fail(error + " set_system_options is Play-mode debug state (not scene data): it resets when Play stops.");
        return;
    }
    SystemScheduler& scheduler = sp->scene->getScheduler();
    const std::string name = args.value("name", "");
    std::vector<std::string> names;
    for (const auto& system : scheduler.getSystems()){
        if (std::find(names.begin(), names.end(), system->getName()) == names.end()) names.push_back(system->getName());
    }
    if (std::find(names.begin(), names.end(), name) == names.end()){
        std::string list;
        for (const std::string& n : names) list += " " + n;
        call->fail("No system \"" + name + "\". Systems:" + list + ".");
        return;
    }
    if (!args.contains("enabled") && !args.contains("every_n_frames")){
        call->fail("Pass enabled and/or every_n_frames.");
        return;
    }
    if (args.contains("enabled")) scheduler.setEnabled(name, args["enabled"].get<bool>());
    if (args.contains("every_n_frames")) scheduler.setEveryNFrames(name, (uint32_t)args["every_n_frames"].get<int>());

    bool enabled = true;
    uint32_t every = 1;
    for (const auto& system : scheduler.getSystems()){
        if (system->getName() == name){
            enabled = system->isEnabled();
            every = system->getEveryNFrames();
            break;
        }
    }
    call->ok(name + " is " + (enabled ? "enabled" : "disabled") + ", every " + std::to_string(every) +
             " frame(s), until Play stops.", Json{{"name", name}, {"enabled", enabled}, {"every_n_frames", every}});
}

// ------------------------------------------------------------------ query_entities

void queryEntitiesTool(const CallPtr& call){
    const Json& args = call->args();
    SceneProject* sp = ecsScene(call);
    if (!sp) return;
    std::vector<ComponentId> include, exclude;
    if (!componentIds(call, sp, args.contains("components") ? args["components"] : Json(), include)) return;
    if (!componentIds(call, sp, args.contains("exclude") ? args["exclude"] : Json(), exclude)) return;
    if (include.empty()){
        call->fail("components needs at least one component name; the first one sets the order.");
        return;
    }
    const size_t limit = (size_t)std::clamp(args.value("limit", 200), 1, 10000);
    Tick since = 0;
    const bool changed = args.contains("changed_since_tick");
    if (changed){
        since = (Tick)std::max<long long>(0, args["changed_since_tick"].get<long long>());
    }
    // one more than asked tells whether the list was cut
    std::vector<Entity> found = sp->scene->queryEntities(include, exclude, changed ? &since : nullptr, limit + 1);
    const bool more = found.size() > limit;
    if (more) found.resize(limit);

    EntityPath::Cache cache;
    Json list = Json::array();
    std::string text;
    for (Entity entity : found){
        const std::string path = EntityPath::of(sp->scene, entity, &cache);
        list.push_back({{"id", entity}, {"name", sp->scene->getEntityName(entity)}, {"path", path}});
        if (text.size() < 6000) text += "\n" + std::to_string(entity) + " " + path;
    }
    Json data = {{"scene", McpRefs::sceneJson(call->ctx().project(), *sp)}, {"entities", list}, {"count", list.size()},
                 {"more", more}, {"change_tick", sp->scene->changeTick()}};
    call->ok(std::to_string(list.size()) + (more ? "+" : "") + " GameObject(s), in the order of the first component's array:" + text, data);
}

}

void registerEcsTools(McpRegistry& registry){
    const Json runtime = boolean("The running game's scene.");
    {
        ToolDef tool;
        tool.name = "get_ecs_stats";
        tool.title = "Get ECS Stats";
        tool.description = "Entity and component counts (per type, holes, order), types used of 256, memory, change_tick.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"runtime", runtime}});
        tool.annotations.readOnly = true;
        tool.handler = getEcsStatsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_system_schedule";
        tool.title = "Get System Schedule";
        tool.description = "Systems per phase and wave: reads/writes, resources, enabled, every_n_frames, timings, last wave trace.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"runtime", runtime}});
        tool.annotations.readOnly = true;
        tool.handler = getSystemScheduleTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_system_options";
        tool.title = "Set System Options";
        tool.description = "Play only: enable/disable a system or run it every N frames, until Play stops.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"name", string("")},
            {"enabled", boolean("")},
            {"every_n_frames", integer("", 1, 1000)}
        }, {"name"});
        tool.handler = setSystemOptionsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "query_entities";
        tool.title = "Query Entities";
        tool.description =
            "GameObjects with every component and none of exclude, in the first one's array order; "
            "changed_since_tick: changed after a get_ecs_stats change_tick.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"components", array(componentName(), "")},
            {"exclude", array(componentName(), "")},
            {"changed_since_tick", integer("")},
            {"runtime", runtime},
            {"limit", withDefault(integer("", 1, 10000), 200)}
        }, {"components"});
        tool.annotations.readOnly = true;
        tool.handler = queryEntitiesTool;
        registry.addTool(std::move(tool));
    }
}

}
