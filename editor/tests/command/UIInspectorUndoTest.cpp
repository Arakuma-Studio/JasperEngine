// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S93 AC42: the Inspector edits of the UI Document (Update Mode, Render Mode World Space, Pixels
// Per Unit, Billboard) and of the Text (Alignment, Auto Size, Min, Max) are each one undo step:
// undo gives the serialized component back, redo the edited one. AC44: their keys are optional,
// an old scene's components load with the defaults.

#include "UndoFakeCommand.h"

#include "Project.h"
#include "Stream.h"
#include "command/type/PropertyCmd.h"
#include "component/RectTransformComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "reflect/Reflect.h"
#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace ui_inspector_undo_test {

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)

    constexpr Entity GAME = 1001;   // a plain GameObject of the fixture's Main Scene

    class UIInspectorUndoTest : public ::testing::Test {
    protected:
        UndoStatics statics;
        jasperengine::test::TestTempDir dir{"ui-inspector-undo"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene)
                    project->loadScene(candidate.filepath, false, false, true);
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

        CommandHistory* history() { return CommandHandle::get(scene->id); }

        // the component as the scene file writes it
        std::string encoded(Entity entity, ComponentType type) {
            const reflect::ComponentDesc* desc = reflect::find(type);
            EXPECT_NE(desc, nullptr);
            reflect::EncodeContext ctx;
            ctx.registry = scene->scene;
            ctx.entity = entity;
            ctx.signature = scene->scene->getSignature(entity);
            const void* component = type == ComponentType::TextComponent ? (const void*)&scene->scene->getComponent<TextComponent>(entity)
                                                                         : (const void*)&scene->scene->getComponent<UIDocumentComponent>(entity);
            return YAML::Dump(reflect::encode(*desc, component, ctx));
        }

        // one Inspector edit: a command, then undo and redo round trips of the encoded component
        template<typename T>
        void roundTrip(Entity entity, ComponentType type, const char* property, const T& value, const char* key) {
            const std::string before = encoded(entity, type);
            EXPECT_EQ(before.find(key), std::string::npos) << key << " is written only away from its default";
            history()->clear();
            history()->addCommandNoMerge(new PropertyCmd<T>(project.get(), scene->id, entity, type, property, value));
            EXPECT_EQ(history()->entries().size(), 1u) << property << ": one undo step";
            const std::string after = encoded(entity, type);
            EXPECT_NE(after, before) << property;
            EXPECT_NE(after.find(key), std::string::npos) << property;
            ASSERT_TRUE(history()->undo()) << property;
            EXPECT_EQ(encoded(entity, type), before) << property << " undo";
            ASSERT_TRUE(history()->redo()) << property;
            EXPECT_EQ(encoded(entity, type), after) << property << " redo";
            ASSERT_TRUE(history()->undo()) << property;
        }
    };

TEST_F(UIInspectorUndoTest, UIDocumentFieldsAreUndoable) {
    Scene* registry = scene->scene;
    registry->addComponent<UIDocumentComponent>(GAME, UIDocumentComponent{});
    history()->clear();

    roundTrip<int>(GAME, ComponentType::UIDocumentComponent, "updateMode", (int)UIDocumentUpdateMode::GAME_TIME, "updateMode");
    roundTrip<int>(GAME, ComponentType::UIDocumentComponent, "renderMode", (int)UIDocumentRenderMode::WORLD, "renderMode");
    roundTrip<float>(GAME, ComponentType::UIDocumentComponent, "pixelsPerUnit", 64.0f, "pixelsPerUnit");
    roundTrip<bool>(GAME, ComponentType::UIDocumentComponent, "billboard", true, "billboard");

    // what the scene file says for World Space
    history()->addCommandNoMerge(new PropertyCmd<int>(project.get(), scene->id, GAME, ComponentType::UIDocumentComponent, "renderMode",
                                                      (int)UIDocumentRenderMode::WORLD));
    EXPECT_NE(encoded(GAME, ComponentType::UIDocumentComponent).find("renderMode: world"), std::string::npos);
}

TEST_F(UIInspectorUndoTest, TextAlignmentAndAutoSizeAreUndoable) {
    Scene* registry = scene->scene;
    registry->addComponent<RectTransformComponent>(GAME, RectTransformComponent{});
    registry->addComponent<UIComponent>(GAME, UIComponent{});
    registry->addComponent<TextComponent>(GAME, TextComponent{});
    history()->clear();

    roundTrip<int>(GAME, ComponentType::TextComponent, "horizontalAlignment", (int)TextHorizontalAlignment::JUSTIFIED, "horizontalAlignment");
    roundTrip<int>(GAME, ComponentType::TextComponent, "verticalAlignment", (int)TextVerticalAlignment::BASELINE, "verticalAlignment");
    roundTrip<bool>(GAME, ComponentType::TextComponent, "autoSize", true, "autoSize");
    roundTrip<unsigned int>(GAME, ComponentType::TextComponent, "fontSizeMin", 12u, "fontSizeMin");
    roundTrip<unsigned int>(GAME, ComponentType::TextComponent, "fontSizeMax", 40u, "fontSizeMax");
}

// AC44: a scene written before S93 (none of the new keys) decodes with the defaults
TEST_F(UIInspectorUndoTest, OldComponentsLoadWithTheDefaults) {
    const reflect::ComponentDesc* document = reflect::find(ComponentType::UIDocumentComponent);
    const reflect::ComponentDesc* text = reflect::find(ComponentType::TextComponent);
    ASSERT_NE(document, nullptr);
    ASSERT_NE(text, nullptr);

    UIDocumentComponent decodedDocument;
    decodedDocument.updateMode = UIDocumentUpdateMode::GAME_TIME;
    decodedDocument.pixelsPerUnit = 7.0f;
    decodedDocument.billboard = true;
    reflect::DecodeContext ctx;
    reflect::DecodeReport report;
    reflect::decode(*document, YAML::Load("filename: ui/menu.html\nsortOrder: 2\nrenderMode: texture\n"), &decodedDocument, ctx, report);
    EXPECT_EQ(decodedDocument.renderMode, UIDocumentRenderMode::TEXTURE);
    EXPECT_EQ(decodedDocument.updateMode, UIDocumentUpdateMode::UNSCALED_TIME);
    EXPECT_FLOAT_EQ(decodedDocument.pixelsPerUnit, 100.0f);
    EXPECT_FALSE(decodedDocument.billboard);

    TextComponent decodedText;
    decodedText.horizontalAlignment = TextHorizontalAlignment::RIGHT;
    decodedText.autoSize = true;
    reflect::decode(*text, YAML::Load("text: Hello\nfontSize: 30\nfixedWidth: true\n"), &decodedText, ctx, report);
    EXPECT_EQ(decodedText.horizontalAlignment, TextHorizontalAlignment::LEFT);
    EXPECT_EQ(decodedText.verticalAlignment, TextVerticalAlignment::TOP);
    EXPECT_FALSE(decodedText.autoSize);
    EXPECT_EQ(decodedText.fontSizeMin, 18u);
    EXPECT_EQ(decodedText.fontSizeMax, 72u);
    EXPECT_EQ(decodedText.fontSize, 30u);
}

// AC44: a scene file written before S93 (scene format 8, no new key) loads with the defaults and
// saves without the new keys; no migration
TEST_F(UIInspectorUndoTest, OldSceneLoadsWithTheDefaults) {
    const fs::path path = fixture("migrations/scene-v8-ui-before-s93.scene");
    DecodedScene decoded = decodeSceneFile(path);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    Scene& loaded = *decoded.scene->scene;
    Entity menu = NULL_ENTITY, title = NULL_ENTITY;
    for (Entity e : decoded.scene->entities) {
        if (loaded.getEntityName(e) == "Menu") menu = e;
        if (loaded.getEntityName(e) == "Title") title = e;
    }
    ASSERT_NE(menu, NULL_ENTITY);
    ASSERT_NE(title, NULL_ENTITY);
    const UIDocumentComponent& document = loaded.getComponent<UIDocumentComponent>(menu);
    EXPECT_EQ(document.renderMode, UIDocumentRenderMode::TEXTURE);
    EXPECT_EQ(document.updateMode, UIDocumentUpdateMode::UNSCALED_TIME);
    EXPECT_FLOAT_EQ(document.pixelsPerUnit, 100.0f);
    EXPECT_FALSE(document.billboard);
    const TextComponent& text = loaded.getComponent<TextComponent>(title);
    EXPECT_EQ(text.horizontalAlignment, TextHorizontalAlignment::LEFT);
    EXPECT_EQ(text.verticalAlignment, TextVerticalAlignment::TOP);
    EXPECT_FALSE(text.autoSize);

    // the components as saved: none of the new keys (written only away from their defaults)
    const YAML::Node saved = encodeScene(decoded);
    for (const YAML::Node& entity : saved["entities"]) {
        const YAML::Node components = entity["components"];
        for (const char* key : {"updateMode", "pixelsPerUnit", "billboard"})
            EXPECT_FALSE(components["uidocument"] && components["uidocument"][key]) << key;
        for (const char* key : {"horizontalAlignment", "verticalAlignment", "autoSize", "fontSizeMin", "fontSizeMax"})
            EXPECT_FALSE(components["text"] && components["text"][key]) << key;
    }
}

}
