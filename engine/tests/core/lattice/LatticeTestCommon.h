// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the lattice unit tests: lattices, matrices built without the engine's
// angle settings, a seeded random source and float comparisons.

#ifndef LATTICE_TEST_COMMON_H
#define LATTICE_TEST_COMMON_H

#include "lattice/LatticeSolver.h"
#include "lattice/LatticePresets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace jasperengine{ namespace test{

    inline LatticeComponent makeLattice(int rx, int ry, int rz,
                                        LatticeInterpolation interpolation = LatticeInterpolation::CUBIC,
                                        LatticeOutside outside = LatticeOutside::FADE, float fadeDistance = 0.25f){
        LatticeComponent lattice;
        lattice.resolutionX = rx;
        lattice.resolutionY = ry;
        lattice.resolutionZ = rz;
        lattice.offsets.assign(size_t(lattice::pointCount(rx, ry, rz)), Vector3(0, 0, 0));
        lattice.interpolation = interpolation;
        lattice.outside = outside;
        lattice.fadeDistance = fadeDistance;
        return lattice;
    }

    // columns c0, c1, c2 and translation t (the engine stores matrices [column][row])
    inline Matrix4 affine(const Vector3& c0, const Vector3& c1, const Vector3& c2, const Vector3& t){
        Matrix4 m;
        const Vector3* columns[3] = {&c0, &c1, &c2};
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++){
                m.set(c, r, (*columns[c])[r]);
            }
        }
        m.set(3, 0, t.x);
        m.set(3, 1, t.y);
        m.set(3, 2, t.z);
        return m;
    }

    inline Matrix3 columns3(const Vector3& c0, const Vector3& c1, const Vector3& c2){
        Matrix3 m;
        m.setColumn(0, c0);
        m.setColumn(1, c1);
        m.setColumn(2, c2);
        return m;
    }

    // right-handed rotation by `radians` around a unit axis (Rodrigues)
    inline Matrix3 rotation(const Vector3& axis, double radians){
        const double c = std::cos(radians), s = std::sin(radians), t = 1.0 - c;
        const double x = axis.x, y = axis.y, z = axis.z;
        return columns3(Vector3(float(t * x * x + c), float(t * x * y + s * z), float(t * x * z - s * y)),
                        Vector3(float(t * x * y - s * z), float(t * y * y + c), float(t * y * z + s * x)),
                        Vector3(float(t * x * z + s * y), float(t * y * z - s * x), float(t * z * z + c)));
    }

    inline Matrix4 affine(const Matrix3& linear, const Vector3& t){
        return affine(linear.column(0), linear.column(1), linear.column(2), t);
    }

    // deterministic random numbers (splitmix64; the standard distributions differ between libraries)
    struct Random{
        uint64_t state;
        explicit Random(uint64_t seed): state(seed) {}
        uint64_t next(){
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        // uniform in [lo, hi)
        float uniform(float lo, float hi){
            return lo + (hi - lo) * float(double(next() >> 11) * (1.0 / 9007199254740992.0));
        }
        Vector3 vector(float lo, float hi){
            const float x = uniform(lo, hi);
            const float y = uniform(lo, hi);
            const float z = uniform(lo, hi);
            return Vector3(x, y, z);
        }
    };

    inline void randomOffsets(LatticeComponent& lattice, uint64_t seed, float amplitude){
        Random random(seed);
        for (Vector3& offset : lattice.offsets){
            offset = random.vector(-amplitude, amplitude);
        }
    }

    // offsets of the linear field offset(R) = G * R + c at the rest positions
    inline void linearOffsets(LatticeComponent& lattice, const Matrix3& g, const Vector3& c){
        const int rx = lattice.resolutionX, ry = lattice.resolutionY, rz = lattice.resolutionZ;
        for (int k = 0; k < rz; k++){
            for (int j = 0; j < ry; j++){
                for (int i = 0; i < rx; i++){
                    const Vector3 rest = lattice::restPosition(rx, ry, rz, i, j, k);
                    lattice.offsets[size_t(lattice::pointIndex(rx, ry, i, j, k))] = g * rest + c;
                }
            }
        }
    }

    inline float maxAbsDifference(const Matrix3& a, const Matrix3& b){
        float result = 0;
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++){
                result = std::max(result, std::fabs(a.get(c, r) - b.get(c, r)));
            }
        }
        return result;
    }

    inline float maxAbsDifference(const Vector3& a, const Vector3& b){
        return std::max(std::fabs(a.x - b.x), std::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
    }

    inline bool bitwiseEqual(const Vector3& a, const Vector3& b){
        return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 && std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
               std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
    }

    inline const char* interpolationName(LatticeInterpolation interpolation){
        switch (interpolation){
            case LatticeInterpolation::SHARP: return "sharp";
            case LatticeInterpolation::SMOOTH: return "smooth";
            default: return "cubic";
        }
    }

    const LatticeInterpolation allInterpolations[] = {
        LatticeInterpolation::SHARP, LatticeInterpolation::SMOOTH, LatticeInterpolation::CUBIC
    };

}}

#endif //LATTICE_TEST_COMMON_H
