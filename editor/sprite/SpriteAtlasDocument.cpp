// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteAtlasDocument.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor::sprite {

    namespace {

        using pixelart::PixelProblem;
        using OJson = nlohmann::ordered_json;

        constexpr const char* kSettingsKeys[] = {"max_size", "padding", "extrude", "trim", "keep_members_together", "power_of_two",
                                                 "square", "compression", "filter", "platforms"};

        bool known(const char* const* keys, size_t n, const std::string& key) {
            for (size_t i = 0; i < n; i++) {
                if (key == keys[i]) return true;
            }
            return false;
        }

        bool intIn(const nlohmann::json& v, int lo, int hi, int& out) {
            if (!v.is_number()) return false;
            const double d = v.get<double>();
            if (d != std::floor(d) || d < lo || d > hi) return false;
            out = static_cast<int>(d);
            return true;
        }

        PixelProblem invalid(const std::string& field, const std::string& message) {
            return {"invalid_settings", field, message, "see jasper://docs/sprites (Atlases) for the settings and their ranges"};
        }

        // one settings key; false with a problem
        bool setOne(SpriteAtlasSettings& s, const std::string& key, const nlohmann::json& v, std::vector<PixelProblem>& problems) {
            if (key == "max_size") {
                if (!intIn(v, 32, 8192, s.maxSize)) { problems.push_back(invalid("max_size", "max_size is an integer from 32 to 8192")); return false; }
            } else if (key == "padding") {
                if (!intIn(v, 0, 16, s.padding)) { problems.push_back(invalid("padding", "padding is an integer from 0 to 16")); return false; }
            } else if (key == "extrude") {
                if (!intIn(v, 0, 4, s.extrude)) { problems.push_back(invalid("extrude", "extrude is an integer from 0 to 4")); return false; }
            } else if (key == "trim" || key == "keep_members_together" || key == "power_of_two" || key == "square") {
                if (!v.is_boolean()) { problems.push_back(invalid(key, key + " is a boolean")); return false; }
                const bool b = v.get<bool>();
                if (key == "trim") s.trim = b;
                else if (key == "keep_members_together") s.keepMembersTogether = b;
                else if (key == "power_of_two") s.powerOfTwo = b;
                else s.square = b;
            } else if (key == "compression") {
                const std::string c = v.is_string() ? v.get<std::string>() : std::string();
                if (c != "auto" && c != "lossless" && c != "platform_default") {
                    problems.push_back(invalid("compression", "compression is auto, lossless or platform_default"));
                    return false;
                }
                s.compression = c;
            } else if (key == "filter") {
                const std::string f = v.is_string() ? v.get<std::string>() : std::string();
                if (f != "auto" && f != "point" && f != "bilinear") {
                    problems.push_back(invalid("filter", "filter is auto, point or bilinear"));
                    return false;
                }
                s.filter = f;
            } else if (key == "platforms") {
                if (!v.is_object()) { problems.push_back(invalid("platforms", "platforms is {desktop|mobile|web|android_etc2: {override, max_size}}")); return false; }
                for (const auto& [platform, o] : v.items()) {
                    if (!isAtlasPlatform(platform) || !o.is_object()) {
                        problems.push_back(invalid("platforms." + platform, "platforms has desktop, mobile, web and android_etc2, each {override, max_size}"));
                        return false;
                    }
                    AtlasPlatformOverride p = s.platforms.count(platform) ? s.platforms[platform] : AtlasPlatformOverride();
                    for (const auto& [k, value] : o.items()) {
                        if (k == "override" && value.is_boolean()) p.override = value.get<bool>();
                        else if (k == "max_size" && intIn(value, 32, 8192, p.maxSize)) {}
                        else {
                            problems.push_back(invalid("platforms." + platform + "." + k, "a platform takes override (boolean) and max_size (32 to 8192)"));
                            return false;
                        }
                    }
                    s.platforms[platform] = p;
                }
            } else {
                problems.push_back(invalid(key, "unknown setting " + key + ": max_size, padding, extrude, trim, keep_members_together, power_of_two, square, compression, filter, platforms"));
                return false;
            }
            return true;
        }

        OJson settingsOrdered(const SpriteAtlasSettings& s) {
            OJson j = OJson::object();
            j["max_size"] = s.maxSize;
            j["padding"] = s.padding;
            j["extrude"] = s.extrude;
            j["trim"] = s.trim;
            j["keep_members_together"] = s.keepMembersTogether;
            j["power_of_two"] = s.powerOfTwo;
            j["square"] = s.square;
            j["compression"] = s.compression;
            j["filter"] = s.filter;
            if (!s.platforms.empty()) {
                OJson platforms = OJson::object();
                for (const char* name : kAtlasPlatforms) {
                    auto it = s.platforms.find(name);
                    if (it == s.platforms.end()) continue;
                    platforms[name] = OJson{{"override", it->second.override}, {"max_size", it->second.maxSize}};
                }
                j["platforms"] = platforms;
            }
            for (const auto& [k, v] : s.unknown.items()) j[k] = v;
            return j;
        }

    }

    bool isAtlasPlatform(const std::string& platform) {
        for (const char* name : kAtlasPlatforms) {
            if (platform == name) return true;
        }
        return false;
    }

    int SpriteAtlasSettings::maxSizeFor(const std::string& platform) const {
        auto it = platforms.find(platform);
        return it != platforms.end() && it->second.override ? it->second.maxSize : maxSize;
    }

    SpriteAtlasParseResult parseSpriteAtlas(const std::string& text) {
        SpriteAtlasParseResult result;
        const OJson root = OJson::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            result.error = {"invalid_settings", "", "not a JSON object", "a .spriteatlas is JSON (jasper://docs/sprites, Atlases)"};
            return result;
        }
        if (root.value("format", std::string()) != kSpriteAtlasFormat) {
            result.error = {"invalid_settings", "format", "format is not " + std::string(kSpriteAtlasFormat), "create it with Assets > Create > Sprite Atlas or edit_sprite_atlas"};
            return result;
        }
        const int version = root.contains("version") && root["version"].is_number_integer() ? root["version"].get<int>() : 0;
        if (version < 1) {
            result.error = {"invalid_settings", "version", "no version", "\"version\": 1"};
            return result;
        }
        if (version > kSpriteAtlasVersion) {
            result.error = {"atlas_version", "version", "the atlas is version " + std::to_string(version) + ", newer than this editor reads (" +
                                                      std::to_string(kSpriteAtlasVersion) + "): read-only", "open the project with a newer editor"};
            return result;
        }
        SpriteAtlasDocument doc;
        for (const auto& [key, value] : root.items()) {
            if (key == "format" || key == "version") continue;
            if (key == "guid") {
                doc.guid = value.is_string() ? Guid::parse(value.get<std::string>()) : Guid();
            } else if (key == "include_in_build") {
                if (value.is_boolean()) doc.includeInBuild = value.get<bool>();
                else result.problems.push_back(invalid("include_in_build", "include_in_build is a boolean"));
            } else if (key == "members") {
                if (!value.is_array()) {
                    result.problems.push_back(invalid("members", "members is a list"));
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++) {
                    const OJson& m = value[i];
                    SpriteAtlasMember member;
                    if (!m.is_object()) {
                        result.problems.push_back(invalid("members[" + std::to_string(i) + "]", "a member is {path, guid} or {folder, types}"));
                        continue;
                    }
                    for (const auto& [mk, mv] : m.items()) {
                        if (mk == "path" && mv.is_string()) member.path = mv.get<std::string>();
                        else if (mk == "guid" && mv.is_string()) member.guid = Guid::parse(mv.get<std::string>());
                        else if (mk == "folder" && mv.is_string()) member.folder = mv.get<std::string>();
                        else if (mk == "types" && mv.is_array()) {
                            for (const OJson& t : mv) {
                                if (t.is_string()) member.types.push_back(t.get<std::string>());
                            }
                        } else member.unknown[mk] = mv;
                    }
                    if (member.path.empty() == member.folder.empty()) {
                        result.problems.push_back(invalid("members[" + std::to_string(i) + "]", "a member has a path or a folder"));
                        continue;
                    }
                    doc.members.push_back(std::move(member));
                }
            } else if (key == "exclude") {
                if (!value.is_array()) {
                    result.problems.push_back(invalid("exclude", "exclude is a list of paths"));
                    continue;
                }
                for (const OJson& e : value) {
                    if (e.is_string()) doc.exclude.push_back(e.get<std::string>());
                }
            } else if (key == "settings") {
                if (!value.is_object()) {
                    result.problems.push_back(invalid("settings", "settings is an object"));
                    continue;
                }
                for (const auto& [sk, sv] : value.items()) {
                    if (!known(kSettingsKeys, std::size(kSettingsKeys), sk)) {
                        doc.settings.unknown[sk] = sv;
                        continue;
                    }
                    SpriteAtlasSettings trial = doc.settings;
                    if (setOne(trial, sk, nlohmann::json::parse(sv.dump()), result.problems)) doc.settings = trial;
                }
            } else {
                doc.unknown[key] = value;
            }
        }
        result.document = std::move(doc);
        return result;
    }

    std::string writeSpriteAtlas(const SpriteAtlasDocument& doc) {
        auto line = [](const OJson& value) { return value.dump(); };
        std::string out = "{\n";
        out += "  \"format\": " + line(OJson(kSpriteAtlasFormat)) + ",\n";
        out += "  \"version\": " + std::to_string(kSpriteAtlasVersion) + ",\n";
        out += "  \"guid\": " + line(OJson(doc.guid.isNull() ? std::string() : doc.guid.str())) + ",\n";
        out += "  \"include_in_build\": " + std::string(doc.includeInBuild ? "true" : "false") + ",\n";
        out += "  \"members\": [";
        for (size_t i = 0; i < doc.members.size(); i++) {
            const SpriteAtlasMember& m = doc.members[i];
            OJson j = OJson::object();
            if (m.isFolder()) {
                j["folder"] = m.folder;
                if (!m.types.empty()) j["types"] = m.types;
            } else {
                j["path"] = m.path;
                if (!m.guid.isNull()) j["guid"] = m.guid.str();
            }
            for (const auto& [k, v] : m.unknown.items()) j[k] = v;
            out += std::string(i == 0 ? "\n" : ",\n") + "    " + line(j);
        }
        out += doc.members.empty() ? "],\n" : "\n  ],\n";
        out += "  \"exclude\": " + line(OJson(doc.exclude)) + ",\n";
        out += "  \"settings\": " + line(settingsOrdered(doc.settings));
        for (const auto& [k, v] : doc.unknown.items()) out += ",\n  " + line(OJson(k)) + ": " + line(v);
        out += "\n}\n";
        return out;
    }

    bool applyAtlasSettings(const nlohmann::json& edit, const std::string& platform, SpriteAtlasSettings& out, std::vector<PixelProblem>& problems) {
        if (!edit.is_object()) {
            problems.push_back(invalid("settings", "settings is an object of the keys to change"));
            return false;
        }
        SpriteAtlasSettings trial = out;
        const size_t before = problems.size();
        if (!platform.empty() && platform != "default") {
            if (!isAtlasPlatform(platform)) {
                problems.push_back(invalid("platform", "platform is desktop, mobile, web or android_etc2"));
                return false;
            }
            AtlasPlatformOverride p = trial.platforms.count(platform) ? trial.platforms[platform] : AtlasPlatformOverride();
            for (const auto& [k, v] : edit.items()) {
                if (k == "max_size" && intIn(v, 32, 8192, p.maxSize)) p.override = true;
                else if (k == "override" && v.is_boolean()) p.override = v.get<bool>();
                else problems.push_back(invalid(k, "a platform override takes max_size (32 to 8192) and override (boolean)"));
            }
            trial.platforms[platform] = p;
        } else {
            for (const auto& [k, v] : edit.items()) setOne(trial, k, v, problems);
        }
        if (problems.size() != before) return false;
        out = trial;
        return true;
    }

    nlohmann::json atlasSettingsJson(const SpriteAtlasSettings& settings) {
        return nlohmann::json::parse(settingsOrdered(settings).dump());
    }

}
