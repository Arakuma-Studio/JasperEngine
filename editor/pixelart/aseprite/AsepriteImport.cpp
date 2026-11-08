// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteImport.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "sprite/SpritePacker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <set>
#include <sstream>

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        const char* modeName(ImportMode m) { return m == ImportMode::LayersAsSprites ? "layers_as_sprites" : "sprite_sheet"; }
        const char* pivotSourceName(PivotSource s) {
            return s == PivotSource::Preset ? "preset" : s == PivotSource::Custom ? "custom" : "slice";
        }
        const char* layoutName(SheetLayout l) { return l == SheetLayout::Grid ? "grid" : "packed"; }
        const char* compressionName(SheetCompression c) { return c == SheetCompression::Project ? "project" : "none"; }

        const std::vector<std::pair<const char*, PivotPreset>>& presetNames() {
            static const std::vector<std::pair<const char*, PivotPreset>> names = {
                {"center", PivotPreset::CENTER}, {"top_center", PivotPreset::TOP_CENTER}, {"bottom_center", PivotPreset::BOTTOM_CENTER},
                {"left_center", PivotPreset::LEFT_CENTER}, {"right_center", PivotPreset::RIGHT_CENTER}, {"top_left", PivotPreset::TOP_LEFT},
                {"bottom_left", PivotPreset::BOTTOM_LEFT}, {"top_right", PivotPreset::TOP_RIGHT}, {"bottom_right", PivotPreset::BOTTOM_RIGHT}};
            return names;
        }

        const char* presetName(PivotPreset p) {
            for (const auto& [name, value] : presetNames()) {
                if (value == p) return name;
            }
            return "bottom_center";
        }

        PixelProblem invalid(const std::string& field, const std::string& message, const std::string& fix) {
            return {"invalid_settings", field, message, fix};
        }

        std::string joined(const std::vector<std::string>& items, size_t limit = 20) {
            std::string out;
            for (size_t i = 0; i < items.size() && i < limit; i++) out += (i ? ", " : "") + items[i];
            if (items.size() > limit) out += ", ...";
            return out;
        }

        // the user data as the sheet record keeps it (describe()'s shape)
        nlohmann::json userDataJson(const Document& doc, const UserData& ud) {
            if (ud.empty()) return nullptr;
            nlohmann::json j = nlohmann::json::object();
            if (ud.text) j["text"] = *ud.text;
            if (ud.color) {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", (*ud.color)[0], (*ud.color)[1], (*ud.color)[2], (*ud.color)[3]);
                j["color"] = buffer;
            }
            for (const auto& [key, map] : ud.properties) {
                if (map.empty()) continue;
                if (key == 0) {
                    j["properties"] = toJson(map);
                    continue;
                }
                std::string name = "extension_" + std::to_string(key);
                for (const ExternalFile& f : doc.externalFiles) {
                    if (f.id == key) name = f.name;
                }
                j["extensions"][name] = toJson(map);
            }
            return j;
        }

        // a frame's pivot in source pixels from the top-left (y down), integers
        IntPoint pivotOf(const Document& doc, const AsepriteImportSettings& s, int frame, const Slice* pivotSlice) {
            const int w = doc.width, h = doc.height;
            if (s.pivotSource == PivotSource::Slice && pivotSlice) {
                if (const SliceKey* key = pivotSlice->keyAt(frame); key && key->pivot) {
                    return {key->bounds.x + key->pivot->x, key->bounds.y + key->pivot->y};
                }
            }
            if (s.pivotSource == PivotSource::Custom) {
                return {static_cast<int>(std::floor(s.customPivot[0] * w + 0.5f)), static_cast<int>(std::floor((1.0f - s.customPivot[1]) * h + 0.5f))};
            }
            // a preset (also the slice source's fallback)
            float px = 0, py = 0;   // y up
            switch (s.pivotPreset) {
                case PivotPreset::CENTER: px = w / 2.0f; py = h / 2.0f; break;
                case PivotPreset::TOP_CENTER: px = w / 2.0f; py = static_cast<float>(h); break;
                case PivotPreset::BOTTOM_CENTER: px = w / 2.0f; break;
                case PivotPreset::LEFT_CENTER: py = h / 2.0f; break;
                case PivotPreset::RIGHT_CENTER: px = static_cast<float>(w); py = h / 2.0f; break;
                case PivotPreset::TOP_LEFT: py = static_cast<float>(h); break;
                case PivotPreset::BOTTOM_LEFT: break;
                case PivotPreset::TOP_RIGHT: px = static_cast<float>(w); py = static_cast<float>(h); break;
                case PivotPreset::BOTTOM_RIGHT: px = static_cast<float>(w); break;
                case PivotPreset::FRAME: px = w / 2.0f; py = h / 2.0f; break;
            }
            return {static_cast<int>(std::floor(px + 0.5f)), static_cast<int>(std::floor(h - py + 0.5f))};
        }

        // left, top, right, bottom pixels of the 9-patch centre's margins inside the frame
        std::optional<std::array<int, 4>> borderOf(const Document& doc, int frame, const Slice* borderSlice) {
            if (!borderSlice) return std::nullopt;
            const SliceKey* key = borderSlice->keyAt(frame);
            if (!key || !key->center) return std::nullopt;
            const IntRect& b = key->bounds;
            const IntRect& c = *key->center;
            const int left = std::max(0, b.x + c.x);
            const int top = std::max(0, b.y + c.y);
            const int right = std::max(0, doc.width - (b.x + c.x + c.w));
            const int bottom = std::max(0, doc.height - (b.y + c.y + c.h));
            return std::array<int, 4>{left, top, right, bottom};
        }

        int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }

        // a frame index sequence as one property track of step keys at the cumulative durations
        ClipTrackData frameTrack(const std::string& path, const std::vector<int>& sequence, const std::vector<int>& frameIndex,
                                       const std::vector<uint32_t>& durations) {
            ClipTrackData track;
            track.path = path;
            track.channel = ClipChannel::Property;
            track.component = "sprite";
            track.property = "frame";
            track.kind = PropertyKind::Frame;
            track.width = 1;
            track.discrete = true;
            track.known = true;
            uint64_t ms = 0;
            for (int f : sequence) {
                ClipKey key;
                key.time = static_cast<float>(static_cast<double>(ms) / 1000.0);
                key.value = {static_cast<float>(frameIndex[static_cast<size_t>(f)])};
                key.ease = EaseType::STEP;
                track.keys.push_back(key);
                ms += durations[static_cast<size_t>(f)];
            }
            return track;
        }

        bool loopForced(const UserData& ud, bool& loop) {
            const PropertyMap* props = ud.userProperties();
            if (!props) return false;
            const PropertyValue* v = findProperty(*props, "loop");
            if (!v) return false;
            if (const bool* b = std::get_if<bool>(&v->value)) {
                loop = *b;
                return true;
            }
            return false;
        }

    }

    // ---- settings

    bool AsepriteImportSettings::fromYaml(const YAML::Node& node, AsepriteImportSettings& out, std::vector<PixelProblem>& problems) {
        out = AsepriteImportSettings();
        if (!node || node.IsNull()) return true;
        if (!node.IsMap()) {
            problems.push_back(invalid("asepriteImporter", "the import settings are not a map", "fix the .meta file or reset the settings"));
            return false;
        }
        out.raw = YAML::Clone(node);
        // the YAML read goes through the MCP form so both share one validation
        nlohmann::json j = nlohmann::json::object();
        std::function<nlohmann::json(const YAML::Node&)> toJ = [&](const YAML::Node& n) -> nlohmann::json {
            if (n.IsMap()) {
                nlohmann::json o = nlohmann::json::object();
                for (auto it = n.begin(); it != n.end(); ++it) {
                    // camelCase -> snake_case
                    std::string key, k = it->first.as<std::string>();
                    for (char c : k) {
                        if (c >= 'A' && c <= 'Z') {
                            key += '_';
                            key += static_cast<char>(c - 'A' + 'a');
                        } else {
                            key += c;
                        }
                    }
                    o[key] = toJ(it->second);
                }
                return o;
            }
            if (n.IsSequence()) {
                nlohmann::json a = nlohmann::json::array();
                for (const YAML::Node& e : n) a.push_back(toJ(e));
                return a;
            }
            if (n.IsScalar()) {
                const std::string text = n.Scalar();
                if (n.Tag() == "!" || text.empty()) return text;
                if (text == "true") return true;
                if (text == "false") return false;
                char* end = nullptr;
                const double d = std::strtod(text.c_str(), &end);
                if (end && *end == '\0' && end != text.c_str()) {
                    if (std::floor(d) == d && std::fabs(d) < 1e9) return static_cast<int>(d);
                    return d;
                }
                return text;
            }
            return nullptr;
        };
        j = toJ(node);
        if (j.contains("version")) {
            if (!j["version"].is_number_integer() || j["version"].get<int>() < 1) {
                problems.push_back(invalid("version", "version must be a positive integer", "set version: 1"));
            } else {
                out.version = j["version"].get<int>();
            }
            j.erase("version");
        }
        std::vector<PixelProblem> local;
        out.applyJson(j, local);
        // a newer file's unknown keys are not errors: it is read with what this editor knows
        for (PixelProblem& p : local) {
            if (out.readOnly() && p.message.find("unknown key") != std::string::npos) continue;
            problems.push_back(std::move(p));
        }
        out.raw = YAML::Clone(node);
        return problems.empty();
    }

    bool AsepriteImportSettings::applyJson(const nlohmann::json& patch, std::vector<PixelProblem>& problems) {
        const size_t before = problems.size();
        if (!patch.is_object()) {
            problems.push_back(invalid("settings", "settings must be an object", "pass {\"import_mode\": \"sprite_sheet\", ...}"));
            return false;
        }
        auto str = [&](const nlohmann::json& v, const std::string& field, std::string& out) {
            if (!v.is_string()) {
                problems.push_back(invalid(field, field + " must be a string", ""));
                return false;
            }
            out = v.get<std::string>();
            return true;
        };
        auto boolean = [&](const nlohmann::json& v, const std::string& field, bool& out) {
            if (!v.is_boolean()) {
                problems.push_back(invalid(field, field + " must be true or false", ""));
                return false;
            }
            out = v.get<bool>();
            return true;
        };
        auto integer = [&](const nlohmann::json& v, const std::string& field, int& out, int lo, int hi) {
            if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi) {
                problems.push_back(invalid(field, field + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi), ""));
                return false;
            }
            out = v.get<int>();
            return true;
        };
        auto strings = [&](const nlohmann::json& v, const std::string& field, std::vector<std::string>& out) {
            if (!v.is_array()) {
                problems.push_back(invalid(field, field + " must be a list of strings", ""));
                return false;
            }
            std::vector<std::string> list;
            for (const auto& e : v) {
                if (!e.is_string()) {
                    problems.push_back(invalid(field, field + " must be a list of strings", ""));
                    return false;
                }
                list.push_back(e.get<std::string>());
            }
            out = std::move(list);
            return true;
        };
        auto unknown = [&](const std::string& field, const std::vector<std::string>& known) {
            problems.push_back(invalid(field, "unknown key '" + field + "'", "the keys are: " + joined(known)));
        };
        auto object = [&](const nlohmann::json& v, const std::string& field) {
            if (!v.is_object()) {
                problems.push_back(invalid(field, field + " must be an object", ""));
                return false;
            }
            return true;
        };

        static const std::vector<std::string> topKeys = {"import_mode", "layers", "pixels_per_unit", "pivot", "border", "trim", "clips", "texture", "palette"};
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& v = it.value();
            if (key == "import_mode") {
                std::string m;
                if (str(v, key, m)) {
                    if (m == "sprite_sheet") importMode = ImportMode::SpriteSheet;
                    else if (m == "layers_as_sprites") importMode = ImportMode::LayersAsSprites;
                    else problems.push_back(invalid(key, "import_mode '" + m + "' is unknown", "use sprite_sheet or layers_as_sprites"));
                }
            } else if (key == "layers") {
                if (!object(v, key)) continue;
                for (auto l = v.begin(); l != v.end(); ++l) {
                    const std::string f = "layers." + l.key();
                    if (l.key() == "include_hidden") boolean(l.value(), f, includeHidden);
                    else if (l.key() == "ignore_prefixes") strings(l.value(), f, ignorePrefixes);
                    else if (l.key() == "include") strings(l.value(), f, include);
                    else if (l.key() == "exclude") strings(l.value(), f, exclude);
                    else unknown(f, {"include_hidden", "ignore_prefixes", "include", "exclude"});
                }
            } else if (key == "pixels_per_unit") {
                if (v.is_string() && v.get<std::string>() == "project") {
                    pixelsPerUnit.reset();
                } else {
                    int ppu = 1;
                    if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 4096) {
                        problems.push_back(invalid(key, "pixels_per_unit must be \"project\" or a whole number from 1 to 4096", ""));
                    } else {
                        ppu = v.get<int>();
                        pixelsPerUnit = ppu;
                    }
                }
            } else if (key == "pivot") {
                if (!object(v, key)) continue;
                for (auto p = v.begin(); p != v.end(); ++p) {
                    const std::string f = "pivot." + p.key();
                    if (p.key() == "source") {
                        std::string s;
                        if (!str(p.value(), f, s)) continue;
                        if (s == "preset") pivotSource = PivotSource::Preset;
                        else if (s == "custom") pivotSource = PivotSource::Custom;
                        else if (s == "slice") pivotSource = PivotSource::Slice;
                        else problems.push_back(invalid(f, "pivot.source '" + s + "' is unknown", "use preset, custom or slice"));
                    } else if (p.key() == "slice") {
                        str(p.value(), f, pivotSlice);
                    } else if (p.key() == "preset") {
                        std::string s;
                        if (!str(p.value(), f, s)) continue;
                        bool found = false;
                        std::vector<std::string> names;
                        for (const auto& [name, value] : presetNames()) {
                            names.push_back(name);
                            if (s == name) {
                                pivotPreset = value;
                                found = true;
                            }
                        }
                        if (!found) problems.push_back(invalid(f, "pivot.preset '" + s + "' is unknown", "presets are: " + joined(names)));
                    } else if (p.key() == "custom") {
                        const nlohmann::json& c = p.value();
                        if (!c.is_array() || c.size() != 2 || !c[0].is_number() || !c[1].is_number() || c[0].get<double>() < 0 ||
                            c[0].get<double>() > 1 || c[1].get<double>() < 0 || c[1].get<double>() > 1) {
                            problems.push_back(invalid(f, "pivot.custom must be [x, y] from 0 to 1 (y from the bottom)", "for the feet use [0.5, 0]"));
                        } else {
                            customPivot[0] = c[0].get<float>();
                            customPivot[1] = c[1].get<float>();
                        }
                    } else {
                        unknown(f, {"source", "slice", "preset", "custom"});
                    }
                }
            } else if (key == "border") {
                if (!object(v, key)) continue;
                for (auto b = v.begin(); b != v.end(); ++b) {
                    if (b.key() == "slice") str(b.value(), "border.slice", borderSlice);
                    else unknown("border." + b.key(), {"slice"});
                }
            } else if (key == "trim") {
                boolean(v, key, trim);
            } else if (key == "clips") {
                if (!object(v, key)) continue;
                for (auto c = v.begin(); c != v.end(); ++c) {
                    const std::string f = "clips." + c.key();
                    if (c.key() == "generate") boolean(c.value(), f, generateClips);
                    else if (c.key() == "tags") strings(c.value(), f, clipTags);
                    else if (c.key() == "events") {
                        if (!object(c.value(), f)) continue;
                        for (auto e = c.value().begin(); e != c.value().end(); ++e) {
                            if (e.key() == "layer") str(e.value(), "clips.events.layer", eventsLayer);
                            else unknown("clips.events." + e.key(), {"layer"});
                        }
                    } else unknown(f, {"generate", "tags", "events"});
                }
            } else if (key == "texture") {
                if (!object(v, key)) continue;
                for (auto t = v.begin(); t != v.end(); ++t) {
                    const std::string f = "texture." + t.key();
                    if (t.key() == "layout") {
                        std::string s;
                        if (!str(t.value(), f, s)) continue;
                        if (s == "packed") layout = SheetLayout::Packed;
                        else if (s == "grid") layout = SheetLayout::Grid;
                        else problems.push_back(invalid(f, "texture.layout '" + s + "' is unknown", "use packed or grid"));
                    } else if (t.key() == "padding") integer(t.value(), f, padding, 0, 64);
                    else if (t.key() == "extrude") integer(t.value(), f, extrude, 0, 16);
                    else if (t.key() == "max_size") integer(t.value(), f, maxSize, 16, 16384);
                    else if (t.key() == "compression") {
                        std::string s;
                        if (!str(t.value(), f, s)) continue;
                        if (s == "none") compression = SheetCompression::None;
                        else if (s == "project") compression = SheetCompression::Project;
                        else problems.push_back(invalid(f, "texture.compression '" + s + "' is unknown", "use none (lossless) or project"));
                    } else unknown(f, {"layout", "padding", "extrude", "max_size", "compression"});
                }
            } else if (key == "palette") {
                if (!object(v, key)) continue;
                for (auto p = v.begin(); p != v.end(); ++p) {
                    if (p.key() == "asset") boolean(p.value(), "palette.asset", paletteAsset);
                    else unknown("palette." + p.key(), {"asset"});
                }
            } else {
                unknown(key, topKeys);
            }
        }
        return problems.size() == before;
    }

    nlohmann::json AsepriteImportSettings::toJson() const {
        nlohmann::json j = {
            {"version", version},
            {"import_mode", modeName(importMode)},
            {"layers", {{"include_hidden", includeHidden}, {"ignore_prefixes", ignorePrefixes}, {"include", include}, {"exclude", exclude}}},
            {"pivot", {{"source", pivotSourceName(pivotSource)}, {"slice", pivotSlice}, {"preset", presetName(pivotPreset)},
                       {"custom", {customPivot[0], customPivot[1]}}}},
            {"border", {{"slice", borderSlice}}},
            {"trim", trim},
            {"clips", {{"generate", generateClips}, {"tags", clipTags}, {"events", {{"layer", eventsLayer}}}}},
            {"texture", {{"layout", layoutName(layout)}, {"padding", padding}, {"extrude", extrude}, {"max_size", maxSize}, {"compression", compressionName(compression)}}},
            {"palette", {{"asset", paletteAsset}}},
        };
        if (pixelsPerUnit) j["pixels_per_unit"] = *pixelsPerUnit;
        else j["pixels_per_unit"] = "project";
        return j;
    }

    YAML::Node AsepriteImportSettings::toYaml() const {
        YAML::Node n = raw && raw.IsMap() ? YAML::Clone(raw) : YAML::Node(YAML::NodeType::Map);
        auto seq = [](const std::vector<std::string>& items) {
            YAML::Node s(YAML::NodeType::Sequence);
            for (const std::string& i : items) s.push_back(i);
            s.SetStyle(YAML::EmitterStyle::Flow);
            return s;
        };
        auto flowMap = [](YAML::Node m) {
            m.SetStyle(YAML::EmitterStyle::Flow);
            return m;
        };
        n["version"] = version;
        n["importMode"] = modeName(importMode);
        YAML::Node layers = n["layers"] && n["layers"].IsMap() ? n["layers"] : YAML::Node(YAML::NodeType::Map);
        layers["includeHidden"] = includeHidden;
        layers["ignorePrefixes"] = seq(ignorePrefixes);
        layers["include"] = seq(include);
        layers["exclude"] = seq(exclude);
        n["layers"] = layers;
        if (pixelsPerUnit) n["pixelsPerUnit"] = *pixelsPerUnit;
        else n["pixelsPerUnit"] = "project";
        YAML::Node pivot = n["pivot"] && n["pivot"].IsMap() ? n["pivot"] : YAML::Node(YAML::NodeType::Map);
        pivot["source"] = pivotSourceName(pivotSource);
        pivot["slice"] = pivotSlice;
        pivot["preset"] = presetName(pivotPreset);
        YAML::Node custom(YAML::NodeType::Sequence);
        custom.push_back(customPivot[0]);
        custom.push_back(customPivot[1]);
        custom.SetStyle(YAML::EmitterStyle::Flow);
        pivot["custom"] = custom;
        n["pivot"] = flowMap(pivot);
        YAML::Node border = n["border"] && n["border"].IsMap() ? n["border"] : YAML::Node(YAML::NodeType::Map);
        border["slice"] = borderSlice;
        n["border"] = flowMap(border);
        n["trim"] = trim;
        YAML::Node clips = n["clips"] && n["clips"].IsMap() ? n["clips"] : YAML::Node(YAML::NodeType::Map);
        clips["generate"] = generateClips;
        clips["tags"] = seq(clipTags);
        YAML::Node events = clips["events"] && clips["events"].IsMap() ? clips["events"] : YAML::Node(YAML::NodeType::Map);
        events["layer"] = eventsLayer;
        clips["events"] = flowMap(events);
        n["clips"] = flowMap(clips);
        YAML::Node texture = n["texture"] && n["texture"].IsMap() ? n["texture"] : YAML::Node(YAML::NodeType::Map);
        texture["layout"] = layoutName(layout);
        texture["padding"] = padding;
        texture["extrude"] = extrude;
        texture["maxSize"] = maxSize;
        texture["compression"] = compressionName(compression);
        n["texture"] = flowMap(texture);
        YAML::Node palette = n["palette"] && n["palette"].IsMap() ? n["palette"] : YAML::Node(YAML::NodeType::Map);
        palette["asset"] = paletteAsset;
        n["palette"] = flowMap(palette);
        return n;
    }

    std::string AsepriteImportSettings::fingerprint() const {
        nlohmann::json j = toJson();
        j.erase("version");
        return j.dump();
    }

    // ---- layers (§6.7)

    std::vector<int> selectLayers(const Document& doc, const AsepriteImportSettings& s, std::vector<PixelProblem>& warnings) {
        const int n = static_cast<int>(doc.layers.size());
        std::vector<bool> on(static_cast<size_t>(n), false);
        auto ignored = [&](int l) {
            for (const std::string& prefix : s.ignorePrefixes) {
                if (!prefix.empty() && doc.layers[static_cast<size_t>(l)].name.rfind(prefix, 0) == 0) return true;
            }
            return false;
        };
        for (int l = 0; l < n; l++) {
            const Layer& layer = doc.layers[static_cast<size_t>(l)];
            if (layer.isGroup() || static_cast<uint16_t>(layer.type) > static_cast<uint16_t>(LayerType::Tilemap)) continue;
            if (layer.reference()) continue;
            bool selected = true;
            // the layer and every group above it: visible (unless hidden ones are included), not ignored
            for (int p = l, guard = 0; p >= 0 && guard <= n; p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                if (!s.includeHidden && !doc.layers[static_cast<size_t>(p)].visible()) selected = false;
                if (ignored(p)) selected = false;
            }
            on[static_cast<size_t>(l)] = selected;
        }
        std::vector<std::string> available;
        for (int l = 0; l < n; l++) available.push_back(doc.layerPath(l));
        auto apply = [&](const std::vector<std::string>& names, bool value, const char* field) {
            for (const std::string& name : names) {
                const int l = doc.findLayer(name);
                if (l < 0) {
                    warnings.push_back({"unknown_layer", field, "the layer '" + name + "' is not in the file", "layers are: " + joined(available)});
                    continue;
                }
                // a group names its subtree
                for (int c = 0; c < n; c++) {
                    bool under = false;
                    for (int p = c, guard = 0; p >= 0 && guard <= n; p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                        if (p == l) under = true;
                    }
                    const Layer& layer = doc.layers[static_cast<size_t>(c)];
                    if (under && !layer.isGroup() && !layer.reference()) on[static_cast<size_t>(c)] = value;
                }
            }
        };
        apply(s.include, true, "layers.include");
        apply(s.exclude, false, "layers.exclude");
        std::vector<int> out;
        for (int l = 0; l < n; l++) {
            if (on[static_cast<size_t>(l)]) out.push_back(l);
        }
        return out;
    }

    bool parseEventText(const std::string& text, std::string& function, nlohmann::json& args) {
        std::string t = text;
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        size_t start = 0;
        while (start < t.size() && std::isspace(static_cast<unsigned char>(t[start]))) start++;
        t = t.substr(start);
        const size_t open = t.find('(');
        function = open == std::string::npos ? t : t.substr(0, open);
        while (!function.empty() && std::isspace(static_cast<unsigned char>(function.back()))) function.pop_back();
        if (!anim::isLuaIdentifier(function)) return false;
        args = nlohmann::json::array();
        if (open == std::string::npos) return true;
        if (t.back() != ')') return false;
        const std::string inside = t.substr(open + 1, t.size() - open - 2);
        nlohmann::json parsed = nlohmann::json::parse("[" + inside + "]", nullptr, false);
        if (parsed.is_discarded() || parsed.size() > MAX_ANIMATION_EVENT_ARGS) return false;
        for (const auto& a : parsed) {
            if (!a.is_number() && !a.is_string() && !a.is_boolean()) return false;
        }
        args = parsed;
        return true;
    }

    // ---- the import

    AsepriteImport AsepriteImport::run(const Document& doc, const AsepriteImportSettings& s, const AsepriteImportContext& ctx) {
        AsepriteImport out;
        const std::string stem = std::filesystem::path(ctx.sourcePath).stem().string();
        out.layers = selectLayers(doc, s, out.warnings);
        if (doc.frameCount() < 1) {
            out.error = {"import_failed", "", "the file has no frames", "add a frame in Aseprite"};
            return out;
        }
        if (doc.colorProfile && doc.colorProfile->type == 2) {
            out.warnings.push_back({"icc_profile", "", "ICC profile ignored: pixels used as sRGB", "set the colour profile to sRGB in Aseprite"});
        }
        if (doc.pixelWidth != doc.pixelHeight) {
            out.warnings.push_back({"pixel_ratio", "", "the sprite has non-square pixels (" + std::to_string(doc.pixelWidth) + ":" +
                                    std::to_string(doc.pixelHeight) + "): applied to the quad width", "use square pixels for pixel-perfect rendering"});
        }

        // the slices that feed the pivot and the border
        const Slice* pivotSlice = nullptr;
        if (s.pivotSource == PivotSource::Slice) {
            const std::string want = s.pivotSlice.empty() ? "pivot" : s.pivotSlice;
            for (const Slice& slice : doc.slices) {
                if (slice.name == want) pivotSlice = &slice;
            }
            if (!pivotSlice && !s.pivotSlice.empty()) {
                std::vector<std::string> names;
                for (const Slice& slice : doc.slices) names.push_back(slice.name);
                out.warnings.push_back({"unknown_slice", "pivot.slice", "the slice '" + s.pivotSlice + "' is not in the file (the preset pivot is used)",
                                        names.empty() ? "the file has no slices" : "slices are: " + joined(names)});
            }
            if (!pivotSlice && s.pivotSlice.empty()) {
                for (const Slice& slice : doc.slices) {
                    bool hasPivot = false;
                    for (const SliceKey& k : slice.keys) hasPivot = hasPivot || k.pivot.has_value();
                    if (hasPivot) {
                        pivotSlice = &slice;
                        break;
                    }
                }
            }
        }
        const Slice* borderSlice = nullptr;
        if (s.borderSlice != "none") {
            for (const Slice& slice : doc.slices) {
                bool nine = false;
                for (const SliceKey& k : slice.keys) nine = nine || k.center.has_value();
                if (s.borderSlice.empty() ? nine : slice.name == s.borderSlice) {
                    borderSlice = &slice;
                    break;
                }
            }
            if (!borderSlice && !s.borderSlice.empty()) {
                out.warnings.push_back({"unknown_slice", "border.slice", "the slice '" + s.borderSlice + "' is not in the file (no border)", ""});
            }
        }

        // the groups of frames: the whole sprite, or one per top-level layer / group with selected layers
        struct Group {
            std::string path;          // "" for the whole sprite
            std::vector<int> leaves;
        };
        std::vector<Group> groups;
        if (s.importMode == ImportMode::SpriteSheet) {
            groups.push_back({"", out.layers});
        } else {
            for (int top : doc.children(-1)) {
                Group g;
                g.path = doc.layerPath(top);
                for (int l : out.layers) {
                    for (int p = l, guard = 0; p >= 0 && guard <= static_cast<int>(doc.layers.size()); p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                        if (p == top) {
                            g.leaves.push_back(l);
                            break;
                        }
                    }
                }
                if (!g.leaves.empty()) groups.push_back(std::move(g));
            }
            // z-index reorders across layers, which separate sprites cannot express
            std::vector<std::string> zCels;
            for (const Cel& c : doc.cels) {
                if (c.zIndex != 0 && std::find(out.layers.begin(), out.layers.end(), c.layer) != out.layers.end()) {
                    zCels.push_back(doc.layerPath(c.layer) + " frame " + std::to_string(c.frame));
                }
            }
            if (!zCels.empty()) {
                out.warnings.push_back({"z_index_ignored", "import_mode", "layers_as_sprites cannot express cel z-index: " + joined(zCels, 8),
                                        "use sprite_sheet, or reorder the layers in Aseprite"});
            }
        }
        if (out.layers.empty()) {
            out.warnings.push_back({"no_layers", "layers", "no layer is imported: every frame is empty", "include a layer or include_hidden"});
        }

        // render, trim and pack
        const int frames = doc.frameCount();
        sprite::PackMember member;
        member.path = ctx.sourcePath;
        struct FrameInfo {
            std::string name, key;
            IntPoint pivot;
            std::array<int, 4> border{0, 0, 0, 0};
            uint32_t duration;
        };
        std::vector<FrameInfo> infos;
        for (const Group& g : groups) {
            RenderOptions ro;
            ro.layers = g.leaves.empty() ? std::vector<int>{-1} : g.leaves;
            ro.includeHidden = true;   // the selection already decided visibility
            ro.zIndex = s.importMode == ImportMode::SpriteSheet;
            for (int f = 0; f < frames; f++) {
                FrameInfo info;
                if (g.path.empty()) {
                    info.name = stem + " " + std::to_string(f);
                    info.key = subAssetIndexKey(SubAssetKind::Frame, static_cast<size_t>(f));
                } else {
                    info.name = g.path + "/" + std::to_string(f);
                    info.key = subAssetNamedKey(SubAssetKind::Frame, info.name);
                }
                info.pivot = pivotOf(doc, s, f, pivotSlice);
                const std::optional<std::array<int, 4>> border = borderOf(doc, f, borderSlice);
                if (border) info.border = *border;
                info.duration = doc.frameDurations[static_cast<size_t>(f)];
                sprite::PackInputFrame in;
                in.name = info.name;
                in.image = std::make_shared<const PixelImage>(renderFrame(doc, f, ro));
                in.noTrim = border.has_value() || !s.trim;
                member.frames.push_back(std::move(in));
                infos.push_back(std::move(info));
            }
        }

        std::vector<sprite::PackedFrame> packed;
        if (s.layout == SheetLayout::Packed) {
            sprite::PackSettings ps;
            ps.padding = s.padding;
            ps.extrude = s.extrude;
            ps.maxSize = s.maxSize;
            ps.trim = s.trim;
            sprite::PackResult r = sprite::pack({member}, ps);
            if (!r.ok()) {
                out.error = r.error;
                if (out.error.kind == "too_large") out.error.field = "texture.max_size";
                return out;
            }
            packed = r.layout.frames;
            out.pages = std::move(r.pages);
            // every frame fully transparent: no pixel to pack, but a sheet record needs a page for
            // its (empty) frames to point at, or a reader drops them and the tags with them
            if (out.pages.empty() && !packed.empty()) {
                out.pages.emplace_back(1, 1);
                for (sprite::PackedFrame& pf : packed) pf.page = 0;
            }
        } else {
            // Aseprite's sheet look: untrimmed cells in rows, as many columns as fit max_size
            const int cw = doc.width + s.padding, ch = doc.height + s.padding;
            const int total = static_cast<int>(member.frames.size());
            const int columns = std::max(1, std::min(total, (s.maxSize - s.padding) / std::max(1, cw)));
            const int rowsPerPage = std::max(1, (s.maxSize - s.padding) / std::max(1, ch));
            if (doc.width + 2 * s.padding > s.maxSize || doc.height + 2 * s.padding > s.maxSize) {
                out.error = {"too_large", "texture.max_size", "a " + std::to_string(doc.width) + "x" + std::to_string(doc.height) + " frame does not fit a " +
                             std::to_string(s.maxSize) + " page", "raise max_size"};
                return out;
            }
            for (int i = 0; i < total; i++) {
                const int page = i / (columns * rowsPerPage);
                const int slot = i % (columns * rowsPerPage);
                if (page >= static_cast<int>(out.pages.size())) {
                    const int left = total - page * columns * rowsPerPage;
                    const int rows = std::min(rowsPerPage, (left + columns - 1) / columns);
                    const int cols = std::min(columns, left);
                    out.pages.emplace_back(s.padding + cols * cw, s.padding + rows * ch);
                }
                sprite::PackedFrame pf;
                pf.page = page;
                pf.rect = {s.padding + (slot % columns) * cw, s.padding + (slot / columns) * ch, doc.width, doc.height};
                pf.sourceSize = {doc.width, doc.height};
                out.pages[static_cast<size_t>(page)].blit(*member.frames[static_cast<size_t>(i)].image, pf.rect.x, pf.rect.y);
                packed.push_back(pf);
            }
        }

        // the sheet record
        SpriteSheetData& sheet = out.sheet;
        sheet.source = ctx.sourcePath;
        sheet.sourceGuid = ctx.sourceGuid;
        sheet.pixelsPerUnit = static_cast<float>(s.pixelsPerUnit.value_or(std::max(1, ctx.projectPixelsPerUnit)));
        sheet.pixelRatio = Vector2(doc.pixelWidth, doc.pixelHeight);
        for (size_t p = 0; p < out.pages.size(); p++) {
            sheet.pages.push_back({"page" + std::to_string(p) + ".png", out.pages[p].width(), out.pages[p].height()});
        }
        for (size_t i = 0; i < infos.size(); i++) {
            const sprite::PackedFrame& pf = packed[i];
            SpriteFrame f;
            f.name = infos[i].name;
            f.key = infos[i].key;
            f.page = static_cast<uint16_t>(pf.page);
            f.rect = Rect(static_cast<float>(pf.rect.x), static_cast<float>(pf.rect.y), static_cast<float>(pf.rect.w), static_cast<float>(pf.rect.h));
            f.sourceSize = Vector2(static_cast<float>(doc.width), static_cast<float>(doc.height));
            f.offset = Vector2(static_cast<float>(pf.offset.x), static_cast<float>(pf.offset.y));
            f.pivot = Vector2(static_cast<float>(infos[i].pivot.x), static_cast<float>(infos[i].pivot.y));
            f.border = Vector4(static_cast<float>(infos[i].border[0]), static_cast<float>(infos[i].border[1]), static_cast<float>(infos[i].border[2]),
                               static_cast<float>(infos[i].border[3]));
            f.durationMs = infos[i].duration;
            sheet.frames.push_back(std::move(f));
        }
        for (const Tag& t : doc.tags) {
            SpriteTag tag;
            tag.name = t.name;
            tag.from = std::clamp(t.from, 0, frames - 1);
            tag.to = std::clamp(t.to, tag.from, frames - 1);
            tag.direction = static_cast<SpriteTagDirection>(t.direction);
            tag.repeat = t.repeat;
            const PixelRgba c = t.color();
            tag.color = Vector4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
            tag.userData = userDataJson(doc, t.userData);
            sheet.tags.push_back(std::move(tag));
        }
        for (const Slice& sl : doc.slices) {
            SpriteSlice slice;
            slice.name = sl.name;
            for (const SliceKey& k : sl.keys) {
                SpriteSliceKey key;
                key.frame = static_cast<int>(std::min<uint32_t>(k.frame, static_cast<uint32_t>(INT32_MAX)));
                key.bounds = Rect(static_cast<float>(k.bounds.x), static_cast<float>(k.bounds.y), static_cast<float>(k.bounds.w), static_cast<float>(k.bounds.h));
                if (k.center) {
                    key.hasCenter = true;
                    key.center = Rect(static_cast<float>(k.center->x), static_cast<float>(k.center->y), static_cast<float>(k.center->w), static_cast<float>(k.center->h));
                }
                if (k.pivot) {
                    key.hasPivot = true;
                    key.pivot = Vector2(static_cast<float>(k.pivot->x), static_cast<float>(k.pivot->y));
                }
                slice.keys.push_back(key);
            }
            slice.userData = userDataJson(doc, sl.userData);
            sheet.slices.push_back(std::move(slice));
        }
        sheet.userData = userDataJson(doc, doc.spriteUserData);

        // clips: one per tag (or one of every frame for a file without tags), step keys
        if (s.generateClips) {
            std::vector<uint32_t> durations(static_cast<size_t>(frames));
            for (int f = 0; f < frames; f++) durations[static_cast<size_t>(f)] = std::max<uint32_t>(1, doc.frameDurations[static_cast<size_t>(f)]);
            // events: the text of the cels of the events layer, at their frame's start
            const int eventsLayer = s.eventsLayer.empty() ? -1 : doc.findLayer(s.eventsLayer);
            struct Pending { int frame; std::string function; nlohmann::json args; };
            std::vector<Pending> events;
            if (eventsLayer >= 0) {
                for (const Cel& c : doc.cels) {
                    if (c.layer != eventsLayer || !c.userData.text || c.userData.text->empty()) continue;
                    Pending e;
                    e.frame = c.frame;
                    if (parseEventText(*c.userData.text, e.function, e.args)) {
                        events.push_back(std::move(e));
                    } else {
                        out.warnings.push_back({"bad_event", "clips.events.layer", "the text '" + *c.userData.text + "' of the events layer on frame " +
                                                std::to_string(c.frame) + " is not a call (it is skipped)", "write it as onStep(\"left\", 1)"});
                    }
                }
            }
            struct Source { int tag; std::string name; std::vector<int> sequence; bool loop; };
            std::vector<Source> sources;
            if (doc.tags.empty()) {
                std::vector<int> all(static_cast<size_t>(frames));
                std::iota(all.begin(), all.end(), 0);
                sources.push_back({-1, stem, all, true});
            }
            for (int t = 0; t < static_cast<int>(sheet.tags.size()); t++) {
                const SpriteTag& tag = sheet.tags[static_cast<size_t>(t)];
                if (!s.clipTags.empty() && std::find(s.clipTags.begin(), s.clipTags.end(), tag.name) == s.clipTags.end()) continue;
                bool loop = tag.repeat == 0;
                loopForced(doc.tags[static_cast<size_t>(t)].userData, loop);
                sources.push_back({t, tag.name, tag.repeat == 0 ? tag.sequence() : tag.playback(), loop});
            }
            // keys over every tag (as the asset database's scan makes them), whatever the filter
            std::vector<std::string> tagNames;
            for (const SpriteTag& tag : sheet.tags) tagNames.push_back(tag.name);
            const std::vector<std::string> tagKeys = subAssetKeys(SubAssetKind::Animation, tagNames);
            for (size_t c = 0; c < sources.size(); c++) {
                const Source& src = sources[c];
                const std::string key = src.tag < 0 ? subAssetIndexKey(SubAssetKind::Animation, 0) : tagKeys[static_cast<size_t>(src.tag)];
                ClipData clip;
                int g = 0;
                uint64_t total = 0;
                for (int f : src.sequence) {
                    g = gcd(g, static_cast<int>(durations[static_cast<size_t>(f)]));
                    total += durations[static_cast<size_t>(f)];
                }
                clip.frameRate = std::clamp(1000.0f / static_cast<float>(std::max(1, g)), anim::ANIM_MIN_FRAME_RATE, anim::ANIM_MAX_FRAME_RATE);
                clip.wrap = src.loop ? ClipWrapMode::Loop : ClipWrapMode::Once;
                clip.length = static_cast<float>(static_cast<double>(total) / 1000.0);
                clip.source.kind = "aseprite";
                clip.source.path = ctx.sourcePath;
                clip.source.name = src.name;
                for (size_t gi = 0; gi < groups.size(); gi++) {
                    std::vector<int> index(static_cast<size_t>(frames));
                    for (int f = 0; f < frames; f++) index[static_cast<size_t>(f)] = static_cast<int>(gi) * frames + f;
                    // layers_as_sprites: one track per layer child (its name, addressable)
                    std::string path;
                    if (!groups[gi].path.empty()) {
                        path = groups[gi].path;
                        std::replace(path.begin(), path.end(), '/', '_');
                    }
                    clip.tracks.push_back(frameTrack(path, src.sequence, index, durations));
                }
                // the events at the start of each step whose frame has one
                uint64_t ms = 0;
                for (int f : src.sequence) {
                    for (const Pending& e : events) {
                        if (e.frame != f) continue;
                        ClipEventData ev;
                        ev.time = static_cast<float>(static_cast<double>(ms) / 1000.0);
                        ev.function = e.function;
                        for (const auto& a : e.args) {
                            if (a.is_number()) ev.args.push_back(AnimationEvent::numberArg(a.get<double>()));
                            else if (a.is_string()) ev.args.push_back(AnimationEvent::stringArg(a.get<std::string>()));
                            else ev.args.push_back(AnimationEvent::boolArg(a.get<bool>()));
                        }
                        clip.events.push_back(std::move(ev));
                    }
                    ms += durations[static_cast<size_t>(f)];
                }
                ClipOutput co;
                co.key = key;
                co.name = src.name;
                co.tag = src.tag;
                co.length = clip.length;
                co.wrap = src.loop ? "loop" : "once";
                co.keys = src.sequence.size();
                co.text = anim::writeClip(clip);
                out.clips.push_back(std::move(co));
            }
        }

        // the hand-offs
        if (s.paletteAsset && !doc.palette.empty()) {
            out.palette = doc.palette;
            out.transparentIndex = doc.mode == ColorMode::Indexed ? doc.transparentIndex : -1;
        }
        {
            std::vector<std::string> names;
            for (const Tileset& t : doc.tilesets) names.push_back(t.name);
            const std::vector<std::string> keys = subAssetKeys(SubAssetKind::Tileset, names);
            for (size_t i = 0; i < doc.tilesets.size(); i++) {
                const Tileset& t = doc.tilesets[i];
                TilesetOutput o;
                o.key = keys[i];
                o.name = t.name;
                o.id = t.id;
                o.tileWidth = t.tileWidth;
                o.tileHeight = t.tileHeight;
                o.baseIndex = t.baseIndex;
                o.tileCount = t.tileCount;
                o.tiles = t.tiles;
                o.tiles.dropIndices();
                for (const UserData& ud : t.tileUserData) o.tileUserData.push_back(userDataJson(doc, ud));
                out.tilesetOutputs.push_back(std::move(o));
            }
            for (const Cel& c : doc.cels) {
                if (!c.tilemap || c.linkedFrame >= 0) continue;
                TilemapCelOutput o;
                o.layer = doc.layerPath(c.layer);
                o.frame = c.frame;
                o.x = c.x;
                o.y = c.y;
                o.tileset = doc.layers[static_cast<size_t>(c.layer)].tilesetIndex;
                o.tilemap = *c.tilemap;
                out.tilemapOutputs.push_back(std::move(o));
            }
        }

        // the sub-assets: frames, pages, clips, slices, tilesets, the palette
        for (const SpriteFrame& f : sheet.frames) out.subAssets.push_back({f.key, f.name, SubAssetKind::Frame});
        for (size_t p = 0; p < sheet.pages.size(); p++) {
            out.subAssets.push_back({subAssetIndexKey(SubAssetKind::Image, p), stem + " page " + std::to_string(p), SubAssetKind::Image});
        }
        for (const ClipOutput& c : out.clips) out.subAssets.push_back({c.key, c.name, SubAssetKind::Animation});
        {
            std::vector<std::string> names;
            for (const Slice& sl : doc.slices) names.push_back(sl.name);
            const std::vector<std::string> keys = subAssetKeys(SubAssetKind::Slice, names);
            for (size_t i = 0; i < keys.size(); i++) out.subAssets.push_back({keys[i], doc.slices[i].name, SubAssetKind::Slice});
        }
        for (const TilesetOutput& t : out.tilesetOutputs) out.subAssets.push_back({t.key, t.name, SubAssetKind::Tileset});
        if (!out.palette.empty()) out.subAssets.push_back({subAssetIndexKey(SubAssetKind::Palette, 0), stem + " palette", SubAssetKind::Palette});
        return out;
    }

    bool scanAsepriteSubAssets(const std::filesystem::path& file, std::vector<ModelSubAsset>& out) {
        out.clear();
        ReadOptions quick;
        quick.pixels = false;
        quick.provenance = false;
        const ReadResult r = readFile(file, quick);
        if (!r.ok()) return false;
        const Document& doc = *r.document;
        auto addNamed = [&](SubAssetKind kind, const std::vector<std::string>& names) {
            const std::vector<std::string> keys = subAssetKeys(kind, names);
            for (size_t i = 0; i < keys.size(); i++) {
                SubAssetKind k;
                std::string name;
                long index = -1;
                if (!parseSubAssetKey(keys[i], k, name, index) || index >= 0) continue;   // index keys are derived
                out.push_back({kind, names[i], i, keys[i]});
            }
        };
        std::vector<std::string> tags, slices, tilesets;
        for (const Tag& t : doc.tags) tags.push_back(t.name);
        for (const Slice& sl : doc.slices) slices.push_back(sl.name);
        for (const Tileset& t : doc.tilesets) tilesets.push_back(t.name);
        addNamed(SubAssetKind::Animation, tags);
        addNamed(SubAssetKind::Slice, slices);
        addNamed(SubAssetKind::Tileset, tilesets);
        return true;
    }

    std::string AsepriteImport::summary() const {
        std::ostringstream o;
        o << sheet.tags.size() << (sheet.tags.size() == 1 ? " tag" : " tags") << " · " << sheet.frames.size()
          << (sheet.frames.size() == 1 ? " frame" : " frames") << " · " << pages.size() << (pages.size() == 1 ? " page" : " pages");
        if (pages.size() == 1) o << " " << pages[0].width() << "x" << pages[0].height();
        o << " · " << sheet.slices.size() << (sheet.slices.size() == 1 ? " slice" : " slices");
        if (!warnings.empty()) o << " · " << warnings.size() << (warnings.size() == 1 ? " warning" : " warnings");
        return o.str();
    }

    nlohmann::json AsepriteImport::toJson() const {
        nlohmann::json pagesJson = nlohmann::json::array();
        for (const PixelImage& p : pages) pagesJson.push_back({{"size", {p.width(), p.height()}}});
        nlohmann::json clipsJson = nlohmann::json::array();
        for (const ClipOutput& c : clips) clipsJson.push_back({{"name", c.name}, {"key", c.key}, {"length", c.length}, {"wrap", c.wrap}, {"keys", c.keys}});
        nlohmann::json warningsJson = nlohmann::json::array();
        for (const PixelProblem& w : warnings) {
            nlohmann::json wj = {{"kind", w.kind}, {"message", w.message}};
            if (!w.fix.empty()) wj["fix"] = w.fix;
            if (!w.field.empty()) wj["field"] = w.field;
            warningsJson.push_back(std::move(wj));
        }
        nlohmann::json slicesJson = nlohmann::json::array();
        for (const SpriteSlice& s : sheet.slices) slicesJson.push_back(s.name);
        nlohmann::json tagsJson = nlohmann::json::array();
        for (const SpriteTag& t : sheet.tags) tagsJson.push_back(t.name);
        return {{"frames", sheet.frames.size()}, {"pages", pagesJson}, {"clips", clipsJson}, {"slices", slicesJson}, {"tags", tagsJson},
                {"pixels_per_unit", sheet.pixelsPerUnit}, {"summary", summary()}, {"warnings", warningsJson}};
    }

}
