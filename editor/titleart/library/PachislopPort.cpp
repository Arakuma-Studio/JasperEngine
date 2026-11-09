// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Pachislop port's mapping stage. Pachislop's model (src/model/types.ts, src/presets/types.ts)
// is by SafEight, used with the author's permission; this file reads the values its own code
// evaluated (the dump), it copies none of its code.

#include "PachislopPort.h"

#include "DeniedTerms.h"
#include "PachislopColourways.h"
#include "TitleLibrary.h"
#include "TitleLibraryFormats.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/fx/PachislopFxMap.h"
#include "titleart/fx/PachislopGeneratorMap.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "titleart/three_d/Title3DConvert.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::titlelibrary;
using titleart::SchemaKind;
using titleart::SchemaMember;
using titleart::TitleArtSchema;
using titleart::ValueType;

namespace fs = std::filesystem;

namespace {

    const Json NULL_JSON = nullptr;

    // ---- small helpers ----

    std::string snake(const std::string& camel) {
        std::string out;
        for (char c : camel) {
            if (c >= 'A' && c <= 'Z') {
                if (!out.empty()) out += '_';
                out += (char)(c - 'A' + 'a');
            } else {
                out += c;
            }
        }
        return out;
    }

    uint32_t fnv1a32(const std::string& text) {
        uint32_t h = 2166136261u;
        for (unsigned char c : text) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

    uint64_t fnv1a64(const std::string& text) {
        uint64_t h = 14695981039346656037ull;
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    std::string hex32(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08x", v);
        return b;
    }

    std::string hex64(uint64_t v) {
        char b[24];
        std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
        return b;
    }

    // the code points of a UTF-8 text, each as its bytes (Pachislop keys chars by code point index)
    std::vector<std::string> codepoints(const std::string& text) {
        std::vector<std::string> out;
        for (size_t i = 0; i < text.size();) {
            const unsigned char c = (unsigned char)text[i];
            size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            n = std::min(n, text.size() - i);
            out.push_back(text.substr(i, n));
            i += n;
        }
        return out;
    }

    size_t visibleLength(const std::string& text) {
        size_t n = 0;
        for (const std::string& cp : codepoints(text)) {
            if (cp != "\n") n++;
        }
        return n;
    }

    bool isHexColor(const std::string& s) {
        if (s.size() != 7 && s.size() != 9) return false;
        if (s[0] != '#') return false;
        for (size_t i = 1; i < s.size(); i++) {
            if (!std::isxdigit((unsigned char)s[i])) return false;
        }
        return true;
    }

    std::string lowerText(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return s;
    }

    // every "#RRGGBB" string of a value lowercase (§6.7.1: colours lowercase)
    void lowercaseColours(Json& v) {
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            if (isHexColor(s)) v = lowerText(s);
        } else if (v.is_object() || v.is_array()) {
            for (auto& child : v) lowercaseColours(child);
        }
    }

    std::string joinList(const std::vector<std::string>& items, const char* sep) {
        std::string out;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) out += sep;
            out += items[i];
        }
        return out;
    }

    // "a", "a and b", "a, b and c"
    std::string joinEnglish(const std::vector<std::string>& items) {
        std::string out;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) out += i + 1 == items.size() ? " and " : ", ";
            out += items[i];
        }
        return out;
    }

    std::string numberWord(size_t n) {
        static const char* const WORDS[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                            "eleven", "twelve"};
        return n < sizeof(WORDS) / sizeof(WORDS[0]) ? WORDS[n] : std::to_string(n);
    }

    std::string article(const std::string& word) {
        if (word.empty()) return "a";
        const char c = word[0];
        return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
    }

    // glob match of dotted patterns: `**` any number of segments, `*` one segment
    std::vector<std::string> splitDots(const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (true) {
            const size_t dot = s.find('.', start);
            out.push_back(s.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return out;
    }

    bool globMatch(const std::vector<std::string>& p, size_t pi, const std::vector<std::string>& s, size_t si) {
        if (pi == p.size()) return si == s.size();
        if (p[pi] == "**") {
            for (size_t k = si; k <= s.size(); k++) {
                if (globMatch(p, pi + 1, s, k)) return true;
            }
            return false;
        }
        if (si == s.size()) return false;
        if (p[pi] != "*" && p[pi] != s[si]) return false;
        return globMatch(p, pi + 1, s, si + 1);
    }

    // ---- names of colours, materials, effects and generators (descriptions and tags) ----

    struct Words {
        std::string tag;    // "" when it gives none
        std::string en;
        std::string ja;
    };

    void hsv(double r, double g, double b, double& h, double& s, double& v) {
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
        v = mx;
        s = mx > 0 ? (mx - mn) / mx : 0;
        if (mx == mn) {
            h = 0;
            return;
        }
        const double d = mx - mn;
        if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = 60 * ((b - r) / d + 2);
        else h = 60 * ((r - g) / d + 4);
        if (h < 0) h += 360;
    }

    bool parseHex(const std::string& s, double& r, double& g, double& b) {
        if (!isHexColor(s)) return false;
        r = std::stoi(s.substr(1, 2), nullptr, 16) / 255.0;
        g = std::stoi(s.substr(3, 2), nullptr, 16) / 255.0;
        b = std::stoi(s.substr(5, 2), nullptr, 16) / 255.0;
        return true;
    }

    // hue bucket of a saturated colour (for the rainbow test), -1 when grey or dark
    int hueBucket(double r, double g, double b) {
        double h, s, v;
        hsv(r, g, b, h, s, v);
        if (s < 0.35 || v < 0.3) return -1;
        static const double EDGES[] = {15, 40, 62, 165, 200, 255, 290, 340};
        for (int i = 0; i < 8; i++) {
            if (h < EDGES[i]) return i;
        }
        return 0;
    }

    Words colourWords(double r, double g, double b, bool ramp) {
        double h, s, v;
        hsv(r, g, b, h, s, v);
        if (v < 0.16) return {"black", "black", "黒"};
        if (s < 0.18) {
            if (v > 0.86 && !ramp) return {"white", "white", "白"};
            if (ramp) return v > 0.4 ? Words{"silver", "silver", "銀"} : Words{"black", "black", "黒"};
            return {"grey", "grey", "灰色"};
        }
        if (h < 12 || h >= 340) return {"red", "red", "赤"};
        if (h < 38) return v < 0.55 ? Words{"brown", "brown", "茶"} : Words{"orange", "orange", "オレンジ"};
        if (h < 58) return v < 0.45 ? Words{"brown", "brown", "茶"} : Words{"gold", "gold", "金"};
        if (h < 70) return {"yellow", "yellow", "黄"};
        if (h < 165) return {"green", "green", "緑"};
        if (h < 200) return {"cyan", "cyan", "水色"};
        if (h < 255) return {"blue", "blue", "青"};
        if (h < 290) return {"purple", "purple", "紫"};
        return {"pink", "pink", "ピンク"};
    }

    // the colour a gradient reads as: rainbow when it runs through four hue buckets, else its
    // stops' mean
    Words gradientWords(const Json& gradient) {
        if (!gradient.is_object() || !gradient.contains("stops") || !gradient["stops"].is_array() || gradient["stops"].empty()) {
            return {"", "", ""};
        }
        std::set<int> buckets;
        double sr = 0, sg = 0, sb = 0;
        int n = 0;
        for (const Json& stop : gradient["stops"]) {
            double r, g, b;
            if (!stop.is_object() || !parseHex(stop.value("color", std::string()), r, g, b)) continue;
            const int bucket = hueBucket(r, g, b);
            if (bucket >= 0) buckets.insert(bucket);
            sr += r;
            sg += g;
            sb += b;
            n++;
        }
        if (buckets.size() >= 4) return {"rainbow", "rainbow", "虹色"};
        if (!n) return {"", "", ""};
        return colourWords(sr / n, sg / n, sb / n, true);
    }

    // the colour of an S80 fill (converted)
    Words fillWords(const Json& fill, bool& gradient) {
        gradient = false;
        if (!fill.is_object()) return {"white", "white", "白"};
        const std::string kind = fill.value("kind", std::string("solid"));
        if (kind != "solid" && kind != "image" && kind != "source") {
            gradient = true;
            return gradientWords(fill.contains("gradient") ? fill["gradient"] : NULL_JSON);
        }
        double r, g, b;
        if (parseHex(fill.value("color", std::string("#ffffff")), r, g, b)) return colourWords(r, g, b, false);
        return {"white", "white", "白"};
    }

    bool enabledIn(const Json& material, const char* key) {
        return material.is_object() && material.contains(key) && material[key].is_object() && material[key].value("enabled", false);
    }

    const std::map<std::string, Words>& patternWords() {
        static const std::map<std::string, Words> words = {
            {"zebra", {"zebra", "zebra", "ゼブラ"}},          {"tiger", {"tiger", "tiger", "虎"}},
            {"giraffe", {"giraffe", "giraffe", "キリン"}},    {"leopard", {"leopard", "leopard", "ヒョウ"}},
            {"checker", {"checker", "checker", "チェッカー"}}, {"hazard", {"hazard", "hazard", "警告ストライプ"}},
            {"stripes", {"stripes", "striped", "ストライプ"}}, {"dots", {"dots", "dotted", "ドット"}},
            {"sakura", {"sakura", "sakura", "桜"}},           {"seigaiha", {"seigaiha", "seigaiha wave", "青海波"}},
            {"asanoha", {"asanoha", "hemp-leaf", "麻の葉"}},  {"shippo", {"shippo", "seven-treasures", "七宝"}},
            {"kikko", {"kikko", "tortoise-shell", "亀甲"}},   {"uroko", {"uroko", "scale", "鱗"}},
        };
        return words;
    }

    const std::map<std::string, Words>& textureWords() {
        static const std::map<std::string, Words> words = {
            {"noise", {"", "noise", "ノイズ"}},           {"sparkle", {"glitter", "glitter", "ラメ"}},
            {"brushed", {"brushed", "brushed", "ブラッシュ"}}, {"hairline", {"brushed", "hairline", "ヘアライン"}},
            {"spun", {"", "spun", "スピン"}},             {"flakes", {"glitter", "flake", "フレーク"}},
            {"ripple", {"", "ripple", "波紋"}},           {"grunge", {"grunge", "grunge", "グランジ"}},
            {"clouds", {"", "cloud", "雲"}},              {"hammered", {"hammered", "hammered", "槌目"}},
            {"crumple", {"crumpled", "crumple", "クシャ"}}, {"liquid", {"liquid", "liquid", "液体"}},
        };
        return words;
    }

    const std::map<std::string, Words>& effectWords() {
        static const std::map<std::string, Words> words = {
            {"outer_glow", {"glow", "an outer glow", "外側の光彩"}},
            {"inner_glow", {"glow", "an inner glow", "内側の光彩"}},
            {"light_glow", {"glow", "a light glow", "ライトグロー"}},
            {"drop_shadow", {"shadow", "a drop shadow", "ドロップシャドウ"}},
            {"inner_shadow", {"shadow", "an inner shadow", "内側の影"}},
            {"outline", {"outline", "an outline", "アウトライン"}},
            {"gradient_overlay", {"gradient", "a gradient overlay", "グラデーションオーバーレイ"}},
            {"adjust", {"", "a colour adjustment", "色調補正"}},
            {"shatter", {"shatter", "a shatter", "破砕"}},
            {"gaussian_blur", {"blur", "a blur", "ぼかし"}},
        };
        return words;
    }

    const std::map<std::string, Words>& generatorWords() {
        static const std::map<std::string, Words> words = {
            {"glints", {"sparkle", "glints", "きらめき"}},       {"sunburst", {"rays", "a sunburst", "サンバースト"}},
            {"particles", {"particles", "particles", "パーティクル"}}, {"fire", {"fire", "fire", "炎"}},
            {"speed_lines", {"speed-lines", "speed lines", "集中線"}}, {"lightning", {"lightning", "lightning", "稲妻"}},
            {"aura", {"aura", "an aura", "オーラ"}},             {"backlight", {"rays", "a backlight", "逆光"}},
            {"lens_flare", {"flare", "a lens flare", "レンズフレア"}}, {"brush_swirl", {"brush", "a brush swirl", "筆の渦"}},
            {"light_streaks", {"streaks", "light streaks", "光の筋"}}, {"shockwave", {"shockwave", "a shockwave", "衝撃波"}},
            {"smoke", {"smoke", "smoke", "煙"}},                 {"ink_splatter", {"ink", "ink splatter", "墨しぶき"}},
            {"magic_circle", {"magic-circle", "a magic circle", "魔法陣"}}, {"fill", {"", "a fill", "塗りつぶし"}},
            {"pattern_fill", {"pattern", "a pattern fill", "パターン塗り"}},
        };
        return words;
    }

    const std::map<std::string, Words>& material3DWords() {
        static const std::map<std::string, Words> words = {
            {"gold", {"gold", "gold", "金"}},                   {"punchy_gold", {"gold", "bright gold", "鮮やかな金"}},
            {"pale_gold", {"gold", "pale gold", "淡い金"}},      {"rose_gold", {"rose-gold", "rose gold", "ローズゴールド"}},
            {"chrome", {"chrome", "chrome", "クローム"}},        {"silver", {"silver", "silver", "銀"}},
            {"copper", {"copper", "copper", "銅"}},              {"red_metal", {"metal", "red metal", "赤のメタル"}},
            {"blue_metal", {"metal", "blue metal", "青のメタル"}}, {"green_metal", {"metal", "green metal", "緑のメタル"}},
            {"purple_metal", {"metal", "purple metal", "紫のメタル"}}, {"black_gloss", {"black", "gloss black", "黒の光沢"}},
            {"white_gloss", {"white", "gloss white", "白の光沢"}}, {"rainbow", {"rainbow", "rainbow", "虹色"}},
            {"glass", {"glass", "glass", "ガラス"}},             {"plastic", {"plastic", "plastic", "プラスチック"}},
            {"emissive", {"glow", "glowing", "発光"}},           {"candy", {"candy", "candy", "キャンディ"}},
            {"diamond", {"diamond", "diamond", "ダイヤモンド"}}, {"ruby", {"ruby", "ruby", "ルビー"}},
            {"sapphire", {"sapphire", "sapphire", "サファイア"}}, {"emerald", {"emerald", "emerald", "エメラルド"}},
        };
        return words;
    }

    // a 3D material's words: its preset's, else its colour with what its values make it
    Words material3DOf(const Json& m, std::vector<std::string>* tags) {
        if (!m.is_object()) return {"", "plain", "無地"};
        const std::string preset = m.value("preset", std::string("custom"));
        auto it = material3DWords().find(preset);
        if (it != material3DWords().end()) {
            if (tags && !it->second.tag.empty()) tags->push_back(it->second.tag);
            if (tags && (preset.find("gold") != std::string::npos || preset == "chrome" || preset == "silver" || preset == "copper" ||
                         preset.find("metal") != std::string::npos)) {
                tags->push_back("metal");
            }
            return it->second;
        }
        Words c{"white", "white", "白"};
        if (m.contains("gradient") && m["gradient"].is_object()) {
            c = gradientWords(m["gradient"]);
        } else {
            double r, g, b;
            if (parseHex(m.value("color", std::string("#ffffff")), r, g, b)) c = colourWords(r, g, b, false);
        }
        if (c.en.empty()) c = {"white", "white", "白"};
        if (tags && !c.tag.empty()) tags->push_back(c.tag);
        const double metallic = m.value("metallic", 0.0), transmission = m.value("transmission", 0.0);
        const double emission = m.value("emission_intensity", 0.0), roughness = m.value("roughness", 0.3);
        if (transmission >= 0.3) {
            if (tags) tags->push_back("glass");
            return {c.tag, c.en + " glass", c.ja + "のガラス"};
        }
        if (metallic >= 0.6) {
            if (tags) tags->push_back("metal");
            return {c.tag, c.en + " metal", c.ja + "のメタル"};
        }
        if (emission > 0) {
            if (tags) tags->push_back("glow");
            return {c.tag, "glowing " + c.en, "発光する" + c.ja};
        }
        if (roughness >= 0.6) return {c.tag, "matte " + c.en, "マットな" + c.ja};
        return c;
    }

    const std::map<std::string, Words>& shapeWords() {
        static const std::map<std::string, Words> words = {
            {"rect", {"", "rectangle", "長方形"}},      {"parallelogram", {"", "slanted band", "斜めの帯"}},
            {"ellipse", {"", "ellipse", "楕円"}},       {"orb", {"orb", "orb", "球"}},
            {"polygon", {"", "polygon", "多角形"}},     {"star", {"star", "star", "星"}},
            {"burst", {"burst", "burst", "バースト"}},  {"shield", {"shield", "shield", "盾"}},
            {"speech", {"speech", "speech frame", "吹き出し"}}, {"shout", {"speech", "shout frame", "叫びの吹き出し"}},
            {"ribbon", {"ribbon", "ribbon", "リボン"}},
        };
        return words;
    }

    const std::map<std::string, Words>& adjustmentWords() {
        static const std::map<std::string, Words> words = {
            {"hue_saturation", {"", "Hue/Saturation", "色相・彩度"}}, {"curves", {"", "Curves", "トーンカーブ"}},
            {"photo_filter", {"", "Photo Filter", "レンズフィルター"}}, {"levels", {"", "Levels", "レベル補正"}},
            {"effects_only", {"", "effects-only", "エフェクトのみの"}}, {"brightness_contrast", {"", "Brightness/Contrast", "明るさ・コントラスト"}},
            {"color_balance", {"", "Color Balance", "カラーバランス"}}, {"gradient_map", {"", "Gradient Map", "グラデーションマップ"}},
        };
        return words;
    }

    void pushUnique(std::vector<std::string>& list, const std::string& v) {
        if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
    }

    // ---- the porter ----

    using FieldSpec = PachislopFieldRule;

    // where a converted value comes from: a concrete path for messages, a pattern for `dropped`
    struct Ctx {
        std::string path;
        std::string pattern;
        Ctx child(const std::string& key) const { return {path + "." + key, pattern + "." + key}; }
        Ctx index(size_t i) const { return {path + "[" + std::to_string(i) + "]", pattern + "[]"}; }
        Ctx mapKey(const std::string& key) const { return {path + "." + key, pattern + ".*"}; }
    };

    struct ItemLog {
        std::string ref;
        std::string kind;
        bool withheld = false;
        std::string file;
        int line = 0;
        std::string item;
        std::map<std::string, std::pair<std::string, int>> dropped;     // pattern -> reason, count
        std::vector<std::string> approximated;
        std::vector<std::string> mapped;
        std::vector<std::string> unmappable;
        size_t errors = 0;
    };

    struct GeneratorOut {
        bool ok = false;
        std::string type;
        Json params = Json::object();
        std::string follow;
    };

    class Porter {
    public:
        Porter(const Json& dump, const PortOptions& options, PortResult& result) : dump(dump), options(options), result(result) {}

        void run() {
            if (!readMap()) return;
            if (!TitleArtSchema::get().kind("style_3d")) {
                result.errors.push_back("this build has no 3D titles (JASPERENGINE_TITLE_ART_3D): S86's members are needed for the 3D looks");
                return;
            }
            if (dump.contains("looks") && dump["looks"].is_array()) {
                for (const Json& item : dump["looks"]) portLook(item);
            }
            if (dump.contains("templates") && dump["templates"].is_array()) {
                for (const Json& item : dump["templates"]) portTemplate(item);
            }
            portColourways();
            writeProvenance();
            result.ok = result.errors.empty();
        }

    private:
        const Json& dump;
        const PortOptions& options;
        PortResult& result;
        const TitleArtSchema& schema = TitleArtSchema::get();

        // the map
        struct IdEntry {
            std::string ref;
            bool withheld = false;
        };
        std::map<std::string, IdEntry> ids;
        std::map<std::string, FieldSpec> fields;
        std::vector<std::pair<std::vector<std::string>, std::string>> droppedPatterns;     // glob, reason
        std::map<std::string, std::string> droppedKinds;                                    // "kind.key" -> reason
        std::vector<std::string> droppedPatternText;

        // the item being ported
        ItemLog log;
        std::vector<ItemLog> logs;
        bool keepIds = false;
        std::set<std::string> usedIds;
        std::map<std::string, std::string> layerIds;        // Pachislop layer id -> ours (templates)

        // ---- map ----

        bool readMap() {
            const Json& map = options.map;
            if (!map.is_object()) {
                result.errors.push_back("pachislop-map.json is not a JSON object");
                return false;
            }
            for (const char* key : {"ids", "families", "names", "fonts", "environments", "fields", "dropped", "template_texts"}) {
                if (!map.contains(key)) result.errors.push_back(std::string("pachislop-map.json has no `") + key + "`");
            }
            if (!result.errors.empty()) return false;
            for (auto it = map["ids"].begin(); it != map["ids"].end(); ++it) {
                IdEntry e;
                if (it.value().is_string()) {
                    e.ref = it.value().get<std::string>();
                } else if (it.value().is_object()) {
                    e.ref = it.value().value("ref", std::string());
                    e.withheld = it.value().value("withheld", false);
                }
                if (e.ref.empty()) {
                    result.errors.push_back("pachislop-map.json ids." + it.key() + ": no ref");
                    continue;
                }
                ids[it.key()] = e;
            }
            for (auto it = map["fields"].begin(); it != map["fields"].end(); ++it) {
                fields[it.key()] = parsePachislopFieldRule(it.value());
            }
            for (const Json& d : map["dropped"]) {
                const std::string path = d.value("path", std::string());
                const std::string reason = d.value("reason", std::string());
                if (path.empty() || reason.empty()) {
                    result.errors.push_back("pachislop-map.json dropped: every entry needs a `path` and a `reason`");
                    continue;
                }
                if (path.rfind("kind:", 0) == 0) droppedKinds[path.substr(5)] = reason;
                else droppedPatterns.push_back({splitDots(path), reason});
            }
            return result.errors.empty();
        }

        const IdEntry* idOf(const std::string& pachislopId) const {
            auto it = ids.find(pachislopId);
            return it == ids.end() ? nullptr : &it->second;
        }

        FieldSpec fieldOf(const std::string& kind, const std::string& key) const {
            auto it = fields.find(kind + "." + key);
            if (it == fields.end()) {
                // a variant kind ("mask.layer") falls back to its base kind's exceptions
                const SchemaKind* k = schema.kind(kind);
                if (k && !k->base.empty()) it = fields.find(k->base + "." + key);
            }
            if (it != fields.end()) return it->second;
            FieldSpec f;
            f.to = snake(key);
            return f;
        }

        bool dropped(const Ctx& c, const std::string& kind, const std::string& key) {
            std::string reason;
            auto k = droppedKinds.find(kind + "." + key);
            if (k != droppedKinds.end()) reason = k->second;
            if (reason.empty()) {
                const std::vector<std::string> segments = splitDots(c.pattern);
                for (const auto& [glob, why] : droppedPatterns) {
                    if (globMatch(glob, 0, segments, 0)) {
                        reason = why;
                        break;
                    }
                }
            }
            if (reason.empty()) return false;
            auto& entry = log.dropped[c.pattern];
            entry.first = reason;
            entry.second++;
            return true;
        }

        void error(const Ctx& c, const std::string& message) {
            log.errors++;
            result.errors.push_back(log.ref + " (" + c.path + "): " + message);
        }

        void approximate(const std::string& note) { pushUnique(log.approximated, note); }

        std::string mintId(const std::string& path) {
            uint32_t h = fnv1a32(log.ref + "|" + path);
            std::string id = hex32(h);
            int salt = 1;
            while (!usedIds.insert(id).second) id = hex32(fnv1a32(log.ref + "|" + path + "#" + std::to_string(salt++)));
            return id;
        }

        // ---- generic conversion against our schema ----

        Json convertObject(const Json& in, const std::string& kindName, const Ctx& c, Json* lifted = nullptr) {
            const SchemaKind* kind = schema.kind(kindName);
            if (!kind) {
                error(c, "our schema has no kind `" + kindName + "`");
                return Json::object();
            }
            if (!in.is_object()) {
                error(c, "expected an object");
                return Json::object();
            }
            if (kindName == "material_3d") return material3D(in, c);
            Json out = Json::object();
            const SchemaKind* variant = kind;
            std::string variantName = kindName;
            std::string fromKey;
            if (!kind->discriminator.empty()) {
                for (auto it = in.begin(); it != in.end(); ++it) {
                    if (fieldOf(kindName, it.key()).to == kind->discriminator) fromKey = it.key();
                }
                if (fromKey.empty() || !in[fromKey].is_string()) {
                    error(c, "has no `" + kind->discriminator + "` (" + kind->label + ")");
                    return out;
                }
                const FieldSpec f = fieldOf(kindName, fromKey);
                const std::string value = enumValue(f, in[fromKey].get<std::string>());
                auto v = kind->variants.find(value);
                if (v == kind->variants.end() || !schema.kind(v->second)) {
                    error(c.child(fromKey), "`" + in[fromKey].get<std::string>() + "` is not a " + kind->label + " type this program has");
                    return out;
                }
                out[kind->discriminator] = value;
                variantName = v->second;
                variant = schema.kind(variantName);
            }
            for (auto it = in.begin(); it != in.end(); ++it) {
                if (it.key() == fromKey) continue;
                const Ctx cc = c.child(it.key());
                if (dropped(cc, variantName, it.key())) continue;
                const FieldSpec f = fieldOf(variantName, it.key());
                if (f.lift) {
                    if (lifted) (*lifted)[f.to] = it.value();
                    else error(cc, "a `^` field outside an object that takes it");
                    continue;
                }
                const SchemaMember* m = schema.member(*variant, f.to);
                if (!m) {
                    error(cc, "no mapping: our " + variant->label + " (" + variantName + ") has no member `" + f.to +
                                  "` and pachislop-map.json has no `fields` exception or `dropped` entry for it");
                    continue;
                }
                convertMember(*m, f, it.value(), cc, out);
            }
            return out;
        }

        std::string enumValue(const FieldSpec& f, const std::string& value) const {
            if (f.values.contains(value) && f.values[value].is_string()) return f.values[value].get<std::string>();
            return snake(value);
        }

        void convertMember(const SchemaMember& m, const FieldSpec& f, const Json& v, const Ctx& c, Json& out) {
            switch (m.type) {
                case ValueType::Id:
                    if (keepIds) out[m.key] = mintId(c.path);
                    else if (!dropped(c, "", m.key)) error(c, "an id, and this item keeps none (a `dropped` entry says why)");
                    return;
                case ValueType::Object:
                    if (v.is_null()) {
                        if (m.nullable) out[m.key] = nullptr;
                        else error(c, "null, and our `" + m.key + "` is not nullable");
                        return;
                    }
                    out[m.key] = m.kind == "material_3d" ? material3D(v, c) : convertObject(v, m.kind, c);
                    return;
                case ValueType::ObjectArray: {
                    if (!v.is_array()) {
                        error(c, "expected a list");
                        return;
                    }
                    Json list = Json::array();
                    for (size_t i = 0; i < v.size(); i++) {
                        const Ctx ci = c.index(i);
                        if (v[i].is_null() && m.nullable) list.push_back(nullptr);
                        else list.push_back(m.kind == "material_3d" ? material3D(v[i], ci) : convertObject(v[i], m.kind, ci));
                    }
                    out[m.key] = list;
                    return;
                }
                case ValueType::ObjectMap: {
                    if (!v.is_object()) {
                        error(c, "expected an object");
                        return;
                    }
                    Json map = Json::object();
                    for (auto it = v.begin(); it != v.end(); ++it) map[it.key()] = convertObject(it.value(), m.kind, c.mapKey(it.key()));
                    out[m.key] = map;
                    return;
                }
                case ValueType::Enum: {
                    if (!v.is_string()) {
                        error(c, "expected a string");
                        return;
                    }
                    const std::string value = enumValue(f, v.get<std::string>());
                    if (std::find(m.options.begin(), m.options.end(), value) == m.options.end()) {
                        error(c, "`" + v.get<std::string>() + "` has no counterpart among our " + m.key + " values (" + joinList(m.options, ", ") + ")");
                        return;
                    }
                    out[m.key] = value;
                    return;
                }
                case ValueType::Color:
                    out[m.key] = v.is_string() ? Json(lowerText(v.get<std::string>())) : v;
                    return;
                case ValueType::Vec2:
                case ValueType::Json:
                    if (f.component >= 0) {
                        if (!v.is_number()) {
                            error(c, "expected a number");
                            return;
                        }
                        Json& slot = out[m.key];
                        if (!slot.is_array()) slot = m.defaultValue.is_array() ? m.defaultValue : Json::array({0, 0});
                        while ((int)slot.size() <= f.component) slot.push_back(0);
                        slot[(size_t)f.component] = f.negate ? Json(-v.get<double>()) : v;
                    } else if (v.is_object() && v.contains("x") && v.contains("y")) {
                        out[m.key] = Json::array({v["x"], v["y"]});
                    } else {
                        out[m.key] = v;
                    }
                    return;
                case ValueType::Vec2List: {
                    if (!v.is_array()) {
                        error(c, "expected a list of points");
                        return;
                    }
                    Json list = Json::array();
                    for (const Json& p : v) list.push_back(p.is_object() ? Json::array({p.value("x", 0.0), p.value("y", 0.0)}) : p);
                    out[m.key] = list;
                    return;
                }
                case ValueType::AssetRef:
                    if (f.environment) {
                        out[m.key] = environmentRef(v, c);
                        return;
                    }
                    if (v.is_null()) {
                        out[m.key] = nullptr;
                        return;
                    }
                    error(c, "an asset reference (`" + v.dump() + "`): the dump carries no asset file to port");
                    return;
                case ValueType::Number:
                case ValueType::Integer:
                    if (!v.is_number()) {
                        error(c, "expected a number");
                        return;
                    }
                    out[m.key] = f.negate ? Json(-v.get<double>()) : v;
                    return;
                case ValueType::LayerRef: {
                    if (!v.is_string()) {
                        error(c, "expected a layer id");
                        return;
                    }
                    const std::string id = v.get<std::string>();
                    if (id.empty()) {
                        out[m.key] = "";
                        return;
                    }
                    auto it = layerIds.find(id);
                    if (it == layerIds.end()) {
                        error(c, "names layer `" + id + "`, which the item does not have");
                        return;
                    }
                    out[m.key] = it->second;
                    return;
                }
                case ValueType::ObjectArrayMap:
                case ValueType::AssetRefList:
                case ValueType::EffectParams:
                case ValueType::GeneratorParams:
                    error(c, "reached the generic mapping (" + std::string(titleart::valueTypeName(m.type)) + "): the port handles it in its own step");
                    return;
                default:
                    out[m.key] = v;
                    return;
            }
        }

        // a 2D reflection map's name as the asset a project gets on first use (§6.7.1, §6.12)
        Json environmentRef(const Json& v, const Ctx& c) {
            if (!v.is_string()) {
                error(c, "expected a reflection map name");
                return nullptr;
            }
            const std::string name = v.get<std::string>();
            const Json& envs = options.map["environments"];
            if (!envs.contains(name) || !envs[name].is_string()) {
                error(c, "reflection map `" + name + "` has no entry in pachislop-map.json environments");
                return nullptr;
            }
            const std::string id = envs[name].get<std::string>();
            const Json& manifest = options.environments;
            if (!manifest.is_object() || !manifest.contains("environments") || !manifest["environments"].contains(id) ||
                !manifest["environments"][id].contains("file")) {
                error(c, "environment `" + id + "` is not in S86's HDRI manifest");
                return nullptr;
            }
            approximate("the 2D reflection map `" + name + "` reads S86's HDRI environment `" + id +
                        "` (the photo of the same place), not Pachislop's own low-resolution panorama");
            return Json{{"path", "TitleArt/Environments/" + manifest["environments"][id]["file"].get<std::string>()}, {"guid", ""}};
        }

        // ---- 3D (S86 R67's converter, then a check that it carried every member) ----

        static std::string rename3D(const std::string& kind, const std::string& key) {
            static const std::map<std::string, std::string> renames = {
                {"metalness", "metallic"}, {"reflectivity", "env_multiplier"}, {"specBoost", "specular_multiplier"},
                {"emissive", "emission"}, {"emissiveIntensity", "emission_intensity"}, {"bevelMaterial", "bevel_material"},
                {"toneMapping", "tone_mapping"}};
            if (kind == "band_3d" && key == "shape") return "plate";
            if (kind == "light_3d" && (key == "x" || key == "y" || key == "z")) return "position";
            auto it = renames.find(key);
            return it != renames.end() ? it->second : snake(key);
        }

        void check3D(const Json& in, Json& out, const Ctx& c, const std::string& kindName) {
            if (!in.is_object() || !out.is_object()) return;
            const SchemaKind* kind = schema.kind(kindName);
            for (auto it = in.begin(); it != in.end(); ++it) {
                const Ctx cc = c.child(it.key());
                const std::string to = rename3D(kindName, it.key());
                if (dropped(cc, kindName, it.key())) {
                    if (to != "position") out.erase(to);
                    continue;
                }
                if (!out.contains(to)) {
                    if (it.key() == "link" && it.value().is_string()) {
                        out["link"] = it.value();     // a material link name (S80's member, R67 leaves it out)
                        continue;
                    }
                    error(cc, "no mapping: S86's title3DFromPachislop does not carry it and pachislop-map.json has no `dropped` entry for it");
                    continue;
                }
                const SchemaMember* m = kind ? schema.member(*kind, to) : nullptr;
                if (!m) continue;       // validate3D names it
                if (m->type == ValueType::Object && it.value().is_object() && out[to].is_object()) {
                    check3D(it.value(), out[to], cc, m->kind);
                } else if (m->type == ValueType::ObjectArray && it.value().is_array() && out[to].is_array()) {
                    for (size_t i = 0; i < it.value().size() && i < out[to].size(); i++) check3D(it.value()[i], out[to][i], cc.index(i), m->kind);
                }
            }
        }

        // every member S86's converter wrote exists in our schema and every enum value is one of ours
        void validate3D(Json& out, const std::string& kindName, const Ctx& c) {
            const SchemaKind* kind = schema.kind(kindName);
            if (!kind || !out.is_object()) return;
            for (auto it = out.begin(); it != out.end(); ++it) {
                const Ctx cc = c.child(it.key());
                const SchemaMember* m = schema.member(*kind, it.key());
                if (!m) {
                    error(cc, "S86's converter wrote `" + it.key() + "`, which our " + kind->label + " has no member for");
                    continue;
                }
                Json& v = it.value();
                if (m->type == ValueType::Enum && v.is_string() &&
                    std::find(m->options.begin(), m->options.end(), v.get<std::string>()) == m->options.end()) {
                    const std::string fixed = snake(v.get<std::string>());
                    if (std::find(m->options.begin(), m->options.end(), fixed) != m->options.end()) v = fixed;
                    else error(cc, "`" + v.get<std::string>() + "` is not one of our " + m->key + " values (" + joinList(m->options, ", ") + ")");
                } else if (m->type == ValueType::Object && v.is_object()) {
                    validate3D(v, m->kind, cc);
                } else if (m->type == ValueType::ObjectArray && v.is_array()) {
                    for (size_t i = 0; i < v.size(); i++) {
                        if (v[i].is_object()) validate3D(v[i], m->kind, cc.index(i));
                    }
                }
            }
        }

        Json material3D(const Json& in, const Ctx& c) {
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
            Json out = titleart::material3DFromPachislop(in);
            check3D(in, out, c, "material_3d");
            validate3D(out, "material_3d", c);
            return out;
#else
            (void)in;
            error(c, "this build has no 3D titles");
            return Json::object();
#endif
        }

        // a style3d as style_3d; the lighting is returned apart (a look's own member)
        Json style3D(const Json& in, const Ctx& c, Json& lighting) {
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
            Json out = titleart::title3DFromPachislop(in);
            check3D(in, out, c, "style_3d");
            validate3D(out, "style_3d", c);
            if (in.contains("shadow") && in["shadow"].is_object() && in["shadow"].value("enabled", false)) {
                approximate("its 3D shadow catcher (enabled in Pachislop) is not drawn: S86 has none");
            }
            if (out.contains("bands")) {
                for (size_t i = 0; i < out["bands"].size(); i++) {
                    const Ctx ci = c.child("bands").index(i).child("id");
                    if (keepIds) out["bands"][i]["id"] = mintId(ci.path);
                    else if (dropped(ci, "band_3d", "id")) out["bands"][i].erase("id");
                    else error(ci, "an id, and this item keeps none");
                }
            }
            if (out.contains("lighting") && out["lighting"].contains("lights")) {
                Json& lights = out["lighting"]["lights"];
                for (size_t i = 0; i < lights.size(); i++) {
                    const Ctx ci = c.child("lighting").child("lights").index(i).child("id");
                    if (keepIds) lights[i]["id"] = mintId(ci.path);
                    else if (dropped(ci, "light_3d", "id")) lights[i].erase("id");
                    else error(ci, "an id, and this item keeps none");
                }
            }
            lighting = out.contains("lighting") ? out["lighting"] : NULL_JSON;
            return out;
#else
            (void)in;
            (void)lighting;
            error(c, "this build has no 3D titles");
            return Json::object();
#endif
        }

        // ---- effects and generators (S83's map, then a check against our registry) ----

        Json paramValue(const titleart::ParamDef& pd, const Json& v, const Ctx& c) {
            switch (pd.kind) {
                case titleart::ParamKind::Gradient: return v.is_object() ? convertObject(v, "gradient", c) : v;
                case titleart::ParamKind::Curve: return v.is_object() ? convertObject(v, "curve", c) : v;
                case titleart::ParamKind::Adjustment: return v.is_object() ? convertObject(v, "adjustment", c) : v;
                case titleart::ParamKind::Material: return v.is_object() ? convertObject(v, "material", c) : v;
                case titleart::ParamKind::Enum: {
                    if (v.is_string()) {
                        const std::string s = v.get<std::string>();
                        const bool known = std::any_of(pd.options.begin(), pd.options.end(), [&](const titleart::EnumOption& o) { return s == o.value; });
                        if (!known) error(c, "`" + s + "` is not one of our " + pd.key + " options");
                    }
                    return v;
                }
                case titleart::ParamKind::Vec2:
                    if (v.is_object() && v.contains("x") && v.contains("y")) return Json::array({v["x"], v["y"]});
                    return v;
                default:
                    return v;
            }
        }

        Json effectList(const Json& list, const Ctx& c) {
            Json out = Json::array();
            if (list.is_null()) return out;
            if (!list.is_array()) {
                error(c, "expected a list of effects");
                return out;
            }
            for (size_t i = 0; i < list.size(); i++) {
                const Ctx ci = c.index(i);
                const Json& e = list[i];
                if (!e.is_object() || !e.contains("type") || !e["type"].is_string()) {
                    error(ci, "an effect needs a `type`");
                    continue;
                }
                const std::string type = e["type"].get<std::string>();
                for (auto it = e.begin(); it != e.end(); ++it) {
                    static const std::set<std::string> known = {"id", "type", "enabled", "params", "fromPreset"};
                    if (known.count(it.key())) continue;
                    if (!dropped(ci.child(it.key()), "effect", it.key())) error(ci.child(it.key()), "no mapping for this effect member");
                }
                Json entry = Json::object();
                if (e.contains("id")) {
                    const Ctx cid = ci.child("id");
                    if (keepIds) entry["id"] = mintId(cid.path);
                    else if (!dropped(cid, "effect", "id")) error(cid, "an id, and this item keeps none");
                }
                Json params = Json::object();
                if (e.contains("params") && e["params"].is_object()) {
                    for (auto p = e["params"].begin(); p != e["params"].end(); ++p) {
                        if (!dropped(ci.child("params").child(p.key()), "fx:" + type, p.key())) params[p.key()] = p.value();
                    }
                }
                const editor::PachislopFxConversion conv = editor::convertPachislopEffect(type, params);
                if (!conv.ok) {
                    error(ci, "S83's effect map: " + conv.error);
                    continue;
                }
                const titleart::EffectDef* def = titleart::findEffect(conv.type);
                if (!def) {
                    error(ci, "our effect `" + conv.type + "` (from `" + type + "`) is not registered in this build");
                    continue;
                }
                entry["type"] = conv.type;
                entry["enabled"] = e.value("enabled", true);
                Json ours = Json::object();
                for (auto p = conv.params.begin(); p != conv.params.end(); ++p) {
                    const titleart::ParamDef* pd = titleart::findParam(def->params, p.key());
                    if (!pd) {
                        error(ci.child("params"), "S83's map gives `" + p.key() + "`, which our " + conv.type + " has no param for");
                        continue;
                    }
                    ours[p.key()] = paramValue(*pd, p.value(), ci.child("params").child(p.key()));
                }
                entry["params"] = ours;
                if (e.value("fromPreset", false)) entry["from_look"] = true;
                pushUnique(log.mapped, "effects through S83's map (" + type + " -> " + conv.type + ")");
                out.push_back(normalizedEntry(entry, "effect", ci));
            }
            return out;
        }

        // S80 keeps at most 32 stops; Pachislop's foil and glint gradients have up to 70: the stops that
        // change the ramp least go first (an approximation the report lists)
        size_t maxStops() const { return schema.member(*schema.kind("gradient"), "stops")->maxItems; }

        void thinGradients(Json& value) {
            if (value.is_array()) {
                for (Json& v : value) thinGradients(v);
                return;
            }
            if (!value.is_object()) return;
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key() == "stops" && it.value().is_array() && it.value().size() > maxStops()) thinStops(it.value());
                else thinGradients(it.value());
            }
        }

        void thinStops(Json& stops) {
            const size_t before = stops.size();
            auto colorOf = [](const Json& stop) {
                titleart::TitleColor c;
                if (stop.contains("color")) titleart::parseTitleColor(stop["color"], c);
                return c;
            };
            while (stops.size() > maxStops()) {
                size_t best = 1;
                double bestError = std::numeric_limits<double>::max();
                for (size_t i = 1; i + 1 < stops.size(); i++) {
                    const double p0 = stops[i - 1].value("pos", 0.0), p1 = stops[i + 1].value("pos", 1.0), p = stops[i].value("pos", 0.5);
                    const double t = p1 > p0 ? (p - p0) / (p1 - p0) : 0.0;
                    const titleart::TitleColor a = colorOf(stops[i - 1]), b = colorOf(stops[i + 1]), m = colorOf(stops[i]);
                    const double error = std::abs(a.r + (b.r - a.r) * t - m.r) + std::abs(a.g + (b.g - a.g) * t - m.g) + std::abs(a.b + (b.b - a.b) * t - m.b);
                    if (error < bestError) {
                        bestError = error;
                        best = i;
                    }
                }
                stops.erase(stops.begin() + (long)best);
            }
            approximate("a gradient of " + std::to_string(before) + " stops thinned to " + std::to_string(stops.size()) + " (S80's limit)");
        }

        // an object of `kind` normalized (defaults filled, ranges clamped), its warnings as notes
        Json normalizedEntry(Json value, const char* kind, const Ctx& c) {
            thinGradients(value);
            std::vector<titleart::TitleWarning> warnings;
            titleart::NormalizeContext context;
            context.warnings = &warnings;
            titleart::normalizeTitleArtObject(value, kind, c.path, context);
            for (const titleart::TitleWarning& w : warnings) noteWarning(w.path, w.message, c);
            return value;
        }

        void noteWarning(const std::string& path, const std::string& message, const Ctx& c) {
            if (message.find("clamped") != std::string::npos) approximate(path + ": " + message);
            else error(c, "normalize: " + path + ": " + message);
        }

        GeneratorOut generator(const std::string& pachislopType, const Json& params, const Ctx& c) {
            GeneratorOut out;
            const editor::TitlePachislopRow* row = editor::findTitlePachislopGenerator(pachislopType);
            if (!row) {
                error(c, "Pachislop generator `" + pachislopType + "` has no row in S83's map");
                return out;
            }
            const titleart::GeneratorDef* def = titleart::findGenerator(row->type);
            if (!def) {
                error(c, "our generator `" + row->type + "` is not registered in this build");
                return out;
            }
            Json filtered = Json::object();
            const size_t errorsBefore = log.errors;
            if (params.is_object()) {
                for (auto it = params.begin(); it != params.end(); ++it) {
                    const Ctx cp = c.child(it.key());
                    if (dropped(cp, "gen:" + pachislopType, it.key())) continue;
                    using Kind = editor::TitlePachislopParam::Kind;
                    const editor::TitlePachislopParam target = editor::titlePachislopGeneratorParam(*row, it.key());
                    if (target.kind == Kind::Param || target.kind == Kind::Component) {
                        const titleart::ParamDef* pd = titleart::findParam(def->params, target.key);
                        if (!pd || (target.kind == Kind::Component && pd->kind != titleart::ParamKind::Vec2)) {
                            error(cp, "no mapping: S83's map sends it to `" + target.key + "`, which our " + row->type + " has no param for");
                            continue;
                        }
                    } else if (target.kind == Kind::RowField) {
                        const titleart::ParamDef* rows = titleart::findParam(def->params, row->rowInto);
                        if (!rows || !titleart::findParam(rows->rows, target.field)) {
                            error(cp, "no mapping: our " + row->type + " rows have no `" + target.field + "`");
                            continue;
                        }
                    }
                    filtered[it.key()] = it.value();
                }
            }
            std::vector<std::string> notes;
            const editor::TitlePachislopGenerator conv = editor::convertPachislopGenerator(pachislopType, filtered, &notes);
            if (log.errors == errorsBefore) {
                for (const std::string& n : notes) error(c, "S83's generator map: " + n);
            }
            for (auto p = conv.params.begin(); p != conv.params.end(); ++p) {
                const titleart::ParamDef* pd = titleart::findParam(def->params, p.key());
                if (pd && pd->kind == titleart::ParamKind::Enum) paramValue(*pd, p.value(), c.child(p.key()));
            }
            out.ok = log.errors == errorsBefore;
            out.type = conv.type;
            out.params = conv.params;
            out.follow = conv.follow;
            pushUnique(log.mapped, "generators through S83's map (" + pachislopType + " -> " + conv.type + ")");
            return out;
        }

        // ---- names, tags, synonyms, descriptions ----

        // the overrides.json / names section of the item's kind (a look and a template may share a ref)
        static const char* sectionOf(const std::string& kind) {
            return kind == "look" ? "looks" : kind == "template" ? "templates" : "color_schemes";
        }

        bool names(const std::string& ref, Json& out, const Ctx& c) {
            const Json& all = options.map["names"];
            const char* section = sectionOf(log.kind);
            const Json& map = all.contains(section) ? all[section] : NULL_JSON;
            if (!map.is_object() || !map.contains(ref) || !map[ref].is_object() || map[ref].value("en", std::string()).empty() ||
                map[ref].value("ja", std::string()).empty()) {
                error(c, std::string("pachislop-map.json names.") + section + " has no {en, ja} for `" + ref + "`");
                return false;
            }
            out = Json{{"en", map[ref]["en"]}, {"ja", map[ref]["ja"]}};
            return true;
        }

        Json synonymsOf(const std::vector<std::string>& tags) const {
            Json en = Json::array(), ja = Json::array();
            std::set<std::string> seenEn, seenJa;
            if (options.map.contains("tag_synonyms")) {
                const Json& vocab = options.map["tag_synonyms"];
                for (const std::string& tag : tags) {
                    if (!vocab.contains(tag)) continue;
                    for (const Json& s : vocab[tag].value("en", Json::array())) {
                        if (s.is_string() && seenEn.insert(s.get<std::string>()).second) en.push_back(s);
                    }
                    for (const Json& s : vocab[tag].value("ja", Json::array())) {
                        if (s.is_string() && seenJa.insert(s.get<std::string>()).second) ja.push_back(s);
                    }
                }
            }
            return Json{{"en", en}, {"ja", ja}};
        }

        // name words the vocabulary knows ("classic-gold" -> classic, gold)
        void nameTags(const Json& name, std::vector<std::string>& tags) const {
            if (!options.map.contains("tag_synonyms")) return;
            const Json& vocab = options.map["tag_synonyms"];
            const std::string slug = slugOf(name.value("en", std::string()));
            std::string word;
            for (size_t i = 0; i <= slug.size(); i++) {
                if (i == slug.size() || slug[i] == '-') {
                    if (vocab.contains(word)) pushUnique(tags, word);
                    word.clear();
                } else {
                    word += slug[i];
                }
            }
        }

        struct Described {
            std::vector<std::string> tags;
            std::string en, ja;
        };

        // the 2D style's sentence halves and tags
        void describeStyle2D(const Json& style, Described& d, std::vector<std::string>& extrasEn, std::vector<std::string>& extrasJa,
                             std::string& faceEn, std::string& faceJa, std::string& rimsEn, std::string& rimsJa) {
            const Json face = style.contains("face") ? style["face"] : Json::object();
            bool gradient = false;
            Words colour = fillWords(face.contains("fill") ? face["fill"] : NULL_JSON, gradient);
            if (colour.en.empty()) colour = {"", "coloured", "色付き"};
            pushUnique(d.tags, colour.tag);
            std::vector<std::string> featEn, featJa;
            std::string patternEn, patternJa;
            if (face.contains("pattern") && face["pattern"].is_object()) {
                const std::string kind = face["pattern"].value("kind", std::string("none"));
                auto p = patternWords().find(kind);
                if (kind != "none" && face["pattern"].value("opacity", 1.0) > 0) {
                    pushUnique(d.tags, "pattern");
                    if (p != patternWords().end()) {
                        pushUnique(d.tags, p->second.tag);
                        patternEn = p->second.en;
                        patternJa = p->second.ja;
                    }
                }
            }
            const bool glass = enabledIn(face, "glass");
            if (gradient) pushUnique(d.tags, "gradient");
            if (glass) pushUnique(d.tags, "glass");
            faceEn = article(colour.en) + " " + colour.en + (gradient ? " gradient" : "") + (glass ? " glass" : "") + " face";
            faceJa = colour.ja + "の" + (gradient ? "グラデーションの" : "") + (glass ? "ガラスの" : "") + "面";
            if (!patternEn.empty()) {
                faceEn += " with " + patternEn + " marks";
                featJa.push_back(patternJa + "柄");
            }
            if (enabledIn(face, "bevel")) {
                pushUnique(d.tags, "bevel");
                featEn.push_back("a bevel");
                featJa.push_back("ベベル");
            }
            if (enabledIn(face, "finish")) {
                pushUnique(d.tags, "metal");
                featEn.push_back("a gradient-map finish");
                featJa.push_back("グラデーションマップ仕上げ");
            }
            if (enabledIn(face, "reflection")) {
                pushUnique(d.tags, "chrome");
                pushUnique(d.tags, "metal");
                featEn.push_back("a mirror reflection");
                featJa.push_back("映り込み");
            }
            if (enabledIn(face, "gloss")) {
                pushUnique(d.tags, "gloss");
                featEn.push_back("a gloss band");
                featJa.push_back("光沢の帯");
            }
            if (face.contains("texture") && face["texture"].is_object()) {
                auto t = textureWords().find(face["texture"].value("kind", std::string("none")));
                if (t != textureWords().end()) {
                    pushUnique(d.tags, t->second.tag);
                    featEn.push_back(article(t->second.en) + " " + t->second.en + " texture");
                    featJa.push_back(t->second.ja + "のテクスチャ");
                }
            }
            if (enabledIn(face, "inner_glow")) {
                featEn.push_back("an inner glow");
                featJa.push_back("内側の光彩");
            }
            if (enabledIn(face, "sheen")) {
                featEn.push_back("a sheen");
                featJa.push_back("つや");
            }
            if (enabledIn(face, "satin")) {
                featEn.push_back("a satin shade");
                featJa.push_back("サテン");
            }
            if (!featEn.empty()) faceEn += (patternEn.empty() ? " with " : " and ") + joinEnglish(featEn);
            if (!featJa.empty()) faceJa += "（" + joinList(featJa, "、") + "）";
            std::vector<std::string> rimEn, rimJa;
            bool plate = false;
            if (style.contains("rims") && style["rims"].is_array()) {
                for (const Json& rim : style["rims"]) {
                    if (!rim.is_object() || !rim.value("enabled", true)) continue;
                    bool g = false;
                    Words c = fillWords(rim.contains("material") && rim["material"].is_object() && rim["material"].contains("fill") ? rim["material"]["fill"] : NULL_JSON, g);
                    if (c.en.empty()) c = {"", "coloured", "色付き"};
                    const Json offset = rim.contains("offset") ? rim["offset"] : Json::array({0, 0});
                    const bool base = offset.is_array() && offset.size() == 2 && (offset[0].get<double>() != 0 || offset[1].get<double>() != 0);
                    if (rim.contains("plate") && rim["plate"].is_object() && rim["plate"].value("kind", std::string("contour")) != "contour") plate = true;
                    rimEn.push_back(base ? article(c.en) + " " + c.en + " dropped base" : c.en);
                    rimJa.push_back(base ? c.ja + "の台" : c.ja);
                }
            }
            if (!rimEn.empty()) {
                pushUnique(d.tags, "outline");
                rimsEn = numberWord(rimEn.size()) + (rimEn.size() == 1 ? " rim" : " rims") + " (" + joinList(rimEn, ", ") + ")";
                rimsJa = std::to_string(rimJa.size()) + "本の縁（" + joinList(rimJa, "、") + "）";
                if (plate) {
                    pushUnique(d.tags, "plate");
                    rimsEn += " on a plate";
                    rimsJa += "のプレート";
                }
            }
            if (style.value("knockout", false)) {
                pushUnique(d.tags, "hollow");
                extrasEn.push_back("a hollow face");
                extrasJa.push_back("中抜き");
            }
            if (enabledIn(style, "erode")) {
                pushUnique(d.tags, "brush");
                extrasEn.push_back("dry-brush edges");
                extrasJa.push_back("かすれた縁");
            }
        }

        void describeExtras(const Json& look, Described& d, std::vector<std::string>& extrasEn, std::vector<std::string>& extrasJa) {
            for (const char* key : {"effects", "group_effects"}) {
                if (!look.contains(key) || !look[key].is_array()) continue;
                for (const Json& e : look[key]) {
                    if (!e.value("enabled", true)) continue;
                    auto w = effectWords().find(e.value("type", std::string()));
                    if (w == effectWords().end()) continue;
                    pushUnique(d.tags, w->second.tag);
                    pushUnique(extrasEn, w->second.en);
                    pushUnique(extrasJa, w->second.ja);
                }
            }
            if (look.contains("generators") && look["generators"].is_array()) {
                for (const Json& g : look["generators"]) {
                    auto w = generatorWords().find(g.value("type", std::string()));
                    if (w == generatorWords().end()) continue;
                    pushUnique(d.tags, w->second.tag);
                    pushUnique(extrasEn, w->second.en);
                    pushUnique(extrasJa, w->second.ja);
                }
            }
        }

        // a 3D style's sentence halves and tags
        void describeStyle3D(const Json& s, Described& d, std::string& headEn, std::string& headJa, std::string& bandsEn, std::string& bandsJa) {
            const std::string mode = s.value("mode", std::string("extrude"));
            const Words face = material3DOf(s.contains("face") ? s["face"] : Json::object(), &d.tags);
            const Json bevel = s.contains("bevel") ? s["bevel"] : Json::object();
            const bool bevelled = bevel.is_object() && bevel.value("width", 0.0) > 0 && mode == "extrude";
            if (mode == "orb") {
                pushUnique(d.tags, "orb");
                headEn = "A 3D orb with " + article(face.en) + " " + face.en + " body";
                headJa = face.ja + "の3Dの球";
            } else if (mode == "inflate") {
                pushUnique(d.tags, "inflate");
                headEn = "A 3D inflated title with " + article(face.en) + " " + face.en + " face";
                headJa = face.ja + "の面のぷっくり膨らんだ3D文字";
            } else {
                headEn = "A 3D extruded title with " + article(face.en) + " " + face.en + " face";
                headJa = face.ja + "の面の3D押し出し文字";
            }
            if (bevelled) {
                pushUnique(d.tags, "bevel");
                headEn += " and a " + bevel.value("profile", std::string("round")) + " bevel";
                headJa += "（ベベル付き）";
            }
            const Json skew = s.contains("skew") ? s["skew"] : Json::array({0, 0});
            if (skew.is_array() && skew.size() == 2 && (skew[0].get<double>() != 0 || skew[1].get<double>() != 0)) {
                pushUnique(d.tags, "pop-out");
                headEn += ", extruded at an angle";
                headJa += "、斜めの押し出し";
            }
            std::vector<std::string> en, ja;
            if (s.contains("bands") && s["bands"].is_array()) {
                for (const Json& b : s["bands"]) {
                    if (!b.is_object() || !b.value("enabled", true)) continue;
                    const Words w = material3DOf(b.contains("material") ? b["material"] : Json::object(), &d.tags);
                    en.push_back(w.en);
                    ja.push_back(w.ja);
                }
            }
            if (!en.empty()) {
                pushUnique(d.tags, "bands");
                pushUnique(d.tags, "outline");
                bandsEn = numberWord(en.size()) + (en.size() == 1 ? " band" : " bands") + " (" + joinList(en, ", ") + ")";
                bandsJa = std::to_string(ja.size()) + "本の帯（" + joinList(ja, "、") + "）";
            }
            if (s.contains("glint") && s["glint"].is_object() && s["glint"].value("enabled", false)) pushUnique(d.tags, "sparkle");
        }

        Described describeLook(const Json& look) {
            Described d;
            pushUnique(d.tags, look.value("family", std::string()));
            const bool is3D = look.value("dimension", std::string()) == "3d";
            pushUnique(d.tags, is3D ? "3d" : "2d");
            std::vector<std::string> extrasEn, extrasJa;
            std::string firstEn, firstJa, secondEn, secondJa;
            if (is3D) {
                describeStyle3D(look.contains("style_3d") ? look["style_3d"] : Json::object(), d, firstEn, firstJa, secondEn, secondJa);
            } else {
                describeStyle2D(look.contains("style") ? look["style"] : Json::object(), d, extrasEn, extrasJa, firstEn, firstJa, secondEn, secondJa);
                if (!firstEn.empty()) firstEn[0] = (char)std::toupper((unsigned char)firstEn[0]);
            }
            describeExtras(look, d, extrasEn, extrasJa);
            nameTags(look.contains("name") ? look["name"] : Json::object(), d.tags);
            std::vector<std::string> partsEn = {firstEn}, partsJa = {firstJa};
            if (!secondEn.empty()) {
                partsEn.push_back(secondEn);
                partsJa.push_back(secondJa);
            }
            for (const std::string& e : extrasEn) partsEn.push_back(e);
            for (const std::string& j : extrasJa) partsJa.push_back(j);
            d.en = partsEn.size() == 1 ? partsEn[0] + "." : partsEn[0] + ", " + joinEnglish(std::vector<std::string>(partsEn.begin() + 1, partsEn.end())) + ".";
            d.ja = joinList(partsJa, "、") + "。";
            return d;
        }

        void finishTags(std::vector<std::string>& tags) const {
            std::vector<std::string> out;
            for (const std::string& t : tags) {
                bool ok = !t.empty();
                for (char ch : t) ok = ok && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-');
                if (ok) pushUnique(out, t);
            }
            if (out.size() > TITLE_LIBRARY_MAX_TAGS) out.resize(TITLE_LIBRARY_MAX_TAGS);
            tags = out;
        }

        // ---- looks ----

        Json fontPair(const std::string& fontId, const Ctx& c) {
            const Json& fonts = options.map["fonts"];
            if (!fonts.contains(fontId) || !fonts[fontId].is_object()) {
                error(c, "font `" + fontId + "` has no entry in pachislop-map.json fonts");
                return nullptr;
            }
            Json pair = fonts[fontId];
            for (const char* key : {"latin", "ja"}) {
                const std::string id = pair.value(key, std::string());
                if (!options.fontPack.is_object() || !options.fontPack.contains("fonts") || !options.fontPack["fonts"].contains(id)) {
                    error(c, "font pack id `" + id + "` (" + key + " of `" + fontId + "`) is not in fonts.json");
                }
            }
            if (pair.contains("approximation") && pair["approximation"].is_string()) approximate(pair["approximation"].get<std::string>());
            Json out = Json{{"latin", pair.value("latin", std::string())}, {"ja", pair.value("ja", std::string())}};
            if (pair.contains("variations") && pair["variations"].is_object() && !pair["variations"].empty()) out["variations"] = pair["variations"];
            return out;
        }

        std::string partKeyOfLook(const std::string& key, const Ctx& c) {
            if (key == "face" || key == "side" || key == "bevel") return key;
            for (const char* prefix : {"rim", "band"}) {
                const std::string p = std::string(prefix) + ":";
                if (key.rfind(p, 0) == 0) {
                    const std::string n = key.substr(p.size());
                    if (!n.empty() && std::all_of(n.begin(), n.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return std::string(prefix) + "#" + n;
                }
            }
            error(c, "part key `" + key + "` is not face, side, rim:<n> or band:<n>");
            return {};
        }

        void beginItem(const std::string& ref, const char* kind, bool withheld, const std::string& file, int line, const std::string& item) {
            log = ItemLog();
            log.ref = ref;
            log.kind = kind;
            log.withheld = withheld;
            log.file = file;
            log.line = line;
            log.item = withheld ? std::string() : item;
            usedIds.clear();
            layerIds.clear();
        }

        Json provenanceOf() const {
            Json p = Json::object();
            p["source"] = "pachislop";
            p["commit"] = PACHISLOP_COMMIT;
            p["file"] = log.file;
            p["line"] = log.line;
            p["item"] = log.withheld ? Json(nullptr) : Json(log.item);
            p["license_note"] = PACHISLOP_LICENSE_NOTE;
            return p;
        }

        bool applyOverrides(Json& root) {
            const char* section = sectionOf(log.kind);
            if (!options.overrides.is_object() || !options.overrides.contains(section) || !options.overrides[section].is_object() ||
                !options.overrides[section].contains(log.ref)) {
                return false;
            }
            root.merge_patch(options.overrides[section][log.ref]);
            pushUnique(log.mapped, "tuned by overrides.json");
            return true;
        }

        bool checkDenied(const Json& root) {
            bool clean = true;
            for (const DeniedTermHit& hit : findDeniedTerms(root)) {
                error({hit.path, hit.path}, "holds the denied term `" + hit.term + "` (tools/titleart/denied_terms.txt)");
                clean = false;
            }
            return clean;
        }

        void portLook(const Json& item) {
            const Json& p = item.contains("preset") ? item["preset"] : NULL_JSON;
            const std::string sourceId = p.is_object() ? p.value("id", std::string()) : std::string();
            const IdEntry* entry = idOf(sourceId);
            const Json source = item.contains("source") ? item["source"] : Json::object();
            if (!p.is_object() || !entry) {
                result.errors.push_back("look #" + std::to_string(item.value("index", -1)) + " (" + sourceId + "): no preset or no entry in pachislop-map.json ids");
                return;
            }
            beginItem(entry->ref, "look", entry->withheld, source.value("file", std::string()), source.value("line", 0), sourceId);
            keepIds = false;
            const Ctx c{"look", "look"};
            const std::string ref = entry->ref;
            const size_t slash = ref.find('/');
            const std::string family = slash == std::string::npos ? std::string() : ref.substr(0, slash);
            if (!options.map["families"].contains(family)) error(c, "family `" + family + "` is not in pachislop-map.json families");

            Json look = Json::object();
            look["format"] = TITLE_LOOK_FORMAT;
            look["version"] = TITLE_LOOK_VERSION;
            look["id"] = ref;
            look["revision"] = 1;
            Json name;
            names(ref, name, c);
            look["name"] = name;
            look["family"] = family;
            const std::string kind = p.value("kind", std::string());
            if (kind != "title2d" && kind != "title3d") error(c.child("kind"), "`" + kind + "` is not title2d or title3d");
            const bool is3D = kind == "title3d";
            look["dimension"] = is3D ? "3d" : "2d";
            look["applies"] = "style";
            look["ref_size"] = p.contains("refSize") && p["refSize"].is_number() ? p["refSize"] : Json(TITLE_LOOK_DEFAULT_REF_SIZE);

            static const std::set<std::string> handled = {"id", "name", "kind", "style", "style3d", "links", "effects", "companions",
                                                          "groupEffects", "partEffects", "refSize", "fontId"};
            for (auto it = p.begin(); it != p.end(); ++it) {
                if (handled.count(it.key())) continue;
                if (!dropped(c.child(it.key()), "look", it.key())) error(c.child(it.key()), "no mapping and no `dropped` entry");
            }
            // a factory look without a font is shown in Pachislop's default face (DEFAULT_FONT_ID): the
            // pair says so, or a 確定 thumbnail would have nothing to draw kanji with
            const std::string lookFont = p.contains("fontId") && p["fontId"].is_string() ? p["fontId"].get<std::string>() : std::string("dela-gothic-one");
            {
                Json pair = fontPair(lookFont, c.child("fontId"));
                if (!pair.is_null()) look["font"] = pair;
            }
            Json lighting;
            if (is3D) {
                if (p.contains("style") && !dropped(c.child("style"), "look", "style")) error(c.child("style"), "a 3D look with a 2D style");
                Json style = style3D(p.contains("style3d") ? p["style3d"] : Json::object(), c.child("style3d"), lighting);
                style.erase("lighting");
                look["style_3d"] = style;
                pushUnique(log.mapped, "style3d through S86's title3DFromPachislop");
            } else {
                if (p.contains("style3d") && !dropped(c.child("style3d"), "look", "style3d")) error(c.child("style3d"), "a 2D look with a 3D style");
                look["style"] = convertObject(p.contains("style") ? p["style"] : Json::object(), "style2d", c.child("style"));
                pushUnique(log.mapped, "style (camelCase -> snake_case, " + std::to_string(look["style"].value("rims", Json::array()).size()) + " rims)");
            }
            look["effects"] = effectList(p.contains("effects") ? p["effects"] : NULL_JSON, c.child("effects"));
            Json parts = Json::object();
            if (p.contains("partEffects") && p["partEffects"].is_object()) {
                for (auto it = p["partEffects"].begin(); it != p["partEffects"].end(); ++it) {
                    const Ctx cp = c.child("partEffects").mapKey(it.key());
                    const std::string key = partKeyOfLook(it.key(), cp);
                    if (!key.empty()) parts[key] = effectList(it.value(), cp);
                }
            }
            look["part_effects"] = parts;
            Json generators = Json::array();
            if (p.contains("companions") && p["companions"].is_array()) {
                for (size_t i = 0; i < p["companions"].size(); i++) {
                    const Json& comp = p["companions"][i];
                    const Ctx cc = c.child("companions").index(i);
                    for (auto it = comp.begin(); it != comp.end(); ++it) {
                        static const std::set<std::string> known = {"generator", "place", "blend", "opacity", "effects", "outer"};
                        if (!known.count(it.key()) && !dropped(cc.child(it.key()), "companion", it.key())) error(cc.child(it.key()), "no mapping for this companion member");
                    }
                    const Json gen = comp.contains("generator") ? comp["generator"] : Json::object();
                    const std::string type = gen.value("type", std::string());
                    for (auto it = gen.begin(); it != gen.end(); ++it) {
                        if (it.key() != "type" && it.key() != "params" && !dropped(cc.child("generator").child(it.key()), "generator", it.key())) {
                            error(cc.child("generator").child(it.key()), "no mapping for this generator member");
                        }
                    }
                    GeneratorOut g = generator(type, gen.contains("params") ? gen["params"] : Json::object(), cc.child("generator").child("params"));
                    if (!g.ok) continue;
                    if (!g.follow.empty()) approximate("a companion's own `follow` is replaced by its look's layer");
                    Json entryJson = Json::object();
                    entryJson["type"] = g.type;
                    entryJson["place"] = comp.value("place", std::string("above"));
                    entryJson["scope"] = comp.value("outer", false) ? "group" : "layer";
                    entryJson["blend"] = comp.value("blend", std::string("normal"));
                    entryJson["opacity"] = comp.contains("opacity") && comp["opacity"].is_number() ? comp["opacity"] : Json(1);
                    Json ref2 = normalizedEntry(Json{{"type", g.type}, {"params", g.params}}, "generator_ref", cc);
                    entryJson["params"] = ref2["params"];
                    entryJson["effects"] = effectList(comp.contains("effects") ? comp["effects"] : NULL_JSON, cc.child("effects"));
                    generators.push_back(entryJson);
                }
            }
            look["generators"] = generators;
            look["group_effects"] = effectList(p.contains("groupEffects") ? p["groupEffects"] : NULL_JSON, c.child("groupEffects"));
            if (is3D && lighting.is_object()) look["lighting"] = normalizedEntry(lighting, "lighting_3d", c.child("style3d").child("lighting"));
            Json links = Json::object();
            if (p.contains("links") && p["links"].is_object()) {
                for (auto it = p["links"].begin(); it != p["links"].end(); ++it) {
                    links[it.key()] = Json{{"name", it.value().is_object() ? it.value().value("name", it.key()) : it.key()}};
                }
            }
            look["links"] = links;
            look["lint_allow"] = Json::array();
            look["check_allow"] = Json::array();
            look["provenance"] = provenanceOf();
            lowercaseColours(look);
            // style normalized on its own to see what clamps (canonicalLookJson's normalize is silent)
            if (is3D) normalizedEntry(look["style_3d"], "style_3d", c.child("style3d"));
            else normalizedEntry(look["style"], "style2d", c.child("style"));

            Described d = describeLook(look);
            finishTags(d.tags);
            look["description"] = Json{{"en", d.en}, {"ja", d.ja}};
            look["tags"] = d.tags;
            look["synonyms"] = synonymsOf(d.tags);
            if (d.tags.size() < 3) error(c, "fewer than 3 tags derived (" + joinList(d.tags, ", ") + "): add an overrides.json entry");
            applyOverrides(look);
            checkDenied(look);
            TitleLook l;
            l.root = look;
            const std::string path = std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/" + ref + TITLE_LOOK_EXTENSION;
            if (log.errors == 0) {
                result.files[path] = writeLook(l);
                result.looks++;
            }
            logs.push_back(log);
        }

        // ---- templates ----

        struct TextEntry {
            std::string source, name, en, ja;
            Json charsEn, charsJa;          // index (ours) -> source index, or null (identity)
            bool claimed = false;
        };

        // the char adjustments of the language a mapping picks (target index -> source index)
        Json mappedChars(const Json& sourceChars, const Json& mapping, size_t length) const {
            Json out = Json::object();
            if (!sourceChars.is_object()) return out;
            if (mapping.is_object()) {
                for (auto it = mapping.begin(); it != mapping.end(); ++it) {
                    const std::string from = it.value().is_number() ? std::to_string(it.value().get<int>()) : it.value().get<std::string>();
                    if (sourceChars.contains(from)) out[it.key()] = sourceChars[from];
                }
            } else {
                for (auto it = sourceChars.begin(); it != sourceChars.end(); ++it) {
                    if ((size_t)std::stoul(it.key()) < length) out[it.key()] = it.value();
                }
            }
            // index order (S80 writes chars sorted numerically)
            std::vector<std::pair<long, std::string>> keys;
            for (auto it = out.begin(); it != out.end(); ++it) keys.push_back({std::stol(it.key()), it.key()});
            std::sort(keys.begin(), keys.end());
            Json sorted = Json::object();
            for (const auto& k : keys) sorted[k.second] = out[k.second];
            return sorted;
        }

        Json sparseChars(const Json& chars) const {
            Json out = Json::object();
            for (auto it = chars.begin(); it != chars.end(); ++it) {
                Json full = expandObject(it.value(), "char_adjust", nullptr);
                out[it.key()] = sparseObject(full, "char_adjust", true);
            }
            return out;
        }

        Json fontRef(const std::string& packId, const Ctx& c) {
            const Json& pack = options.fontPack;
            if (!pack.is_object() || !pack.contains("fonts") || !pack["fonts"].contains(packId) || !pack["fonts"][packId].contains("files") ||
                pack["fonts"][packId]["files"].empty()) {
                error(c, "font pack id `" + packId + "` has no file in fonts.json");
                return nullptr;
            }
            return Json{{"path", "TitleArt/Fonts/" + packId + "/" + pack["fonts"][packId]["files"][0].get<std::string>()}, {"guid", ""}};
        }

        struct TemplateState {
            std::vector<TextEntry> texts;
            bool storeJa = false;
            // per placeholder name: its targets and char adjustments
            std::map<std::string, std::vector<std::string>> targets;
            std::map<std::string, Json> chars;
            bool hasGuideBackdrop = false;
        };

        const TemplateState* currentTemplate = nullptr;     // while a template's layers convert

        // the members every Pachislop layer has (LayerBase), onto ours
        void layerBase(const Json& L, Json& out, const Ctx& c) {
            out["name"] = L.value("name", std::string());
            // a layer or group named after a replaced text takes the replacement (§6.7.3: no old text survives)
            if (currentTemplate) {
                for (const TextEntry& e : currentTemplate->texts) {
                    if (e.source == out["name"].get<std::string>()) out["name"] = currentTemplate->storeJa ? e.ja : e.en;
                }
            }
            for (const char* key : {"visible", "locked", "guide", "clip"}) {
                if (L.contains(key)) out[key] = L[key];
            }
            if (L.contains("blend")) {
                const SchemaMember* m = schema.member(*schema.kind("layer"), "blend");
                convertMember(*m, fieldOf("layer", "blend"), L["blend"], c.child("blend"), out);
            }
            if (L.contains("transform")) {
                Json lifted = Json::object();
                out["transform"] = convertObject(L["transform"], "transform", c.child("transform"), &lifted);
                if (lifted.contains("opacity")) out["opacity"] = lifted["opacity"];
            }
            if (L.contains("masks")) {
                const SchemaMember* m = schema.member(*schema.kind("layer"), "masks");
                convertMember(*m, FieldSpec{"masks"}, L["masks"], c.child("masks"), out);
            }
            out["effects"] = effectList(L.contains("effects") ? L["effects"] : NULL_JSON, c.child("effects"));
        }

        Json convertLayer(const Json& L, const std::string& treePath, const std::map<std::string, std::vector<size_t>>& children,
                          const Json& all, TemplateState& t, const Ctx& c) {
            Json out = Json::object();
            const std::string pachId = L.value("id", std::string());
            const std::string kind = L.value("kind", std::string());
            out["id"] = layerIds[pachId];
            static const std::set<std::string> base = {"id", "name", "visible", "locked", "guide", "clip", "blend", "transform", "masks",
                                                       "effects", "kind", "parentId"};
            std::set<std::string> handled = base;
            Json look = Json{{"id", ""}, {"color_scheme", ""}, {"owner", ""}};
            if (kind == "title2d" || kind == "title3d") {
                handled.insert({"source", "style", "style3d", "partEffects", "warp", "distort", "weight"});
                const Json source = L.contains("source") ? L["source"] : Json::object();
                const std::string sourceKind = source.value("kind", std::string());
                if (sourceKind != "text" && sourceKind != "shape") {
                    error(c.child("source").child("kind"), "a `" + sourceKind + "` source: the dump carries no asset to port it with");
                    return out;
                }
                out["type"] = sourceKind;
                layerBase(L, out, c);
                Json src = source;
                src.erase("kind");
                std::string pachText;
                if (sourceKind == "text") {
                    pachText = source.value("text", std::string());
                    src.erase("fontId");
                    src.erase("text");
                }
                Json members = convertObject(src, "layer." + sourceKind, c.child("source"));
                for (auto it = members.begin(); it != members.end(); ++it) out[it.key()] = it.value();
                if (sourceKind == "text") {
                    textLayer(L, source, pachText, out, t, c);
                }
                for (const char* key : {"warp", "distort"}) {
                    if (L.contains(key)) out[key] = convertObject(L[key], key, c.child(key));
                }
                if (L.contains("weight")) out["weight"] = L["weight"];
                out["dimension"] = kind == "title3d" ? "3d" : "2d";
                std::map<std::string, std::string> partIds;     // Pachislop rim:/band: key -> ours
                if (kind == "title2d") {
                    if (L.contains("style3d") && !dropped(c.child("style3d"), "layer", "style3d")) error(c.child("style3d"), "a 2D layer with a 3D style");
                    out["style"] = convertObject(L.contains("style") ? L["style"] : Json::object(), "style2d", c.child("style"));
                    const Json rims = L.contains("style") && L["style"].contains("rims") ? L["style"]["rims"] : Json::array();
                    for (size_t i = 0; i < rims.size() && i < out["style"].value("rims", Json::array()).size(); i++) {
                        partIds["rim:" + rims[i].value("id", std::string())] = "rim:" + out["style"]["rims"][i].value("id", std::string());
                    }
                } else {
                    if (L.contains("style") && !dropped(c.child("style"), "layer", "style")) error(c.child("style"), "a 3D layer with a 2D style");
                    Json lighting;
                    out["style_3d"] = style3D(L.contains("style3d") ? L["style3d"] : Json::object(), c.child("style3d"), lighting);
                    const Json bands = L.contains("style3d") && L["style3d"].contains("bands") ? L["style3d"]["bands"] : Json::array();
                    for (size_t i = 0; i < bands.size() && i < out["style_3d"].value("bands", Json::array()).size(); i++) {
                        partIds["band:" + bands[i].value("id", std::string())] = "band:" + out["style_3d"]["bands"][i].value("id", std::string());
                    }
                }
                Json parts = Json::object();
                if (L.contains("partEffects") && L["partEffects"].is_object()) {
                    for (auto it = L["partEffects"].begin(); it != L["partEffects"].end(); ++it) {
                        const Ctx cp = c.child("partEffects").mapKey(it.key());
                        std::string key = it.key();
                        if (key != "face" && key != "side" && key != "bevel") {
                            auto found = partIds.find(key);
                            if (found == partIds.end()) {
                                error(cp, "part key `" + key + "` names no rim or band of the layer");
                                continue;
                            }
                            key = found->second;
                        }
                        parts[key] = effectList(it.value(), cp);
                    }
                }
                out["part_effects"] = parts;
            } else if (kind == "background") {
                handled.insert("fill");
                out["type"] = "generator";
                layerBase(L, out, c);
                Json params = Json::object();
                const titleart::GeneratorDef* fill = titleart::findGenerator("fill");
                const Json f = L.contains("fill") ? L["fill"] : Json::object();
                for (auto it = f.begin(); it != f.end(); ++it) {
                    const Ctx cf = c.child("fill").child(it.key());
                    if (dropped(cf, "background", it.key())) continue;
                    const std::string key = snake(it.key());
                    const titleart::ParamDef* pd = fill ? titleart::findParam(fill->params, key) : nullptr;
                    if (!pd) {
                        error(cf, "no mapping: our Fill generator has no param `" + key + "`");
                        continue;
                    }
                    Json v = it.value();
                    if (pd->kind == titleart::ParamKind::Enum && v.is_string()) v = snake(v.get<std::string>());
                    params[key] = paramValue(*pd, v, cf);
                }
                out["generator"] = Json{{"type", "fill"}, {"params", params}};
                out["follow"] = "";
                if (L.value("guide", false)) t.hasGuideBackdrop = true;
                pushUnique(log.mapped, "background layers -> Fill generator layers");
            } else if (kind == "group") {
                handled.insert({"collapsed", "companionGroupOf"});
                out["type"] = "group";
                layerBase(L, out, c);
                out["collapsed"] = L.value("collapsed", false);
                if (L.contains("companionGroupOf")) look["owner"] = layerRef(L["companionGroupOf"], c.child("companionGroupOf"));
                Json kids = Json::array();
                auto it = children.find(pachId);
                if (it != children.end()) {
                    for (size_t j = 0; j < it->second.size(); j++) {
                        const size_t index = it->second[j];
                        kids.push_back(convertLayer(all[index], treePath + ".children[" + std::to_string(j) + "]", children, all, t,
                                                    Ctx{"template.layers[" + std::to_string(index) + "]", "template.layers[]"}));
                    }
                }
                out["children"] = kids;
            } else if (kind == "adjustment") {
                handled.insert("adjustment");
                out["type"] = "adjustment";
                layerBase(L, out, c);
                out["adjustment"] = convertObject(L.contains("adjustment") ? L["adjustment"] : Json::object(), "adjustment", c.child("adjustment"));
            } else if (kind == "generator") {
                handled.insert({"generator", "companionOf"});
                out["type"] = "generator";
                layerBase(L, out, c);
                const Json gen = L.contains("generator") ? L["generator"] : Json::object();
                for (auto it = gen.begin(); it != gen.end(); ++it) {
                    if (it.key() != "type" && it.key() != "params" && !dropped(c.child("generator").child(it.key()), "generator", it.key())) {
                        error(c.child("generator").child(it.key()), "no mapping for this generator member");
                    }
                }
                GeneratorOut g = generator(gen.value("type", std::string()), gen.contains("params") ? gen["params"] : Json::object(),
                                           c.child("generator").child("params"));
                out["generator"] = Json{{"type", g.type}, {"params", g.params}};
                out["follow"] = g.follow.empty() ? std::string() : layerRef(g.follow, c.child("generator").child("params").child("follow"));
                if (L.contains("companionOf")) look["owner"] = layerRef(L["companionOf"], c.child("companionOf"));
            } else {
                error(c.child("kind"), "`" + kind + "` is not a layer kind this port knows");
                return out;
            }
            out["look"] = look;
            for (auto it = L.begin(); it != L.end(); ++it) {
                if (handled.count(it.key())) continue;
                if (!dropped(c.child(it.key()), "layer", it.key())) error(c.child(it.key()), "no mapping and no `dropped` entry");
            }
            return out;
        }

        std::string layerRef(const Json& v, const Ctx& c) {
            const std::string id = v.is_string() ? v.get<std::string>() : std::string();
            auto it = layerIds.find(id);
            if (it == layerIds.end()) {
                error(c, "names layer `" + id + "`, which the template does not have");
                return {};
            }
            return it->second;
        }

        // a text layer's font, text and char adjustments for the stored language (§6.10.1)
        void textLayer(const Json& L, const Json& source, const std::string& pachText, Json& out, TemplateState& t, const Ctx& c) {
            if (source.contains("fontId")) {
                const Json pair = fontPair(source.value("fontId", std::string()), c.child("source").child("fontId"));
                if (pair.is_object()) {
                    out["font"] = fontRef(pair.value("latin", std::string()), c);
                    const std::string ja = pair.value("ja", std::string());
                    out["fallback_fonts"] = ja != pair.value("latin", std::string()) ? Json::array({fontRef(ja, c)}) : Json::array();
                    if (pair.contains("variations")) out["variations"] = pair["variations"];
                }
            }
            TextEntry* entry = nullptr;
            for (TextEntry& e : t.texts) {
                if (e.source == pachText) entry = &e;
            }
            if (!entry) {
                error(c.child("source").child("text"), "this text has no pachislop-map.json template_texts entry");
                return;
            }
            entry->claimed = true;
            const std::string stored = t.storeJa ? entry->ja : entry->en;
            out["text"] = stored;
            if (L.value("name", std::string()) == pachText) out["name"] = stored;
            const Json sourceChars = out.contains("chars") ? out["chars"] : Json::object();
            const Json enMap = !entry->charsEn.is_null() ? entry->charsEn : entry->en == pachText ? Json() : Json::object();
            if (!sourceChars.empty() && entry->charsEn.is_null() && entry->en != pachText) {
                error(c.child("source").child("chars"), "the text has per-character styling and its template_texts entry no `chars_en`");
            }
            const Json enChars = mappedChars(sourceChars, enMap, codepoints(entry->en).size());
            const Json jaChars = mappedChars(sourceChars, entry->charsJa, codepoints(entry->ja).size());
            out["chars"] = t.storeJa ? jaChars : enChars;
            t.targets[entry->name].push_back(out["id"].get<std::string>());
            if (!sourceChars.empty()) {
                const Json chars = Json{{"en", sparseChars(enChars)}, {"ja", sparseChars(jaChars)}};
                auto existing = t.chars.find(entry->name);
                if (existing != t.chars.end() && existing->second != chars) {
                    error(c, "the targets of placeholder `" + entry->name + "` have different per-character styling");
                }
                t.chars[entry->name] = chars;
            }
        }

        void readTexts(const std::string& ref, TemplateState& t, const Ctx& c) {
            const Json& all = options.map["template_texts"];
            if (!all.contains(ref)) return;
            for (const Json& e : all[ref]) {
                TextEntry entry;
                entry.source = e.value("source", std::string());
                entry.name = e.value("name", std::string());
                entry.en = e.value("en", std::string());
                entry.ja = e.value("ja", std::string());
                if (e.contains("chars_en")) entry.charsEn = e["chars_en"];
                if (e.contains("chars_ja")) entry.charsJa = e["chars_ja"];
                static const std::set<std::string> names = {"text", "subtitle", "kicker", "counter", "label", "line_2"};
                if (!names.count(entry.name)) error(c, "template_texts: `" + entry.name + "` is not text, subtitle, kicker, counter, label or line_2");
                if (entry.source.empty() || entry.en.empty() || entry.ja.empty()) error(c, "template_texts: every entry needs source, en and ja");
                t.texts.push_back(entry);
            }
        }

        Described describeTemplate(const Json& doc, const std::string& family, const TemplateState& t) {
            Described d;
            pushUnique(d.tags, family);
            size_t texts = 0, texts3D = 0;
            std::vector<std::string> shapesEn, shapesJa, gensEn, gensJa, adjEn, adjJa;
            bool any3D = false;
            std::function<void(const Json&)> walk = [&](const Json& layers) {
                for (const Json& L : layers) {
                    const std::string type = L.value("type", std::string());
                    const bool is3D = L.value("dimension", std::string()) == "3d";
                    if (type == "text" || type == "shape") {
                        any3D = any3D || is3D;
                        Described sub;
                        std::vector<std::string> xe, xj;
                        std::string a, b, e, f;
                        if (is3D) describeStyle3D(L.contains("style_3d") ? L["style_3d"] : Json::object(), sub, a, b, e, f);
                        else describeStyle2D(L.contains("style") ? L["style"] : Json::object(), sub, xe, xj, a, b, e, f);
                        describeExtras(L, sub, xe, xj);
                        for (const std::string& tag : sub.tags) pushUnique(d.tags, tag);
                        if (type == "text") {
                            texts++;
                            if (is3D) texts3D++;
                        } else {
                            auto w = shapeWords().find(L.value("shape", std::string()));
                            if (w != shapeWords().end()) {
                                pushUnique(d.tags, w->second.tag);
                                pushUnique(shapesEn, w->second.en);
                                pushUnique(shapesJa, w->second.ja);
                            }
                        }
                    } else if (type == "generator") {
                        auto w = generatorWords().find(L.contains("generator") ? L["generator"].value("type", std::string()) : std::string());
                        if (w != generatorWords().end() && w->first != "fill") {
                            pushUnique(d.tags, w->second.tag);
                            const std::string en = w->second.en.rfind("a ", 0) == 0 ? w->second.en.substr(2) : w->second.en.rfind("an ", 0) == 0 ? w->second.en.substr(3) : w->second.en;
                            pushUnique(gensEn, en);
                            pushUnique(gensJa, w->second.ja);
                        }
                    } else if (type == "adjustment") {
                        auto w = adjustmentWords().find(L.contains("adjustment") ? L["adjustment"].value("type", std::string()) : std::string());
                        if (w != adjustmentWords().end()) {
                            pushUnique(adjEn, w->second.en);
                            pushUnique(adjJa, w->second.ja);
                        }
                    }
                    if (L.contains("children")) walk(L["children"]);
                }
            };
            walk(doc["layers"]);
            d.tags.insert(d.tags.begin() + 1, any3D ? "3d" : "2d");
            const int w = doc["canvas"].value("width", 1920), h = doc["canvas"].value("height", 1080);
            if (h > w) pushUnique(d.tags, "vertical");
            std::vector<std::string> en, ja;
            en.push_back(texts == 0 ? "no text layer" : numberWord(texts) + (texts3D == texts ? " 3D" : "") + (texts == 1 ? " text layer" : " text layers"));
            ja.push_back(texts == 0 ? "テキストなし" : (texts3D == texts ? "3Dテキスト" : "テキスト") + std::to_string(texts) + "つ");
            if (!shapesEn.empty()) {
                en.push_back(article(shapesEn[0]) + " " + joinEnglish(shapesEn) + (shapesEn.size() == 1 ? " shape" : " shapes"));
                ja.push_back(joinList(shapesJa, "と") + "の図形");
            }
            if (!gensEn.empty()) {
                en.push_back(joinEnglish(gensEn) + (gensEn.size() == 1 ? " generator" : " generators"));
                ja.push_back(joinList(gensJa, "と") + "のジェネレーター");
            }
            if (!adjEn.empty()) {
                en.push_back(article(adjEn[0]) + " " + joinEnglish(adjEn) + (adjEn.size() == 1 ? " adjustment layer" : " adjustment layers"));
                ja.push_back(joinList(adjJa, "と") + "の調整レイヤー");
            }
            if (t.hasGuideBackdrop) {
                en.push_back("a guide backdrop");
                ja.push_back("ガイドの背景");
            }
            d.en = "A " + std::to_string(w) + " × " + std::to_string(h) + " template with " + joinEnglish(en) + ".";
            d.ja = std::to_string(w) + " × " + std::to_string(h) + " のテンプレート：" + joinList(ja, "、") + "。";
            return d;
        }

        void portTemplate(const Json& item) {
            const std::string sourceId = item.value("id", std::string());
            const IdEntry* entry = idOf(sourceId);
            if (!entry) {
                result.errors.push_back("template " + sourceId + ": no entry in pachislop-map.json ids");
                return;
            }
            beginItem(entry->ref, "template", entry->withheld, "src/presets/templates.ts", item.value("line", 0), sourceId);
            keepIds = true;
            const Ctx c{"template", "template"};
            const std::string ref = entry->ref;
            const size_t slash = ref.find('/');
            const std::string family = slash == std::string::npos ? std::string() : ref.substr(0, slash);
            if (!options.map["families"].contains(family)) error(c, "family `" + family + "` is not in pachislop-map.json families");
            const Json project = item.contains("project") ? item["project"] : Json::object();
            for (auto it = project.begin(); it != project.end(); ++it) {
                static const std::set<std::string> known = {"comp", "layers", "materialLinks", "name"};
                if (!known.count(it.key()) && !dropped(c.child(it.key()), "project", it.key())) error(c.child(it.key()), "no mapping and no `dropped` entry");
            }
            Json name;
            names(ref, name, c);

            TemplateState t;
            readTexts(ref, t, c);
            const Json layers = project.contains("layers") && project["layers"].is_array() ? project["layers"] : Json::array();
            for (const Json& L : layers) {
                const Json src = L.contains("source") ? L["source"] : Json();
                if (src.is_object() && src.value("kind", std::string()) == "text" && hasJapaneseScript(src.value("text", std::string()))) t.storeJa = true;
            }

            // the tree (Pachislop: a flat list, bottom first, `parentId` naming a group)
            std::map<std::string, std::vector<size_t>> children;
            std::vector<size_t> top;
            std::set<std::string> groups;
            for (const Json& L : layers) {
                if (L.value("kind", std::string()) == "group") groups.insert(L.value("id", std::string()));
            }
            for (size_t i = 0; i < layers.size(); i++) {
                const std::string parent = layers[i].value("parentId", std::string());
                if (parent.empty()) top.push_back(i);
                else if (!groups.count(parent)) error(Ctx{"template.layers[" + std::to_string(i) + "]", "template.layers[]"}, "parentId `" + parent + "` is not a group");
                else children[parent].push_back(i);
            }
            // ids first (masks, follow and owners name layers anywhere in the tree)
            std::function<void(size_t, const std::string&)> assign = [&](size_t index, const std::string& treePath) {
                layerIds[layers[index].value("id", std::string())] = mintId(treePath);
                auto it = children.find(layers[index].value("id", std::string()));
                if (it == children.end()) return;
                for (size_t j = 0; j < it->second.size(); j++) assign(it->second[j], treePath + ".children[" + std::to_string(j) + "]");
            };
            for (size_t k = 0; k < top.size(); k++) assign(top[k], "layers[" + std::to_string(k) + "]");
            Json ours = Json::array();
            currentTemplate = &t;
            for (size_t k = 0; k < top.size(); k++) {
                ours.push_back(convertLayer(layers[top[k]], "layers[" + std::to_string(k) + "]", children, layers, t,
                                            Ctx{"template.layers[" + std::to_string(top[k]) + "]", "template.layers[]"}));
            }
            currentTemplate = nullptr;
            for (const TextEntry& e : t.texts) {
                if (!e.claimed) error(c, "template_texts entry `" + e.source + "` matches no text layer");
            }

            // the document
            Json root = Json::object();
            const std::string guid = hex64(fnv1a64(ref)) + hex64(fnv1a64(ref + "/guid"));
            root["format"] = titleart::TITLE_ART_FORMAT;
            root["version"] = titleart::TITLE_ART_VERSION;
            root["guid"] = guid;
            root["name"] = name["en"];
            const Json comp = project.contains("comp") ? project["comp"] : Json::object();
            const Ctx cc = c.child("comp");
            Json canvas = Json::object();
            Json scene = Json::object();
            for (auto it = comp.begin(); it != comp.end(); ++it) {
                const std::string& key = it.key();
                if (key == "width" || key == "height") canvas[key] = it.value();
                else if (key == "background") canvas["background"] = convertObject(it.value(), "canvas_background", cc.child(key));
                else if (key == "globalLight") root["global_light"] = convertObject(it.value(), "global_light", cc.child(key));
                else if (!dropped(cc.child(key), "comp", key)) {
                    Json one = convertObject(Json{{key, it.value()}}, "scene_3d", cc);
                    for (auto o = one.begin(); o != one.end(); ++o) scene[o.key()] = o.value();
                }
            }
            root["canvas"] = canvas;
            root["seed"] = 1;
            Json links = Json::object();
            if (project.contains("materialLinks") && project["materialLinks"].is_object()) {
                for (auto it = project["materialLinks"].begin(); it != project["materialLinks"].end(); ++it) {
                    links[it.key()] = convertObject(it.value(), "material_link", c.child("materialLinks").mapKey(it.key()));
                }
            }
            root["material_links"] = links;
            root["layers"] = ours;
            Json exposed = Json::array(), placeholders = Json::array();
            static const std::map<std::string, std::string> labels = {{"text", "Text"},       {"subtitle", "Subtitle"}, {"kicker", "Kicker"},
                                                                      {"counter", "Counter"}, {"label", "Label"},       {"line_2", "Line 2"}};
            for (const TextEntry& e : t.texts) {
                if (std::any_of(placeholders.begin(), placeholders.end(), [&](const Json& p) { return p["name"] == e.name; })) continue;
                Json targets = Json::array();
                for (const std::string& id : t.targets[e.name]) targets.push_back(Json{{"layer", id}, {"field", "text"}});
                exposed.push_back(Json{{"name", e.name}, {"label", labels.at(e.name)}, {"type", "text"}, {"targets", targets}});
                Json p = Json::object();
                p["name"] = e.name;
                p["role"] = e.name == "text" ? "title" : e.name;
                p["sample"] = Json{{"en", e.en}, {"ja", e.ja}};
                p["max_chars"] = (int)std::max(visibleLength(e.en), visibleLength(e.ja)) + 4;
                auto chars = t.chars.find(e.name);
                if (chars != t.chars.end()) p["chars"] = chars->second;
                placeholders.push_back(p);
            }
            root["exposed"] = exposed;
            root["outputs"] = Json::array({Json{{"id", "main"}}});
            if (!scene.empty()) root["scene_3d"] = scene;
            pushUnique(log.unmappable, "the looks each layer was built with (layer `look.id`) are not in the dump: left empty");
            lowercaseColours(root);

            titleart::TitleArtDoc doc;
            doc.root = root;
            std::vector<titleart::TitleWarning> warnings;
            titleart::normalizeTitleArt(doc, &warnings);
            for (const titleart::TitleWarning& w : warnings) noteWarning(w.path, w.message, c);

            Described d = describeTemplate(doc.root, family, t);
            nameTags(name, d.tags);
            finishTags(d.tags);
            const int w = doc.root["canvas"].value("width", 1920), h = doc.root["canvas"].value("height", 1080);
            const bool transparent = doc.root["canvas"]["background"].value("transparent", true);
            std::string note = std::to_string(w) + " × " + std::to_string(h) + ", " +
                               (transparent ? std::string("transparent") : "background " + doc.root["canvas"]["background"].value("color", std::string()));
            if (t.hasGuideBackdrop) note += "; the backdrop is a guide layer";
            Json meta = Json::object();
            meta["version"] = TITLE_TEMPLATE_MEMBER_VERSION;
            meta["id"] = ref;
            meta["revision"] = 1;
            meta["name"] = name;
            meta["description"] = Json{{"en", d.en}, {"ja", d.ja}};
            meta["family"] = family;
            meta["tags"] = d.tags;
            meta["synonyms"] = synonymsOf(d.tags);
            meta["placeholders"] = placeholders;
            meta["canvas_note"] = note;
            meta["provenance"] = provenanceOf();
            if (d.tags.size() < 3) error(c, "fewer than 3 tags derived: add an overrides.json entry");

            Json whole = doc.root;
            whole["template"] = meta;
            if (applyOverrides(whole)) {
                meta = whole["template"];
                whole.erase("template");
                doc.root = whole;
                warnings.clear();
                titleart::normalizeTitleArt(doc, &warnings);
                for (const titleart::TitleWarning& wn : warnings) noteWarning(wn.path, wn.message, c);
                whole = doc.root;
                whole["template"] = meta;
            }
            checkDenied(whole);
            TitleTemplate tmpl;
            tmpl.doc = doc;
            tmpl.meta = meta;
            const std::string path = std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/templates/" + ref + TITLE_TEMPLATE_EXTENSION;
            if (log.errors == 0) {
                result.files[path] = writeTemplate(tmpl);
                result.templates++;
            }
            pushUnique(log.mapped, "layers (" + std::to_string(layers.size()) + ", groups nested by parentId), comp -> canvas, global_light and scene_3d");
            logs.push_back(log);
        }

        // ---- colourways ----

        // the canonical .titlecolors text of colourway `id` (the colourway port, PachislopColourways.h)
        bool colourwayScheme(const std::string& id, const Json& colourways, std::string& text, std::vector<std::string>& notes) {
            const Json scheme = editor::colorSchemeFromPachislop(id, colourways, notes);
            if (!scheme.is_object()) return false;
            text = titleart::writeTitleArtJson(scheme) + "\n";
            return true;
        }

        void portColourways() {
            if (!dump.contains("colourways") || !dump["colourways"].is_object()) return;
            const Json& colourways = dump["colourways"];
            const Json order = colourways.contains("order") && colourways["order"].is_array() ? colourways["order"] : Json::array();
            for (const Json& idJson : order) {
                const std::string id = idJson.is_string() ? idJson.get<std::string>() : std::string();
                const IdEntry* entry = idOf(id);
                if (!entry) {
                    result.errors.push_back("colourway " + id + ": no entry in pachislop-map.json ids");
                    continue;
                }
                int line = 0;
                if (colourways.contains("COLOURWAY_DEFS") && colourways["COLOURWAY_DEFS"].contains(id)) {
                    line = colourways["COLOURWAY_DEFS"][id].value("line", 0);
                }
                beginItem(entry->ref, "color_scheme", entry->withheld, "src/presets/colourways.ts", line, id);
                std::string text;
                std::vector<std::string> notes;
                if (!colourwayScheme(id, colourways, text, notes)) {
                    error(Ctx{"colourway", "colourway"}, "the colourway port wrote no scheme" + (notes.empty() ? std::string() : ": " + joinList(notes, "; ")));
                    logs.push_back(log);
                    continue;
                }
                pushUnique(log.mapped, "the colourway through the colourway port (PachislopColourways)");
                for (const std::string& n : notes) {
                    if (n.find("not carried") != std::string::npos) log.dropped["colourway: " + n].first = "the colourway port does not carry it";
                    else pushUnique(log.approximated, n);
                }
                const Json scheme = Json::parse(text, nullptr, false);
                if (scheme.is_discarded()) error(Ctx{"colourway", "colourway"}, "the colourway port wrote text that is not JSON");
                else checkDenied(scheme);
                if (log.errors == 0) {
                    result.files[std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/color-schemes/" + entry->ref + TITLE_COLORS_EXTENSION] = text;
                    result.colorSchemes++;
                }
                logs.push_back(log);
            }
        }

        // ---- provenance ----

        void writeProvenance() {
            std::vector<const ItemLog*> sorted;
            for (const ItemLog& l : logs) sorted.push_back(&l);
            static const std::map<std::string, int> order = {{"look", 0}, {"template", 1}, {"color_scheme", 2}};
            std::sort(sorted.begin(), sorted.end(), [](const ItemLog* a, const ItemLog* b) {
                if (a->kind != b->kind) return order.at(a->kind) < order.at(b->kind);
                return a->ref < b->ref;
            });
            Json items = Json::array();
            for (const ItemLog* l : sorted) {
                Json notes = Json::array();
                for (const std::string& m : l->mapped) notes.push_back("mapped: " + m);
                for (const std::string& a : l->approximated) notes.push_back("approximated: " + a);
                for (const auto& [pattern, d] : l->dropped) {
                    notes.push_back("dropped: " + pattern + (d.second > 1 ? " (x" + std::to_string(d.second) + ")" : std::string()) + ": " + d.first);
                }
                for (const std::string& u : l->unmappable) notes.push_back("not mappable: " + u);
                Json entry = Json::object();
                entry["ref"] = l->ref;
                entry["kind"] = l->kind;
                entry["file"] = l->file;
                entry["line"] = l->line;
                entry["commit"] = PACHISLOP_COMMIT;
                entry["item"] = l->withheld ? Json(nullptr) : Json(l->item);
                entry["notes"] = notes;
                items.push_back(entry);
                for (const auto& n : notes) result.notes.push_back(l->ref + ": " + n.get<std::string>());
            }
            Json prov = Json::object();
            prov["format"] = "jasperengine.titleprovenance";
            prov["version"] = 1;
            prov["source"] = "pachislop";
            prov["author"] = "SafEight";
            prov["commit"] = PACHISLOP_COMMIT;
            prov["license_note"] = PACHISLOP_LICENSE_NOTE;
            prov["dump"] = PACHISLOP_DUMP_PATH;
            prov["items"] = items;
            for (const DeniedTermHit& hit : findDeniedTerms(prov)) {
                result.errors.push_back("provenance (" + hit.path + "): holds the denied term `" + hit.term + "`");
            }
            result.provenance = prov;
            result.files[PACHISLOP_PROVENANCE_PATH] = titleart::writeTitleArtJson(prov) + "\n";
        }
    };

}

// ---- PachislopPort ----

namespace jasperengine::editor::titlelibrary {

bool loadPortOptions(const fs::path& repoDir, PortOptions& options, std::string* error) {
    auto read = [&](const fs::path& rel, Json& out, bool optional) {
        const fs::path path = repoDir / rel;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (optional) return true;
            if (error) *error = path.string() + ": cannot be read";
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        out = Json::parse(ss.str(), nullptr, false);
        if (out.is_discarded()) {
            if (error) *error = path.string() + ": not JSON";
            return false;
        }
        return true;
    };
    return read(PACHISLOP_MAP_PATH, options.map, false) && read(PACHISLOP_OVERRIDES_PATH, options.overrides, true) &&
           read(fs::path(TITLE_LIBRARY_RESOURCES_PATH) / "fonts" / "fonts.json", options.fontPack, false) &&
           read(fs::path(TITLE_LIBRARY_RESOURCES_PATH) / "hdri" / "manifest.json", options.environments, false);
}

PortResult portDump(const Json& dump, const PortOptions& options) {
    PortResult result;
    if (!dump.is_object()) {
        result.errors.push_back("the dump is not a JSON object");
        return result;
    }
    if (dump.contains("meta") && dump["meta"].value("commit", std::string()) != PACHISLOP_COMMIT) {
        result.errors.push_back("the dump is of Pachislop " + dump["meta"].value("commit", std::string()) + ", the map of " + PACHISLOP_COMMIT);
        return result;
    }
    Porter porter(dump, options, result);
    porter.run();
    return result;
}

bool canonicalLibraryText(const std::string& fileName, const std::string& text, std::string& canonical, std::string* error) {
    auto endsWith = [&](const char* ext) {
        const std::string e(ext);
        return fileName.size() >= e.size() && fileName.compare(fileName.size() - e.size(), e.size(), e) == 0;
    };
    if (endsWith(TITLE_LOOK_EXTENSION)) {
        LookParseResult r = parseLook(text);
        if (!r.ok) {
            if (error) *error = r.errorCode + ": " + r.error;
            return false;
        }
        canonical = writeLook(r.look);
        return true;
    }
    if (endsWith(TITLE_TEMPLATE_EXTENSION)) {
        TemplateParseResult r = parseTemplate(text);
        if (!r.ok) {
            if (error) *error = r.errorCode + ": " + r.error;
            return false;
        }
        canonical = writeTemplate(r.tmpl);
        return true;
    }
    if (error) *error = "not a .titlelook or .titletemplate file";
    return false;
}

std::map<std::string, std::string> normalizedPortFiles(const PortResult& result, std::vector<std::string>* errors) {
    std::map<std::string, std::string> out;
    for (const auto& [path, text] : result.files) {
        const std::string ext = fs::path(path).extension().string();
        std::string canonical = text, error;
        if ((ext == TITLE_LOOK_EXTENSION || ext == TITLE_TEMPLATE_EXTENSION) && !canonicalLibraryText(path, text, canonical, &error)) {
            if (errors) errors->push_back(path + ": " + error);
            canonical = text;
        }
        out[path] = canonical;
    }
    return out;
}

}

namespace {

    bool readFileText(const fs::path& path, std::string& text) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
        return true;
    }

    std::string firstDifference(const std::string& a, const std::string& b) {
        size_t line = 1, start = 0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                const size_t endA = a.find('\n', start), endB = b.find('\n', start);
                return "line " + std::to_string(line) + ": produced `" + a.substr(start, endA - start) + "`, committed `" + b.substr(start, endB - start) + "`";
            }
            if (a[i] == '\n') {
                line++;
                start = i + 1;
            }
        }
        return "line " + std::to_string(line) + ": one file ends first (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " bytes)";
    }

    // a committed library file made by the port (its provenance source is pachislop)
    bool fromPachislop(const fs::path& path) {
        std::string text;
        if (!readFileText(path, text)) return false;
        const Json j = Json::parse(text, nullptr, false);
        if (!j.is_object()) return false;
        const Json* prov = nullptr;
        if (j.contains("template") && j["template"].is_object() && j["template"].contains("provenance")) prov = &j["template"]["provenance"];
        else if (j.contains("provenance")) prov = &j["provenance"];
        return prov && prov->is_object() && prov->value("source", std::string()) == "pachislop";
    }

}

std::vector<PortFinding> jasperengine::editor::titlelibrary::comparePortFiles(const std::map<std::string, std::string>& files, const fs::path& root) {
    std::vector<PortFinding> findings;
    for (const auto& [path, text] : files) {
        std::string committed;
        if (!readFileText(root / path, committed)) {
            findings.push_back({path, "missing", "produced by the port, not in the tree"});
        } else if (committed != text) {
            findings.push_back({path, "differs", firstDifference(text, committed)});
        }
    }
    std::error_code ec;
    for (const char* dir : {"looks", "templates", "color-schemes"}) {
        const fs::path base = root / TITLE_LIBRARY_RESOURCES_PATH / dir;
        if (!fs::exists(base, ec)) continue;
        std::vector<std::string> stale;
        for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            const std::string rel = fs::relative(it->path(), root, ec).generic_string();
            if (files.count(rel)) continue;
            const std::string ext = it->path().extension().string();
            if ((ext == TITLE_LOOK_EXTENSION || ext == TITLE_TEMPLATE_EXTENSION || ext == TITLE_COLORS_EXTENSION) && fromPachislop(it->path())) {
                stale.push_back(rel);
            }
        }
        std::sort(stale.begin(), stale.end());
        for (const std::string& s : stale) findings.push_back({s, "stale", "has Pachislop provenance and the port no longer produces it"});
    }
    return findings;
}

// ---- the embedded map (the importer's view: S87's PachislopMembers) ----

PachislopFieldRule jasperengine::editor::titlelibrary::parsePachislopFieldRule(const Json& value) {
    PachislopFieldRule f;
    if (value.is_string()) {
        f.to = value.get<std::string>();
    } else if (value.is_object()) {
        f.to = value.value("to", std::string());
        f.component = value.value("component", -1);
        f.negate = value.value("negate", false);
        f.environment = value.value("environment", false);
        if (value.contains("values") && value["values"].is_object()) f.values = value["values"];
    }
    if (!f.to.empty() && f.to[0] == '^') {
        f.lift = true;
        f.to = f.to.substr(1);
    }
    return f;
}

const Json& jasperengine::editor::titlelibrary::embeddedPachislopMap() {
    static const Json map = [] {
        const auto& files = TitleLibrary::get().builtinFiles();
        auto it = files.find("pachislop-map.json");
        Json parsed = it == files.end() ? Json() : Json::parse(it->second, nullptr, false);
        return parsed.is_object() ? parsed : Json::object();
    }();
    return map;
}

const PachislopFieldRule* jasperengine::editor::titlelibrary::pachislopFieldRule(const std::string& kind, const std::string& key) {
    static const std::map<std::string, PachislopFieldRule> rules = [] {
        std::map<std::string, PachislopFieldRule> out;
        const Json& map = embeddedPachislopMap();
        if (map.contains("fields") && map["fields"].is_object()) {
            for (auto it = map["fields"].begin(); it != map["fields"].end(); ++it) out[it.key()] = parsePachislopFieldRule(it.value());
        }
        return out;
    }();
    auto it = rules.find(kind + "." + key);
    if (it == rules.end()) {
        const SchemaKind* k = TitleArtSchema::get().kind(kind);
        if (k && !k->base.empty()) it = rules.find(k->base + "." + key);
    }
    return it == rules.end() ? nullptr : &it->second;
}

std::string jasperengine::editor::titlelibrary::pachislopPackFont(const std::string& fontId, bool japanese) {
    const Json& map = embeddedPachislopMap();
    if (!map.contains("fonts") || !map["fonts"].contains(fontId)) return {};
    const Json& entry = map["fonts"][fontId];
    if (entry.is_string()) return entry.get<std::string>();
    if (!entry.is_object()) return {};
    const char* key = japanese ? "ja" : "latin";
    return entry.value(key, entry.value("latin", std::string()));
}

std::string jasperengine::editor::titlelibrary::pachislopEnvironment(const std::string& reflectionMap) {
    const Json& map = embeddedPachislopMap();
    if (!map.contains("environments") || !map["environments"].contains(reflectionMap)) return {};
    const Json& entry = map["environments"][reflectionMap];
    return entry.is_string() ? entry.get<std::string>() : std::string();
}
