// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The values scenes are made of: vectors, quaternions, matrices, rectangles, boxes, stencils.
// Written as flow sequences of exact decimal text, read back to the same floats; NaN and
// infinity never reach a file.

#include "StreamTestAccess.h"
#include "component/Transform.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // through text, as a file round-trips
    YAML::Node reread(const YAML::Node& node){
        return YAML::Load(YAML::Dump(node));
    }
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
}

TEST(StreamValue, Vector2RoundTrips){
    const Vector2 v(1.5f, -0.1f);
    EXPECT_EQ(StreamTestAccess::decodeVector2(reread(StreamTestAccess::encodeVector2(v))), v);
}

TEST(StreamValue, Vector3RoundTripsExactly){
    const Vector3 v(0.1f, -123456.789f, 3.0e-7f);
    EXPECT_EQ(StreamTestAccess::decodeVector3(reread(StreamTestAccess::encodeVector3(v))), v);
}

TEST(StreamValue, Vector3IsAFlowSequence){
    EXPECT_EQ(YAML::Dump(StreamTestAccess::encodeVector3(Vector3(1, 2.5f, -3))), "[1, 2.5, -3]");
}

TEST(StreamValue, Vector4RoundTrips){
    const Vector4 v(0.25f, 0.5f, 0.75f, 1.0f);
    EXPECT_EQ(StreamTestAccess::decodeVector4(reread(StreamTestAccess::encodeVector4(v))), v);
}

TEST(StreamValue, QuaternionRoundTrips){
    const Quaternion q(33.0f, Vector3(1, 2, 3).normalized());
    EXPECT_EQ(StreamTestAccess::decodeQuaternion(reread(StreamTestAccess::encodeQuaternion(q))), q);
}

TEST(StreamValue, Matrix4RoundTrips){
    const Matrix4 m = Matrix4::translateMatrix(1, 2, 3) * Matrix4::rotateMatrix(17.0f, Vector3(0, 1, 0)) * Matrix4::scaleMatrix(0.3f);
    EXPECT_EQ(StreamTestAccess::decodeMatrix4(reread(StreamTestAccess::encodeMatrix4(m))), m);
}

TEST(StreamValue, RectRoundTrips){
    Rect rect(0.1f, 0.2f, 0.3f, 0.4f);
    Rect read = StreamTestAccess::decodeRect(reread(StreamTestAccess::encodeRect(rect)));
    EXPECT_TRUE(read == rect);
}

TEST(StreamValue, AABBRoundTrips){
    const AABB box(Vector3(-1, -2, -3), Vector3(4, 5, 6));
    EXPECT_TRUE(StreamTestAccess::decodeAABB(reread(StreamTestAccess::encodeAABB(box))) == box);
}

TEST(StreamValue, NonFiniteValuesAreWrittenAsZero){
    EXPECT_EQ(StreamTestAccess::decodeVector3(reread(StreamTestAccess::encodeVector3(Vector3(notANumber, infinity, -infinity)))), Vector3(0, 0, 0));
    EXPECT_FLOAT_EQ(StreamTestAccess::finiteOr(notANumber, 7.0f), 7.0f);
    EXPECT_FLOAT_EQ(StreamTestAccess::finiteOr(infinity, notANumber), 0.0f);
    EXPECT_FLOAT_EQ(StreamTestAccess::positiveFiniteOr(-2.0f, 3.0f), 3.0f);
    EXPECT_FLOAT_EQ(StreamTestAccess::positiveFiniteOr(0.0f, -1.0f), 1.0f);
    EXPECT_FLOAT_EQ(StreamTestAccess::positiveFiniteOr(2.0f, 3.0f), 2.0f);
}

TEST(StreamValue, MalformedVectorsGiveTheirDefaults){
    EXPECT_EQ(StreamTestAccess::decodeVector3(YAML::Load("[1, 2]")), Vector3());
    EXPECT_EQ(StreamTestAccess::decodeVector3(YAML::Load("text")), Vector3());
    EXPECT_EQ(StreamTestAccess::decodeQuaternion(YAML::Load("[1, 2, 3]")), Quaternion::IDENTITY);
    EXPECT_EQ(StreamTestAccess::decodeVector3(YAML::Node()), Vector3());
}

TEST(StreamValue, LayerMasksReadUnsignedAndMinusOne){
    EXPECT_EQ(StreamTestAccess::decodeLayerMask(YAML::Load("5"), 0), 5u);
    EXPECT_EQ(StreamTestAccess::decodeLayerMask(YAML::Load("4294967295"), 0), 0xFFFFFFFFu);
    EXPECT_EQ(StreamTestAccess::decodeLayerMask(YAML::Load("-1"), 0), 0xFFFFFFFFu);
    EXPECT_EQ(StreamTestAccess::decodeLayerMask(YAML::Load("lots"), 9), 9u);
    EXPECT_EQ(StreamTestAccess::decodeLayerMask(YAML::Node(), 3), 3u);
}

TEST(StreamValue, StencilWritesOnlyWhatDiffersFromTheDefaults){
    const YAML::Node defaults = StreamTestAccess::encodeStencil(MaterialStencil());
    EXPECT_EQ(defaults.size(), 1u);
    MaterialStencil stencil;
    stencil.compare = MaterialStencilCompare::EQUAL;
    stencil.ref = 3;
    stencil.pass = MaterialStencilOp::REPLACE;
    const YAML::Node node = StreamTestAccess::encodeStencil(stencil);
    EXPECT_EQ(node.size(), 3u);
    EXPECT_TRUE(StreamTestAccess::decodeStencil(reread(node)) == stencil);
}

TEST(StreamValue, TransformKeepsItsValues){
    Transform transform;
    transform.position = Vector3(1, 2, 3);
    transform.rotation = Quaternion(45.0f, Vector3::UNIT_Z);
    transform.scale = Vector3(1, 2, 1);
    const Transform read = Stream::decodeTransform(reread(Stream::encodeTransform(transform)));
    EXPECT_EQ(read.position, transform.position);
    EXPECT_EQ(read.rotation, transform.rotation);
    EXPECT_EQ(read.scale, transform.scale);
}
