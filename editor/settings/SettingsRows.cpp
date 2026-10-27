// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in settings rows (S25 R29). The first part is the MCP settings table that
// get_project_settings and set_project_settings used before S25 (moved from
// mcp/tools/ProjectTools.cpp, every key with its type, get and set unchanged); each of its
// keys then gets the page, section and label the windows show it under. The rest are the rows
// the windows draw that MCP does not set (their get reads the value).

#include "SettingsRegistry.h"

#include "App.h"
#include "AppSettings.h"
#include "Out.h"
#include "SceneBackup.h"
#include "Project.h"
#include "input/InputActions.h"
#include "log/Logger.h"
#include "mcp/McpRefs.h"
#include "registry/TagManager.h"
#include "util/CxxStandards.h"
#include "util/FileUtils.h"
#include "window/ConsoleWindow.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>

namespace jasperengine::editor {

namespace {

namespace fs = std::filesystem;
using mcp::Json;

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}


struct Setting {
    std::string key;
    std::string type;          // bool, int, number, string, enum, path, list, object
    std::string description;
    std::vector<std::string> values;   // enum values
    std::function<Json(SettingsContext&)> get;
    // Validates `value`; applies it when apply is true. Returns false with error.
    std::function<bool(SettingsContext&, const Json& value, bool apply, std::string& error)> set;
    bool preference = false;   // editor/machine preference, not project.yaml
};

template <typename T>
struct EnumMap {
    const char* name;
    T value;
};

const EnumMap<Scaling> scalingModes[] = {
    {"fit_width", Scaling::FITWIDTH}, {"fit_height", Scaling::FITHEIGHT}, {"letterbox", Scaling::LETTERBOX},
    {"crop", Scaling::CROP}, {"stretch", Scaling::STRETCH}, {"native", Scaling::NATIVE}};
const EnumMap<TextureStrategy> textureStrategies[] = {
    {"fit", TextureStrategy::FIT}, {"resize", TextureStrategy::RESIZE}, {"none", TextureStrategy::NONE}};
const EnumMap<PbrTier> pbrTiers[] = {{"full", PbrTier::FULL}, {"reduced", PbrTier::REDUCED}};
const EnumMap<AtmosphereQuality> atmosphereQualities[] = {
    {"off", AtmosphereQuality::OFF}, {"low", AtmosphereQuality::LOW}, {"medium", AtmosphereQuality::MEDIUM}, {"high", AtmosphereQuality::HIGH}};
const EnumMap<ShadowTier> shadowTiers[] = {
    {"low", ShadowTier::LOW}, {"medium", ShadowTier::MEDIUM}, {"high", ShadowTier::HIGH}, {"ultra", ShadowTier::ULTRA}};
const EnumMap<WindowMode> windowModes[] = {
    {"windowed", WindowMode::WINDOWED}, {"maximized", WindowMode::MAXIMIZED}, {"fullscreen", WindowMode::FULLSCREEN}};
const EnumMap<AndroidOrientation> androidOrientations[] = {
    {"unspecified", AndroidOrientation::Unspecified}, {"portrait", AndroidOrientation::Portrait},
    {"landscape", AndroidOrientation::Landscape}, {"sensor_portrait", AndroidOrientation::SensorPortrait},
    {"sensor_landscape", AndroidOrientation::SensorLandscape}, {"full_sensor", AndroidOrientation::FullSensor}};

template <typename T, size_t N>
std::vector<std::string> enumNames(const EnumMap<T> (&map)[N]) {
    std::vector<std::string> names;
    for (const auto& item : map) names.push_back(item.name);
    return names;
}

template <typename T, size_t N>
std::string enumName(const EnumMap<T> (&map)[N], T value) {
    for (const auto& item : map) {
        if (item.value == value) return item.name;
    }
    return map[0].name;
}

template <typename T, size_t N>
bool parseEnum(const EnumMap<T> (&map)[N], const Json& value, T& out, std::string& error) {
    if (value.is_string()) {
        const std::string text = lowerText(value.get<std::string>());
        for (const auto& item : map) {
            if (text == item.name) {
                out = item.value;
                return true;
            }
        }
    }
    error = "must be one of:";
    for (const auto& item : map) error += std::string(" ") + item.name;
    return false;
}

bool readBool(const Json& value, bool& out, std::string& error) {
    if (!value.is_boolean()) {
        error = "must be true or false";
        return false;
    }
    out = value.get<bool>();
    return true;
}

bool readInt(const Json& value, long long minimum, long long maximum, long long& out, std::string& error) {
    if (!value.is_number_integer() && !(value.is_number_float() && value.get<double>() == static_cast<long long>(value.get<double>()))) {
        error = "must be an integer";
        return false;
    }
    out = value.is_number_integer() ? value.get<long long>() : static_cast<long long>(value.get<double>());
    if (out < minimum || out > maximum) {
        error = "must be between " + std::to_string(minimum) + " and " + std::to_string(maximum);
        return false;
    }
    return true;
}

bool readNumber(const Json& value, double minimum, double maximum, double& out, std::string& error) {
    if (!value.is_number()) {
        error = "must be a number";
        return false;
    }
    out = value.get<double>();
    if (out < minimum || out > maximum) {
        std::ostringstream text;
        text << "must be between " << minimum << " and " << maximum;
        error = text.str();
        return false;
    }
    return true;
}

bool readString(const Json& value, std::string& out, std::string& error, size_t maxLength = 4096) {
    if (!value.is_string()) {
        error = "must be a string";
        return false;
    }
    out = value.get<std::string>();
    if (out.size() > maxLength) {
        error = "is too long (max " + std::to_string(maxLength) + " characters)";
        return false;
    }
    return true;
}

// "" clears; otherwise an existing project-relative file
bool readProjectFile(SettingsContext& ctx, const Json& value, fs::path& out, std::string& error) {
    std::string text;
    if (!readString(value, text, error)) return false;
    if (text.empty()) {
        out.clear();
        return true;
    }
    fs::path absolute;
    if (!mcp::McpRefs::resolveProjectPath(ctx.project(), text, absolute, error, true)) return false;
    out = FileUtils::pathFromUtf8(text).lexically_normal();
    return true;
}

// Validation shared with Project Settings (window/dialog/ProjectSettingsWindow.cpp)
bool isNumericVersion(const std::string& value, int maxParts, unsigned long long maxPart) {
    if (value.empty()) return false;
    int parts = 0;
    size_t start = 0;
    while (true) {
        const size_t dot = value.find('.', start);
        const size_t end = dot == std::string::npos ? value.size() : dot;
        if (start >= end) return false;
        unsigned long long part = 0;
        for (size_t i = start; i < end; i++) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
            part = part * 10 + static_cast<unsigned>(value[i] - '0');
            if (part > maxPart) return false;
        }
        if (++parts > maxParts) return false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

bool isDottedIdentifier(const std::string& value, bool (*partValid)(const std::string&, size_t, size_t)) {
    if (value.empty() || value.back() == '.') return false;
    size_t start = 0;
    int parts = 0;
    while (start < value.size()) {
        const size_t dot = value.find('.', start);
        const size_t end = dot == std::string::npos ? value.size() : dot;
        if (!partValid(value, start, end)) return false;
        parts++;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts >= 2;
}

bool sharedIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || !std::isalpha(static_cast<unsigned char>(v[s]))) return false;
    for (size_t i = s; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i]))) return false;
    return true;
}
bool javaIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || (!std::isalpha(static_cast<unsigned char>(v[s])) && v[s] != '_')) return false;
    for (size_t i = s + 1; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i])) && v[i] != '_') return false;
    return true;
}
bool appleIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || v[s] == '-' || v[e - 1] == '-') return false;
    for (size_t i = s; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i])) && v[i] != '-') return false;
    return true;
}

using Validator = std::function<bool(const std::string&)>;

Setting stringSetting(const std::string& key, const std::string& description,
                      std::function<std::string&(Project&)> field, Validator valid = nullptr,
                      const std::string& validHint = {}) {
    Setting s;
    s.key = key;
    s.type = "string";
    s.description = description;
    s.get = [field](SettingsContext& ctx) { return Json(field(*ctx.project())); };
    s.set = [field, valid, validHint](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        std::string text;
        if (!readString(value, text, error)) return false;
        if (!text.empty() && valid && !valid(text)) {
            error = "\"" + text + "\" is not valid: " + validHint;
            return false;
        }
        if (apply) field(*ctx.project()) = text;
        return true;
    };
    return s;
}

Setting boolSetting(const std::string& key, const std::string& description,
                    std::function<bool(Project&)> get, std::function<void(Project&, bool)> set) {
    Setting s;
    s.key = key;
    s.type = "bool";
    s.description = description;
    s.get = [get](SettingsContext& ctx) { return Json(get(*ctx.project())); };
    s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        bool b;
        if (!readBool(value, b, error)) return false;
        if (apply) set(*ctx.project(), b);
        return true;
    };
    return s;
}

Setting boolField(const std::string& key, const std::string& description, std::function<bool&(Project&)> field) {
    return boolSetting(key, description, [field](Project& p) { return field(p); },
                       [field](Project& p, bool v) { field(p) = v; });
}

Setting pathSetting(const std::string& key, const std::string& description, std::function<fs::path&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "path";
    s.description = description + " Project-relative file path; \"\" clears it.";
    s.get = [field](SettingsContext& ctx) { return Json(FileUtils::pathToGenericUtf8(field(*ctx.project()))); };
    s.set = [field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        fs::path path;
        if (!readProjectFile(ctx, value, path, error)) return false;
        if (apply) field(*ctx.project()) = path;
        return true;
    };
    return s;
}

Setting intField(const std::string& key, const std::string& description, long long minimum, long long maximum,
                 std::function<unsigned int&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "int";
    s.description = description;
    s.get = [field](SettingsContext& ctx) { return Json(field(*ctx.project())); };
    s.set = [field, minimum, maximum](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        long long v;
        if (!readInt(value, minimum, maximum, v, error)) return false;
        if (apply) field(*ctx.project()) = static_cast<unsigned int>(v);
        return true;
    };
    return s;
}

Json layerCollisionJson(const LayerCollisionRows& rows) {
    // Only what differs from "everything collides": the pairs that don't
    Json pairs = Json::array();
    for (int a = 0; a < MAX_LAYERS; a++) {
        for (int b = a; b < MAX_LAYERS; b++) {
            const bool collide = (rows[a] & (1u << b)) && (rows[b] & (1u << a));
            if (!collide) {
                const std::string nameA = TagManager::layerToName(a);
                const std::string nameB = TagManager::layerToName(b);
                pairs.push_back(Json::array({nameA.empty() ? Json(a) : Json(nameA), nameB.empty() ? Json(b) : Json(nameB)}));
            }
        }
    }
    return pairs;
}

bool parseLayer(const Json& value, int& layer, std::string& error) {
    if (value.is_number_integer()) {
        layer = value.get<int>();
        if (layer < 0 || layer >= MAX_LAYERS) {
            error = "layer index " + std::to_string(layer) + " is outside 0-31";
            return false;
        }
        return true;
    }
    if (value.is_string()) {
        layer = TagManager::nameToLayer(value.get<std::string>());
        if (layer < 0) {
            error = "no layer is named \"" + value.get<std::string>() + "\" (name it in tags_and_layers.layers first, or use the index)";
            return false;
        }
        return true;
    }
    error = "a layer is an index (0-31) or a layer name";
    return false;
}

Setting layerCollisionSetting(const std::string& key, bool is3D) {
    Setting s;
    s.key = key;
    s.type = "list";
    s.description = std::string("Layer Collision Matrix of ") + (is3D ? "Physics (3D)" : "Physics 2D") +
                    ": the layer pairs that do NOT collide, as [[a, b], ...] with layer names or indexes "
                    "(everything else collides). Setting it replaces the whole list; [] makes every layer collide. Layer names must "
                    "exist before the call (name them with tags_and_layers.layers in an earlier call) or use indexes.";
    s.get = [is3D](SettingsContext& ctx) {
        Project* p = ctx.project();
        return layerCollisionJson(is3D ? p->getLayerCollisionMatrix3D() : p->getLayerCollisionMatrix2D());
    };
    s.set = [is3D](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        if (!value.is_array()) {
            error = "must be a list of [layerA, layerB] pairs";
            return false;
        }
        LayerCollisionRows rows = allLayersCollide();
        for (const Json& pair : value) {
            if (!pair.is_array() || pair.size() != 2) {
                error = "each entry must be a [layerA, layerB] pair";
                return false;
            }
            int a, b;
            if (!parseLayer(pair[0], a, error) || !parseLayer(pair[1], b, error)) return false;
            rows[a] &= ~(1u << b);
            rows[b] &= ~(1u << a);
        }
        if (apply) {
            Project* p = ctx.project();
            if (is3D) p->setLayerCollisionMatrices(rows, p->getLayerCollisionMatrix2D());
            else p->setLayerCollisionMatrices(p->getLayerCollisionMatrix3D(), rows);
        }
        return true;
    };
    return s;
}

std::vector<Setting> mcpSettingsTable() {
    return [] {
        std::vector<Setting> t;
        auto add = [&t](Setting s) { t.push_back(std::move(s)); };

        // ---- player
        {
            Setting s;
            s.key = "player.product_name";
            s.type = "string";
            s.description = "Project name (Product Name): the editor title, the default app/window name and the executable name.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getName()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 255)) return false;
                if (text.empty()) {
                    error = "must not be empty";
                    return false;
                }
                if (apply && text != ctx.project()->getName()) ctx.project()->setName(text);
                return true;
            };
            add(s);
        }
        add(stringSetting("player.application_name", "Application display name shared by every platform; \"\" uses the project name.",
                          [](Project& p) -> std::string& { return p.getApplicationSettings().name; }));
        {
            Setting s = stringSetting("player.identifier", "Application identifier (reverse domain, letters and digits, e.g. com.mycompany.game) shared by every platform.",
                                      [](Project& p) -> std::string& { return p.getApplicationSettings().identifier; },
                                      [](const std::string& v) { return isDottedIdentifier(v, sharedIdPart); },
                                      "two or more dot-separated parts of letters and digits, each starting with a letter");
            add(s);
        }
        add(stringSetting("player.version", "Application version (1 to 3 numeric parts, e.g. 1.2.0).",
                          [](Project& p) -> std::string& { return p.getApplicationSettings().version; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts, each 0-65535"));
        add(intField("player.build", "Build number (Apple build number, Android version code).", 1, 2100000000,
                     [](Project& p) -> unsigned int& { return p.getApplicationSettings().build; }));
        {
            Setting s;
            s.key = "player.window_mode";
            s.type = "enum";
            s.values = enumNames(windowModes);
            s.description = "Desktop window mode of the built player.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(windowModes, ctx.project()->getWindowMode())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                WindowMode mode;
                if (!parseEnum(windowModes, value, mode, error)) return false;
                if (apply) ctx.project()->setWindowMode(mode);
                return true;
            };
            add(s);
        }
        for (const bool width : {true, false}) {
            Setting s;
            s.key = width ? "player.window_width" : "player.window_height";
            s.type = "int";
            s.description = std::string("Desktop window ") + (width ? "width" : "height") + " in pixels.";
            s.get = [width](SettingsContext& ctx) {
                return Json(width ? ctx.project()->getWindowWidth() : ctx.project()->getWindowHeight());
            };
            s.set = [width](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 16384, v, error)) return false;
                if (apply) {
                    Project* p = ctx.project();
                    if (width) p->setWindowSize(static_cast<unsigned>(v), p->getWindowHeight());
                    else p->setWindowSize(p->getWindowWidth(), static_cast<unsigned>(v));
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("player.window_resizable", "The desktop window can be resized.",
                        [](Project& p) { return p.isWindowResizable(); }, [](Project& p, bool v) { p.setWindowResizable(v); }));
        {
            Setting s;
            s.key = "player.window_title";
            s.type = "string";
            s.description = "Desktop window title; \"\" uses the project name.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getWindowTitle()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 255)) return false;
                if (apply) ctx.project()->setWindowTitle(text);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.window_icon";
            s.type = "path";
            s.description = "Default icon (desktop window and platforms without their own), a project-relative image; \"\" clears it.";
            s.get = [](SettingsContext& ctx) { return Json(FileUtils::pathToGenericUtf8(ctx.project()->getWindowIcon())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                fs::path path;
                if (!readProjectFile(ctx, value, path, error)) return false;
                if (apply) ctx.project()->setWindowIcon(path);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_scene";
            s.type = "scene";
            s.description = "Splash/loading scene shown while the start scene loads: a saved scene (id, name or .scene path), or null for none.";
            s.get = [](SettingsContext& ctx) {
                const SceneProject* scene = ctx.project()->getScene(ctx.project()->getLoadingSettings().sceneId);
                return scene ? Json{{"id", scene->id}, {"name", scene->name}} : Json(nullptr);
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                uint32_t id = 0;
                if (!value.is_null() && !(value.is_number_integer() && value.get<long long>() == 0)) {
                    SceneProject* scene = mcp::McpRefs::resolveScene(ctx.project(), value, error);
                    if (!scene) return false;
                    if (scene->filepath.empty()) {
                        error = "the scene is not saved, so it is not in the build";
                        return false;
                    }
                    id = scene->id;
                }
                if (apply) ctx.project()->getLoadingSettings().sceneId = id;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_delay";
            s.type = "number";
            s.description = "Seconds the loading scene stays at least.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getLoadingSettings().delay); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 600.0, v, error)) return false;
                if (apply) ctx.project()->getLoadingSettings().delay = static_cast<float>(v);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_timeout";
            s.type = "number";
            s.description = "Seconds before the loading scene gives up waiting.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getLoadingSettings().timeout); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 3600.0, v, error)) return false;
                if (apply) ctx.project()->getLoadingSettings().timeout = static_cast<float>(v);
                return true;
            };
            add(s);
        }
        add(boolField("player.async_loading", "Load the start scene asynchronously behind the loading scene.",
                      [](Project& p) -> bool& { return p.getLoadingSettings().asyncLoading; }));
        // ---- threading (Project Settings > Player > Threading)
        {
            struct ThreadCount {
                const char* key;
                const char* description;
                int minimum;
                int maximum;
                int ThreadingProjectSettings::*field;
            };
            const ThreadCount counts[] = {
                {"player.threading.workers", "Job system workers: -1 Auto, 1-15. Applies on the next launch.", 1, 15, &ThreadingProjectSettings::workers},
                {"player.threading.reserved_frame_workers", "Workers kept free of Background work: -1 Auto, 0-4. Applies live.", 0, 4, &ThreadingProjectSettings::reservedFrameWorkers},
                {"player.threading.io_threads", "Asset reader threads: -1 Auto, 0-4. Applies on the next launch.", 0, 4, &ThreadingProjectSettings::ioThreads},
            };
            for (const ThreadCount& count : counts) {
                Setting s;
                s.key = count.key;
                s.type = "int";
                s.description = count.description;
                const auto field = count.field;
                const int minimum = count.minimum;
                const int maximum = count.maximum;
                s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getThreadingSettings().*field); };
                s.set = [field, minimum, maximum](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                    long long v;
                    if (!readInt(value, -1, maximum, v, error)) return false;
                    if (v >= 0 && v < minimum) {
                        error = "must be -1 (Auto) or " + std::to_string(minimum) + "-" + std::to_string(maximum);
                        return false;
                    }
                    if (apply) {
                        ThreadingProjectSettings settings = ctx.project()->getThreadingSettings();
                        settings.*field = static_cast<int>(v);
                        ctx.project()->setThreadingSettings(settings);
                    }
                    return true;
                };
                add(s);
            }
        }
        {
            Setting s;
            s.key = "player.threading.web_pump_budget_ms";
            s.type = "number";
            s.description = "Web builds without threads: Background job time per frame (ms, 0.5-50).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getThreadingSettings().webPumpBudgetMs); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.5, 50.0, v, error)) return false;
                if (apply) {
                    ThreadingProjectSettings settings = ctx.project()->getThreadingSettings();
                    settings.webPumpBudgetMs = static_cast<float>(v);
                    ctx.project()->setThreadingSettings(settings);
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("player.threading.web_multithreaded", "Web export also builds a pthreads player, picked when the page is cross-origin isolated.",
                        [](Project& p) { return p.getThreadingSettings().webMultithreaded; },
                        [](Project& p, bool v) { ThreadingProjectSettings settings = p.getThreadingSettings(); settings.webMultithreaded = v; p.setThreadingSettings(settings); }));
        add(boolSetting("player.development_build", "Development Build: Release builds keep the profiler and --profile/--profile-capture (S15).",
                        [](Project& p) { return p.isDevelopmentBuild(); }, [](Project& p, bool v) { p.setDevelopmentBuild(v); }));
        add(boolSetting("player.pack_native_resources", "Experimental: pack assets and Lua into resources.pak in native builds instead of copying folders.",
                        [](Project& p) { return p.shouldPackNativeResources(); }, [](Project& p, bool v) { p.setPackNativeResources(v); }));

        // ---- per platform (overrides: "" inherits the shared player value)
        add(stringSetting("web.application_name", "Web page title override.", [](Project& p) -> std::string& { return p.getWebProjectSettings().applicationName; }));
        add(pathSetting("web.favicon", "Web favicon.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().favicon; }));
        add(pathSetting("web.custom_html_shell", "Custom Emscripten HTML shell.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().customHtmlShell; }));
        add(stringSetting("web.head_include", "Extra HTML put in the page <head>.", [](Project& p) -> std::string& { return p.getWebProjectSettings().headInclude; }));
        add(boolField("web.resize_canvas_to_window", "The canvas follows the browser window size.", [](Project& p) -> bool& { return p.getWebProjectSettings().resizeCanvasToWindow; }));
        add(boolField("web.hide_emscripten_ui", "Hide the Emscripten status/console UI.", [](Project& p) -> bool& { return p.getWebProjectSettings().hideEmscriptenUI; }));
        add(stringSetting("linux.application_name", "Linux .desktop name override.", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().applicationName; }));
        add(stringSetting("linux.comment", "Linux .desktop comment.", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().comment; }));
        add(stringSetting("linux.categories", "Linux .desktop categories (e.g. Game;).", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().categories; }));
        add(stringSetting("windows.product_name", "Windows product name override.", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().productName; }));
        add(stringSetting("windows.company_name", "Windows company name.", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().companyName; }));
        add(stringSetting("windows.file_version", "Windows file version override (1-4 numeric parts).", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().fileVersion; },
                          [](const std::string& v) { return isNumericVersion(v, 4, 65535); }, "1 to 4 numeric parts"));
        add(stringSetting("windows.product_version", "Windows product version override (1-4 numeric parts).", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().productVersion; },
                          [](const std::string& v) { return isNumericVersion(v, 4, 65535); }, "1 to 4 numeric parts"));
        add(stringSetting("macos.application_name", "macOS name override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().applicationName; }));
        add(stringSetting("macos.bundle_identifier", "macOS bundle identifier override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().bundleIdentifier; },
                          [](const std::string& v) { return isDottedIdentifier(v, appleIdPart); }, "dot-separated parts of letters, digits and hyphens"));
        add(stringSetting("macos.version_name", "macOS version override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().versionName; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts"));
        add(stringSetting("macos.build_number", "macOS build number override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().buildNumber; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 0xFFFFFFFFull); }, "1 to 3 numeric parts"));
        add(pathSetting("macos.icon", "macOS icon.", [](Project& p) -> fs::path& { return p.getMacOSProjectSettings().icon; }));
        add(boolField("macos.high_dpi", "macOS high DPI (Retina) rendering.", [](Project& p) -> bool& { return p.getMacOSProjectSettings().highDpi; }));
        add(stringSetting("ios.application_name", "iOS name override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().applicationName; }));
        add(stringSetting("ios.bundle_identifier", "iOS bundle identifier override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().bundleIdentifier; },
                          [](const std::string& v) { return isDottedIdentifier(v, appleIdPart); }, "dot-separated parts of letters, digits and hyphens"));
        add(stringSetting("ios.version_name", "iOS version override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().versionName; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts"));
        add(stringSetting("ios.build_number", "iOS build number override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().buildNumber; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 0xFFFFFFFFull); }, "1 to 3 numeric parts"));
        add(pathSetting("ios.icon", "iOS icon.", [](Project& p) -> fs::path& { return p.getIOSProjectSettings().icon; }));
        add(boolField("ios.hide_status_bar", "Hide the iOS status bar.", [](Project& p) -> bool& { return p.getIOSProjectSettings().hideStatusBar; }));
        add(boolField("ios.hide_home_indicator", "Hide the iOS home indicator.", [](Project& p) -> bool& { return p.getIOSProjectSettings().hideHomeIndicator; }));
        add(boolField("ios.supports_high_refresh_rate", "Allow 120 Hz on ProMotion devices.", [](Project& p) -> bool& { return p.getIOSProjectSettings().supportsHighRefreshRate; }));
        add(boolField("ios.admob_enabled", "Link Google Mobile Ads into the iOS export (System.initializeAdMob and the interstitial calls); needs ios.admob_app_id.",
                      [](Project& p) -> bool& { return p.getIOSProjectSettings().admobEnabled; }));
        add(stringSetting("ios.admob_app_id", "iOS AdMob App ID (GADApplicationIdentifier) from the AdMob console; without it the export leaves AdMob out.",
                          [](Project& p) -> std::string& { return p.getIOSProjectSettings().admobAppId; },
                          isAdMobAppIdValid, "an AdMob app ID like ca-app-pub-0000000000000000~0000000000 (not an ad unit ID)"));
        add(stringSetting("android.application_name", "Android label override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().applicationName; }));
        add(stringSetting("android.package_name", "Android package name override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().packageName; },
                          [](const std::string& v) { return isDottedIdentifier(v, javaIdPart); }, "a Java package name (dot-separated identifiers)"));
        add(intField("android.version_code", "Android version code override (0 inherits player.build).", 0, 2100000000,
                     [](Project& p) -> unsigned int& { return p.getAndroidProjectSettings().versionCode; }));
        add(stringSetting("android.version_name", "Android version name override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().versionName; }));
        add(pathSetting("android.launcher_icon", "Android launcher icon.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().launcherIcon; }));
        add(pathSetting("android.adaptive_icon_foreground", "Android adaptive icon foreground.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().adaptiveIconForeground; }));
        add(pathSetting("android.adaptive_icon_background", "Android adaptive icon background.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().adaptiveIconBackground; }));
        {
            Setting s;
            s.key = "android.min_sdk";
            s.type = "int";
            s.description = "Android minimum SDK level.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAndroidProjectSettings().minSdk); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 100, v, error)) return false;
                if (apply) {
                    auto& a = ctx.project()->getAndroidProjectSettings();
                    a.minSdk = static_cast<unsigned>(v);
                    a.targetSdk = std::max(a.targetSdk, a.minSdk);
                }
                return true;
            };
            add(s);
        }
        add(intField("android.target_sdk", "Android target SDK level.", 1, 100, [](Project& p) -> unsigned int& { return p.getAndroidProjectSettings().targetSdk; }));
        {
            Setting s;
            s.key = "android.orientation";
            s.type = "enum";
            s.values = enumNames(androidOrientations);
            s.description = "Android screen orientation.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(androidOrientations, ctx.project()->getAndroidProjectSettings().orientation)); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                AndroidOrientation o;
                if (!parseEnum(androidOrientations, value, o, error)) return false;
                if (apply) ctx.project()->getAndroidProjectSettings().orientation = o;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "android.abis";
            s.type = "list";
            s.values = {"armeabi-v7a", "arm64-v8a", "x86", "x86_64"};
            s.description = "Android ABIs to build (at least one of armeabi-v7a, arm64-v8a, x86, x86_64).";
            s.get = [](SettingsContext& ctx) {
                const auto& a = ctx.project()->getAndroidProjectSettings();
                Json list = Json::array();
                if (a.abiArmeabiV7a) list.push_back("armeabi-v7a");
                if (a.abiArm64V8a) list.push_back("arm64-v8a");
                if (a.abiX86) list.push_back("x86");
                if (a.abiX86_64) list.push_back("x86_64");
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array() || value.empty()) {
                    error = "must be a non-empty list of: armeabi-v7a, arm64-v8a, x86, x86_64";
                    return false;
                }
                bool v7 = false, v8 = false, x86 = false, x64 = false;
                for (const Json& item : value) {
                    const std::string abi = item.is_string() ? item.get<std::string>() : "";
                    if (abi == "armeabi-v7a") v7 = true;
                    else if (abi == "arm64-v8a") v8 = true;
                    else if (abi == "x86") x86 = true;
                    else if (abi == "x86_64") x64 = true;
                    else {
                        error = "unknown ABI " + item.dump() + "; use armeabi-v7a, arm64-v8a, x86, x86_64";
                        return false;
                    }
                }
                if (apply) {
                    auto& a = ctx.project()->getAndroidProjectSettings();
                    a.abiArmeabiV7a = v7;
                    a.abiArm64V8a = v8;
                    a.abiX86 = x86;
                    a.abiX86_64 = x64;
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "android.permissions";
            s.type = "list";
            s.description = "Android permissions, lower-case manifest names without the prefix (internet, vibrate, camera, record_audio, access_fine_location, ...).";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                for (const std::string& p : ctx.project()->getAndroidProjectSettings().permissions) list.push_back(p);
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of permission names";
                    return false;
                }
                std::set<std::string> permissions;
                for (const Json& item : value) {
                    if (!item.is_string() || item.get<std::string>().empty()) {
                        error = "permissions must be non-empty strings";
                        return false;
                    }
                    permissions.insert(lowerText(item.get<std::string>()));
                }
                if (apply) ctx.project()->getAndroidProjectSettings().permissions = permissions;
                return true;
            };
            add(s);
        }
        add(boolField("android.allow_backup", "Android allowBackup.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().allowBackup; }));
        add(boolField("android.fullscreen", "Android fullscreen theme.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().fullscreen; }));
        add(boolField("android.keep_screen_on", "Keep the Android screen on.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().keepScreenOn; }));
        add(boolField("android.admob_enabled", "Compile Google Mobile Ads into the Android export (System.initializeAdMob and the interstitial calls); needs android.admob_app_id.",
                      [](Project& p) -> bool& { return p.getAndroidProjectSettings().admobEnabled; }));
        add(stringSetting("android.admob_app_id", "Android AdMob App ID (com.google.android.gms.ads.APPLICATION_ID) from the AdMob console; without it the export leaves AdMob out.",
                          [](Project& p) -> std::string& { return p.getAndroidProjectSettings().admobAppId; },
                          isAdMobAppIdValid, "an AdMob app ID like ca-app-pub-0000000000000000~0000000000 (not an ad unit ID)"));

        // ---- graphics / quality
        for (const bool width : {true, false}) {
            Setting s;
            s.key = width ? "graphics.canvas_width" : "graphics.canvas_height";
            s.type = "int";
            s.description = std::string("Reference resolution ") + (width ? "width" : "height") +
                            " the game is designed for (UI and 2D coordinates), scaled to the screen by graphics.scaling_mode.";
            s.get = [width](SettingsContext& ctx) {
                return Json(width ? ctx.project()->getCanvasWidth() : ctx.project()->getCanvasHeight());
            };
            s.set = [width](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 16384, v, error)) return false;
                if (apply) {
                    Project* p = ctx.project();
                    if (width) p->setCanvasSize(static_cast<unsigned>(v), p->getCanvasHeight());
                    else p->setCanvasSize(p->getCanvasWidth(), static_cast<unsigned>(v));
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.scaling_mode";
            s.type = "enum";
            s.values = enumNames(scalingModes);
            s.description = "How the canvas maps to the screen.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(scalingModes, ctx.project()->getScalingMode())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                Scaling mode;
                if (!parseEnum(scalingModes, value, mode, error)) return false;
                if (apply) ctx.project()->setScalingMode(mode);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.texture_strategy";
            s.type = "enum";
            s.values = enumNames(textureStrategies);
            s.description = "How textures larger than the GPU limit are handled.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(textureStrategies, ctx.project()->getTextureStrategy())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                TextureStrategy v;
                if (!parseEnum(textureStrategies, value, v, error)) return false;
                if (apply) ctx.project()->setTextureStrategy(v);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.mobile_pbr_tier";
            s.type = "enum";
            s.values = enumNames(pbrTiers);
            s.description = "Built-in Lit extended lobes on Android, iOS and HTML5 exports: reduced leaves out iridescence, anisotropy direction maps and clear coat normal maps.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(pbrTiers, ctx.project()->getMobilePbrTier())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                PbrTier v;
                if (!parseEnum(pbrTiers, value, v, error)) return false;
                if (apply) ctx.project()->setMobilePbrTier(v);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.html_ui_msaa_samples";
            s.type = "int";
            s.description = "MSAA samples of the HTML/CSS UI documents' targets (1, 2, 4 or 8; default 4). Smoother "
                            "rounded corners and rotated edges at more GPU memory; devices without MSAA use 1.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getHtmlUiMsaaSamples()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 8, v, error)) return false;
                if (!Project::isValidHtmlUiMsaaSamples(static_cast<int>(v))) {
                    error = "must be 1, 2, 4 or 8";
                    return false;
                }
                if (apply) ctx.project()->setHtmlUiMsaaSamples(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        // graphics.shadow_tier.<platform>: the shadow quality tier a scene of Quality Auto renders with
        for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++) {
            Setting s;
            s.key = std::string("graphics.shadow_tier.") + Project::getShadowTierPlatformName(platform);
            s.type = "enum";
            s.values = enumNames(shadowTiers);
            s.description = std::string("Shadow quality tier of ") + Project::getShadowTierPlatformName(platform) +
                            " builds (and of the editor on it) for scenes whose shadow Quality is Auto: low, medium, high, ultra "
                            "(default " + ShadowTiers::getName(ShadowTiers::defaultTier(platform)) + "; docs/rendering/shadows.md).";
            s.get = [platform](SettingsContext& ctx) { return Json(enumName(shadowTiers, ctx.project()->getShadowTier(platform))); };
            s.set = [platform](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                ShadowTier tier;
                if (!parseEnum(shadowTiers, value, tier, error)) return false;
                if (apply) ctx.project()->setShadowTier(platform, tier);
                return true;
            };
            add(s);
        }
        add(boolSetting("graphics.runtime_shaders", "Builds carry the runtime shader compiler for variants not shipped precompiled (scripted meshes/materials). Off makes builds smaller and faster to compile.",
                        [](Project& p) { return p.isRuntimeShadersEnabled(); }, [](Project& p, bool v) { p.setRuntimeShadersEnabled(v); }));
        add(boolSetting("quality.vsync", "VSync in the built player.",
                        [](Project& p) { return p.isVSyncEnabled(); }, [](Project& p, bool v) { p.setVSyncEnabled(v); }));
        // the tiers of volumetric fog and clouds per platform group
        for (int group = 0; group < 3; group++) {
            static const char* const keys[3] = {"quality.atmosphere_quality_desktop", "quality.atmosphere_quality_mobile", "quality.atmosphere_quality_web"};
            static const char* const groups[3] = {"Windows, macOS and Linux games and the editor's views", "Android and iOS games", "HTML5 games"};
            Setting s;
            s.key = keys[group];
            s.type = "enum";
            s.values = enumNames(atmosphereQualities);
            s.description = std::string("Atmosphere Quality (volumetric fog and clouds: off, low, medium, high) of ") + groups[group] + ".";
            s.get = [group](SettingsContext& ctx) { return Json(enumName(atmosphereQualities, ctx.project()->getAtmosphereQuality()[group])); };
            s.set = [group](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                AtmosphereQuality quality;
                if (!parseEnum(atmosphereQualities, value, quality, error)) return false;
                if (apply) {
                    std::array<AtmosphereQuality, 3> all = ctx.project()->getAtmosphereQuality();
                    all[group] = quality;
                    ctx.project()->setAtmosphereQuality(all);
                }
                return true;
            };
            add(s);
        }

        // ---- player logging
        {
            Setting s;
            s.key = "player.logging.level";
            s.type = "enum";
            s.description = "The lowest level the built game logs.";
            s.values = {"trace", "debug", "info", "warn", "error", "off"};
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getPlayerLogging().level); };
            s.set = [values = s.values](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (std::find(values.begin(), values.end(), text) == values.end()) {
                    error = "\"" + text + "\" is not one of trace, debug, info, warn, error, off";
                    return false;
                }
                if (apply) {
                    PlayerLoggingSettings logging = ctx.project()->getPlayerLogging();
                    logging.level = text;
                    ctx.project()->setPlayerLogging(logging);
                }
                return true;
            };
            add(std::move(s));
        }
        add(boolSetting("player.logging.file", "Desktop builds also write <userdata>/logs/player.log.",
                        [](Project& p) { return p.getPlayerLogging().fileLog; },
                        [](Project& p, bool v) { PlayerLoggingSettings l = p.getPlayerLogging(); l.fileLog = v; p.setPlayerLogging(l); }));
        add(boolSetting("player.logging.backtrace", "Errors in the built game record their call stack.",
                        [](Project& p) { return p.getPlayerLogging().backtrace; },
                        [](Project& p, bool v) { PlayerLoggingSettings l = p.getPlayerLogging(); l.backtrace = v; p.setPlayerLogging(l); }));

        // ---- physics
        add(boolSetting("physics.enabled", "3D physics (Jolt). Off leaves Rigidbodies and joints out of builds.",
                        [](Project& p) { return p.isPhysics3DEnabled(); }, [](Project& p, bool v) { p.setPhysics3DEnabled(v); }));
        add(layerCollisionSetting("physics.layer_collision_matrix", true));
        add(boolSetting("physics2d.enabled", "2D physics (Box2D). Off leaves Rigidbody2Ds and joints out of builds.",
                        [](Project& p) { return p.isPhysics2DEnabled(); }, [](Project& p, bool v) { p.setPhysics2DEnabled(v); }));
        add(layerCollisionSetting("physics2d.layer_collision_matrix", false));

        // ---- input (S22)
        add(pathSetting("input.actions", "The Input Actions asset Play and the game load at start (an .inputactions file in the assets directory).",
                        [](Project& p) -> fs::path& { return p.getInputActionsAsset(); }));
        {
            Setting s;
            s.key = "input.deadzone";
            s.type = "number";
            s.description = "Default deadzone of AXIS and VECTOR2 actions (0..0.95).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getInputDeadzone()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 0.95, v, error)) return false;
                if (apply) ctx.project()->setInputDeadzone(static_cast<float>(v));
                return true;
            };
            add(std::move(s));
        }

        // ---- scripting
        {
            Setting s;
            s.key = "scripting.cxx_standard";
            s.type = "enum";
            for (int v : cxxStandards) s.values.push_back(std::to_string(v));
            s.description = "C++ standard for scripts and builds (" + cxxStandardList() + "). The next Play/build uses it; never edit CMAKE_CXX_STANDARD in the generated CMakeLists.txt.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getCxxStandard()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v = 0;
                if (value.is_string()) {
                    std::string text = value.get<std::string>();
                    if (text.rfind("c++", 0) == 0 || text.rfind("C++", 0) == 0) text = text.substr(3);
                    try { v = std::stoll(text); } catch (...) { v = 0; }
                } else if (!readInt(value, 0, 100, v, error)) {
                    return false;
                }
                if (!isSupportedCxxStandard(static_cast<int>(v))) {
                    error = "must be " + cxxStandardList();
                    return false;
                }
                if (apply) ctx.project()->setCxxStandard(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "scripting.script_dirs";
            s.type = "list";
            s.description = "C++ script roots: include directories whose sources all compile. Pass the complete list; entries read "
                            "here can be passed back verbatim; a NEW entry must be an existing project-relative directory.";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                for (const fs::path& dir : ctx.project()->getScriptDirs()) list.push_back(FileUtils::pathToGenericUtf8(dir));
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                Project* p = ctx.project();
                if (!value.is_array()) {
                    error = "must be a list of project-relative directories";
                    return false;
                }
                const std::vector<fs::path>& configured = p->getScriptDirs();
                std::vector<fs::path> dirs;
                for (const Json& item : value) {
                    if (!item.is_string()) {
                        error = "entries must be strings";
                        return false;
                    }
                    const fs::path dir = FileUtils::pathFromUtf8(item.get<std::string>()).lexically_normal();
                    const bool known = std::any_of(configured.begin(), configured.end(),
                                                   [&dir](const fs::path& c) { return c.lexically_normal() == dir; });
                    if (!known) {
                        fs::path absolute;
                        if (!mcp::McpRefs::resolveProjectPath(p, item.get<std::string>(), absolute, error, true)) return false;
                        std::error_code ec;
                        if (!fs::is_directory(absolute, ec)) {
                            error = item.get<std::string>() + " is not a directory";
                            return false;
                        }
                    }
                    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
                }
                if (apply) p->setScriptDirs(dirs);
                return true;
            };
            add(s);
        }
        for (const bool lua : {true, false}) {
            Setting s;
            s.key = lua ? "scripting.lua_dir" : "editor.assets_dir";
            s.type = "string";
            s.description = lua
                ? "Root of Lua script references (lua://), project-relative directory (\".\" = project root). Changing it MOVES referenced files and rewrites references."
                : "Root of asset references (textures, models, sounds, fonts), project-relative directory (\".\" = project root). Changing it MOVES referenced files and rewrites references.";
            s.get = [lua](SettingsContext& ctx) {
                return Json(FileUtils::pathToGenericUtf8(lua ? ctx.project()->getLuaDir() : ctx.project()->getAssetsDir()));
            };
            s.set = [lua](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                Project* p = ctx.project();
                std::string text;
                if (!readString(value, text, error)) return false;
                fs::path dir = text.empty() ? fs::path(".") : FileUtils::pathFromUtf8(text).lexically_normal();
                if (dir != ".") {
                    fs::path absolute;
                    if (!mcp::McpRefs::resolveProjectPath(p, text, absolute, error)) return false;
                }
                if (ctx.isPlaying()) {
                    error = "cannot change asset roots during Play";
                    return false;
                }
                if (apply) {
                    std::error_code ec;
                    fs::create_directories(p->getProjectPath() / dir, ec);
                    if (lua) p->changeAssetRoots(p->getAssetsDir(), dir);
                    else p->changeAssetRoots(dir, p->getLuaDir());
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("editor.version_control_files", "Write version-control metadata files (.gitignore etc.) with the project.",
                        [](Project& p) { return p.hasVersionControlMetadata(); }, [](Project& p, bool v) { p.setVersionControlMetadata(v); }));

        // ---- tags and layers
        {
            Setting s;
            s.key = "tags_and_layers.tags";
            s.type = "list";
            s.description = "User tags (after the built-in Untagged, Respawn, Finish, EditorOnly, MainCamera, Player, GameController). Pass the complete list.";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                for (const std::string& tag : ctx.project()->getTags()) list.push_back(tag);
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of tag names";
                    return false;
                }
                std::vector<std::string> tags;
                for (const Json& item : value) {
                    if (!item.is_string()) {
                        error = "tags must be strings";
                        return false;
                    }
                    const std::string tag = TagManager::trimName(item.get<std::string>());
                    if (tag.empty()) {
                        error = "tags must not be empty";
                        return false;
                    }
                    if (TagManager::isBuiltinTag(tag)) {
                        error = "\"" + tag + "\" is a built-in tag";
                        return false;
                    }
                    if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
                        error = "\"" + tag + "\" is listed twice";
                        return false;
                    }
                    tags.push_back(tag);
                }
                if (apply) ctx.project()->setTagsAndLayers(tags, ctx.project()->getLayerNames());
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "tags_and_layers.layers";
            s.type = "object";
            s.description = "User layer names by index, {\"3\": \"Player\", \"8\": \"Enemies\"} (user layers are 3 and 6-31; "
                            "0 Default, 1 TransparentFX, 2 Ignore Raycast, 4 Water, 5 UI are built-in). Setting merges: a "
                            "listed index gets that name, \"\" unnames it, unlisted ones keep theirs. Reading returns every named layer, built-ins included.";
            s.get = [](SettingsContext& ctx) {
                Json map = Json::object();
                const std::vector<std::string>& names = ctx.project()->getLayerNames();
                for (int i = 0; i < MAX_LAYERS; i++) {
                    std::string name = TagManager::isBuiltinLayer(i) ? TagManager::getBuiltinLayerName(i)
                                                                     : (i < static_cast<int>(names.size()) ? names[i] : std::string());
                    if (!name.empty()) map[std::to_string(i)] = name;
                }
                return map;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_object()) {
                    error = "must be an object {\"index\": \"name\"}";
                    return false;
                }
                std::vector<std::string> names = ctx.project()->getLayerNames();
                names.resize(MAX_LAYERS);
                for (auto it = value.begin(); it != value.end(); ++it) {
                    int index = -1;
                    try { index = std::stoi(it.key()); } catch (...) { index = -1; }
                    if (index < 0 || index >= MAX_LAYERS || std::to_string(index) != it.key()) {
                        error = "layer key \"" + it.key() + "\" must be an index 0-31";
                        return false;
                    }
                    if (TagManager::isBuiltinLayer(index)) {
                        error = "layer " + it.key() + " is built-in (" + TagManager::getBuiltinLayerName(index) + ")";
                        return false;
                    }
                    if (!it.value().is_string()) {
                        error = "layer names must be strings";
                        return false;
                    }
                    names[index] = TagManager::trimName(it.value().get<std::string>());
                }
                std::set<std::string> seen;
                for (int i = 0; i < MAX_LAYERS; i++) {
                    const std::string name = TagManager::isBuiltinLayer(i) ? TagManager::getBuiltinLayerName(i) : names[i];
                    if (name.empty()) continue;
                    if (!seen.insert(name).second) {
                        error = "two layers are named \"" + name + "\"";
                        return false;
                    }
                }
                if (apply) ctx.project()->setTagsAndLayers(ctx.project()->getTags(), names);
                return true;
            };
            add(s);
        }

        // ---- editor preferences (this machine / this editor session)
        {
            Setting s;
            s.key = "preferences.clear_on_play";
            s.type = "bool";
            s.preference = true;
            s.description = "Console: clear the log when Play starts (editor session).";
            s.get = [](SettingsContext& ctx) { return ctx.consoleWindow() ? Json(ctx.consoleWindow()->isClearOnPlay()) : Json(nullptr); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                if (!ctx.consoleWindow()) {
                    error = "no Console window";
                    return false;
                }
                if (apply) ctx.consoleWindow()->setClearOnPlay(b);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.error_pause";
            s.type = "bool";
            s.preference = true;
            s.description = "Console: pause Play when an error is logged (editor session). Usually off for agents.";
            s.get = [](SettingsContext& ctx) { return ctx.consoleWindow() ? Json(ctx.consoleWindow()->isErrorPause()) : Json(nullptr); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                if (!ctx.consoleWindow()) {
                    error = "no Console window";
                    return false;
                }
                if (apply) ctx.consoleWindow()->setErrorPause(b);
                return true;
            };
            add(s);
        }
        {
            Setting s = boolSetting("preferences.simulate_touch", "Play acts as a touch device: the mouse is one finger and isTouchDevice() is true.",
                                    [](Project& p) { return p.isSimulateTouch(); }, [](Project& p, bool v) { p.setSimulateTouch(v); });
            s.preference = true;
            add(s);
        }
        auto stringPreference = [&add](const std::string& key, const std::string& description,
                                       std::function<std::string()> get, std::function<void(const std::string&)> set) {
            Setting s;
            s.key = key;
            s.type = "string";
            s.preference = true;
            s.description = description;
            s.get = [get](SettingsContext&) { return Json(get()); };
            s.set = [set](SettingsContext&, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (apply) set(text);
                return true;
            };
            add(s);
        };
        // Preferences > Logging (saved to settings.yaml at once, like the dialog's OK)
        auto loggingPreference = [&add](Setting s, std::function<bool(LoggingSettings&, const Json&, std::string&)> apply,
                                        std::function<Json(const LoggingSettings&)> read) {
            s.preference = true;
            s.get = [read](SettingsContext&) { return read(AppSettings::getLoggingSettings()); };
            s.set = [apply](SettingsContext&, const Json& value, bool commit, std::string& error) {
                LoggingSettings settings = AppSettings::getLoggingSettings();
                if (!apply(settings, value, error)) return false;
                if (commit) {
                    AppSettings::setLoggingSettings(settings);
                    AppSettings::saveSettings();
                }
                return true;
            };
            add(std::move(s));
        };
        auto levelName = [](const Json& value, std::string& level, std::string& error) {
            LogLevel parsed;
            if (!value.is_string() || !parseLogLevel(value.get<std::string>().c_str(), parsed)) {
                error = "must be a level: trace, debug, info, warn, error, fatal or off";
                return false;
            }
            level = logLevelName(parsed);
            return true;
        };
        {
            Setting s;
            s.key = "preferences.logging.levels";
            s.type = "object";
            s.description = "Preferences > Logging: category levels, {\"render\": \"debug\"}. Setting merges (\"info\" resets a category, {} "
                            "resets all); categories not listed log at Info.";
            loggingPreference(s, [levelName](LoggingSettings& settings, const Json& value, std::string& error) {
                if (!value.is_object()) {
                    error = "must be an object {\"category\": \"level\"}";
                    return false;
                }
                if (value.empty()) {
                    settings.categoryLevels.clear();
                    return true;
                }
                for (const auto& [category, level] : value.items()) {
                    std::string name;
                    if (!levelName(level, name, error)) {
                        error = category + ": " + error;
                        return false;
                    }
                    if (name == "info") settings.categoryLevels.erase(category);
                    else settings.categoryLevels[category] = name;
                }
                return true;
            }, [](const LoggingSettings& settings) {
                Json map = Json::object();
                for (const auto& [category, level] : settings.categoryLevels) map[category] = level;
                return map;
            });
        }
        {
            Setting s;
            s.key = "preferences.logging.file_level";
            s.type = "enum";
            s.values = {"trace", "debug", "info", "warn", "error", "fatal", "off"};
            s.description = "Preferences > Logging: the lowest level written to editor.log.";
            loggingPreference(s, [levelName](LoggingSettings& settings, const Json& value, std::string& error) {
                return levelName(value, settings.fileLevel, error);
            }, [](const LoggingSettings& settings) { return Json(settings.fileLevel); });
        }
        {
            Setting s;
            s.key = "preferences.logging.backtrace_on_error";
            s.type = "bool";
            s.description = "Preferences > Logging: errors record their call stack (editor.log).";
            loggingPreference(s, [](LoggingSettings& settings, const Json& value, std::string& error) {
                return readBool(value, settings.backtraceOnError, error);
            }, [](const LoggingSettings& settings) { return Json(settings.backtraceOnError); });
        }
        {
            Setting s;
            s.key = "preferences.logging.show_source";
            s.type = "bool";
            s.description = "Preferences > Logging: file:line in the Console's details and editor.log.";
            loggingPreference(s, [](LoggingSettings& settings, const Json& value, std::string& error) {
                return readBool(value, settings.showSource, error);
            }, [](const LoggingSettings& settings) { return Json(settings.showSource); });
        }
        stringPreference("preferences.cmake_path", "External Tools: cmake executable (\"\" = from PATH).",
                         [] { return AppSettings::getCMakePath(); }, [](const std::string& v) { AppSettings::setCMakePath(v); });
        stringPreference("preferences.emsdk_path", "External Tools: Emscripten SDK root for Web builds (\"\" = EMSDK / PATH).",
                         [] { return AppSettings::getEmsdkPath(); }, [](const std::string& v) { AppSettings::setEmsdkPath(v); });
        stringPreference("preferences.ffmpeg_path", "External Tools: ffmpeg for video imports (\"\" = JASPER_FFMPEG / PATH).",
                         [] { return AppSettings::getFfmpegPath(); }, [](const std::string& v) { AppSettings::setFfmpegPath(v); });
        stringPreference("preferences.default_export_dir", "Default output directory for builds of projects that have none yet.",
                         [] { return FileUtils::pathToGenericUtf8(AppSettings::getDefaultExportDirectory()); },
                         [](const std::string& v) { AppSettings::setDefaultExportDirectory(FileUtils::pathFromUtf8(v)); });
        // Compiler kit and jobs, per project (.jasperengine/user/build.yaml)
        struct KitField {
            const char* key;
            const char* description;
            std::string LocalBuildSettings::* member;
        };
        static const KitField kitFields[] = {
            {"preferences.build_c_compiler", "C compiler for Desktop builds of this project (\"\" = CMake default).", &LocalBuildSettings::cCompiler},
            {"preferences.build_cxx_compiler", "C++ compiler for Desktop builds of this project (\"\" = CMake default).", &LocalBuildSettings::cxxCompiler},
            {"preferences.build_generator", "CMake generator for Desktop builds of this project, e.g. \"Ninja\" or \"Unix Makefiles\" (\"\" = CMake default).", &LocalBuildSettings::generator},
        };
        for (const KitField& field : kitFields) {
            Setting s;
            s.key = field.key;
            s.type = "string";
            s.preference = true;
            s.description = field.description;
            auto member = field.member;
            s.get = [member](SettingsContext& ctx) {
                return Json(AppSettings::getBuildSettings(ctx.project()->getProjectPath() / "project.yaml").*member);
            };
            s.set = [member](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (apply) {
                    const fs::path file = ctx.project()->getProjectPath() / "project.yaml";
                    LocalBuildSettings build = AppSettings::getBuildSettings(file);
                    build.*member = text;
                    if (!AppSettings::setBuildSettings(file, build)) {
                        error = "could not write the local build settings";
                        return false;
                    }
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.build_jobs";
            s.type = "int";
            s.preference = true;
            s.description = "Parallel compile jobs for Desktop builds of this project (0 = automatic).";
            s.get = [](SettingsContext& ctx) {
                return Json(AppSettings::getBuildSettings(ctx.project()->getProjectPath() / "project.yaml").buildJobs);
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 0, 1024, v, error)) return false;
                if (apply) {
                    const fs::path file = ctx.project()->getProjectPath() / "project.yaml";
                    LocalBuildSettings build = AppSettings::getBuildSettings(file);
                    build.buildJobs = static_cast<unsigned>(v);
                    if (!AppSettings::setBuildSettings(file, build)) {
                        error = "could not write the local build settings";
                        return false;
                    }
                }
                return true;
            };
            add(s);
        }
        return t;
    }();
}

// Where each MCP key sits in the windows: page, section, label (as drawn), search keywords
struct Placement {
    const char* key;
    const char* page;
    const char* section;
    const char* label;
    const char* keywords;
};

const Placement placements[] = {
    {"player.product_name", "Player", "", "Project Name", "name title"},
    {"player.application_name", "Player", "", "Product Name", "application display name"},
    {"player.identifier", "Player", "", "Application Identifier", "bundle package reverse domain"},
    {"player.version", "Player", "", "Version", ""},
    {"player.build", "Player", "", "Build", "build number version code"},
    {"player.window_icon", "Player", "Icon", "Default Icon", "icon window taskbar"},
    {"player.window_mode", "Player", "Resolution and Presentation", "Fullscreen Mode", "window mode windowed maximized"},
    {"player.window_width", "Player", "Resolution and Presentation", "Default Screen Width", "window resolution"},
    {"player.window_height", "Player", "Resolution and Presentation", "Default Screen Height", "window resolution"},
    {"player.window_resizable", "Player", "Resolution and Presentation", "Resizable Window", ""},
    {"player.window_title", "Player", "Resolution and Presentation", "Window Title", ""},
    {"player.loading_scene", "Player", "Splash and Loading", "Loading Scene", "splash"},
    {"player.loading_delay", "Player", "Splash and Loading", "Loading Delay", "splash"},
    {"player.loading_timeout", "Player", "Splash and Loading", "Loading Timeout", "splash"},
    {"player.async_loading", "Player", "Splash and Loading", "Async Loading", "asynchronous"},
    {"player.logging.level", "Player", "Logging", "Log Level", "player log"},
    {"player.logging.file", "Player", "Logging", "Log File", "player log"},
    {"player.logging.backtrace", "Player", "Logging", "Capture Backtrace on Errors", "stack trace"},
    {"player.threading.workers", "Player", "Threading", "Worker Threads", "jobs threads"},
    {"player.threading.reserved_frame_workers", "Player", "Threading", "Reserved Frame Workers", "jobs threads"},
    {"player.threading.io_threads", "Player", "Threading", "Asset I/O Threads", "io loading threads"},
    {"player.threading.web_pump_budget_ms", "Player", "Threading", "Web Pump Budget", "web jobs"},
    {"player.threading.web_multithreaded", "Player", "Threading", "Web: Multithreaded", "pthreads cross origin isolated"},
    {"player.development_build", "Player", "Publishing Settings", "Development Build", "profiler"},
    {"player.pack_native_resources", "Player", "Publishing Settings", "Native Resource Pack", "resources pak"},
    {"web.application_name", "Player", "Web", "App Name", "web page title"},
    {"web.favicon", "Player", "Web", "Favicon", "web icon"},
    {"web.custom_html_shell", "Player", "Web", "Custom HTML Shell", "web emscripten template"},
    {"web.head_include", "Player", "Web", "Head Include", "web html head"},
    {"web.resize_canvas_to_window", "Player", "Web", "Resize Canvas To Window", "web"},
    {"web.hide_emscripten_ui", "Player", "Web", "Hide Emscripten UI", "web"},
    {"linux.application_name", "Player", "Linux", "App Name", "linux desktop"},
    {"linux.comment", "Player", "Linux", "Comment", "linux desktop"},
    {"linux.categories", "Player", "Linux", "Categories", "linux desktop"},
    {"windows.product_name", "Player", "Windows", "Product Name", "windows"},
    {"windows.company_name", "Player", "Windows", "Company Name", "windows"},
    {"windows.file_version", "Player", "Windows", "File Version", "windows"},
    {"windows.product_version", "Player", "Windows", "Product Version", "windows"},
    {"macos.application_name", "Player", "macOS", "App Name", "macos mac"},
    {"macos.bundle_identifier", "Player", "macOS", "Bundle Identifier", "macos mac"},
    {"macos.version_name", "Player", "macOS", "Version Name", "macos mac"},
    {"macos.build_number", "Player", "macOS", "Build Number", "macos mac"},
    {"macos.icon", "Player", "macOS", "Icon", "macos mac"},
    {"macos.high_dpi", "Player", "macOS", "High DPI", "macos mac retina"},
    {"ios.application_name", "Player", "iOS", "App Name", "ios iphone ipad"},
    {"ios.bundle_identifier", "Player", "iOS", "Bundle Identifier", "ios"},
    {"ios.version_name", "Player", "iOS", "Version Name", "ios"},
    {"ios.build_number", "Player", "iOS", "Build Number", "ios"},
    {"ios.icon", "Player", "iOS", "Icon", "ios"},
    {"ios.hide_status_bar", "Player", "iOS", "Hide Status Bar", "ios"},
    {"ios.hide_home_indicator", "Player", "iOS", "Hide Home Indicator", "ios"},
    {"ios.supports_high_refresh_rate", "Player", "iOS", "High Refresh Rate", "ios promotion 120 hz"},
    {"ios.admob_enabled", "Player", "iOS", "Google AdMob", "ios ads advertising mobile ads sdk"},
    {"ios.admob_app_id", "Player", "iOS", "AdMob App ID", "ios ads advertising gadapplicationidentifier"},
    {"android.application_name", "Player", "Android", "App Name", "android label"},
    {"android.package_name", "Player", "Android", "Package Name", "android"},
    {"android.version_code", "Player", "Android", "Version Code", "android"},
    {"android.version_name", "Player", "Android", "Version Name", "android"},
    {"android.launcher_icon", "Player", "Android", "Launcher Icon", "android icon launcher icons"},
    {"android.adaptive_icon_foreground", "Player", "Android", "Adaptive Foreground", "android launcher icons adaptive icon"},
    {"android.adaptive_icon_background", "Player", "Android", "Adaptive Background", "android launcher icons adaptive icon"},
    {"android.min_sdk", "Player", "Android", "Min SDK", "android api level"},
    {"android.target_sdk", "Player", "Android", "Target SDK", "android api level"},
    {"android.orientation", "Player", "Android", "Orientation", "android portrait landscape"},
    {"android.abis", "Player", "Android", "Architectures", "android abi arm64 x86"},
    {"android.permissions", "Player", "Android", "Permissions", "android"},
    {"android.allow_backup", "Player", "Android", "Application Options", "android allow backup"},
    {"android.keep_screen_on", "Player", "Android", "Keep Screen On", "android application options"},
    {"android.fullscreen", "Player", "Android", "Screen Options", "android fullscreen"},
    {"android.admob_enabled", "Player", "Android", "Google AdMob", "android ads advertising mobile ads sdk"},
    {"android.admob_app_id", "Player", "Android", "AdMob App ID", "android ads advertising application id"},
    {"graphics.canvas_width", "Graphics", "", "Canvas Width", "reference resolution"},
    {"graphics.canvas_height", "Graphics", "", "Canvas Height", "reference resolution"},
    {"graphics.scaling_mode", "Graphics", "", "Scaling Mode", "letterbox fit crop stretch"},
    {"graphics.texture_strategy", "Graphics", "", "Texture Strategy", "power of two"},
    {"graphics.mobile_pbr_tier", "Graphics", "", "Mobile PBR Tier", "clear coat iridescence anisotropy lit android ios web"},
    {"graphics.html_ui_msaa_samples", "Graphics", "", "HTML UI MSAA", "anti-aliasing antialiasing multisampling"},
    {"graphics.shadow_tier.windows", "Graphics", "Shadows", "Windows Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.linux", "Graphics", "Shadows", "Linux Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.macos", "Graphics", "Shadows", "macOS Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.web", "Graphics", "Shadows", "Web Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.android", "Graphics", "Shadows", "Android Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.ios", "Graphics", "Shadows", "iOS Shadow Tier", "shadow quality"},
    {"graphics.runtime_shaders", "Graphics", "", "Runtime Shader Compilation", "variants"},
    {"quality.vsync", "Quality", "", "VSync", "vertical sync refresh"},
    {"quality.atmosphere_quality_desktop", "Quality", "", "Atmosphere Quality (Desktop)", "volumetric fog clouds"},
    {"quality.atmosphere_quality_mobile", "Quality", "", "Atmosphere Quality (Mobile)", "volumetric fog clouds"},
    {"quality.atmosphere_quality_web", "Quality", "", "Atmosphere Quality (Web)", "volumetric fog clouds"},
    {"physics.enabled", "Physics", "", "3D Physics", "jolt rigidbody"},
    {"physics.layer_collision_matrix", "Physics", "Layer Collision Matrix", "Layer Collision Matrix", "layers collide"},
    {"physics2d.enabled", "Physics 2D", "", "2D Physics", "box2d rigidbody 2d"},
    {"physics2d.layer_collision_matrix", "Physics 2D", "Layer Collision Matrix", "Layer Collision Matrix", "layers collide"},
    {"input.actions", "Input", "", "Default Actions Asset", "input actions bindings controls keyboard gamepad touch inputactions"},
    {"input.deadzone", "Input", "", "Default Deadzone", "input stick axis analog gamepad dead zone"},
    {"scripting.lua_dir", "Scripting", "", "Lua Directory", "lua"},
    {"scripting.script_dirs", "Scripting", "", "Script Directories", "c++ cpp"},
    {"scripting.cxx_standard", "Scripting", "", "C++ Standard", "cpp c++17 c++20 c++23"},
    {"editor.version_control_files", "Editor", "", "Version Control Files", "git gitignore gitattributes vcs"},
    {"editor.assets_dir", "Editor", "", "Assets Directory", "assets root"},
    {"tags_and_layers.tags", "Tags and Layers", "Tags", "Tags", "tag"},
    {"tags_and_layers.layers", "Tags and Layers", "Layers", "Layers", "layer"},
    {"preferences.clear_on_play", "General", "Console", "Clear on Play", "console"},
    {"preferences.error_pause", "General", "Console", "Error Pause", "console"},
    {"preferences.simulate_touch", "General", "Game View", "Simulate Touch", "game view mouse finger"},
    {"preferences.default_export_dir", "General", "", "Default Build Directory", "output export"},
    {"preferences.cmake_path", "External Tools", "Desktop Build", "CMake", "cmake executable"},
    {"preferences.build_c_compiler", "External Tools", "Desktop Build", "Compiler", "compiler kit c"},
    {"preferences.build_cxx_compiler", "External Tools", "Desktop Build", "C++ Compiler", "compiler kit cpp"},
    {"preferences.build_generator", "External Tools", "Desktop Build", "Generator", "cmake ninja makefiles"},
    {"preferences.build_jobs", "External Tools", "Desktop Build", "Build Jobs", "parallel jobs"},
    {"preferences.emsdk_path", "External Tools", "Web Build", "Emscripten SDK", "emsdk web"},
    {"preferences.ffmpeg_path", "External Tools", "Video Import", "FFmpeg", "video av1"},
    {"preferences.logging.file_level", "Logging", "", "Log File Level", "editor log"},
    {"preferences.logging.backtrace_on_error", "Logging", "", "Capture Backtrace on Errors", "stack trace"},
    {"preferences.logging.show_source", "Logging", "", "Show Source Location", "file line"},
    {"preferences.logging.levels", "Logging", "Category Levels", "Category Levels", "category level"},
};

const Placement* placementOf(const std::string& key) {
    for (const Placement& placement : placements) {
        if (key == placement.key) return &placement;
    }
    return nullptr;
}

std::string titleCase(const std::string& key) {
    std::string leaf = key.substr(key.rfind('.') + 1);
    std::string out;
    bool upper = true;
    for (char c : leaf) {
        if (c == '_') {
            out.push_back(' ');
            upper = true;
            continue;
        }
        out.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        upper = false;
    }
    return out;
}

// A row the windows draw that MCP does not set: its value for the palette and search
SettingRow uiRow(const char* key, const char* page, const char* section, const char* label, const char* keywords,
                 std::function<Json(SettingsContext&)> get, const char* tooltip = "") {
    SettingRow row;
    row.key = key;
    row.page = page;
    row.section = section;
    row.label = label;
    row.keywords = keywords;
    row.tooltip = tooltip;
    row.get = std::move(get);
    return row;
}

void addOrAbort(SettingsRegistry& registry, SettingRow row) {
    std::string why;
    if (!registry.addRow(std::move(row), &why)) {
        Out::error("Settings registry: %s", why.c_str());
    }
}

} // namespace

void registerBuiltinSettings(SettingsRegistry& registry) {
    // the pages, in the windows' order (the category arrays the windows had before S25)
    const SettingsPage pages[] = {
        {"Player", SettingsScope::Project, 0, "project product application icon resolution splash loading publishing platform"},
        {"Graphics", SettingsScope::Project, 1, "canvas scaling texture shaders shadows"},
        {"Quality", SettingsScope::Project, 2, "atmosphere fog clouds"},   // VSync: its row (a page match shows every row)
        {"Physics", SettingsScope::Project, 3, "3d jolt"},
        {"Physics 2D", SettingsScope::Project, 4, "2d box2d"},
        {"Input", SettingsScope::Project, 5, "input actions bindings controls gamepad deadzone"},
        {"Scripting", SettingsScope::Project, 6, "lua c++"},
        {"Tags and Layers", SettingsScope::Project, 7, "tags layers"},
        {"Editor", SettingsScope::Project, 8, "version control assets labels"},
        {"General", SettingsScope::Preference, 0, "editor auto save backup html ui"},
        {"Code Editor", SettingsScope::Preference, 1, "font"},
        {"Asset Pipeline", SettingsScope::Preference, 2, "refresh watcher hot reload"},
        {"Scripting", SettingsScope::Preference, 3, "hot reload during play external script editor"},
        {"Undo", SettingsScope::Preference, 4, "undo redo history"},
        {"Lattice", SettingsScope::Preference, 5, "lattice cage deformer"},
        {"External Tools", SettingsScope::Preference, 6, "cmake compiler emscripten ffmpeg"},
        {"Logging", SettingsScope::Preference, 7, "log levels"},
        {"Shader Cache", SettingsScope::Preference, 8, "shaders cache clear"},
    };
    for (const SettingsPage& page : pages) {
        // two scopes may share a title (Scripting): names stay unique
        SettingsPage copy = page;
        if (page.scope == SettingsScope::Preference && registry.findPage(page.name)) {
            copy.title = page.name;
            copy.name = page.name + " (Preferences)";
        }
        registry.addPage(copy);
    }

    for (Setting& setting : mcpSettingsTable()) {
        SettingRow row;
        row.key = setting.key;
        row.type = setting.type;
        row.description = setting.description;
        row.values = setting.values;
        row.get = setting.get;
        row.set = setting.set;
        row.mcp = true;
        row.tooltip = setting.description;
        if (const Placement* placement = placementOf(setting.key)) {
            row.page = placement->page;
            row.section = placement->section;
            row.label = placement->label;
            row.keywords = placement->keywords;
        } else {
            row.page = setting.preference ? "General" : "Editor";
            row.label = titleCase(setting.key);
        }
        if (setting.preference && row.page == "Scripting") row.page = "Scripting (Preferences)";
        addOrAbort(registry, std::move(row));
    }

    // ---- Project Settings rows MCP does not set
    addOrAbort(registry, uiRow("player.start_scene", "Player", "Splash and Loading", "Start Scene", "first scene build",
        [](SettingsContext& ctx) {
            const SceneProject* scene = ctx.project()->getScene(ctx.project()->getStartSceneId());
            return scene ? Json(scene->name) : Json(nullptr);
        }, "The scene a build starts with (edit_build_scenes sets it)."));
    addOrAbort(registry, uiRow("editor.asset_labels", "Editor", "Asset Labels", "Asset Labels", "label color colour rename delete tags",
        [](SettingsContext& ctx) {
            Json labels = Json::array();
            for (const AssetLabels::LabelInfo& info : ctx.project()->getAssetLabels().labels()) {
                labels.push_back(Json{{"name", info.name}, {"color", info.color}, {"assets", info.assets}});
            }
            return labels;
        }, "The project's asset labels: colour, rename and delete (edit_asset_labels)."));

    addOrAbort(registry, uiRow("input.rebinding_overrides", "Input", "", "Rebinding Overrides", "rebind reset overrides saved controls",
        [](SettingsContext&) {
            return InputActions::isLoaded() ? Json(InputActions::getOverridesKey()) : Json(nullptr);
        }, "Players' rebinding is kept per asset in the user settings; Reset clears the editor's copy, which Play uses."));

    // ---- Preferences rows MCP does not set
    const char* general = "General";
    addOrAbort(registry, uiRow("preferences.editor_vsync", general, "", "Editor VSync", "vertical sync refresh",
        [](SettingsContext&) { return Json(AppSettings::getEditorVSyncEnabled()); }));
    addOrAbort(registry, uiRow("preferences.ui_scale", general, "", "UI Scale", "zoom size text panels dpi",
        [](SettingsContext&) { return Json(AppSettings::getUiScale()); }));
    addOrAbort(registry, uiRow("preferences.auto_save", general, "Auto Save", "Auto Save Scenes", "autosave backup",
        [](SettingsContext&) { return Json(AppSettings::isAutosaveEnabled()); }));
    addOrAbort(registry, uiRow("preferences.auto_save_interval", general, "Auto Save", "Auto Save Interval (s)", "autosave backup seconds",
        [](SettingsContext&) { return Json(AppSettings::getAutosaveIntervalSeconds()); }));
    addOrAbort(registry, uiRow("preferences.backups", general, "Auto Save", "Backups", "backup folder crash recovery",
        [](SettingsContext& ctx) { return Json(SceneBackups::root(ctx.project()->getProjectPath()).generic_string()); }));
    addOrAbort(registry, uiRow("preferences.html_ui_import_remote", general, "HTML UI", "Import Remote Resources", "google fonts download",
        [](SettingsContext&) { return Json(AppSettings::getHtmlUiAutoImport()); }));
    addOrAbort(registry, uiRow("preferences.html_ui_verbose_diagnostics", general, "HTML UI", "Verbose Diagnostics", "css compatibility notes",
        [](SettingsContext&) { return Json(AppSettings::getHtmlUiVerboseDiagnostics()); }));
    addOrAbort(registry, uiRow("preferences.html_ui_hot_reload", general, "HTML UI", "Hot Reload", "patch rebuild reconcile html reload",
        [](SettingsContext&) { return Json(AppSettings::getHtmlUiPatchReload() ? "patch" : "rebuild"); }));
    addOrAbort(registry, uiRow("preferences.code_editor_font_size", "Code Editor", "", "Font Size", "text zoom",
        [](SettingsContext&) { return Json(AppSettings::getCodeEditorFontSize()); }));
    const char* pipeline = "Asset Pipeline";
    addOrAbort(registry, uiRow("preferences.auto_refresh", pipeline, "", "Auto Refresh", "hot reload file changes",
        [](SettingsContext&) { return Json(AppSettings::getAutoRefresh()); }));
    addOrAbort(registry, uiRow("preferences.file_watcher", pipeline, "", "Watcher", "native polling file notifications",
        [](SettingsContext&) { return Json(AppSettings::getFileWatcher()); }));
    addOrAbort(registry, uiRow("preferences.polling_interval", pipeline, "", "Polling Interval", "watcher seconds",
        [](SettingsContext&) { return Json(AppSettings::getPollingInterval()); }));
    addOrAbort(registry, uiRow("preferences.log_reloads", pipeline, "", "Log Reloads", "hot reload console",
        [](SettingsContext&) { return Json(AppSettings::getLogReloads()); }));
    const char* scripting = "Scripting (Preferences)";
    addOrAbort(registry, uiRow("preferences.script_changes_while_playing", scripting, "", "Script Changes While Playing", "lua hot reload play",
        [](SettingsContext&) { return Json(AppSettings::getScriptChangesWhilePlaying()); }));
    addOrAbort(registry, uiRow("preferences.reload_cpp_during_play", scripting, "", "Reload C++ Scripts During Play", "cpp hot reload",
        [](SettingsContext&) { return Json(AppSettings::getReloadCppDuringPlay()); }));
    addOrAbort(registry, uiRow("preferences.external_script_editor", "External Tools", "Scripts", "External Script Editor", "ide vscode visual studio rider clion xcode sublime zed",
        [](SettingsContext&) { return Json(AppSettings::getExternalScriptEditor()); }));
    addOrAbort(registry, uiRow("preferences.external_script_editor_command", "External Tools", "Scripts", "Command", "external script editor custom",
        [](SettingsContext&) { return Json(AppSettings::getExternalScriptEditorCommand()); }));
    const char* undo = "Undo";
    addOrAbort(registry, uiRow("preferences.undo_max_steps", undo, "", "Max Undo Steps", "history",
        [](SettingsContext&) { return Json(AppSettings::getUndoMaxSteps()); }));
    addOrAbort(registry, uiRow("preferences.undo_memory_limit", undo, "", "Undo Memory Limit (MB)", "history memory",
        [](SettingsContext&) { return Json(AppSettings::getUndoMemoryLimitMB()); }));
    addOrAbort(registry, uiRow("preferences.undo_record_selection", undo, "", "Record Selection in Undo", "selection history",
        [](SettingsContext&) { return Json(AppSettings::getUndoRecordSelection()); }));
    addOrAbort(registry, uiRow("preferences.undo_review_play_changes", undo, "", "Review Play Mode Changes", "keep play mode changes",
        [](SettingsContext&) { return Json(AppSettings::getUndoReviewPlayChanges()); }));
    const char* lattice = "Lattice";
    auto latticeRow = [&](const char* key, const char* section, const char* label, const char* keywords, auto value) {
        addOrAbort(registry, uiRow(key, lattice, section, label, keywords,
            [value](SettingsContext&) { return Json(value(AppSettings::getLatticeViewSettings())); }));
    };
    latticeRow("preferences.lattice.line_color", "Cage", "Line Colour", "color edges",
               [](const LatticeViewSettings& v) { return v.lineColor; });
    latticeRow("preferences.lattice.line_thickness", "Cage", "Line Thickness", "width edges",
               [](const LatticeViewSettings& v) { return v.lineThickness; });
    latticeRow("preferences.lattice.behind_opacity", "Cage", "Behind Geometry", "opacity hidden occluded",
               [](const LatticeViewSettings& v) { return v.behindOpacity; });
    latticeRow("preferences.lattice.point_size", "Points", "Point Size", "squares",
               [](const LatticeViewSettings& v) { return v.pointSize; });
    latticeRow("preferences.lattice.point_color", "Points", "Point Colour", "color",
               [](const LatticeViewSettings& v) { return v.pointColor; });
    latticeRow("preferences.lattice.selected_point_color", "Points", "Selected Point Colour", "color selection",
               [](const LatticeViewSettings& v) { return v.selectedPointColor; });
    latticeRow("preferences.lattice.stretch_coloring", "Stretch", "Stretch Colouring", "coloring gradient length",
               [](const LatticeViewSettings& v) { return v.stretchColoring; });
    latticeRow("preferences.lattice.squash_color", "Stretch", "Squash Colour", "color gradient",
               [](const LatticeViewSettings& v) { return v.squashColor; });
    latticeRow("preferences.lattice.stretch_color", "Stretch", "Stretch Colour", "color gradient",
               [](const LatticeViewSettings& v) { return v.stretchColor; });
    latticeRow("preferences.lattice.falloff_curve", "Proportional Falloff", "Falloff Curve", "smooth linear sharp constant",
               [](const LatticeViewSettings& v) { return v.falloffCurve; });
    latticeRow("preferences.lattice.falloff_radius", "Proportional Falloff", "Default Radius", "falloff size",
               [](const LatticeViewSettings& v) { return v.falloffRadius; });
    addOrAbort(registry, uiRow("preferences.logging.reset", "Logging", "Category Levels", "Reset", "category levels info",
        [](SettingsContext&) { return Json(AppSettings::getLoggingSettings().categoryLevels.size()); }));
    addOrAbort(registry, uiRow("preferences.shader_cache", "Shader Cache", "", "Shader Cache", "clear shaders",
        [](SettingsContext&) { return Json(App::getUserShaderCacheDir().generic_string()); }));
}

} // namespace jasperengine::editor
