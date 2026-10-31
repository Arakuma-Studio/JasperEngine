// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Global shader values (shader globals spec 9): ShaderGlobals' ids, declarations, setters and
// their conversions, versions; the globals block packed at the offsets reflection gives, for a
// synthetic block and for blocks compiled with the real pipeline on every backend (the game's
// four vertex lights among them); material and component bindings over the stored value and
// the manifest default.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Color.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class ShaderGlobalsTest : public ::testing::Test {
    protected:
        void SetUp() override { ShaderGlobals::clear(); }
        void TearDown() override { ShaderGlobals::clear(); }
    };

    using GlobalsBlockPackTest = ShaderGlobalsTest;
    using ShaderGlobalBindingTest = ShaderGlobalsTest;

    ShaderGlobalDecl decl(const std::string& name, ShaderGlobalType type, std::vector<Vector4> value = {}, unsigned int arraySize = 0) {
        ShaderGlobalDecl d = ShaderGlobals::makeDecl(name, type, arraySize);
        if (!value.empty()) d.defaultValue = value;
        return d;
    }

    Vector4 leading(const std::string& name) {
        Vector4 value;
        EXPECT_TRUE(ShaderGlobals::getVector(ShaderGlobals::findID(name), value)) << name;
        return value;
    }

    float floatAt(const std::vector<uint8_t>& bytes, size_t offset) {
        float value = 0.0f;
        if (offset + 4 <= bytes.size()) memcpy(&value, bytes.data() + offset, 4);
        return value;
    }

    ShaderUniform member(const std::string& name, ShaderUniformType type, unsigned int offset, unsigned int count = 1) {
        ShaderUniform uniform;
        uniform.name = "globals." + name;
        uniform.type = type;
        uniform.offset = offset;
        uniform.arrayCount = count;
        return uniform;
    }

    // a vertex-stage block with one member of each kind, std140
    ShaderData syntheticData() {
        ShaderData data;
        ShaderStage stage;
        stage.type = ShaderStageType::VERTEX;
        ShaderUniformBlock block;
        block.name = ShaderGlobals::VERTEX_BLOCK;
        block.instName = "globals";
        block.set = 0;
        block.binding = 0;
        block.slot = 3;
        block.sizeBytes = 240;
        block.flattened = false;
        block.uniforms = {member("f", ShaderUniformType::FLOAT, 0), member("v2", ShaderUniformType::FLOAT2, 8),
                          member("v3", ShaderUniformType::FLOAT3, 16), member("v4", ShaderUniformType::FLOAT4, 32),
                          member("m", ShaderUniformType::MAT4, 48), member("fa", ShaderUniformType::FLOAT, 112, 4),
                          member("va", ShaderUniformType::FLOAT4, 176, 4)};
        stage.uniformblocks.push_back(block);
        data.stages.push_back(stage);
        return data;
    }

    // the globals of syntheticData's block, with values whose every float differs
    void setEveryKind() {
        ShaderGlobals::setFloat(ShaderGlobals::propertyToID("f"), 1.5f);
        ShaderGlobals::setVector(ShaderGlobals::propertyToID("v2"), Vector4(2, 3, 99, 99));
        ShaderGlobals::setVector(ShaderGlobals::propertyToID("v3"), Vector4(4, 5, 6, 99));
        ShaderGlobals::setVector(ShaderGlobals::propertyToID("v4"), Vector4(7, 8, 9, 10));
        Matrix4 m;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) m.set(c, r, 20.0f + c * 4 + r);
        ShaderGlobals::setMatrix(ShaderGlobals::propertyToID("m"), m);
        const float fa[4] = {40, 41, 42, 43};
        ShaderGlobals::setFloatArray(ShaderGlobals::propertyToID("fa"), fa, 4);
        const Vector4 va[4] = {Vector4(50, 51, 52, 53), Vector4(54, 55, 56, 57), Vector4(58, 59, 60, 61), Vector4(62, 63, 64, 65)};
        ShaderGlobals::setVectorArray(ShaderGlobals::propertyToID("va"), va, 4);
    }

    // what syntheticData's packer must write, by the member offsets of `block`
    void expectEveryKind(const std::vector<uint8_t>& bytes, const std::map<std::string, unsigned int>& offsets, const std::string& where) {
        auto at = [&](const std::string& name, unsigned int extra) { return floatAt(bytes, offsets.at(name) + extra); };
        EXPECT_EQ(at("f", 0), 1.5f) << where;
        EXPECT_EQ(at("v2", 0), 2.0f) << where;
        EXPECT_EQ(at("v2", 4), 3.0f) << where;
        EXPECT_EQ(at("v3", 0), 4.0f) << where;
        EXPECT_EQ(at("v3", 8), 6.0f) << where;
        EXPECT_EQ(at("v4", 12), 10.0f) << where;
        for (unsigned int c = 0; c < 4; c++)
            for (unsigned int r = 0; r < 4; r++) EXPECT_EQ(at("m", c * 16 + r * 4), 20.0f + c * 4 + r) << where << " m[" << c << "][" << r << "]";
        for (unsigned int e = 0; e < 4; e++) EXPECT_EQ(at("fa", e * 16), 40.0f + e) << where << " fa[" << e << "]";
        for (unsigned int e = 0; e < 4; e++)
            for (unsigned int i = 0; i < 4; i++) EXPECT_EQ(at("va", e * 16 + i * 4), 50.0f + e * 4 + i) << where << " va[" << e << "]";
    }

    // a custom mesh shader (a fork on the library vertex path) compiled from memory
    bool compileFork(const std::string& base, const std::string& vert, const std::string& frag, ShaderBackend backend, ShaderData& out,
                     std::string& error) {
        CustomShaderSources sources;
        sources.valid = true;
        sources.vertFile = base + ".vert";
        sources.fragFile = base + ".frag";
        sources.files[sources.vertFile] = vert;
        sources.files[sources.fragFile] = frag;
        sources.signature = vert + "\n--\n" + frag;
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, 0, ShaderPool::registerCustomShader(base));
        std::string log;
        const bool ok = ShaderVariantCompiler::compile(key, backend, &sources, out, &error, &log);
        if (!ok) error += "\n" + log;
        return ok;
    }

    const ShaderUniformBlock* findBlock(const ShaderData& data, const std::string& name) {
        for (const ShaderStage& stage : data.stages) {
            for (const ShaderUniformBlock& block : stage.uniformblocks) {
                if (block.name == name) return &block;
            }
        }
        return nullptr;
    }

    const char* const kFragment = R"(#version 450
in vec4 v_globalsValue;
out vec4 g_finalColor;
#include "includes/output.glsl"
void main() {
    g_finalColor = finalizeColor(v_globalsValue);
}
)";

}

// ---------------------------------------------------------------------------- ShaderGlobalsTest

TEST_F(ShaderGlobalsTest, IdsAreStableForTheRun) {
    const int a = ShaderGlobals::propertyToID("_Wind");
    EXPECT_GT(a, 0);
    EXPECT_EQ(ShaderGlobals::propertyToID("_Wind"), a);
    EXPECT_NE(ShaderGlobals::propertyToID("_Other"), a);
    EXPECT_EQ(ShaderGlobals::nameOf(a), "_Wind");
    EXPECT_EQ(ShaderGlobals::findID("_Wind"), a);
    EXPECT_EQ(ShaderGlobals::findID("_NeverSeen"), 0);
    EXPECT_EQ(ShaderGlobals::nameOf(0), "");
    // an id is not a global: nothing exists until declared or set
    EXPECT_FALSE(ShaderGlobals::exists(a));
    ShaderGlobals::clear();
    EXPECT_EQ(ShaderGlobals::propertyToID("_Wind"), a) << "clear keeps the ids";
}

TEST_F(ShaderGlobalsTest, NamesAreGlslIdentifiersOutsideTheEngines) {
    std::string error;
    EXPECT_TRUE(ShaderGlobals::isValidName("_VertexLightColor0"));
    EXPECT_TRUE(ShaderGlobals::isValidName("FogTint"));
    EXPECT_TRUE(ShaderGlobals::isValidName(std::string(64, 'a')));
    EXPECT_FALSE(ShaderGlobals::isValidName(std::string(65, 'a')));
    EXPECT_FALSE(ShaderGlobals::isValidName(""));
    EXPECT_FALSE(ShaderGlobals::isValidName("1st"));
    EXPECT_FALSE(ShaderGlobals::isValidName("fog tint"));
    EXPECT_FALSE(ShaderGlobals::isValidName("u_time", &error));
    EXPECT_NE(error.find("u_"), std::string::npos) << error;
    EXPECT_FALSE(ShaderGlobals::isValidName("gl_Position"));
    EXPECT_FALSE(ShaderGlobals::isValidName("a__b"));
}

TEST_F(ShaderGlobalsTest, DeclareAndResetToTheDefaults) {
    ShaderGlobals::declare({decl("FogTint", ShaderGlobalType::Color, {Vector4(0.5f, 0.25f, 0.125f, 1.0f)}),
                            decl("Wind", ShaderGlobalType::Vector4, {Vector4(1, 0, 0, 2)})});
    EXPECT_TRUE(ShaderGlobals::isDeclared(ShaderGlobals::findID("FogTint")));
    EXPECT_EQ(leading("FogTint"), Vector4(0.5f, 0.25f, 0.125f, 1.0f));
    EXPECT_EQ(ShaderGlobals::getNames(), (std::vector<std::string>{"FogTint", "Wind"}));

    ShaderGlobals::setVector(ShaderGlobals::findID("Wind"), Vector4(9, 9, 9, 9));
    // an unchanged declaration keeps the value scripts set; a changed one takes its default
    ShaderGlobals::declare({decl("FogTint", ShaderGlobalType::Color, {Vector4(0, 1, 0, 1)}), decl("Wind", ShaderGlobalType::Vector4, {Vector4(1, 0, 0, 2)})});
    EXPECT_EQ(leading("Wind"), Vector4(9, 9, 9, 9));
    EXPECT_EQ(leading("FogTint"), Vector4(0, 1, 0, 1));

    ShaderGlobals::resetToDefaults();
    EXPECT_EQ(leading("Wind"), Vector4(1, 0, 0, 2));

    // a global no longer declared is dropped
    ShaderGlobals::declare({decl("Wind", ShaderGlobalType::Vector4)});
    EXPECT_FALSE(ShaderGlobals::exists(ShaderGlobals::findID("FogTint")));
}

TEST_F(ShaderGlobalsTest, InvalidDeclarationsAreSkipped) {
    EXPECT_NE(ShaderGlobals::validate({decl("A", ShaderGlobalType::Float), decl("A", ShaderGlobalType::Float)}), "");
    EXPECT_NE(ShaderGlobals::validate({decl("u_x", ShaderGlobalType::Float)}), "");
    ShaderGlobalDecl array = decl("Arr", ShaderGlobalType::FloatArray, {}, 4);
    array.arraySize = 65;
    EXPECT_NE(ShaderGlobals::validate({array}), "");
    std::vector<ShaderGlobalDecl> many;
    for (unsigned int i = 0; i <= ShaderGlobals::MAX_DECLARED; i++) many.push_back(decl("g" + std::to_string(i), ShaderGlobalType::Float));
    EXPECT_NE(ShaderGlobals::validate(many), "") << "at most 256";

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    ShaderGlobals::declare({decl("Good", ShaderGlobalType::Float), decl("Good", ShaderGlobalType::Vector4), decl("u_bad", ShaderGlobalType::Float)});
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(ShaderGlobals::getDeclarations().size(), 1u);
    EXPECT_EQ(ShaderGlobals::getDeclarations()[0].type, ShaderGlobalType::Float);
}

TEST_F(ShaderGlobalsTest, SettersConvertBetweenKinds) {
    ShaderGlobals::declare({decl("F", ShaderGlobalType::Float), decl("V2", ShaderGlobalType::Vector2), decl("V3", ShaderGlobalType::Vector3),
                            decl("V4", ShaderGlobalType::Vector4), decl("Arr", ShaderGlobalType::Vector4Array, {}, 3)});
    // a float into a vector: (x, 0, 0, 0); a vector into a float: its x
    EXPECT_TRUE(ShaderGlobals::setFloat(ShaderGlobals::findID("V4"), 2.0f));
    EXPECT_EQ(leading("V4"), Vector4(2, 0, 0, 0));
    EXPECT_TRUE(ShaderGlobals::setVector(ShaderGlobals::findID("F"), Vector4(3, 4, 5, 6)));
    EXPECT_EQ(leading("F"), Vector4(3, 0, 0, 0));
    // a vector keeps the components its type has
    EXPECT_TRUE(ShaderGlobals::setVector(ShaderGlobals::findID("V2"), Vector4(1, 2, 3, 4)));
    EXPECT_EQ(leading("V2"), Vector4(1, 2, 0, 0));
    EXPECT_TRUE(ShaderGlobals::setVector(ShaderGlobals::findID("V3"), Vector4(1, 2, 3, 4)));
    EXPECT_EQ(leading("V3"), Vector4(1, 2, 3, 0));
    // an array takes a vector setter in element 0
    EXPECT_TRUE(ShaderGlobals::setVector(ShaderGlobals::findID("Arr"), Vector4(7, 7, 7, 7)));
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(ShaderGlobals::findID("Arr"), value));
    ASSERT_EQ(value.values.size(), 3u);
    EXPECT_EQ(value.values[0], Vector4(7, 7, 7, 7));
    EXPECT_EQ(value.values[1], Vector4(0, 0, 0, 0));
}

TEST_F(ShaderGlobalsTest, ColoursAreSetAsSrgbAndStoredLinear) {
    // the engine's colour conversion (Color::sRGBToLinear, gamma 2.2), as materials and the
    // Inspector's colour pickers: 0.5 -> 0.2176376
    const int id = ShaderGlobals::propertyToID("Tint");
    EXPECT_TRUE(ShaderGlobals::setColor(id, Vector4(0.5f, 1.0f, 0.0f, 0.5f)));
    const Vector4 stored = leading("Tint");
    EXPECT_NEAR(stored.x, std::pow(0.5f, 2.2f), 1e-6f);
    EXPECT_NEAR(stored.x, 0.21763764f, 1e-6f);
    EXPECT_EQ(stored.y, 1.0f);
    EXPECT_EQ(stored.w, 0.5f) << "alpha as is";
    EXPECT_EQ(stored, Color::sRGBToLinear(Vector4(0.5f, 1.0f, 0.0f, 0.5f)));
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(id, value));
    EXPECT_EQ(value.type, ShaderGlobalType::Color) << "made by its first setter";
}

TEST_F(ShaderGlobalsTest, AMatrixOnlyTakesAMatrix) {
    ShaderGlobals::declare({decl("M", ShaderGlobalType::Matrix4), decl("V", ShaderGlobalType::Vector4)});
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    EXPECT_FALSE(ShaderGlobals::setVector(ShaderGlobals::findID("M"), Vector4(1, 2, 3, 4)));
    EXPECT_FALSE(ShaderGlobals::setFloat(ShaderGlobals::findID("M"), 1.0f));
    EXPECT_FALSE(ShaderGlobals::setMatrix(ShaderGlobals::findID("V"), Matrix4()));
    const std::string logged = testing::internal::GetCapturedStdout() + testing::internal::GetCapturedStderr();
    EXPECT_NE(logged.find("'M' is matrix4"), std::string::npos) << logged;
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(ShaderGlobals::findID("M"), value));
    EXPECT_EQ(value.values[0], Vector4(1, 0, 0, 0)) << "the identity stays";
    Matrix4 m = Matrix4::translateMatrix(Vector3(1, 2, 3));
    EXPECT_TRUE(ShaderGlobals::setMatrix(ShaderGlobals::findID("M"), m));
    ASSERT_TRUE(ShaderGlobals::get(ShaderGlobals::findID("M"), value));
    EXPECT_EQ(value.values[3], m.column(3));
}

TEST_F(ShaderGlobalsTest, ArraysAreSizedByTheirFirstSetAndTruncated) {
    const int id = ShaderGlobals::propertyToID("Weights");
    const float first[3] = {1, 2, 3};
    EXPECT_TRUE(ShaderGlobals::setFloatArray(id, first, 3));
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(id, value));
    EXPECT_EQ(value.type, ShaderGlobalType::FloatArray);
    EXPECT_EQ(value.arraySize, 3u);
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const float longer[5] = {5, 6, 7, 8, 9};
    EXPECT_TRUE(ShaderGlobals::setFloatArray(id, longer, 5));
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
    ASSERT_TRUE(ShaderGlobals::get(id, value));
    ASSERT_EQ(value.values.size(), 3u) << "truncated";
    EXPECT_EQ(value.values[2].x, 7.0f);
    // a shorter one sets its elements, the rest stay
    const float shorter[1] = {0};
    EXPECT_TRUE(ShaderGlobals::setFloatArray(id, shorter, 1));
    ASSERT_TRUE(ShaderGlobals::get(id, value));
    EXPECT_EQ(value.values[0].x, 0.0f);
    EXPECT_EQ(value.values[1].x, 6.0f);
    // an array setter does not take a scalar global
    ShaderGlobals::setFloat(ShaderGlobals::propertyToID("Scalar"), 1.0f);
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    EXPECT_FALSE(ShaderGlobals::setFloatArray(ShaderGlobals::findID("Scalar"), first, 3));
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
}

TEST_F(ShaderGlobalsTest, TheVersionMovesOnlyWhenAValueChanges) {
    const int id = ShaderGlobals::propertyToID("Wind");
    ShaderGlobals::setVector(id, Vector4(1, 2, 3, 4));
    const uint64_t version = ShaderGlobals::getVersion();
    EXPECT_TRUE(ShaderGlobals::setVector(id, Vector4(1, 2, 3, 4)));
    EXPECT_EQ(ShaderGlobals::getVersion(), version) << "the same bytes";
    ShaderGlobals::setVector(id, Vector4(1, 2, 3, 5));
    EXPECT_EQ(ShaderGlobals::getVersion(), version + 1);
    // textures too
    const int tex = ShaderGlobals::propertyToID("Noise");
    ShaderGlobals::setTexture(tex, Texture("textures/noise.png"));
    const uint64_t textured = ShaderGlobals::getVersion();
    ShaderGlobals::setTexture(tex, Texture("textures/noise.png"));
    EXPECT_EQ(ShaderGlobals::getVersion(), textured);
    ShaderGlobals::setTexture(tex, Texture("textures/other.png"));
    EXPECT_GT(ShaderGlobals::getVersion(), textured);
}

TEST_F(ShaderGlobalsTest, UndeclaredNamesAreTypedByTheirFirstSetAndDroppedByReset) {
    ShaderGlobals::declare({decl("Declared", ShaderGlobalType::Float, {Vector4(4, 0, 0, 0)})});
    const uint64_t names = ShaderGlobals::getNamesVersion();
    const int id = ShaderGlobals::propertyToID("_LightRadius");
    EXPECT_FALSE(ShaderGlobals::exists(id));
    ShaderGlobals::setVector(id, Vector4(1, 2, 3, 4));
    EXPECT_TRUE(ShaderGlobals::exists(id));
    EXPECT_FALSE(ShaderGlobals::isDeclared(id));
    EXPECT_GT(ShaderGlobals::getNamesVersion(), names);
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(id, value));
    EXPECT_EQ(value.type, ShaderGlobalType::Vector4);
    EXPECT_EQ(ShaderGlobals::getNames(), (std::vector<std::string>{"Declared", "_LightRadius"}));
    ShaderGlobals::setFloat(ShaderGlobals::findID("Declared"), 9.0f);

    ShaderGlobals::resetToDefaults();
    EXPECT_FALSE(ShaderGlobals::exists(id)) << "a reset drops what scripts made";
    EXPECT_EQ(leading("Declared").x, 4.0f);
}

TEST_F(ShaderGlobalsTest, TextureGlobalsTakeTexturesOnly) {
    ShaderGlobals::declare({decl("Noise", ShaderGlobalType::Texture2D), decl("Sky", ShaderGlobalType::TextureCube)});
    const int noise = ShaderGlobals::findID("Noise");
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    EXPECT_FALSE(ShaderGlobals::setFloat(noise, 1.0f));
    Texture cube;
    cube.setCubeMap("textures/sky.png");
    EXPECT_FALSE(ShaderGlobals::setTexture(noise, cube)) << "a 2D global takes no cube map";
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
    EXPECT_TRUE(ShaderGlobals::setTexture(noise, Texture("textures/noise.png")));
    ShaderGlobalValue value;
    ASSERT_TRUE(ShaderGlobals::get(noise, value));
    EXPECT_EQ(value.texture.getPath(0), "textures/noise.png");
    EXPECT_TRUE(ShaderGlobals::clearTexture(noise));
    ASSERT_TRUE(ShaderGlobals::get(noise, value));
    EXPECT_TRUE(value.texture.empty()) << "back to the default (none: the white fallback)";
    EXPECT_TRUE(ShaderGlobals::setTexture(ShaderGlobals::findID("Sky"), cube));
}

TEST_F(ShaderGlobalsTest, TheGlslBlockDeclaresEveryGlobalOfTheStage) {
    const std::vector<ShaderGlobalDecl> decls = {decl("FogTint", ShaderGlobalType::Color), decl("W", ShaderGlobalType::FloatArray, {}, 8),
                                                 decl("M", ShaderGlobalType::Matrix4), decl("_Noise", ShaderGlobalType::Texture2D)};
    const std::string vs = ShaderGlobals::getGlslBlock(decls, ShaderStageType::VERTEX);
    EXPECT_NE(vs.find("uniform u_vs_globals {"), std::string::npos) << vs;
    EXPECT_NE(vs.find("    vec4 FogTint;\n"), std::string::npos) << vs;
    EXPECT_NE(vs.find("    float W[8];\n"), std::string::npos) << vs;
    EXPECT_NE(vs.find("    mat4 M;\n"), std::string::npos) << vs;
    EXPECT_EQ(vs.find("_Noise"), std::string::npos) << "textures go in the fragment stage";
    const std::string fs = ShaderGlobals::getGlslBlock(decls, ShaderStageType::FRAGMENT, {"_Noise"});
    EXPECT_EQ(fs, "uniform texture2D _Noise;\nuniform sampler _Noise_smp;\n");
}

// ---------------------------------------------------------------------------- GlobalsBlockPackTest

TEST_F(GlobalsBlockPackTest, PacksEveryKindAtItsReflectionOffset) {
    ShaderData data = syntheticData();
    std::shared_ptr<GlobalsBlockLayout> layout = ShaderGlobals::acquireLayout(data, ShaderGlobals::VERTEX_BLOCK);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->size, 240u);
    // nothing set: zero
    for (uint8_t byte : layout->bytes) ASSERT_EQ(byte, 0);

    setEveryKind();
    layout->update();
    expectEveryKind(layout->bytes, {{"f", 0}, {"v2", 8}, {"v3", 16}, {"v4", 32}, {"m", 48}, {"fa", 112}, {"va", 176}}, "synthetic");
    // vec3 takes xyz only: the float after it (v4.x at 32) is its own
    EXPECT_EQ(floatAt(layout->bytes, 28), 0.0f);
}

TEST_F(GlobalsBlockPackTest, MismatchedKindsReadTheirLeadingComponentsOrZero) {
    ShaderData data = syntheticData();
    std::shared_ptr<GlobalsBlockLayout> layout = ShaderGlobals::acquireLayout(data, ShaderGlobals::VERTEX_BLOCK);
    // a vector4 global read by the float member f, a float read by the vec4 v4, a matrix by vec2
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("f"), Vector4(3, 4, 5, 6));
    ShaderGlobals::setFloat(ShaderGlobals::propertyToID("v4"), 2.0f);
    ShaderGlobals::setMatrix(ShaderGlobals::propertyToID("v2"), Matrix4::translateMatrix(Vector3(7, 7, 7)));
    // a 2-element array read by the 4-element fa: the rest 0; a vector read by the mat4: 0
    const float two[2] = {8, 9};
    ShaderGlobals::setFloatArray(ShaderGlobals::propertyToID("fa"), two, 2);
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("m"), Vector4(1, 1, 1, 1));
    layout->update();
    EXPECT_EQ(floatAt(layout->bytes, 0), 3.0f);
    EXPECT_EQ(floatAt(layout->bytes, 32), 2.0f);
    EXPECT_EQ(floatAt(layout->bytes, 36), 0.0f);
    EXPECT_EQ(floatAt(layout->bytes, 8), 0.0f) << "a matrix does not feed a vec2";
    EXPECT_EQ(floatAt(layout->bytes, 112), 8.0f);
    EXPECT_EQ(floatAt(layout->bytes, 128), 9.0f);
    EXPECT_EQ(floatAt(layout->bytes, 144), 0.0f);
    EXPECT_EQ(floatAt(layout->bytes, 48), 0.0f) << "a vector does not feed a mat4";
}

TEST_F(GlobalsBlockPackTest, ProgramsWithTheSameBlockShareOnePacking) {
    ShaderData a = syntheticData();
    ShaderData b = syntheticData();
    b.stages[0].uniformblocks[0].slot = 6; // another program, another slot, the same layout
    std::shared_ptr<GlobalsBlockLayout> first = ShaderGlobals::acquireLayout(a, ShaderGlobals::VERTEX_BLOCK);
    std::shared_ptr<GlobalsBlockLayout> second = ShaderGlobals::acquireLayout(b, ShaderGlobals::VERTEX_BLOCK);
    EXPECT_EQ(first.get(), second.get());
    // packed once per change of the values
    ShaderGlobals::setFloat(ShaderGlobals::propertyToID("f"), 1.0f);
    first->update();
    const uint64_t packed = first->packedVersion;
    second->update();
    EXPECT_EQ(second->packedVersion, packed);
    // another layout is its own
    ShaderData c = syntheticData();
    c.stages[0].uniformblocks[0].uniforms.pop_back();
    c.stages[0].uniformblocks[0].sizeBytes = 176;
    EXPECT_NE(ShaderGlobals::acquireLayout(c, ShaderGlobals::VERTEX_BLOCK).get(), first.get());
    // a program without the block has none
    ShaderData none;
    EXPECT_FALSE(ShaderGlobals::acquireLayout(none, ShaderGlobals::FRAGMENT_BLOCK));
}

TEST_F(GlobalsBlockPackTest, CompiledBlocksMatchThePackerOnEveryBackend) {
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    const std::string vert = R"(#version 450
#define SURFACE_VERTEX
#include "includes/surface_vs.glsl"
uniform u_vs_globals {
    float f;
    vec2 v2;
    vec3 v3;
    vec4 v4;
    mat4 m;
    float fa[4];
    vec4 va[4];
} globals;
out vec4 v_globalsValue;
void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv) {
    v_globalsValue = vec4(globals.f) + vec4(globals.v2, 0.0, 0.0) + vec4(globals.v3, 0.0) + globals.v4 +
                     globals.m * vec4(worldPosition, 1.0) + vec4(globals.fa[0] + globals.fa[3]) + globals.va[0] + globals.va[3];
}
)";
    setEveryKind();
    for (ShaderBackend backend : ShaderPool::getShaderBackends()) {
        const std::string name = ShaderPool::getShaderBackendName(backend);
        ShaderData data;
        std::string error;
        ASSERT_TRUE(compileFork("sgtest/everykind", vert, kFragment, backend, data, error)) << name << ": " << error;
        const ShaderUniformBlock* block = findBlock(data, ShaderGlobals::VERTEX_BLOCK);
        ASSERT_TRUE(block) << name;
        std::map<std::string, unsigned int> offsets;
        for (const ShaderUniform& uniform : block->uniforms) offsets[ShaderData::getUniformShortName(uniform.name)] = uniform.offset;
        // std140, as the shader declares it
        const std::map<std::string, unsigned int> std140 = {{"f", 0}, {"v2", 8}, {"v3", 16}, {"v4", 32}, {"m", 48}, {"fa", 112}, {"va", 176}};
        EXPECT_EQ(offsets, std140) << name;
        EXPECT_GE(block->sizeBytes, 240u) << name;
        std::shared_ptr<GlobalsBlockLayout> layout = ShaderGlobals::acquireLayout(data, ShaderGlobals::VERTEX_BLOCK);
        ASSERT_TRUE(layout) << name;
        layout->update();
        expectEveryKind(layout->bytes, offsets, name);
        EXPECT_TRUE(ShaderGlobals::checkShader(data, {}).size() >= 7u) << "every member undeclared: " << name;
    }
}

TEST_F(GlobalsBlockPackTest, TheGamesFourVertexLightsOnEveryBackend) {
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    // VertexLightsUpdater.cs: four colours and the X/Y/Z positions and radii as vec4s, set every
    // frame; a fork lights its vertices with them (tools/fixtures/shader-globals)
    const std::string vert = R"(#version 450
#define SURFACE_VERTEX
#include "includes/surface_vs.glsl"
uniform u_vs_globals {
    vec4 _LightPositionX;
    vec4 _LightPositionY;
    vec4 _LightPositionZ;
    vec4 _LightRadius;
    vec4 _VertexLightColor0;
    vec4 _VertexLightColor1;
    vec4 _VertexLightColor2;
    vec4 _VertexLightColor3;
} globals;
out vec4 v_globalsValue;
void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv) {
    vec3 light = vec3(0.0);
    vec4 colors[4] = vec4[4](globals._VertexLightColor0, globals._VertexLightColor1, globals._VertexLightColor2, globals._VertexLightColor3);
    for (int i = 0; i < 4; i++) {
        vec3 toLight = vec3(globals._LightPositionX[i], globals._LightPositionY[i], globals._LightPositionZ[i]) - worldPosition;
        float fade = clamp(1.0 - length(toLight) / max(globals._LightRadius[i], 0.0001), 0.0, 1.0);
        light += colors[i].rgb * fade * max(dot(worldNormal, normalize(toLight)), 0.0);
    }
    v_globalsValue = vec4(light, 1.0);
}
)";
    const std::vector<std::string> names = {"_LightPositionX", "_LightPositionY", "_LightPositionZ", "_LightRadius",
                                            "_VertexLightColor0", "_VertexLightColor1", "_VertexLightColor2", "_VertexLightColor3"};
    std::vector<ShaderGlobalDecl> decls;
    for (size_t i = 0; i < names.size(); i++) decls.push_back(decl(names[i], i < 4 ? ShaderGlobalType::Vector4 : ShaderGlobalType::Color));
    ShaderGlobals::declare(decls);
    // what the updater pushes in a frame (positions of four lights, their radii and colours)
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("_LightPositionX"), Vector4(-2, 2, 0, 0));
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("_LightPositionY"), Vector4(1, 1, 3, -3));
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("_LightPositionZ"), Vector4(0, 0, 1, 1));
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("_LightRadius"), Vector4(5, 5, 4, 4));
    const Vector4 colors[4] = {Vector4(1, 0, 0, 1), Vector4(0, 1, 0, 1), Vector4(0, 0, 1, 1), Vector4(1, 1, 1, 1)};
    for (int i = 0; i < 4; i++) ShaderGlobals::setColor(ShaderGlobals::propertyToID("_VertexLightColor" + std::to_string(i)), colors[i]);

    for (ShaderBackend backend : ShaderPool::getShaderBackends()) {
        const std::string name = ShaderPool::getShaderBackendName(backend);
        ShaderData data;
        std::string error;
        ASSERT_TRUE(compileFork("sgtest/vertexlights", vert, kFragment, backend, data, error)) << name << ": " << error;
        const ShaderUniformBlock* block = findBlock(data, ShaderGlobals::VERTEX_BLOCK);
        ASSERT_TRUE(block) << name;
        EXPECT_EQ(block->sizeBytes, 128u) << name;
        EXPECT_TRUE(ShaderGlobals::checkShader(data, decls).empty()) << "every member a declared vector4 / color: " << name;
        std::shared_ptr<GlobalsBlockLayout> layout = ShaderGlobals::acquireLayout(data, ShaderGlobals::VERTEX_BLOCK);
        ASSERT_TRUE(layout) << name;
        layout->update();
        for (const ShaderUniform& uniform : block->uniforms) {
            const std::string member = ShaderData::getUniformShortName(uniform.name);
            Vector4 expected = leading(member);
            for (int c = 0; c < 4; c++) {
                const float want = c == 0 ? expected.x : c == 1 ? expected.y : c == 2 ? expected.z : expected.w;
                EXPECT_EQ(floatAt(layout->bytes, uniform.offset + c * 4), want) << name << " " << member << "[" << c << "]";
            }
        }
        EXPECT_EQ(floatAt(layout->bytes, 0), -2.0f) << name << ": light 0's x first";
    }
}

TEST_F(GlobalsBlockPackTest, ABlockOverItsSizeFailsTheCompile) {
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    const std::string vert = R"(#version 450
#define SURFACE_VERTEX
#include "includes/surface_vs.glsl"
uniform u_vs_globals {
    vec4 big[65];
} globals;
out vec4 v_globalsValue;
void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv) {
    v_globalsValue = globals.big[0] + globals.big[64];
}
)";
    ShaderData data;
    std::string error;
    EXPECT_FALSE(compileFork("sgtest/toolarge", vert, kFragment, ShaderBackend::GLCore, data, error));
    EXPECT_EQ(error.rfind("globals-block-too-large", 0), 0u) << error;
}

TEST_F(GlobalsBlockPackTest, CheckShaderReportsUndeclaredAndMismatchedMembers) {
    ShaderData data = syntheticData();
    const std::vector<ShaderGlobalDecl> decls = {decl("f", ShaderGlobalType::Float), decl("v2", ShaderGlobalType::Vector4),
                                                 decl("v3", ShaderGlobalType::Vector3), decl("v4", ShaderGlobalType::Color),
                                                 decl("m", ShaderGlobalType::Matrix4), decl("fa", ShaderGlobalType::FloatArray, {}, 4),
                                                 decl("va", ShaderGlobalType::Vector4Array, {}, 2)};
    std::vector<ShaderGlobalDiagnostic> found = ShaderGlobals::checkShader(data, decls);
    // v2 (a vec2 of a vector4) and va (4 elements of 2) mismatch; the rest match
    ASSERT_EQ(found.size(), 2u);
    for (const ShaderGlobalDiagnostic& d : found) {
        EXPECT_EQ(d.code, "global-type-mismatch");
        EXPECT_FALSE(d.error);
    }
    EXPECT_NE(found[0].message.find("'v2'"), std::string::npos) << found[0].message;
    EXPECT_NE(found[1].message.find("'va'"), std::string::npos) << found[1].message;
    found = ShaderGlobals::checkShader(data, {});
    ASSERT_EQ(found.size(), 7u);
    EXPECT_EQ(found[0].code, "global-undeclared");
    data.stages[0].uniformblocks[0].sizeBytes = 1040;
    found = ShaderGlobals::checkShader(data, decls);
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(found[0].code, "globals-block-too-large");
    EXPECT_TRUE(found[0].error);
}

// ---------------------------------------------------------------------------- ShaderGlobalBindingTest

TEST_F(ShaderGlobalBindingTest, ABoundMemberFollowsTheGlobalOverItsValueAndTheDefault) {
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get("library:unlit");
    ASSERT_TRUE(manifest && manifest->present) << "the engine library";
    CustomUniformBlock block;
    ShaderUniform baseColor;
    baseColor.name = "baseColor";
    baseColor.type = ShaderUniformType::FLOAT4;
    baseColor.offset = 0;
    baseColor.arrayCount = 1;
    block.slot = 1;
    block.members = {baseColor};
    block.data.assign(16, 0);
    auto written = [&block]() {
        return Vector4(floatAt(block.data, 0), floatAt(block.data, 4), floatAt(block.data, 8), floatAt(block.data, 12));
    };

    ShaderGlobals::declare({decl("FogTint", ShaderGlobalType::Color, {Color::sRGBToLinear(Vector4(0.5f, 0.5f, 0.5f, 1.0f))})});
    ShaderUniformValues values;
    ShaderUniforms::set(values, "baseColor", Vector4(0, 1, 0, 1));
    ShaderGlobalBindings bindings;
    EXPECT_TRUE(ShaderGlobals::setBinding(bindings, "baseColor", "FogTint"));
    EXPECT_FALSE(ShaderGlobals::setBinding(bindings, "baseColor", "FogTint")) << "nothing changed";
    EXPECT_EQ(ShaderGlobals::getBinding(bindings, "baseColor"), "FogTint");

    block.writeValues(values, nullptr, manifest.get(), &bindings);
    EXPECT_EQ(written(), leading("FogTint")) << "over the stored value";
    ShaderUniformValues none;
    block.writeValues(none, nullptr, manifest.get(), &bindings);
    EXPECT_EQ(written(), leading("FogTint")) << "over the manifest default";

    // the global changes: the blocks written before are outdated once
    const uint64_t seen = ShaderGlobals::getVersion();
    EXPECT_FALSE(ShaderGlobals::isOutdated(bindings, seen));
    ShaderGlobals::setColor(ShaderGlobals::findID("FogTint"), Vector4(1, 0, 0, 1));
    EXPECT_TRUE(ShaderGlobals::isOutdated(bindings, seen));
    block.writeValues(values, nullptr, manifest.get(), &bindings);
    EXPECT_EQ(written(), Vector4(1, 0, 0, 1));
    const uint64_t rewritten = ShaderGlobals::getVersion();
    ShaderGlobals::setColor(ShaderGlobals::findID("FogTint"), Vector4(1, 0, 0, 1));
    EXPECT_FALSE(ShaderGlobals::isOutdated(bindings, rewritten)) << "the same value is no change";
    EXPECT_FALSE(ShaderGlobals::isOutdated(ShaderGlobalBindings(), seen)) << "nothing bound, nothing to rewrite";

    // unbound: its own value again, else the manifest default
    EXPECT_TRUE(ShaderGlobals::setBinding(bindings, "baseColor", ""));
    EXPECT_TRUE(bindings.empty());
    block.writeValues(values, nullptr, manifest.get(), &bindings);
    EXPECT_EQ(written(), Vector4(0, 1, 0, 1));
    block.writeValues(none, nullptr, manifest.get(), &bindings);
    Vector4 fallback;
    ASSERT_TRUE(manifest->getDefault("baseColor", fallback));
    EXPECT_EQ(written(), fallback);
}

TEST_F(ShaderGlobalBindingTest, AnUnknownGlobalReadsZeroAndTexturesCannotBeBound) {
    CustomUniformBlock block;
    ShaderUniform tint;
    tint.name = "tint";
    tint.type = ShaderUniformType::FLOAT4;
    tint.offset = 0;
    tint.arrayCount = 1;
    block.slot = 1;
    block.members = {tint};
    block.data.assign(16, 0xFF);
    ShaderUniformValues values;
    ShaderUniforms::set(values, "tint", Vector4(1, 1, 1, 1));
    ShaderGlobalBindings bindings = {{"tint", "_NotSetYet"}};
    block.writeValues(values, nullptr, nullptr, &bindings);
    EXPECT_EQ(floatAt(block.data, 0), 0.0f) << "bound to a global no one set: 0, like Unity";
    // a script sets it later: the next write takes it
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("_NotSetYet"), Vector4(0.25f, 0, 0, 0));
    block.writeValues(values, nullptr, nullptr, &bindings);
    EXPECT_EQ(floatAt(block.data, 0), 0.25f);

    ShaderGlobals::declare({decl("_Noise", ShaderGlobalType::Texture2D)});
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    EXPECT_FALSE(ShaderGlobals::setBinding(bindings, "tint", "_Noise"));
    EXPECT_FALSE(ShaderGlobals::setBinding(bindings, "tint", "u_bad"));
    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(ShaderGlobals::getBinding(bindings, "tint"), "_NotSetYet");
}
