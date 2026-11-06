// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R1: a .procmat is an asset with its GUID in the file, written the way the graph model writes it.

#include "util/AssetDatabase.h"
#include "asset/AssetTypes.h"
#include "asset/AssetType.h"
#include "graph/Graph.h"
#include "util/Util.h"

#include "TempDir.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    const char* GRAPH = R"({"formatVersion": 1, "kind": "procedural_material", "kindVersion": 1, "nextId": 2,
        "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0]}], "anUnknownKey": 1})";

    const std::string guidA = "0123456789abcdef0123456789abcdef";
    const std::string guidB = "fedcba9876543210fedcba9876543210";

}

TEST(ProcMatAsset, ItsTypeIsTheNextFreeValueAndHasAName) {
    EXPECT_EQ(static_cast<int>(AssetType::ProceduralMaterial), 29) << "24-27 are the pixel-art program's, 28 SoundEvent; the value is a file format";
    EXPECT_STREQ(assetTypeName(AssetType::ProceduralMaterial), "procedural_material");
    EXPECT_EQ(assetTypeFromName("procedural_material"), AssetType::ProceduralMaterial);
    EXPECT_EQ(assetTypeFromName("vertex_animation"), AssetType::VertexAnimation);
    EXPECT_EQ(assetTypeFromName("unknown"), AssetType::Unknown) << "a value with no name yet must not answer to this one";
}

TEST(ProcMatAsset, TheTableTracksItWithItsGuidInTheFile) {
    const AssetTypeInfo* info = AssetTypes::find("assets/wood.procmat", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::ProceduralMaterial);
    EXPECT_EQ(info->identity, AssetIdentity::InFile);
    EXPECT_EQ(info->base, AssetPathBase::Assets);
    EXPECT_TRUE(Util::isProceduralMaterialFile("assets/wood.procmat"));
    EXPECT_FALSE(Util::isProceduralMaterialFile("assets/wood.material"));
}

TEST(ProcMatAsset, TheDatabaseWritesItsIdentityIntoTheGraph) {
    TempDir dir;
    dir.write("wood.procmat", GRAPH);
    const Guid current = Guid::parse(guidA);
    const std::vector<Guid> former = {Guid::parse(guidB)};
    ASSERT_TRUE(AssetDatabase::writeInFileIdentity(dir.file("wood.procmat"), current, former));

    Guid read;
    std::vector<Guid> readFormer;
    ASSERT_TRUE(AssetDatabase::readInFileIdentity(dir.file("wood.procmat"), read, readFormer)) << dir.read("wood.procmat");
    EXPECT_EQ(read, current);
    EXPECT_EQ(readFormer, former);

    // still a graph, unknown keys kept, and written as the graph model writes it: an edit after this
    // moves nothing around
    const std::string written = dir.read("wood.procmat");
    graph::LoadReport report;
    const graph::Graph file = graph::load(written, report);
    ASSERT_TRUE(report.ok) << report.error;
    EXPECT_EQ(file.nodes.size(), 1u);
    EXPECT_TRUE(file.passthrough.contains("anUnknownKey"));
    EXPECT_EQ(graph::save(file), written);
}

TEST(ProcMatAsset, ABrokenFileIsLeftAlone) {
    TempDir dir;
    dir.write("broken.procmat", "{ not json");
    EXPECT_FALSE(AssetDatabase::writeInFileIdentity(dir.file("broken.procmat"), Guid::parse(guidA), {}));
    EXPECT_EQ(dir.read("broken.procmat"), "{ not json") << "its error must keep its line";
}

TEST(ProcMatAsset, ProjectOpenGivesANewFileAGuid) {
    TempDir project;
    project.write("project.yaml", "name: P\n");
    project.write("assets/wood.procmat", GRAPH);
    AssetDatabase database;
    database.open(project.path());
    Guid read;
    std::vector<Guid> former;
    ASSERT_TRUE(AssetDatabase::readInFileIdentity(project.file("assets/wood.procmat"), read, former)) << project.read("assets/wood.procmat");
    EXPECT_FALSE(read.isNull());
    EXPECT_EQ(database.typeOf(project.file("assets/wood.procmat")), AssetType::ProceduralMaterial);
}
