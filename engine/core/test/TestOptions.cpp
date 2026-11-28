// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestOptions.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace jasperengine;

namespace jasperengine::gametest {

namespace {

    bool stringList(const Json& value, std::vector<std::string>& out) {
        if (value.is_string()) {
            out.push_back(value.get<std::string>());
            return true;
        }
        if (!value.is_array()) return false;
        for (const Json& item : value) {
            if (!item.is_string()) return false;
            out.push_back(item.get<std::string>());
        }
        return true;
    }

    // one key; false + error for a refused value
    bool applyKey(CaseSpec& spec, const std::string& key, const Json& v, std::string& error) {
        {
            auto wantString = [&](std::string& field) {
                if (!v.is_string()) {
                    error = key + " must be a string";
                    return false;
                }
                field = v.get<std::string>();
                return true;
            };
            auto wantPositive = [&](int64_t low, int64_t high, int64_t& field) {
                if (!v.is_number_integer() || v.get<int64_t>() < low || v.get<int64_t>() > high) {
                    error = key + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high);
                    return false;
                }
                field = v.get<int64_t>();
                return true;
            };
            if (key == "mode") {
                const std::string mode = v.is_string() ? v.get<std::string>() : std::string();
                if (mode != "play" && mode != "edit") {
                    error = "mode is \"play\" or \"edit\"";
                    return false;
                }
                spec.mode = mode == "edit" ? Mode::Edit : Mode::Play;
            } else if (key == "scene") {
                if (!wantString(spec.scene)) return false;
            } else if (key == "data") {
                if (!wantString(spec.data)) return false;
                if (spec.data == "project") {
                    error = PROJECT_DATA_REFUSED;
                    return false;
                }
                if (spec.data != "clean" && !isSnapshotName(spec.data)) {
                    error = "data is \"clean\" or a snapshot name of Tests/Data ([A-Za-z0-9_-], at most 64 characters)";
                    return false;
                }
            } else if (key == "replay") {
                if (!wantString(spec.replay)) return false;
            } else if (key == "fixedFps") {
                int64_t fps = spec.fixedFps;
                if (!wantPositive(1, 1000, fps)) return false;
                spec.fixedFps = static_cast<int>(fps);
            } else if (key == "resolution") {
                std::string text;
                if (!wantString(text)) return false;
                int w = 0, h = 0;
                if (std::sscanf(text.c_str(), "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 16384 || h > 16384) {
                    error = "resolution is \"WIDTHxHEIGHT\", for example \"1280x720\"";
                    return false;
                }
                spec.width = w;
                spec.height = h;
            } else if (key == "timeoutFrames") {
                if (!wantPositive(1, INT64_MAX, spec.timeoutFrames)) return false;
            } else if (key == "tags") {
                std::vector<std::string> tags;
                if (!stringList(v, tags)) {
                    error = "tags is a list of strings";
                    return false;
                }
                for (const std::string& tag : tags)
                    if (std::find(spec.tags.begin(), spec.tags.end(), tag) == spec.tags.end()) spec.tags.push_back(tag);
            } else if (key == "platforms") {
                spec.platforms.clear();
                if (!stringList(v, spec.platforms)) {
                    error = "platforms is a list of strings (editor, linux, android, web...)";
                    return false;
                }
            } else if (key == "skip") {
                if (!wantString(spec.skip)) return false;
            } else if (key == "expectErrors") {
                if (!stringList(v, spec.expectErrors)) {
                    error = "expectErrors is a list of Lua patterns";
                    return false;
                }
            } else {
                error = "unknown option \"" + key + "\" (mode, scene, data, replay, fixedFps, resolution, timeoutFrames, tags, "
                        "platforms, skip, expectErrors)";
                return false;
            }
        }
        return true;
    }

    // every key is applied: a refused one does not hide the others (the first error is kept)
    bool applyOne(CaseSpec& spec, const Json& options, std::string& error) {
        if (!options.is_object()) return true;
        bool ok = true;
        for (auto it = options.begin(); it != options.end(); ++it) {
            std::string keyError;
            if (!applyKey(spec, it.key(), it.value(), keyError) && ok) {
                ok = false;
                error = keyError;
            }
        }
        return ok;
    }

}

bool isSnapshotName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

bool applyOptions(CaseSpec& spec, const Settings& settings, const Json& config, const Json& options,
                                      std::string& error) {
    spec.fixedFps = settings.fixedFps;
    spec.width = settings.width;
    spec.height = settings.height;
    spec.timeoutFrames = settings.timeoutFrames;
    spec.waitFrames = settings.waitFrames;
    spec.seed = settings.seed;
    std::string configError;
    const bool configOk = applyOne(spec, config, configError);
    std::string optionError;
    const bool optionsOk = applyOne(spec, options, optionError);
    if (!configOk) error = "Test.config: " + configError;
    else if (!optionsOk) error = optionError;
    return configOk && optionsOk;
}

}  // namespace jasperengine::gametest
