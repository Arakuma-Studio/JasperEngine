// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Version control budgets (docs/specs/S29-version-control.md R24-R26, AC18). The times are
// enforced with the benchmark server's slack (JASPER_TEST_PERF_SLACK, docs/testing.md); elsewhere
// and under a sanitizer they are printed.

#include "VcsTestSupport.h"

#include "support/EditorTestSupport.h"
#include "StreamMigrations.h"
#include "vcs/GitProvider.h"
#include "vcs/scene/SceneDiff.h"

#include <chrono>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace scene = jasperengine::editor::vcs::scene;

namespace {

    using Clock = std::chrono::steady_clock;

    double msSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    void budget(const char* what, double measured, double limit, bool enforced = true) {
        std::printf("[ VcsBench ] %s: %.2f ms (budget %.0f ms%s)\n", what, measured, limit, enforced ? "" : ", reported");
        const double slack = editor::test::perfSlack();
        if (enforced && slack > 0.0) {
            EXPECT_LE(measured, limit * slack) << what;
        }
    }

    // a scene of `count` GameObjects in the current format, each with a transform, a light every
    // 400th (a scene has at most MAX_LIGHTS), a script with properties every fifth; ids from 1000
    std::string bigScene(int count, int changedEvery = 0) {
        std::string text = "formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) +
                           "\nid: 1\nname: Big\nscene:\n  backgroundColor: [0.25, 0.45, 0.65, 1]\nsceneType: scene_3d\nmainCamera: 0\nentities:\n";
        text.reserve(static_cast<size_t>(count) * 420);
        for (int i = 0; i < count; i++) {
            const bool changed = changedEvery > 0 && i % changedEvery == 0;
            text += "  - type: Entity\n    entity: " + std::to_string(1000 + i) + "\n    name: Object " + std::to_string(i) +
                    "\n    components:\n      transform:\n        position: [" + std::to_string(i % 100) + ", " + (changed ? "7" : "0") +
                    ", " + std::to_string(i / 100) + "]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n        visible: true\n";
            if (i % 400 == 0) text += "      light:\n        type: point\n        intensity: " + std::string(changed ? "2.5" : "1") + "\n        range: 10\n";
            if (i % 5 == 0) {
                text += "      script:\n        scripts:\n          - type: lua\n            path: Mover.lua\n            className: Mover\n"
                        "            enabled: true\n            properties:\n              - name: speed\n                type: Float\n"
                        "                value: 1.5\n              - name: target\n                type: EntityReference\n"
                        "                value: " + std::to_string(1000 + (i + 1) % count) + "\n";
            }
        }
        return text;
    }

}

TEST(VcsBench, StatusPollOfATenThousandFileRepository) {
    VcsSandbox box;
    const fs::path repo = box.path("big");
    fs::create_directories(repo);
    box.gitOk(repo, {"init", "-q"});
    for (int i = 0; i < 10000; i++) {
        writeFile(repo / ("assets/folder" + std::to_string(i / 200) + "/file" + std::to_string(i) + ".txt"), std::to_string(i));
    }
    box.gitOk(repo, {"add", "-A"});
    box.gitOk(repo, {"commit", "-q", "-m", "10k files"});
    for (int i = 0; i < 10000; i += 100) writeFile(repo / ("assets/folder" + std::to_string(i / 200) + "/file" + std::to_string(i) + ".txt"), "changed");
    for (int i = 0; i < 100; i++) writeFile(repo / ("assets/new/untracked" + std::to_string(i) + ".txt"), "new");

    GitProvider provider(box.config(repo, repo));
    // warm: the index and the file system cache, as an editor's later polls find them
    double best = 1e9;
    for (int round = 0; round < 4; round++) {
        double ms = 0.0;
        size_t changed = 0;
        std::thread poll([&]() {
            const Clock::time_point start = Clock::now();
            VcsSnapshot snapshot;
            const VcsResult result = provider.status(snapshot, StatusDepth::Fast);
            snapshot.finalize(nullptr);
            ms = msSince(start);
            EXPECT_TRUE(result.ok) << result.message;
            changed = snapshot.changedCount();
        });
        poll.join();
        EXPECT_EQ(changed, 200u);
        if (round > 0) best = std::min(best, ms);
    }
    budget("status poll, 10k-file repository", best, 400.0);
}

TEST(VcsBench, SnapshotSwapAndAggregationAtTwentyThousandEntries) {
    VcsSnapshot raw;
    raw.entries.reserve(20000);
    for (int i = 0; i < 20000; i++) {
        VcsChange change;
        char path[96];
        std::snprintf(path, sizeof(path), "assets/area%02d/zone%03d/item%05d.png", i / 1000, (i / 50) % 1000, i);
        change.path = path;
        change.worktree = i % 7 == 0 ? FileState::Untracked : FileState::Modified;
        if (i % 7 == 0) change.index = FileState::Untracked;
        change.conflict = i == 12345;
        raw.entries.push_back(std::move(change));
    }
    double best = 1e9;
    SnapshotPtr current;
    for (int round = 0; round < 5; round++) {
        VcsSnapshot snapshot = raw;
        const Clock::time_point start = Clock::now();
        snapshot.finalize(nullptr);
        // the main thread's part: the pointer swap
        SnapshotPtr fresh = std::make_shared<const VcsSnapshot>(std::move(snapshot));
        current.swap(fresh);
        best = std::min(best, msSince(start));
    }
    ASSERT_TRUE(current);
    EXPECT_EQ(current->entries.size(), 20000u);
    ASSERT_NE(current->findDir("assets"), nullptr);
    EXPECT_EQ(current->findDir("assets")->state, DirState::Conflict);
    budget("snapshot swap and directory aggregation, 20k entries", best, 1.0);

    // a row's lookup (R25: <= 2 us)
    const Clock::time_point start = Clock::now();
    size_t found = 0;
    for (int i = 0; i < 20000; i++) found += current->find(current->entries[static_cast<size_t>(i)].path) != nullptr;
    const double perRowUs = msSince(start) * 1000.0 / 20000.0;
    EXPECT_EQ(found, 20000u);
    std::printf("[ VcsBench ] status lookup per row: %.3f us (budget 2 us)\n", perRowUs);
    if (editor::test::perfSlack() > 0.0) {
        EXPECT_LE(perRowUs, 2.0 * editor::test::perfSlack());
    }
}

TEST(VcsBench, DiffOfATwoThousandEntityScene) {
    const std::string base = bigScene(2000);
    const std::string current = bigScene(2000, 50);
    double best = 1e9, bestParse = 0.0;
    size_t modified = 0;
    for (int round = 0; round < 4; round++) {
        double ms = 0.0, parseMs = 0.0;
        // on a job thread, as the editor runs it
        std::thread job([&]() {
            const Clock::time_point start = Clock::now();
            // both sides at once, as scene-diff and the merge driver read them
            std::vector<scene::SceneYamlModel> models;
            std::vector<std::string> errors;
            EXPECT_TRUE(scene::SceneYamlModel::parseAll({base, current}, models, errors)) << errors[0] << errors[1];
            parseMs = msSince(start);
            const scene::SceneDiffResult diff = scene::diffScenes(models[0], models[1]);
            ms = msSince(start);
            modified = diff.modified;
        });
        job.join();
        if (round > 0 && ms < best) {
            best = ms;
            bestParse = parseMs;
        }
    }
    EXPECT_EQ(modified, 40u);
    std::printf("[ VcsBench ] (of which parsing both sides, each on a job: %.2f ms; %zu bytes each)\n", bestParse, base.size());
    budget("scene diff, 2,000 GameObjects (parse both + diff)", best, 150.0);
}

TEST(VcsBench, NoProcessOnTheMainThreadDuringACompletedSession) {
    VcsSandbox box;
    const fs::path repo = box.gitRepo("repo", {{"a.txt", "a\n"}, {"b.txt", "b\n"}});
    VcsService service;
    const uint64_t mainBefore = spawnCount(true);
    const uint64_t otherBefore = spawnCount(false);
    service.open(repo, repo / ".jasperengine", box.settings(), false);
    ASSERT_TRUE(pumpUntil(service, [&]() { return service.state() != VcsState::Detecting; }));
    ASSERT_TRUE(service.active()) << service.stateMessage();

    // Finish each operation through its receipt: elapsed time says nothing about which ran.
    writeFile(repo / "a.txt", "changed\n");
    const SnapshotPtr snapshot = refreshed(service, true);
    ASSERT_TRUE(snapshot);
    EXPECT_NE(snapshot->find("a.txt"), nullptr);
    std::vector<VcsLogEntry> entries;
    VcsResult result = await(service, [&](VcsService::Done done) {
        service.log(std::nullopt, 10, 0, [&, done](const VcsResult& r, std::vector<VcsLogEntry> log) {
            entries = std::move(log);
            done(r);
        });
    });
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].message, "initial");
    std::string diff;
    result = await(service, [&](VcsService::Done done) {
        service.textDiff("a.txt", "HEAD", [&, done](const VcsResult& r, std::string text) {
            diff = std::move(text);
            done(r);
        });
    });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(diff.find("+changed"), std::string::npos);
    std::string revision;
    result = await(service, [&](VcsService::Done done) {
        service.commit({"a.txt"}, "edited", false, [&, done](const VcsResult& r, const std::string& rev) {
            revision = rev;
            done(r);
        });
    });
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(revision.empty());
    const SnapshotPtr committed = refreshed(service);
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed->find("a.txt"), nullptr);
    service.close();
    EXPECT_EQ(spawnCount(true), mainBefore);
    EXPECT_GT(spawnCount(false), otherBefore);
}
