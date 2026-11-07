// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// McpValues::enumTable names the tools look up by name (SYNC#4): edit_colliders' 3D body reads its
// interpolation through enumTable("rigidbody_interpolation") and dereferences the table; the S08
// table map had dropped upstream's physics-3d entries, so every edit_colliders call on a 3D body
// dereferenced null and the editor died (SIGSEGV in the MCP suite's physics, water, buoyancy,
// character controller and jobs tests).

#include <gtest/gtest.h>

#include "mcp/McpValues.h"
#include "component/RigidbodyComponent.h"
#include "SceneSettings.h"

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor::mcp;

// the expression of edit_colliders' body summary (WorldTools.cpp, bodyJson)
TEST(McpEnumTables, EditCollidersBodyInterpolationReadsByName) {
    RigidbodyComponent body;
    body.interpolation = RigidbodyInterpolation::INTERPOLATE;
    const jasperengine::editor::mcp::Json name = McpValues::enumToJson((int)body.interpolation, *McpValues::enumTable("rigidbody_interpolation"));
    EXPECT_EQ(name, jasperengine::editor::mcp::Json("interpolate"));
}

// every table a tool dereferences by name exists (the physics-3d ones upstream's tools use too)
TEST(McpEnumTables, EveryNamedTableExists) {
    for (const char* name : {"framebuffer_filter", "light_state", "light_unit", "lit_quality", "material_blend_mode",
                             "material_cull_mode", "material_depth_test", "material_depth_write", "material_lighting",
                             "material_pass_order", "material_stencil_compare", "material_stencil_op", "scene_color_scale",
                             "shadow_debug_view", "shadow_quality", "rigidbody_interpolation", "joint_motion", "swing_type",
                             "body_type", "collider3d_type", "water_wake_resolution"}) {
        const McpValues::EnumOptions* table = McpValues::enumTable(name);
        ASSERT_NE(table, nullptr) << name;
        EXPECT_FALSE(table->empty()) << name;
    }
}

// set_scene_settings' Water Wakes resolution (SceneTools.cpp, SettingKind::WaterWakeResolution)
TEST(McpEnumTables, WaterWakeResolutionNames) {
    const McpValues::EnumOptions* table = McpValues::enumTable("water_wake_resolution");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(McpValues::enumToJson((int)WaterWakeResolution::AUTO, *table), jasperengine::editor::mcp::Json("auto"));
    EXPECT_EQ(McpValues::enumToJson((int)WaterWakeResolution::HIGH, *table), jasperengine::editor::mcp::Json("high"));
    EXPECT_EQ(table->size(), 4u);
}
