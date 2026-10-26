// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 R1, R24, R30: the sidecar's textureImporter section and project.yaml's texture block.

#include <gtest/gtest.h>

#include "texture/TextureImportSettings.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(TextureImportSettings, AbsentFieldsResolveToTheTypeDefaults){
    TextureImportSettings settings;
    const TextureProjectSettings project;
    ResolvedTextureSettings r = settings.resolve(TextureFamily::Desktop, project);
    EXPECT_EQ(r.type, TextureImportType::Default);
    EXPECT_TRUE(r.srgb);
    EXPECT_TRUE(r.mipmaps);
    EXPECT_EQ(r.maxSize, 2048);
    EXPECT_EQ(r.mipFilter, MipFilter::Mitchell);
    EXPECT_FLOAT_EQ(r.alphaCutoff, 0.5f);
    EXPECT_EQ(r.quality, TextureCookQuality::Normal);
    EXPECT_TRUE(r.streamable);

    settings.type = TextureImportType::Normal;
    r = settings.resolve(TextureFamily::Desktop, project);
    EXPECT_FALSE(r.srgb);
    EXPECT_TRUE(r.mipmaps);

    // sprites and UI: no mips, alpha bleeding, never streamable (R4, R12)
    for (TextureImportType type : {TextureImportType::Sprite, TextureImportType::UI}){
        settings.type = type;
        r = settings.resolve(TextureFamily::Mobile, project);
        EXPECT_TRUE(r.srgb);
        EXPECT_FALSE(r.mipmaps);
        EXPECT_TRUE(r.alphaIsTransparency);
        EXPECT_FALSE(r.streamable);
    }
    settings.type = TextureImportType::SingleChannel;
    EXPECT_FALSE(settings.resolve(TextureFamily::Web, project).srgb);
    settings.type = TextureImportType::HDR;
    EXPECT_FALSE(settings.resolve(TextureFamily::Web, project).srgb);
}

TEST(TextureImportSettings, PlatformOverridesApplyOnlyWhenOn){
    TextureImportSettings settings;
    settings.maxSize = 1024;
    settings.quality = TextureCookQuality::High;
    TexturePlatformSettings& mobile = settings.platforms[(size_t)TextureFamily::Mobile];
    mobile.maxSize = 256;
    mobile.quality = TextureCookQuality::Low;
    mobile.format = "ASTC_8x8";
    const TextureProjectSettings project;
    ResolvedTextureSettings r = settings.resolve(TextureFamily::Mobile, project);
    EXPECT_EQ(r.maxSize, 1024);
    EXPECT_EQ(r.quality, TextureCookQuality::High);
    EXPECT_TRUE(r.format.empty());
    mobile.override = true;
    r = settings.resolve(TextureFamily::Mobile, project);
    EXPECT_EQ(r.maxSize, 256);
    EXPECT_EQ(r.quality, TextureCookQuality::Low);
    EXPECT_EQ(r.format, "ASTC_8x8");
    // the other families keep the texture's own values
    r = settings.resolve(TextureFamily::Desktop, project);
    EXPECT_EQ(r.maxSize, 1024);
    EXPECT_TRUE(r.format.empty());
}

TEST(TextureImportSettings, ProjectDefaultsFillTheGaps){
    TextureProjectSettings project;
    project.defaultQuality[(size_t)TextureFamily::Web] = TextureCookQuality::Low;
    project.defaultMaxSize[(size_t)TextureFamily::Web] = 512;
    TextureImportSettings settings;
    ResolvedTextureSettings r = settings.resolve(TextureFamily::Web, project);
    EXPECT_EQ(r.quality, TextureCookQuality::Low);
    EXPECT_EQ(r.maxSize, 512);
    settings.maxSize = 4096;
    EXPECT_EQ(settings.resolve(TextureFamily::Web, project).maxSize, 4096);
    EXPECT_EQ(settings.resolve(TextureFamily::Desktop, project).quality, TextureCookQuality::Normal);
}

TEST(TextureImportSettings, RoundTripsAndKeepsUnknownKeys){
    const YAML::Node node = YAML::Load(
        "version: 1\n"
        "type: normal\n"
        "inferredFrom: \"normal slot: MeshComponent.submeshes[0].material.normalTexture of scenes/a.scene\"\n"
        "flipGreen: true\n"
        "maxSize: 1024\n"
        "mipFilter: catmull_rom\n"
        "futureField: {a: 1}\n"
        "platforms:\n"
        "  web: {override: true, quality: high, rdo: 0.5}\n");
    TextureImportSettings settings;
    std::string error;
    ASSERT_TRUE(TextureImportSettings::parse(node, settings, &error)) << error;
    EXPECT_EQ(settings.type, TextureImportType::Normal);
    EXPECT_EQ(settings.flipGreen, true);
    EXPECT_EQ(settings.maxSize, 1024);
    EXPECT_EQ(settings.mipFilter, MipFilter::CatmullRom);
    EXPECT_TRUE(settings.platforms[(size_t)TextureFamily::Web].override);
    EXPECT_FLOAT_EQ(*settings.platforms[(size_t)TextureFamily::Web].rdo, 0.5f);
    const YAML::Node written = settings.serialize();
    ASSERT_TRUE(written["futureField"]);
    EXPECT_EQ(written["futureField"]["a"].as<int>(), 1);
    TextureImportSettings again;
    ASSERT_TRUE(TextureImportSettings::parse(written, again, &error)) << error;
    EXPECT_TRUE(again == settings);
    // what is absent stays absent
    EXPECT_FALSE(written["srgb"]);
    EXPECT_FALSE(written["quality"]);
}

TEST(TextureImportSettings, InvalidValuesNameWhatIsAllowed){
    TextureImportSettings settings;
    std::string error;
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("type: bump"), settings, &error));
    EXPECT_NE(error.find("type"), std::string::npos);
    EXPECT_NE(error.find("normal"), std::string::npos) << error;
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("maxSize: 20"), settings, &error));
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("platforms: {console: {override: true}}"), settings, &error));
    EXPECT_NE(error.find("android_etc2"), std::string::npos) << error;
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("alphaCutoff: 2"), settings, &error));
    EXPECT_FALSE(TextureImportSettings::parse(YAML::Load("[1, 2]"), settings, &error));

    EXPECT_TRUE(validateTextureSettingsField("maxSize", YAML::Node(512), error));
    EXPECT_FALSE(validateTextureSettingsField("maxSize", YAML::Node(500), error));
    EXPECT_NE(error.find("power of two"), std::string::npos);
    EXPECT_FALSE(validateTextureSettingsField("version", YAML::Node(2), error));
    EXPECT_FALSE(validateTextureSettingsField("colour", YAML::Node(true), error));
    EXPECT_TRUE(validateTextureSettingsField("quality", YAML::Node("low"), error));
    EXPECT_FALSE(validateTextureSettingsField("effort", YAML::Node("extreme"), error));
}

TEST(TextureImportSettings, CanonicalFormChangesWithEveryField){
    const TextureProjectSettings project;
    TextureImportSettings base;
    const std::string reference = base.resolve(TextureFamily::Desktop, project).canonical();
    std::vector<TextureImportSettings> edits(16, base);
    edits[0].type = TextureImportType::Sprite;
    edits[1].srgb = false;
    edits[2].alphaSource = TextureAlphaSource::None;
    edits[3].alphaIsTransparency = true;
    edits[4].readable = true;
    edits[5].maxSize = 1024;
    edits[6].resizeFilter = MipFilter::Box;
    edits[7].mipmaps = false;
    edits[8].mipFilter = MipFilter::Triangle;
    edits[9].mipLimit = 3;
    edits[10].preserveCoverage = true;
    edits[11].alphaCutoff = 0.3f;
    edits[12].flipGreen = true;
    edits[13].quality = TextureCookQuality::High;
    edits[14].effort = TextureEffort::Best;
    edits[15].streamable = false;
    for (size_t i = 0; i < edits.size(); i++)
        EXPECT_NE(edits[i].resolve(TextureFamily::Desktop, project).canonical(), reference) << "edit " << i;
    // and the same settings give the same text
    EXPECT_EQ(base.resolve(TextureFamily::Desktop, project).canonical(), reference);
}

TEST(TextureImportSettings, ProjectSettingsWriteOnlyWhatDiffers){
    TextureProjectSettings settings;
    EXPECT_EQ(encodeTextureProjectSettings(settings).size(), 0u);
    settings.androidEtc2Fallback = true;
    settings.defaultQuality[(size_t)TextureFamily::Mobile] = TextureCookQuality::High;
    settings.memoryBudgetMB[2] = 256;
    settings.lowMemoryFallback[0] = true;
    settings.textureQuality[1] = 2;
    settings.cacheSizeMB = 1024;
    settings.globalCacheDir = "/tmp/cache";
    const YAML::Node node = encodeTextureProjectSettings(settings);
    ASSERT_GT(node.size(), 0u);
    EXPECT_EQ(node["defaults"]["mobile"]["quality"].as<std::string>(), "high");
    EXPECT_FALSE(node["defaults"]["desktop"]);
    EXPECT_EQ(node["platforms"]["web"]["memoryBudgetMB"].as<int>(), 256);
    TextureProjectSettings decoded;
    decodeTextureProjectSettings(YAML::Load(YAML::Dump(node)), decoded);
    EXPECT_TRUE(decoded == settings);
    // values out of range clamp, nonsense is ignored
    decodeTextureProjectSettings(YAML::Load("platforms: {mobile: {textureQuality: 9}}\ndefaults: {web: {maxSize: 300, quality: ultra}}"), decoded);
    EXPECT_EQ(decoded.textureQuality[1], 3);
    EXPECT_EQ(decoded.defaultMaxSize[(size_t)TextureFamily::Web], 2048);
    EXPECT_EQ(decoded.defaultQuality[(size_t)TextureFamily::Web], TextureCookQuality::Normal);
}
