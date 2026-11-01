// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The palette layout (19 swatches a row, 4 rows, a default spectrum row) follows OpenEngine's
// Apps/Editor/Source/Panels/ColorPicker.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "SwatchLibrary.h"

#include "io/AtomicFile.h"
#include "math/CurveJson.h"
#include "widget/ColorMath.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::ordered_json;

    bool readFloat(const Json& j, float& out) {
        if (!j.is_number()) return false;
        out = j.get<float>();
        return std::isfinite(out);
    }

}

fs::path swatches::userDirectory(const fs::path& settingsDirectory) {
    return settingsDirectory / "swatches";
}

fs::path swatches::projectDirectory(const fs::path& projectPath) {
    return projectPath / "EditorSettings" / "Swatches";
}

SwatchLibrary swatches::defaultLibrary() {
    SwatchLibrary library;
    library.name = "Default";
    auto addSrgb = [&library](const char* name, const Vector3& srgb) {
        library.swatches.push_back({name, Vector4(colormath::srgbToLinear(srgb), 1.0f), false});
    };
    addSrgb("Black", Vector3(0.0f, 0.0f, 0.0f));
    addSrgb("White", Vector3(1.0f, 1.0f, 1.0f));
    addSrgb("Grey", Vector3(0.5f, 0.5f, 0.5f));
    static const char* const kHues[16] = {"Red", "Vermilion", "Orange", "Amber", "Yellow", "Lime", "Chartreuse", "Green",
                                          "Spring", "Cyan", "Azure", "Blue", "Indigo", "Violet", "Magenta", "Rose"};
    for (int i = 0; i < 16; i++) {
        addSrgb(kHues[i], colormath::hsvToRgb(Vector3((float)i / 16.0f, 1.0f, 1.0f)));
    }
    return library;
}

std::string swatches::toJson(const SwatchLibrary& library) {
    Json j = Json::object();
    j["version"] = SwatchLibrary::kVersion;
    j["name"] = library.name;
    Json list = Json::array();
    for (const Swatch& s : library.swatches) {
        Json entry = Json::object();
        entry["name"] = s.name;
        entry["linear"] = Json::array({curvejson::jsonFloat(s.linear.x), curvejson::jsonFloat(s.linear.y),
                                       curvejson::jsonFloat(s.linear.z), curvejson::jsonFloat(s.linear.w)});
        entry["hdr"] = s.hdr;
        list.push_back(std::move(entry));
    }
    j["swatches"] = std::move(list);
    Json gradients = Json::array();
    for (const GradientPreset& g : library.gradients) {
        Json entry = Json::object();
        entry["name"] = g.name;
        entry["gradient"] = curvejson::gradientToJson(g.gradient);
        gradients.push_back(std::move(entry));
    }
    j["gradients"] = std::move(gradients);
    return j.dump(2) + "\n";
}

bool swatches::fromJson(const std::string& text, SwatchLibrary& library, std::string& error) {
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "not a JSON object";
        return false;
    }
    library.swatches.clear();
    library.gradients.clear();
    library.readOnly = false;
    library.warning.clear();
    const Json& version = j["version"];
    if (!version.is_number_integer()) {
        error = "no \"version\"";
        return false;
    }
    if (version.get<int>() != SwatchLibrary::kVersion) {
        library.readOnly = true;
        library.warning = "version " + std::to_string(version.get<int>()) + " of a newer editor: read-only";
    }
    if (j.contains("name") && j["name"].is_string()) library.name = j["name"].get<std::string>();
    if (j.contains("swatches")) {
        if (!j["swatches"].is_array()) {
            error = "\"swatches\" is not an array";
            return false;
        }
        for (const Json& entry : j["swatches"]) {
            Swatch s;
            const Json& linear = entry.is_object() && entry.contains("linear") ? entry["linear"] : Json();
            if (!linear.is_array() || linear.size() != 4 || !readFloat(linear[0], s.linear.x) || !readFloat(linear[1], s.linear.y) ||
                !readFloat(linear[2], s.linear.z) || !readFloat(linear[3], s.linear.w)) {
                error = "a swatch is {name, linear: [r, g, b, a], hdr}";
                return false;
            }
            if (entry.contains("name") && entry["name"].is_string()) s.name = entry["name"].get<std::string>();
            if (entry.contains("hdr") && entry["hdr"].is_boolean()) s.hdr = entry["hdr"].get<bool>();
            if (library.swatches.size() < SwatchLibrary::kMaxSwatches) library.swatches.push_back(std::move(s));
        }
    }
    if (j.contains("gradients")) {
        if (!j["gradients"].is_array()) {
            error = "\"gradients\" is not an array";
            return false;
        }
        for (const Json& entry : j["gradients"]) {
            GradientPreset g;
            std::vector<curvejson::Issue> issues;
            if (!entry.is_object() || !entry.contains("gradient") || !curvejson::gradientFromJson(entry["gradient"], g.gradient, issues)) {
                error = "a gradient preset is {name, gradient: {colorKeys, alphaKeys}}";
                return false;
            }
            if (entry.contains("name") && entry["name"].is_string()) g.name = entry["name"].get<std::string>();
            library.gradients.push_back(std::move(g));
        }
    }
    return true;
}

bool swatches::read(const fs::path& file, LibraryScope scope, SwatchLibrary& library, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open " + file.string();
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    library = SwatchLibrary();
    library.name = file.stem().string();
    library.scope = scope;
    library.path = file;
    if (!fromJson(text.str(), library, error)) {
        error = file.filename().string() + ": " + error;
        return false;
    }
    return true;
}

bool swatches::write(const SwatchLibrary& library, std::string& error) {
    if (library.readOnly) {
        error = "\"" + library.name + "\" is read-only (" + library.warning + ")";
        return false;
    }
    if (library.path.empty()) {
        error = "\"" + library.name + "\" has no file";
        return false;
    }
    const io::WriteResult result = io::writeFileAtomic(library.path, toJson(library));
    if (!result.ok()) {
        error = result.message;
        return false;
    }
    return true;
}

std::vector<SwatchLibrary> swatches::loadAll(const fs::path& userDir, const fs::path& projectDir, std::vector<std::string>* errors) {
    std::vector<SwatchLibrary> libraries;
    auto scan = [&](const fs::path& dir, LibraryScope scope) {
        std::vector<SwatchLibrary> found;
        std::error_code ec;
        if (!dir.empty() && fs::is_directory(dir, ec)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != kExtension) continue;
                SwatchLibrary library;
                std::string error;
                if (read(entry.path(), scope, library, error)) {
                    found.push_back(std::move(library));
                } else if (errors) {
                    errors->push_back(error);
                }
            }
        }
        std::sort(found.begin(), found.end(), [](const SwatchLibrary& a, const SwatchLibrary& b) { return a.name < b.name; });
        return found;
    };
    std::vector<SwatchLibrary> user = scan(userDir, LibraryScope::User);
    if (user.empty() && !userDir.empty()) {
        SwatchLibrary seeded = defaultLibrary();
        seeded.path = libraryPath(userDir, seeded.name);
        std::string error;
        if (!write(seeded, error) && errors) errors->push_back(error);
        user.push_back(std::move(seeded));
    }
    for (SwatchLibrary& library : user) libraries.push_back(std::move(library));
    for (SwatchLibrary& library : scan(projectDir, LibraryScope::Project)) libraries.push_back(std::move(library));
    return libraries;
}

fs::path swatches::libraryPath(const fs::path& directory, const std::string& name) {
    std::string file;
    for (char c : name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                         (unsigned char)c < 32;
        file.push_back(bad ? '_' : c);
    }
    while (!file.empty() && (file.back() == ' ' || file.back() == '.')) file.pop_back();
    if (file.empty() || file.front() == '.') file.insert(file.begin(), '_');
    return directory / (file + kExtension);
}

namespace {

    bool editable(const SwatchLibrary& library, std::string& error) {
        if (library.readOnly) {
            error = "\"" + library.name + "\" is read-only (" + library.warning + ")";
            return false;
        }
        return true;
    }

    bool inRange(const SwatchLibrary& library, size_t index, std::string& error) {
        if (index >= library.swatches.size()) {
            error = "no swatch " + std::to_string(index) + " (\"" + library.name + "\" has " + std::to_string(library.swatches.size()) + ")";
            return false;
        }
        return true;
    }

}

bool swatches::add(SwatchLibrary& library, const Swatch& swatch, std::string& error) {
    if (!editable(library, error)) return false;
    if (library.swatches.size() >= SwatchLibrary::kMaxSwatches) {
        error = "\"" + library.name + "\" is full (" + std::to_string(SwatchLibrary::kMaxSwatches) + " swatches)";
        return false;
    }
    library.swatches.push_back(swatch);
    return true;
}

bool swatches::remove(SwatchLibrary& library, size_t index, std::string& error) {
    if (!editable(library, error) || !inRange(library, index, error)) return false;
    library.swatches.erase(library.swatches.begin() + (std::ptrdiff_t)index);
    return true;
}

bool swatches::replace(SwatchLibrary& library, size_t index, const Vector4& linear, bool hdr, std::string& error) {
    if (!editable(library, error) || !inRange(library, index, error)) return false;
    library.swatches[index].linear = linear;
    library.swatches[index].hdr = hdr;
    return true;
}

bool swatches::rename(SwatchLibrary& library, size_t index, const std::string& name, std::string& error) {
    if (!editable(library, error) || !inRange(library, index, error)) return false;
    library.swatches[index].name = name;
    return true;
}

bool swatches::move(SwatchLibrary& library, size_t index, int direction, std::string& error) {
    if (!editable(library, error) || !inRange(library, index, error)) return false;
    const long to = (long)index + (direction < 0 ? -1 : 1);
    if (to < 0 || (size_t)to >= library.swatches.size()) {
        error = "the swatch is already at the end";
        return false;
    }
    std::swap(library.swatches[index], library.swatches[(size_t)to]);
    return true;
}

bool swatches::createLibrary(const fs::path& directory, LibraryScope scope, const std::string& name, SwatchLibrary& library,
                             std::string& error) {
    if (name.empty()) {
        error = "a library needs a name";
        return false;
    }
    const fs::path file = libraryPath(directory, name);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        error = "a library \"" + name + "\" exists";
        return false;
    }
    library = SwatchLibrary();
    library.name = name;
    library.scope = scope;
    library.path = file;
    return write(library, error);
}

bool swatches::renameLibrary(SwatchLibrary& library, const std::string& name, std::string& error) {
    if (!editable(library, error)) return false;
    if (name.empty()) {
        error = "a library needs a name";
        return false;
    }
    const fs::path file = libraryPath(library.path.parent_path(), name);
    std::error_code ec;
    if (file != library.path && fs::exists(file, ec)) {
        error = "a library \"" + name + "\" exists";
        return false;
    }
    const fs::path old = library.path;
    library.name = name;
    library.path = file;
    if (!write(library, error)) {
        library.path = old;
        return false;
    }
    if (old != file) fs::remove(old, ec);
    return true;
}

bool swatches::deleteLibrary(const SwatchLibrary& library, std::string& error) {
    std::error_code ec;
    if (!fs::remove(library.path, ec) || ec) {
        error = "cannot delete " + library.path.string() + (ec ? ": " + ec.message() : std::string());
        return false;
    }
    return true;
}
