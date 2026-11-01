// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleNoise.h"
#include "ParticleRandom.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    float fade(float t){
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    float lerp(float a, float b, float f){
        return a + (b - a) * f;
    }

    // Perlin's 12 edge gradients (and 4 repeats) picked by the lattice hash
    float grad(uint32_t hash, float x, float y, float z){
        const uint32_t h = hash & 15u;
        const float u = h < 8u ? x : y;
        const float v = h < 4u ? y : ((h == 12u || h == 14u) ? x : z);
        return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
    }

    uint32_t lattice(int x, int y, int z){
        return hash32((uint32_t)x * 73856093u ^ hash32((uint32_t)y * 19349663u ^ hash32((uint32_t)z * 83492791u)));
    }

}

float particles::gradientNoise3(const Vector3& p){
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    const float x = p.x - fx, y = p.y - fy, z = p.z - fz;
    const float u = fade(x), v = fade(y), w = fade(z);

    const float n000 = grad(lattice(ix, iy, iz), x, y, z);
    const float n100 = grad(lattice(ix + 1, iy, iz), x - 1, y, z);
    const float n010 = grad(lattice(ix, iy + 1, iz), x, y - 1, z);
    const float n110 = grad(lattice(ix + 1, iy + 1, iz), x - 1, y - 1, z);
    const float n001 = grad(lattice(ix, iy, iz + 1), x, y, z - 1);
    const float n101 = grad(lattice(ix + 1, iy, iz + 1), x - 1, y, z - 1);
    const float n011 = grad(lattice(ix, iy + 1, iz + 1), x, y - 1, z - 1);
    const float n111 = grad(lattice(ix + 1, iy + 1, iz + 1), x - 1, y - 1, z - 1);

    const float value = lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v),
                             lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w);
    // improved noise peaks a little above 1 in rare corners
    return std::max(-1.0f, std::min(1.0f, value));
}

Vector3 particles::fbm3(const Vector3& p, int octaves, float octaveMultiplier, float octaveScale){
    octaves = std::max(1, std::min(octaves, 4));
    // three far apart offsets: the axes do not move together
    static const Vector3 offsets[3] = {Vector3(0.0f, 0.0f, 0.0f), Vector3(31.416f, 47.853f, 12.793f), Vector3(-63.134f, 19.271f, 88.411f)};
    Vector3 sum(0, 0, 0);
    float weight = 1.0f, frequency = 1.0f, total = 0.0f;
    for (int o = 0; o < octaves; o++){
        const Vector3 q = p * frequency;
        sum.x += gradientNoise3(q + offsets[0]) * weight;
        sum.y += gradientNoise3(q + offsets[1]) * weight;
        sum.z += gradientNoise3(q + offsets[2]) * weight;
        total += weight;
        weight *= octaveMultiplier;
        frequency *= octaveScale;
    }
    return total > 0.0f ? sum / total : sum;
}
