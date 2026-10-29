// The runner injects the baseline production vector transform methods as an
// independent reference, then links the actual Matrix4/Vector/AABB sources.
// Keep fixture assertions enabled while production math compiles with the
// Release -DNDEBUG setting in its separate translation units.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <random>
#include <vector>
#include "math/Matrix4.h"
#include "math/AABB.h"
using namespace jasperengine;
struct BaselineMatrix4 {
    float matrix[4][4];
    explicit BaselineMatrix4(const Matrix4& m) {
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) matrix[c][r] = m.get(c,r);
    }
    float get(int c, int r) const { return matrix[c][r]; }
    Vector3 operator*(const Vector3&) const;
    Vector4 operator*(const Vector4&) const;
};
// BASELINE_METHODS

static bool same(float a, float b) {
    // All ordinary results, including signed zero, must match bit for bit.
    // NaN payload selection is not part of the engine math API.
    return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, sizeof(float)) == 0;
}
static void check(const Matrix4& m, const Vector3& v3, const Vector4& v4) {
    const BaselineMatrix4 reference(m);
    const Vector3 a = m * v3, b = reference * v3;
    const Vector4 c = m * v4, d = reference * v4;
    assert(same(a.x,b.x) && same(a.y,b.y) && same(a.z,b.z));
    assert(same(c.x,d.x) && same(c.y,d.y) && same(c.z,d.z) && same(c.w,d.w));
}
static void checkBox(const Matrix4& m, const AABB& box) {
    AABB expected(box);
    if (box.isFinite()) {
        const BaselineMatrix4 reference(m);
        const Vector3 lo = box.getMinimum(), hi = box.getMaximum();
        expected.setNull();
        // The same corner order as AABB::transform, including its first-corner
        // initialization (preserves tied/signed-zero extrema).
        for (const Vector3& corner : {lo, Vector3(lo.x,lo.y,hi.z), Vector3(lo.x,hi.y,hi.z),
                Vector3(lo.x,hi.y,lo.z), Vector3(hi.x,hi.y,lo.z), hi,
                Vector3(hi.x,lo.y,hi.z), Vector3(hi.x,lo.y,lo.z)})
            expected.merge(reference * corner);
    }
    const AABB actual = m * box;
    assert(actual.isNull() == expected.isNull());
    assert(actual.isInfinite() == expected.isInfinite());
    if (actual.isFinite()) {
        for (unsigned axis=0;axis<3;++axis) {
            assert(same(actual.getMinimum()[axis], expected.getMinimum()[axis]));
            assert(same(actual.getMaximum()[axis], expected.getMaximum()[axis]));
        }
    }
}
static void checks() {
    std::mt19937 random(0x5eed);
    std::uniform_real_distribution<float> value(-100,100);
    for (unsigned n = 0; n < 20000; ++n) {
        Matrix4 m;
        for (unsigned c=0;c<4;++c)
            for (unsigned r=0;r<4;++r) m[c][r] = value(random);
        if ((n % 3) == 0) { // affine, including negative scale/shear
            m[0][3] = m[1][3] = m[2][3] = 0; m[3][3] = 1;
        }
        check(m, Vector3(value(random),value(random),value(random)),
            Vector4(value(random),value(random),value(random),value(random)));
        // Affine bounds include rotations, reflection, shear and zero scale.
        // Projective vector behavior is checked above; a projective box that
        // crosses w=0 is outside the finite-box transform's preconditions.
        if ((n % 3) == 0) {
            const Vector3 lo(value(random),value(random),value(random));
            const Vector3 size(std::abs(value(random)),std::abs(value(random)),std::abs(value(random)));
            AABB box(lo, lo + size);
            if (n % 9 == 0) box.getCorners(); // populated cached-corner ownership
            checkBox(m, box);
            checkBox(m, AABB(lo,lo)); // degenerate bounds
        }
    }
    const float special[] = {0.f, -0.f, 1.f, -1.f, std::numeric_limits<float>::denorm_min(),
        std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()};
    for (float coefficient : special) for (float position : special) {
        Matrix4 m;
        for (unsigned c=0;c<4;++c) for (unsigned r=0;r<4;++r)
            m[c][r] = ((c+r)%3) ? coefficient : -coefficient;
        check(m, Vector3(position,-position,position), Vector4(position,-position,position,-position));
    }
    checkBox(Matrix4(), AABB());
    checkBox(Matrix4(), AABB(AABB::BOXTYPE_INFINITE));
    checkBox(Matrix4(), AABB(Vector3(-0.f,-0.f,-0.f),Vector3(0.f,0.f,0.f)));
    std::fprintf(stderr,"matrix/vector and AABB equivalence: 20,000 random affine/projective, 100 special float and 13,337 box cases passed\n");
}
static volatile float consumed;
int main(int argc, char** argv) {
    checks();
#ifdef CHECK_ONLY
    return 0;
#endif
    constexpr unsigned count = 4096;
    std::vector<Matrix4> matrices(count);
    std::vector<Vector3> vectors3(count), results3(count);
    std::vector<Vector4> vectors4(count), results4(count);
    std::vector<AABB> boxes(count), resultsBox(count);
    std::mt19937 random(42);
    std::uniform_real_distribution<float> value(-10,10);
    for (unsigned i=0;i<count;++i) {
        Matrix4& m = matrices[i];
        for (unsigned c=0;c<4;++c) for (unsigned r=0;r<3;++r) m[c][r]=value(random);
        vectors3[i] = Vector3(value(random),value(random),value(random));
        vectors4[i] = Vector4(value(random),value(random),value(random),value(random));
        boxes[i] = AABB(Vector3(-2,-3,-4),Vector3(3,4,5));
    }
    const unsigned samples = argc > 1 ? unsigned(std::atoi(argv[1])) : 7;
    for (unsigned sample=0;sample<samples;++sample) {
        for (unsigned kind=0;kind<3;++kind) {
            const unsigned repeats = kind == 2 ? 40 : 200;
            const auto start = std::clock();
            for (unsigned rep=0;rep<repeats;++rep) {
                for (unsigned i=0;i<count;++i) {
                    if (kind==0) results3[i] = matrices[i] * vectors3[i];
                    else if (kind==1) results4[i] = matrices[i] * vectors4[i];
                    else resultsBox[i] = matrices[i] * boxes[i];
                }
            }
            const double ns = 1e9 * double(std::clock()-start) / CLOCKS_PER_SEC / repeats / count;
            consumed = results3[sample].x + results4[sample].y + resultsBox[sample].getMinimum().z;
            std::printf("%s,%u,%.3f\n",kind==0?"vec3":kind==1?"vec4":"aabb",sample,ns);
        }
    }
}
