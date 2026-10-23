// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Synthesizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace jasperengine::editor::texsynth::detail {

    namespace {

        using Clock = std::chrono::steady_clock;

        double secondsSince(Clock::time_point start) {
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        uint64_t splitmix64(uint64_t x) {
            x += 0x9e3779b97f4a7c15ULL;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
            return x ^ (x >> 31);
        }

        // PCG32 (O'Neill's pcg32_random_r), as upstream's rand_pcg::Pcg32
        struct Pcg32 {
            uint64_t state = 0;
            uint64_t inc = 0;

            explicit Pcg32(uint64_t seed) {
                inc = (splitmix64(seed ^ 0x2545f4914f6cdd1dULL) << 1) | 1u;
                next();
                state += seed;
                next();
            }
            uint32_t next() {
                const uint64_t old = state;
                state = old * 6364136223846793005ULL + inc;
                const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
                const uint32_t rot = static_cast<uint32_t>(old >> 59u);
                return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
            }
            // uniform in [0, n) (multiply-shift: deterministic, a negligible bias)
            uint32_t below(uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(next()) * n) >> 32); }
        };

        // Persistent workers for the batches: parallelFor wakes them, runs chunks on the calling
        // thread too, and returns when every chunk is done. Which thread runs which chunk does
        // not matter: a chunk writes only its own result slots.
        class WorkerPool {
        public:
            using Task = std::function<void(size_t begin, size_t end, int worker)>;

            explicit WorkerPool(int threads) {
                for (int i = 1; i < threads; i++) workers.emplace_back([this, i] { loop(i); });
            }
            ~WorkerPool() {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    quit = true;
                }
                wake.notify_all();
                for (std::thread& t : workers) t.join();
            }
            int threadCount() const { return static_cast<int>(workers.size()) + 1; }

            void parallelFor(size_t count, size_t chunk, const Task& task) {
                if (workers.empty() || count <= chunk) {
                    for (size_t b = 0; b < count; b += chunk) task(b, std::min(count, b + chunk), 0);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    current = &task;
                    total = count;
                    chunkSize = chunk;
                    next.store(0, std::memory_order_relaxed);
                    active = static_cast<int>(workers.size());
                    generation++;
                }
                wake.notify_all();
                work(0);
                std::unique_lock<std::mutex> lock(mutex);
                done.wait(lock, [this] { return active == 0; });
                current = nullptr;
            }

        private:
            void work(int worker) {
                for (;;) {
                    const size_t b = next.fetch_add(chunkSize, std::memory_order_relaxed);
                    if (b >= total) break;
                    (*current)(b, std::min(total, b + chunkSize), worker);
                }
            }
            void loop(int worker) {
                uint64_t seen = 0;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [&] { return quit || generation != seen; });
                        if (quit) return;
                        seen = generation;
                    }
                    work(worker);
                    std::lock_guard<std::mutex> lock(mutex);
                    if (--active == 0) done.notify_one();
                }
            }

            std::vector<std::thread> workers;
            std::mutex mutex;
            std::condition_variable wake;
            std::condition_variable done;
            const Task* current = nullptr;
            size_t total = 0;
            size_t chunkSize = 1;
            std::atomic<size_t> next{0};
            int active = 0;
            uint64_t generation = 0;
            bool quit = false;
        };

        const uint8_t kOutside[4] = {0, 0, 0, 255};   // upstream's outside colour

        // Z-order of a pixel: nearby pixels get nearby codes
        uint32_t morton(uint32_t x, uint32_t y) {
            auto spread = [](uint32_t v) {
                v &= 0xFFFFu;
                v = (v | (v << 8)) & 0x00FF00FFu;
                v = (v | (v << 4)) & 0x0F0F0F0Fu;
                v = (v | (v << 2)) & 0x33333333u;
                v = (v | (v << 1)) & 0x55555555u;
                return v;
            };
            return spread(x) | (spread(y) << 1);
        }

        class Resolver {
        public:
            Resolver(const CoreJob& coreJob, const Control& coreControl)
                : job(coreJob), control(coreControl), W(job.width), H(job.height), N(static_cast<size_t>(job.width) * job.height),
                  k(job.params.nearestNeighbors), m(job.params.randomSampleLocations) {}

            bool run(CoreResult& out);

        private:
            struct Candidate {
                int x;
                int y;
                int map;
            };
            struct Scratch {
                std::vector<uint64_t> keys;
                std::vector<KnnGrid::Neighbor> neighbors;
                std::vector<uint32_t> neighborIndex;
                std::vector<float> weights;
                std::vector<uint8_t> pattern;
                std::vector<uint8_t> guidePattern;
                std::vector<Candidate> candidates;
                // byte offsets of the neighbours in each example (filled per pixel when needed)
                std::vector<int> offsets;
                std::vector<uint32_t> offsetsStamp;
                uint32_t stamp = 0;
            };
            struct Slot {
                SourceCoord src;
                float uncertainty;
            };

            void setLevel(int newLevel);
            void setGuideWeight(float w);
            SourceCoord randomSample(Pcg32& rng) const;
            void resolvePixel(uint32_t pixel, uint64_t pixelSeed, Scratch& s, Slot& slot) const;
            void report(int stage, int stageCount, int64_t stageDone, int64_t stageTotal, int64_t doneBefore, bool force);
            std::shared_ptr<const Image> preview() const;

            const CoreJob& job;
            const Control& control;
            const int W;
            const int H;
            const size_t N;
            const int k;
            const int m;

            int level = -1;
            std::vector<const uint8_t*> levelPixels;   // per example, current level RGBA8
            std::vector<const uint8_t*> guidePixels;   // per example (null without guides)
            const uint8_t* targetGuide = nullptr;
            bool guideActive = false;
            bool alphaMatters = true;   // false: every alpha is 255, its cost always 0 (skipped, same sums)
            // costs by signed difference a - b: table[255 + a - b]
            float mainCost[511];
            float guideCost[511];
            float cauchyCost[511];

            std::vector<SourceCoord> coord;
            std::vector<uint8_t> colour;
            std::vector<uint8_t> resolved;
            std::vector<float> uncertainty;
            KnnGrid fine;
            KnnGrid coarse;
            bool useCoarse = false;
            // dense stages: offsets sorted by the k-NN key, tested against `resolved`
            bool useSpiral = false;
            DenseKnn spiral;

            std::vector<std::vector<uint32_t>> validSamples;   // per example: flat indices that may be copied
            std::vector<int> sampleableMaps;

            Clock::time_point started;
            Clock::time_point lastProgress;
            Clock::time_point lastPreview;
            int64_t overallTotal = 1;
        };

        void Resolver::setLevel(int newLevel) {
            level = newLevel;
            levelPixels.assign(job.examples.size(), nullptr);
            guidePixels.assign(job.examples.size(), nullptr);
            for (size_t e = 0; e < job.examples.size(); e++) {
                const CoreExample& ex = job.examples[e];
                levelPixels[e] = ex.levels[std::min<size_t>(level, ex.levels.size() - 1)].data.data();
                if (!ex.guideLevels.empty()) guidePixels[e] = ex.guideLevels[std::min<size_t>(level, ex.guideLevels.size() - 1)].data.data();
            }
            targetGuide = job.targetGuideLevels.empty()
                              ? nullptr
                              : job.targetGuideLevels[std::min<size_t>(level, job.targetGuideLevels.size() - 1)].data.data();
            // every resolved pixel takes its colour from the new level (upstream next_pyramid_level)
            for (size_t p = 0; p < N; p++) {
                if (!resolved[p]) continue;
                const SourceCoord c = coord[p];
                const CoreExample& ex = job.examples[c.example];
                std::memcpy(&colour[p * 4], levelPixels[c.example] + (static_cast<size_t>(c.y) * ex.width + c.x) * 4, 4);
            }
        }

        void Resolver::setGuideWeight(float w) {
            const float sigma = job.params.cauchyDispersion;
            const float sig2 = sigma * sigma;
            guideActive = w > 0.0f && targetGuide;
            for (int d = 0; d < 256; d++) {
                const float x = static_cast<float>(d) / 255.0f;
                const float x2 = x * x;
                const float cauchy = std::log1p(x2 / sig2);
                cauchyCost[255 + d] = cauchyCost[255 - d] = cauchy;
                mainCost[255 + d] = mainCost[255 - d] = guideActive ? (1.0f - w) * cauchy : cauchy;
                guideCost[255 + d] = guideCost[255 - d] = guideActive ? w * x2 : 0.0f;
            }
        }

        SourceCoord Resolver::randomSample(Pcg32& rng) const {
            const int map = sampleableMaps.size() == 1 ? sampleableMaps[0] : sampleableMaps[rng.below(static_cast<uint32_t>(sampleableMaps.size()))];
            const CoreExample& ex = job.examples[map];
            SourceCoord c;
            c.example = static_cast<uint16_t>(map);
            if (ex.allowed.empty()) {
                // without a mask every pixel is valid: no table, no division
                c.x = static_cast<uint16_t>(rng.below(static_cast<uint32_t>(ex.width)));
                c.y = static_cast<uint16_t>(rng.below(static_cast<uint32_t>(ex.height)));
            } else {
                const uint32_t index = validSamples[map][rng.below(static_cast<uint32_t>(validSamples[map].size()))];
                c.x = static_cast<uint16_t>(index % ex.width);
                c.y = static_cast<uint16_t>(index / ex.width);
            }
            return c;
        }

        void Resolver::resolvePixel(uint32_t pixel, uint64_t pixelSeed, Scratch& s, Slot& slot) const {
            const int px = static_cast<int>(pixel % W);
            const int py = static_cast<int>(pixel / W);
            Pcg32 rng(pixelSeed);

            // the same neighbours whichever structure answers (exact, same tie-break): only the speed differs
            int n = useSpiral ? spiral.query(resolved.data(), W, H, job.tiling, px, py, k, s.neighbors.data()) : -1;
            if (n < 0) n = (useCoarse ? coarse : fine).query(px, py, k, s.keys, s.neighbors.data());
            if (n == 0) {
                // nothing resolved yet: a random source pixel
                slot.src = randomSample(rng);
                slot.uncertainty = 0.0f;
                return;
            }

            // distance weights: exp(-d^2 / mean d^2), nearest first
            double sum = 0.0;
            for (int j = 0; j < n; j++) sum += s.neighbors[j].d2;
            const double mean = sum / n;
            float weightSum = 0.0f;
            for (int j = 0; j < n; j++) {
                const float w = mean > 0.0 ? static_cast<float>(std::exp(-static_cast<double>(s.neighbors[j].d2) / mean)) : 1.0f;
                s.weights[j] = w;
                weightSum += w;
            }

            // the output neighbourhood (wrapped in tiling mode), and the box of the offsets
            int minDx = 0, maxDx = 0, minDy = 0, maxDy = 0;
            for (int j = 0; j < n; j++) {
                minDx = std::min(minDx, s.neighbors[j].dx);
                maxDx = std::max(maxDx, s.neighbors[j].dx);
                minDy = std::min(minDy, s.neighbors[j].dy);
                maxDy = std::max(maxDy, s.neighbors[j].dy);
                int nx = px + s.neighbors[j].dx;
                int ny = py + s.neighbors[j].dy;
                if (job.tiling) {
                    nx = ((nx % W) + W) % W;
                    ny = ((ny % H) + H) % H;
                }
                const uint32_t index = static_cast<uint32_t>(ny) * W + nx;
                s.neighborIndex[j] = index;
                std::memcpy(&s.pattern[j * 4], &colour[static_cast<size_t>(index) * 4], 4);
                if (guideActive) std::memcpy(&s.guidePattern[j * 4], targetGuide + static_cast<size_t>(index) * 4, 4);
            }

            // candidates: every neighbour's source shifted by its offset (coherence), then m random.
            // Both compare the neighbourhood at candidate + (neighbour - pixel); upstream compared
            // random candidates at candidate - (neighbour - pixel), the point-mirrored pattern (a
            // sign slip in find_candidates). Duplicate coherence candidates are scored once (a
            // later duplicate could never win: ties keep the first).
            int count = 0;
            for (int j = 0; j < n; j++) {
                const SourceCoord c = coord[s.neighborIndex[j]];
                const int map = c.example;
                const CoreExample& ex = job.examples[map];
                if (!ex.sampleable) continue;
                const int cx = static_cast<int>(c.x) - s.neighbors[j].dx;
                const int cy = static_cast<int>(c.y) - s.neighbors[j].dy;
                if (cx < 0 || cy < 0 || cx >= ex.width || cy >= ex.height) continue;
                if (!ex.allowed.empty() && !ex.allowed[static_cast<size_t>(cy) * ex.width + cx]) continue;
                bool duplicate = false;
                for (int i = 0; i < count && !duplicate; i++) {
                    duplicate = s.candidates[i].x == cx && s.candidates[i].y == cy && s.candidates[i].map == map;
                }
                if (!duplicate) s.candidates[count++] = {cx, cy, map};
            }
            for (int r = 0; r < m; r++) {
                const SourceCoord c = randomSample(rng);
                s.candidates[count++] = {c.x, c.y, c.example};
            }

            // the cheapest candidate neighbourhood (early exit once a candidate can't win)
            const float* mc = mainCost + 255;
            const float* gc = guideCost + 255;
            const bool alpha = alphaMatters;
            const bool guided = guideActive;
            auto neighbourCost = [&](const uint8_t* pat, const uint8_t* pix, const uint8_t* gpat, const uint8_t* g) {
                float cost = mc[pat[0] - pix[0]] + mc[pat[1] - pix[1]] + mc[pat[2] - pix[2]];
                if (alpha) cost += mc[pat[3] - pix[3]];
                if (guided) {
                    cost += gc[gpat[0] - g[0]] + gc[gpat[1] - g[1]] + gc[gpat[2] - g[2]];
                    if (alpha) cost += gc[gpat[3] - g[3]];
                }
                return cost;
            };
            s.stamp++;
            float best = std::numeric_limits<float>::max();
            int bestIndex = 0;
            for (int i = 0; i < count; i++) {
                const Candidate& cand = s.candidates[i];
                const CoreExample& ex = job.examples[cand.map];
                const uint8_t* image = levelPixels[cand.map];
                const uint8_t* guide = guidePixels[cand.map];
                float score = 0.0f;
                bool rejected = false;
                if (cand.x + minDx >= 0 && cand.y + minDy >= 0 && cand.x + maxDx < ex.width && cand.y + maxDy < ex.height) {
                    // the whole neighbourhood inside the example: precomputed offsets, no bounds checks
                    int* offsets = &s.offsets[static_cast<size_t>(cand.map) * k];
                    if (s.offsetsStamp[cand.map] != s.stamp) {
                        s.offsetsStamp[cand.map] = s.stamp;
                        for (int j = 0; j < n; j++) offsets[j] = (s.neighbors[j].dy * ex.width + s.neighbors[j].dx) * 4;
                    }
                    const size_t base = (static_cast<size_t>(cand.y) * ex.width + cand.x) * 4;
                    const uint8_t* centre = image + base;
                    const uint8_t* guideCentre = guide ? guide + base : nullptr;
                    for (int j = 0; j < n; j++) {
                        const float cost = neighbourCost(&s.pattern[j * 4], centre + offsets[j], &s.guidePattern[j * 4],
                                                         guided ? guideCentre + offsets[j] : kOutside);
                        score += cost * s.weights[j];
                        if (score >= best) {
                            rejected = true;
                            break;
                        }
                    }
                } else {
                    for (int j = 0; j < n; j++) {
                        const int qx = cand.x + s.neighbors[j].dx;
                        const int qy = cand.y + s.neighbors[j].dy;
                        const bool inside = qx >= 0 && qy >= 0 && qx < ex.width && qy < ex.height;
                        const size_t offset = inside ? (static_cast<size_t>(qy) * ex.width + qx) * 4 : 0;
                        const float cost = neighbourCost(&s.pattern[j * 4], inside ? image + offset : kOutside, &s.guidePattern[j * 4],
                                                         guided && inside ? guide + offset : kOutside);
                        score += cost * s.weights[j];
                        if (score >= best) {
                            rejected = true;
                            break;
                        }
                    }
                }
                if (!rejected) {
                    best = score;
                    bestIndex = i;
                }
            }

            const Candidate& win = s.candidates[bestIndex];
            float texture = best;
            if (guideActive) {
                // the uncertainty is the example mismatch alone
                const CoreExample& ex = job.examples[win.map];
                const float* cc = cauchyCost + 255;
                texture = 0.0f;
                for (int j = 0; j < n; j++) {
                    const int qx = win.x + s.neighbors[j].dx;
                    const int qy = win.y + s.neighbors[j].dy;
                    const bool inside = qx >= 0 && qy >= 0 && qx < ex.width && qy < ex.height;
                    const uint8_t* pix = inside ? levelPixels[win.map] + (static_cast<size_t>(qy) * ex.width + qx) * 4 : kOutside;
                    const uint8_t* pat = &s.pattern[j * 4];
                    float cost = cc[pat[0] - pix[0]] + cc[pat[1] - pix[1]] + cc[pat[2] - pix[2]];
                    if (alpha) cost += cc[pat[3] - pix[3]];
                    texture += s.weights[j] * cost;
                }
            }
            const double perChannel = weightSum > 0.0f ? texture / (4.0 * weightSum) : 0.0;
            slot.uncertainty = static_cast<float>(job.params.cauchyDispersion * std::sqrt(std::max(0.0, std::expm1(perChannel))));
            slot.src.x = static_cast<uint16_t>(win.x);
            slot.src.y = static_cast<uint16_t>(win.y);
            slot.src.example = static_cast<uint16_t>(win.map);
        }

        std::shared_ptr<const Image> Resolver::preview() const {
            const int maxSize = std::max(1, control.previewMaxSize);
            int pw = W, ph = H;
            if (std::max(W, H) > maxSize) {
                pw = std::max(1, static_cast<int>(static_cast<int64_t>(W) * maxSize / std::max(W, H)));
                ph = std::max(1, static_cast<int>(static_cast<int64_t>(H) * maxSize / std::max(W, H)));
            }
            auto image = std::make_shared<Image>(Image::make(pw, ph, 4, 8));
            for (int y = 0; y < ph; y++) {
                const int sy = static_cast<int>(static_cast<int64_t>(y) * H / ph);
                for (int x = 0; x < pw; x++) {
                    const int sx = static_cast<int>(static_cast<int64_t>(x) * W / pw);
                    const size_t p = static_cast<size_t>(sy) * W + sx;
                    if (resolved[p]) std::memcpy(image->pixel(x, y), &colour[p * 4], 4);
                }
            }
            return image;
        }

        void Resolver::report(int stage, int stageCount, int64_t stageDone, int64_t stageTotal, int64_t doneBefore, bool force) {
            if (!control.onProgress) return;
            const Clock::time_point now = Clock::now();
            const bool due = std::chrono::duration<double>(now - lastProgress).count() >= control.progressIntervalSeconds;
            if (!force && !due) return;
            lastProgress = now;
            Progress progress;
            progress.stage = stage;
            progress.stageCount = stageCount;
            progress.stageFraction = stageTotal > 0 ? static_cast<double>(stageDone) / stageTotal : 1.0;
            progress.fraction = std::min(1.0, static_cast<double>(doneBefore + stageDone) / overallTotal);
            progress.elapsedSeconds = secondsSince(started);
            if (progress.fraction > 0.02) progress.etaSeconds = progress.elapsedSeconds / progress.fraction * (1.0 - progress.fraction);
            if (control.previewIntervalSeconds > 0.0 &&
                (force || std::chrono::duration<double>(now - lastPreview).count() >= control.previewIntervalSeconds)) {
                lastPreview = now;
                progress.preview = preview();
            }
            control.onProgress(progress);
        }

        bool Resolver::run(CoreResult& out) {
            started = Clock::now();
            lastProgress = lastPreview = started - std::chrono::hours(1);
            const Params& params = job.params;

            // sampling sources
            validSamples.resize(job.examples.size());
            for (size_t e = 0; e < job.examples.size(); e++) {
                const CoreExample& ex = job.examples[e];
                if (!ex.sampleable) continue;
                if (ex.allowed.empty()) {
                    sampleableMaps.push_back(static_cast<int>(e));
                    continue;
                }
                for (size_t i = 0; i < ex.allowed.size(); i++) {
                    if (ex.allowed[i]) validSamples[e].push_back(static_cast<uint32_t>(i));
                }
                if (!validSamples[e].empty()) sampleableMaps.push_back(static_cast<int>(e));
            }

            alphaMatters = false;
            auto hasAlpha = [](const Image& image) {
                for (size_t i = 3; i < image.data.size(); i += 4)
                    if (image.data[i] != 255) return true;
                return false;
            };
            for (const CoreExample& ex : job.examples) {
                for (const Image& img : ex.levels) alphaMatters = alphaMatters || hasAlpha(img);
                for (const Image& img : ex.guideLevels) alphaMatters = alphaMatters || hasAlpha(img);
            }
            for (const Image& img : job.targetGuideLevels) alphaMatters = alphaMatters || hasAlpha(img);

            coord.assign(N, SourceCoord());
            colour.assign(N * 4, 0);
            resolved.assign(N, 0);
            uncertainty.assign(N, 0.0f);
            fine.init(W, H, job.tiling, 4);
            coarse.init(W, H, job.tiling, 32);

            // kept pixels: identity into examples[0], resolved from the start and never redone
            std::vector<uint32_t> order;
            order.reserve(N);
            int64_t lockedCount = 0;
            for (size_t p = 0; p < N; p++) {
                if (!job.locked.empty() && job.locked[p]) {
                    coord[p].x = static_cast<uint16_t>(p % W);
                    coord[p].y = static_cast<uint16_t>(p / W);
                    coord[p].example = 0;
                    resolved[p] = 1;
                    fine.insert(static_cast<int>(p % W), static_cast<int>(p / W));
                    coarse.insert(static_cast<int>(p % W), static_cast<int>(p / W));
                    lockedCount++;
                } else {
                    order.push_back(static_cast<uint32_t>(p));
                }
            }
            // the resolution order: a seeded Fisher-Yates shuffle of the unresolved pixels
            Pcg32 shuffle(splitmix64(job.seed));
            for (size_t i = order.size(); i > 1; i--) {
                const size_t j = shuffle.below(static_cast<uint32_t>(i));
                std::swap(order[i - 1], order[j]);
            }

            const int64_t total = static_cast<int64_t>(order.size());
            const std::vector<int64_t> counts = stagePixelCounts(total, params);
            const int stageCount = static_cast<int>(counts.size());
            overallTotal = 0;
            for (int64_t c : counts) overallTotal += c;
            overallTotal = std::max<int64_t>(1, overallTotal);

            WorkerPool pool(std::max(1, job.threads));
            const int threads = pool.threadCount();
            std::vector<Scratch> scratch(threads);
            for (Scratch& s : scratch) {
                s.keys.reserve(4096);
                s.neighbors.resize(k);
                s.neighborIndex.resize(k);
                s.weights.resize(k);
                s.pattern.resize(static_cast<size_t>(k) * 4);
                s.guidePattern.resize(static_cast<size_t>(k) * 4);
                s.candidates.resize(static_cast<size_t>(k) + m);
                s.offsets.resize(job.examples.size() * static_cast<size_t>(k));
                s.offsetsStamp.assign(job.examples.size(), 0);
            }
            constexpr int64_t kMaxBatch = 4096;
            std::vector<Slot> slots(kMaxBatch);
            // a batch is computed in Z-order (its pixels' neighbourhoods share cache lines) and
            // committed in its own order: the order of computing never changes a result
            std::vector<uint64_t> zKeys(kMaxBatch);
            std::vector<uint32_t> computeOrder(kMaxBatch);

            const auto cancelled = [this] { return control.cancel && control.cancel->load(std::memory_order_relaxed); };
            int64_t doneBefore = 0;
            int64_t resolvedBefore = 0;   // unlocked pixels resolved by the previous stages
            out.stageSeconds.clear();
            for (int stage = 0; stage < stageCount; stage++) {
                const Clock::time_point stageStart = Clock::now();
                const int64_t count = counts[stage];
                const int newLevel = std::min(stage, std::max(1, job.pyramidLevels) - 1);
                if (newLevel != level) setLevel(newLevel);
                // guide weight (upstream: (alpha * (1 - resolved share))^3, none in the last stage)
                float w = 0.0f;
                if (!job.targetGuideLevels.empty()) {
                    const double f = total > 0 ? static_cast<double>(resolvedBefore) / total : 0.0;
                    if (!params.guideDecay) w = params.guideAlpha;
                    else if (stage < stageCount - 1) w = static_cast<float>(std::pow(params.guideAlpha * (1.0 - f), 3.0));
                }
                setGuideWeight(w);
                const uint64_t stageSeed = splitmix64(job.seed ^ splitmix64(0x7374616765ULL + static_cast<uint64_t>(stage)));

                report(stage, stageCount, 0, count, doneBefore, true);
                int64_t pos = 0;
                while (pos < count) {
                    if (cancelled()) return false;
                    // a batch reads what the batches before it committed: its size grows with the
                    // resolved pixels, so a pixel misses few neighbours of its own batch (1/32: the
                    // mean uncertainty matched one-pixel batches within run-to-run noise)
                    const int64_t committed = lockedCount + std::max(pos, resolvedBefore);
                    const int64_t batch = std::clamp<int64_t>(committed / 32, 1, kMaxBatch);
                    const int64_t end = std::min(count, pos + batch);
                    const size_t size = static_cast<size_t>(end - pos);
                    useCoarse = fine.size() * 32 < static_cast<int64_t>(N);
                    useSpiral = fine.size() * 8 >= static_cast<int64_t>(N);

                    for (size_t b = 0; b < size; b++) {
                        const uint32_t p = order[pos + static_cast<int64_t>(b)];
                        zKeys[b] = (static_cast<uint64_t>(morton(p % W, p / W)) << 32) | b;
                    }
                    std::sort(zKeys.begin(), zKeys.begin() + static_cast<std::ptrdiff_t>(size));
                    for (size_t b = 0; b < size; b++) computeOrder[b] = static_cast<uint32_t>(zKeys[b]);

                    const auto task = [&](size_t begin, size_t stop, int worker) {
                        if (cancelled()) return;
                        Scratch& s = scratch[worker];
                        for (size_t i = begin; i < stop; i++) {
                            const size_t b = computeOrder[i];
                            const int64_t position = pos + static_cast<int64_t>(b);
                            resolvePixel(order[position], splitmix64(stageSeed + static_cast<uint64_t>(position)), s, slots[b]);
                        }
                    };
                    if (size < static_cast<size_t>(threads) * 8) {
                        task(0, size, 0);   // too small to share
                    } else {
                        pool.parallelFor(size, 16, task);
                    }
                    if (cancelled()) return false;

                    // commit, in order
                    for (size_t b = 0; b < size; b++) {
                        const uint32_t p = order[pos + static_cast<int64_t>(b)];
                        const SourceCoord c = slots[b].src;
                        coord[p] = c;
                        const CoreExample& ex = job.examples[c.example];
                        std::memcpy(&colour[static_cast<size_t>(p) * 4], levelPixels[c.example] + (static_cast<size_t>(c.y) * ex.width + c.x) * 4, 4);
                        uncertainty[p] = slots[b].uncertainty;
                        if (!resolved[p]) {
                            resolved[p] = 1;
                            fine.insert(static_cast<int>(p % W), static_cast<int>(p / W));
                            coarse.insert(static_cast<int>(p % W), static_cast<int>(p / W));
                        }
                    }
                    pos = end;
                    report(stage, stageCount, pos, count, doneBefore, false);
                }
                doneBefore += count;
                resolvedBefore = std::max(resolvedBefore, count);
                out.stageSeconds.push_back(secondsSince(stageStart));
                report(stage, stageCount, count, count, doneBefore - count, true);
            }

            // the final colours come from the full-resolution examples
            if (level != std::max(1, job.pyramidLevels) - 1) setLevel(std::max(1, job.pyramidLevels) - 1);
            out.coords = std::move(coord);
            out.uncertainty = std::move(uncertainty);
            out.rgba = Image::make(W, H, 4, 8);
            out.rgba.data = std::move(colour);
            return true;
        }

    }

    std::vector<int64_t> stagePixelCounts(int64_t total, const Params& params) {
        const int stages = std::max(0, params.backtrackStages);
        std::vector<int64_t> counts(stages + 1, 0);
        for (int i = 0; i <= stages; i++) {
            const int s = stages - i;
            counts[i] = s == 0 ? total : static_cast<int64_t>(std::pow(static_cast<double>(params.backtrackPercent), s) * static_cast<double>(total));
            if (i > 0) counts[i] = std::max(counts[i], counts[i - 1]);
        }
        return counts;
    }

    // ---- KnnGrid

    void KnnGrid::init(int w, int h, bool wrap, int cellSize) {
        width = w;
        height = h;
        tiling = wrap;
        cell = std::max(1, cellSize);
        gridW = (w + cell - 1) / cell;
        gridH = (h + cell - 1) / cell;
        // a narrower last cell can shorten a wrapped path by its missing width
        slack = wrap ? std::max(cell - (w - (gridW - 1) * cell), cell - (h - (gridH - 1) * cell)) : 0;
        count = 0;
        start.assign(static_cast<size_t>(gridW) * gridH, 0);
        used.assign(static_cast<size_t>(gridW) * gridH, 0);
        uint32_t offset = 0;
        for (int cy = 0; cy < gridH; cy++) {
            for (int cx = 0; cx < gridW; cx++) {
                start[static_cast<size_t>(cy) * gridW + cx] = offset;
                const int cw = std::min(cell, w - cx * cell);
                const int ch = std::min(cell, h - cy * cell);
                offset += static_cast<uint32_t>(cw * ch);
            }
        }
        points.assign(offset, 0);
    }

    void KnnGrid::insert(int x, int y) {
        const size_t c = static_cast<size_t>(y / cell) * gridW + x / cell;
        points[start[c] + used[c]++] = static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 16);
        count++;
    }

    int KnnGrid::query(int qx, int qy, int k, std::vector<uint64_t>& keys, Neighbor* out) const {
        keys.clear();
        if (k <= 0 || count == 0) return 0;
        const int cx = qx / cell, cy = qy / cell;
        // tiling: each cell once, at its offset in [lo, hi]
        const int loX = tiling ? -((gridW - 1) / 2) : -cx;
        const int hiX = tiling ? gridW / 2 : gridW - 1 - cx;
        const int loY = tiling ? -((gridH - 1) / 2) : -cy;
        const int hiY = tiling ? gridH / 2 : gridH - 1 - cy;
        const int maxR = std::max(std::max(-loX, hiX), std::max(-loY, hiY));
        const int halfW = width / 2, halfH = height / 2;
        const size_t kk = static_cast<size_t>(k);
        // the k-th smallest key so far: a point above it can't be one of the k nearest (keys are
        // unique: distance, then the offset)
        uint64_t limit = std::numeric_limits<uint64_t>::max();

        auto visit = [&](int ox, int oy) {
            if (ox < loX || ox > hiX || oy < loY || oy > hiY) return;
            int gx = cx + ox, gy = cy + oy;
            if (tiling) {
                gx = ((gx % gridW) + gridW) % gridW;
                gy = ((gy % gridH) + gridH) % gridH;
            }
            const size_t c = static_cast<size_t>(gy) * gridW + gx;
            const uint32_t* p = points.data() + start[c];
            const uint32_t n = used[c];
            for (uint32_t i = 0; i < n; i++) {
                int dx = static_cast<int>(p[i] & 0xFFFFu) - qx;
                int dy = static_cast<int>(p[i] >> 16) - qy;
                if (tiling) {
                    if (dx < -halfW) dx += width;
                    else if (dx >= width - halfW) dx -= width;
                    if (dy < -halfH) dy += height;
                    else if (dy >= height - halfH) dy -= height;
                }
                const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy);
                const uint64_t key = (d2 << 32) | (static_cast<uint64_t>(dy + 32768) << 16) | static_cast<uint64_t>(dx + 32768);
                if (key < limit) keys.push_back(key);
            }
        };
        // keep the k smallest keys, and their largest as the limit
        auto prune = [&] {
            if (keys.size() < kk) return;
            if (keys.size() > kk) {
                std::nth_element(keys.begin(), keys.begin() + (k - 1), keys.end());
                keys.resize(kk);
            }
            limit = *std::max_element(keys.begin(), keys.end());
        };

        for (int r = 0; r <= maxR; r++) {
            if (keys.size() >= kk && r >= 1) {
                // every point of ring r is at least this far away
                const int64_t bound = static_cast<int64_t>(r - 1) * cell + 1 - slack;
                if (bound > 0 && static_cast<uint64_t>(bound * bound) > (limit >> 32)) break;
            }
            if (r == 0) {
                visit(0, 0);
            } else {
                for (int ox = -r; ox <= r; ox++) {
                    visit(ox, -r);
                    visit(ox, r);
                }
                for (int oy = -r + 1; oy <= r - 1; oy++) {
                    visit(-r, oy);
                    visit(r, oy);
                }
            }
            prune();
        }

        std::sort(keys.begin(), keys.end());
        const int size = static_cast<int>(std::min(keys.size(), kk));
        for (int i = 0; i < size; i++) {
            const uint64_t key = keys[i];
            out[i].d2 = static_cast<int>(key >> 32);
            out[i].dy = static_cast<int>((key >> 16) & 0xFFFFu) - 32768;
            out[i].dx = static_cast<int>(key & 0xFFFFu) - 32768;
        }
        return size;
    }

    // ---- DenseKnn

    DenseKnn::DenseKnn(int radius) {
        std::vector<uint64_t> keys;
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                // a disc: every offset left out is farther than every offset kept
                const uint64_t d2 = static_cast<uint64_t>(dx * dx + dy * dy);
                if (d2 > static_cast<uint64_t>(radius * radius)) continue;
                keys.push_back((d2 << 32) | (static_cast<uint64_t>(dy + 32768) << 16) | static_cast<uint64_t>(dx + 32768));
            }
        }
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys) {
            offsets.push_back({static_cast<int>(key & 0xFFFFu) - 32768, static_cast<int>((key >> 16) & 0xFFFFu) - 32768, static_cast<int>(key >> 32)});
        }
    }

    int DenseKnn::query(const uint8_t* present, int width, int height, bool tiling, int qx, int qy, int k, KnnGrid::Neighbor* out) const {
        const int halfW = width / 2, halfH = height / 2;
        int found = 0;
        for (const KnnGrid::Neighbor& o : offsets) {
            int x = qx + o.dx, y = qy + o.dy;
            if (tiling) {
                // each pixel once: offsets in the minimal-image range only
                if (o.dx < -halfW || o.dx >= width - halfW || o.dy < -halfH || o.dy >= height - halfH) continue;
                if (x < 0) x += width;
                else if (x >= width) x -= width;
                if (y < 0) y += height;
                else if (y >= height) y -= height;
            } else if (x < 0 || y < 0 || x >= width || y >= height) {
                continue;
            }
            if (!present[static_cast<size_t>(y) * width + x]) continue;
            out[found++] = o;
            if (found == k) return found;
        }
        return -1;
    }

    bool runSynthesis(const CoreJob& job, const Control& control, CoreResult& out) {
        Resolver resolver(job, control);
        return resolver.run(out);
    }

}
