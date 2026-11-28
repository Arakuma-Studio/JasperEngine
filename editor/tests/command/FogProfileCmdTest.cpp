// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Fog Profile commands (docs/specs/S101-vapor.md R23, R24, AC11) on a headless project: an edit of
// a .fogprofile is one step that undo puts back byte for byte, a drag merges, a creation keeps
// its GUID on redo and its undo deletes the file; ticking and unticking a row is a round trip;
// Link copies a built-in on first use and switches a non-Height Fog to Height in the same step;
// Unlink keeps the look the Fog showed.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Platform.h"
#include "Project.h"
#include "Scene.h"
#include "ScriptDataTestSupport.h"
#include "asset/AssetTypes.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/FogProfileAssetCmd.h"
#include "command/type/LinkFogProfileCmd.h"
#include "component/FogComponent.h"
#include "render/FogProfile.h"
#include "util/AssetDatabase.h"

#include "support/PosixCompat.h"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct FogHeadless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    FogHeadless& fogHeadless() {
        static FogHeadless h = []() {
            FogHeadless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-fog-profile-cmd-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: fogprofiles\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            headlesssystem::use(out.platform.get());
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        headlesssystem::use(h.platform.get());
        return h;
    }

    std::string fogReadAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    fs::path fogAssetFile(const std::string& assetPath) {
        return fogHeadless().project->getAssetsPath() / assetPath;
    }

    // a profile file of the project, written through the command (its creation step)
    void fogWriteProfile(const std::string& assetPath, const FogProfileData& data) {
        Project* project = fogHeadless().project.get();
        std::unique_ptr<FogProfileAssetCmd> create(FogProfileAssetCmd::create(project, assetPath, data));
        ASSERT_TRUE(create->execute());
    }

    FogProfileData murkyProfile() {
        FogProfileData data;
        data.name = "Murky";
        FogComponent values;
        values.attenuationDistance = 25.0f;
        values.color = Vector3(0.1f, 0.2f, 0.3f);
        values.density = 0.05f;
        data.setFrom(values, FogField::AttenuationDistance);
        data.setFrom(values, FogField::Color);
        data.setFrom(values, FogField::Density);
        return data;
    }

}

// a scene with a Fog, per test, in the suite's headless project
class FogProfileCmdFixture : public ::testing::Test {
protected:
    Project* project = nullptr;
    SceneProject* sceneProject = nullptr;
    Entity fog = NULL_ENTITY;
    bool wasPaused = false;

    void SetUp() override {
        wasPaused = Engine::areGameEventsPaused();
        Engine::pauseGameEvents(true);
        ASSERT_TRUE(fogHeadless().ready);
        project = fogHeadless().project.get();
        project->createNewScene("FogProfiles", SceneType::SCENE_3D);
        sceneProject = project->getScene(project->getSelectedSceneId());
        ASSERT_TRUE(sceneProject && sceneProject->scene);
        fog = sceneProject->scene->createEntity();
        sceneProject->scene->setEntityName(fog, "Fog");
        sceneProject->scene->addComponent<FogComponent>(fog, {});
        sceneProject->entities.push_back(fog);
    }
    void TearDown() override {
        Engine::pauseGameEvents(wasPaused);
    }

    FogComponent& fogComponent() { return sceneProject->scene->getComponent<FogComponent>(fog); }
    CommandHistory* history() { return CommandHandle::get(sceneProject->id); }
};

class FogProfileAssetCmdTest : public FogProfileCmdFixture {};
class LinkFogProfileCmdTest : public FogProfileCmdFixture {};

TEST_F(FogProfileAssetCmdTest, EditUndoRestoresFile) {
    const std::string path = "FogProfiles/edit.fogprofile";
    fogWriteProfile(path, murkyProfile());
    const std::string original = fogReadAll(fogAssetFile(path));
    ASSERT_FALSE(original.empty());

    FogProfileData after = murkyProfile();
    FogComponent values;
    values.attenuationDistance = 80.0f;
    after.setFrom(values, FogField::AttenuationDistance);
    after.description = "thinner";
    std::unique_ptr<FogProfileAssetCmd> edit(FogProfileAssetCmd::edit(project, path, murkyProfile(), after));
    ASSERT_TRUE(edit);
    ASSERT_TRUE(edit->execute());
    EXPECT_EQ(fogReadAll(fogAssetFile(path)), FogProfileIO::write(after));
    std::shared_ptr<const FogProfileData> live = FogProfileLibrary::get(path);
    ASSERT_TRUE(live);
    EXPECT_FLOAT_EQ(live->values.attenuationDistance, 80.0f) << "every user reads the edit on the next frame (R5)";

    edit->undo();
    EXPECT_EQ(fogReadAll(fogAssetFile(path)), original) << "undo puts the file back byte for byte";
    live = FogProfileLibrary::get(path);
    ASSERT_TRUE(live);
    EXPECT_FLOAT_EQ(live->values.attenuationDistance, 25.0f) << "and the library with it";
    EXPECT_EQ(*live, murkyProfile());

    EXPECT_FALSE(FogProfileAssetCmd::edit(project, path, murkyProfile(), murkyProfile())) << "the same profile is no edit";
}

TEST_F(FogProfileAssetCmdTest, DragMergesIntoOneStep) {
    const std::string path = "FogProfiles/drag.fogprofile";
    fogWriteProfile(path, murkyProfile());
    const std::string original = fogReadAll(fogAssetFile(path));
    const size_t steps = history()->position();

    FogProfileData current = murkyProfile();
    for (float distance : {30.0f, 35.0f, 40.0f}) {
        FogProfileData next = current;
        FogComponent values;
        values.attenuationDistance = distance;
        next.setFrom(values, FogField::AttenuationDistance);
        history()->addCommand(FogProfileAssetCmd::edit(project, path, current, next));
        current = next;
    }
    EXPECT_EQ(history()->position(), steps + 1) << "a drag is one step";
    EXPECT_FLOAT_EQ(FogProfileLibrary::get(path)->values.attenuationDistance, 40.0f);
    history()->undo();
    EXPECT_EQ(fogReadAll(fogAssetFile(path)), original);
    history()->redo();
    EXPECT_EQ(fogReadAll(fogAssetFile(path)), FogProfileIO::write(current));
}

TEST_F(FogProfileAssetCmdTest, CreateKeepsItsGuidAndUndoDeletesTheFile) {
    const std::string path = FogProfileAssetCmd::freePath(*project, "FogProfiles", "Created");
    EXPECT_EQ(path, "FogProfiles/Created.fogprofile");
    std::unique_ptr<FogProfileAssetCmd> create(FogProfileAssetCmd::create(project, path, murkyProfile()));
    const Guid guid = create->getGuid();
    ASSERT_FALSE(guid.isNull());
    ASSERT_TRUE(create->execute());
    const fs::path file = fogAssetFile(path);
    ASSERT_TRUE(fs::exists(file));
    const std::string relative = project->getAssetDatabase().toRelative(file).generic_string();
    EXPECT_EQ(project->getAssetDatabase().lookupPath(relative).guid, guid);
    EXPECT_EQ(FogProfileAssetCmd::freePath(*project, "FogProfiles", "Created"), "FogProfiles/Created 1.fogprofile") << "a taken name gets a number";

    create->undo();
    EXPECT_FALSE(fs::exists(file)) << "undoing a creation deletes the file";
    EXPECT_FALSE(fs::exists(project->getProjectPath() / AssetTypes::metaPathFor(relative))) << "and its .meta";
    EXPECT_FALSE(FogProfileLibrary::get(path)) << "its users read it as missing (R6)";

    ASSERT_TRUE(create->execute());
    EXPECT_EQ(project->getAssetDatabase().lookupPath(relative).guid, guid) << "redo makes it with the same GUID";
    create->undo();
}

TEST_F(FogProfileAssetCmdTest, TickUntickRoundTrip) {
    // the Inspector's Set by profile box: ticking copies the Fog's value in, unticking removes it
    const std::string path = "FogProfiles/tick.fogprofile";
    fogWriteProfile(path, murkyProfile());
    const std::string original = fogReadAll(fogAssetFile(path));
    fogComponent().baseHeight = 12.5f;

    FogProfileData ticked = murkyProfile();
    ticked.setFrom(fogComponent(), FogField::BaseHeight);
    history()->addCommandNoMerge(FogProfileAssetCmd::edit(project, path, murkyProfile(), ticked));
    ASSERT_TRUE(FogProfileLibrary::get(path)->isSet(FogField::BaseHeight));
    EXPECT_FLOAT_EQ(FogProfileLibrary::get(path)->values.baseHeight, 12.5f) << "ticking copies the Fog's value";

    FogProfileData unticked = ticked;
    unticked.clear(FogField::Density);
    history()->addCommandNoMerge(FogProfileAssetCmd::edit(project, path, ticked, unticked));
    EXPECT_FALSE(FogProfileLibrary::get(path)->isSet(FogField::Density)) << "unticking removes the override";
    FogProfileData parsed;
    ASSERT_TRUE(FogProfileIO::parse(fogReadAll(fogAssetFile(path)), parsed));
    EXPECT_FALSE(parsed.isSet(FogField::Density)) << "from the file too";

    history()->undo();
    EXPECT_TRUE(FogProfileLibrary::get(path)->isSet(FogField::Density));
    history()->undo();
    EXPECT_EQ(fogReadAll(fogAssetFile(path)), original) << "each change is one step and undoes the file";
    EXPECT_FALSE(FogProfileLibrary::get(path)->isSet(FogField::BaseHeight));
}

TEST_F(LinkFogProfileCmdTest, UnlinkCopiesResolved) {
    Scene* scene = sceneProject->scene;
    FogComponent own;
    own.type = FogType::HEIGHT;
    own.baseHeight = 7.0f;
    own.density = 0.2f;
    fogComponent() = own;
    CommandHistory* timeline = history();

    const std::string path = "FogProfiles/unlink.fogprofile";
    fogWriteProfile(path, murkyProfile());
    std::string error;
    timeline->addCommand(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, path, error));
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(scene->getComponent<FogComponent>(fog).profile.path, path);
    const FogComponent shown = LinkFogProfileCmd::resolvedLook(scene->getComponent<FogComponent>(fog), FogProfileLibrary::get(path).get());

    LinkFogProfileCmd* unlink = LinkFogProfileCmd::unlink(project, sceneProject->id, {fog}, ComponentType::FogComponent, error);
    ASSERT_TRUE(unlink) << error;
    EXPECT_STREQ(unlink->typeName(), "LinkFogProfileCmd");
    const size_t steps = timeline->position();
    timeline->addCommand(unlink);
    EXPECT_EQ(timeline->position(), steps + 1) << "one step";
    const FogComponent& after = scene->getComponent<FogComponent>(fog);
    EXPECT_FALSE(after.profile.isSet()) << "the link is cleared";
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
        const FogField field = static_cast<FogField>(i);
        EXPECT_TRUE(FogFields::equal(after, shown, field)) << FogFields::name(field) << ": the Fog keeps the look it showed";
    }
    EXPECT_FLOAT_EQ(after.attenuationDistance, 25.0f) << "a field the profile set is the Fog's own now";
    EXPECT_FLOAT_EQ(after.baseHeight, 7.0f) << "a field it did not set stays the Fog's";

    timeline->undo();
    const FogComponent& undone = scene->getComponent<FogComponent>(fog);
    EXPECT_EQ(undone.profile.path, path) << "undo links it again";
    EXPECT_FLOAT_EQ(undone.attenuationDistance, own.attenuationDistance) << "and puts its own values back";
    EXPECT_FLOAT_EQ(undone.density, 0.2f);
}

TEST_F(LinkFogProfileCmdTest, BuiltinCopiedOnFirstLinkAndUndoRemovesTheCopy) {
    Scene* scene = sceneProject->scene;
    fogComponent().type = FogType::HEIGHT;
    CommandHistory* timeline = history();

    const std::string copyPath = FogProfileLibrary::normalizeKey(FogProfiles::builtinCopyPath("night"));
    const fs::path copy = fogAssetFile(copyPath);
    ASSERT_FALSE(fs::exists(copy));
    std::string error;
    const size_t steps = timeline->position();
    timeline->addCommand(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "builtin:night", error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(timeline->position(), steps + 1) << "the copy and the link are one step";
    ASSERT_TRUE(fs::exists(copy)) << "the first use copies the built-in into the project (R4)";
    EXPECT_EQ(fogReadAll(copy), FogProfileIO::write(FogProfiles::findBuiltin("night")->data));
    const AssetRef linked = scene->getComponent<FogComponent>(fog).profile;
    EXPECT_EQ(linked.path, copyPath) << "the Fog stores the project copy";
    EXPECT_FALSE(linked.guid.isNull()) << "by GUID";

    timeline->undo();
    EXPECT_FALSE(fs::exists(copy)) << "undo removes the copy";
    EXPECT_FALSE(scene->getComponent<FogComponent>(fog).profile.isSet()) << "and the link";
    timeline->redo();
    ASSERT_TRUE(fs::exists(copy));
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).profile.guid, linked.guid) << "redo keeps the copy's GUID";

    // the copy is reused afterwards: an edit of it survives another link
    FogProfileData edited = *FogProfileLibrary::get(copyPath);
    edited.description = "our night";
    timeline->addCommandNoMerge(FogProfileAssetCmd::edit(project, copyPath, *FogProfileLibrary::get(copyPath), edited));
    timeline->addCommand(LinkFogProfileCmd::unlink(project, sceneProject->id, {fog}, ComponentType::FogComponent, error));
    timeline->addCommand(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "builtin:night", error));
    ASSERT_TRUE(error.empty()) << error;
    FogProfileData reread;
    ASSERT_TRUE(FogProfileIO::parse(fogReadAll(copy), reread));
    EXPECT_EQ(reread.description, "our night") << "the second link reuses the project's copy";
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).profile.path, copyPath);
}

TEST_F(LinkFogProfileCmdTest, TypeSwitchesToHeightInTheSameStep) {
    Scene* scene = sceneProject->scene;
    CommandHistory* timeline = history();
    ASSERT_EQ(scene->getComponent<FogComponent>(fog).type, FogType::LINEAR);

    // a profile of distance fields only leaves the type alone
    FogProfileData distance;
    distance.name = "Grey";
    FogComponent values;
    values.color = Vector3(0.5f, 0.5f, 0.5f);
    distance.setFrom(values, FogField::Color);
    fogWriteProfile("FogProfiles/grey.fogprofile", distance);
    std::string error;
    timeline->addCommand(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "FogProfiles/grey.fogprofile", error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).type, FogType::LINEAR) << "Color is read by every type";

    // one that sets height fields makes it a Height fog in the same step
    fogWriteProfile("FogProfiles/heavy.fogprofile", murkyProfile());
    const size_t steps = timeline->position();
    timeline->addCommand(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "FogProfiles/heavy.fogprofile", error));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(timeline->position(), steps + 1);
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).type, FogType::HEIGHT);
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).profile.path, "FogProfiles/heavy.fogprofile");
    timeline->undo();
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).type, FogType::LINEAR) << "one undo puts the type back";
    EXPECT_EQ(scene->getComponent<FogComponent>(fog).profile.path, "FogProfiles/grey.fogprofile") << "and the previous link";

    // nothing to change: no command, no error
    LinkFogProfileCmd* same = LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "FogProfiles/grey.fogprofile", error);
    EXPECT_EQ(same, nullptr);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(LinkFogProfileCmd::link(project, sceneProject->id, {fog}, ComponentType::FogComponent, "builtin:nope", error), nullptr);
    EXPECT_NE(error.find("builtin:"), std::string::npos) << "an unknown built-in names the valid ones: " << error;
}
