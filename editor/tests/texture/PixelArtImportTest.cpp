// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S42 AC1 (R4, R7, R8): the Pixel Art switch of a texture's import settings, the Filter Mode and
// Pixels Per Unit an assignment copies, and the cook keys of every texture without the new
// fields staying what they were.

#include <gtest/gtest.h>

#include "render/Render.h"
#include "texture/TextureImportSettings.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(PixelArtImport, ResolvesToThePixelArtDefaultsOnEveryFamily){
    TextureImportSettings settings;
    settings.pixelArt = true;
    const TextureProjectSettings project;
    for (size_t f = 0; f < kTextureFamilyCount; f++){
        const ResolvedTextureSettings r = settings.resolve((TextureFamily)f, project);
        SCOPED_TRACE(textureFamilyName((TextureFamily)f));
        EXPECT_EQ(r.type, TextureImportType::Sprite);
        EXPECT_FALSE(r.mipmaps);
        EXPECT_EQ(r.mipFilter, MipFilter::Nearest);
        EXPECT_EQ(r.resizeFilter, MipFilter::Nearest);
        EXPECT_EQ(r.quality, TextureCookQuality::None);
        EXPECT_TRUE(r.alphaIsTransparency);
        EXPECT_TRUE(r.alphaClip);
        EXPECT_FLOAT_EQ(r.alphaCutoff, 0.5f);
        EXPECT_FALSE(r.streamable);
        EXPECT_TRUE(r.processing().alphaClip);
    }
    EXPECT_EQ(settings.resolvedFilterMode(TextureFilterMode::Bilinear), TextureFilterMode::Point);
    EXPECT_EQ(settings.resolvedPixelsPerUnit(16), 16);
    settings.pixelsPerUnit = 32;
    EXPECT_EQ(settings.resolvedPixelsPerUnit(16), 32);

    // without Pixel Art the project's Default Filter Mode is what an assignment copies
    TextureImportSettings plain;
    EXPECT_EQ(plain.resolvedFilterMode(TextureFilterMode::Bilinear), TextureFilterMode::Bilinear);
    EXPECT_EQ(plain.resolvedFilterMode(TextureFilterMode::Point), TextureFilterMode::Point);
    TextureFilter minFilter, magFilter;
    textureFilterModeFilters(TextureFilterMode::Point, minFilter, magFilter);
    EXPECT_EQ(minFilter, TextureFilter::NEAREST);
    EXPECT_EQ(magFilter, TextureFilter::NEAREST);
    textureFilterModeFilters(TextureFilterMode::Trilinear, minFilter, magFilter);
    EXPECT_EQ(minFilter, TextureFilter::LINEAR_MIPMAP_LINEAR);
    EXPECT_EQ(magFilter, TextureFilter::LINEAR);
}

TEST(PixelArtImport, ExplicitFieldsAndPlatformOverridesWin){
    TextureImportSettings settings;
    settings.pixelArt = true;
    settings.mipmaps = true;
    settings.type = TextureImportType::Default;
    TexturePlatformSettings& mobile = settings.platforms[(size_t)TextureFamily::Mobile];
    mobile.override = true;
    mobile.format = "ASTC_6x6";
    const TextureProjectSettings project;
    const ResolvedTextureSettings desktop = settings.resolve(TextureFamily::Desktop, project);
    EXPECT_TRUE(desktop.mipmaps) << "an explicit mipmaps: true wins";
    EXPECT_EQ(desktop.type, TextureImportType::Default) << "a written type wins";
    EXPECT_EQ(desktop.quality, TextureCookQuality::None);
    EXPECT_TRUE(desktop.format.empty());
    const ResolvedTextureSettings r = settings.resolve(TextureFamily::Mobile, project);
    EXPECT_EQ(r.format, "ASTC_6x6") << "a mobile override to ASTC wins";
    settings.alphaClip = false;
    EXPECT_FALSE(settings.resolve(TextureFamily::Web, project).alphaClip);
}

TEST(PixelArtImport, NewKeysRoundTrip){
    const YAML::Node node = YAML::Load("version: 1\ntype: sprite\npixelArt: true\nalphaClip: false\nfilterMode: bilinear\n"
                                       "pixelsPerUnit: 32\nresizeFilter: nearest\nmipFilter: nearest\n");
    TextureImportSettings settings;
    std::string error;
    ASSERT_TRUE(TextureImportSettings::parse(node, settings, &error)) << error;
    EXPECT_EQ(settings.pixelArt, true);
    EXPECT_EQ(settings.alphaClip, false);
    EXPECT_EQ(settings.filterMode, TextureFilterMode::Bilinear);
    EXPECT_EQ(settings.pixelsPerUnit, 32);
    EXPECT_EQ(settings.resizeFilter, MipFilter::Nearest);
    TextureImportSettings again;
    ASSERT_TRUE(TextureImportSettings::parse(settings.serialize(), again, &error)) << error;
    EXPECT_TRUE(again == settings);
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("filterMode: smooth"), settings, &error));
    EXPECT_NE(error.find("point"), std::string::npos) << error;
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("pixelsPerUnit: 0"), settings, &error));
}

// R8: a texture without the new fields keeps its cache key, so no project recooks on upgrade
TEST(PixelArtImport, CookKeysOfOlderSettingsAreUnchanged){
    const TextureProjectSettings project;
    // the key of the default texture as S14 printed it before S42
    const std::string before =
        "{\"type\":\"default\",\"shape\":\"2d\",\"srgb\":1,\"alphaSource\":\"input\",\"alphaIsTransparency\":0,\"readable\":0,"
        "\"maxSize\":2048,\"resizeFilter\":\"mitchell\",\"mipmaps\":1,\"mipFilter\":\"mitchell\",\"mipLimit\":0,"
        "\"preserveCoverage\":0,\"alphaCutoff\":0.5,\"flipGreen\":0,\"channel\":\"r\",\"quality\":\"normal\",\"effort\":\"normal\","
        "\"streamable\":1,\"format\":\"\",\"rdo\":-1,\"cpuReadable\":0,\"strategy\":0}";
    EXPECT_EQ(TextureImportSettings().resolve(TextureFamily::Desktop, project).canonical(), before);
    // every type and family of the S14 corpus: nothing S42 added appears in their keys
    for (TextureImportType type : {TextureImportType::Default, TextureImportType::Normal, TextureImportType::Sprite, TextureImportType::UI,
                                   TextureImportType::Lightmap, TextureImportType::SingleChannel, TextureImportType::HDR}){
        for (size_t f = 0; f < kTextureFamilyCount; f++){
            TextureImportSettings settings;
            settings.type = type;
            const std::string key = settings.resolve((TextureFamily)f, project).canonical();
            EXPECT_EQ(key.find("alphaClip"), std::string::npos) << key;
            EXPECT_EQ(key.find("nearest"), std::string::npos) << key;
            EXPECT_EQ(key.substr(key.size() - 13), "\"strategy\":0}") << key;
        }
    }
    // Pixel Art's own keys differ (they cook differently)
    TextureImportSettings pixel;
    pixel.pixelArt = true;
    const std::string key = pixel.resolve(TextureFamily::Desktop, project).canonical();
    EXPECT_NE(key.find("\"alphaClip\":1"), std::string::npos) << key;
    EXPECT_NE(key.find("\"resizeFilter\":\"nearest\""), std::string::npos) << key;
}
