// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A Tilemap Layer in a scene file (docs/specs/S50-grid-tilemap.md R86, R89, §6.3): chunks that do
// not read (a value that is no string, a key that is no pair of 32-bit integers, a chunk outside
// the cell range) are refused with a load issue instead of throwing out of the decode or reading
// a wrapped number, their text is kept for the save; a layer whose tileset does not load keeps its
// cells, says so in the load report and writes them back.

#include "support/EditorTestSupport.h"

#include "Scene.h"
#include "component/TilemapLayerComponent.h"
#include "tilemap/TileCells.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    constexpr Entity kStreamLayer = 1001;

    std::string tilemapSceneText(const std::string& layerBody) {
        return "formatVersion: 1\nid: 77\nname: Tiles\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nmainCamera: 0\n"
               "entities:\n  - type: Entity\n    entity: 1001\n    name: Layer\n    components:\n      transform:\n        position: [0, 0, 0]\n"
               "      tilemaplayer:\n        version: 1\n" + layerBody;
    }

    // one chunk of 8 x 8 with tile 2 at (1, 1)
    std::string oneChunkText() {
        TileCells cells(8);
        cells.setTile(CellCoord{1, 1}, 2);
        return cells.encodeChunk(ChunkCoord{0, 0});
    }

    const LoadIssue* issueOn(const LoadReport& report, const std::string& field) {
        for (const LoadIssue& issue : report.issues) {
            if (issue.field.find(field) != std::string::npos) return &issue;
        }
        return nullptr;
    }

    std::string describeIssues(const LoadReport& report) {
        std::string out;
        for (const LoadIssue& issue : report.issues) out += issue.consoleLine() + "\n";
        return out;
    }

}

TEST(TilemapStream, AValidChunkLoadsWithoutIssues) {
    DecodedScene decoded = decodeSceneText(tilemapSceneText("        chunks:\n          \"0,0\": \"" + oneChunkText() + "\"\n"));
    const TilemapLayerComponent* layer = decoded.scene->scene->findComponent<TilemapLayerComponent>(kStreamLayer);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->cells.read().getTile(CellCoord{1, 1}), 2);
    // (the format's own migration note aside: the text is written in format 1)
    EXPECT_EQ(issueOn(decoded.report, "chunks"), nullptr) << describeIssues(decoded.report);
    EXPECT_EQ(issueOn(decoded.report, "tileset"), nullptr) << describeIssues(decoded.report);
    for (const LoadIssue& issue : decoded.report.issues) EXPECT_NE(issue.fate, LoadIssueFate::Dropped) << issue.consoleLine();
}

TEST(TilemapStream, UnreadableChunksAreRefusedWithALoadIssue) {
    const std::string chunk = oneChunkText();
    struct Case { const char* what; std::string chunks; const char* key; };
    const Case cases[] = {
        {"a sequence for a chunk", "          \"0,0\": [1, 2]\n", "0,0"},
        {"a map for a chunk", "          \"0,0\": {a: b}\n", "0,0"},
        {"a key past int32", "          \"99999999999,0\": \"" + chunk + "\"\n", "99999999999,0"},
        {"a key with a tail", "          \"0,0x\": \"" + chunk + "\"\n", "0,0x"},
        {"a key of one number", "          \"7\": \"" + chunk + "\"\n", "7"},
        {"a chunk past the cell range", "          \"300000000,0\": \"" + chunk + "\"\n", "300000000,0"},
        {"a bad base64 string", "          \"0,0\": \"@@@@\"\n", "0,0"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        DecodedScene decoded;
        EXPECT_NO_THROW(decoded = decodeSceneText(tilemapSceneText("        chunks:\n" + c.chunks)));
        ASSERT_TRUE(decoded.scene);
        const TilemapLayerComponent* layer = decoded.scene->scene->findComponent<TilemapLayerComponent>(kStreamLayer);
        ASSERT_NE(layer, nullptr) << "the layer itself loads";
        EXPECT_TRUE(layer->cells.read().empty());
        const LoadIssue* issue = issueOn(decoded.report, "chunks");
        ASSERT_NE(issue, nullptr) << describeIssues(decoded.report);
        EXPECT_EQ(issue->kind, LoadIssueKind::InvalidValue);
        // the authored text goes back into the file on save
        EXPECT_NE(encodeSceneText(decoded).find(c.key), std::string::npos);
    }
}

TEST(TilemapStream, AMissingTilesetWarnsAndKeepsTheCells) {
    const std::string chunk = oneChunkText();
    DecodedScene decoded = decodeSceneText(tilemapSceneText("        tileset: missing/none.tileset\n        chunks:\n          \"0,0\": \"" + chunk + "\"\n"));
    const TilemapLayerComponent* layer = decoded.scene->scene->findComponent<TilemapLayerComponent>(kStreamLayer);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->cells.read().cellCount(), 1u);
    const LoadIssue* issue = issueOn(decoded.report, "tileset");
    ASSERT_NE(issue, nullptr) << describeIssues(decoded.report);
    EXPECT_EQ(issue->fate, LoadIssueFate::Info);
    EXPECT_NE(issue->message.find("tileset_missing"), std::string::npos) << issue->message;
    EXPECT_NE(issue->message.find("1 cells"), std::string::npos) << issue->message;
    EXPECT_EQ(issue->value, "missing/none.tileset");
    EXPECT_NE(encodeSceneText(decoded).find(chunk), std::string::npos) << "the cells are saved as they are";
    // no tileset at all is no issue
    DecodedScene none = decodeSceneText(tilemapSceneText("        chunks:\n          \"0,0\": \"" + chunk + "\"\n"));
    EXPECT_EQ(issueOn(none.report, "tileset"), nullptr);
}
