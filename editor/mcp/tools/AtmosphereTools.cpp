// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere (docs/specs/S12-atmosphere.md): the Fog and Volumetric Clouds presets and
// what a view's height fog, volumetric fog and clouds ran last frame. The components' fields
// themselves are set_properties' (Catalog).
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/PlayViewCommon.h"

#include "Project.h"
#include "command/type/AtmospherePresetCmd.h"
#include "render/AtmospherePresets.h"
#include "render/RenderUtil.h"
#include "subsystem/RenderSystem.h"

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

const char* qualityName(AtmosphereQuality quality) {
    switch (quality) {
        case AtmosphereQuality::OFF: return "off";
        case AtmosphereQuality::LOW: return "low";
        case AtmosphereQuality::MEDIUM: return "medium";
        case AtmosphereQuality::HIGH: return "high";
    }
    return "off";
}

Json presetsJson(const std::vector<AtmospherePreset>& presets) {
    Json list = Json::array();
    for (const AtmospherePreset& preset : presets)
        list.push_back({{"id", preset.id}, {"name", preset.name}, {"description", preset.description}});
    return list;
}

void listAtmospherePresetsTool(const CallPtr& call) {
    const std::string kind = call->args().value("kind", std::string("fog"));
    if (kind != "fog" && kind != "clouds") {
        call->fail("kind must be fog or clouds.");
        return;
    }
    const std::vector<AtmospherePreset>& presets = kind == "fog" ? AtmospherePresets::getFogPresets() : AtmospherePresets::getCloudsPresets();
    Json data = {{"kind", kind}, {"presets", presetsJson(presets)}};
    std::string text = std::to_string(presets.size()) + (kind == "fog" ? " Fog" : " Volumetric Clouds") + " presets:";
    for (const AtmospherePreset& preset : presets)
        text += " " + preset.id;
    call->ok(text, data);
}

void applyAtmospherePresetTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        call->fail(error);
        return;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    const Entity entity = McpRefs::resolveEntity(scene, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    // the GameObject's Fog, else its Volumetric Clouds
    ComponentType component;
    if (scene->scene->findComponent<FogComponent>(entity)) {
        component = ComponentType::FogComponent;
    } else if (scene->scene->findComponent<VolumetricCloudsComponent>(entity)) {
        component = ComponentType::VolumetricCloudsComponent;
    } else {
        call->fail("\"" + scene->scene->getEntityName(entity) + "\" has neither a Fog nor a Volumetric Clouds component.");
        return;
    }
    const std::string preset = args.value("preset", std::string());
    Command* cmd = AtmospherePresetCmd::build(project, scene->id, {entity}, component, preset, error);
    if (!cmd) {
        if (!error.empty()) {
            const bool fog = component == ComponentType::FogComponent;
            std::string ids;
            for (const AtmospherePreset& p : fog ? AtmospherePresets::getFogPresets() : AtmospherePresets::getCloudsPresets())
                ids += (ids.empty() ? "" : ", ") + p.id;
            call->fail(error + " (" + (fog ? "fog" : "clouds") + " presets: " + ids + ")");
            return;
        }
        call->ok("The preset is already applied: nothing changed.", {{"changed", false}});
        return;
    }
    if (!runSceneCommand(scene->id, cmd, error)) {
        call->fail(error);
        return;
    }
    call->ok("Applied the " + preset + " preset to " + scene->scene->getEntityName(entity) + " (one undo step).",
             {{"changed", true}, {"entity", (uint64_t)entity}, {"component", Catalog::getComponentName(component, true)}, {"preset", preset}});
}

void getAtmosphereInfoTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string view = args.value("view", std::string("game"));
    if (view != "game" && view != "scene") {
        call->fail("view must be game or scene.");
        return;
    }
    std::string error;
    const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    SceneProject* scene = nullptr;
    if (playing && !args.contains("scene")) {
        scene = w3::resolveRuntimeScene(project, Json(), error);
    } else if (!args.contains("scene") && view == "game") {
        scene = w3::gameViewScene(project);
        if (!scene) error = "No scene shows in the Game view.";
    } else {
        scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    }
    if (!scene) {
        call->fail(error);
        return;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    // the frame's own view is the game while playing, else the Scene view (RenderUtil.h ids)
    uint32_t viewId = 0;
    if (view == "game" && !playing) viewId = GAME_VIEW_ID;
    if (view == "scene" && playing) viewId = SCENE_VIEW_ID;
    const AtmosphereInfo& info = scene->scene->getSystem<RenderSystem>()->getAtmosphereInfo(viewId);

    Json passes = Json::array();
    for (const std::string& label : info.passes)
        passes.push_back({{"label", label}});
    Json volumetric = {
        {"enabled", info.volumetric},
        {"active", info.volumetricActive},
        {"width", info.fogWidth},
        {"height", info.fogHeight},
        {"steps", info.fogSteps},
        {"format", info.floatTargets ? "RGBA16F" : "RGBA8"},
        {"volumes", {{"used", info.volumesUsed}, {"culled", info.volumesCulled}}},
        {"localLights", info.localLights},
        {"forcedDepthPrePass", info.forcedDepthPrePass}
    };
    const char* mode = info.cloudsMode == CloudsRendering::SCREEN ? "screen" : "sky_map";
    Json data = {
        {"view", view},
        {"quality", qualityName(info.quality)},
        {"platformDefault", qualityName(info.platformDefault)},
        {"fog", {{"present", info.fog}, {"type", info.heightFog ? "height" : (info.fog ? "distance" : "none")},
                 {"horizonSource", info.horizonSource}, {"volumetric", volumetric}}},
        {"clouds", {{"present", info.clouds}, {"active", info.cloudsActive}, {"mode", info.cloudsActive ? Json(mode) : Json(nullptr)},
                    {"noiseReady", info.noiseReady}, {"mapSize", info.mapSize}, {"sliceFrames", info.sliceFrames}}},
        {"passes", passes},
        {"vramBytes", (uint64_t)info.vramBytes},
        {"vramScreenBytes", (uint64_t)info.vramScreenBytes}
    };
    std::string text = "Atmosphere (" + view + " view): quality " + qualityName(info.quality) + ", " + std::to_string(info.passes.size()) + " passes";
    if (info.volumetricActive)
        text += ", volumetric fog " + std::to_string(info.fogWidth) + "x" + std::to_string(info.fogHeight) + " " + std::to_string(info.fogSteps) + " steps";
    if (info.cloudsActive)
        text += std::string(", clouds ") + mode;
    call->ok(text, data);
}

} // namespace

void registerAtmosphereTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "list_atmosphere_presets";
        tool.title = "List Atmosphere Presets";
        tool.description = "The presets apply_atmosphere_preset takes: fog (Fog of type Height looks) or clouds (Volumetric Clouds).";
        tool.inputSchema = object({
            {"kind", withDefault(stringEnum("", {"fog", "clouds"}), "fog")}
        });
        tool.annotations.readOnly = true;
        tool.handler = listAtmospherePresetsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "apply_atmosphere_preset";
        tool.title = "Apply Atmosphere Preset";
        tool.description = "Set the fields of a Fog (makes it a height fog) or Volumetric Clouds preset on that GameObject's component, as one "
                           "undo step. Preset ids: list_atmosphere_presets.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("The GameObject with the Fog or Volumetric Clouds.")},
            {"preset", string("Preset id, e.g. morning_haze or overcast.")}
        }, {"entity", "preset"});
        tool.allowedWhilePlaying = false;
        tool.handler = applyAtmospherePresetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_atmosphere_info";
        tool.title = "Get Atmosphere Info";
        tool.description = "What the height fog, volumetric fog and clouds of a view ran last frame: the quality tier, the volumetric fog's "
                           "target size, steps, format and volumes, the clouds' mode and noise bake, the Atmosphere/* passes and their VRAM.";
        tool.inputSchema = object({
            {"view", withDefault(stringEnum("", {"game", "scene"}), "game")},
            {"scene", sceneRef("Default: the playing scene, else the Game view's.")}
        });
        tool.annotations.readOnly = true;
        tool.handler = getAtmosphereInfoTool;
        registry.addTool(std::move(tool));
    }
}

}
