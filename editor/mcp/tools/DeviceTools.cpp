// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Device tier tools (spec S56 R9, docs/mcp/guide/device.md): what the device is and gets (the
// GPU, the detected tier and its reason, the thermal and memory state, the overrides in effect,
// the upload queue), and the Game view's device simulation for Play testing. The tier table
// itself goes through get/set_project_settings (quality.device_tiers.<group>.<tier>, undoable).

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Engine.h"
#include "Project.h"
#include "device/Device.h"
#include "device/QualityResolver.h"
#include "render/SystemRender.h"
#include "window/GameWindow.h"

#include <cmath>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

Json deviceCellJson(DeviceOverride field, float value){
    const DeviceOverrideInfo& info = DeviceOverrides::info(field);
    if (info.kind == DeviceOverrideInfo::Kind::Off)
        return false;
    if (info.integer)
        return (int)std::lround(value);
    return value;
}

Json deviceSimulationJson(const GameViewSettings& settings){
    return Json{
        {"tier", settings.deviceTier >= 0 ? Json(DeviceNames::tier((DeviceTier)settings.deviceTier)) : Json("auto")},
        {"platform", settings.devicePlatform >= 0 ? Json(DeviceNames::group((DevicePlatformGroup)settings.devicePlatform)) : Json("auto")},
        {"thermal_state", settings.deviceThermal >= 0 ? Json(DeviceNames::thermal((ThermalState)settings.deviceThermal)) : Json("device")},
    };
}

// the device's state now, and a one-line summary of it
Json deviceInfoJson(McpContext& ctx, std::string& summary){
    const DeviceDetection& detection = Device::getDetection();
    const DeviceSimulation& simulation = Device::getSimulation();
    const bool applied = Device::isTierApplied();
    const char* source = !applied ? "none" : (simulation.tier ? "simulated" : (Device::isTierSet() ? "set" : "detected"));
    const DevicePlatformGroup group = QualityResolver::getPlatformGroup();

    Json overrides = Json::array();
    for (const AppliedDeviceOverride& item : QualityResolver::getAppliedOverrides()){
        const DeviceOverrideInfo& info = DeviceOverrides::info(item.field);
        Json entry = {{"setting", info.key}, {"source", item.source}};
        // a thermal step scales the render scale, a memory warning adds mips to the drop
        if (item.source == "thermal" && item.field == DeviceOverride::RenderScale)
            entry["factor"] = item.value;
        else if (item.source == "memory")
            entry["extra_mips"] = (int)item.value;
        else
            entry["value"] = deviceCellJson(item.field, item.value);
        overrides.push_back(entry);
    }

    const DeviceSpecs& specs = detection.profile;
    Json info = {
        {"gpu", specs.gpuName},
        {"backend", specs.backend},
        {"vendor", detection.vendor},
        {"memory_mb", specs.memoryMB},
        {"cores", specs.cores},
        {"detected_tier", detection.detected ? Json(DeviceNames::tier(detection.tier)) : Json(nullptr)},
        {"rule", detection.kind},
        {"reason", detection.reason},
        {"tier", DeviceNames::tier(Device::getTier())},
        {"tier_applied", applied},
        {"tier_source", source},
        {"platform", DeviceNames::group(group)},
        {"thermal_state", DeviceNames::thermal(Device::getThermalState())},
        {"thermal_response", DeviceNames::thermal(QualityResolver::getThermalResponse())},
        {"thermal_response_enabled", (bool)QualityResolver::getSettings().thermalResponse[(size_t)group]},
        {"recovery_seconds", QualityResolver::getRecoverySeconds()},
        {"low_memory_warnings", Device::getLowMemoryCount()},
        {"texture_mip_drop", QualityResolver::getMemoryDrop()},
        {"applied_overrides", overrides},
        {"lod_bias", QualityResolver::lodBias()},
        {"shadow_lod_bias", QualityResolver::shadowLodBias()},
        {"upload_budget_mb", (double)SystemRender::getUploadBudget() / (1024.0 * 1024.0)},
        {"deferred_uploads", SystemRender::getDeferredUploadCount()},
        {"playing", ctx.project() && ctx.project()->isAnyScenePlaying()},
    };
    if (GameWindow* game = ctx.gameWindow())
        info["simulation"] = deviceSimulationJson(game->getSettings());
    // what an agent testing a tier does next, where it is not obvious
    if (!applied && !info["playing"].get<bool>())
        info["next"] = "set_device_simulation {tier}, then play: the editor applies a tier only in Play";
    else if (Device::getThermalState() >= ThermalState::Serious && !QualityResolver::getSettings().thermalResponse[(size_t)group])
        info["next"] = std::string("the thermal response is off on ") + DeviceNames::group(group) +
                       ": set_device_simulation {platform: \"mobile\"} or quality.thermal_response." + DeviceNames::group(group);

    summary = (specs.gpuName.empty() ? std::string("No GPU name") : specs.gpuName) + ": tier " + DeviceNames::tierLabel(Device::getTier()) +
              (applied ? std::string(" (") + source + ", " + std::to_string(overrides.size()) + " override(s) in effect)"
                       : std::string(" detected, none applied (the editor applies a simulated or set tier only)")) +
              "; thermal " + DeviceNames::thermalLabel(Device::getThermalState()) + "; " + DeviceNames::groupLabel(group) + " table";
    return info;
}

void getDeviceInfoTool(const CallPtr& call){
    std::string summary;
    const Json info = deviceInfoJson(call->ctx(), summary);
    call->ok(summary + ".", info);
}

void setDeviceSimulationTool(const CallPtr& call){
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    GameWindow* game = ctx.gameWindow();
    if (!game){
        call->fail("No Game view: device simulation needs the editor's Game view.");
        return;
    }
    GameViewSettings settings = game->getSettings();
    if (args.contains("tier")){
        const std::string name = args.value("tier", "auto");
        DeviceTier tier;
        if (name == "auto") settings.deviceTier = -1;
        else if (DeviceNames::tierFromName(name.c_str(), tier)) settings.deviceTier = (int)tier;
        else { call->fail("tier must be auto, low, medium, high or ultra."); return; }
    }
    if (args.contains("platform")){
        const std::string name = args.value("platform", "auto");
        DevicePlatformGroup group;
        if (name == "auto") settings.devicePlatform = -1;
        else if (DeviceNames::groupFromName(name.c_str(), group)) settings.devicePlatform = (int)group;
        else { call->fail("platform must be auto, desktop, mobile or web."); return; }
    }
    if (args.contains("thermal_state")){
        const std::string name = args.value("thermal_state", "device");
        ThermalState state;
        if (name == "device") settings.deviceThermal = -1;
        else if (DeviceNames::thermalFromName(name.c_str(), state)) settings.deviceThermal = (int)state;
        else { call->fail("thermal_state must be device, nominal, fair, serious or critical."); return; }
    }
    game->applySettings(settings);

    const bool playing = ctx.project() && ctx.project()->isAnyScenePlaying();
    if (!playing){
        call->ok("Device simulation set; it applies when Play starts.", Json{{"simulation", deviceSimulationJson(game->getSettings())},
                 {"playing", false}, {"next", "play {action: \"start\"}, then get_device_info or screenshot"}});
        return;
    }
    // at once, then the frames that apply it (the thermal step, the resolved settings)
    Device::setSimulation(game->getDeviceSimulation());
    call->afterFrames(2, [call](){
        std::string summary;
        const Json info = deviceInfoJson(call->ctx(), summary);
        call->ok("Device simulation applied. " + summary + ".", info);
    });
}

}

void registerDeviceTools(McpRegistry& registry){
    {
        ToolDef tool;
        tool.name = "get_device_info";
        tool.title = "Get Device Info";
        tool.description =
            "The GPU, its detected Device Tier and why, the tier in effect (simulated, set, detected; none in the editor until "
            "simulated), thermal and memory state, the overrides in effect and the upload queue. Tier table: "
            "quality.device_tiers.* in set_project_settings. jasper://docs/device";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.allowedWhilePlaying = true;
        tool.handler = getDeviceInfoTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_device_simulation";
        tool.title = "Set Device Simulation";
        tool.description =
            "Play testing (Game view toolbar, an editor preference, not undoable): the Device Tier whose overrides apply in Play "
            "(auto: none), the platform family whose table it reads (auto: mobile with a simulated phone) and the thermal "
            "state (device: the machine's). Applies at once while playing.";
        tool.inputSchema = object({
            {"tier", stringEnum("", {"auto", "low", "medium", "high", "ultra"})},
            {"platform", stringEnum("", {"auto", "desktop", "mobile", "web"})},
            {"thermal_state", stringEnum("", {"device", "nominal", "fair", "serious", "critical"})},
        });
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Control;
        tool.allowedWhilePlaying = true;
        tool.handler = setDeviceSimulationTool;
        registry.addTool(std::move(tool));
    }
}

}
