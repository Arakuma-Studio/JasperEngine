// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Files written by today's editor load with zero issues, and saving them changes nothing but
// the added format version; a second round trip is byte-identical (docs/specs/S03-data-safety.md
// R41, AC18)

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamMigrations.h"
#include "StreamPreserved.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "util/Prefab.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    std::vector<fs::path> corpus(const std::vector<std::string>& extensions) {
        std::vector<fs::path> files;
        const fs::path root = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures";
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            const std::string extension = entry.path().extension().string();
            if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end() ||
                std::find(extensions.begin(), extensions.end(), name) != extensions.end())
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string problems(const LoadReport& report) {
        std::string text;
        for (const LoadIssue& issue : report.issues) {
            if (issue.fate != LoadIssueFate::Info) text += "\n  " + issue.consoleLine();
        }
        return text;
    }

    // Where two texts first differ, a few lines of each
    std::string firstDifference(const std::string& a, const std::string& b) {
        std::istringstream ia(a), ib(b);
        std::string la, lb;
        for (int line = 1;; line++) {
            const bool ha = (bool)std::getline(ia, la);
            const bool hb = (bool)std::getline(ib, lb);
            if (!ha && !hb) return "";
            if (!ha || !hb || la != lb) {
                return "line " + std::to_string(line) + ":\n  saved:    " + (ha ? la : "<end>") + "\n  original: " + (hb ? lb : "<end>");
            }
        }
    }

    // The text without its shaderKeys block (top-level key to the next top-level key)
    std::string withoutShaderKeys(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        bool skipping = false;
        while (std::getline(in, line)) {
            const bool topLevel = !line.empty() && line[0] != ' ' && line[0] != '-';
            if (topLevel) skipping = line.rfind("shaderKeys:", 0) == 0;
            if (!skipping) out += line + "\n";
        }
        return out;
    }

    // shaderKeys as a sorted list: the set is ordered by keys holding custom shader ids, which
    // each session assigns in its own order
    std::vector<std::string> shaderKeySet(const std::string& text) {
        std::vector<std::string> keys;
        const YAML::Node root = YAML::Load(text);
        for (const YAML::Node& key : root["shaderKeys"]) keys.push_back(YAML::Dump(key));
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    // The lines the scene 1 -> 2 migration rewrites (S10: the shadow quality Low is Auto, a
    // light's shadow bias in texels) left out of both texts
    std::string withoutShadowMigration(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            const size_t start = line.find_first_not_of(' ');
            if (start != std::string::npos && (line.compare(start, 14, "shadowQuality:") == 0 || line.compare(start, 11, "shadowBias:") == 0))
                continue;
            out += line + "\n";
        }
        return out;
    }

    // The input's lines without the transforms' derived values, which saves no longer write
    // (scene format 4, docs/specs/S29-version-control.md R20)
    std::string withoutDerivedTransform(const std::string& text) {
        // and a RectTransform's scissor, the clip rectangle of the last draw (no longer written either)
        static const char* const keys[] = {"worldPosition:", "worldRotation:", "worldScale:", "localMatrix:", "modelMatrix:", "normalMatrix:",
                                           "scissor:"};
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            const size_t start = line.find_first_not_of(' ');
            bool derived = false;
            for (const char* key : keys) {
                if (start != std::string::npos && line.compare(start, std::char_traits<char>::length(key), key) == 0) derived = true;
            }
            if (!derived) out += line + "\n";
        }
        return out;
    }

    bool flagOr(const YAML::Node& map, const char* key, bool fallback) {
        const YAML::Node value = std::as_const(map)[key];
        return value && value.IsScalar() ? value.as<bool>() : fallback;
    }

    void removeKeys(YAML::Node map, std::initializer_list<const char*> keys) {
        if (!map || !map.IsMap()) return;
        for (const char* key : keys) map.remove(key);
    }

    // One GameObject's components without what a frame computes, each only under the rule by which
    // a save leaves it out (docs/specs/S63-saved-output-determinism.md). The same key elsewhere (an
    // authored camera's clips, a plain Mesh's vertex counts and bounds, a UI's bounds and vertex
    // count, ranges set by hand) is compared.
    void removeComputedValues(YAML::Node components) {
        const CameraComponent cameraDefaults;
        const TerrainComponent terrainDefaults;
        YAML::Node camera = components["camera"];
        if (camera && camera.IsMap() && flagOr(camera, "autoResize", cameraDefaults.autoResize) &&
            !flagOr(camera, "renderToTexture", cameraDefaults.renderToTexture)) {
            removeKeys(camera, {"leftClip", "rightClip", "bottomClip", "topClip", "aspect"});
        }
        removeKeys(components["light"], {"worldDirection"});
        removeKeys(components["ui"], {"worldAABB"});

        YAML::Node mesh = components["mesh"];
        removeKeys(mesh, {"worldAABB"});
        const bool generated = components["sprite"] || components["tilemap"] || components["meshpolygon"] || components["terrain"] ||
                               components["splineextrude"];
        if (mesh && mesh.IsMap() && generated) {
            removeKeys(mesh, {"aabb", "verticesAABB"});
            YAML::Node submeshes = mesh["submeshes"];
            if (submeshes && submeshes.IsSequence()) {
                for (YAML::Node submesh : submeshes) removeKeys(submesh, {"vertexCount"});
            }
        }

        YAML::Node terrain = components["terrain"];
        if (!terrain || !terrain.IsMap()) return;
        if (flagOr(terrain, "autoSetRanges", terrainDefaults.autoSetRanges)) removeKeys(terrain, {"ranges"});
        // what MeshSystem::applyTerrainMapSettings gives its maps
        removeKeys(terrain["heightMap"], {"wrapU", "wrapV", "releaseDataAfterLoad"});
        YAML::Node blendMaps = terrain["blendMaps"];
        if (blendMaps && blendMaps.IsSequence()) {
            for (YAML::Node blendMap : blendMaps) removeKeys(blendMap, {"wrapU", "wrapV"});
        }
        // a Terrain's two submeshes exist before it first ran: those no edit touched are as none,
        // and a submesh with an authored value is compared
        if (mesh && mesh.IsMap()) {
            const std::string unbuilt = YAML::Dump(Stream::encodeSubmesh(Submesh{}, true, false));
            YAML::Node kept(YAML::NodeType::Sequence);
            YAML::Node submeshes = mesh["submeshes"];
            if (submeshes && submeshes.IsSequence()) {
                for (YAML::Node submesh : submeshes) {
                    if (YAML::Dump(submesh) != unbuilt) kept.push_back(submesh);
                }
            }
            mesh.remove("submeshes");
            if (kept.size() > 0) mesh["submeshes"] = kept;
        }
    }

    void removeComputedValuesUnder(YAML::Node node) {
        if (node.IsMap()) {
            YAML::Node components = node["components"];
            if (components && components.IsMap()) removeComputedValues(components);
            for (auto it = node.begin(); it != node.end(); ++it) removeComputedValuesUnder(it->second);
        } else if (node.IsSequence()) {
            for (YAML::Node child : node) removeComputedValuesUnder(child);
        }
    }

    // A document's text as the comparison sees it, without what a frame computes
    std::string withoutComputedValues(const std::string& text) {
        YAML::Node root = YAML::Load(text);
        removeComputedValuesUnder(root);
        return YAML::Dump(root) + "\n";
    }

    // The input's lines without the format version line
    std::string withoutVersionLine(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            if (line.rfind("formatVersion:", 0) == 0) continue;
            // the scene's asset identity (S04): the project's, which a bare decode does not carry
            if (line.rfind("guid:", 0) == 0 || line.rfind("formerGuids:", 0) == 0) continue;
            out += line + "\n";
        }
        return out;
    }

}

// The comparison below must still see a lost value: the filter takes out only what a save leaves out
TEST(Compat, TheFilterHidesOnlyWhatASaveLeavesOut) {
    const auto entity = [](const std::string& components) { return "entities:\n  - name: A\n    components:\n" + components; };
    const auto same = [&](const std::string& a, const std::string& b) { return withoutComputedValues(entity(a)) == withoutComputedValues(entity(b)); };

    // a camera: its clips and aspect are computed only when the canvas sizes it
    EXPECT_TRUE(same("      camera:\n        autoResize: true\n        aspect: 2\n        rightClip: 9\n", "      camera:\n        autoResize: true\n"));
    EXPECT_FALSE(same("      camera:\n        autoResize: false\n        aspect: 2\n", "      camera:\n        autoResize: false\n"));
    EXPECT_FALSE(same("      camera:\n        autoResize: false\n        rightClip: 9\n", "      camera:\n        autoResize: false\n"));
    EXPECT_FALSE(same("      camera:\n        autoResize: true\n        renderToTexture: true\n        aspect: 2\n",
                      "      camera:\n        autoResize: true\n        renderToTexture: true\n"));
    EXPECT_FALSE(same("      camera:\n        autoResize: true\n        yfov: 1\n", "      camera:\n        autoResize: true\n"));

    // a mesh: its vertex counts and bounds are computed only when a component generates its geometry
    const std::string plainMesh = "      mesh:\n        aabb: [1]\n        verticesAABB: [2]\n        submeshes:\n          - vertexCount: 6\n";
    const std::string bareMesh = "      mesh:\n        submeshes:\n          - {}\n";
    EXPECT_FALSE(same(plainMesh, bareMesh));
    EXPECT_TRUE(same(plainMesh + "      sprite:\n        width: 1\n", bareMesh + "      sprite:\n        width: 1\n"));
    EXPECT_FALSE(same("      mesh:\n        aabb: [1]\n", "      mesh:\n        {}\n"));
    EXPECT_TRUE(same("      mesh:\n        worldAABB: [1]\n", "      mesh:\n        {}\n"));

    // a UI's own values stay
    EXPECT_FALSE(same("      ui:\n        vertexCount: 6\n", "      ui:\n        {}\n"));
    EXPECT_FALSE(same("      ui:\n        aabb: [1]\n", "      ui:\n        {}\n"));

    // a terrain: ranges are computed only under Auto Ranges, and only its maps' first-run settings go
    EXPECT_TRUE(same("      terrain:\n        autoSetRanges: true\n        ranges: [1]\n", "      terrain:\n        autoSetRanges: true\n"));
    EXPECT_FALSE(same("      terrain:\n        autoSetRanges: false\n        ranges: [1]\n", "      terrain:\n        autoSetRanges: false\n"));
    EXPECT_TRUE(same("      terrain:\n        heightMap:\n          path: a\n          wrapU: repeat\n", "      terrain:\n        heightMap:\n          path: a\n"));
    EXPECT_FALSE(same("      terrain:\n        heightMap:\n          path: a\n", "      terrain:\n        heightMap:\n          path: b\n"));
    EXPECT_FALSE(same("      terrain:\n        heightMap:\n          minFilter: nearest\n", "      terrain:\n        heightMap:\n          {}\n"));
    // an authored submesh of a terrain's mesh is compared
    EXPECT_FALSE(same("      terrain: {}\n      mesh:\n        submeshes:\n          - material:\n              name: Rock\n",
                      "      terrain: {}\n      mesh:\n        submeshes: ~\n"));
    const std::string unbuilt = YAML::Dump(Stream::encodeSubmesh(Submesh{}, true, false));
    YAML::Node builtRoot = YAML::Load(entity("      terrain: {}\n      mesh:\n        submeshes: ~\n"));
    builtRoot["entities"][0]["components"]["mesh"]["submeshes"].push_back(YAML::Load(unbuilt));
    EXPECT_EQ(withoutComputedValues(YAML::Dump(builtRoot)), withoutComputedValues(entity("      terrain: {}\n      mesh:\n        submeshes: ~\n")));
}

TEST(Compat, CorpusRoundTrip) {
    const std::vector<fs::path> scenes = corpus({".scene"});
    ASSERT_FALSE(scenes.empty());
    for (const fs::path& file : scenes) {
        SCOPED_TRACE(file.string());
        std::printf("[ corpus   ] %s\n", file.lexically_relative(JASPER_TEST_REPO_DIR).generic_string().c_str());
        std::fflush(stdout);
        const std::string original = readText(file);
        DecodedScene decoded = decodeSceneFile(file);
        EXPECT_TRUE(decoded.report.empty()) << problems(decoded.report);

        const std::string first = encodeSceneText(decoded);
        DecodedScene again = decodeSceneText(first, file.filename().string());
        EXPECT_TRUE(again.report.empty()) << problems(again.report);
        EXPECT_EQ(encodeSceneText(again), first) << "a second round trip changed the file";

        // written by the editor (a scene saved with its build data): only the version line is new
        // (a scene the current editor saved has it already).
        // Hand-written fixtures and scenes holding prefab instances (their members come from the
        // prefab files, which this test does not load) are normalised by their first save.
        const bool editorWritten = original.find("\nmaxValues:") != std::string::npos &&
                                   original.find("prefabInstance") == std::string::npos && original.find("\n      prefab:") == std::string::npos &&
                                   // a model's images are textures by id (the model file's path on the
                                   // machine that saved it), which a bare decode cannot load
                                   original.find("|gltf-image|") == std::string::npos &&
                                   // old Particle Systems (S20 D9's, origin/main's version 2) become
                                   // inline effects on their targets: their GameObjects change
                                   (original.find("particlesystem:") == std::string::npos || original.find("inlineEffect:") != std::string::npos);
        if (editorWritten) {
            const std::string difference =
                firstDifference(withoutComputedValues(withoutDerivedTransform(withoutShadowMigration(withoutShaderKeys(withoutVersionLine(first))))),
                                withoutComputedValues(withoutDerivedTransform(withoutShadowMigration(withoutShaderKeys(withoutVersionLine(original))))));
            EXPECT_TRUE(difference.empty()) << "the save changed more than the version line, at " << difference;
            EXPECT_EQ(shaderKeySet(first), shaderKeySet(original)) << "the shader keys";
            EXPECT_EQ(first.rfind("formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) + "\n", 0), 0u);
        }
    }

    for (const fs::path& file : corpus({".material"})) {
        SCOPED_TRACE(file.string());
        const LoadReport report = DocumentFiles::materialFileReport(file);
        EXPECT_TRUE(report.empty()) << problems(report);
        const YAML::Node encoded = Stream::encodeMaterial(Stream::decodeMaterial(DocumentFiles::read(file, DocumentKind::Material).root));
        const std::string first = DocumentFiles::materialText(encoded, file);
        EXPECT_EQ(first.rfind("formatVersion: 1\n", 0), 0u);
    }

    for (const fs::path& file : corpus({".prefab"})) {
        SCOPED_TRACE(file.string());
        std::printf("[ corpus   ] %s\n", file.lexically_relative(JASPER_TEST_REPO_DIR).generic_string().c_str());
        std::fflush(stdout);
        ensureEngine();
        LoadReport report;
        Prefab prefab;
        {
            ScopedLoadReport scope(report);
            prefab.loadRegistry(DocumentFiles::read(file, DocumentKind::Prefab).root, file.stem().string());
        }
        EXPECT_TRUE(report.empty()) << problems(report);
    }

    for (const fs::path& file : corpus({"project.yaml"})) {
        SCOPED_TRACE(file.string());
        LoadReport report;
        {
            ScopedLoadReport scope(report);
            StreamLoad::reportRootKeys(DocumentFiles::read(file, DocumentKind::Project).root, RootKind::Project);
        }
        EXPECT_TRUE(report.empty()) << problems(report);
    }
}
