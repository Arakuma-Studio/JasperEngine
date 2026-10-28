// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset locators (docs/specs/S08-reflection.md R12, AC11): every asset field below the components
// of tools/fixtures/reflection-parity/AllComponents.scene, nested structs and arrays included,
// gives the locator list in editor/tests/reflect/locators_expected.txt (JASPER_TEST_UPDATE_GOLDEN=1
// rewrites it), and each locator resolves back to the field it names.

#include "support/EditorTestSupport.h"

#include "Catalog.h"
#include "reflect/Reflect.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_locators_test {

    struct Visit {
        std::string locator;
        reflect::AssetKind kind;
        void* ref;
    };

    void collect(void* user, const std::string& locator, reflect::AssetKind kind, void* ref, const reflect::FieldDesc&) {
        static_cast<std::vector<Visit>*>(user)->push_back({locator, kind, ref});
    }

    TEST(ReflectLocators, AllComponentsScene) {
        const fs::path scenePath = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "reflection-parity" / "AllComponents.scene";
        const fs::path expectedPath = fs::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "reflect" / "locators_expected.txt";
        DecodedScene decoded = decodeSceneFile(scenePath);
        Scene* registry = decoded.scene->scene;

        std::string listed;
        size_t count = 0;
        for (Entity entity : decoded.scene->entities) {
            for (ComponentType type : Catalog::findComponents(registry, entity)) {
                const reflect::ComponentDesc& desc = *reflect::find(type);
                std::vector<Visit> visits;
                reflect::visitAssetRefs(desc, desc.find(registry, entity), &collect, &visits);
                for (const Visit& visit : visits) {
                    listed += std::to_string(entity) + " " + visit.locator + " " + reflect::assetKindName(visit.kind) + "\n";
                    // the locator names the field it came from
                    const reflect::ComponentDesc* found = nullptr;
                    const reflect::FieldRef back = reflect::resolveLocator(registry, entity, visit.locator, &found);
                    EXPECT_EQ(found, &desc) << visit.locator;
                    EXPECT_EQ(back.ref, visit.ref) << visit.locator;
                    count++;
                }
            }
        }
        EXPECT_GT(count, 20u);
        if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
            std::ofstream(expectedPath, std::ios::binary) << listed;
        }
        std::stringstream expected;
        expected << std::ifstream(expectedPath, std::ios::binary).rdbuf();
        EXPECT_EQ(listed, expected.str());
    }

    TEST(ReflectLocators, ResolveThroughAliases) {
        // a locator written with an older field name still resolves (R12)
        Scene scene;
        std::vector<Entity> entities;
        const Entity entity = scene.createEntity();
        entities.push_back(entity);
        const reflect::ComponentDesc& desc = *reflect::find(ComponentType::LayoutGroupComponent);
        reflect::Value value(desc.body.ops);
        desc.add(&scene, entity, value.get());
        const reflect::FieldRef ref = reflect::resolveLocator(&scene, entity, "layoutgroup/fillAvailableSpace");
        ASSERT_TRUE(ref);
        EXPECT_TRUE(ref.viaAlias);
        EXPECT_STREQ(ref.field->name, "useAllWrapSpace");
    }

}
