// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AnimationClipIO (spec 2.1, 9.R8/R20/R35-R38): the strict failures (JSON syntax with line:col,
// root, format, version, Unity YAML), the lenient rules (warnings with JSON paths, bad parts
// dropped), the canonical writer (key order, numbers, quaternions, lattice union, morph padding,
// unknown keys, guid placement) and the fixed point: parse + write of a canonical file is a no-op.

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/KeyframeSampling.h"
#include "Engine.h"

#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // every channel, tangents, eases, events, preview, source, a property track and an unknown key
    const char* const canonicalText =
        "{\n"
        "  \"format\": \"jasperengine.anim\",\n"
        "  \"version\": 1,\n"
        "  \"guid\": \"0123456789abcdef0123456789abcdef\",\n"
        "  \"frame_rate\": 30,\n"
        "  \"wrap\": \"loop\",\n"
        "  \"length\": 1.5,\n"
        "  \"tracks\": [\n"
        "    {\"path\": \"\", \"channel\": \"position\", \"keys\": [\n"
        "      [0, [0, 0, 0]],\n"
        "      [0.5, [0, 1, 0], \"quad_out\"],\n"
        "      [1, [0, 0, 0]]\n"
        "    ]},\n"
        "    {\"path\": \"Arm/Hand\", \"channel\": \"rotation\", \"relative\": true, \"keys\": [\n"
        "      [0, [0, 0, 0, 1]],\n"
        "      [1, [0, 0.707107, 0, 0.707107], \"step\"]\n"
        "    ]},\n"
        "    {\"path\": \"Arm/Hand\", \"channel\": \"scale\", \"loop\": true, \"keys\": [\n"
        "      {\"time\": 0, \"value\": [1, 1, 1], \"in\": [0, 0, 0], \"out\": [1, 0, 0]},\n"
        "      {\"time\": 0.25, \"value\": [2, 1, 1], \"in\": [1, 0, 0], \"out\": [0, 0, 0]}\n"
        "    ]},\n"
        "    {\"path\": \"Body\", \"channel\": \"morph\", \"keys\": [\n"
        "      [0, [0, 0.5]],\n"
        "      [1, [1, 0]]\n"
        "    ]},\n"
        "    {\"path\": \"Cage\", \"channel\": \"lattice\", \"keys\": [\n"
        "      [0, {\"points\": [[13, [0, 0, 0]], [20, [0.1, 0, 0]]]}],\n"
        "      [1, {\"points\": [[13, [0, 0.2, 0]], [20, [0, 0, 0]]]}]\n"
        "    ]},\n"
        "    {\"path\":\"Cage\",\"channel\":\"property\",\"component\":\"Light\",\"property\":\"intensity\",\"keys\":[[0,1]]},\n"
        "    {\"path\": \"Empty\", \"channel\": \"position\", \"keys\": []}\n"
        "  ],\n"
        "  \"events\": [\n"
        "    {\"time\": 0.5, \"function\": \"onFootstep\", \"args\": [\"left\", 1, true, -2.5]},\n"
        "    {\"time\": 0.75, \"function\": \"onLand\"}\n"
        "  ],\n"
        "  \"preview\": {\"model\": \"models/hero.gltf\"},\n"
        "  \"source\": {\"kind\": \"model\", \"path\": \"models/hero.gltf\", \"name\": \"Walk\"},\n"
        "  \"cycle_offset\": 0.25,\n"
        "  \"curves\": {\"a\":[1,2]}\n"
        "}\n";

    std::string wrapClip(const std::string& members){
        return "{\"format\": \"jasperengine.anim\", \"version\": 1" + (members.empty() ? std::string() : ", " + members) + "}";
    }

    ClipData parseOk(const std::string& text, std::vector<ClipWarning>* warnings = nullptr){
        anim::ClipParseResult result = anim::parseClip(text);
        EXPECT_TRUE(result.ok()) << result.error.toString();
        if (warnings) *warnings = result.warnings;
        return result.data ? *result.data : ClipData();
    }

    bool hasWarning(const std::vector<ClipWarning>& warnings, const std::string& path, const std::string& part){
        for (const ClipWarning& warning : warnings){
            if (warning.path == path && warning.message.find(part) != std::string::npos) return true;
        }
        return false;
    }

    std::string listWarnings(const std::vector<ClipWarning>& warnings){
        std::string text;
        for (const ClipWarning& warning : warnings) text += "[" + warning.path + "] " + warning.message + "\n";
        return text;
    }

    ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

}

// ---- strict failures

TEST(ClipIO, JsonSyntaxErrorsCarryLineAndColumn){
    std::string text = "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  oops\n}";
    anim::ClipParseResult result = anim::parseClip(text);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.line, 4);
    EXPECT_EQ(result.error.column, 3);
    EXPECT_EQ(result.error.toString().rfind("4:3: not valid JSON", 0), 0u) << result.error.toString();

    result = anim::parseClip("");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.line, 1);
}

TEST(ClipIO, RootFormatAndVersionAreRequired){
    EXPECT_FALSE(anim::parseClip("[1, 2]").ok());
    EXPECT_FALSE(anim::parseClip("{\"version\": 1}").ok());
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.poseanim\", \"version\": 1}").ok());
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\"}").ok());
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 0}").ok());
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1.5}").ok());
    // version 2 is the humanoid clip format (anim-retarget), 3 the clip with audio cues (S40); 4 is
    // newer than this engine
    EXPECT_TRUE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 2}").ok());
    EXPECT_TRUE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 3}").ok());
    anim::ClipParseResult newer = anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 4}");
    ASSERT_FALSE(newer.ok());
    EXPECT_NE(newer.error.message.find("version 4"), std::string::npos) << newer.error.message;

    anim::ClipParseResult minimal = anim::parseClip(wrapClip(""));
    ASSERT_TRUE(minimal.ok());
    EXPECT_TRUE(minimal.warnings.empty()) << listWarnings(minimal.warnings);
    EXPECT_EQ(minimal.data->frameRate, 30.0f);
    EXPECT_EQ(minimal.data->wrap, ClipWrapMode::Once);
    EXPECT_FALSE(minimal.data->hasExplicitLength());
}

TEST(ClipIO, UnityYamlIsRefusedWithTheGuideText){
    const char* unity = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n";
    anim::ClipParseResult result = anim::parseClip(unity);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.message, anim::unityYamlRefusal());
    EXPECT_EQ(result.error.line, 0);
    EXPECT_EQ(std::string(anim::unityYamlRefusal()),
              "This is a Unity .anim (YAML). JasperEngine clips are JSON {\"format\": \"jasperengine.anim\"}: "
              "create one with Assets > Create > Animation Clip or edit_animation {clip, create}.");
    EXPECT_FALSE(anim::parseClip("--- !u!74 &7400000\nAnimationClip:\n").ok());
}

// ---- canonical text

TEST(ClipIO, CanonicalFileRoundTripsByteForByte){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(canonicalText, &warnings);
    EXPECT_EQ(anim::writeClip(data), canonicalText);
    // the property track this engine cannot animate (kept verbatim) and the unknown keys are the
    // only warnings
    EXPECT_TRUE(hasWarning(warnings, "tracks[5]", "unknown animatable property 'Light.intensity'")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "", "cycle_offset, curves")) << listWarnings(warnings);
    EXPECT_EQ(warnings.size(), 2u) << listWarnings(warnings);

    ASSERT_EQ(data.tracks.size(), 7u);
    EXPECT_EQ(data.guid, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(data.wrap, ClipWrapMode::Loop);
    EXPECT_FLOAT_EQ(data.length, 1.5f);
    EXPECT_EQ(data.tracks[1].channel, ClipChannel::Rotation);
    EXPECT_TRUE(data.tracks[1].relative);
    EXPECT_EQ(data.tracks[1].keys[1].ease, EaseType::STEP);
    EXPECT_TRUE(data.tracks[2].loop);
    EXPECT_TRUE(data.tracks[2].keys[0].hasTangents());
    EXPECT_EQ(data.tracks[4].points, (std::vector<int>{13, 20}));
    EXPECT_EQ(data.tracks[5].component, "Light");
    EXPECT_EQ(data.tracks[5].property, "intensity");
    ASSERT_EQ(data.events.size(), 2u);
    ASSERT_EQ(data.events[0].args.size(), 4u);
    EXPECT_EQ(std::get<std::string>(data.events[0].args[0]), "left");
    EXPECT_EQ(std::get<double>(data.events[0].args[1]), 1.0);
    EXPECT_EQ(std::get<bool>(data.events[0].args[2]), true);
    EXPECT_EQ(data.previewModel, "models/hero.gltf");
    EXPECT_EQ(data.source.kind, "model");
    EXPECT_EQ(data.source.name, "Walk");
    ASSERT_EQ(data.unknownKeys.size(), 2u);
    EXPECT_EQ(data.unknownKeys[0].first, "cycle_offset");
    EXPECT_EQ(data.unknownKeys[1].second, "{\"a\":[1,2]}");

    // writing twice gives the same bytes, and the runtime form converts back to the same text
    EXPECT_EQ(anim::writeClip(data), anim::writeClip(data));
    EXPECT_EQ(anim::writeClip(AnimationClip::build(data)->toData()), canonicalText);
}

TEST(ClipIO, WriterOrdersKeysTracksAndEvents){
    ClipData data;
    data.wrap = ClipWrapMode::PingPong;
    ClipTrackData scale;
    scale.path = "B";
    scale.channel = ClipChannel::Scale;
    scale.keys = {key(1, {1, 1, 1}), key(0, {2, 2, 2})};
    ClipTrackData position;
    position.path = "B";
    position.channel = ClipChannel::Position;
    position.keys = {key(0, {1, 2, 3})};
    ClipTrackData root;
    root.channel = ClipChannel::Rotation;
    root.keys = {key(0, {0, 0, 0, 1})};
    data.tracks = {scale, position, root};
    ClipEventData late;
    late.time = 1;
    late.function = "b";
    ClipEventData earlyB;
    earlyB.time = 0.5f;
    earlyB.function = "b";
    ClipEventData earlyA;
    earlyA.time = 0.5f;
    earlyA.function = "a";
    data.events = {late, earlyB, earlyA};
    data.guid = "ffffffffffffffffffffffffffffffff";
    data.unknownKeys = {{"zeta", "1"}, {"alpha", "\"x\""}};

    const std::string expected =
        "{\n"
        "  \"format\": \"jasperengine.anim\",\n"
        "  \"version\": 1,\n"
        "  \"guid\": \"ffffffffffffffffffffffffffffffff\",\n"
        "  \"frame_rate\": 30,\n"
        "  \"wrap\": \"ping_pong\",\n"
        "  \"tracks\": [\n"
        "    {\"path\": \"\", \"channel\": \"rotation\", \"keys\": [\n"
        "      [0, [0, 0, 0, 1]]\n"
        "    ]},\n"
        "    {\"path\": \"B\", \"channel\": \"position\", \"keys\": [\n"
        "      [0, [1, 2, 3]]\n"
        "    ]},\n"
        "    {\"path\": \"B\", \"channel\": \"scale\", \"keys\": [\n"
        "      [0, [2, 2, 2]],\n"
        "      [1, [1, 1, 1]]\n"
        "    ]}\n"
        "  ],\n"
        "  \"events\": [\n"
        "    {\"time\": 0.5, \"function\": \"a\"},\n"
        "    {\"time\": 0.5, \"function\": \"b\"},\n"
        "    {\"time\": 1, \"function\": \"b\"}\n"
        "  ],\n"
        "  \"zeta\": 1,\n"
        "  \"alpha\": \"x\"\n"
        "}\n";
    EXPECT_EQ(anim::writeClip(data), expected);
    EXPECT_EQ(anim::writeClip(parseOk(expected)), expected);
}

TEST(ClipIO, GuidIsWrittenRightAfterVersion){
    std::string text = wrapClip("\"frame_rate\": 24, \"guid\": \"00112233445566778899aabbccddeeff\"");
    std::string written = anim::writeClip(parseOk(text));
    EXPECT_EQ(written.find("  \"version\": 1,\n  \"guid\": \"00112233445566778899aabbccddeeff\",\n  \"frame_rate\": 24,"),
              written.find("  \"version\": 1,")) << written;
    EXPECT_EQ(anim::readClipGuid(written), "00112233445566778899aabbccddeeff");

    // a bad guid is dropped with a warning; the T0 helper inserts one where the writer puts it
    std::vector<ClipWarning> warnings;
    ClipData noGuid = parseOk(wrapClip("\"guid\": \"XYZ\""), &warnings);
    EXPECT_TRUE(noGuid.guid.empty());
    EXPECT_TRUE(hasWarning(warnings, "guid", "32 lowercase hex")) << listWarnings(warnings);
    std::string plain = anim::writeClip(noGuid);
    ASSERT_TRUE(anim::replaceClipGuid(plain, "00112233445566778899aabbccddeeff"));
    noGuid.guid = "00112233445566778899aabbccddeeff";
    EXPECT_EQ(plain, anim::writeClip(noGuid));
}

TEST(ClipIO, NumbersUseSixSignificantDigitsAndTimesSixDecimals){
    ClipData data;
    ClipTrackData track;
    track.channel = ClipChannel::Position;
    track.keys = {key(0.1234567f, {1.23456789f, -0.000012345678f, 123456789.0f}),
                  key(2.0000004f, {-0.0f, 1e-7f, 0.5f})};
    data.tracks = {track};
    data.length = 3.25f;
    std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("[0.123457, [1.23457, -1.23457e-05, 1.23457e+08]]"), std::string::npos) << text;
    EXPECT_NE(text.find("[2, [0, 1e-07, 0.5]]"), std::string::npos) << text;
    EXPECT_NE(text.find("\"length\": 3.25,"), std::string::npos) << text;
    EXPECT_EQ(anim::writeClip(parseOk(text)), text);
}

// ---- quaternions (R35, R37)

TEST(ClipIO, QuaternionsNormalizeOnlyWhenOffByMoreThan1e5){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                                     "[0, [0, 0, 0, 0.999995]], [1, [0, 0, 0, 2]], [2, [0, 0, 0, 0]]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    ASSERT_EQ(data.tracks[0].keys.size(), 2u) << "the zero quaternion is dropped";
    EXPECT_FLOAT_EQ(data.tracks[0].keys[0].value[3], 0.999995f) << "within 1e-5: kept as written";
    EXPECT_FLOAT_EQ(data.tracks[0].keys[1].value[3], 1.0f);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value", "normalized")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].value", "zero quaternion")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[0].value", "normalized"));
    std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("[0, [0, 0, 0, 0.999995]]"), std::string::npos) << text;

    // the threshold itself: 8e-6 off stays, 2e-5 off is normalized, and that key's tangents
    // scale with it
    warnings.clear();
    data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                            "{\"time\": 0, \"value\": [0, 0, 0, 1.000008], \"in\": [0, 0, 0, 0], \"out\": [0, 0, 0, 0]},"
                            "{\"time\": 1, \"value\": [0, 0, 0, 1.00002], \"in\": [0, 0, 0, 1.00002], \"out\": [0, 2.00004, 0, 0]}]}]"),
                   &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    EXPECT_FLOAT_EQ(data.tracks[0].keys[0].value[3], 1.000008f);
    EXPECT_FLOAT_EQ(data.tracks[0].keys[1].value[3], 1.0f);
    EXPECT_FLOAT_EQ(data.tracks[0].keys[1].inTangent[3], 1.0f);
    EXPECT_FLOAT_EQ(data.tracks[0].keys[1].outTangent[1], 2.0f);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[0].value", "normalized")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value", "normalized")) << listWarnings(warnings);
}

// The writer decides normalization on the value it writes: quaternions whose norm sits in the
// 1e-5 band (where rounding to %.6g can cross the threshold) still give a fixed point
TEST(ClipIO, QuaternionsNearTheToleranceAreAFixedPoint){
    std::mt19937 random(4242);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> band(-1.2e-5f, 1.2e-5f);
    for (int round = 0; round < 3000; round++){
        float q[4];
        float length = 0;
        do{
            length = 0;
            for (float& v : q){ v = unit(random); length += v * v; }
            length = std::sqrt(length);
        }while (length < 0.1f);
        float scale = (1.0f + band(random)) / length;
        ClipData data;
        ClipTrackData track;
        track.channel = ClipChannel::Rotation;
        // built by key() (a braced list assigned to a fresh key's value trips GCC's -Wnonnull)
        ClipKey a = key(0, {0, 0, 0, 1});
        ClipKey b = key(1, {q[0] * scale, q[1] * scale, q[2] * scale, q[3] * scale});
        if (round % 2){
            a.inTangent = {0, 0, 0, 0};
            a.outTangent = {unit(random), 0, 0, 0};
            b.inTangent = {0, unit(random), 0, 0};
            b.outTangent = {0, 0, unit(random), 0};
        }
        track.keys = {a, b};
        data.tracks = {track};
        std::string first = anim::writeClip(data);
        anim::ClipParseResult parsed = anim::parseClip(first);
        ASSERT_TRUE(parsed.ok());
        EXPECT_TRUE(parsed.warnings.empty()) << listWarnings(parsed.warnings) << first;
        ASSERT_EQ(anim::writeClip(*parsed.data), first) << "round " << round;
    }
}

TEST(ClipIO, HemisphereContinuityFlipsSlerpKeys){
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                                     "[0, [0, 0, 0, 1]], [1, [0, -0.6, 0, -0.8]], [2, [0, 0, 0, -1]]]}]"));
    ASSERT_EQ(data.tracks.size(), 1u);
    const ClipKey& flipped = data.tracks[0].keys[1];
    EXPECT_FLOAT_EQ(flipped.value[1], 0.6f);
    EXPECT_FLOAT_EQ(flipped.value[3], 0.8f);
    EXPECT_FLOAT_EQ(data.tracks[0].keys[2].value[3], 1.0f) << "on the side of the flipped key before it";
    std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("[1, [0, 0.6, 0, 0.8]]"), std::string::npos) << text;
}

// A CUBICSPLINE (Hermite) rotation track keeps its signs: a flipped key would change the segment
// into it whatever its tangents became, so the clip samples like the glTF / the model's entity
// tracks (the same sampler over the values as given)
TEST(ClipIO, HermiteRotationTracksKeepTheirSignsAndSampleAsGiven){
    const std::string text = wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                                      "{\"time\": 0, \"value\": [0, 0, 0, 1], \"in\": [0, 0, 0, 0], \"out\": [0, 1, 0, 0]},"
                                      "{\"time\": 1, \"value\": [0, -0.6, 0, -0.8], \"in\": [0, 2, 0, 0], \"out\": [0, 0, 0.5, 0]},"
                                      "{\"time\": 2, \"value\": [0, 0.6, 0, 0.8], \"in\": [0.5, 0, 0, 0], \"out\": [0, 0, 0, 0]}]}]");
    ClipData data = parseOk(text);
    ASSERT_EQ(data.tracks.size(), 1u);
    ASSERT_EQ(data.tracks[0].keys.size(), 3u);
    const ClipKey& kept = data.tracks[0].keys[1];
    EXPECT_EQ(kept.value, (std::vector<float>{0, -0.6f, 0, -0.8f}));
    EXPECT_EQ(kept.inTangent, (std::vector<float>{0, 2, 0, 0}));
    EXPECT_EQ(kept.outTangent, (std::vector<float>{0, 0, 0.5f, 0}));
    EXPECT_EQ(data.tracks[0].keys[2].value, (std::vector<float>{0, 0.6f, 0, 0.8f}));
    const std::string written = anim::writeClip(data);
    EXPECT_NE(written.find("{\"time\": 1, \"value\": [0, -0.6, 0, -0.8], \"in\": [0, 2, 0, 0], \"out\": [0, 0, 0.5, 0]}"),
              std::string::npos) << written;
    EXPECT_EQ(anim::writeClip(parseOk(written)), written) << "a fixed point";

    // the clip's sample against the entity tracks' sampler over the values and tangents as given
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_EQ(clip->getTracks().size(), 1u);
    const float times[] = {0, 1, 2};
    const float raw[3][3][4] = {   // in, value, out per key (x y z w)
        {{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 0, 0}},
        {{0, 2, 0, 0}, {0, -0.6f, 0, -0.8f}, {0, 0, 0.5f, 0}},
        {{0.5f, 0, 0, 0}, {0, 0.6f, 0, 0.8f}, {0, 0, 0, 0}}};
    Quaternion ins[3], values[3], outs[3];
    for (int k = 0; k < 3; k++){
        ins[k] = quaternionFromXYZW(raw[k][0]);
        values[k] = quaternionFromXYZW(raw[k][1]);
        outs[k] = quaternionFromXYZW(raw[k][2]);
    }
    for (float t = 0.05f; t < 2.0f; t += 0.1f){
        anim::TrackSample sample;
        ASSERT_TRUE(anim::sampleTrack(clip->getTracks()[0], t, sample));
        const Quaternion expected = anim::sampleQuaternion(anim::locateKey(times, 3, nullptr, 0, t), values, ins, outs);
        EXPECT_NEAR(sample.rotation.w, expected.w, 1e-6f) << "t " << t;
        EXPECT_NEAR(sample.rotation.x, expected.x, 1e-6f) << "t " << t;
        EXPECT_NEAR(sample.rotation.y, expected.y, 1e-6f) << "t " << t;
        EXPECT_NEAR(sample.rotation.z, expected.z, 1e-6f) << "t " << t;
    }
}

TEST(ClipIO, EulerIsReadInEditAnimationsOrderAndWrittenAsQuaternions){
    // (30, 90, 0) degrees, ZYX (qz * qy * qx) by hand: qy * qx = (cos45 cos15, cos45 sin15,
    // sin45 cos15, -sin45 sin15) = (w 0.683013, x 0.183013, y 0.683013, z -0.183013)
    const float expected[4] = {0.183013f, 0.683013f, -0.183013f, 0.683013f};   // x y z w
    const std::string text = wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                                      "[0, {\"euler\": [0, 0, 0]}], [1, {\"euler\": [30, 90, 0]}], [2, {\"euler\": [30, 300, 0]}]]}]");
    // degrees whatever the engine's angle unit (a game may switch it to radians)
    bool degrees = Engine::isUseDegrees();
    for (bool useDegrees : {true, false}){
        Engine::setUseDegrees(useDegrees);
        std::vector<ClipWarning> warnings;
        ClipData data = parseOk(text, &warnings);
        ASSERT_EQ(data.tracks.size(), 1u);
        ASSERT_EQ(data.tracks[0].keys.size(), 3u);
        const std::vector<float>& value = data.tracks[0].keys[1].value;
        for (int i = 0; i < 4; i++) EXPECT_NEAR(value[i], expected[i], 2e-6f) << "component " << i << " useDegrees " << useDegrees;
        Quaternion helper = anim::eulerDegreesToQuaternion(30, 90, 0);
        EXPECT_NEAR(helper.w, expected[3], 2e-6f);
        EXPECT_NEAR(helper.z, expected[2], 2e-6f);
        EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].value", "more than 180 degrees on y")) << listWarnings(warnings);
        EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[1].value", "180 degrees")) << listWarnings(warnings);
    }
    Engine::setUseDegrees(degrees);
    // in degrees mode the helper is edit_animation's Quaternion(x, y, z) (McpValues::parseQuaternion)
    Quaternion mcp(30.0f, 90.0f, 0.0f);
    Quaternion helper = anim::eulerDegreesToQuaternion(30, 90, 0);
    EXPECT_NEAR(helper.w, mcp.w, 1e-6f);
    EXPECT_NEAR(helper.x, mcp.x, 1e-6f);
    EXPECT_NEAR(helper.y, mcp.y, 1e-6f);
    EXPECT_NEAR(helper.z, mcp.z, 1e-6f);

    std::string written = anim::writeClip(parseOk(text));
    EXPECT_EQ(written.find("euler"), std::string::npos) << written;
    EXPECT_EQ(anim::writeClip(parseOk(written)), written);
}

TEST(ClipIO, EulerTurnsAreCheckedInTimeOrder){
    // written out of time order: sorted, the 170 -> -170 segment turns 340 degrees
    std::vector<ClipWarning> warnings;
    parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                     "[1, {\"euler\": [0, 170, 0]}], [0, {\"euler\": [0, 0, 0]}], [2, {\"euler\": [0, -170, 0]}]]}]"), &warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].value", "more than 180 degrees on y")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[0].value", "180 degrees")) << listWarnings(warnings);
    // a quaternion key between two Euler keys breaks the pair
    warnings.clear();
    parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"rotation\", \"keys\": ["
                     "[0, {\"euler\": [0, 0, 0]}], [1, [0, 0, 0, 1]], [2, {\"euler\": [0, 270, 0]}]]}]"), &warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[2].value", "180 degrees")) << listWarnings(warnings);
}

// ---- leniency (warnings with JSON paths, bad parts dropped)

TEST(ClipIO, BadKeysTracksAndEventsAreDroppedWithPaths){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip(
        "\"tracks\": ["
        "  {\"path\": \"A\", \"channel\": \"position\", \"keys\": [[0, [0, 0, 0]], [0.5, [1, 2]], [1, [1, 1, 1], \"wobble\"]]},"
        "  {\"path\": \"B\", \"channel\": \"colour\", \"keys\": []},"
        "  {\"path\": 3, \"channel\": \"scale\"},"
        "  {\"channel\": \"scale\", \"keys\": [[\"x\", [1, 1, 1]]], \"speed\": 2},"
        "  \"nope\""
        "],"
        "\"events\": [{\"time\": 0.1, \"function\": \"not a name\"}, {\"time\": 0.2, \"function\": \"end\"},"
        "             {\"time\": 0.3, \"function\": \"ok\", \"args\": [1, {\"x\": 1}, 2, 3, 4, 5, 6, 7, 8, 9]},"
        "             {\"function\": \"noTime\"}],"
        "\"frame_rate\": 500, \"wrap\": \"bounce\", \"length\": -2, \"preview\": {\"model\": 5}"), &warnings);
    std::string all = listWarnings(warnings);

    ASSERT_EQ(data.tracks.size(), 2u) << all;
    EXPECT_EQ(data.tracks[0].path, "");
    EXPECT_TRUE(data.tracks[0].keys.empty());
    EXPECT_EQ(data.tracks[1].path, "A");
    ASSERT_EQ(data.tracks[1].keys.size(), 2u);
    EXPECT_EQ(data.tracks[1].keys[1].ease, EaseType::LINEAR);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value", "expected [x, y, z]")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].ease", "using linear")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[1].channel", "track dropped")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[2].path", "track dropped")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[3].path", "the root")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[3].keys[0].time", "key dropped")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[3].speed", "unknown key")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[4]", "track dropped")) << all;

    ASSERT_EQ(data.events.size(), 1u) << all;
    EXPECT_EQ(data.events[0].function, "ok");
    EXPECT_EQ(data.events[0].args.size(), MAX_ANIMATION_EVENT_ARGS);
    EXPECT_EQ(std::get<double>(data.events[0].args[1]), 2.0);
    EXPECT_TRUE(hasWarning(warnings, "events[0].function", "not a Lua function name")) << all;
    EXPECT_TRUE(hasWarning(warnings, "events[1].function", "not a Lua function name")) << all;
    EXPECT_TRUE(hasWarning(warnings, "events[2].args[1]", "dropped")) << all;
    EXPECT_TRUE(hasWarning(warnings, "events[2].args", "more than 8")) << all;
    EXPECT_TRUE(hasWarning(warnings, "events[3].time", "event dropped")) << all;

    EXPECT_EQ(data.frameRate, 240.0f);
    EXPECT_TRUE(hasWarning(warnings, "frame_rate", "outside 1..240")) << all;
    EXPECT_EQ(data.wrap, ClipWrapMode::Once);
    EXPECT_TRUE(hasWarning(warnings, "wrap", "using once")) << all;
    EXPECT_FALSE(data.hasExplicitLength());
    EXPECT_TRUE(hasWarning(warnings, "length", "automatic")) << all;
    EXPECT_TRUE(hasWarning(warnings, "preview.model", "ignored")) << all;
}

TEST(ClipIO, KeysAreSortedAndCloseKeysCollapse){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"position\", \"keys\": ["
                                     "[1, [1, 0, 0]], [0, [0, 0, 0]], [1.0000002, [2, 0, 0]], [-1, [5, 0, 0]]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const std::vector<ClipKey>& keys = data.tracks[0].keys;
    ASSERT_EQ(keys.size(), 2u) << listWarnings(warnings);
    EXPECT_EQ(keys[0].time, 0.0f);
    EXPECT_EQ(keys[0].value[0], 5.0f) << "the negative key moved to 0, and as the later key at 0 it won";
    EXPECT_EQ(keys[1].time, 1.0f);
    EXPECT_EQ(keys[1].value[0], 2.0f) << "the later of the two keys within 1e-6 s wins";
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys", "not sorted")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[3].time", "negative")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[0]", "merged")) << listWarnings(warnings);
}

TEST(ClipIO, KeysTwoMicrosecondsApartStaySeparate){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"position\", \"keys\": ["
                                     "[1, [1, 0, 0]], [1.000002, [2, 0, 0]], [1.0000004, [3, 0, 0]]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const std::vector<ClipKey>& keys = data.tracks[0].keys;
    ASSERT_EQ(keys.size(), 2u) << listWarnings(warnings);
    EXPECT_EQ(keys[0].value[0], 3.0f) << "0.4 us after 1 s collapses into it (the later key wins)";
    EXPECT_FLOAT_EQ(keys[1].time, 1.000002f);
    EXPECT_EQ(keys[1].value[0], 2.0f) << "2 us apart stays a key of its own";
}

TEST(ClipIO, OverflowingAndNonFiniteNumbers){
    // numbers outside the double range are a JSON error with the number's position (they used
    // to escape as nlohmann::out_of_range and terminate the process)
    std::string text = "{\"format\": \"jasperengine.anim\", \"version\": 1,\n"
                       "  \"tracks\": [{\"path\": \"\", \"channel\": \"position\", \"keys\": [[0, [1e999, 0, 0]]]}]}";
    anim::ClipParseResult result = anim::parseClip(text);
    ASSERT_FALSE(result.ok());
    size_t at = text.find("1e999");
    size_t lineStart = text.rfind('\n', at) + 1;
    EXPECT_EQ(result.error.line, 2);
    EXPECT_EQ(result.error.column, (int)(at - lineStart) + 1) << result.error.toString();
    EXPECT_NE(result.error.message.find("1e999"), std::string::npos) << result.error.message;

    std::string version = "{\"format\": \"jasperengine.anim\", \"version\": 1e400}";
    result = anim::parseClip(version);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.line, 1);
    EXPECT_EQ(result.error.column, (int)version.find("1e400") + 1) << result.error.toString();
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1, \"frame_rate\": -1e999}").ok());
    // NaN and Infinity are not JSON
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1, \"length\": NaN}").ok());
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1, \"length\": Infinity}").ok());
    // a huge version is just newer
    EXPECT_FALSE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1e300}").ok());

    // doubles that don't fit a float are lenient: the key / setting goes with a warning
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"frame_rate\": 1e300, \"length\": 1e300, \"tracks\": [{\"path\": \"\", \"channel\": \"position\", "
                                     "\"keys\": [[0, [0, 0, 0]], [1, [1e300, 0, 0]], [1e300, [1, 1, 1]]]}],"
                                     "\"events\": [{\"time\": 1, \"function\": \"f\", \"args\": [1e300]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    EXPECT_EQ(data.tracks[0].keys.size(), 1u) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value", "expected [x, y, z]")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].time", "key dropped")) << listWarnings(warnings);
    EXPECT_EQ(data.frameRate, 30.0f);
    EXPECT_TRUE(hasWarning(warnings, "frame_rate", "using 30")) << listWarnings(warnings);
    EXPECT_FALSE(data.hasExplicitLength());
    ASSERT_EQ(data.events.size(), 1u);
    EXPECT_EQ(std::get<double>(data.events[0].args[0]), 1e300) << "a double arg keeps its value";
    std::string written = anim::writeClip(data);
    EXPECT_EQ(anim::writeClip(parseOk(written)), written);
}

TEST(ClipIO, SyntaxColumnsCountCodePointsAfterTheBom){
    // the BOM is not a column; "é" (2 bytes) is one
    anim::ClipParseResult bom = anim::parseClip("\xEF\xBB\xBF{\"format\": oops}");
    ASSERT_FALSE(bom.ok());
    EXPECT_EQ(bom.error.line, 1);
    EXPECT_EQ(bom.error.column, 12) << bom.error.toString();
    anim::ClipParseResult wide = anim::parseClip("{\"né\": 1, oops}");
    ASSERT_FALSE(wide.ok());
    EXPECT_EQ(wide.error.column, 11) << wide.error.toString();   // {"né": 1, o
    // a BOM before a valid clip is fine
    EXPECT_TRUE(anim::parseClip("\xEF\xBB\xBF" + wrapClip("")).ok());
}

TEST(ClipIO, MixedTangentTracksReportEachDroppedEase){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"position\", \"keys\": ["
                                     "{\"time\": 0, \"value\": [0, 0, 0], \"in\": [0, 0, 0], \"out\": [1, 0, 0]},"
                                     "[1, [1, 0, 0], \"step\"], [2, [2, 0, 0]], [3, [3, 0, 0], \"quad_in\"]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    for (const ClipKey& key : data.tracks[0].keys){
        EXPECT_TRUE(key.hasTangents());
        // a step stays (a Constant segment holds the key's value: anim-properties' tangent edits)
        EXPECT_EQ(key.ease, key.time == 1.0f ? EaseType::STEP : EaseType::LINEAR);
    }
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[1].ease", "the track uses tangents")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[3].ease", "the track uses tangents")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[2].ease", "")) << "a linear key loses nothing";
}

TEST(ClipIO, UnsortedSourceTracksAreReportedThroughAMerge){
    std::vector<ClipWarning> warnings;
    parseOk(wrapClip("\"tracks\": [{\"path\": \"A\", \"channel\": \"position\", \"keys\": [[0, [0, 0, 0]], [2, [2, 0, 0]]]},"
                     "{\"path\": \"A\", \"channel\": \"position\", \"keys\": [[3, [3, 0, 0]], [1, [1, 0, 0]]]}]"), &warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[1].keys", "not sorted")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys", "not sorted")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[1]", "merged")) << listWarnings(warnings);
}

TEST(ClipIO, DuplicateTracksMergeWithTheLaterKeysWinning){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": ["
                                     "{\"path\": \"A\", \"channel\": \"position\", \"keys\": [[0, [0, 0, 0]], [1, [1, 0, 0]]]},"
                                     "{\"path\": \"A\", \"channel\": \"position\", \"keys\": [[1, [9, 0, 0]], [2, [2, 0, 0]]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const std::vector<ClipKey>& keys = data.tracks[0].keys;
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[1].value[0], 9.0f);
    EXPECT_EQ(keys[2].time, 2.0f);
    EXPECT_TRUE(hasWarning(warnings, "tracks[1]", "merged")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys", "not sorted")) << listWarnings(warnings);
}

TEST(ClipIO, TangentsWinOverTheEase){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"\", \"channel\": \"position\", \"keys\": ["
                                     "{\"time\": 0, \"value\": [0, 0, 0], \"ease\": \"quad_in\", \"in\": [0, 0, 0], \"out\": [1, 0, 0]},"
                                     "{\"time\": 1, \"value\": [1, 0, 0], \"in\": [1, 0, 0]}]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const std::vector<ClipKey>& keys = data.tracks[0].keys;
    EXPECT_EQ(keys[0].ease, EaseType::LINEAR);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[0].ease", "tangents win over the ease")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1]", "in and out tangents go together")) << listWarnings(warnings);
    // the key without tangents gets the straight line's, so the track stays one cubic track
    ASSERT_TRUE(keys[1].hasTangents());
    EXPECT_FLOAT_EQ(keys[1].inTangent[0], 1.0f);
    EXPECT_FLOAT_EQ(keys[1].outTangent[0], 1.0f);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys", "linear tangents")) << listWarnings(warnings);
}

TEST(ClipIO, LatticePointsAreTheUnionOfTheKeys){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"Cage\", \"channel\": \"lattice\", \"keys\": ["
                                     "[0, {\"points\": [[20, [1, 0, 0]], [13, [0, 1, 0]]]}],"
                                     "[1, {\"points\": [[13, [0, 2, 0]], [7, [0, 0, 3]], [-1, [1, 1, 1]], [13, [0, 4, 0]]]}]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const ClipTrackData& track = data.tracks[0];
    EXPECT_EQ(track.points, (std::vector<int>{7, 13, 20}));
    ASSERT_EQ(track.keys.size(), 2u);
    EXPECT_EQ(track.keys[0].value, (std::vector<float>{0, 0, 0, 0, 1, 0, 1, 0, 0}));
    EXPECT_EQ(track.keys[1].value, (std::vector<float>{0, 0, 3, 0, 4, 0, 0, 0, 0}));
    std::string all = listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[0]", "point 7 missing, using 0")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1]", "point 20 missing, using 0")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value.points[2]", "point dropped")) << all;
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[1].value.points[3]", "listed twice")) << all;

    // the writer lists every point in every key
    std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("[0, {\"points\": [[7, [0, 0, 0]], [13, [0, 1, 0]], [20, [1, 0, 0]]]}]"), std::string::npos) << text;
    EXPECT_NE(text.find("[1, {\"points\": [[7, [0, 0, 3]], [13, [0, 4, 0]], [20, [0, 0, 0]]]}]"), std::string::npos) << text;
    std::vector<ClipWarning> again;
    EXPECT_EQ(anim::writeClip(parseOk(text, &again)), text);
    EXPECT_TRUE(again.empty()) << listWarnings(again);
}

TEST(ClipIO, MorphWeightsArePaddedAndClamped){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"Body\", \"channel\": \"morph\", \"keys\": ["
                                     "[0, [0.5]], [1, [1, 0.25]], [2, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]]]}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    const std::vector<ClipKey>& keys = data.tracks[0].keys;
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0].value, (std::vector<float>{0.5f, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(keys[1].value.size(), (size_t)MAX_MORPHTARGETS);
    EXPECT_EQ(keys[2].value, (std::vector<float>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys[2].value", "more than 8")) << listWarnings(warnings);
    EXPECT_FALSE(hasWarning(warnings, "tracks[0].keys[0].value", "")) << "shorter arrays mean 0 for the rest";
}

TEST(ClipIO, NormalizeRestoresTheInvariantsOfHandBuiltData){
    ClipData data;
    data.frameRate = 0.0f;
    data.wrap = ClipWrapMode::Default;
    ClipTrackData morph;
    morph.path = "M";
    morph.channel = ClipChannel::Morph;
    morph.relative = true;
    morph.keys = {key(0.5f, {1}), key(0, {0, 1, 1})};
    ClipTrackData lattice;
    lattice.path = "L";
    lattice.channel = ClipChannel::Lattice;
    lattice.points = {9, 3};
    lattice.keys = {key(0, {9, 9, 9, 3, 3, 3})};
    data.tracks = {morph, lattice};
    std::vector<ClipWarning> warnings;
    anim::normalize(data, &warnings);
    EXPECT_EQ(data.frameRate, 1.0f);
    EXPECT_EQ(data.wrap, ClipWrapMode::Once);
    ASSERT_EQ(data.tracks.size(), 2u);
    EXPECT_EQ(data.tracks[0].path, "L");
    EXPECT_EQ(data.tracks[0].points, (std::vector<int>{3, 9}));
    EXPECT_EQ(data.tracks[0].keys[0].value, (std::vector<float>{3, 3, 3, 9, 9, 9}));
    EXPECT_FALSE(data.tracks[1].relative);
    EXPECT_EQ(data.tracks[1].keys[0].time, 0.0f);
    EXPECT_EQ(data.tracks[1].keys[1].value, (std::vector<float>{1, 0, 0}));
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].relative", "only position, rotation and scale")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0].keys", "not sorted")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "wrap", "using \"once\"")) << listWarnings(warnings);
}

TEST(ClipIO, UnknownKeysArePreservedInFileOrder){
    std::string text = wrapClip("\"zz\": [1, 2], \"tracks\": [], \"aa\": {\"nested\": true}, \"zz\": [3]");
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(text, &warnings);
    ASSERT_EQ(data.unknownKeys.size(), 2u);
    EXPECT_EQ(data.unknownKeys[0].first, "zz");
    EXPECT_EQ(data.unknownKeys[0].second, "[3]") << "a repeated key keeps its last value";
    EXPECT_EQ(data.unknownKeys[1].first, "aa");
    std::string written = anim::writeClip(data);
    EXPECT_NE(written.find("  \"events\": [],\n  \"zz\": [3],\n  \"aa\": {\"nested\":true}\n}\n"), std::string::npos) << written;
    EXPECT_EQ(warnings.size(), 1u) << "unknown keys are warned once: " << listWarnings(warnings);
}

TEST(ClipIO, LengthRules){
    ClipData data;
    ClipTrackData track;
    track.channel = ClipChannel::Position;
    track.keys = {key(0, {0, 0, 0}), key(0.75f, {1, 0, 0})};
    data.tracks = {track};
    ClipEventData event;
    event.time = 1.25f;
    event.function = "late";
    data.events = {event};
    EXPECT_FLOAT_EQ(data.effectiveLength(), 1.25f) << "the last key or event";
    std::string text = anim::writeClip(data);
    EXPECT_EQ(text.find("\"length\""), std::string::npos) << "automatic length is not written";
    data.length = 0.5f;
    EXPECT_FLOAT_EQ(AnimationClip::build(data)->getLength(), 0.5f);
    EXPECT_TRUE(AnimationClip::build(data)->hasExplicitLength());
    EXPECT_NE(anim::writeClip(data).find("  \"length\": 0.5,\n"), std::string::npos);
    EXPECT_TRUE(parseOk(wrapClip("\"length\": null")).length < 0.0f);
}

// Random clips of every channel (tangents, near-opposite quaternions, odd times): the canonical
// text is a fixed point of parse + write
TEST(ClipIO, WriterIsAFixedPointForRandomClips){
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> time(0.0f, 20.0f);
    for (int round = 0; round < 200; round++){
        ClipData data;
        data.frameRate = 1.0f + std::fabs(unit(random)) * 239.0f;
        for (int t = 0; t < 5; t++){
            ClipTrackData track;
            track.path = "N" + std::to_string(t % 3);
            track.channel = (ClipChannel)(t % 5);
            track.loop = (round + t) % 4 == 0;
            track.relative = track.channel <= ClipChannel::Scale && (round + t) % 3 == 0;
            if (track.channel == ClipChannel::Lattice) track.points = {(int)(random() % 50), (int)(random() % 50) + 50};
            size_t width = track.channel == ClipChannel::Rotation ? 4 : track.channel == ClipChannel::Morph ? 1 + random() % 8
                         : track.channel == ClipChannel::Lattice ? 6 : 3;
            bool tangents = round % 5 == 0;
            int keyCount = 1 + (int)(random() % 6);
            for (int k = 0; k < keyCount; k++){
                ClipKey key;
                key.time = time(random);
                for (size_t i = 0; i < width; i++) key.value.push_back(unit(random) * (round % 2 ? 1000.0f : 1.0f));
                if (track.channel == ClipChannel::Rotation && k % 2 == 1){
                    // nearly opposite to the previous key: the hemisphere decision sits at a dot ~ 0
                    key.value = {1, 0, 0, 1e-7f * unit(random)};
                }
                // some rounds mix keys with and without tangents (normalize completes them)
                bool keyTangents = tangents || (round % 7 == 3 && k % 2 == 0);
                if (keyTangents){
                    for (size_t i = 0; i < width; i++){
                        key.inTangent.push_back(unit(random));
                        key.outTangent.push_back(unit(random));
                    }
                }else{
                    key.ease = (EaseType)(random() % (int)EaseType::CUSTOM);
                }
                track.keys.push_back(key);
            }
            data.tracks.push_back(track);
        }
        ClipEventData event;
        event.time = time(random);
        event.function = "f" + std::to_string(round);
        event.args = {AnimationEvent::numberArg(unit(random) * 1e6), AnimationEvent::stringArg("a\"b\\c\n"), AnimationEvent::boolArg(false)};
        data.events.push_back(event);

        std::string first = anim::writeClip(data);
        anim::ClipParseResult parsed = anim::parseClip(first);
        ASSERT_TRUE(parsed.ok()) << parsed.error.toString() << "\n" << first;
        EXPECT_TRUE(parsed.warnings.empty()) << "round " << round << ": " << listWarnings(parsed.warnings) << first;
        std::string second = anim::writeClip(*parsed.data);
        ASSERT_EQ(second, first) << "round " << round;
        EXPECT_EQ(anim::writeClip(AnimationClip::build(*parsed.data)->toData()), first) << "round " << round;
    }
}

TEST(ClipIO, PropertyTracksAreKeptAndFollowPathEdits){
    std::vector<ClipWarning> warnings;
    ClipData data = parseOk(wrapClip("\"tracks\": [{\"path\": \"Lamp\", \"channel\": \"property\", \"component\": \"Light\", "
                                     "\"property\": \"intensity\", \"keys\": [[0, 1], [1, 2]], \"future\": {\"x\": 1}}]"), &warnings);
    ASSERT_EQ(data.tracks.size(), 1u);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0]", "kept in the file, not played")) << listWarnings(warnings);
    EXPECT_TRUE(hasWarning(warnings, "tracks[0]", "did you mean 'light'")) << "component keys are lowercase: " << listWarnings(warnings);
    EXPECT_EQ(data.findTrack("Lamp", ClipChannel::Property), -1) << "findTrack never returns property tracks";
    std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("    {\"path\":\"Lamp\",\"channel\":\"property\",\"component\":\"Light\",\"property\":\"intensity\","
                        "\"keys\":[[0,1],[1,2]],\"future\":{\"x\":1}}\n"), std::string::npos) << text;
    // a Retarget renames the path: the written track follows, the rest stays verbatim
    data.tracks[0].path = "Rig/Lamp";
    text = anim::writeClip(data);
    EXPECT_NE(text.find("{\"path\":\"Rig/Lamp\",\"channel\":\"property\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"future\":{\"x\":1}"), std::string::npos) << text;
    EXPECT_EQ(anim::writeClip(parseOk(text)), text);
}
