// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/glints.ts (smooth, boxMean, vertex, gridBox, regionalMax,
// findHighlights, pickHighlights, highlightSize, wobble, renderPeaks' picking) by SafEight, used
// with the author's permission.

#include "HighlightFinder.h"

#include "titleart/TitleArtHash.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr size_t HIGHLIGHT_CACHE = 32;
    // cells per side of a regionalMax block (8 x 4 = 32 canvas px) and blocks each way it reaches
    constexpr int REG_BLOCK = 8;
    constexpr int REG_REACH = 4;
    // the Hessian's step, cells (8 canvas px)
    constexpr int HESSIAN_STEP = 2;

    // [1 2 1]/4 along x then y, four times (sigma ~1.4 cells): 1 px bevel lines, whose
    // antialiasing differs between render scales, pool into one stable peak. The sums run in
    // double and store as float, as Pachislop's Float32Arrays do
    std::vector<float> smooth(const std::vector<float>& src, int w, int h) {
        std::vector<float> a = src;
        std::vector<float> b((size_t)w * h);
        for (int pass = 0; pass < 4; pass++) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)y * w + x;
                    b[i] = (float)(0.25 * a[x > 0 ? i - 1 : i] + 0.5 * a[i] + 0.25 * a[x < w - 1 ? i + 1 : i]);
                }
            }
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)y * w + x;
                    a[i] = (float)(0.25 * b[y > 0 ? i - w : i] + 0.5 * b[i] + 0.25 * b[y < h - 1 ? i + w : i]);
                }
            }
        }
        return a;
    }

    // the mean of v over the (2r+1)^2 box around every cell (clamped at the edges), by an
    // integral image
    std::vector<float> boxMean(const std::vector<float>& v, int w, int h, int r) {
        const size_t W = (size_t)w + 1;
        std::vector<double> sat(W * ((size_t)h + 1), 0.0);
        for (int y = 0; y < h; y++) {
            double row = 0.0;
            for (int x = 0; x < w; x++) {
                row += v[(size_t)y * w + x];
                sat[(size_t)(y + 1) * W + x + 1] = sat[(size_t)y * W + x + 1] + row;
            }
        }
        std::vector<float> out((size_t)w * h);
        for (int y = 0; y < h; y++) {
            const int y0 = std::max(0, y - r);
            const int y1 = std::min(h, y + r + 1);
            for (int x = 0; x < w; x++) {
                const int x0 = std::max(0, x - r);
                const int x1 = std::min(w, x + r + 1);
                const double s = sat[(size_t)y1 * W + x1] - sat[(size_t)y0 * W + x1] - sat[(size_t)y1 * W + x0] + sat[(size_t)y0 * W + x0];
                out[(size_t)y * w + x] = (float)(s / ((double)(x1 - x0) * (y1 - y0)));
            }
        }
        return out;
    }

    // sub-cell offset (-0.5..0.5) of the vertex of a parabola through three samples
    double vertex(double l, double c, double r) {
        const double d = l - 2.0 * c + r;
        return d < 0.0 ? std::max(-0.5, std::min(0.5, (0.5 * (l - r)) / d)) : 0.0;
    }

    double smoothstep(double a, double b, double x) {
        const double t = std::min(1.0, std::max(0.0, (x - a) / (b - a)));
        return t * t * (3.0 - 2.0 * t);
    }

    // the brightest on-shape luminance within ~128 canvas px of each cell, by blocks: normalising
    // a peak by it lets a dimmer second line still get its own brightest spots
    struct RegionalMax {
        int bw = 0;
        std::vector<float> out;

        RegionalMax(const std::vector<float>& lum, const std::vector<float>& alpha, int w, int h) {
            bw = (w + REG_BLOCK - 1) / REG_BLOCK;
            const int bh = (h + REG_BLOCK - 1) / REG_BLOCK;
            std::vector<float> block((size_t)bw * bh, 0.0f);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)y * w + x;
                    if (alpha[i] < 0.5f) continue;
                    const size_t b = (size_t)(y / REG_BLOCK) * bw + x / REG_BLOCK;
                    block[b] = std::max(block[b], lum[i]);
                }
            }
            out.assign((size_t)bw * bh, 0.0f);
            for (int by = 0; by < bh; by++) {
                for (int bx = 0; bx < bw; bx++) {
                    float m = 0.0f;
                    for (int y = std::max(0, by - REG_REACH); y <= std::min(bh - 1, by + REG_REACH); y++) {
                        for (int x = std::max(0, bx - REG_REACH); x <= std::min(bw - 1, bx + REG_REACH); x++) m = std::max(m, block[(size_t)y * bw + x]);
                    }
                    out[(size_t)by * bw + bx] = m;
                }
            }
        }

        double at(int x, int y) const {
            return std::max(1e-6, (double)out[(size_t)(y / REG_BLOCK) * bw + x / REG_BLOCK]);
        }
    };

    struct HighlightCache {
        std::mutex mutex;
        std::map<uint64_t, std::shared_ptr<const Highlights>> results;
        std::list<uint64_t> order;      // most recent first
    };

    HighlightCache& cache() {
        static HighlightCache c;
        return c;
    }

    uint64_t optionsKey(uint64_t gridKey, const FinderOptions& o) {
        const double d[4] = {o.threshold, o.faceBias, o.ridge, o.regional};
        return hashCombine(gridKey, fnv1a64(std::string_view(reinterpret_cast<const char*>(d), sizeof(d))));
    }

}

bool analysis::gridBox(const Grid& g, InkBox& box, float minAlpha) {
    int x0 = std::numeric_limits<int>::max(), y0 = std::numeric_limits<int>::max();
    int x1 = std::numeric_limits<int>::min(), y1 = std::numeric_limits<int>::min();
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) {
            if (g.alpha[(size_t)y * g.w + x] >= minAlpha) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
        }
    }
    if (x0 > x1) return false;
    const double c = g.cellComp;
    box = {g.x0 + x0 * c, g.y0 + y0 * c, g.x0 + x1 * c, g.y0 + y1 * c};
    return true;
}

Highlights analysis::findHighlights(const Grid& g, const FinderOptions& options) {
    Highlights result;
    const int w = g.w, h = g.h;
    if (g.empty() || g.alpha.size() < (size_t)w * h || g.lum.size() < (size_t)w * h) return result;
    result.hasBox = gridBox(g, result.box);
    if (!result.hasBox) return result;
    const std::vector<float>& alpha = g.alpha;
    const std::vector<float> lum = smooth(g.lum, w, h);
    const std::vector<float> bg = boxMean(lum, w, h, HL_BG);
    const double faceBias = options.faceBias, ridge = options.ridge, regional = options.regional;
    // the distance of on-shape cells to the nearest off-shape cell: gridDistance of the inverted mask
    std::vector<float> depth;
    if (faceBias > 0.0) {
        std::vector<float> inverted(alpha.size());
        for (size_t i = 0; i < alpha.size(); i++) inverted[i] = alpha[i] >= 0.5f ? 0.0f : 1.0f;
        depth = gridDistance(w, h, inverted);
    }
    float maxL = 0.0f, maxD = 0.0f;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (alpha[i] >= 0.5f) {
            maxL = std::max(maxL, lum[i]);
            if (!depth.empty()) maxD = std::max(maxD, depth[i]);
        }
    }
    if (maxL <= 0.0f) return result;
    std::unique_ptr<RegionalMax> reg;
    if (regional > 0.0) reg = std::make_unique<RegionalMax>(lum, alpha, w, h);
    const int R = HESSIAN_STEP;
    const double cell = g.cellComp;
    std::vector<Highlight>& out = result.peaks;
    for (int y = R; y < h - R; y++) {
        for (int x = R; x < w - R; x++) {
            const size_t i = (size_t)y * w + x;
            const float c = lum[i];
            if (alpha[i] < 0.5f) continue;
            // the reference brightness: the global maximum blended toward the regional one
            const double cRef = reg ? (1.0 - regional) * maxL + regional * reg->at(x, y) : (double)maxL;
            if ((double)c < cRef * options.threshold) continue;
            // strict against the neighbours before this cell in scan order, >= against those
            // after it, so a flat plateau yields one peak
            if (lum[i - w - 1] >= c || lum[i - w] >= c || lum[i - w + 1] >= c || lum[i - 1] >= c) continue;
            if (lum[i + 1] > c || lum[i + w - 1] > c || lum[i + w] > c || lum[i + w + 1] > c) continue;
            const double fx = vertex(lum[i - 1], c, lum[i + 1]);
            const double fy = vertex(lum[i - w], c, lum[i + w]);
            const double wDepth = !depth.empty() && maxD > 0.0f ? 1.0 - faceBias * (1.0 - smoothstep(0.1, 0.5, (double)depth[i] / maxD)) : 1.0;
            double wRidge = 1.0;
            if (ridge > 0.0) {
                // Hessian eigenvalues: a spot or corner curves down both ways, a ridge (a lit edge
                // running along a stroke or plate) only across itself; ridge peaks lose up to `ridge`
                const double lxx = (double)lum[i - R] - 2.0 * c + lum[i + R];
                const double lyy = (double)lum[i - (size_t)R * w] - 2.0 * c + lum[i + (size_t)R * w];
                const double lxy = 0.25 * ((double)lum[i + (size_t)R * w + R] - lum[i + (size_t)R * w - R] - lum[i - (size_t)R * w + R] +
                                           lum[i - (size_t)R * w - R]);
                const double m = 0.5 * (lxx + lyy);
                const double q = std::sqrt(0.25 * (lxx - lyy) * (lxx - lyy) + lxy * lxy);
                const double strong = std::min(0.0, m - q);
                const double weak = std::min(0.0, m + q);
                const double iso = strong < 0.0 ? weak / strong : 0.0;
                wRidge = 1.0 - ridge * (1.0 - std::sqrt(iso));
            }
            // the cell counted from the canvas centre (the lattice's corner is a whole number of cells)
            const double cx = x + std::round(g.x0 / cell);
            const double cy = y + std::round(g.y0 / cell);
            Highlight hl;
            hl.x = (cx + 0.5 + fx) * cell;
            hl.y = (cy + 0.5 + fy) * cell;
            hl.score = (((double)c - bg[i] + 0.15 * c) / cRef) * wDepth * wRidge;
            hl.key = (int64_t)cx * 7919 + (int64_t)cy;
            out.push_back(hl);
        }
    }
    // the key breaks score ties, so the ranking is fully deterministic
    std::sort(out.begin(), out.end(), [](const Highlight& a, const Highlight& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.key < b.key;
    });
    return result;
}

uint64_t analysis::gridKeyOf(uint64_t sourceKey, const GridSpec& s) {
    if (!sourceKey) return 0;
    const double d[3] = {s.x0, s.y0, s.cellComp};
    const int i[5] = {s.w, s.h, s.maxTaps, s.ceilTaps ? 1 : 0, (int)s.packing};
    const uint64_t k = hashCombine(sourceKey, fnv1a64(std::string_view(reinterpret_cast<const char*>(d), sizeof(d))));
    return hashCombine(k, fnv1a64(std::string_view(reinterpret_cast<const char*>(i), sizeof(i))));
}

std::shared_ptr<const Highlights> analysis::findHighlightsCached(const Grid& g, uint64_t gridKey, const FinderOptions& options) {
    if (!gridKey) return std::make_shared<const Highlights>(findHighlights(g, options));
    const uint64_t k = optionsKey(gridKey, options);
    HighlightCache& c = cache();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        auto hit = c.results.find(k);
        if (hit != c.results.end()) {
            c.order.remove(k);
            c.order.push_front(k);
            return hit->second;
        }
    }
    auto found = std::make_shared<const Highlights>(findHighlights(g, options));
    std::lock_guard<std::mutex> lock(c.mutex);
    if (c.results.emplace(k, found).second) {
        c.order.push_front(k);
        while (c.order.size() > HIGHLIGHT_CACHE) {
            c.results.erase(c.order.back());
            c.order.pop_back();
        }
    }
    return found;
}

void analysis::clearHighlightCache() {
    HighlightCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.results.clear();
    c.order.clear();
}

std::vector<Pick> analysis::pickHighlights(const std::vector<Highlight>& peaks, const std::vector<int>& order, int count,
                                           const std::function<double(const Highlight&)>& radius, double spacing, double minDist,
                                           const std::vector<Pick>& taken) {
    std::vector<Pick> out;
    if (count <= 0) return out;
    const double margin = HL_FADE * (order.empty() ? 1.0 : peaks[order[0]].score);
    std::vector<char> merged(peaks.size(), 0);
    auto clash = [&](const Highlight& oh, double orad, const Highlight& h, double r) {
        const double d = std::max(minDist, spacing * (orad + r));
        return (oh.x - h.x) * (oh.x - h.x) + (oh.y - h.y) * (oh.y - h.y) < d * d;
    };
    for (size_t k = 0; k < order.size() && out.size() <= (size_t)count; k++) {
        const int idx = order[k];
        const Highlight& h = peaks[idx];
        if (merged[idx]) continue;
        const double r = radius(h);
        // a pick of the same peak (not moved by a blend) or one too close
        bool blocked = false;
        for (const Pick& o : taken) {
            if ((!o.blended && o.source == idx) || clash(o.h, o.r, h, r)) {
                blocked = true;
                break;
            }
        }
        for (size_t j = 0; !blocked && j < out.size(); j++) blocked = clash(out[j].h, out[j].r, h, r);
        if (blocked) continue;
        // near-tied peaks it suppresses within HL_TIE_REACH of its radius pull the star toward them
        double sw = margin, sx = h.x * margin, sy = h.y * margin;
        for (size_t j = k + 1; j < order.size() && h.score - peaks[order[j]].score < margin; j++) {
            const int tdx = order[j];
            const Highlight& t = peaks[tdx];
            const double reach = HL_TIE_REACH * r;
            if (merged[tdx] || (t.x - h.x) * (t.x - h.x) + (t.y - h.y) * (t.y - h.y) > reach * reach || !clash(h, r, t, radius(t))) continue;
            const double wt = margin - (h.score - t.score);
            sw += wt;
            sx += t.x * wt;
            sy += t.y * wt;
            merged[tdx] = 1;
        }
        Pick pick;
        pick.h = h;
        pick.source = idx;
        pick.r = r;
        if (sw > margin) {
            pick.h.x = sx / sw;
            pick.h.y = sy / sw;
            pick.blended = true;
        }
        out.push_back(pick);
    }
    double runnerUp = -std::numeric_limits<double>::infinity();
    if (out.size() > (size_t)count) {
        runnerUp = out.back().h.score;
        out.pop_back();
    }
    for (Pick& o : out) o.fade = std::min(1.0, (o.h.score - runnerUp) / margin);
    return out;
}

std::vector<Pick> analysis::pickHighlights(const std::vector<Highlight>& peaks, int count, const std::function<double(const Highlight&)>& radius,
                                           double spacing, double minDist, const std::vector<Pick>& taken) {
    std::vector<int> order(peaks.size());
    for (size_t i = 0; i < peaks.size(); i++) order[i] = (int)i;
    return pickHighlights(peaks, order, count, radius, spacing, minDist, taken);
}

double analysis::highlightSize(double score, double top, double variation) {
    // continuous in the score, not the rank: near-tied peaks that swap between export scales
    // still get near-equal sizes
    const double t = std::min(1.0, std::max(0.0, (top - score) / (0.3 * top)));
    return 1.0 - variation * t;
}

double analysis::glintWobble(double x, double y, double seed, int k) {
    const double s = seed * 1.618 + k * 2.399;
    return 0.5 + 0.25 * std::sin(x * 0.061 + y * 0.037 + s) + 0.25 * std::sin(y * 0.079 - x * 0.043 + 1.7 * s);
}

FinderOptions analysis::finderOptions(const HighlightOptions& o) {
    FinderOptions f;
    f.threshold = o.threshold;
    f.faceBias = o.faceBias;
    f.ridge = o.ridge;
    f.regional = o.regional;
    return f;
}

GlintPicks analysis::pickGlints(const Highlights& found, const HighlightOptions& o) {
    GlintPicks picks;
    if (!found.hasBox || found.peaks.empty()) return picks;
    const std::vector<Highlight>& hl = found.peaks;
    const double top = hl[0].score;
    const double size = o.size, sv = o.sizeVariation, minD = o.spacing;
    const InkBox& box = found.box;
    // large glints also keep a share of the title's size apart, so they dress the whole title
    // instead of crowding its single brightest letter; the score shrinks them by at most half
    // the size variation
    const double spanD = (0.7 * std::hypot(box.x1 - box.x0, box.y1 - box.y0)) / std::max(1, o.large);
    picks.large = pickHighlights(hl, o.large, [&](const Highlight& h) { return size * highlightSize(h.score, top, 0.5 * sv); }, 0.6,
                                 std::max(minD, spanD));
    picks.small = pickHighlights(hl, o.small, [&](const Highlight&) { return size * SMALL_SPARKLE; }, 0.6, minD * 0.5, picks.large);
    return picks;
}

std::vector<HighlightAnchor> analysis::anchorsOf(const GlintPicks& picks, double seed) {
    std::vector<HighlightAnchor> anchors;
    for (const std::vector<Pick>* list : {&picks.large, &picks.small}) {
        for (const Pick& pk : *list) {
            HighlightAnchor a;
            a.x = pk.h.x;
            a.y = pk.h.y;
            a.radius = pk.r;
            a.score = pk.h.score;
            a.phase = glintWobble(pk.h.x, pk.h.y, seed, 3);
            a.fade = pk.fade;
            anchors.push_back(a);
        }
    }
    return anchors;
}

std::vector<HighlightAnchor> analysis::highlightAnchors(const Grid& g, const HighlightOptions& options, uint64_t gridKey) {
    const std::shared_ptr<const Highlights> found = findHighlightsCached(g, gridKey, finderOptions(options));
    return anchorsOf(pickGlints(*found, options), options.seed);
}

std::vector<SidecarAnchor> analysis::sidecarAnchors(const std::vector<HighlightAnchor>& anchors, const TitleRectI& frame, double canvasWidth,
                                                    double canvasHeight) {
    std::vector<SidecarAnchor> out;
    if (anchors.empty() || frame.empty()) return out;
    std::vector<const HighlightAnchor*> order;
    for (const HighlightAnchor& a : anchors) order.push_back(&a);
    std::stable_sort(order.begin(), order.end(), [](const HighlightAnchor* a, const HighlightAnchor* b) { return a->score > b->score; });
    if (order.size() > SIDECAR_ANCHORS_MAX) order.resize(SIDECAR_ANCHORS_MAX);
    const double top = order[0]->score;
    for (const HighlightAnchor* a : order) {
        // document space (about the canvas centre, +y up) -> image space of the canvas -> the frame
        const double ix = canvasWidth / 2.0 + a->x, iy = canvasHeight / 2.0 - a->y;
        SidecarAnchor s;
        s.x = (ix - frame.x) / frame.width;
        s.y = (iy - frame.y) / frame.height;
        s.size = a->radius / frame.height;
        s.strength = top > 0.0 ? std::clamp(a->score / top, 0.0, 1.0) : 0.0;
        s.fade = std::clamp(a->fade, 0.0, 1.0);
        out.push_back(s);
    }
    return out;
}

Json analysis::sidecarAnchorsJson(const std::vector<SidecarAnchor>& anchors) {
    Json list = Json::array();
    for (const SidecarAnchor& a : anchors) list.push_back(Json{{"x", a.x}, {"y", a.y}, {"size", a.size}, {"strength", a.strength}, {"fade", a.fade}});
    return list;
}
