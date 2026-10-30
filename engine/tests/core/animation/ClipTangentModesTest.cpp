// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The key tangent modes of the curves view (docs/specs/S21-animation.md R27, AC13): the computed
// modes' reference values (the Clamped Auto table), the step of Constant keys kept with tangents,
// Free and Broken keeping what was stored, the modes surviving a write / parse round trip, and
// the tangents following a moved key.

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/KeyframeSampling.h"

#include <vector>

using namespace jasperengine;

namespace {

    // a 1D curve as a position track (x only): keys (time, value) with one mode
    ClipTrackData curve(const std::vector<std::pair<float, float>>& keys, ClipTangentMode mode){
        ClipTrackData track;
        track.channel = ClipChannel::Position;
        for (const auto& entry : keys){
            ClipKey key;
            key.time = entry.first;
            key.value = {entry.second, 0.0f, 0.0f};
            key.tangentMode = mode;
            track.keys.push_back(key);
        }
        return track;
    }

    ClipData clipOf(const ClipTrackData& track){
        ClipData data;
        data.tracks.push_back(track);
        return data;
    }

    ClipData normalizedClip(ClipData data, std::vector<ClipWarning>* warnings){
        anim::normalize(data, warnings);
        return data;
    }

    std::string describe(const ClipWarning& warning){
        return warning.path + ": " + warning.message;
    }

}

// the reference table: Clamped Auto is 0 at the ends and at extrema, the neighbours' slope
// elsewhere, clamped to 3x the smaller side; Auto the plain neighbours' slope
TEST(ClipTangentModes, ClampedAutoTable){
    struct Row{ ClipTangentMode mode; std::vector<std::pair<float, float>> keys; std::vector<float> in, out; };
    const std::vector<Row> table = {
        // rise then fall: 0, 1.5 (inside the clamp), 0 (a maximum), 0
        {ClipTangentMode::ClampedAuto, {{0, 0}, {1, 1}, {2, 3}, {3, 2}}, {0, 1.5f, 0, 0}, {0, 1.5f, 0, 0}},
        // a slow then steep rise: the neighbours' slope 5 is clamped to 3 x 0.1
        {ClipTangentMode::ClampedAuto, {{0, 0}, {1, 0.1f}, {2, 10}}, {0, 0.3f, 0}, {0, 0.3f, 0}},
        // uneven times: (6 - 0) / (3 - 0) = 2, within 3 x min(2 / 1, 4 / 2)
        {ClipTangentMode::ClampedAuto, {{0, 0}, {1, 2}, {3, 6}}, {0, 2.0f, 0}, {0, 2.0f, 0}},
        // Auto keeps the overshooting slopes
        {ClipTangentMode::Auto, {{0, 0}, {1, 1}, {2, 3}, {3, 2}}, {0, 1.5f, 0.5f, 0}, {0, 1.5f, 0.5f, 0}},
        {ClipTangentMode::Auto, {{0, 0}, {1, 0.1f}, {2, 10}}, {0, 5.0f, 0}, {0, 5.0f, 0}},
        // Linear: each side its segment's slope (the ends both sides the one segment's)
        {ClipTangentMode::Linear, {{0, 0}, {1, 1}, {2, 3}, {3, 2}}, {1, 1, 2, -1}, {1, 2, -1, -1}},
        // Flat and Constant: 0
        {ClipTangentMode::Flat, {{0, 0}, {1, 1}, {2, 3}}, {0, 0, 0}, {0, 0, 0}},
        {ClipTangentMode::Constant, {{0, 0}, {1, 1}, {2, 3}}, {0, 0, 0}, {0, 0, 0}},
    };
    for (size_t r = 0; r < table.size(); r++){
        ClipTrackData track = curve(table[r].keys, table[r].mode);
        anim::applyTangentModes(track);
        for (size_t k = 0; k < track.keys.size(); k++){
            ASSERT_TRUE(track.keys[k].hasTangents()) << "row " << r << " key " << k;
            EXPECT_NEAR(track.keys[k].inTangent[0], table[r].in[k], 1e-6f) << "row " << r << " key " << k;
            EXPECT_NEAR(track.keys[k].outTangent[0], table[r].out[k], 1e-6f) << "row " << r << " key " << k;
            // the other components are flat (constant 0 curves)
            EXPECT_EQ(track.keys[k].inTangent[1], 0.0f);
            EXPECT_EQ(track.keys[k].outTangent[2], 0.0f);
        }
    }
}

// Constant keys keep the step ease with their tangents, the segment holds the key's value
TEST(ClipTangentModes, ConstantStepsAndSurvivesNormalize){
    ClipTrackData track = curve({{0, 0}, {1, 4}, {2, 2}}, ClipTangentMode::ClampedAuto);
    track.keys[1].tangentMode = ClipTangentMode::Constant;
    std::vector<ClipWarning> warnings;
    const ClipData data = normalizedClip(clipOf(track), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const ClipTrackData& out = data.tracks[0];
    EXPECT_EQ(out.keys[1].ease, EaseType::STEP);
    EXPECT_EQ(out.keys[0].ease, EaseType::LINEAR);
    for (const ClipWarning& w : warnings) EXPECT_EQ(w.path.find("ease"), std::string::npos) << describe(w);
    // sampled: the segment after the Constant key holds 4 until the next key
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_TRUE(clip);
    anim::TrackSample sample;
    ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[0], 1.5f, sample));
    EXPECT_FLOAT_EQ(sample.vector.x, 4.0f);
    ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[0], 2.0f, sample));
    EXPECT_FLOAT_EQ(sample.vector.x, 2.0f);
}

// Free and Broken keep their stored tangents; without tangents they start from Clamped Auto's
TEST(ClipTangentModes, FreeAndBrokenKeepTheirTangents){
    ClipTrackData track = curve({{0, 0}, {1, 1}, {2, 3}}, ClipTangentMode::Broken);
    track.keys[1].inTangent = {7, 0, 0};
    track.keys[1].outTangent = {-2, 0, 0};
    track.keys[2].tangentMode = ClipTangentMode::Free;
    anim::applyTangentModes(track);
    EXPECT_FLOAT_EQ(track.keys[1].inTangent[0], 7.0f);
    EXPECT_FLOAT_EQ(track.keys[1].outTangent[0], -2.0f);
    ASSERT_TRUE(track.keys[2].hasTangents());
    EXPECT_FLOAT_EQ(track.keys[2].inTangent[0], 0.0f) << "an end key: Clamped Auto's 0";
}

// a key without a mode in a moded track gets the straight lines' tangents, without a warning
TEST(ClipTangentModes, KeysWithoutAModeGetLinearTangents){
    ClipTrackData track = curve({{0, 0}, {1, 2}, {2, 3}}, ClipTangentMode::None);
    track.keys[1].tangentMode = ClipTangentMode::Flat;
    std::vector<ClipWarning> warnings;
    const ClipData data = normalizedClip(clipOf(track), &warnings);
    const ClipTrackData& out = data.tracks[0];
    ASSERT_TRUE(out.keys[0].hasTangents());
    EXPECT_FLOAT_EQ(out.keys[0].outTangent[0], 2.0f);
    EXPECT_FLOAT_EQ(out.keys[2].inTangent[0], 1.0f);
    EXPECT_FLOAT_EQ(out.keys[1].inTangent[0], 0.0f);
    EXPECT_TRUE(warnings.empty()) << describe(warnings.front());
}

// the modes are written ("tangent_mode") and read back; the text is a fixed point
TEST(ClipTangentModes, RoundTrip){
    ClipTrackData track = curve({{0, 0}, {0.5f, 1}, {1, 3}, {1.5f, 2}}, ClipTangentMode::ClampedAuto);
    track.keys[1].tangentMode = ClipTangentMode::Auto;
    track.keys[2].tangentMode = ClipTangentMode::Broken;
    track.keys[2].inTangent = {1, 0, 0};
    track.keys[2].outTangent = {-1, 0, 0};
    track.keys[3].tangentMode = ClipTangentMode::Constant;
    const std::string text = anim::writeClip(clipOf(track));
    EXPECT_NE(text.find("\"tangent_mode\": \"broken\""), std::string::npos) << text;
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const ClipTrackData& back = parsed.data->tracks[0];
    EXPECT_EQ(back.keys[0].tangentMode, ClipTangentMode::ClampedAuto);
    EXPECT_EQ(back.keys[1].tangentMode, ClipTangentMode::Auto);
    EXPECT_EQ(back.keys[2].tangentMode, ClipTangentMode::Broken);
    EXPECT_EQ(back.keys[3].tangentMode, ClipTangentMode::Constant);
    EXPECT_FLOAT_EQ(back.keys[2].outTangent[0], -1.0f);
    EXPECT_EQ(anim::writeClip(*parsed.data), text);
    // an unknown mode is a warning, the key stays
    anim::ClipParseResult bad = anim::parseClip(text.substr(0, text.find("\"broken\"")) + "\"wobbly\"" + text.substr(text.find("\"broken\"") + 8));
    ASSERT_TRUE(bad.ok());
    EXPECT_EQ(bad.data->tracks[0].keys.size(), 4u);
    bool warned = false;
    for (const ClipWarning& w : bad.warnings) warned = warned || w.path.find("tangent_mode") != std::string::npos;
    EXPECT_TRUE(warned);
}

// moving a key moves its neighbours' computed tangents with it (every commit normalizes)
TEST(ClipTangentModes, ComputedTangentsFollowAMovedKey){
    ClipTrackData track = curve({{0, 0}, {1, 1}, {2, 3}, {3, 2}}, ClipTangentMode::Auto);
    ClipData data = normalizedClip(clipOf(track), nullptr);
    EXPECT_NEAR(data.tracks[0].keys[1].outTangent[0], 1.5f, 1e-6f);
    data.tracks[0].keys[2].value[0] = 5.0f;
    data = normalizedClip(data, nullptr);
    EXPECT_NEAR(data.tracks[0].keys[1].outTangent[0], 2.5f, 1e-6f);
    EXPECT_NEAR(data.tracks[0].keys[2].outTangent[0], 0.5f, 1e-6f);
}

// a rotation's neighbours are taken on the key's hemisphere: a flipped neighbour gives the same
// tangents as the unflipped one
TEST(ClipTangentModes, RotationNeighboursOnTheKeysHemisphere){
    ClipTrackData a;
    a.channel = ClipChannel::Rotation;
    const float s = 0.38268343f, c = 0.92387953f;   // 45 degrees about y
    const std::vector<std::vector<float>> values = {{0, 0, 0, 1}, {0, s, 0, c}, {0, 0.70710678f, 0, 0.70710678f}};
    for (size_t k = 0; k < values.size(); k++){
        ClipKey key;
        key.time = (float)k;
        key.value = values[k];
        key.tangentMode = ClipTangentMode::Auto;
        a.keys.push_back(key);
    }
    ClipTrackData b = a;
    for (float& v : b.keys[2].value) v = -v;
    anim::applyTangentModes(a);
    anim::applyTangentModes(b);
    for (size_t i = 0; i < 4; i++) EXPECT_NEAR(a.keys[1].outTangent[i], b.keys[1].outTangent[i], 1e-6f) << i;
}
