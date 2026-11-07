// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S74 R18, D3: the numbers of a clip come from analyzeClip and the field table and the report's
// JSON hold the same keys.

#include <gtest/gtest.h>

#include "animation/AnimationClipIO.h"
#include "animation/ClipReport.h"
#include "pool/AnimationClipPool.h"

#include <filesystem>
#include <fstream>
#include <set>

using namespace jasperengine;

namespace {

    ClipKey key(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.keys = std::move(keys);
        return t;
    }

    std::shared_ptr<const AnimationClip> sample(){
        ClipData data;
        data.tracks.push_back(track("Moves", ClipChannel::Position, {key(0, {0, 0, 0}), key(1.5f, {1, 0, 0})}));
        data.tracks.push_back(track("Holds", ClipChannel::Position, {key(0, {1, 2, 3}), key(0.5f, {1, 2, 3}), key(1, {1, 2, 3})}));
        data.tracks.push_back(track("Holds", ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0, 0, 1})}));
        data.tracks.push_back(track("OneKey", ClipChannel::Position, {key(0, {1, 1, 1})}));
        return AnimationClip::build(data);
    }

}

TEST(ClipReport, TheTableAndTheJsonHoldTheSameKeys){
    anim::ClipReportRequest request;
    request.clip = sample();
    const nlohmann::json report = anim::clipReportJson(anim::analyzeClip(request));
    std::set<std::string> table;
    for (const anim::ClipReportField& field : anim::kClipReportFields){
        EXPECT_TRUE(report.contains(field.key)) << field.key << " is in the table, not in the JSON";
        EXPECT_TRUE(table.insert(field.key).second) << field.key << " twice in the table";
        EXPECT_STRNE(field.label, "") << field.key;
        EXPECT_STRNE(field.unit, "") << field.key;
    }
    for (const auto& item : report.items()){
        EXPECT_TRUE(table.count(item.key())) << item.key() << " is in the JSON, not in the table";
    }
}

TEST(ClipReport, StatsOfABuiltClip){
    std::shared_ptr<const AnimationClip> clip = sample();
    anim::ClipReportRequest request;
    request.clip = clip;
    const anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.ok);
    EXPECT_EQ(report.runtimeBytes, clip->memoryBytes());
    EXPECT_FALSE(report.fileBytes.has_value());
    EXPECT_EQ(report.keys, 8u);
    EXPECT_EQ(report.tracks, 4u);
    EXPECT_FLOAT_EQ(report.lengthSeconds, 1.5f);
    EXPECT_EQ(report.constantTracks, 2u) << "Holds twice: not Moves, and one key is already minimal";
    const nlohmann::json json = anim::clipReportJson(report);
    EXPECT_EQ(json["runtime_bytes"].get<uint64_t>(), clip->memoryBytes());
    EXPECT_TRUE(json["file_bytes"].is_null());
    EXPECT_EQ(json["length_s"].get<double>(), 1.5);
}

TEST(ClipReport, TangentsThatMoveAreNotConstant){
    ClipData data;
    ClipKey a = key(0, {1, 1, 1});
    a.inTangent = {0, 0, 0};
    a.outTangent = {1, 0, 0};
    ClipKey b = key(1, {1, 1, 1});
    b.inTangent = {0, 0, 0};
    b.outTangent = {0, 0, 0};
    data.tracks.push_back(track("Curve", ClipChannel::Position, {a, b}));
    anim::ClipReportRequest request;
    request.clip = AnimationClip::build(data);
    EXPECT_EQ(anim::analyzeClip(request).constantTracks, 0u) << "equal keys, and an overshoot between them";
}

TEST(ClipReport, AFileIsLoadedThroughThePoolAndLeavesItAsItWas){
    AnimationClipPool::clear();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "je_clip_report_test";
    std::filesystem::create_directories(directory);
    const std::filesystem::path file = directory / "walk.anim";
    ClipData data;
    data.tracks.push_back(track("Moves", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 0, 0})}));
    const std::string text = anim::writeClip(data);
    {
        std::ofstream out(file, std::ios::binary);
        out << text;
    }

    anim::ClipReportRequest request;
    request.reference = file.generic_string();
    const anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.ok) << report.error;
    ASSERT_TRUE(report.fileBytes.has_value());
    EXPECT_EQ(*report.fileBytes, text.size());
    EXPECT_GT(report.runtimeBytes, 0u);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u) << "asking does not change Clip memory";

    // a clip somebody holds stays, and is the one reported
    std::shared_ptr<const AnimationClip> held = AnimationClipPool::getOrLoad(request.reference);
    ASSERT_TRUE(held);
    const uint64_t heldBytes = AnimationClipPool::loadedBytes();
    EXPECT_EQ(anim::analyzeClip(request).runtimeBytes, held->memoryBytes());
    EXPECT_EQ(AnimationClipPool::loadedBytes(), heldBytes);

    request.reference = (directory / "missing.anim").generic_string();
    const anim::ClipReport missing = anim::analyzeClip(request);
    EXPECT_FALSE(missing.ok);
    EXPECT_FALSE(missing.error.empty());

    held.reset();
    AnimationClipPool::clear();
    std::filesystem::remove_all(directory);
}

TEST(ClipReport, FieldsPrintForPeople){
    const anim::ClipReportField* memory = anim::findClipReportField("runtime_bytes");
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(anim::formatClipField(*memory, 512.0), "512 B");
    EXPECT_EQ(anim::formatClipField(*memory, 31.0 * 1024.0), "31 KB");
    EXPECT_EQ(anim::formatClipField(*memory, 3.5 * 1024.0 * 1024.0), "3.5 MB");
    EXPECT_EQ(anim::formatClipField(*anim::findClipReportField("keys"), 2340.0), "2,340");
    EXPECT_EQ(anim::formatClipField(*anim::findClipReportField("keys"), 1234567.0), "1,234,567");
    EXPECT_EQ(anim::formatClipField(*anim::findClipReportField("length_s"), 1.2), "1.20 s");
    EXPECT_EQ(anim::findClipReportField("not_a_key"), nullptr);
}
