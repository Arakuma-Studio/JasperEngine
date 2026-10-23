// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What makes a document degraded (dropped issues) and what Save Anyway clears
// (docs/specs/S03-data-safety.md R26, R31, AC15). The gate itself (Project::saveSceneFile
// refusing to write, Save As, no autosave) runs in a real editor: test_data_safety.py.

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamPreserved.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(SaveGate, DroppedIssues) {
    DecodedScene decoded = decodeSceneFile(fixture("data-safety/dropped.scene"));
    ASSERT_EQ(decoded.report.dropped(), 2u);

    const LoadIssue* type = nullptr;
    const LoadIssue* nested = nullptr;
    for (const LoadIssue& issue : decoded.report.issues) {
        if (issue.kind == LoadIssueKind::UnknownEntityType) type = &issue;
        if (issue.kind == LoadIssueKind::UnknownField && issue.fate == LoadIssueFate::Dropped) nested = &issue;
    }
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->value, "Hologram");
    EXPECT_EQ(type->entityPath, "Ghost");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->component, "mesh");
    EXPECT_EQ(nested->field, "submeshes[0].glint");
    EXPECT_GT(nested->line, 0);

    // outstanding whatever the store holds; Save Anyway / Save As clear them
    EXPECT_EQ(decoded.report.outstanding(findPreservedStore(decoded.scene->scene)).size(), 2u);
    decoded.report.clearDropped();
    EXPECT_EQ(decoded.report.dropped(), 0u);

    // the save writes what the editor has: the unknown entity is gone
    const YAML::Node saved = encodeScene(decoded);
    for (const YAML::Node& entity : saved["entities"])
        EXPECT_EQ(entity["type"].Scalar(), "Entity");
}

TEST(SaveGate, ParseErrorNamesThePosition) {
    try {
        DocumentFiles::parse("a: [1, 2\nb: 3\n", "Broken.scene");
        FAIL() << "a syntax error parsed";
    } catch (const DocumentParseError& e) {
        EXPECT_EQ(std::string(e.what()).rfind("Broken.scene:", 0), 0u) << e.what();
        EXPECT_GT(e.mark.line, -1);
    }
}
