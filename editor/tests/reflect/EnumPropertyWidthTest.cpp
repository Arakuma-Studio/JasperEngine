// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An Enum property is read and written at its own width (Catalog.h's PropertyData::size). A
// Title Art Renderer's Transition is a one-byte enum and its Transition Duration float starts
// inside the four bytes an int would cover: every path that took an Enum for an int (the
// Inspector's and MCP's PropertyCmd<int> and its undo, a prefab's copy and compare of a
// property, MCP's read) wrote over or read that float.

#include "support/EditorTestSupport.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "component/Transform.h"
#include "mcp/McpValues.h"
#include "project/ProjectPrefabInternal.h"
#include "reflect/ReflectTestSupport.h"
#include "reflect/Reflect.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace enum_property_width_test {

    constexpr ComponentType TITLE = ComponentType::TitleArtRendererComponent;

    // the float the test watches lies inside the int an Enum used to be read and written as
    bool durationWithinAnIntOfTheTransition(const TitleArtRendererComponent& title) {
        const ptrdiff_t gap = reinterpret_cast<const char*>(&title.transitionDuration) - reinterpret_cast<const char*>(&title.transition);
        return sizeof(title.transition) == 1 && gap > 0 && gap < static_cast<ptrdiff_t>(sizeof(int));
    }

    uint32_t bitsOf(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    Entity addTitle(Scene& scene, TitleArtTransition transition, float duration) {
        const Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        TitleArtRendererComponent title;
        title.transition = transition;
        title.transitionDuration = duration;
        scene.addComponent<TitleArtRendererComponent>(entity, title);
        return entity;
    }

    // tools/fixtures/prefab-runtime copied to a temp dir, loaded as the CLI's scene-dump loads one
    class EnumPropertyWidthTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"enum-width"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            project->setSelectedSceneId(scene->id);
        }

        void TearDown() override {
            if (project) closeProject(*project);
            project.reset();
        }
    };

}

using namespace enum_property_width_test;

TEST_F(EnumPropertyWidthTest, AnEditAndItsUndoLeaveTheNextFieldAlone) {
    const Entity entity = addTitle(*scene->scene, TitleArtTransition::CUT, 0.15f);
    const TitleArtRendererComponent& title = scene->scene->getComponent<TitleArtRendererComponent>(entity);
    ASSERT_TRUE(durationWithinAnIntOfTheTransition(title)) << "the layout moved: watch another one-byte enum with a neighbour";
    const uint32_t duration = bitsOf(title.transitionDuration);

    CommandHistory* history = CommandHandle::get(scene->id);
    history->addCommandNoMerge(new PropertyCmd<int>(project.get(), scene->id, entity, TITLE, "transition", (int)TitleArtTransition::CROSSFADE));
    EXPECT_EQ(title.transition, TitleArtTransition::CROSSFADE);
    EXPECT_EQ(bitsOf(title.transitionDuration), duration) << "the edit wrote over Transition Duration";

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(title.transition, TitleArtTransition::CUT);
    EXPECT_EQ(bitsOf(title.transitionDuration), duration);
}

TEST(EnumPropertyWidth, CopyCompareAndReadSeeOnlyTheEnumsByte) {
    Scene source;
    const Entity from = addTitle(source, TitleArtTransition::CROSSFADE, 2.0f);
    Scene target;
    const Entity to = addTitle(target, TitleArtTransition::CUT, 0.15f);
    const TitleArtRendererComponent& copied = target.getComponent<TitleArtRendererComponent>(to);
    ASSERT_TRUE(durationWithinAnIntOfTheTransition(copied));

    // MCP's read: the transition alone, not the float's bytes after it
    mcp::McpValues::ReadContext ctx;
    ctx.component = TITLE;
    EXPECT_EQ(mcp::McpValues::readProperty(ctx, "transition", Catalog::findProperty(&source, from, TITLE, "transition")), mcp::Json("crossfade"));

    // a prefab override's copy of one property
    Catalog::copyPropertyValue(&source, from, &target, to, TITLE, "transition");
    EXPECT_EQ(copied.transition, TitleArtTransition::CROSSFADE);
    EXPECT_EQ(bitsOf(copied.transitionDuration), bitsOf(0.15f)) << "the copy wrote over Transition Duration";

    // what a prefab instance overrides: the duration, not the transition both hold
    Scene asset;
    const Entity assetRoot = addTitle(asset, TitleArtTransition::CUT, 0.15f);
    Scene instance;
    const Entity instanceRoot = addTitle(instance, TitleArtTransition::CUT, 1.0f);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, instanceRoot, TITLE),
              std::vector<std::string>{"transitionDuration"});
}

// Every Enum property of every component, the ones made by hand included: its width is its
// field's, and a write there changes no other byte of the component
TEST(EnumPropertyWidth, EveryEnumPropertyWritesOnlyItsOwnBytes) {
    size_t narrow = 0;
    // (a camera made here owns the framebuffer its constructor allocates)
    OwnedFramebuffers owned;
    for (const reflect::ComponentDesc* desc : reflect::listingOrder()) {
        reflect::Value value(desc->body.ops);
        if (desc->type == ComponentType::CameraComponent) owned.keep(static_cast<const CameraComponent*>(value.get()));
        unsigned char* begin = static_cast<unsigned char*>(value.get());
        const size_t bytes = desc->body.ops->size;
        for (const auto& [name, property] : Catalog::getProperties(desc->type, value.get())) {
            if (property.type != PropertyType::Enum || !property.ref) continue;
            const std::string where = std::string(desc->cppName) + "." + name;
            const reflect::FieldRef field = reflect::resolve(*desc, value.get(), name);
            if (field) {
                EXPECT_EQ(property.size, field.ops->size) << where;
            }
            const size_t width = property.size ? property.size : sizeof(int);
            if (width < sizeof(int)) narrow++;

            unsigned char* at = static_cast<unsigned char*>(property.ref);
            const bool inside = at >= begin && at + width <= begin + bytes;
            std::vector<unsigned char> before(begin, begin + bytes);
            const int64_t old = readPropertyInteger(property, property.ref);
            writePropertyInteger(property, property.ref, old ^ 1);
            EXPECT_EQ(readPropertyInteger(property, property.ref), old ^ 1) << where;
            if (inside) {
                std::vector<unsigned char> after(begin, begin + bytes);
                const size_t offset = static_cast<size_t>(at - begin);
                for (size_t i = 0; i < bytes; i++) {
                    if (i >= offset && i < offset + width) continue;
                    if (after[i] != before[i]) {
                        ADD_FAILURE() << where << " changed byte " << i << " of the component (it is at " << offset << ", " << width << " wide)";
                        break;
                    }
                }
            }
            writePropertyInteger(property, property.ref, old);
        }
    }
    EXPECT_GT(narrow, 0u) << "no Enum narrower than an int: the sweep proves nothing";
}
