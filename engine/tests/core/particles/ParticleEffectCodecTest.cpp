// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "particles/ParticleCompiler.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "ParticleTestEffects.h"

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::particles;
using particletest::everyModule;
using particletest::referenceStack;

// AC1: every module with non-default values survives JSON -> compile -> JSON byte for byte
TEST(ParticleEffectCodec, RoundTrip){
    const ParticleEffectDocument doc = everyModule();
    const std::string first = ParticleEffectCodec::write(doc);
    // every module is in the file
    for (const ModuleDesc& m : ParticleModules::all())
        EXPECT_NE(first.find(std::string("\"type\": \"") + m.key + "\""), std::string::npos) << m.key;

    ParticleEffectDocument read;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(first, read, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    const auto compiled = ParticleCompiler::compile(read);
    ASSERT_TRUE(compiled->valid) << ParticleEffectCodec::format(compiled->diagnostics);
    const std::string second = ParticleEffectCodec::write(compiled->document);
    EXPECT_EQ(first, second);
}

// a module's own fields never take the keys of its entry's header: in the file a module is
// {"type": <module>, "enabled": ..., ...fields}, so a field named "type" overwrote the module
TEST(ParticleEffectCodec, ModuleFieldsKeepOutOfTheHeader){
    for (const ModuleDesc& m : ParticleModules::all()){
        for (const FieldDesc& f : m.fields.fields){
            EXPECT_STRNE(f.key, "type") << m.key;
            EXPECT_STRNE(f.key, "enabled") << m.key;
        }
    }
}

TEST(ParticleEffectCodec, WorldCollisionRoundTrips){
    ParticleEffectDocument doc;
    doc.collision.present = true;
    doc.collision.type = CollisionType::World;
    ParticleEffectDocument read;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(ParticleEffectCodec::write(doc), read, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_TRUE(read.collision.present);
    EXPECT_EQ(read.collision.type, CollisionType::World);
}

TEST(ParticleEffectCodec, DefaultsAreLeftOut){
    const std::string text = ParticleEffectCodec::write(ParticleEffectDocument());
    // the version, the modules shown by default and nothing else
    EXPECT_NE(text.find("\"version\": 1"), std::string::npos);
    EXPECT_EQ(text.find("duration"), std::string::npos);
    EXPECT_NE(text.find("\"type\": \"main\""), std::string::npos);
    EXPECT_NE(text.find("\"type\": \"emission\""), std::string::npos);
    EXPECT_NE(text.find("\"type\": \"shape\""), std::string::npos);
    EXPECT_NE(text.find("\"type\": \"renderer\""), std::string::npos);
    EXPECT_EQ(text.find("\"type\": \"noise\""), std::string::npos);
}

TEST(ParticleEffectCodec, FloatsAreShortest){
    ParticleEffectDocument doc;
    doc.duration = 0.1f;
    const std::string text = ParticleEffectCodec::write(doc);
    EXPECT_NE(text.find("\"duration\": 0.1,"), std::string::npos) << text;
}

// AC1: the reference stack compiles in at most 0.3 ms (enforced on the benchmark server)
TEST(ParticleEffectCodec, CompileTimeOfTheReferenceStack){
    const ParticleEffectDocument doc = referenceStack(true);
    const std::string text = ParticleEffectCodec::write(doc);
    double best = 1e9;
    for (int run = 0; run < 20; run++){
        const auto start = std::chrono::steady_clock::now();
        ParticleEffectDocument read;
        std::vector<Diagnostic> diagnostics;
        ASSERT_TRUE(ParticleEffectCodec::read(text, read, diagnostics));
        const auto compiled = ParticleCompiler::compile(read);
        ASSERT_TRUE(compiled->valid);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("[bench] ParticleEffectCodec reference stack read + compile: %.3f ms (budget 0.3 ms)\n", best);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(best, 0.3 * testsupport::perfSlack());
    }
}

namespace {

    struct InvalidCase {
        const char* json;
        const char* path;
    };

}

// AC1: invalid documents, each with the diagnostic at its path
TEST(ParticleEffectCodec, Validation){
    const std::vector<InvalidCase> cases = {
        {R"({"modules": []})", "version"},
        {R"({"version": 2, "modules": []})", "version"},
        {R"({"version": 0, "modules": []})", "version"},
        {R"({"version": 1, "duration": "long"})", "duration"},
        {R"({"version": 1, "duration": 0})", "duration"},
        {R"({"version": 1, "tickRate": 500})", "tickRate"},
        {R"({"version": 1, "maxParticles": 0})", "maxParticles"},
        {R"({"version": 1, "maxParticles": 2000000})", "maxParticles"},
        {R"({"version": 1, "simulationSpace": "galaxy"})", "simulationSpace"},
        {R"({"version": 1, "modules": {}})", "modules"},
        {R"({"version": 1, "modules": [{"enabled": true}]})", "modules[0]"},
        {R"({"version": 1, "modules": [{"type": "lasers"}]})", "modules[0].type"},
        {R"({"version": 1, "modules": [{"type": "noise"}, {"type": "noise"}]})", "modules[1].type"},
        {R"({"version": 1, "modules": [{"type": "main", "enabled": false}]})", "main.enabled"},
        {R"({"version": 1, "modules": [{"type": "main", "startLifetime": 0}]})", "main.startLifetime"},
        {R"({"version": 1, "modules": [{"type": "main", "startSize": {"mode": "curve"}}]})", "main.startSize.curve"},
        {R"({"version": 1, "modules": [{"type": "main", "startSpeed": {"mode": "sometimes"}}]})", "main.startSpeed.mode"},
        {R"({"version": 1, "modules": [{"type": "main", "startSpeed": {"mode": "curve", "curve": [[0.5, 1], [0.2, 2]]}}]})", "main.startSpeed.curve[1]"},
        {R"({"version": 1, "modules": [{"type": "main", "startColor": [1, 1]}]})", "main.startColor"},
        {R"({"version": 1, "modules": [{"type": "main", "startColor": {"mode": "gradient", "gradient": {"colorKeys": [[2, 1, 1, 1]]}}}]})", "main.startColor.gradient.colorKeys[0]"},
        {R"({"version": 1, "modules": [{"type": "emission", "bursts": [{"probability": 3}]}]})", "emission.bursts[0].probability"},
        {R"({"version": 1, "startDelay": {"mode": "curve", "curve": [[0, 1], [1, 2]]}, "modules": []})", "startDelay"},
        {R"({"version": 1, "modules": [{"type": "shape", "radiusThickness": 2}]})", "shape.radiusThickness"},
        {R"({"version": 1, "modules": [{"type": "shape", "shape": "mesh", "mesh": {"type": "model"}}]})", "shape.mesh.model"},
        {R"({"version": 1, "modules": [{"type": "noise", "octaves": 5}]})", "noise.octaves"},
        {R"({"version": 1, "modules": [{"type": "orbit", "axis": [0, 0, 0]}]})", "orbit.axis"},
        {R"({"version": 1, "modules": [{"type": "collision", "planes": [{"normal": [0, 0, 0]}]}]})", "collision.planes[0].normal"},
        {R"({"version": 1, "modules": [{"type": "textureSheetAnimation", "mode": "sprites"}]})", "textureSheetAnimation.sprites"},
        {R"({"version": 1, "modules": [{"type": "trails", "maxPoints": 100}]})", "trails.maxPoints"},
        {R"({"version": 1, "modules": [{"type": "renderer", "renderMode": "mesh"}]})", "renderer.meshes"},
        {R"({"version": 1, "modules": [{"type": "renderer", "lighting": "sixWay"}]})", "renderer.sixWayMapA"},
        {R"({"version": 1, "subEmitters": [{"trigger": "death"}]})", "subEmitters[0].effect"},
        {R"({"version": 1, "subEmitters": [{"trigger": "trigger", "effect": "a.particles"}]})", "subEmitters[0].triggerName"},
        {R"({"version": 1, "exposed": [{"name": "Rate", "path": "emission.nothing"}]})", "exposed[0].path"},
        {R"({"version": 1, "exposed": [{"name": "Rate", "path": "emission.rateOverTime", "type": "vector3"}]})", "exposed[0].type"},
        {R"({"version": 1, "exposed": [{"name": "A", "path": "duration"}, {"name": "A", "path": "duration"}]})", "exposed[1].name"},
        {R"({"version": 1, "modules": [{"type": "setOverAge", "entries": [{"property": "frame"}]}]})", "setOverAge.entries[0].frames"},
        {R"({not json)", "$"},
    };
    ASSERT_GE(cases.size(), 30u);
    for (const InvalidCase& c : cases){
        ParticleEffectDocument doc;
        std::vector<Diagnostic> diagnostics;
        EXPECT_FALSE(ParticleEffectCodec::read(c.json, doc, diagnostics)) << c.json;
        bool found = false;
        for (const Diagnostic& d : diagnostics)
            found = found || (d.severity == Diagnostic::Severity::Error && d.path == c.path);
        EXPECT_TRUE(found) << c.json << "\nexpected an error at " << c.path << ", got:\n" << ParticleEffectCodec::format(diagnostics);
    }
}

TEST(ParticleEffectCodec, UnknownFieldsWarn){
    ParticleEffectDocument doc;
    std::vector<Diagnostic> diagnostics;
    EXPECT_TRUE(ParticleEffectCodec::read(R"({"version": 1, "sparkle": true, "modules": [{"type": "noise", "wiggle": 2}]})", doc, diagnostics));
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].severity, Diagnostic::Severity::Warning);
    EXPECT_EQ(diagnostics[0].path, "sparkle");
    EXPECT_EQ(diagnostics[1].path, "noise.wiggle");
}

TEST(ParticleEffectCodec, MergePatchEditsModules){
    ParticleEffectDocument doc;
    std::vector<Diagnostic> diagnostics;
    const auto patch = nlohmann::ordered_json::parse(R"({"duration": 2, "modules": {"emission": {"rateOverTime": 50}, "noise": {"strength": 3}, "shape": null}})");
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(doc, patch, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(doc.duration, 2.0f);
    EXPECT_EQ(doc.emission.rateOverTime.constant, 50.0f);
    EXPECT_TRUE(doc.noise.present);
    EXPECT_EQ(doc.noise.strength.constant, 3.0f);
    EXPECT_FALSE(doc.shape.present);
    // an invalid patch leaves the document as it was
    const ParticleEffectDocument before = doc;
    diagnostics.clear();
    EXPECT_FALSE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(R"({"modules": {"noise": {"octaves": 9}}})"), diagnostics));
    EXPECT_EQ(ParticleEffectCodec::write(doc), ParticleEffectCodec::write(before));
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics.front().path, "noise.octaves");
}

TEST(ParticleEffectCodec, ResolvesFieldPaths){
    ParticleEffectDocument doc = everyModule();
    auto r = ParticleModules::resolve(doc, "emission.rateOverTime.constant");
    ASSERT_NE(r.field, nullptr);
    EXPECT_EQ(r.member, "constant");
    EXPECT_EQ(r.storage, &doc.emission.rateOverTime);
    r = ParticleModules::resolve(doc, "emission.bursts[0].probability");
    ASSERT_NE(r.field, nullptr);
    EXPECT_EQ(r.storage, &doc.emission.bursts[0].probability);
    r = ParticleModules::resolve(doc, "duration");
    EXPECT_EQ(r.storage, &doc.duration);
    EXPECT_EQ(ParticleModules::resolve(doc, "emission.bursts[9].time").field, nullptr);
    EXPECT_EQ(ParticleModules::resolve(doc, "nothing.here").field, nullptr);
}

TEST(ParticleEffectCodec, MergePatchRefusesAnUnknownField){
    ParticleEffectDocument doc;
    doc.emission.rateOverTime = MinMaxCurve(7.0f);
    const std::string before = ParticleEffectCodec::write(doc);
    auto refused = [&](const char* patch, const char* path, const char* name){
        std::vector<Diagnostic> diagnostics;
        EXPECT_FALSE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(patch), diagnostics)) << patch;
        EXPECT_EQ(ParticleEffectCodec::write(doc), before) << "the document stays as it was: " << patch;
        bool found = false;
        for (const Diagnostic& d : diagnostics){
            if (d.path != path) continue;
            found = true;
            EXPECT_EQ(d.severity, Diagnostic::Severity::Error) << patch;
            EXPECT_NE(d.message.find(std::string("did you mean '") + name + "'"), std::string::npos) << d.message;
        }
        EXPECT_TRUE(found) << patch << ": " << ParticleEffectCodec::format(diagnostics);
    };
    refused(R"({"duratoin": 3})", "duratoin", "duration");
    refused(R"({"modules": {"emission": {"rateOverTim": 20}}})", "emission.rateOverTim", "rateOverTime");
    refused(R"({"modules": {"emission": {"rateOverTime": {"mode": "twoConstants", "constant": 1, "constantMxa": 2}}}})", "emission.rateOverTime.constantMxa", "constantMax");
    refused(R"({"modules": {"emision": {"rateOverTime": 20}}})", "modules.emision", "emission");
    refused(R"({"modules": {"emission": {"bursts": [{"tiem": 1, "count": 2}]}}})", "emission.bursts[0].tiem", "time");
    refused(R"({"modules": {"sizeOverLifetime": {"size": {"mode": "curve", "curve": {"keys": [[0, 1], [1, 0]], "preWrapMod": "loop"}}}}})", "sizeOverLifetime.size.curve.preWrapMod", "preWrapMode");
    refused(R"({"modules": {"colorOverLifetime": {"color": {"mode": "gradient", "gradient": {"colorKyes": []}}}}})", "colorOverLifetime.color.gradient.colorKyes", "colorKeys");
}

TEST(ParticleEffectCodec, ReadingAnOldFileKeepsWarning){
    // the same unknown field in a file: it loads, the field is read past, and the warning says what it meant
    ParticleEffectDocument doc;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(R"({"version": 1, "duratoin": 3})", doc, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].severity, Diagnostic::Severity::Warning);
    EXPECT_NE(diagnostics[0].message.find("did you mean 'duration'"), std::string::npos) << diagnostics[0].message;
}

TEST(ParticleEffectCodec, MergePatchKeepsItsWarnings){
    ParticleEffectDocument doc;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(R"({"modules": {"renderer": {"renderMode": "none"}}})"), diagnostics))
        << ParticleEffectCodec::format(diagnostics);
    bool found = false;
    for (const Diagnostic& d : diagnostics) found = found || (d.path == "renderer.renderMode" && d.severity == Diagnostic::Severity::Warning);
    EXPECT_TRUE(found) << ParticleEffectCodec::format(diagnostics);
}

TEST(ParticleEffectCodec, AnEmptyPatchPassesEveryModule){
    // what the keyed form itself writes is never "unknown": an old effect stays editable
    ParticleEffectDocument doc = everyModule();
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::object(), diagnostics)) << ParticleEffectCodec::format(diagnostics);
}

TEST(ParticleEffectCodec, MergePatchTakesTheOtherGradientForm){
    // the keyed form writes colorKeys and alphaKeys; {colors, alphas} is the other form of a gradient, replacing them
    ParticleEffectDocument doc;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(R"({"modules": {"colorOverLifetime": {"color":
        {"mode": "gradient", "gradient": {"colorKeys": [[0, 1, 1, 1], [1, 0, 0, 0]], "alphaKeys": [[0, 1], [1, 1]]}}}}})"), diagnostics))
        << ParticleEffectCodec::format(diagnostics);
    ASSERT_EQ(doc.colorOverLifetime.color.gradient.colorKeys.size(), 2u);

    diagnostics.clear();
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(R"({"modules": {"colorOverLifetime": {"color":
        {"gradient": {"colors": [[0, "#ff0000"], [0.5, "#00ff00"], [1, "#0000ff"]], "alphas": [[0, 1], [1, 0]]}}}}})"), diagnostics))
        << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(doc.colorOverLifetime.color.gradient.colorKeys.size(), 3u);
    EXPECT_EQ(doc.colorOverLifetime.color.gradient.alphaKeys.back().alpha, 0.0f);

    // a misspelled key of that form is still the patch's
    const std::string before = ParticleEffectCodec::write(doc);
    diagnostics.clear();
    EXPECT_FALSE(ParticleEffectCodec::applyMergePatch(doc, nlohmann::ordered_json::parse(R"({"modules": {"colorOverLifetime": {"color":
        {"gradient": {"colors": [[0, "#ff0000"], [1, "#0000ff"]], "alphsa": [[0, 1]]}}}}})"), diagnostics));
    EXPECT_EQ(ParticleEffectCodec::write(doc), before);
}

TEST(ParticleEffectCodec, GenerateLightingDataLoadsButIsNotShown){
    // BF23 F4: nothing reads it, so the Inspector does not offer it (InspectorParticles shows no
    // hidden field); a file that has it still loads clean and keeps it
    const ModuleDesc* trails = ParticleModules::find("trails");
    ASSERT_NE(trails, nullptr);
    const FieldDesc* field = trails->fields.find("generateLightingData");
    ASSERT_NE(field, nullptr) << "unknown keys are not read: the key stays for old files";
    EXPECT_TRUE(field->flags & FieldFlag_Hidden);

    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(R"({"version": 1, "modules": [{"type": "trails", "generateLightingData": true}]})", document, diagnostics));
    EXPECT_TRUE(diagnostics.empty()) << ParticleEffectCodec::format(diagnostics);
    EXPECT_TRUE(document.trails.generateLightingData);
    EXPECT_NE(ParticleEffectCodec::write(document).find("generateLightingData"), std::string::npos);
}
