// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Autosave encode and load report overhead (docs/specs/S03-data-safety.md R35, AC19): the
// main-thread encode of a ~2k-entity scene for a backup (its text and the write are a worker's);
// decoding with a load report installed is at most 5% slower than without, on 5k entities.

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // A scene of `count` GameObjects in a few hierarchies, each with a Transform and a light
    std::string syntheticScene(int count) {
        std::string text = "formatVersion: 1\nid: 9000\nname: Bench\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nmainCamera: 0\nentities:\n";
        for (int i = 0; i < count; i++) {
            text += "  - type: Entity\n    entity: " + std::to_string(1001 + i) + "\n    name: Object" + std::to_string(i) +
                    "\n    components:\n      transform:\n        position: [" + std::to_string(i % 50) + ", 0, " + std::to_string(i / 50) +
                    "]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n" +
                    (i % 4 == 0 ? "      light:\n        type: point\n        intensity: 2\n        range: 10\n" : "");
        }
        return text;
    }

    double ms(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

}

TEST(AutosaveEncodeBench, MediumSceneEncode) {
    DecodedScene decoded = decodeSceneText(syntheticScene(2000), "Bench.scene");
    ASSERT_EQ(decoded.scene->entities.size(), 2000u);
    // the main thread builds the YAML tree; the text, its hash and the write are a worker's
    double encode = 1e9, dump = 1e9;
    for (int run = 0; run < 7; run++) {
        const auto start = std::chrono::steady_clock::now();
        const YAML::Node root = Stream::encodeSceneProject(nullptr, decoded.scene.get());
        encode = std::min(encode, ms(start));
        const auto dumpStart = std::chrono::steady_clock::now();
        const std::string text = DocumentFiles::text(root);
        dump = std::min(dump, ms(dumpStart));
        ASSERT_FALSE(text.empty());
    }
    std::printf("[ bench    ] autosave of 2000 entities: encode %.2f ms (main thread), text %.2f ms (worker)\n", encode, dump);
    // R35 asks 10 ms; building yaml-cpp's tree costs ~0.1 ms per GameObject (S03 Deviations), so
    // a slow encode doubles the scene's interval instead. Guarded against regressions in the
    // benchmark run (perfSlack(), docs/testing.md; 0 in a sanitizer build, a Debug build several
    // times slower)
    if (const double slack = perfSlack(); slack > 0.0) {
        EXPECT_LE(encode / 2000.0, 0.25 * slack) << "ms per GameObject";
    }
}

TEST(LoadReportOverheadBench, DecodeWithReport) {
    ensureEngine();
    const YAML::Node root = YAML::Load(syntheticScene(5000));
    auto decodeOnce = [&root](bool withReport) {
        SceneProject scene;
        LoadReport report;
        const auto start = std::chrono::steady_clock::now();
        if (withReport) {
            ScopedLoadReport scope(report);
            Stream::decodeSceneProject(&scene, root, true);
            Stream::decodeSceneProjectEntities(nullptr, &scene, root);
        } else {
            Stream::decodeSceneProject(&scene, root, true);
            Stream::decodeSceneProjectEntities(nullptr, &scene, root);
        }
        const double elapsed = ms(start);
        delete scene.scene;
        return elapsed;
    };
    // alternated, best of each: the build machines are shared and their load comes in bursts
    double without = 1e9, with = 1e9;
    for (int run = 0; run < 9; run++) {
        if (run % 2 == 0) {
            without = std::min(without, decodeOnce(false));
            with = std::min(with, decodeOnce(true));
        } else {
            with = std::min(with, decodeOnce(true));
            without = std::min(without, decodeOnce(false));
        }
    }
    std::printf("[ bench    ] decode of 5000 entities: %.1f ms without a report, %.1f ms with\n", without, with);
    // a ratio of two wall-clock measurements is still a timing: a burst on a shared builder lands
    // on one side (D: 57.1 vs 63.0 ms; E: within 1%), so the benchmark run enforces it
    if (perfSlack() > 0.0) {
        EXPECT_LE(with, without * 1.05 + 1.0);
    }
}
