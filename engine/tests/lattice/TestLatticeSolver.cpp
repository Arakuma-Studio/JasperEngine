// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The lattice math (spec §3): indexing, padded grid, identity at rest, linear-field
// reproduction, analytic Jacobians against finite differences, normals, tangents, stretch,
// fade, masks, world-space helpers and folded-cell detection.

#include "LatticeTestCommon.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace{

    const LatticeOutside outsides[] = {LatticeOutside::EXTEND, LatticeOutside::FADE};

    bool isIdentity(const Matrix3& m){
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++){
                if (m.get(c, r) != (c == r ? 1.0f : 0.0f)) return false;
            }
        }
        return true;
    }

    // u' = u + delta(u) in double, for finite differences
    void deformed(const LatticeComponent& lattice, const std::vector<Vector3>& padded, const double* u, float weight, double* out){
        const Vector3 uf((float)u[0], (float)u[1], (float)u[2]);
        const lattice::Evaluation e = lattice::evaluate(lattice, padded, uf, weight);
        out[0] = u[0] + e.delta.x;
        out[1] = u[1] + e.delta.y;
        out[2] = u[2] + e.delta.z;
    }

    // central (or forward) differences of u + delta(u); column a = derivative along a
    Matrix3 finiteJacobian(const LatticeComponent& lattice, const std::vector<Vector3>& padded, const Vector3& u,
                           float weight, double h, bool forward = false){
        Matrix3 j;
        for (int a = 0; a < 3; a++){
            double plus[3] = {u.x, u.y, u.z};
            double minus[3] = {u.x, u.y, u.z};
            plus[a] += h;
            if (!forward) minus[a] -= h;
            double p[3], m[3];
            deformed(lattice, padded, plus, weight, p);
            deformed(lattice, padded, minus, weight, m);
            const double span = forward ? h : 2.0 * h;
            j.setColumn(a, Vector3(float((p[0] - m[0]) / span), float((p[1] - m[1]) / span), float((p[2] - m[2]) / span)));
        }
        return j;
    }

    // float steps of exactly representable sizes keep the stencil where it is meant to be
    const double FD_STEP = 1.0 / 1024.0;

    // a lattice placed in the world with rotation, non-uniform scale and translation
    Matrix4 placedLatticeModel(){
        const Matrix3 r = rotation(Vector3(1, 2, 3).normalized(), 0.7);
        const Matrix3 s = columns3(Vector3(2, 0, 0), Vector3(0, 0.5f, 0), Vector3(0, 0, 1.5f));
        return affine(r * s, Vector3(3, -1, 2));
    }

    Matrix4 placedObjectModel(){
        const Matrix3 r = rotation(Vector3(-2, 1, 0.5f).normalized(), -0.4);
        const Matrix3 s = columns3(Vector3(0.8f, 0, 0), Vector3(0, 1.3f, 0), Vector3(0, 0, 0.6f));
        return affine(r * s, Vector3(2.5f, -0.8f, 2.2f));
    }

    // a random normalized point whose parameter t is in [margin, 1 - margin] on every axis
    Vector3 pointInsideCell(Random& random, int rx, int ry, int rz, float margin){
        const int res[3] = {rx, ry, rz};
        float u[3];
        for (int a = 0; a < 3; a++){
            const int cell = std::min(int(random.uniform(0.0f, float(res[a] - 1))), res[a] - 2);
            u[a] = (float(cell) + random.uniform(margin, 1.0f - margin)) / float(res[a] - 1);
        }
        return Vector3(u[0], u[1], u[2]);
    }

    Vector3 cofactorApply(const Matrix3& j, const Vector3& n){
        const Vector3 j1 = j.column(0), j2 = j.column(1), j3 = j.column(2);
        return j2.crossProduct(j3) * n.x + j3.crossProduct(j1) * n.y + j1.crossProduct(j2) * n.z;
    }

}

// --- indexing ---

TEST(LatticeSolver, IndexingAndRestPositions){
    EXPECT_EQ(lattice::pointCount(3, 3, 3), 27);
    EXPECT_EQ(lattice::pointCount(2, 4, 5), 40);
    EXPECT_EQ(lattice::pointCount(1, 0, -3), 8);       // clamped to 2
    EXPECT_EQ(lattice::pointCount(20, 16, 17), 4096);  // clamped to 16

    EXPECT_EQ(lattice::pointIndex(3, 4, 0, 0, 0), 0);
    EXPECT_EQ(lattice::pointIndex(3, 4, 2, 0, 0), 2);
    EXPECT_EQ(lattice::pointIndex(3, 4, 0, 1, 0), 3);
    EXPECT_EQ(lattice::pointIndex(3, 4, 1, 2, 3), 1 + 3 * (2 + 4 * 3));

    EXPECT_EQ(lattice::restPosition(3, 3, 3, 0, 0, 0), Vector3(-0.5f, -0.5f, -0.5f));
    EXPECT_EQ(lattice::restPosition(3, 3, 3, 1, 1, 1), Vector3(0, 0, 0));
    EXPECT_EQ(lattice::restPosition(3, 3, 3, 2, 2, 2), Vector3(0.5f, 0.5f, 0.5f));
    EXPECT_EQ(lattice::restPosition(5, 2, 3, 1, 1, 0), Vector3(-0.25f, 0.5f, -0.5f));
}

// --- padded grid (§3.1) ---

TEST(LatticeSolver, PaddedGridInteriorAndGhosts){
    LatticeComponent lattice = makeLattice(3, 4, 2);
    randomOffsets(lattice, 7, 0.3f);
    lattice.dynamicOffsets.assign(lattice.offsets.size(), Vector3(0, 0, 0));
    lattice.dynamicOffsets[5] = Vector3(0.1f, 0.2f, 0.3f);

    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    const int rx = 3, ry = 4, rz = 2, px = 5, py = 6, pz = 4;
    ASSERT_EQ(padded.size(), size_t(px * py * pz));

    auto at = [&](int a, int b, int c){ return padded[size_t((a + 1) + px * ((b + 1) + py * (c + 1)))]; };
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            for (int i = 0; i < rx; i++){
                const size_t index = size_t(lattice::pointIndex(rx, ry, i, j, k));
                EXPECT_EQ(at(i, j, k), lattice.offsets[index] + lattice.dynamicOffsets[index]);
            }
        }
    }
    // x ghosts on interior rows, y ghosts on x-padded rows, z ghosts everywhere
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            EXPECT_EQ(at(-1, j, k), at(0, j, k) * 2.0f - at(1, j, k));
            EXPECT_EQ(at(rx, j, k), at(rx - 1, j, k) * 2.0f - at(rx - 2, j, k));
        }
        for (int i = -1; i <= rx; i++){
            EXPECT_EQ(at(i, -1, k), at(i, 0, k) * 2.0f - at(i, 1, k));
            EXPECT_EQ(at(i, ry, k), at(i, ry - 1, k) * 2.0f - at(i, ry - 2, k));
        }
    }
    for (int j = -1; j <= ry; j++){
        for (int i = -1; i <= rx; i++){
            EXPECT_EQ(at(i, j, -1), at(i, j, 0) * 2.0f - at(i, j, 1));
            EXPECT_EQ(at(i, j, rz), at(i, j, rz - 1) * 2.0f - at(i, j, rz - 2));
        }
    }

    EXPECT_FLOAT_EQ(lattice::maxPaddedOffset(padded), [&]{
        float m = 0;
        for (const Vector3& p : padded) m = std::max(m, p.length());
        return m;
    }());
    EXPECT_EQ(lattice::maxPaddedOffset({}), 0.0f);
}

TEST(LatticeSolver, PaddedGridMismatchedSizesAndReuse){
    std::vector<Vector3> offsets(5, Vector3(1, 2, 3)); // 27 expected
    std::vector<Vector3> dynamic(40, Vector3(0.5f, 0, 0)); // too long: the extra entries are ignored
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(3, 3, 3, offsets, &dynamic, padded);
    ASSERT_EQ(padded.size(), size_t(125));
    auto at = [&](int a, int b, int c){ return padded[size_t((a + 1) + 5 * ((b + 1) + 5 * (c + 1)))]; };
    EXPECT_EQ(at(0, 0, 0), Vector3(1.5f, 2, 3));
    EXPECT_EQ(at(1, 1, 0), Vector3(1.5f, 2, 3)); // index 4, the last one given
    EXPECT_EQ(at(2, 1, 0), Vector3(0.5f, 0, 0)); // index 5: missing offset counts as zero

    // the output vector is reused (no reallocation for the same size), and an empty or null
    // dynamic vector adds nothing
    const Vector3* data = padded.data();
    lattice::buildPaddedGrid(3, 3, 3, offsets, nullptr, padded);
    EXPECT_EQ(padded.data(), data);
    EXPECT_EQ(at(0, 0, 0), Vector3(1, 2, 3));
    std::vector<Vector3> empty;
    lattice::buildPaddedGrid(3, 3, 3, offsets, &empty, padded);
    EXPECT_EQ(at(0, 0, 0), Vector3(1, 2, 3));

    // out-of-range resolutions are clamped like everywhere else
    lattice::buildPaddedGrid(1, 30, 3, {}, nullptr, padded);
    EXPECT_EQ(padded.size(), size_t(4 * 18 * 5));
}

// --- identity at rest (§3.2-3.4) ---

TEST(LatticeSolver, IdentityAtRestForEveryModeAndOutside){
    const float fades[] = {0.25f, 0.0f, -1.0f, 2.0f};
    Random random(11);
    for (LatticeInterpolation interpolation : allInterpolations){
        for (LatticeOutside outside : outsides){
            for (float fade : fades){
                LatticeComponent lattice = makeLattice(4, 3, 5, interpolation, outside, fade);
                std::vector<Vector3> padded;
                lattice::buildPaddedGrid(lattice, padded);
                for (int n = 0; n < 200; n++){
                    // inside, outside and exactly on the faces
                    Vector3 u = random.vector(-1.0f, 2.0f);
                    if (n % 10 == 0) u.x = 0.0f;
                    if (n % 10 == 1) u.y = 1.0f;
                    const lattice::Evaluation e = lattice::evaluate(lattice, padded, u, 1.0f);
                    EXPECT_EQ(e.delta, Vector3(0, 0, 0)) << interpolationName(interpolation);
                    EXPECT_TRUE(isIdentity(e.jacobian)) << interpolationName(interpolation) << " " << e.jacobian.toString();
                    EXPECT_EQ(e.stretch, 1.0f);
                }

                // the world and vertex paths leave everything bit-identical
                const Matrix4 latticeModel = placedLatticeModel();
                const Matrix4 objectModel = placedObjectModel();
                lattice::ObjectSpaceLattice binding;
                binding.lattice = &lattice;
                binding.padded = &padded;
                binding.objectToNormalized = lattice::objectToNormalized(latticeModel, objectModel);
                binding.normalizedToObject = binding.objectToNormalized.inverse();
                for (int n = 0; n < 50; n++){
                    const Vector3 world = random.vector(-2.0f, 6.0f);
                    EXPECT_EQ(lattice::deformWorldPoint(lattice, padded, latticeModel, world), world);
                    EXPECT_TRUE(isIdentity(lattice::worldJacobian(lattice, padded, latticeModel, world)));

                    const Vector3 position = random.vector(-1.0f, 1.0f);
                    const Vector3 normal = random.vector(-1.0f, 1.0f);
                    const Vector3 tangent = random.vector(-1.0f, 1.0f);
                    Vector3 p = position, nrm = normal, tan = tangent;
                    float stretch = 1.0f;
                    lattice::deformVertex(binding, p, &nrm, &tan, &stretch, nullptr);
                    EXPECT_TRUE(bitwiseEqual(p, position));
                    EXPECT_TRUE(bitwiseEqual(nrm, normal));
                    EXPECT_TRUE(bitwiseEqual(tan, tangent));
                    EXPECT_EQ(stretch, 1.0f);
                }
            }
        }
    }
}

TEST(LatticeSolver, RestEvaluationForDisabledInvalidAndZeroWeight){
    LatticeComponent lattice = makeLattice(3, 3, 3, LatticeInterpolation::SHARP);
    randomOffsets(lattice, 3, 0.2f);
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    const Vector3 u(0.3f, 0.6f, 0.4f);
    ASSERT_NE(lattice::evaluate(lattice, padded, u, 1.0f).delta, Vector3(0, 0, 0));

    EXPECT_EQ(lattice::evaluate(lattice, padded, u, 0.0f).delta, Vector3(0, 0, 0));
    EXPECT_EQ(lattice::evaluate(lattice, padded, Vector3(std::nanf(""), 0.5f, 0.5f), 1.0f).delta, Vector3(0, 0, 0));

    // a padded grid that does not belong to this resolution is never read
    std::vector<Vector3> wrong(padded.begin(), padded.end() - 1);
    EXPECT_EQ(lattice::evaluate(lattice, wrong, u, 1.0f).delta, Vector3(0, 0, 0));
    EXPECT_EQ(lattice::evaluate(lattice, {}, u, 1.0f).delta, Vector3(0, 0, 0));

    lattice.enabled = false;
    const lattice::Evaluation e = lattice::evaluate(lattice, padded, u, 1.0f);
    EXPECT_EQ(e.delta, Vector3(0, 0, 0));
    EXPECT_TRUE(isIdentity(e.jacobian));
    EXPECT_EQ(e.stretch, 1.0f);
}

TEST(LatticeSolver, ClampedResolutionMatchesPaddedGrid){
    // resolution 1 behaves as 2 and 20 as 16, in the grid and in the evaluation
    LatticeComponent lattice = makeLattice(1, 20, 3, LatticeInterpolation::SHARP);
    ASSERT_EQ(lattice.offsets.size(), size_t(2 * 16 * 3));
    lattice.offsets[size_t(lattice::pointIndex(1, 20, 1, 15, 2))] = Vector3(0.1f, 0, 0);
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    const lattice::Evaluation e = lattice::evaluate(lattice, padded, Vector3(1, 1, 1), 1.0f);
    EXPECT_FLOAT_EQ(e.delta.x, 0.1f);
}

// --- linear fields (§3.1) ---

TEST(LatticeSolver, LinearFieldReproduction){
    const Matrix3 g = columns3(Vector3(0.2f, -0.1f, 0.05f), Vector3(0.1f, 0.3f, -0.2f), Vector3(-0.15f, 0.05f, -0.25f));
    const Vector3 c(0.05f, -0.02f, 0.1f);
    Random random(5);
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(4, 3, 5, interpolation, LatticeOutside::EXTEND);
        linearOffsets(lattice, g, c);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        const Matrix3 expectedJacobian = Matrix3() + g;

        if (interpolation == LatticeInterpolation::SMOOTH){
            // smoothstep weights are not linear in t: the field passes through the points
            // (with zero slope there), it is not reproduced between them
            for (int k = 0; k < 5; k++){
                for (int j = 0; j < 3; j++){
                    for (int i = 0; i < 4; i++){
                        const Vector3 rest = lattice::restPosition(4, 3, 5, i, j, k);
                        const lattice::Evaluation e = lattice::evaluate(lattice, padded, rest + Vector3(0.5f, 0.5f, 0.5f), 1.0f);
                        EXPECT_LT(maxAbsDifference(e.delta, g * rest + c), 1e-6f);
                    }
                }
            }
            continue;
        }
        // sharp and cubic reproduce it everywhere inside, borders included
        for (int n = 0; n < 300; n++){
            Vector3 u = random.vector(0.0f, 1.0f);
            if (n % 7 == 0) u.x = 0.0f;
            if (n % 7 == 1) u.z = 1.0f;
            if (n % 7 == 2) u.y = 0.5f;
            const lattice::Evaluation e = lattice::evaluate(lattice, padded, u, 1.0f);
            const Vector3 expected = g * (u - Vector3(0.5f, 0.5f, 0.5f)) + c;
            EXPECT_LT(maxAbsDifference(e.delta, expected), 2e-6f) << interpolationName(interpolation);
            EXPECT_LT(maxAbsDifference(e.jacobian, expectedJacobian), 2e-5f) << interpolationName(interpolation);
            EXPECT_NEAR(e.stretch, expectedJacobian.determinant(), 2e-5f);
        }
    }
}

// --- Jacobian against finite differences ---

TEST(LatticeSolver, JacobianMatchesFiniteDifferencesInside){
    Random random(21);
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(4, 5, 3, interpolation);
        randomOffsets(lattice, 99, 0.15f);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        const int res[3] = {4, 5, 3};
        for (int n = 0; n < 200; n++){
            // keep the stencil inside one cell for sharp (its derivative jumps between cells)
            float u[3];
            for (int a = 0; a < 3; a++){
                const int cell = int(random.uniform(0.0f, float(res[a] - 1) - 1e-3f));
                u[a] = (float(cell) + random.uniform(0.05f, 0.95f)) / float(res[a] - 1);
            }
            const Vector3 point(u[0], u[1], u[2]);
            const float weight = random.uniform(0.2f, 2.0f);
            const lattice::Evaluation e = lattice::evaluate(lattice, padded, point, weight);
            const Matrix3 fd = finiteJacobian(lattice, padded, point, weight, FD_STEP);
            EXPECT_LT(maxAbsDifference(e.jacobian, fd), 2e-3f) << interpolationName(interpolation) << " at " << point.toString();
            EXPECT_NEAR(e.stretch, e.jacobian.determinant(), 1e-6f);
        }
    }
}

TEST(LatticeSolver, JacobianMatchesFiniteDifferencesAtCellBorders){
    Random random(22);
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(5, 3, 5, interpolation);
        randomOffsets(lattice, 98, 0.15f);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        // node coordinates that are exact in float: 0.25, 0.5, 0.75 (rx = rz = 5), 0.5 (ry = 3)
        const float xs[] = {0.25f, 0.5f, 0.75f};
        for (float x : xs){
            for (float z : xs){
                const Vector3 point(x, random.uniform(0.1f, 0.9f), z);
                const lattice::Evaluation e = lattice::evaluate(lattice, padded, point, 1.0f);
                // smooth and cubic are C1 across borders; a sharp lattice's derivative there is the
                // one of the cell starting at the border (c = floor(g)), so a forward difference
                const bool forward = interpolation == LatticeInterpolation::SHARP;
                // a smaller step: smooth's second derivative jumps at the node, so central
                // differences are only first-order accurate across it
                const Matrix3 fd = finiteJacobian(lattice, padded, point, 1.0f, 1.0 / 8192.0, forward);
                EXPECT_LT(maxAbsDifference(e.jacobian, fd), 2e-3f) << interpolationName(interpolation) << " at " << point.toString();
            }
        }
    }
}

TEST(LatticeSolver, JacobianMatchesFiniteDifferencesOutside){
    Random random(23);
    for (LatticeInterpolation interpolation : allInterpolations){
        for (LatticeOutside outside : outsides){
            LatticeComponent lattice = makeLattice(3, 4, 3, interpolation, outside, 0.4f);
            randomOffsets(lattice, 97, 0.15f);
            std::vector<Vector3> padded;
            lattice::buildPaddedGrid(lattice, padded);
            int checked = 0;
            for (int n = 0; n < 400 && checked < 120; n++){
                const Vector3 u = random.vector(-0.45f, 1.45f);
                // away from the faces (the derivative factor drops to 0 there) and, for sharp,
                // from the cell borders, by more than the stencil
                const Vector3 uc(std::min(std::max(u.x, 0.0f), 1.0f), std::min(std::max(u.y, 0.0f), 1.0f),
                                 std::min(std::max(u.z, 0.0f), 1.0f));
                const float d = (u - uc).length();
                if (d < 0.01f) continue;
                if (outside == LatticeOutside::FADE && std::fabs(d - 0.4f) < 0.01f) continue;
                bool nearFace = false;
                const int res[3] = {3, 4, 3};
                for (int a = 0; a < 3; a++){
                    if (std::fabs(u[a]) < 0.01f || std::fabs(u[a] - 1.0f) < 0.01f) nearFace = true;
                    if (interpolation == LatticeInterpolation::SHARP && u[a] > 0 && u[a] < 1){
                        const float g = u[a] * float(res[a] - 1);
                        if (std::fabs(g - std::round(g)) < 0.02f) nearFace = true;
                    }
                }
                if (nearFace) continue;
                checked++;
                const lattice::Evaluation e = lattice::evaluate(lattice, padded, u, 1.0f);
                const Matrix3 fd = finiteJacobian(lattice, padded, u, 1.0f, FD_STEP);
                EXPECT_LT(maxAbsDifference(e.jacobian, fd), 3e-3f)
                    << interpolationName(interpolation) << (outside == LatticeOutside::FADE ? " fade" : " extend") << " at " << u.toString();
            }
            EXPECT_GE(checked, 100);
        }
    }
}

// --- outside the cage (§3.3) ---

TEST(LatticeSolver, FadeIsContinuousAndEndsAtFadeDistance){
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(3, 3, 3, interpolation, LatticeOutside::FADE, 0.25f);
        randomOffsets(lattice, 31, 0.2f);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);

        // along a diagonal leaving the +x+y edge region
        const Vector3 start(1.0f, 1.0f, 0.4f);
        const Vector3 dir = Vector3(1, 1, 0).normalized();
        const Vector3 border = lattice::evaluate(lattice, padded, start, 1.0f).delta;
        Vector3 previous = border;
        float maxJump = 0;
        for (int n = 1; n <= 300; n++){
            const float d = 0.3f * float(n) / 300.0f;
            const Vector3 delta = lattice::evaluate(lattice, padded, start + dir * d, 1.0f).delta;
            maxJump = std::max(maxJump, maxAbsDifference(delta, previous));
            previous = delta;
            // exactly f * D(border) (D is clamped outside, so constant along this ray)
            const float q = std::min(d / 0.25f, 1.0f);
            const float f = 1.0f - q * q * (3.0f - 2.0f * q);
            EXPECT_LT(maxAbsDifference(delta, border * f), 1e-6f);
            if (d >= 0.25f){
                EXPECT_EQ(delta, Vector3(0, 0, 0));
            }
        }
        EXPECT_LT(maxJump, 0.01f);
        // continuous at the face itself
        const Vector3 justOutside = lattice::evaluate(lattice, padded, start + dir * 1e-5f, 1.0f).delta;
        EXPECT_LT(maxAbsDifference(justOutside, border), 1e-5f);
    }
}

TEST(LatticeSolver, FadeDistanceZeroAndExtend){
    LatticeComponent lattice = makeLattice(3, 3, 3, LatticeInterpolation::CUBIC, LatticeOutside::FADE, 0.0f);
    randomOffsets(lattice, 32, 0.2f);
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    EXPECT_NE(lattice::evaluate(lattice, padded, Vector3(1.0f, 0.5f, 0.5f), 1.0f).delta, Vector3(0, 0, 0));
    EXPECT_EQ(lattice::evaluate(lattice, padded, Vector3(1.0001f, 0.5f, 0.5f), 1.0f).delta, Vector3(0, 0, 0));
    lattice.fadeDistance = -1.0f;
    EXPECT_EQ(lattice::evaluate(lattice, padded, Vector3(0.5f, -0.01f, 0.5f), 1.0f).delta, Vector3(0, 0, 0));

    // extend: the border value continues unchanged, with no derivative across the clamped axis
    lattice.outside = LatticeOutside::EXTEND;
    const lattice::Evaluation border = lattice::evaluate(lattice, padded, Vector3(1.0f, 0.3f, 0.6f), 1.0f);
    const lattice::Evaluation far = lattice::evaluate(lattice, padded, Vector3(7.0f, 0.3f, 0.6f), 1.0f);
    EXPECT_EQ(far.delta, border.delta);
    EXPECT_EQ(far.jacobian.column(0), Vector3(1, 0, 0));
    EXPECT_EQ(far.jacobian.column(1), border.jacobian.column(1));
}

// --- world space ---

TEST(LatticeSolver, WorldSpaceMapping){
    // a lattice of size 2 at (5, 0, 0): the world point (6, 0, 0) is on its +x face
    const Matrix4 latticeModel = affine(Matrix3() * 2.0f, Vector3(5, 0, 0));
    const Matrix4 toNormalized = lattice::objectToNormalized(latticeModel, Matrix4());
    EXPECT_LT(maxAbsDifference(toNormalized * Vector3(6, 0, 0), Vector3(1.0f, 0.5f, 0.5f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(toNormalized * Vector3(4, -1, 1), Vector3(0.0f, 0.0f, 1.0f)), 1e-6f);

    // the object's own model matrix comes first
    const Matrix4 objectModel = Matrix4::translateMatrix(1, 0, 0);
    EXPECT_LT(maxAbsDifference(lattice::objectToNormalized(latticeModel, objectModel) * Vector3(5, 0, 0),
                               Vector3(1.0f, 0.5f, 0.5f)), 1e-6f);

    // the +x face moved by 0.1 local units moves (6, 0, 0) by 0.2 world units
    LatticeComponent lattice = makeLattice(2, 2, 2, LatticeInterpolation::SHARP);
    for (int index : lattice::selectFace(2, 2, 2, lattice::LatticeAxis::X, true)){
        lattice.offsets[size_t(index)] = Vector3(0.1f, 0, 0);
    }
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    EXPECT_LT(maxAbsDifference(lattice::deformWorldPoint(lattice, padded, latticeModel, Vector3(6, 0, 0)), Vector3(6.2f, 0, 0)), 1e-6f);
    EXPECT_LT(maxAbsDifference(lattice::deformWorldPoint(lattice, padded, latticeModel, Vector3(4, 0, 0)), Vector3(4, 0, 0)), 1e-6f);
    EXPECT_LT(maxAbsDifference(lattice::deformWorldPoint(lattice, padded, latticeModel, Vector3(5, 0, 0), 0.5f), Vector3(5.05f, 0, 0)), 1e-6f);
    // stretch 1.1 along x in both spaces
    EXPECT_LT(maxAbsDifference(lattice::worldJacobian(lattice, padded, latticeModel, Vector3(5, 0.3f, 0)),
                               columns3(Vector3(1.1f, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1))), 1e-6f);
}

TEST(LatticeSolver, WorldJacobianMatchesFiniteDifferences){
    const Matrix4 latticeModel = placedLatticeModel();
    const Matrix4 inverse = latticeModel.inverse();
    Random random(41);
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(4, 3, 4, interpolation, LatticeOutside::FADE, 0.3f);
        randomOffsets(lattice, 77, 0.15f);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        for (int n = 0; n < 100; n++){
            // inside cells (away from sharp borders)
            float u[3];
            const int res[3] = {4, 3, 4};
            for (int a = 0; a < 3; a++){
                const int cell = int(random.uniform(0.0f, float(res[a] - 1) - 1e-3f));
                u[a] = (float(cell) + random.uniform(0.1f, 0.9f)) / float(res[a] - 1);
            }
            const Vector3 world = latticeModel * (Vector3(u[0], u[1], u[2]) - Vector3(0.5f, 0.5f, 0.5f));
            const float weight = 1.5f;
            const Matrix3 analytic = lattice::worldJacobian(lattice, padded, latticeModel, world, weight);

            // the definition: Lm * J * Lm^-1
            const lattice::Evaluation e = lattice::evaluate(lattice, padded, inverse * world + Vector3(0.5f, 0.5f, 0.5f), weight);
            EXPECT_LT(maxAbsDifference(analytic, latticeModel.linear() * e.jacobian * inverse.linear()), 1e-4f);

            Matrix3 fd;
            const float h = 1.0f / 512.0f;
            for (int a = 0; a < 3; a++){
                Vector3 step(0, 0, 0);
                step[a] = h;
                const Vector3 plus = lattice::deformWorldPoint(lattice, padded, latticeModel, world + step, weight);
                const Vector3 minus = lattice::deformWorldPoint(lattice, padded, latticeModel, world - step, weight);
                fd.setColumn(a, (plus - minus) / (2.0f * h));
            }
            EXPECT_LT(maxAbsDifference(analytic, fd), 5e-3f) << interpolationName(interpolation);
            EXPECT_NEAR(analytic.determinant(), e.stretch, 1e-3f);

            // deformWorldPoint is latticeModel * (u + delta - 0.5)
            const Vector3 expected = latticeModel * (inverse * world + e.delta);
            EXPECT_LT(maxAbsDifference(lattice::deformWorldPoint(lattice, padded, latticeModel, world, weight), expected), 1e-4f);
        }
    }
}

TEST(LatticeSolver, NonInvertibleLatticeModel){
    LatticeComponent lattice = makeLattice(3, 3, 3);
    randomOffsets(lattice, 1, 0.2f);
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    const Matrix4 flat = Matrix4::scaleMatrix(Vector3(1, 0, 1));
    const Vector3 p(0.1f, 0.2f, 0.3f);
    EXPECT_EQ(lattice::deformWorldPoint(lattice, padded, flat, p), p);
    EXPECT_TRUE(isIdentity(lattice::worldJacobian(lattice, padded, flat, p)));
    EXPECT_FALSE(lattice::objectToNormalized(flat, Matrix4()).isValid());
    EXPECT_TRUE(lattice::objectToNormalized(Matrix4(), Matrix4()).isValid());
}

// --- vertices: normals, tangents, stretch, masks (§3.4) ---

TEST(LatticeSolver, NormalsAndTangentsForAKnownShear){
    // x' = x + k y in every space (identity matrices): planes y = c keep their normal, planes
    // x = c get normal (1, -k, 0); a tangent along y becomes (k, 1, 0)
    const float k = 0.5f;
    for (LatticeInterpolation interpolation : {LatticeInterpolation::SHARP, LatticeInterpolation::CUBIC}){
        LatticeComponent lattice = makeLattice(3, 3, 3, interpolation);
        linearOffsets(lattice, columns3(Vector3(0, 0, 0), Vector3(k, 0, 0), Vector3(0, 0, 0)), Vector3(0, 0, 0));
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        lattice::ObjectSpaceLattice binding;
        binding.lattice = &lattice;
        binding.padded = &padded;
        binding.objectToNormalized = lattice::objectToNormalized(Matrix4(), Matrix4());
        binding.normalizedToObject = binding.objectToNormalized.inverse();

        Vector3 position(0.1f, 0.2f, -0.3f);
        Vector3 normalY(0, 1, 0), normalX(1, 0, 0), tangent(0, 1, 0);
        float stretch = 1.0f;
        lattice::deformVertex(binding, position, &normalY, &tangent, &stretch, nullptr);
        Vector3 unused = Vector3(0.1f, 0.2f, -0.3f);
        lattice::deformVertex(binding, unused, &normalX, nullptr, nullptr, nullptr);

        EXPECT_LT(maxAbsDifference(position, Vector3(0.1f + k * 0.2f, 0.2f, -0.3f)), 1e-6f);
        EXPECT_LT(maxAbsDifference(normalY, Vector3(0, 1, 0)), 1e-6f);
        EXPECT_LT(maxAbsDifference(normalX, Vector3(1, -k, 0).normalized()), 1e-6f);
        EXPECT_LT(maxAbsDifference(tangent, Vector3(k, 1, 0).normalized()), 1e-6f);
        EXPECT_NEAR(stretch, 1.0f, 1e-6f); // a shear keeps the volume
    }
}

TEST(LatticeSolver, NormalsAndTangentsForAKnownRotation){
    // offsets (R - I) * rest rotate the whole cage: normals and tangents rotate with it, even
    // through placed lattice and object matrices
    const Matrix3 r = rotation(Vector3(0, 0, 1), 0.5);
    for (LatticeInterpolation interpolation : {LatticeInterpolation::SHARP, LatticeInterpolation::CUBIC}){
        LatticeComponent lattice = makeLattice(3, 3, 3, interpolation);
        linearOffsets(lattice, r - Matrix3(), Vector3(0, 0, 0));
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);

        // the lattice at the origin, scaled uniformly, the object placed inside it
        const Matrix4 latticeModel = affine(Matrix3() * 4.0f, Vector3(0, 0, 0));
        const Matrix4 objectModel = affine(rotation(Vector3(1, 0, 0), 0.3) * 0.7f, Vector3(0.2f, -0.1f, 0.3f));
        lattice::ObjectSpaceLattice binding;
        binding.lattice = &lattice;
        binding.padded = &padded;
        binding.objectToNormalized = lattice::objectToNormalized(latticeModel, objectModel);
        binding.normalizedToObject = binding.objectToNormalized.inverse();

        // in world space everything rotates by r around the lattice centre (the origin)
        const Matrix3 objectLinear = objectModel.linear();
        const Matrix3 objectInverse = objectModel.inverse().linear();
        const Vector3 normal = Vector3(0.3f, -0.5f, 0.8f).normalized();
        const Vector3 tangent = Vector3(0.6f, 0.2f, -0.1f).normalized();
        Vector3 p(0.1f, 0.2f, 0.3f), n = normal, t = tangent;
        float stretch = 1.0f;
        lattice::deformVertex(binding, p, &n, &t, &stretch, nullptr);

        const Vector3 world = objectModel * Vector3(0.1f, 0.2f, 0.3f);
        EXPECT_LT(maxAbsDifference(objectModel * p, r * world), 2e-5f) << interpolationName(interpolation);
        // world normal: inverse-transpose of the object matrix (uniform scale: the matrix itself)
        const Vector3 worldNormal = (objectInverse.transpose() * normal).normalized();
        const Vector3 expectedNormal = (objectLinear.transpose() * (r * worldNormal)).normalized();
        EXPECT_LT(maxAbsDifference(n, expectedNormal), 2e-5f);
        const Vector3 expectedTangent = (objectInverse * (r * (objectLinear * tangent))).normalized();
        EXPECT_LT(maxAbsDifference(t, expectedTangent), 2e-5f);
        EXPECT_NEAR(stretch, 1.0f, 2e-5f);
    }
}

TEST(LatticeSolver, DeformVertexMatchesObjectSpaceFiniteDifferences){
    // general case: random offsets, placed lattice and object, weight; the object-space map
    // p -> p' differentiated numerically gives Jo; normals follow cof(Jo), tangents Jo,
    // stretch is det(Jo)
    const Matrix4 latticeModel = placedLatticeModel();
    const Matrix4 objectModel = placedObjectModel();
    Random random(51);
    for (LatticeInterpolation interpolation : {LatticeInterpolation::SMOOTH, LatticeInterpolation::CUBIC}){
        LatticeComponent lattice = makeLattice(4, 4, 3, interpolation, LatticeOutside::FADE, 0.3f);
        randomOffsets(lattice, 61, 0.12f);
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(lattice, padded);
        lattice::ObjectSpaceLattice binding;
        binding.lattice = &lattice;
        binding.padded = &padded;
        binding.weight = 0.8f;
        binding.objectToNormalized = lattice::objectToNormalized(latticeModel, objectModel);
        binding.normalizedToObject = binding.objectToNormalized.inverse();
        ASSERT_TRUE(binding.objectToNormalized.isValid());

        auto deform = [&](const Vector3& p){
            Vector3 q = p;
            lattice::deformVertex(binding, q, nullptr, nullptr, nullptr, nullptr);
            return q;
        };

        for (int n = 0; n < 100; n++){
            // object-space points inside cells, away from the cell borders (smooth's second
            // derivative jumps there, which central differences do not like)
            const Vector3 p = binding.normalizedToObject * pointInsideCell(random, 4, 4, 3, 0.15f);
            const Vector3 u = binding.objectToNormalized * p;

            Matrix3 jo;
            const float h = 1.0f / 1024.0f;
            for (int a = 0; a < 3; a++){
                Vector3 step(0, 0, 0);
                step[a] = h;
                jo.setColumn(a, (deform(p + step) - deform(p - step)) / (2.0f * h));
            }

            const Vector3 normal = random.vector(-1, 1).normalized();
            const Vector3 tangent = random.vector(-1, 1).normalized();
            Vector3 q = p, nrm = normal, tan = tangent;
            float stretch = 2.0f; // multiplied, not replaced
            lattice::deformVertex(binding, q, &nrm, &tan, &stretch, nullptr);

            // the spec's position formula: Minv * (M p + delta)
            const lattice::Evaluation e = lattice::evaluate(lattice, padded, u, binding.weight);
            EXPECT_LT(maxAbsDifference(q, binding.normalizedToObject * (u + e.delta)), 2e-5f);

            EXPECT_LT(maxAbsDifference(nrm, cofactorApply(jo, normal).normalized()), 3e-3f) << interpolationName(interpolation);
            EXPECT_LT(maxAbsDifference(tan, (jo * tangent).normalized()), 3e-3f);
            EXPECT_NEAR(stretch, 2.0f * jo.determinant(), 6e-3f);
            EXPECT_NEAR(stretch, 2.0f * e.stretch, 1e-5f);
            EXPECT_NEAR(nrm.length(), 1.0f, 1e-5f);
            EXPECT_NEAR(tan.length(), 1.0f, 1e-5f);
        }
    }
}

TEST(LatticeSolver, VertexColorMaskAndWeight){
    LatticeComponent lattice = makeLattice(3, 3, 3, LatticeInterpolation::SHARP);
    randomOffsets(lattice, 71, 0.2f);
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(lattice, padded);
    lattice::ObjectSpaceLattice binding;
    binding.lattice = &lattice;
    binding.padded = &padded;
    binding.objectToNormalized = lattice::objectToNormalized(Matrix4(), Matrix4());
    binding.normalizedToObject = binding.objectToNormalized.inverse();

    const Vector3 rest(0.1f, -0.2f, 0.15f);
    const Vector3 u = rest + Vector3(0.5f, 0.5f, 0.5f);
    auto moved = [&](float weight, LatticeMaskChannel mask, const Vector4* color){
        binding.weight = weight;
        binding.mask = mask;
        Vector3 p = rest;
        float stretch = 1.0f;
        lattice::deformVertex(binding, p, nullptr, nullptr, &stretch, color);
        return p;
    };
    auto expected = [&](float w){ return rest + lattice::evaluate(lattice, padded, u, w).delta; };

    const Vector4 color(0.25f, 0.5f, 0.75f, 0.1f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::NONE, &color), expected(1)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::RED, &color), expected(0.25f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::GREEN, &color), expected(0.5f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::BLUE, &color), expected(0.75f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::ALPHA, &color), expected(0.1f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_RED, &color), expected(0.75f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_GREEN, &color), expected(0.5f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_BLUE, &color), expected(0.25f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_ALPHA, &color), expected(0.9f)), 1e-6f);
    // weight times mask
    EXPECT_LT(maxAbsDifference(moved(2, LatticeMaskChannel::BLUE, &color), expected(1.5f)), 1e-6f);
    // float colours outside [0, 1] are clamped (the culling bound assumes a mask <= 1)
    const Vector4 hdr(3.0f, -0.5f, 1.5f, -2.0f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::RED, &hdr), expected(1.0f)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::GREEN, &hdr), rest), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_BLUE, &hdr), rest), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_ALPHA, &hdr), expected(1.0f)), 1e-6f);
    // no colour: mask 1, for the inverted channels too
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::RED, nullptr), expected(1)), 1e-6f);
    EXPECT_LT(maxAbsDifference(moved(1, LatticeMaskChannel::INVERTED_RED, nullptr), expected(1)), 1e-6f);
    // mask 0: untouched
    const Vector4 black(0, 0, 0, 1);
    EXPECT_TRUE(bitwiseEqual(moved(1, LatticeMaskChannel::RED, &black), rest));
    EXPECT_TRUE(bitwiseEqual(moved(1, LatticeMaskChannel::INVERTED_ALPHA, &black), rest));
    // weight 2 doubles the displacement
    EXPECT_LT(maxAbsDifference(moved(2, LatticeMaskChannel::NONE, nullptr) - rest, (expected(1) - rest) * 2.0f), 1e-6f);

    // a binding without lattice or grid does nothing
    binding.weight = 1;
    binding.mask = LatticeMaskChannel::NONE;
    binding.padded = nullptr;
    Vector3 p = rest;
    lattice::deformVertex(binding, p, nullptr, nullptr, nullptr, nullptr);
    EXPECT_TRUE(bitwiseEqual(p, rest));
}

TEST(LatticeSolver, StackedStretchMultiplies){
    LatticeComponent a = makeLattice(2, 2, 2, LatticeInterpolation::SHARP);
    LatticeComponent b = makeLattice(2, 2, 2, LatticeInterpolation::SHARP);
    // a scales x by 1.5, b scales y by 2 (linear fields)
    linearOffsets(a, columns3(Vector3(0.5f, 0, 0), Vector3(0, 0, 0), Vector3(0, 0, 0)), Vector3(0, 0, 0));
    linearOffsets(b, columns3(Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 0)), Vector3(0, 0, 0));
    std::vector<Vector3> pa, pb;
    lattice::buildPaddedGrid(a, pa);
    lattice::buildPaddedGrid(b, pb);
    lattice::ObjectSpaceLattice ba, bb;
    ba.lattice = &a; ba.padded = &pa;
    bb.lattice = &b; bb.padded = &pb;
    // b is twice as large, so the point stays inside it after a moves it
    ba.objectToNormalized = lattice::objectToNormalized(Matrix4(), Matrix4());
    bb.objectToNormalized = lattice::objectToNormalized(Matrix4::scaleMatrix(2.0f), Matrix4());
    ba.normalizedToObject = ba.objectToNormalized.inverse();
    bb.normalizedToObject = bb.objectToNormalized.inverse();
    Vector3 p(0.2f, 0.1f, 0);
    float stretch = 1.0f;
    lattice::deformVertex(ba, p, nullptr, nullptr, &stretch, nullptr);
    lattice::deformVertex(bb, p, nullptr, nullptr, &stretch, nullptr);
    EXPECT_NEAR(stretch, 3.0f, 1e-5f);
    EXPECT_LT(maxAbsDifference(p, Vector3(0.3f, 0.2f, 0)), 1e-6f);
}

// --- folded cells ---

TEST(LatticeSolver, FoldedCellDetection){
    for (LatticeInterpolation interpolation : allInterpolations){
        LatticeComponent lattice = makeLattice(3, 3, 3, interpolation);
        EXPECT_TRUE(lattice::findFoldedCells(lattice).empty());
        randomOffsets(lattice, 81, 0.05f); // small: no fold
        EXPECT_TRUE(lattice::findFoldedCells(lattice).empty()) << interpolationName(interpolation);
    }

    // the centre point pushed past its -x neighbour folds the four cells between them
    LatticeComponent lattice = makeLattice(3, 3, 3, LatticeInterpolation::SHARP);
    lattice.offsets[size_t(lattice::pointIndex(3, 3, 1, 1, 1))] = Vector3(-0.6f, 0, 0);
    std::vector<std::array<int, 3>> folded = lattice::findFoldedCells(lattice);
    const std::vector<std::array<int, 3>> expected = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}};
    EXPECT_EQ(folded, expected);

    // a disabled lattice still reports its shape
    lattice.enabled = false;
    EXPECT_EQ(lattice::findFoldedCells(lattice), expected);
    lattice.enabled = true;

    // cubic smooths the same fold but still reports cells next to it, and only those
    lattice.interpolation = LatticeInterpolation::CUBIC;
    lattice.offsets[size_t(lattice::pointIndex(3, 3, 1, 1, 1))] = Vector3(-1.5f, 0, 0);
    folded = lattice::findFoldedCells(lattice);
    EXPECT_FALSE(folded.empty());
    for (const auto& cell : folded){
        EXPECT_EQ(cell[0], 0);
    }

    // a flattened cage (every x offset -rest.x) has det 0 everywhere: <= 1e-6 counts as folded
    LatticeComponent flat = makeLattice(3, 2, 2, LatticeInterpolation::SMOOTH);
    linearOffsets(flat, columns3(Vector3(-1, 0, 0), Vector3(0, 0, 0), Vector3(0, 0, 0)), Vector3(0, 0, 0));
    EXPECT_EQ(lattice::findFoldedCells(flat).size(), size_t(2));

    // the dynamic offsets count (what is rendered)
    LatticeComponent jiggling = makeLattice(3, 3, 3, LatticeInterpolation::SHARP);
    jiggling.dynamicOffsets.assign(27, Vector3(0, 0, 0));
    jiggling.dynamicOffsets[size_t(lattice::pointIndex(3, 3, 1, 1, 1))] = Vector3(-0.6f, 0, 0);
    EXPECT_EQ(lattice::findFoldedCells(jiggling), expected);
}
