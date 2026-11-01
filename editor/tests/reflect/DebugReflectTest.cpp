// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Debug on its reflection table (editor/reflect/DebugReflect.cpp, S08): the
// enums' fallbacks are DebugSettings' defaults (a value that does not read keeps its default), the
// names DebugSettingsNames gives are the table's tokens, every field is one S25 row on the Debug
// page with the table's label and section, and the schema lists the block.

#include "DebugSettings.h"
#include "reflect/Reflect.h"
#include "settings/SettingsRegistry.h"
#include "Project.h"
#include "command/CommandHistory.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(DebugReflect, FallbacksAreTheDefaults){
    DebugSettings defaults;
    for (const reflect::FieldDesc& field : reflect::debugSettingsStruct().fields){
        if (!field.enumDesc) continue;
        EXPECT_EQ(field.enumDesc->fallback, reflect::readEnum(field, field.ref(&defaults))) << field.name;
    }
}

TEST(DebugReflect, NamesAreTheTablesTokens){
    EXPECT_EQ(DebugSettingsNames::modes(), (std::vector<std::string>{"never", "development", "always"}));
    EXPECT_EQ(DebugSettingsNames::gestures(), (std::vector<std::string>{"three_finger_hold", "four_finger_tap", "none"}));
    EXPECT_EQ(DebugSettingsNames::popups(), (std::vector<std::string>{"never", "touch_devices", "always"}));
    EXPECT_EQ(DebugSettingsNames::stats(), (std::vector<std::string>{"off", "fps", "full"}));
    EXPECT_EQ(DebugSettingsNames::corners(), (std::vector<std::string>{"top_left", "top_right", "bottom_left", "bottom_right"}));
    EXPECT_EQ(DebugSettingsNames::allowLua(), (std::vector<std::string>{"development", "always", "never"}));
    EXPECT_EQ(DebugSettingsNames::stackTraces(), (std::vector<std::string>{"none", "errors", "warnings", "all"}));
    EXPECT_EQ(DebugSettingsNames::name(DebugOverlaySettings::Gesture::Off), "none");
}

TEST(DebugReflect, EveryFieldIsADebugRow){
    const SettingsRegistry& registry = SettingsRegistry::get();
    std::set<std::string> fields;
    for (const DebugSettingsNames::SettingKey& entry : DebugSettingsNames::settingKeys()){
        const reflect::FieldDesc* field = reflect::debugSettingsField(entry.field);
        ASSERT_NE(field, nullptr) << entry.field;
        fields.insert(entry.field);
        const SettingRow* row = registry.find(entry.key);
        ASSERT_NE(row, nullptr) << entry.key;
        EXPECT_EQ(row->page, "Debug") << entry.key;
        EXPECT_EQ(row->section, field->group) << entry.key;
        EXPECT_EQ(row->label, field->label) << entry.key;
        EXPECT_TRUE(row->mcp && row->get && row->set) << entry.key;
        if (field->enumDesc) { EXPECT_EQ(row->values.size(), field->enumDesc->items.size()) << entry.key; }
        EXPECT_EQ(DebugSettingsNames::settingKeyOf(entry.field), entry.key);
    }
    EXPECT_EQ(fields.size(), reflect::debugSettingsStruct().fields.size()) << "a field without a row";
}

TEST(DebugReflect, OnlyChangedValuesAreWrittenAndBadOnesKeepTheirDefault){
    DebugSettings changed;
    changed.popup = DebugOverlaySettings::Popup::AllDevices;
    changed.logBufferSize = 64;
    const YAML::Node node = reflect::encode(reflect::debugSettingsStruct(), &changed, reflect::EncodeContext{});
    ASSERT_TRUE(node.IsMap());
    EXPECT_EQ(node.size(), 2u) << YAML::Dump(node);
    EXPECT_EQ(node["popup"].as<std::string>(), "always");

    YAML::Node bad(YAML::NodeType::Map);
    bad["gesture"] = "Four-Finger Tap";
    bad["stats"] = "sometimes";
    bad["statsKey"] = "nokey";
    bad["logBufferSize"] = 99999999;
    DebugSettings read;
    reflect::DecodeContext ctx;
    ctx.component = "debug";
    reflect::decode(reflect::debugSettingsStruct(), bad, &read, ctx);
    std::vector<std::string> rejected;
    read.sanitize([&rejected](const std::string& key, const std::string&){ rejected.push_back(key); });
    EXPECT_EQ(read.gesture, DebugOverlaySettings::Gesture::FourFingerTap) << "case and '-' are normalized";
    EXPECT_EQ(read.stats, DebugSettings().stats);
    EXPECT_EQ(read.statsKey, DebugSettings().statsKey);
    EXPECT_EQ(rejected, (std::vector<std::string>{"statsKey"}));
    EXPECT_EQ(read.logBufferSize, DebugSettings::maxLogBufferSize);
}

// set_project_settings' path: each debug.* row applied is one undoable step of the project history
TEST(DebugReflect, EachRowIsOneUndoStep){
    Project project;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    const SettingsRegistry& registry = SettingsRegistry::get();
    std::string error;
    ASSERT_TRUE(registry.find("debug.log_buffer_size")->set(ctx, mcp::Json(200), true, error)) << error;
    ASSERT_TRUE(registry.find("debug.overlay")->set(ctx, mcp::Json("always"), true, error)) << error;
    ASSERT_TRUE(registry.find("debug.overlay_allow_lua")->set(ctx, mcp::Json("always"), true, error)) << error;
    EXPECT_EQ(project.getDebugSettings().overlay, DebugSettings::Mode::AllBuilds);
    EXPECT_EQ(project.getDebugSettings().logBufferSize, 200u);
    project.getProjectCommandHistory()->undo();
    EXPECT_EQ(project.getDebugSettings().allowLua, DebugSettings::AllowLua::Development);
    EXPECT_EQ(project.getDebugSettings().overlay, DebugSettings::Mode::AllBuilds) << "one step undone";
    EXPECT_EQ(project.getDebugSettings().logBufferSize, 200u);
    project.getProjectCommandHistory()->redo();
    EXPECT_EQ(project.getDebugSettings().allowLua, DebugSettings::AllowLua::AllBuilds);
}
