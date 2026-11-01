// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Normalised presets mapped by the host and the "My Preset N" naming follow OpenEngine's
// Apps/Editor/Source/UI/Controls/CurvePresets.cpp, Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License. See NOTICE.md.

#include "CurvePresets.h"

#include "io/AtomicFile.h"
#include "math/CurveJson.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::ordered_json;

    CurvePreset preset(const char* name, std::vector<CurveKey> keys) {
        return {name, Curve(std::move(keys))};
    }

}

const CurvePresetLibrary& curvepresets::builtIn() {
    static const CurvePresetLibrary library = [] {
        CurvePresetLibrary l;
        l.name = "Built-in";
        l.scope = CurveLibraryScope::BuiltIn;
        l.readOnly = true;
        l.presets = {
            preset("Constant", {{0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}}),
            preset("Linear", {{0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}}),
            preset("Linear Down", {{0.0f, 1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, -1.0f, -1.0f}}),
            preset("Ease In", {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 2.0f, 2.0f}}),
            preset("Ease Out", {{0.0f, 0.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 0.0f, 0.0f}}),
            preset("Ease In Out", {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}}),
            preset("Ease In Out Down", {{0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}),
            preset("Bell", {{0.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}),
        };
        return l;
    }();
    return library;
}

fs::path curvepresets::userDirectory(const fs::path& settingsDirectory) {
    return settingsDirectory / "curves";
}

fs::path curvepresets::projectDirectory(const fs::path& projectPath) {
    return projectPath / "EditorSettings" / "Curves";
}

std::string curvepresets::toJson(const CurvePresetLibrary& library) {
    Json j = Json::object();
    j["version"] = CurvePresetLibrary::kVersion;
    j["name"] = library.name;
    Json presets = Json::array();
    for (const CurvePreset& p : library.presets) {
        Json entry = Json::object();
        entry["name"] = p.name;
        entry["curve"] = curvejson::curveToJson(p.curve);
        presets.push_back(std::move(entry));
    }
    j["presets"] = std::move(presets);
    return j.dump(2) + "\n";
}

bool curvepresets::fromJson(const std::string& text, CurvePresetLibrary& library, std::string& error) {
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "not a JSON object";
        return false;
    }
    if (!j.contains("version") || !j["version"].is_number_integer()) {
        error = "no \"version\"";
        return false;
    }
    library.presets.clear();
    library.readOnly = false;
    library.warning.clear();
    if (j["version"].get<int>() != CurvePresetLibrary::kVersion) {
        library.readOnly = true;
        library.warning = "version " + std::to_string(j["version"].get<int>()) + " of a newer editor: read-only";
    }
    if (j.contains("name") && j["name"].is_string()) library.name = j["name"].get<std::string>();
    if (j.contains("presets")) {
        if (!j["presets"].is_array()) {
            error = "\"presets\" is not an array";
            return false;
        }
        for (const Json& entry : j["presets"]) {
            CurvePreset p;
            std::vector<curvejson::Issue> issues;
            if (!entry.is_object() || !entry.contains("curve") || !curvejson::curveFromJson(entry["curve"], p.curve, issues)) {
                error = "a preset is {name, curve: [[time, value, inTangent, outTangent]]}";
                return false;
            }
            if (entry.contains("name") && entry["name"].is_string()) p.name = entry["name"].get<std::string>();
            library.presets.push_back(std::move(p));
        }
    }
    return true;
}

bool curvepresets::read(const fs::path& file, CurveLibraryScope scope, CurvePresetLibrary& library, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open " + file.string();
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    library = CurvePresetLibrary();
    library.name = file.stem().string();
    library.scope = scope;
    library.path = file;
    if (!fromJson(text.str(), library, error)) {
        error = file.filename().string() + ": " + error;
        return false;
    }
    return true;
}

bool curvepresets::write(const CurvePresetLibrary& library, std::string& error) {
    if (library.readOnly || library.path.empty()) {
        error = "\"" + library.name + "\" is read-only" + (library.warning.empty() ? std::string() : " (" + library.warning + ")");
        return false;
    }
    const io::WriteResult result = io::writeFileAtomic(library.path, toJson(library));
    if (!result.ok()) {
        error = result.message;
        return false;
    }
    return true;
}

std::vector<CurvePresetLibrary> curvepresets::loadAll(const fs::path& userDir, const fs::path& projectDir, std::vector<std::string>* errors) {
    std::vector<CurvePresetLibrary> libraries{builtIn()};
    auto scan = [&](const fs::path& dir, CurveLibraryScope scope) {
        std::vector<CurvePresetLibrary> found;
        std::error_code ec;
        if (!dir.empty() && fs::is_directory(dir, ec)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != kExtension) continue;
                CurvePresetLibrary library;
                std::string error;
                if (read(entry.path(), scope, library, error)) found.push_back(std::move(library));
                else if (errors) errors->push_back(error);
            }
        }
        std::sort(found.begin(), found.end(), [](const CurvePresetLibrary& a, const CurvePresetLibrary& b) { return a.name < b.name; });
        return found;
    };
    std::vector<CurvePresetLibrary> user = scan(userDir, CurveLibraryScope::User);
    if (user.empty() && !userDir.empty()) {
        CurvePresetLibrary mine;
        mine.name = "My Presets";
        mine.scope = CurveLibraryScope::User;
        mine.path = userDir / (std::string("My Presets") + kExtension);
        std::string error;
        if (!write(mine, error) && errors) errors->push_back(error);
        user.push_back(std::move(mine));
    }
    for (CurvePresetLibrary& l : user) libraries.push_back(std::move(l));
    for (CurvePresetLibrary& l : scan(projectDir, CurveLibraryScope::Project)) libraries.push_back(std::move(l));
    return libraries;
}

std::string curvepresets::nextPresetName(const CurvePresetLibrary& library) {
    for (int n = 1;; n++) {
        const std::string name = "My Preset " + std::to_string(n);
        const bool taken = std::any_of(library.presets.begin(), library.presets.end(), [&](const CurvePreset& p) { return p.name == name; });
        if (!taken) return name;
    }
}

bool curvepresets::addPreset(CurvePresetLibrary& library, const std::string& name, const Curve& normalised, std::string& error) {
    if (library.readOnly || library.path.empty()) {
        error = "\"" + library.name + "\" is read-only";
        return false;
    }
    library.presets.push_back({name.empty() ? nextPresetName(library) : name, normalised});
    if (!write(library, error)) {
        library.presets.pop_back();
        return false;
    }
    return true;
}

bool curvepresets::removePreset(CurvePresetLibrary& library, size_t index, std::string& error) {
    if (library.readOnly || library.path.empty()) {
        error = "\"" + library.name + "\" is read-only";
        return false;
    }
    if (index >= library.presets.size()) {
        error = "no preset " + std::to_string(index);
        return false;
    }
    const CurvePreset removed = library.presets[index];
    library.presets.erase(library.presets.begin() + (std::ptrdiff_t)index);
    if (!write(library, error)) {
        library.presets.insert(library.presets.begin() + (std::ptrdiff_t)index, removed);
        return false;
    }
    return true;
}
