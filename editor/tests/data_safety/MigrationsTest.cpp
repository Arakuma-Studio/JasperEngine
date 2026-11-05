// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The loaders' use of the migrations (docs/specs/S03-data-safety.md R20-R23, AC17)

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "util/Prefab.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(Migrations, SceneV0ToV1) {
    DecodedScene decoded = decodeSceneFile(fixture("migrations/scene-v0.scene"));
    EXPECT_TRUE(decoded.report.empty());
    EXPECT_EQ(decoded.report.dropped(), 0u);
    EXPECT_EQ(decoded.report.preserved(), 0u);
    EXPECT_EQ(decoded.report.versionRead, 0);
    ASSERT_EQ(decoded.report.infos(), 1u);
    EXPECT_EQ(decoded.report.issues[0].kind, LoadIssueKind::Migrated);
    // a migrated document is not modified
    EXPECT_FALSE(decoded.scene->isModified);

    // its next save writes the current format, the version first, and nothing else changes
    const YAML::Node saved = encodeScene(decoded);
    EXPECT_EQ(saved.begin()->first.Scalar(), "formatVersion");
    EXPECT_EQ(saved["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    // and nothing else changes but what the later migrations change (scene 1 -> 2: the old
    // default shadow quality Low is Auto, a light's shadow bias is in texels; 3 -> 4: the
    // transforms' derived values are not written; 5 -> 6: the Line and Trail Renderers' curves
    // in S27's form, none here) and what a frame computes (a Light's world direction, S63)
    YAML::Node expected = YAML::Load(readText(fixture("migrations/scene-v0.scene")));
    expected["scene"]["shadowQuality"] = "auto";
    for (YAML::Node entity : expected["entities"]) {
        if (entity["components"] && entity["components"]["light"] && entity["components"]["light"]["shadowBias"])
            entity["components"]["light"]["shadowBias"] = 1.0f;
        if (entity["components"] && entity["components"]["light"])
            entity["components"]["light"].remove("worldDirection");
        if (entity["components"] && entity["components"]["transform"]) {
            YAML::Node transform = entity["components"]["transform"];
            for (const char* key : {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"}) transform.remove(key);
        }
    }
    std::string why;
    EXPECT_TRUE(structurallyEqual(withoutKey(saved, "formatVersion"), expected, &why)) << why;
}

TEST(Migrations, NewerFormatGated) {
    DecodedScene decoded = decodeSceneFile(fixture("migrations/scene-v99.scene"));
    ASSERT_EQ(decoded.report.dropped(), 1u);
    EXPECT_TRUE(decoded.report.newerFormat());
    EXPECT_EQ(decoded.report.issues[0].message, "Written by a newer JasperEngine (scene format 99; this editor writes " +
                                                    std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) + ").");
    // loaded best-effort
    EXPECT_EQ(decoded.scene->name, "FromTheFuture");
    EXPECT_EQ(decoded.scene->entities.size(), 1u);

    // a prefab from a newer editor is gated the same way
    LoadReport prefabReport;
    {
        ScopedLoadReport scope(prefabReport);
        DocumentFile prefab = DocumentFiles::read(fixture("migrations/prefab-v99.prefab"), DocumentKind::Prefab);
        EXPECT_EQ(prefab.migration.versionRead, 99);
        EXPECT_EQ(prefab.root["version"].as<int>(), 99);
    }
    ASSERT_EQ(prefabReport.dropped(), 1u);
    EXPECT_EQ(prefabReport.issues[0].message, "Written by a newer JasperEngine (prefab format 99; this editor writes " +
                                                  std::to_string(Prefab::FILE_VERSION) + ").");
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Prefab), Prefab::FILE_VERSION);
}
