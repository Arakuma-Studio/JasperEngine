// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Presets, resample, mirror, selection helpers and copyAsLua (spec §4).

#include "LatticeTestCommon.h"

#include <algorithm>
#include <limits>
#include <cstdio>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;
using lattice::LatticeAxis;
using lattice::LatticePreset;

namespace{

    const LatticePreset allPresets[] = {
        LatticePreset::SQUASH, LatticePreset::STRETCH, LatticePreset::TAPER, LatticePreset::TWIST,
        LatticePreset::BEND, LatticePreset::BULGE, LatticePreset::SHEAR, LatticePreset::NOISE
    };

    lattice::PresetParams preset(LatticePreset type, LatticeAxis axis, float amount, LatticeAxis toward = LatticeAxis::X){
        lattice::PresetParams params;
        params.preset = type;
        params.axis = axis;
        params.toward = toward;
        params.amount = amount;
        return params;
    }

    // rest + offset of point (i, j, k)
    Vector3 position(int rx, int ry, int rz, const std::vector<Vector3>& offsets, int i, int j, int k){
        return lattice::restPosition(rx, ry, rz, i, j, k) + offsets[size_t(lattice::pointIndex(rx, ry, i, j, k))];
    }

    bool sameBits(const std::vector<Vector3>& a, const std::vector<Vector3>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            if (!bitwiseEqual(a[i], b[i])) return false;
        }
        return true;
    }

    std::vector<Vector3> applied(int rx, int ry, int rz, const lattice::PresetParams& params, const std::vector<int>& selection = {}){
        std::vector<Vector3> offsets(size_t(lattice::pointCount(rx, ry, rz)), Vector3(0, 0, 0));
        lattice::applyPreset(rx, ry, rz, offsets, params, selection);
        return offsets;
    }

}

// --- names ---

TEST(LatticePresets, Names){
    const std::vector<std::string> names = lattice::presetNames();
    const std::vector<std::string> expected = {"squash", "stretch", "taper", "twist", "bend", "bulge", "shear", "noise"};
    EXPECT_EQ(names, expected);
    for (size_t i = 0; i < expected.size(); i++){
        LatticePreset value = LatticePreset::NOISE;
        ASSERT_TRUE(lattice::presetFromName(expected[i], value));
        EXPECT_EQ(value, allPresets[i]);
        EXPECT_EQ(std::string(lattice::presetName(allPresets[i])), expected[i]);
    }
    LatticePreset untouched = LatticePreset::BEND;
    EXPECT_FALSE(lattice::presetFromName("", untouched));
    EXPECT_FALSE(lattice::presetFromName("wobble", untouched));
    EXPECT_FALSE(lattice::presetFromName("Twist", untouched));
    EXPECT_EQ(untouched, LatticePreset::BEND);
}

// --- presets ---

TEST(LatticePresets, AmountZeroIsANoOpForEveryPreset){
    LatticeComponent lattice = makeLattice(4, 3, 5);
    randomOffsets(lattice, 5, 0.2f);
    lattice.offsets[3] = Vector3(-0.0f, 0, 0);
    for (LatticePreset type : allPresets){
        for (LatticeAxis axis : {LatticeAxis::X, LatticeAxis::Y, LatticeAxis::Z}){
            std::vector<Vector3> offsets = lattice.offsets;
            lattice::applyPreset(4, 3, 5, offsets, preset(type, axis, 0.0f));
            EXPECT_TRUE(sameBits(offsets, lattice.offsets)) << lattice::presetName(type);
        }
        // replace with amount 0 puts the selected points at rest, and only those
        std::vector<Vector3> offsets = lattice.offsets;
        lattice::PresetParams params = preset(type, LatticeAxis::Y, 0.0f);
        params.replace = true;
        lattice::applyPreset(4, 3, 5, offsets, params, {1, 7});
        for (size_t i = 0; i < offsets.size(); i++){
            if (i == 1 || i == 7){
                EXPECT_EQ(offsets[i], Vector3(0, 0, 0));
            }else{
                EXPECT_TRUE(bitwiseEqual(offsets[i], lattice.offsets[i]));
            }
        }
    }
}

TEST(LatticePresets, SquashAndStretchKeepTheVolume){
    for (LatticePreset type : {LatticePreset::SQUASH, LatticePreset::STRETCH}){
        for (float amount : {0.25f, 0.5f, 0.9f}){
            for (LatticeInterpolation interpolation : {LatticeInterpolation::SHARP, LatticeInterpolation::CUBIC}){
                LatticeComponent lattice = makeLattice(3, 4, 3, interpolation);
                lattice::applyPreset(3, 4, 3, lattice.offsets, preset(type, LatticeAxis::Y, amount));
                const float s = type == LatticePreset::SQUASH ? 1.0f - amount : 1.0f + amount;
                // along the axis scaled by s, across by 1/sqrt(s)
                const Vector3 top = position(3, 4, 3, lattice.offsets, 2, 3, 2);
                EXPECT_NEAR(top.y, 0.5f * s, 1e-6f);
                EXPECT_NEAR(top.x, 0.5f / std::sqrt(s), 1e-6f);
                EXPECT_NEAR(top.z, 0.5f / std::sqrt(s), 1e-6f);

                std::vector<Vector3> padded;
                lattice::buildPaddedGrid(lattice, padded);
                Random random(3);
                for (int n = 0; n < 50; n++){
                    const lattice::Evaluation e = lattice::evaluate(lattice, padded, random.vector(0, 1), 1.0f);
                    EXPECT_NEAR(e.stretch, 1.0f, 2e-5f) << lattice::presetName(type) << " " << amount;
                }
            }
        }
    }
    // squash is capped at 0.95 (the cage never flattens), stretch floored at -0.95
    const std::vector<Vector3> capped = applied(3, 3, 3, preset(LatticePreset::SQUASH, LatticeAxis::Z, 3.0f));
    EXPECT_NEAR(position(3, 3, 3, capped, 1, 1, 2).z, 0.5f * 0.05f, 1e-6f);
    const std::vector<Vector3> floored = applied(3, 3, 3, preset(LatticePreset::STRETCH, LatticeAxis::Z, -3.0f));
    EXPECT_NEAR(position(3, 3, 3, floored, 1, 1, 2).z, 0.5f * 0.05f, 1e-6f);
}

TEST(LatticePresets, Taper){
    const std::vector<Vector3> offsets = applied(3, 3, 3, preset(LatticePreset::TAPER, LatticeAxis::Y, 0.6f));
    // no change at the -0.5 end, 1 - amount at +0.5, halfway between in the middle
    for (int k = 0; k < 3; k++){
        for (int i = 0; i < 3; i++){
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, 3, i, 0, k))], Vector3(0, 0, 0));
            const Vector3 rest = lattice::restPosition(3, 3, 3, i, 2, k);
            EXPECT_LT(maxAbsDifference(position(3, 3, 3, offsets, i, 2, k), Vector3(rest.x * 0.4f, 0.5f, rest.z * 0.4f)), 1e-6f);
            const Vector3 middle = lattice::restPosition(3, 3, 3, i, 1, k);
            EXPECT_LT(maxAbsDifference(position(3, 3, 3, offsets, i, 1, k), Vector3(middle.x * 0.7f, 0, middle.z * 0.7f)), 1e-6f);
        }
    }
}

TEST(LatticePresets, TwistTurnsCounterclockwiseSeenFromThePositiveAxis){
    const std::vector<Vector3> offsets = applied(3, 3, 3, preset(LatticePreset::TWIST, LatticeAxis::Y, 90.0f));
    // the +0.5 end turns 90 degrees around +y (x goes to -z), the middle 45, the -0.5 end stays
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, offsets, 2, 2, 1), Vector3(0, 0.5f, -0.5f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, offsets, 1, 2, 2), Vector3(0.5f, 0.5f, 0)), 1e-6f);
    const float h = 0.5f * std::sqrt(0.5f);
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, offsets, 2, 1, 1), Vector3(h, 0, -h)), 1e-6f);
    for (int k = 0; k < 3; k++){
        for (int i = 0; i < 3; i++){
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, 3, i, 0, k))], Vector3(0, 0, 0));
        }
    }
    // around x: y goes to z; around z: x goes to y
    const std::vector<Vector3> aroundX = applied(3, 3, 3, preset(LatticePreset::TWIST, LatticeAxis::X, 90.0f));
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, aroundX, 2, 2, 1), Vector3(0.5f, 0, 0.5f)), 1e-6f);
    const std::vector<Vector3> aroundZ = applied(3, 3, 3, preset(LatticePreset::TWIST, LatticeAxis::Z, 90.0f));
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, aroundZ, 2, 1, 2), Vector3(0, 0.5f, 0.5f)), 1e-6f);
}

TEST(LatticePresets, BendIsAnArcAroundTheNegativeEndCentre){
    const double theta = 90.0 * 3.14159265358979323846 / 180.0;
    const double radius = 1.0 / theta;
    const int ry = 9;
    const std::vector<Vector3> offsets = applied(3, ry, 3, preset(LatticePreset::BEND, LatticeAxis::Y, 90.0f, LatticeAxis::X));
    // the -0.5 end stays put
    for (int k = 0; k < 3; k++){
        for (int i = 0; i < 3; i++){
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, ry, i, 0, k))], Vector3(0, 0, 0));
        }
    }
    // the axis line (x = z = 0) lies on the circle of radius 1/theta centred at (x, y) = (R, -0.5),
    // at angle theta * s, so its length (1) is kept
    Vector3 previous(0, -0.5f, 0);
    for (int j = 0; j < ry; j++){
        const double s = double(j) / double(ry - 1);
        const Vector3 p = position(3, ry, 3, offsets, 1, j, 1);
        EXPECT_NEAR(p.x, radius - radius * std::cos(theta * s), 1e-6);
        EXPECT_NEAR(p.y, -0.5 + radius * std::sin(theta * s), 1e-6);
        EXPECT_EQ(p.z, 0.0f);
        // neighbours are one chord of the arc apart
        EXPECT_NEAR(p.distance(previous), j == 0 ? 0.0 : 2.0 * radius * std::sin(theta / double(ry - 1) / 2.0), 1e-6);
        previous = p;
    }
    // the +0.5 end has turned 90 degrees: the end face now faces +x, and a point at
    // toward-coordinate b sits b along the arc normal (here -y) from the end centre
    const Vector3 endCentre = position(3, ry, 3, offsets, 1, ry - 1, 1);
    const Vector3 endPlusX = position(3, ry, 3, offsets, 2, ry - 1, 1);
    EXPECT_LT(maxAbsDifference(endCentre, Vector3(float(radius), float(-0.5 + radius), 0)), 1e-6f);
    EXPECT_LT(maxAbsDifference(endPlusX - endCentre, Vector3(0, -0.5f, 0)), 1e-6f);

    // a negative amount bends toward -x, the mirror image
    const std::vector<Vector3> negative = applied(3, ry, 3, preset(LatticePreset::BEND, LatticeAxis::Y, -90.0f, LatticeAxis::X));
    for (int j = 0; j < ry; j++){
        const Vector3 a = position(3, ry, 3, offsets, 2, j, 1);
        const Vector3 b = position(3, ry, 3, negative, 0, j, 1);
        EXPECT_LT(maxAbsDifference(a, Vector3(-b.x, b.y, b.z)), 1e-6f);
    }

    // a tiny bend is nearly nothing (no cancellation blow-up)
    const std::vector<Vector3> tiny = applied(3, 3, 3, preset(LatticePreset::BEND, LatticeAxis::Y, 1e-4f, LatticeAxis::X));
    for (const Vector3& offset : tiny){
        EXPECT_LT(offset.length(), 1e-5f);
    }
}

TEST(LatticePresets, BendAndTwistUseTheCageProportions){
    // a cage 1 x 4 x 1 in the world: the bend turns its 4-unit axis by a true 60 degrees and keeps
    // the 1-unit cross-section rigid (in local units the cage is a unit cube)
    const double theta = 60.0 * 3.14159265358979323846 / 180.0;
    const double radius = 4.0 / theta; // world units
    const int ry = 9;
    lattice::PresetParams bend = preset(LatticePreset::BEND, LatticeAxis::Y, 60.0f, LatticeAxis::X);
    bend.proportions = Vector3(1.0f, 4.0f, 1.0f);
    const std::vector<Vector3> offsets = applied(3, ry, 3, bend);
    auto world = [](const Vector3& local){ return Vector3(local.x * 1.0f, local.y * 4.0f, local.z * 1.0f); };
    for (int j = 0; j < ry; j++){
        const double s = double(j) / double(ry - 1);
        const Vector3 p = world(position(3, ry, 3, offsets, 1, j, 1));
        EXPECT_NEAR(p.x, radius - radius * std::cos(theta * s), 1e-5);
        EXPECT_NEAR(p.y, -2.0 + radius * std::sin(theta * s), 1e-5);
    }
    // the end face has turned 60 degrees: its +x half-width (0.5 world units) now points along
    // (cos 60, -sin 60)
    const Vector3 endCentre = world(position(3, ry, 3, offsets, 1, ry - 1, 1));
    const Vector3 endPlusX = world(position(3, ry, 3, offsets, 2, ry - 1, 1));
    EXPECT_LT(maxAbsDifference(endPlusX - endCentre, Vector3(float(0.5 * std::cos(theta)), float(-0.5 * std::sin(theta)), 0)), 1e-5f);

    // twist around y of a cage 2 x 1 x 1: the +x corner of the top (1 world unit out) turns to
    // 1 world unit along -z, which is local z = -1 in a cage 1 unit deep
    lattice::PresetParams twist = preset(LatticePreset::TWIST, LatticeAxis::Y, 90.0f);
    twist.proportions = Vector3(2.0f, 1.0f, 1.0f);
    const std::vector<Vector3> twisted = applied(3, 3, 3, twist);
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, twisted, 2, 2, 1), Vector3(0, 0.5f, -1.0f)), 1e-6f);
    // and the -z corner (0.5 world units out) turns to +x: local x = 0.25 on a 2-unit-wide cage
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, twisted, 1, 2, 0), Vector3(-0.25f, 0.5f, 0)), 1e-6f);

    // unit proportions are the plain unit-cube presets; invalid components take the largest valid one
    lattice::PresetParams unit = preset(LatticePreset::BEND, LatticeAxis::Y, 60.0f);
    lattice::PresetParams invalid = unit;
    invalid.proportions = Vector3(0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(sameBits(applied(3, 3, 3, unit), applied(3, 3, 3, invalid)));

    // presetProportions reads the axis lengths of a cage's world matrix (rotation ignored)
    Matrix4 model = Matrix4::translateMatrix(Vector3(5, 6, 7)) * Matrix4::rotateMatrix(30.0f, Vector3(0, 1, 0)) *
                    Matrix4::scaleMatrix(Vector3(2, 3, 4));
    EXPECT_LT(maxAbsDifference(lattice::presetProportions(model), Vector3(2, 3, 4)), 1e-5f);
}

TEST(LatticePresets, BulgeAndShear){
    const std::vector<Vector3> bulge = applied(3, 3, 3, preset(LatticePreset::BULGE, LatticeAxis::Y, 0.5f));
    // max at the middle (scale 1.5), none at the ends
    EXPECT_LT(maxAbsDifference(position(3, 3, 3, bulge, 2, 1, 0), Vector3(0.75f, 0, -0.75f)), 1e-6f);
    EXPECT_EQ(bulge[size_t(lattice::pointIndex(3, 3, 2, 0, 0))], Vector3(0, 0, 0));
    EXPECT_EQ(bulge[size_t(lattice::pointIndex(3, 3, 2, 2, 0))], Vector3(0, 0, 0));

    const std::vector<Vector3> shear = applied(3, 3, 3, preset(LatticePreset::SHEAR, LatticeAxis::Y, 0.4f, LatticeAxis::Z));
    for (int k = 0; k < 3; k++){
        for (int j = 0; j < 3; j++){
            for (int i = 0; i < 3; i++){
                EXPECT_LT(maxAbsDifference(shear[size_t(lattice::pointIndex(3, 3, i, j, k))], Vector3(0, 0, 0.4f * float(j) / 2.0f)), 1e-6f);
            }
        }
    }
    // toward equal to the axis means the next axis (y -> z)
    EXPECT_TRUE(sameBits(applied(3, 3, 3, preset(LatticePreset::SHEAR, LatticeAxis::Y, 0.4f, LatticeAxis::Y)), shear));
}

TEST(LatticePresets, NoiseIsDeterministicAndUniformInABall){
    lattice::PresetParams params = preset(LatticePreset::NOISE, LatticeAxis::Y, 0.5f);
    params.seed = 1;
    const std::vector<Vector3> a = applied(3, 3, 3, params);
    // golden values, from an independent implementation of the same hash; exact floats
    EXPECT_EQ(a[0], Vector3(0.23616504669189453f, -0.16026008129119873f, 0.15222352743148804f));
    EXPECT_EQ(a[1], Vector3(0.1117849349975586f, 0.07511788606643677f, 0.338405966758728f));
    EXPECT_EQ(a[26], Vector3(-0.2540292739868164f, -0.0646248459815979f, -0.2682218551635742f));
    params.seed = 7;
    const std::vector<Vector3> b = applied(3, 3, 3, params);
    EXPECT_EQ(b[0], Vector3(0.37712550163269043f, -0.25802427530288696f, -0.18276143074035645f));
    EXPECT_EQ(b[13], Vector3(-0.2064039707183838f, -0.30310922861099243f, 0.15502071380615234f));
    params.seed = 4000000000u;
    EXPECT_EQ(applied(3, 3, 3, params)[5], Vector3(-0.24047428369522095f, 0.19255346059799194f, -0.13246667385101318f));

    // same seed, same result; the axis does not matter
    params.seed = 7;
    params.axis = LatticeAxis::X;
    EXPECT_TRUE(sameBits(applied(3, 3, 3, params), b));

    // uniform in the ball: inside it, and a ball of half the radius holds about 1/8 of the points
    params.seed = 12345;
    params.amount = 2.0f;
    const std::vector<Vector3> many = applied(16, 16, 16, params);
    int inner = 0;
    Vector3 mean(0, 0, 0);
    for (const Vector3& v : many){
        EXPECT_LT(v.length(), 2.0f);
        if (v.length() < 1.0f) inner++;
        mean += v;
    }
    EXPECT_NEAR(double(inner) / double(many.size()), 0.125, 0.02);
    EXPECT_LT((mean / float(many.size())).length(), 0.08f);

    // add mode adds the same vectors on top
    params.seed = 1;
    params.amount = 0.5f;
    std::vector<Vector3> offsets(27, Vector3(1, 1, 1));
    lattice::applyPreset(3, 3, 3, offsets, params);
    EXPECT_EQ(offsets[1], a[1] + Vector3(1, 1, 1));
}

TEST(LatticePresets, OnlySelectedPointsChange){
    const std::vector<int> selection = {0, 4, 26, 4, -3, 99}; // repeats and invalid indices
    for (LatticePreset type : allPresets){
        const lattice::PresetParams params = preset(type, LatticeAxis::Y, type == LatticePreset::TWIST || type == LatticePreset::BEND ? 30.0f : 0.3f);
        const std::vector<Vector3> all = applied(3, 3, 3, params);
        const std::vector<Vector3> some = applied(3, 3, 3, params, selection);
        for (int i = 0; i < 27; i++){
            if (i == 0 || i == 4 || i == 26){
                EXPECT_TRUE(bitwiseEqual(some[size_t(i)], all[size_t(i)])) << lattice::presetName(type) << " " << i;
            }else{
                EXPECT_EQ(some[size_t(i)], Vector3(0, 0, 0)) << lattice::presetName(type) << " " << i;
            }
        }
    }
    // a wrongly sized offsets vector is resized first
    std::vector<Vector3> offsets(3, Vector3(1, 0, 0));
    lattice::applyPreset(3, 3, 3, offsets, preset(LatticePreset::SHEAR, LatticeAxis::Y, 0.2f));
    ASSERT_EQ(offsets.size(), size_t(27));
    EXPECT_EQ(offsets[0], Vector3(1, 0, 0));
    EXPECT_EQ(offsets[26], Vector3(0.2f, 0, 0));
}

// --- resample ---

TEST(LatticePresets, ResampleIdentityAndCoincidentPoints){
    LatticeComponent lattice = makeLattice(3, 4, 5);
    randomOffsets(lattice, 8, 0.3f);
    // same resolution: the same bits
    EXPECT_TRUE(sameBits(lattice::resample(3, 4, 5, lattice.offsets, 3, 4, 5), lattice.offsets));

    // 3 -> 5 along x and 4 -> 7 along y: the old points sit at even x, at every other y; exact
    const std::vector<Vector3> finer = lattice::resample(3, 4, 5, lattice.offsets, 5, 7, 5);
    ASSERT_EQ(finer.size(), size_t(5 * 7 * 5));
    for (int k = 0; k < 5; k++){
        for (int j = 0; j < 4; j++){
            for (int i = 0; i < 3; i++){
                EXPECT_TRUE(bitwiseEqual(finer[size_t(lattice::pointIndex(5, 7, 2 * i, 2 * j, k))],
                                         lattice.offsets[size_t(lattice::pointIndex(3, 4, i, j, k))]));
            }
        }
    }
    // in between: the trilinear midpoint
    const Vector3 mid = (lattice.offsets[size_t(lattice::pointIndex(3, 4, 0, 0, 0))] +
                         lattice.offsets[size_t(lattice::pointIndex(3, 4, 1, 0, 0))]) * 0.5f;
    EXPECT_LT(maxAbsDifference(finer[size_t(lattice::pointIndex(5, 7, 1, 0, 0))], mid), 1e-6f);

    // and back down: the round trip is exact
    EXPECT_TRUE(sameBits(lattice::resample(5, 7, 5, finer, 3, 4, 5), lattice.offsets));

    // the deformation is kept: a resampled sharp lattice deforms like the old one at its points
    LatticeComponent sharp = makeLattice(3, 4, 5, LatticeInterpolation::SHARP);
    sharp.offsets = lattice.offsets;
    LatticeComponent resampled = makeLattice(9, 2, 16, LatticeInterpolation::SHARP);
    resampled.offsets = lattice::resample(3, 4, 5, sharp.offsets, 9, 2, 16);
    std::vector<Vector3> oldPadded, newPadded;
    lattice::buildPaddedGrid(sharp, oldPadded);
    lattice::buildPaddedGrid(resampled, newPadded);
    for (int k = 0; k < 16; k++){
        for (int j = 0; j < 2; j++){
            for (int i = 0; i < 9; i++){
                const Vector3 u = lattice::restPosition(9, 2, 16, i, j, k) + Vector3(0.5f, 0.5f, 0.5f);
                EXPECT_LT(maxAbsDifference(lattice::evaluate(sharp, oldPadded, u, 1).delta,
                                           resampled.offsets[size_t(lattice::pointIndex(9, 2, i, j, k))]), 1e-6f);
            }
        }
    }

    // a linear field survives any resampling
    LatticeComponent linear = makeLattice(2, 3, 4);
    const Matrix3 g = columns3(Vector3(0.1f, 0.2f, 0), Vector3(0, -0.3f, 0.1f), Vector3(0.2f, 0, 0.1f));
    linearOffsets(linear, g, Vector3(0.05f, 0, 0));
    const std::vector<Vector3> linearResampled = lattice::resample(2, 3, 4, linear.offsets, 7, 4, 3);
    for (int k = 0; k < 3; k++){
        for (int j = 0; j < 4; j++){
            for (int i = 0; i < 7; i++){
                const Vector3 rest = lattice::restPosition(7, 4, 3, i, j, k);
                EXPECT_LT(maxAbsDifference(linearResampled[size_t(lattice::pointIndex(7, 4, i, j, k))], g * rest + Vector3(0.05f, 0, 0)), 1e-6f);
            }
        }
    }

    // a short vector counts its missing entries as zero
    const std::vector<Vector3> fromShort = lattice::resample(3, 3, 3, {Vector3(1, 0, 0)}, 3, 3, 3);
    ASSERT_EQ(fromShort.size(), size_t(27));
    EXPECT_EQ(fromShort[0], Vector3(1, 0, 0));
    EXPECT_EQ(fromShort[1], Vector3(0, 0, 0));
}

// --- mirror ---

TEST(LatticePresets, Mirror){
    LatticeComponent lattice = makeLattice(3, 2, 2);
    randomOffsets(lattice, 9, 0.3f);
    const std::vector<Vector3> original = lattice.offsets;

    std::vector<Vector3> offsets = original;
    lattice::mirror(3, 2, 2, offsets, LatticeAxis::X, true);
    for (int k = 0; k < 2; k++){
        for (int j = 0; j < 2; j++){
            const Vector3 source = original[size_t(lattice::pointIndex(3, 2, 2, j, k))];
            EXPECT_TRUE(bitwiseEqual(offsets[size_t(lattice::pointIndex(3, 2, 2, j, k))], source));      // source half kept
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, 2, 0, j, k))], Vector3(-source.x, source.y, source.z));
            const Vector3 middle = original[size_t(lattice::pointIndex(3, 2, 1, j, k))];
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, 2, 1, j, k))], Vector3(0, middle.y, middle.z));
        }
    }
    // the mirrored cage is symmetric: positions mirror across x = 0
    for (int k = 0; k < 2; k++){
        for (int j = 0; j < 2; j++){
            const Vector3 a = position(3, 2, 2, offsets, 0, j, k);
            const Vector3 b = position(3, 2, 2, offsets, 2, j, k);
            EXPECT_EQ(a, Vector3(-b.x, b.y, b.z));
        }
    }

    // from the negative half, along an even axis (no middle plane)
    offsets = original;
    lattice::mirror(3, 2, 2, offsets, LatticeAxis::Y, false);
    for (int k = 0; k < 2; k++){
        for (int i = 0; i < 3; i++){
            const Vector3 source = original[size_t(lattice::pointIndex(3, 2, i, 0, k))];
            EXPECT_TRUE(bitwiseEqual(offsets[size_t(lattice::pointIndex(3, 2, i, 0, k))], source));
            EXPECT_EQ(offsets[size_t(lattice::pointIndex(3, 2, i, 1, k))], Vector3(source.x, -source.y, source.z));
        }
    }

    // mirroring twice changes nothing more
    std::vector<Vector3> twice = offsets;
    lattice::mirror(3, 2, 2, twice, LatticeAxis::Y, false);
    EXPECT_TRUE(sameBits(twice, offsets));
}

// --- selection helpers ---

TEST(LatticePresets, SelectionHelpers){
    EXPECT_EQ(lattice::selectAll(2, 2, 2), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(lattice::selectAll(3, 3, 3).size(), size_t(27));

    const std::vector<int> top = lattice::selectFace(3, 3, 3, LatticeAxis::Y, true);
    EXPECT_EQ(top, (std::vector<int>{6, 7, 8, 15, 16, 17, 24, 25, 26}));
    EXPECT_EQ(lattice::selectFace(3, 3, 3, LatticeAxis::X, false), (std::vector<int>{0, 3, 6, 9, 12, 15, 18, 21, 24}));
    EXPECT_EQ(lattice::selectFace(2, 2, 3, LatticeAxis::Z, true), (std::vector<int>{8, 9, 10, 11}));

    EXPECT_EQ(lattice::selectExterior(3, 3, 3).size(), size_t(26));
    EXPECT_EQ(lattice::selectInterior(3, 3, 3), (std::vector<int>{13}));
    EXPECT_TRUE(lattice::selectInterior(2, 5, 5).empty());
    EXPECT_EQ(lattice::selectInterior(4, 4, 4).size(), size_t(8));

    // ranges: negative lo means any, hi is clamped, an empty range selects nothing
    EXPECT_EQ(lattice::selectRange(3, 3, 3, 1, 1, -1, 0, 0, 0), (std::vector<int>{1, 4, 7}));
    EXPECT_EQ(lattice::selectRange(3, 3, 3, -1, 0, -1, 0, -1, 0).size(), size_t(27));
    EXPECT_EQ(lattice::selectRange(3, 3, 3, 1, 99, 2, 2, 2, 2), (std::vector<int>{25, 26}));
    EXPECT_TRUE(lattice::selectRange(3, 3, 3, 2, 1, -1, 0, -1, 0).empty());
    EXPECT_TRUE(lattice::selectRange(3, 3, 3, 5, 6, -1, 0, -1, 0).empty());

    // expand / shrink on the 6-neighbourhood
    EXPECT_EQ(lattice::expandSelection(3, 3, 3, {13}), (std::vector<int>{4, 10, 12, 13, 14, 16, 22}));
    EXPECT_EQ(lattice::expandSelection(3, 3, 3, {0}), (std::vector<int>{0, 1, 3, 9}));
    EXPECT_TRUE(lattice::expandSelection(3, 3, 3, {}).empty());
    EXPECT_EQ(lattice::shrinkSelection(3, 3, 3, lattice::selectAll(3, 3, 3)).size(), size_t(27));
    EXPECT_EQ(lattice::shrinkSelection(3, 3, 3, lattice::expandSelection(3, 3, 3, {13})), (std::vector<int>{13}));
    EXPECT_TRUE(lattice::shrinkSelection(3, 3, 3, {13}).empty());
    // a 3x3x3 block inside 5x5x5 shrinks to its centre
    const std::vector<int> block = lattice::selectRange(5, 5, 5, 1, 3, 1, 3, 1, 3);
    EXPECT_EQ(lattice::shrinkSelection(5, 5, 5, block), (std::vector<int>{lattice::pointIndex(5, 5, 2, 2, 2)}));

    EXPECT_EQ(lattice::invertSelection(2, 2, 2, {1, 2, 3, 99, -1}), (std::vector<int>{0, 4, 5, 6, 7}));
    EXPECT_TRUE(lattice::invertSelection(2, 2, 2, lattice::selectAll(2, 2, 2)).empty());
    // input order and repeats do not matter, output is ascending
    EXPECT_EQ(lattice::expandSelection(2, 2, 2, {7, 7, 0}), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(LatticePresets, SelectInWorldBoxAndSphere){
    // a 2-unit lattice at (10, 0, 0): its points are at x = 9, 10, 11
    const Matrix4 model = affine(Matrix3() * 2.0f, Vector3(10, 0, 0));
    std::vector<Vector3> offsets(27, Vector3(0, 0, 0));
    EXPECT_EQ(lattice::selectInWorldBox(3, 3, 3, offsets, model, Vector3(10.5f, -2, -2), Vector3(12, 2, 2)),
              lattice::selectFace(3, 3, 3, LatticeAxis::X, true));
    // min and max may come swapped; the test is inclusive
    EXPECT_EQ(lattice::selectInWorldBox(3, 3, 3, offsets, model, Vector3(11, 2, 2), Vector3(10.5f, -2, -2)),
              lattice::selectFace(3, 3, 3, LatticeAxis::X, true));
    // current positions: the moved point leaves the box
    offsets[size_t(lattice::pointIndex(3, 3, 2, 1, 1))] = Vector3(-0.4f, 0, 0); // world x 10.2
    const std::vector<int> moved = lattice::selectInWorldBox(3, 3, 3, offsets, model, Vector3(10.5f, -2, -2), Vector3(12, 2, 2));
    EXPECT_EQ(moved.size(), size_t(8));
    EXPECT_EQ(std::count(moved.begin(), moved.end(), lattice::pointIndex(3, 3, 2, 1, 1)), 0);

    EXPECT_EQ(lattice::selectInWorldSphere(3, 3, 3, offsets, model, Vector3(10, 0, 0), 0.01f), (std::vector<int>{13}));
    EXPECT_EQ(lattice::selectInWorldSphere(3, 3, 3, offsets, model, Vector3(10, 0, 0), 1.0f).size(), size_t(7));
    // inclusive radius, the moved point counted where it is now
    EXPECT_EQ(lattice::selectInWorldSphere(3, 3, 3, offsets, model, Vector3(10.2f, 0, 0), 1e-4f),
              (std::vector<int>{lattice::pointIndex(3, 3, 2, 1, 1)}));
    EXPECT_TRUE(lattice::selectInWorldSphere(3, 3, 3, offsets, model, Vector3(10, 0, 0), -1.0f).empty());
}

// --- copyAsLua ---

TEST(LatticePresets, CopyAsLuaFormatting){
    std::vector<Vector3> offsets(27, Vector3(0, 0, 0));
    offsets[size_t(lattice::pointIndex(3, 3, 1, 2, 0))] = Vector3(0, 0.25f, 0);
    offsets[size_t(lattice::pointIndex(3, 3, 2, 0, 1))] = Vector3(-1.5f, 0.1f, 1e-7f);
    offsets[size_t(lattice::pointIndex(3, 3, 0, 0, 2))] = Vector3(-0.0f, 0.3333333432674408f, 2);
    EXPECT_EQ(lattice::copyAsLua("lattice", 3, 3, 3, offsets),
              "lattice:setOffset(1, 2, 0, Vector3(0, 0.25, 0))\n"
              "lattice:setOffset(2, 0, 1, Vector3(-1.5, 0.1, 1e-07))\n"
              "lattice:setOffset(0, 0, 2, Vector3(0, 0.33333334, 2))\n");
    // only the selected non-zero points
    EXPECT_EQ(lattice::copyAsLua("cage", 3, 3, 3, offsets, {lattice::pointIndex(3, 3, 1, 2, 0), 0, 5}),
              "cage:setOffset(1, 2, 0, Vector3(0, 0.25, 0))\n");
    EXPECT_EQ(lattice::copyAsLua("cage", 3, 3, 3, std::vector<Vector3>(27, Vector3(0, 0, 0))), "");
    // every printed number reads back as the same float
    Random random(4);
    std::vector<Vector3> randomOffsetsList(8);
    for (Vector3& v : randomOffsetsList) v = random.vector(-3, 3);
    const std::string text = lattice::copyAsLua("l", 2, 2, 2, randomOffsetsList);
    std::istringstream lines(text);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)){
        const size_t open = line.find("Vector3(");
        ASSERT_NE(open, std::string::npos);
        float x, y, z;
        ASSERT_EQ(std::sscanf(line.c_str() + open, "Vector3(%f, %f, %f)", &x, &y, &z), 3);
        EXPECT_TRUE(bitwiseEqual(Vector3(x, y, z), randomOffsetsList[size_t(count)]));
        count++;
    }
    EXPECT_EQ(count, 8);
}
