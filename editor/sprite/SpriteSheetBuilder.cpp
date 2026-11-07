// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSheetBuilder.h"

#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"

#include "io/AtomicFile.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::sprite {

    namespace {

        using pixelart::PixelProblem;

        struct PresetName {
            PivotPreset preset;
            const char* name;
        };
        constexpr PresetName kPresets[] = {
            {PivotPreset::CENTER, "center"}, {PivotPreset::TOP_CENTER, "top_center"}, {PivotPreset::BOTTOM_CENTER, "bottom_center"},
            {PivotPreset::LEFT_CENTER, "left_center"}, {PivotPreset::RIGHT_CENTER, "right_center"}, {PivotPreset::TOP_LEFT, "top_left"},
            {PivotPreset::BOTTOM_LEFT, "bottom_left"}, {PivotPreset::TOP_RIGHT, "top_right"}, {PivotPreset::BOTTOM_RIGHT, "bottom_right"},
        };

        // [a, b] of integers
        bool intPair(const YAML::Node& node, int& a, int& b) {
            if (!node || !node.IsSequence() || node.size() != 2) return false;
            try {
                a = node[0].as<int>();
                b = node[1].as<int>();
            } catch (const std::exception&) {
                return false;
            }
            return true;
        }

        bool intList(const YAML::Node& node, size_t n, int* out) {
            if (!node || !node.IsSequence() || node.size() != n) return false;
            try {
                for (size_t i = 0; i < n; i++) out[i] = node[i].as<int>();
            } catch (const std::exception&) {
                return false;
            }
            return true;
        }

        YAML::Node flowList(std::initializer_list<int> values) {
            YAML::Node list(YAML::NodeType::Sequence);
            list.SetStyle(YAML::EmitterStyle::Flow);
            for (int v : values) list.push_back(v);
            return list;
        }

        bool jsonInts(const nlohmann::json& j, size_t n, int* out) {
            if (!j.is_array() || j.size() != n) return false;
            for (size_t i = 0; i < n; i++) {
                if (!j[i].is_number()) return false;
                const double v = j[i].get<double>();
                if (v != std::floor(v) || std::fabs(v) > 1e7) return false;
                out[i] = static_cast<int>(v);
            }
            return true;
        }

        PixelProblem invalid(const std::string& field, const std::string& message, const std::string& fix) {
            return {"invalid_settings", field, message, fix};
        }

    }

    const char* spriteModeName(TextureSpriteMode mode) {
        switch (mode) {
            case TextureSpriteMode::GRID: return "grid";
            case TextureSpriteMode::MULTIPLE: return "multiple";
            case TextureSpriteMode::SINGLE: break;
        }
        return "single";
    }

    bool spriteModeFromName(const std::string& name, TextureSpriteMode& out) {
        for (TextureSpriteMode mode : {TextureSpriteMode::SINGLE, TextureSpriteMode::GRID, TextureSpriteMode::MULTIPLE}) {
            if (name == spriteModeName(mode)) {
                out = mode;
                return true;
            }
        }
        return false;
    }

    const char* pivotPresetName(PivotPreset preset) {
        for (const PresetName& p : kPresets) {
            if (p.preset == preset) return p.name;
        }
        return "center";
    }

    bool pivotPresetFromName(const std::string& name, PivotPreset& out) {
        for (const PresetName& p : kPresets) {
            if (name == p.name) {
                out = p.preset;
                return true;
            }
        }
        return false;
    }

    bool sheetSettingsFromYaml(const YAML::Node& node, TextureSheetSettings& out, std::vector<PixelProblem>& problems) {
        out = TextureSheetSettings();
        if (!node || !node.IsMap()) {
            problems.push_back(invalid("spriteSheet", "import.spriteSheet is not a map", "set the Sprite Mode again in the texture's import settings"));
            return false;
        }
        int version = 1;
        try {
            if (node["version"]) version = node["version"].as<int>();
        } catch (const std::exception&) {
            version = 0;
        }
        if (version > TextureSheetSettings::kVersion) {
            problems.push_back(invalid("spriteSheet.version", "import.spriteSheet is version " + std::to_string(version) + ", newer than this editor reads (" +
                                       std::to_string(TextureSheetSettings::kVersion) + "): read-only", "open the project with a newer editor"));
            return false;
        }
        if (node["mode"]) {
            const std::string mode = node["mode"].as<std::string>("");
            if (!spriteModeFromName(mode, out.mode)) {
                problems.push_back(invalid("spriteSheet.mode", "unknown Sprite Mode \"" + mode + "\"", "single, grid or multiple"));
            }
        }
        if (const YAML::Node grid = node["grid"]) {
            SpriteGridSettings& g = out.grid;
            if (grid["cell"] && !intPair(grid["cell"], g.cellWidth, g.cellHeight)) problems.push_back(invalid("spriteSheet.grid.cell", "cell is [width, height]", ""));
            if (grid["offset"] && !intPair(grid["offset"], g.offsetX, g.offsetY)) problems.push_back(invalid("spriteSheet.grid.offset", "offset is [x, y]", ""));
            if (grid["spacing"] && !intPair(grid["spacing"], g.spacingX, g.spacingY)) problems.push_back(invalid("spriteSheet.grid.spacing", "spacing is [x, y]", ""));
            try {
                if (grid["count"]) g.count = std::max(0, grid["count"].as<int>());
                if (grid["skipEmpty"]) g.skipEmpty = grid["skipEmpty"].as<bool>();
                if (grid["prefix"]) g.prefix = grid["prefix"].as<std::string>();
            } catch (const std::exception&) {
                problems.push_back(invalid("spriteSheet.grid", "count is an integer, skipEmpty a boolean, prefix a string", ""));
            }
        }
        if (const YAML::Node frames = node["frames"]) {
            for (size_t i = 0; frames.IsSequence() && i < frames.size(); i++) {
                const YAML::Node f = frames[i];
                const std::string field = "spriteSheet.frames[" + std::to_string(i) + "]";
                int r[4] = {0, 0, 0, 0};
                if (!f.IsMap() || !intList(f["rect"], 4, r)) {
                    problems.push_back(invalid(field, "a frame needs rect [x, y, width, height] (left out)", ""));
                    continue;
                }
                TextureSheetFrame frame;
                frame.name = f["name"] ? f["name"].as<std::string>("") : std::string();
                frame.rect = Rect(static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]), static_cast<float>(r[3]));
                int p[2] = {0, 0};
                if (f["pivot"] && intList(f["pivot"], 2, p)) {
                    frame.hasPivot = true;
                    frame.pivot = Vector2(static_cast<float>(p[0]), static_cast<float>(p[1]));
                }
                int b[4] = {0, 0, 0, 0};
                if (f["border"] && intList(f["border"], 4, b)) {
                    frame.border = Vector4(static_cast<float>(std::max(0, b[0])), static_cast<float>(std::max(0, b[1])),
                                           static_cast<float>(std::max(0, b[2])), static_cast<float>(std::max(0, b[3])));
                }
                out.frames.push_back(frame);
            }
        }
        if (const YAML::Node pivot = node["pivot"]) {
            if (pivot.IsScalar()) {
                if (!pivotPresetFromName(pivot.as<std::string>(""), out.pivotPreset)) {
                    problems.push_back(invalid("spriteSheet.pivot", "unknown pivot \"" + pivot.as<std::string>("") + "\"", "a preset (bottom_center...) or [x, y] normalized"));
                }
            } else if (pivot.IsSequence() && pivot.size() == 2) {
                try {
                    out.customPivot = true;
                    out.pivotNormalized = Vector2(pivot[0].as<float>(), pivot[1].as<float>());
                } catch (const std::exception&) {
                    out.customPivot = false;
                    problems.push_back(invalid("spriteSheet.pivot", "pivot [x, y] takes numbers", ""));
                }
            }
        }
        return true;
    }

    YAML::Node sheetSettingsToYaml(const TextureSheetSettings& s) {
        YAML::Node node(YAML::NodeType::Map);
        node["version"] = TextureSheetSettings::kVersion;
        node["mode"] = spriteModeName(s.mode);
        if (s.mode == TextureSpriteMode::GRID) {
            YAML::Node grid(YAML::NodeType::Map);
            grid.SetStyle(YAML::EmitterStyle::Flow);
            grid["cell"] = flowList({s.grid.cellWidth, s.grid.cellHeight});
            grid["offset"] = flowList({s.grid.offsetX, s.grid.offsetY});
            grid["spacing"] = flowList({s.grid.spacingX, s.grid.spacingY});
            grid["count"] = s.grid.count;
            grid["skipEmpty"] = s.grid.skipEmpty;
            grid["prefix"] = s.grid.prefix;
            node["grid"] = grid;
        }
        if (s.mode == TextureSpriteMode::MULTIPLE) {
            YAML::Node frames(YAML::NodeType::Sequence);
            for (const TextureSheetFrame& f : s.frames) {
                YAML::Node frame(YAML::NodeType::Map);
                frame.SetStyle(YAML::EmitterStyle::Flow);
                Rect r = f.rect;
                frame["name"] = f.name;
                frame["rect"] = flowList({static_cast<int>(r.getX()), static_cast<int>(r.getY()), static_cast<int>(r.getWidth()), static_cast<int>(r.getHeight())});
                if (f.hasPivot) frame["pivot"] = flowList({static_cast<int>(f.pivot.x), static_cast<int>(f.pivot.y)});
                if (f.border.x > 0 || f.border.y > 0 || f.border.z > 0 || f.border.w > 0) {
                    frame["border"] = flowList({static_cast<int>(f.border.x), static_cast<int>(f.border.y), static_cast<int>(f.border.z), static_cast<int>(f.border.w)});
                }
                frames.push_back(frame);
            }
            node["frames"] = frames;
        }
        if (s.customPivot) {
            YAML::Node pivot(YAML::NodeType::Sequence);
            pivot.SetStyle(YAML::EmitterStyle::Flow);
            pivot.push_back(s.pivotNormalized.x);
            pivot.push_back(s.pivotNormalized.y);
            node["pivot"] = pivot;
        } else {
            node["pivot"] = pivotPresetName(s.pivotPreset);
        }
        return node;
    }

    bool sheetSettingsFromJson(const nlohmann::json& j, const TextureSheetSettings& base, TextureSheetSettings& out, std::vector<PixelProblem>& problems) {
        out = base;
        if (!j.is_object()) {
            problems.push_back(invalid("sprite_mode", "sprite_mode is an object {mode, grid, frames, pivot}", "see jasper://docs/sprites (Sheets)"));
            return false;
        }
        const size_t before = problems.size();
        for (const auto& [key, value] : j.items()) {
            if (key == "mode") {
                if (!value.is_string() || !spriteModeFromName(value.get<std::string>(), out.mode)) {
                    problems.push_back(invalid("mode", "mode is single, grid or multiple", ""));
                }
            } else if (key == "grid") {
                if (!value.is_object()) {
                    problems.push_back(invalid("grid", "grid is an object {cell, offset, spacing, count, skip_empty, prefix}", ""));
                    continue;
                }
                SpriteGridSettings& g = out.grid;
                for (const auto& [gk, gv] : value.items()) {
                    int v[2] = {0, 0};
                    if (gk == "cell" || gk == "offset" || gk == "spacing") {
                        if (!jsonInts(gv, 2, v) || (gk == "cell" && (v[0] < 1 || v[1] < 1)) || v[0] < 0 || v[1] < 0) {
                            problems.push_back(invalid("grid." + gk, "grid." + gk + " is [x, y] in pixels" + std::string(gk == "cell" ? ", at least 1" : ", not negative"), ""));
                            continue;
                        }
                        if (gk == "cell") { g.cellWidth = v[0]; g.cellHeight = v[1]; }
                        if (gk == "offset") { g.offsetX = v[0]; g.offsetY = v[1]; }
                        if (gk == "spacing") { g.spacingX = v[0]; g.spacingY = v[1]; }
                    } else if (gk == "count" && gv.is_number_integer() && gv.get<int>() >= 0) {
                        g.count = gv.get<int>();
                    } else if (gk == "skip_empty" && gv.is_boolean()) {
                        g.skipEmpty = gv.get<bool>();
                    } else if (gk == "prefix" && gv.is_string()) {
                        g.prefix = gv.get<std::string>();
                    } else {
                        problems.push_back(invalid("grid." + gk, "grid takes cell, offset, spacing ([x, y]), count (>= 0), skip_empty (boolean) and prefix (string)", ""));
                    }
                }
            } else if (key == "frames") {
                if (!value.is_array()) {
                    problems.push_back(invalid("frames", "frames is a list of {name, rect: [x, y, w, h], pivot?: [x, y], border?: [l, t, r, b]}", ""));
                    continue;
                }
                out.frames.clear();
                for (size_t i = 0; i < value.size(); i++) {
                    const nlohmann::json& f = value[i];
                    const std::string field = "frames[" + std::to_string(i) + "]";
                    int r[4] = {0, 0, 0, 0};
                    if (!f.is_object() || !f.contains("rect") || !jsonInts(f["rect"], 4, r) || r[2] < 1 || r[3] < 1) {
                        problems.push_back(invalid(field, field + " needs rect [x, y, width, height] in pixels from the image's top-left", ""));
                        continue;
                    }
                    TextureSheetFrame frame;
                    frame.name = f.value("name", std::string());
                    frame.rect = Rect(static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]), static_cast<float>(r[3]));
                    int p[2] = {0, 0};
                    if (f.contains("pivot")) {
                        if (!jsonInts(f["pivot"], 2, p)) {
                            problems.push_back(invalid(field + ".pivot", "pivot is [x, y] pixels from the frame's top-left", ""));
                        } else {
                            frame.hasPivot = true;
                            frame.pivot = Vector2(static_cast<float>(p[0]), static_cast<float>(p[1]));
                        }
                    }
                    int b[4] = {0, 0, 0, 0};
                    if (f.contains("border")) {
                        if (!jsonInts(f["border"], 4, b) || b[0] < 0 || b[1] < 0 || b[2] < 0 || b[3] < 0 || b[0] + b[2] > r[2] || b[1] + b[3] > r[3]) {
                            problems.push_back(invalid(field + ".border", "border is [left, top, right, bottom] pixels inside the frame", ""));
                        } else {
                            frame.border = Vector4(static_cast<float>(b[0]), static_cast<float>(b[1]), static_cast<float>(b[2]), static_cast<float>(b[3]));
                        }
                    }
                    out.frames.push_back(frame);
                }
            } else if (key == "pivot") {
                if (value.is_string()) {
                    out.customPivot = false;
                    if (!pivotPresetFromName(value.get<std::string>(), out.pivotPreset)) {
                        problems.push_back(invalid("pivot", "pivot is a preset (center, bottom_center, ...) or [x, y] normalized from the bottom-left", ""));
                    }
                } else if (value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number()) {
                    out.customPivot = true;
                    out.pivotNormalized = Vector2(value[0].get<float>(), value[1].get<float>());
                } else {
                    problems.push_back(invalid("pivot", "pivot is a preset (center, bottom_center, ...) or [x, y] normalized from the bottom-left", ""));
                }
            } else {
                problems.push_back(invalid(key, "unknown key " + key + ": sprite_mode takes mode, grid, frames, pivot", "see jasper://docs/sprites (Sheets)"));
            }
        }
        return problems.size() == before;
    }

    nlohmann::json sheetSettingsToJson(const TextureSheetSettings& s) {
        nlohmann::json j = {{"mode", spriteModeName(s.mode)}};
        if (s.mode == TextureSpriteMode::GRID) {
            j["grid"] = {{"cell", {s.grid.cellWidth, s.grid.cellHeight}}, {"offset", {s.grid.offsetX, s.grid.offsetY}},
                         {"spacing", {s.grid.spacingX, s.grid.spacingY}}, {"count", s.grid.count}, {"skip_empty", s.grid.skipEmpty},
                         {"prefix", s.grid.prefix}};
        }
        if (s.mode == TextureSpriteMode::MULTIPLE) {
            nlohmann::json frames = nlohmann::json::array();
            for (const TextureSheetFrame& f : s.frames) {
                Rect r = f.rect;
                nlohmann::json frame = {{"name", f.name}, {"rect", {static_cast<int>(r.getX()), static_cast<int>(r.getY()), static_cast<int>(r.getWidth()), static_cast<int>(r.getHeight())}}};
                if (f.hasPivot) frame["pivot"] = {static_cast<int>(f.pivot.x), static_cast<int>(f.pivot.y)};
                if (f.border.x > 0 || f.border.y > 0 || f.border.z > 0 || f.border.w > 0) {
                    frame["border"] = {static_cast<int>(f.border.x), static_cast<int>(f.border.y), static_cast<int>(f.border.z), static_cast<int>(f.border.w)};
                }
                frames.push_back(frame);
            }
            j["frames"] = frames;
        }
        if (s.customPivot) j["pivot"] = {s.pivotNormalized.x, s.pivotNormalized.y};
        else j["pivot"] = pivotPresetName(s.pivotPreset);
        return j;
    }

    bool framesFromAsepriteJson(const std::string& text, std::vector<TextureSheetFrame>& out, PixelProblem& problem) {
        out.clear();
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || !root.contains("frames")) {
            problem = {"invalid_settings", "json", "not Aseprite --data JSON (no frames)", "export it with Aseprite's --data (json-hash or json-array)"};
            return false;
        }
        auto add = [&](const std::string& name, const nlohmann::json& f) {
            int r[4] = {0, 0, 0, 0};
            const nlohmann::json& rect = f.contains("frame") ? f["frame"] : nlohmann::json();
            if (!rect.is_object()) return;
            r[0] = rect.value("x", 0); r[1] = rect.value("y", 0); r[2] = rect.value("w", 0); r[3] = rect.value("h", 0);
            if (r[2] <= 0 || r[3] <= 0) return;
            TextureSheetFrame frame;
            // Aseprite names frames "<file> <i>.<ext>": the extension is not part of the name
            frame.name = name;
            const size_t dot = frame.name.rfind('.');
            if (dot != std::string::npos && dot > 0 && frame.name.find(' ') != std::string::npos) frame.name = frame.name.substr(0, dot);
            frame.rect = Rect(static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]), static_cast<float>(r[3]));
            out.push_back(frame);
        };
        const nlohmann::json& frames = root["frames"];
        if (frames.is_object()) {
            // json-hash: the object keeps Aseprite's order (nlohmann::json sorts keys, so read it ordered)
            const nlohmann::ordered_json ordered = nlohmann::ordered_json::parse(text, nullptr, false);
            for (const auto& [name, f] : ordered["frames"].items()) add(name, nlohmann::json::parse(f.dump()));
        } else if (frames.is_array()) {
            for (const nlohmann::json& f : frames) add(f.value("filename", std::string("frame_") + std::to_string(out.size())), f);
        }
        // 9-patch slices name their frame by its index (Aseprite's meta.slices; key frame 0)
        if (root.contains("meta") && root["meta"].is_object() && root["meta"].contains("slices") && root["meta"]["slices"].is_array()) {
            for (const nlohmann::json& slice : root["meta"]["slices"]) {
                if (!slice.contains("keys") || !slice["keys"].is_array()) continue;
                for (const nlohmann::json& key : slice["keys"]) {
                    const int index = key.value("frame", -1);
                    if (index < 0 || index >= static_cast<int>(out.size())) continue;
                    if (key.contains("center") && key["center"].is_object() && key.contains("bounds") && key["bounds"].is_object()) {
                        const nlohmann::json& b = key["bounds"];
                        const nlohmann::json& c = key["center"];
                        const int bw = b.value("w", 0), bh = b.value("h", 0);
                        const int cx = c.value("x", 0), cy = c.value("y", 0), cw = c.value("w", 0), ch = c.value("h", 0);
                        out[static_cast<size_t>(index)].border = Vector4(static_cast<float>(b.value("x", 0) + cx), static_cast<float>(b.value("y", 0) + cy),
                                                                         static_cast<float>(std::max(0, bw - cx - cw)), static_cast<float>(std::max(0, bh - cy - ch)));
                    }
                    if (key.contains("pivot") && key["pivot"].is_object() && key.contains("bounds")) {
                        out[static_cast<size_t>(index)].hasPivot = true;
                        out[static_cast<size_t>(index)].pivot = Vector2(static_cast<float>(key["bounds"].value("x", 0) + key["pivot"].value("x", 0)),
                                                                        static_cast<float>(key["bounds"].value("y", 0) + key["pivot"].value("y", 0)));
                    }
                }
            }
        }
        if (out.empty()) {
            problem = {"invalid_settings", "json", "the JSON lists no frames with a size", "export it with Aseprite's --data"};
            return false;
        }
        return true;
    }

    TextureSheetInfo textureSheetSettings(const fs::path& absoluteTexture) {
        TextureSheetInfo info;
        const YAML::Node node = SetAssetMetaCmd::read(absoluteTexture, kSpriteSheetKey);
        if (node && !node.IsNull()) {
            info.source = TextureSheetInfo::Source::Meta;
            if (!sheetSettingsFromYaml(node, info.settings, info.problems)) {
                info.readOnly = true;
                info.settings = TextureSheetSettings();
            }
            SpriteSheetSources::parseCellSize(absoluteTexture.generic_string(), info.cellWidth, info.cellHeight);
            return info;
        }
        std::string base;
        if (SpriteSheetSources::parseCellSize(absoluteTexture.generic_string(), info.cellWidth, info.cellHeight, &base)) {
            info.source = TextureSheetInfo::Source::FileName;
            info.settings = SpriteSheetSources::cellSizeSettings(info.cellWidth, info.cellHeight, base);
        }
        return info;
    }

    TextureSheetBuild buildTextureSheet(const fs::path& absoluteTexture, const std::string& assetPath, const TextureSheetSettings& settings,
                                        const pixelart::PixelImage* image) {
        TextureSheetBuild out;
        std::optional<pixelart::PixelImage> loaded;
        if (!image) {
            std::string error;
            loaded = pixelart::PixelImage::loadPng(absoluteTexture, &error);
            if (!loaded) {
                out.error = {"not_a_sheet", "texture", assetPath + ": " + error, "save the texture as an image this editor reads"};
                return out;
            }
            image = &*loaded;
        }
        out.width = image->width();
        out.height = image->height();
        out.sheet = SpriteSheetSources::buildTextureSheet(assetPath, out.width, out.height, settings, [image](int x, int y, int w, int h) {
            const pixelart::IntRect r = pixelart::IntRect{x, y, w, h}.intersect(image->bounds());
            for (int yy = r.y; yy < r.y + r.h; yy++) {
                const uint8_t* row = image->row(yy);
                for (int xx = r.x; xx < r.x + r.w; xx++) {
                    if (row[xx * 4 + 3] != 0) return true;
                }
            }
            return false;
        }, &out.warnings);
        out.ok = true;
        return out;
    }

    SpriteSheetBuilder& SpriteSheetBuilder::get() {
        static SpriteSheetBuilder* builder = new SpriteSheetBuilder();
        return *builder;
    }

    void SpriteSheetBuilder::attach(Project* newProject) {
        std::lock_guard<std::mutex> lock(mutex);
        project = newProject;
        memos.clear();
        assetsRoot = project ? project->getAssetsPath() : fs::path();
        cacheRoot = project ? project->getProjectInternalPath() / "cache" / "imported" / "v1" / kCacheDir : fs::path();
    }

    void SpriteSheetBuilder::invalidate(const std::string& assetPath) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            memos.erase(assetPath);
        }
        SpriteSheetPool::invalidate(assetPath);
    }

    bool SpriteSheetBuilder::resolve(const std::string& assetPath, std::string& fileOut) {
        fs::path assets, cache;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!project) return false;
            assets = assetsRoot;
            cache = cacheRoot;
        }
        if (!SpriteSheetSources::isTexturePath(assetPath)) return false;
        const fs::path absolute = (assets / assetPath).lexically_normal();
        std::error_code ec;
        Memo stamp;
        stamp.textureTime = fs::last_write_time(absolute, ec);
        if (ec) return false;
        stamp.textureSize = fs::file_size(absolute, ec);
        const fs::path meta = SetAssetMetaCmd::metaPathOf(absolute);
        std::error_code metaEc;
        stamp.metaTime = fs::last_write_time(meta, metaEc);
        stamp.hasMeta = !metaEc;
        if (stamp.hasMeta) stamp.metaSize = fs::file_size(meta, metaEc);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = memos.find(assetPath);
            if (it != memos.end() && it->second.textureTime == stamp.textureTime && it->second.textureSize == stamp.textureSize &&
                it->second.hasMeta == stamp.hasMeta && (!stamp.hasMeta || (it->second.metaTime == stamp.metaTime && it->second.metaSize == stamp.metaSize))) {
                if (it->second.file.empty()) return false;
                fileOut = it->second.file;
                return true;
            }
        }
        // a name@WxH file without a meta needs no meta read
        int cw = 0, ch = 0;
        if (!stamp.hasMeta && !SpriteSheetSources::parseCellSize(assetPath, cw, ch)) {
            std::lock_guard<std::mutex> lock(mutex);
            memos[assetPath] = stamp;
            return false;
        }
        const TextureSheetInfo info = textureSheetSettings(absolute);
        if (info.isSheet()) {
            std::ifstream in(absolute, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string key = sha256Hex(sha256Hex(bytes) + "|path " + assetPath + "|" + YAML::Dump(sheetSettingsToYaml(info.settings)) +
                                              "|sheet " + std::to_string(SPRITESHEET_VERSION) + "|v1");
            const fs::path file = cache / key.substr(0, 2) / (key + ".json");
            if (!fs::exists(file, ec)) {
                std::optional<pixelart::PixelImage> image = pixelart::PixelImage::decodePng(
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), nullptr);
                if (!image) return false;
                TextureSheetBuild build = buildTextureSheet(absolute, assetPath, info.settings, &*image);
                if (!build.ok) return false;
                io::AtomicWriteOptions options;
                options.durability = io::Durability::Cache;
                if (!io::writeFileAtomic(file, writeSheet(build.sheet), options).ok()) return false;
            }
            stamp.file = file.string();
        }
        std::lock_guard<std::mutex> lock(mutex);
        memos[assetPath] = stamp;
        if (stamp.file.empty()) return false;
        fileOut = stamp.file;
        return true;
    }

}
