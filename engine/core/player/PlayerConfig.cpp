// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerConfig.h"
#include "pixelart/PixelColor.h"

#include "audio/AudioMixer.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::player;
using nlohmann::json;

namespace {

    template<typename E>
    struct EnumNames {
        E value;
        const char* name;
    };

    const EnumNames<Scaling> kScaling[] = {{Scaling::FITWIDTH, "fitwidth"}, {Scaling::FITHEIGHT, "fitheight"},
        {Scaling::LETTERBOX, "letterbox"}, {Scaling::CROP, "crop"}, {Scaling::STRETCH, "stretch"}, {Scaling::NATIVE, "native"},
        {Scaling::INTEGER, "integer"}, {Scaling::INTEGER_EXPAND, "integer_expand"}};
    const EnumNames<TextureStrategy> kTextureStrategy[] = {{TextureStrategy::FIT, "fit"}, {TextureStrategy::RESIZE, "resize"},
        {TextureStrategy::NONE, "none"}};
    const EnumNames<AtmosphereQuality> kAtmosphere[] = {{AtmosphereQuality::OFF, "off"}, {AtmosphereQuality::LOW, "low"},
        {AtmosphereQuality::MEDIUM, "medium"}, {AtmosphereQuality::HIGH, "high"}};
    const EnumNames<FoliageQuality> kFoliage[] = {{FoliageQuality::LOW, "low"}, {FoliageQuality::MEDIUM, "medium"}, {FoliageQuality::HIGH, "high"}};
    const EnumNames<PbrTier> kPbrTier[] = {{PbrTier::FULL, "full"}, {PbrTier::REDUCED, "reduced"}};
    const EnumNames<ShadowTier> kShadowTier[] = {{ShadowTier::LOW, "low"}, {ShadowTier::MEDIUM, "medium"}, {ShadowTier::HIGH, "high"},
        {ShadowTier::ULTRA, "ultra"}};
    const char* const kWindowModes[] = {"windowed", "maximized", "fullscreen"};
    const EnumNames<ShaderGlobalType> kShaderGlobalTypes[] = {{ShaderGlobalType::Float, "float"}, {ShaderGlobalType::Vector2, "vector2"},
        {ShaderGlobalType::Vector3, "vector3"}, {ShaderGlobalType::Vector4, "vector4"}, {ShaderGlobalType::Color, "color"},
        {ShaderGlobalType::Matrix4, "matrix4"}, {ShaderGlobalType::FloatArray, "float_array"}, {ShaderGlobalType::Vector4Array, "vector4_array"},
        {ShaderGlobalType::Texture2D, "texture2d"}, {ShaderGlobalType::TextureCube, "texture_cube"}};
    const EnumNames<PlayerConfig::BuildMode> kBuildModes[] = {{PlayerConfig::BuildMode::Development, "development"},
        {PlayerConfig::BuildMode::Never, "never"}, {PlayerConfig::BuildMode::Always, "always"}};
    const EnumNames<DebugOverlaySettings::Gesture> kGestures[] = {{DebugOverlaySettings::Gesture::ThreeFingerHold, "three_finger_hold"},
        {DebugOverlaySettings::Gesture::FourFingerTap, "four_finger_tap"}, {DebugOverlaySettings::Gesture::Off, "off"}};
    const EnumNames<DebugOverlaySettings::Popup> kPopups[] = {{DebugOverlaySettings::Popup::TouchDevices, "touch_devices"},
        {DebugOverlaySettings::Popup::Never, "never"}, {DebugOverlaySettings::Popup::AllDevices, "all_devices"}};
    const EnumNames<DebugOverlaySettings::Stats> kStats[] = {{DebugOverlaySettings::Stats::Off, "off"}, {DebugOverlaySettings::Stats::Fps, "fps"},
        {DebugOverlaySettings::Stats::Full, "full"}};
    const EnumNames<DebugOverlaySettings::Corner> kCorners[] = {{DebugOverlaySettings::Corner::TopLeft, "top_left"},
        {DebugOverlaySettings::Corner::TopRight, "top_right"}, {DebugOverlaySettings::Corner::BottomLeft, "bottom_left"},
        {DebugOverlaySettings::Corner::BottomRight, "bottom_right"}};
    const EnumNames<LogStackTraces> kStackTraces[] = {{LogStackTraces::Warnings, "warnings"}, {LogStackTraces::Off, "off"},
        {LogStackTraces::Errors, "errors"}, {LogStackTraces::All, "all"}};
    const EnumNames<UIScaleMode> kScaleModes[] = {{UIScaleMode::FollowGameCanvas, "follow_game_canvas"},
        {UIScaleMode::ConstantPixelSize, "constant_pixel_size"}, {UIScaleMode::ScaleWithScreenSize, "scale_with_screen_size"},
        {UIScaleMode::ConstantPhysicalSize, "constant_physical_size"}};
    const EnumNames<ScreenMatchMode> kMatchModes[] = {{ScreenMatchMode::MatchWidthOrHeight, "match_width_or_height"},
        {ScreenMatchMode::Expand, "expand"}, {ScreenMatchMode::Shrink, "shrink"}};
    const EnumNames<PhysicalUnit> kPhysicalUnits[] = {{PhysicalUnit::Points, "points"}, {PhysicalUnit::Centimeters, "centimeters"},
        {PhysicalUnit::Millimeters, "millimeters"}, {PhysicalUnit::Inches, "inches"}, {PhysicalUnit::Picas, "picas"}};

    json appInfoJson(const ApplicationInfo& info) {
        return {{"name", info.name}, {"identifier", info.identifier}, {"version", info.version}, {"build", info.build}};
    }

    ApplicationInfo appInfoFrom(const json& in) {
        ApplicationInfo info;
        if (!in.is_object()) return info;
        info.name = in.value("name", "");
        info.identifier = in.value("identifier", "");
        info.version = in.value("version", "");
        info.build = in.value("build", int64_t(0));
        return info;
    }

    template<typename E, size_t N>
    const char* nameOf(const EnumNames<E> (&names)[N], E value) {
        for (const auto& item : names)
            if (item.value == value) return item.name;
        return names[0].name;
    }

    template<typename E, size_t N>
    bool valueOf(const EnumNames<E> (&names)[N], const std::string& name, E& out) {
        for (const auto& item : names) {
            if (name == item.name) {
                out = item.value;
                return true;
            }
        }
        return false;
    }

}

PlayerConfig::PlayerConfig() {
    navigation = nav::NavigationSettings::defaults();
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) shadowTiers[platform] = ShadowTiers::defaultTier(platform);
    collision3D.fill(LAYER_MASK_EVERYTHING);
    collision2D.fill(LAYER_MASK_EVERYTHING);
}

json PlayerConfig::toJson() const {
    json out;
    out["schemaVersion"] = SCHEMA_VERSION;
    out["name"] = name;
    out["appId"] = appId;
    out["version"] = version;
    out["startScene"] = startScene;
    out["scenes"] = json::array();
    for (const GameScene& scene : scenes)
        out["scenes"].push_back({{"id", scene.id}, {"name", scene.name}, {"file", scene.file}, {"involved", scene.involved}, {"active", scene.active}});
    out["prefabs"] = json::array();
    for (const GamePrefab& prefab : prefabs)
        out["prefabs"].push_back({{"id", prefab.id}, {"name", prefab.name}, {"guid", prefab.guid}, {"file", prefab.file}});
    out["window"] = {{"mode", kWindowModes[std::clamp(windowMode, 0, 2)]}, {"width", windowWidth}, {"height", windowHeight},
                     {"resizable", resizable}, {"title", title}, {"vsync", vsync}, {"icon", icon}, {"storageId", storageId}};
    out["canvas"] = {{"width", canvasWidth}, {"height", canvasHeight}, {"scaling", nameOf(kScaling, scaling)},
                     {"textureStrategy", nameOf(kTextureStrategy, textureStrategy)}, {"pixelsPerUnit", pixelsPerUnit},
                     {"bitmapTextCacheMiB", bitmapTextCacheMiB}};
    // written only when Point, so a bilinear game's config is what it was before S42
    if (defaultFilterPoint)
        out["canvas"]["defaultFilterMode"] = "point";
    out["loading"] = {{"scene", loadingScene}, {"delay", loadingDelay}, {"timeout", loadingTimeout}, {"async", asyncLoading}};
    out["threading"] = {{"workers", threading.workers}, {"reservedFrameWorkers", threading.reservedFrameWorkers},
                        {"ioThreads", threading.ioThreads}, {"webPumpBudgetMs", threading.webPumpBudgetMs}};
    if (particleWorldBudget != 0)
        out["particleWorldBudget"] = particleWorldBudget;
    out["atmosphereQuality"] = {nameOf(kAtmosphere, atmosphereQuality[0]), nameOf(kAtmosphere, atmosphereQuality[1]),
                                nameOf(kAtmosphere, atmosphereQuality[2])};
    if (!foliageQualityUncapped) {
        out["foliageQuality"] = {nameOf(kFoliage, foliageQuality[0]), nameOf(kFoliage, foliageQuality[1]), nameOf(kFoliage, foliageQuality[2])};
    }
    out["foliageDistanceScale"] = {foliageDistanceScale[0], foliageDistanceScale[1], foliageDistanceScale[2]};
    out["input"] = {{"deadzone", inputDeadzone}, {"actions", inputActions}};
    json areas = json::array();
    for (const nav::AreaSettings& area : navigation.areas)
        areas.push_back({{"name", area.name}, {"cost", area.cost}, {"color", {area.color.x, area.color.y, area.color.z, area.color.w}}});
    json agents = json::array();
    for (const nav::AgentTypeSettings& agent : navigation.agentTypes)
        agents.push_back({{"id", agent.id}, {"name", agent.name}, {"radius", agent.radius}, {"height", agent.height},
                          {"stepHeight", agent.stepHeight}, {"maxSlope", agent.maxSlope}});
    out["navigation"] = {{"areas", areas}, {"agentTypes", agents}, {"crowdMaxAgents", navigation.crowdMaxAgents},
                         {"webBakeBudgetMs", navigation.webBakeBudgetMs}, {"maxGridSolvesPerTick", navigation.maxGridSolvesPerTick}};
    // (only when set: a game without them keeps its file)
    if (voxelMaxDimension > 0 || voxelCpuBudgetMB > 0 || voxelMeshBudgetMB > 0)
        out["voxel"] = {{"maxDimension", voxelMaxDimension}, {"cpuBudgetMB", voxelCpuBudgetMB}, {"meshBudgetMB", voxelMeshBudgetMB}};
    json textures = json::array();
    for (const Engine::TexturePlatformDefaults& group : textureDefaults)
        textures.push_back({{"quality", group.quality}, {"memoryBudgetMB", group.memoryBudgetMB}, {"lowMemoryFallback", group.lowMemoryFallback}});
    out["textureDefaults"] = textures;
    out["htmlUi"] = {{"msaa", htmlUiMsaa}, {"glyphAtlasMiB", glyphAtlasMiB}, {"sdfPrimitives", sdfPrimitives},
                     {"transformAwareText", transformAwareText}};
    out["mobilePbrTier"] = nameOf(kPbrTier, mobilePbrTier);
    out["dynamicBatching"] = dynamicBatching;
    out["lightMode"] = LightingTiers::getModeName(lightMode);
    if (transitions != pixelart::TransitionDefaults()){
        char color[8];
        auto byte = [](float v){ return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        std::snprintf(color, sizeof(color), "#%02x%02x%02x", byte(transitions.color.x), byte(transitions.color.y), byte(transitions.color.z));
        out["transitions"] = {{"kind", pixelart::transitionKindName(transitions.kind)}, {"duration", transitions.duration}, {"color", color},
                              {"ramp", transitions.ramp}, {"rim", transitions.rim}, {"pixelSize", transitions.pixelSize}, {"coverUI", transitions.coverUI}};
    }
    json tiers = json::array();
    for (ShadowTier tier : shadowTiers) tiers.push_back(nameOf(kShadowTier, tier));
    out["shadowTiers"] = tiers;
    out["device"] = json::parse(device.toJson());
    out["terrainQualityTiers"] = terrainQualityTiers;
    out["terrainShippedTiers"] = terrainShippedTiers;
    out["tags"] = tags;
    out["layers"] = layerNames;
    if (!sortingLayers.empty()) {
        json layers = json::array();
        for (const SortingLayerInfo& layer : sortingLayers) layers.push_back({{"id", layer.id}, {"name", layer.name}});
        out["sortingLayers"] = layers;
    }
    if (transparencySortMode == TransparencySortMode::CUSTOM_AXIS) out["transparencySortMode"] = "custom_axis";
    if (transparencySortAxis != Vector3(0.0f, 1.0f, 0.0f))
        out["transparencySortAxis"] = {transparencySortAxis.x, transparencySortAxis.y, transparencySortAxis.z};
    out["collision3D"] = collision3D;
    out["collision2D"] = collision2D;
    out["logging"] = {{"level", logLevel}, {"file", logFile}, {"backtrace", logBacktrace}};
    out["crash"] = {{"handler", crashHandler}, {"logTail", crashLogTail}, {"dumpWithHeap", crashDumpWithHeap},
                    {"hangWatchdog", hangWatchdog}, {"hangMs", hangMs}, {"webOverlay", crashWebOverlay}};
    out["audio"] = {{"mixer", audioMixer}, {"maxRealVoices", audioMaxRealVoices}, {"maxVoices", audioMaxVoices},
                    {"sampleRate", audioSampleRate}, {"outputStage", audioOutputStage}, {"iosSessionCategory", audioIosSession}, {"seed", audioSeed},
                    {"musicGroup", audioMusicGroup}, {"unusedClipCacheMb", audioUnusedClipCacheMb}};
    if (!nativePlatformOnly.empty()) out["nativePlatformOnly"] = nativePlatformOnly;
    if (!nativePlatformOnlyByTarget.empty()) out["nativePlatformOnlyByTarget"] = nativePlatformOnlyByTarget;
    if (hasPlayerSettings) {
        out["player"] = {{"targetFrameRate", targetFrameRate}, {"defaultLanguage", defaultLanguage},
                         {"defaultOrientation", static_cast<int>(defaultOrientation)}, {"allowedOrientations", allowedOrientations},
                         {"keepScreenOn", keepScreenOn}, {"allowInsecureHttp", allowInsecureHttp},
                         {"applicationInfo", {{"linux", appInfoJson(linuxApp)}, {"windows", appInfoJson(windowsApp)},
                                              {"macos", appInfoJson(macosApp)}, {"web", appInfoJson(webApp)}}}};
    }
    // (each only when not the default: a game without them keeps its file)
    if (shaderCreation != ShaderCreationSettings()) {
        out["shaderCreation"] = {{"asyncCreation", shaderCreation.asyncCreation}, {"mainThreadBudgetMs", shaderCreation.mainThreadBudgetMs},
                                 {"glWarmDraws", shaderCreation.glWarmDraws}, {"holdFirstSceneForWarmup", shaderCreation.holdFirstSceneForWarmup}};
    }
    if (!gpuInstancing) out["gpuInstancing"] = false;
    if (!shaderGlobals.empty()) {
        json globals = json::array();
        for (const ShaderGlobalDecl& decl : shaderGlobals) {
            json values = json::array();
            for (const Vector4& v : decl.defaultValue) values.push_back({v.x, v.y, v.z, v.w});
            globals.push_back({{"name", decl.name}, {"type", nameOf(kShaderGlobalTypes, decl.type)}, {"arraySize", decl.arraySize},
                               {"default", values}, {"texture", decl.defaultTexture}});
        }
        out["shaderGlobals"] = globals;
    }
    if (canvasScaler != CanvasScalerSettings()) {
        out["canvas"]["scaler"] = {{"mode", nameOf(kScaleModes, canvasScaler.mode)}, {"scaleFactor", canvasScaler.scaleFactor},
                                   {"referenceResolution", {canvasScaler.referenceResolution.x, canvasScaler.referenceResolution.y}},
                                   {"screenMatchMode", nameOf(kMatchModes, canvasScaler.screenMatchMode)},
                                   {"matchWidthOrHeight", canvasScaler.matchWidthOrHeight},
                                   {"physicalUnit", nameOf(kPhysicalUnits, canvasScaler.physicalUnit)},
                                   {"fallbackScreenDPI", canvasScaler.fallbackScreenDPI}};
    }
    if (hasPhysics3DCapacity) {
        out["physics3D"] = {{"maxBodies", maxBodies3D}, {"maxBodyPairs", maxBodyPairs3D}, {"maxContactConstraints", maxContactConstraints3D}};
    }
    if (hasDebugSettings) {
        out["debug"] = {{"overlay", nameOf(kBuildModes, debugOverlayMode)}, {"allowLua", nameOf(kBuildModes, debugAllowLuaMode)},
                        {"toggleKey", debugOverlay.toggleKey}, {"statsKey", debugOverlay.statsKey},
                        {"gesture", nameOf(kGestures, debugOverlay.gesture)}, {"popup", nameOf(kPopups, debugOverlay.popup)},
                        {"openOnError", debugOverlay.openOnError}, {"stats", nameOf(kStats, debugOverlay.stats)},
                        {"statsCorner", nameOf(kCorners, debugOverlay.statsCorner)}, {"logBufferSize", logBufferSize},
                        {"logStackTraces", nameOf(kStackTraces, logStackTraces)}};
    }
    return out;
}

bool PlayerConfig::fromJson(const json& in, PlayerConfig& out, std::string& error) {
    try {
        if (!in.is_object()) {
            error = "game.json is not a JSON object";
            return false;
        }
        const int schema = in.value("schemaVersion", 0);
        if (schema < 1) {
            error = "game.json has no schemaVersion";
            return false;
        }
        if (schema > SCHEMA_VERSION) {
            error = "game.json is schema " + std::to_string(schema) + ", this player reads " + std::to_string(SCHEMA_VERSION) +
                    " (build the game with the editor of this player's version)";
            return false;
        }
        PlayerConfig config;
        config.name = in.value("name", "");
        config.appId = in.value("appId", "");
        config.version = in.value("version", "");
        config.startScene = in.value("startScene", 0u);
        for (const json& scene : in.value("scenes", json::array())) {
            GameScene item;
            item.id = scene.at("id").get<uint32_t>();
            item.name = scene.value("name", "");
            item.file = scene.at("file").get<std::string>();
            item.involved = scene.value("involved", std::vector<uint32_t>{item.id});
            item.active = scene.value("active", std::vector<uint32_t>{item.id});
            config.scenes.push_back(std::move(item));
        }
        for (const json& prefab : in.value("prefabs", json::array())) {
            GamePrefab item;
            item.id = prefab.at("id").get<uint32_t>();
            item.name = prefab.value("name", "");
            item.guid = prefab.value("guid", "");
            item.file = prefab.at("file").get<std::string>();
            config.prefabs.push_back(std::move(item));
        }
        if (const auto window = in.find("window"); window != in.end()) {
            const std::string mode = window->value("mode", "windowed");
            config.windowMode = mode == "fullscreen" ? 2 : mode == "maximized" ? 1 : 0;
            config.windowWidth = window->value("width", config.windowWidth);
            config.windowHeight = window->value("height", config.windowHeight);
            config.resizable = window->value("resizable", config.resizable);
            config.title = window->value("title", config.name);
            config.vsync = window->value("vsync", config.vsync);
            config.icon = window->value("icon", "");
            config.storageId = window->value("storageId", config.appId);
        }
        if (const auto canvas = in.find("canvas"); canvas != in.end()) {
            config.canvasWidth = canvas->value("width", config.canvasWidth);
            config.canvasHeight = canvas->value("height", config.canvasHeight);
            valueOf(kScaling, canvas->value("scaling", "fitwidth"), config.scaling);
            valueOf(kTextureStrategy, canvas->value("textureStrategy", "none"), config.textureStrategy);
            config.pixelsPerUnit = canvas->value("pixelsPerUnit", 1);
            config.defaultFilterPoint = canvas->value("defaultFilterMode", std::string("bilinear")) == "point";
            config.bitmapTextCacheMiB = canvas->value("bitmapTextCacheMiB", 0);
        }
        if (const auto loading = in.find("loading"); loading != in.end()) {
            config.loadingScene = loading->value("scene", 0u);
            config.loadingDelay = loading->value("delay", config.loadingDelay);
            config.loadingTimeout = loading->value("timeout", config.loadingTimeout);
            config.asyncLoading = loading->value("async", config.asyncLoading);
        }
        config.particleWorldBudget = in.value("particleWorldBudget", config.particleWorldBudget);
        if (const auto threading = in.find("threading"); threading != in.end()) {
            config.threading.workers = threading->value("workers", config.threading.workers);
            config.threading.reservedFrameWorkers = threading->value("reservedFrameWorkers", config.threading.reservedFrameWorkers);
            config.threading.ioThreads = threading->value("ioThreads", config.threading.ioThreads);
            config.threading.webPumpBudgetMs = threading->value("webPumpBudgetMs", config.threading.webPumpBudgetMs);
        }
        if (const auto foliage = in.find("foliageQuality"); foliage != in.end() && foliage->is_array()) {
            config.foliageQualityUncapped = false;
            for (size_t i = 0; i < 3 && i < foliage->size(); i++) valueOf(kFoliage, (*foliage)[i].get<std::string>(), config.foliageQuality[i]);
        }
        if (const auto atmosphere = in.find("atmosphereQuality"); atmosphere != in.end() && atmosphere->is_array()) {
            for (size_t i = 0; i < 3 && i < atmosphere->size(); i++) valueOf(kAtmosphere, (*atmosphere)[i].get<std::string>(), config.atmosphereQuality[i]);
        }
        if (const auto foliage = in.find("foliageDistanceScale"); foliage != in.end() && foliage->is_array()) {
            for (size_t i = 0; i < 3 && i < foliage->size(); i++){
                if ((*foliage)[i].is_number()) config.foliageDistanceScale[i] = (*foliage)[i].get<float>();
            }
        }
        if (const auto input = in.find("input"); input != in.end()) {
            config.inputDeadzone = input->value("deadzone", config.inputDeadzone);
            config.inputActions = input->value("actions", "");
        }
        if (const auto voxel = in.find("voxel"); voxel != in.end()) {
            config.voxelMaxDimension = voxel->value("maxDimension", 0);
            config.voxelCpuBudgetMB = voxel->value("cpuBudgetMB", 0);
            config.voxelMeshBudgetMB = voxel->value("meshBudgetMB", 0);
        }
        if (const auto navigation = in.find("navigation"); navigation != in.end()) {
            nav::NavigationSettings settings = nav::NavigationSettings::defaults();
            const json areas = navigation->value("areas", json::array());
            for (size_t i = 0; i < areas.size() && i < settings.areas.size(); i++) {
                settings.areas[i].name = areas[i].value("name", settings.areas[i].name);
                settings.areas[i].cost = areas[i].value("cost", settings.areas[i].cost);
                const std::vector<float> color = areas[i].value("color", std::vector<float>{});
                if (color.size() == 4) settings.areas[i].color = Vector4(color[0], color[1], color[2], color[3]);
            }
            if (navigation->contains("agentTypes")) {
                settings.agentTypes.clear();
                for (const json& agent : navigation->at("agentTypes")) {
                    nav::AgentTypeSettings item;
                    item.id = agent.value("id", 0);
                    item.name = agent.value("name", item.name);
                    item.radius = agent.value("radius", item.radius);
                    item.height = agent.value("height", item.height);
                    item.stepHeight = agent.value("stepHeight", item.stepHeight);
                    item.maxSlope = agent.value("maxSlope", item.maxSlope);
                    settings.agentTypes.push_back(item);
                }
            }
            settings.crowdMaxAgents = navigation->value("crowdMaxAgents", settings.crowdMaxAgents);
            settings.webBakeBudgetMs = navigation->value("webBakeBudgetMs", settings.webBakeBudgetMs);
            settings.maxGridSolvesPerTick = navigation->value("maxGridSolvesPerTick", settings.maxGridSolvesPerTick);
            config.navigation = settings;
        }
        if (const auto textures = in.find("textureDefaults"); textures != in.end() && textures->is_array()) {
            for (size_t i = 0; i < 3 && i < textures->size(); i++) {
                config.textureDefaults[i].quality = (*textures)[i].value("quality", 0);
                config.textureDefaults[i].memoryBudgetMB = (*textures)[i].value("memoryBudgetMB", 0);
                config.textureDefaults[i].lowMemoryFallback = (*textures)[i].value("lowMemoryFallback", false);
            }
        }
        if (const auto htmlUi = in.find("htmlUi"); htmlUi != in.end()) {
            config.htmlUiMsaa = htmlUi->value("msaa", config.htmlUiMsaa);
            config.glyphAtlasMiB = htmlUi->value("glyphAtlasMiB", config.glyphAtlasMiB);
            config.sdfPrimitives = htmlUi->value("sdfPrimitives", config.sdfPrimitives);
            config.transformAwareText = htmlUi->value("transformAwareText", config.transformAwareText);
        }
        valueOf(kPbrTier, in.value("mobilePbrTier", "reduced"), config.mobilePbrTier);
        config.dynamicBatching = in.value("dynamicBatching", config.dynamicBatching);
        // absent (a config written before it) or unknown: Automatic
        if (const auto transitions = in.find("transitions"); transitions != in.end() && transitions->is_object()){
            pixelart::TransitionDefaults& t = config.transitions;
            pixelart::transitionKindFromName(transitions->value("kind", std::string("iris")), t.kind);
            t.duration = transitions->value("duration", t.duration);
            pixelart::Rgba8 color;
            if (pixelart::parseHex(transitions->value("color", std::string()), color))
                t.color = Vector4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f);
            t.ramp = transitions->value("ramp", t.ramp);
            t.rim = transitions->value("rim", t.rim);
            t.pixelSize = transitions->value("pixelSize", t.pixelSize);
            t.coverUI = transitions->value("coverUI", t.coverUI);
        }
        const std::string lightMode = in.value("lightMode", "automatic");
        if (!LightingTiers::modeFromName(lightMode.c_str(), config.lightMode)) config.lightMode = LightMode::AUTOMATIC;
        if (const auto tiers = in.find("shadowTiers"); tiers != in.end() && tiers->is_array()) {
            for (size_t i = 0; i < config.shadowTiers.size() && i < tiers->size(); i++)
                valueOf(kShadowTier, (*tiers)[i].get<std::string>(), config.shadowTiers[i]);
        }
        if (const auto device = in.find("device"); device != in.end()) {
            std::string deviceError;
            if (!DeviceSettings::fromJson(device->dump(), config.device, deviceError)) {
                error = "game.json: device: " + deviceError;
                return false;
            }
        }
        if (const auto terrainTiers = in.find("terrainQualityTiers"); terrainTiers != in.end() && terrainTiers->is_array()) {
            for (size_t i = 0; i < config.terrainQualityTiers.size() && i < terrainTiers->size(); i++)
                config.terrainQualityTiers[i] = (*terrainTiers)[i].get<int>();
        }
        config.terrainShippedTiers = in.value("terrainShippedTiers", config.terrainShippedTiers);
        config.tags = in.value("tags", std::vector<std::string>{});
        config.layerNames = in.value("layers", std::vector<std::string>{});
        if (const auto layers = in.find("sortingLayers"); layers != in.end() && layers->is_array()) {
            for (const json& layer : *layers) {
                if (layer.is_object() && layer.contains("id") && layer["id"].is_number_integer() && layer.contains("name") && layer["name"].is_string())
                    config.sortingLayers.push_back({layer["id"].get<int32_t>(), layer["name"].get<std::string>()});
            }
        }
        if (in.value("transparencySortMode", std::string()) == "custom_axis") config.transparencySortMode = TransparencySortMode::CUSTOM_AXIS;
        if (const auto axis = in.find("transparencySortAxis"); axis != in.end() && axis->is_array() && axis->size() == 3 &&
                (*axis)[0].is_number() && (*axis)[1].is_number() && (*axis)[2].is_number())
            config.transparencySortAxis = Vector3((*axis)[0].get<float>(), (*axis)[1].get<float>(), (*axis)[2].get<float>());
        const std::vector<uint32_t> collision3D = in.value("collision3D", std::vector<uint32_t>{});
        const std::vector<uint32_t> collision2D = in.value("collision2D", std::vector<uint32_t>{});
        for (size_t i = 0; i < collision3D.size() && i < config.collision3D.size(); i++) config.collision3D[i] = collision3D[i];
        for (size_t i = 0; i < collision2D.size() && i < config.collision2D.size(); i++) config.collision2D[i] = collision2D[i];
        if (const auto logging = in.find("logging"); logging != in.end()) {
            config.logLevel = logging->value("level", "");
            config.logFile = logging->value("file", config.logFile);
            config.logBacktrace = logging->value("backtrace", config.logBacktrace);
        }
        if (const auto crash = in.find("crash"); crash != in.end()) {
            config.crashHandler = crash->value("handler", config.crashHandler);
            config.crashLogTail = crash->value("logTail", config.crashLogTail);
            config.crashDumpWithHeap = crash->value("dumpWithHeap", config.crashDumpWithHeap);
            config.hangWatchdog = crash->value("hangWatchdog", config.hangWatchdog);
            config.hangMs = crash->value("hangMs", config.hangMs);
            config.crashWebOverlay = crash->value("webOverlay", config.crashWebOverlay);
        }
        // absent (a game.json of an older editor): no mixer, the default voices
        if (const auto audio = in.find("audio"); audio != in.end()) {
            config.audioMixer = audio->value("mixer", "");
            config.audioMaxRealVoices = audio->value("maxRealVoices", config.audioMaxRealVoices);
            config.audioMaxVoices = audio->value("maxVoices", config.audioMaxVoices);
            config.audioSampleRate = audio->value("sampleRate", config.audioSampleRate);
            config.audioSeed = audio->value("seed", config.audioSeed);
            config.audioOutputStage = audio->value("outputStage", config.audioOutputStage);
            config.audioIosSession = audio->value("iosSessionCategory", config.audioIosSession);
            config.audioMusicGroup = audio->value("musicGroup", config.audioMusicGroup);
            config.audioUnusedClipCacheMb = audio->value("unusedClipCacheMb", config.audioUnusedClipCacheMb);
        }
        // absent: no native plugin of the project is for other platforms only
        if (const auto native = in.find("nativePlatformOnly"); native != in.end() && native->is_object()) {
            for (const auto& [plugin, message] : native->items()) {
                if (message.is_string()) config.nativePlatformOnly[plugin] = message.get<std::string>();
            }
        }
        if (const auto byTarget = in.find("nativePlatformOnlyByTarget"); byTarget != in.end() && byTarget->is_object()) {
            for (const auto& [target, plugins] : byTarget->items()) {
                if (!plugins.is_object()) continue;
                for (const auto& [plugin, message] : plugins.items())
                    if (message.is_string()) config.nativePlatformOnlyByTarget[target][plugin] = message.get<std::string>();
            }
        }
        if (const auto player = in.find("player"); player != in.end() && player->is_object()) {
            config.hasPlayerSettings = true;
            config.targetFrameRate = player->value("targetFrameRate", config.targetFrameRate);
            config.defaultLanguage = player->value("defaultLanguage", config.defaultLanguage);
            config.defaultOrientation = static_cast<ScreenOrientation>(player->value("defaultOrientation", static_cast<int>(config.defaultOrientation)));
            config.allowedOrientations = player->value("allowedOrientations", config.allowedOrientations);
            config.keepScreenOn = player->value("keepScreenOn", config.keepScreenOn);
            config.allowInsecureHttp = player->value("allowInsecureHttp", config.allowInsecureHttp);
            if (const auto apps = player->find("applicationInfo"); apps != player->end() && apps->is_object()) {
                config.linuxApp = appInfoFrom(apps->value("linux", json()));
                config.windowsApp = appInfoFrom(apps->value("windows", json()));
                config.macosApp = appInfoFrom(apps->value("macos", json()));
                config.webApp = appInfoFrom(apps->value("web", json()));
            }
        }
        if (const auto creation = in.find("shaderCreation"); creation != in.end() && creation->is_object()) {
            config.shaderCreation.asyncCreation = creation->value("asyncCreation", config.shaderCreation.asyncCreation);
            config.shaderCreation.mainThreadBudgetMs = creation->value("mainThreadBudgetMs", config.shaderCreation.mainThreadBudgetMs);
            config.shaderCreation.glWarmDraws = creation->value("glWarmDraws", config.shaderCreation.glWarmDraws);
            config.shaderCreation.holdFirstSceneForWarmup = creation->value("holdFirstSceneForWarmup", config.shaderCreation.holdFirstSceneForWarmup);
        }
        config.gpuInstancing = in.value("gpuInstancing", config.gpuInstancing);
        if (const auto globals = in.find("shaderGlobals"); globals != in.end() && globals->is_array()) {
            for (const json& global : *globals) {
                if (!global.is_object()) continue;
                ShaderGlobalDecl decl;
                decl.name = global.value("name", "");
                valueOf(kShaderGlobalTypes, global.value("type", std::string("vector4")), decl.type);
                decl.arraySize = global.value("arraySize", 0u);
                for (const json& value : global.value("default", json::array())) {
                    const std::vector<float> v = value.is_array() ? value.get<std::vector<float>>() : std::vector<float>{};
                    decl.defaultValue.push_back(v.size() == 4 ? Vector4(v[0], v[1], v[2], v[3]) : Vector4(0.0f, 0.0f, 0.0f, 0.0f));
                }
                decl.defaultTexture = global.value("texture", "");
                if (!decl.name.empty()) config.shaderGlobals.push_back(std::move(decl));
            }
        }
        if (const auto canvas = in.find("canvas"); canvas != in.end()) {
            if (const auto scaler = canvas->find("scaler"); scaler != canvas->end() && scaler->is_object()) {
                CanvasScalerSettings& c = config.canvasScaler;
                valueOf(kScaleModes, scaler->value("mode", std::string("follow_game_canvas")), c.mode);
                c.scaleFactor = scaler->value("scaleFactor", c.scaleFactor);
                const std::vector<float> reference = scaler->value("referenceResolution", std::vector<float>{});
                if (reference.size() == 2) c.referenceResolution = Vector2(reference[0], reference[1]);
                valueOf(kMatchModes, scaler->value("screenMatchMode", std::string("match_width_or_height")), c.screenMatchMode);
                c.matchWidthOrHeight = scaler->value("matchWidthOrHeight", c.matchWidthOrHeight);
                valueOf(kPhysicalUnits, scaler->value("physicalUnit", std::string("points")), c.physicalUnit);
                c.fallbackScreenDPI = scaler->value("fallbackScreenDPI", c.fallbackScreenDPI);
            }
        }
        if (const auto physics = in.find("physics3D"); physics != in.end() && physics->is_object()) {
            config.hasPhysics3DCapacity = true;
            config.maxBodies3D = physics->value("maxBodies", config.maxBodies3D);
            config.maxBodyPairs3D = physics->value("maxBodyPairs", config.maxBodyPairs3D);
            config.maxContactConstraints3D = physics->value("maxContactConstraints", config.maxContactConstraints3D);
        }
        if (const auto debug = in.find("debug"); debug != in.end() && debug->is_object()) {
            config.hasDebugSettings = true;
            valueOf(kBuildModes, debug->value("overlay", std::string("development")), config.debugOverlayMode);
            valueOf(kBuildModes, debug->value("allowLua", std::string("development")), config.debugAllowLuaMode);
            DebugOverlaySettings& overlay = config.debugOverlay;
            overlay.toggleKey = debug->value("toggleKey", overlay.toggleKey);
            overlay.statsKey = debug->value("statsKey", overlay.statsKey);
            valueOf(kGestures, debug->value("gesture", std::string("three_finger_hold")), overlay.gesture);
            valueOf(kPopups, debug->value("popup", std::string("touch_devices")), overlay.popup);
            overlay.openOnError = debug->value("openOnError", overlay.openOnError);
            valueOf(kStats, debug->value("stats", std::string("off")), overlay.stats);
            valueOf(kCorners, debug->value("statsCorner", std::string("top_left")), overlay.statsCorner);
            config.logBufferSize = debug->value("logBufferSize", config.logBufferSize);
            valueOf(kStackTraces, debug->value("logStackTraces", std::string("warnings")), config.logStackTraces);
        }
        if (config.title.empty()) config.title = config.name;
        if (config.storageId.empty()) config.storageId = config.appId.empty() ? config.name : config.appId;
        out = std::move(config);
        return true;
    } catch (const json::exception& e) {
        error = std::string("game.json: ") + e.what();
        return false;
    }
}

bool PlayerConfig::parse(const std::string& text, PlayerConfig& out, std::string& error) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        error = "game.json is not valid JSON";
        return false;
    }
    return fromJson(parsed, out, error);
}

void jasperengine::player::applyAudio(const PlayerConfig& game) {
    const AudioConfig defaults;
    const AudioOutputStage stage = AudioSystem::outputStageFromName(game.audioOutputStage);
    const IosAudioSession session = AudioSystem::iosSessionFromName(game.audioIosSession);
    if (game.audioMaxRealVoices != defaults.maxRealVoices || game.audioMaxVoices != defaults.maxVoices ||
        game.audioSampleRate != defaults.sampleRate || stage != defaults.outputStage || session != defaults.iosSession ||
        game.audioSeed != (int)defaults.seed || game.audioMusicGroup != defaults.musicGroup) {
        AudioSystem::configure({game.audioMaxRealVoices, game.audioMaxVoices, game.audioSampleRate, stage, session, (uint32_t)game.audioSeed,
                                game.audioMusicGroup});
    }
    if (game.audioUnusedClipCacheMb >= 0 && (uint64_t)game.audioUnusedClipCacheMb * 1024 * 1024 != SoundPool::defaultUnusedBudget) {
        SoundPool::setUnusedBudget((uint64_t)game.audioUnusedClipCacheMb * 1024 * 1024);
    }
    if (!game.audioMixer.empty()) {
        AudioMixer::load(game.audioMixer);
    }
}

const GameScene* PlayerConfig::findScene(uint32_t id) const {
    for (const GameScene& scene : scenes)
        if (scene.id == id) return &scene;
    return nullptr;
}

const GameScene* PlayerConfig::findScene(const std::string& nameOrId) const {
    for (const GameScene& scene : scenes)
        if (scene.name == nameOrId) return &scene;
    try {
        size_t used = 0;
        const unsigned long id = std::stoul(nameOrId, &used);
        if (used == nameOrId.size()) return findScene(static_cast<uint32_t>(id));
    } catch (...) {
    }
    return nullptr;
}
