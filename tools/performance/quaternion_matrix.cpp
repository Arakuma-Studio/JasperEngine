// The runner injects the original production method as an independent reference.
// Actual Quaternion and Matrix4 sources compile in separate translation units.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <random>
#include <vector>
#include "math/Quaternion.h"
using namespace jasperengine;

struct BaselineQuaternion {
    float w, x, y, z;
    Matrix4 getRotationMatrix() const;
};
// BASELINE_METHOD

static bool same(float a, float b) {
    return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, sizeof(float)) == 0;
}

static void check(const Quaternion& quaternion) {
    const BaselineQuaternion reference{quaternion.w, quaternion.x, quaternion.y, quaternion.z};
    const Matrix4 expected = reference.getRotationMatrix();
    const Matrix4 actual = quaternion.getRotationMatrix();
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            assert(same(actual.get(column, row), expected.get(column, row)));
}

static void checks() {
    std::mt19937 random(0x5eed);
    std::uniform_real_distribution<float> value(-100, 100);
    for (unsigned i = 0; i < 20000; ++i) {
        Quaternion quaternion(value(random), value(random), value(random), value(random));
        check(quaternion);
        const float norm = std::sqrt(quaternion.w * quaternion.w + quaternion.x * quaternion.x +
                                     quaternion.y * quaternion.y + quaternion.z * quaternion.z);
        quaternion.w /= norm;
        quaternion.x /= norm;
        quaternion.y /= norm;
        quaternion.z /= norm;
        check(quaternion);
    }
    const float special[] = {0.0f, -0.0f, 1.0f, -1.0f,
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
        std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (float w : special) for (float x : special)
        for (float y : special) for (float z : special)
            check(Quaternion(w, x, y, z));
    std::fprintf(stderr, "quaternion matrix equivalence: 40,000 random normalized/non-unit and 10,000 special-float cases passed\n");
}

static volatile float consumed;
int main(int argc, char** argv) {
    checks();
#ifdef CHECK_ONLY
    return 0;
#endif
    constexpr unsigned count = 4096;
    std::vector<Quaternion> rotations(count);
    std::vector<Matrix4> matrices(count);
    std::vector<Vector3> positions(count), scales(count);
    std::mt19937 random(42);
    std::uniform_real_distribution<float> value(-1, 1);
    for (unsigned i = 0; i < count; ++i) {
        rotations[i] = Quaternion(value(random), value(random), value(random), value(random));
        positions[i] = Vector3(value(random), value(random), value(random));
        scales[i] = Vector3(value(random), value(random), value(random));
    }
    const unsigned samples = argc > 1 ? unsigned(std::atoi(argv[1])) : 3;
    for (unsigned sample = 0; sample < samples; ++sample) {
        for (unsigned kind = 0; kind < 2; ++kind) {
            constexpr unsigned repeats = 400;
            const auto start = std::clock();
            for (unsigned repeat = 0; repeat < repeats; ++repeat)
                for (unsigned i = 0; i < count; ++i) {
                    if (kind == 0)
                        matrices[i] = rotations[i].getRotationMatrix();
                    else
                        matrices[i] = Matrix4::translateMatrix(positions[i]) * rotations[i].getRotationMatrix() * Matrix4::scaleMatrix(scales[i]);
                }
            const double ns = 1e9 * double(std::clock() - start) / CLOCKS_PER_SEC / repeats / count;
            consumed = matrices[sample].get(0, 0);
            std::printf("%s,%u,%.3f\n", kind == 0 ? "rotation" : "trs", sample, ns);
        }
    }
}
