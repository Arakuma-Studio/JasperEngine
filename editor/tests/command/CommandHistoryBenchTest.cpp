// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S24 R43 / AC17 budgets: a push with trimming, freeing a big step, the YAML size estimate.
// Instrumented builds (sanitizers) are slower by design: they run the work, not the budgets.

#include "UndoFakeCommand.h"

#include "support/EditorTestSupport.h"
#include "Stream.h"
#include "util/YamlFootprint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr bool checkBudgets = false;
#else
    constexpr bool checkBudgets = true;
#endif

    using Clock = std::chrono::steady_clock;

    double microseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::micro>(duration).count();
    }

    // A step that holds a big YAML tree, freed off the main thread as DeleteEntityCmd is
    class HeavyStep : public UndoFake {
    public:
        YAML::Node data;
        HeavyStep(FakeState& state, int value, int gameObjects): UndoFake(state, "Delete", value) {
            data = YAML::Node(YAML::NodeType::Sequence);
            for (int i = 0; i < gameObjects; i++) {
                YAML::Node entity;
                entity["entity"] = 1000 + i;
                entity["name"] = "GameObject " + std::to_string(i);
                YAML::Node transform;
                for (const char* key : {"position", "rotation", "scale", "worldPosition", "worldRotation", "worldScale"}) {
                    YAML::Node vec(YAML::NodeType::Sequence);
                    for (int c = 0; c < 4; c++) vec.push_back(0.5f * c);
                    transform[key] = vec;
                }
                entity["components"]["transform"] = transform;
                data.push_back(entity);
            }
            bytes = yamlFootprint(data);
            offThread = true;
            recordDeletion = false;
        }
    };

}

TEST(CommandHistoryBench, PushWithTrimming) {
    UndoStatics statics;
    FakeState state;
    CommandHistory::maxEntries = 1000;
    CommandHistory history(1);
    constexpr int pushes = 10000;
    const auto start = Clock::now();
    for (int i = 0; i < pushes; i++) {
        history.addCommand(new UndoFake(state, "Set Value", i + 1));
    }
    const double average = microseconds(Clock::now() - start) / pushes;
    EXPECT_EQ(history.appliedCount(), 1000u);
    std::printf("[ bench ] push with trimming: %.2f us average (budget 50)\n", average);
    if (checkBudgets) { EXPECT_LE(average, 50.0); }
}

TEST(CommandHistoryBench, BigStepReleasedOffTheMainThread) {
    UndoStatics statics;
    FakeState state;
    CommandHistory history(1);
    history.addCommand(new HeavyStep(state, 1, 1000));
    ASSERT_GE(history.bytes(), CommandHistory::OFF_THREAD_RELEASE_BYTES);
    history.undo();
    // the redo tail freed by a new step: BackgroundRelease takes the tree
    const auto start = Clock::now();
    history.addCommand(new UndoFake(state, "next", 2));
    const double elapsed = microseconds(Clock::now() - start);
    std::printf("[ bench ] 1000-GameObject step released in %.1f us on the main thread (budget 500)\n", elapsed);
    if (checkBudgets) { EXPECT_LE(elapsed, 500.0); }
}

TEST(CommandHistoryBench, YamlFootprintCostsLittleNextToTheEncode) {
    // a 1000-GameObject scene, decoded and encoded as the editor does it
    std::string text = "formatVersion: 1\nid: 9001\nname: Bench\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\n"
                       "mainCamera: 0\nentities:\n";
    for (int i = 0; i < 1000; i++) {
        text += "  - type: Entity\n    entity: " + std::to_string(2000 + i) + "\n    name: Cube " + std::to_string(i) +
                "\n    components:\n      transform:\n        position: [" + std::to_string(i) + ", 0, 0]\n"
                "        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n";
    }
    DecodedScene decoded = decodeSceneText(text, "bench.scene");
    ASSERT_TRUE(decoded.scene);
    ASSERT_EQ(decoded.scene->entities.size(), 1000u);

    // the best of a few runs of each (a shared build server)
    double encodeTime = 1e12;
    double footprintTime = 1e12;
    size_t bytes = 0;
    for (int run = 0; run < 5; run++) {
        const auto encodeStart = Clock::now();
        YAML::Node encoded = encodeScene(decoded);
        encodeTime = std::min(encodeTime, microseconds(Clock::now() - encodeStart));
        const auto footprintStart = Clock::now();
        bytes = yamlFootprint(encoded);
        footprintTime = std::min(footprintTime, microseconds(Clock::now() - footprintStart));
    }
    EXPECT_GT(bytes, 1000u * 64u);
    std::printf("[ bench ] yamlFootprint %.0f us, encode %.0f us (%.1f%%, budget 10%%)\n", footprintTime, encodeTime,
                100.0 * footprintTime / encodeTime);
    if (checkBudgets) { EXPECT_LE(footprintTime, encodeTime * 0.10); }
}
