// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/palette.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupPalette.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;
    using jasperengine::pixelart::fromOklab;
    using jasperengine::pixelart::packRgb;
    using jasperengine::pixelart::unpackRgb;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace palette_ts {

        // Two colours closer than this (OKLab) are the same colour for Dominant (palette.ts)
        constexpr double MERGE_RADIUS = 0.04;
        // Distinct ignores colours used by fewer than this share of the cells (palette.ts)
        constexpr double NOISE_SHARE = 0.002;

        // palette.ts's seeded generator: mulberry32
        class Mulberry32 {
        public:
            explicit Mulberry32(uint32_t seed) : state(seed) {}
            double operator()() {
                state += 0x6d2b79f5u;
                uint32_t t = state;
                t = (t ^ (t >> 15)) * (t | 1u);
                t ^= t + (t ^ (t >> 7)) * (t | 61u);
                return double(t ^ (t >> 14)) / 4294967296.0;
            }

        private:
            uint32_t state;
        };

        // Double-precision OKLab centres: k-means averages them
        struct LabD {
            double l = 0, a = 0, b = 0;
        };

        LabD labD(const Lab& lab) { return {lab.l, lab.a, lab.b}; }

        double distance2(const LabD& x, const LabD& y) {
            const double dl = x.l - y.l, da = x.a - y.a, db = x.b - y.b;
            return dl * dl + da * da + db * db;
        }

        Rgba8 colorOf(const LabD& lab) { return fromOklab(Lab{float(lab.l), float(lab.a), float(lab.b)}, 255); }

        // Unique colours in the order first met, with their counts (a JS Map of keys)
        struct Weighted {
            std::vector<uint32_t> keys;
            std::unordered_map<uint32_t, int> weights;
        };

        Weighted weighOf(const std::vector<Rgba8>& colors) {
            Weighted out;
            for (const Rgba8& color : colors) {
                const uint32_t key = packRgb(color);
                auto [it, inserted] = out.weights.emplace(key, 0);
                if (inserted) out.keys.push_back(key);
                it->second++;
            }
            return out;
        }

    }

    std::vector<Rgba8> extractPalette(const std::vector<Rgba8>& colors, int count, const std::vector<Rgba8>& locked, uint32_t seed) {
        using namespace palette_ts;
        const Weighted weighted = weighOf(colors);
        std::unordered_set<uint32_t> lockedKeys;
        for (const Rgba8& color : locked) lockedKeys.insert(packRgb(color));
        const int free = std::max(0, count - int(locked.size()));
        std::vector<uint32_t> unlocked;
        for (uint32_t key : weighted.keys) {
            if (!lockedKeys.count(key)) unlocked.push_back(key);
        }
        std::vector<Rgba8> result;
        for (const Rgba8& color : locked) result.push_back(Rgba8{color.r, color.g, color.b, 255});
        if (int(unlocked.size()) <= free) {
            for (uint32_t key : unlocked) result.push_back(unpackRgb(key));
            return result;
        }

        struct Point {
            LabD lab;
            int weight;
        };
        std::vector<Point> points;
        points.reserve(weighted.keys.size());
        for (uint32_t key : weighted.keys) points.push_back({labD(oklabOf(unpackRgb(key))), weighted.weights.at(key)});
        Mulberry32 random(seed);
        std::vector<LabD> centers;
        for (const Rgba8& color : locked) centers.push_back(labD(oklabOf(color)));
        const size_t lockedCount = centers.size();

        // k-means++: each new centre is drawn with odds growing with its distance to the nearest so far
        if (centers.empty()) centers.push_back(points[size_t(random() * double(points.size()))].lab);
        std::vector<double> reach(points.size());
        while (centers.size() < lockedCount + size_t(free)) {
            double total = 0;
            for (size_t p = 0; p < points.size(); p++) {
                double nearest = std::numeric_limits<double>::infinity();
                for (const LabD& center : centers) nearest = std::min(nearest, distance2(points[p].lab, center));
                reach[p] = points[p].weight * nearest;
                total += reach[p];
            }
            if (total == 0) break;
            double pick = random() * total;
            size_t chosen = 0;
            for (; chosen < reach.size() - 1; chosen++) {
                pick -= reach[chosen];
                if (pick <= 0) break;
            }
            centers.push_back(points[chosen].lab);
        }
        // Without locked colours the first centre is a random pick, not a locked one
        const size_t fixed = lockedCount;

        for (int iteration = 0; iteration < 24; iteration++) {
            struct Sum {
                double l = 0, a = 0, b = 0, weight = 0;
            };
            std::vector<Sum> sums(centers.size());
            for (const Point& point : points) {
                size_t best = 0;
                double bestDistance = std::numeric_limits<double>::infinity();
                for (size_t c = 0; c < centers.size(); c++) {
                    const double d = distance2(point.lab, centers[c]);
                    if (d < bestDistance) {
                        bestDistance = d;
                        best = c;
                    }
                }
                Sum& sum = sums[best];
                sum.l += point.lab.l * point.weight;
                sum.a += point.lab.a * point.weight;
                sum.b += point.lab.b * point.weight;
                sum.weight += point.weight;
            }
            bool moved = false;
            for (size_t c = fixed; c < centers.size(); c++) {
                const Sum& sum = sums[c];
                if (sum.weight == 0) continue;
                const LabD next{sum.l / sum.weight, sum.a / sum.weight, sum.b / sum.weight};
                if (distance2(next, centers[c]) > 1e-10) moved = true;
                centers[c] = next;
            }
            if (!moved) break;
        }

        std::unordered_set<uint32_t> seen = lockedKeys;
        std::vector<Rgba8> extracted;
        for (size_t c = fixed; c < centers.size(); c++) {
            const Rgba8 color = colorOf(centers[c]);
            if (!seen.insert(packRgb(color)).second) continue;
            extracted.push_back(color);
        }
        // Darkest first among the extracted colours, so the swatches read in order
        std::stable_sort(extracted.begin(), extracted.end(), [](const Rgba8& x, const Rgba8& y) { return oklabOf(x).l < oklabOf(y).l; });
        result.insert(result.end(), extracted.begin(), extracted.end());
        return result;
    }

    std::vector<Proposal> proposePalette(const std::vector<Rgba8>& colors, int count, ProposeMethod method, uint32_t seed) {
        using namespace palette_ts;
        if (colors.empty() || count < 1) return {};
        const Weighted weighted = weighOf(colors);
        struct Point {
            Rgba8 color;
            LabD lab;
            int weight;
        };
        std::vector<Point> points;
        for (uint32_t key : weighted.keys) {
            const Rgba8 color = unpackRgb(key);
            points.push_back({color, labD(oklabOf(color)), weighted.weights.at(key)});
        }
        std::stable_sort(points.begin(), points.end(), [](const Point& x, const Point& y) { return y.weight < x.weight; });
        const double merge2 = MERGE_RADIUS * MERGE_RADIUS;

        std::vector<Rgba8> picked;
        if (method == ProposeMethod::Balanced) {
            picked = extractPalette(colors, count, {}, seed);
        } else if (method == ProposeMethod::Dominant) {
            std::vector<const Point*> chosen;
            for (const Point& point : points) {
                if (int(chosen.size()) >= count) break;
                if (std::all_of(chosen.begin(), chosen.end(), [&](const Point* other) { return distance2(point.lab, other->lab) > merge2; })) {
                    chosen.push_back(&point);
                }
            }
            for (const Point* point : chosen) picked.push_back(point->color);
        } else {
            std::vector<const Point*> pool;
            for (const Point& point : points) {
                if (point.weight >= NOISE_SHARE * double(colors.size())) pool.push_back(&point);
            }
            if (pool.empty()) {
                for (const Point& point : points) pool.push_back(&point);
            }
            std::vector<const Point*> chosen{pool[0]};
            std::vector<double> reach(pool.size());
            for (size_t i = 0; i < pool.size(); i++) reach[i] = distance2(pool[i]->lab, pool[0]->lab);
            while (chosen.size() < std::min(size_t(count), pool.size())) {
                size_t far = 0;
                for (size_t i = 1; i < pool.size(); i++) {
                    if (reach[i] > reach[far]) far = i;
                }
                if (reach[far] <= merge2) break;
                chosen.push_back(pool[far]);
                const LabD farLab = pool[far]->lab;
                for (size_t i = 0; i < pool.size(); i++) reach[i] = std::min(reach[i], distance2(pool[i]->lab, farLab));
            }
            for (const Point* point : chosen) picked.push_back(point->color);
        }

        // k-means may split one colour in two when asked for more colours than the picture has
        std::vector<Rgba8> kept;
        for (size_t i = 0; i < picked.size(); i++) {
            const LabD lab = labD(oklabOf(picked[i]));
            bool apart = true;
            for (size_t j = 0; j < i && apart; j++) apart = distance2(lab, labD(oklabOf(picked[j]))) > merge2;
            if (apart) kept.push_back(picked[i]);
        }
        std::vector<LabD> labs;
        for (const Rgba8& color : kept) labs.push_back(labD(oklabOf(color)));
        std::vector<double> shares(kept.size(), 0.0);
        for (const Point& point : points) {
            size_t best = 0;
            for (size_t c = 1; c < labs.size(); c++) {
                if (distance2(point.lab, labs[c]) < distance2(point.lab, labs[best])) best = c;
            }
            shares[best] += point.weight;
        }
        std::vector<Proposal> out;
        for (size_t i = 0; i < kept.size(); i++) out.push_back({kept[i], shares[i] / double(colors.size())});
        std::stable_sort(out.begin(), out.end(), [](const Proposal& x, const Proposal& y) { return y.share < x.share; });
        return out;
    }

}
