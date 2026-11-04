// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// unity/UnityYaml and unity/UnityProject (particles spec 6.10.2-3): Unity's multi-document YAML
// split by its `--- !u!<class> &<id> [stripped]` headers, scalars typed by the reader (the
// m_VertexStreams octal trap stays a string), references, the built-in asset table and the
// guid index of a project's .meta files. Synthetic inputs only.

#include <gtest/gtest.h>
#include "Project.h"

#include "unity/UnityProject.h"
#include "unity/UnityYaml.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor::unity;
namespace fs = std::filesystem;

namespace {

    const char* const FILE_TEXT =
        "%YAML 1.1\n"
        "%TAG !u! tag:unity3d.com,2011:\n"
        "--- !u!1 &3192825672447820913\n"
        "GameObject:\n"
        "  m_Name: Sparks\n"
        "  m_IsActive: 1\n"
        "--- !u!199 &-8679921383154817045\n"
        "ParticleSystemRenderer:\n"
        "  m_VertexStreams: 00010304\n"
        "  m_MaxParticleSize: Infinity\n"
        "  m_Materials:\n"
        "  - {fileID: 2100000, guid: feedb012a6f47ec4bbff1f15972b5c2b, type: 2}\n"
        "  m_Pivot: {x: 0.5, y: -1, z: 2}\n"
        "--- !u!4 &1501 stripped\n"
        "Transform:\n"
        "  m_CorrespondingSourceObject: {fileID: 400000, guid: 0123, type: 3}\n"
        "  m_PrefabInstance: {fileID: 1500}\n";

}

TEST(UnityYamlTest, SplitsDocumentsByHeader) {
    YamlFile file;
    std::string error;
    ASSERT_TRUE(file.parse(FILE_TEXT, error)) << error;
    ASSERT_EQ(file.documents.size(), 3u);
    EXPECT_EQ(file.documents[0].classId, CLASS_GAMEOBJECT);
    EXPECT_EQ(file.documents[0].fileId, 3192825672447820913LL);
    EXPECT_EQ(file.documents[0].typeName, "GameObject");
    EXPECT_FALSE(file.documents[0].stripped);
    EXPECT_EQ(file.documents[1].fileId, -8679921383154817045LL);
    EXPECT_TRUE(file.documents[2].stripped);
    EXPECT_EQ(readString(file.documents[0].body, "m_Name"), "Sparks");
    ASSERT_NE(file.find(1501), nullptr);
    EXPECT_EQ(file.find(1501)->classId, CLASS_TRANSFORM);
    EXPECT_EQ(file.find(42), nullptr);
}

TEST(UnityYamlTest, ScalarsAreTypedByTheReader) {
    YamlFile file;
    std::string error;
    ASSERT_TRUE(file.parse(FILE_TEXT, error)) << error;
    const YAML::Node renderer = file.documents[1].body;
    // a YAML 1.1 reader would make this an octal number
    EXPECT_EQ(readString(renderer, "m_VertexStreams"), "00010304");
    EXPECT_TRUE(std::isinf(readFloat(renderer, "m_MaxParticleSize")));
    EXPECT_FLOAT_EQ(readFloat(renderer, "missing", 7.0f), 7.0f);
    EXPECT_EQ(readInt(file.documents[0].body, "m_IsActive"), 1);
    EXPECT_TRUE(readBool(file.documents[0].body, "m_IsActive"));
    const Vector3 pivot = readVector3(renderer, "m_Pivot");
    EXPECT_FLOAT_EQ(pivot.x, 0.5f);
    EXPECT_FLOAT_EQ(pivot.y, -1.0f);
    EXPECT_FLOAT_EQ(pivot.z, 2.0f);
    const Ref material = toRef(renderer["m_Materials"][0]);
    EXPECT_EQ(material.fileID, 2100000);
    EXPECT_EQ(material.guid, "feedb012a6f47ec4bbff1f15972b5c2b");
    EXPECT_EQ(material.type, 2);
    const Ref instance = readRef(file.documents[2].body, "m_PrefabInstance");
    EXPECT_EQ(instance.fileID, 1500);
    EXPECT_TRUE(instance.guid.empty());
    EXPECT_TRUE(readRef(file.documents[2].body, "nothing").isNull());
}

TEST(UnityYamlTest, RejectsAForeignHeader) {
    YamlFile file;
    std::string error;
    EXPECT_FALSE(file.parse("--- !u!1 &1\nGameObject:\n  m_Name: a\n--- not unity\nfoo: 1\n", error));
    EXPECT_NE(error.find("header"), std::string::npos) << error;
}

TEST(UnityYamlTest, BuiltinAssetsByTheirIds) {
    const BuiltinAsset* cube = Project::builtin({10202, "0000000000000000e000000000000000", 0});
    ASSERT_NE(cube, nullptr);
    EXPECT_EQ(cube->kind, BuiltinAsset::Mesh);
    EXPECT_STREQ(cube->engine, "cube");
    const BuiltinAsset* additive = Project::builtin({10720, "0000000000000000f000000000000000", 0});
    ASSERT_NE(additive, nullptr);
    EXPECT_EQ(additive->kind, BuiltinAsset::Shader);
    EXPECT_STREQ(additive->name, "Mobile/Particles/Additive");
    // an id not in the table is unknown, never guessed
    EXPECT_EQ(Project::builtin({99999, "0000000000000000f000000000000000", 0}), nullptr);
    EXPECT_TRUE(Project::isBuiltinGuid("0000000000000000f000000000000000"));
    EXPECT_FALSE(Project::isBuiltinGuid("feedb012a6f47ec4bbff1f15972b5c2b"));
}

TEST(UnityYamlTest, ProjectIndexesGuidsAndFindsItsRoot) {
    const fs::path root = fs::path(JASPER_TEST_FIXTURES_DIR) / "unity-particles" / "Project";
    EXPECT_EQ(Project::findRoot(root / "Assets" / "Fx" / "Flame.prefab").lexically_normal(), root.lexically_normal());
    EXPECT_TRUE(Project::findRoot(fs::path(JASPER_TEST_FIXTURES_DIR) / "particles-legacy" / "presets.yaml").empty());
    Project project;
    std::string error;
    ASSERT_TRUE(project.open(root, error)) << error;
    const fs::path flame = project.pathOf("000000000000000000000000a11ce000");
    EXPECT_EQ(flame.filename(), "Flame.prefab");
    EXPECT_EQ(project.relative(flame), "Assets/Fx/Flame.prefab");
    EXPECT_TRUE(project.pathOf("ffffffffffffffffffffffffffffffff").empty());
    const YAML::Node meta = project.metaOf(project.pathOf("000000000000000000000000a11ce003"));
    ASSERT_TRUE(meta && meta["TextureImporter"]);
}
