// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 7.2)
// - perlinNoise(x, y): Ken Perlin's improved noise in 2D with his fixed permutation table, mapped
//   to [0, 1] (0.5 at every integer lattice point), the range of Unity's Mathf.PerlinNoise. It is
//   smooth (C2: the quintic fade) and repeats every 256 units on each axis. The values are not
//   bit-identical to Unity's; the camera shake formulas only need the same range and smoothness.
// - Pure and header-only: any thread.

#ifndef JASPERENGINE_NOISE_H
#define JASPERENGINE_NOISE_H

#include <cmath>
#include <cstdint>

namespace jasperengine{

    namespace noise_detail{

        // Ken Perlin's reference permutation (improved noise, 2002), repeated once so p[i + 1]
        // and p[p[i] + j] never index past the end
        inline const uint8_t* permutation(){
            static const uint8_t table[512] = {
                151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
                190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,
                125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,
                105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,
                135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,255,
                82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,
                153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
                251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,
                157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,
                66,215,61,156,180,
                151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
                190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,
                125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,
                105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,
                135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,255,
                82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,
                153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
                251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,
                157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,
                66,215,61,156,180
            };
            return table;
        }

        inline float fade(float t){
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        inline float lerp(float t, float a, float b){
            return a + t * (b - a);
        }

        // one of 8 gradients: the 4 diagonals (length sqrt 2) and the 4 axes, dotted with (x, y);
        // with these the noise stays within [-1, 1]
        inline float gradient(uint8_t hash, float x, float y){
            switch (hash & 7){
                case 0: return  x + y;
                case 1: return -x + y;
                case 2: return  x - y;
                case 3: return -x - y;
                case 4: return  x;
                case 5: return -x;
                case 6: return  y;
                default: return -y;
            }
        }

    }

    // [0, 1]; 0.5 at integer lattice points
    inline float perlinNoise(float x, float y){
        using namespace noise_detail;
        const uint8_t* p = permutation();

        const float fx = std::floor(x);
        const float fy = std::floor(y);
        // the lattice cell modulo 256 (two's complement & keeps negative cells in range)
        const int X = (int)(int64_t)fx & 255;
        const int Y = (int)(int64_t)fy & 255;
        const float xf = x - fx;
        const float yf = y - fy;

        const float u = fade(xf);
        const float v = fade(yf);

        const uint8_t aa = p[p[X] + Y];
        const uint8_t ab = p[p[X] + Y + 1];
        const uint8_t ba = p[p[X + 1] + Y];
        const uint8_t bb = p[p[X + 1] + Y + 1];

        const float n = lerp(v,
                             lerp(u, gradient(aa, xf, yf), gradient(ba, xf - 1.0f, yf)),
                             lerp(u, gradient(ab, xf, yf - 1.0f), gradient(bb, xf - 1.0f, yf - 1.0f)));

        const float mapped = (n + 1.0f) * 0.5f;
        return mapped < 0.0f ? 0.0f : (mapped > 1.0f ? 1.0f : mapped);
    }

}

#endif //JASPERENGINE_NOISE_H
