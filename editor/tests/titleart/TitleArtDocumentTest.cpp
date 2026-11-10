// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Project.h"
#include "asset/AssetTypes.h"
#include "command/CommandHistory.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "util/AssetDatabase.h"
#include "util/Util.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using titleart::Json;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-titleart-document-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: titleartdocument\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string topName() {
        const UndoEntry* top = history()->top();
        return top ? top->name() : std::string();
    }

    std::string folder(const std::string& name) {
        fs::remove_all(root() / "assets" / name);
        fs::create_directories(root() / "assets" / name);
        return "assets/" + name;
    }

    std::shared_ptr<TitleArtDocument> create(const std::string& relative) {
        std::string error;
        std::shared_ptr<TitleArtDocument> document = TitleArtDocument::create(project(), relative, nullptr, "Create Title Art", &error);
        EXPECT_TRUE(document) << error;
        return document;
    }

    class TitleArtDocumentTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_TRUE(headless().ready) << "the headless project did not open"; }
        void TearDown() override { TitleArtDocument::closeAll(); }
    };

}

TEST_F(TitleArtDocumentTest, CreateWritesTheCanonicalDocumentAsOneStep) {
    const std::string dir = folder("create");
    const size_t position = history()->position();
    std::shared_ptr<TitleArtDocument> document = create(dir + "/New Title Art.titleart");
    ASSERT_TRUE(document);
    EXPECT_EQ(history()->position(), position + 1);
    EXPECT_EQ(topName(), "Create Title Art");
    const fs::path file = root() / dir / "New Title Art.titleart";
    const std::string text = readAll(file);
    const titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(titleart::writeTitleArt(parsed.doc), text) << "canonical";
    EXPECT_TRUE(titleart::isTitleArtGuid(document->guid()));
    EXPECT_EQ(document->doc().layers()[0]["text"], "Title");
    EXPECT_EQ(document->assetPath(), "create/New Title Art.titleart");
    std::string error;
    EXPECT_FALSE(TitleArtDocument::create(project(), dir + "/New Title Art.titleart", nullptr, "", &error)) << "refused when it exists";
    EXPECT_FALSE(TitleArtDocument::create(project(), dir + "/x.png", nullptr, "", &error));
    EXPECT_FALSE(TitleArtDocument::create(project(), "/tmp/outside.titleart", nullptr, "", &error));
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(file)) << "undo deletes the document";
}

TEST_F(TitleArtDocumentTest, EditIsOneNamedStepAndUndoRestoresTheFile) {
    const std::string dir = folder("edit");
    std::shared_ptr<TitleArtDocument> document = create(dir + "/logo.titleart");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();
    const std::string before = readAll(file);
    const uint64_t rev = document->revision();
    int notified = 0;
    TitleArtChangeOrigin last = TitleArtChangeOrigin::Disk;
    const int listener = document->addListener([&](TitleArtChangeOrigin origin) {
        notified++;
        last = origin;
    });
    TitleArtEditOptions options;
    options.origin = TitleArtChangeOrigin::Agent;
    const TitleOpsResult result = document->edit(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.5}}}}, options);
    ASSERT_TRUE(result.ok) << result.error();
    EXPECT_EQ(topName(), "Set Opacity");
    EXPECT_GT(document->revision(), rev);
    EXPECT_EQ(document->doc().layers()[0]["opacity"], 0.5);
    EXPECT_EQ(notified, 1);
    EXPECT_EQ(last, TitleArtChangeOrigin::Agent) << "a commit's listeners hear who made it";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(file), before) << "undo gives the exact file back";
    EXPECT_EQ(document->doc().layers()[0]["opacity"], 1);
    EXPECT_EQ(last, TitleArtChangeOrigin::History);
    ASSERT_TRUE(history()->redo());
    EXPECT_EQ(document->doc().layers()[0]["opacity"], 0.5);
    document->removeListener(listener);
}

TEST_F(TitleArtDocumentTest, ExpectRevAndDryRun) {
    const std::string dir = folder("rev");
    std::shared_ptr<TitleArtDocument> document = create(dir + "/logo.titleart");
    ASSERT_TRUE(document);
    const std::string before = readAll(document->absolutePath());
    TitleArtEditOptions stale;
    stale.expectRev = (int64_t)document->revision() + 5;
    const TitleOpsResult refused = document->edit(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.5}}}}, stale);
    EXPECT_FALSE(refused.ok);
    ASSERT_FALSE(refused.problems.empty());
    EXPECT_EQ(refused.problems[0].code, "rev_mismatch");
    EXPECT_NE(refused.error().find("Nothing was applied."), std::string::npos);
    TitleArtEditOptions dry;
    dry.dryRun = true;
    dry.expectRev = (int64_t)document->revision();
    const TitleOpsResult preview = document->edit(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.5}}}}, dry);
    EXPECT_TRUE(preview.ok);
    EXPECT_EQ(preview.doc.layers()[0]["opacity"], 0.5);
    EXPECT_EQ(readAll(document->absolutePath()), before) << "a dry run writes nothing";
}

TEST_F(TitleArtDocumentTest, CommitTextRestoresAVersionAsOneStep) {
    const std::string dir = folder("restore");
    std::shared_ptr<TitleArtDocument> document = create(dir + "/logo.titleart");
    ASSERT_TRUE(document);
    const std::string original = document->text();
    ASSERT_TRUE(document->edit(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"text", "Jackpot"}}}}).ok);
    const size_t position = history()->position();
    std::string error;
    ASSERT_TRUE(document->commitText(original, "Restore This Version", &error)) << error;
    EXPECT_EQ(history()->position(), position + 1);
    EXPECT_EQ(topName(), "Restore This Version");
    EXPECT_EQ(document->doc().layers()[0]["text"], "Title");
    EXPECT_EQ(document->text(), original);
    EXPECT_FALSE(document->commitText("{ not json", "", &error));
}

TEST_F(TitleArtDocumentTest, OutsideChangesReloadAndNewerVersionsAreReadOnly) {
    const std::string dir = folder("outside");
    std::shared_ptr<TitleArtDocument> document = create(dir + "/logo.titleart");
    ASSERT_TRUE(document);
    const fs::path file = document->absolutePath();
    TitleArtChangeOrigin last = TitleArtChangeOrigin::Person;
    document->addListener([&](TitleArtChangeOrigin origin) { last = origin; });
    titleart::TitleArtDoc changed = document->doc();
    changed.layers()[0]["text"] = "Edited outside";
    writeAll(file, titleart::writeTitleArt(changed));
    TitleArtDocument::checkDiskChanges();
    EXPECT_EQ(document->doc().layers()[0]["text"], "Edited outside");
    EXPECT_EQ(last, TitleArtChangeOrigin::Disk);
    // a newer build's file: read-only, nothing edits it
    writeAll(file, R"({"format": "jasperengine.titleart", "version": 99, "guid": ")" + document->guid() + "\"}\n");
    TitleArtDocument::checkDiskChanges();
    EXPECT_FALSE(document->editable());
    EXPECT_EQ(document->versionRead(), 99);
    EXPECT_NE(document->versionError().find("version 99"), std::string::npos);
    const TitleOpsResult refused = document->edit(Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"opacity", 0.5}}}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.problems[0].code, "read_only");
    std::string error;
    EXPECT_FALSE(document->commit(changed, "", "", &error));
    // a file that stops parsing keeps the last good value
    writeAll(file, "{ broken");
    TitleArtDocument::checkDiskChanges();
    EXPECT_FALSE(document->loadError().empty());
}

TEST_F(TitleArtDocumentTest, OpenFindsTheSharedDocument) {
    const std::string dir = folder("open");
    std::shared_ptr<TitleArtDocument> created = create(dir + "/logo.titleart");
    ASSERT_TRUE(created);
    std::string error;
    EXPECT_EQ(TitleArtDocument::open(project(), dir + "/logo.titleart", &error), created);
    EXPECT_EQ(TitleArtDocument::open(project(), "open/logo.titleart", &error), created) << "an asset-relative path";
    EXPECT_EQ(TitleArtDocument::find(created->absolutePath()), created);
    EXPECT_FALSE(TitleArtDocument::open(project(), dir + "/missing.titleart", &error));
    EXPECT_FALSE(TitleArtDocument::open(project(), dir + "/logo.controller", &error));
}

// ---- the asset type (S80 R3) ----

TEST(AssetTypesTest, TitleArt) {
    const AssetTypeInfo* info = AssetTypes::find("assets/logos/logo.titleart", "assets");
    ASSERT_TRUE(info);
    EXPECT_EQ(info->type, AssetType::TitleArt);
    EXPECT_EQ(info->identity, AssetIdentity::InFile);
    EXPECT_EQ(info->base, AssetPathBase::Assets);
    const AssetTypeInfo* ttc = AssetTypes::find("assets/fonts/noto.ttc", "assets");
    ASSERT_TRUE(ttc);
    EXPECT_EQ(ttc->type, AssetType::Font);
    EXPECT_TRUE(Util::isTitleArtFile("a/LOGO.TitleArt"));
    EXPECT_TRUE(Util::isFontFile("noto.ttc"));
    EXPECT_TRUE(Util::isAssetFile("logo.titleart"));
    // the baked PNG beside it is an ordinary texture
    EXPECT_EQ(AssetTypes::find("assets/logo.titleart.png", "assets")->type, AssetType::Texture);
}

TEST_F(TitleArtDocumentTest, TheAssetDatabaseGivesADocumentItsGuidInTheFile) {
    const std::string dir = folder("identity");
    // a hand-written document without a GUID
    const fs::path file = root() / dir / "hand.titleart";
    writeAll(file, "{\"format\": \"jasperengine.titleart\", \"version\": 1, \"layers\": [{\"type\": \"text\", \"text\": \"Hi\"}]}\n");
    AssetDatabase database;
    database.open(root(), "assets");
    const Guid guid = database.getAssetGuid(fs::path(dir) / "hand.titleart");
    ASSERT_FALSE(guid.isNull());
    const titleart::TitleArtParseResult parsed = titleart::parseTitleArt(readAll(file));
    ASSERT_TRUE(parsed.ok) << "still a valid document: " << parsed.error.toString();
    EXPECT_EQ(parsed.doc.guid(), guid.str()) << "the identity lives in the file";
    EXPECT_EQ(titleart::readTitleArtGuid(readAll(file)), guid.str());
    EXPECT_EQ(database.typeOf(fs::path(dir) / "hand.titleart"), AssetType::TitleArt);
    EXPECT_FALSE(fs::exists(root() / dir / "hand.titleart.meta")) << "no sidecar";
}

TEST_F(TitleArtDocumentTest, FilesAStepImportsAreInTheAssetDatabaseWhenItReturns) {
    // a library apply writes its fonts beside the document in the same step; the next render or
    // check resolves them through the asset database, with no frame between: they must be there
    // already, not after the file watcher's walk (a full gate run missed a font under load)
    const std::string dir = folder("imports");
    const fs::path font = root() / dir / "Fonts" / "Imported-Regular.ttf";
    const std::string rel = dir + "/Fonts/Imported-Regular.ttf";
    ASSERT_FALSE(project()->getAssetDatabase().lookupPath(rel).found());
    std::vector<TitleOpFile> files = {{font, std::string("font bytes"), true, false}};
    std::string error;
    std::shared_ptr<TitleArtDocument> document = TitleArtDocument::create(project(), dir + "/logo.titleart", nullptr, "Create Title Art", &error, &files);
    ASSERT_TRUE(document) << error;
    ASSERT_TRUE(fs::exists(font));
    EXPECT_TRUE(project()->getAssetDatabase().lookupPath(rel).found()) << "the imported font resolves as soon as the step returns";
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(font));
    EXPECT_FALSE(project()->getAssetDatabase().lookupPath(rel).found()) << "and its undo takes it out again";
}
