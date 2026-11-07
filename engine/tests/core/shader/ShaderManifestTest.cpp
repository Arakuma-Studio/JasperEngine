// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shader library's properties block ("// @directive" lines at the top of a shader source,
// docs/shaders) parsed by ShaderManifest, and the ShaderReference strings variants are keyed by.

#include "shader/ShaderManifest.h"
#include "util/Color.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // the decal of the engine library, abridged
    const char* const decalSource =
        "// @shader mesh\n"
        "// @name \"Decal\"\n"
        "// @category \"Effects\"\n"
        "// @description \"Mesh decal\"\n"
        "// @require uv1 color\n"
        "// @require tangents feature=NORMAL_MAP\n"
        "// @state blend=alpha zwrite=off shadows=off\n"
        "// @state[DECAL_BLEND_MULTIPLY] blend=multiply\n"
        "// @property tint color (0.5, 0.5, 0.5) \"Tint\"\n"
        "// @property tiling vector4 (1, 1, 0, 0) \"Tiling\"\n"
        "// @property opacity range(0, 2) 1 \"Opacity\" feature=!VERTEX_COLOR\n"
        "// @property flip toggle 5\n"
        "// @texture u_baseTexture white \"Decal\"\n"
        "// @texture u_envTexture black cube\n"
        "// @feature VERTEX_COLOR \"Vertex colour\"\n"
        "// @feature NORMAL_MAP \"Normal map\" default=on\n"
        "// @keywords DECAL_BLEND \"Default blend\" ALPHA MULTIPLY\n"
        "\n"
        "#include \"includes/surface.glsl\"\n"
        "// @name \"After the code\"\n";

    ShaderManifest parse(const std::string& source){
        ShaderManifest manifest;
        manifest.parse(source, "test.frag");
        return manifest;
    }

    bool hasDiagnostic(const ShaderManifest& manifest, bool error, const std::string& text){
        for (const ShaderManifestDiagnostic& d : manifest.diagnostics){
            if (d.error == error && d.message.find(text) != std::string::npos)
                return true;
        }
        return false;
    }

}

TEST(ShaderManifest, SourceWithoutDirectivesHasNoManifest){
    const ShaderManifest manifest = parse("#version 330\nvoid main(){}\n");
    EXPECT_FALSE(manifest.present);
    EXPECT_FALSE(ShaderManifest::hasManifestLines("void main(){}"));
    EXPECT_TRUE(ShaderManifest::hasManifestLines(decalSource));
}

TEST(ShaderManifest, ParsesTheHeaderDirectives){
    const ShaderManifest manifest = parse(decalSource);
    EXPECT_TRUE(manifest.present);
    EXPECT_FALSE(manifest.hasErrors());
    EXPECT_TRUE(manifest.hasType);
    EXPECT_EQ(manifest.type, ShaderType::MESH);
    EXPECT_EQ(manifest.category, "Effects");
    EXPECT_EQ(manifest.description, "Mesh decal");
}

TEST(ShaderManifest, StopsAtTheFirstLineOfCode){
    EXPECT_EQ(parse(decalSource).name, "Decal");
}

TEST(ShaderManifest, ColourDefaultsAreStoredLinear){
    const ShaderManifest manifest = parse(decalSource);
    const ShaderManifestProperty* tint = manifest.findProperty("tint");
    ASSERT_NE(tint, nullptr);
    EXPECT_EQ(tint->kind, ShaderPropertyKind::COLOR);
    EXPECT_NEAR(tint->defaultValue.x, std::pow(0.5f, 2.2f), 1e-5f);
    // three components: alpha 1
    EXPECT_FLOAT_EQ(tint->defaultValue.w, 1.0f);
    EXPECT_EQ(tint->label, "Tint");
}

TEST(ShaderManifest, RangeAndVectorProperties){
    const ShaderManifest manifest = parse(decalSource);
    const ShaderManifestProperty* opacity = manifest.findProperty("opacity");
    ASSERT_NE(opacity, nullptr);
    EXPECT_EQ(opacity->kind, ShaderPropertyKind::RANGE);
    EXPECT_FLOAT_EQ(opacity->min, 0.0f);
    EXPECT_FLOAT_EQ(opacity->max, 2.0f);
    EXPECT_EQ(opacity->feature, "!VERTEX_COLOR");
    Vector4 tiling;
    ASSERT_TRUE(manifest.getDefault("tiling", tiling));
    EXPECT_EQ(tiling, Vector4(1, 1, 0, 0));
    EXPECT_FALSE(manifest.getDefault("missing", tiling));
}

TEST(ShaderManifest, ToggleDefaultsAreZeroOrOne){
    const ShaderManifest manifest = parse(decalSource);
    const ShaderManifestProperty* flip = manifest.findProperty("flip");
    ASSERT_NE(flip, nullptr);
    EXPECT_EQ(flip->kind, ShaderPropertyKind::TOGGLE);
    EXPECT_FLOAT_EQ(flip->defaultValue.x, 1.0f);
}

TEST(ShaderManifest, Textures){
    const ShaderManifest manifest = parse(decalSource);
    const ShaderManifestTexture* base = manifest.findTexture("u_baseTexture");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->defaultTexture, ShaderTextureDefault::WHITE);
    EXPECT_FALSE(base->cube);
    EXPECT_EQ(base->label, "Decal");
    const ShaderManifestTexture* env = manifest.findTexture("u_envTexture");
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(env->defaultTexture, ShaderTextureDefault::BLACK);
    EXPECT_TRUE(env->cube);
}

TEST(ShaderManifest, FeaturesAndKeywords){
    const ShaderManifest manifest = parse(decalSource);
    ASSERT_NE(manifest.findFeature("VERTEX_COLOR"), nullptr);
    EXPECT_FALSE(manifest.findFeature("VERTEX_COLOR")->defaultOn);
    EXPECT_TRUE(manifest.findFeature("NORMAL_MAP")->defaultOn);
    const ShaderManifestKeywords* blend = manifest.findKeywordGroup("DECAL_BLEND_MULTIPLY");
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->values, (std::vector<std::string>{"ALPHA", "MULTIPLY"}));
    EXPECT_EQ(manifest.findKeywordGroup("DECAL_BLEND_ADD"), nullptr);
}

TEST(ShaderManifest, ResolveFeaturesAppliesDefaultsAndChoices){
    const ShaderManifest manifest = parse(decalSource);
    EXPECT_EQ(manifest.resolveFeatures({}), (std::vector<std::string>{"DECAL_BLEND_ALPHA", "NORMAL_MAP"}));
    EXPECT_EQ(manifest.resolveFeatures({"VERTEX_COLOR", "!NORMAL_MAP", "DECAL_BLEND_MULTIPLY", "UNDECLARED"}),
              (std::vector<std::string>{"DECAL_BLEND_MULTIPLY", "VERTEX_COLOR"}));
}

TEST(ShaderManifest, RowsFollowTheirFeature){
    EXPECT_TRUE(ShaderManifest::isRowShown("", {}));
    EXPECT_TRUE(ShaderManifest::isRowShown("A", {"A", "B"}));
    EXPECT_FALSE(ShaderManifest::isRowShown("A", {"B"}));
    EXPECT_TRUE(ShaderManifest::isRowShown("!A", {"B"}));
    EXPECT_FALSE(ShaderManifest::isRowShown("!A", {"A"}));
}

TEST(ShaderManifest, RequireRulesDependOnFeatures){
    const ShaderManifest manifest = parse(decalSource);
    EXPECT_EQ(manifest.require, uint32_t(ShaderRequire_UV1 | ShaderRequire_Color));
    EXPECT_EQ(manifest.resolveRequire({"NORMAL_MAP"}), uint32_t(ShaderRequire_UV1 | ShaderRequire_Color | ShaderRequire_Tangents));
    EXPECT_EQ(manifest.resolveRequire({}), uint32_t(ShaderRequire_UV1 | ShaderRequire_Color));
    EXPECT_EQ(ShaderManifest::getRequireNames(ShaderRequire_Color | ShaderRequire_Position | ShaderRequire_UV1),
              (std::vector<std::string>{"position", "uv1", "color"}));
}

TEST(ShaderManifest, StateRulesApplyOverTheBaseState){
    const ShaderManifest manifest = parse(decalSource);
    EXPECT_EQ(manifest.state.blend, MaterialBlendMode::ALPHA);
    EXPECT_EQ(manifest.state.depthWrite, MaterialDepthWrite::DISABLED);
    EXPECT_EQ(manifest.state.shadows, ShaderShadowCasting::OFF);
    const ShaderManifestState multiply = manifest.resolveState({"DECAL_BLEND_MULTIPLY"});
    EXPECT_EQ(multiply.blend, MaterialBlendMode::MULTIPLY);
    EXPECT_EQ(multiply.depthWrite, MaterialDepthWrite::DISABLED);
    EXPECT_EQ(manifest.resolveState({"DECAL_BLEND_ALPHA"}).blend, MaterialBlendMode::ALPHA);
}

TEST(ShaderManifest, LightingModesTheFirstIsTheDefault){
    const ShaderManifest lit = parse("// @shader mesh\n// @lighting vertex pixel vertex\n");
    EXPECT_EQ(lit.lighting.size(), 2u);
    EXPECT_EQ(lit.getDefaultLighting(), ShaderLightingMode::VERTEX);
    EXPECT_TRUE(lit.supportsLighting(ShaderLightingMode::PIXEL));
    EXPECT_FALSE(lit.supportsLighting(ShaderLightingMode::MIXED));
    EXPECT_EQ(parse("// @shader mesh\n").getDefaultLighting(), ShaderLightingMode::NONE);
}

TEST(ShaderManifest, UnknownDirectivesAreWarnings){
    const ShaderManifest manifest = parse("// @shader mesh\n// @frobnicate 3\n");
    EXPECT_FALSE(manifest.hasErrors());
    EXPECT_TRUE(hasDiagnostic(manifest, false, "unknown directive @frobnicate"));
}

TEST(ShaderManifest, InvalidPropertiesAreErrors){
    const ShaderManifest manifest = parse(
        "// @shader mesh\n"
        "// @property speed wobbly 1\n"
        "// @property time float 0\n"
        "// @property amount float 1\n"
        "// @property amount float 2\n"
        "// @property vec vector3 (1, 2)\n");
    EXPECT_TRUE(manifest.hasErrors());
    EXPECT_TRUE(hasDiagnostic(manifest, true, "unknown @property kind 'wobbly'"));
    EXPECT_TRUE(hasDiagnostic(manifest, true, "'time' is written by the engine"));
    EXPECT_TRUE(hasDiagnostic(manifest, true, "declared twice"));
    EXPECT_TRUE(hasDiagnostic(manifest, true, "needs 3 components"));
    EXPECT_EQ(manifest.properties.size(), 1u);
}

TEST(ShaderManifest, DiagnosticsCarryFileAndLine){
    const ShaderManifest manifest = parse("// @shader mesh\n// @shader wobbly\n");
    ASSERT_EQ(manifest.diagnostics.size(), 1u);
    EXPECT_EQ(manifest.diagnostics[0].file, "test.frag");
    EXPECT_EQ(manifest.diagnostics[0].line, 2);
}

TEST(ShaderManifest, KeywordGroupsNeedTwoValues){
    const ShaderManifest manifest = parse("// @shader mesh\n// @keywords MODE ONLY\n");
    EXPECT_TRUE(hasDiagnostic(manifest, true, "needs at least two values"));
    EXPECT_TRUE(manifest.keywords.empty());
}

TEST(ShaderManifest, LibraryShadersMustBeNamed){
    ShaderManifest manifest = parse("// @shader mesh\n");
    manifest.checkComplete("lib.frag", true);
    EXPECT_TRUE(manifest.hasErrors());
    ShaderManifest named = parse(decalSource);
    named.checkComplete("decal.frag", true);
    EXPECT_FALSE(named.hasErrors());
}

TEST(ShaderManifest, ColorWriteNamesRoundTrip){
    for (const char* name : {"none", "rgba", "rgb", "a", "rg", "gb"})
        EXPECT_EQ(ShaderManifest::getColorWriteName(ShaderManifest::parseColorWrite(name)), name);
    EXPECT_EQ(ShaderManifest::parseColorWrite("ar"), ShaderManifest::parseColorWrite("ra"));
    EXPECT_EQ(ShaderManifest::parseColorWrite("rr"), -1);
    EXPECT_EQ(ShaderManifest::parseColorWrite("x"), -1);
    EXPECT_EQ(ShaderManifest::getColorWriteName(-1), "auto");
}

TEST(ShaderManifest, StencilNamesRoundTrip){
    for (const char* name : {"off", "never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"}){
        MaterialStencilCompare compare;
        ASSERT_TRUE(ShaderManifest::parseStencilCompare(name, compare)) << name;
        EXPECT_STREQ(ShaderManifest::getStencilCompareName(compare), name);
    }
    for (const char* name : {"keep", "zero", "replace", "incr", "decr", "invert", "incrwrap", "decrwrap"}){
        MaterialStencilOp op;
        ASSERT_TRUE(ShaderManifest::parseStencilOp(name, op)) << name;
        EXPECT_STREQ(ShaderManifest::getStencilOpName(op), name);
    }
    MaterialStencilCompare unused;
    EXPECT_FALSE(ShaderManifest::parseStencilCompare("sometimes", unused));
}

TEST(ShaderReference, LibraryReferences){
    EXPECT_TRUE(ShaderReference::isLibrary("library:toon"));
    EXPECT_FALSE(ShaderReference::isLibrary("shaders/toon"));
    EXPECT_FALSE(ShaderReference::isLibrary(""));
    EXPECT_EQ(ShaderReference::getLibraryName("library:toon#RIM"), "toon");
}

TEST(ShaderReference, FeatureSuffix){
    EXPECT_EQ(ShaderReference::getBase("library:toon#RIM,OUTLINE"), "library:toon");
    EXPECT_EQ(ShaderReference::getFeatures("library:toon#RIM, OUTLINE"), (std::vector<std::string>{"RIM", "OUTLINE"}));
    EXPECT_TRUE(ShaderReference::getFeatures("library:toon").empty());
    EXPECT_EQ(ShaderReference::withFeatures("shaders/fork", {"A", "B"}), "shaders/fork#A,B");
    EXPECT_EQ(ShaderReference::withFeatures("shaders/fork", {}), "shaders/fork");
}

TEST(ShaderReference, DefineAndLibraryNameRules){
    EXPECT_TRUE(ShaderReference::isValidDefine("NORMAL_MAP2"));
    EXPECT_FALSE(ShaderReference::isValidDefine("2D"));
    EXPECT_FALSE(ShaderReference::isValidDefine("normal"));
    EXPECT_FALSE(ShaderReference::isValidDefine(""));
    EXPECT_TRUE(ShaderReference::isValidLibraryName("depth_fade"));
    EXPECT_FALSE(ShaderReference::isValidLibraryName("DepthFade"));
    EXPECT_FALSE(ShaderReference::isValidLibraryName("_fade"));
}

TEST(ShaderReference, BlendDefines){
    EXPECT_STREQ(ShaderReference::getBlendDefine(MaterialBlendMode::NONE), "BLEND_NONE");
    EXPECT_STREQ(ShaderReference::getBlendDefine(MaterialBlendMode::ADDITIVE), "BLEND_ADDITIVE");
    EXPECT_TRUE(ShaderReference::isBlendDefine("BLEND_MULTIPLY"));
    EXPECT_FALSE(ShaderReference::isBlendDefine("BLEND_SCREEN"));
}

// @role: what the engine treats a shader as, whatever its name (forks of library:water)
TEST(ShaderManifest, RoleDirective){
    const ShaderManifest none = parse("// @shader mesh\n// @name \"Plain\"\n");
    EXPECT_EQ(none.role, ShaderRole::NONE);

    const ShaderManifest water = parse("// @shader mesh\n// @name \"My water\"\n// @role water\n");
    EXPECT_EQ(water.role, ShaderRole::WATER);
    EXPECT_FALSE(water.hasErrors());

    const ShaderManifest underwater = parse("// @shader postprocess\n// @name \"My underwater\"\n// @role Underwater\n");
    EXPECT_EQ(underwater.role, ShaderRole::UNDERWATER);

    const ShaderManifest unknown = parse("// @shader mesh\n// @role ocean\n");
    EXPECT_EQ(unknown.role, ShaderRole::NONE);
    EXPECT_TRUE(hasDiagnostic(unknown, true, "@role takes one of"));

    const ShaderManifest twice = parse("// @shader mesh\n// @role water\n// @role underwater\n");
    EXPECT_EQ(twice.role, ShaderRole::WATER);
    EXPECT_TRUE(hasDiagnostic(twice, true, "@role given twice"));

    // a water is a mesh shader, an underwater pass a post-process one
    ShaderManifest wrongType = parse("// @shader postprocess\n// @name \"Wrong\"\n// @role water\n");
    wrongType.checkComplete("test.frag", false);
    EXPECT_TRUE(hasDiagnostic(wrongType, true, "@role water needs @shader mesh"));
}

// @require planarreflection: the define, the name, mesh shaders only
TEST(ShaderManifest, PlanarReflectionRequire){
    ShaderManifest manifest = parse("// @shader mesh\n// @name \"Mirror\"\n// @require planarreflection feature=MIRROR\n"
                                    "// @feature MIRROR \"Mirror\"\n");
    manifest.checkComplete("test.frag", true);
    EXPECT_FALSE(manifest.hasErrors());
    EXPECT_EQ(manifest.resolveRequire({}) & ShaderRequire_PlanarReflection, 0u);
    EXPECT_NE(manifest.resolveRequire({"MIRROR"}) & ShaderRequire_PlanarReflection, 0u);
    const std::vector<std::string> defines = ShaderManifest::getRequireDefines(ShaderRequire_PlanarReflection);
    ASSERT_EQ(defines.size(), 1u);
    EXPECT_EQ(defines[0], "SURFACE_PLANAR_REFLECTION");
    const std::vector<std::string> names = ShaderManifest::getRequireNames(ShaderRequire_PlanarReflection);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "planarreflection");

    ShaderManifest ui = parse("// @shader ui\n// @name \"UI\"\n// @require planarreflection\n");
    ui.checkComplete("test.frag", true);
    EXPECT_TRUE(hasDiagnostic(ui, true, "@require planarreflection needs @shader mesh"));
}
