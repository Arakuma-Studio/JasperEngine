// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerConfig.h"

#include "audio/AudioMixer.h"
#include "subsystem/AudioSystem.h"

#include <algorithm>

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
    const EnumNames<PbrTier> kPbrTier[] = {{PbrTier::FULL, "full"}, {PbrTier::REDUCED, "reduced"}};
    const EnumNames<ShadowTier> kShadowTier[] = {{ShadowTier::LOW, "low"}, {ShadowTier::MEDIUM, "medium"}, {ShadowTier::HIGH, "high"},
        {ShadowTier::ULTRA, "ultra"}};
    const char* const kWindowModes[] = {"windowed", "maximized", "fullscreen"};

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
                     {"textureStrategy", nameOf(kTextureStrategy, textureStrategy)}, {"pixelsPerUnit", pixelsPerUnit}};
    out["loading"] = {{"scene", loadingScene}, {"delay", loadingDelay}, {"timeout", loadingTimeout}, {"async", asyncLoading}};
    out["threading"] = {{"workers", threading.workers}, {"reservedFrameWorkers", threading.reservedFrameWorkers},
                        {"ioThreads", threading.ioThreads}, {"webPumpBudgetMs", threading.webPumpBudgetMs}};
    out["atmosphereQuality"] = {nameOf(kAtmosphere, atmosphereQuality[0]), nameOf(kAtmosphere, atmosphereQuality[1]),
                                nameOf(kAtmosphere, atmosphereQuality[2])};
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
    json textures = json::array();
    for (const Engine::TexturePlatformDefaults& group : textureDefaults)
        textures.push_back({{"quality", group.quality}, {"memoryBudgetMB", group.memoryBudgetMB}, {"lowMemoryFallback", group.lowMemoryFallback}});
    out["textureDefaults"] = textures;
    out["htmlUi"] = {{"msaa", htmlUiMsaa}, {"glyphAtlasMiB", glyphAtlasMiB}, {"sdfPrimitives", sdfPrimitives},
                     {"transformAwareText", transformAwareText}};
    out["mobilePbrTier"] = nameOf(kPbrTier, mobilePbrTier);
    out["dynamicBatching"] = dynamicBatching;
    out["lightMode"] = LightingTiers::getModeName(lightMode);
    json tiers = json::array();
    for (ShadowTier tier : shadowTiers) tiers.push_back(nameOf(kShadowTier, tier));
    out["shadowTiers"] = tiers;
    out["device"] = json::parse(device.toJson());
    out["tags"] = tags;
    out["layers"] = layerNames;
    out["collision3D"] = collision3D;
    out["collision2D"] = collision2D;
    out["logging"] = {{"level", logLevel}, {"file", logFile}, {"backtrace", logBacktrace}};
    out["crash"] = {{"handler", crashHandler}, {"logTail", crashLogTail}, {"dumpWithHeap", crashDumpWithHeap},
                    {"hangWatchdog", hangWatchdog}, {"hangMs", hangMs}, {"webOverlay", crashWebOverlay}};
    out["audio"] = {{"mixer", audioMixer}, {"maxRealVoices", audioMaxRealVoices}, {"maxVoices", audioMaxVoices},
                    {"sampleRate", audioSampleRate}, {"outputStage", audioOutputStage}, {"iosSessionCategory", audioIosSession}, {"seed", audioSeed},
                    {"musicGroup", audioMusicGroup}};
    if (!nativePlatformOnly.empty()) out["nativePlatformOnly"] = nativePlatformOnly;
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
        }
        if (const auto loading = in.find("loading"); loading != in.end()) {
            config.loadingScene = loading->value("scene", 0u);
            config.loadingDelay = loading->value("delay", config.loadingDelay);
            config.loadingTimeout = loading->value("timeout", config.loadingTimeout);
            config.asyncLoading = loading->value("async", config.asyncLoading);
        }
        if (const auto threading = in.find("threading"); threading != in.end()) {
            config.threading.workers = threading->value("workers", config.threading.workers);
            config.threading.reservedFrameWorkers = threading->value("reservedFrameWorkers", config.threading.reservedFrameWorkers);
            config.threading.ioThreads = threading->value("ioThreads", config.threading.ioThreads);
            config.threading.webPumpBudgetMs = threading->value("webPumpBudgetMs", config.threading.webPumpBudgetMs);
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
        config.tags = in.value("tags", std::vector<std::string>{});
        config.layerNames = in.value("layers", std::vector<std::string>{});
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
        }
        // absent: no native plugin of the project is for other platforms only
        if (const auto native = in.find("nativePlatformOnly"); native != in.end() && native->is_object()) {
            for (const auto& [plugin, message] : native->items()) {
                if (message.is_string()) config.nativePlatformOnly[plugin] = message.get<std::string>();
            }
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
