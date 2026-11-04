// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The known-key and enum tables stay honest (docs/specs/S03-data-safety.md R24, AC12): every
// key an encoder writes is listed, every enum name reads back as itself. Every key a decoder
// reads: tools/check_stream_keys.py.

#include "support/EditorTestSupport.h"
#include "component/ScriptComponent.h"
#include "component/FogComponent.h"

#include "Stream.h"
#include "StreamPreserved.h"
#include "StreamSchema.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    void expectKnownComponentKeys(const YAML::Node& components, const std::string& where) {
        for (auto it = components.begin(); it != components.end(); ++it) {
            const std::string name = it->first.Scalar();
            ComponentType type;
            ASSERT_TRUE(StreamSchema::findComponentType(name, type)) << where << ": " << name;
            if (!it->second.IsMap()) continue;
            for (auto field = it->second.begin(); field != it->second.end(); ++field) {
                EXPECT_TRUE(StreamSchema::isKnownFieldKey(type, field->first.Scalar()))
                    << where << ": " << name << "." << field->first.Scalar() << " is written but not in knownFieldKeys";
            }
        }
    }

}

TEST(StreamSchema, EncoderKeysAreKnown) {
    ensureEngine();
    // every component the editor can add, as it is added (the encoders write their full set)
    Scene scene;
    std::vector<Entity> entities;
    for (int typeIndex = (int)ComponentType::Transform; typeIndex <= (int)LAST_COMPONENT_TYPE; typeIndex++) {
        const ComponentType type = (ComponentType)typeIndex;
        const Entity entity = scene.createUserEntity();
        entities.push_back(entity);
        ProjectUtils::addEntityComponent(&scene, entity, type, entities);
        const YAML::Node components = Stream::encodeComponentsLive(entity, &scene, scene.getSignature(entity));
        expectKnownComponentKeys(components, Catalog::getComponentName(type, true));
    }

    // and with the values of real scenes (live values only: their unknown data is not the encoders')
    for (const char* file : {"data-safety/future.scene", "migrations/scene-v0.scene"}) {
        DecodedScene decoded = decodeSceneFile(fixture(file));
        Scene* decodedScene = decoded.scene->scene;
        for (Entity entity : decoded.scene->entities) {
            expectKnownComponentKeys(Stream::encodeComponentsLive(entity, decodedScene, decodedScene->getSignature(entity)), file);
        }
    }
}

TEST(StreamSchema, EveryComponentHasATable) {
    for (int typeIndex = (int)ComponentType::Transform; typeIndex <= (int)LAST_COMPONENT_TYPE; typeIndex++) {
        EXPECT_FALSE(StreamSchema::knownFieldKeys((ComponentType)typeIndex).empty()) << Catalog::getComponentName((ComponentType)typeIndex, true);
    }
    for (NestedKind kind : {NestedKind::Submesh, NestedKind::Material, NestedKind::MaterialPass, NestedKind::Stencil, NestedKind::Texture}) {
        EXPECT_FALSE(StreamSchema::knownNestedKeys(kind).empty()) << StreamSchema::nestedKindName(kind);
    }
    EXPECT_TRUE(StreamSchema::isKnownRootKey(RootKind::Scene, "formatVersion"));
    EXPECT_TRUE(StreamSchema::isKnownRootKey(RootKind::Material, "baseColorFactor"));
    EXPECT_FALSE(StreamSchema::isKnownRootKey(RootKind::Entity, "stance"));
}

TEST(StreamSchema, EnumNamesReadBack) {
    auto check = [](const auto& table) {
        for (const std::string& name : table.names()) {
            EXPECT_EQ(table.toString(table.fromString(name)), name) << table.typeName();
        }
        EXPECT_EQ(table.fromString("certainly-not-a-name"), table.fallback()) << table.typeName();
    };
    check(StreamSchema::lightType());
    check(StreamSchema::easeType());
    check(StreamSchema::materialBlendMode());
    check(StreamSchema::textureFilter());
    check(StreamSchema::collider3DType());
    check(StreamSchema::pivotPreset());
    check(StreamSchema::uiEventState());
    check(StreamSchema::scalingMode());
    check(StreamSchema::terrainBrushMode());
    check(StreamSchema::materialPassOrder());
    // aliases read, and write as the current name
    EXPECT_EQ(StreamSchema::materialBlendMode().fromString("OPAQUE"), MaterialBlendMode::NONE);
    EXPECT_EQ(StreamSchema::fogType().fromString("exponentialSquared"), FogType::EXPONENTIALSQUARED);
    EXPECT_EQ(StreamSchema::scriptType().fromString("script_lua"), ScriptType::LUA);
    EXPECT_EQ(StreamSchema::easeType().fromString("quad-in"), EaseType::QUAD_IN);
    EXPECT_EQ(Stream::stringToEaseType("quad-in"), EaseType::QUAD_IN);
}

TEST(StreamSchema, UnknownEnumOutsideAComponentIsDropped) {
    // a nested value (a submesh's material) cannot be kept in v1: reported as dropped
    LoadReport report;
    {
        ScopedLoadReport scope(report);
        const YAML::Node node = YAML::Load("{blendMode: SUPER}");
        StreamLoad::BlockScope block(nullptr, "", node, RootKind::Material);
        EXPECT_EQ(decodeEnum(node, "blendMode", StreamSchema::materialBlendMode()), MaterialBlendMode::AUTO);
    }
    ASSERT_EQ(report.issues.size(), 1u);
    EXPECT_EQ(report.issues[0].kind, LoadIssueKind::UnknownEnumValue);
    EXPECT_EQ(report.issues[0].fate, LoadIssueFate::Dropped);
    EXPECT_EQ(report.issues[0].value, "SUPER");
}
