// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The semantic scene and prefab diff (docs/specs/S29-version-control.md R13, R14, AC11). The cases
// of OpenEngine's SceneDiffTests (Tests/Editor/SceneDiffTests.cpp, MIT, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors; NOTICE.md) are ported to our YAML: alignment across
// reordering, added/removed/modified properties, GameObject addition and removal, reparenting to
// and from the root, an unchanged parent, CRLF input, a GUID that is no id.

#include "vcs/scene/SceneDiff.h"

#include "support/EditorTestSupport.h"
#include "StreamMigrations.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vcs::scene;

namespace {

    std::string versionLine() {
        return "formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) + "\n";
    }

    std::string entity(uint64_t id, const std::string& name, const std::string& position = "[0, 0, 0]", const std::string& extra = "",
                       const std::string& indent = "  ") {
        return indent + "- type: Entity\n" + indent + "  entity: " + std::to_string(id) + "\n" + indent + "  name: " + name + "\n" +
               indent + "  components:\n" + indent + "    transform:\n" + indent + "      position: " + position + "\n" + indent +
               "      rotation: [1, 0, 0, 0]\n" + indent + "      scale: [1, 1, 1]\n" + extra;
    }

    std::string scene(const std::string& entities, const std::string& settings = "scene:\n  backgroundColor: [0, 0, 0, 1]\n") {
        return versionLine() + "id: 1\nname: Test\n" + settings + "sceneType: scene_3d\nmainCamera: 0\nentities:\n" + entities;
    }

    SceneYamlModel model(const std::string& text) {
        SceneYamlModel out;
        std::string error;
        EXPECT_TRUE(SceneYamlModel::parse(text, out, &error)) << error << "\n" << text;
        return out;
    }

    SceneDiffResult diff(const std::string& a, const std::string& b, bool changedOnly = true) {
        SceneDiffOptions options;
        options.changedOnly = changedOnly;
        return diffScenes(model(a), model(b), options);
    }

    std::vector<std::string> paths(const SceneDiffResult& result, uint64_t id) {
        std::vector<std::string> out;
        const ObjectDiff* object = result.find(id);
        if (!object) return out;
        for (const ComponentDiff& component : object->components) {
            for (const PropertyDiff& property : component.properties) out.push_back(propertyPath(component, property));
        }
        return out;
    }

}

TEST(SceneDiff, AlignsEntitiesByStableIdAcrossReordering) {
    const std::string a = scene(entity(10, "A") + entity(11, "B") + entity(12, "C"));
    const std::string b = scene(entity(12, "C") + entity(10, "A") + entity(11, "B"));
    const SceneDiffResult result = diff(a, b);
    EXPECT_EQ(result.added, 0u);
    EXPECT_EQ(result.removed, 0u);
    EXPECT_EQ(result.propertyCount(), 0u);
    // one GameObject moved: the others kept their order among themselves
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].id, 12u);
    EXPECT_TRUE(result.objects[0].reordered);
    EXPECT_FALSE(result.objects[0].reparented);
}

TEST(SceneDiff, ReportsAddedRemovedAndModifiedProperties) {
    const std::string a = scene(entity(10, "A", "[0, 0, 0]", "      mesh:\n        castShadows: true\n        receiveShadows: true\n"));
    const std::string b = scene(entity(10, "A", "[1, 2, 3]", "      mesh:\n        castShadows: true\n        cullingMode: back\n"));
    const SceneDiffResult result = diff(a, b);
    ASSERT_EQ(result.objects.size(), 1u);
    const ObjectDiff& object = result.objects[0];
    EXPECT_EQ(object.state, ChangeState::Modified);
    const ComponentDiff* transform = object.component("transform");
    ASSERT_NE(transform, nullptr);
    ASSERT_EQ(transform->properties.size(), 1u);
    EXPECT_EQ(propertyPath(*transform, transform->properties[0]), "transform.position");
    EXPECT_EQ(transform->properties[0].baseValue, "[0, 0, 0]");
    EXPECT_EQ(transform->properties[0].currentValue, "[1, 2, 3]");
    const ComponentDiff* mesh = object.component("mesh");
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->properties.size(), 2u);
    for (const PropertyDiff& property : mesh->properties) {
        if (property.path == "receiveShadows") {
            EXPECT_TRUE(property.inBase);
            EXPECT_FALSE(property.inCurrent);
        } else {
            EXPECT_EQ(property.path, "cullingMode");
            EXPECT_FALSE(property.inBase);
            EXPECT_EQ(property.currentValue, "back");
        }
    }
}

TEST(SceneDiff, ComponentsAddedAndRemoved) {
    const std::string a = scene(entity(10, "A", "[0, 0, 0]", "      light:\n        type: point\n"));
    const std::string b = scene(entity(10, "A", "[0, 0, 0]", "      audio:\n        volume: 1\n"));
    const SceneDiffResult result = diff(a, b);
    ASSERT_EQ(result.objects.size(), 1u);
    ASSERT_NE(result.objects[0].component("light"), nullptr);
    EXPECT_EQ(result.objects[0].component("light")->state, ChangeState::Removed);
    ASSERT_NE(result.objects[0].component("audio"), nullptr);
    EXPECT_EQ(result.objects[0].component("audio")->state, ChangeState::Added);
}

TEST(SceneDiff, KeepsSceneSettingsOutOfTheLastGameObject) {
    const std::string a = scene(entity(10, "A")) + "extraSetting: 1\n";
    const std::string b = scene(entity(10, "A")) + "extraSetting: 2\n";
    const SceneDiffResult result = diff(a, b);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_TRUE(result.objects[0].settings);
    EXPECT_EQ(result.objects[0].name, "Scene Settings");
    EXPECT_EQ(paths(result, kSceneSettingsId), std::vector<std::string>{"extraSetting"});
    EXPECT_EQ(result.find(10), nullptr);
}

TEST(SceneDiff, SceneSettingsProperties) {
    const SceneDiffResult result = diff(scene(entity(10, "A")), scene(entity(10, "A"), "scene:\n  backgroundColor: [1, 0, 0, 1]\n"));
    EXPECT_EQ(paths(result, kSceneSettingsId), std::vector<std::string>{"scene.backgroundColor"});
}

TEST(SceneDiff, ReportsGameObjectAdditionAndRemoval) {
    const SceneDiffResult result = diff(scene(entity(10, "A") + entity(11, "B")), scene(entity(10, "A") + entity(12, "C")));
    EXPECT_EQ(result.added, 1u);
    EXPECT_EQ(result.removed, 1u);
    ASSERT_NE(result.find(12), nullptr);
    EXPECT_EQ(result.find(12)->state, ChangeState::Added);
    ASSERT_NE(result.find(11), nullptr);
    EXPECT_EQ(result.find(11)->state, ChangeState::Removed);
    EXPECT_EQ(result.find(11)->name, "B");
}

TEST(SceneDiff, ReparentingAGameObjectIsAChange) {
    const std::string child = entity(12, "Child", "[0, 0, 0]", "", "      ");
    const std::string a = scene(entity(10, "A", "[0, 0, 0]", "    children:\n" + child) + entity(11, "B"));
    const std::string b = scene(entity(10, "A") + entity(11, "B", "[0, 0, 0]", "    children:\n" + child));
    const SceneDiffResult result = diff(a, b);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].id, 12u);
    EXPECT_TRUE(result.objects[0].reparented);
    EXPECT_EQ(result.objects[0].baseParent, 10u);
    EXPECT_EQ(result.objects[0].currentParent, 11u);
    EXPECT_EQ(result.propertyCount(), 0u);
}

TEST(SceneDiff, MovingAGameObjectToAndFromTheRootIsAReparent) {
    const std::string child = entity(12, "Child", "[0, 0, 0]", "", "      ");
    const std::string nested = scene(entity(10, "A", "[0, 0, 0]", "    children:\n" + child));
    const std::string flat = scene(entity(10, "A") + entity(12, "Child"));
    const SceneDiffResult toRoot = diff(nested, flat);
    ASSERT_NE(toRoot.find(12), nullptr);
    EXPECT_TRUE(toRoot.find(12)->reparented);
    EXPECT_EQ(toRoot.find(12)->currentParent, 0u);
    const SceneDiffResult fromRoot = diff(flat, nested);
    ASSERT_NE(fromRoot.find(12), nullptr);
    EXPECT_TRUE(fromRoot.find(12)->reparented);
    EXPECT_EQ(fromRoot.find(12)->currentParent, 10u);
}

TEST(SceneDiff, UnchangedParentIsNotAChange) {
    const std::string child = entity(12, "Child", "[0, 0, 0]", "", "      ");
    const std::string a = scene(entity(10, "A", "[0, 0, 0]", "    children:\n" + child));
    EXPECT_TRUE(diff(a, a).empty());
    EXPECT_TRUE(diff(a, a).objects.empty());
    // every object listed when asked, none changed
    const SceneDiffResult all = diff(a, a, false);
    EXPECT_EQ(all.objects.size(), 3u);
    for (const ObjectDiff& object : all.objects) EXPECT_FALSE(object.changed());
}

TEST(SceneDiff, CrlfInputMakesNoFalseChange) {
    const std::string lf = scene(entity(10, "A", "[1, 2, 3]") + entity(11, "B"));
    std::string crlf;
    for (char c : lf) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    EXPECT_TRUE(diff(lf, crlf).empty());
}

TEST(SceneDiff, GuidIsNotMistakenForAnId) {
    // a GUID made of digits, and an "entity:" inside a component, are values, not ids
    const std::string extra = "      prefab:\n        guid: 00000000000000000000000000000011\n        entity: 11\n";
    const std::string a = scene(entity(10, "A", "[0, 0, 0]", extra) + entity(11, "B"));
    const std::string b = scene(entity(10, "A", "[0, 0, 0]", extra) + entity(11, "B", "[5, 0, 0]"));
    const SceneDiffResult result = diff(a, b);
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].id, 11u);
    EXPECT_EQ(paths(result, 11), std::vector<std::string>{"transform.position"});
}

TEST(SceneDiff, NumbersCompareWithinARelativeEpsilon) {
    EXPECT_TRUE(diff(scene(entity(10, "A", "[1, 2, 3]")), scene(entity(10, "A", "[1.0000001, 2, 3.0]"))).empty());
    EXPECT_FALSE(diff(scene(entity(10, "A", "[1, 2, 3]")), scene(entity(10, "A", "[1.001, 2, 3]"))).empty());
    EXPECT_TRUE(diff(scene(entity(10, "A", "[1000000, 0, 0]")), scene(entity(10, "A", "[1000000.5, 0, 0]"))).empty());
}

TEST(SceneDiff, IgnoresDerivedTransformKeys) {
    const std::string derivedA = "        worldPosition: [0, 0, 0]\n        worldRotation: [1, 0, 0, 0]\n        worldScale: [1, 1, 1]\n"
                                 "        localMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]\n"
                                 "        modelMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]\n"
                                 "        normalMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]\n";
    const std::string derivedB = "        worldPosition: [7, 7, 7]\n        modelMatrix: [[2, 0, 0, 0], [0, 2, 0, 0], [0, 0, 2, 0], [7, 7, 7, 1]]\n";
    // a parent moved: its children's derived values change, nothing else
    const std::string a = "id: 1\nname: Test\nentities:\n" + entity(10, "A", "[0, 0, 0]", derivedA);
    const std::string b = versionLine() + "id: 1\nname: Test\nentities:\n" + entity(10, "A", "[0, 0, 0]", derivedB);
    EXPECT_TRUE(diff(a, b).empty());
    EXPECT_TRUE(isDerivedKey("transform", "modelMatrix"));
    EXPECT_FALSE(isDerivedKey("transform", "position"));
    EXPECT_FALSE(isDerivedKey("mesh", "modelMatrix"));
}

TEST(SceneDiff, KeyedScriptProperties) {
    auto script = [](const std::string& speed, bool reversed) {
        const std::string speedProperty = "              - name: speed\n                type: Float\n                value: " + speed + "\n";
        const std::string bodyProperty = "              - name: body\n                type: EntityReference\n                value: 11\n";
        return std::string("      script:\n        scripts:\n          - type: lua\n            path: Enemy.lua\n            className: Enemy\n"
                           "            enabled: true\n            properties:\n") +
               (reversed ? bodyProperty + speedProperty : speedProperty + bodyProperty);
    };
    const std::string a = scene(entity(10, "Enemy", "[0, 0, 0]", script("1.5", false)) + entity(11, "Body"));
    // reordering the script properties is no change
    EXPECT_TRUE(diff(a, scene(entity(10, "Enemy", "[0, 0, 0]", script("1.5", true)) + entity(11, "Body"))).empty());
    // editing speed is exactly one property
    const SceneDiffResult result = diff(a, scene(entity(10, "Enemy", "[0, 0, 0]", script("3", true)) + entity(11, "Body")));
    EXPECT_EQ(paths(result, 10), std::vector<std::string>{"script.scripts[lua:Enemy.lua].properties[speed].value"});
    const ComponentDiff* component = result.find(10)->component("script");
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->properties[0].baseValue, "1.5");
    EXPECT_EQ(component->properties[0].currentValue, "3");
}

TEST(SceneDiff, PrefabsListIsKeyedByGuid) {
    const std::string prefabs = "prefabs:\n  - path: a.prefab\n    guid: aaaa\n  - path: b.prefab\n    guid: bbbb\n";
    const std::string moved = "prefabs:\n  - path: b.prefab\n    guid: bbbb\n  - path: renamed/a.prefab\n    guid: aaaa\n";
    const SceneDiffResult result = diff(scene(entity(10, "A")) + prefabs, scene(entity(10, "A")) + moved);
    EXPECT_EQ(paths(result, kSceneSettingsId), std::vector<std::string>{"prefabs[aaaa].path"});
}

TEST(SceneDiff, MigratesBaseline) {
    // a v0 baseline: the old default shadow quality (low) reads as auto after migration, the
    // derived values go; only the real edit remains
    const std::string v0 = "id: 1\nname: Test\nscene:\n  shadowQuality: low\nsceneType: scene_3d\nentities:\n" +
                           entity(10, "A", "[0, 0, 0]", "        worldPosition: [0, 0, 0]\n");
    SceneYamlModel base = model(v0);
    EXPECT_TRUE(base.migration.migrated());
    EXPECT_EQ(base.formatVersion, StreamMigrations::currentVersion(DocumentKind::Scene));
    const std::string current = versionLine() + "id: 1\nname: Test\nscene:\n  shadowQuality: auto\nsceneType: scene_3d\nentities:\n" +
                                entity(10, "A", "[4, 0, 0]");
    const SceneDiffResult result = diffScenes(base, model(current));
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(paths(result, 10), std::vector<std::string>{"transform.position"});
}

TEST(SceneDiff, PrefabMembers) {
    const std::filesystem::path file = std::filesystem::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime/prefabs/Enemy.prefab";
    const std::string text = editor::test::readText(file);
    SceneYamlModel prefab = model(text);
    EXPECT_EQ(prefab.kind, DocumentKind::Prefab);
    EXPECT_EQ(prefab.prefabRoot, 1001u);
    // the members' nested children are GameObjects below the root
    ASSERT_NE(prefab.find(1002), nullptr);
    EXPECT_EQ(prefab.find(1002)->name, "Body");
    EXPECT_EQ(prefab.find(1002)->parentId, 1001u);
    ASSERT_NE(prefab.find(1003), nullptr);
    EXPECT_EQ(prefab.find(1003)->siblingIndex, 1);
    EXPECT_TRUE(diffScenes(prefab, model(text)).empty());

    // a member moved
    std::string edited = text;
    const size_t at = edited.find("position: [0, 1, 0]");
    ASSERT_NE(at, std::string::npos);
    edited.replace(at, 19, "position: [0, 2, 0]");
    const SceneDiffResult result = diffScenes(prefab, model(edited));
    ASSERT_EQ(result.objects.size(), 1u);
    EXPECT_EQ(result.objects[0].id, 1002u);
    EXPECT_EQ(paths(result, 1002), std::vector<std::string>{"transform.position"});
    // and it encodes back to what it read
    std::string error;
    SceneYamlModel again;
    ASSERT_TRUE(SceneYamlModel::parse(prefab.text(), again, &error)) << error;
    EXPECT_TRUE(diffScenes(prefab, again).empty());
}

TEST(SceneDiff, PrefabInstanceOverridesShowOnTheirMembers) {
    auto instance = [](const std::string& position) {
        return std::string("    prefabInstance:\n      version: 5\n      memberIds:\n        - [1001, 20]\n        - [1002, 21]\n") +
               (position.empty() ? std::string() :
                "      overrides:\n        - registryEntity: 1002\n          properties:\n            transform:\n              - position\n"
                "          components:\n            transform:\n              position: " + position + "\n");
    };
    const std::string root = "      prefab:\n        name: Enemy\n        path: prefabs/Enemy.prefab\n        guid: e0e0e0e0e0e0e0e0e0e0e0e0e0e00001\n";
    const std::string a = scene(entity(20, "Enemy", "[0, 0, 0]", root + instance("")));
    const std::string b = scene(entity(20, "Enemy", "[0, 0, 0]", root + instance("[3, 0, 0]")));
    const SceneDiffResult result = diff(a, b);
    const ObjectDiff* member = result.find(21);
    ASSERT_NE(member, nullptr);
    EXPECT_TRUE(member->prefabMember);
    EXPECT_EQ(member->instanceRoot, 20u);
    EXPECT_EQ(member->registryEntity, 1002u);
    const ComponentDiff* transform = member->component("transform");
    ASSERT_NE(transform, nullptr);
    ASSERT_EQ(transform->properties.size(), 1u);
    EXPECT_EQ(transform->properties[0].path, "position");
    EXPECT_FALSE(transform->properties[0].inBase);
    EXPECT_EQ(transform->properties[0].currentValue, "[3, 0, 0]");
    // the override list changed too
    EXPECT_NE(member->component("prefabOverrides"), nullptr);
}
