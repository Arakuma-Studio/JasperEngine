// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R18, D3: the numbers of a clip come from analyzeClip and the field table and the report's
// JSON hold the same keys.

#include <gtest/gtest.h>

#include "animation/AnimationClipIO.h"
#include "animation/ClipOptimize.h"
#include "animation/ClipReport.h"
#include "animation/ClipStatistics.h"
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

    // A report's JSON keys in the table's form: bare at the top and under compare, dotted under the others
    std::set<std::string> flattened(const nlohmann::json& report){
        std::set<std::string> keys;
        for (const auto& item : report.items()){
            if (item.key() == "compare"){
                for (const auto& inner : item.value().items()) keys.insert(inner.key());
            }else if (item.key() == "loop_seam" || item.key() == "root_motion"){
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
    const nlohmann::json report = anim::clipReportJson(anim::analyzeClip(request));
    std::set<std::string> stats;
    std::set<std::string> table;
    for (const anim::ClipReportField& field : anim::kClipReportFields){
        EXPECT_TRUE(table.insert(field.key).second) << field.key << " twice in the table";
        EXPECT_STRNE(field.label, "") << field.key;
        EXPECT_STRNE(field.unit, "") << field.key;
        if (field.section == anim::ClipFieldSection::Stats) stats.insert(field.key);
    }
    // a stats request has the Stats rows, no more
    EXPECT_EQ(flattened(report), stats);

    // a report with every check, and a reduction's entry, hold the rest
    const anim::ClipRig rig = armRig();
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    data.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0.5f, 0, 0})}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    request.clip = clip;
    request.rig = &rig;
    request.loopSeam = true;
    request.rootMotion = true;
    request.compareTo = clip;
    request.toleranceMm = 1.0;
    std::set<std::string> full = flattened(anim::clipReportJson(anim::analyzeClip(request)));
    // a named value: items() of a temporary would outlive it (no lifetime extension before C++23)
    const nlohmann::json reduction = anim::clipOptimizeJson(anim::ClipOptimizeResult());
    for (const auto& item : reduction.items()) full.insert(item.key());
    for (const char* notAField : {"clip", "written_to", "kept", "kept_all_keys"}) full.erase(notAField);
    EXPECT_EQ(full, table);
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
    EXPECT_EQ(report.audioCues, 0u);
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

TEST(ClipReport, FieldsInMillimetresAndTheLines){
    EXPECT_EQ(anim::formatClipField("max_error_mm", 0.814), "0.81 mm");
    EXPECT_EQ(anim::formatClipField("root_motion.average_speed_mps", 3.125), "3.12 m/s");
    anim::ClipReport report;
    report.lengthSeconds = 1.2f;
    report.frameRate = 30.0f;
    report.tracks = 65;
    report.keys = 2340;
    report.runtimeBytes = 31 * 1024;
    report.fileBytes = 112 * 1024;
    report.constantTracks = 9;
    EXPECT_EQ(anim::clipInfoLine(report), "1.20 s \xC2\xB7 30 Hz \xC2\xB7 65 tracks \xC2\xB7 2,340 keys \xC2\xB7 Memory 31 KB \xC2\xB7 File 112 KB \xC2\xB7 9 constant tracks");
    report.fileBytes.reset();
    report.constantTracks = 0;
    EXPECT_EQ(anim::clipInfoLine(report), "1.20 s \xC2\xB7 30 Hz \xC2\xB7 65 tracks \xC2\xB7 2,340 keys \xC2\xB7 Memory 31 KB");
    anim::ClipErrorReport error;
    error.maxErrorMm = 0.81;
    error.meanErrorMm = 0.21;
    EXPECT_EQ(anim::clipErrorLine(error), "Error: max 0.81 mm, mean 0.21 mm") << "VAT's wording";
}

// ---- measureClipError: AC9's planted move ----

TEST(ClipReport, APlanted5mmBoneMoveFailsACompare){
    const anim::ClipRig rig = armRig();
    ClipData reference;
    reference.tracks.push_back(track("Root/Elbow", ClipChannel::Position, {key(0, {0, 0.5f, 0}), key(1, {0, 0.5f, 0})}));
    reference.tracks.push_back(track("Root", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(30))}));
    ClipData moved = reference;
    moved.tracks[0].keys[1].value[0] += 0.005f;  // 5 mm at the end of the clip

    anim::ClipReportRequest request;
    request.clip = AnimationClip::build(moved);
    request.rig = &rig;
    request.compareTo = AnimationClip::build(reference);
    request.compareToName = "reference";
    request.toleranceMm = 1.0;
    const anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.compare.has_value());
    EXPECT_TRUE(report.compare->error.measured);
    EXPECT_NEAR(report.compare->error.maxErrorMm, 5.0, 0.01);
    EXPECT_TRUE(report.compare->error.worstJoint == "Elbow" || report.compare->error.worstJoint == "Hand") << report.compare->error.worstJoint << ": the moved bone, or its child, the same 5 mm away";
    EXPECT_NEAR(report.compare->error.worstTimeSeconds, 1.0f, 1e-5);
    ASSERT_TRUE(report.compare->withinTolerance.has_value());
    EXPECT_FALSE(*report.compare->withinTolerance);

    request.clip = request.compareTo;
    const anim::ClipReport same = anim::analyzeClip(request);
    EXPECT_EQ(same.compare->error.maxErrorMm, 0.0);
    EXPECT_TRUE(*same.compare->withinTolerance);
}

TEST(ClipReport, TheErrorShowsAtVirtualPointsOnALeafThatOnlyTurns){
    // the joint's origin stays; its skin at 100 mm moves. A joints-only metric says 0
    const anim::ClipRig rig = armRig();
    ClipData reference;
    reference.tracks.push_back(track("Root/Elbow/Hand", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(0))}));
    ClipData turned;
    turned.tracks.push_back(track("Root/Elbow/Hand", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(10))}));
    const anim::ClipErrorReport report = anim::measureClipError(*AnimationClip::build(reference), *AnimationClip::build(turned), rig, 100.0);
    ASSERT_TRUE(report.measured);
    const double expected = 2.0 * std::sin(10.0 * 3.14159265358979323846 / 360.0) * 100.0;
    EXPECT_NEAR(report.maxErrorMm, expected, 0.01);
}

TEST(ClipReport, ADifferenceBetweenKeysIsSeen){
    // both pass through the same values at their key times; only halfway do they part
    const anim::ClipRig rig = armRig();
    ClipData a;
    a.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0, 0})}));
    ClipData b;
    b.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0, 0})}));
    ClipKey up = key(0, {0, 0, 0});
    up.inTangent = {0, 0, 0};
    up.outTangent = {4, 0, 0};
    ClipKey down = key(1, {0, 0, 0});
    down.inTangent = {-4, 0, 0};
    down.outTangent = {0, 0, 0};
    b.tracks[0].keys = {up, down};
    const anim::ClipErrorReport report = anim::measureClipError(*AnimationClip::build(a), *AnimationClip::build(b), rig, 100.0);
    EXPECT_GT(report.maxErrorMm, 100.0);
}

TEST(ClipReport, NoTrackOnTheRigIsNotZeroError){
    const anim::ClipRig rig = armRig();
    ClipData elsewhere;
    elsewhere.tracks.push_back(track("Other", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 0, 0})}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(elsewhere);
    const anim::ClipErrorReport report = anim::measureClipError(*clip, *clip, rig, 100.0);
    EXPECT_FALSE(report.measured) << "nothing was measured: no number to trust";
}

TEST(ClipReport, TracksThatAreNotBonesCountWhenTheyDiffer){
    const anim::ClipRig rig = armRig();
    ClipData a;
    a.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0, 0})}));
    a.tracks.push_back(track("Root", ClipChannel::Morph, {key(0, {0.5f}), key(1, {0.5f})}));
    ClipData b = a;
    b.tracks[1].keys[1].value[0] = 0.75f;
    ClipEventData event;
    event.time = 0.5f;
    event.function = "onStep";
    b.events.push_back(event);
    ClipAudioCue cue;
    cue.time = 0.25f;
    cue.kind = ClipCueKind::Snapshot;
    cue.snapshot = "Quiet";
    b.audio.push_back(cue);
    b.length = 3.0f;
    anim::ClipReportRequest request;
    request.clip = AnimationClip::build(b);
    request.rig = &rig;
    request.compareTo = AnimationClip::build(a);
    anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.compare.has_value());
    EXPECT_EQ(report.compare->unmeasuredDifferences, 1u);
    EXPECT_FALSE(report.compare->eventsMatch);
    EXPECT_FALSE(report.compare->audioCuesMatch);
    EXPECT_EQ(report.audioCues, 1u);
    EXPECT_FALSE(report.compare->lengthMatches);
    request.clip = request.compareTo;
    report = anim::analyzeClip(request);
    EXPECT_EQ(report.compare->unmeasuredDifferences, 0u);
    EXPECT_TRUE(report.compare->eventsMatch);
    EXPECT_TRUE(report.compare->audioCuesMatch);
    EXPECT_TRUE(report.compare->lengthMatches);
}

// ---- Loop Match and the root's numbers ----

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
    request.rootMotion = true;
    anim::ClipReport report = anim::analyzeClip(request);
    ASSERT_TRUE(report.loopSeam.has_value());
    EXPECT_LT(report.loopSeam->maxErrorMm, 1e-3) << "the worst point: " << report.loopSeam->worstJoint;
    ASSERT_TRUE(report.rootMotion.has_value());
    EXPECT_EQ(report.rootMotion->bone, "Root");
    EXPECT_NEAR(report.rootMotion->averageSpeedMps, 1.0, 1e-5);
    EXPECT_NEAR(report.rootMotion->netTranslationMetres.z, 1.0f, 1e-5);
    EXPECT_NEAR(report.rootMotion->loopGapMm, 0.0, 1e-3);

    // the arm ends 20 degrees from where it starts: a 171 mm jump at the hand, with the skin past it
    walks.tracks[1].keys.back().value = aroundZ(20);
    request.clip = AnimationClip::build(walks);
    report = anim::analyzeClip(request);
    EXPECT_GT(report.loopSeam->maxErrorMm, 171.0);
    EXPECT_EQ(report.loopSeam->worstJoint, "Hand");

    // a height jump at the root counts in both
    ClipData hop;
    hop.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 0.05f, 1})}));
    request.clip = AnimationClip::build(hop);
    report = anim::analyzeClip(request);
    EXPECT_NEAR(report.rootMotion->loopGapMm, 50.0, 0.01);
    EXPECT_NEAR(report.loopSeam->maxErrorMm, 50.0, 0.01);
}

TEST(ClipReport, TheRootTurningTwiceReadsAs720Degrees){
    const anim::ClipRig rig = armRig();
    ClipData spin;
    std::vector<ClipKey> keys;
    for (int i = 0; i <= 16; i++) keys.push_back(key((float)i * 0.125f, {0, (float)std::sin(i * 3.14159265358979323846 / 8.0), 0, (float)std::cos(i * 3.14159265358979323846 / 8.0)}));
    spin.tracks.push_back(track("Root", ClipChannel::Rotation, keys));
    spin.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {0, 0, 0}), key(2, {0, 0, 0})}));
    anim::ClipReportRequest request;
    request.clip = AnimationClip::build(spin);
    request.rig = &rig;
    request.rootMotion = true;
    const anim::ClipReport report = anim::analyzeClip(request);
    EXPECT_NEAR(report.rootMotion->netYawDegrees, 720.0, 0.5);
    EXPECT_NEAR(report.rootMotion->averageYawDegS, 360.0, 0.5);
}

// ---- what check --anims warns of ----

TEST(ClipReport, RedundantKeysAreThoseTheirNeighboursGive){
    ClipData data;
    data.tracks.push_back(track("Line", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 0, 0}), key(2, {2, 0, 0}), key(3, {3, 0, 0})}));
    data.tracks.push_back(track("Bend", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 0, 0}), key(2, {0, 0, 0})}));
    data.tracks.push_back(track("Spin", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(10)), key(2, aroundZ(20))}));
    ClipKey eased = key(0, {0, 0, 0});
    eased.ease = EaseType::QUAD_OUT;
    data.tracks.push_back(track("Eased", ClipChannel::Position, {eased, key(1, {1, 0, 0}), key(2, {2, 0, 0})}));
    anim::normalize(data);
    EXPECT_EQ(anim::countRedundantKeys(data), 3u) << "two on Line, one on Spin; Bend bends and Eased is not linear";
}

TEST(ClipReport, RotationFlipsAreFoundInTheFileAndAcrossAWideSegment){
    // a file whose keys change sign: the reader takes that out, and says so
    const std::string flipped = "{\"format\": \"jasperengine.anim\", \"version\": 1, \"tracks\": [{\"path\": \"Arm\", \"channel\": \"rotation\", \"keys\": ["
                                "[0, [0, 0, 0, 1]], [1, [0, 0, -0.1, -0.994987]]]}], \"events\": []}";
    const anim::ClipParseResult parsed = anim::parseClip(flipped);
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.rotationSignFlips.size(), 1u);
    EXPECT_EQ(parsed.rotationSignFlips[0].first, "Arm");
    EXPECT_FLOAT_EQ(parsed.rotationSignFlips[0].second, 1.0f);
    EXPECT_TRUE(anim::parseClip(anim::writeClip(*parsed.data)).rotationSignFlips.empty()) << "the canonical file has none";

    ClipData wide;
    wide.tracks.push_back(track("Arm", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(170))}));
    wide.tracks.push_back(track("Fine", ClipChannel::Rotation, {key(0, aroundZ(0)), key(1, aroundZ(120))}));
    anim::normalize(wide);
    const std::vector<anim::ClipRotationFlip> flips = anim::findRotationFlips(wide);
    ASSERT_EQ(flips.size(), 1u);
    EXPECT_EQ(flips[0].path, "Arm");
}

TEST(ClipReport, TheOptimizeRecordRoundTrips){
    ClipData data;
    anim::ClipOptimizeRecord record;
    record.toleranceMm = 2.0;
    record.skinDistanceMm = 50.0;
    record.maxErrorMm = 1.5;
    record.sourceSha1 = "abc";
    record.source = "assets/anim/walk.anim";
    anim::writeClipOptimizeRecord(data, record);
    record.maxErrorMm = 1.25;
    anim::writeClipOptimizeRecord(data, record);
    ASSERT_EQ(data.unknownKeys.size(), 1u) << "one block, replaced";
    const anim::ClipOptimizeRecord read = anim::readClipOptimizeRecord(data);
    EXPECT_TRUE(read.present);
    EXPECT_EQ(read.maxErrorMm, 1.25);
    EXPECT_EQ(read.source, "assets/anim/walk.anim");
    EXPECT_FALSE(anim::readClipOptimizeRecord(ClipData()).present);
}

// ---- Animation Statistics ----

TEST(ClipStatistics, RowsPrintTheReportAndTheRecord){
    AnimationClipPool::clear();
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "je_clip_statistics_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets/anim");
    {
        std::ofstream(root / "project.yaml") << "name: stats\nassetsDir: assets\n";
    }
    ClipData plain;
    plain.tracks.push_back(track("Moves", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 0, 0})}));
    {
        std::ofstream(root / "assets/anim/plain.anim", std::ios::binary) << anim::writeClip(plain);
    }
    ClipData reduced = plain;
    anim::ClipOptimizeRecord record;
    record.toleranceMm = 1.0;
    record.maxErrorMm = 0.5;
    anim::writeClipOptimizeRecord(reduced, record);
    {
        std::ofstream(root / "assets/anim/reduced.anim", std::ios::binary) << anim::writeClip(reduced);
    }
    std::vector<std::string> notes;
    const std::vector<anim::ProjectClip> clips = anim::listProjectClips(root, notes);
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_TRUE(notes.empty());
    anim::ClipStatRow first = anim::readClipStatRow(clips[0]);
    anim::ClipStatRow second = anim::readClipStatRow(clips[1]);
    EXPECT_EQ(first.name, "plain");
    EXPECT_EQ(first.source, "plain.anim");
    EXPECT_EQ(first.clip.display, "assets/anim/plain.anim");
    EXPECT_FALSE(first.maxErrorMm.has_value()) << "no reduction, no number";
    ASSERT_TRUE(second.maxErrorMm.has_value());
    EXPECT_EQ(*second.maxErrorMm, 0.5);

    anim::ClipReportRequest request;
    request.reference = clips[0].reference;
    const anim::ClipReport tool = anim::analyzeClip(request);
    EXPECT_EQ(first.report.runtimeBytes, tool.runtimeBytes);
    EXPECT_EQ(first.report.keys, tool.keys);
    EXPECT_EQ(*first.report.fileBytes, *tool.fileBytes);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u) << "a scan keeps nothing loaded";

    first.users = 2;
    first.loaded = true;
    EXPECT_TRUE(anim::clipStatRowShown(first, anim::ClipStatFilter::Loaded, 0));
    EXPECT_FALSE(anim::clipStatRowShown(second, anim::ClipStatFilter::Loaded, 0));
    EXPECT_FALSE(anim::clipStatRowShown(first, anim::ClipStatFilter::Unused, 0));
    EXPECT_TRUE(anim::clipStatRowShown(second, anim::ClipStatFilter::Unused, 0));
    EXPECT_TRUE(anim::clipStatRowShown(first, anim::ClipStatFilter::OverBudget, first.report.runtimeBytes - 1));
    EXPECT_FALSE(anim::clipStatRowShown(first, anim::ClipStatFilter::OverBudget, first.report.runtimeBytes));
    EXPECT_TRUE(anim::clipStatRowShown(second, anim::ClipStatFilter::All, 0));

    const anim::ClipStatTotals totals = anim::clipStatTotals({first, second});
    EXPECT_EQ(totals.clips, 2u);
    EXPECT_EQ(totals.memoryBytes, first.report.runtimeBytes + second.report.runtimeBytes);
    EXPECT_EQ(totals.loadedBytes, first.report.runtimeBytes);
    EXPECT_EQ(totals.unusedClips, 1u);
    EXPECT_EQ(totals.unusedBytes, second.report.runtimeBytes);
    std::filesystem::remove_all(root);
}
