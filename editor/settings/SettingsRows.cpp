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
#include "diag/CrashHandler.h"
#include "Project.h"
#include "util/PlatformRules.h"
#include "command/CommandHistory.h"
#include "command/type/ShaderCreationSettingsCmd.h"
#include "command/type/ShaderGlobalsCmd.h"
#include "command/type/InputActionsCmd.h"
#include "command/type/DebugSettingsCmd.h"
#include "command/type/DrawSubmissionSettingsCmd.h"
#include "command/type/TransitionSettingsCmd.h"
#include "pixelart/PaletteRampTexture.h"
#include "pixelart/PixelColor.h"
#include "command/type/DeviceSettingsCmd.h"
#include "command/type/ApplePrivacySettingsCmd.h"
#include "command/UndoOrigin.h"
#include "DebugSettings.h"
#include "Generator.h"
#include "audio/AudioMixer.h"
#include "util/ShaderGlobalsJson.h"
#include "reflect/Reflect.h"
#include "input/InputActions.h"
#include "players/PlayerRuntimes.h"
#include "texture/TextureCookQueue.h"
#include "model/ModelCookQueue.h"
#include "texture/TextureImportSettings.h"
#include "model/ModelImportSettings.h"
#include "log/Logger.h"
#include "mcp/McpRefs.h"
#include "mcp/McpValues.h"
#include "mcp/tools/W4Shared.h"
#include "registry/TagManager.h"
#include "util/CxxStandards.h"
#include "util/FileUtils.h"
#include "util/NativePlugins.h"
#include "util/SystemFonts.h"
#include "window/ConsoleWindow.h"
#include "vcs/VcsService.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace jasperengine::editor {

namespace {

namespace fs = std::filesystem;
using mcp::Json;

// An asset-root change for set_project_settings (project-relative paths)
mcp::Json assetRootChangeJson(Project* p, const AssetRootChange& change) {
    auto rel = [p](const fs::path& path) { return FileUtils::pathToGenericUtf8(p->normalizeToProjectRelative(path)); };
    auto list = [&](const std::vector<fs::path>& paths) {
        mcp::Json out = mcp::Json::array();
        for (const fs::path& path : paths) out.push_back(rel(path));
        return out;
    };
    mcp::Json moved = mcp::Json::array();
    for (const AssetRootChange::Move& move : change.moves) moved.push_back({{"from", rel(move.from)}, {"to", rel(move.to)}});
    mcp::Json out = {{"moved", moved}, {"already_there", list(change.alreadyThere)}, {"kept_outside", list(change.keptOutside)},
                     {"rewritten", list(change.rewritten)}, {"saved_scenes_with_edits", list(change.savedScenesWithEdits)},
                     {"removed_folders", list(change.removedFolders)}, {"failed_saves", list(change.failedSaves)},
                     {"text_references_broken", change.textReferencesBroken}};
    if (!change.refusal().empty()) out["refused"] = change.refusal();
    return out;
}

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
    // a program to run or a folder outside the project: only the user sets it (read-only over MCP)
    bool userOnly = false;
};

template <typename T>
struct EnumMap {
    const char* name;
    T value;
};

const EnumMap<Scaling> scalingModes[] = {
    {"fit_width", Scaling::FITWIDTH}, {"fit_height", Scaling::FITHEIGHT}, {"letterbox", Scaling::LETTERBOX},
    {"crop", Scaling::CROP}, {"stretch", Scaling::STRETCH}, {"native", Scaling::NATIVE},
    // S41: whole-number scales of the canvas
    {"integer", Scaling::INTEGER}, {"integer_expand", Scaling::INTEGER_EXPAND}};
const EnumMap<TextureStrategy> textureStrategies[] = {
    {"fit", TextureStrategy::FIT}, {"resize", TextureStrategy::RESIZE}, {"none", TextureStrategy::NONE}};
const EnumMap<PbrTier> pbrTiers[] = {{"full", PbrTier::FULL}, {"reduced", PbrTier::REDUCED}};
const EnumMap<LightMode> lightModes[] = {
    {"automatic", LightMode::AUTOMATIC}, {"per_object", LightMode::PER_OBJECT}, {"tiled", LightMode::TILED}};
const EnumMap<AtmosphereQuality> atmosphereQualities[] = {
    {"off", AtmosphereQuality::OFF}, {"low", AtmosphereQuality::LOW}, {"medium", AtmosphereQuality::MEDIUM}, {"high", AtmosphereQuality::HIGH}};
const EnumMap<ShadowTier> shadowTiers[] = {
    {"low", ShadowTier::LOW}, {"medium", ShadowTier::MEDIUM}, {"high", ShadowTier::HIGH}, {"ultra", ShadowTier::ULTRA}};
const EnumMap<WindowMode> windowModes[] = {
    {"windowed", WindowMode::WINDOWED}, {"maximized", WindowMode::MAXIMIZED}, {"fullscreen", WindowMode::FULLSCREEN}};
const EnumMap<ScreenOrientation> playerOrientations[] = {
    {"portrait", ScreenOrientation::Portrait}, {"portrait_upside_down", ScreenOrientation::PortraitUpsideDown},
    {"landscape_left", ScreenOrientation::LandscapeLeft}, {"landscape_right", ScreenOrientation::LandscapeRight},
    {"auto_rotation", ScreenOrientation::AutoRotation}};
const EnumMap<AndroidInternetAccess> androidInternetAccesses[] = {
    {"auto", AndroidInternetAccess::Auto}, {"require", AndroidInternetAccess::Require}};

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
    // a backslash is a separator on every platform (Stream::portablePath)
    text = FileUtils::portablePath(text);
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

// Unity's Input Manager actions (a list in the shape InputManagerFormat reads, or "defaults") into the
// project's actions asset's "Input Manager" map: one step of the project history (InputActionsCmd)
bool setInputManagerActions(SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
    std::vector<InputManagerAction> actions;
    if (value.is_string() && value.get<std::string>() == "defaults") {
        actions = InputManagerFormat::defaults();
    } else if (!value.is_array()) {
        error = "must be a list of actions [{name, type, bindings}, ...] or \"defaults\"";
        return false;
    } else if (!InputManagerFormat::parseActionsJson(value.dump(), actions, &error)) {
        return false;
    }
    if (!apply) return true;
    Project* p = ctx.project();
    if (InputManagerFormat::actionsToJson(actions) == InputManagerFormat::actionsToJson(p->getInputManagerActions()) &&
            !p->getInputActionsAsset().empty())
        return true;
    // a project without an asset that keeps the defaults keeps having none
    if (p->getInputActionsAsset().empty() && InputManagerFormat::actionsToJson(actions) == InputManagerFormat::actionsToJson(InputManagerFormat::defaults()))
        return true;
    auto* command = new InputActionsCmd(p, actions);
    command->setNoMerge();
    if (!p->getProjectCommandHistory()->addCommand(command)) {
        error = "the input actions could not be applied";
        return false;
    }
    return true;
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


// ---- native plugins (Project Settings > Native Plugins)

// A list of strings; `valid` checks each ("" when fine, else why)
Setting stringListField(const std::string& key, const std::string& description, std::function<std::vector<std::string>&(Project&)> field,
                        std::function<std::string(const std::string&)> valid = nullptr) {
    Setting s;
    s.key = key;
    s.type = "list";
    s.description = description;
    s.get = [field](SettingsContext& ctx) {
        Json list = Json::array();
        for (const std::string& item : field(*ctx.project())) list.push_back(item);
        return list;
    };
    s.set = [field, valid](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        if (!value.is_array()) {
            error = "must be a list of strings";
            return false;
        }
        std::vector<std::string> items;
        for (const Json& item : value) {
            std::string text;
            if (!readString(item, text, error, 2048)) return false;
            text.erase(0, text.find_first_not_of(" \t"));
            text.erase(text.find_last_not_of(" \t") + 1);
            if (text.empty()) continue;
            if (valid) {
                const std::string why = valid(text);
                if (!why.empty()) {
                    error = "\"" + text + "\" " + why;
                    return false;
                }
            }
            items.push_back(text);
        }
        if (apply) field(*ctx.project()) = items;
        return true;
    };
    return s;
}

// Raw XML, refused when it is not well-formed
Setting xmlField(const std::string& key, const std::string& description, std::function<std::string&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "string";
    s.description = description + " Refused unless well-formed XML; \"\" clears it.";
    s.get = [field](SettingsContext& ctx) { return Json(field(*ctx.project())); };
    s.set = [field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        std::string text;
        if (!readString(value, text, error, 65536)) return false;
        const std::string why = NativePlugins::xmlFragmentError(text);
        if (!why.empty()) {
            error = "is " + why;
            return false;
        }
        if (apply) field(*ctx.project()) = text;
        return true;
    };
    return s;
}

// Project Settings > Player > iOS / macOS > Privacy Manifest: one key of the project's additions,
// validated whole and applied as one undoable step (undo scope project)
bool applyApplePrivacy(SettingsContext& ctx, ApplePrivacy::Manifest privacy, bool apply, std::string& error) {
    privacy = ApplePrivacy::normalized(privacy);
    error = ApplePrivacy::validate(privacy);
    if (!error.empty()) return false;
    if (!apply || privacy == ctx.project()->getApplePrivacy()) return true;
    ApplePrivacySettingsCmd* command = new ApplePrivacySettingsCmd(ctx.project(), privacy);
    // the apple.privacy_* keys of one agent call are one undo step (the history merges a call's own commands)
    if (UndoOriginScope::current().kind != UndoOriginKind::Agent) command->setNoMerge();
    if (!ctx.project()->getProjectCommandHistory()->addCommand(command)) {
        error = "the privacy manifest settings could not be applied";
        return false;
    }
    return true;
}

bool readNameList(const Json& value, const char* what, std::vector<std::string>& out, std::string& error) {
    if (!value.is_array()) {
        error = std::string("must be a list of ") + what;
        return false;
    }
    out.clear();
    for (const Json& item : value) {
        if (!item.is_string()) {
            error = std::string("must be a list of ") + what;
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

void addApplePrivacySettings(const std::function<void(Setting)>& add) {
    const std::string tail = " Added to the engine's own entries (always there); iOS and macOS. Undoable (undo scope project). "
                             "Details: jasper://docs/shipping#privacy-manifest.";
    {
        Setting s;
        s.key = "apple.privacy_tracking";
        s.type = "bool";
        s.description = "Privacy manifest NSPrivacyTracking: the app or its SDKs track users (an ads SDK)." + tail;
        s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getApplePrivacy().tracking); };
        s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
            ApplePrivacy::Manifest privacy = ctx.project()->getApplePrivacy();
            if (!readBool(value, privacy.tracking, error)) return false;
            return applyApplePrivacy(ctx, privacy, apply, error);
        };
        add(s);
    }
    {
        Setting s;
        s.key = "apple.privacy_tracking_domains";
        s.type = "list";
        s.description = "Privacy manifest NSPrivacyTrackingDomains: host names that track (\"ads.example.com\")." + tail;
        s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getApplePrivacy().trackingDomains); };
        s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
            ApplePrivacy::Manifest privacy = ctx.project()->getApplePrivacy();
            if (!readNameList(value, "host names", privacy.trackingDomains, error)) return false;
            if (privacy.trackingDomains.size() > 64) {
                error = "holds at most 64 domains";
                return false;
            }
            return applyApplePrivacy(ctx, privacy, apply, error);
        };
        add(s);
    }
    {
        Setting s;
        s.key = "apple.privacy_collected_data";
        s.type = "list";
        s.description = "Privacy manifest NSPrivacyCollectedDataTypes: [{type, linked?, tracking?, purposes}], Apple's names short "
                        "(type \"DeviceID\", purposes [\"ThirdPartyAdvertising\"])." + tail;
        s.get = [](SettingsContext& ctx) {
            Json list = Json::array();
            for (const ApplePrivacy::CollectedData& data : ctx.project()->getApplePrivacy().collectedData) {
                list.push_back({{"type", data.type}, {"linked", data.linked}, {"tracking", data.tracking}, {"purposes", data.purposes}});
            }
            return list;
        };
        s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
            ApplePrivacy::Manifest privacy = ctx.project()->getApplePrivacy();
            if (!value.is_array()) {
                error = "must be a list of {type, linked, tracking, purposes}";
                return false;
            }
            privacy.collectedData.clear();
            for (const Json& item : value) {
                ApplePrivacy::CollectedData data;
                if (!item.is_object() || !item.contains("type") || !item["type"].is_string()) {
                    error = "each entry is {type, linked?, tracking?, purposes}";
                    return false;
                }
                for (const auto& [key, field] : item.items()) {
                    if (key != "type" && key != "linked" && key != "tracking" && key != "purposes") {
                        error = "has an unknown key \"" + key + "\" (type, linked, tracking, purposes)";
                        return false;
                    }
                }
                data.type = item["type"].get<std::string>();
                if (item.contains("linked") && !readBool(item["linked"], data.linked, error)) return false;
                if (item.contains("tracking") && !readBool(item["tracking"], data.tracking, error)) return false;
                if (item.contains("purposes") && !readNameList(item["purposes"], "purposes", data.purposes, error)) return false;
                privacy.collectedData.push_back(data);
            }
            return applyApplePrivacy(ctx, privacy, apply, error);
        };
        add(s);
    }
    {
        Setting s;
        s.key = "apple.privacy_accessed_apis";
        s.type = "list";
        s.description = "Privacy manifest NSPrivacyAccessedAPITypes the game's own code adds: [{type, reasons}] (type FileTimestamp, "
                        "SystemBootTime, DiskSpace, ActiveKeyboards or UserDefaults; reasons Apple's codes, \"E174.1\")." + tail;
        s.get = [](SettingsContext& ctx) {
            Json list = Json::array();
            for (const ApplePrivacy::AccessedApi& api : ctx.project()->getApplePrivacy().accessedApis) {
                list.push_back({{"type", api.type}, {"reasons", api.reasons}});
            }
            return list;
        };
        s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
            ApplePrivacy::Manifest privacy = ctx.project()->getApplePrivacy();
            if (!value.is_array()) {
                error = "must be a list of {type, reasons}";
                return false;
            }
            privacy.accessedApis.clear();
            for (const Json& item : value) {
                ApplePrivacy::AccessedApi api;
                if (!item.is_object() || !item.contains("type") || !item["type"].is_string() || !item.contains("reasons")) {
                    error = "each entry is {type, reasons}";
                    return false;
                }
                for (const auto& [key, field] : item.items()) {
                    if (key != "type" && key != "reasons") {
                        error = "has an unknown key \"" + key + "\" (type, reasons)";
                        return false;
                    }
                }
                api.type = item["type"].get<std::string>();
                if (!readNameList(item["reasons"], "reason codes", api.reasons, error)) return false;
                privacy.accessedApis.push_back(api);
            }
            return applyApplePrivacy(ctx, privacy, apply, error);
        };
        add(s);
    }
}

Json plistValueJson(const NativePlistValue& value) {
    switch (value.kind) {
        case NativePlistValue::Kind::Bool: return Json(value.flag);
        case NativePlistValue::Kind::Integer: return Json(value.integer);
        case NativePlistValue::Kind::Real: return Json(value.real);
        case NativePlistValue::Kind::StringArray: {
            Json list = Json::array();
            for (const std::string& item : value.items) list.push_back(item);
            return list;
        }
        case NativePlistValue::Kind::String: break;
    }
    return Json(value.text);
}

Setting plistEntriesField(const std::string& key, const std::string& description, std::function<std::map<std::string, NativePlistValue>&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "object";
    s.description = description;
    s.get = [field](SettingsContext& ctx) {
        Json object = Json::object();
        for (const auto& [name, value] : field(*ctx.project())) object[name] = plistValueJson(value);
        return object;
    };
    s.set = [field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        if (!value.is_object()) {
            error = "must be an object {\"NSUserTrackingUsageDescription\": \"...\", \"ITSAppUsesNonExemptEncryption\": false}";
            return false;
        }
        std::map<std::string, NativePlistValue> entries;
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string& name = it.key();
            if (name.empty() || name.find_first_of("<>&\"") != std::string::npos) {
                error = "key \"" + name + "\" is not an Info.plist key";
                return false;
            }
            const Json& v = it.value();
            NativePlistValue entry;
            if (v.is_boolean()) {
                entry.kind = NativePlistValue::Kind::Bool;
                entry.flag = v.get<bool>();
            } else if (v.is_number_integer()) {
                entry.kind = NativePlistValue::Kind::Integer;
                entry.integer = v.get<long long>();
            } else if (v.is_number()) {
                entry.kind = NativePlistValue::Kind::Real;
                entry.real = v.get<double>();
            } else if (v.is_string()) {
                entry.text = v.get<std::string>();
            } else if (v.is_array()) {
                entry.kind = NativePlistValue::Kind::StringArray;
                for (const Json& item : v) {
                    if (!item.is_string()) {
                        error = name + ": a list holds strings only (other shapes: ios.plist_xml)";
                        return false;
                    }
                    entry.items.push_back(item.get<std::string>());
                }
            } else {
                error = name + ": a value is a string, a boolean, a number or a list of strings (dictionaries: ios.plist_xml)";
                return false;
            }
            entries[name] = entry;
        }
        if (apply) field(*ctx.project()) = entries;
        return true;
    };
    return s;
}

std::string frameworkNameError(const std::string& name) {
    std::string bare = name;
    if (!bare.empty() && bare.back() == '?') bare.pop_back();
    if (bare.empty() || !std::all_of(bare.begin(), bare.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; })) {
        return "is not a system framework name (\"StoreKit\", \"AppTrackingTransparency?\" for weak)";
    }
    return std::string();
}

// Project Settings > Debug: one key of DebugSettings, applied as one undoable step (undo scope project)
Setting debugSetting(const std::string& key, const std::string& type, const std::string& description, std::vector<std::string> values,
                     std::function<Json(const DebugSettings&)> get, std::function<bool(DebugSettings&, const Json&, std::string&)> set) {
    Setting s;
    s.key = key;
    s.type = type;
    s.description = description + " Undoable (undo scope project).";
    s.values = std::move(values);
    s.get = [get](SettingsContext& ctx) { return get(ctx.project()->getDebugSettings()); };
    s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        Project* p = ctx.project();
        DebugSettings changed = p->getDebugSettings();
        if (!set(changed, value, error)) return false;
        if (apply && changed != p->getDebugSettings()) {
            auto* command = new DebugSettingsCmd(p, p->getDebugSettings(), changed);
            command->setNoMerge();
            if (!p->getProjectCommandHistory()->addCommand(command)) {
                error = "the debug settings could not be applied";
                return false;
            }
        }
        return true;
    };
    return s;
}

// GPU Instancing (spec S55): DrawSubmissionSettings changed, applied as one undoable step (undo
// scope project)
bool setDrawSubmission(SettingsContext& ctx, const std::function<void(DrawSubmissionSettings&)>& change, bool apply, std::string& error) {
    Project* p = ctx.project();
    DrawSubmissionSettings changed = p->getDrawSubmissionSettings();
    change(changed);
    if (apply && changed != p->getDrawSubmissionSettings()) {
        auto* command = new DrawSubmissionSettingsCmd(p, changed);
        command->setNoMerge();
        if (!p->getProjectCommandHistory()->addCommand(command)) {
            error = "the draw submission settings could not be applied";
            return false;
        }
    }
    return true;
}

// Screen Transitions (S48 R41): the defaults changed, applied as one undoable step (undo scope project)
bool setTransitionDefaults(SettingsContext& ctx, const std::function<void(jasperengine::pixelart::TransitionDefaults&)>& change, bool apply) {
    Project* p = ctx.project();
    jasperengine::pixelart::TransitionDefaults changed = p->getTransitionDefaults();
    change(changed);
    if (apply && changed != p->getTransitionDefaults()) {
        auto* command = new TransitionSettingsCmd(p, changed);
        command->setNoMerge();
        p->getProjectCommandHistory()->addCommand(command);
    }
    return true;
}

// One key of the device settings (spec S56), applied as one undoable step (undo scope project):
// `set` changes a copy, which a DeviceSettingsCmd applies when it differs
Setting deviceSetting(const std::string& key, const std::string& type, const std::string& description,
                      std::function<Json(const DeviceSettings&)> get, std::function<bool(DeviceSettings&, const Json&, std::string&)> set) {
    Setting s;
    s.key = key;
    s.type = type;
    s.description = description + " Undoable (undo scope project).";
    s.get = [get](SettingsContext& ctx) { return get(ctx.project()->getDeviceSettings()); };
    s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
        Project* p = ctx.project();
        DeviceSettings changed = p->getDeviceSettings();
        if (!set(changed, value, error)) return false;
        if (apply && changed != p->getDeviceSettings()) {
            auto* command = new DeviceSettingsCmd(p, changed);
            command->setNoMerge();
            if (!p->getProjectCommandHistory()->addCommand(command)) {
                error = "the device settings could not be applied";
                return false;
            }
        }
        return true;
    };
    return s;
}

// a tier's cells as JSON: every field, null where it inherits; enumerated cells by name
Json deviceTierJson(const DeviceTierOverrides& tier) {
    Json out = Json::object();
    for (int f = 0; f < DEVICE_OVERRIDE_COUNT; f++) {
        const DeviceOverride field = (DeviceOverride)f;
        const DeviceOverrideInfo& info = DeviceOverrides::info(field);
        const std::optional<float>& cell = tier.get(field);
        if (!cell) {
            out[info.key] = nullptr;
        } else if (info.kind == DeviceOverrideInfo::Kind::Off) {
            out[info.key] = false;
        } else if (info.names) {
            out[info.key] = lowerText(DeviceOverrides::format(field, *cell));
        } else if (info.integer) {
            out[info.key] = (int)*cell;
        } else {
            out[info.key] = *cell;
        }
    }
    return out;
}

// merges {field: value | null} into a tier: a listed field is replaced, null inherits again
bool mergeDeviceTier(DeviceTierOverrides& tier, const Json& value, std::string& error) {
    if (!value.is_object()) {
        error = "must be an object of cells ({\"render_scale\": 0.6, \"ssao\": false, \"msaa_samples\": null}); null inherits";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        DeviceOverride field;
        if (!DeviceOverrides::fromKey(it.key(), field)) {
            std::string keys;
            for (int f = 0; f < DEVICE_OVERRIDE_COUNT; f++) keys += std::string(f ? ", " : "") + DeviceOverrides::info((DeviceOverride)f).key;
            error = "\"" + it.key() + "\" is not a cell (" + keys + ")";
            return false;
        }
        const Json& cell = it.value();
        float number = 0.0f;
        if (cell.is_null()) {
            tier.set(field, std::nullopt);
            continue;
        }
        if (cell.is_boolean()) {
            if (DeviceOverrides::info(field).kind != DeviceOverrideInfo::Kind::Off || cell.get<bool>()) {
                error = it.key() + (DeviceOverrides::info(field).kind == DeviceOverrideInfo::Kind::Off
                                    ? ": a tier only turns it off (false); null inherits" : ": must be a number");
                return false;
            }
        } else if (cell.is_string()) {
            if (!DeviceOverrides::parse(field, cell.get<std::string>(), number, error)) return false;
        } else if (cell.is_number()) {
            number = cell.get<float>();
            if (!DeviceOverrides::validate(field, number, error)) return false;
        } else {
            error = it.key() + ": must be a number, a name or null";
            return false;
        }
        tier.set(field, number);
    }
    return true;
}

// Project Settings > Quality > Device Tiers and the rest of the device settings (spec S56)
void addDeviceSettings(const std::function<void(Setting)>& add) {
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++) {
        const DevicePlatformGroup group = (DevicePlatformGroup)g;
        for (int t = 0; t < DEVICE_TIER_COUNT; t++) {
            const DeviceTier tier = (DeviceTier)t;
            add(deviceSetting(std::string("quality.device_tiers.") + DeviceNames::group(group) + "." + DeviceNames::tier(tier), "object",
                std::string("The ") + DeviceNames::tierLabel(tier) + " tier's overrides on " + DeviceNames::groupLabel(group) +
                " (docs/device-tiers.md): an object of cells merged in, null inherits the project's value; caps lower, never raise "
                "(render_scale, msaa_samples, shadow_quality, shadow_distance, lit_quality, target_frame_rate, max_pixel_lights, "
                "particle_budget), texture_quality drops at least, bloom/ssao/volumetrics false turns them off, lod_bias and "
                "shadow_lod_bias multiply.",
                [group, tier](const DeviceSettings& d) { return deviceTierJson(d.tier(group, tier)); },
                [group, tier](DeviceSettings& d, const Json& v, std::string& e) { return mergeDeviceTier(d.tier(group, tier), v, e); }));
        }
        add(deviceSetting(std::string("quality.thermal_response.") + DeviceNames::group(group), "bool",
            std::string("On ") + DeviceNames::groupLabel(group) + ", a Serious thermal state lowers the render scale by 20% and caps the "
            "frame rate at 30; Critical also takes the tier below's shadows and post-processing; recovery after 30 s.",
            [g](const DeviceSettings& d) { return Json((bool)d.thermalResponse[g]); },
            [g](DeviceSettings& d, const Json& v, std::string& e) {
                bool on;
                if (!readBool(v, on, e)) return false;
                d.thermalResponse[g] = on;
                return true;
            }));
        add(deviceSetting(std::string("graphics.upload_budget_mb.") + DeviceNames::group(group), "int",
            std::string("MB of queued texture and buffer uploads a ") + DeviceNames::groupLabel(group) +
            " frame runs at most (a larger one goes alone); the rest waits for the next frames.",
            [g](const DeviceSettings& d) { return Json(d.uploadBudgetMB[g]); },
            [g](DeviceSettings& d, const Json& v, std::string& e) {
                long long mb;
                if (!readInt(v, DeviceSettings::MIN_UPLOAD_BUDGET_MB, DeviceSettings::MAX_UPLOAD_BUDGET_MB, mb, e)) return false;
                d.uploadBudgetMB[g] = (int)mb;
                return true;
            }));
    }
    add(deviceSetting("quality.lod_bias", "number",
        "Multiplies every mesh's LOD Bias (below 1 switches to coarser LODs nearer); a Device Tier's lod_bias multiplies it again.",
        [](const DeviceSettings& d) { return Json(d.lodBias); },
        [](DeviceSettings& d, const Json& v, std::string& e) {
            double bias;
            if (!readNumber(v, 0.05, 8.0, bias, e)) return false;
            d.lodBias = (float)bias;
            return true;
        }));
    add(deviceSetting("quality.shadow_lod_bias", "number",
        "Multiplies the LOD bias of what shadow maps draw; a Device Tier's shadow_lod_bias multiplies it again.",
        [](const DeviceSettings& d) { return Json(d.shadowLodBias); },
        [](DeviceSettings& d, const Json& v, std::string& e) {
            double bias;
            if (!readNumber(v, 0.05, 8.0, bias, e)) return false;
            d.shadowLodBias = (float)bias;
            return true;
        }));
}

// "mainThreadBudgetMs" -> "main_thread_budget_ms"
std::string snakeCase(const char* name) {
    std::string out;
    for (const char* c = name; *c; c++) {
        if (std::isupper(static_cast<unsigned char>(*c))) {
            if (!out.empty()) out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        } else {
            out += *c;
        }
    }
    return out;
}

// Project Settings > Graphics > Shader Creation (spec S32): one MCP row per field of the reflection
// table (editor/reflect/ShaderCreationReflect.cpp), "shader_creation.<field>"; a change is an
// undoable command
void addShaderCreationSettings(const std::function<void(Setting)>& add) {
    for (const reflect::FieldDesc& field : reflect::shaderCreationStruct().fields) {
        Setting s;
        s.key = std::string("shader_creation.") + snakeCase(field.name);
        s.type = field.kind == reflect::Kind::Bool ? "bool" : "number";
        s.description = field.tooltip ? field.tooltip : "";
        const reflect::FieldDesc* desc = &field;
        s.get = [desc](SettingsContext& ctx) {
            ShaderCreationProjectSettings settings = ctx.project()->getShaderCreationSettings();
            void* value = desc->ref(&settings);
            return desc->kind == reflect::Kind::Bool ? Json(*static_cast<bool*>(value)) : Json(*static_cast<float*>(value));
        };
        s.set = [desc](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
            ShaderCreationProjectSettings settings = ctx.project()->getShaderCreationSettings();
            void* target = desc->ref(&settings);
            if (desc->kind == reflect::Kind::Bool) {
                bool b;
                if (!readBool(value, b, error)) return false;
                *static_cast<bool*>(target) = b;
            } else {
                double v;
                if (!readNumber(value, desc->min, desc->max, v, error)) return false;
                *static_cast<float*>(target) = static_cast<float>(v);
            }
            if (apply && settings != ctx.project()->getShaderCreationSettings())
                ctx.project()->getProjectCommandHistory()->addCommand(new ShaderCreationSettingsCmd(ctx.project(), settings));
            return true;
        };
        add(s);
    }
}

const DebugSettingsNames::SettingKey* debugSettingKey(const std::string& key) {
    for (const DebugSettingsNames::SettingKey& entry : DebugSettingsNames::settingKeys()) {
        if (key == entry.key) return &entry;
    }
    return nullptr;
}

// One row per field: its type, values, help and range are the table's; a change is one undoable
// step (DebugSettingsCmd)
void addDebugSettings(const std::function<void(Setting)>& add) {
    for (const DebugSettingsNames::SettingKey& entry : DebugSettingsNames::settingKeys()) {
        const reflect::FieldDesc* field = reflect::debugSettingsField(entry.field);
        if (!field) continue;
        const std::string description = field->tooltip ? field->tooltip : "";
        if (field->enumDesc) {
            std::vector<std::string> names;
            for (const reflect::EnumItem& item : field->enumDesc->items) names.push_back(item.token);
            add(debugSetting(entry.key, "enum", description, names,
                [field](const DebugSettings& d) {
                    const reflect::EnumItem* item = reflect::enumItem(*field->enumDesc, reflect::readEnum(*field, field->ref(const_cast<DebugSettings*>(&d))));
                    return Json(item ? item->token : "");
                },
                [field, names](DebugSettings& d, const Json& value, std::string& error) {
                    const reflect::EnumItem* item = value.is_string() ? reflect::enumItemByToken(*field->enumDesc, value.get<std::string>()) : nullptr;
                    if (!item) {
                        error = "must be one of:";
                        for (const std::string& name : names) error += " " + name;
                        return false;
                    }
                    reflect::writeEnum(*field, field->ref(&d), item->value);
                    return true;
                }));
        } else if (field->kind == reflect::Kind::Bool) {
            add(debugSetting(entry.key, "bool", description, {},
                [field](const DebugSettings& d) { return Json(*static_cast<const bool*>(field->ref(const_cast<DebugSettings*>(&d)))); },
                [field](DebugSettings& d, const Json& value, std::string& error) {
                    return readBool(value, *static_cast<bool*>(field->ref(&d)), error);
                }));
        } else if (field->drawer && std::string(field->drawer) == "key") {
            add(debugSetting(entry.key, "string", description + " A key name (backquote, f3, f12, escape, a...) or \"\" for none.", {},
                [field](const DebugSettings& d) { return Json(*static_cast<const std::string*>(field->ref(const_cast<DebugSettings*>(&d)))); },
                [field](DebugSettings& d, const Json& value, std::string& error) {
                    std::string text;
                    if (!readString(value, text, error, 64)) return false;
                    if (!DebugSettingsNames::isKeyName(text)) {
                        error = "is not a key name (backquote, f3, f12, escape, a...)";
                        return false;
                    }
                    *static_cast<std::string*>(field->ref(&d)) = text;
                    return true;
                }));
        } else {
            add(debugSetting(entry.key, "int", description, {},
                [field](const DebugSettings& d) { return Json(*static_cast<const unsigned int*>(field->ref(const_cast<DebugSettings*>(&d)))); },
                [field](DebugSettings& d, const Json& value, std::string& error) {
                    long long v;
                    if (!readInt(value, static_cast<long long>(field->min), static_cast<long long>(field->max), v, error)) return false;
                    *static_cast<unsigned int*>(field->ref(&d)) = static_cast<unsigned int>(v);
                    return true;
                }));
        }
    }
}

std::string titleCase(const std::string& key);

// the Debug page's rows: section (the table's group), label and help the table's
void debugRowPlacement(const std::string& key, SettingRow& row) {
    row.page = "Debug";
    row.label = titleCase(key);
    const DebugSettingsNames::SettingKey* entry = debugSettingKey(key);
    if (!entry) return;
    row.keywords = entry->keywords;
    if (const reflect::FieldDesc* field = reflect::debugSettingsField(entry->field)) {
        if (field->group) row.section = field->group;
        if (field->label) row.label = field->label;
    }
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
        // Player > Resolution and Presentation (upstream's platform services; android.orientation and
        // android.keep_screen_on of earlier projects migrate to these)
        {
            Setting s;
            s.key = "player.default_orientation";
            s.type = "enum";
            s.values = enumNames(playerOrientations);
            s.description = "Orientation phones and tablets start in (Android and iOS); auto_rotation turns within "
                            "player.allowed_orientations. landscape_left = device turned counter-clockwise.";
            s.get = [](SettingsContext& ctx) {
                return Json(enumName(playerOrientations, ctx.project()->getPresentationSettings().defaultOrientation));
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                ScreenOrientation o;
                if (!parseEnum(playerOrientations, value, o, error)) return false;
                if (apply) ctx.project()->getPresentationSettings().defaultOrientation = o;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.allowed_orientations";
            s.type = "list";
            s.values = {"portrait", "portrait_upside_down", "landscape_left", "landscape_right"};
            s.description = "Allowed Orientations for Auto Rotation: non-empty subset; a fixed default is always allowed too.";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                const uint8_t mask = ctx.project()->getPresentationSettings().allowedOrientations;
                for (int i = 0; i < 4; i++) {
                    if (mask & PlatformRules::orientationBit(playerOrientations[i].value)) list.push_back(playerOrientations[i].name);
                }
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array() || value.empty()) {
                    error = "must be a non-empty list of portrait, portrait_upside_down, landscape_left, landscape_right";
                    return false;
                }
                uint8_t mask = 0;
                for (const Json& item : value) {
                    ScreenOrientation o;
                    if (!parseEnum(playerOrientations, item, o, error) || !PlatformRules::isFixedOrientation(o)) {
                        error = "must be a non-empty list of portrait, portrait_upside_down, landscape_left, landscape_right";
                        return false;
                    }
                    mask |= PlatformRules::orientationBit(o);
                }
                if (apply) ctx.project()->getPresentationSettings().allowedOrientations = mask;
                return true;
            };
            add(s);
        }
        add(boolField("player.keep_screen_on", "Initial Screen.sleepTimeout on Android/iOS: true = NeverSleep, false = the device setting.",
                      [](Project& p) -> bool& { return p.getPresentationSettings().keepScreenOn; }));
        {
            Setting s;
            s.key = "player.target_frame_rate";
            s.type = "int";
            s.description = "Initial Engine.targetFrameRate; -1 = platform default (60 mobile; desktops ignore it with vsync on).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getPresentationSettings().targetFrameRate); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, -1, 1000, v, error)) return false;
                if (apply) ctx.project()->getPresentationSettings().targetFrameRate = v <= 0 ? -1 : (int)v;
                return true;
            };
            add(s);
        }
        add(boolField("player.allow_insecure_http", "Plain http:// to hosts other than localhost/127.0.0.1/::1 (Http). Off (default): "
                      "insecure_http on every platform, editor Play included; exports set Android cleartext and iOS/macOS ATS to match.",
                      [](Project& p) -> bool& { return p.getApplicationSettings().allowInsecureHttp; }));
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
        // S30: how Build makes the game, and the prebuilt player's content pack
        auto nameChoice = [&add](const std::string& key, const std::string& description, std::vector<std::string> names,
                                 std::function<std::string(Project&)> get, std::function<void(Project&, const std::string&)> set) {
            Setting s;
            s.key = key;
            s.type = "enum";
            s.values = names;
            s.description = description;
            s.get = [get](SettingsContext& ctx) { return Json(get(*ctx.project())); };
            s.set = [names, set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (std::find(names.begin(), names.end(), text) == names.end()) {
                    error = "one of";
                    for (const std::string& name : names) error += " " + name;
                    return false;
                }
                if (apply) set(*ctx.project(), text);
                return true;
            };
            add(s);
        };
        nameChoice("player.build_method", "Build Settings > Build Method: auto (a prebuilt player unless the project needs native code), prebuilt or compile.",
                   {"auto", "prebuilt", "compile"}, [](Project& p) { return p.getBuildMethod(); },
                   [](Project& p, const std::string& v) { p.setBuildMethod(v); });
        add(boolSetting("player.pack_content", "Prebuilt builds keep their content in one content.jpak (the web always packs it).",
                        [](Project& p) { return p.isPackContent(); }, [](Project& p, bool v) { p.setPackContent(v); }));
        add(boolSetting("player.strip_engine_code", "Builds leave out the engine modules nothing in the project reaches (HTML UI, video, the runtime "
                        "shader compiler, 3D or 2D physics): a smaller player that behaves the same. Off: every module the settings allow.",
                        [](Project& p) { return p.isStripEngineCode(); }, [](Project& p, bool v) { p.setStripEngineCode(v); }));
        nameChoice("android.package_format", "Android builds make an App Bundle (aab, for Google Play: one ABI per device), one APK per ABI "
                   "(apk-per-abi), or one universal APK of every selected ABI (apk).", {"aab", "apk-per-abi", "apk"},
                   [](Project& p) { return p.getAndroidProjectSettings().packageFormat; },
                   [](Project& p, const std::string& v) { p.getAndroidProjectSettings().packageFormat = v; });
        add(pathSetting("android.keystore_path", "The keystore that signs Android release builds (passwords are asked at build time, never saved).",
                        [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().keystorePath; }));
        add(stringSetting("android.keystore_alias", "The key alias of the Android keystore.",
                          [](Project& p) -> std::string& { return p.getAndroidProjectSettings().keystoreAlias; }));
        add(stringSetting("ios.team_id", "The Apple Developer team id that signs iOS device builds (DEVELOPMENT_TEAM).",
                          [](Project& p) -> std::string& { return p.getIOSProjectSettings().teamId; }));
        nameChoice("ios.export_method", "How an iOS archive is exported to an .ipa.", {"development", "ad-hoc", "app-store", "enterprise"},
                   [](Project& p) { return p.getIOSProjectSettings().exportMethod; },
                   [](Project& p, const std::string& v) { p.getIOSProjectSettings().exportMethod = v; });

        // ---- per platform (overrides: "" inherits the shared player value)
        add(stringSetting("web.application_name", "Web page title override.", [](Project& p) -> std::string& { return p.getWebProjectSettings().applicationName; }));
        add(pathSetting("web.favicon", "Web favicon.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().favicon; }));
        add(pathSetting("web.custom_html_shell", "Custom Emscripten HTML shell.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().customHtmlShell; }));
        add(stringSetting("web.head_include", "Extra HTML put in the page <head>.", [](Project& p) -> std::string& { return p.getWebProjectSettings().headInclude; }));
        add(boolField("web.resize_canvas_to_window", "The canvas follows the browser window size.", [](Project& p) -> bool& { return p.getWebProjectSettings().resizeCanvasToWindow; }));
        add(boolField("web.hide_emscripten_ui", "Hide the Emscripten status/console UI.", [](Project& p) -> bool& { return p.getWebProjectSettings().hideEmscriptenUI; }));
        add(stringListField("web.script_urls", "Native plugins: SDK script URLs, each a <script src> in <head> before plugins/native/web/*.js (in order).",
                            [](Project& p) -> std::vector<std::string>& { return p.getWebProjectSettings().scriptUrls; },
                            [](const std::string& v) { return v.find_first_of("<>\"") != std::string::npos ? std::string("is not a URL") : std::string(); }));
        add(boolField("web.viewport_fit_cover", "Page viewport-fit=cover: canvas under the notch, safe area from env().", [](Project& p) -> bool& { return p.getWebProjectSettings().viewportFitCover; }));
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
        add(plistEntriesField("macos.plist_entries", "Native plugins: macOS Info.plist keys -> string, boolean, number or string list (replacing the export's keys).",
                              [](Project& p) -> std::map<std::string, NativePlistValue>& { return p.getMacOSProjectSettings().plistEntries; }));
        add(stringListField("macos.frameworks", "Native plugins: macOS system frameworks linked (\"GameKit\"; \"Name?\" = weak).",
                            [](Project& p) -> std::vector<std::string>& { return p.getMacOSProjectSettings().frameworks; }, frameworkNameError));
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
        add(plistEntriesField("ios.plist_entries", "Native plugins: iOS Info.plist keys -> string, boolean, number or string list, replacing the export's "
                              "(NSUserTrackingUsageDescription, NSCameraUsageDescription...).",
                              [](Project& p) -> std::map<std::string, NativePlistValue>& { return p.getIOSProjectSettings().plistEntries; }));
        add(xmlField("ios.plist_xml", "Native plugins: raw <key>..</key><value> pairs appended to the iOS Info.plist top dict (SKAdNetworkItems...).",
                     [](Project& p) -> std::string& { return p.getIOSProjectSettings().plistXml; }));
        add(stringListField("ios.frameworks", "Native plugins: iOS system frameworks linked (\"StoreKit\", \"AppTrackingTransparency?\" = weak).",
                            [](Project& p) -> std::vector<std::string>& { return p.getIOSProjectSettings().frameworks; }, frameworkNameError));
        {
            Setting s;
            s.key = "ios.cocoapods";
            s.type = "list";
            s.description = "Native plugins: CocoaPods [{name, version?, source?}] in workspaces/xcode/Podfile (run pod install there, open the "
                            ".xcworkspace); version like \"~> 1.7\", source a spec repository URL.";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                for (const NativeCocoaPod& pod : ctx.project()->getIOSProjectSettings().cocoapods) {
                    Json item = {{"name", pod.name}};
                    if (!pod.version.empty()) item["version"] = pod.version;
                    if (!pod.source.empty()) item["source"] = pod.source;
                    list.push_back(item);
                }
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of {name, version?, source?}";
                    return false;
                }
                std::vector<NativeCocoaPod> pods;
                for (const Json& item : value) {
                    NativeCocoaPod pod;
                    if (item.is_string()) {
                        pod.name = item.get<std::string>();
                    } else if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                        pod.name = item["name"].get<std::string>();
                        if (item.contains("version") && !readString(item["version"], pod.version, error, 256)) return false;
                        if (item.contains("source") && !readString(item["source"], pod.source, error, 2048)) return false;
                    } else {
                        error = "each pod is {name, version?, source?}";
                        return false;
                    }
                    if (pod.name.empty() || pod.name.find_first_of(" '\"\\") != std::string::npos) {
                        error = "\"" + pod.name + "\" is not a pod name";
                        return false;
                    }
                    if (pod.version.find_first_of("'\\\n") != std::string::npos || pod.source.find_first_of("'\\\n ") != std::string::npos) {
                        error = pod.name + ": version and source cannot hold quotes, backslashes or line breaks";
                        return false;
                    }
                    pods.push_back(pod);
                }
                if (apply) ctx.project()->getIOSProjectSettings().cocoapods = pods;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "ios.swift_packages";
            s.type = "list";
            s.description = "Native plugins: Swift packages [{url, version, products[]}] of the iOS target (up to the next major version).";
            s.get = [](SettingsContext& ctx) {
                Json list = Json::array();
                for (const NativeSwiftPackage& package : ctx.project()->getIOSProjectSettings().swiftPackages) {
                    list.push_back({{"url", package.url}, {"version", package.version}, {"products", package.products}});
                }
                return list;
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of {url, version, products}";
                    return false;
                }
                std::vector<NativeSwiftPackage> packages;
                for (const Json& item : value) {
                    if (!item.is_object() || !item.contains("url") || !item["url"].is_string()) {
                        error = "each package is {url, version, products}";
                        return false;
                    }
                    NativeSwiftPackage package;
                    package.url = item["url"].get<std::string>();
                    if (package.url.rfind("https://", 0) != 0 && package.url.rfind("git@", 0) != 0) {
                        error = "\"" + package.url + "\" is not a repository URL (https://github.com/...)";
                        return false;
                    }
                    if (!item.contains("version") || !readString(item["version"], package.version, error, 64) ||
                        !std::regex_match(package.version, std::regex("^[0-9]+(\\.[0-9]+){0,2}([-+][0-9A-Za-z.-]+)?$"))) {
                        error = package.url + ": version must be a minimum version like \"4.0.0\"";
                        return false;
                    }
                    if (!item.contains("products") || !item["products"].is_array() || item["products"].empty()) {
                        error = package.url + ": products lists the package products to link (e.g. [\"RevenueCat\"])";
                        return false;
                    }
                    for (const Json& product : item["products"]) {
                        if (!product.is_string() || product.get<std::string>().empty()) {
                            error = package.url + ": products are names";
                            return false;
                        }
                        package.products.push_back(product.get<std::string>());
                    }
                    packages.push_back(package);
                }
                if (apply) ctx.project()->getIOSProjectSettings().swiftPackages = packages;
                return true;
            };
            add(s);
        }
        add(xmlField("ios.entitlements_xml", "Native plugins: the <key>..</key><value> pairs of ios/JasperEngine.entitlements (CODE_SIGN_ENTITLEMENTS).",
                     [](Project& p) -> std::string& { return p.getIOSProjectSettings().entitlementsXml; }));
        addApplePrivacySettings(add);
        add(stringSetting("ios.deployment_target", "iOS deployment target of every target (IPHONEOS_DEPLOYMENT_TARGET), 15.0 or later.",
                          [](Project& p) -> std::string& { return p.getIOSProjectSettings().deploymentTarget; },
                          [](const std::string& v) { return std::regex_match(v, std::regex("^[0-9]+\\.[0-9]+$")) && std::stod(v) >= 15.0; },
                          "a version like 15.0 or 16.4, at least 15.0"));
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
        {
            Setting s;
            s.key = "android.internet_access";
            s.type = "enum";
            s.values = enumNames(androidInternetAccesses);
            s.description = "INTERNET permission: auto adds it when exported Lua/C++ uses Http (or android.permissions has internet); "
                            "require always adds it (e.g. a native plugin uses the network).";
            s.get = [](SettingsContext& ctx) { return Json(enumName(androidInternetAccesses, ctx.project()->getAndroidProjectSettings().internetAccess)); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                AndroidInternetAccess access;
                if (!parseEnum(androidInternetAccesses, value, access, error)) return false;
                if (apply) ctx.project()->getAndroidProjectSettings().internetAccess = access;
                return true;
            };
            add(s);
        }
        add(stringListField("android.gradle_dependencies", "Native plugins: Gradle dependencies; \"group:name:version\" becomes api \"...\" in the "
                            "nativeplugins module, any other text is a dependencies line as is (implementation platform('...')).",
                            [](Project& p) -> std::vector<std::string>& { return p.getAndroidProjectSettings().gradleDependencies; },
                            [](const std::string& v) { return v.find('\n') != std::string::npos ? std::string("is more than one line") : std::string(); }));
        add(stringListField("android.gradle_repositories", "Native plugins: Maven repository URLs (allprojects.repositories).",
                            [](Project& p) -> std::vector<std::string>& { return p.getAndroidProjectSettings().gradleRepositories; },
                            [](const std::string& v) { return (v.find("://") == std::string::npos || v.find_first_of("\" \n") != std::string::npos)
                                ? std::string("is not a repository URL (https://...)") : std::string(); }));
        add(stringListField("android.gradle_properties", "Native plugins: gradle.properties lines \"key=value\" (replacing a key already there).",
                            [](Project& p) -> std::vector<std::string>& { return p.getAndroidProjectSettings().gradleProperties; },
                            [](const std::string& v) { return (v.find('=') == std::string::npos || v.find('=') == 0 || v.find('\n') != std::string::npos)
                                ? std::string("is not a key=value line") : std::string(); }));
        add(stringListField("android.extra_permissions", "Native plugins: <uses-permission> by full name (com.google.android.gms.permission.AD_ID).",
                            [](Project& p) -> std::vector<std::string>& { return p.getAndroidProjectSettings().extraPermissions; },
                            [](const std::string& v) { return (v.find('.') == std::string::npos || v.find_first_of("\"<>& ") != std::string::npos)
                                ? std::string("is not a full permission name (android.permission.X)") : std::string(); }));
        add(xmlField("android.manifest_xml", "Native plugins: raw XML inside <manifest>, before <application> (<queries>, tools:node=\"remove\" entries).",
                     [](Project& p) -> std::string& { return p.getAndroidProjectSettings().manifestXml; }));
        add(xmlField("android.manifest_application_xml", "Native plugins: raw XML inside <application> (<meta-data>, <activity>, <provider>).",
                     [](Project& p) -> std::string& { return p.getAndroidProjectSettings().manifestApplicationXml; }));

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
            // S41 R6
            Setting s;
            s.key = "graphics.pixels_per_unit";
            s.type = "int";
            s.description = "Texels per world unit of sprites and tilemaps whose own Pixels Per Unit is 0 (1: a unit is a pixel).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getPixelsPerUnit()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, Project::minPixelsPerUnit, Project::maxPixelsPerUnit, v, error)) return false;
                if (apply) ctx.project()->setPixelsPerUnit(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        {
            // S51 R12
            Setting s;
            s.key = "graphics.bitmap_text_cache_mib";
            s.type = "int";
            s.description = "MiB of styled bitmap glyphs (pixel outlines and shadows); 0: 2 on mobile and web, 8 on desktop.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getBitmapTextCacheMiB()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 0, Project::maxBitmapTextCacheMiB, v, error)) return false;
                if (apply) ctx.project()->setBitmapTextCacheMiB(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.ui_scale";
            s.type = "object";
            s.description = "Canvas Scaler of every scene's UI and screen HTML documents unless a scene overrides it (scene setting "
                            "canvas_scaler): {mode: follow_game_canvas (default, UI = game canvas)|constant_pixel_size|"
                            "scale_with_screen_size|constant_physical_size, reference: [w,h], match_mode: "
                            "match_width_or_height|expand|shrink, match: 0 width..1 height, scale_factor, physical_unit, "
                            "fallback_dpi}; fields not given keep their value.";
            s.get = [](SettingsContext& ctx) { return mcp::McpValues::canvasScalerToJson(ctx.project()->getUIScale()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                CanvasScalerSettings settings = ctx.project()->getUIScale();
                if (!mcp::McpValues::canvasScalerFromJson(value, settings, error)) return false;
                if (apply) ctx.project()->setUIScale(settings);
                return true;
            };
            add(s);
        }
        {
            // S45 R18
            Setting s;
            s.key = "sprite_atlas.mode";
            s.type = "enum";
            s.values = {"enabled", "enabled_for_builds", "disabled"};
            s.description = "Sprite Atlases: enabled (editor, Play and builds draw from them), enabled_for_builds (only builds), disabled.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getSpriteAtlasMode()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                const std::string mode = value.is_string() ? value.get<std::string>() : std::string();
                if (mode != "enabled" && mode != "enabled_for_builds" && mode != "disabled") {
                    error = "sprite_atlas.mode is enabled, enabled_for_builds or disabled";
                    return false;
                }
                if (apply) ctx.project()->setSpriteAtlasMode(mode);
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
            s.key = "graphics.light_mode";
            s.type = "enum";
            s.values = enumNames(lightModes);
            s.description = "How lit meshes find their point and spot lights. Tiled lights every point and spot light per pixel from "
                            "screen tiles (Forward+, up to 32 a tile; a per-frame CPU job and one data texture); Per Object caps each "
                            "object's lights at Max Lights Per Object; Automatic (default) is Tiled on Medium tiers and above.";
            s.get = [](SettingsContext& ctx) { return Json(enumName(lightModes, ctx.project()->getLightMode())); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                LightMode v;
                if (!parseEnum(lightModes, value, v, error)) return false;
                if (apply) ctx.project()->setLightMode(v);
                return true;
            };
            add(s);
        }
        // ---- texture (Project Settings > Texture, S14 R24)
        {
            auto textureSetting = [&](const std::string& key, const std::string& type, const std::string& description,
                                      std::function<Json(const TextureProjectSettings&)> get,
                                      std::function<bool(TextureProjectSettings&, const Json&, std::string&)> set,
                                      std::vector<std::string> values = {}) {
                Setting s;
                s.key = key;
                s.type = type;
                s.description = description;
                s.values = std::move(values);
                s.get = [get](SettingsContext& ctx) { return get(ctx.project()->getTextureSettings()); };
                s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                    TextureProjectSettings next = ctx.project()->getTextureSettings();
                    if (!set(next, value, error)) return false;
                    if (apply) ctx.project()->setTextureSettings(next);
                    return true;
                };
                add(s);
            };
            textureSetting("texture.android_etc2_fallback", "bool",
                           "Android exports also carry ETC2/EAC cooks for devices without ASTC (and the manifest stops asking for ASTC).",
                           [](const TextureProjectSettings& t) { return Json(t.androidEtc2Fallback); },
                           [](TextureProjectSettings& t, const Json& v, std::string& e) { return readBool(v, t.androidEtc2Fallback, e); });
            for (size_t f = 0; f < kTextureFamilyCount; f++) {
                const std::string family = textureFamilyName((TextureFamily)f);
                textureSetting("texture.default_quality." + family, "enum",
                               "Compression of " + family + " cooks without an override (none keeps them uncompressed).",
                               [f](const TextureProjectSettings& t) { return Json(textureQualityName(t.defaultQuality[f])); },
                               [f](TextureProjectSettings& t, const Json& v, std::string& e) {
                                   std::string name;
                                   if (!readString(v, name, e)) return false;
                                   for (TextureCookQuality q : {TextureCookQuality::None, TextureCookQuality::Low, TextureCookQuality::Normal, TextureCookQuality::High}) {
                                       if (name == textureQualityName(q)) {
                                           t.defaultQuality[f] = q;
                                           return true;
                                       }
                                   }
                                   e = "\"" + name + "\" is not one of none, low, normal, high";
                                   return false;
                               }, {"none", "low", "normal", "high"});
                textureSetting("texture.default_max_size." + family, "int",
                               "Default Max Size of " + family + " cooks (a power of two, 32-16384).",
                               [f](const TextureProjectSettings& t) { return Json(t.defaultMaxSize[f]); },
                               [f](TextureProjectSettings& t, const Json& v, std::string& e) {
                                   long long size;
                                   if (!readInt(v, 32, 16384, size, e)) return false;
                                   if (size & (size - 1)) {
                                       e = "must be a power of two";
                                       return false;
                                   }
                                   t.defaultMaxSize[f] = (int)size;
                                   return true;
                               });
            }
            for (size_t g = 0; g < 3; g++) {
                const std::string group = texturePlatformGroupName(g);
                textureSetting("texture.texture_quality." + group, "int",
                               "Top mips " + group + " games drop from streamable textures: 0 full, 1 half, 2 quarter, 3 eighth.",
                               [g](const TextureProjectSettings& t) { return Json(t.textureQuality[g]); },
                               [g](TextureProjectSettings& t, const Json& v, std::string& e) {
                                   long long q;
                                   if (!readInt(v, 0, 3, q, e)) return false;
                                   t.textureQuality[g] = (int)q;
                                   return true;
                               });
                textureSetting("texture.memory_budget_mb." + group, "int",
                               "Resident texture MB of " + group + " games (0 = unlimited); over it the largest streamable textures drop mips.",
                               [g](const TextureProjectSettings& t) { return Json(t.memoryBudgetMB[g]); },
                               [g](TextureProjectSettings& t, const Json& v, std::string& e) {
                                   long long mb;
                                   if (!readInt(v, 0, 1 << 20, mb, e)) return false;
                                   t.memoryBudgetMB[g] = (int)mb;
                                   return true;
                               });
                textureSetting("texture.low_memory_fallback." + group, "bool",
                               "RGB565 / RGBA4444 instead of RGBA8 when a " + group + " GL device samples none of a texture's formats.",
                               [g](const TextureProjectSettings& t) { return Json((bool)t.lowMemoryFallback[g]); },
                               [g](TextureProjectSettings& t, const Json& v, std::string& e) {
                                   bool b;
                                   if (!readBool(v, b, e)) return false;
                                   t.lowMemoryFallback[g] = b;
                                   return true;
                               });
            }
            textureSetting("texture.cache_size_mb", "int", "MB the cooked texture cache keeps (least recently used first).",
                           [](const TextureProjectSettings& t) { return Json(t.cacheSizeMB); },
                           [](TextureProjectSettings& t, const Json& v, std::string& e) {
                               long long mb;
                               if (!readInt(v, 64, 1 << 20, mb, e)) return false;
                               t.cacheSizeMB = (int)mb;
                               return true;
                           });
            textureSetting("texture.global_cache_dir", "string", "A cooked texture cache every project shares (\"\" = the project's own).",
                           [](const TextureProjectSettings& t) { return Json(t.globalCacheDir); },
                           [](TextureProjectSettings& t, const Json& v, std::string& e) { return readString(v, t.globalCacheDir, e); });
        }
        // ---- mesh (Project Settings > Mesh, S53 R19) and the render budgets (Quality, R25)
        {
            auto meshSetting = [&](const std::string& key, const std::string& type, const std::string& description,
                                   std::function<Json(const ModelProjectSettings&)> get,
                                   std::function<bool(ModelProjectSettings&, const Json&, std::string&)> set,
                                   std::vector<std::string> values = {}) {
                Setting s;
                s.key = key;
                s.type = type;
                s.description = description;
                s.values = std::move(values);
                s.get = [get](SettingsContext& ctx) { return get(ctx.project()->getModelSettings()); };
                s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                    ModelProjectSettings next = ctx.project()->getModelSettings();
                    if (!set(next, value, error)) return false;
                    if (apply) ctx.project()->setModelSettings(next);
                    return true;
                };
                add(s);
            };
            for (size_t f = 0; f < kModelFamilyCount; f++) {
                const std::string family = modelFamilyName((ModelFamily)f);
                // each R8 setting's default for models that do not set it ("" = the built-in default)
                for (const std::string& metaKey : modelSettingKeys()) {
                    const std::vector<std::string>& choices = modelEnumValues(metaKey);
                    const bool isBool = metaKey == "weldVertices" || metaKey == "shipSource";
                    std::vector<std::string> values;
                    if (!choices.empty()) {
                        values = choices;
                        values.insert(values.begin(), "");
                    }
                    std::string snake;
                    for (char c : metaKey) snake += std::isupper((unsigned char)c) ? std::string("_") + (char)std::tolower((unsigned char)c) : std::string(1, c);
                    meshSetting("mesh.defaults." + family + "." + snake, !choices.empty() ? "enum" : isBool ? "bool" : metaKey == "lodCount" ? "int" : "float",
                                "Default " + snake + " of " + family + " cooks for models that do not set it.",
                                [f, metaKey](const ModelProjectSettings& m) {
                                    ModelProjectSettings one;
                                    one.defaults[f] = m.defaults[f];
                                    const YAML::Node encoded = encodeModelProjectSettings(one);
                                    const YAML::Node value = encoded["defaults"] ? encoded["defaults"][modelFamilyName((ModelFamily)f)][metaKey] : YAML::Node();
                                    if (!value) return Json(nullptr);
                                    const std::string text = value.Scalar();
                                    if (text == "true" || text == "false") return Json(text == "true");
                                    if (metaKey == "lodCount") return Json(std::atoi(text.c_str()));
                                    if (metaKey == "smoothingAngle") return Json(std::atof(text.c_str()));
                                    return Json(text);
                                },
                                [f, metaKey](ModelProjectSettings& m, const Json& v, std::string& e) {
                                    ModelProjectSettings one;
                                    one.defaults[f] = m.defaults[f];
                                    YAML::Node encoded = encodeModelProjectSettings(one);
                                    const std::string family = modelFamilyName((ModelFamily)f);
                                    if (!encoded["defaults"]) encoded["defaults"] = YAML::Node(YAML::NodeType::Map);
                                    if (!encoded["defaults"][family]) encoded["defaults"][family] = YAML::Node(YAML::NodeType::Map);
                                    YAML::Node block = encoded["defaults"][family];
                                    if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
                                        block.remove(metaKey);
                                    } else {
                                        YAML::Node value = v.is_boolean() ? YAML::Node(v.get<bool>()) : v.is_number_integer() ? YAML::Node(v.get<long long>())
                                                         : v.is_number() ? YAML::Node(v.get<double>()) : v.is_string() ? YAML::Node(v.get<std::string>()) : YAML::Node();
                                        if (!value || !validateModelSettingsField(metaKey, value, e)) {
                                            if (e.empty()) e = "expected a boolean, number or string";
                                            return false;
                                        }
                                        block[metaKey] = value;
                                    }
                                    ModelProjectSettings decoded;
                                    decodeModelProjectSettings(encoded, decoded);
                                    m.defaults[f] = decoded.defaults[f];
                                    return true;
                                }, values);
                }
                meshSetting("quality.render_budgets." + family + ".triangles", "int",
                            "Triangles a " + family + " scene may draw (check_render_budgets; 0 = no limit).",
                            [f](const ModelProjectSettings& m) { return Json(m.renderBudgets[f].triangles); },
                            [f](ModelProjectSettings& m, const Json& v, std::string& e) {
                                long long n;
                                if (!readInt(v, 0, 1LL << 40, n, e)) return false;
                                m.renderBudgets[f].triangles = n;
                                return true;
                            });
                meshSetting("quality.render_budgets." + family + ".draw_calls", "int",
                            "Draw calls a " + family + " scene may make (0 = no limit).",
                            [f](const ModelProjectSettings& m) { return Json(m.renderBudgets[f].drawCalls); },
                            [f](ModelProjectSettings& m, const Json& v, std::string& e) {
                                long long n;
                                if (!readInt(v, 0, 1LL << 32, n, e)) return false;
                                m.renderBudgets[f].drawCalls = n;
                                return true;
                            });
                meshSetting("quality.render_budgets." + family + ".mesh_memory_mb", "float",
                            "MB of mesh data a " + family + " scene's models may hold on the GPU (0 = no limit).",
                            [f](const ModelProjectSettings& m) { return Json(m.renderBudgets[f].meshMemoryMB); },
                            [f](ModelProjectSettings& m, const Json& v, std::string& e) {
                                double n;
                                if (!readNumber(v, 0.0, 1e9, n, e)) return false;
                                m.renderBudgets[f].meshMemoryMB = n;
                                return true;
                            });
                meshSetting("quality.render_budgets." + family + ".download_mb", "float",
                            "MB a " + family + " scene's cooked model files may download (0 = no limit).",
                            [f](const ModelProjectSettings& m) { return Json(m.renderBudgets[f].downloadMB); },
                            [f](ModelProjectSettings& m, const Json& v, std::string& e) {
                                double n;
                                if (!readNumber(v, 0.0, 1e9, n, e)) return false;
                                m.renderBudgets[f].downloadMB = n;
                                return true;
                            });
            }
            meshSetting("mesh.cache_size_mb", "int", "MB the cooked model cache keeps (least recently used first).",
                        [](const ModelProjectSettings& m) { return Json(m.cacheSizeMB); },
                        [](ModelProjectSettings& m, const Json& v, std::string& e) {
                            long long mb;
                            if (!readInt(v, 64, 1 << 20, mb, e)) return false;
                            m.cacheSizeMB = (int)mb;
                            return true;
                        });
            meshSetting("mesh.keep_cpu_data_for_scripts", "enum",
                        "Whether models keep their CPU mesh data for scripts: automatic (when a script calls a mesh-reading API), keep or release.",
                        [](const ModelProjectSettings& m) { return Json(scriptMeshDataName(m.scriptMeshData)); },
                        [](ModelProjectSettings& m, const Json& v, std::string& e) {
                            std::string name;
                            if (!readString(v, name, e)) return false;
                            for (ScriptMeshData d : {ScriptMeshData::Automatic, ScriptMeshData::Keep, ScriptMeshData::Release}) {
                                if (name == scriptMeshDataName(d)) {
                                    m.scriptMeshData = d;
                                    return true;
                                }
                            }
                            e = "\"" + name + "\" is not one of automatic, keep, release";
                            return false;
                        }, {"automatic", "keep", "release"});
            // Project Settings > Animation (S77 R38); stored with the model settings
            meshSetting("animation.animation_entities", "enum",
                        "Models placed in a scene: always make an Animation entity per clip, or auto: none for a model of 5+ clips "
                        "(new projects; play its clips with an Animation Player or Animator).",
                        [](const ModelProjectSettings& m) { return Json(animationEntitiesModeName(m.animationEntities)); },
                        [](ModelProjectSettings& m, const Json& v, std::string& e) {
                            std::string name;
                            if (!readString(v, name, e)) return false;
                            for (AnimationEntitiesMode mode : {AnimationEntitiesMode::Always, AnimationEntitiesMode::Auto}) {
                                if (name == animationEntitiesModeName(mode)) {
                                    m.animationEntities = mode;
                                    return true;
                                }
                            }
                            e = "\"" + name + "\" is not one of always, auto";
                            return false;
                        }, {"always", "auto"});
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
        // Project Settings > Graphics > Shader Globals (upstream's shader globals): the list, replaced
        // whole, one undo step of the project history (graphics.shader_globals.<name>: ProjectTools)
        {
            Setting s;
            s.key = "graphics.shader_globals";
            s.type = "list";
            s.description =
                "Project Settings > Graphics > Shader Globals: global shader values (Lua Shader.setGlobal*) with their defaults, "
                "read by shaders declaring u_vs_globals / u_fs_globals members (or a texture uniform) of the same name. Each "
                "{name, type: float|vector2|vector3|vector4|color|matrix4|float_array|vector4_array|texture2d|texturecube, "
                "array_size (arrays, 1-64), default}: color \"#RRGGBB[AA]\" or [r,g,b,a] sRGB, vectors [x,y,...], matrix4 16 "
                "numbers column-major, arrays N values, textures an asset path. Setting it replaces the list; "
                "graphics.shader_globals.<name> upserts one ({type, default, array_size, name: renames}; null removes). The "
                "read result adds glsl_vs / glsl_fs, the blocks to paste. Undoable. Details: jasper://docs/shaders#global-values.";
            s.get = [](SettingsContext& ctx) { return ShaderGlobalsJson::listToJson(ctx.project()->getShaderGlobals()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::vector<ShaderGlobalDecl> decls;
                if (!value.is_null() && !ShaderGlobalsJson::listFromJson(value, decls, error)) return false;
                if (!apply) return true;
                Project* p = ctx.project();
                if (decls == p->getShaderGlobals()) return true;
                auto* command = new ShaderGlobalsCmd(p, p->getShaderGlobals(), decls, {});
                command->setNoMerge();
                if (!p->getProjectCommandHistory()->addCommand(command)) {
                    error = "the shader globals could not be applied";
                    return false;
                }
                return true;
            };
            add(s);
        }
        // HTML UI rendering (S16): the glyph atlas budget, SDF primitives, transform-aware text
        {
            Setting s;
            s.key = "graphics.html_ui_glyph_atlas_mib";
            s.type = "int";
            s.description = "GPU memory of the HTML/CSS UI glyph atlas in MiB (two thirds bitmap glyphs, one third "
                            "distance-field glyphs); 0 = the platform's default (24 on desktop, 12 on Android, iOS and the web), "
                            "else 4-256. Pages not drawn lately are cleared when text needs more.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getHtmlUiRenderSettings().glyphAtlasMiB); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 0, Project::maxHtmlUiGlyphAtlasMiB, v, error)) return false;
                if (!Project::isValidHtmlUiGlyphAtlasMiB(static_cast<int>(v))) {
                    error = "must be 0 or 4-256";
                    return false;
                }
                if (apply) {
                    HtmlUiRenderSettings settings = ctx.project()->getHtmlUiRenderSettings();
                    settings.glyphAtlasMiB = static_cast<int>(v);
                    ctx.project()->setHtmlUiRenderSettings(settings);
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("graphics.html_ui_sdf_primitives",
                        "HTML/CSS UI: solid backgrounds, borders, outlines and box shadows drawn as antialiased shapes in batched "
                        "draws (no shadow textures or layers); false draws them as meshes.",
                        [](Project& p) { return p.getHtmlUiRenderSettings().sdfPrimitives; },
                        [](Project& p, bool b) {
                            HtmlUiRenderSettings settings = p.getHtmlUiRenderSettings();
                            settings.sdfPrimitives = b;
                            p.setHtmlUiRenderSettings(settings);
                        }));
        add(boolSetting("graphics.html_ui_transform_aware_text",
                        "HTML/CSS UI: text rasterized at its on-screen size under scale transforms and fractional pixel ratios, "
                        "drawn from distance fields while that size animates; false keeps a bitmap at the document's scale.",
                        [](Project& p) { return p.getHtmlUiRenderSettings().transformAwareText; },
                        [](Project& p, bool b) {
                            HtmlUiRenderSettings settings = p.getHtmlUiRenderSettings();
                            settings.transformAwareText = b;
                            p.setHtmlUiRenderSettings(settings);
                        }));
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
        add(boolSetting("graphics.dynamic_batching", "Consecutive Sprites, small meshes and UI elements that share a material draw as one draw call (Unity's Dynamic Batching). The same image; off draws each one alone, for comparison.",
                        [](Project& p) { return p.isDynamicBatching(); }, [](Project& p, bool v) { p.setDynamicBatching(v); }));
        add(boolSetting("graphics.runtime_shaders", "Builds carry the runtime shader compiler for variants not shipped precompiled (scripted meshes/materials). Off makes builds smaller and faster to compile.",
                        [](Project& p) { return p.isRuntimeShadersEnabled(); }, [](Project& p, bool v) { p.setRuntimeShadersEnabled(v); }));
        // GPU Instancing (S55): draws of one mesh, material and shader drawn as one
        {
            Setting s;
            s.key = "graphics.gpu_instancing";
            s.type = "bool";
            s.description = "GPU Instancing: opaque Mesh draws of the same mesh, material and shader are drawn as one instanced draw "
                            "(the image is the same), constant uniforms are applied once, and passes sort their draws by state. "
                            "A Mesh opts out with Allow GPU Instancing. Off submits every draw alone, as before. Undoable (undo scope project).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getDrawSubmissionSettings().gpuInstancing); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                return setDrawSubmission(ctx, [b](DrawSubmissionSettings& settings) { settings.gpuInstancing = b; }, apply, error);
            };
            add(s);
        }
        // Screen Transitions (S48 R41): what Lua's Transition and the Game view's preview take by default
        {
            using jasperengine::pixelart::TransitionDefaults;
            auto transitionSetting = [&](const std::string& key, const std::string& type, const std::string& description,
                                         std::function<Json(const TransitionDefaults&)> get,
                                         std::function<bool(TransitionDefaults&, const Json&, std::string&)> set,
                                         std::vector<std::string> values = {}) {
                Setting s;
                s.key = key;
                s.type = type;
                s.values = std::move(values);
                s.description = description + " Undoable (undo scope project).";
                s.get = [get](SettingsContext& ctx) { return get(ctx.project()->getTransitionDefaults()); };
                s.set = [set](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                    TransitionDefaults changed = ctx.project()->getTransitionDefaults();
                    if (!set(changed, value, error)) return false;
                    return setTransitionDefaults(ctx, [&changed](TransitionDefaults& settings) { settings = changed; }, apply);
                };
                add(s);
            };
            std::vector<std::string> kinds;
            for (int k = 0; k < jasperengine::pixelart::kTransitionKindCount; k++)
                kinds.push_back(jasperengine::pixelart::transitionKindName((jasperengine::pixelart::TransitionKind)k));
            transitionSetting("graphics.screen_transitions.kind", "enum", "The screen transition Transition.play and the Game view use when none is named.",
                [](const TransitionDefaults& t) { return Json(jasperengine::pixelart::transitionKindName(t.kind)); },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    if (!v.is_string() || !jasperengine::pixelart::transitionKindFromName(v.get<std::string>(), t.kind)) {
                        e = "must be one of iris, blinds, slash, pixel_dissolve, crt_off, burn_away, fade";
                        return false;
                    }
                    return true;
                }, kinds);
            transitionSetting("graphics.screen_transitions.duration", "number", "Seconds of a whole transition (cover, hold, reveal) when none is given.",
                [](const TransitionDefaults& t) { return Json(t.duration); },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    double d;
                    if (!readNumber(v, 0.0, 30.0, d, e)) return false;
                    t.duration = (float)d;
                    return true;
                });
            transitionSetting("graphics.screen_transitions.color", "string", "The ink the screen is covered in, \"#rrggbb\" (sRGB).",
                [](const TransitionDefaults& t) {
                    auto byte = [](float c) { return (uint8_t)std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f); };
                    return Json(jasperengine::pixelart::toHex(jasperengine::pixelart::Rgba8{byte(t.color.x), byte(t.color.y), byte(t.color.z), 255}, false));
                },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    jasperengine::pixelart::Rgba8 c;
                    if (!v.is_string() || !jasperengine::pixelart::parseHex(v.get<std::string>(), c)) {
                        e = "must be \"#rrggbb\"";
                        return false;
                    }
                    t.color = Vector4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
                    return true;
                });
            transitionSetting("graphics.screen_transitions.ramp", "string",
                "The rims' ramp, \"<palette>.palette#ramp:<name>\" (\"\": the built-in ember ramp).",
                [](const TransitionDefaults& t) { return Json(t.ramp); },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    std::string ramp;
                    if (!readString(v, ramp, e, 512)) return false;
                    if (!ramp.empty() && !jasperengine::pixelart::parsePaletteRampPath(ramp)) {
                        e = "must be \"<palette>.palette#ramp:<name>\" or \"\"";
                        return false;
                    }
                    t.ramp = ramp;
                    return true;
                });
            transitionSetting("graphics.screen_transitions.rim", "int", "Rim width in art pixels (0..8).",
                [](const TransitionDefaults& t) { return Json(t.rim); },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    long long n;
                    if (!readInt(v, 0, 8, n, e)) return false;
                    t.rim = (int)n;
                    return true;
                });
            transitionSetting("graphics.screen_transitions.pixel_size", "int",
                "Screen pixels per art pixel (0: the Pixel Perfect Camera's grid, else 1 without one).",
                [](const TransitionDefaults& t) { return Json(t.pixelSize); },
                [](TransitionDefaults& t, const Json& v, std::string& e) {
                    long long n;
                    if (!readInt(v, 0, 64, n, e)) return false;
                    t.pixelSize = (int)n;
                    return true;
                });
            transitionSetting("graphics.screen_transitions.cover_ui", "bool",
                "On: transitions cover every scene (the HUD too). Off: they draw after the main scene, under its layer scenes.",
                [](const TransitionDefaults& t) { return Json(t.coverUI); },
                [](TransitionDefaults& t, const Json& v, std::string& e) { return readBool(v, t.coverUI, e); });
        }
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
        // the foliage distance scale per platform group (Engine::setFoliageDistanceScaleDefaults)
        for (int group = 0; group < 3; group++) {
            static const char* const keys[3] = {"quality.foliage_distance_desktop", "quality.foliage_distance_mobile", "quality.foliage_distance_web"};
            static const char* const groups[3] = {"Windows, macOS and Linux games and the editor's views", "Android and iOS games", "HTML5 games"};
            Setting s;
            s.key = keys[group];
            s.type = "number";
            s.description = std::string("Foliage Distance (0-10) of ") + groups[group] +
                            ": multiplies every terrain foliage layer's Draw Distance and Min Draw Distance; 0 draws none (Lua: Engine.foliageDistanceScale).";
            s.get = [group](SettingsContext& ctx) { return Json(ctx.project()->getFoliageDistanceScale()[group]); };
            s.set = [group](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 10.0, v, error)) return false;
                if (apply) {
                    std::array<float, 3> all = ctx.project()->getFoliageDistanceScale();
                    all[group] = static_cast<float>(v);
                    ctx.project()->setFoliageDistanceScale(all);
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

        // ---- player crash handling (docs/crash-diagnostics.md)
        auto crashBool = [&add](const std::string& key, const std::string& description, bool PlayerCrashSettings::* member) {
            add(boolSetting(key, description, [member](Project& p) { return p.getPlayerCrash().*member; },
                            [member](Project& p, bool v) { PlayerCrashSettings c = p.getPlayerCrash(); c.*member = v; p.setPlayerCrash(c); }));
        };
        crashBool("player.crash.enabled", "The built game writes crash reports (<user data>/crash/reports); none are sent.",
                  &PlayerCrashSettings::enabled);
        crashBool("player.crash.log_tail", "Crash reports keep the last log lines (log-tail.txt).", &PlayerCrashSettings::includeLogTail);
        crashBool("player.crash.dump_with_heap", "Windows minidumps also hold the memory the stacks point to (else Small).",
                  &PlayerCrashSettings::dumpWithHeap);
        crashBool("player.crash.hang_watchdog", "Development Builds report a main thread that stops responding.",
                  &PlayerCrashSettings::hangWatchdog);
        crashBool("player.crash.web_overlay", "Web: show \"The game stopped working. [Reload]\" after a crash.",
                  &PlayerCrashSettings::webCrashOverlay);
        {
            Setting s;
            s.key = "player.crash.hang_seconds";
            s.type = "int";
            s.description = "The development build's hang threshold, seconds.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getPlayerCrash().hangSeconds); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, PlayerCrashSettings::minHangSeconds, PlayerCrashSettings::maxHangSeconds, v, error)) return false;
                if (apply) {
                    PlayerCrashSettings crash = ctx.project()->getPlayerCrash();
                    crash.hangSeconds = static_cast<int>(v);
                    ctx.project()->setPlayerCrash(crash);
                }
                return true;
            };
            add(std::move(s));
        }

        // ---- particles (S20 R15; Project Settings > Graphics)
        {
            Setting s;
            s.key = "particles.world_budget";
            s.type = "int";
            s.description = "Particles alive at once in a scene, sub-emitter children included (0: the platform's default); over it, lower priorities emit less. Applies at once.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getParticleWorldBudget()); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 0, Project::maxParticleWorldBudget, v, error)) return false;
                if (apply) ctx.project()->setParticleWorldBudget(static_cast<uint32_t>(v));
                return true;
            };
            add(s);
        }
        // the thresholds of the particle checks (S91 R28)
        for (const ParticleQaField& field : particleQaFields()) {
            Setting s;
            s.key = field.key;
            s.type = field.real ? "number" : "int";
            s.description = field.help;
            s.get = [&field](SettingsContext& ctx) {
                const ParticleQaSettings& qa = ctx.project()->getParticleQaSettings();
                return field.real ? Json(qa.*field.real) : Json(qa.*field.whole);
            };
            s.set = [&field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_number() || value.get<double>() < field.minimum || value.get<double>() > field.maximum ||
                    (field.whole && !value.is_number_integer())) {
                    error = "must be a number from " + std::to_string(field.minimum) + " to " + std::to_string(field.maximum);
                    return false;
                }
                if (apply) {
                    ParticleQaSettings settings = ctx.project()->getParticleQaSettings();
                    if (field.real) settings.*field.real = value.get<float>();
                    else settings.*field.whole = value.get<int>();
                    ctx.project()->setParticleQaSettings(settings);
                }
                return true;
            };
            add(std::move(s));
        }
        // the thresholds of the material checks (S90 R36)
        for (const procmat::ProcMatQaField& field : procmat::procMatQaFields()) {
            Setting s;
            s.key = field.key;
            s.type = field.real ? "number" : "int";
            s.description = field.help;
            s.get = [&field](SettingsContext& ctx) {
                const procmat::ProcMatQaSettings& qa = ctx.project()->getProcMatQaSettings();
                return field.real ? Json(qa.*field.real) : Json(qa.*field.whole);
            };
            s.set = [&field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_number() || value.get<double>() < field.minimum || value.get<double>() > field.maximum ||
                    (field.whole && !value.is_number_integer())) {
                    error = "must be a number from " + std::to_string(field.minimum) + " to " + std::to_string(field.maximum);
                    return false;
                }
                if (apply) {
                    procmat::ProcMatQaSettings settings = ctx.project()->getProcMatQaSettings();
                    if (field.real) settings.*field.real = value.get<float>();
                    else settings.*field.whole = value.get<int>();
                    ctx.project()->setProcMatQaSettings(settings);
                }
                return true;
            };
            add(std::move(s));
        }
        // ---- physics
        add(boolSetting("physics.enabled", "3D physics (Jolt). Off leaves Rigidbodies and joints out of builds.",
                        [](Project& p) { return p.isPhysics3DEnabled(); }, [](Project& p, bool v) { p.setPhysics3DEnabled(v); }));
        add(layerCollisionSetting("physics.layer_collision_matrix", true));
        // the 3D world capacity (worlds made from now on: the next Play, exported builds): upstream's
        // physics-3d keys (Project Settings > Physics > 3D World Capacity)
        struct CapacityKey { const char* key; const char* description; unsigned int Physics3DWorldSettings::* field; };
        static const CapacityKey capacityKeys[] = {
            {"physics.max_bodies", "Bodies a 3D world holds (default 8192); past it new bodies are refused with an error naming this setting.", &Physics3DWorldSettings::maxBodies},
            {"physics.max_body_pairs", "Touching body pairs a 3D world tracks (default 8192); past it contacts are dropped and reported once.", &Physics3DWorldSettings::maxBodyPairs},
            {"physics.max_contact_constraints", "Contact constraints of a 3D world (default 8192); past it contacts are dropped and reported once.", &Physics3DWorldSettings::maxContactConstraints}};
        for (const CapacityKey& capacity : capacityKeys) {
            Setting s;
            s.key = capacity.key;
            s.type = "int";
            s.description = capacity.description;
            unsigned int Physics3DWorldSettings::* field = capacity.field;
            s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getPhysics3DWorldSettings().*field); };
            s.set = [field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 16, 8388607, v, error)) return false;
                if (apply) {
                    Physics3DWorldSettings world = ctx.project()->getPhysics3DWorldSettings();
                    world.*field = static_cast<unsigned int>(v);
                    ctx.project()->setPhysics3DWorldSettings(world);
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("physics2d.enabled", "2D physics (Box2D). Off leaves Rigidbody2Ds and joints out of builds.",
                        [](Project& p) { return p.isPhysics2DEnabled(); }, [](Project& p, bool v) { p.setPhysics2DEnabled(v); }));
        add(layerCollisionSetting("physics2d.layer_collision_matrix", false));
        // ---- navigation (S19; agent types and areas: Window > AI > Navigation, set_navigation_settings)
        add(boolSetting("navigation.include_in_builds", "Recast/Detour in exported builds. Off: navmesh bakes, queries and agents fail; grids still work.",
                        [](Project& p) { return p.getNavigationSettings().includeInBuilds; },
                        [](Project& p, bool v) { NavigationProjectSettings n = p.getNavigationSettings(); n.includeInBuilds = v; p.setNavigationSettings(n); }));
        {
            struct NavCount {
                const char* key;
                const char* description;
                int minimum;
                int maximum;
                int nav::NavigationSettings::*field;
            };
            const NavCount counts[] = {
                {"navigation.crowd_max_agents", "Agents per crowd (one per surface and agent type) before it grows, with a warning (1-4096).", 1, 4096, &nav::NavigationSettings::crowdMaxAgents},
                {"navigation.max_grid_solves_per_tick", "Grid path requests solved per fixed tick; the rest wait (1-4096).", 1, 4096, &nav::NavigationSettings::maxGridSolvesPerTick},
                {"navigation.carve_tiles_per_tick", "Carved tiles each NavMesh Surface rebuilds per fixed tick; 0: Auto (4 on desktop, 2 on Android, iOS and web) (0-64).", 0, 64, &nav::NavigationSettings::carveTilesPerTick},
            };
            for (const NavCount& count : counts) {
                Setting s;
                s.key = count.key;
                s.type = "int";
                s.description = count.description;
                const auto field = count.field;
                const int minimum = count.minimum;
                const int maximum = count.maximum;
                s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getNavigationSettings().settings.*field); };
                s.set = [field, minimum, maximum](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                    long long v;
                    if (!readInt(value, minimum, maximum, v, error)) return false;
                    if (apply) {
                        NavigationProjectSettings n = ctx.project()->getNavigationSettings();
                        n.settings.*field = static_cast<int>(v);
                        ctx.project()->setNavigationSettings(n);
                    }
                    return true;
                };
                add(s);
            }
        }
        {
            Setting s;
            s.key = "navigation.web_bake_budget_ms";
            s.type = "number";
            s.description = "Main-thread bake time per frame without worker threads (web), ms (0.5-50).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getNavigationSettings().settings.webBakeBudgetMs); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.5, 50.0, v, error)) return false;
                if (apply) {
                    NavigationProjectSettings n = ctx.project()->getNavigationSettings();
                    n.settings.webBakeBudgetMs = static_cast<float>(v);
                    ctx.project()->setNavigationSettings(n);
                }
                return true;
            };
            add(s);
        }

        // ---- the Default Palette (S43 R4b): Snap to Palette, apply_palette without a palette, the
        // pixel-art tools' new art
        add(pathSetting("palettes.default", "The Default Palette: the .palette used where none is named (apply_palette, Snap to "
                        "Palette, new pixel art).", [](Project& p) -> fs::path& { return p.getDefaultPalette(); }));

        // ---- input (S22, and Unity's Input Manager absorbed from the older Project Settings > Input)
        {
            // the asset path; an older editor's list of Input Manager actions (or "defaults") set
            // here goes into the asset's Input Manager map, as input.manager_actions does
            Setting s = pathSetting("input.actions", "The Input Actions asset Play and the game load at start (an .inputactions file in "
                                    "the assets directory). Also takes a list of Input Manager actions or \"defaults\" (input.manager_actions).",
                                    [](Project& p) -> fs::path& { return p.getInputActionsAsset(); });
            auto setPath = s.set;
            s.set = [setPath](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (value.is_array() || (value.is_string() && value.get<std::string>() == "defaults"))
                    return setInputManagerActions(ctx, value, apply, error);
                return setPath(ctx, value, apply, error);
            };
            add(std::move(s));
        }
        {
            Setting s;
            s.key = "input.manager_actions";
            s.type = "list";
            s.description =
                "Unity's Input Manager: the actions asset's \"Input Manager\" map (Unity's 11 defaults without an asset) as virtual "
                "axes and buttons, read by Input.getAxis / getButton / getButtonDown. Each {name, type: axis|button, description?, "
                "press_point? (0.5), bindings: [{kind: buttons, positive: [control names], negative: [...], gravity, sensitivity, "
                "dead, snap, invert, gamepad: any|0-15} | {kind: gamepad_axis, axis: left_x..right_trigger, gamepad, dead, "
                "sensitivity, invert} | {kind: mouse_delta, axis: x|y|wheel, sensitivity, invert}]}. Control names: a, space, left, "
                "left_shift, kp_0, f1, mouse_left, gamepad_a, gamepad_dpad_up... (Unity spellings accepted). Setting it replaces the "
                "whole list (get it first to add one) and makes InputManager.inputactions when the project has no asset; "
                "\"defaults\" restores Unity's 11. Undoable (undo scope project). Details: jasper://docs/input.";
            s.get = [](SettingsContext& ctx) {
                return Json::parse(InputManagerFormat::actionsToJson(ctx.project()->getInputManagerActions()))["actions"];
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                return setInputManagerActions(ctx, value, apply, error);
            };
            add(std::move(s));
        }
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

        // ---- audio (Project Settings > Audio)
        {
            Setting s;
            s.key = "audio.mixer";
            s.type = "path";
            s.description = "The project's Audio Mixer (a .mixer in the assets; \"\" = none: an implicit Master). audio_mixer "
                            "edit creates audio/Main.mixer there when unset. Guide: jasper://docs/audio.";
            s.get = [](SettingsContext& ctx) {
                Project* p = ctx.project();
                return Json(p->getAudioSettings().mixer.empty() ? std::string() : mcp::w4::projectRelative(p, p->getAudioMixerFile()));
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_string()) {
                    error = "must be a .mixer path (\"audio/Main.mixer\") or \"\"";
                    return false;
                }
                std::string text = value.get<std::string>();
                Project* p = ctx.project();
                std::string relative;
                if (!text.empty()) {
                    const fs::path asProject = p->getProjectPath() / FileUtils::pathFromUtf8(text);
                    std::error_code ec;
                    fs::path absolute = fs::exists(asProject, ec) || text.rfind("assets/", 0) == 0 ? asProject : p->resolveAssetPath(FileUtils::pathFromUtf8(text));
                    if (!p->isInsideAssetsPath(absolute)) {
                        error = "must be inside the assets directory";
                        return false;
                    }
                    relative = FileUtils::pathToGenericUtf8(p->normalizeToAssetsRelative(absolute));
                    if (FileUtils::pathFromUtf8(relative).extension() != ".mixer") {
                        error = "must be a .mixer file";
                        return false;
                    }
                }
                if (apply) {
                    AudioSettings settings = p->getAudioSettings();
                    settings.mixer = relative;
                    p->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        }
        auto audioInt = [&add](const char* key, const char* description, long long minimum, long long maximum, int AudioSettings::*field) {
            Setting s;
            s.key = key;
            s.type = "int";
            s.description = description;
            s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().*field); };
            s.set = [field, minimum, maximum](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, minimum, maximum, v, error)) return false;
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.*field = static_cast<int>(v);
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        };
        audioInt("audio.max_real_voices", "Voices mixed at once (default 32); the quietest others are virtualised. At most 255 minus "
                 "the mixer's groups.", 1, 255, &AudioSettings::maxRealVoices);
        {
            // SoLoud mixes at most 255 voices at once, the mixer's buses among them
            Setting& s = t.back();
            auto inner = s.set;
            s.set = [inner](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                const long long groups = (long long)AudioMixer::getGroupNames().size();
                const long long limit = (long long)AudioMixer::maxActiveVoices - groups;
                if (value.is_number_integer() && value.get<long long>() > limit) {
                    error = "must be at most " + std::to_string(limit) + " (255 voices at once, the mixer's " +
                            std::to_string(groups) + " group(s) included)";
                    return false;
                }
                return inner(ctx, value, apply, error);
            };
        }
        audioInt("audio.max_voices", "Voices playing at all (default 256); past it the least important (AudioSource priority) stops.", 1, 1000, &AudioSettings::maxVoices);
        audioInt("audio.seed", "The seed of Sound Event picks in a run without a device (default 1), so it repeats. A run with a device draws its own.", 0, 2147483647LL, &AudioSettings::seed);
        audioInt("audio.capture_max_seconds", "Seconds of audio a Play's capture keeps (default 120); its events go on being logged after them.", 1, 3600, &AudioSettings::captureMaxSeconds);
        audioInt("audio.capture_keep", "Capture sessions kept in the project (default 3); the oldest goes first.", 1, 20, &AudioSettings::captureKeep);
        for (const AudioQaField& field : audioQaFields()) {
            Setting s;
            s.key = field.key;
            s.type = field.real ? "number" : "int";
            s.description = field.help;
            s.get = [&field](SettingsContext& ctx) {
                const AudioQaSettings& qa = ctx.project()->getAudioSettings().qa;
                return field.real ? Json(qa.*field.real) : Json(qa.*field.whole);
            };
            s.set = [&field](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_number() || value.get<double>() < field.minimum || value.get<double>() > field.maximum ||
                    (field.whole && !value.is_number_integer())) {
                    error = "must be a number from " + std::to_string(field.minimum) + " to " + std::to_string(field.maximum);
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    if (field.real) settings.qa.*field.real = value.get<float>();
                    else settings.qa.*field.whole = value.get<int>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(std::move(s));
        }
        {
            Setting s;
            s.key = "audio.qa.music_group";
            s.type = "string";
            s.description = "The mixer group of the game's music, for the silence check.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().qa.musicGroup); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_string()) {
                    error = "must be a group name";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.qa.musicGroup = value.get<std::string>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(std::move(s));
        }
        {
            Setting s;
            s.key = "audio.capture_stems";
            s.type = "bool";
            s.description = "A capture also writes each mixer group's audio to its own WAV file.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().captureStems); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_boolean()) {
                    error = "must be true or false";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.captureStems = value.get<bool>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(std::move(s));
        }
        {
            Setting s;
            s.key = "audio.sample_rate";
            s.type = "enum";
            s.values = {"auto", "22050", "44100", "48000"};
            s.description = "Output sample rate (auto = the device's).";
            s.get = [](SettingsContext& ctx) {
                const int rate = ctx.project()->getAudioSettings().sampleRate;
                return Json(rate == 0 ? std::string("auto") : std::to_string(rate));
            };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                int rate = -1;
                if (value.is_string()) {
                    const std::string text = value.get<std::string>();
                    if (text == "auto") rate = 0;
                    else if (text == "22050" || text == "44100" || text == "48000") rate = std::stoi(text);
                } else if (value.is_number_integer()) {
                    const int v = value.get<int>();
                    if (v == 0 || v == 22050 || v == 44100 || v == 48000) rate = v;
                }
                if (rate < 0) {
                    error = "must be one of: auto 22050 44100 48000";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.sampleRate = rate;
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "audio.default_output_group";
            s.type = "string";
            s.description = "The mixer group new Audio Sources play through (\"\" = Master).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().defaultOutputGroup); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 64)) return false;
                if (!text.empty() && !AudioMixer::hasGroup(text)) {
                    error = "no mixer group '" + text + "' (audio_mixer get lists them)";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.defaultOutputGroup = text;
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        }

        {
            Setting s;
            s.key = "audio.music_group";
            s.type = "string";
            s.description = "The mixer group music plays through when neither Audio.playMusic nor its Sound Event names one (\"\" = Master).";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().musicGroup); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 64)) return false;
                if (!text.empty() && !AudioMixer::hasGroup(text)) {
                    error = "no mixer group '" + text + "' (audio_mixer get lists them)";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.musicGroup = text;
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        }

        {
            Setting s;
            s.key = "audio.credits_file";
            s.type = "bool";
            s.description = "Exports write licenses/THIRD-PARTY-AUDIO.txt, the licences of the shipped clips with a provenance.";
            s.get = [](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().creditsFile); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_boolean()) {
                    error = "true or false";
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.creditsFile = value.get<bool>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        }

        auto audioNumber = [&add](const char* key, const char* description, float minimum, float maximum, float AudioSettings::*field) {
            Setting s;
            s.key = key;
            s.type = "number";
            s.description = description;
            s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().*field); };
            s.set = [field, minimum, maximum](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_number() || value.get<double>() < minimum || value.get<double>() > maximum) {
                    char text[64];
                    snprintf(text, sizeof(text), "must be a number from %g to %g", minimum, maximum);
                    error = text;
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.*field = value.get<float>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        };
        audioNumber("audio.loudness_target_lufs", "Loudness target in LUFS (default -18). Measurement only: it changes no sound.", -40.0f, -6.0f, &AudioSettings::loudnessTargetLufs);
        audioNumber("audio.loudness_tolerance_lu", "How far from the target still passes, in LU (default 2). Measurement only.", 0.5f, 10.0f, &AudioSettings::loudnessToleranceLu);
        audioNumber("audio.true_peak_ceiling_dbtp", "True-peak ceiling the checks read against, in dBTP (default -1). Measurement only: a Master limiter's threshold is what holds it.", -12.0f, 0.0f, &AudioSettings::truePeakCeilingDbtp);
        auto audioEnum = [&add](const char* key, const char* description, std::vector<std::string> names, std::string AudioSettings::*field) {
            Setting s;
            s.key = key;
            s.type = "enum";
            s.description = description;
            s.values = names;
            s.get = [field](SettingsContext& ctx) { return Json(ctx.project()->getAudioSettings().*field); };
            s.set = [field, names](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_string() || std::find(names.begin(), names.end(), value.get<std::string>()) == names.end()) {
                    error = "must be one of:";
                    for (const std::string& name : names) error += " " + name;
                    return false;
                }
                if (apply) {
                    AudioSettings settings = ctx.project()->getAudioSettings();
                    settings.*field = value.get<std::string>();
                    ctx.project()->setAudioSettings(settings);
                }
                return true;
            };
            add(s);
        };
        audioEnum("audio.output_stage", "What the output does to the mix: roundoff (a soft clipper, -1.66 dB) or linear (unchanged, hard clip at 0 dBFS).",
                  {"roundoff", "linear"}, &AudioSettings::outputStage);
        audioEnum("audio.ios_session_category", "The iOS audio session: default (miniaudio's: ignores the silent switch, stops the player's music), ambient, solo_ambient, playback.",
                  {"default", "ambient", "solo_ambient", "playback"}, &AudioSettings::iosSessionCategory);

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
                // a plain project-relative folder: no absolute path, no ".." (even one that comes
                // back in), no hidden or editor-owned folder (upstream's round 9 checks)
                if (const std::string why = mcp::McpRefs::rootValueRefusal(text); !why.empty()) {
                    error = why + ": an asset root is a folder of the project, given project-relative without '..'";
                    return false;
                }
                fs::path dir = text.empty() ? fs::path(".") : FileUtils::pathFromUtf8(text).lexically_normal();
                if (dir != ".") {
                    fs::path absolute;
                    // the referenced files are moved there, and it becomes a trusted root: inside the
                    // project directory itself once links are resolved (never via the current
                    // assets directory's link)
                    if (!mcp::McpRefs::resolveProjectPath(p, text, absolute, error)) return false;
                    if (const std::string escape = mcp::McpRefs::projectOnlyEscape(p, absolute); !escape.empty()) {
                        error = escape;
                        return false;
                    }
                }
                if (ctx.isPlaying()) {
                    error = "cannot change asset roots during Play";
                    return false;
                }
                const fs::path newAssets = lua ? p->getAssetsDir() : dir;
                const fs::path newLua = lua ? dir : p->getLuaDir();
                // the preflight while validating (nothing is changed when it refuses), the move when applying;
                // an agent's unsaved Code Editor buffers of moving files refuse instead of being saved
                const AssetRootChange change = apply ? p->changeAssetRoots(newAssets, newLua, true)
                                                     : p->planAssetRootChange(newAssets, newLua, true);
                if (ctx.report && (change.changed || !change.moves.empty() || !change.ok())) {
                    // one call moving both roots reports both relocations, in one
                    Json relocation = assetRootChangeJson(p, change);
                    Json& reported = (*ctx.report)["relocation"];
                    if (reported.is_object()) {
                        for (auto it = relocation.begin(); it != relocation.end(); ++it) {
                            if (it.value().is_array() && reported.contains(it.key()) && reported[it.key()].is_array()) {
                                for (const Json& item : it.value()) reported[it.key()].push_back(item);
                            } else if (!reported.contains(it.key())) {
                                reported[it.key()] = it.value();
                            }
                        }
                    } else {
                        reported = std::move(relocation);
                    }
                }
                if (!change.ok()) {
                    error = "asset directories not changed: " + change.refusal();
                    return false;
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("editor.version_control_files", "Write version-control metadata files (.gitignore etc.) with the project.",
                        [](Project& p) { return p.hasVersionControlMetadata(); }, [](Project& p, bool v) { p.setVersionControlMetadata(v); }));
        // ---- version control (S29): the project's choice, project.yaml versionControl
        {
            Setting s;
            s.key = "editor.vcs.provider";
            s.type = "enum";
            s.description = "Version control: auto finds the nearest .git or .svn above the project; none turns it off.";
            s.values = {"auto", "git", "svn", "none"};
            s.get = [](SettingsContext& ctx) { return Json(vcs::providerChoiceName(ctx.project()->getVcsProjectSettings().provider)); };
            s.set = [](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                vcs::ProviderChoice choice;
                if (!value.is_string() || !vcs::parseProviderChoice(value.get<std::string>(), choice)) {
                    error = "must be one of: auto git svn none";
                    return false;
                }
                if (apply) {
                    VcsProjectSettings settings = ctx.project()->getVcsProjectSettings();
                    settings.provider = choice;
                    ctx.project()->setVcsProjectSettings(settings);
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("editor.vcs.lfs", "Version control: the generated .gitattributes stores binaries in Git LFS and makes them lockable.",
                        [](Project& p) { return p.getVcsProjectSettings().lfs; },
                        [](Project& p, bool v) { VcsProjectSettings settings = p.getVcsProjectSettings(); settings.lfs = v; p.setVcsProjectSettings(settings); }));
        add(boolSetting("editor.vcs.lock_scenes", "Version control: scenes and prefabs lockable too (a lock on a non-LFS file depends on the server).",
                        [](Project& p) { return p.getVcsProjectSettings().lockScenes; },
                        [](Project& p, bool v) { VcsProjectSettings settings = p.getVcsProjectSettings(); settings.lockScenes = v; p.setVcsProjectSettings(settings); }));
        // ---- version control: this user's (settings.yaml version_control)
        auto vcsPreference = [&add](Setting s, std::function<bool(VersionControlSettings&, const Json&, std::string&)> apply) {
            s.preference = true;
            s.set = [apply](SettingsContext& ctx, const Json& value, bool commit, std::string& error) {
                VersionControlSettings settings = AppSettings::getVersionControlSettings();
                if (!apply(settings, value, error)) return false;
                if (commit) {
                    AppSettings::setVersionControlSettings(settings);
                    if (Project* project = ctx.project()) project->getVcs().applySettings(project->versionControlServiceSettings());
                }
                return true;
            };
            add(std::move(s));
        };
        auto vcsPath = [&vcsPreference](const char* key, const char* tool, std::string VersionControlSettings::*field) {
            Setting s;
            s.key = key;
            s.type = "string";
            s.description = std::string("Version control: the absolute path of ") + tool + " (\"\" finds it on PATH; the project folder is never searched).";
            s.get = [field](SettingsContext&) { return Json(AppSettings::getVersionControlSettings().*field); };
            vcsPreference(s, [field](VersionControlSettings& settings, const Json& value, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (!text.empty() && !FileUtils::pathFromUtf8(text).is_absolute()) {
                    error = "must be an absolute path, or \"\"";
                    return false;
                }
                settings.*field = text;
                return true;
            });
        };
        vcsPath("editor.vcs.git_path", "git", &VersionControlSettings::gitPath);
        vcsPath("editor.vcs.svn_path", "svn", &VersionControlSettings::svnPath);
        {
            Setting s;
            s.key = "editor.vcs.lock_on_edit";
            s.type = "enum";
            s.description = "Version control: the first edit of a lockable file off, asks to lock it, or locks it (auto).";
            s.values = {"off", "ask", "auto"};
            s.get = [](SettingsContext&) {
                const LockOnEdit mode = AppSettings::getVersionControlSettings().lockOnEdit;
                return Json(mode == LockOnEdit::Off ? "off" : mode == LockOnEdit::Auto ? "auto" : "ask");
            };
            vcsPreference(s, [](VersionControlSettings& settings, const Json& value, std::string& error) {
                const std::string text = value.is_string() ? value.get<std::string>() : std::string();
                if (text != "off" && text != "ask" && text != "auto") {
                    error = "must be one of: off ask auto";
                    return false;
                }
                settings.lockOnEdit = text == "off" ? LockOnEdit::Off : text == "auto" ? LockOnEdit::Auto : LockOnEdit::Ask;
                return true;
            });
        }
        auto vcsInt = [&vcsPreference](const char* key, const char* description, long long minimum, long long maximum, bool zeroOff,
                                       int VersionControlSettings::*field) {
            Setting s;
            s.key = key;
            s.type = "int";
            s.description = description;
            s.get = [field](SettingsContext&) { return Json(AppSettings::getVersionControlSettings().*field); };
            vcsPreference(s, [field, minimum, maximum, zeroOff](VersionControlSettings& settings, const Json& value, std::string& error) {
                long long v = 0;
                if (zeroOff && value.is_number_integer() && value.get<long long>() == 0) {
                    settings.*field = 0;
                    return true;
                }
                if (!readInt(value, minimum, maximum, v, error)) return false;
                settings.*field = static_cast<int>(v);
                return true;
            });
        };
        vcsInt("editor.vcs.poll_seconds", "Version control: seconds between status polls (2-120).", 2, 120, false, &VersionControlSettings::pollSeconds);
        vcsInt("editor.vcs.lock_poll_seconds", "Version control: seconds between lock and server reads (30-600, 0 off).", 30, 600, true,
               &VersionControlSettings::lockPollSeconds);
        vcsInt("editor.vcs.auto_fetch_minutes", "Version control: minutes between automatic fetches (0 off).", 1, 24 * 60, true,
               &VersionControlSettings::autoFetchMinutes);
        {
            Setting s;
            s.key = "editor.vcs.commit_and_push";
            s.type = "bool";
            s.description = "Version control: the Commit window's Commit and Push.";
            s.get = [](SettingsContext&) { return Json(AppSettings::getVersionControlSettings().commitAndPush); };
            vcsPreference(s, [](VersionControlSettings& settings, const Json& value, std::string& error) {
                return readBool(value, settings.commitAndPush, error);
            });
        }
        {
            Setting s;
            s.key = "editor.vcs.code_editor_changes";
            s.type = "bool";
            s.description = "Version control: the code editor marks the lines changed since the last commit.";
            s.get = [](SettingsContext&) { return Json(AppSettings::getVersionControlSettings().codeEditorChanges); };
            vcsPreference(s, [](VersionControlSettings& settings, const Json& value, std::string& error) {
                return readBool(value, settings.codeEditorChanges, error);
            });
        }

        // ---- debug: the on-device console and stats overlay, the log (jasper://docs/debugging),
        // from the reflection table
        addDebugSettings(add);

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
            // S43 R8: no network request of the editor's own (Lospec says it is offline at once)
            Setting s;
            s.key = "preferences.network.offline";
            s.type = "bool";
            s.preference = true;
            s.description = "General: Offline, no network access from the editor (Import > From Lospec reports network_unavailable at once).";
            s.get = [](SettingsContext&) { return Json(AppSettings::isNetworkOffline()); };
            s.set = [](SettingsContext&, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                if (apply) {
                    AppSettings::setNetworkOffline(b);
                    AppSettings::saveSettings();
                }
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
            s.userOnly = true;
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
        for (const auto& [key, description, getter, setter] : std::initializer_list<std::tuple<const char*, const char*, bool (*)(), void (*)(bool)>>{
                 {"preferences.sound_browser_autoplay", "Sound Browser: selecting a sound plays it.", &AppSettings::getSoundBrowserAutoplay, &AppSettings::setSoundBrowserAutoplay},
                 {"preferences.sound_browser_match_loudness", "Sound Browser: auditions play at -20 LUFS-M, so sounds compare by character.", &AppSettings::getSoundBrowserMatchLoudness, &AppSettings::setSoundBrowserMatchLoudness}}) {
            Setting s;
            s.key = key;
            s.type = "bool";
            s.preference = true;
            s.description = description;
            s.get = [getter = getter](SettingsContext&) { return Json(getter()); };
            s.set = [setter = setter](SettingsContext&, const Json& value, bool apply, std::string& error) {
                if (!value.is_boolean()) {
                    error = "must be true or false";
                    return false;
                }
                if (apply) {
                    setter(value.get<bool>());
                    AppSettings::saveSettings();
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.project_hub_at_startup";
            s.type = "enum";
            s.values = {"when_no_project", "always"};
            s.preference = true;
            s.description = "General: when the Project Hub opens at start (when_no_project: the last project is missing or fails to load).";
            s.get = [](SettingsContext&) { return Json(AppSettings::getProjectHubAlways() ? "always" : "when_no_project"); };
            s.set = [](SettingsContext&, const Json& value, bool apply, std::string& error) {
                if (!value.is_string() || (value.get<std::string>() != "always" && value.get<std::string>() != "when_no_project")) {
                    error = "must be when_no_project or always";
                    return false;
                }
                if (apply) {
                    AppSettings::setProjectHubAlways(value.get<std::string>() == "always");
                    AppSettings::saveSettings();
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.cjk_font_language";
            s.type = "enum";
            s.values = {"auto", "japanese", "korean", "simplified_chinese", "traditional_chinese"};
            s.preference = true;
            s.description = "General: which language's glyph forms the editor uses for Chinese characters, from the "
                            "system's fonts (auto: the system language). Applies at the next start.";
            s.get = [](SettingsContext&) { return Json(AppSettings::getCjkFontLanguage()); };
            s.set = [](SettingsContext&, const Json& value, bool apply, std::string& error) {
                if (!value.is_string() || !SystemFonts::isLanguageSetting(value.get<std::string>())) {
                    error = "must be auto, japanese, korean, simplified_chinese or traditional_chinese";
                    return false;
                }
                if (apply) {
                    AppSettings::setCjkFontLanguage(value.get<std::string>());
                    AppSettings::saveSettings();
                }
                return true;
            };
            add(s);
        }
        // S30: the platform tools of builds and releases (editor/platform/ExternalTools.h)
        auto toolPreference = [&stringPreference](const std::string& key, const std::string& tool, const std::string& description) {
            stringPreference(key, description, [tool] { return AppSettings::getExternalToolPath(tool); },
                             [tool](const std::string& v) { AppSettings::setExternalToolPath(tool, v); });
        };
        toolPreference("preferences.android_sdk_path", "androidSdk", "External Tools: the Android SDK of prebuilt Android builds (\"\" = ANDROID_HOME / ANDROID_SDK_ROOT).");
        toolPreference("preferences.jdk_path", "jdk", "External Tools: the JDK (keytool, jarsigner, java) of Android builds (\"\" = JAVA_HOME / PATH).");
        toolPreference("preferences.steamcmd_path", "steamcmd", "External Tools: SteamCMD for Steam uploads (\"\" = STEAMCMD_HOME / PATH).");
        toolPreference("preferences.rcodesign_path", "rcodesign", "External Tools: rcodesign, which signs macOS builds on any computer (\"\" = PATH).");
        // S44 §6.8: settings.yaml externalTools.aseprite.path
        toolPreference("preferences.aseprite_path", "aseprite.path", "External Tools: the Aseprite executable Open in Aseprite starts (\"\" = JASPER_ASEPRITE, then the usual install places and PATH).");
        stringPreference("preferences.ffmpeg_path", "External Tools: ffmpeg for video imports and Recorder movies (\"\" = JASPER_FFMPEG / PATH).",
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
            // the compilers are programs the build runs: user-only (the generator is just a name)
            s.userOnly = field.member != &LocalBuildSettings::generator;
            s.description = field.description;
            auto member = field.member;
            s.get = [member](SettingsContext& ctx) {
                return Json(AppSettings::getBuildSettings(ctx.project()->getProjectPath() / "project.yaml").*member);
            };
            s.set = [member](SettingsContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                // the generator is a name CMake must recognise (it drives the build command): an
                // arbitrary string is refused, never stored (upstream's round 9)
                if (member == &LocalBuildSettings::generator && !text.empty() && !Generator::isKnownGenerator(text)) {
                    error = "not a CMake generator name (e.g. \"Ninja\", \"Unix Makefiles\", \"Visual Studio 17 2022\")";
                    return false;
                }
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
        // ---- graphics: shader creation (spec S32), from its reflection table
        addShaderCreationSettings(add);
        // ---- quality: device tiers, LOD biases, thermal response; graphics: upload budget (S56)
        addDeviceSettings(add);
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
    {"player.default_orientation", "Player", "Resolution and Presentation", "Default Orientation", "orientation portrait landscape auto rotation android ios"},
    {"player.allowed_orientations", "Player", "Resolution and Presentation", "Allowed Orientations for Auto Rotation", "orientation rotation android ios"},
    {"player.keep_screen_on", "Player", "Resolution and Presentation", "Keep Screen On", "sleep timeout screen on"},
    {"player.target_frame_rate", "Player", "Resolution and Presentation", "Target Frame Rate", "fps frame rate"},
    {"player.allow_insecure_http", "Player", "Other Settings", "Allow Insecure HTTP", "http cleartext network"},
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
    {"player.crash.enabled", "Player", "Crash Handling", "Enabled", "crash reports diagnostics"},
    {"player.crash.log_tail", "Player", "Crash Handling", "Include Log Tail", "crash reports log"},
    {"player.crash.dump_with_heap", "Player", "Crash Handling", "Windows Dump Type", "crash minidump heap small"},
    {"player.crash.hang_watchdog", "Player", "Crash Handling", "Hang Watchdog In Development Builds", "hang freeze not responding"},
    {"player.crash.hang_seconds", "Player", "Crash Handling", "Hang Threshold (s)", "hang freeze seconds"},
    {"player.crash.web_overlay", "Player", "Crash Handling", "Web > Show Crash Overlay", "crash web reload overlay"},
    {"player.threading.workers", "Player", "Threading", "Worker Threads", "jobs threads"},
    {"player.threading.reserved_frame_workers", "Player", "Threading", "Reserved Frame Workers", "jobs threads"},
    {"player.threading.io_threads", "Player", "Threading", "Asset I/O Threads", "io loading threads"},
    {"player.threading.web_pump_budget_ms", "Player", "Threading", "Web Pump Budget", "web jobs"},
    {"player.threading.web_multithreaded", "Player", "Threading", "Web: Multithreaded", "pthreads cross origin isolated"},
    {"player.development_build", "Player", "Publishing Settings", "Development Build", "profiler"},
    {"player.pack_native_resources", "Player", "Publishing Settings", "Native Resource Pack", "resources pak"},
    {"player.build_method", "Player", "Publishing Settings", "Build Method", "prebuilt compile player"},
    {"player.pack_content", "Player", "Publishing Settings", "Content Pack", "jpak pack"},
    {"player.strip_engine_code", "Player", "Publishing Settings", "Strip Engine Code", "size modules variant smaller"},
    {"android.package_format", "Player", "Android", "Package Format", "android apk aab bundle abi universal split"},
    {"android.keystore_path", "Player", "Android", "Keystore", "android signing key"},
    {"android.keystore_alias", "Player", "Android", "Key Alias", "android signing key"},
    {"ios.team_id", "Player", "iOS", "Team ID", "ios signing apple developer"},
    {"ios.export_method", "Player", "iOS", "Export Method", "ios ipa app store ad hoc"},
    {"web.application_name", "Player", "Web", "App Name", "web page title"},
    {"web.favicon", "Player", "Web", "Favicon", "web icon"},
    {"web.custom_html_shell", "Player", "Web", "Custom HTML Shell", "web emscripten template"},
    {"web.head_include", "Player", "Web", "Head Include", "web html head"},
    {"web.resize_canvas_to_window", "Player", "Web", "Resize Canvas To Window", "web"},
    {"web.hide_emscripten_ui", "Player", "Web", "Hide Emscripten UI", "web"},
    {"web.viewport_fit_cover", "Player", "Web", "Viewport Fit Cover", "web safe area notch viewport-fit env safe-area-inset"},
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
    {"android.application_name", "Player", "Android", "App Name", "android label"},
    {"android.package_name", "Player", "Android", "Package Name", "android"},
    {"android.version_code", "Player", "Android", "Version Code", "android"},
    {"android.version_name", "Player", "Android", "Version Name", "android"},
    {"android.launcher_icon", "Player", "Android", "Launcher Icon", "android icon launcher icons"},
    {"android.adaptive_icon_foreground", "Player", "Android", "Adaptive Foreground", "android launcher icons adaptive icon"},
    {"android.adaptive_icon_background", "Player", "Android", "Adaptive Background", "android launcher icons adaptive icon"},
    {"android.min_sdk", "Player", "Android", "Min SDK", "android api level"},
    {"android.target_sdk", "Player", "Android", "Target SDK", "android api level"},
    {"android.internet_access", "Player", "Android", "Internet Access", "android internet permission network http"},
    {"android.abis", "Player", "Android", "Architectures", "android abi arm64 x86"},
    {"android.permissions", "Player", "Android", "Permissions", "android"},
    {"android.allow_backup", "Player", "Android", "Application Options", "android allow backup"},
    {"android.fullscreen", "Player", "Android", "Screen Options", "android fullscreen"},
    {"sprite_atlas.mode", "2D", "Sprite Atlas", "Mode", "sprite atlas packer pages enabled for builds"},
    {"graphics.canvas_width", "Graphics", "", "Canvas Width", "reference resolution"},
    {"graphics.canvas_height", "Graphics", "", "Canvas Height", "reference resolution"},
    {"graphics.scaling_mode", "Graphics", "", "Scaling Mode", "letterbox fit crop stretch integer pixel perfect"},
    {"graphics.pixels_per_unit", "Graphics", "", "Pixels Per Unit", "pixel art ppu sprite tilemap"},
    {"graphics.ui_scale", "Graphics", "", "UI Scale", "canvas scaler ui scaling reference resolution match width height constant pixel physical size dpi"},
    {"graphics.texture_strategy", "Graphics", "", "Texture Strategy", "power of two"},
    {"graphics.mobile_pbr_tier", "Graphics", "", "Mobile PBR Tier", "clear coat iridescence anisotropy lit android ios web"},
    {"graphics.light_mode", "Graphics", "", "Light Mode", "forward+ forward plus tiled clustered per object pixel lights max lights"},
    {"graphics.html_ui_msaa_samples", "Graphics", "", "HTML UI MSAA", "anti-aliasing antialiasing multisampling"},
    {"graphics.html_ui_glyph_atlas_mib", "Graphics", "", "HTML UI Glyph Atlas", "font text memory budget mib"},
    {"graphics.html_ui_sdf_primitives", "Graphics", "", "HTML UI SDF Primitives", "batching shapes borders box shadows"},
    {"graphics.html_ui_transform_aware_text", "Graphics", "", "HTML UI Transform-Aware Text", "sharp text scale msdf distance field"},
    {"particles.world_budget", "Graphics", "", "World Particle Budget", "particles limit max effects sub-emitters"},
    {"particles.qa.overdraw_mobile", "Graphics", "Particle Checks", "Overdraw Layers Mobile", "particles check overdraw fill rate"},
    {"particles.qa.overdraw_web", "Graphics", "Particle Checks", "Overdraw Layers Web", "particles check overdraw fill rate"},
    {"particles.qa.overdraw_desktop", "Graphics", "Particle Checks", "Overdraw Layers Desktop", "particles check overdraw fill rate"},
    {"particles.qa.cpu_ms_mobile", "Graphics", "Particle Checks", "CPU Budget Mobile (ms)", "particles check platform budget cost"},
    {"particles.qa.cpu_ms_web", "Graphics", "Particle Checks", "CPU Budget Web (ms)", "particles check platform budget cost"},
    {"particles.qa.cpu_ms_desktop", "Graphics", "Particle Checks", "CPU Budget Desktop (ms)", "particles check platform budget cost"},
    {"particles.qa.blown_out_percent", "Graphics", "Particle Checks", "Blown Out (%)", "particles check additive saturated white"},
    {"particles.qa.invisible_percent", "Graphics", "Particle Checks", "Invisible Below (%)", "particles check blank empty"},
    {"particles.qa.pop_percent", "Graphics", "Particle Checks", "Pop (%)", "particles check death fade"},
    {"particles.qa.fix_rounds", "Graphics", "Particle Checks", "Fix Rounds", "particles check agent"},
    {"materials.qa.seam_fail", "Graphics", "Material Checks", "Seam Fail", "procedural material check tile seam"},
    {"materials.qa.repetition_warn", "Graphics", "Material Checks", "Repetition Warn", "procedural material check tiling repeat"},
    {"materials.qa.clipped_percent", "Graphics", "Material Checks", "Clipped (%)", "procedural material check histogram clipping"},
    {"materials.qa.dielectric_outside_percent", "Graphics", "Material Checks", "Dielectric Outside (%)", "procedural material check albedo base colour"},
    {"materials.qa.normal_length_error", "Graphics", "Material Checks", "Normal Length Error", "procedural material check normal map"},
    {"materials.qa.roughness_min_range", "Graphics", "Material Checks", "Roughness Min Range", "procedural material check roughness flat"},
    {"materials.qa.metallic_mid_percent", "Graphics", "Material Checks", "Metallic Mid (%)", "procedural material check metal"},
    {"materials.qa.fix_rounds", "Graphics", "Material Checks", "Fix Rounds", "procedural material check agent"},
    {"graphics.shadow_tier.windows", "Graphics", "Shadows", "Windows Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.linux", "Graphics", "Shadows", "Linux Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.macos", "Graphics", "Shadows", "macOS Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.web", "Graphics", "Shadows", "Web Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.android", "Graphics", "Shadows", "Android Shadow Tier", "shadow quality"},
    {"graphics.shadow_tier.ios", "Graphics", "Shadows", "iOS Shadow Tier", "shadow quality"},
    {"graphics.dynamic_batching", "Graphics", "", "Dynamic Batching", "batching batches draw calls saved by batching sprites"},
    {"graphics.runtime_shaders", "Graphics", "", "Runtime Shader Compilation", "variants"},
    {"graphics.gpu_instancing", "Graphics", "", "GPU Instancing", "batching instanced draw calls auto instancing"},
    // S48 R41
    {"graphics.screen_transitions.kind", "Graphics", "Screen Transitions", "Default Kind", "transition iris wipe fade scene change pixel"},
    {"graphics.screen_transitions.duration", "Graphics", "Screen Transitions", "Duration", "transition seconds"},
    {"graphics.screen_transitions.color", "Graphics", "Screen Transitions", "Colour", "transition ink color"},
    {"graphics.screen_transitions.ramp", "Graphics", "Screen Transitions", "Ramp", "transition rim palette ramp"},
    {"graphics.screen_transitions.rim", "Graphics", "Screen Transitions", "Rim", "transition rim width art pixels"},
    {"graphics.screen_transitions.pixel_size", "Graphics", "Screen Transitions", "Pixel Size", "transition art pixel grid"},
    {"graphics.screen_transitions.cover_ui", "Graphics", "Screen Transitions", "Cover UI", "transition hud layer scenes"},
    {"quality.vsync", "Quality", "", "VSync", "vertical sync refresh"},
    {"quality.atmosphere_quality_desktop", "Quality", "", "Atmosphere Quality (Desktop)", "volumetric fog clouds"},
    {"quality.atmosphere_quality_mobile", "Quality", "", "Atmosphere Quality (Mobile)", "volumetric fog clouds"},
    {"quality.atmosphere_quality_web", "Quality", "", "Atmosphere Quality (Web)", "volumetric fog clouds"},
    {"quality.foliage_distance_desktop", "Quality", "", "Foliage Distance (Desktop)", "terrain grass trees detail draw distance"},
    {"quality.foliage_distance_mobile", "Quality", "", "Foliage Distance (Mobile)", "terrain grass trees detail draw distance"},
    {"quality.foliage_distance_web", "Quality", "", "Foliage Distance (Web)", "terrain grass trees detail draw distance"},
    {"quality.lod_bias", "Quality", "", "LOD Bias", "lod level of detail bias meshes distance"},
    {"quality.shadow_lod_bias", "Quality", "", "Shadow LOD Bias", "lod level of detail shadows casters bias"},
    {"physics.enabled", "Physics", "", "3D Physics", "jolt rigidbody"},
    {"physics.layer_collision_matrix", "Physics", "Layer Collision Matrix", "Layer Collision Matrix", "layers collide"},
    {"physics.max_bodies", "Physics", "3D World Capacity", "Max Bodies", "jolt capacity limit rigidbodies"},
    {"physics.max_body_pairs", "Physics", "3D World Capacity", "Max Body Pairs", "jolt capacity limit contacts"},
    {"physics.max_contact_constraints", "Physics", "3D World Capacity", "Max Contact Constraints", "jolt capacity limit contacts"},
    {"physics2d.enabled", "Physics 2D", "", "2D Physics", "box2d rigidbody 2d"},
    {"physics2d.layer_collision_matrix", "Physics 2D", "Layer Collision Matrix", "Layer Collision Matrix", "layers collide"},
    {"animation.animation_entities", "Animation", "", "Animation Entities", "model clips animation player animator entities auto"},
    {"navigation.include_in_builds", "Navigation", "", "Include in Builds", "navmesh recast detour strip export"},
    {"navigation.crowd_max_agents", "Navigation", "", "Crowd Max Agents", "navmesh agents crowd"},
    {"navigation.max_grid_solves_per_tick", "Navigation", "", "Grid Solves per Tick", "nav grid a* pathfinding"},
    {"navigation.carve_tiles_per_tick", "Navigation", "", "Carve Tiles Per Tick", "navmesh obstacle carving budget tile cache"},
    {"navigation.web_bake_budget_ms", "Navigation", "", "Web Bake Budget", "navmesh bake web single thread"},
    {"input.actions", "Input", "", "Default Actions Asset", "input actions bindings controls keyboard gamepad touch inputactions"},
    {"input.deadzone", "Input", "", "Default Deadzone", "input stick axis analog gamepad dead zone"},
    {"input.manager_actions", "Input", "Input Manager", "Input Manager", "input manager virtual axes buttons horizontal vertical jump fire gravity sensitivity unity"},
    // Project Settings > Pixel Art (S43 R4b; S42's Pixel Art page joins it)
    {"palettes.default", "Pixel Art", "", "Default Palette", "palette colours colors swatches snap lospec apply palette sweetie pico"},
    // Project Settings > Audio
    {"audio.mixer", "Audio", "", "Audio Mixer", "mixer groups master bus sound music"},
    {"audio.default_output_group", "Audio", "", "Default Output Group", "mixer group master output audio source"},
    {"audio.music_group", "Audio", "", "Music Group", "mixer group music track playlist playMusic stinger"},
    {"audio.max_real_voices", "Audio", "", "Max Real Voices", "voices virtual virtualised polyphony"},
    {"audio.max_voices", "Audio", "", "Max Voices", "voices polyphony priority"},
    {"audio.sample_rate", "Audio", "", "Sample Rate", "hz 22050 44100 48000 output frequency"},
    {"audio.seed", "Audio", "", "Random Seed", "seed sound event random shuffle deterministic repeat"},
    {"audio.capture_max_seconds", "Audio", "Capture and QA", "Capture Max Seconds", "capture session length truncate"},
    {"audio.capture_keep", "Audio", "Capture and QA", "Capture Keep", "capture sessions kept delete oldest"},
    {"audio.capture_stems", "Audio", "Capture and QA", "Capture Stems", "capture stems groups wav"},
    {"audio.qa.head_silence_max_ms", "Audio", "Capture and QA", "Head Silence Max (ms)", "check sync head silence"},
    {"audio.qa.sync_tolerance_ms", "Audio", "Capture and QA", "Sync Tolerance (ms)", "check coverage sync"},
    {"audio.qa.audibility_margin_db", "Audio", "Capture and QA", "Audibility Margin (dB)", "check masking"},
    {"audio.qa.voice_budget_warn", "Audio", "Capture and QA", "Voice Budget Warning", "check voices"},
    {"audio.qa.important_priority", "Audio", "Capture and QA", "Important Priority", "check voices stolen"},
    {"audio.qa.voice_over_music_db", "Audio", "Capture and QA", "Voice Over Music (dB)", "check duck"},
    {"audio.qa.max_silence_seconds", "Audio", "Capture and QA", "Max Silence (s)", "check gap music"},
    {"audio.qa.music_group", "Audio", "Capture and QA", "Music Group", "check music silence"},
    {"audio.qa.loudness_min_seconds", "Audio", "Capture and QA", "Loudness Min Seconds", "check lufs"},
    {"audio.qa.limiter_warn_db", "Audio", "Capture and QA", "Limiter Warning (dB)", "check limiter hot"},
    {"audio.qa.fix_rounds", "Audio", "Capture and QA", "Fix Rounds", "check agent stop report"},
    {"audio.qa.repeat_min_plays", "Audio", "Capture and QA", "Repeat Min Plays", "check repetition"},
    {"audio.qa.repeat_window_s", "Audio", "Capture and QA", "Repeat Window (s)", "check repetition"},
    {"audio.qa.regression_timing_ms", "Audio", "Capture and QA", "Regression Timing (ms)", "check baseline"},
    {"audio.qa.regression_band_db", "Audio", "Capture and QA", "Regression Band (dB)", "check baseline"},
    {"audio.loudness_target_lufs", "Audio", "Loudness and Output", "Loudness Target", "lufs loudness level r128 target"},
    {"audio.loudness_tolerance_lu", "Audio", "Loudness and Output", "Loudness Tolerance", "lu lufs loudness tolerance"},
    {"audio.true_peak_ceiling_dbtp", "Audio", "Loudness and Output", "True Peak Ceiling", "dbtp true peak ceiling limiter"},
    {"audio.output_stage", "Audio", "Loudness and Output", "Output Stage", "roundoff linear clip clipper output limiter"},
    {"audio.ios_session_category", "Audio", "Loudness and Output", "iOS Audio Session", "ios ambient silent switch session category music"},
    {"scripting.lua_dir", "Scripting", "", "Lua Directory", "lua"},
    {"scripting.script_dirs", "Scripting", "", "Script Directories", "c++ cpp"},
    {"scripting.cxx_standard", "Scripting", "", "C++ Standard", "cpp c++17 c++20 c++23"},
    {"editor.version_control_files", "Editor", "Version Control", "Version Control Files", "git gitignore gitattributes vcs"},
    {"editor.vcs.provider", "Editor", "Version Control", "Provider", "git svn subversion vcs repository"},
    {"editor.vcs.lfs", "Editor", "Version Control", "Git LFS", "lfs large files lockable binaries"},
    {"editor.vcs.lock_scenes", "Editor", "Version Control", "Lockable Scenes", "lock scenes prefabs"},
    {"editor.vcs.git_path", "Version Control", "Tools", "Git Path", "git executable"},
    {"editor.vcs.svn_path", "Version Control", "Tools", "Subversion Path", "svn executable"},
    {"editor.vcs.lock_on_edit", "Version Control", "Locking", "Lock on Edit", "lock ask auto"},
    {"editor.vcs.poll_seconds", "Version Control", "Refresh", "Status Poll (s)", "refresh interval seconds"},
    {"editor.vcs.lock_poll_seconds", "Version Control", "Refresh", "Lock Poll (s)", "locks server refresh seconds"},
    {"editor.vcs.auto_fetch_minutes", "Version Control", "Refresh", "Auto Fetch (min)", "fetch remote minutes"},
    {"editor.vcs.commit_and_push", "Version Control", "Commit", "Commit and Push", "push remote"},
    {"editor.vcs.code_editor_changes", "Version Control", "Code Editor", "Show Changes in Code Editor", "gutter markers diff lines"},
    {"editor.assets_dir", "Editor", "", "Assets Directory", "assets root"},
    {"tags_and_layers.tags", "Tags and Layers", "Tags", "Tags", "tag"},
    {"tags_and_layers.layers", "Tags and Layers", "Layers", "Layers", "layer"},
    // Project Settings > Native Plugins
    {"android.gradle_dependencies", "Native Plugins", "Android", "Gradle Dependencies", "android gradle dependencies sdk api implementation"},
    {"android.gradle_repositories", "Native Plugins", "Android", "Maven Repositories", "android gradle maven repository url"},
    {"android.gradle_properties", "Native Plugins", "Android", "Gradle Properties", "android gradle properties"},
    {"android.extra_permissions", "Native Plugins", "Android", "Extra Permissions", "android uses-permission permissions"},
    {"android.manifest_xml", "Native Plugins", "Android", "Manifest XML", "android manifest xml queries uses-feature"},
    {"android.manifest_application_xml", "Native Plugins", "Android", "Application XML", "android manifest application xml meta-data activity provider"},
    {"ios.plist_entries", "Native Plugins", "iOS", "Info.plist Entries", "ios info.plist plist keys"},
    {"ios.plist_xml", "Native Plugins", "iOS", "Info.plist XML", "ios info.plist plist xml skadnetwork"},
    {"apple.privacy_tracking", "Player", "iOS", "Privacy Tracking", "privacy manifest xcprivacy tracking att app store ios"},
    {"apple.privacy_tracking_domains", "Player", "iOS", "Privacy Tracking Domains", "privacy manifest xcprivacy tracking domains ios"},
    {"apple.privacy_collected_data", "Player", "iOS", "Privacy Collected Data", "privacy manifest xcprivacy collected data nutrition label ios"},
    {"apple.privacy_accessed_apis", "Player", "iOS", "Privacy Required Reason APIs", "privacy manifest xcprivacy required reason api ios"},
    {"ios.frameworks", "Native Plugins", "iOS", "Frameworks", "ios frameworks link weak storekit"},
    {"ios.cocoapods", "Native Plugins", "iOS", "CocoaPods", "ios cocoapods pods podfile"},
    {"ios.swift_packages", "Native Plugins", "iOS", "Swift Packages", "ios swift packages spm"},
    {"ios.entitlements_xml", "Native Plugins", "iOS", "Entitlements", "ios entitlements capabilities xml"},
    {"ios.deployment_target", "Native Plugins", "iOS", "Deployment Target", "ios deployment target minimum version iphoneos"},
    {"macos.plist_entries", "Native Plugins", "macOS", "Info.plist Entries", "macos mac info.plist plist keys"},
    {"macos.frameworks", "Native Plugins", "macOS", "Frameworks", "macos mac frameworks link weak gamekit"},
    {"web.script_urls", "Native Plugins", "Web", "Script URLs", "web script sdk javascript head url"},
    {"preferences.clear_on_play", "General", "Console", "Clear on Play", "console"},
    {"preferences.error_pause", "General", "Console", "Error Pause", "console"},
    {"preferences.simulate_touch", "General", "Game View", "Simulate Touch", "game view mouse finger"},
    {"preferences.network.offline", "General", "Network", "Offline", "network internet offline proxy lospec download http"},
    {"preferences.default_export_dir", "General", "", "Default Build Directory", "output export"},
    {"preferences.sound_browser_autoplay", "General", "Sound Browser", "Auto-play", "sound browser audition select play"},
    {"preferences.sound_browser_match_loudness", "General", "Sound Browser", "Match Loudness", "sound browser audition loudness lufs"},
    {"preferences.project_hub_at_startup", "General", "", "Show Project Hub at Startup", "project hub startup launcher templates"},
    {"preferences.cjk_font_language", "General", "Editor Font", "CJK Font Language", "cjk chinese japanese korean kanji hanja han font language glyphs"},
    {"preferences.cmake_path", "External Tools", "Desktop Build", "CMake", "cmake executable"},
    {"preferences.build_c_compiler", "External Tools", "Desktop Build", "Compiler", "compiler kit c"},
    {"preferences.build_cxx_compiler", "External Tools", "Desktop Build", "C++ Compiler", "compiler kit cpp"},
    {"preferences.build_generator", "External Tools", "Desktop Build", "Generator", "cmake ninja makefiles"},
    {"preferences.build_jobs", "External Tools", "Desktop Build", "Build Jobs", "parallel jobs"},
    {"preferences.emsdk_path", "External Tools", "Web Build", "Emscripten SDK", "emsdk web"},
    {"preferences.ffmpeg_path", "External Tools", "Video Import", "FFmpeg", "video av1"},
    {"preferences.android_sdk_path", "External Tools", "Shipping", "Android SDK", "android sdk aapt2 apksigner build-tools"},
    {"preferences.jdk_path", "External Tools", "Shipping", "JDK", "java keytool jarsigner"},
    {"preferences.steamcmd_path", "External Tools", "Shipping", "SteamCMD", "steam upload steampipe"},
    {"preferences.rcodesign_path", "External Tools", "Shipping", "rcodesign", "macos signing codesign"},
    {"preferences.aseprite_path", "External Tools", "Aseprite", "Aseprite", "aseprite pixel art sprite editor executable"},
    {"preferences.logging.file_level", "Logging", "", "Log File Level", "editor log"},
    {"preferences.logging.backtrace_on_error", "Logging", "", "Capture Backtrace on Errors", "stack trace"},
    {"preferences.logging.show_source", "Logging", "", "Show Source Location", "file line"},
    {"preferences.logging.levels", "Logging", "Category Levels", "Category Levels", "category level"},
};

std::string titleCase(const std::string& key);

// Project Settings > Texture (S14 R24): texture.<setting>[.<family or platform group>]
// Graphics > Shader Creation; the warm-up options under their own heading. The label is the
// reflection table's
void shaderCreationRowPlacement(const std::string& key, SettingRow& row) {
    row.page = "Graphics";
    const std::string name = key.substr(std::string("shader_creation.").size());
    const bool warmup = name == "record_during_play" || name == "include_in_builds" || name == "hold_first_scene";
    row.section = warmup ? "Shader Warm-up" : "Shader Creation";
    row.keywords = warmup ? "shader warm-up warmup prewarm variants pipelines record export loading"
                          : "shader compile compilation pipeline async asynchronous stall hitch budget warm";
    row.label = titleCase(key);
    for (const reflect::FieldDesc& field : reflect::shaderCreationStruct().fields) {
        if (snakeCase(field.name) == name && field.label)
            row.label = field.label;
    }
}

// Quality > Device Tiers (quality.device_tiers.<group>.<tier>), Quality > Thermal Response
// (quality.thermal_response.<group>), Graphics > Upload Budget (graphics.upload_budget_mb.<group>)
bool deviceRowPlacement(const std::string& key, SettingRow& row) {
    auto groupLabel = [](const std::string& name) {
        DevicePlatformGroup group;
        return DeviceNames::groupFromName(name.c_str(), group) ? std::string(DeviceNames::groupLabel(group)) : name;
    };
    static const std::string tiers = "quality.device_tiers.";
    static const std::string thermal = "quality.thermal_response.";
    static const std::string upload = "graphics.upload_budget_mb.";
    if (key.rfind(tiers, 0) == 0) {
        const std::string rest = key.substr(tiers.size());
        const size_t dot = rest.find('.');
        DeviceTier tier = DeviceTier::High;
        DeviceNames::tierFromName(rest.substr(dot + 1).c_str(), tier);
        row.page = "Quality";
        row.section = "Device Tiers";
        row.label = std::string(DeviceNames::tierLabel(tier)) + " (" + groupLabel(rest.substr(0, dot)) + ")";
        row.keywords = "device tier gpu low medium high ultra render scale msaa shadows lod bias texture bloom ssao volumetrics frame rate lights particles";
        return true;
    }
    if (key.rfind(thermal, 0) == 0) {
        row.page = "Quality";
        row.section = "Thermal Response";
        row.label = "Thermal Response (" + groupLabel(key.substr(thermal.size())) + ")";
        row.keywords = "thermal throttling heat battery render scale frame rate";
        return true;
    }
    if (key.rfind(upload, 0) == 0) {
        row.page = "Graphics";
        row.section = "Upload Budget";
        row.label = "Upload Budget MB (" + groupLabel(key.substr(upload.size())) + ")";
        row.keywords = "upload budget streaming textures buffers hitch megabytes per frame";
        return true;
    }
    return false;
}

void textureRowPlacement(const std::string& key, SettingRow& row) {
    static const std::map<std::string, std::pair<const char*, const char*>> settings = {
        {"android_etc2_fallback", {"Android", "ETC2 Fallback"}},
        {"default_quality", {"Defaults", "Quality"}},
        {"default_max_size", {"Defaults", "Max Size"}},
        {"texture_quality", {"Runtime", "Texture Quality"}},
        {"memory_budget_mb", {"Runtime", "Memory Budget (MB)"}},
        {"low_memory_fallback", {"Runtime", "Low-Memory Fallback"}},
        {"cache_size_mb", {"Cache", "Cache Size (MB)"}},
        {"global_cache_dir", {"Cache", "Global Cache Directory"}},
    };
    static const std::map<std::string, const char*> targets = {
        {"desktop", "Desktop"}, {"mobile", "Mobile"}, {"android_etc2", "Android ETC2"}, {"web", "Web"}};
    const std::string rest = key.substr(std::string("texture.").size());
    const size_t dot = rest.find('.');
    const std::string name = rest.substr(0, dot);
    row.page = "Texture";
    row.keywords = "texture compression cook";
    auto it = settings.find(name);
    if (it == settings.end()) {
        row.label = titleCase(key);
        return;
    }
    row.section = it->second.first;
    row.label = it->second.second;
    if (dot != std::string::npos) {
        const std::string target = rest.substr(dot + 1);
        auto t = targets.find(target);
        row.label += std::string(" (") + (t != targets.end() ? t->second : target.c_str()) + ")";
        row.keywords += " " + target;
    }
}

// mesh.defaults.<platform>.<setting>, mesh.*, quality.render_budgets.<platform>.<metric> (S53)
void meshRowPlacement(const std::string& key, SettingRow& row) {
    static const std::map<std::string, const char*> labels = {
        {"optimize_mesh", "Optimize Mesh"}, {"mesh_compression", "Mesh Compression"}, {"weld_vertices", "Weld Vertices"},
        {"index_format", "Index Format"}, {"read_write", "Read/Write"}, {"normals", "Normals"}, {"smoothing_angle", "Smoothing Angle"},
        {"generate_lods", "Generate LODs"}, {"lod_count", "LOD Count"}, {"lod_reduction", "Reduction"}, {"ship_source", "Ship Source Files"},
        {"cache_size_mb", "Cache Size (MB)"}, {"keep_cpu_data_for_scripts", "Keep CPU Data for Scripts"},
        {"triangles", "Triangle Budget"}, {"draw_calls", "Draw Call Budget"}, {"mesh_memory_mb", "Mesh Memory Budget (MB)"},
        {"download_mb", "Download Budget (MB)"}};
    static const std::map<std::string, const char*> platforms = {{"desktop", "Desktop"}, {"mobile", "Mobile"}, {"web", "Web"}};
    const std::string leaf = key.substr(key.rfind('.') + 1);
    auto label = labels.find(leaf);
    row.label = label != labels.end() ? label->second : titleCase(key);
    std::string platform;
    for (const auto& [name, title] : platforms)
        if (key.find("." + name + ".") != std::string::npos) platform = title;
    if (!platform.empty()) row.label += " (" + platform + ")";
    if (key.rfind("quality.", 0) == 0) {
        row.page = "Quality";
        row.section = "Render Budgets";
        row.keywords = "render budget triangles draw calls memory download check_render_budgets";
    } else {
        row.page = "Mesh";
        row.section = key.rfind("mesh.defaults.", 0) == 0 ? "Defaults" : "Cache";
        row.keywords = "mesh model import cook";
    }
}

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
    // the pages, in the windows' order (the category arrays the windows had before S25; Project
    // Settings' ProjectCategory, window/dialog/ProjectSettingsWindow.cpp, follows these orders)
    const SettingsPage pages[] = {
        {"Player", SettingsScope::Project, 0, "project product application icon resolution splash loading publishing platform"},
        {"Graphics", SettingsScope::Project, 1, "canvas scaling texture shaders shadows"},
        {"Quality", SettingsScope::Project, 2, "atmosphere fog clouds"},   // VSync: its row (a page match shows every row)
        {"Texture", SettingsScope::Project, 3, "texture compression import cook format astc etc2 bc7 basis mipmap budget cache"},
        {"Physics", SettingsScope::Project, 4, "3d jolt"},
        {"Physics 2D", SettingsScope::Project, 5, "2d box2d"},
        {"Input", SettingsScope::Project, 6, "input actions bindings controls gamepad deadzone"},
        // upstream's (SYNC#4: audio-mixer, debug-overlay, native-bridge)
        {"Audio", SettingsScope::Project, 7, "audio mixer groups voices real voices sample rate output group sound music volume"},
        {"Scripting", SettingsScope::Project, 8, "lua c++"},
        {"Debug", SettingsScope::Project, 9, "debug overlay console log stats fps graph development build toggle key gesture popup error commands lua "
                                             "buffer stack traces ingamedebugconsole graphy"},
        {"Tags and Layers", SettingsScope::Project, 10, "tags layers"},
        {"Native Plugins", SettingsScope::Project, 11, "native plugins android java kotlin ios macos objective-c swift web javascript c++ gradle dependencies "
                                                       "repositories maven properties permissions manifest xml application info.plist plist frameworks cocoapods "
                                                       "pods swift packages spm entitlements deployment target script urls sdk"},
        {"Editor", SettingsScope::Project, 12, "version control assets labels"},
        {"Navigation", SettingsScope::Project, 13, "navigation navmesh agents areas crowd grid pathfinding ai"},
        {"Mesh", SettingsScope::Project, 14, "mesh model import cook optimize compression lod meshopt read write cache audit"},
        {"2D", SettingsScope::Project, 15, "2d sprite atlas packer pages pixel art"},
        {"Animation", SettingsScope::Project, 16, "animation clips model animator animation player entities"},
        {"Pixel Art", SettingsScope::Project, 17, "pixel art 2d palette palettes colours colors swatches"},
        {"General", SettingsScope::Preference, 0, "editor auto save backup html ui"},
        {"Code Editor", SettingsScope::Preference, 1, "font"},
        {"Asset Pipeline", SettingsScope::Preference, 2, "refresh watcher hot reload"},
        {"Scripting", SettingsScope::Preference, 3, "hot reload during play external script editor"},
        {"Undo", SettingsScope::Preference, 4, "undo redo history"},
        {"Lattice", SettingsScope::Preference, 5, "lattice cage deformer"},
        {"Scene View", SettingsScope::Preference, 6, "picking pick drop surface rulers animation preview particles"},
        {"External Tools", SettingsScope::Preference, 7, "cmake compiler emscripten ffmpeg"},
        {"Logging", SettingsScope::Preference, 8, "log levels"},
        {"Diagnostics", SettingsScope::Preference, 9, "crash hang watchdog freeze not responding reports"},
        {"Shader Cache", SettingsScope::Preference, 10, "shaders cache clear"},
        {"Version Control", SettingsScope::Preference, 11, "git svn subversion lfs lock commit merge"},
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
        row.userOnly = setting.userOnly;
        row.tooltip = setting.description;
        if (const Placement* placement = placementOf(setting.key)) {
            row.page = placement->page;
            row.section = placement->section;
            row.label = placement->label;
            row.keywords = placement->keywords;
        } else if (setting.key.rfind("texture.", 0) == 0) {
            textureRowPlacement(setting.key, row);
        } else if (setting.key.rfind("mesh.", 0) == 0 || setting.key.rfind("quality.render_budgets.", 0) == 0) {
            meshRowPlacement(setting.key, row);
        } else if (setting.key.rfind("shader_creation.", 0) == 0) {
            shaderCreationRowPlacement(setting.key, row);
        } else if (setting.key.rfind("debug.", 0) == 0) {
            debugRowPlacement(setting.key, row);
        } else if (!deviceRowPlacement(setting.key, row)) {
            row.page = setting.preference ? "General" : "Editor";
            row.label = titleCase(setting.key);
        }
        if (setting.preference && row.page == "Scripting") row.page = "Scripting (Preferences)";
        addOrAbort(registry, std::move(row));
    }

    // ---- Project Settings rows MCP does not set
    // the privacy manifest's section is on Player > macOS too (the same additions as iOS)
    for (const char* key : {"apple.privacy_tracking", "apple.privacy_tracking_domains", "apple.privacy_collected_data", "apple.privacy_accessed_apis"}) {
        const SettingRow* ios = registry.find(key);
        if (!ios) continue;
        SettingRow mac = uiRow("", "Player", "macOS", "", "", ios->get, "");
        mac.key = "macos." + std::string(key).substr(std::string("apple.").size());
        mac.label = ios->label;
        mac.keywords = ios->keywords.substr(0, ios->keywords.rfind(" ios")) + " macos mac";
        mac.tooltip = ios->tooltip;
        addOrAbort(registry, std::move(mac));
    }
    addOrAbort(registry, uiRow("shader_creation.warmup_entries", "Graphics", "Shader Warm-up", "Warm-up List",
        "shader warm-up warmup list entries clear recorded variants pipelines",
        [](SettingsContext& ctx) {
            // the file as saved (a recording in progress is merged into it when Play stops)
            const std::filesystem::path path = ctx.project()->getShaderWarmupPath();
            size_t shaders = 0, pipelines = 0;
            std::ifstream file(path, std::ios::binary);
            if (file) {
                std::stringstream text;
                text << file.rdbuf();
                const Json list = Json::parse(text.str(), nullptr, false);
                if (list.is_object()) {
                    shaders = list.value("shaders", Json::array()).size();
                    pipelines = list.value("pipelines", Json::array()).size();
                }
            }
            return Json{{"shaders", shaders}, {"pipelines", pipelines}, {"path", path.filename().string()}};
        }, "The shader variants and pipelines Play recorded (ShaderWarmup.json next to project.yaml); Clear empties it."));
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
    addOrAbort(registry, uiRow("mesh.clear_cache", "Mesh", "Cache", "Mesh Cache", "clear cooked models cache size audit",
        [](SettingsContext& ctx) {
            ModelCookQueue& cooks = ModelCookQueue::get();
            return cooks.getProject() == ctx.project() ? Json(cooks.getCache().totalBytes()) : Json(nullptr);
        }, "The cooked model cache's size, Clear Mesh Cache (reimport_models cooks again) and the Mesh Optimization Audit."));
    addOrAbort(registry, uiRow("texture.clear_cache", "Texture", "Cache", "Texture Cache", "clear cooked textures cache size",
        [](SettingsContext& ctx) {
            TextureCookQueue& cooks = TextureCookQueue::get();
            return cooks.getProject() == ctx.project() ? Json(cooks.getCache().totalBytes()) : Json(nullptr);
        }, "The cooked texture cache's size, and Clear Texture Cache (reimport_textures cooks again)."));

    // ---- Preferences rows MCP does not set
    addOrAbort(registry, uiRow("preferences.player_runtimes", "External Tools", "Shipping", "Player Runtimes", "prebuilt players download install remove",
        [](SettingsContext&) {
            Json players = Json::array();
            for (const PlayerInfo& info : PlayerRuntimes::list()) players.push_back(Json{{"id", info.id}, {"engine_version", info.engineVersion}});
            return players;
        }, "The installed prebuilt player runtimes: Remove, Install from file... (install_player_runtime installs one)."));
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
    // the Scene view toolbar's toggles that follow the user (S26 R12, R16, R25)
    const char* sceneView = "Scene View";
    addOrAbort(registry, uiRow("preferences.scene_view.exact_pick", sceneView, "Picking", "Exact Pick", "select click prefab model root",
        [](SettingsContext&) { return Json(AppSettings::getSceneViewSettings().exactPick); },
        "A click selects the GameObject under it, never its prefab or model root"));
    addOrAbort(registry, uiRow("preferences.scene_view.align_to_surface", sceneView, "Drops", "Align to Surface", "drop normal up axis",
        [](SettingsContext&) { return Json(AppSettings::getSceneViewSettings().alignToSurface); },
        "Objects dropped into the view turn their up axis onto the surface they land on (Alt while dropping)"));
    addOrAbort(registry, uiRow("preferences.scene_view.rulers", sceneView, "Views", "Rulers", "ruler scale 2d orthographic",
        [](SettingsContext&) { return Json(AppSettings::getSceneViewSettings().rulers); },
        "Top and left rulers in 2D, UI and orthographic axis views"));
    addOrAbort(registry, uiRow("preferences.scene_view.preview_particles", sceneView, "Animation", "Preview Particle Systems", "animator animation preview particles vfx scrub",
        [](SettingsContext&) { return Json(AppSettings::getSceneViewSettings().previewParticles); },
        "Animation previews in edit mode (Animator, Animation windows) play and scrub the Particle Systems under the previewed GameObject"));
    addOrAbort(registry, uiRow("preferences.logging.reset", "Logging", "Category Levels", "Reset", "category levels info",
        [](SettingsContext&) { return Json(AppSettings::getLoggingSettings().categoryLevels.size()); }));
    const char* diagnostics = "Diagnostics";
    addOrAbort(registry, uiRow("preferences.diagnostics.hang_watchdog", diagnostics, "", "Hang Watchdog", "hang freeze not responding report",
        [](SettingsContext&) { return Json(AppSettings::getDiagnosticsSettings().hangWatchdog); },
        "Writes a hang report when the editor stops responding (set_hang_watchdog sets it)."));
    addOrAbort(registry, uiRow("preferences.diagnostics.report_after", diagnostics, "", "Report After (s)", "hang threshold seconds",
        [](SettingsContext&) { return Json(AppSettings::getDiagnosticsSettings().reportAfterSeconds); }));
    addOrAbort(registry, uiRow("preferences.diagnostics.break_lua_after", diagnostics, "", "Interrupt Runaway Lua In Play After (s)",
        "runaway lua script interrupt infinite loop play",
        [](SettingsContext&) { return Json(AppSettings::getDiagnosticsSettings().breakLuaAfterSeconds); }));
    addOrAbort(registry, uiRow("preferences.diagnostics.show_crash_notice", diagnostics, "", "Show Crash Notice On Startup",
        "crash notice banner unexpectedly closed startup",
        [](SettingsContext&) { return Json(AppSettings::getDiagnosticsSettings().showCrashNotice); }));
    addOrAbort(registry, uiRow("preferences.diagnostics.reports_folder", diagnostics, "Reports", "Crash Reports", "crash reports folder open",
        [](SettingsContext&) { return Json(diag::crash::reportsDirectory().generic_string()); }));
    addOrAbort(registry, uiRow("preferences.shader_cache", "Shader Cache", "", "Shader Cache", "clear shaders",
        [](SettingsContext&) { return Json(App::getUserShaderCacheDir().generic_string()); }));
    // S29: what the project's version control runs with, and Set Up Smart Merge (R22)
    addOrAbort(registry, uiRow("preferences.vcs.status", "Version Control", "", "Version Control", "status available provider version",
        [](SettingsContext& ctx) {
            const vcs::VcsService& service = ctx.project()->getVcs();
            return Json{{"state", vcs::vcsStateName(service.state())}, {"message", service.stateMessage()}};
        }, "The provider and its tool, or why version control is not available"));
    addOrAbort(registry, uiRow("preferences.vcs.smart_merge", "Version Control", "Merge", "Smart Merge", "merge driver scenes prefabs conflicts",
        [](SettingsContext& ctx) {
            const vcs::SnapshotPtr& snapshot = ctx.project()->getVcs().snapshot();
            return Json(snapshot && snapshot->mergeDriverMissing ? "not set up" : "");
        }, "Set Up Smart Merge: git merges scenes and prefabs with the editor's scene merge (merge-scene) instead of its text merge"));
}

} // namespace jasperengine::editor
