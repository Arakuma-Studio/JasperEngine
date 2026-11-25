// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .mesh assets in scenes (docs/specs/S98-editor-tooling-gaps.md F1, Deviations): a Mesh Renderer
// showing one saves the reference, not the buffers; a scene save moves large geometry into one
// (format 9), through an undo step of its own; a format 8 scene with its buffers inline loads as
// before; Save Mesh As Asset is one undo step.

#include "support/EditorTestSupport.h"
#include "voxel/VoxelTestScene.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamMigrations.h"
#include "asset/MeshAssetFiles.h"
#include "command/CommandHandle.h"
#include "export/AssetStripping.h"
#include "command/CommandHistory.h"
#include "command/type/MeshChangeCmd.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "model/MeshAsset.h"
#include "object/Primitive.h"
#include "reload/HotReloadService.h"
#include "reload/handlers/ReloadHandlers.h"
#include "subsystem/MeshSystem.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace mesh_asset_files_test {

    namespace fs = std::filesystem;

    bool sameBytes(const Buffer& a, const Buffer& b) {
        return a.getSize() == b.getSize() && (a.getSize() == 0 || std::memcmp(a.getData(), b.getData(), a.getSize()) == 0);
    }

    std::string fileText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    YAML::Node meshOf(const YAML::Node& root, const std::string& name) {
        for (const YAML::Node& entity : root["entities"]) {
            if (entity["name"] && entity["name"].as<std::string>() == name) return entity["components"]["mesh"];
        }
        return YAML::Node();
    }

    // the voxel tests' project (tools/fixtures/prefab-runtime: assets in "assets", the scene beside them)
    class MeshAssetSaveTest : public voxeltest::VoxelSceneTest {
    protected:
        Entity addPrimitive(const std::string& name, bool sphere) {
            Scene* scene = sceneProject->scene;
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, {});
            scene->addComponent<MeshComponent>(entity, {});
            scene->setEntityName(entity, name);
            // a handle that does not own it: the GameObject stays with the scene
            Primitive primitive(scene, entity);
            if (sphere) primitive.createSphere(1.0f, 36, 18);
            else primitive.createBox(1, 1, 1);
            sceneProject->entities.push_back(entity);
            return entity;
        }
        MeshComponent& meshOf(Entity entity) { return sceneProject->scene->getComponent<MeshComponent>(entity); }
    };

}

using namespace mesh_asset_files_test;

TEST(MeshAssetStream, TheReferenceReplacesTheBuffers) {
    editor::test::ensureEngine();
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 12, 8);
    MeshComponent mesh = sphere.getComponent<MeshComponent>();
    const Guid guid = Guid::parse("0123456789abcdef0123456789abcdef");
    mesh.meshAsset.set("meshes/ball.mesh", guid);

    const YAML::Node node = Stream::encodeMeshComponent(mesh);
    ASSERT_TRUE(node["meshAsset"]);
    EXPECT_EQ(node["meshAsset"].as<std::string>(), "meshes/ball.mesh");
    EXPECT_EQ(node["meshAssetGuid"].as<std::string>(), guid.str());
    EXPECT_FALSE(node["buffer"]) << "the file's geometry is not written";
    EXPECT_FALSE(node["indices"]);
    EXPECT_FALSE(node["aabb"]) << "nor what it gives: a save is the same whether the file was read yet";
    EXPECT_FALSE(node["submeshes"][0]["vertexCount"]);
    EXPECT_TRUE(node["submeshes"][0]["material"]) << "the materials stay with the component";

    const MeshComponent read = Stream::decodeMeshComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(read.meshAsset.path, "meshes/ball.mesh");
    EXPECT_EQ(read.meshAsset.guid, guid);
    EXPECT_TRUE(read.loadedMeshAsset.empty()) << "MeshSystem reads it";

    // none: inline as before, and decoding over a mesh that showed a file lets it go
    MeshComponent own = sphere.getComponent<MeshComponent>();
    const YAML::Node inlineNode = Stream::encodeMeshComponent(own);
    EXPECT_FALSE(inlineNode["meshAsset"]);
    EXPECT_TRUE(inlineNode["buffer"]);
    MeshComponent shown = read;
    shown.loadedMeshAsset = "meshes/ball.mesh";
    const MeshComponent over = Stream::decodeMeshComponent(inlineNode, &shown);
    EXPECT_TRUE(over.meshAsset.empty());
    EXPECT_TRUE(over.loadedMeshAsset.empty());
}

TEST(MeshAssetMigration, AFormat8SceneKeepsItsInlineBuffers) {
    const fs::path path = editor::test::fixture("migrations/scene-v8-inline-mesh.scene");
    const YAML::Node original = YAML::Load(editor::test::readText(path));
    ASSERT_EQ(original["formatVersion"].as<int>(), 8);

    YAML::Node migrated = YAML::Clone(original);
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, migrated);
    ASSERT_EQ(result.applied.size(), 2u) << "S98's mesh assets 8 -> 9, then S92's HTML UI render mode 9 -> 10";
    EXPECT_NE(result.applied[0].find("mesh assets"), std::string::npos) << result.applied[0];
    EXPECT_EQ(migrated["formatVersion"].as<int>(), 10);
    std::string why;
    EXPECT_TRUE(editor::test::structurallyEqual(meshOf(migrated, "Sphere"), meshOf(original, "Sphere"), &why)) << "identity: " << why;

    editor::test::DecodedScene decoded = editor::test::decodeSceneFile(path);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    Scene& scene = *decoded.scene->scene;
    ASSERT_EQ(decoded.scene->entities.size(), 1u);
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(decoded.scene->entities[0]);
    EXPECT_TRUE(mesh.meshAsset.empty());
    EXPECT_EQ(mesh.buffer.getSize(), 33744u) << "the buffers read as before";
    EXPECT_GT(MeshAsset::geometryBytes(mesh), MeshAssetFiles::EXTRACT_ABOVE_BYTES);

    // without a project to write a .mesh into, the save holds them as it read them
    const YAML::Node saved = editor::test::encodeScene(decoded);
    EXPECT_EQ(saved["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_TRUE(meshOf(saved, "Sphere")["buffer"]);
}

TEST_F(MeshAssetSaveTest, SceneSaveMovesLargeGeometryIntoMeshAssets) {
    const Entity ball = addPrimitive("Ball", true);
    const Entity twin = addPrimitive("Ball", true);
    const Entity crate = addPrimitive("Crate", false);
    const MeshComponent before = meshOf(ball);

    // a scene outside the assets folder: its meshes go to <assets>/Meshes/<scene>
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    const fs::path file = project->getAssetsPath() / "Meshes" / "Main Scene" / "Ball.mesh";
    ASSERT_TRUE(fs::exists(file)) << file;
    EXPECT_TRUE(fs::exists(fs::path(file.string() + ".meta"))) << "it has a GUID (S04)";
    EXPECT_FALSE(fs::exists(file.parent_path() / "Ball 1.mesh")) << "the twin's geometry is the same file";
    EXPECT_FALSE(fs::exists(file.parent_path() / "Crate.mesh")) << "a box stays in the scene";

    EXPECT_EQ(meshOf(ball).meshAsset.path, "Meshes/Main Scene/Ball.mesh");
    EXPECT_FALSE(meshOf(ball).meshAsset.guid.isNull());
    EXPECT_EQ(meshOf(twin).meshAsset.path, "Meshes/Main Scene/Ball.mesh");
    EXPECT_TRUE(meshOf(crate).meshAsset.empty());

    const fs::path sceneFile = project->getProjectPath() / "Main Scene.scene";
    const std::string text = fileText(sceneFile);
    const YAML::Node root = YAML::Load(text);
    EXPECT_EQ(root["formatVersion"].as<int>(), 10);
    const YAML::Node saved = mesh_asset_files_test::meshOf(root, "Ball");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved["meshAsset"].as<std::string>(), "Meshes/Main Scene/Ball.mesh");
    EXPECT_FALSE(saved["buffer"]);
    EXPECT_TRUE(mesh_asset_files_test::meshOf(root, "Crate")["buffer"]);

    // the file holds the geometry the scene had
    MeshComponent read;
    std::string error;
    const std::string bytes = fileText(file);
    ASSERT_TRUE(MeshAsset::decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), read, &error)) << error;
    EXPECT_TRUE(sameBytes(read.buffer, before.buffer));
    EXPECT_TRUE(sameBytes(read.indices, before.indices));

    // saved again: the same bytes, no new file
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    EXPECT_EQ(fileText(sceneFile), text);

    // its own geometry again (an undo past the save) finds the file holding it
    meshOf(ball).meshAsset.clear();
    meshOf(ball).loadedMeshAsset.clear();
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    EXPECT_EQ(meshOf(ball).meshAsset.path, "Meshes/Main Scene/Ball.mesh");
    EXPECT_FALSE(fs::exists(file.parent_path() / "Ball 1.mesh"));

    // another geometry under the same name never writes over it
    Primitive(sceneProject->scene, twin).createSphere(2.0f, 24, 12);
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    EXPECT_EQ(meshOf(twin).meshAsset.path, "Meshes/Main Scene/Ball 1.mesh");
    EXPECT_EQ(fileText(file), bytes);
}

// The save shows the files through one undo step, "Save Meshes As Assets": undone, the GameObject
// holds the geometry it had in the scene again (the edit before the save included), and undoing the
// edit then finds no file named; redone, it shows the file again
TEST_F(MeshAssetSaveTest, TheSaveShowsTheFilesThroughAnUndoStep) {
    const Entity ball = addPrimitive("Ball", true);
    const MeshComponent first = meshOf(ball);
    Scene other;
    Primitive bigger(&other);
    bigger.createSphere(2.0f, 24, 12);
    MeshComponent edited = bigger.getComponent<MeshComponent>();
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    ASSERT_TRUE(history->addCommandNoMerge(new MeshChangeCmd(project.get(), sceneProject->id, ball, edited)));
    const size_t beforeSave = history->position();

    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    EXPECT_EQ(meshOf(ball).meshAsset.path, "Meshes/Main Scene/Ball.mesh");
    EXPECT_FALSE(sceneProject->isModified);
    ASSERT_EQ(history->position(), beforeSave + 1) << "one step";
    ASSERT_NE(history->top(), nullptr);
    EXPECT_EQ(history->top()->name(), "Save Meshes As Assets");

    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(meshOf(ball).meshAsset.empty());
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, edited.buffer)) << "the geometry of the edit, in the scene";
    EXPECT_TRUE(sceneProject->isModified) << "the file shows a .mesh, the scene holds its geometry";
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(meshOf(ball).meshAsset.empty());
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, first.buffer));

    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(meshOf(ball).meshAsset.path, "Meshes/Main Scene/Ball.mesh");
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, edited.buffer));
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "Meshes" / "Main Scene" / "Ball.mesh")) << "the file stays throughout";
}

TEST_F(MeshAssetSaveTest, AScenesMeshesGoBesideItInTheAssetsFolder) {
    addPrimitive("Dome", true);
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "assets/levels/Arena.scene"));
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "levels" / "Arena" / "Dome.mesh"));
}

TEST_F(MeshAssetSaveTest, SaveMeshAsAssetIsOneUndoStep) {
    const Entity ball = addPrimitive("Ball", true);
    const MeshComponent before = meshOf(ball);
    std::string written, error;
    Command* save = MeshAssetFiles::saveAs(*project, sceneProject->id, ball, meshOf(ball), "assets/shapes/ball.mesh", written, &error);
    ASSERT_NE(save, nullptr) << error;
    EXPECT_EQ(written, "shapes/ball.mesh");
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    ASSERT_TRUE(history->addCommandNoMerge(save));
    EXPECT_EQ(meshOf(ball).meshAsset.path, "shapes/ball.mesh");
    sceneProject->scene->getSystem<MeshSystem>()->update(0);
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, before.buffer)) << "read back from the file";

    history->undo();
    EXPECT_TRUE(meshOf(ball).meshAsset.empty());
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, before.buffer)) << "its own geometry again";
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "shapes" / "ball.mesh")) << "the file stays";

    // refused: another extension, outside the assets folder, a mesh made by its model or generator
    EXPECT_EQ(MeshAssetFiles::saveAs(*project, sceneProject->id, ball, meshOf(ball), "assets/ball.glb", written, &error), nullptr);
    EXPECT_NE(error.find(".mesh"), std::string::npos) << error;
    EXPECT_EQ(MeshAssetFiles::saveAs(*project, sceneProject->id, ball, meshOf(ball), "../ball.mesh", written, &error), nullptr);
    EXPECT_NE(error.find("outside the assets folder"), std::string::npos) << error;
}

TEST_F(MeshAssetSaveTest, AFileAnotherProgramChangedIsReadAgain) {
    const Entity ball = addPrimitive("Ball", true);
    std::string written, error;
    ASSERT_TRUE(CommandHandle::get(sceneProject->id)->addCommandNoMerge(
        MeshAssetFiles::saveAs(*project, sceneProject->id, ball, meshOf(ball), "assets/shapes/ball.mesh", written, &error)))
        << error;
    std::shared_ptr<MeshSystem> meshSystem = sceneProject->scene->getSystem<MeshSystem>();
    meshSystem->update(0);

    Scene other;
    Primitive box(&other);
    box.createBox(3, 3, 3);
    const fs::path file = project->getAssetsPath() / "shapes" / "ball.mesh";
    ASSERT_TRUE(MeshAsset::save(file, box.getComponent<MeshComponent>(), &error)) << error;
    EXPECT_EQ(reload::kindOf(file), "mesh");
    HotReloadService& service = HotReloadService::get();
    service.attach(*project);
    service.apply(ChangeSet::fromPaths({file}), ApplyOptions());
    meshSystem->update(0);
    EXPECT_TRUE(sameBytes(meshOf(ball).buffer, box.getComponent<MeshComponent>().buffer));
}

// Referenced Only (S98 REQ-8) ships the .mesh a build scene shows and leaves out one nothing names
TEST_F(MeshAssetSaveTest, AssetStrippingShipsTheMeshesTheScenesShow) {
    addPrimitive("Ball", true);
    ASSERT_TRUE(project->saveSceneToPath(sceneProject->id, "Main Scene.scene"));
    Scene stray;
    Primitive ring(&stray);
    ring.createTorus(1.0f, 0.25f, 16, 12);
    AssetRef ref;
    std::string error;
    ASSERT_TRUE(MeshAssetFiles::write(*project, ring.getComponent<MeshComponent>(), "meshes/stray.mesh", ref, &error)) << error;

    project->getAssetIndex().waitUntilReady();
    AssetStrippingSettings settings;
    settings.mode = AssetStrippingMode::ReferencedOnly;
    project->setAssetStripping(settings);
    const AssetStrippingPlan plan = AssetStrippingPlan::make(*project);
    EXPECT_EQ(plan.leftOutReason("assets/Meshes/Main Scene/Ball.mesh"), "") << "the build scene shows it";
    EXPECT_EQ(plan.leftOutReason("assets/meshes/stray.mesh"), "unreferenced");
}
