// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A dialogue's source text edited in the editor (docs/specs/dialogue-graphs.md R21, R26, AC7): the
// translations of that key turn needs_review in the same undo step, and one undo puts both files
// back byte for byte; a new dialogue gets its namespace, source language and GUID; an export ships
// it cooked (R16).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "TempDir.h"

#include "Engine.h"
#include "Exporter.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "dialogue/DialogueEditor.h"
#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueProgram.h"
#include "graph/GraphDocument.h"
#include "localization/StringTableDocument.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace DialogueEditTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    void writeDialogueFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    std::string readDialogueFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    class DialogueEditTest : public ::testing::Test {
    protected:
        jasperengine::test::TempDir dir;
        Project project;

        void SetUp() override {
            editor::test::ensureEngine();
            std::string error;
            ASSERT_TRUE(project.createProjectAt(dir.path() / "Dialogues", "Dialogues", &error)) << error;
            Engine::setAsyncLoading(false);
            dialogues::install(&project);
        }
        void TearDown() override {
            GraphDocument::closeAll();
            dialogues::uninstall();
            project.releaseScenes();
        }
    };

    const char* const GUARD = R"({"formatVersion": 1, "kind": "dialogue", "kindVersion": 1, "nextId": 9,
 "nodes": [{"id": "n1", "type": "entry", "position": [0, 0], "params": {"name": "start"}},
           {"id": "n2", "type": "line", "position": [200, 0], "params": {"speaker": "guard", "text": "Halt.",
             "options": [{"id": "o1", "text": "Hello"}], "next_sub_id": 2},
            "ports": [{"id": "in", "direction": "in", "type": "flow", "name": "In", "multi": true, "optional": true},
                      {"id": "next", "direction": "out", "type": "flow", "name": "Next", "optional": true},
                      {"id": "o1", "direction": "out", "type": "flow", "name": "Hello", "optional": true}]}],
 "edges": [{"id": "e5", "from": {"node": "n1", "port": "next"}, "to": {"node": "n2", "port": "in"}}],
 "metadata": {"namespace": "guard", "source_language": "en"}, "guid": "6a1f0000-0000-4000-8000-0000000000d1"})";

}

TEST_F(DialogueEditTest, SourceEditMarksTranslationsForReview) {
    const fs::path file = project.getAssetsPath() / "npc" / "guard.dialogue";
    writeDialogueFile(file, GUARD);
    l10n::TranslationDoc german;
    german.guid = "6a1f0000-0000-4000-8000-0000000000d2";
    german.ownerGuid = "6a1f0000-0000-4000-8000-0000000000d1";
    german.ownerPath = "npc/guard.dialogue";
    german.language = "de";
    german.entries["n2"] = {"Halt.", "translated", l10n::sourceHash("Halt."), {}};
    german.entries["n2.o1"] = {"Hallo", "translated", l10n::sourceHash("Hello"), {}};
    const fs::path translation = l10n::translationPathFor(file, "de");
    writeDialogueFile(translation, l10n::saveTranslation(german));
    const std::string dialogueBefore = readDialogueFile(file);
    const std::string translationBefore = readDialogueFile(translation);

    std::string error;
    std::shared_ptr<GraphDocument> document = GraphDocument::open(&project, file.string(), &error);
    ASSERT_TRUE(document) << error;
    CommandHistory* history = project.getProjectCommandHistory();
    ASSERT_TRUE(document->apply({{"setParam", {{"node", "n2"}, {"name", "text"}, {"value", "Halt, stranger."}}}}, "Set Text", &error)) << error;

    l10n::TranslationDoc after;
    ASSERT_TRUE(l10n::parseTranslation(readDialogueFile(translation), after));
    EXPECT_EQ(after.entries["n2"].status, "needs_review") << "the edited text's translation";
    EXPECT_EQ(after.entries["n2.o1"].status, "translated") << "the other keys are untouched";
    EXPECT_NE(readDialogueFile(file).find("Halt, stranger."), std::string::npos);

    // one step: one undo puts both files back exactly
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(readDialogueFile(file), dialogueBefore);
    EXPECT_EQ(readDialogueFile(translation), translationBefore);
    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(l10n::parseTranslation(readDialogueFile(translation), after));
    EXPECT_EQ(after.entries["n2"].status, "needs_review");

    // a move or an option reorder changes no text: no translation file is written
    ASSERT_TRUE(document->apply({{"moveNodes", {{"ids", {"n2"}}, {"delta", {10, 0}}}}}, "", &error)) << error;
    const std::string reviewed = readDialogueFile(translation);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(readDialogueFile(translation), reviewed);
}

TEST_F(DialogueEditTest, NewDialogueGetsNamespaceLanguageAndGuid) {
    graph::Graph created;
    created.kind = "dialogue";
    dialogues::prepareNew(&project, project.getAssetsPath() / "Village Elder.dialogue", created, true);
    EXPECT_EQ(created.metadata.value("namespace", std::string()), "village_elder");
    EXPECT_EQ(created.metadata.value("source_language", std::string()), "en");
    EXPECT_FALSE(created.passthrough.value("guid", std::string()).empty());
    ASSERT_EQ(created.nodes.size(), 2u);
    EXPECT_EQ(created.nodes[0].type, "entry");
    EXPECT_EQ(created.nodes[1].type, "line");
    ASSERT_EQ(created.edges.size(), 1u);
    EXPECT_TRUE(graph::validate(created).size() == 1u && graph::validate(created)[0].code == "empty-line")
        << "the starter is clean but for its empty line";
    // a namespace another dialogue holds gets a number
    writeDialogueFile(project.getAssetsPath() / "elder.dialogue", GUARD);
    graph::Graph second;
    second.kind = "dialogue";
    dialogues::prepareNew(&project, project.getAssetsPath() / "sub" / "guard.dialogue", second, false);
    EXPECT_EQ(second.metadata.value("namespace", std::string()), "guard_2");
}

// R16: the export writes a dialogue as its compiled program, which the game reads by the same path
TEST_F(DialogueEditTest, ExportShipsTheCompiledProgram) {
    std::string guard = GUARD;
    // no namespace: the game's comes from the file's stem, as the editor's does
    const std::string named = R"("metadata": {"namespace": "guard", "source_language": "en"})";
    guard.replace(guard.find(named), named.size(), R"("metadata": {"source_language": "en"})");
    const fs::path source = project.getAssetsPath() / "talk" / "Village Elder.dialogue";
    writeDialogueFile(source, guard);
    const fs::path shipped = dir.path() / "export" / "assets" / "talk" / "Village Elder.dialogue";
    fs::create_directories(shipped.parent_path());
    std::string error;
    ASSERT_TRUE(Exporter::cookDialogue(source, shipped, "talk/Village Elder.dialogue", error)) << error;
    const std::string bytes = readDialogueFile(shipped);
    ASSERT_TRUE(dialogue::Program::isCooked(bytes));
    const std::shared_ptr<const dialogue::Program> game = dialogue::Programs::fromText("talk/Village Elder.dialogue", bytes, &error);
    ASSERT_TRUE(game) << error;
    const std::shared_ptr<const dialogue::Program> editor = dialogue::Programs::fromText("talk/Village Elder.dialogue", guard, &error);
    ASSERT_TRUE(editor) << error;
    EXPECT_EQ(game->cook(), editor->cook());
    EXPECT_EQ(game->ns, "village_elder");
    EXPECT_EQ(game->identity(), "6a1f0000-0000-4000-8000-0000000000d1");
    ASSERT_EQ(game->nodes.size(), 2u);
    EXPECT_EQ(game->str(game->nodes[1].text), "Halt.");

    // one that does not load is left to the caller, which ships it as it is
    writeDialogueFile(source, "{not json");
    const fs::path broken = dir.path() / "export" / "assets" / "talk" / "broken.dialogue";
    EXPECT_FALSE(Exporter::cookDialogue(source, broken, "talk/broken.dialogue", error));
    EXPECT_NE(error.find("is not a dialogue"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(broken));
}

}  // namespace DialogueEditTest_cpp
