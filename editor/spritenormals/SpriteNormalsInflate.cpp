// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The inflate profile's Poisson solve (2026-10-30-sprite-normals-hires-design.md, rule 4):
// -Laplacian(u) = 1 on the mask, u = 0 on the other pixels, by a conjugate gradient in double
// preconditioned by one geometric multigrid V-cycle in float (cell-centred, bilinear
// prolongation and its transpose, red-black Gauss-Seidel). Every pass works by rows (disjoint
// outputs) and every dot product is a sum of per-row partials added in row order, so the result
// is the same bits on any number of threads. Compiled with -ffp-contract=off (CMakeLists.txt).

#include "SpriteNormalsField.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace jasperengine::editor::spritenormals::detail {

    namespace {

        // the relative residual that ends the solve: the normals are then within 0.03 degrees of
        // a 1e-9 solve on the high-resolution evaluation sprites
        constexpr double kTolerance = 1e-4;
        constexpr int kPreSweeps = 2;           // red-black sweeps before and after the coarse correction
        constexpr int kCoarsestSweeps = 30;
        constexpr int kMinCoarseSide = 3;       // a level is coarsened while both sides are at least twice this

        inline int wrapIndex(int i, int n, Border border) {
            if (i >= 0 && i < n) return i;
            switch (border) {
                case Border::Zero: return -1;
                case Border::Wrap: {
                    const int m = i % n;
                    return m < 0 ? m + n : m;
                }
                case Border::Clamp: return i < 0 ? 0 : n - 1;
            }
            return -1;
        }

        // One level of the hierarchy: spacing^2, the mask, the diagonal (the neighbour slots that
        // are not the pixel itself: a Neumann side is not one), u, f and the residual (float)
        struct Level {
            int w = 0, h = 0;
            float h2 = 1.0f;
            std::vector<uint8_t> inside;
            std::vector<uint8_t> diag;
            std::vector<float> u, f, r;
            // to the next coarser level, per fine column / row: up to two (coarse index, weight)
            // slots of the bilinear prolongation (index -1: none)
            std::vector<std::array<int, 2>> xc, yc;
            std::vector<std::array<float, 2>> xw, yw;
            // the transpose, per coarse column / row: (fine index, weight) lists
            std::vector<std::vector<std::pair<int, float>>> xt, yt;
        };

        struct Solver {
            Border border = Border::Zero;
            Workers* workers = nullptr;
            std::vector<Level> levels;
            std::vector<float> zeros;           // a row of 0 (the widest level)
            static constexpr float kInverse[5] = {0.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f};

            Workers* poolFor(const Level& L) const { return static_cast<int64_t>(L.w) * L.h >= kParallelPixels ? workers : nullptr; }

            // The four neighbour values of row y (outside pixels hold 0 in every vector, so they
            // need no test): the rows above and below (a zero row beyond a Zero border or through a
            // Neumann side), and the left / right neighbours of the first and last pixel
            template <typename T>
            struct Rows {
                const T* up;
                const T* down;
                int firstLeft, lastRight;   // -1: none (0)
            };
            template <typename T>
            Rows<T> rows(const Level& L, const T* v, const T* zeroRow, int y) const {
                Rows<T> r;
                const int up = wrapIndex(y - 1, L.h, border), down = wrapIndex(y + 1, L.h, border);
                r.up = up >= 0 && up != y ? v + static_cast<size_t>(up) * L.w : zeroRow;
                r.down = down >= 0 && down != y ? v + static_cast<size_t>(down) * L.w : zeroRow;
                const int left = wrapIndex(-1, L.w, border), right = wrapIndex(L.w, L.w, border);
                r.firstLeft = left != 0 ? left : -1;
                r.lastRight = right != L.w - 1 ? right : -1;
                return r;
            }
            template <typename T>
            static inline T neighbourSum(const Level& L, const T* row, const Rows<T>& r, int x) {
                const T left = x > 0 ? row[x - 1] : (r.firstLeft >= 0 ? row[r.firstLeft] : T(0));
                const T right = x + 1 < L.w ? row[x + 1] : (r.lastRight >= 0 ? row[r.lastRight] : T(0));
                return ((left + right) + r.up[x]) + r.down[x];
            }

            void computeDiag(Level& L) const {
                L.diag.assign(L.inside.size(), 0);
                for (int y = 0; y < L.h; y++) {
                    const int up = wrapIndex(y - 1, L.h, border), down = wrapIndex(y + 1, L.h, border);
                    for (int x = 0; x < L.w; x++) {
                        const int left = wrapIndex(x - 1, L.w, border), right = wrapIndex(x + 1, L.w, border);
                        int d = 0;
                        d += left != x ? 1 : 0;
                        d += right != x ? 1 : 0;
                        d += up != y ? 1 : 0;
                        d += down != y ? 1 : 0;
                        L.diag[static_cast<size_t>(y) * L.w + x] = static_cast<uint8_t>(d);
                    }
                }
            }

            // the bilinear slots of every fine index of a line of n onto the coarse line of cn
            void slots(int n, int cn, std::vector<std::array<int, 2>>& c, std::vector<std::array<float, 2>>& w,
                       std::vector<std::vector<std::pair<int, float>>>& t) const {
                c.assign(static_cast<size_t>(n), {-1, -1});
                w.assign(static_cast<size_t>(n), {0.0f, 0.0f});
                t.assign(static_cast<size_t>(cn), {});
                for (int i = 0; i < n; i++) {
                    const int k = i / 2;
                    const int other = wrapIndex(i % 2 ? k + 1 : k - 1, cn, border);
                    c[static_cast<size_t>(i)] = {k, other};
                    w[static_cast<size_t>(i)] = {0.75f, other >= 0 ? 0.25f : 0.0f};
                    t[static_cast<size_t>(k)].push_back({i, 0.75f});
                    if (other >= 0) t[static_cast<size_t>(other)].push_back({i, 0.25f});
                }
            }

            void build(const std::vector<uint8_t>& mask, int width, int height) {
                levels.clear();
                Level fine;
                fine.w = width;
                fine.h = height;
                fine.inside = mask;
                levels.push_back(std::move(fine));
                for (;;) {
                    Level& L = levels.back();
                    if (L.w < 2 * kMinCoarseSide || L.h < 2 * kMinCoarseSide) break;
                    Level C;
                    C.w = (L.w + 1) / 2;
                    C.h = (L.h + 1) / 2;
                    C.h2 = L.h2 * 4.0f;
                    C.inside.assign(static_cast<size_t>(C.w) * C.h, 0);
                    bool fineHasOutside = false;
                    size_t firstOutside = 0;
                    size_t count = 0;
                    for (int cy = 0; cy < C.h; cy++) {
                        for (int cx = 0; cx < C.w; cx++) {
                            int children = 0, in = 0;
                            for (int dy = 0; dy < 2; dy++) {
                                for (int dx = 0; dx < 2; dx++) {
                                    const int fx = 2 * cx + dx, fy = 2 * cy + dy;
                                    if (fx >= L.w || fy >= L.h) continue;
                                    children++;
                                    const size_t fi = static_cast<size_t>(fy) * L.w + fx;
                                    if (L.inside[fi]) in++;
                                    else if (!fineHasOutside) {
                                        fineHasOutside = true;
                                        firstOutside = static_cast<size_t>(cy) * C.w + cx;
                                    }
                                }
                            }
                            // inside only when every child is: the coarse domain never reaches past the
                            // fine one (a larger one overshoots; pinholes would vanish from it)
                            const bool on = in == children;
                            C.inside[static_cast<size_t>(cy) * C.w + cx] = on;
                            count += on;
                        }
                    }
                    if (count == 0) break;
                    // Wrap / Clamp: keep a pixel held at 0 when the fine level has one (else the
                    // coarse problem has no solution)
                    if (border != Border::Zero && fineHasOutside && count == C.inside.size()) C.inside[firstOutside] = 0;
                    slots(L.w, C.w, L.xc, L.xw, L.xt);
                    slots(L.h, C.h, L.yc, L.yw, L.yt);
                    levels.push_back(std::move(C));
                }
                zeros.assign(static_cast<size_t>(width), 0.0f);
                for (Level& L : levels) {
                    computeDiag(L);
                    const size_t n = static_cast<size_t>(L.w) * L.h;
                    L.u.assign(n, 0.0f);
                    L.f.assign(n, 0.0f);
                    L.r.assign(n, 0.0f);
                }
            }

            // ---- the float multigrid

            // one red (color 0) or black (1) half-sweep of Gauss-Seidel on L.u for L.f. Rows of one
            // colour never read each other, except rows 0 and h - 1 across a wrapped seam of an
            // odd height (both start with the same colour): those two run first, in order, then
            // the others in parallel. Inside a row the order is left to right.
            void halfSweep(Level& L, int color) {
                // only this colour's inside pixels are computed: they read the other colour alone (no
                // pixel another thread writes in this pass), left to right inside a row
                auto row = [&](int y) {
                    const Rows<float> nb = rows(L, L.u.data(), zeros.data(), y);
                    const size_t base = static_cast<size_t>(y) * L.w;
                    float* u = L.u.data() + base;
                    const float* f = L.f.data() + base;
                    const uint8_t* in = L.inside.data() + base;
                    const uint8_t* d = L.diag.data() + base;
                    const int w = L.w;
                    const float h2 = L.h2;
                    for (int x = (y + color) & 1; x < w; x += 2) {
                        if (!in[x]) continue;
                        const float around = x > 0 && x + 1 < w ? (((u[x - 1] + u[x + 1]) + nb.up[x]) + nb.down[x]) : neighbourSum(L, u, nb, x);
                        u[x] = (around + h2 * f[x]) * kInverse[d[x]];
                    }
                };
                const bool seam = border == Border::Wrap && (L.h % 2) == 1 && L.h > 1;
                int first = 0, last = L.h;
                if (seam) {
                    row(0);
                    row(L.h - 1);
                    first = 1;
                    last = L.h - 1;
                }
                parallelFor(poolFor(L), last - first, [&](int b, int e) {
                    for (int y = first + b; y < first + e; y++) row(y);
                }, 16);
            }

            void residual(Level& L) {
                parallelFor(poolFor(L), L.h, [&](int b, int e) {
                    for (int y = b; y < e; y++) {
                        const Rows<float> nb = rows(L, L.u.data(), zeros.data(), y);
                        const size_t base = static_cast<size_t>(y) * L.w;
                        const float* u = L.u.data() + base;
                        const float* f = L.f.data() + base;
                        const uint8_t* in = L.inside.data() + base;
                        const uint8_t* d = L.diag.data() + base;
                        float* r = L.r.data() + base;
                        const float inv = 1.0f / L.h2;
                        for (int x = 0; x < L.w; x++) {
                            r[x] = in[x] ? f[x] - (static_cast<float>(d[x]) * u[x] - neighbourSum(L, u, nb, x)) * inv : 0.0f;
                        }
                    }
                }, 16);
            }

            // f of the coarse level = (1/4) P^T r of the fine one: the fine rows of each coarse row
            // combined first (vectorizes over x), then the columns
            void restrictTo(Level& L, Level& C) {
                parallelFor(poolFor(C), C.h, [&](int b, int e) {
                    std::vector<float> line(static_cast<size_t>(L.w));
                    for (int cy = b; cy < e; cy++) {
                        std::fill(line.begin(), line.end(), 0.0f);
                        for (const auto& [fy, wy] : L.yt[static_cast<size_t>(cy)]) {
                            const float* r = L.r.data() + static_cast<size_t>(fy) * L.w;
                            for (int x = 0; x < L.w; x++) line[static_cast<size_t>(x)] += wy * r[x];
                        }
                        const size_t base = static_cast<size_t>(cy) * C.w;
                        for (int cx = 0; cx < C.w; cx++) {
                            float sum = 0.0f;
                            for (const auto& [fx, wx] : L.xt[static_cast<size_t>(cx)]) sum += wx * line[static_cast<size_t>(fx)];
                            C.f[base + cx] = C.inside[base + cx] ? 0.25f * sum : 0.0f;
                        }
                    }
                }, 16);
            }

            // u of the fine level += P u of the coarse one (inside pixels only): the two coarse
            // rows of each fine row blended first, then the columns
            void prolongAdd(Level& L, const Level& C) {
                parallelFor(poolFor(L), L.h, [&](int b, int e) {
                    std::vector<float> line(static_cast<size_t>(C.w));
                    for (int y = b; y < e; y++) {
                        const std::array<int, 2>& ys = L.yc[static_cast<size_t>(y)];
                        const std::array<float, 2>& yws = L.yw[static_cast<size_t>(y)];
                        const float* c0 = C.u.data() + static_cast<size_t>(ys[0]) * C.w;
                        const float* c1 = ys[1] >= 0 ? C.u.data() + static_cast<size_t>(ys[1]) * C.w : zeros.data();
                        for (int cx = 0; cx < C.w; cx++) line[static_cast<size_t>(cx)] = yws[0] * c0[cx] + yws[1] * c1[cx];
                        const size_t base = static_cast<size_t>(y) * L.w;
                        float* u = L.u.data() + base;
                        const uint8_t* in = L.inside.data() + base;
                        for (int x = 0; x < L.w; x++) {
                            const std::array<int, 2>& xs = L.xc[static_cast<size_t>(x)];
                            const std::array<float, 2>& xws = L.xw[static_cast<size_t>(x)];
                            const float v = xws[0] * line[static_cast<size_t>(xs[0])] + (xs[1] >= 0 ? xws[1] * line[static_cast<size_t>(xs[1])] : 0.0f);
                            if (in[x]) u[x] += v;
                        }
                    }
                }, 16);
            }

            void vcycle(size_t l) {
                Level& L = levels[l];
                parallelFor(poolFor(L), L.h, [&](int b, int e) {
                    std::fill(L.u.begin() + static_cast<std::ptrdiff_t>(b) * L.w, L.u.begin() + static_cast<std::ptrdiff_t>(e) * L.w, 0.0f);
                }, 64);
                if (l + 1 == levels.size()) {
                    for (int s = 0; s < kCoarsestSweeps; s++) {
                        halfSweep(L, s & 1);
                        halfSweep(L, (s & 1) ^ 1);
                    }
                    return;
                }
                for (int s = 0; s < kPreSweeps; s++) {
                    halfSweep(L, 0);
                    halfSweep(L, 1);
                }
                residual(L);
                restrictTo(L, levels[l + 1]);
                vcycle(l + 1);
                prolongAdd(L, levels[l + 1]);
                for (int s = 0; s < kPreSweeps; s++) {
                    halfSweep(L, 1);
                    halfSweep(L, 0);
                }
            }

            // ---- the double conjugate gradient on the fine level

            // sum over rows of per-row partials, in row order (the same bits on any thread count)
            double rowSum(const Level& L, const std::function<double(int)>& rowValue) {
                std::vector<double> partial(static_cast<size_t>(L.h), 0.0);
                parallelFor(poolFor(L), L.h, [&](int b, int e) {
                    for (int y = b; y < e; y++) partial[static_cast<size_t>(y)] = rowValue(y);
                }, 16);
                double total = 0.0;
                for (double v : partial) total += v;
                return total;
            }
        };

    }

    PoissonStatus poisson(const std::vector<uint8_t>& mask, int width, int height, Border border, std::vector<double>& u, Workers* workers,
                          const std::atomic<bool>* cancel, int* iterations) {
        const size_t n = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
        u.assign(n, 0.0);
        if (iterations) *iterations = 0;
        size_t count = 0;
        for (uint8_t m : mask) count += m ? 1 : 0;
        if (count == 0) return PoissonStatus::Solved;
        if (border != Border::Zero && count == n) return PoissonStatus::Singular;

        Solver solver;
        solver.border = border;
        solver.workers = workers;
        solver.build(mask, width, height);
        Level& L = solver.levels.front();
        Workers* pool = solver.poolFor(L);
        // over the rows, a vector op per row
        auto byRows = [&](const std::function<void(size_t, size_t)>& op) {
            parallelFor(pool, height, [&](int b, int e) { op(static_cast<size_t>(b) * width, static_cast<size_t>(e) * width); }, 16);
        };
        // The iterate u is double; the residual r (the V-cycle's right-hand side, L.f), the
        // preconditioned residual z (the V-cycle's result, L.u) and the direction p are float.
        // A p is evaluated in double from the stored p, so the recurrence of r stays the residual
        // of u to float rounding of r itself (far below the tolerance).
        std::vector<float>& r = L.f;
        const std::vector<float>& z = L.u;
        std::vector<float> p(n, 0.0f);
        std::vector<float> zerosF(static_cast<size_t>(width), 0.0f);
        byRows([&](size_t b, size_t e) {
            for (size_t i = b; i < e; i++) r[i] = L.inside[i] ? 1.0f : 0.0f;
        });
        const double bNorm = std::sqrt(static_cast<double>(count));
        // (A p)_i in double at (x, y) of a row
        auto applyRow = [&](int y, auto&& use) {
            const Solver::Rows<float> nb = solver.rows(L, p.data(), zerosF.data(), y);
            const size_t base = static_cast<size_t>(y) * width;
            const float* pr = p.data() + base;
            const uint8_t* in = L.inside.data() + base;
            const uint8_t* d = L.diag.data() + base;
            for (int x = 0; x < width; x++) {
                if (!in[x]) continue;
                const float left = x > 0 ? pr[x - 1] : (nb.firstLeft >= 0 ? pr[nb.firstLeft] : 0.0f);
                const float right = x + 1 < width ? pr[x + 1] : (nb.lastRight >= 0 ? pr[nb.lastRight] : 0.0f);
                const double around = ((static_cast<double>(left) + static_cast<double>(right)) + static_cast<double>(nb.up[x])) + static_cast<double>(nb.down[x]);
                use(base + x, static_cast<double>(d[x]) * static_cast<double>(pr[x]) - around);
            }
        };
        auto rowDot = [&](int y) {
            double s = 0.0;
            const size_t base = static_cast<size_t>(y) * width;
            for (int x = 0; x < width; x++) s += static_cast<double>(r[base + x]) * static_cast<double>(z[base + x]);
            return s;
        };

        solver.vcycle(0);
        byRows([&](size_t b, size_t e) { std::copy(z.begin() + static_cast<std::ptrdiff_t>(b), z.begin() + static_cast<std::ptrdiff_t>(e), p.begin() + static_cast<std::ptrdiff_t>(b)); });
        double rz = solver.rowSum(L, rowDot);
        int it = 0;
        bool converged = false;
        for (; it < kPoissonMaxIterations; it++) {
            if (cancel && cancel->load()) {
                std::fill(u.begin(), u.end(), 0.0);
                return PoissonStatus::Cancelled;
            }
            const double pq = solver.rowSum(L, [&](int y) {
                double s = 0.0;
                applyRow(y, [&](size_t i, double ap) { s += static_cast<double>(p[i]) * ap; });
                return s;
            });
            if (!(pq > 0.0)) {
                converged = true;   // nothing left to reduce (an exact solution)
                break;
            }
            const double alpha = rz / pq;
            const double rr = solver.rowSum(L, [&](int y) {
                double s = 0.0;
                applyRow(y, [&](size_t i, double ap) {
                    u[i] += alpha * static_cast<double>(p[i]);
                    const float ri = static_cast<float>(static_cast<double>(r[i]) - alpha * ap);
                    r[i] = ri;
                    s += static_cast<double>(ri) * static_cast<double>(ri);
                });
                return s;
            });
            if (std::sqrt(rr) <= kTolerance * bNorm) {
                it++;
                converged = true;
                break;
            }
            solver.vcycle(0);
            const double rzNew = solver.rowSum(L, rowDot);
            const double beta = rzNew / rz;
            rz = rzNew;
            byRows([&](size_t b, size_t e) {
                for (size_t i = b; i < e; i++) p[i] = L.inside[i] ? z[i] + static_cast<float>(beta) * p[i] : 0.0f;
            });
        }
        if (iterations) *iterations = it;
        byRows([&](size_t b, size_t e) {
            for (size_t i = b; i < e; i++) {
                if (!L.inside[i] || !(u[i] > 0.0)) u[i] = 0.0;
            }
        });
        return converged ? PoissonStatus::Solved : PoissonStatus::NotConverged;
    }

}
