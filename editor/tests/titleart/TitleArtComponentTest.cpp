// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Art Renderer (docs/specs/S81-title-art-runtime.md AC1, AC2, AC20 Baked part): its S08
// table (defaults omitted, round trip), the companion it adds, Set Native Size, the companion's
// own values while a Live Effect drives it and after the component goes, the overrides as
// properties (prefab copies by name, set_properties by name), and the Lua sentences.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "reflect/Reflect.h"
#include "subsystem/TitleArtSystem.h"
#include "support/EditorTestSupport.h"
#include "titleart/TitleArtIO.h"
#include "titleart/runtime/TitleArtOverrides.h"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        std::unique_ptr<Project> project;
        fs::path dir;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            test::ensureEngine();
            const fs::path root = fs::temp_directory_path() / ("jasper-titleart-component-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "titles");
            // the default assets directory (the project's root): components name project-relative
            // paths ("assets/titles/logo.titleart"), as MCP's tools and the Inspector do
            std::ofstream(out.dir / "project.yaml") << "name: titleartcomponent\n";
            // a document exposing its text and a number, and the sidecar of its bake
            titleart::TitleArtDoc doc = titleart::makeNewTitleArt({800, 400, "JACKPOT", ""});
            titleart::Json number = {{"name", "glow_amount"}, {"label", "Glow Amount"}, {"type", "number"}, {"min", 0}, {"max", 1},
                                     {"targets", titleart::Json::array({{{"layer", doc.layers()[0]["id"]}, {"field", "opacity"}}})}};
            doc.root["exposed"].push_back(number);
            titleart::normalizeTitleArt(doc);
            std::ofstream(out.dir / "assets" / "titles" / "logo.titleart") << titleart::writeTitleArt(doc);
            std::ofstream(out.dir / "assets" / "titles" / "logo.titleart.bake.json")
                << R"({"format": "jasperengine.titleart.bakeinfo", "version": 1, "frame": {"x": 200, "y": 100, "w": 400, "h": 200},
                      "origin": [100, 150], "outputs": [{"id": "main", "file": "logo.titleart.png", "content": "all", "size": [400, 200]}],
                      "anchors": [{"x": 0.5, "y": 0.5, "size": 1, "strength": 1, "fade": 1}],
                      "alpha_bounds": {"x": 210, "y": 110, "w": 380, "h": 180}, "source_hash": "00", "bake_version": 1})";
            AppSettings::initialize();
            out.project.reset(new Project());
            test::useProjectPaths(out.project.get());
            out.ready = out.project->loadProject(out.dir, false);
            test::releaseAtExit(out.project.get());
            return out;
        }();
        // the test System drops the project's paths at the end of every test
        if (h.project) test::useProjectPaths(h.project.get());
        return h;
    }

    uint32_t newScene(SceneType type) {
        Project* p = headless().project.get();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Titles", type);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return headless().project->getScene(sceneId)->scene; }

    const reflect::ComponentDesc& desc() { return *reflect::find(ComponentType::TitleArtRendererComponent); }

    YAML::Node encodeTitle(const TitleArtRendererComponent& title) {
        return reflect::encode(desc(), &title, reflect::EncodeContext{});
    }

    TitleArtRendererComponent roundTrip(const TitleArtRendererComponent& title) {
        TitleArtRendererComponent out;
        reflect::DecodeReport report;
        reflect::decode(desc(), YAML::Load(YAML::Dump(encodeTitle(title))), &out, reflect::DecodeContext{}, report);
        return out;
    }

    // an Image with a Title Art Renderer, as GameObject > UI > Title Art makes it
    Entity makeUITitle(uint32_t sceneId, const std::string& title) {
        CreateEntityCmd* create = new CreateEntityCmd(headless().project.get(), sceneId, "Logo", EntityCreationType::TITLE_ART_UI);
        create->setQuiet(true);
        create->setTitleArt(title);
        CommandHandle::get(sceneId)->addCommandNoMerge(create);
        return create->getEntity();
    }

}

TEST(TitleArtComponent, DefaultsAreOmittedAndValuesRoundTrip) {
    ASSERT_TRUE(headless().ready);
    const TitleArtRendererComponent defaults;
    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.renderMode, TitleArtRenderMode::BAKED);
    EXPECT_TRUE(defaults.pivotFromTitle);
    EXPECT_FLOAT_EQ(defaults.resolutionScale, 1.0f);
    EXPECT_EQ(defaults.atlasPageSize, 1024);
    EXPECT_EQ(defaults.atlasMaxPages, 4);
    EXPECT_EQ(defaults.qualityTier, "auto");
    EXPECT_FLOAT_EQ(defaults.sweepWidth, 0.15f);
    EXPECT_FLOAT_EQ(defaults.glintSize, 0.06f);

    const YAML::Node encoded = encodeTitle(defaults);
    // only the enabled checkbox: every other field at its default is omitted (R58)
    for (auto it = encoded.begin(); it != encoded.end(); ++it) EXPECT_EQ(it->first.as<std::string>(), "enabled") << YAML::Dump(encoded);

    TitleArtRendererComponent title;
    title.title = AssetRef("assets/titles/logo.titleart");
    title.renderMode = TitleArtRenderMode::REALTIME;
    title.overrides.push_back({"text", "text", Vector4(0, 0, 0, 0), "WIN"});
    title.overrides.push_back({"glow_amount", "number", Vector4(0.25f, 0, 0, 0), ""});
    title.sweepEnabled = true;
    title.sweepColor = Vector4(0.5f, 0.25f, 1.0f, 0.75f);
    title.transition = TitleArtTransition::CROSSFADE;
    title.runtimeEnvironments = {"studio"};
    const TitleArtRendererComponent back = roundTrip(title);
    EXPECT_EQ(back.title.path, title.title.path);
    EXPECT_EQ(back.renderMode, TitleArtRenderMode::REALTIME);
    ASSERT_EQ(back.overrides.size(), 2u);
    EXPECT_EQ(back.overrides[0], title.overrides[0]);
    EXPECT_EQ(back.overrides[1], title.overrides[1]);
    EXPECT_TRUE(back.sweepEnabled);
    EXPECT_EQ(back.sweepColor, title.sweepColor);
    EXPECT_EQ(back.transition, TitleArtTransition::CROSSFADE);
    EXPECT_EQ(back.runtimeEnvironments, title.runtimeEnvironments);
    EXPECT_EQ(encodeTitle(title)["renderMode"].as<std::string>(), "realtime");
}

TEST(TitleArtComponent, AddingToAnEmptyGameObjectAddsTheScenesCompanionInOneStep) {
    ASSERT_TRUE(headless().ready);
    for (SceneType type : {SceneType::SCENE_UI, SceneType::SCENE_2D}) {
        const uint32_t sceneId = newScene(type);
        CreateEntityCmd* create = new CreateEntityCmd(headless().project.get(), sceneId, "Empty", EntityCreationType::GAMEOBJECT);
        create->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new AddComponentCmd(headless().project.get(), sceneId, entity, ComponentType::TitleArtRendererComponent));
        Scene* scene = sceneOf(sceneId);
        ASSERT_TRUE(scene->findComponent<TitleArtRendererComponent>(entity));
        if (type == SceneType::SCENE_UI) {
            EXPECT_TRUE(scene->findComponent<UIComponent>(entity) && scene->findComponent<ImageComponent>(entity)) << "R1: an Image in a UI scene";
            EXPECT_EQ(TitleArtSystem::spaceOf(scene, entity), titleart::TitleArtSpace::UI);
        } else {
            EXPECT_TRUE(scene->findComponent<MeshComponent>(entity) && scene->findComponent<SpriteComponent>(entity)) << "R1: a Sprite elsewhere";
            EXPECT_EQ(TitleArtSystem::spaceOf(scene, entity), titleart::TitleArtSpace::World);
        }
        ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
        EXPECT_FALSE(scene->findComponent<TitleArtRendererComponent>(entity));
        EXPECT_FALSE(scene->findComponent<ImageComponent>(entity)) << "one undo step takes the companion too";
        EXPECT_FALSE(scene->findComponent<SpriteComponent>(entity));
    }
}

TEST(TitleArtComponent, SetNativeSizeUsesTheBakeFrameAndOrigin) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene(SceneType::SCENE_UI);
    const Entity entity = makeUITitle(sceneId, "assets/titles/logo.titleart");
    Scene* scene = sceneOf(sceneId);
    // created at native size (R6): frame 400 x 200, origin (100, 150) of it
    const RectTransformComponent& rect = scene->getComponent<RectTransformComponent>(entity);
    EXPECT_EQ(rect.width, 400u);
    EXPECT_EQ(rect.height, 200u);
    EXPECT_FLOAT_EQ(rect.pivot.x, 0.25f);
    EXPECT_FLOAT_EQ(rect.pivot.y, 0.75f);
    scene->getComponent<TitleArtRendererComponent>(entity).uiScale = 0.5f;
    std::string error;
    ASSERT_TRUE(TitleArtSystem::setNativeSize(scene, entity, error)) << error;
    EXPECT_EQ(scene->getComponent<RectTransformComponent>(entity).width, 200u) << "frame px x uiScale (R3)";
    EXPECT_EQ(scene->getComponent<RectTransformComponent>(entity).height, 100u);

    // world: frame px / Pixels Per Unit, the nearest pivot preset
    const uint32_t worldScene = newScene(SceneType::SCENE_2D);
    CreateEntityCmd* create = new CreateEntityCmd(headless().project.get(), worldScene, "Logo", EntityCreationType::TITLE_ART_SPRITE);
    create->setQuiet(true);
    create->setTitleArt("assets/titles/logo.titleart");
    CommandHandle::get(worldScene)->addCommandNoMerge(create);
    Scene* world = sceneOf(worldScene);
    const Entity sprite = create->getEntity();
    EXPECT_EQ(world->getComponent<SpriteComponent>(sprite).width, 400u);
    EXPECT_EQ(world->getComponent<SpriteComponent>(sprite).height, 200u);
    EXPECT_EQ(world->getComponent<SpriteComponent>(sprite).pivotPreset, PivotPreset::BOTTOM_LEFT);
    world->getComponent<TitleArtRendererComponent>(sprite).pixelsPerUnit = 4.0f;
    ASSERT_TRUE(TitleArtSystem::setNativeSize(world, sprite, error)) << error;
    EXPECT_EQ(world->getComponent<SpriteComponent>(sprite).width, 100u);
    EXPECT_EQ(world->getComponent<SpriteComponent>(sprite).height, 50u);
    ASSERT_TRUE(CommandHandle::get(worldScene)->undoCommand());
    EXPECT_EQ(CommandHandle::get(worldScene)->undoCommand()->describe(), "Create Title Art (logo)") << "R6: one step, named";
}

TEST(TitleArtComponent, LiveEffectsDriveTheShaderAndTheCompanionKeepsItsOwn) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene(SceneType::SCENE_UI);
    const Entity entity = makeUITitle(sceneId, "assets/titles/logo.titleart");
    Scene* scene = sceneOf(sceneId);
    scene->getComponent<UIComponent>(entity).customShader = ShaderRef("library:ui_fx");
    scene->getComponent<UIComponent>(entity).shaderFeatures = {"SHINE"};
    scene->getComponent<TitleArtRendererComponent>(entity).sweepEnabled = true;
    scene->getComponent<TitleArtRendererComponent>(entity).sweepPosition = 0.5f;
    scene->getSystem<TitleArtSystem>()->update(0.0);
    const UIComponent& ui = scene->getComponent<UIComponent>(entity);
    EXPECT_EQ(ui.customShader.path, "library:title_fx") << "R33: a Live Effect draws through title_fx";
    EXPECT_EQ(ui.shaderFeatures, std::vector<std::string>{"SWEEP"});
    EXPECT_EQ(ShaderUniforms::get(ui.shaderUniforms, "sweepParams").x, 0.5f);

    // R2: what the editor saves is the Image's own shader
    const reflect::ComponentDesc& uiDesc = *reflect::find(ComponentType::UIComponent);
    reflect::EncodeContext ctx;
    ctx.registry = scene;
    ctx.entity = entity;
    ctx.signature = scene->getSignature(entity);
    const YAML::Node saved = reflect::encode(uiDesc, &ui, ctx);
    EXPECT_NE(YAML::Dump(saved).find("library:ui_fx"), std::string::npos) << YAML::Dump(saved);
    EXPECT_EQ(YAML::Dump(saved).find("title_fx"), std::string::npos) << YAML::Dump(saved);
    EXPECT_EQ(scene->getComponent<UIComponent>(entity).customShader.path, "library:title_fx") << "the scope put the driven values back";

    // removing the component restores the companion's saved values
    scene->removeComponent<TitleArtRendererComponent>(entity);
    EXPECT_EQ(scene->getComponent<UIComponent>(entity).customShader.path, "library:ui_fx");
    EXPECT_EQ(scene->getComponent<UIComponent>(entity).shaderFeatures, std::vector<std::string>{"SHINE"});
}

TEST(TitleArtComponent, OverridesAreOnePropertyPerName) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene(SceneType::SCENE_UI);
    const Entity a = makeUITitle(sceneId, "assets/titles/logo.titleart");
    const Entity b = makeUITitle(sceneId, "assets/titles/logo.titleart");
    Scene* scene = sceneOf(sceneId);
    scene->getComponent<TitleArtRendererComponent>(a).overrides.push_back({"text", "text", Vector4(0, 0, 0, 0), "WIN"});
    const PropertyData property = Catalog::findProperty(scene, a, ComponentType::TitleArtRendererComponent, "overrides.text");
    ASSERT_TRUE(property.ref);
    EXPECT_EQ(property.type, PropertyType::String);
    EXPECT_EQ(*static_cast<std::string*>(property.ref), "WIN");
    EXPECT_TRUE(Catalog::findEntityProperties(scene, a, ComponentType::TitleArtRendererComponent).count("overrides.text"));

    // Runtime Environments (S86 R26) copy as one list, through a prefab override and undo's route
    scene->getComponent<TitleArtRendererComponent>(a).runtimeEnvironments = {"softbox", "neon"};
    const PropertyData environments = Catalog::findProperty(scene, a, ComponentType::TitleArtRendererComponent, "runtimeEnvironments");
    ASSERT_TRUE(environments.ref);
    EXPECT_EQ(static_cast<std::vector<std::string>*>(environments.ref)->size(), 2u);
    Catalog::copyPropertyValue(scene, a, scene, b, ComponentType::TitleArtRendererComponent, "runtimeEnvironments");
    EXPECT_EQ(scene->getComponent<TitleArtRendererComponent>(b).runtimeEnvironments, std::vector<std::string>({"softbox", "neon"}));

    // a prefab override copies one entry by name, making it where the target has none (AC2)
    Catalog::copyPropertyValue(scene, a, scene, b, ComponentType::TitleArtRendererComponent, "overrides.text");
    ASSERT_EQ(scene->getComponent<TitleArtRendererComponent>(b).overrides.size(), 1u);
    EXPECT_EQ(scene->getComponent<TitleArtRendererComponent>(b).overrides[0].text, "WIN");
    scene->getComponent<TitleArtRendererComponent>(a).overrides.clear();
    Catalog::copyPropertyValue(scene, a, scene, b, ComponentType::TitleArtRendererComponent, "overrides.text");
    EXPECT_TRUE(scene->getComponent<TitleArtRendererComponent>(b).overrides.empty()) << "and removes it where the source has none";

    // set_properties' route: by name into the component's YAML, the type from the document
    YAML::Node yaml(YAML::NodeType::Map);
    std::string error;
    ASSERT_TRUE(applyTitleArtOverrideEdits(scene->getComponent<TitleArtRendererComponent>(a), yaml,
                                           {{"text", mcp::Json("JACKPOT!")}, {"glow_amount", mcp::Json(0.5)}}, error)) << error;
    ASSERT_TRUE(yaml["overrides"].IsSequence());
    EXPECT_EQ(yaml["overrides"].size(), 2u);
    EXPECT_FALSE(applyTitleArtOverrideEdits(scene->getComponent<TitleArtRendererComponent>(a), yaml, {{"glowamount", mcp::Json(1)}}, error));
    EXPECT_NE(error.find("Exposed: text, glow_amount"), std::string::npos) << error;
    EXPECT_FALSE(applyTitleArtOverrideEdits(scene->getComponent<TitleArtRendererComponent>(a), yaml, {{"glow_amount", mcp::Json("x")}}, error));
    EXPECT_NE(error.find("is a number"), std::string::npos) << error;
}

TEST(TitleArtComponent, ScriptSentencesSayTheFix) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene(SceneType::SCENE_UI);
    const Entity entity = makeUITitle(sceneId, "assets/titles/logo.titleart");
    Scene* scene = sceneOf(sceneId);
    std::string error;
    // §6.6: text on a Baked title
    EXPECT_FALSE(TitleArtSystem::setParameter(scene, entity, "text", TitleArtValue::fromString("WIN"), error));
    EXPECT_EQ(error, "TitleArt 'Logo': text cannot change on a Baked title (its pixels are the asset's bake). Set Render Mode to Realtime "
                     "(or Glyph Atlas for text that changes every frame) in the Title Art Renderer, or bake one Title Art per text and "
                     "enable the one you need.");
    scene->getComponent<TitleArtRendererComponent>(entity).renderMode = TitleArtRenderMode::REALTIME;
    EXPECT_FALSE(TitleArtSystem::setParameter(scene, entity, "glowamount", TitleArtValue::fromNumber(1), error));
    EXPECT_EQ(error, "TitleArt 'Logo': no exposed parameter 'glowamount' (did you mean 'glow_amount'?). Exposed: text, glow_amount.");
    EXPECT_FALSE(TitleArtSystem::setParameter(scene, entity, "glow_amount", TitleArtValue::fromString("x"), error));
    EXPECT_EQ(error, "TitleArt 'Logo': 'glow_amount' is a number; got string.");
    // R66: document paths are not names
    EXPECT_FALSE(TitleArtSystem::setParameter(scene, entity, "Title/fx:outer_glow.size", TitleArtValue::fromNumber(3), error));
    EXPECT_NE(error.find("Exposed: text, glow_amount."), std::string::npos) << error;
    // out of range clamps with a warning, no error
    std::string warning;
    EXPECT_TRUE(TitleArtSystem::setParameter(scene, entity, "glow_amount", TitleArtValue::fromNumber(4), error, &warning));
    EXPECT_NE(warning.find("clamped to 1"), std::string::npos) << warning;
    TitleArtValue value;
    ASSERT_TRUE(TitleArtSystem::getParameter(scene, entity, "glow_amount", value, error)) << error;
    EXPECT_DOUBLE_EQ(value.number, 1.0);
    ASSERT_TRUE(TitleArtSystem::setParameter(scene, entity, "text", TitleArtValue::fromString("WIN"), error)) << error;
    ASSERT_TRUE(TitleArtSystem::getParameter(scene, entity, "text", value, error));
    EXPECT_EQ(value.text, "WIN");
    ASSERT_TRUE(TitleArtSystem::resetParameter(scene, entity, "text", error));
    ASSERT_TRUE(TitleArtSystem::getParameter(scene, entity, "text", value, error));
    EXPECT_EQ(value.text, "JACKPOT") << "R5: the document's value again";
}
