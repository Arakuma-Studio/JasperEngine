// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R8, R19, R25, R29: the sidecar's modelImporter section (sparse, versioned, unknown
// keys kept), its resolution per platform and project.yaml's mesh block.

#include <gtest/gtest.h>

#include "model/ModelImportSettings.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ModelImportSettings, AbsentMeansTheUnityDefaults){
    const ModelImportSettings settings;
    const ModelProjectSettings project;
    for (ModelFamily family : {ModelFamily::Desktop, ModelFamily::Mobile, ModelFamily::Web}){
        const ResolvedModelSettings r = settings.resolve(family, project);
        EXPECT_EQ(r.optimizeMesh, OptimizeMesh::Everything);
        EXPECT_EQ(r.meshCompression, MeshCompression::Lossless);
        EXPECT_TRUE(r.weldVertices);
        EXPECT_EQ(r.indexFormat, IndexFormat::Auto);
        EXPECT_EQ(r.readWrite, ModelReadWrite::Automatic);
        EXPECT_EQ(r.normals, ModelNormals::Import);
        EXPECT_FLOAT_EQ(r.smoothingAngle, 60.0f);
        EXPECT_EQ(r.generateLods, GenerateLods::Automatic);
        EXPECT_EQ(r.lodCount, 3);
        EXPECT_EQ(r.lodReduction, LodReduction::Balanced);
        EXPECT_FALSE(r.shipSource);
    }
    EXPECT_TRUE(settings.isDefault());
    // a sparse section: only the version
    EXPECT_EQ(YAML::Dump(settings.serialize()), "version: 1");
}

TEST(ModelImportSettings, OwnValueThenProjectDefaultThenOverride){
    ModelImportSettings settings;
    ModelProjectSettings project;
    project.defaults[(size_t)ModelFamily::Mobile].meshCompression = MeshCompression::Medium;
    project.defaults[(size_t)ModelFamily::Mobile].lodCount = 2;
    EXPECT_EQ(settings.resolve(ModelFamily::Mobile, project).meshCompression, MeshCompression::Medium);
    EXPECT_EQ(settings.resolve(ModelFamily::Desktop, project).meshCompression, MeshCompression::Lossless);
    settings.values.meshCompression = MeshCompression::Off;
    EXPECT_EQ(settings.resolve(ModelFamily::Mobile, project).meshCompression, MeshCompression::Off);
    EXPECT_EQ(settings.resolve(ModelFamily::Mobile, project).lodCount, 2);
    ModelPlatformSettings& mobile = settings.platforms[(size_t)ModelFamily::Mobile];
    mobile.values.meshCompression = MeshCompression::High;
    EXPECT_EQ(settings.resolve(ModelFamily::Mobile, project).meshCompression, MeshCompression::Off) << "without Override the block waits";
    mobile.override = true;
    EXPECT_EQ(settings.resolve(ModelFamily::Mobile, project).meshCompression, MeshCompression::High);
    EXPECT_EQ(settings.resolve(ModelFamily::Web, project).meshCompression, MeshCompression::Off);
}

TEST(ModelImportSettings, RoundTripKeepsUnknownKeysAndPlatformBlocks){
    const YAML::Node node = YAML::Load(
        "version: 1\n"
        "optimizeMesh: polygon_order\n"
        "meshCompression: medium\n"
        "weldVertices: false\n"
        "indexFormat: u32\n"
        "readWrite: on\n"
        "normals: calculate\n"
        "smoothingAngle: 30\n"
        "generateLods: on\n"
        "lodCount: 2\n"
        "lodReduction: smallest\n"
        "shipSource: true\n"
        "futureKnob: {a: 1}\n"
        "platforms:\n"
        "  web: {override: true, generateLods: off, meshCompression: high}\n");
    ModelImportSettings settings;
    std::string error;
    ASSERT_TRUE(ModelImportSettings::parse(node, settings, &error)) << error;
    EXPECT_EQ(settings.values.optimizeMesh, OptimizeMesh::PolygonOrder);
    EXPECT_EQ(settings.values.readWrite, ModelReadWrite::On);
    EXPECT_EQ(settings.values.generateLods, GenerateLods::On);
    EXPECT_EQ(settings.values.indexFormat, IndexFormat::Bits32);
    EXPECT_EQ(settings.platforms[(size_t)ModelFamily::Web].values.generateLods, GenerateLods::Off);
    EXPECT_TRUE(settings.platforms[(size_t)ModelFamily::Web].override);
    ASSERT_TRUE(settings.unknown["futureKnob"]);
    // what is written reads back as the same settings, the unknown key included
    ModelImportSettings again;
    ASSERT_TRUE(ModelImportSettings::parse(YAML::Load(YAML::Dump(settings.serialize())), again, &error)) << error;
    EXPECT_TRUE(again == settings);
    EXPECT_EQ(again.unknown["futureKnob"]["a"].as<int>(), 1);
    const ResolvedModelSettings web = settings.resolve(ModelFamily::Web, ModelProjectSettings());
    EXPECT_EQ(web.generateLods, GenerateLods::Off);
    EXPECT_EQ(web.meshCompression, MeshCompression::High);
    EXPECT_FLOAT_EQ(web.smoothingAngle, 30.0f);
}

TEST(ModelImportSettings, MalformedValuesNameTheFieldAndTheChoices){
    ModelImportSettings settings;
    std::string error;
    EXPECT_FALSE(ModelImportSettings::parse(YAML::Load("meshCompression: extreme"), settings, &error));
    EXPECT_NE(error.find("meshCompression"), std::string::npos);
    EXPECT_NE(error.find("off, lossless, low, medium, high"), std::string::npos) << error;
    error.clear();
    EXPECT_FALSE(ModelImportSettings::parse(YAML::Load("lodCount: 5"), settings, &error));
    EXPECT_NE(error.find("1...3"), std::string::npos) << error;
    error.clear();
    EXPECT_FALSE(ModelImportSettings::parse(YAML::Load("platforms: {android_etc2: {override: true}}"), settings, &error));
    EXPECT_NE(error.find("desktop, mobile, web"), std::string::npos) << error;
    error.clear();
    EXPECT_FALSE(validateModelSettingsField("lodCounts", YAML::Node(2), error));
    EXPECT_NE(error.find("lodCount"), std::string::npos) << "the valid keys are listed: " << error;
    error.clear();
    EXPECT_TRUE(validateModelSettingsField("smoothingAngle", YAML::Node(45), error)) << error;
    EXPECT_FALSE(validateModelSettingsField("smoothingAngle", YAML::Node(200), error));
}

TEST(ModelImportSettings, TheKeyFollowsWhatTheCookReads){
    ResolvedModelSettings a, b;
    EXPECT_EQ(a.canonical(), b.canonical());
    // Read/Write and Ship Source File are export decisions, not cook inputs
    b.readWrite = ModelReadWrite::On;
    b.shipSource = true;
    EXPECT_EQ(a.canonical(), b.canonical());
    // Automatic bakes the runtime's levels: LOD Count and Reduction do not reach its key
    b.lodCount = 1;
    EXPECT_EQ(a.canonical(), b.canonical());
    b.generateLods = GenerateLods::On;
    EXPECT_NE(a.canonical(), b.canonical());
    b = a;
    b.meshCompression = MeshCompression::Low;
    EXPECT_NE(a.canonical(), b.canonical());
}

TEST(ModelImportSettings, ProjectSettingsWriteOnlyWhatDiffers){
    ModelProjectSettings settings;
    EXPECT_EQ(encodeModelProjectSettings(settings).size(), 0u);
    settings.defaults[(size_t)ModelFamily::Web].meshCompression = MeshCompression::Medium;
    settings.cacheSizeMB = 512;
    settings.scriptMeshData = ScriptMeshData::Keep;
    settings.renderBudgets[(size_t)ModelFamily::Mobile].triangles = 150000;
    settings.renderBudgets[(size_t)ModelFamily::Mobile].downloadMB = 40.5;
    const YAML::Node node = encodeModelProjectSettings(settings);
    ModelProjectSettings decoded;
    decodeModelProjectSettings(YAML::Load(YAML::Dump(node)), decoded);
    EXPECT_TRUE(decoded == settings);
    // bad values are skipped one by one, out-of-range sizes clamped
    decodeModelProjectSettings(YAML::Load("defaults: {mobile: {lodCount: 9, meshCompression: high}}\ncacheSizeMB: 1"), decoded);
    EXPECT_FALSE(decoded.defaults[(size_t)ModelFamily::Mobile].lodCount);
    EXPECT_EQ(decoded.defaults[(size_t)ModelFamily::Mobile].meshCompression, MeshCompression::High);
    EXPECT_EQ(decoded.cacheSizeMB, 64);
}
