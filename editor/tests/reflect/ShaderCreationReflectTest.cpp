// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Graphics > Shader Creation (docs/specs/S32-async-shaders.md R26, R32, AC20):
// the reflection table writes only what differs from the defaults (an older project re-saves
// unchanged), reads every field back, clamps the budget, and every field is a settings row with
// the table's label on the Graphics page.

#include "Generator.h"
#include "reflect/Reflect.h"
#include "settings/SettingsRegistry.h"

#include <gtest/gtest.h>

#include <cctype>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    YAML::Node encodeSettings(const ShaderCreationProjectSettings& settings){
        return reflect::encode(reflect::shaderCreationStruct(), &settings, reflect::EncodeContext{});
    }

    ShaderCreationProjectSettings decodeSettings(const YAML::Node& node){
        ShaderCreationProjectSettings settings;
        reflect::DecodeContext ctx;
        ctx.component = "shaderCreation";
        reflect::decode(reflect::shaderCreationStruct(), node, &settings, ctx);
        return settings;
    }

}

TEST(ShaderCreationReflect, DefaultsWriteNothing){
    const YAML::Node node = encodeSettings(ShaderCreationProjectSettings());
    EXPECT_TRUE(!node.IsMap() || node.size() == 0) << YAML::Dump(node);
    EXPECT_EQ(decodeSettings(YAML::Node(YAML::NodeType::Map)), ShaderCreationProjectSettings()) << "an absent block reads as the defaults";
}

TEST(ShaderCreationReflect, EveryFieldRoundTrips){
    ShaderCreationProjectSettings changed;
    changed.asyncCreation = false;
    changed.mainThreadBudgetMs = 6.5f;
    changed.glWarmDraws = false;
    changed.recordDuringPlay = true;
    changed.includeInBuilds = false;
    changed.holdFirstScene = false;
    const YAML::Node node = encodeSettings(changed);
    ASSERT_TRUE(node.IsMap());
    EXPECT_EQ(node.size(), reflect::shaderCreationStruct().fields.size()) << YAML::Dump(node);
    EXPECT_EQ(decodeSettings(node), changed);
    EXPECT_EQ(changed.engineSettings().mainThreadBudgetMs, 6.5f);
    EXPECT_FALSE(changed.engineSettings().holdFirstSceneForWarmup);
}

TEST(ShaderCreationReflect, TheBudgetIsClamped){
    YAML::Node node(YAML::NodeType::Map);
    node["mainThreadBudgetMs"] = 100.0f;
    EXPECT_EQ(decodeSettings(node).mainThreadBudgetMs, 16.0f);
    node["mainThreadBudgetMs"] = 0.0f;
    EXPECT_EQ(decodeSettings(node).mainThreadBudgetMs, 0.5f);
}

TEST(ShaderCreationReflect, EveryFieldIsASettingsRow){
    const SettingsRegistry& registry = SettingsRegistry::get();
    for (const reflect::FieldDesc& field : reflect::shaderCreationStruct().fields){
        std::string key = "shader_creation.";
        for (const char* c = field.name; *c; c++){
            if (std::isupper(static_cast<unsigned char>(*c))){
                key += '_';
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
            }else{
                key += *c;
            }
        }
        const SettingRow* row = registry.find(key);
        ASSERT_NE(row, nullptr) << key;
        EXPECT_EQ(row->page, "Graphics") << key;
        EXPECT_EQ(row->label, field.label) << key;
        EXPECT_TRUE(row->mcp) << key;
        EXPECT_TRUE(row->get && row->set) << key;
    }
    ASSERT_NE(registry.find("shader_creation.warmup_entries"), nullptr);
}
