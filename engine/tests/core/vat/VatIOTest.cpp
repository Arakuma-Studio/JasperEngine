// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .vat parser and canonical writer (spec 2.1): deterministic bytes, parse(write(d)) == d,
// write(parse(canonical)) byte-identical, canonical key order, every leniency warning with its
// JSON path, the generated-field refusal message, unknown keys, and other tools' files refused
// with a pointer to the importer.

#include <gtest/gtest.h>

#include "VatTestEnvironment.h"

#include "animation/VertexAnimation.h"
#include "json.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    const std::string REBAKE = "rebake (Inspector > Rebake, or bake_vertex_animation {rebake})";

    std::string generatedError(const std::string& path){
        return "generated field " + path + " is invalid: " + REBAKE;
    }

    // the canonical text of the sample, as a JSON document to edit
    nlohmann::ordered_json sampleJson(){
        return nlohmann::ordered_json::parse(writeVat(vattest::sampleData()));
    }

    VatParseResult parseJson(const nlohmann::ordered_json& json){
        return parseVat(json.dump());
    }

    // the warning at `path`, or null
    const ClipWarning* findWarning(const VatParseResult& result, const std::string& path){
        for (const ClipWarning& warning : result.warnings){
            if (warning.path == path) return &warning;
        }
        return nullptr;
    }

    std::string warningsText(const VatParseResult& result){
        std::string text;
        for (const ClipWarning& warning : result.warnings) text += "[" + warning.path + "] " + warning.message + "\n";
        return text;
    }

    // one edit of the sample must load with a warning at `path` containing `message`
    void expectWarning(const std::function<void(nlohmann::ordered_json&)>& edit, const std::string& path,
                       const std::string& message, VatParseResult* out = nullptr){
        nlohmann::ordered_json json = sampleJson();
        edit(json);
        VatParseResult result = parseJson(json);
        ASSERT_TRUE(result.ok()) << result.error;
        const ClipWarning* warning = findWarning(result, path);
        ASSERT_NE(warning, nullptr) << "no warning at " << path << "; got:\n" << warningsText(result);
        EXPECT_NE(warning->message.find(message), std::string::npos) << warning->message;
        if (out) *out = result;
    }

    // one edit of the sample must fail as an invalid generated field at `path`
    void expectGenerated(const std::function<void(nlohmann::ordered_json&)>& edit, const std::string& path){
        nlohmann::ordered_json json = sampleJson();
        edit(json);
        VatParseResult result = parseJson(json);
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error, generatedError(path));
        // the reason is a warning at the same path
        EXPECT_NE(findWarning(result, path), nullptr) << warningsText(result);
    }

}

TEST(VatIO, WriteIsDeterministicAndRoundTrips){
    VatData data = vattest::sampleData();
    std::string text = writeVat(data);
    EXPECT_EQ(text, writeVat(data));

    VatParseResult parsed = parseVat(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty()) << warningsText(parsed);
    EXPECT_EQ(writeVat(*parsed.data), text);

    const VatData& back = *parsed.data;
    EXPECT_EQ(back.guid, data.guid);
    EXPECT_EQ(back.mode, data.mode);
    EXPECT_EQ(back.mesh, data.mesh);
    EXPECT_EQ(back.layout.precision, data.layout.precision);
    EXPECT_EQ(back.layout.width, data.layout.width);
    EXPECT_EQ(back.layout.height, data.layout.height);
    EXPECT_EQ(back.layout.samples, data.layout.samples);
    EXPECT_EQ(back.layout.vertices, data.layout.vertices);
    EXPECT_EQ(back.layout.bones, data.layout.bones);
    EXPECT_EQ(back.layout.normals, data.layout.normals);
    EXPECT_EQ(back.layout.colors, data.layout.colors);
    EXPECT_EQ(back.layout.layoutHash, data.layout.layoutHash);
    EXPECT_TRUE(back.bounds.valid);
    EXPECT_EQ(back.bounds.min, data.bounds.min);
    EXPECT_EQ(back.bounds.max, data.bounds.max);
    ASSERT_EQ(back.clips.size(), 2u);
    for (size_t i = 0; i < back.clips.size(); i++){
        const VatClip& a = back.clips[i];
        const VatClip& b = data.clips[i];
        EXPECT_EQ(a.name, b.name);
        EXPECT_EQ(a.start, b.start);
        EXPECT_EQ(a.frames, b.frames);
        EXPECT_EQ(a.fps, b.fps);
        EXPECT_EQ(a.wrap, b.wrap);
        EXPECT_EQ(a.speed, b.speed);
        EXPECT_EQ(a.interpolate, b.interpolate);
        EXPECT_EQ(a.restNone, b.restNone);
        EXPECT_EQ(a.bounds.valid, b.bounds.valid);
        ASSERT_EQ(a.events.size(), b.events.size());
        for (size_t e = 0; e < a.events.size(); e++){
            EXPECT_EQ(a.events[e].time, b.events[e].time);
            EXPECT_EQ(a.events[e].function, b.events[e].function);
            EXPECT_EQ(a.events[e].args, b.events[e].args);
        }
    }
    ASSERT_EQ(back.sockets.size(), 2u);
    EXPECT_EQ(back.sockets[1].name, "Head");
    EXPECT_EQ(back.sockets[1].path, "Root/Head");
    ASSERT_EQ(back.recipe.sources.size(), 1u);
    EXPECT_EQ(back.recipe.sources[0].kind, "model");
    EXPECT_EQ(back.recipe.sources[0].path, "models/hero.gltf");
    EXPECT_EQ(back.recipe.sources[0].clips, data.recipe.sources[0].clips);
    EXPECT_EQ(back.recipe.sources[0].hashes, data.recipe.sources[0].hashes);
    EXPECT_EQ(back.recipe.sockets, data.recipe.sockets);
    EXPECT_EQ(back.stats.bytes, data.stats.bytes);
    EXPECT_EQ(back.stats.maxErrorMm, data.stats.maxErrorMm);
    EXPECT_EQ(back.stats.meanErrorMm, data.stats.meanErrorMm);
}

TEST(VatIO, CanonicalTextOfASmallAsset){
    VatData data;
    data.mode = VatMode::Bones;
    data.mesh = "crate.vat.glb";
    data.layout.precision = VatPrecision::Float;
    data.layout.width = 4;
    data.layout.height = 2;
    data.layout.samples = 2;
    data.layout.bones = 2;
    data.layout.normals = false;
    data.layout.layoutHash = "abc123";
    VatClip clip;
    clip.name = "Open";
    clip.frames = 2;
    clip.fps = 30.0f;
    clip.wrap = ClipWrapMode::PingPong;
    clip.restNone = true;
    clip.bounds.extend(Vector3(0, 0, 0));
    clip.bounds.extend(Vector3(1, 0.5f, -0.0f));
    ClipEventData event;
    event.time = 0.0333333f;
    event.function = "onOpen";
    clip.events.push_back(event);
    data.clips.push_back(clip);
    data.recipe.importOptions = "{ \"fps\" : 24, \"axis\": \"unity\" }";
    data.unknownKeys.push_back({"x_note", "{\"a\":1}"});

    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.vat\",\n"
        "  \"version\": 1,\n"
        "  \"mode\": \"bones\",\n"
        "  \"mesh\": \"crate.vat.glb\",\n"
        "  \"layout\": {\n"
        "    \"precision\": \"float\",\n"
        "    \"width\": 4,\n"
        "    \"height\": 2,\n"
        "    \"samples\": 2,\n"
        "    \"vertices\": 0,\n"
        "    \"bones\": 2,\n"
        "    \"normals\": false,\n"
        "    \"colors\": false,\n"
        "    \"layout_hash\": \"abc123\"\n"
        "  },\n"
        "  \"clips\": [\n"
        "    {\n"
        "      \"name\": \"Open\",\n"
        "      \"start\": 0,\n"
        "      \"frames\": 2,\n"
        "      \"fps\": 30,\n"
        "      \"wrap\": \"ping_pong\",\n"
        "      \"speed\": 1,\n"
        "      \"interpolate\": true,\n"
        "      \"rest\": \"none\",\n"
        "      \"bounds\": {\"min\": [0, 0, 0], \"max\": [1, 0.5, 0]},\n"
        "      \"events\": [\n"
        "        {\"time\": 0.033333, \"function\": \"onOpen\"}\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"sockets\": [],\n"
        "  \"recipe\": {\n"
        "    \"sources\": [],\n"
        "    \"mode\": \"auto\",\n"
        "    \"fps\": 30,\n"
        "    \"preset\": \"mobile_web\",\n"
        "    \"normals\": true,\n"
        "    \"precision\": \"auto\",\n"
        "    \"tolerance_mm\": 1,\n"
        "    \"sockets\": [],\n"
        "    \"import\": {\"fps\":24,\"axis\":\"unity\"}\n"
        "  },\n"
        "  \"stats\": {\"bytes\": 0, \"max_error_mm\": 0, \"mean_error_mm\": 0},\n"
        "  \"x_note\": {\"a\":1}\n"
        "}\n";
    EXPECT_EQ(writeVat(data), expected);

    VatParseResult parsed = parseVat(expected);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(writeVat(*parsed.data), expected);
    EXPECT_EQ(parsed.data->recipe.importOptions, "{\"fps\":24,\"axis\":\"unity\"}");
    EXPECT_TRUE(parsed.data->clips[0].restNone);
}

TEST(VatIO, AnyKeyOrderAndSpacingReadsToTheCanonicalText){
    std::string canonical = writeVat(vattest::sampleData());
    // the same document, compact, members reversed at every level
    std::function<nlohmann::ordered_json(const nlohmann::ordered_json&)> reverse = [&](const nlohmann::ordered_json& json){
        if (json.is_object()){
            nlohmann::ordered_json out = nlohmann::ordered_json::object();
            std::vector<std::string> keys;
            for (auto it = json.begin(); it != json.end(); ++it) keys.push_back(it.key());
            std::reverse(keys.begin(), keys.end());
            for (const std::string& key : keys) out[key] = reverse(json[key]);
            return out;
        }
        if (json.is_array()){
            nlohmann::ordered_json out = nlohmann::ordered_json::array();
            for (const auto& item : json) out.push_back(reverse(item));
            return out;
        }
        return json;
    };
    nlohmann::ordered_json shuffled = reverse(nlohmann::ordered_json::parse(canonical));
    // keep the hashes' (file) order, the only order the data keeps
    shuffled["recipe"]["sources"][0]["hashes"] = nlohmann::ordered_json::parse(canonical)["recipe"]["sources"][0]["hashes"];
    VatParseResult parsed = parseVat(shuffled.dump());
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty()) << warningsText(parsed);
    EXPECT_EQ(writeVat(*parsed.data), canonical);
}

TEST(VatIO, CanonicalKeyOrder){
    VatData data = vattest::sampleData();
    data.unknownKeys.push_back({"zz_extra", "1"});
    std::string text = writeVat(data);
    const char* const order[] = {"\"format\"", "\"version\"", "\"guid\"", "\"mode\"", "\"mesh\"", "\"layout\"", "\"bounds\"",
                                 "\"clips\"", "\"sockets\"", "\"recipe\"", "\"stats\"", "\"zz_extra\""};
    size_t last = 0;
    for (const char* key : order){
        size_t at = text.find(std::string("\n  ") + key);
        ASSERT_NE(at, std::string::npos) << key;
        EXPECT_GT(at, last) << key;
        last = at;
    }
    // inside a clip
    const char* const clipOrder[] = {"\"name\"", "\"start\"", "\"frames\"", "\"fps\"", "\"wrap\"", "\"speed\"",
                                     "\"interpolate\"", "\"bounds\"", "\"events\""};
    size_t clips = text.find("\"clips\"");
    last = clips;
    for (const char* key : clipOrder){
        size_t at = text.find(key, clips);
        ASSERT_NE(at, std::string::npos) << key;
        EXPECT_GT(at, last) << key;
        last = at;
    }
    // no guid, no bounds: the keys are left out, not written empty
    data.guid.clear();
    data.bounds = VatBounds();
    text = writeVat(data);
    EXPECT_EQ(text.find("\"guid\""), std::string::npos);
    EXPECT_EQ(text.find("\n  \"bounds\""), std::string::npos);
}

TEST(VatIO, FloatsAreShortestRoundTrip){
    VatData data = vattest::sampleData();
    data.clips[0].fps = 4.0f / 1.0333f;      // not representable in 6 digits
    data.clips[1].speed = -0.0f;
    data.recipe.toleranceMm = 0.1f;
    data.layout.layoutHash = computeLayoutHash(data);
    std::string text = writeVat(data);
    VatParseResult parsed = parseVat(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.data->clips[0].fps, data.clips[0].fps);
    EXPECT_EQ(parsed.data->layout.layoutHash, computeLayoutHash(*parsed.data));
    EXPECT_NE(text.find("\"speed\": 0,"), std::string::npos);
    EXPECT_NE(text.find("\"tolerance_mm\": 0.1,"), std::string::npos);
    EXPECT_NE(text.find("\"fps\": 2,"), std::string::npos);
    EXPECT_EQ(writeVat(*parsed.data), text);
}

TEST(VatIO, EventsFollowTheAnimRules){
    VatParseResult result;
    // sorted by (time, function); args kept with their types
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse(
            "[{\"time\": 0.5, \"function\": \"b\"}, {\"time\": 0.5, \"function\": \"a\", \"args\": [1.5, \"x\", false]},"
            " {\"time\": 0.1, \"function\": \"c\", \"extra\": 1}]");
    }, "clips[0].events[2].extra", "unknown key ignored", &result);
    const std::vector<ClipEventData>& events = result.data->clips[0].events;
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].function, "c");
    EXPECT_EQ(events[1].function, "a");
    EXPECT_EQ(events[2].function, "b");
    ASSERT_EQ(events[1].args.size(), 3u);
    EXPECT_EQ(events[1].args[0], AnimationEventArg(1.5));
    EXPECT_EQ(events[1].args[1], AnimationEventArg(std::string("x")));
    EXPECT_EQ(events[1].args[2], AnimationEventArg(false));

    expectWarning([](nlohmann::ordered_json& json){ json["clips"][0]["events"] = 3; },
                  "clips[0].events", "expected an array of events");
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][0]["events"] = nlohmann::ordered_json::array({5}); },
                  "clips[0].events[0]", "event dropped");
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"function\": \"f\"}]");
    }, "clips[0].events[0].time", "event dropped");
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0, \"function\": 3}]");
    }, "clips[0].events[0].function", "event dropped");
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0, \"function\": \"end\"}]");
    }, "clips[0].events[0].function", "is not a Lua function name, event dropped", &result);
    EXPECT_TRUE(result.data->clips[0].events.empty());
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0, \"function\": \"on-step\"}]");
    }, "clips[0].events[0].function", "is not a Lua function name");
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0, \"function\": \"f\", \"args\": 3}]");
    }, "clips[0].events[0].args", "expected an array");
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0, \"function\": \"f\", \"args\": [1, {}]}]");
    }, "clips[0].events[0].args[1]", "dropped", &result);
    EXPECT_EQ(result.data->clips[0].events[0].args.size(), 1u);
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse(
            "[{\"time\": 0, \"function\": \"f\", \"args\": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}]");
    }, "clips[0].events[0].args", "more than 8 args", &result);
    EXPECT_EQ(result.data->clips[0].events[0].args.size(), MAX_ANIMATION_EVENT_ARGS);
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": -1, \"function\": \"f\"}]");
    }, "clips[0].events[0].time", "negative time", &result);
    EXPECT_EQ(result.data->clips[0].events[0].time, 0.0f);
    // Walk is 1 s long
    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 3, \"function\": \"f\"}]");
    }, "clips[0].events[0].time", "after the clip's end", &result);
    EXPECT_EQ(result.data->clips[0].events[0].time, 1.0f);
    // times on the 1e-6 grid
    nlohmann::ordered_json json = sampleJson();
    json["clips"][0]["events"] = nlohmann::ordered_json::parse("[{\"time\": 0.12345678, \"function\": \"f\"}]");
    VatParseResult grid = parseJson(json);
    ASSERT_TRUE(grid.ok());
    EXPECT_FLOAT_EQ(grid.data->clips[0].events[0].time, 0.123457f);
}

TEST(VatIO, EditableClipFieldsAreRepairedWithWarnings){
    VatParseResult result;
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["name"] = 5; },
                  "clips[1].name", "using \"Clip 2\"", &result);
    EXPECT_EQ(result.data->clips[1].name, "Clip 2");
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["name"] = ""; }, "clips[1].name", "expected a clip name");

    expectWarning([](nlohmann::ordered_json& json){
        json["clips"][1]["name"] = "Walk";
        json["clips"].push_back(json["clips"][0]);
    }, "clips[1].name", "renamed \"Walk 2\"", &result);
    ASSERT_EQ(result.data->clips.size(), 3u);
    EXPECT_EQ(result.data->clips[2].name, "Walk 3");
    ASSERT_NE(findWarning(result, "clips[2].name"), nullptr);
    EXPECT_NE(findWarning(result, "clips[2].name")->message.find("renamed \"Walk 3\""), std::string::npos);

    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["wrap"] = "bounce"; },
                  "clips[1].wrap", "using loop", &result);
    EXPECT_EQ(result.data->clips[1].wrap, ClipWrapMode::Loop);
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["wrap"] = "default"; }, "clips[1].wrap", "using loop");
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["wrap"] = 1; }, "clips[1].wrap", "using loop");

    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["speed"] = "fast"; },
                  "clips[1].speed", "using 1", &result);
    EXPECT_EQ(result.data->clips[1].speed, 1.0f);
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["interpolate"] = "yes"; },
                  "clips[1].interpolate", "using true", &result);
    EXPECT_TRUE(result.data->clips[1].interpolate);
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][1]["mood"] = "happy"; },
                  "clips[1].mood", "unknown key ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["clips"][0]["bounds"] = {{"min", {1, 1, 1}}, {"max", {0, 0, 0}}}; },
                  "clips[0].bounds", "min <= max", &result);
    EXPECT_FALSE(result.data->clips[0].bounds.valid);

    // a negative speed plays backwards: accepted as is
    nlohmann::ordered_json json = sampleJson();
    json["clips"][1]["speed"] = -2;
    VatParseResult backwards = parseJson(json);
    ASSERT_TRUE(backwards.ok());
    EXPECT_EQ(backwards.data->clips[1].speed, -2.0f);
    EXPECT_TRUE(backwards.warnings.empty()) << warningsText(backwards);

    // missing editable fields take their defaults silently
    json = sampleJson();
    json["clips"][1].erase("wrap");
    json["clips"][1].erase("speed");
    json["clips"][1].erase("interpolate");
    VatParseResult defaults = parseJson(json);
    ASSERT_TRUE(defaults.ok());
    EXPECT_TRUE(defaults.warnings.empty()) << warningsText(defaults);
    EXPECT_EQ(defaults.data->clips[1].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(defaults.data->clips[1].speed, 1.0f);
    EXPECT_TRUE(defaults.data->clips[1].interpolate);
}

TEST(VatIO, RemovedAndExtraClips){
    VatParseResult result;
    // removing a clip is an edit: its rows stay in the payload
    nlohmann::ordered_json json = sampleJson();
    json["clips"].erase(0);
    VatParseResult removed = parseJson(json);
    ASSERT_TRUE(removed.ok()) << removed.error;
    EXPECT_TRUE(removed.warnings.empty()) << warningsText(removed);
    ASSERT_EQ(removed.data->clips.size(), 1u);
    EXPECT_EQ(removed.data->clips[0].name, "Wave");

    json = sampleJson();
    json["clips"] = nlohmann::ordered_json::array();
    VatParseResult none = parseJson(json);
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.data->clips.empty());

    expectWarning([](nlohmann::ordered_json& json){ json["clips"].push_back("Run"); }, "clips[2]", "dropped", &result);
    EXPECT_EQ(result.data->clips.size(), 2u);

    expectWarning([](nlohmann::ordered_json& json){
        nlohmann::ordered_json clip = json["clips"][0];
        json["clips"] = nlohmann::ordered_json::array();
        for (int i = 0; i < VAT_MAX_CLIPS + 2; i++){
            clip["name"] = "C" + std::to_string(i);
            json["clips"].push_back(clip);
        }
    }, "clips[64]", "more than 64 clips", &result);
    EXPECT_EQ(result.data->clips.size(), (size_t)VAT_MAX_CLIPS);
    EXPECT_NE(findWarning(result, "clips[65]"), nullptr);
}

TEST(VatIO, SocketsAreLenient){
    VatParseResult result;
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"] = "Hand"; }, "sockets", "ignored", &result);
    EXPECT_TRUE(result.data->sockets.empty());
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"].push_back(3); }, "sockets[2]", "socket dropped");
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"][0].erase("path"); }, "sockets[0].path", "socket dropped",
                  &result);
    EXPECT_EQ(result.data->sockets.size(), 1u);
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"][0]["path"] = "Root/Foot"; }, "sockets[0].path",
                  "not a baked socket", &result);
    ASSERT_EQ(result.data->sockets.size(), 1u);
    EXPECT_EQ(result.data->sockets[0].name, "Head");
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"][0].erase("name"); }, "sockets[0].name",
                  "using \"Hand\"", &result);
    EXPECT_EQ(result.data->sockets[0].name, "Hand");
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"][1]["name"] = "Hand"; }, "sockets[1].name",
                  "renamed \"Hand 2\"", &result);
    EXPECT_EQ(result.data->sockets[1].name, "Hand 2");
    expectWarning([](nlohmann::ordered_json& json){ json["sockets"][1]["bone"] = 1; }, "sockets[1].bone", "unknown key");

    // renaming and reordering are edits
    nlohmann::ordered_json json = sampleJson();
    json["sockets"] = nlohmann::ordered_json::parse("[{\"name\": \"Hat\", \"path\": \"Root/Head\"}, "
                                                    "{\"name\": \"Grip\", \"path\": \"Root/Hand\"}]");
    VatParseResult edited = parseJson(json);
    ASSERT_TRUE(edited.ok());
    EXPECT_TRUE(edited.warnings.empty()) << warningsText(edited);
    EXPECT_EQ(edited.data->findSocket("Grip"), 1);
    EXPECT_EQ(edited.data->findSocket("Hat"), 0);
    EXPECT_EQ(edited.data->findSocket("Hand"), -1);

    // without recipe sockets (imports) any path is kept
    json = sampleJson();
    json["recipe"]["sockets"] = nlohmann::ordered_json::array();
    json["sockets"][0]["path"] = "anything";
    VatParseResult imported = parseJson(json);
    ASSERT_TRUE(imported.ok());
    EXPECT_EQ(imported.data->sockets.size(), 2u);
}

TEST(VatIO, OtherTopLevelFieldsAreLenient){
    VatParseResult result;
    expectWarning([](nlohmann::ordered_json& json){ json["guid"] = "XYZ"; }, "guid", "dropped", &result);
    EXPECT_TRUE(result.data->guid.empty());
    expectWarning([](nlohmann::ordered_json& json){ json["bounds"] = {{"min", {0, 0}}, {"max", {1, 1, 1}}}; }, "bounds",
                  "ignored", &result);
    EXPECT_FALSE(result.data->bounds.valid);
    expectWarning([](nlohmann::ordered_json& json){ json["layout"]["pixels"] = 1; }, "layout.pixels", "unknown key");

    expectWarning([](nlohmann::ordered_json& json){ json["stats"] = 1; }, "stats", "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["stats"]["bytes"] = -1; }, "stats.bytes", "using 0");
    expectWarning([](nlohmann::ordered_json& json){ json["stats"]["max_error_mm"] = "big"; }, "stats.max_error_mm", "using 0");
    expectWarning([](nlohmann::ordered_json& json){ json["stats"]["mean_error_mm"] = -1; }, "stats.mean_error_mm", "using 0");
    expectWarning([](nlohmann::ordered_json& json){ json["stats"]["fps"] = 1; }, "stats.fps", "unknown key");
}

TEST(VatIO, RecipeIsLenient){
    VatParseResult result;
    expectWarning([](nlohmann::ordered_json& json){ json.erase("recipe"); }, "recipe", "Rebake");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"] = 1; }, "recipe", "Rebake");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"] = 1; }, "recipe.sources", "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0] = 1; }, "recipe.sources[0]", "source dropped");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0].erase("kind"); }, "recipe.sources[0].kind",
                  "source dropped", &result);
    EXPECT_TRUE(result.data->recipe.sources.empty());
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0]["path"] = 3; }, "recipe.sources[0].path",
                  "source dropped");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0]["clips"] = "Walk"; }, "recipe.sources[0].clips",
                  "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0]["hashes"] = 1; }, "recipe.sources[0].hashes",
                  "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0]["hashes"]["a.png"] = 1; },
                  "recipe.sources[0].hashes.a.png", "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sources"][0]["date"] = 1; }, "recipe.sources[0].date",
                  "unknown key");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["mode"] = "magic"; }, "recipe.mode", "using auto");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["fps"] = 0; }, "recipe.fps", "using 30");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["preset"] = "console"; }, "recipe.preset", "using mobile_web");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["normals"] = 1; }, "recipe.normals", "using true");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["precision"] = "double"; }, "recipe.precision", "using auto");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["tolerance_mm"] = -1; }, "recipe.tolerance_mm", "using 1");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["sockets"] = {1}; }, "recipe.sockets", "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["import"] = "fps=24"; }, "recipe.import", "ignored");
    expectWarning([](nlohmann::ordered_json& json){ json["recipe"]["seed"] = 1; }, "recipe.seed", "unknown key");

    // an entity source keeps its scene
    nlohmann::ordered_json json = sampleJson();
    json["recipe"]["sources"][0]["kind"] = "entity";
    json["recipe"]["sources"][0]["scene"] = "scenes/main.scene";
    VatParseResult entity = parseJson(json);
    ASSERT_TRUE(entity.ok());
    EXPECT_EQ(entity.data->recipe.sources[0].scene, "scenes/main.scene");
    EXPECT_NE(writeVat(*entity.data).find("\"scene\": \"scenes/main.scene\""), std::string::npos);
}

TEST(VatIO, UnknownTopLevelKeysAreKeptInFileOrder){
    nlohmann::ordered_json json = sampleJson();
    json["b_tool"] = {{"x", 1}};
    json["a_note"] = "hi";
    VatParseResult parsed = parseJson(json);
    ASSERT_TRUE(parsed.ok());
    const ClipWarning* warning = findWarning(parsed, "");
    ASSERT_NE(warning, nullptr);
    EXPECT_NE(warning->message.find("b_tool, a_note"), std::string::npos);
    ASSERT_EQ(parsed.data->unknownKeys.size(), 2u);
    std::string text = writeVat(*parsed.data);
    size_t b = text.find("\"b_tool\": {\"x\":1}");
    size_t a = text.find("\"a_note\": \"hi\"");
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(a, std::string::npos);
    EXPECT_LT(b, a);
    EXPECT_GT(b, text.find("\"stats\""));
    VatParseResult again = parseVat(text);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(writeVat(*again.data), text);

    // the writer never lets an unknown key shadow a known one; a repeated name keeps the last
    VatData data = vattest::sampleData();
    data.unknownKeys = {{"mode", "\"x\""}, {"k", "1"}, {"k", "2"}, {"empty", ""}};
    text = writeVat(data);
    EXPECT_EQ(text.find("\"mode\": \"x\""), std::string::npos);
    EXPECT_EQ(text.find("\"k\": 1"), std::string::npos);
    EXPECT_NE(text.find("\"k\": 2"), std::string::npos);
    EXPECT_NE(text.find("\"empty\": null"), std::string::npos);
}

TEST(VatIO, InvalidGeneratedFieldsRefuseToLoad){
    using J = nlohmann::ordered_json;
    expectGenerated([](J& json){ json.erase("mode"); }, "mode");
    expectGenerated([](J& json){ json["mode"] = "bone"; }, "mode");
    expectGenerated([](J& json){ json.erase("mesh"); }, "mesh");
    expectGenerated([](J& json){ json["mesh"] = ""; }, "mesh");
    expectGenerated([](J& json){ json["mesh"] = 3; }, "mesh");
    expectGenerated([](J& json){ json.erase("layout"); }, "layout");
    expectGenerated([](J& json){ json["layout"] = J::array(); }, "layout");
    expectGenerated([](J& json){ json["layout"]["precision"] = "double"; }, "layout.precision");
    expectGenerated([](J& json){ json["layout"].erase("width"); }, "layout.width");
    expectGenerated([](J& json){ json["layout"]["width"] = 0; }, "layout.width");
    expectGenerated([](J& json){ json["layout"]["width"] = 8.5; }, "layout.width");
    expectGenerated([](J& json){ json["layout"]["height"] = 0; }, "layout.height");
    // the payload rows must hold every sample: 7 x 3 x 2 = 42 texels > 8 x 5
    expectGenerated([](J& json){ json["layout"]["height"] = 5; }, "layout.height");
    expectGenerated([](J& json){ json["layout"]["samples"] = 0; }, "layout.samples");
    expectGenerated([](J& json){ json["layout"]["vertices"] = 0; }, "layout.vertices");
    expectGenerated([](J& json){ json["layout"]["vertices"] = VAT_MAX_VERTICES + 1; }, "layout.vertices");
    expectGenerated([](J& json){ json["layout"]["bones"] = -1; }, "layout.bones");
    expectGenerated([](J& json){ json["layout"]["normals"] = 1; }, "layout.normals");
    expectGenerated([](J& json){ json["layout"].erase("colors"); }, "layout.colors");
    expectGenerated([](J& json){ json["layout"]["layout_hash"] = ""; }, "layout.layout_hash");
    expectGenerated([](J& json){ json["layout"]["layout_hash"] = "not hex"; }, "layout.layout_hash");
    // colors add a texel per sample: 7 x 3 x 3 = 63 > 48
    expectGenerated([](J& json){ json["layout"]["colors"] = true; }, "layout.height");
    // bones mode: bones >= 1 and float precision
    expectGenerated([](J& json){ json["mode"] = "bones"; }, "layout.bones");
    expectGenerated([](J& json){
        json["mode"] = "bones";
        json["layout"]["precision"] = "half";
        json["layout"]["bones"] = 1;
    }, "layout.precision");
    expectGenerated([](J& json){ json.erase("clips"); }, "clips");
    expectGenerated([](J& json){ json["clips"] = J::object(); }, "clips");
    expectGenerated([](J& json){ json["clips"][1].erase("start"); }, "clips[1].start");
    expectGenerated([](J& json){ json["clips"][1]["start"] = -1; }, "clips[1].start");
    expectGenerated([](J& json){ json["clips"][1]["frames"] = 0; }, "clips[1].frames");
    expectGenerated([](J& json){ json["clips"][1]["frames"] = 4; }, "clips[1].frames");   // 4 + 4 > 7 samples
    expectGenerated([](J& json){ json["clips"][1]["fps"] = 0; }, "clips[1].fps");
    expectGenerated([](J& json){ json["clips"][1]["fps"] = "30"; }, "clips[1].fps");
    expectGenerated([](J& json){ json["clips"][1]["rest"] = "bind"; }, "clips[1].rest");

    // a valid bones layout loads
    nlohmann::ordered_json json = sampleJson();
    json["mode"] = "bones";
    json["layout"]["bones"] = 2;
    VatParseResult bones = parseJson(json);
    ASSERT_TRUE(bones.ok()) << bones.error;
    EXPECT_EQ(bones.data->mode, VatMode::Bones);
}

TEST(VatIO, FilesThatAreNotVertexAnimations){
    VatParseResult result = parseVat("{\"format\": \"jasperengine.vat\", \"version\": 1,\n  \"mode\": }");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error.rfind("2:11: not valid JSON", 0), 0u) << result.error;
    result = parseVat("[1, 2]");
    EXPECT_EQ(result.error, "not a vertex animation: the top level must be a JSON object");
    result = parseVat("{\"format\": \"other\", \"version\": 1}");
    EXPECT_EQ(result.error, "not a vertex animation: \"format\" must be \"jasperengine.vat\"");
    result = parseVat("{\"format\": \"jasperengine.vat\"}");
    EXPECT_EQ(result.error, "missing \"version\" (1)");
    result = parseVat("{\"format\": \"jasperengine.vat\", \"version\": \"1\"}");
    EXPECT_EQ(result.error, "invalid \"version\": expected a whole number from 1");
    result = parseVat("{\"format\": \"jasperengine.vat\", \"version\": 2}");
    EXPECT_EQ(result.error, "unsupported vertex animation version 2 (this engine reads versions up to 1)");
    result = parseVat("{\"format\": \"jasperengine.vat\", \"version\": 1e999}");
    EXPECT_NE(result.error.find("out of range"), std::string::npos) << result.error;

    // other tools' files: refused with a pointer to the importer
    const std::string pointer = "Import as Vertex Animation";
    struct Foreign{
        std::string text;
        std::string kind;
    };
    const std::vector<Foreign> foreign = {
        {"%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!21 &2100000\nMaterial:\n", "Unity asset"},
        {"--- !u!114 &11400000\nMonoBehaviour:\n  m_Name: VatData\n", "Unity asset"},
        {"{\"Bound Min X\": -1, \"Houdini FPS\": 24, \"Number of Frames\": 100}", "Houdini Labs VAT"},
        {"{\"os-remap\": {\"Min\": [0, 0, 0]}, \"animations\": {}}", "OpenVAT"},
        {std::string("\x76\x2f\x31\x01\x02\x00\x00\x00", 8), "EXR image"},
        {std::string("\x89PNG\r\n\x1a\n\0\0", 10), "PNG image"},
        {"Kaydara FBX Binary  \0\x1a\0", "FBX model"},
        {std::string("glTF\x02\0\0\0", 8), "binary glTF"},
        {std::string("\x01\x02\0\x03", 4), "binary file"},
        {"{\"format\": \"jasperengine.anim\", \"version\": 1, \"tracks\": []}", "animation clip"},
    };
    for (const Foreign& file : foreign){
        result = parseVat(file.text);
        EXPECT_FALSE(result.ok());
        EXPECT_NE(result.error.find(file.kind), std::string::npos) << result.error;
        EXPECT_NE(result.error.find(pointer), std::string::npos) << result.error;
        EXPECT_NE(result.error.find("jasperengine.vat"), std::string::npos) << result.error;
    }
}

TEST(VatIO, NameHelpers){
    VatMode mode = VatMode::Vertices;
    EXPECT_STREQ(modeName(VatMode::Bones), "bones");
    EXPECT_STREQ(modeName(VatMode::Vertices), "vertices");
    EXPECT_TRUE(modeFromName("bones", mode));
    EXPECT_EQ(mode, VatMode::Bones);
    EXPECT_TRUE(modeFromName("vertices", mode));
    EXPECT_EQ(mode, VatMode::Vertices);
    EXPECT_FALSE(modeFromName("Bones", mode));
    VatPrecision precision = VatPrecision::Half;
    EXPECT_STREQ(precisionName(VatPrecision::Half), "half");
    EXPECT_STREQ(precisionName(VatPrecision::Float), "float");
    EXPECT_TRUE(precisionFromName("float", precision));
    EXPECT_EQ(precision, VatPrecision::Float);
    EXPECT_TRUE(precisionFromName("half", precision));
    EXPECT_EQ(precision, VatPrecision::Half);
    EXPECT_FALSE(precisionFromName("unorm16", precision));

    VatData data = vattest::sampleData();
    EXPECT_EQ(data.findClip("Wave"), 1);
    EXPECT_EQ(data.findClip("Run"), -1);
    EXPECT_EQ(data.findSocket("Head"), 1);
    EXPECT_EQ(data.findSocket("Foot"), -1);
}

TEST(VatIO, BoundsExtend){
    VatBounds bounds;
    EXPECT_FALSE(bounds.valid);
    VatBounds empty;
    bounds.extend(empty);
    EXPECT_FALSE(bounds.valid);
    bounds.extend(Vector3(1, -2, 3));
    EXPECT_TRUE(bounds.valid);
    EXPECT_EQ(bounds.min, Vector3(1, -2, 3));
    EXPECT_EQ(bounds.max, Vector3(1, -2, 3));
    bounds.extend(Vector3(-1, 5, 0));
    EXPECT_EQ(bounds.min, Vector3(-1, -2, 0));
    EXPECT_EQ(bounds.max, Vector3(1, 5, 3));
    VatBounds other;
    other.extend(Vector3(10, 10, 10));
    bounds.extend(other);
    EXPECT_EQ(bounds.max, Vector3(10, 10, 10));
    EXPECT_EQ(bounds.min, Vector3(-1, -2, 0));
}
