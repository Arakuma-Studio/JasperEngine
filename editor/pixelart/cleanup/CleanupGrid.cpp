// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/grid.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupGrid.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <utility>

namespace jasperengine::editor::pixelart::cleanup {

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace grid_ts {

        ParallelFor& parallelHook() {
            static ParallelFor hook;
            return hook;
        }

        void gridParallel(int count, const std::function<void(int)>& body) {
            const ParallelFor& hook = parallelHook();
            if (hook && count > 1) {
                hook(count, body);
                return;
            }
            for (int i = 0; i < count; i++) body(i);
        }

        // Two cell sizes this close (a share of the larger) are one size (grid.ts)
        constexpr double SAME_SIZE = 0.02;
        // Two cell sizes this close are one square cell, stretched (grid.ts): a screenshot or a
        // resize often scales one axis a few percent more than the other
        constexpr double NEAR_SQUARE = 0.1;
        // How much less clean the other axis's size may be and still be chosen (grid.ts)
        constexpr double SQUARE_SLACK = 0.12;
        // How much a cut on a dark stroke costs, in units of a typical edge's worth (grid.ts)
        constexpr double CROSSING_COST = 4;
        // How much a gap a share s off the expected cell costs: SPACING_COST * s * s (grid.ts)
        constexpr double SPACING_COST = 20;
        // Rows (columns) of cells on each side whose edges also place a bent line's cuts (grid.ts)
        constexpr int BEND_SPAN = 1;

        int difference(const uint8_t* data, size_t i, size_t j) {
            return std::abs(int(data[i]) - int(data[j])) + std::abs(int(data[i + 1]) - int(data[j + 1])) +
                   std::abs(int(data[i + 2]) - int(data[j + 2])) + std::abs(int(data[i + 3]) - int(data[j + 3]));
        }

        // The profile blurred by [1, 2, 1] / 4
        std::vector<double> smoothed(const std::vector<double>& profile) {
            const size_t n = profile.size();
            std::vector<double> out(n);
            for (size_t x = 0; x < n; x++) {
                const double before = x > 0 ? profile[x - 1] : 0.0;
                const double after = x + 1 < n ? profile[x + 1] : 0.0;
                out[x] = (before + 2 * profile[x] + after) / 4;
            }
            return out;
        }

        // The profile at a fractional position, read linearly between boundaries
        double at(const std::vector<double>& profile, double position) {
            if (position < 0 || position > double(profile.size()) - 1) return 0;
            const size_t i = size_t(std::floor(position));
            const double f = position - double(i);
            const double next = i + 1 < profile.size() ? profile[i + 1] : 0.0;
            return profile[i] * (1 - f) + next * f;
        }

        // The mean edge strength on the lines of a grid with this cell and offset
        double onLines(const std::vector<double>& profile, double length, double cell, double offset) {
            double sum = 0;
            int count = 0;
            for (int k = 0;; k++) {
                const double position = offset + k * cell;
                if (position >= length - 0.5) break;
                if (position > 0.5) {
                    sum += at(profile, position);
                    count++;
                }
            }
            return count == 0 ? 0 : sum / count;
        }

        struct Candidate {
            double cell = 0, offset = 0, score = -1;
        };

        Candidate bestOffset(const std::vector<double>& profile, double length, double cell, double from, double to, double step) {
            Candidate best{cell, 0, -1};
            for (double offset = from; offset < to; offset += step) {
                const double score = onLines(profile, length, cell, offset);
                if (score > best.score) best = {cell, offset, score};
            }
            return best;
        }

        // How strongly the profile repeats with this period: its Fourier component's magnitude
        double strength(const std::vector<double>& centered, double cell) {
            double re = 0, im = 0;
            const double step = (2 * std::numbers::pi) / cell;
            for (size_t x = 1; x + 1 < centered.size(); x++) {
                const double value = centered[x];
                if (value == 0) continue;
                re += value * std::cos(step * double(x));
                im += value * std::sin(step * double(x));
            }
            return std::hypot(re, im);
        }

        // JavaScript's ((a % b) + b) % b on doubles
        double positiveMod(double a, double b) { return std::fmod(std::fmod(a, b) + b, b); }

        // The mean edge strength inside the cells (away from their lines), within from..to
        double insideCells(const std::vector<double>& profile, int from, int to, double cell, double offset) {
            const double reach = std::min(std::max(1.0, 0.15 * cell), cell / 2 - 0.5);
            double sum = 0;
            int count = 0;
            for (int x = from; x <= to; x++) {
                const double phase = positiveMod(x - offset, cell);
                if (std::min(phase, cell - phase) <= reach) continue;
                sum += profile[size_t(x)];
                count++;
            }
            return count == 0 ? 0 : sum / count;
        }

        // How often the middle half of a cell holds an edge, per line across and cell (grid.ts)
        double edgesInside(const RgbaImage& image, Axis axis, double cell, double offset, double threshold) {
            const int length = axis == Axis::X ? image.width() : image.height();
            const int across = axis == Axis::X ? image.height() : image.width();
            const size_t step = axis == Axis::X ? 4 : size_t(image.width()) * 4;
            const size_t lineStep = axis == Axis::X ? size_t(image.width()) * 4 : 4;
            const uint8_t* data = image.pixels().data();
            const std::vector<double> cuts = uniformCuts(length, cell, offset);
            long long crossed = 0, count = 0;
            for (size_t i = 0; i + 1 < cuts.size(); i++) {
                const double from = cuts[i];
                const double to = cuts[i + 1];
                const int first = int(std::ceil(from + (to - from) * 0.25 - 0.5));
                const int last = std::min(length - 1, std::max(first + 1, int(std::floor(to - (to - from) * 0.25 - 0.5))));
                for (int line = 0; line < across; line++) {
                    for (int a = first + 1; a <= last; a++) {
                        const size_t p = size_t(line) * lineStep + size_t(a) * step;
                        if (difference(data, p, p - step) > threshold) {
                            crossed++;
                            break;
                        }
                    }
                    count++;
                }
            }
            return count == 0 ? 0 : double(crossed) / double(count);
        }

        // A step between neighbouring pixels larger than this is an edge, not noise (grid.ts)
        double edgeThreshold(const RgbaImage& image) {
            std::vector<int> steps;
            const long long pixels = (long long)image.width() * image.height();
            const long long stride = std::max<long long>(1, pixels / 20000);
            const uint8_t* data = image.pixels().data();
            for (long long i = 0; i < pixels; i += stride) {
                if (i % image.width() == 0) continue;
                steps.push_back(difference(data, size_t(i) * 4, size_t(i) * 4 - 4));
            }
            std::sort(steps.begin(), steps.end());
            const int median = steps.empty() ? 0 : steps[steps.size() / 2];
            return std::max(24.0, 3.0 * median);
        }

        // The edge profile of one axis over a band of lines only (rows from..to for X)
        std::vector<double> bandProfile(const RgbaImage& image, Axis axis, int from, int to) {
            const int width = image.width();
            const int length = axis == Axis::X ? image.width() : image.height();
            std::vector<double> out(size_t(length) + 1, 0.0);
            const size_t stride = size_t(width) * 4;
            const int lines = std::max(1, to - from);
            const uint8_t* data = image.pixels().data();
            for (int a = 1; a < length; a++) {
                double sum = 0;
                for (int line = from; line < to; line++) {
                    const size_t i = axis == Axis::X ? size_t(line) * stride + size_t(a) * 4 : size_t(a) * stride + size_t(line) * 4;
                    sum += difference(data, i, axis == Axis::X ? i - 4 : i - stride);
                }
                out[size_t(a)] = sum / lines;
            }
            return out;
        }

        // refineCuts without joining slivers: always as many cuts as given
        std::vector<double> placeCuts(const std::vector<double>& profile, const std::vector<double>& cuts, double length,
                                      const std::vector<double>* crossings) {
            std::vector<int> movable;
            for (size_t i = 0; i < cuts.size(); i++) {
                if (cuts[i] > 0 && cuts[i] < length) movable.push_back(int(i));
            }
            if (movable.size() < 2) return cuts;
            // Smoothed, so a blurred edge (a plateau a few pixels wide) peaks in its middle
            const std::vector<double> smooth = smoothed(profile);
            double mean = 0;
            for (int x = 1; x < length; x++) mean += smooth[size_t(x)];
            mean = mean / std::max(1.0, length - 1);
            if (mean == 0 || std::isnan(mean)) mean = 1;
            const int first = movable.front();
            const int last = movable.back();
            const double spacing = (cuts[size_t(last)] - cuts[size_t(first)]) / (last - first);
            const double step = std::max(0.25, spacing / 40);
            const double reach = spacing / 2;
            auto gain = [&](double position) {
                return std::min(at(smooth, position) / mean, 6.0) - (crossings == nullptr ? 0.0 : CROSSING_COST * at(*crossings, position));
            };
            std::vector<std::vector<double>> candidates;
            candidates.reserve(movable.size());
            for (int i : movable) {
                std::vector<double> list;
                const double cut = cuts[size_t(i)];
                for (double position = cut - reach; position <= cut + reach + 1e-9; position += step) {
                    if (position > 0.5 && position < length - 0.5) list.push_back(position);
                }
                if (list.empty()) list.push_back(cut);
                candidates.push_back(std::move(list));
            }
            std::vector<double> score(candidates[0].size());
            for (size_t b = 0; b < score.size(); b++) score[b] = gain(candidates[0][b]);
            std::vector<std::vector<int>> back;
            back.emplace_back(candidates[0].size(), -1);
            for (size_t k = 1; k < candidates.size(); k++) {
                const std::vector<double>& here = candidates[k];
                const std::vector<double>& before = candidates[k - 1];
                const int gaps = movable[k] - movable[k - 1];
                std::vector<double> next(here.size(), -std::numeric_limits<double>::infinity());
                std::vector<int> from(here.size(), -1);
                for (size_t b = 0; b < here.size(); b++) {
                    const double position = here[b];
                    for (size_t a = 0; a < before.size(); a++) {
                        const double gap = (position - before[a]) / gaps;
                        if (gap < spacing / 2) continue;
                        const double off = (gap - spacing) / spacing;
                        const double value = score[a] - SPACING_COST * (off * off);
                        if (value > next[b]) {
                            next[b] = value;
                            from[b] = int(a);
                        }
                    }
                    next[b] += gain(position);
                }
                score = std::move(next);
                back.push_back(std::move(from));
            }
            size_t pick = 0;
            for (size_t b = 1; b < score.size(); b++) {
                if (score[b] > score[pick]) pick = b;
            }
            std::vector<double> out = cuts;
            for (size_t k = candidates.size(); k-- > 0;) {
                out[size_t(movable[k])] = jsRound(candidates[k][pick] * 100) / 100;
                pick = size_t(std::max(0, back[k][pick]));
            }
            return out;
        }

        // Bent lines for one axis: in every band of cells, the cuts placed from that band's edges and
        // its BEND_SPAN neighbours'; result[i][j] is cut i in band j
        std::vector<std::vector<double>> bendCuts(const std::vector<std::vector<double>>& profiles,
                                                  const std::vector<std::vector<double>>* crossings,
                                                  const std::vector<double>& cuts, double length) {
            const size_t size = size_t(length) + 1;
            auto sum = [&](const std::vector<std::vector<double>>& list, int j) {
                std::vector<double> out(size, 0.0);
                for (int k = std::max(0, j - BEND_SPAN); k <= std::min(int(list.size()) - 1, j + BEND_SPAN); k++) {
                    for (size_t x = 0; x < size; x++) out[x] += list[size_t(k)][x];
                }
                return out;
            };
            std::vector<std::vector<double>> bands(profiles.size());
            gridParallel(int(profiles.size()), [&](int j) {
                const std::vector<double> crossing = crossings == nullptr ? std::vector<double>() : sum(*crossings, j);
                bands[size_t(j)] = placeCuts(sum(profiles, j), cuts, length, crossings == nullptr ? nullptr : &crossing);
            });
            std::vector<std::vector<double>> out(cuts.size());
            for (size_t i = 0; i < cuts.size(); i++) {
                out[i].reserve(bands.size());
                for (const std::vector<double>& band : bands) out[i].push_back(band[i]);
            }
            return out;
        }

    }

    void setParallelFor(ParallelFor parallel) { grid_ts::parallelHook() = std::move(parallel); }

    std::array<double, 4> cellBox(const Grid& grid, int x, int y) {
        using namespace grid_ts;
        if (!grid.bend) {
            return {grid.xs[size_t(x)], grid.ys[size_t(y)], grid.xs[size_t(x) + 1], grid.ys[size_t(y) + 1]};
        }
        const GridBend& bend = *grid.bend;
        return {bend.xs[size_t(x)][size_t(y)], bend.ys[size_t(y)][size_t(x)], bend.xs[size_t(x) + 1][size_t(y)], bend.ys[size_t(y) + 1][size_t(x)]};
    }

    std::vector<double> uniformCuts(double length, double cell, double offset) {
        using namespace grid_ts;
        if (!(cell > 0) || length <= 0) return {0, length};
        const double first = positiveMod(offset, cell);
        std::vector<double> cuts;
        if (first >= cell / 2) cuts.push_back(0);
        for (int k = 0;; k++) {
            const double position = first + k * cell;
            if (position > length + 1e-9) break;
            cuts.push_back(position);
        }
        // grid.ts reads `cuts[-1]` here: undefined when the first line is past the picture, and then
        // the comparison is false and the picture is one cell
        if (!cuts.empty() && length - cuts.back() >= cell / 2) cuts.push_back(length);
        // A picture smaller than one cell is one cell
        if (cuts.size() < 2) return {0, length};
        return cuts;
    }

    Grid uniformGrid(int width, int height, const GridSpec& spec) {
        using namespace grid_ts;
        Grid grid;
        grid.xs = uniformCuts(width, spec.cellWidth, spec.offsetX);
        grid.ys = uniformCuts(height, spec.cellHeight, spec.offsetY);
        return grid;
    }

    GridSize gridSize(const Grid& grid) {
        using namespace grid_ts;
        return {std::max(0, int(grid.xs.size()) - 1), std::max(0, int(grid.ys.size()) - 1)};
    }

    EdgeProfiles edgeProfiles(const RgbaImage& image) {
        using namespace grid_ts;
        const int width = image.width();
        const int height = image.height();
        EdgeProfiles out;
        out.columns.assign(size_t(width) + 1, 0.0);
        out.rows.assign(size_t(height) + 1, 0.0);
        const uint8_t* data = image.pixels().data();
        const size_t stride = size_t(width) * 4;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const size_t i = size_t(y) * stride + size_t(x) * 4;
                if (x > 0) out.columns[size_t(x)] += difference(data, i, i - 4);
                if (y > 0) out.rows[size_t(y)] += difference(data, i, i - stride);
            }
        }
        for (double& v : out.columns) v /= std::max(1, height);
        for (double& v : out.rows) v /= std::max(1, width);
        return out;
    }

    std::optional<AxisFit> detectAxis(const RgbaImage& image, Axis axis, const std::vector<double>& profile, double maxCell,
                                      std::optional<double> prefer) {
        using namespace grid_ts;
        const int length = axis == Axis::X ? image.width() : image.height();
        const double top = std::min(maxCell, std::floor(length / 2.0));
        if (top < 2) return std::nullopt;
        double mean = 0;
        for (int x = 1; x < length; x++) mean += profile[size_t(x)];
        mean /= std::max(1, length - 1);
        if (mean == 0) return std::nullopt;

        // Only the part of the picture with something in it counts (a sprite on a wide empty
        // background would otherwise read as one huge repeat), and the strongest edges are capped
        // (the outline against transparency would otherwise drown the rest)
        int first = 1;
        int last = length - 1;
        const double peak = *std::max_element(profile.begin(), profile.end());
        while (first < last && profile[size_t(first)] < 0.02 * peak) first++;
        while (last > first && profile[size_t(last)] < 0.02 * peak) last--;
        std::vector<double> active;
        for (int x = first; x <= last; x++) {
            if (profile[size_t(x)] > 0) active.push_back(profile[size_t(x)]);
        }
        std::sort(active.begin(), active.end());
        const size_t capIndex = size_t(std::floor(double(active.size()) * 0.9));
        const double cap = capIndex < active.size() ? active[capIndex] : peak;
        double activeMean = 0;
        for (int x = first; x <= last; x++) activeMean += std::min(cap, profile[size_t(x)]);
        activeMean /= std::max(1, last - first + 1);
        const int span = last - first + 2;
        std::vector<double> centered(profile.size(), 0.0);
        for (int x = first; x <= last; x++) {
            const double s = std::sin((std::numbers::pi * (x - first + 1)) / span);
            centered[size_t(x)] = (std::min(cap, profile[size_t(x)]) - activeMean) * (s * s);
        }

        // Sampled evenly in frequency, four samples per spectrum bin, from the largest cell to 2
        std::vector<double> cells;
        const double lowest = 1.0 / std::max(2.0, std::min(top, std::floor(span / 2.0)));
        const double fineness = 1.0 / (4.0 * length);
        for (double frequency = lowest; frequency <= 0.5 + 1e-12; frequency += fineness) cells.push_back(1 / frequency);
        std::vector<double> values(cells.size());
        gridParallel(int(cells.size()), [&](int i) { values[size_t(i)] = strength(centered, cells[size_t(i)]); });
        const double highest = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
        if (highest == 0) return std::nullopt;
        struct Sample {
            double cell, value;
        };
        std::vector<Sample> peaks;
        for (size_t i = 0; i < values.size(); i++) {
            const double before = i > 0 ? values[i - 1] : 0.0;
            const double after = i + 1 < values.size() ? values[i + 1] : 0.0;
            if (values[i] >= 0.25 * highest && values[i] >= before && values[i] >= after) peaks.push_back({cells[i], values[i]});
        }
        std::stable_sort(peaks.begin(), peaks.end(), [](const Sample& a, const Sample& b) { return b.value < a.value; });
        if (peaks.size() > 12) peaks.resize(12);
        // The true size's own peak can be weaker than its fractions' (sharp edges repeat at every
        // fraction), so whole multiples of each peak are candidates too
        std::vector<double> sizes;
        if (prefer && *prefer <= top) sizes.push_back(*prefer);
        for (const Sample& p : peaks) {
            for (int k = 1; k <= 4; k++) {
                const double size = p.cell * k;
                if (size > top) break;
                if (std::all_of(sizes.begin(), sizes.end(), [&](double other) { return std::abs(other - size) > 0.01 * size; })) sizes.push_back(size);
            }
        }

        // Lines are scored on a smoothed profile: a sharp edge is one pixel wide, and a line a
        // fraction of a pixel off it would otherwise miss it
        const std::vector<double> smooth = smoothed(profile);
        auto refine = [&](double cell) {
            const Candidate coarse = bestOffset(smooth, length, cell, 0, cell, 0.25);
            Candidate fine = coarse;
            const double reach = std::max(0.05, 0.03 * cell);
            for (double size = std::max(2.0, cell - reach); size <= cell + reach; size += reach / 40) {
                const Candidate candidate = bestOffset(smooth, length, size, coarse.offset - 0.75, coarse.offset + 0.75, 0.05);
                if (candidate.score > fine.score) fine = candidate;
            }
            return fine;
        };

        // Each candidate is judged by how often the middle of its cells holds an edge: the true size
        // almost never, a wrong size often. A fraction of the true size is just as clean but has half
        // its lines mid-cell, so among the sizes about as clean as the cleanest, the strongest lines win.
        const double threshold = edgeThreshold(image);
        struct Judged {
            Candidate candidate;
            double residual = 0;
        };
        std::vector<Judged> judged(sizes.size());
        gridParallel(int(sizes.size()), [&](int i) {
            Judged& entry = judged[size_t(i)];
            entry.candidate = refine(sizes[size_t(i)]);
            entry.residual = edgesInside(image, axis, entry.candidate.cell, entry.candidate.offset, threshold);
        });
        if (judged.empty()) return std::nullopt;
        double flattest = judged[0].residual;
        for (const Judged& entry : judged) flattest = std::min(flattest, entry.residual);
        std::vector<const Judged*> square;
        if (prefer) {
            for (const Judged& entry : judged) {
                if (std::abs(entry.candidate.cell - *prefer) <= NEAR_SQUARE * *prefer && entry.residual <= flattest + SQUARE_SLACK) square.push_back(&entry);
            }
        }
        std::vector<const Judged*> pool = square;
        if (pool.empty()) {
            for (const Judged& entry : judged) {
                if (entry.residual <= flattest + 0.04) pool.push_back(&entry);
            }
        }
        const Judged* best = pool.front();
        for (const Judged* entry : pool) {
            if (entry->candidate.score > best->candidate.score) best = entry;
        }
        const Candidate& fine = best->candidate;
        const double cell = jsRound(fine.cell * 1000) / 1000;
        const double offset = jsRound(positiveMod(fine.offset, cell) * 100) / 100;
        // Sure when the middles of the cells hold no edges and the lines stand out from them
        const double lines = onLines(profile, length, cell, offset);
        const double contrast = lines > 0 ? 1 - insideCells(profile, first, last, cell, offset) / lines : 0;
        const double confidence = std::min(1 - best->residual, contrast);
        AxisFit fit;
        fit.cell = cell;
        fit.offset = offset >= cell ? 0 : offset;
        fit.confidence = std::min(1.0, std::max(0.0, confidence));
        return fit;
    }

    std::optional<Detection> detectGrid(const RgbaImage& image, double maxCell) {
        using namespace grid_ts;
        const EdgeProfiles profiles = edgeProfiles(image);
        std::optional<AxisFit> x = detectAxis(image, Axis::X, profiles.columns, maxCell);
        std::optional<AxisFit> y = detectAxis(image, Axis::Y, profiles.rows, maxCell);
        if (!x || !y) return std::nullopt;
        // Cells are square: when the axes disagree, the less sure one looks again for the surer
        // one's size, or one near it (a fraction of the true size always looks at least as clean)
        if (std::abs(x->cell - y->cell) > SAME_SIZE * std::max(x->cell, y->cell)) {
            if (x->confidence >= y->confidence) {
                const std::optional<AxisFit> again = detectAxis(image, Axis::Y, profiles.rows, maxCell, x->cell);
                if (again && std::abs(again->cell - x->cell) <= NEAR_SQUARE * x->cell) y = again;
            } else {
                const std::optional<AxisFit> again = detectAxis(image, Axis::X, profiles.columns, maxCell, y->cell);
                if (again && std::abs(again->cell - y->cell) <= NEAR_SQUARE * y->cell) x = again;
            }
        }
        Detection detection;
        detection.spec = {x->cell, y->cell, x->offset, y->offset};
        detection.x = *x;
        detection.y = *y;
        detection.confidence = std::min(x->confidence, y->confidence);
        return detection;
    }

    std::vector<double> crossingProfile(const OutlineMask& mask, Axis axis, int from, int to) {
        using namespace grid_ts;
        const int width = mask.width;
        const int length = axis == Axis::X ? mask.width : mask.height;
        if (to < 0) to = axis == Axis::X ? mask.height : mask.width;
        std::vector<double> out(size_t(length) + 1, 0.0);
        const int lines = std::max(1, to - from);
        for (int a = 1; a < length; a++) {
            int count = 0;
            for (int line = from; line < to; line++) {
                const size_t i = axis == Axis::X ? size_t(line) * width + size_t(a) : size_t(a) * width + size_t(line);
                const size_t before = axis == Axis::X ? i - 1 : i - size_t(width);
                if (mask.dark[i] == 1 && mask.dark[before] == 1) count++;
            }
            out[size_t(a)] = double(count) / lines;
        }
        return out;
    }

    std::vector<double> refineCuts(const std::vector<double>& profile, const std::vector<double>& cuts, double length,
                                   const std::vector<double>* crossings) {
        using namespace grid_ts;
        std::vector<double> out = placeCuts(profile, cuts, length, crossings);
        // Moving cuts can leave a sliver at either end of the picture: it joins its neighbour
        if (out.size() > 3 && out[0] == 0 && out[1] < (out[2] - out[1]) / 2) out.erase(out.begin() + 1);
        const size_t n = out.size();
        if (n > 3 && out[n - 1] == length && length - out[n - 2] < (out[n - 2] - out[n - 3]) / 2) out.erase(out.begin() + long(n - 2));
        return out;
    }

    Grid refineGrid(const RgbaImage& image, const Grid& grid, const RefineOptions& options) {
        using namespace grid_ts;
        const OutlineMask* mask = options.outlines;
        if (!options.bend || grid.xs.size() < 3 || grid.ys.size() < 3) {
            const EdgeProfiles profiles = edgeProfiles(image);
            Grid out;
            if (mask) {
                const std::vector<double> cx = crossingProfile(*mask, Axis::X);
                const std::vector<double> cy = crossingProfile(*mask, Axis::Y);
                out.xs = refineCuts(profiles.columns, grid.xs, image.width(), &cx);
                out.ys = refineCuts(profiles.rows, grid.ys, image.height(), &cy);
            } else {
                out.xs = refineCuts(profiles.columns, grid.xs, image.width());
                out.ys = refineCuts(profiles.rows, grid.ys, image.height());
            }
            return out;
        }
        auto band = [](const std::vector<double>& cuts, size_t j, int limit) {
            const int from = std::max(0, int(jsRound(cuts[j])));
            const int to = std::min(limit, std::max(int(jsRound(cuts[j])) + 1, int(jsRound(cuts[j + 1]))));
            return std::pair<int, int>(from, to);
        };
        auto bands = [&](Axis axis, const std::vector<double>& across, int limit, std::vector<std::vector<double>>& profiles,
                         std::vector<std::vector<double>>& crossings) {
            const size_t count = across.size() - 1;
            profiles.resize(count);
            if (mask) crossings.resize(count);
            gridParallel(int(count), [&](int j) {
                const auto [from, to] = band(across, size_t(j), limit);
                profiles[size_t(j)] = bandProfile(image, axis, from, to);
                if (mask) crossings[size_t(j)] = crossingProfile(*mask, axis, from, to);
            });
        };
        std::vector<std::vector<double>> rowProfiles, rowCrossings, columnProfiles, columnCrossings;
        bands(Axis::X, grid.ys, image.height(), rowProfiles, rowCrossings);
        bands(Axis::Y, grid.xs, image.width(), columnProfiles, columnCrossings);
        GridBend bend;
        bend.xs = bendCuts(rowProfiles, mask ? &rowCrossings : nullptr, grid.xs, image.width());
        bend.ys = bendCuts(columnProfiles, mask ? &columnCrossings : nullptr, grid.ys, image.height());
        auto mean = [](const std::vector<double>& line) {
            double total = 0;
            for (double v : line) total += v;
            return jsRound((total / double(line.size())) * 100) / 100;
        };
        Grid out;
        for (const std::vector<double>& line : bend.xs) out.xs.push_back(mean(line));
        for (const std::vector<double>& line : bend.ys) out.ys.push_back(mean(line));
        out.bend = std::move(bend);
        return out;
    }

    GridOffsets fitOffsets(const RgbaImage& image, double cellWidth, double cellHeight) {
        using namespace grid_ts;
        const EdgeProfiles profiles = edgeProfiles(image);
        auto fit = [](const std::vector<double>& profile, double length, double cell) {
            const double found = bestOffset(smoothed(profile), length, cell, 0, cell, 0.05).offset;
            return jsRound(found * 100) / 100;
        };
        return {fit(profiles.columns, image.width(), cellWidth), fit(profiles.rows, image.height(), cellHeight)};
    }

}
