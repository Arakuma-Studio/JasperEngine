// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_RANDOM_H
#define PARTICLES_PARTICLE_RANDOM_H

// The deterministic randomness of version 2 systems (particles spec 5.5): a PCG32 stream per
// system, and per particle values hashed from the particle's seed and a value id, so a
// particle keeps its randoms for its life and a fixed seed replays exactly.

#include <cstdint>

namespace jasperengine {
namespace particles {

    // PCG32 (O'Neill, pcg-random.org, the minimal "XSH RR" generator)
    struct Pcg32 {
        uint64_t& state;
        uint64_t inc;

        Pcg32(uint64_t& state, uint64_t inc): state(state), inc(inc | 1u) {}

        uint32_t next(){
            const uint64_t old = state;
            state = old * 6364136223846793005ULL + inc;
            const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
            const uint32_t rot = (uint32_t)(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
        }

        // [0, 1)
        float next01(){
            return (float)(next() >> 8) * (1.0f / 16777216.0f);
        }
    };

    inline void pcgSeed(uint64_t& state, uint64_t& inc, uint64_t seed, uint64_t sequence){
        state = 0;
        inc = (sequence << 1u) | 1u;
        Pcg32 rng(state, inc);
        rng.next();
        state += seed;
        rng.next();
    }

    // a 32-bit integer hash (Wellons' lowbias32)
    inline uint32_t hash32(uint32_t x){
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    // [0, 1) from a particle seed and a value id
    inline float hash01(uint32_t seed, uint32_t id){
        return (float)(hash32(seed ^ hash32(id * 0x9E3779B9U + 0x632BE5ABU)) >> 8) * (1.0f / 16777216.0f);
    }

}
}

#endif //PARTICLES_PARTICLE_RANDOM_H
