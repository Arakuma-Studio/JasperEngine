// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unknown data preserved through load and save (docs/specs/S03-data-safety.md R25-R27, R42,
// AC13, AC14)

#include "support/EditorTestSupport.h"
#include "component/LightComponent.h"
#include "component/Transform.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamPreserved.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // The 1-based line of the first line holding `text` in the fixture
    int lineOf(const std::string& file, const std::string& text) {
        std::istringstream in(file);
        std::string line;
        for (int number = 1; std::getline(in, line); number++) {
            if (line.find(text) != std::string::npos)
                return number;
        }
        return 0;
    }

    using IssueKey = std::tuple<LoadIssueKind, std::string, std::string, int, LoadIssueFate>;

    std::vector<IssueKey> keys(const LoadReport& report) {
        std::vector<IssueKey> result;
        for (const LoadIssue& issue : report.issues) {
            if (issue.fate != LoadIssueFate::Info)
                result.emplace_back(issue.kind, issue.component, issue.field, issue.line, issue.fate);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    const LoadIssue* findIssue(const LoadReport& report, const std::string& component, const std::string& field) {
        for (const LoadIssue& issue : report.issues) {
            if (issue.component == component && issue.field == field)
                return &issue;
        }
        return nullptr;
    }

    Entity entityNamed(const SceneProject& scene, const std::string& name) {
        for (Entity entity : scene.entities) {
            if (scene.scene->getEntityName(entity) == name)
                return entity;
        }
        return NULL_ENTITY;
    }

}

// AC13
TEST(Preservation, RoundTrip) {
    const std::string text = readText(fixture("data-safety/future.scene"));
    DecodedScene decoded = decodeSceneFile(fixture("data-safety/future.scene"));

    std::vector<IssueKey> expected = {
        {LoadIssueKind::UnknownRootKey, "", "lighting2", lineOf(text, "lighting2:"), LoadIssueFate::Preserved},
        {LoadIssueKind::InvalidValue, "transform", "position", lineOf(text, "position: [a, b]"), LoadIssueFate::Preserved},
        {LoadIssueKind::UnknownEnumValue, "light", "type", lineOf(text, "type: area"), LoadIssueFate::Preserved},
        {LoadIssueKind::UnknownField, "light", "areaSize", lineOf(text, "        areaSize:"), LoadIssueFate::Preserved},
        {LoadIssueKind::UnknownComponent, "jumpable", "", lineOf(text, "      jumpable:"), LoadIssueFate::Preserved},
        {LoadIssueKind::UnknownRootKey, "", "stance", lineOf(text, "    stance:"), LoadIssueFate::Preserved},
    };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(keys(decoded.report), expected);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    EXPECT_EQ(decoded.report.preserved(), 6u);

    const LoadIssue* type = findIssue(decoded.report, "light", "type");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->suggestion, "spot");
    EXPECT_EQ(type->value, "area");
    EXPECT_EQ(type->entityPath, "Player");
    EXPECT_EQ(type->describe(), "Player \xE2\x80\xBA light.type = \"area\": unknown value (did you mean \"spot\"?) \xE2\x80\x94 future.scene:" +
              std::to_string(type->line));
    EXPECT_EQ(type->consoleLine(), "future.scene:" + std::to_string(type->line) +
              ": light.type: unknown value \"area\" (did you mean \"spot\"?) (in Player) \xE2\x80\x94 kept");

    // the live values took their defaults; the hierarchy and the other fields are intact
    const Entity player = entityNamed(*decoded.scene, "Player");
    ASSERT_NE(player, NULL_ENTITY);
    EXPECT_EQ(decoded.scene->scene->getComponent<LightComponent>(player).type, LightType::DIRECTIONAL);
    EXPECT_FLOAT_EQ(decoded.scene->scene->getComponent<LightComponent>(player).intensity, 3.0f);
    EXPECT_EQ(decoded.scene->scene->getComponent<Transform>(player).position, Vector3(0, 0, 0));

    // nothing dropped: the save writes the file back as it was, with every unknown in place
    const YAML::Node saved = encodeScene(decoded);
    std::string why;
    EXPECT_TRUE(structurallyEqual(saved, YAML::Load(text), &why)) << why;
    EXPECT_EQ(saved.begin()->first.Scalar(), "formatVersion");

    // and again from what was saved: byte-identical
    DecodedScene again = decodeSceneText(DocumentFiles::text(saved), "future.scene");
    EXPECT_EQ(encodeSceneText(again), DocumentFiles::text(saved));
    EXPECT_EQ(again.report.preserved(), 6u);
}

// AC14
TEST(Preservation, Retire) {
    DecodedScene decoded = decodeSceneFile(fixture("data-safety/future.scene"));
    Scene* scene = decoded.scene->scene;
    const Entity player = entityNamed(*decoded.scene, "Player");
    ASSERT_NE(player, NULL_ENTITY);
    PreservedStore* store = findPreservedStore(scene);
    ASSERT_NE(store, nullptr);

    // still the load's live value: the authored one stands
    EXPECT_EQ(encodeScene(decoded)["entities"][0]["components"]["light"]["type"].Scalar(), "area");

    // the user picks a type: it wins and the stand-in retires
    scene->getComponent<LightComponent>(player).type = LightType::POINT;
    EXPECT_EQ(encodeScene(decoded)["entities"][0]["components"]["light"]["type"].Scalar(), "point");
    EXPECT_FALSE(store->holds(player, "light", "type"));
    EXPECT_EQ(decoded.report.outstandingPreserved(store), 5u);
    // the unknown field of the same component is still written
    EXPECT_TRUE(encodeScene(decoded)["entities"][0]["components"]["light"]["areaSize"]);

    // duplicate: the copy carries the unknown component too
    const YAML::Node copy = Stream::encodeEntity(player, scene);
    std::vector<Entity> made = Stream::decodeEntity(copy, scene, &decoded.scene->entities, nullptr, nullptr, NULL_ENTITY, true);
    ASSERT_FALSE(made.empty());
    EXPECT_NE(made[0], player);
    EXPECT_TRUE(store->holds(made[0], "jumpable", ""));
    EXPECT_TRUE(Stream::encodeEntity(made[0], scene)["components"]["jumpable"]);

    // delete, then undo (the snapshot decoded back): the unknown data comes back with it
    const YAML::Node snapshot = Stream::encodeEntity(made[0], scene);
    scene->destroyEntity(made[0]);
    EXPECT_FALSE(store->holds(made[0], "jumpable", ""));
    std::vector<Entity> restored = Stream::decodeEntity(snapshot, scene, nullptr, nullptr, nullptr, NULL_ENTITY, false);
    ASSERT_FALSE(restored.empty());
    EXPECT_EQ(restored[0], made[0]);
    EXPECT_TRUE(store->holds(restored[0], "jumpable", ""));
    EXPECT_EQ(Stream::encodeEntity(restored[0], scene)["stance"].Scalar(), "crouched");

    // removing the unknown component drops it from the save
    EXPECT_TRUE(store->removePreservedComponent(player, "jumpable"));
    EXPECT_FALSE(encodeScene(decoded)["entities"][0]["components"]["jumpable"]);

    // removing a known component retires its preserved fields
    scene->removeComponent<LightComponent>(player);
    EXPECT_FALSE(store->holds(player, "light", "areaSize"));
}

TEST(Preservation, SingleComponentCopiesLeaveUnknownComponentsOut) {
    DecodedScene decoded = decodeSceneFile(fixture("data-safety/future.scene"));
    Scene* scene = decoded.scene->scene;
    const Entity player = entityNamed(*decoded.scene, "Player");
    const YAML::Node light = Stream::encodeComponents(player, scene, Catalog::componentTypeToSignature(scene, ComponentType::LightComponent));
    EXPECT_TRUE(light["light"]["areaSize"]);
    EXPECT_FALSE(light["jumpable"]);
}

TEST(Preservation, NoReportStillPreserves) {
    ensureEngine();
    // a clipboard paste: no report installed, the unknown data still travels
    Scene scene;
    const YAML::Node node = YAML::Load("{type: Entity, entity: 1001, name: Box, glow: 3, components: {wobble: {speed: 1}}}");
    std::vector<Entity> made = Stream::decodeEntity(node, &scene, nullptr, nullptr, nullptr, NULL_ENTITY, true);
    ASSERT_EQ(made.size(), 1u);
    const YAML::Node encoded = Stream::encodeEntity(made[0], &scene);
    EXPECT_EQ(encoded["glow"].Scalar(), "3");
    EXPECT_EQ(encoded["components"]["wobble"]["speed"].Scalar(), "1");
}
