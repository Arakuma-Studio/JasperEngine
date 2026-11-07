// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S74 R18, D3: the numbers of a clip come from analyzeClip and the field table and the report's
// JSON hold the same keys. Loop Match (R48) measures the seam on a rig.

#include <gtest/gtest.h>

#include "animation/AnimationClipIO.h"
#include "animation/ClipReport.h"
#include "pool/AnimationClipPool.h"

#include <cmath>
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

namespace {

    // A report's JSON keys in the table's form: bare at the top, dotted under the others
    std::set<std::string> flattened(const nlohmann::json& report){
        std::set<std::string> keys;
        for (const auto& item : report.items()){
            if (item.key() == "loop_seam"){
                for (const auto& inner : item.value().items()) keys.insert(item.key() + "." + inner.key());
            }else{
                keys.insert(item.key());
            }
        }
        return keys;
    }

    anim::ModelNodeInfo node(const std::string& name, int parent, const std::string& path, Vector3 position){
        anim::ModelNodeInfo info;
        info.name = name;
        info.parent = parent;
        info.path = path;
        info.position = position;
        return info;
    }

    anim::ClipRig armRig(){
        anim::ClipRig rig;
        rig.assign("arm", {node("Root", -1, "Root", Vector3(0, 0, 0)), node("Elbow", 0, "Root/Elbow", Vector3(0, 0.5f, 0)),
                           node("Hand", 1, "Root/Elbow/Hand", Vector3(0, 0.5f, 0))});
        return rig;
    }

    std::vector<float> aroundZ(double degrees){
        const double half = degrees * 3.14159265358979323846 / 360.0;
        return {0.0f, 0.0f, (float)std::sin(half), (float)std::cos(half)};
    }

}

TEST(ClipReport, TheTableAndTheJsonHoldTheSameKeys){
    anim::ClipReportRequest request;
    request.clip = sample();
    std::set<std::string> table;
    for (const anim::ClipReportField& field : anim::kClipReportFields){
        EXPECT_TRUE(table.insert(field.key).second) << field.key << " twice in the table";
        EXPECT_STRNE(field.label, "") << field.key;
        EXPECT_STRNE(field.unit, "") << field.key;
    }
    // a stats request has no loop_seam; one with the rig check has every row
    std::set<std::string> stats = table;
    stats.erase("loop_seam.max_error_mm");
    stats.erase("loop_seam.worst_joint");
    EXPECT_EQ(flattened(anim::clipReportJson(anim::analyzeClip(request))), stats);
    const anim::ClipRig rig = armRig();
    request.rig = &rig;
    request.loopSeam = true;
    EXPECT_EQ(flattened(anim::clipReportJson(anim::analyzeClip(request))), table);
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

// ---- Loop Match ----

TEST(ClipReport, LoopMatchIgnoresTheRootsTravelAndSeesABodyJump){
    const anim::ClipRig rig = armRig();
    // the root walks 1 m and comes back to the same pose: a clean loop
    ClipData walks;
    walks.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0, 1})}));
    walks.tracks.push_back(track("Root/Elbow", ClipChannel::Rotation, {key(0, aroundZ(0)), key(0.5f, aroundZ(40)), key(1, aroundZ(0))}));
    anim::ClipReportRequest request;
    request.clip = AnimationClip::build(walks);
    request.rig = &rig;
    request.loopSeam = true;
    anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.loopSeam.has_value());
    EXPECT_LT(report.loopSeam->maxErrorMm, 1e-3) << "the worst point: " << report.loopSeam->worstJoint;

    // the arm ends 20 degrees from where it starts: a 171 mm jump at the hand, with the skin past it
    walks.tracks[1].keys.back().value = aroundZ(20);
    request.clip = AnimationClip::build(walks);
    report = anim::analyzeClip(request);
    EXPECT_GT(report.loopSeam->maxErrorMm, 171.0);
    EXPECT_EQ(report.loopSeam->worstJoint, "Hand");
    EXPECT_EQ(anim::formatClipField(*anim::findClipReportField("loop_seam.max_error_mm"), 0.814), "0.81 mm");

    // a height jump at the root counts
    ClipData hop;
    hop.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0.05f, 1})}));
    request.clip = AnimationClip::build(hop);
    report = anim::analyzeClip(request);
    EXPECT_NEAR(report.loopSeam->maxErrorMm, 50.0, 0.01);
}
