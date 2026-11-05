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
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/TerrainComponent.h"

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
    // upstream curve form 6 -> 7 / prefab 7 -> 8)
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Scene), 7);
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Prefab), 8);
    EXPECT_EQ(Prefab::FILE_VERSION, 8);
    YAML::Node root = YAML::Load(old);
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    ASSERT_EQ(result.applied.size(), 4u);
    EXPECT_EQ(result.versionRead, 3);
    EXPECT_EQ(root["formatVersion"].as<int>(), 7);
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
    EXPECT_EQ(prefab["version"].as<int>(), 8);

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

// What a frame computes (spec S63): an auto-resizing camera's clips and aspect are the canvas's,
// a Light's world direction and a Mesh's world bounds its transform's, a Terrain's Auto Ranges its
// camera's far clip. Written, a save followed what was drawn before it. Read when a file has them.
TEST(Stream, ValuesAFrameComputesNotWritten) {
    const std::string old =
        "formatVersion: 7\nid: 43\nname: Derived\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nmainCamera: 0\nentities:\n"
        "  - type: Entity\n    entity: 1001\n    name: Sized\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      camera:\n        autoResize: true\n        rightClip: 1280\n        topClip: 661\n        aspect: 1.9364599\n        farClip: 77\n"
        "  - type: Entity\n    entity: 1002\n    name: Fixed\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      camera:\n        autoResize: false\n        rightClip: 321\n        aspect: 2.5\n"
        "  - type: Entity\n    entity: 1003\n    name: Lamp\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      light:\n        direction: [1, 0, 0]\n        worldDirection: [0, 0, 1]\n"
        "  - type: Entity\n    entity: 1004\n    name: Box\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      mesh:\n        aabb:\n          min: [1, 1, 1]\n          max: [2, 2, 2]\n        worldAABB:\n          min: [3, 3, 3]\n          max: [4, 4, 4]\n"
        "  - type: Entity\n    entity: 1005\n    name: Land\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      terrain:\n        autoSetRanges: true\n        ranges: [8, 4000]\n"
        "  - type: Entity\n    entity: 1006\n    name: Hand\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      terrain:\n        autoSetRanges: false\n        ranges: [9, 99]\n";
    const DecodedScene decoded = decodeSceneText(old, "derived.scene");
    Scene* scene = decoded.scene->scene;
    EXPECT_FLOAT_EQ(scene->getComponent<CameraComponent>(1001).rightClip, 1280.0f) << "read when a file has it";
    EXPECT_FLOAT_EQ(scene->getComponent<LightComponent>(1003).worldDirection.z, 1.0f) << "read when a file has it";
    EXPECT_EQ(scene->getComponent<TerrainComponent>(1005).ranges.size(), 2u) << "read when a file has it";

    const YAML::Node saved = YAML::Load(encodeSceneText(decoded));
    const auto components = [&saved](const std::string& name) {
        for (const YAML::Node& entity : saved["entities"]) {
            if (entity["name"].as<std::string>() == name) return entity["components"];
        }
        ADD_FAILURE() << "no " << name;
        return YAML::Node();
    };
    for (const char* key : {"leftClip", "rightClip", "bottomClip", "topClip", "aspect"}) {
        EXPECT_FALSE(components("Sized")["camera"][key]) << key;
    }
    EXPECT_EQ(components("Sized")["camera"]["farClip"].as<float>(), 77.0f) << "what the canvas does not give stays";
    EXPECT_EQ(components("Fixed")["camera"]["rightClip"].as<float>(), 321.0f) << "a camera without Auto Resize keeps its own";
    EXPECT_EQ(components("Fixed")["camera"]["aspect"].as<float>(), 2.5f) << "a camera without Auto Resize keeps its own";
    EXPECT_FALSE(components("Lamp")["light"]["worldDirection"]);
    EXPECT_FALSE(components("Box")["mesh"]["worldAABB"]);
    EXPECT_TRUE(components("Box")["mesh"]["aabb"]) << "the geometry's bounds are not computed again";
    EXPECT_FALSE(components("Land")["terrain"]["ranges"]);
    EXPECT_EQ(components("Hand")["terrain"]["ranges"].size(), 2u) << "ranges set by hand stay";
}

// A Terrain's mesh and maps as its first run leaves them, whether or not it ran (spec S63): two
// submeshes, its generator's counts and bounds not written, its height and blend maps clamped and
// the height map's pixels kept. A draw wrote those, so a save followed what was drawn before it.
TEST(Stream, ATerrainSavesWhatItsFirstRunLeaves) {
    const std::string old =
        "formatVersion: 7\nid: 44\nname: Land\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nmainCamera: 0\nentities:\n"
        "  - type: Entity\n    entity: 1001\n    name: Land\n    components:\n      transform:\n        position: [0, 0, 0]\n"
        "      terrain:\n        heightMap:\n          textureType: 2D\n          source: path\n          path: height.png\n"
        "          wrapU: repeat\n          wrapV: repeat\n          releaseDataAfterLoad: true\n"
        "        blendMaps:\n          - textureType: 2D\n            source: path\n            path: blend.png\n"
        "            wrapU: repeat\n            wrapV: repeat\n"
        "      mesh:\n        submeshes: ~\n        aabb:\n          min: [1, 1, 1]\n          max: [2, 2, 2]\n";
    const DecodedScene decoded = decodeSceneText(old, "land.scene");
    const YAML::Node saved = YAML::Load(encodeSceneText(decoded));
    const YAML::Node components = saved["entities"][0]["components"];
    EXPECT_EQ(components["terrain"]["heightMap"]["wrapU"].as<std::string>(), "clamp_to_edge");
    EXPECT_EQ(components["terrain"]["heightMap"]["wrapV"].as<std::string>(), "clamp_to_edge");
    EXPECT_EQ(components["terrain"]["heightMap"]["releaseDataAfterLoad"].as<std::string>(), "false");
    EXPECT_EQ(components["terrain"]["blendMaps"][0]["wrapU"].as<std::string>(), "clamp_to_edge");
    ASSERT_EQ(components["mesh"]["submeshes"].size(), 2u);
    for (const YAML::Node& submesh : components["mesh"]["submeshes"]) EXPECT_FALSE(submesh["vertexCount"]);
    EXPECT_FALSE(components["mesh"]["aabb"]);
    EXPECT_FALSE(components["mesh"]["verticesAABB"]);
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
