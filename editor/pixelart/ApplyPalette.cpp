// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The lost colours follow ritmo's lostColors (packages/tools/src/pixel/core/lost.ts, MIT,
// Copyright (c) 2026 Sandro Maglione): its 0.06 OKLab threshold and grouping radius, and its
// two-pixel minimum, with the groups also 8-connected so each has a place in the picture.

#include "pixelart/ApplyPalette.h"

#include "pixelart/PaletteUsage.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace jasperengine::editor::pixelart {
namespace {

    namespace pxa = jasperengine::pixelart;

    constexpr float kApplyLostDistance = 0.06f;     // ritmo's defaultLostThreshold
    constexpr float kApplyLostRadius = 0.06f;       // ritmo's GROUP_RADIUS
    constexpr int kApplyLostMinPixels = 2;          // ritmo's minCells: a lone pixel is noise
    constexpr double kApplyFixableDeltaE = 15.0;    // R40: 0.15 OKLab
    constexpr int kApplyFixableLost = 16;           // R40: a lost group this big needs a person
    constexpr int kApplyReduceIterations = 8;       // R23
    constexpr int kApplyReduceMaxColors = 256;      // R23, Palette::kMaxColors

    // lostColor marks: a pixel that is not lost, or already in a group
    constexpr uint32_t kApplyNotLost = 0xffffffffu;
    constexpr uint32_t kApplyGrouped = 0xfffffffeu;

    float applyOklabDistance(const pxa::Lab& x, const pxa::Lab& y) {
        const float dl = x.l - y.l, da = x.a - y.a, db = x.b - y.b;
        return std::sqrt(dl * dl + da * da + db * db);
    }

    // murmur3's finaliser, so the table's low bits see every channel
    uint32_t applyMix32(uint32_t h) {
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
        return h;
    }

    bool applySameRgb(pxa::Rgba8 x, pxa::Rgba8 y) {
        return x.r == y.r && x.g == y.g && x.b == y.b;
    }

    // What a distinct RGB of the picture (or a Floyd–Steinberg target) needs, worked out once:
    // pictures repeat few colours many times, and the pair search is far dearer than a lookup
    struct ApplyColor {
        pxa::Lab lab;                     // OKLab, for the statistics and the error
        pxa::PaletteMatcher::Mix mix;     // without an ordered dither: a = b = the nearest
        bool exact = false;               // the colour is a palette colour's RGB
    };

    class ApplyColorTable {
    public:
        ApplyColorTable(const pxa::PaletteMatcher& paletteMatcher, std::span<const pxa::Rgba8> colors, int bayerSize, float ditherStrength)
            : matcher(paletteMatcher), palette(colors), ordered(bayerSize > 0), strength(ditherStrength), steps(std::max(1, bayerSize * bayerSize)) {}

        // the entry of the colour's RGB, added on first sight; entries never move index
        uint32_t find(pxa::Rgba8 color) {
            if ((resolvedColors.size() + 1) * 2 > keys.size()) grow();
            const uint32_t key = pxa::packRgb(color) + 1u;
            const size_t mask = keys.size() - 1;
            size_t at = applyMix32(key) & mask;
            while (keys[at] != 0) {
                if (keys[at] == key) return entries[at];
                at = (at + 1) & mask;
            }
            keys[at] = key;
            entries[at] = (uint32_t)resolvedColors.size();
            resolvedColors.push_back(resolve(color));
            return entries[at];
        }

        const ApplyColor& operator[](uint32_t entry) const { return resolvedColors[entry]; }

    private:
        ApplyColor resolve(pxa::Rgba8 color) const {
            ApplyColor resolved;
            resolved.lab = pxa::toOklab(color);
            if (ordered) {
                resolved.mix = matcher.ditherPair(color, strength, steps);
            } else {
                const uint16_t nearest = (uint16_t)matcher.nearestUncached(color).first;
                resolved.mix.a = resolved.mix.b = nearest;
                resolved.mix.ratio16 = 0;
            }
            resolved.exact = applySameRgb(palette[resolved.mix.a], color);
            return resolved;
        }

        void grow() {
            std::vector<uint32_t> oldKeys = std::move(keys);
            std::vector<uint32_t> oldEntries = std::move(entries);
            const size_t size = oldKeys.empty() ? 1024 : oldKeys.size() * 2;
            keys.assign(size, 0);
            entries.assign(size, 0);
            const size_t mask = size - 1;
            for (size_t i = 0; i < oldKeys.size(); i++) {
                if (oldKeys[i] == 0) continue;
                size_t at = applyMix32(oldKeys[i]) & mask;
                while (keys[at] != 0) at = (at + 1) & mask;
                keys[at] = oldKeys[i];
                entries[at] = oldEntries[i];
            }
        }

        const pxa::PaletteMatcher& matcher;
        std::span<const pxa::Rgba8> palette;
        bool ordered = false;
        float strength = 1.0f;
        int steps = 16;              // the matrix's cells: the ratio's steps
        std::vector<ApplyColor> resolvedColors;
        std::vector<uint32_t> keys;       // rgb + 1, 0 is empty
        std::vector<uint32_t> entries;    // the index into resolvedColors, per key slot
    };

    int applyBayerSize(Dither dither) {
        switch (dither) {
            case Dither::Bayer2: return 2;
            case Dither::Bayer4: return 4;
            case Dither::Bayer8: return 8;
            default: return 0;
        }
    }

    int applyDistinctColors(const std::vector<pxa::Rgba8>& pixels) {
        ColorTally tally;
        for (const pxa::Rgba8& pixel : pixels) {
            if (pixel.a != 0) tally.add(packRgba(pixel));
        }
        return (int)tally.size();
    }

    // The 8-connected groups of lost pixels, each grown from its first pixel in reading order
    // and joined by the lost neighbours within kApplyLostRadius of the group's running mean
    // (ritmo's rule, so a gradient does not chain into one group); `lostColor` holds each lost
    // pixel's table entry and is consumed. Returns the largest group's size.
    int applyGroupLost(int width, int height, std::vector<uint32_t>& lostColor, const std::vector<uint16_t>& lostMapped,
                        const ApplyColorTable& table, std::span<const pxa::Rgba8> palette, std::vector<LostGroup>& groups) {
        int largest = 0;
        std::vector<uint32_t> queue;
        std::vector<int64_t> became(palette.size(), 0);
        std::vector<uint16_t> touched;
        const size_t count = lostColor.size();
        for (size_t seed = 0; seed < count; seed++) {
            if (lostColor[seed] >= kApplyGrouped) continue;
            double sumL = 0.0, sumA = 0.0, sumB = 0.0;
            int members = 0;
            pxa::Lab centre;
            int minX = width, minY = height, maxX = -1, maxY = -1;
            queue.clear();
            size_t head = 0;
            auto join = [&](uint32_t pixel) {
                const pxa::Lab& lab = table[lostColor[pixel]].lab;
                sumL += lab.l;
                sumA += lab.a;
                sumB += lab.b;
                members++;
                centre = pxa::Lab{(float)(sumL / members), (float)(sumA / members), (float)(sumB / members)};
                const int x = (int)(pixel % (uint32_t)width), y = (int)(pixel / (uint32_t)width);
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                const uint16_t mapped = lostMapped[pixel];
                if (became[mapped]++ == 0) touched.push_back(mapped);
                lostColor[pixel] = kApplyGrouped;
                queue.push_back(pixel);
            };
            join((uint32_t)seed);
            while (head < queue.size()) {
                const uint32_t pixel = queue[head++];
                const int px = (int)(pixel % (uint32_t)width), py = (int)(pixel / (uint32_t)width);
                for (int dy = -1; dy <= 1; dy++) {
                    const int ny = py + dy;
                    if (ny < 0 || ny >= height) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        const int nx = px + dx;
                        if ((dx == 0 && dy == 0) || nx < 0 || nx >= width) continue;
                        const uint32_t neighbour = (uint32_t)ny * (uint32_t)width + (uint32_t)nx;
                        if (lostColor[neighbour] >= kApplyGrouped) continue;
                        if (applyOklabDistance(table[lostColor[neighbour]].lab, centre) <= kApplyLostRadius) join(neighbour);
                    }
                }
            }
            // the palette colour most of the group became, ties to the lower index
            int best = -1;
            for (uint16_t index : touched) {
                if (best < 0 || became[index] > became[best] || (became[index] == became[best] && index < best)) best = index;
            }
            for (uint16_t index : touched) became[index] = 0;
            touched.clear();
            largest = std::max(largest, members);
            if (members < kApplyLostMinPixels) continue;
            LostGroup group;
            group.original = pxa::fromOklab(centre);
            group.mapped = palette[best];
            group.count = members;
            group.x = minX;
            group.y = minY;
            group.w = maxX - minX + 1;
            group.h = maxY - minY + 1;
            groups.push_back(group);
        }
        // the groups were found in reading order, which the stable sort keeps for equal sizes
        std::stable_sort(groups.begin(), groups.end(), [](const LostGroup& x, const LostGroup& y) { return x.count > y.count; });
        return largest;
    }

}

    const char* ditherName(Dither dither) {
        switch (dither) {
            case Dither::None: return "none";
            case Dither::Bayer2: return "bayer2";
            case Dither::Bayer4: return "bayer4";
            case Dither::Bayer8: return "bayer8";
            case Dither::FloydSteinberg: return "floyd_steinberg";
        }
        return "none";
    }

    bool parseDither(std::string_view name, Dither& out) {
        for (Dither dither : {Dither::None, Dither::Bayer2, Dither::Bayer4, Dither::Bayer8, Dither::FloydSteinberg}) {
            if (name == ditherName(dither)) {
                out = dither;
                return true;
            }
        }
        return false;
    }

    int verdictExitCode(Verdict verdict) {
        switch (verdict) {
            case Verdict::Clean: return 0;
            case Verdict::Fixable: return 3;
            case Verdict::NeedsPerson: return 4;
        }
        return 4;
    }

    ApplyResult applyPalette(const RgbaImage& image, const ApplyRequest& request) {
        ApplyResult result;
        result.image = image;
        result.palette = request.palette;
        const bool binary = request.alphaThreshold > 0;
        const int alphaThreshold = std::min(request.alphaThreshold, 255);
        auto mappable = [&](pxa::Rgba8 pixel) { return pixel.a != 0 && (!binary || pixel.a >= alphaThreshold); };

        if (!image.valid()) {
            for (size_t i = 0; i < result.palette.size(); i++) result.usage.push_back(ColorUsage{(int)i, result.palette[i], 0});
            result.verdict = Verdict::NeedsPerson;
            return result;
        }
        const int width = image.width, height = image.height;
        const size_t pixelCount = image.pixelCount();

        if (request.reduce > 0) {
            const int count = std::clamp(request.reduce, 2, kApplyReduceMaxColors);
            // medianCut keeps the locked colours first, opaque and without duplicates; kMeansRefine
            // must hold exactly those still
            std::vector<pxa::Rgba8> keep;
            for (uint16_t index : request.locked) {
                if (index >= request.palette.size() || request.palette[index].a == 0) continue;
                const pxa::Rgba8 color{request.palette[index].r, request.palette[index].g, request.palette[index].b, 255};
                if (std::none_of(keep.begin(), keep.end(), [&](const pxa::Rgba8& kept) { return applySameRgb(kept, color); })) keep.push_back(color);
            }
            std::vector<pxa::Rgba8> source;
            source.reserve(pixelCount);
            for (const pxa::Rgba8& pixel : image.pixels) {
                if (mappable(pixel)) source.push_back(pixel);
            }
            std::vector<pxa::Rgba8> found = pxa::medianCut(source, count, keep);
            const int fixed = std::min((int)keep.size(), (int)found.size());
            result.palette = pxa::kMeansRefine(source, std::move(found), fixed, kApplyReduceIterations);
        }
        const std::vector<pxa::Rgba8>& palette = result.palette;

        ApplyStats& stats = result.stats;
        stats.colorsBefore = applyDistinctColors(image.pixels);
        std::vector<int64_t> counts(palette.size(), 0);

        const bool canMap = !palette.empty();
        const int bayerSize = applyBayerSize(request.dither);
        const bool diffuse = request.dither == Dither::FloydSteinberg;
        const float strength = std::isfinite(request.ditherStrength) ? std::clamp(request.ditherStrength, 0.0f, 1.0f) : 0.0f;
        std::vector<pxa::Lab> paletteLabs;
        paletteLabs.reserve(palette.size());
        for (const pxa::Rgba8& color : palette) paletteLabs.push_back(pxa::toOklab(color));
        const pxa::PaletteMatcher matcher(palette, request.metric);
        ApplyColorTable table(matcher, palette, bayerSize, strength);

        float thresholds[64] = {};
        for (int y = 0; y < bayerSize; y++) {
            for (int x = 0; x < bayerSize; x++) thresholds[y * bayerSize + x] = pxa::bayerThreshold(bayerSize, x, y);
        }
        // Floyd–Steinberg's error for this row and the next, one pixel of margin on each side
        std::vector<pxa::Lab> errorRows;
        if (diffuse && canMap) errorRows.assign(2 * ((size_t)width + 2), pxa::Lab{});
        pxa::Lab* errorHere = errorRows.empty() ? nullptr : errorRows.data();
        pxa::Lab* errorNext = errorRows.empty() ? nullptr : errorRows.data() + width + 2;

        std::vector<uint32_t> lostColor;     // per pixel: its table entry when lost, else kApplyNotLost
        std::vector<uint16_t> lostMapped;    // per lost pixel: the palette index it took
        double deltaSum = 0.0;
        uint32_t lastKey = 0, lastEntry = 0;

        for (int y = 0; y < height; y++) {
            if (errorNext) std::fill(errorNext, errorNext + width + 2, pxa::Lab{});
            for (int x = 0; x < width; x++) {
                const size_t i = (size_t)y * (size_t)width + (size_t)x;
                const pxa::Rgba8 source = image.pixels[i];
                pxa::Rgba8& out = result.image.pixels[i];
                if (source.a == 0) continue;
                if (binary && source.a < alphaThreshold) {
                    out = pxa::Rgba8{0, 0, 0, 0};
                    continue;
                }
                if (!canMap) {
                    if (binary) out.a = 255;
                    continue;
                }

                const uint32_t key = pxa::packRgb(source) + 1u;
                if (key != lastKey) {
                    lastKey = key;
                    lastEntry = table.find(source);
                }
                const ApplyColor own = table[lastEntry];
                int index = own.mix.a;
                if (bayerSize > 0) {
                    const float threshold = thresholds[(y & (bayerSize - 1)) * bayerSize + (x & (bayerSize - 1))];
                    if (threshold < own.mix.ratio16 / (float)(bayerSize * bayerSize)) index = own.mix.b;
                } else if (diffuse && !own.exact) {
                    // an exact palette colour stays itself and swallows the error it receives
                    const pxa::Lab incoming = errorHere[x + 1];
                    const pxa::Lab target{own.lab.l + incoming.l, own.lab.a + incoming.a, own.lab.b + incoming.b};
                    float linear[3];
                    pxa::oklabToLinear(target, linear);
                    const bool inGamut = linear[0] >= 0.0f && linear[0] <= 1.0f && linear[1] >= 0.0f && linear[1] <= 1.0f &&
                                         linear[2] >= 0.0f && linear[2] <= 1.0f;
                    const ApplyColor pick = table[table.find(pxa::fromOklab(target))];
                    index = pick.mix.a;
                    // past the gamut the error is what the clamped colour misses, so it cannot grow without bound
                    const pxa::Lab reached = inGamut ? target : pick.lab;
                    const pxa::Lab& chosenLab = paletteLabs[index];
                    const pxa::Lab error{(reached.l - chosenLab.l) * strength, (reached.a - chosenLab.a) * strength,
                                         (reached.b - chosenLab.b) * strength};
                    auto spread = [&error](pxa::Lab& into, float weight) {
                        into.l += error.l * weight;
                        into.a += error.a * weight;
                        into.b += error.b * weight;
                    };
                    spread(errorHere[x + 2], 7.0f / 16.0f);
                    spread(errorNext[x], 3.0f / 16.0f);
                    spread(errorNext[x + 1], 5.0f / 16.0f);
                    spread(errorNext[x + 2], 1.0f / 16.0f);
                }

                const pxa::Rgba8 chosen = palette[index];
                out = pxa::Rgba8{chosen.r, chosen.g, chosen.b, binary ? (uint8_t)255 : source.a};
                counts[index]++;
                stats.pixelsMapped++;
                const float distance = applySameRgb(chosen, source) ? 0.0f : applyOklabDistance(own.lab, paletteLabs[index]);
                const double deltaE = (double)distance * 100.0;
                deltaSum += deltaE;
                stats.maxDeltaE = std::max(stats.maxDeltaE, deltaE);
                if (distance > kApplyLostDistance) {
                    if (lostColor.empty()) {
                        lostColor.assign(pixelCount, kApplyNotLost);
                        lostMapped.assign(pixelCount, 0);
                    }
                    lostColor[i] = lastEntry;
                    lostMapped[i] = (uint16_t)index;
                }
            }
            if (errorHere) std::swap(errorHere, errorNext);
        }

        for (size_t i = 0; i < pixelCount; i++) {
            if (result.image.pixels[i] != image.pixels[i]) stats.pixelsChanged++;
        }
        stats.colorsAfter = applyDistinctColors(result.image.pixels);
        stats.meanDeltaE = stats.pixelsMapped > 0 ? deltaSum / (double)stats.pixelsMapped : 0.0;

        result.usage.reserve(palette.size());
        for (size_t i = 0; i < palette.size(); i++) result.usage.push_back(ColorUsage{(int)i, palette[i], counts[i]});

        int largestLost = 0;
        if (!lostColor.empty()) largestLost = applyGroupLost(width, height, lostColor, lostMapped, table, palette, result.lost);

        if (stats.pixelsChanged == 0) {
            result.verdict = Verdict::Clean;
        } else if (stats.maxDeltaE <= kApplyFixableDeltaE && largestLost < kApplyFixableLost) {
            result.verdict = Verdict::Fixable;
        } else {
            result.verdict = Verdict::NeedsPerson;
        }
        return result;
    }

    RgbaImage remapIndices(const RgbaImage& image, std::span<const pxa::Rgba8> from, std::span<const pxa::Rgba8> to) {
        RgbaImage out = image;
        if (!image.valid() || from.empty()) return out;
        // Palette Swap's rule (PaletteSwapData): the RGB of a source colour matches, and a fully
        // transparent slot is not a colour (an opaque black must not take "#00000000"'s place)
        const ExactColorIndex lookup(from, true);
        uint32_t lastRgb = 0xffffffffu;
        int lastIndex = -1;
        for (pxa::Rgba8& pixel : out.pixels) {
            const uint32_t rgb = pxa::packRgb(pixel);
            if (rgb != lastRgb) {
                lastRgb = rgb;
                lastIndex = lookup.find(pixel);
            }
            if (lastIndex < 0 || (size_t)lastIndex >= to.size()) continue;
            const pxa::Rgba8 target = to[(size_t)lastIndex];
            // round(a t / 255): a t / 255 is never a half, so this is the GPU's UNORM rounding
            pixel = pxa::Rgba8{target.r, target.g, target.b, (uint8_t)(((int)pixel.a * (int)target.a + 127) / 255)};
        }
        return out;
    }

}
