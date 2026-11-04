// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// R20 (docs/specs/S29-version-control.md, AC16): saves no longer write the transform values the
// editor derives (worldPosition, worldRotation, worldScale, localMatrix, modelMatrix,
// normalMatrix), a migration drops them from older files, and the world matrices after a load
// are what they were. editor/tests/fixtures/vcs/world/<fixture>.yaml holds every GameObject's
// world values as `scene-dump --world` gave them in the build before the change
// (JASPER_TEST_UPDATE_GOLDEN=1 writes them again).

#include "VcsTestSupport.h"

#include "support/EditorTestSupport.h"
#include "util/Prefab.h"
#include "Stream.h"
#include "StreamMigrations.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    const char* const kDerived[] = {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"};

    fs::path goldenDir() {
        return fs::path(JASPER_TEST_FIXTURES_DIR) / "vcs" / "world";
    }

    std::vector<fs::path> fixtureProjects() {
        std::vector<fs::path> projects;
        for (const auto& entry : fs::directory_iterator(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures")) {
            if (entry.is_directory() && fs::exists(entry.path() / "project.yaml")) projects.push_back(entry.path());
        }
        std::sort(projects.begin(), projects.end());
        return projects;
    }

    // scene name -> GameObject path -> its world values
    YAML::Node worldOf(const YAML::Node& dump) {
        YAML::Node out(YAML::NodeType::Map);
        for (const YAML::Node& scene : dump["scenes"]) {
            YAML::Node entities(YAML::NodeType::Map);
            for (auto it = scene["entities"].begin(); it != scene["entities"].end(); ++it) {
                if (it->second["world"]) entities[it->first.Scalar()] = it->second["world"];
            }
            out[scene["name"].Scalar()] = entities;
        }
        return out;
    }

    bool closeValues(const YAML::Node& a, const YAML::Node& b, std::string& why, const std::string& at) {
        if (a.IsSequence() && b.IsSequence()) {
            if (a.size() != b.size()) {
                why = at + ": " + std::to_string(a.size()) + " values against " + std::to_string(b.size());
                return false;
            }
            for (size_t i = 0; i < a.size(); i++) {
                const double x = a[i].as<double>(), y = b[i].as<double>();
                if (std::fabs(x - y) > 1e-5 * std::max(1.0, std::max(std::fabs(x), std::fabs(y)))) {
                    why = at + "[" + std::to_string(i) + "]: " + a[i].Scalar() + " against " + b[i].Scalar();
                    return false;
                }
            }
            return true;
        }
        if (a.IsMap() && b.IsMap()) {
            if (a.size() != b.size()) {
                why = at + ": " + std::to_string(a.size()) + " keys against " + std::to_string(b.size());
                return false;
            }
            for (auto it = a.begin(); it != a.end(); ++it) {
                const std::string key = it->first.Scalar();
                if (!b[key]) {
                    why = at + "/" + key + " is missing";
                    return false;
                }
                if (!closeValues(it->second, b[key], why, at + "/" + key)) return false;
            }
            return true;
        }
        why = at + ": not the same kind of value";
        return false;
    }

    void expectNoDerivedKeys(const std::string& text, const std::string& what) {
        for (const char* key : kDerived) {
            EXPECT_EQ(text.find(std::string(key) + ":"), std::string::npos) << what << " still writes " << key;
        }
    }

}

TEST(Stream, DerivedTransformNotWritten) {
    // a saved scene has no derived values
    const DecodedScene decoded = decodeSceneFile(fixture("scenes/shader-library-export-2d.scene"));
    const std::string saved = encodeSceneText(decoded);
    EXPECT_NE(saved.find("position:"), std::string::npos);
    expectNoDerivedKeys(saved, "a saved scene");
    // an old fixture with them loads, and saving it drops them
    const std::string old = readText(fixture("migrations/scene-v3-derived.scene"));
    EXPECT_NE(old.find("modelMatrix:"), std::string::npos);
    const DecodedScene migrated = decodeSceneText(old, "scene-v3-derived.scene");
    const std::string resaved = encodeSceneText(migrated);
    expectNoDerivedKeys(resaved, "a re-saved old scene");
    const fs::path expected = fixture("migrations/scene-v3-derived.expected.scene");
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) writeFile(expected, resaved);
    EXPECT_EQ(resaved, readText(expected));
    // what was authored survives: the child's local position under its scaled parent
    EXPECT_NE(resaved.find("position: [0, 1, 0]"), std::string::npos);
    EXPECT_NE(resaved.find("scale: [2, 2, 2]"), std::string::npos);
    // the format version moved on, with the migration that drops them (then S18's identity 4 -> 5
    // and the Animation Player controllers' move to Animators, 5 -> 6 / prefab 6 -> 7, and the
    // upstream curve form 6 -> 7 / prefab 7 -> 8, and the sprite sheets' identity 7 -> 8 / prefab 8 -> 9)
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Scene), 8);
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Prefab), 9);
    EXPECT_EQ(Prefab::FILE_VERSION, 9);
    YAML::Node root = YAML::Load(old);
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    ASSERT_EQ(result.applied.size(), 5u);
    EXPECT_EQ(result.versionRead, 3);
    EXPECT_EQ(root["formatVersion"].as<int>(), 8);
    expectNoDerivedKeys(YAML::Dump(root), "a migrated scene");
}

TEST(Stream, DerivedTransformMigrationReachesPrefabsAndInstances) {
    // a v5 prefab and an instance's overrides lose them too
    YAML::Node prefab = YAML::Load(
        "type: Prefab\nversion: 5\nguid: abcd\nnextEntity: 1003\nroot: 1001\nmembers:\n"
        "  - type: Entity\n    entity: 1001\n    name: Root\n    components:\n      transform:\n        position: [1, 2, 3]\n"
        "        worldPosition: [1, 2, 3]\n        modelMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [1, 2, 3, 1]]\n"
        "    children:\n      - type: Entity\n        entity: 1002\n        name: Child\n        components:\n          transform:\n"
        "            position: [0, 1, 0]\n            normalMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]\n");
    StreamMigrations::migrate(DocumentKind::Prefab, prefab);
    expectNoDerivedKeys(YAML::Dump(prefab), "a migrated prefab");
    EXPECT_EQ(prefab["members"][0]["components"]["transform"]["position"].size(), 3u);
    EXPECT_EQ(prefab["version"].as<int>(), 9);

    YAML::Node scene = YAML::Load(
        "formatVersion: 3\nentities:\n  - type: Entity\n    entity: 20\n    name: Instance\n    components:\n      transform:\n"
        "        position: [0, 0, 0]\n        worldScale: [1, 1, 1]\n    prefabInstance:\n      version: 5\n      overrides:\n"
        "        - registryEntity: 1002\n          components:\n            transform:\n              position: [5, 0, 0]\n"
        "              localMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [5, 0, 0, 1]]\n      addedGameObjects:\n"
        "        - entity: 30\n          parentRegistryEntity: 1002\n          components:\n            transform:\n"
        "              worldRotation: [1, 0, 0, 0]\n");
    StreamMigrations::migrate(DocumentKind::Scene, scene);
    expectNoDerivedKeys(YAML::Dump(scene), "a migrated instance");
    EXPECT_EQ(scene["entities"][0]["prefabInstance"]["overrides"][0]["components"]["transform"]["position"][0].as<int>(), 5);
}

TEST(Stream, DerivedTransformWorldMatricesUnchanged) {
    const bool update = std::getenv("JASPER_TEST_UPDATE_GOLDEN") != nullptr;
    const fs::path editorBinary = fs::path(JASPER_TEST_EDITOR_BINARY);
    for (const fs::path& source : fixtureProjects()) {
        const std::string name = source.filename().string();
        SCOPED_TRACE(name);
        // a copy: a load may write the project's .jasperengine folder
        jasperengine::test::TempDir dir;
        const fs::path project = dir.path() / name;
        fs::copy(source, project, fs::copy_options::recursive);
        const fs::path out = dir.path() / "dump.yaml";
        ProcessSpec spec;
        spec.exe = editorBinary;
        spec.args = {"scene-dump", "--world", "--project", project.string(), "--out", out.string()};
        spec.timeoutMs = 300000;
        // under AddressSanitizer: the CLI's exit leaves its scenes to the process's end, which
        // LeakSanitizer would turn into a failed dump (what is compared is the dump)
        const char* asan = std::getenv("ASAN_OPTIONS");
        spec.env.emplace_back("ASAN_OPTIONS", std::string(asan ? asan : "") + (asan && *asan ? ":" : "") + "detect_leaks=0");
        const ProcessResult result = runOffMain(spec);
        ASSERT_EQ(result.exitCode, 0) << result.message();
        const YAML::Node world = worldOf(YAML::LoadFile(out.string()));
        const fs::path golden = goldenDir() / (name + ".yaml");
        if (update) {
            writeFile(golden, YAML::Dump(world) + "\n");
            continue;
        }
        ASSERT_TRUE(fs::exists(golden)) << golden << " (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        std::string why;
        EXPECT_TRUE(closeValues(YAML::LoadFile(golden.string()), world, why, name)) << why;
    }
}

// A RectTransform's scissor is the clip rectangle the last draw computed, in the pixels of the view
// that drew it (RenderSystem sets it every frame): a save wrote whatever the editor's window last
// laid out, so opening and saving a UI scene again rewrote it (float noise, a resized Game view),
// a change in version control for no edit. Found by tools/ci/upgrade_from_main (an origin/main
// HUD scene changed on its second save). It is read when a file has it and no longer written.
TEST(Stream, DerivedUiScissorNotWritten) {
    const std::string old = readText(fixture("scenes/shader-library-export-ui.scene"));
    ASSERT_NE(old.find("scissor:"), std::string::npos);
    const DecodedScene decoded = decodeSceneText(old, "shader-library-export-ui.scene");
    const std::string saved = encodeSceneText(decoded);
    EXPECT_EQ(saved.find("scissor:"), std::string::npos) << "a saved scene still writes the UI scissor";
    // the layout an author sets is kept
    EXPECT_NE(saved.find("anchorPreset:"), std::string::npos);
    EXPECT_EQ(saved, encodeSceneText(decodeSceneText(saved, "resaved.scene"))) << "a second save is the same";
}
