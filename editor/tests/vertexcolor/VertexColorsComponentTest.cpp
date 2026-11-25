// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R1, R5-R7, R15, R130-R132: the Vertex Colors component in the editor:
// its table and scene YAML (defaults not written), where it may go, the .vcolors asset type, the
// generic value commands and their undo, the prefab override copy, and scenes from before it.

#include "support/EditorTestSupport.h"
#include "support/PosixCompat.h"
#include "support/TestTempDir.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "Engine.h"
#include "asset/AssetType.h"
#include "asset/AssetTypes.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "component/VertexColorsComponent.h"
#include "reflect/Reflect.h"
#include "util/AssetDatabase.h"
#include "util/VersionControlFiles.h"
#include "window/dialog/ComponentAddDialog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_colors_component_test {
    using namespace jasperengine::editor::test;

    constexpr ComponentType VC = ComponentType::VertexColorsComponent;

    constexpr const char* kHeader = "formatVersion: 1\nid: 4243\nname: Painted\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\n"
                                    "mainCamera: 0\nentities:\n";

    // a GameObject with the component's YAML `fields` (where it may go is an Add Component rule: a
    // file is read as it is)
    std::string withColors(const std::string& fields) {
        return std::string(kHeader) + "  - type: Entity\n    entity: 1001\n    name: Rock\n    components:\n      transform:\n        position: [0, 0, 0]\n"
               "      vertexcolors:" + fields;
    }

    const VertexColorsComponent* colorsOf(const DecodedScene& decoded) {
        if (decoded.scene->entities.empty()) return nullptr;
        return decoded.scene->scene->findComponent<VertexColorsComponent>(decoded.scene->entities.front());
    }

    std::set<std::string> componentKeys(const YAML::Node& entity) {
        std::set<std::string> keys;
        for (auto it = entity["components"].begin(); it != entity["components"].end(); ++it) keys.insert(it->first.Scalar());
        return keys;
    }

    // ---- one headless project for the command tests

    struct Headless {
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-vertex-colors-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            std::ofstream(dir / "project.yaml") << "name: vertexcolors\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            useProjectPaths(out.project.get());
            Engine::setAsyncLoading(false);
            if (!out.project->loadProject(dir, false)) throw std::runtime_error("the test project did not load");
            return out;
        }();
        if (h.project) useProjectPaths(h.project.get());
        return h;
    }

TEST(VertexColorsScene, DefaultsAreNotWritten) {
    const reflect::ComponentDesc* desc = reflect::find(VC);
    ASSERT_NE(desc, nullptr);
    EXPECT_STREQ(desc->displayName, "Vertex Colors");
    EXPECT_STREQ(desc->key, "vertexcolors");
    EXPECT_EQ(Catalog::getComponentType("VertexColorsComponent"), VC);
    EXPECT_TRUE(desc->hasEnabled);

    DecodedScene decoded = decodeSceneText(withColors(" {}\n"), "Painted.scene");
    const VertexColorsComponent* colors = colorsOf(decoded);
    ASSERT_NE(colors, nullptr);
    EXPECT_TRUE(colors->enabled);
    EXPECT_TRUE(colors->data.path.empty());
    EXPECT_EQ(colors->format, vertexcolor::Format::RGBA8);
    EXPECT_EQ(colors->initialValue, vertexcolor::InitialValue::Asset);
    const YAML::Node component = encodeScene(decoded)["entities"][0]["components"]["vertexcolors"];
    ASSERT_TRUE(component.IsDefined()) << encodeSceneText(decoded);
    EXPECT_EQ(component.size(), 0u) << encodeSceneText(decoded);
}

TEST(VertexColorsScene, NonDefaultValuesRoundTrip) {
    const std::string fields = "\n        enabled: false\n        data: paint/Rock.vcolors\n        format: rgba16f\n        initialValue: custom\n"
                               "        customInitial: [0.25, 0.5, 2, 1]\n        channelNames: [Moss, \"\", Wetness, Snow]\n";
    DecodedScene decoded = decodeSceneText(withColors(fields), "Painted.scene");
    for (const LoadIssue& issue : decoded.report.issues) {
        EXPECT_EQ(issue.fate, LoadIssueFate::Info) << issue.consoleLine();
    }
    const VertexColorsComponent* colors = colorsOf(decoded);
    ASSERT_NE(colors, nullptr);
    EXPECT_FALSE(colors->enabled);
    EXPECT_EQ(colors->data.path, "paint/Rock.vcolors");
    EXPECT_EQ(colors->format, vertexcolor::Format::RGBA16F);
    EXPECT_EQ(colors->initialValue, vertexcolor::InitialValue::Custom);
    EXPECT_EQ(colors->customInitial, Vector4(0.25f, 0.5f, 2.0f, 1.0f));
    EXPECT_EQ(colors->channelNames, (std::array<std::string, 4>{"Moss", "", "Wetness", "Snow"}));

    // the enums are written as their tokens, and a second save is the first
    const YAML::Node component = encodeScene(decoded)["entities"][0]["components"]["vertexcolors"];
    EXPECT_EQ(component["format"].Scalar(), "rgba16f");
    EXPECT_EQ(component["initialValue"].Scalar(), "custom");
    EXPECT_EQ(component["channelNames"].size(), 4u);
    const std::string once = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(once, "Painted.scene");
    EXPECT_EQ(encodeSceneText(again), once);
}

// the channel names are always four: a hand-edited list of another length keeps that shape
TEST(VertexColorsScene, ChannelNamesKeepFourEntries) {
    DecodedScene shorter = decodeSceneText(withColors("\n        channelNames: [Moss]\n"), "Painted.scene");
    ASSERT_NE(colorsOf(shorter), nullptr);
    EXPECT_EQ(colorsOf(shorter)->channelNames, (std::array<std::string, 4>{"Moss", "", "", ""}));
    DecodedScene longer = decodeSceneText(withColors("\n        channelNames: [A, B, C, D, E, F]\n"), "Painted.scene");
    ASSERT_NE(colorsOf(longer), nullptr);
    EXPECT_EQ(colorsOf(longer)->channelNames, (std::array<std::string, 4>{"A", "B", "C", "D"}));
}

// R130: a scene an earlier editor saved, with Models and Meshes and no Vertex Colors, loads as it
// was: every GameObject's components are the file's, and none gains the new one
TEST(VertexColorsScene, OldSceneLoadsUnchanged) {
    const fs::path file = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "physics-placement" / "RingPeg.scene";
    const YAML::Node original = YAML::LoadFile(file.string());
    DecodedScene decoded = decodeSceneFile(file);
    for (const LoadIssue& issue : decoded.report.issues) {
        EXPECT_EQ(issue.fate, LoadIssueFate::Info) << issue.consoleLine();
    }
    ASSERT_FALSE(decoded.scene->entities.empty());
    for (Entity entity : decoded.scene->entities) {
        EXPECT_EQ(decoded.scene->scene->findComponent<VertexColorsComponent>(entity), nullptr);
    }
    const YAML::Node encoded = encodeScene(decoded);
    ASSERT_EQ(encoded["entities"].size(), original["entities"].size());
    size_t meshes = 0;
    for (size_t i = 0; i < original["entities"].size(); i++) {
        const std::set<std::string> keys = componentKeys(original["entities"][i]);
        meshes += keys.count("mesh");
        EXPECT_EQ(componentKeys(encoded["entities"][i]), keys) << original["entities"][i]["name"].Scalar();
    }
    EXPECT_GT(meshes, 0u) << "the fixture has no Mesh: nothing here was tested";
    EXPECT_EQ(encodeSceneText(decoded).find("vertexcolors"), std::string::npos);
}

// R1: on the GameObject of a placed Model or Mesh; never on a mesh another component makes, nor on a
// Model's mesh node (a reload makes those again)
TEST(VertexColorsCatalog, GoesOnAModelOrAPlainMesh) {
    EXPECT_TRUE(ComponentAddDialog::isListed(VC));
    Scene scene;
    const Entity empty = scene.createEntity();
    scene.addComponent<Transform>(empty, {});
    EXPECT_NE(Catalog::getComponentConflict(&scene, empty, VC).find("Model or a Mesh"), std::string::npos);

    const Entity mesh = scene.createEntity();
    scene.addComponent<Transform>(mesh, {});
    scene.addComponent<MeshComponent>(mesh, {});
    EXPECT_EQ(Catalog::getComponentConflict(&scene, mesh, VC), "");

    const Entity model = scene.createEntity();
    scene.addComponent<Transform>(model, {});
    scene.addComponent<ModelComponent>(model, {});
    EXPECT_EQ(Catalog::getComponentConflict(&scene, model, VC), "");

    const Entity sprite = scene.createEntity();
    scene.addComponent<Transform>(sprite, {});
    scene.addComponent<MeshComponent>(sprite, {});
    scene.addComponent<SpriteComponent>(sprite, {});
    EXPECT_NE(Catalog::getComponentConflict(&scene, sprite, VC).find("Sprite"), std::string::npos);

    const Entity node = scene.createEntity();
    Transform child;
    child.parent = model;
    scene.addComponent<Transform>(node, child);
    scene.addComponent<MeshComponent>(node, {});
    scene.getComponent<ModelComponent>(model).meshNodesMapping[0] = node;
    EXPECT_NE(Catalog::getComponentConflict(&scene, node, VC).find("mesh nodes"), std::string::npos);
    // a mesh put below a Model by hand is no node of it
    const Entity added = scene.createEntity();
    scene.addComponent<Transform>(added, child);
    scene.addComponent<MeshComponent>(added, {});
    EXPECT_EQ(Catalog::getComponentConflict(&scene, added, VC), "");
}

// The editor reads and writes every enum property as an int (the Inspector's rows, MCP, prefab
// override copies): a write of one field leaves its neighbours alone
TEST(VertexColorsCatalog, PropertiesAreTheFieldsAndWritesStayInThem) {
    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<MeshComponent>(entity, {});
    VertexColorsComponent authored;
    authored.initialValue = vertexcolor::InitialValue::Custom;
    authored.customInitial = Vector4(0.1f, 0.2f, 0.3f, 0.4f);
    scene.addComponent<VertexColorsComponent>(entity, authored);

    const std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(&scene, entity, VC);
    for (const char* name : {"enabled", "data", "format", "initialValue", "customInitial", "channelNames[0]", "channelNames[3]"}) {
        EXPECT_TRUE(properties.count(name)) << name;
    }
    EXPECT_FALSE(properties.count("channelNames[4]"));
    EXPECT_FALSE(properties.count("revision")) << "runtime state is no property";
    EXPECT_FALSE(properties.count("loaded"));

    int* format = Catalog::getPropertyRef<int>(&scene, entity, VC, "format");
    ASSERT_NE(format, nullptr);
    *format = static_cast<int>(vertexcolor::Format::RGBA16F);
    const VertexColorsComponent& colors = scene.getComponent<VertexColorsComponent>(entity);
    EXPECT_EQ(colors.format, vertexcolor::Format::RGBA16F);
    EXPECT_EQ(colors.initialValue, vertexcolor::InitialValue::Custom) << "the format's write reached the next field";
    int* initial = Catalog::getPropertyRef<int>(&scene, entity, VC, "initialValue");
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(*initial, static_cast<int>(vertexcolor::InitialValue::Custom));
    *initial = static_cast<int>(vertexcolor::InitialValue::Black);
    EXPECT_EQ(colors.customInitial, Vector4(0.1f, 0.2f, 0.3f, 0.4f)) << "the initial value's write reached the next field";

    // a prefab instance takes an overridden field from its source, one property at a time
    Scene other;
    const Entity target = other.createEntity();
    other.addComponent<Transform>(target, {});
    other.addComponent<MeshComponent>(target, {});
    other.addComponent<VertexColorsComponent>(target, {});
    scene.getComponent<VertexColorsComponent>(entity).channelNames[2] = "Wetness";
    Catalog::copyPropertyValue(&scene, entity, &other, target, VC, "format");
    Catalog::copyPropertyValue(&scene, entity, &other, target, VC, "channelNames[2]");
    const VertexColorsComponent& copied = other.getComponent<VertexColorsComponent>(target);
    EXPECT_EQ(copied.format, vertexcolor::Format::RGBA16F);
    EXPECT_EQ(copied.initialValue, vertexcolor::InitialValue::Asset);
    EXPECT_EQ(copied.channelNames[2], "Wetness");
    EXPECT_EQ(copied.channelNames[1], "");
}

// Add Component and every field edit are undo steps of the generic commands
TEST(VertexColorsUndo, AddAndFieldEditsUndo) {
    Project* project = headless().project.get();
    ASSERT_NE(project, nullptr);
    if (SceneProject* current = project->getSelectedScene()) current->isModified = false;
    project->createNewScene("Painted", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    ASSERT_TRUE(sceneProject && sceneProject->scene);
    Scene* scene = sceneProject->scene;
    CommandHistory* history = CommandHandle::get(sceneId);

    const Entity bare = scene->createEntity();
    scene->addComponent<Transform>(bare, {});
    sceneProject->entities.push_back(bare);
    history->addCommandNoMerge(new AddComponentCmd(project, sceneId, bare, VC));
    EXPECT_EQ(scene->findComponent<VertexColorsComponent>(bare), nullptr) << "added to a GameObject without a Model or a Mesh";

    const Entity rock = scene->createEntity();
    scene->addComponent<Transform>(rock, {});
    scene->addComponent<MeshComponent>(rock, {});
    sceneProject->entities.push_back(rock);
    history->addCommandNoMerge(new AddComponentCmd(project, sceneId, rock, VC));
    ASSERT_NE(scene->findComponent<VertexColorsComponent>(rock), nullptr);

    history->addCommandNoMerge(new PropertyCmd<bool>(project, sceneId, rock, VC, "enabled", false));
    history->addCommandNoMerge(new PropertyCmd<int>(project, sceneId, rock, VC, "format", static_cast<int>(vertexcolor::Format::RGBA16F)));
    history->addCommandNoMerge(new PropertyCmd<int>(project, sceneId, rock, VC, "initialValue", static_cast<int>(vertexcolor::InitialValue::Custom)));
    history->addCommandNoMerge(new PropertyCmd<Vector4>(project, sceneId, rock, VC, "customInitial", Vector4(0.0f, 1.0f, 0.0f, 1.0f)));
    history->addCommandNoMerge(new PropertyCmd<std::string>(project, sceneId, rock, VC, "channelNames[1]", std::string("Moss")));
    history->addCommandNoMerge(new PropertyCmd<std::string>(project, sceneId, rock, VC, "data", std::string("paint/Rock.vcolors")));
    {
        const VertexColorsComponent& colors = scene->getComponent<VertexColorsComponent>(rock);
        EXPECT_FALSE(colors.enabled);
        EXPECT_EQ(colors.format, vertexcolor::Format::RGBA16F);
        EXPECT_EQ(colors.initialValue, vertexcolor::InitialValue::Custom);
        EXPECT_EQ(colors.customInitial, Vector4(0.0f, 1.0f, 0.0f, 1.0f));
        EXPECT_EQ(colors.channelNames[1], "Moss");
        EXPECT_EQ(colors.data.path, "paint/Rock.vcolors");
    }
    for (int step = 0; step < 6; step++) ASSERT_TRUE(history->undo()) << "step " << step;
    {
        const VertexColorsComponent& colors = scene->getComponent<VertexColorsComponent>(rock);
        const VertexColorsComponent defaults;
        EXPECT_TRUE(colors.enabled);
        EXPECT_EQ(colors.format, defaults.format);
        EXPECT_EQ(colors.initialValue, defaults.initialValue);
        EXPECT_EQ(colors.customInitial, defaults.customInitial);
        EXPECT_EQ(colors.channelNames[1], "");
        EXPECT_TRUE(colors.data.path.empty());
    }
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene->findComponent<VertexColorsComponent>(rock), nullptr) << "undo of Add Component";
    ASSERT_TRUE(history->redo());
    EXPECT_NE(scene->findComponent<VertexColorsComponent>(rock), nullptr);
    history->clear();
    sceneProject->isModified = false;
}

// R1, R131: a .vcolors is an asset of its own type with a GUID in a .meta beside it, and a binary
TEST(AssetTypes, VertexColorsValueIs40) {
    // a file format (assetmap.bin): the number and the name stay
    EXPECT_EQ(static_cast<int>(AssetType::VertexColors), 40);
    EXPECT_STREQ(assetTypeName(AssetType::VertexColors), "vertex_colors");
    EXPECT_EQ(assetTypeFromName("vertex_colors"), AssetType::VertexColors);
    for (const char* path : {"assets/paint/Rock.vcolors", "assets/paint/Rock.VColors"}) {
        const AssetTypeInfo* info = AssetTypes::find(path, "assets");
        ASSERT_NE(info, nullptr) << path;
        EXPECT_EQ(info->type, AssetType::VertexColors) << path;
        EXPECT_EQ(info->identity, AssetIdentity::Sidecar) << path;
        EXPECT_EQ(info->base, AssetPathBase::Assets) << path;
    }
    EXPECT_NE(vcsfiles::attributesBlock(true, false).find("*.vcolors"), std::string::npos) << "an LFS object like .jetrees";

    jasperengine::test::TestTempDir dir("vertex-colors-asset");
    fs::create_directories(dir.path() / "assets/paint");
    std::ofstream(dir.path() / "assets/paint/Rock.vcolors", std::ios::binary) << "VCOL";
    AssetDatabase database;
    database.open(dir.path(), "assets");
    database.waitForBackgroundWork();
    const Guid guid = database.getAssetGuid("assets/paint/Rock.vcolors");
    EXPECT_FALSE(guid.isNull());
    EXPECT_TRUE(fs::exists(dir.path() / "assets/paint/Rock.vcolors.meta"));
    const AssetDatabase::Resolution reference = database.lookupPath("assets/paint/Rock.vcolors");
    EXPECT_TRUE(reference.found());
    EXPECT_EQ(reference.guid, guid);
}

} // namespace vertex_colors_component_test
