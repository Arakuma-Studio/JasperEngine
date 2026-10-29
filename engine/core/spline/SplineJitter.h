// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Include/Components/Spline/SplineStationJitter.h and
// Engine/Include/Components/Spline/SplinePoolSelection.h, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEJITTER_H
#define SPLINE_SPLINEJITTER_H

#include <cstdint>

namespace jasperengine{ namespace spline{

    // Which per-station random channel a draw comes from. Channels are salted apart so a
    // station's draws are independent of one another: if yaw moved with spacing, every piece
    // that slid forward would also turn the same way and the row would read as one slither
    // rather than as noise. Values are part of the saved-layout contract (every authored
    // scene's layout reshuffles if one is renumbered), so APPEND ONLY.
    enum class SplineJitterChannel : uint32_t{
        Spacing = 0,
        Yaw = 1,
        Lateral = 2,
        Dropout = 3,
        Scale = 4
    };

    // Which pool a pick is drawn from. Salts the selection hash so roles sharing an ordinal draw
    // independently. APPEND ONLY, for the same reason as SplineJitterChannel.
    enum class SplinePoolRole : uint32_t{
        // Spline Instantiate Spacing and Tile pieces
        Tile = 0,
        // Fence posts, spans, gates and the crest row's cells. Caps are not a role: the cap that
        // closes a remainder is chosen by the length that fits it, never by a seeded pick.
        Post = 3,
        Span = 4,
        Gate = 5,
        Crest = 6
    };

    // Lifts the jitter channels clear of SplinePoolRole, which salts the same mix with its own
    // small integers. Without it, channel 0 and role 0 would hash identically for a given seed
    // and station, and a station's pool pick would move in lockstep with its spacing wobble.
    inline constexpr uint64_t SPLINE_JITTER_CHANNEL_SALT = 0x5000ull;

    // splitmix64 finalizer: the fixed, implementation-independent integer mix every draw is built
    // from. Not std::hash, whose results are unspecified: scenes must lay out identically on
    // every platform and toolchain.
    constexpr uint64_t splineSelectionMix(uint64_t x){
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    // A per-station draw in [0, 1): a pure function of (seed, channel, ordinal) and nothing else
    // (no running RNG, no iteration order, no entity id), so a rebuild reproduces the layout
    // exactly and nudging knot N leaves every station before it untouched. The ordinal is the
    // pre-dropout position along the spline, so removing a station reshuffles no neighbour.
    constexpr float splineJitterUnit(uint32_t seed, SplineJitterChannel channel, uint32_t ordinal){
        uint64_t h = splineSelectionMix(seed);
        h = splineSelectionMix(h ^ (static_cast<uint64_t>(channel) + SPLINE_JITTER_CHANNEL_SALT));
        h = splineSelectionMix(h ^ static_cast<uint64_t>(ordinal));
        // the top 24 bits over 2^24: every quotient is exact in float, so the draw is
        // bit-identical on any IEEE toolchain
        return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
    }

    // The same draw mapped to [-1, 1): a knob of N metres means "up to N metres either way".
    constexpr float splineJitterSigned(uint32_t seed, SplineJitterChannel channel, uint32_t ordinal){
        return splineJitterUnit(seed, channel, ordinal) * 2.0f - 1.0f;
    }

    // The hash a pool pick is drawn from: a pure function of (seed, role, ordinal).
    constexpr uint64_t splinePoolHash(uint32_t seed, SplinePoolRole role, uint32_t ordinal){
        uint64_t h = splineSelectionMix(seed);
        h = splineSelectionMix(h ^ static_cast<uint64_t>(role));
        return splineSelectionMix(h ^ static_cast<uint64_t>(ordinal));
    }

    // The uniform pool pick: an index in [0, activeCount); activeCount must be >= 1.
    constexpr uint32_t splinePoolSelect(uint32_t seed, SplinePoolRole role, uint32_t ordinal, uint32_t activeCount){
        return static_cast<uint32_t>(splinePoolHash(seed, role, ordinal) % static_cast<uint64_t>(activeCount));
    }

    // The weighted pool pick over activeCount weights read through weightAt(i). Equal weights
    // (the default) give exactly splinePoolSelect, so weighting a pool is opt-in and an unweighted
    // pool picks as it always did. A non-finite or negative weight counts as 0; a pool whose
    // weights are all 0 picks uniformly; an empty pool picks 0.
    template<typename WeightAt>
    constexpr uint32_t splinePoolSelectWeighted(uint32_t seed, SplinePoolRole role, uint32_t ordinal,
                                                uint32_t activeCount, WeightAt weightAt){
        if (activeCount <= 1u){
            return 0u;
        }
        const auto usable = [&weightAt](uint32_t i){
            const float w = weightAt(i);
            // also false for NaN; an infinite weight would swallow the whole pool
            return (w > 0.0f && w <= 3.0e38f) ? w : 0.0f;
        };
        bool equal = true;
        float total = 0.0f;
        const float first = usable(0);
        for (uint32_t i = 0; i < activeCount; i++){
            const float w = usable(i);
            equal = equal && w == first;
            total += w;
        }
        if (equal || !(total > 0.0f)){
            return splinePoolSelect(seed, role, ordinal, activeCount);
        }
        const float u = static_cast<float>(splinePoolHash(seed, role, ordinal) >> 40) * (1.0f / 16777216.0f) * total;
        float cumulative = 0.0f;
        uint32_t lastPositive = 0;
        for (uint32_t i = 0; i < activeCount; i++){
            const float w = usable(i);
            if (w <= 0.0f){
                continue;
            }
            cumulative += w;
            lastPositive = i;
            if (u < cumulative){
                return i;
            }
        }
        return lastPositive;
    }

}}

#endif // SPLINE_SPLINEJITTER_H
