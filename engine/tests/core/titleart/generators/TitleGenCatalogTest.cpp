// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's generator definitions without a GPU (docs/specs/S83-title-effects-generators.md §7):
// - TitleGenCatalog.EveryGeneratorComplete (AC1, generator half): the sixteen ids of §6.7 are
//   registered, each passes S80's definition checks, has aliases, a preset file whose presets set
//   only known params inside their ranges, live params that exist and are marked Live, light
//   colours marked `light`, and Unsafe glyph safety when it follows a layer (§6.13); a test-only
//   definition with a default outside its range fails (red control).
// - TitleFxBudget.GeneratorStaticModel (AC14, generator half): every generator at defaults and at
//   each preset, at 1920 x 1080 (desktop) and 1024 x 512 (mobile), stays inside its class's static
//   limit (a preset may state one class heavier); a definition declaring C1 with 5 FRE fails.
// - TitleGenStreams: Pachislop's mulberry32 seedings and integer hash, bit for bit.
// - TitleFxNoClock (AC26, generator half): no clock in the generator code and shaders.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/gpu/QuadBatch.h"
#include "titleart/render/TitlePasses.h"

#include "../support/TitleGenTestSupport.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    const std::vector<std::string> S83_GENERATORS = {"particles", "ink_splatter", "speed_lines", "light_streaks", "shockwave", "magic_circle",
                                                     "brush_swirl", "glints", "lens_flare", "backlight", "fire", "smoke",
                                                     "aura", "lightning", "sunburst", "fractal_noise"};

    // §6.4's static limits per class: FRE and taps per pixel (C3 and C4 are not bounded by taps here)
    bool withinClass(const FxCost& c, CostClass cls) {
        switch (cls) {
            case CostClass::C0: return c.fre <= 1.0 && c.maxTapsPerPixel <= 16;
            case CostClass::C1: return c.fre <= 3.0 && c.maxTapsPerPixel <= 64;
            case CostClass::C2: return c.fre <= 10.0 && c.maxTapsPerPixel <= 200;
            case CostClass::C3: return c.fre <= 40.0;
            case CostClass::C4: return true;
        }
        return false;
    }

    CostClass heavier(CostClass c) {
        return c == CostClass::C4 ? c : (CostClass)((int)c + 1);
    }

    bool classFromName(const std::string& name, CostClass& out) {
        for (int i = 0; i <= 4; i++) {
            if (name == costClassName((CostClass)i)) {
                out = (CostClass)i;
                return true;
            }
        }
        return false;
    }

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

}

TEST(TitleGenCatalog, EveryGeneratorComplete) {
    TitleEffectRegistry& registry = TitleEffectRegistry::get();
    for (const std::string& type : S83_GENERATORS) {
        SCOPED_TRACE(type);
        const GeneratorDef* def = registry.generator(type);
        ASSERT_TRUE(def) << type << " is not registered";
        const std::vector<std::string> problems = checkTitleGeneratorDef(*def);
        EXPECT_TRUE(problems.empty()) << problems.front();
        EXPECT_FALSE(def->aliases.empty());
        ASSERT_TRUE(def->presetFile);
        EXPECT_EQ(std::string(def->presetFile), "editor/resources/titleart/fx-presets/" + type + ".yaml");
        EXPECT_TRUE(def->render);
        EXPECT_TRUE(def->estimate) << "every generator states its cost model";
        if (def->follow != FollowMode::None) EXPECT_EQ(def->glyphSafety, GlyphSafety::Unsafe) << "§6.13: a following generator needs the whole text";
        for (const char* live : def->live.params) {
            const ParamDef* p = findParam(def->params, live);
            ASSERT_TRUE(p) << "live param " << live << " does not exist";
            EXPECT_EQ(p->anim, FxAnim::Live) << live << " is live but not marked L";
        }
        if (def->live.kind != LiveKind::None) {
            EXPECT_FALSE(def->live.params.empty());
            EXPECT_TRUE(def->live.program & TITLE_VARIANT_GENERATOR) << "the live kernel is a generator program";
        }
        const bool light = def->addsLight && def->addsLight(ParamValues(defaultParams(def->params)));
        for (const ParamDef& p : def->params) {
            if (p.kind == ParamKind::Number && p.unit == ParamUnit::Percent) ADD_FAILURE() << p.key << ": a generator percent is not of the layer's size (unit None)";
            if (light && (p.kind == ParamKind::Color || p.kind == ParamKind::Gradient)) {
                const std::string key = p.key;
                if (key.find("shadow") == std::string::npos) EXPECT_TRUE(p.light) << key << " is a colour of light (R55)";
            }
        }
        const Json presets = loadGeneratorPresets(type);
        EXPECT_FALSE(presets.empty()) << "R7: every type has presets";
        std::set<std::string> ids;
        for (const Json& preset : presets) {
            const std::string id = preset.value("id", std::string());
            EXPECT_TRUE(ids.insert(id).second) << "preset id " << id << " twice";
            EXPECT_FALSE(preset.value("name", std::string()).empty()) << id;
            const Json presetParams = preset.value("params", Json::object());
            for (auto it = presetParams.begin(); it != presetParams.end(); ++it) {
                const ParamDef* p = findParam(def->params, it.key());
                if (!p) {
                    ADD_FAILURE() << id << ": unknown param " << it.key();
                    continue;
                }
                Json value = it.value();
                std::vector<TitleWarning> warnings;
                NormalizeContext context;
                context.warnings = &warnings;
                EXPECT_TRUE(normalizeParamValue(*p, value, it.key(), context)) << id << "." << it.key();
                EXPECT_TRUE(warnings.empty()) << id << "." << it.key() << ": " << (warnings.empty() ? "" : warnings.front().message);
            }
            const std::string source = preset.value("source", std::string());
            if (!source.empty()) {
                std::string ref = source;
                std::replace(ref.begin(), ref.end(), '/', '-');
                EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / "engine/tests/core/golden/titleart/pachislop/generator" / (ref + ".png")))
                    << id << ": no Pachislop reference " << ref;
            }
        }
    }
    // red control: a default outside its range is reported
    GeneratorDef broken = *registry.generator("shockwave");
    broken.type = "test_broken_generator";
    broken.label = "Test Broken Generator";
    broken.params[1].defaultValue = 99999.0;
    EXPECT_FALSE(checkTitleGeneratorDef(broken).empty()) << "a default outside its range passed the checks";
}

TEST(TitleFxBudget, GeneratorStaticModel) {
    const FxSize desktop{1920, 1080, 1.0f}, mobile{1024, 512, 1024.0f / 1920.0f};
    for (const std::string& type : S83_GENERATORS) {
        const GeneratorDef* def = findGenerator(type);
        ASSERT_TRUE(def && def->estimate) << type;
        std::vector<std::pair<std::string, Json>> cases = {{"defaults", defaultParams(def->params)}};
        std::map<std::string, CostClass> stated;
        for (const Json& preset : loadGeneratorPresets(type)) {
            Json values = defaultParams(def->params);
            const Json presetParams = preset.value("params", Json::object());
            for (auto it = presetParams.begin(); it != presetParams.end(); ++it) values[it.key()] = it.value();
            const std::string id = preset.value("id", std::string());
            cases.push_back({id, values});
            CostClass c;
            if (classFromName(preset.value("class", std::string()), c)) stated[id] = c;
        }
        for (const auto& [name, values] : cases) {
            SCOPED_TRACE(type + "/" + name);
            const ParamValues p(values);
            CostClass allowed = def->costClass;
            if (name != "defaults") allowed = stated.count(name) ? stated[name] : heavier(def->costClass);
            if (name != "defaults" && stated.count(name)) EXPECT_LE((int)stated[name], (int)heavier(def->costClass)) << "a preset is at most one class heavier";
            const FxCost d = def->estimate(p, desktop, FxTier::Desktop);
            EXPECT_TRUE(withinClass(d, allowed)) << "desktop: " << d.fre << " FRE, " << d.maxTapsPerPixel << " taps over " << costClassName(allowed);
            const FxCost m = def->estimate(p, mobile, FxTier::Mobile);
            // the mobile tier's reductions keep a definition inside its class (C4 is baked there)
            if (def->costClass != CostClass::C4) EXPECT_TRUE(withinClass(m, allowed)) << "mobile: " << m.fre << " FRE, " << m.maxTapsPerPixel << " taps";
            EXPECT_GE(d.passes, 1);
            EXPECT_LE(d.instances, gen::QUAD_BATCH_MAX);
        }
    }
    // red control: a definition declaring C1 that costs 5 FRE fails the check
    FxCost five;
    five.passes = 2;
    five.fre = 5.0;
    five.maxTapsPerPixel = 20;
    EXPECT_FALSE(withinClass(five, CostClass::C1));
}

TEST(TitleGenStreams, PachislopBitForBit) {
    // the first three values of each seeding and the hash, from an independent transcription of
    // Pachislop's src/generators/common.ts rng, src/effects/b/common.ts rng and hash with
    // JavaScript's ToInt32, Math.imul and >>> semantics (tools-free: the values are written here)
    const struct {
        double seed;
        gen::Rng::Seeding seeding;
        double v[3];
    } streams[] = {
        {1, gen::Rng::Seeding::Generators, {0.6547490986995399, 0.3688110620714724, 0.05952108884230256}},
        {7, gen::Rng::Seeding::Generators, {0.8460092612076551, 0.29240244743414223, 0.8800481026992202}},
        {0.5, gen::Rng::Seeding::Generators, {0.4798972646240145, 0.9535404334310442, 0.8577825401443988}},
        {-3, gen::Rng::Seeding::Generators, {0.09750956669449806, 0.42305788095109165, 0.25273536052554846}},
        {1, gen::Rng::Seeding::EffectsB, {0.1780288484878838, 0.6602697039488703, 0.8400148365180939}},
        {7, gen::Rng::Seeding::EffectsB, {0.5828149127773941, 0.3743700582999736, 0.5567959367763251}},
        {0.5, gen::Rng::Seeding::EffectsB, {0.3324293149635196, 0.0682323994114995, 0.5569243221543729}},
        {-3, gen::Rng::Seeding::EffectsB, {0.06393334548920393, 0.12087185471318662, 0.0879044234752655}},
    };
    for (const auto& s : streams) {
        gen::Rng r(s.seed, s.seeding);
        for (double v : s.v) EXPECT_EQ(r.next(), v) << "seed " << s.seed;
    }
    EXPECT_EQ(gen::hashN({1.0, 2.0}), 0.7101880214177072);
    EXPECT_EQ(gen::hashN({2.0, 1.0}), 0.7336211451329291);
    EXPECT_EQ(gen::hashN({-5.5, 3.25, 1000.0}), 0.3953359234146774);
    EXPECT_EQ(gen::hashN({123456.789}), 0.3731854052748531);
}

// the 'Gen' bit is part of a program's name, so a generator program's compiled shader never
// shares a cache file with the effect program of the same number (it did before the bit was named)
TEST(TitleGenStreams, GeneratorProgramKeysAreTheirOwn) {
    TitleArtCaps caps;
    const ShaderKey gen12 = gen::genProgramKey(gen::GenProgram::Sunburst, gen::genProgramVariant(gen::GenProgram::Sunburst, caps));
    const ShaderKey fx12 = titleProgramKey(TitleProgram::ColorOverlay);
    EXPECT_NE(ShaderPool::getShaderStr(ShaderType::TITLE_ART, gen12.properties), ShaderPool::getShaderStr(ShaderType::TITLE_ART, fx12.properties));
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::TITLE_ART, gen12.properties), "titleart_Pb2Pb3Gen");
    std::set<std::string> names;
    for (gen::GenProgram p : gen::genPrograms()) EXPECT_TRUE(names.insert(gen::genProgramName(p)).second) << gen::genProgramName(p);
}

TEST(TitleFxNoClock, GeneratorsReadNoTime) {
    const std::filesystem::path root = repositoryRoot();
    const std::regex shaderClock(R"(\b(u_?[Tt]ime|iTime|uClock|u_clock|frameTime)\b)");
    const std::regex cppClock(R"(\b(steady_clock|system_clock|high_resolution_clock|std::time\(|clock\(\)|getTime\(|Engine::getDeltatime|getFrameTime))");
    int scanned = 0;
    for (const char* dir : {"engine/shaderlib/titleart/gen", "engine/core/titleart/generators", "engine/core/titleart/analysis", "engine/core/titleart/gpu"}) {
        if (!std::filesystem::exists(root / dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root / dir)) {
            const std::string text = readFile(entry.path());
            const bool shader = entry.path().extension() == ".glsl";
            EXPECT_FALSE(std::regex_search(text, shader ? shaderClock : cppClock)) << entry.path().string() << " reads a clock";
            scanned++;
        }
    }
    EXPECT_GE(scanned, 10);
}
