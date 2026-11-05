// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S53 R10, R11: what scenes and scripts do with a model, read from their files. A blend
// override marks the submesh the cook keeps in order; Read/Write Automatic keeps a model's CPU copy
// for each runtime reader (a Mesh Collider, a runtime navigation bake, a particle mesh shape,
// runtime LOD building, a skinned mesh, a script's mesh-reading call) and releases it otherwise.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include "model/ModelUsage.h"

#include <algorithm>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    void writeText(const fs::path& file, const std::string& text){
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    // a scene with the panel model: its submesh 1 overridden to blend, a mesh collider from it
    const char* const kReaderScene =
        "id: 7\nname: Readers\nscene:\n  meshLodEnabled: true\nsceneType: scene_3d\nentities:\n"
        "  - type: Entity\n    entity: 1001\n    name: Panel\n    components:\n"
        "      model:\n        filename: models/panel.gltf\n"
        "      mesh:\n        submeshes:\n          - material: {alphaMode: ALPHA_OPAQUE}\n            overrideFields: 0\n"
        "          - material: {alphaMode: BLEND}\n            overrideFields: 32\n"
        "      rigidbody:\n        colliders:\n          - {type: mesh, source: entity_mesh}\n";

}

TEST(ModelUsage, ScenesMarkBlendedSubmeshesAndReaders){
    jasperengine::test::TestTempDir dir{"model-usage"};
    fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/textures", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto project = std::make_unique<Project>();
    project->setSkipProjectLock(true);
    ASSERT_TRUE(project->loadProject(dir.path(), false));
    ModelUsageIndex& index = ModelUsageIndex::get();
    index.refresh(*project);

    // the fixture's Main.scene uses the panel and reads nothing: released
    ModelUsage usage = index.usageOf("models/panel.gltf");
    ASSERT_EQ(usage.documents.size(), 1u);
    EXPECT_EQ(usage.objects(), 1);
    EXPECT_TRUE(usage.blended().empty());
    ModelReadWriteDecision decision = index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Automatic, false, true);
    EXPECT_FALSE(decision.keep) << (decision.reasons.empty() ? "" : decision.reasons.front());
    // On always keeps it, a skinned model always does
    EXPECT_TRUE(index.decideReadWrite("models/panel.gltf", ModelReadWrite::On, ScriptMeshData::Automatic, false, true).keep);
    EXPECT_TRUE(index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Automatic, true, true).keep);

    writeText(dir.path() / "Readers.scene", kReaderScene);
    index.refresh(*project);
    usage = index.usageOf("models/panel.gltf");
    EXPECT_EQ(usage.objects(), 2);
    const std::set<std::pair<int, int>> blended = usage.blended();
    EXPECT_EQ(blended, (std::set<std::pair<int, int>>{{-1, 1}})) << "the override names submesh 1 of every mesh";
    ASSERT_FALSE(usage.readers.empty());
    EXPECT_NE(usage.readers[0].find("Mesh Collider on Panel in Readers.scene"), std::string::npos) << usage.readers[0];
    decision = index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Automatic, false, true);
    EXPECT_TRUE(decision.keep);
    // LOD on in a scene and no baked levels: the runtime builds them from the CPU copy
    decision = index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Automatic, false, false);
    EXPECT_TRUE(std::any_of(decision.reasons.begin(), decision.reasons.end(),
                            [](const std::string& r){ return r.find("Mesh LOD is on in Readers.scene") != std::string::npos; }));

    // a script's mesh-reading call keeps every model, unless the project says release
    fs::remove(dir.path() / "Readers.scene");
    writeText(dir.path() / "scripts" / "Spawner.lua", "local Spawner = {}\nfunction Spawner:init() self.body:createMeshCollider() end\nreturn Spawner\n");
    index.refresh(*project);
    ASSERT_EQ(index.scriptReaders().size(), 1u);
    EXPECT_NE(index.scriptReaders()[0].find("scripts/Spawner.lua calls createMeshCollider"), std::string::npos) << index.scriptReaders()[0];
    EXPECT_TRUE(index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Automatic, false, true).keep);
    EXPECT_FALSE(index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Release, false, true).keep);
    EXPECT_TRUE(index.decideReadWrite("models/panel.gltf", ModelReadWrite::Automatic, ScriptMeshData::Keep, false, true).keep);
    index.clear();
    closeProject(*project);
}
