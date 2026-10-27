// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime asset (spec 3.1, 3.7): create()'s size validation, clip lengths, socket sampling
// (single clip, frame interpolation with the quaternion hemisphere fix, the loop seam, a crossfade
// midpoint, a phase-synced blend, B alone, sockets resolved through recipe.sockets whatever their
// order), and the payload texture (format, sampler, unique ids).

#include <gtest/gtest.h>

#include "VatTestEnvironment.h"

#include "animation/VertexAnimation.h"
#include "texture/Texture.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    std::shared_ptr<const VertexAnimation> makeAsset(VatData data){
        VatPayload payload = vattest::samplePayload(data);
        VatSocketTracks sockets = vattest::sampleSockets(data);
        std::string error;
        std::shared_ptr<const VertexAnimation> asset = VertexAnimation::create(std::move(data), std::move(payload),
                                                                               std::move(sockets), &error);
        EXPECT_TRUE(asset) << error;
        return asset;
    }

    std::string createError(const VatData& data, const VatPayload& payload, const VatSocketTracks& sockets){
        std::string error;
        EXPECT_FALSE(VertexAnimation::create(data, payload, sockets, &error));
        return error;
    }

    // degrees about Y of a quaternion with only y/w (the sample sockets' rotations)
    float yawDegrees(const Quaternion& q){
        return (float)(2.0 * std::atan2((double)q.y, (double)q.w) * 180.0 / 3.14159265358979323846);
    }

}

TEST(VatAsset, CreateKeepsEverything){
    VatData data = vattest::sampleData();
    std::shared_ptr<const VertexAnimation> asset = makeAsset(data);
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset->getData().clips.size(), 2u);
    EXPECT_EQ(asset->getPayload().bytes.size(), 8u * 6u * 16u);
    EXPECT_EQ(asset->getSocketTracks().sockets, 2);
    EXPECT_EQ(asset->getSocketTracks().frames, 7);
    EXPECT_TRUE(asset->getPath().empty());
    EXPECT_EQ(asset->findClip("Wave"), 1);
    EXPECT_FLOAT_EQ(asset->clipLength(0), 1.0f);
    EXPECT_FLOAT_EQ(asset->clipLength(1), 1.0f);
    EXPECT_EQ(asset->clipLength(2), 0.0f);
    EXPECT_EQ(asset->clipLength(-1), 0.0f);

    // no sockets at all is fine
    data.sockets.clear();
    data.recipe.sockets.clear();
    std::string error;
    EXPECT_TRUE(VertexAnimation::create(data, vattest::samplePayload(data), VatSocketTracks(), &error)) << error;
}

TEST(VatAsset, CreateValidatesSizes){
    const VatData data = vattest::sampleData();
    const VatPayload payload = vattest::samplePayload(data);
    const VatSocketTracks sockets = vattest::sampleSockets(data);

    VatPayload half = payload;
    half.precision = VatPrecision::Half;
    EXPECT_NE(createError(data, half, sockets).find("the payload is 8 x 6 half, the layout says 8 x 6 float"), std::string::npos);
    VatPayload wide = payload;
    wide.width = 16;
    EXPECT_NE(createError(data, wide, sockets).find("8 x 6 float"), std::string::npos);
    VatPayload shortPayload = payload;
    shortPayload.bytes.resize(shortPayload.bytes.size() - 16);
    EXPECT_NE(createError(data, shortPayload, sockets).find("the payload has 752 bytes"), std::string::npos);

    VatData small = data;
    small.layout.height = 5;                 // 40 texels, the samples need 42
    VatPayload smallPayload = vattest::samplePayload(small);
    EXPECT_NE(createError(small, smallPayload, sockets).find("the samples need 42 texels"), std::string::npos);

    VatData past = data;
    past.clips[1].frames = 4;                // 4 + 4 > 7 samples
    EXPECT_NE(createError(past, payload, sockets).find("clip \"Wave\""), std::string::npos);
    VatData noRate = data;
    noRate.clips[0].fps = 0.0f;
    EXPECT_NE(createError(noRate, payload, sockets).find("clip \"Walk\""), std::string::npos);

    VatSocketTracks shortTracks = sockets;
    shortTracks.data.pop_back();
    EXPECT_NE(createError(data, payload, shortTracks).find("the socket tracks have 97 floats"), std::string::npos);
    VatSocketTracks fewFrames = sockets;
    fewFrames.frames = 6;
    fewFrames.data.resize(6 * 2 * 7);
    EXPECT_NE(createError(data, payload, fewFrames).find("6 frames, the payload 7 samples"), std::string::npos);
    EXPECT_NE(createError(data, payload, VatSocketTracks()).find("socket \"Hand\" has no baked track"), std::string::npos);
    VatData unknownSocket = data;
    unknownSocket.sockets[1].path = "Root/Foot";
    EXPECT_NE(createError(unknownSocket, payload, sockets).find("socket \"Head\""), std::string::npos);

    VatData bones = data;
    bones.mode = VatMode::Bones;
    bones.layout.bones = 0;
    EXPECT_NE(createError(bones, payload, sockets).find("bones >= 1"), std::string::npos);
    VatData noVertices = data;
    noVertices.layout.vertices = 0;
    EXPECT_NE(createError(noVertices, payload, sockets).find("vertices >= 1"), std::string::npos);
    VatData empty = data;
    empty.layout.height = 0;
    EXPECT_NE(createError(empty, payload, sockets).find("invalid layout"), std::string::npos);

    // every error asks for a rebake; a null error pointer is fine
    EXPECT_NE(createError(data, half, sockets).find("rebake"), std::string::npos);
    EXPECT_FALSE(VertexAnimation::create(data, half, sockets, nullptr));
}

TEST(VatAsset, SocketOfOneClip){
    std::shared_ptr<const VertexAnimation> asset = makeAsset(vattest::sampleData());
    ASSERT_TRUE(asset);
    VatTrackPair pair;
    pair.a = {0, 2.0f, 1.0f};               // Walk started at 2 s
    Vector3 position;
    Quaternion rotation;
    // Walk frame 1 at 2.25 s: socket 0 (Hand) at (1, 0, 0), 10 degrees
    ASSERT_TRUE(asset->sampleSocket(0, pair, 2.25f, position, rotation));
    EXPECT_NEAR(position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(position.y, 0.0f, 1e-5f);
    EXPECT_NEAR(yawDegrees(rotation), 10.0f, 1e-3f);
    // socket 1 (Head) is 10 up
    ASSERT_TRUE(asset->sampleSocket(1, pair, 2.25f, position, rotation));
    EXPECT_NEAR(position.y, 10.0f, 1e-5f);
    // between frames 1 and 2: lerp + nlerp
    ASSERT_TRUE(asset->sampleSocket(0, pair, 2.375f, position, rotation));
    EXPECT_NEAR(position.x, 1.5f, 1e-5f);
    EXPECT_NEAR(yawDegrees(rotation), 15.0f, 0.05f);
    EXPECT_NEAR(rotation.w * rotation.w + rotation.y * rotation.y, 1.0f, 1e-5f);
    // the loop seam: frame 3 into frame 0
    ASSERT_TRUE(asset->sampleSocket(0, pair, 2.875f, position, rotation));
    EXPECT_NEAR(position.x, 1.5f, 1e-5f);
    EXPECT_NEAR(yawDegrees(rotation), 15.0f, 0.05f);      // 30 and 0 degrees
    // the track's speed: Walk at double speed, 0.125 s in = frame 1
    pair.a.speed = 2.0f;
    ASSERT_TRUE(asset->sampleSocket(0, pair, 2.125f, position, rotation));
    EXPECT_NEAR(position.x, 1.0f, 1e-5f);
    // backwards: -0.125 s = frame 3.5
    pair.a.speed = -1.0f;
    ASSERT_TRUE(asset->sampleSocket(0, pair, 2.125f, position, rotation));
    EXPECT_NEAR(position.x, 1.5f, 1e-5f);
    // Wave (no interpolation): the lower sample, global frames 4..6
    pair.a = {1, 0.0f, 1.0f};
    ASSERT_TRUE(asset->sampleSocket(0, pair, 0.75f, position, rotation));
    EXPECT_NEAR(position.x, 5.0f, 1e-5f);
    ASSERT_TRUE(asset->sampleSocket(0, pair, 9.0f, position, rotation));
    EXPECT_NEAR(position.x, 6.0f, 1e-5f);

    // unknown socket, no clip, a clip index past the table
    EXPECT_FALSE(asset->sampleSocket(2, pair, 0.0f, position, rotation));
    EXPECT_FALSE(asset->sampleSocket(-1, pair, 0.0f, position, rotation));
    VatTrackPair rest;
    EXPECT_FALSE(asset->sampleSocket(0, rest, 0.0f, position, rotation));
    rest.a.clip = 2;
    EXPECT_FALSE(asset->sampleSocket(0, rest, 0.0f, position, rotation));
}

TEST(VatAsset, SocketsResolveThroughTheRecipeWhateverTheirOrder){
    VatData data = vattest::sampleData();
    std::swap(data.sockets[0], data.sockets[1]);     // "Head" listed first
    data.sockets[0].name = "Hat";
    std::shared_ptr<const VertexAnimation> asset = makeAsset(data);
    ASSERT_TRUE(asset);
    VatTrackPair pair;
    pair.a = {0, 0.0f, 1.0f};
    Vector3 position;
    Quaternion rotation;
    ASSERT_TRUE(asset->sampleSocket(asset->getData().findSocket("Hat"), pair, 0.0f, position, rotation));
    EXPECT_NEAR(position.y, 10.0f, 1e-5f);
    ASSERT_TRUE(asset->sampleSocket(asset->getData().findSocket("Hand"), pair, 0.0f, position, rotation));
    EXPECT_NEAR(position.y, 0.0f, 1e-5f);

    // imports list no recipe sockets: the JSON order is the track order
    data = vattest::sampleData();
    VatSocketTracks tracks = vattest::sampleSockets(data);
    data.recipe.sockets.clear();
    std::swap(data.sockets[0], data.sockets[1]);
    std::string error;
    asset = VertexAnimation::create(data, vattest::samplePayload(data), tracks, &error);
    ASSERT_TRUE(asset) << error;
    ASSERT_TRUE(asset->sampleSocket(0, pair, 0.0f, position, rotation));
    EXPECT_NEAR(position.y, 0.0f, 1e-5f);
}

TEST(VatAsset, SocketThroughACrossfadeAndABlend){
    std::shared_ptr<const VertexAnimation> asset = makeAsset(vattest::sampleData());
    ASSERT_TRUE(asset);
    Vector3 position;
    Quaternion rotation;

    // crossfade midpoint: A Walk at frame 2 (x 2), B Wave at its own time 0.5 s = frame 5 (x 5)
    VatTrackPair fade;
    fade.a = {0, 0.0f, 1.0f};
    fade.b = {1, 0.0f, 1.0f};
    fade.fadeStart = 0.0f;
    fade.fadeEnd = 1.0f;
    ASSERT_TRUE(asset->sampleSocket(0, fade, 0.5f, position, rotation));
    EXPECT_NEAR(position.x, 3.5f, 1e-5f);
    // rotations: 20 and 50 degrees, nlerp half way ~ 35
    EXPECT_NEAR(yawDegrees(rotation), 35.0f, 0.5f);
    // before and after the fade
    ASSERT_TRUE(asset->sampleSocket(0, fade, 0.0f, position, rotation));
    EXPECT_NEAR(position.x, 0.0f, 1e-5f);
    fade.fadeStart = 0.25f;
    fade.fadeEnd = 0.5f;
    ASSERT_TRUE(asset->sampleSocket(0, fade, 0.5f, position, rotation));
    EXPECT_NEAR(position.x, 5.0f, 1e-5f);

    // blend: B's phase follows A's (Walk at 0.5 = half way; Wave half way = frame 5) whatever
    // B's own start
    VatTrackPair blend;
    blend.a = {0, 0.0f, 1.0f};
    blend.b = {1, 100.0f, 1.0f};
    blend.blend = 0.5f;
    ASSERT_TRUE(asset->sampleSocket(0, blend, 0.5f, position, rotation));
    EXPECT_NEAR(position.x, 3.5f, 1e-5f);
    blend.blend = 1.0f;
    ASSERT_TRUE(asset->sampleSocket(0, blend, 0.25f, position, rotation));
    EXPECT_NEAR(position.x, 4.0f, 1e-5f);     // Wave at phase 0.25 = frame 4.5, no interpolation
    blend.blend = 0.0f;
    ASSERT_TRUE(asset->sampleSocket(0, blend, 0.25f, position, rotation));
    EXPECT_NEAR(position.x, 1.0f, 1e-5f);

    // B alone (A at rest): the sockets have no rest pose, B is used as it is
    VatTrackPair onlyB;
    onlyB.b = {1, 0.0f, 1.0f};
    onlyB.fadeStart = 0.0f;
    onlyB.fadeEnd = 10.0f;
    ASSERT_TRUE(asset->sampleSocket(0, onlyB, 0.5f, position, rotation));
    EXPECT_NEAR(position.x, 5.0f, 1e-5f);
}

TEST(VatAsset, SocketRotationsTakeTheShortWay){
    VatData data = vattest::sampleData();
    VatSocketTracks sockets = vattest::sampleSockets(data);
    // frame 2 of socket 0 stored as -q (the same rotation, the other hemisphere)
    for (int i = 3; i < 7; i++) sockets.data[(2 * 2 + 0) * 7 + i] *= -1.0f;
    std::string error;
    std::shared_ptr<const VertexAnimation> asset = VertexAnimation::create(data, vattest::samplePayload(data), sockets, &error);
    ASSERT_TRUE(asset) << error;
    VatTrackPair pair;
    pair.a = {0, 0.0f, 1.0f};
    Vector3 position;
    Quaternion rotation;
    ASSERT_TRUE(asset->sampleSocket(0, pair, 0.375f, position, rotation));
    float length = std::sqrt(rotation.w * rotation.w + rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z);
    EXPECT_NEAR(length, 1.0f, 1e-5f);
    // 15 degrees about Y (a lerp without the fix would pass near zero and point anywhere)
    const double half = 7.5 * 3.14159265358979323846 / 180.0;
    EXPECT_NEAR(rotation.y, (float)std::sin(half), 1e-4f);
    EXPECT_NEAR(rotation.w, (float)std::cos(half), 1e-4f);
}

TEST(VatAsset, PayloadTexture){
    VatData data = vattest::sampleData();
    std::shared_ptr<const VertexAnimation> asset = makeAsset(data);
    ASSERT_TRUE(asset);
    Texture* texture = asset->getTexture();
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture, asset->getTexture());
    EXPECT_EQ(texture->getId().rfind("vat:memory#", 0), 0u) << texture->getId();
    EXPECT_EQ(texture->getMinFilter(), TextureFilter::NEAREST);
    EXPECT_EQ(texture->getMagFilter(), TextureFilter::NEAREST);
    EXPECT_EQ(texture->getWrapU(), TextureWrap::CLAMP_TO_EDGE);
    EXPECT_EQ(texture->getWrapV(), TextureWrap::CLAMP_TO_EDGE);
    ASSERT_TRUE(texture->hasData());
    TextureData& pixels = texture->getData();
    EXPECT_EQ(pixels.getWidth(), 8);
    EXPECT_EQ(pixels.getHeight(), 6);
    EXPECT_EQ(pixels.getChannels(), 4);
    EXPECT_EQ(pixels.getColorFormat(), ColorFormat::RGBA32F);
    EXPECT_EQ(pixels.getSize(), 8u * 6u * 16u);
    EXPECT_EQ(pixels.getData(), (const void*)asset->getPayload().bytes.data());
    EXPECT_FALSE(texture->isReleaseDataAfterLoad());

    // half precision: RGBA16F; another asset never shares the pooled pixels
    VatData halfData = data;
    halfData.layout.precision = VatPrecision::Half;
    std::shared_ptr<const VertexAnimation> half = makeAsset(halfData);
    ASSERT_TRUE(half);
    Texture* halfTexture = half->getTexture();
    EXPECT_NE(halfTexture->getId(), texture->getId());
    EXPECT_EQ(halfTexture->getData().getColorFormat(), ColorFormat::RGBA16F);
    EXPECT_EQ(halfTexture->getData().getSize(), 8u * 6u * 8u);
    EXPECT_EQ(halfTexture->getData().getData(), (const void*)half->getPayload().bytes.data());

    // released with the asset: its pooled data entry goes too
    std::string id = halfTexture->getId();
    half.reset();
    EXPECT_EQ(TextureDataPool::get(id), nullptr);
}
