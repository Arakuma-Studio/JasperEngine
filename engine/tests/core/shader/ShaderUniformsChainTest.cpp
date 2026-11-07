// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The value a custom-block member is written with (docs/superpowers/specs/
// 2026-11-04-code-editor-git-design.md 6.6): its global binding, the own value, a mesh's under its
// material's, the scene's default-shader value, the manifest default, zero; and the bytes
// CustomUniformBlock::writeValues packs from that chain.

#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace jasperengine;

namespace {

    Vector4 written(const CustomUniformBlock& block, size_t member) {
        float values[4];
        std::memcpy(values, block.data.data() + block.members[member].offset, sizeof(values));
        return Vector4(values[0], values[1], values[2], values[3]);
    }

}

TEST(ShaderUniformsChain, Precedence) {
    ShaderUniformValues own = {{"own", Vector4(1, 0, 0, 0)}, {"bound", Vector4(5, 0, 0, 0)}};
    ShaderUniformValues fallback = {{"own", Vector4(-1, 0, 0, 0)}, {"fallback", Vector4(2, 0, 0, 0)}};
    ShaderUniformValues sceneDefault = {{"fallback", Vector4(-2, 0, 0, 0)}, {"scene", Vector4(3, 0, 0, 0)}};
    ShaderManifest manifest;
    manifest.present = true;
    ShaderManifestProperty property;
    property.member = "scene";
    property.defaultValue = Vector4(-3, 0, 0, 0);
    manifest.properties.push_back(property);
    property.member = "manifest";
    property.defaultValue = Vector4(4, 0, 0, 0);
    manifest.properties.push_back(property);
    ShaderGlobals::setVector(ShaderGlobals::propertyToID("chainTestGlobal"), Vector4(9, 9, 9, 9));
    const ShaderGlobalBindings bindings = {{"bound", "chainTestGlobal"}};

    UniformChain chain;
    chain.values = &own;
    chain.fallback = &fallback;
    chain.sceneDefault = &sceneDefault;
    chain.manifest = &manifest;
    chain.bindings = &bindings;

    struct Expect { const char* name; float x; UniformSource source; };
    const Expect expected[] = {
        {"bound", 9, UniformSource::Global},
        {"own", 1, UniformSource::Own},
        {"fallback", 2, UniformSource::Fallback},
        {"scene", 3, UniformSource::SceneDefault},
        {"manifest", 4, UniformSource::ShaderDefault},
        {"nothing", 0, UniformSource::Zero},
    };
    for (const Expect& e : expected) {
        UniformSource source = UniformSource::Zero;
        EXPECT_FLOAT_EQ(ShaderUniforms::resolve(chain, e.name, &source).x, e.x) << e.name;
        EXPECT_EQ(source, e.source) << e.name;
    }
    EXPECT_STREQ(uniformSourceName(UniformSource::SceneDefault), "scene_default");
    EXPECT_STREQ(uniformSourceName(UniformSource::ShaderDefault), "shader_default");

    // no scene layer: the manifest default shows through
    chain.sceneDefault = nullptr;
    UniformSource source;
    EXPECT_FLOAT_EQ(ShaderUniforms::resolve(chain, "scene", &source).x, -3.0f);
    EXPECT_EQ(source, UniformSource::ShaderDefault);
    ShaderGlobals::resetToDefaults();
}

TEST(ShaderUniformsChain, WriteValuesPacksTheChain) {
    CustomUniformBlock block;
    block.members = {{"tint", ShaderUniformType::FLOAT4, 1, 0}, {"time", ShaderUniformType::FLOAT, 1, 16}, {"count", ShaderUniformType::INT, 1, 20}};
    block.timeMember = 1;
    block.data.assign(32, 0);

    const ShaderUniformValues own = {{"count", Vector4(7, 0, 0, 0)}};
    const ShaderUniformValues sceneDefault = {{"tint", Vector4(0.25f, 0.5f, 0.75f, 1)}, {"time", Vector4(99, 0, 0, 0)}};
    UniformChain chain;
    chain.values = &own;
    chain.sceneDefault = &sceneDefault;
    block.writeValues(chain);
    EXPECT_EQ(written(block, 0), Vector4(0.25f, 0.5f, 0.75f, 1));
    // the engine-written member is left alone
    float time = -1.0f;
    std::memcpy(&time, block.data.data() + 16, sizeof(time));
    EXPECT_FLOAT_EQ(time, 0.0f);
    int count = 0;
    std::memcpy(&count, block.data.data() + 20, sizeof(count));
    EXPECT_EQ(count, 7);

    // the old overload has no scene layer: the member reads zero
    block.writeValues(own, nullptr, nullptr);
    EXPECT_EQ(written(block, 0), Vector4(0, 0, 0, 0));
}
