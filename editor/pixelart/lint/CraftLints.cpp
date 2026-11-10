// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// jaggies, doubles, holes and contrast are ported from ritmo (packages/tools/src/fx/core/craft.ts,
// check.ts), and colors, transparent_background, size, near_duplicates and orphans from its
// pixel/core/lint.ts: MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CraftLints.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace jasperengine::editor::pixelart::lint {

    namespace {

        using jasperengine::pixelart::Rgba8;

        // craft.ts LINE: a shape this small is a speck (an ember, a chunk), not a line: its corner is its shape
        constexpr int CRAFT_LINE = 5;
        // craft.ts FAINT: colours closer than this (OKLab) to a background barely show on it
        constexpr double CRAFT_FAINT = 0.12;
        // check.ts FAINT_SHARE: a backdrop warns when more than this share of the pixels are faint on it
        constexpr double CRAFT_FAINT_SHARE = 0.5;
        // R27: a pillow-shading body needs this many pixels and lightness levels to be judged
        constexpr int PILLOW_MIN_PIXELS = 64;
        constexpr int PILLOW_MIN_LEVELS = 3;
        constexpr double PILLOW_MAX_R2 = 0.25;
        constexpr double PILLOW_MAX_BALANCE = 0.03;
        // R27: a ramp run has at least this many runs, each at most this long
        constexpr int BAND_MIN_RUNS = 3;
        constexpr int BAND_MAX_RUN = 2;
        constexpr int BAND_MIN_LINES = 3;

        LintResult craftStarted(const char* id, const LintThreshold& threshold) {
            LintResult r;
            r.id = id;
            r.threshold = threshold.value;
            r.thresholdSource = threshold.source;
            return r;
        }

        LintResult craftSkipped(const char* id, const std::string& why, const LintThreshold* threshold = nullptr) {
            LintResult r;
            r.id = id;
            r.status = LintStatus::Skip;
            r.message = why;
            r.thresholdSource = threshold ? threshold->source : std::string("default");
            if (threshold) r.threshold = threshold->value;
            return r;
        }

        bool craftDrawn(const PixelImage& f, int x, int y) {
            return f.row(y)[x * 4 + 3] != 0;
        }

        // R26 and R27 say "opaque": a soft pixel is partial_alpha's to report, not part of a body or a border
        bool craftOpaque(const PixelImage& f, int x, int y) {
            return f.row(y)[x * 4 + 3] == 255;
        }

        // 0xRRGGBBAA of a drawn pixel, 0 for every empty one (a drawn pixel has alpha > 0, so never 0)
        uint32_t craftKey(const PixelImage& f, int x, int y) {
            const uint8_t* p = f.row(y) + x * 4;
            if (p[3] == 0) return 0;
            return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }

        uint32_t craftRgb(const uint8_t* p) {
            return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }

        std::string craftHex(uint32_t rgb) {
            char text[16];
            std::snprintf(text, sizeof(text), "#%06x", rgb & 0xffffffu);
            return text;
        }

        std::string craftNumber(double v, int digits) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", digits, v);
            return text;
        }

        std::string craftWhere(const LintResult& r) {
            if (r.at.empty()) return "x,y";
            const LintSpot& s = r.at.front();
            return std::to_string(s.x) + "," + std::to_string(s.y) + (s.frame > 1 ? " (frame " + std::to_string(s.frame) + ")" : "");
        }

        double craftDistance(const jasperengine::pixelart::Lab& a, const jasperengine::pixelart::Lab& b) {
            const double dl = (double)a.l - b.l, da = (double)a.a - b.a, db = (double)a.b - b.b;
            return std::sqrt(dl * dl + da * da + db * db);
        }

        // OKLab per 24-bit colour, computed once per call: art has few colours and the conversion
        // (S43's fixed Newton roots) costs far more than a lookup
        class CraftLabCache {
        public:
            const jasperengine::pixelart::Lab& of(uint32_t rgb) {
                if (keys.empty()) grow(1024);
                size_t slot = hash(rgb) & (keys.size() - 1);
                while (keys[slot] != 0) {
                    if (keys[slot] == rgb + 1) return labs[slot];
                    slot = (slot + 1) & (keys.size() - 1);
                }
                if ((used + 1) * 2 > keys.size()) {
                    grow(keys.size() * 2);
                    return of(rgb);
                }
                keys[slot] = rgb + 1;
                labs[slot] = jasperengine::pixelart::toOklab(jasperengine::pixelart::unpackRgb(rgb));
                used++;
                return labs[slot];
            }

        private:
            static size_t hash(uint32_t v) { return (size_t)((v * 2654435761u) >> 8); }
            void grow(size_t size) {
                std::vector<uint32_t> oldKeys = std::move(keys);
                std::vector<jasperengine::pixelart::Lab> oldLabs = std::move(labs);
                keys.assign(size, 0);
                labs.assign(size, {});
                used = 0;
                for (size_t i = 0; i < oldKeys.size(); i++) {
                    if (oldKeys[i] == 0) continue;
                    size_t slot = hash(oldKeys[i] - 1) & (size - 1);
                    while (keys[slot] != 0) slot = (slot + 1) & (size - 1);
                    keys[slot] = oldKeys[i];
                    labs[slot] = oldLabs[i];
                    used++;
                }
            }
            std::vector<uint32_t> keys;   // rgb + 1; 0 is empty
            std::vector<jasperengine::pixelart::Lab> labs;
            size_t used = 0;
        };

        // A spot-counting craft rule's verdict: WARN from `threshold` spots; at is cleared on a PASS
        void craftSpotVerdict(LintResult& r, const std::string& what, const std::string& fix) {
            r.measured = r.count;
            if (r.count > 0 && r.count >= r.threshold) {
                r.status = LintStatus::Warn;
                r.message = std::to_string(r.count) + " " + what;
                r.fix = fix;
            } else {
                r.status = LintStatus::Pass;
                r.at.clear();
            }
        }

        // ---- jaggies (craft.ts topEdgeChains, turns)

        struct CraftEdgeRun {
            int y = 0, x0 = 0, x1 = 0;
        };

        // A mask turned so that one of its four edges is on top; back() maps a spot back
        struct CraftTurn {
            int w = 0, h = 0, kind = 0;
            int srcW = 0, srcH = 0;
            void back(int x, int y, int& sx, int& sy) const {
                switch (kind) {
                    case 0: sx = x; sy = y; break;                 // top edges
                    case 1: sx = x; sy = srcH - 1 - y; break;      // bottom edges
                    case 2: sx = y; sy = x; break;                 // left edges
                    default: sx = srcW - 1 - y; sy = x; break;     // right edges
                }
            }
        };

        struct CraftChains {
            std::vector<uint8_t> turned;
            std::vector<CraftEdgeRun> runs;
            std::vector<int> startAt;      // run index + 1 by (x0, y)
            std::vector<uint8_t> endAt;    // a run ends at (x1, y)
            std::vector<int> chain;
        };

        void craftJaggiesOf(const PixelImage& f, int frame, LintResult& r, CraftChains& s) {
            const int w = f.width(), h = f.height();
            for (int kind = 0; kind < 4; kind++) {
                CraftTurn t;
                t.kind = kind;
                t.srcW = w;
                t.srcH = h;
                t.w = kind < 2 ? w : h;
                t.h = kind < 2 ? h : w;
                s.turned.assign((size_t)t.w * t.h, 0);
                for (int y = 0; y < t.h; y++) {
                    for (int x = 0; x < t.w; x++) {
                        int sx, sy;
                        t.back(x, y, sx, sy);
                        s.turned[(size_t)y * t.w + x] = craftDrawn(f, sx, sy) ? 1 : 0;
                    }
                }
                s.runs.clear();
                s.startAt.assign((size_t)t.w * t.h, 0);
                s.endAt.assign((size_t)t.w * t.h, 0);
                for (int y = 0; y < t.h; y++) {
                    int x = 0;
                    auto edge = [&](int xx) {
                        return s.turned[(size_t)y * t.w + xx] == 1 && (y == 0 || s.turned[(size_t)(y - 1) * t.w + xx] == 0);
                    };
                    while (x < t.w) {
                        if (!edge(x)) {
                            x++;
                            continue;
                        }
                        const int x0 = x;
                        while (x < t.w && edge(x)) x++;
                        s.runs.push_back({y, x0, x - 1});
                    }
                }
                for (size_t i = 0; i < s.runs.size(); i++) {
                    const CraftEdgeRun& run = s.runs[i];
                    s.startAt[(size_t)run.y * t.w + run.x0] = (int)i + 1;
                    s.endAt[(size_t)run.y * t.w + run.x1] = 1;
                }
                auto startingAt = [&](int x, int y) -> int {
                    if (x < 0 || y < 0 || x >= t.w || y >= t.h) return -1;
                    return s.startAt[(size_t)y * t.w + x] - 1;
                };
                auto endingAt = [&](int x, int y) -> bool {
                    if (x < 0 || y < 0 || x >= t.w || y >= t.h) return false;
                    return s.endAt[(size_t)y * t.w + x] != 0;
                };
                for (size_t first = 0; first < s.runs.size(); first++) {
                    for (int dy : {1, -1}) {
                        // each chain is read once, from its first run: runs another run leads into are skipped
                        if (endingAt(s.runs[first].x0 - 1, s.runs[first].y - dy)) continue;
                        s.chain.clear();
                        s.chain.push_back((int)first);
                        int current = (int)first;
                        for (;;) {
                            const int next = startingAt(s.runs[(size_t)current].x1 + 1, s.runs[(size_t)current].y + dy);
                            if (next < 0) break;
                            s.chain.push_back(next);
                            current = next;
                        }
                        if (s.chain.size() < 3) continue;
                        for (size_t i = 1; i + 1 < s.chain.size(); i++) {
                            auto length = [&](size_t k) {
                                const CraftEdgeRun& part = s.runs[(size_t)s.chain[k]];
                                return part.x1 - part.x0 + 1;
                            };
                            const int before = length(i - 1), self = length(i), after = length(i + 1);
                            if (self <= std::min(before, after) - 2 || self >= std::max(before, after) + 2) {
                                int sx, sy;
                                const CraftEdgeRun& part = s.runs[(size_t)s.chain[i]];
                                t.back(part.x0, part.y, sx, sy);
                                r.addSpot({frame, sx, sy});
                            }
                        }
                    }
                }
            }
        }

        // ---- banding (R27)

        struct BandRun {
            int pos = 0, length = 0;   // pos: the run's first pixel along the line's position axis
            uint32_t key = 0;          // 0: not opaque
            float l = 0.0f;
        };

        struct BandRamp {
            int pos = 0;               // the first run's first pixel
            int runLength = 0;
            int first = 0, count = 0;  // its colours: BandScratch::pool[first, first + count)
            int next[3] = {-1, -1, -1};    // by shift + 1: the same ramp on the next line, shifted
            bool linked[3] = {false, false, false};   // by shift + 1: the previous line continues into it
        };

        struct BandScratch {
            CraftLabCache labs;
            std::vector<BandRun> runs;
            std::vector<uint32_t> pool;    // the ramps' colours, one family at a time
            std::vector<uint32_t> keys;    // per pixel of the frame: rgb | 1 << 24 when opaque, else 0
            std::vector<float> lightness;  // per opaque pixel: OKLab L
            std::vector<std::vector<BandRamp>> lines;
            std::vector<uint8_t> inGroup;
        };

        // A line family: rows, columns, diagonals (down-right) and anti-diagonals (up-right). Each line
        // is walked by its position p; pixelOf gives (x, y), false when off the frame.
        struct BandFamily {
            int kind = 0, w = 0, h = 0;
            int lineCount() const { return kind == 0 ? h : kind == 1 ? w : w + h - 1; }
            // positions along line l: [first, last]
            void span(int l, int& first, int& last) const {
                switch (kind) {
                    case 0: first = 0; last = w - 1; break;
                    case 1: first = 0; last = h - 1; break;
                    case 2: {   // x - y = l - (h - 1); p = x
                        const int d = l - (h - 1);
                        first = std::max(0, d);
                        last = std::min(w - 1, d + h - 1);
                        break;
                    }
                    default: {  // x + y = l; p = x
                        first = std::max(0, l - (h - 1));
                        last = std::min(w - 1, l);
                        break;
                    }
                }
            }
            void pixelOf(int l, int p, int& x, int& y) const {
                switch (kind) {
                    case 0: x = p; y = l; break;
                    case 1: x = l; y = p; break;
                    case 2: x = p; y = p - (l - (h - 1)); break;
                    default: x = p; y = l - p; break;
                }
            }
        };

        void bandRampsOf(const BandFamily& fam, int l, BandScratch& s, std::vector<BandRamp>& out) {
            out.clear();
            int first, last;
            fam.span(l, first, last);
            s.runs.clear();
            for (int p = first; p <= last; p++) {
                int x, y;
                fam.pixelOf(l, p, x, y);
                const size_t pixel = (size_t)y * fam.w + x;
                const uint32_t key = s.keys[pixel];
                if (!s.runs.empty() && s.runs.back().key == key && s.runs.back().pos + s.runs.back().length == p) {
                    s.runs.back().length++;
                    continue;
                }
                BandRun run;
                run.pos = p;
                run.length = 1;
                run.key = key;
                run.l = s.lightness[pixel];
                s.runs.push_back(run);
            }
            const size_t n = s.runs.size();
            size_t i = 0;
            while (i < n) {
                const BandRun& a = s.runs[i];
                if (a.key == 0 || a.length > BAND_MAX_RUN) {
                    i++;
                    continue;
                }
                size_t j = i + 1;
                int direction = 0;
                while (j < n) {
                    const BandRun& prev = s.runs[j - 1];
                    const BandRun& run = s.runs[j];
                    if (run.key == 0 || run.length != a.length) break;
                    const int step = run.l > prev.l ? 1 : run.l < prev.l ? -1 : 0;
                    if (step == 0 || (direction != 0 && step != direction)) break;
                    direction = step;
                    j++;
                }
                if ((int)(j - i) >= BAND_MIN_RUNS) {
                    BandRamp ramp;
                    ramp.pos = a.pos;
                    ramp.runLength = a.length;
                    ramp.first = (int)s.pool.size();
                    ramp.count = (int)(j - i);
                    for (size_t k = i; k < j; k++) s.pool.push_back(s.runs[k].key);
                    out.push_back(ramp);
                    // the last run may start a ramp the other way (a peak belongs to both slopes)
                    i = j - 1;
                } else {
                    i++;
                }
            }
        }

        const BandRamp* bandNext(const BandScratch& s, const std::vector<BandRamp>& line, const BandRamp& ramp, int shift) {
            for (const BandRamp& other : line) {
                if (other.pos == ramp.pos + shift && other.runLength == ramp.runLength && other.count == ramp.count &&
                    std::equal(s.pool.begin() + other.first, s.pool.begin() + other.first + other.count, s.pool.begin() + ramp.first)) {
                    return &other;
                }
            }
            return nullptr;
        }

        void bandMark(const BandFamily& fam, int l, const BandRamp& ramp, int w, std::vector<uint8_t>& mask) {
            const int pixels = ramp.runLength * ramp.count;
            for (int k = 0; k < pixels; k++) {
                int x, y;
                fam.pixelOf(l, ramp.pos + k, x, y);
                mask[(size_t)y * w + x] = 1;
            }
        }

        // ---- pillow shading (R27)

        struct PillowScratch {
            CraftLabCache labs;
            std::vector<int> body;          // pixel indices of the body being measured
            std::vector<int> label;         // body id + 1 per pixel
            std::vector<int> dist;          // 4-connected distance to the outside, 0 outside the body
            std::vector<int> queue;
            std::vector<float> lightness;   // per pixel
            std::vector<double> sorted, levelRank;
            std::vector<double> rankL, rankD;
        };

        // Average ranks (1-based) of values, ties sharing their mean rank; returns the distinct
        // values. Art has few tones, so the ranks come from the sorted distinct values.
        size_t pillowRanks(const std::vector<double>& values, std::vector<double>& sorted, std::vector<double>& levelRank, std::vector<double>& ranks) {
            const size_t n = values.size();
            sorted = values;
            std::sort(sorted.begin(), sorted.end());
            size_t distinct = 0;
            size_t i = 0;
            levelRank.clear();
            while (i < n) {
                size_t j = i + 1;
                while (j < n && sorted[j] == sorted[i]) j++;
                sorted[distinct++] = sorted[i];
                levelRank.push_back((double)(i + 1 + j) / 2.0);
                i = j;
            }
            sorted.resize(distinct);
            ranks.resize(n);
            for (size_t k = 0; k < n; k++) {
                ranks[k] = levelRank[(size_t)(std::lower_bound(sorted.begin(), sorted.end(), values[k]) - sorted.begin())];
            }
            return distinct;
        }

        double pillowPearson(const std::vector<double>& a, const std::vector<double>& b) {
            const size_t n = a.size();
            double ma = 0, mb = 0;
            for (size_t i = 0; i < n; i++) {
                ma += a[i];
                mb += b[i];
            }
            ma /= (double)n;
            mb /= (double)n;
            double sab = 0, saa = 0, sbb = 0;
            for (size_t i = 0; i < n; i++) {
                sab += (a[i] - ma) * (b[i] - mb);
                saa += (a[i] - ma) * (a[i] - ma);
                sbb += (b[i] - mb) * (b[i] - mb);
            }
            if (saa <= 0.0 || sbb <= 0.0) return 0.0;
            return sab / std::sqrt(saa * sbb);
        }

        struct PillowBody {
            int frame = 0;
            double rho = 0, r2 = 0, balance = 0;
            LintSpot centroid, outer, inner;
            bool pillow = false;
        };

    }

    LintResult lintColors(const FrameView& frames, const LintThreshold* maxColors) {
        if (!maxColors) return craftSkipped("colors", "no rules.max_colors given");
        LintResult r = craftStarted("colors", *maxColors);
        std::vector<uint32_t> used;
        for (const PixelImage* f : frames) {
            if (!f) continue;
            for (int y = 0; y < f->height(); y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < f->width(); x++) {
                    if (row[x * 4 + 3] != 0) used.push_back(craftRgb(row + x * 4));
                }
            }
        }
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        r.measured = (double)used.size();
        r.count = (int)used.size();
        if (r.measured > r.threshold) {
            r.status = LintStatus::Fail;
            r.message = std::to_string(used.size()) + " colours, more than the " + craftNumber(r.threshold, 0) + " allowed";
            r.fix = "fix_pixel_art with palette: {count: " + craftNumber(r.threshold, 0) + "} (or apply_palette onto a palette that small)";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintTransparentBackground(const FrameView& frames, bool required) {
        if (!required) return craftSkipped("transparent_background", "rules.transparent_background is not set");
        LintThreshold half;
        half.source = "rules.transparent_background";
        LintResult r = craftStarted("transparent_background", half);
        bool any = false, failed = false;
        double worst = -1.0;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f || f->empty()) continue;
            const int w = f->width(), h = f->height();
            // lint.ts's border walk: both full rows, then the side columns between them
            int border = 0, opaque = 0;
            auto visit = [&](int x, int y, bool spot) {
                border++;
                if (craftOpaque(*f, x, y)) {
                    opaque++;
                    if (spot) r.addSpot({(int)fi + 1, x, y});
                }
            };
            const auto walk = [&](bool spot) {
                border = opaque = 0;
                for (int x = 0; x < w; x++) {
                    visit(x, 0, spot);
                    visit(x, h - 1, spot);
                }
                for (int y = 1; y < h - 1; y++) {
                    visit(0, y, spot);
                    visit(w - 1, y, spot);
                }
            };
            walk(false);
            const int corners = (int)craftOpaque(*f, 0, 0) + (int)craftOpaque(*f, w - 1, 0) + (int)craftOpaque(*f, 0, h - 1) + (int)craftOpaque(*f, w - 1, h - 1);
            // a sprite may touch its edges; a background left in fills the corners too
            const bool fails = corners >= 3 && opaque * 2 > border;
            if (!any || opaque > worst) {
                worst = opaque;
                r.threshold = border / 2.0;
            }
            any = true;
            if (fails) {
                failed = true;
                walk(true);
            }
        }
        if (!any) return craftSkipped("transparent_background", "no frames given");
        r.measured = worst;
        if (failed) {
            r.status = LintStatus::Fail;
            r.message = std::to_string(r.count) + " border pixels are opaque: the background was not removed";
            r.fix = "fix_pixel_art with remove_background: true";
        } else {
            r.status = LintStatus::Pass;
            r.count = 0;
            r.at.clear();
        }
        return r;
    }

    LintResult lintSize(const FrameView& frames, std::span<const std::pair<int, int>> sizes) {
        if (sizes.empty()) return craftSkipped("size", "no rules.sizes given");
        LintThreshold none;
        none.source = "rules.sizes";
        LintResult r = craftStarted("size", none);
        std::string allowed;
        for (const auto& s : sizes) allowed += (allowed.empty() ? "" : ", ") + std::to_string(s.first) + "x" + std::to_string(s.second);
        std::string first;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            const bool ok = std::any_of(sizes.begin(), sizes.end(), [&](const std::pair<int, int>& s) { return s.first == f->width() && s.second == f->height(); });
            if (!ok) {
                if (first.empty()) first = std::to_string(f->width()) + "x" + std::to_string(f->height());
                r.addSpot({(int)fi + 1, 0, 0});
            }
        }
        r.measured = r.count;
        if (r.count > 0) {
            r.status = LintStatus::Fail;
            r.message = first + " is not an allowed size (" + allowed + ")";
            r.fix = "resize the canvas to an allowed size (" + allowed + ") in Aseprite or with edit_sprite (S47)";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintNearDuplicates(const FrameView& frames, const LintThreshold& threshold) {
        // the used colours in the order first met, with where
        std::vector<uint32_t> sorted;
        struct Used {
            uint32_t rgb;
            LintSpot first;
        };
        std::vector<Used> used;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            for (int y = 0; y < f->height(); y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < f->width(); x++) {
                    if (row[x * 4 + 3] == 0) continue;
                    const uint32_t rgb = craftRgb(row + x * 4);
                    auto it = std::lower_bound(sorted.begin(), sorted.end(), rgb);
                    if (it != sorted.end() && *it == rgb) continue;
                    sorted.insert(it, rgb);
                    used.push_back({rgb, {(int)fi + 1, x, y}});
                }
            }
        }
        if (used.size() < 2) return craftSkipped("near_duplicates", "fewer than 2 colours", &threshold);
        LintResult r = craftStarted("near_duplicates", threshold);
        std::vector<jasperengine::pixelart::Lab> labs;
        labs.reserve(used.size());
        for (const Used& u : used) labs.push_back(jasperengine::pixelart::toOklab(jasperengine::pixelart::unpackRgb(u.rgb)));
        double closest = -1.0;
        int pairs = 0;
        std::string listed;
        for (size_t i = 0; i < used.size(); i++) {
            for (size_t j = i + 1; j < used.size(); j++) {
                const double d = craftDistance(labs[i], labs[j]);
                if (closest < 0.0 || d < closest) closest = d;
                if (d < threshold.value) {
                    pairs++;
                    r.addSpot(used[j].first);
                    if (pairs <= 6) listed += (listed.empty() ? "" : ", ") + craftHex(used[i].rgb) + "/" + craftHex(used[j].rgb);
                }
            }
        }
        r.measured = closest;
        r.count = pairs;
        if (pairs > 0) {
            r.status = LintStatus::Warn;
            r.message = std::to_string(pairs) + " pair" + (pairs == 1 ? "" : "s") + " of colours too close to tell apart (" + listed + (pairs > 6 ? ", ..." : "") + ")";
            r.fix = "merge them: fix_pixel_art with a palette that keeps one of each pair, or edit_palette";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintOrphans(const FrameView& frames, bool sheet, const LintThreshold& threshold) {
        if (sheet) return craftSkipped("orphans", "an animation sheet: sparks and dust are single pixels on purpose", &threshold);
        LintResult r = craftStarted("orphans", threshold);
        std::vector<uint64_t> grid;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            const int w = f->width(), h = f->height();
            // the keys with a 1-pixel frame of "off the picture" (a value no pixel has), so the
            // neighbours need no bounds checks; ritmo's at() skips them
            constexpr uint64_t OFF = 1ull << 32;
            const int pw = w + 2;
            grid.assign((size_t)pw * (h + 2), OFF);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) grid[(size_t)(y + 1) * pw + x + 1] = craftKey(*f, x, y);
            }
            uint64_t keys[8];
            int counts[8];
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const uint64_t* c = grid.data() + (size_t)(y + 1) * pw + x + 1;
                    const uint64_t self = *c;
                    if (self == 0) continue;
                    if (c[-1] == self || c[1] == self || c[-pw] == self || c[pw] == self) continue;
                    int distinct = 0;
                    bool orphan = false;
                    for (int dy = -1; dy <= 1 && !orphan; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const uint64_t v = c[dy * pw + dx];
                            if ((dx == 0 && dy == 0) || v == OFF) continue;
                            if (v == self) continue;
                            int k = 0;
                            while (k < distinct && keys[k] != v) k++;
                            if (k == distinct) {
                                keys[distinct] = v;
                                counts[distinct++] = 0;
                            }
                            if (++counts[k] >= 5) {
                                orphan = true;
                                break;
                            }
                        }
                    }
                    if (orphan) r.addSpot({(int)fi + 1, x, y});
                }
            }
        }
        craftSpotVerdict(r, std::string("lone pixel") + (r.count == 1 ? "" : "s") + " surrounded by another colour: noise, unless a deliberate highlight", "");
        if (r.status == LintStatus::Warn) {
            r.fix = "fix_pixel_art with cleanup: {size: 1}, or edit_sprite: recolour the lone pixel at " + craftWhere(r) + " (S47)";
        }
        return r;
    }

    LintResult lintJaggies(const FrameView& frames, const LintThreshold& threshold) {
        LintResult r = craftStarted("jaggies", threshold);
        CraftChains scratch;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            if (frames[fi] && !frames[fi]->empty()) craftJaggiesOf(*frames[fi], (int)fi + 1, r, scratch);
        }
        craftSpotVerdict(r, "uneven steps along edges (a run of 3, 1, 3 or 1, 3, 1): the curve looks broken", "");
        if (r.status == LintStatus::Warn) {
            r.fix = "edit_sprite: even out the step at " + craftWhere(r) + " so the runs grow or shrink steadily (S47)";
        }
        return r;
    }

    LintResult lintDoubles(const FrameView& frames, const LintThreshold& threshold) {
        LintResult r = craftStarted("doubles", threshold);
        std::vector<uint8_t> mask, seen;
        std::vector<int> stack, touched;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f || f->empty()) continue;
            const int w = f->width(), h = f->height(), pw = w + 2;
            // the drawn pixels with a 1-pixel empty frame, so neighbours need no bounds checks
            mask.assign((size_t)pw * (h + 2), 0);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) mask[(size_t)(y + 1) * pw + x + 1] = craftDrawn(*f, x, y) ? 1 : 0;
            }
            seen.assign(mask.size(), 0);
            // whether a pixel's shape (8-connected, craft.ts shapeSizes) reaches CRAFT_LINE pixels:
            // the flood stops there, as only that much decides
            auto lineSized = [&](int start) {
                touched.clear();
                stack.clear();
                stack.push_back(start);
                seen[(size_t)start] = 1;
                touched.push_back(start);
                while (!stack.empty() && (int)touched.size() < CRAFT_LINE) {
                    const int i = stack.back();
                    stack.pop_back();
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int j = i + dy * pw + dx;
                            if (mask[(size_t)j] && !seen[(size_t)j]) {
                                seen[(size_t)j] = 1;
                                touched.push_back(j);
                                stack.push_back(j);
                            }
                        }
                    }
                }
                const bool large = (int)touched.size() >= CRAFT_LINE;
                for (int i : touched) seen[(size_t)i] = 0;
                return large;
            };
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const int i = (y + 1) * pw + x + 1;
                    const uint8_t* m = mask.data() + i;
                    if (!*m) continue;
                    const int count = m[-pw - 1] + m[-pw] + m[-pw + 1] + m[-1] + m[1] + m[pw - 1] + m[pw] + m[pw + 1];
                    if (count != 2) continue;
                    const bool side = m[-1] || m[1];
                    const bool upDown = m[-pw] || m[pw];
                    if (side && upDown && lineSized(i)) r.addSpot({(int)fi + 1, x, y});
                }
            }
        }
        craftSpotVerdict(r, "corner pixels thicken one-pixel lines into an L", "");
        if (r.status == LintStatus::Warn) r.fix = "edit_sprite: remove the corner pixel at " + craftWhere(r) + " (S47)";
        return r;
    }

    LintResult lintHoles(const FrameView& frames, const LintThreshold& threshold) {
        LintResult r = craftStarted("holes", threshold);
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            for (int y = 1; y < f->height() - 1; y++) {
                for (int x = 1; x < f->width() - 1; x++) {
                    if (!craftDrawn(*f, x, y) && craftDrawn(*f, x - 1, y) && craftDrawn(*f, x + 1, y) && craftDrawn(*f, x, y - 1) && craftDrawn(*f, x, y + 1)) {
                        r.addSpot({(int)fi + 1, x, y});
                    }
                }
            }
        }
        craftSpotVerdict(r, "pinholes: empty pixels with drawn pixels on all four sides", "");
        if (r.status == LintStatus::Warn) r.fix = "edit_sprite: fill the pinhole at " + craftWhere(r) + " (S47)";
        return r;
    }

    LintResult lintContrast(const FrameView& frames, std::span<const Rgba8> backdrops, const LintThreshold& threshold) {
        if (backdrops.empty()) return craftSkipped("contrast", "no backdrops given (options.backdrops, the pixel_art.backdrops setting or a 2D camera's clear colour)", &threshold);
        LintResult r = craftStarted("contrast", threshold);
        // the drawn pixels by colour (craft.ts counts each palette entry's pixels)
        std::vector<std::pair<uint32_t, int>> counts;
        int all = 0;
        for (const PixelImage* f : frames) {
            if (!f) continue;
            for (int y = 0; y < f->height(); y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < f->width(); x++) {
                    if (row[x * 4 + 3] == 0) continue;
                    const uint32_t rgb = craftRgb(row + x * 4);
                    auto it = std::lower_bound(counts.begin(), counts.end(), rgb, [](const std::pair<uint32_t, int>& e, uint32_t v) { return e.first < v; });
                    if (it == counts.end() || it->first != rgb) it = counts.insert(it, {rgb, 0});
                    it->second++;
                    all++;
                }
            }
        }
        std::vector<jasperengine::pixelart::Lab> labs;
        labs.reserve(counts.size());
        for (const auto& c : counts) labs.push_back(jasperengine::pixelart::toOklab(jasperengine::pixelart::unpackRgb(c.first)));
        int worst = -1;
        double worstShare = -1.0;
        for (size_t b = 0; b < backdrops.size(); b++) {
            const jasperengine::pixelart::Lab back = jasperengine::pixelart::toOklab(Rgba8{backdrops[b].r, backdrops[b].g, backdrops[b].b, 255});
            int faint = 0;
            for (size_t c = 0; c < counts.size(); c++) {
                if (craftDistance(labs[c], back) < CRAFT_FAINT) faint += counts[c].second;
            }
            const double share = all == 0 ? 0.0 : (double)faint / all;
            if (share > worstShare) {
                worstShare = share;
                worst = (int)b;
            }
        }
        r.measured = worstShare;
        const Rgba8 bad = backdrops[(size_t)worst];
        const jasperengine::pixelart::Lab back = jasperengine::pixelart::toOklab(Rgba8{bad.r, bad.g, bad.b, 255});
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            for (int y = 0; y < f->height(); y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < f->width(); x++) {
                    if (row[x * 4 + 3] == 0) continue;
                    const uint32_t rgb = craftRgb(row + x * 4);
                    auto it = std::lower_bound(counts.begin(), counts.end(), rgb, [](const std::pair<uint32_t, int>& e, uint32_t v) { return e.first < v; });
                    if (craftDistance(labs[(size_t)(it - counts.begin())], back) < CRAFT_FAINT) r.addSpot({(int)fi + 1, x, y});
                }
            }
        }
        const std::string hex = craftHex((uint32_t)bad.r << 16 | (uint32_t)bad.g << 8 | bad.b);
        if (worstShare > threshold.value) {
            r.status = LintStatus::Warn;
            r.message = "on the backdrop " + hex + ", " + craftNumber(worstShare * 100.0, 0) + "% of the drawn pixels barely differ from it: the sprite will not read there";
            r.fix = "edit_sprite: add a darker or lighter outline so the sprite reads on " + hex + " (S47)";
        } else {
            r.status = LintStatus::Pass;
            r.at.clear();
        }
        return r;
    }

    LintResult lintBanding(const FrameView& frames, const LintThreshold& threshold) {
        LintResult r = craftStarted("banding", threshold);
        BandScratch s;
        int pixels = 0;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f || f->empty()) continue;
            const int w = f->width(), h = f->height();
            s.inGroup.assign((size_t)w * h, 0);
            s.keys.assign((size_t)w * h, 0);
            s.lightness.assign((size_t)w * h, 0.0f);
            for (int y = 0; y < h; y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < w; x++) {
                    if (row[x * 4 + 3] != 255) continue;
                    const uint32_t rgb = craftRgb(row + x * 4);
                    s.keys[(size_t)y * w + x] = rgb | 0x1000000u;
                    s.lightness[(size_t)y * w + x] = s.labs.of(rgb).l;
                }
            }
            for (int kind = 0; kind < 4; kind++) {
                BandFamily fam;
                fam.kind = kind;
                fam.w = w;
                fam.h = h;
                const int lines = fam.lineCount();
                s.lines.resize((size_t)lines);
                s.pool.clear();
                for (int l = 0; l < lines; l++) bandRampsOf(fam, l, s, s.lines[(size_t)l]);
                for (int l = 0; l + 1 < lines; l++) {
                    std::vector<BandRamp>& below = s.lines[(size_t)l + 1];
                    for (BandRamp& ramp : s.lines[(size_t)l]) {
                        for (int shift = -1; shift <= 1; shift++) {
                            const BandRamp* next = bandNext(s, below, ramp, shift);
                            if (!next) continue;
                            const int index = (int)(next - below.data());
                            ramp.next[shift + 1] = index;
                            below[(size_t)index].linked[shift + 1] = true;
                        }
                    }
                }
                for (int shift = -1; shift <= 1; shift++) {
                    for (int l = 0; l + BAND_MIN_LINES <= lines; l++) {
                        for (const BandRamp& ramp : s.lines[(size_t)l]) {
                            // a group is read once, from its first line
                            if (ramp.linked[shift + 1]) continue;
                            int length = 1;
                            const BandRamp* at = &ramp;
                            while (at->next[shift + 1] >= 0) {
                                at = &s.lines[(size_t)(l + length)][(size_t)at->next[shift + 1]];
                                length++;
                            }
                            if (length < BAND_MIN_LINES) continue;
                            at = &ramp;
                            for (int k = 0; k < length; k++) {
                                bandMark(fam, l + k, *at, w, s.inGroup);
                                if (k + 1 < length) at = &s.lines[(size_t)(l + k + 1)][(size_t)at->next[shift + 1]];
                            }
                            int x, y;
                            fam.pixelOf(l, ramp.pos, x, y);
                            r.addSpot({(int)fi + 1, x, y});
                        }
                    }
                }
            }
            for (uint8_t v : s.inGroup) pixels += v;
        }
        r.measured = pixels;
        if (r.count > 0 && pixels >= threshold.value) {
            r.status = LintStatus::Warn;
            r.message = std::to_string(pixels) + " pixels in " + std::to_string(r.count) + " band group" + (r.count == 1 ? "" : "s") +
                        ": tone steps of one length line up into stripes";
            r.fix = "edit_sprite: break the stripes at " + craftWhere(r) + " (vary the step lengths, or merge two tones of the ramp) (S47)";
        } else {
            r.status = LintStatus::Pass;
            r.at.clear();
        }
        return r;
    }

    LintResult lintPillowShading(const FrameView& frames, const LintThreshold& threshold) {
        PillowScratch s;
        std::vector<PillowBody> bodies;
        std::vector<double> lvalues, dvalues;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f || f->empty()) continue;
            const int w = f->width(), h = f->height();
            const size_t n = (size_t)w * h;
            s.label.assign(n, 0);
            s.dist.assign(n, 0);
            s.lightness.assign(n, 0.0f);
            for (int y = 0; y < h; y++) {
                const uint8_t* row = f->row(y);
                for (int x = 0; x < w; x++) {
                    if (row[x * 4 + 3] == 255) s.lightness[(size_t)y * w + x] = s.labs.of(craftRgb(row + x * 4)).l;
                }
            }
            int bodyId = 0;
            for (int start = 0; start < (int)n; start++) {
                if (s.label[(size_t)start] != 0 || !craftOpaque(*f, start % w, start / w)) continue;
                bodyId++;
                s.body.clear();
                s.body.push_back(start);
                s.label[(size_t)start] = bodyId;
                for (size_t k = 0; k < s.body.size(); k++) {
                    const int i = s.body[k], x = i % w, y = i / w;
                    const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                    for (const auto& p : nb) {
                        if (p[0] < 0 || p[1] < 0 || p[0] >= w || p[1] >= h) continue;
                        const int j = p[1] * w + p[0];
                        if (s.label[(size_t)j] == 0 && craftOpaque(*f, p[0], p[1])) {
                            s.label[(size_t)j] = bodyId;
                            s.body.push_back(j);
                        }
                    }
                }
                if ((int)s.body.size() < PILLOW_MIN_PIXELS) continue;
                // distinct lightness levels
                lvalues.clear();
                for (int i : s.body) lvalues.push_back(s.lightness[(size_t)i]);
                if ((int)pillowRanks(lvalues, s.sorted, s.levelRank, s.rankL) < PILLOW_MIN_LEVELS) continue;
                // d: 4-connected distance to the outside (off the frame counts as outside)
                s.queue.clear();
                for (int i : s.body) {
                    const int x = i % w, y = i / w;
                    auto outside = [&](int nx, int ny) { return nx < 0 || ny < 0 || nx >= w || ny >= h || s.label[(size_t)ny * w + nx] != bodyId; };
                    if (outside(x - 1, y) || outside(x + 1, y) || outside(x, y - 1) || outside(x, y + 1)) {
                        s.dist[(size_t)i] = 1;
                        s.queue.push_back(i);
                    } else {
                        s.dist[(size_t)i] = 0;
                    }
                }
                for (size_t k = 0; k < s.queue.size(); k++) {
                    const int i = s.queue[k], x = i % w, y = i / w;
                    const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                    for (const auto& p : nb) {
                        if (p[0] < 0 || p[1] < 0 || p[0] >= w || p[1] >= h) continue;
                        const int j = p[1] * w + p[0];
                        if (s.label[(size_t)j] == bodyId && s.dist[(size_t)j] == 0) {
                            s.dist[(size_t)j] = s.dist[(size_t)i] + 1;
                            s.queue.push_back(j);
                        }
                    }
                }
                dvalues.clear();
                for (int i : s.body) dvalues.push_back(s.dist[(size_t)i]);
                PillowBody body;
                body.frame = (int)fi + 1;
                pillowRanks(dvalues, s.sorted, s.levelRank, s.rankD);
                body.rho = pillowPearson(s.rankL, s.rankD);
                // the least-squares plane L = a + b x + c y, centred
                const double count = (double)s.body.size();
                double mx = 0, my = 0, ml = 0;
                for (size_t k = 0; k < s.body.size(); k++) {
                    mx += s.body[k] % w;
                    my += s.body[k] / w;
                    ml += lvalues[k];
                }
                mx /= count;
                my /= count;
                ml /= count;
                double sxx = 0, syy = 0, sxy = 0, sxl = 0, syl = 0, sll = 0;
                for (size_t k = 0; k < s.body.size(); k++) {
                    const double x = s.body[k] % w - mx, y = s.body[k] / w - my, l = lvalues[k] - ml;
                    sxx += x * x;
                    syy += y * y;
                    sxy += x * y;
                    sxl += x * l;
                    syl += y * l;
                    sll += l * l;
                }
                const double det = sxx * syy - sxy * sxy;
                double explained = 0.0;
                if (std::fabs(det) > 1e-12) {
                    const double b = (sxl * syy - syl * sxy) / det;
                    const double c = (syl * sxx - sxl * sxy) / det;
                    explained = b * sxl + c * syl;
                } else if (sxx > 0.0) {
                    explained = sxl * sxl / sxx;
                } else if (syy > 0.0) {
                    explained = syl * syl / syy;
                }
                body.r2 = sll > 0.0 ? std::clamp(explained / sll, 0.0, 1.0) : 0.0;
                // the spread of the edge pixels' mean L over the quadrants around the centroid
                double sum[4] = {0, 0, 0, 0};
                int num[4] = {0, 0, 0, 0};
                int maxD = 0;
                for (size_t k = 0; k < s.body.size(); k++) {
                    const int i = s.body[k];
                    maxD = std::max(maxD, s.dist[(size_t)i]);
                    if (s.dist[(size_t)i] != 1) continue;
                    const int q = (i % w < mx ? 0 : 1) + (i / w < my ? 0 : 2);
                    sum[q] += lvalues[k];
                    num[q]++;
                }
                double lo = 0, hi = 0;
                bool first = true;
                for (int q = 0; q < 4; q++) {
                    if (num[q] == 0) continue;
                    const double mean = sum[q] / num[q];
                    lo = first ? mean : std::min(lo, mean);
                    hi = first ? mean : std::max(hi, mean);
                    first = false;
                }
                body.balance = hi - lo;
                body.pillow = body.rho >= threshold.value && body.r2 <= PILLOW_MAX_R2 && body.balance <= PILLOW_MAX_BALANCE;
                body.centroid = {body.frame, (int)std::floor(mx + 0.5), (int)std::floor(my + 0.5)};
                // the extreme rings: the first pixel (in raster order) of the outer ring and of the innermost
                int outer = -1, inner = -1;
                for (int i : s.body) {
                    if (s.dist[(size_t)i] == 1 && (outer < 0 || i < outer)) outer = i;
                    if (s.dist[(size_t)i] == maxD && (inner < 0 || i < inner)) inner = i;
                }
                body.outer = {body.frame, outer % w, outer / w};
                body.inner = {body.frame, inner % w, inner / w};
                bodies.push_back(body);
            }
        }
        if (bodies.empty()) {
            return craftSkipped("pillow_shading", "no opaque shape of at least 64 pixels with 3 or more lightness levels", &threshold);
        }
        LintResult r = craftStarted("pillow_shading", threshold);
        const PillowBody* worst = nullptr;
        int pillows = 0;
        for (const PillowBody& b : bodies) {
            if (b.pillow) {
                pillows++;
                r.addSpot(b.centroid);
                r.addSpot(b.outer);
                r.addSpot(b.inner);
            }
            const bool better = !worst || (b.pillow && !worst->pillow) || (b.pillow == worst->pillow && b.rho > worst->rho);
            if (better) worst = &b;
        }
        r.measured = worst->rho;
        if (pillows > 0) {
            r.status = LintStatus::Warn;
            r.message = std::to_string(pillows) + " shape" + (pillows == 1 ? "" : "s") + " shaded from every edge inward (rho " + craftNumber(worst->rho, 2) +
                        ", plane R^2 " + craftNumber(worst->r2, 2) + ", edge balance " + craftNumber(worst->balance, 3) + "): pillow shading";
            r.fix = "edit_sprite: shade from one light direction (lighter on the lit side, darker opposite), not from every edge inward (S47)";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintEmptyFrame(const FrameView& frames, std::span<const LintAnimation> animations) {
        if (animations.empty()) return craftSkipped("empty_frame", "no animations given");
        LintThreshold none;
        LintResult r = craftStarted("empty_frame", none);
        std::vector<uint8_t> judged(frames.size(), 0);
        for (const LintAnimation& a : animations) {
            for (int i : a.frames) {
                if (i < 0 || i >= (int)frames.size() || judged[(size_t)i]) continue;
                judged[(size_t)i] = 1;
                const PixelImage* f = frames[(size_t)i];
                bool any = false;
                for (int y = 0; f && y < f->height() && !any; y++) {
                    for (int x = 0; x < f->width(); x++) {
                        if (craftDrawn(*f, x, y)) {
                            any = true;
                            break;
                        }
                    }
                }
                if (!any) r.addSpot({i + 1, 0, 0});
            }
        }
        r.measured = r.count;
        if (r.count > 0) {
            r.status = LintStatus::Fail;
            r.message = "frame " + std::to_string(r.at.front().frame) + (r.count > 1 ? " and " + std::to_string(r.count - 1) + " more" : "") + " have nothing in them";
            r.fix = "fix_pixel_art: move the frame's box onto the art in recipe.sheet, or set its include to false";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintOneFrame(std::span<const LintAnimation> animations) {
        if (animations.empty()) return craftSkipped("one_frame", "no animations given");
        LintThreshold none;
        LintResult r = craftStarted("one_frame", none);
        std::string names;
        for (const LintAnimation& a : animations) {
            if (a.frames.size() != 1) continue;
            r.addSpot({a.frames.front() + 1, 0, 0});
            if (r.count <= 4) names += (names.empty() ? "" : ", ") + a.name;
        }
        r.measured = r.count;
        if (r.count > 0) {
            r.status = LintStatus::Warn;
            r.message = names + (r.count == 1 ? " has" : " have") + " 1 frame";
            r.fix = "fix_pixel_art: check its frames in recipe.sheet, or join it to the animation it belongs to";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintFrameScale(std::span<const double> frameCells, double sharedCell, const LintThreshold& threshold) {
        if (sharedCell <= 0.0 || frameCells.empty()) return craftSkipped("frame_scale", "no shared cell size (a cleaned animation sheet only)", &threshold);
        LintResult r = craftStarted("frame_scale", threshold);
        for (size_t i = 0; i < frameCells.size(); i++) {
            const double cell = frameCells[i];
            if (cell > 0.0 && std::fabs(cell - sharedCell) > threshold.value * sharedCell) r.addSpot({(int)i + 1, 0, 0});
        }
        r.measured = r.count;
        if (r.count > 0) {
            r.status = LintStatus::Warn;
            r.message = "frame " + std::to_string(r.at.front().frame) + " looks drawn at another size (" + craftNumber(frameCells[(size_t)r.at.front().frame - 1], 2) +
                        " px cells, against " + craftNumber(sharedCell, 2) + "): it is read at the animation's size";
            r.fix = "fix_pixel_art: check that frame's box in recipe.sheet, or leave it out";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

}
