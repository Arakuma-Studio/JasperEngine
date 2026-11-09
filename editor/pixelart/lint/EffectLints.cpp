// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The spill, one-shot end, loop seam, determinism and slow-frame rules follow pixelfx's
// tools/check.mjs (MIT, Copyright (c) 2026 Patch), re-implemented on engine captures.

#include "EffectLints.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace jasperengine::editor::pixelart::lint {

    namespace {

        using jasperengine::pixelart::Rgba8;

        LintResult started(const char* id, const char* thresholdSource = "default") {
            LintResult r;
            r.id = id;
            r.thresholdSource = thresholdSource;
            return r;
        }

        LintResult skipped(const char* id, const std::string& why) {
            LintResult r = started(id);
            r.status = LintStatus::Skip;
            r.message = why;
            return r;
        }

        bool drawn(const PixelImage& image, int x, int y) {
            return image.row(y)[x * 4 + 3] != 0;
        }

        bool samePixel(const PixelImage& a, const PixelImage& b, int x, int y) {
            return std::memcmp(a.row(y) + x * 4, b.row(y) + x * 4, 4) == 0;
        }

        // pixels that differ (sizes differing: every pixel of the larger one)
        int changedPixels(const PixelImage& a, const PixelImage& b, IntPoint* first = nullptr) {
            if (a.width() != b.width() || a.height() != b.height()) {
                return std::max(a.width() * a.height(), b.width() * b.height());
            }
            int changed = 0;
            for (int y = 0; y < a.height(); y++) {
                for (int x = 0; x < a.width(); x++) {
                    if (!samePixel(a, b, x, y)) {
                        if (changed == 0 && first) *first = {x, y};
                        changed++;
                    }
                }
            }
            return changed;
        }

        std::string hexOf(const uint8_t* p) {
            char text[16];
            std::snprintf(text, sizeof(text), "#%02x%02x%02x", p[0], p[1], p[2]);
            return text;
        }

        std::string fixedText(double v, int digits) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", digits, v);
            return text;
        }

        std::string percentText(double share) {
            return fixedText(share * 100.0, 1) + " %";
        }

        // ---- the rim measure: 0 uncovered, 1 ink, 2 rim
        struct Classes {
            int w = 0, h = 0;
            std::vector<uint8_t> c;
            int at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? -1 : c[(size_t)y * w + x]; }
        };

        Classes classify(const PixelImage& frame, std::span<const Rgba8> rimColors, bool transpose) {
            Classes k;
            k.w = transpose ? frame.height() : frame.width();
            k.h = transpose ? frame.width() : frame.height();
            k.c.assign((size_t)k.w * k.h, 0);
            for (int y = 0; y < frame.height(); y++) {
                for (int x = 0; x < frame.width(); x++) {
                    const uint8_t* p = frame.row(y) + x * 4;
                    uint8_t cls = 0;
                    if (p[3] != 0) {
                        cls = 1;
                        for (const Rgba8& rim : rimColors) {
                            if (rim.r == p[0] && rim.g == p[1] && rim.b == p[2]) {
                                cls = 2;
                                break;
                            }
                        }
                    }
                    const int cx = transpose ? y : x, cy = transpose ? x : y;
                    k.c[(size_t)cy * k.w + cx] = cls;
                }
            }
            return k;
        }

        struct Run {
            int y = 0;
            int edge = 0;     // the rim / uncovered boundary, in pixel-edge coordinates
            int length = 0;
            int side = 0;     // +1: uncovered after the run, -1: before it
            int x0 = 0;
        };

        // Rim runs along each line between ink on one side and the uncovered screen on the other,
        // each turned into a width across the edge: the mean run length over the neighbouring lines
        // times the cosine of the edge's slope (fitted on the same lines), so a slanted or curved rim
        // of a constant width measures that width; lines steeper than 45 degrees are the other pass's
        void collectWidths(const Classes& k, bool transposed, std::vector<std::pair<int, IntPoint>>& out) {
            std::vector<std::vector<Run>> lines((size_t)k.h);
            for (int y = 0; y < k.h; y++) {
                int x = 0;
                while (x < k.w) {
                    if (k.at(x, y) != 2) {
                        x++;
                        continue;
                    }
                    const int x0 = x;
                    while (x < k.w && k.at(x, y) == 2) x++;
                    const int before = k.at(x0 - 1, y), after = k.at(x, y);
                    if ((before == 1 && after == 0) || (before == 0 && after == 1)) {
                        Run run;
                        run.y = y;
                        run.x0 = x0;
                        run.length = x - x0;
                        run.side = after == 0 ? 1 : -1;
                        run.edge = after == 0 ? x : x0;
                        lines[(size_t)y].push_back(run);
                    }
                }
            }
            constexpr int K = 3;
            for (int y = 0; y < k.h; y++) {
                for (const Run& run : lines[(size_t)y]) {
                    double sy = 0, se = 0, syy = 0, sye = 0, sl = 0;
                    int n = 0;
                    int previousEdge = run.edge;
                    // walk outwards from the line so each neighbour is matched to the closest run of
                    // its side near the previous match (a curve drifts a little each line)
                    auto take = [&](int yy, int& lastEdge) -> bool {
                        if (yy < 0 || yy >= k.h) return false;
                        const Run* best = nullptr;
                        for (const Run& other : lines[(size_t)yy]) {
                            if (other.side != run.side) continue;
                            if (!best || std::abs(other.edge - lastEdge) < std::abs(best->edge - lastEdge)) best = &other;
                        }
                        if (!best || std::abs(best->edge - lastEdge) > 2 + run.length) return false;
                        lastEdge = best->edge;
                        sy += yy;
                        se += best->edge;
                        syy += (double)yy * yy;
                        sye += (double)yy * best->edge;
                        sl += best->length;
                        n++;
                        return true;
                    };
                    take(y, previousEdge);
                    int up = run.edge, down = run.edge;
                    for (int d = 1; d <= K; d++) {
                        if (!take(y - d, up)) break;
                    }
                    for (int d = 1; d <= K; d++) {
                        if (!take(y + d, down)) break;
                    }
                    if (n < 2 * K) continue;
                    const double denom = n * syy - sy * sy;
                    const double slope = denom != 0.0 ? (n * sye - sy * se) / denom : 0.0;
                    if (std::abs(slope) > 1.0) continue;
                    const double width = (sl / n) / std::sqrt(1.0 + slope * slope);
                    const int rounded = (int)std::lround(width);
                    const IntPoint at = transposed ? IntPoint{y, run.x0} : IntPoint{run.x0, y};
                    out.push_back({rounded, at});
                }
            }
        }

    }

    FrameView frameView(std::span<const PixelImage> frames) {
        FrameView view;
        view.reserve(frames.size());
        for (const PixelImage& f : frames) view.push_back(&f);
        return view;
    }

    const std::vector<std::string>& effectLintIds() {
        static const std::vector<std::string> ids = {"spill", "one_shot_end", "loop_seam", "determinism", "flicker",
                                                     "off_palette", "partial_alpha", "frame_cost", "hit_stop_length", "transition_edges"};
        return ids;
    }

    bool isEffectLintId(const std::string& id) {
        const std::vector<std::string>& ids = effectLintIds();
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    LintResult lintSpill(const std::vector<FrameView>& runs, const std::vector<uint32_t>& seeds, const IntRect& bounds, int margin) {
        LintResult r = started("spill");
        r.threshold = 0;
        bool any = false;
        for (const FrameView& run : runs) any = any || !run.empty();
        if (!any) return skipped("spill", "no frames captured");
        const IntRect inside{bounds.x - margin, bounds.y - margin, bounds.w + 2 * margin, bounds.h + 2 * margin};
        int worst = 0, worstRun = 0, worstFrame = 0;
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        bool spans = false;
        for (size_t ri = 0; ri < runs.size(); ri++) {
            for (size_t fi = 0; fi < runs[ri].size(); fi++) {
                const PixelImage* frame = runs[ri][fi];
                if (!frame) continue;
                int outside = 0;
                for (int y = 0; y < frame->height(); y++) {
                    for (int x = 0; x < frame->width(); x++) {
                        if (!drawn(*frame, x, y) || inside.contains(x, y)) continue;
                        if (outside == 0 && r.at.size() < LintResult::kMaxSpots) r.at.push_back({(int)fi + 1, x, y});
                        outside++;
                        // the drawing's reach past the bounds, as pixelfx reports it
                        const int dx = x - bounds.x, dy = y - bounds.y;
                        if (!spans) {
                            minX = maxX = dx;
                            minY = maxY = dy;
                            spans = true;
                        } else {
                            minX = std::min(minX, dx);
                            maxX = std::max(maxX, dx);
                            minY = std::min(minY, dy);
                            maxY = std::max(maxY, dy);
                        }
                    }
                }
                if (outside > worst) {
                    worst = outside;
                    worstRun = (int)ri;
                    worstFrame = (int)fi + 1;
                }
            }
        }
        r.measured = worst;
        r.count = worst;
        if (worst == 0) {
            r.status = LintStatus::Pass;
            r.at.clear();
            return r;
        }
        r.status = LintStatus::Warn;
        // how far past the margin it reaches on each side
        const int left = std::max(0, -minX - margin), right = std::max(0, maxX - (bounds.w - 1) - margin);
        const int top = std::max(0, -minY - margin), bottom = std::max(0, maxY - (bounds.h - 1) - margin);
        const std::string seed = worstRun < (int)seeds.size() ? "seed " + std::to_string(seeds[(size_t)worstRun]) : "run " + std::to_string(worstRun + 1);
        r.message = std::to_string(worst) + " px drawn outside the " + std::to_string(bounds.w) + "x" + std::to_string(bounds.h) +
                    " bounds + " + std::to_string(margin) + " px margin (" + seed + ", frame " + std::to_string(worstFrame) +
                    "); the drawing spans x " + std::to_string(minX) + ".." + std::to_string(maxX) + ", y " + std::to_string(minY) + ".." +
                    std::to_string(maxY) + " from the bounds' corner, " + std::to_string(std::max({left, right, top, bottom})) +
                    " px past the margin (left " + std::to_string(left) + ", right " + std::to_string(right) + ", top " +
                    std::to_string(top) + ", bottom " + std::to_string(bottom) + ")";
        r.fix = "keep it inside the margin: lower the speed, spread, lifetime or size that throws it out, or move its origin";
        return r;
    }

    LintResult lintOneShotEnd(const PixelImage* last, bool loop) {
        if (loop) return skipped("one_shot_end", "a loop has no end");
        if (!last || last->empty()) return skipped("one_shot_end", "no frame at t = duration");
        LintResult r = started("one_shot_end");
        r.threshold = 0;
        for (int y = 0; y < last->height(); y++) {
            for (int x = 0; x < last->width(); x++) {
                if (drawn(*last, x, y)) r.addSpot({1, x, y});
            }
        }
        r.measured = r.count;
        r.status = r.count > 0 ? LintStatus::Warn : LintStatus::Pass;
        if (r.count > 0) {
            r.message = std::to_string(r.count) + " px still drawn at t = duration: a one-shot should end empty";
            r.fix = "end everything by the duration (shorter lifetimes, a dissolve or fade that reaches 1), or capture a longer duration";
        }
        return r;
    }

    LintResult lintLoopSeam(const FrameView& frames, bool loop) {
        if (!loop) return skipped("loop_seam", "a one-shot has no seam");
        if (frames.size() < 3) return skipped("loop_seam", "fewer than 3 frames");
        for (const PixelImage* f : frames) {
            if (!f) return skipped("loop_seam", "a frame is missing");
        }
        std::vector<int> steps;
        for (size_t i = 0; i + 1 < frames.size(); i++) steps.push_back(changedPixels(*frames[i], *frames[i + 1]));
        std::sort(steps.begin(), steps.end());
        const size_t p90 = std::min(steps.size() - 1, (size_t)std::floor(steps.size() * 0.9));
        const int typical = steps[p90];
        IntPoint first;
        const int seam = changedPixels(*frames.back(), *frames.front(), &first);
        LintResult r = started("loop_seam");
        r.threshold = std::max(2.0 * typical, 12.0);
        r.measured = seam;
        r.count = seam;
        if (seam > 0) r.at.push_back({1, first.x, first.y});
        if (seam > 2.0 * r.threshold) r.status = LintStatus::Fail;
        else if (seam > r.threshold) r.status = LintStatus::Warn;
        else r.status = LintStatus::Pass;
        if (r.status != LintStatus::Pass) {
            r.message = "loop seam: " + std::to_string(seam) + " px change from the last frame to the first (a normal step is " +
                        std::to_string(typical) + ")";
            r.fix = "make t = duration match t = 0 (a whole number of cycles, the same keys at both ends), or capture with the loop's true period";
        } else {
            r.at.clear();
        }
        return r;
    }

    LintResult lintDeterminism(const PixelImage* first, const PixelImage* second, double time) {
        if (!first || !second || first->empty() || second->empty()) return skipped("determinism", "two fresh captures were not made");
        LintResult r = started("determinism");
        r.threshold = 0;
        IntPoint at;
        const int changed = changedPixels(*first, *second, &at);
        r.measured = changed;
        r.count = changed;
        if (changed > 0) {
            r.at.push_back({1, at.x, at.y});
            r.status = LintStatus::Fail;
            r.message = "not deterministic: two fresh captures of t = " + fixedText(time, 3) + " s differ in " + std::to_string(changed) + " px";
            r.fix = "seed every random source (math.randomseed in onStart, a fixed particle seed), and drive motion from game time";
        } else {
            r.status = LintStatus::Pass;
        }
        return r;
    }

    LintResult lintFlicker(const FrameView& frames, bool loop, double warnShare) {
        if (frames.size() < 3) return skipped("flicker", "fewer than 3 frames");
        const PixelImage* base = frames.front();
        if (!base) return skipped("flicker", "a frame is missing");
        for (const PixelImage* f : frames) {
            if (!f || f->width() != base->width() || f->height() != base->height()) return skipped("flicker", "the frames differ in size");
        }
        LintResult r = started("flicker");
        r.threshold = warnShare;
        const int n = (int)frames.size();
        int lit = 0, blinking = 0;
        for (int y = 0; y < base->height(); y++) {
            for (int x = 0; x < base->width(); x++) {
                bool any = false;
                for (const PixelImage* f : frames) any = any || drawn(*f, x, y);
                if (!any) continue;
                lit++;
                bool blinks = false;
                for (int i = 0; i < n; i++) {
                    if (!loop && (i == 0 || i == n - 1)) continue;
                    const PixelImage& before = *frames[(size_t)((i - 1 + n) % n)];
                    const PixelImage& now = *frames[(size_t)i];
                    const PixelImage& after = *frames[(size_t)((i + 1) % n)];
                    if (!samePixel(now, before, x, y) && samePixel(after, before, x, y)) {
                        if (!blinks) r.addSpot({i + 1, x, y});
                        blinks = true;
                    }
                }
                blinking += blinks ? 1 : 0;
            }
        }
        r.count = blinking;
        r.measured = lit > 0 ? (double)blinking / lit : 0.0;
        r.status = r.measured > warnShare ? LintStatus::Warn : LintStatus::Pass;
        if (r.status == LintStatus::Warn) {
            r.message = std::to_string(blinking) + " of " + std::to_string(lit) + " drawn pixels (" + percentText(r.measured) +
                        ") change for exactly one frame and change back";
            r.fix = "hold changes for at least two frames (Pose Frame Rate, slower cycling, longer particle lifetimes)";
        } else {
            r.at.clear();
        }
        return r;
    }

    LintResult lintOffPalette(const FrameView& frames, std::span<const Rgba8> palette, const OffPaletteOptions& options) {
        if (palette.empty()) return skipped("off_palette", "no palette given");
        LintResult r = started("off_palette", options.thresholdSource.c_str());
        r.threshold = options.warnCount;
        std::set<uint32_t> colors;
        for (const Rgba8& c : palette) colors.insert((uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b);
        int opaque = 0;
        std::vector<std::string> listed;
        std::set<uint32_t> seen;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            for (int y = 0; y < f->height(); y++) {
                for (int x = 0; x < f->width(); x++) {
                    const uint8_t* p = f->row(y) + x * 4;
                    if (p[3] != 255) continue;
                    opaque++;
                    const uint32_t key = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
                    if (colors.count(key)) continue;
                    r.addSpot({(int)fi + 1, x, y});
                    if (listed.size() < 8 && seen.insert(key).second) listed.push_back(hexOf(p));
                }
            }
        }
        r.measured = r.count;
        const double share = opaque > 0 ? (double)r.count / opaque : 0.0;
        if (r.count > 0 && share > options.failShare) r.status = LintStatus::Fail;
        else if (r.count > options.warnCount) r.status = LintStatus::Warn;
        else r.status = LintStatus::Pass;
        if (r.status != LintStatus::Pass) {
            std::string list;
            for (const std::string& c : listed) list += (list.empty() ? "" : ", ") + c;
            r.message = std::to_string(r.count) + " opaque px (" + percentText(share) + ") are not palette colours: " + list;
            r.fix = options.paletteName.empty() ? std::string("map the art onto the palette (apply_palette), or use ramp colours in the effect")
                                                : "fix_pixel_art with palette: " + options.paletteName + " (or apply_palette onto it)";
        }
        return r;
    }

    LintResult lintPartialAlpha(const FrameView& frames) {
        LintResult r = started("partial_alpha");
        r.threshold = 0;
        bool any = false;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const PixelImage* f = frames[fi];
            if (!f) continue;
            any = true;
            for (int y = 0; y < f->height(); y++) {
                for (int x = 0; x < f->width(); x++) {
                    const uint8_t a = f->row(y)[x * 4 + 3];
                    if (a > 0 && a < 255) r.addSpot({(int)fi + 1, x, y});
                }
            }
        }
        if (!any) return skipped("partial_alpha", "no frames captured");
        r.measured = r.count;
        r.status = r.count > 0 ? LintStatus::Fail : LintStatus::Pass;
        if (r.count > 0) {
            r.message = std::to_string(r.count) + " px are partly transparent (0 < alpha < 255)";
            r.fix = "use whole pixels: Dither Fade instead of alpha fades, a Point (no filter) texture, alpha cut to 0 or 255";
        }
        return r;
    }

    LintResult lintFrameCost(const FrameCost& cost) {
        const bool cpu = cost.shownMs >= 0.0 && cost.hiddenMs >= 0.0;
        if (!cpu && cost.gpuMs < 0.0 && cost.taps < 0) {
            return skipped("frame_cost", cost.skipReason.empty() ? std::string("nothing was measured") : cost.skipReason);
        }
        LintResult r = started("frame_cost");
        r.threshold = 1.0;
        const double added = cpu ? std::max(0.0, cost.shownMs - cost.hiddenMs) : 0.0;
        const double worst = std::max(added, std::max(0.0, cost.gpuMs));
        r.measured = worst;
        const bool overTaps = cost.taps >= 0 && cost.taps > cost.tapBudget;
        if (worst > 4.0) r.status = LintStatus::Fail;
        else if (worst > 1.0 || overTaps) r.status = LintStatus::Warn;
        else r.status = LintStatus::Pass;
        r.count = (worst > 1.0 ? 1 : 0) + (overTaps ? 1 : 0);
        std::string parts;
        if (cpu) parts = "CPU +" + fixedText(added, 3) + " ms (shown " + fixedText(cost.shownMs, 3) + ", hidden " + fixedText(cost.hiddenMs, 3) + ")";
        if (cost.gpuMs >= 0.0) parts += std::string(parts.empty() ? "" : ", ") + "GPU +" + fixedText(cost.gpuMs, 3) + " ms";
        if (cost.taps >= 0) parts += std::string(parts.empty() ? "" : ", ") + std::to_string(cost.taps) + " texture taps (budget " + std::to_string(cost.tapBudget) + ")";
        r.message = "median frame cost: " + parts;
        if (r.status != LintStatus::Pass) r.fix = "turn off features it does not need, lower particle counts, or share one material between the copies";
        return r;
    }

    LintResult lintHitStopLength(double seconds, const std::string& skipReason) {
        if (seconds < 0.0) return skipped("hit_stop_length", skipReason.empty() ? std::string("hit-stops are measured in a Play capture") : skipReason);
        LintResult r = started("hit_stop_length");
        r.threshold = 0.3;
        r.measured = seconds;
        r.status = seconds > 0.3 ? LintStatus::Warn : LintStatus::Pass;
        r.count = seconds > 0.3 ? 1 : 0;
        if (r.status == LintStatus::Warn) {
            r.message = "a hit-stop of " + fixedText(seconds, 3) + " s: past 0.3 s it reads as a stall";
            r.fix = "freeze for 0.05-0.15 s (HitStop.play seconds), and let a shake carry the impact";
        }
        return r;
    }

    RimMeasure measureRim(const PixelImage& frame, std::span<const Rgba8> rimColors) {
        RimMeasure m;
        if (frame.empty()) return m;
        std::vector<std::pair<int, IntPoint>> widths;
        collectWidths(classify(frame, rimColors, false), false, widths);
        collectWidths(classify(frame, rimColors, true), true, widths);
        if (widths.empty()) return m;
        std::map<int, int> histogram;
        for (const auto& w : widths) histogram[w.first]++;
        int common = widths.front().first, best = 0;
        for (const auto& [width, count] : histogram) {
            if (count > best) {
                best = count;
                common = width;
            }
        }
        m.samples = (int)widths.size();
        m.narrowest = histogram.begin()->first;
        m.widest = histogram.rbegin()->first;
        for (const auto& w : widths) {
            if (w.first != common) {
                m.at = w.second;
                break;
            }
        }
        return m;
    }

    LintResult lintTransitionEdges(const FrameView& frames, std::span<const Rgba8> rimColors, bool transition) {
        if (!transition) return skipped("transition_edges", "not a transition");
        if (rimColors.empty()) return skipped("transition_edges", "the transition has no rim (rim 0)");
        LintResult r = started("transition_edges");
        r.threshold = 0;
        int spread = 0, measuredFrames = 0;
        for (size_t fi = 0; fi < frames.size(); fi++) {
            if (!frames[fi]) continue;
            const RimMeasure m = measureRim(*frames[fi], rimColors);
            if (m.samples == 0) continue;
            measuredFrames++;
            const int s = m.widest - m.narrowest;
            if (s > 0) r.addSpot({(int)fi + 1, m.at.x, m.at.y});
            spread = std::max(spread, s);
        }
        if (measuredFrames == 0) return skipped("transition_edges", "no frame shows a rim between ink and screen");
        r.measured = spread;
        r.status = spread > 0 ? LintStatus::Warn : LintStatus::Pass;
        if (spread > 0) {
            r.message = "the rim's width varies by " + std::to_string(spread) + " art px along the edge in " + std::to_string(r.count) + " frame(s)";
            r.fix = "draw the rim at whole art pixels (pixel_size 0 or the S41 grid scale) and keep rim within 0..8";
        }
        return r;
    }

    LintReport runEffectLints(const EffectLintInput& input, const std::vector<std::string>& only) {
        auto wanted = [&](const char* id) { return only.empty() || std::find(only.begin(), only.end(), id) != only.end(); };
        LintReport report;
        if (wanted("spill")) {
            if (!input.spillApplies) {
                report.add(skipped("spill", input.spillSkipReason.empty() ? std::string("the whole view is drawn") : input.spillSkipReason));
            } else {
                std::vector<FrameView> runs;
                runs.push_back(input.frames);
                for (const FrameView& run : input.spillRuns) runs.push_back(run);
                report.add(lintSpill(runs, input.spillSeeds, input.bounds, input.margin));
            }
        }
        if (wanted("one_shot_end")) {
            const PixelImage* last = input.last ? input.last : (input.frames.empty() ? nullptr : input.frames.back());
            // a transition draws no bounds, but its overlay pixels left at the end are what this counts (R28)
            if (!input.spillApplies && !input.loop && !input.transition) {
                report.add(skipped("one_shot_end", input.spillSkipReason.empty() ? std::string("the whole view is drawn") : input.spillSkipReason));
            } else {
                report.add(lintOneShotEnd(last, input.loop));
            }
        }
        if (wanted("loop_seam")) report.add(lintLoopSeam(input.frames, input.loop));
        if (wanted("determinism")) {
            if (!input.determinismFirst || !input.determinismSecond) {
                report.add(skipped("determinism", input.determinismSkipReason.empty() ? std::string("two fresh captures were not made")
                                                                                       : input.determinismSkipReason));
            } else {
                report.add(lintDeterminism(input.determinismFirst, input.determinismSecond, input.determinismTime));
            }
        }
        if (wanted("flicker")) report.add(lintFlicker(input.frames, input.loop));
        if (wanted("off_palette")) {
            OffPaletteOptions options;
            options.paletteName = input.paletteName;
            report.add(lintOffPalette(input.frames, input.palette, options));
        }
        if (wanted("partial_alpha")) report.add(lintPartialAlpha(input.frames));
        if (wanted("frame_cost")) report.add(lintFrameCost(input.cost));
        if (wanted("hit_stop_length")) report.add(lintHitStopLength(input.hitStopSeconds, input.hitStopSkipReason));
        if (wanted("transition_edges")) report.add(lintTransitionEdges(input.frames, input.rimColors, input.transition));
        return report;
    }

    qa::Json toCheckRow(const LintResult& result, const std::vector<double>& times) {
        qa::Json row = qa::row(result.id, "effect");
        row["status"] = lintStatusName(result.status);
        row["method"] = "capture";
        qa::Json spots = qa::Json::array();
        std::set<int> frames;
        for (const LintSpot& spot : result.at) {
            spots.push_back({spot.frame, spot.x, spot.y});
            if (frames.insert(spot.frame).second && spot.frame >= 1 && spot.frame <= (int)times.size()) qa::addAt(row, times[(size_t)spot.frame - 1]);
        }
        row["spots"] = std::move(spots);
        row["count"] = result.count;
        if (result.status != LintStatus::Skip) {
            row["measured"] = result.measured;
            row["threshold"] = result.threshold;
            row["threshold_source"] = result.thresholdSource;
        }
        if (!result.message.empty()) row["limited_by"] = result.message;
        if (!result.fix.empty()) row["fix_hint"] = result.fix;
        return row;
    }

    qa::Json toCheckRows(const LintReport& report, const std::vector<double>& times) {
        qa::Json rows = qa::Json::array();
        for (const LintResult& r : report.results) rows.push_back(toCheckRow(r, times));
        return rows;
    }

    Verdict effectVerdict(const LintReport& report) {
        Verdict verdict = Verdict::Clean;
        for (const LintResult& r : report.results) {
            if (r.status == LintStatus::Fail && (r.id == "determinism" || r.id == "frame_cost")) return Verdict::NeedsPerson;
            if (r.status == LintStatus::Warn || r.status == LintStatus::Fail) verdict = Verdict::Fixable;
        }
        return verdict;
    }

}
