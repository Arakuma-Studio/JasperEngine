// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83 AC23 (R51, D16), the generator half of the Pachislop id map
// (editor/resources/titleart/pachislop-fx-map.yaml through editor/titleart/fx/PachislopFxMap):
// every Pachislop generator type but patternFill (S80's) has a row; every Pachislop preset id maps
// to one of our presets, whose `source` names it back; every Pachislop param key lands on a param
// of our definition (or the layer's `follow`); every negated param is a degree param of ours; and
// converting each Pachislop preset gives exactly our preset file's params. Pachislop's presets and
// param keys come from fixtures/pachislop-generator-presets.json (transcribed from its TS sources,
// see its `_provenance`).

#include <gtest/gtest.h>

#include "titleart/TitleEffectRegistry.h"
#include "titleart/fx/FxPresets.h"
#include "titleart/fx/PachislopGeneratorMap.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::ParamDef;
using titleart::ParamKind;
using titleart::ParamUnit;
using Kind = TitlePachislopParam::Kind;

namespace {

    // src/generators/registry.ts in its order, without patternFill (S80's pattern_fill)
    const std::vector<std::string> PACHISLOP_GENERATORS = {"particles", "inkSplatter", "speedLines", "lightStreaks", "sunburst",
                                                           "shockwave", "magicCircle", "brushSwirl", "glints", "lensFlare",
                                                           "backlight", "fire", "smoke", "corona", "lightning"};

    const Json& fixture() {
        static const Json parsed = []() {
            std::ifstream in(std::string(JASPER_EDITOR_TEST_FIXTURES) + "/../titleart/fixtures/pachislop-generator-presets.json");
            std::stringstream text;
            text << in.rdbuf();
            return Json::parse(text.str(), nullptr, false);
        }();
        return parsed;
    }

    // Pachislop's param keys by generator type
    Json paramKeys() {
        return fixture().is_object() ? fixture().value("_param_keys", Json::object()) : Json::object();
    }

    // the fixture's presets: "<pachislopType>/<presetId>" -> its params
    std::vector<std::pair<std::string, const Json*>> pachislopPresets() {
        std::vector<std::pair<std::string, const Json*>> out;
        if (!fixture().is_object()) return out;
        for (auto it = fixture().begin(); it != fixture().end(); ++it) {
            if (it.key().empty() || it.key()[0] == '_') continue;
            out.emplace_back(it.key(), &it.value());
        }
        return out;
    }

    // where `a` (the conversion) and `b` (the preset file) differ, ignoring key order and integer
    // vs float; "" when they are the same
    std::string difference(const Json& a, const Json& b, const std::string& path) {
        if (a.is_number() && b.is_number()) {
            return a.get<double>() == b.get<double>() ? std::string() : path + ": converted " + a.dump() + ", the file has " + b.dump();
        }
        if (a.is_object() && b.is_object()) {
            for (auto it = a.begin(); it != a.end(); ++it) {
                if (!b.contains(it.key())) return path + "." + it.key() + ": converted " + it.value().dump() + ", not in the file";
                const std::string d = difference(it.value(), b[it.key()], path + "." + it.key());
                if (!d.empty()) return d;
            }
            for (auto it = b.begin(); it != b.end(); ++it) {
                if (!a.contains(it.key())) return path + "." + it.key() + ": the file has " + it.value().dump() + ", not converted";
            }
            return {};
        }
        if (a.is_array() && b.is_array()) {
            if (a.size() != b.size()) return path + ": converted " + a.dump() + ", the file has " + b.dump();
            for (size_t i = 0; i < a.size(); i++) {
                const std::string d = difference(a[i], b[i], path + "[" + std::to_string(i) + "]");
                if (!d.empty()) return d;
            }
            return {};
        }
        return a == b ? std::string() : path + ": converted " + a.dump() + ", the file has " + b.dump();
    }

    const titleart::GeneratorDef& ours(const TitlePachislopRow& row) {
        const titleart::GeneratorDef* def = titleart::findGenerator(row.type);
        EXPECT_TRUE(def) << row.pachislop << " -> " << row.type << " is not registered";
        static const titleart::GeneratorDef none;
        return def ? *def : none;
    }

    bool hasOption(const ParamDef& def, const std::string& value) {
        return std::any_of(def.options.begin(), def.options.end(), [&](const titleart::EnumOption& o) { return value == o.value; });
    }

    // a key of `negated` or `values`: "rows[].angle" names a field of the RowArray `rows`
    const ParamDef* paramOrRowField(const titleart::GeneratorDef& def, const std::string& key) {
        const size_t dot = key.find("[].");
        if (dot == std::string::npos) return titleart::findParam(def.params, key);
        const ParamDef* rows = titleart::findParam(def.params, key.substr(0, dot));
        if (!rows || rows->kind != ParamKind::RowArray) return nullptr;
        return titleart::findParam(rows->rows, key.substr(dot + 3));
    }

}

TEST(PachislopGeneratorMapTest, FixtureParses) {
    ASSERT_TRUE(fixture().is_object()) << "fixtures/pachislop-generator-presets.json does not parse";
    ASSERT_TRUE(fixture().contains("_param_keys"));
    EXPECT_EQ(pachislopPresets().size(), 72u);
}

TEST(PachislopGeneratorMapTest, EveryGeneratorTypeHasARow) {
    const std::vector<TitlePachislopRow>& rows = titlePachislopGeneratorRows();
    ASSERT_EQ(rows.size(), PACHISLOP_GENERATORS.size());
    for (size_t i = 0; i < rows.size(); i++) EXPECT_EQ(rows[i].pachislop, PACHISLOP_GENERATORS[i]);
    for (const std::string& type : PACHISLOP_GENERATORS) {
        const std::string mapped = titlePachislopGeneratorType(type);
        ASSERT_FALSE(mapped.empty()) << type;
        EXPECT_TRUE(titleart::findGenerator(mapped)) << type << " -> " << mapped << " is not registered";
        EXPECT_EQ(titlePachislopTypeOfGenerator(mapped), type);
        EXPECT_TRUE(paramKeys().contains(type)) << type << " has no param keys in the fixture";
    }
    EXPECT_EQ(titlePachislopGeneratorType("corona"), "aura");
    EXPECT_EQ(titlePachislopGeneratorType("patternFill"), "");
}

TEST(PachislopGeneratorMapTest, EveryPresetIdMapsToOurs) {
    std::set<std::string> theirs;
    for (const auto& [source, params] : pachislopPresets()) {
        theirs.insert(source);
        const size_t slash = source.find('/');
        ASSERT_NE(slash, std::string::npos) << source;
        const std::string type = titlePachislopGeneratorType(source.substr(0, slash));
        const std::string id = titlePachislopGeneratorPresetId(source.substr(0, slash), source.substr(slash + 1));
        ASSERT_FALSE(id.empty()) << source << " has no preset id of ours";
        const TitleFxPreset* preset = TitleFxPresets::find(type, id);
        ASSERT_TRUE(preset) << source << " -> " << type << "/" << id << " is not in its preset file";
        EXPECT_EQ(preset->source, source) << type << "/" << id;
    }
    // the map names no preset Pachislop lacks, and every ported preset of ours is in the map
    for (const TitlePachislopRow& row : titlePachislopGeneratorRows()) {
        for (const auto& [id, ourId] : row.presets) EXPECT_TRUE(theirs.count(row.pachislop + "/" + id)) << row.pachislop << "/" << id;
        for (const TitleFxPreset& preset : TitleFxPresets::forType(row.type)) {
            if (preset.source.empty()) continue;
            EXPECT_TRUE(theirs.count(preset.source)) << row.type << "/" << preset.id << ": source " << preset.source;
        }
    }
}

TEST(PachislopGeneratorMapTest, EveryParamKeyLandsOnOurs) {
    const Json keys = paramKeys();
    for (const TitlePachislopRow& row : titlePachislopGeneratorRows()) {
        const titleart::GeneratorDef& def = ours(row);
        ASSERT_TRUE(keys.contains(row.pachislop)) << row.pachislop;
        std::set<std::string> theirs;
        for (const Json& k : keys[row.pachislop]) {
            const std::string key = k.get<std::string>();
            theirs.insert(key);
            const std::string at = row.pachislop + "." + key;
            const TitlePachislopParam target = titlePachislopGeneratorParam(row, key);
            const ParamDef* pd = target.kind == Kind::LayerFollow ? nullptr : titleart::findParam(def.params, target.key);
            switch (target.kind) {
                case Kind::LayerFollow:
                    EXPECT_NE(def.follow, titleart::FollowMode::None) << at << ": " << row.type << " follows no layer";
                    break;
                case Kind::Component:
                    ASSERT_TRUE(pd && pd->kind == ParamKind::Vec2) << at << " -> " << target.key << " is no point of " << row.type;
                    EXPECT_TRUE(target.index == 0 || target.index == 1) << at;
                    break;
                case Kind::RowCount:
                    EXPECT_TRUE(pd && pd->kind == ParamKind::RowArray) << at << " -> " << target.key;
                    break;
                case Kind::RowField:
                    ASSERT_TRUE(pd && pd->kind == ParamKind::RowArray) << at << " -> " << target.key;
                    EXPECT_TRUE(titleart::findParam(pd->rows, target.field)) << at << " -> " << target.key << "[]." << target.field;
                    EXPECT_TRUE(target.index >= 0 && (size_t)target.index < pd->maxRows) << at;
                    break;
                case Kind::Param:
                    EXPECT_TRUE(pd) << at << " -> " << target.key << " is not a param of " << row.type;
                    break;
            }
        }
        // the row's own keys name params Pachislop has
        if (!row.follow.empty()) EXPECT_TRUE(theirs.count(row.follow)) << row.pachislop << " follow " << row.follow;
        if (!row.rowCount.empty()) EXPECT_TRUE(theirs.count(row.rowCount)) << row.pachislop << " rows " << row.rowCount;
        for (const auto& [key, target] : row.vec2) EXPECT_TRUE(theirs.count(key)) << row.pachislop << " vec2 " << key;
        for (const auto& [key, target] : row.renames) EXPECT_TRUE(theirs.count(key)) << row.pachislop << " rename " << key;
        // value maps land on options of ours
        for (const auto& [key, map] : row.values) {
            const ParamDef* pd = paramOrRowField(def, key);
            ASSERT_TRUE(pd && pd->kind == ParamKind::Enum) << row.pachislop << " values " << key;
            for (const auto& [from, to] : map) EXPECT_TRUE(hasOption(*pd, to)) << row.pachislop << " values " << key << ": " << from << " -> " << to;
        }
    }
}

TEST(PachislopGeneratorMapTest, NegatedParamsAreDegrees) {
    size_t negated = 0;
    for (const TitlePachislopRow& row : titlePachislopGeneratorRows()) {
        const titleart::GeneratorDef& def = ours(row);
        for (const std::string& key : row.negated) {
            const ParamDef* pd = paramOrRowField(def, key);
            ASSERT_TRUE(pd) << row.pachislop << " negates " << key << ", which " << row.type << " lacks";
            EXPECT_EQ(pd->kind, ParamKind::Number) << row.type << "." << key;
            EXPECT_EQ(pd->unit, ParamUnit::Deg) << row.type << "." << key << " is negated but not in degrees";
            negated++;
        }
    }
    // §6.9's 17: two each for particles, sunburst, magic circle and glints, one for nine others
    EXPECT_EQ(negated, 17u);
}

TEST(PachislopGeneratorMapTest, PresetsConvertToOurPresetFiles) {
    for (const auto& [source, params] : pachislopPresets()) {
        const size_t slash = source.find('/');
        const std::string pachislopType = source.substr(0, slash);
        std::vector<std::string> notes;
        const TitlePachislopGenerator converted = convertPachislopGenerator(pachislopType, *params, &notes);
        for (const std::string& n : notes) ADD_FAILURE() << source << ": " << n;
        const TitleFxPreset* preset = TitleFxPresets::find(converted.type, titlePachislopGeneratorPresetId(pachislopType, source.substr(slash + 1)));
        ASSERT_TRUE(preset) << source;
        const std::string d = difference(converted.params, preset->params, converted.type + "/" + preset->id);
        EXPECT_TRUE(d.empty()) << source << ": " << d;
    }
}

TEST(PachislopGeneratorMapTest, ConvertsTheRules) {
    std::vector<std::string> notes;
    // follow -> the layer member, angles negated, camelCase -> snake_case, companionOf a note
    const Json layer = Json::parse(R"({"kind": "generator", "companionOf": "t1",
        "generator": {"type": "fire", "params": {"follow": "t1", "angle": 90, "overEdge": true}}})");
    const TitlePachislopGenerator fire = convertPachislopGeneratorLayer(layer, &notes);
    EXPECT_EQ(fire.type, "fire");
    EXPECT_EQ(fire.follow, "t1");
    EXPECT_EQ(difference(fire.params, Json::parse(R"({"angle": -90, "over_edge": true})"), "fire"), "");
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("t1"), std::string::npos) << notes[0];

    // half a pair keeps our default's other component; renames and value maps
    notes.clear();
    const TitlePachislopGenerator flare = convertPachislopGenerator("lensFlare", Json::parse(R"({"y": 10})"), &notes);
    const ParamDef* position = titleart::findParam(titleart::findGenerator("lens_flare")->params, "position");
    ASSERT_TRUE(position);
    EXPECT_EQ(difference(flare.params["position"], Json::array({position->defaultValue[0], 10}), "position"), "");
    const TitlePachislopGenerator aura = convertPachislopGenerator("corona", Json::parse(R"({"mode": "aura", "aura": "weak"})"), &notes);
    EXPECT_EQ(difference(aura.params, Json::parse(R"({"mode": "rising", "strength": "weak"})"), "aura"), "");
    EXPECT_TRUE(notes.empty());

    // numbered rows: as many as `rows` says, the rest of each row our default, row angles negated
    const TitlePachislopGenerator circle = convertPachislopGenerator("magicCircle", Json::parse(R"({"rows": "2", "row2Angle": 30, "text": "eto"})"), &notes);
    ASSERT_TRUE(circle.params["rows"].is_array());
    ASSERT_EQ(circle.params["rows"].size(), 2u);
    const ParamDef* rows = titleart::findParam(titleart::findGenerator("magic_circle")->params, "rows");
    ASSERT_TRUE(rows);
    EXPECT_EQ(difference(circle.params["rows"][0], rows->defaultValue[0], "rows[0]"), "");
    EXPECT_EQ(circle.params["rows"][1]["angle"].get<double>(), -30.0);
    EXPECT_EQ(circle.params["glyphs"], "zodiac");
    EXPECT_TRUE(notes.empty());

    // what has no counterpart is dropped with a note
    const TitlePachislopGenerator unknown = convertPachislopGenerator("shockwave", Json::parse(R"({"noSuchParam": 1, "radius": 200})"), &notes);
    EXPECT_EQ(difference(unknown.params, Json::parse(R"({"radius": 200})"), "shockwave"), "");
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("noSuchParam"), std::string::npos) << notes[0];
    notes.clear();
    EXPECT_EQ(convertPachislopGenerator("patternFill", Json::object(), &notes).type, "");
    EXPECT_EQ(notes.size(), 1u);
}
