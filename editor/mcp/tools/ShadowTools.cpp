// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shadow tools (spec S10 R30, docs/mcp/guide/shadows.md): get_shadow_stats reads what the last
// update's shadow passes of a scene did (tier, atlases, memory, per light and slot the fit and
// the cache path); set_shadow_debug moves only the camera the cascades are fitted to, holds or
// redraws the cache (editor only, transient: the stability tests).

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "subsystem/RenderSystem.h"
#include "render/shadow/ShadowTiers.h"
#include "component/LightComponent.h"

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

const char* lightTypeName(LightType type){
    switch (type){
        case LightType::DIRECTIONAL: return "directional";
        case LightType::POINT: return "point";
        case LightType::SPOT: return "spot";
    }
    return "directional";
}

Json rectJson(const ShadowRect& rect){
    return Json::array({rect.x, rect.y, rect.width, rect.height});
}

// the scene whose stats are read: the editor's, or its running game's ("game")
SceneProject* statsScene(const CallPtr& call){
    Project* project = call->ctx().project();
    std::string error;
    const Json& args = call->args();
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene){
        call->fail(error);
        return nullptr;
    }
    if (args.value("camera", std::string("scene")) == "game")
        scene = McpRefs::runtimeScene(project, scene);
    if (!scene->scene){
        call->fail(McpRefs::notOpenError(project, *scene));
        return nullptr;
    }
    return scene;
}

void getShadowStatsTool(const CallPtr& call){
    const std::string camera = call->args().value("camera", std::string("scene"));
    if (camera != "scene" && camera != "game"){
        call->fail("camera must be \"scene\" (the editor's view) or \"game\" (the running game's).");
        return;
    }
    SceneProject* sceneProject = statsScene(call);
    if (!sceneProject) return;
    RenderSystem* render = sceneProject->scene->getSystem<RenderSystem>().get();
    const RenderSystem::ShadowStats& stats = render->getShadowStats();

    static const char* const pathNames[] = {"skip", "restore", "static", "full", "uncached"};
    Json draws = Json::object();
    uint32_t totalDraws = 0;
    for (int p = 0; p < 5; p++){
        draws[pathNames[p]] = stats.drawCalls[p];
        totalDraws += stats.drawCalls[p];
    }

    Json lights = Json::array();
    for (const RenderSystem::ShadowLightStats& light : stats.lights){
        Json entry = {
            {"entity", McpRefs::entityJson(sceneProject, light.entity)},
            {"type", lightTypeName(light.type)},
            {"direction", McpValues::toJson(light.direction)},
            {"first_slot", light.firstSlot},
        };
        Json resolutions = Json::array();
        const int maps = (light.type == LightType::POINT) ? 6 : ((light.type == LightType::DIRECTIONAL) ? (int)light.cascades : 1);
        for (int m = 0; m < maps && m < 6; m++)
            resolutions.push_back(light.resolution[m]);
        entry["resolution"] = resolutions;
        if (light.type == LightType::DIRECTIONAL){
            entry["cascades"] = light.cascades;
            entry["shadow_distance"] = light.shadowDistance;
            Json splits = Json::array(), radius = Json::array(), texel = Json::array();
            for (unsigned int c = 0; c < light.cascades && c < MAX_SHADOWCASCADES; c++){
                splits.push_back(light.splitFar[c]);
                radius.push_back(light.radius[c]);
                texel.push_back(light.texelWorld[c]);
            }
            entry["cascade_splits"] = splits;
            entry["radius"] = radius;
            entry["texel_world"] = texel;
        }
        Json slots = Json::array();
        for (const RenderSystem::ShadowSlotStats& slot : light.slots){
            Json slotJson = {
                {"slot", slot.slot},
                {"atlas", slot.point ? "point" : "projective"},
                {"rect", rectJson(slot.rect)},
                {"resolution", slot.resolution},
                {"path", shadowSlotPathName(slot.path)},
                {"dirty_cause", shadowDirtyCauseName(slot.cause)},
                {"frames_since_render", slot.framesSinceRender},
                {"last_render", {{"path", shadowSlotPathName(slot.renderPath)},
                                 {"dirty_cause", shadowDirtyCauseName(slot.renderCause)},
                                 {"dirty_rect", rectJson(slot.dirtyRect)}}},
                {"static_layer", slot.staticLayer},
            };
            if (slot.path == ShadowSlotPath::RESTORE_DYNAMIC)
                slotJson["dirty_rect"] = rectJson(slot.dirtyRect);
            slots.push_back(slotJson);
        }
        entry["slots"] = slots;
        lights.push_back(entry);
    }

    Json result = {
        {"scene_id", sceneProject->id},
        {"camera", camera},
        {"tier", ShadowTiers::getName(stats.tier)},
        {"hard_only", stats.hardOnly},
        {"atlas", {{"projective", {stats.projectiveWidth, stats.projectiveHeight}},
                   {"point", {stats.pointWidth, stats.pointHeight}},
                   {"static", {stats.staticWidth, stats.staticHeight}},
                   {"static_point", {stats.staticPointWidth, stats.staticPointHeight}}}},
        {"memory_bytes", stats.memoryBytes},
        {"memory_budget_bytes", ShadowTiers::getSettings(stats.tier).memoryBytes},
        {"update", stats.frame},
        {"passes", stats.passes},
        {"draw_calls", draws},
        {"draw_calls_total", totalDraws},
        // GPU pass timings come with the profiling stream (S15); until then CPU-side counts only
        {"gpu_ms", nullptr},
        {"lights", lights},
    };
    std::string text = "Shadows of \"" + sceneProject->name + "\": tier " + ShadowTiers::getName(stats.tier) +
        (stats.hardOnly ? " (hard only)" : "") + ", " + std::to_string(stats.lights.size()) + " shadowed light(s), " +
        std::to_string(stats.passes) + " pass(es), " + std::to_string(totalDraws) + " draw(s), " +
        std::to_string(stats.memoryBytes / (1024 * 1024)) + " MB.";
    call->ok(text, result);
}

bool readVector3(const Json& value, Vector3& out){
    if (!value.is_array() || value.size() != 3)
        return false;
    for (int i = 0; i < 3; i++){
        if (!value[i].is_number())
            return false;
    }
    out = Vector3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
    return true;
}

void setShadowDebugTool(const CallPtr& call){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject){
        call->fail(error);
        return;
    }
    if (!sceneProject->scene){
        call->fail(McpRefs::notOpenError(project, *sceneProject));
        return;
    }
    RenderSystem* render = sceneProject->scene->getSystem<RenderSystem>().get();
    RenderSystem::ShadowDebugSettings settings = render->getShadowDebug();
    if (args.contains("fit_camera_offset")){
        if (!readVector3(args["fit_camera_offset"], settings.fitCameraOffset)){
            call->fail("fit_camera_offset must be [x, y, z] (world units).");
            return;
        }
    }
    if (args.contains("fit_camera_euler")){
        if (!readVector3(args["fit_camera_euler"], settings.fitCameraEuler)){
            call->fail("fit_camera_euler must be [pitch, yaw, roll] (degrees).");
            return;
        }
    }
    if (args.contains("freeze_cache")){
        if (!args["freeze_cache"].is_boolean()){
            call->fail("freeze_cache must be a boolean.");
            return;
        }
        settings.freezeCache = args["freeze_cache"].get<bool>();
    }
    if (args.contains("force_rerender")){
        if (!args["force_rerender"].is_boolean()){
            call->fail("force_rerender must be a boolean.");
            return;
        }
        settings.forceRerender = args["force_rerender"].get<bool>();
    }
    render->setShadowDebug(settings);
    sceneProject->needUpdateRender = true;
    call->ok("Shadow debug of \"" + sceneProject->name + "\" set (transient, never saved).", {
        {"scene_id", sceneProject->id},
        {"fit_camera_offset", McpValues::toJson(settings.fitCameraOffset)},
        {"fit_camera_euler", McpValues::toJson(settings.fitCameraEuler)},
        {"freeze_cache", settings.freezeCache},
        {"force_rerender", settings.forceRerender},
    });
}

}

void registerShadowTools(McpRegistry& registry){
    {
        ToolDef tool;
        tool.name = "get_shadow_stats";
        tool.title = "Get Shadow Stats";
        tool.description =
            "A scene's shadows on its last update: tier, atlases, memory vs budget; per light its slots, splits, texel size; per "
            "slot the cache path now and its last_render. camera game: the running game's. (jasper://docs/shadows)";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"camera", withDefault(stringEnum("", {"scene", "game"}), "scene")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getShadowStatsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_shadow_debug";
        tool.title = "Set Shadow Debug";
        tool.description =
            "Editor-only, transient: fit_camera_offset/_euler move only the camera the cascades fit (the view stays); "
            "freeze_cache holds every slot; force_rerender redraws them all. Omitted keys stay; zeros/false undo.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"fit_camera_offset", array(number(""), "[x,y,z] world offset.")},
            {"fit_camera_euler", array(number(""), "[pitch,yaw,roll] degrees.")},
            {"freeze_cache", boolean("")},
            {"force_rerender", boolean("")}
        });
        tool.annotations.idempotent = true;
        tool.handler = setShadowDebugTool;
        registry.addTool(std::move(tool));
    }
}

}
