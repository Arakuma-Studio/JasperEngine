// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Window layouts, the pure part (S25 R15-R19, AC13): document windows are filtered out, a
// preset round-trips through YAML, imports get unique names, and bundles hold several.

#include "layout/LayoutPresets.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

namespace {

    const char* const SAMPLE_INI =
        "[Window][Inspector]\r\n"
        "Pos=900,40\r\n"
        "Size=300,700\r\n"
        "DockId=0x00000002,0\r\n"
        "\r\n"
        "[Window][###Scene3]\r\n"
        "Pos=200,40\r\n"
        "DockId=0x00000001,1\r\n"
        "\r\n"
        "[Window][###scripts/player.lua]\r\n"
        "Pos=200,40\r\n"
        "\r\n"
        "[Window][###ImageViewer:textures/hero.png]\r\n"
        "Size=400,400\r\n"
        "\r\n"
        "[Window][Hierarchy]\r\n"
        "Pos=0,40\r\n"
        "DockId=0x00000003,0\r\n"
        "\r\n"
        "[Docking][Data]\r\n"
        "DockSpace ID=0x8B93E3BD Window=0xA787BDB4 Pos=0,40 Size=1200,700 Split=X\r\n"
        "  DockNode ID=0x00000001 Parent=0x8B93E3BD SizeRef=900,700 CentralNode=1\r\n"
        "\r\n\r\n";

}

TEST(LayoutPresets, FilterRoundTripUnique) {
    const std::string filtered = LayoutPresets::filterDocumentWindows(SAMPLE_INI);
    EXPECT_EQ(filtered.find("###"), std::string::npos) << filtered;
    EXPECT_EQ(filtered.find('\r'), std::string::npos);
    EXPECT_NE(filtered.find("[Window][Inspector]\nPos=900,40\nSize=300,700\nDockId=0x00000002,0\n"), std::string::npos) << filtered;
    EXPECT_NE(filtered.find("[Window][Hierarchy]"), std::string::npos);
    EXPECT_NE(filtered.find("[Docking][Data]\nDockSpace ID=0x8B93E3BD"), std::string::npos);
    EXPECT_FALSE(filtered.ends_with("\n\n\n"));
    // filtering is idempotent
    EXPECT_EQ(LayoutPresets::filterDocumentWindows(filtered), filtered);

    LayoutPreset preset;
    preset.name = "Animation: wide";
    preset.uiScale = 1.5f;
    preset.panels = {{"Hierarchy", true}, {"Inspector", true}, {"Animation", true}, {"Console", false}};
    preset.ini = filtered;
    const std::string text = LayoutPresets::serialize(preset, false);
    LayoutPreset back;
    std::string error;
    ASSERT_TRUE(LayoutPresets::parse(text, back, &error)) << error;
    EXPECT_EQ(back.name, preset.name);
    EXPECT_FLOAT_EQ(back.uiScale, 1.5f);
    EXPECT_EQ(back.panels, preset.panels);
    EXPECT_EQ(back.ini, preset.ini);
    EXPECT_TRUE(back.activePreset.empty());
    // the working layout also names its preset
    preset.activePreset = "Animation: wide";
    ASSERT_TRUE(LayoutPresets::parse(LayoutPresets::serialize(preset, true), back));
    EXPECT_EQ(back.activePreset, "Animation: wide");
    // a document window in a hand-edited file is filtered on load
    ASSERT_TRUE(LayoutPresets::parse("layoutVersion: 1\nname: X\nini: |\n  [Window][###Scene1]\n  Pos=1,1\n  [Window][Game]\n  Pos=2,2\n", back));
    EXPECT_EQ(back.ini, "[Window][Game]\nPos=2,2\n");

    // importing "Animation" twice
    std::vector<std::string> existing = {"Default", "Animation"};
    EXPECT_EQ(LayoutPresets::uniqueName("Animation", existing), "Animation (2)");
    existing.push_back("animation (2)");
    EXPECT_EQ(LayoutPresets::uniqueName("Animation", existing), "Animation (3)");
    EXPECT_EQ(LayoutPresets::uniqueName("Tall", existing), "Tall");
    EXPECT_EQ(LayoutPresets::uniqueName("  ", existing), "Layout");

    // bundles
    LayoutPreset other = preset;
    other.name = "Tall";
    other.activePreset.clear();
    std::vector<LayoutPreset> presets;
    ASSERT_TRUE(LayoutPresets::parseFile(LayoutPresets::serializeBundle({preset, other}), presets, &error)) << error;
    ASSERT_EQ(presets.size(), 2u);
    EXPECT_EQ(presets[0].name, "Animation: wide");
    EXPECT_EQ(presets[1].name, "Tall");
    EXPECT_TRUE(presets[0].activePreset.empty());   // a bundle holds presets, not working layouts
    ASSERT_TRUE(LayoutPresets::parseFile(LayoutPresets::serialize(other, false), presets));
    ASSERT_EQ(presets.size(), 1u);
}

TEST(LayoutPresets, RejectsCorruptAndNewerFiles) {
    LayoutPreset preset;
    std::string error;
    EXPECT_FALSE(LayoutPresets::parse("name: [unclosed", preset, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(LayoutPresets::parse("name: No Ini\n", preset, &error));
    EXPECT_FALSE(LayoutPresets::parse("layoutVersion: 99\nname: X\nini: \"\"\n", preset, &error));
    EXPECT_NE(error.find("newer"), std::string::npos);
    std::vector<LayoutPreset> presets;
    EXPECT_FALSE(LayoutPresets::parseFile("layoutBundleVersion: 1\npresets: 3\n", presets, &error));
    EXPECT_FALSE(LayoutPresets::parseFile("- a\n- b\n", presets, &error));
    // a bad UI scale falls back to 1
    ASSERT_TRUE(LayoutPresets::parse("name: X\nuiScale: 100\nini: \"\"\n", preset, &error)) << error;
    EXPECT_FLOAT_EQ(preset.uiScale, 1.0f);
}

TEST(LayoutPresets, NamesAndSlugs) {
    EXPECT_TRUE(LayoutPresets::isValidName("Animation"));
    EXPECT_FALSE(LayoutPresets::isValidName(""));
    EXPECT_FALSE(LayoutPresets::isValidName("   "));
    EXPECT_FALSE(LayoutPresets::isValidName(std::string(65, 'a')));
    EXPECT_FALSE(LayoutPresets::isValidName("bad\tname"));
    std::string why;
    EXPECT_FALSE(LayoutPresets::isValidName("default", &why));
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(LayoutPresets::slug("My Layout"), "my-layout");
    EXPECT_EQ(LayoutPresets::slug("  Animation: wide (2) "), "animation-wide-2");
    EXPECT_EQ(LayoutPresets::slug("***"), "layout");
}
