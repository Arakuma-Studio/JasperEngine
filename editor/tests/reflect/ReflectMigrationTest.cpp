// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Renames and semantic changes of a component's YAML (docs/specs/S08-reflection.md R13, AC12), on
// a component of the test's own: a field read under its old key and an enum value under its old
// token load into the new ones and re-save under the canonical names; a migration step for format
// version N runs only on files older than N; a prefab override stored under the old path
// reaches the field; a hot reload keeps, converts or resets each value (R13, AC13).

#include "reflect/Field.h"
#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_migration_test {

    enum class Gait { WALK, RUN };

    struct Mover {
        float speed = 1.0f;
        Gait gait = Gait::WALK;
        int steps = 0;
    };

    constexpr const char* kRunAliases[] = {"sprint"};
    constexpr EnumItem kGaitItems[] = {
        {0, "walk", "Walk", "Gait::WALK"},
        {1, "run", "Run", "Gait::RUN", nullptr, kRunAliases},
    };
    const EnumDesc kGait{"Gait", kGaitItems, 0};

    constexpr const char* kSpeedAliases[] = {"velocity"};
    constexpr FieldDesc kMoverFields[] = {
        JE_FIELD(Mover, speed, .aliases = kSpeedAliases),
        JE_FIELD(Mover, gait, .enumDesc = &kGait),
        JE_FIELD(Mover, steps),
    };

    // format version 7 counts steps in pairs: older files' counts are halved
    void halveSteps(YAML::Node& node) {
        if (node["steps"]) node["steps"] = node["steps"].as<int>() / 2;
    }
    constexpr Migration kMoverMigrations[] = {{7, &halveSteps}};

    const ComponentDesc& moverDesc() {
        static const ComponentDesc desc{
            .type = ComponentType::Transform, .cppName = "Mover", .key = "mover", .displayName = "Mover", .icon = "",
            .body = JE_STRUCT_DESC(Mover, "Mover", kMoverFields),
            .hasEnabled = false, .structural = false,
            .find = nullptr, .add = nullptr, .remove = nullptr, .componentId = nullptr,
            .migrations = kMoverMigrations,
        };
        return desc;
    }

    TEST(ReflectMigration, AliasesLoadIntoTheNewNamesAndSaveCanonical) {
        Mover mover;
        DecodeReport report;
        decode(moverDesc(), YAML::Load("{velocity: 2.5, gait: sprint, steps: 3}"), &mover, DecodeContext{}, report);
        EXPECT_FLOAT_EQ(mover.speed, 2.5f);
        EXPECT_EQ(mover.gait, Gait::RUN);
        EXPECT_TRUE(report.skipped.empty());

        const YAML::Node saved = encode(moverDesc(), &mover, EncodeContext{});
        EXPECT_FALSE(saved["velocity"]);
        EXPECT_EQ(saved["speed"].as<std::string>(), "2.5");
        EXPECT_EQ(saved["gait"].as<std::string>(), "run");
        // the old key is a key the decode knows
        const std::vector<std::string>& keys = knownKeys(moverDesc());
        EXPECT_NE(std::find(keys.begin(), keys.end(), "velocity"), keys.end());
    }

    TEST(ReflectMigration, StepRunsOnlyForOlderFiles) {
        YAML::Node older = YAML::Load("{steps: 8}");
        EXPECT_EQ(migrate(moverDesc(), older, 6), 1);
        EXPECT_EQ(older["steps"].as<int>(), 4);

        YAML::Node current = YAML::Load("{steps: 8}");
        EXPECT_EQ(migrate(moverDesc(), current, 7), 0);
        EXPECT_EQ(current["steps"].as<int>(), 8);
    }

    TEST(ReflectMigration, OverrideUnderTheOldPathReachesTheField) {
        // prefab overrides and the clipboard name properties by path: the old one resolves too,
        // and is recorded under the field's own name
        Mover mover;
        const PropertyData old = property(moverDesc(), &mover, "velocity");
        ASSERT_NE(old.ref, nullptr);
        EXPECT_EQ(old.ref, &mover.speed);
        EXPECT_EQ(canonicalPath(moverDesc(), &mover, "velocity"), "speed");
        bool viaAlias = false;
        EXPECT_EQ(findField(moverDesc(), "velocity", &viaAlias), &kMoverFields[0]);
        EXPECT_TRUE(viaAlias);
    }

    // ---------------------------------------------------------------- hot reload (S23)

    struct Before {
        int speed = 0;
        std::string name;
        float scale = 0.0f;
        Vector2 offset;
    };
    struct After {
        float speed = 0.0f;
        AssetRef name;
        Vector3 scale = Vector3(7.0f, 7.0f, 7.0f);
        Vector3 offset;
        bool added = true;
    };

    constexpr FieldDesc kBeforeFields[] = {
        JE_FIELD(Before, speed), JE_FIELD(Before, name), JE_FIELD(Before, scale), JE_FIELD(Before, offset),
    };
    constexpr FieldDesc kAfterFields[] = {
        JE_FIELD(After, speed), JE_FIELD(After, name), JE_FIELD(After, scale), JE_FIELD(After, offset), JE_FIELD(After, added),
    };
    const StructDesc kBefore = JE_STRUCT_DESC(Before, "Before", kBeforeFields);
    const StructDesc kAfter = JE_STRUCT_DESC(After, "After", kAfterFields);

    FieldSet fieldsOf(const StructDesc& desc) {
        FieldSet set;
        set.owner = &desc;
        for (const FieldDesc& field : desc.fields) set.fields.push_back(&field);
        return set;
    }

    TEST(ReflectRemap, KeepsConvertsOrResets) {
        Before before;
        before.speed = 3;
        before.name = "textures/a.png";
        before.scale = 2.0f;
        before.offset = Vector2(1.0f, 2.0f);
        const FieldSet from = fieldsOf(kBefore);
        ValueSet values;
        for (const FieldDesc* field : from.fields) values.emplace_back(field->ops, field->ref(&before));

        const RemapResult result = remapByName(from, values, fieldsOf(kAfter));
        ASSERT_EQ(result.values.size(), 5u);
        // Int -> Float converted, String -> AssetPath converted, Vector2 widened
        EXPECT_FLOAT_EQ(*static_cast<const float*>(result.values[0].get()), 3.0f);
        EXPECT_EQ(static_cast<const AssetRef*>(result.values[1].get())->path, "textures/a.png");
        EXPECT_EQ(*static_cast<const Vector3*>(result.values[3].get()), Vector3(1.0f, 2.0f, 0.0f));
        // Float -> Vector3 does not convert: the default, and a report entry
        EXPECT_EQ(*static_cast<const Vector3*>(result.values[2].get()), Vector3(7.0f, 7.0f, 7.0f));
        // an added field: its default
        EXPECT_TRUE(*static_cast<const bool*>(result.values[4].get()));
        ASSERT_EQ(result.report.size(), 2u);
        EXPECT_NE(result.report[0].find("scale"), std::string::npos);
        EXPECT_NE(result.report[1].find("added"), std::string::npos);

        // same name and kind: kept
        const RemapResult same = remapByName(from, values, from);
        EXPECT_EQ(*static_cast<const int*>(same.values[0].get()), 3);
        EXPECT_EQ(*static_cast<const std::string*>(same.values[1].get()), "textures/a.png");
        EXPECT_TRUE(same.report.empty());
    }

}
