// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime's reader of a Title Art bake sidecar (docs/specs/S81-title-art-runtime.md R8, R36):
// what S80's bake writes, lenient like S80's codec, anchors strongest first and at most 16.

#include <gtest/gtest.h>

#include "util/TitleArtBakeInfo.h"

#include <string>

using namespace jasperengine;

TEST(TitleArtBakeInfo, ReadsWhatTheBakeWrites) {
    // S80's TitleBakeInfo::toText form
    const std::string text = R"({
  "format": "jasperengine.titleart.bakeinfo",
  "version": 1,
  "frame": {"x": 200, "y": 100, "w": 400, "h": 200},
  "origin": [100, 150],
  "outputs": [
    {"id": "main", "file": "logo.titleart.png", "content": "all", "size": [800, 400]},
    {"id": "glow", "file": "logo.titleart.glow.png", "content": "glow", "size": [400, 200]}
  ],
  "alpha_bounds": {"x": 210, "y": 110, "w": 380, "h": 180},
  "source_hash": "0123456789abcdef",
  "bake_version": 7,
  "anchors": [{"x": 0.2, "y": 0.4, "size": 1, "strength": 0.5, "fade": 1}, {"x": 0.7, "y": 0.3, "size": 2, "strength": 1, "fade": 1}],
  "language": "de"
})";
    TitleArtBakeInfo info;
    std::string error;
    ASSERT_TRUE(TitleArtBakeInfo::parse(text, info, error)) << error;
    EXPECT_EQ(info.frame.x, 200);
    EXPECT_EQ(info.frame.width, 400);
    EXPECT_EQ(info.frame.height, 200);
    EXPECT_FLOAT_EQ(info.pivotX(), 0.25f);
    EXPECT_FLOAT_EQ(info.pivotY(), 0.75f) << "from the top-left, RectTransform's convention";
    ASSERT_EQ(info.outputs.size(), 2u);
    EXPECT_EQ(info.firstOutput("glow")->file, "logo.titleart.glow.png");
    EXPECT_EQ(info.output("main")->width, 800);
    EXPECT_EQ(info.alphaBounds.width, 380);
    EXPECT_EQ(info.sourceHash, "0123456789abcdef");
    EXPECT_EQ(info.bakeVersion, 7u);
    EXPECT_EQ(info.language, "de");
    ASSERT_EQ(info.anchors.size(), 2u);
    EXPECT_FLOAT_EQ(info.anchors[0].x, 0.7f) << "the strongest anchor first";
    EXPECT_EQ(titleArtSidecarPath("assets/titles/logo.titleart"), "assets/titles/logo.titleart.bake.json");
    EXPECT_EQ(titleArtSiblingPath("assets/titles/logo.titleart", "logo.titleart.png"), "assets/titles/logo.titleart.png");
    EXPECT_EQ(titleArtDefaultBakeFile("assets/titles/logo.titleart"), "logo.titleart.png");
    EXPECT_EQ(titleArtPrebakePath("assets/t.titleart", 0xabcull), "assets/t.titleart.prebake-0000000000000abc.png");
}

TEST(TitleArtBakeInfo, IsLenientAndRefusesWhatIsNotASidecar) {
    TitleArtBakeInfo info;
    std::string error;
    EXPECT_FALSE(TitleArtBakeInfo::parse("[1, 2]", info, error));
    EXPECT_NE(error.find("not a Title Art bake sidecar"), std::string::npos);
    EXPECT_FALSE(TitleArtBakeInfo::parse(R"({"format": "jasperengine.titleart.bakeinfo", "version": 9})", info, error));
    EXPECT_NE(error.find("newer editor"), std::string::npos) << error;
    // wrong types take their defaults, unknown members are ignored, too many anchors are cut
    std::string anchors;
    for (int i = 0; i < 20; i++) anchors += std::string(i ? "," : "") + R"({"x": 0.5, "y": 0.5, "strength": )" + std::to_string(i / 20.0) + "}";
    ASSERT_TRUE(TitleArtBakeInfo::parse(R"({"format": "jasperengine.titleart.bakeinfo", "version": 1, "frame": "wide", "origin": "x",
                                         "bake_version": "seven", "extra": {"a": 1}, "anchors": [)" + anchors + "]}", info, error)) << error;
    EXPECT_TRUE(info.frame.empty());
    EXPECT_EQ(info.bakeVersion, 0u);
    EXPECT_EQ(info.anchors.size(), TITLE_ART_MAX_ANCHORS);
    EXPECT_GE(info.anchors.front().strength, info.anchors.back().strength);
}
