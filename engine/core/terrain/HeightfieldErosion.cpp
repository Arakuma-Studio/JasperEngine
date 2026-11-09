// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "terrain/HeightfieldErosion.h"

#include "noise/NoiseDetMath.h"
#include "noise/NoiseParallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace jasperengine { namespace terraincore {

    namespace {

        constexpr int kErosionRounds = 4;
        constexpr int kErosionColours = 4;

        uint32_t mixHash(uint32_t h, uint32_t v){
            h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
            h ^= h >> 16;
            h *= 0x7FEB352Du;
            h ^= h >> 15;
            h *= 0x846CA68Bu;
            return h ^ (h >> 16);
        }

        uint32_t mixHash64(uint32_t h, int64_t v){
            const uint64_t bits = static_cast<uint64_t>(v);
            return mixHash(mixHash(h, static_cast<uint32_t>(bits)), static_cast<uint32_t>(bits >> 32));
        }

        float unitFloat(uint32_t h){
            return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
        }

        int64_t floorDiv(int64_t a, int64_t b){
            const int64_t q = a / b;
            return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
        }

        struct BrushCell { int dx, dz; float weight; };

        std::vector<BrushCell> makeBrush(int radius){
            std::vector<BrushCell> cells;
            float sum = 0.0f;
            const float reach = static_cast<float>(radius) + 0.5f;
            for (int dz = -radius; dz <= radius; dz++){
                for (int dx = -radius; dx <= radius; dx++){
                    const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                    const float weight = 1.0f - distance / reach;
                    if (weight > 0.0f){
                        cells.push_back(BrushCell{dx, dz, weight});
                        sum += weight;
                    }
                }
            }
            for (BrushCell& cell : cells) cell.weight /= sum;
            return cells;
        }

        struct HydraulicRun {
            const HydraulicParams* params;
            int width, depth;
            int radius;
            float mpt;
            float* height;              // world units
            const float* hardness;
            float* flow;
            float* sediment;
            const std::vector<BrushCell>* brush;
        };

        // Adds amount (negative: takes it) over the brush around the droplet's nearest texel. Deposits
        // and cuts share the brush: settling on the four cells under a droplet while cutting through a
        // wider brush roughens the ground texel by texel. The part of a brush past the buffer is shared
        // out among what is left, so the material moved is what the droplet carries. From the cell
        // below a droplet this reaches radius + 1 texels, which the padding and the tile side count.
        void applyBrush(const HydraulicRun& run, int ix, int iz, float fx, float fz, float amount){
            const int cx = fx >= 0.5f ? ix + 1 : ix;
            const int cz = fz >= 0.5f ? iz + 1 : iz;
            const size_t w = static_cast<size_t>(run.width);
            const int reach = run.radius;
            if (cx >= reach && cz >= reach && cx + reach < run.width && cz + reach < run.depth){
                // The usual case, and the hot path: the whole brush is inside and its weights sum to 1
                for (const BrushCell& cell : *run.brush){
                    const size_t i = static_cast<size_t>(cz + cell.dz) * w + static_cast<size_t>(cx + cell.dx);
                    const float share = cell.weight * amount;
                    run.height[i] += share;
                    if (run.sediment && share > 0.0f) run.sediment[i] += share;
                }
                return;
            }
            float inside = 0.0f;
            for (const BrushCell& cell : *run.brush){
                const int x = cx + cell.dx, z = cz + cell.dz;
                if (x >= 0 && x < run.width && z >= 0 && z < run.depth) inside += cell.weight;
            }
            const float scale = amount / inside;
            for (const BrushCell& cell : *run.brush){
                const int x = cx + cell.dx, z = cz + cell.dz;
                if (x < 0 || x >= run.width || z < 0 || z >= run.depth) continue;
                const size_t i = static_cast<size_t>(z) * w + static_cast<size_t>(x);
                const float share = cell.weight * scale;
                run.height[i] += share;
                if (run.sediment && share > 0.0f) run.sediment[i] += share;
            }
        }

        // One droplet (Beyer 2015). Moves one texel a step, so it can touch at most
        // lifetime + radius + 2 texels from its start, which is what the tile colouring relies on (N6).
        uint64_t runDroplet(const HydraulicRun& run, float px, float pz, uint32_t dropletHash){
            const HydraulicParams& p = *run.params;
            const size_t w = static_cast<size_t>(run.width);
            float dirX = 0.0f, dirZ = 0.0f;
            float velocity = 1.0f;
            float water = 1.0f;
            float carried = 0.0f;
            int ix = static_cast<int>(px);
            int iz = static_cast<int>(pz);
            float fx = px - static_cast<float>(ix);
            float fz = pz - static_cast<float>(iz);
            uint64_t steps = 0;
            for (int step = 0; step < p.lifetime; step++){
                steps++;
                const size_t i = static_cast<size_t>(iz) * w + static_cast<size_t>(ix);
                const float h00 = run.height[i], h10 = run.height[i + 1], h01 = run.height[i + w], h11 = run.height[i + w + 1];
                const float gx = (h10 - h00) * (1.0f - fz) + (h11 - h01) * fz;
                const float gz = (h01 - h00) * (1.0f - fx) + (h11 - h10) * fx;
                const float oldHeight = h00 * (1.0f - fx) * (1.0f - fz) + h10 * fx * (1.0f - fz) + h01 * (1.0f - fx) * fz + h11 * fx * fz;

                dirX = dirX * p.inertia - gx * (1.0f - p.inertia);
                dirZ = dirZ * p.inertia - gz * (1.0f - p.inertia);
                float length = std::sqrt(dirX * dirX + dirZ * dirZ);
                if (length < 1e-6f){
                    // flat: on in a direction of its own, as a flat cell gives none
                    const uint32_t h = mixHash(dropletHash, static_cast<uint32_t>(step));
                    dirX = unitFloat(h) * 2.0f - 1.0f;
                    dirZ = unitFloat(mixHash(h, 1u)) * 2.0f - 1.0f;
                    length = std::sqrt(dirX * dirX + dirZ * dirZ);
                    if (length < 1e-6f) break;
                }
                dirX /= length;
                dirZ /= length;
                const float nextX = static_cast<float>(ix) + fx + dirX;
                const float nextZ = static_cast<float>(iz) + fz + dirZ;
                if (run.flow) run.flow[i] += water;
                if (!(nextX >= 0.0f && nextX < static_cast<float>(run.width - 1) && nextZ >= 0.0f && nextZ < static_cast<float>(run.depth - 1))) break;

                const int nix = static_cast<int>(nextX);
                const int niz = static_cast<int>(nextZ);
                const float nfx = nextX - static_cast<float>(nix);
                const float nfz = nextZ - static_cast<float>(niz);
                const size_t n = static_cast<size_t>(niz) * w + static_cast<size_t>(nix);
                const float newHeight = run.height[n] * (1.0f - nfx) * (1.0f - nfz) + run.height[n + 1] * nfx * (1.0f - nfz) +
                                        run.height[n + w] * (1.0f - nfx) * nfz + run.height[n + w + 1] * nfx * nfz;
                const float deltaHeight = newHeight - oldHeight;
                const float slope = -deltaHeight / run.mpt;

                // In metres per step, from the dimensionless slope: a texel's height changes alike at any
                // resolution, because the visits per texel do not depend on it (R26)
                const float capacity = (slope > p.minSlope ? slope : p.minSlope) * velocity * water * p.capacity;
                if (carried > capacity || deltaHeight > 0.0f){
                    float amount = deltaHeight > 0.0f ? (deltaHeight < carried ? deltaHeight : carried) : (carried - capacity) * p.depositRate;
                    carried -= amount;
                    applyBrush(run, ix, iz, fx, fz, amount);
                } else {
                    float amount = (capacity - carried) * p.erodeRate;
                    const float limit = -deltaHeight;
                    if (amount > limit) amount = limit;
                    if (run.hardness){
                        float hard = run.hardness[i];
                        hard = hard < 0.0f ? 0.0f : (hard > 1.0f ? 1.0f : hard);
                        amount *= 1.0f - hard;
                    }
                    applyBrush(run, ix, iz, fx, fz, -amount);
                    carried += amount;
                }
                const float speed2 = velocity * velocity + slope * p.gravity;
                velocity = speed2 > 0.0f ? std::sqrt(speed2) : 0.0f;
                water *= 1.0f - p.evaporation;
                ix = nix;
                iz = niz;
                fx = nfx;
                fz = nfz;
            }
            // What it still carries settles where it stopped: nothing leaves the map with a droplet
            if (carried > 0.0f) applyBrush(run, ix, iz, fx, fz, carried);
            return steps;
        }

    }

    bool HeightfieldErosion::hydraulic(const HydraulicParams& input, const ErosionRegion& region, float* height, const float* hardness,
                                       float* flow, float* sediment, const ErosionControl& control, uint64_t* dropletSteps){
        if (region.width < 2 || region.depth < 2) return true;
        HydraulicParams params = input;
        params.lifetime = std::clamp(params.lifetime, 1, 1024);
        params.radius = std::clamp(params.radius, 1, 8);
        const size_t texels = static_cast<size_t>(region.width) * static_cast<size_t>(region.depth);
        const float maxHeight = region.maxHeight > 0.0f ? region.maxHeight : 1.0f;
        const double mpt = region.metresPerTexel > 0.0 ? region.metresPerTexel : 1.0;
        for (size_t i = 0; i < texels; i++) height[i] *= maxHeight;
        if (flow) std::fill(flow, flow + texels, 0.0f);
        if (sediment) std::fill(sediment, sediment + texels, 0.0f);

        const std::vector<BrushCell> brush = makeBrush(params.radius);
        HydraulicRun run{&params, region.width, region.depth, params.radius, static_cast<float>(mpt), height, hardness, flow, sediment, &brush};

        const int64_t side = 2 * static_cast<int64_t>(HeightfieldErosion::hydraulicPadding(params));
        std::atomic<uint64_t> totalSteps{0};
        bool finished = true;
        struct Tile { int64_t x, z; };
        std::vector<Tile> tiles;
        for (int round = 0; round < kErosionRounds && finished; round++){
            // A fresh tile grid per round, so tile borders leave no pattern
            const uint32_t roundHash = mixHash(params.seed ^ 0x51ED270Bu, static_cast<uint32_t>(round));
            const int64_t offsetX = static_cast<int64_t>(mixHash(roundHash, 1u) % static_cast<uint32_t>(side));
            const int64_t offsetZ = static_cast<int64_t>(mixHash(roundHash, 2u) % static_cast<uint32_t>(side));
            const int64_t firstX = floorDiv(region.startX + offsetX, side);
            const int64_t lastX = floorDiv(region.startX + region.width - 1 + offsetX, side);
            const int64_t firstZ = floorDiv(region.startZ + offsetZ, side);
            const int64_t lastZ = floorDiv(region.startZ + region.depth - 1 + offsetZ, side);
            for (int colour = 0; colour < kErosionColours && finished; colour++){
                tiles.clear();
                for (int64_t tz = firstZ; tz <= lastZ; tz++){
                    for (int64_t tx = firstX; tx <= lastX; tx++){
                        if (static_cast<int>((tx & 1) | ((tz & 1) << 1)) == colour) tiles.push_back(Tile{tx, tz});
                    }
                }
                finished = noise::detail::parallelChunks(tiles.size(), control.parallel, control.cancel, [&](size_t t){
                    // R26: at least once per tile, on whichever thread claimed it
                    if (noise::detail::cancelled(control.cancel)) return;
                    const Tile tile = tiles[t];
                    const int64_t x0 = std::max(tile.x * side - offsetX, region.startX);
                    const int64_t z0 = std::max(tile.z * side - offsetZ, region.startZ);
                    const int64_t x1 = std::min((tile.x + 1) * side - offsetX, region.startX + region.width);
                    const int64_t z1 = std::min((tile.z + 1) * side - offsetZ, region.startZ + region.depth);
                    if (x1 <= x0 || z1 <= z0) return;
                    const float spanX = static_cast<float>(x1 - x0);
                    const float spanZ = static_cast<float>(z1 - z0);
                    const uint32_t tileHash = mixHash64(mixHash64(roundHash, tile.x), tile.z);
                    const float expected = params.dropletsPerTexel * 0.25f * spanX * spanZ;
                    int droplets = static_cast<int>(expected);
                    if (unitFloat(mixHash(tileHash, 0xFFFFFFFFu)) < expected - static_cast<float>(droplets)) droplets++;
                    uint64_t steps = 0;
                    const float baseX = static_cast<float>(x0 - region.startX);
                    const float baseZ = static_cast<float>(z0 - region.startZ);
                    const float limitX = static_cast<float>(region.width - 1) - 0.001f;
                    const float limitZ = static_cast<float>(region.depth - 1) - 0.001f;
                    for (int index = 0; index < droplets; index++){
                        const uint32_t h = mixHash(tileHash, static_cast<uint32_t>(index));
                        const uint32_t h2 = mixHash(h, 3u);
                        const float px = std::min(baseX + unitFloat(h) * spanX, limitX);
                        const float pz = std::min(baseZ + unitFloat(h2) * spanZ, limitZ);
                        steps += runDroplet(run, px, pz, h);
                    }
                    totalSteps.fetch_add(steps, std::memory_order_relaxed);
                });
            }
        }

        for (size_t i = 0; i < texels; i++) height[i] /= maxHeight;
        // Saturating, not divided by a maximum: a texel's value must not depend on one far hot spot.
        // Squared, so the sheet of water every slope carries stays near 0 and channels stand out as a mask.
        if (flow){
            const float k = params.dropletsPerTexel * static_cast<float>(params.lifetime) * 2.0f;
            for (size_t i = 0; i < texels; i++){
                const float t = flow[i] / (flow[i] + k);
                flow[i] = t * t;
            }
        }
        if (sediment){
            for (size_t i = 0; i < texels; i++){
                const float settled = sediment[i] > 0.0f ? sediment[i] : 0.0f;
                sediment[i] = settled / (settled + 1.0f);
            }
        }
        if (dropletSteps) *dropletSteps = totalSteps.load(std::memory_order_relaxed);
        return finished;
    }

    bool HeightfieldErosion::thermal(const ThermalParams& params, const ErosionRegion& region, float* height, const ErosionControl& control){
        if (region.width < 2 || region.depth < 2) return true;
        const size_t width = static_cast<size_t>(region.width);
        const size_t depth = static_cast<size_t>(region.depth);
        const size_t texels = width * depth;
        const float maxHeight = region.maxHeight > 0.0f ? region.maxHeight : 1.0f;
        const float mpt = region.metresPerTexel > 0.0 ? static_cast<float>(region.metresPerTexel) : 1.0f;
        // R25: the talus height difference of one texel, in normalized heights
        const float radians = params.talusAngle * (noise::detmath::kPi / 180.0f);
        const float talus = noise::detmath::DetMath<noise::Lane1>::tan(radians) * mpt / maxHeight;
        const float share = params.rate * 0.25f;
        std::unique_ptr<float[]> copy(new float[texels]);
        float* from = height;
        float* to = copy.get();
        const int iterations = params.iterations < 1 ? 1 : params.iterations;
        bool finished = true;
        for (int iteration = 0; iteration < iterations && finished; iteration++){
            // Jacobi: every pair's flux comes from the read-only side, with the same magnitude and
            // opposite sign seen from either end, so row order and thread count cannot matter
            finished = noise::detail::parallelChunks(depth, control.parallel, control.cancel, [&](size_t z){
                const float* row = from + z * width;
                float* out = to + z * width;
                for (size_t x = 0; x < width; x++){
                    const float h = row[x];
                    float moved = 0.0f;
                    const auto pair = [&moved, h, talus](float neighbour){
                        const float d = h - neighbour;
                        if (d > talus) moved += d - talus;
                        else if (d < -talus) moved += d + talus;
                    };
                    if (x > 0) pair(row[x - 1]);
                    if (x + 1 < width) pair(row[x + 1]);
                    if (z > 0) pair(row[x - width]);
                    if (z + 1 < depth) pair(row[x + width]);
                    out[x] = h - moved * share;
                }
            });
            std::swap(from, to);
        }
        if (from != height) std::copy(from, from + texels, height);
        return finished;
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
