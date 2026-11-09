// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/sheet.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupSheet.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;

    namespace {

        // sheet.ts sameRow: middles at about one height and heights alike
        bool sheetSameRow(const FrameBox& a, const FrameBox& b) {
            const double least = std::min(a.height, b.height);
            return std::abs(a.y + a.height / 2.0 - (b.y + b.height / 2.0)) <= 0.25 * least &&
                   std::max(a.height, b.height) <= 1.5 * least;
        }

        // sheet.ts rowsOf: each row's indices left to right, the rows top to bottom
        std::vector<std::vector<int>> sheetRowsOf(const std::vector<FrameBox>& boxes) {
            std::vector<std::vector<int>> rows;
            for (int i = 0; i < (int)boxes.size(); i++) {
                auto row = std::find_if(rows.begin(), rows.end(), [&](const std::vector<int>& other) {
                    return sheetSameRow(boxes[other[0]], boxes[i]);
                });
                if (row == rows.end()) rows.push_back({i});
                else row->push_back(i);
            }
            for (auto& row : rows) {
                std::stable_sort(row.begin(), row.end(), [&](int a, int b) { return boxes[a].x < boxes[b].x; });
            }
            std::stable_sort(rows.begin(), rows.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
                const FrameBox& first = boxes[a[0]];
                const FrameBox& second = boxes[b[0]];
                const bool level = std::abs(first.y - second.y) <= 0.25 * std::min(first.height, second.height);
                return level ? first.x < second.x : first.y < second.y;
            });
            return rows;
        }

        // A name no other animation has: name, or name-2, name-3, ...
        std::string sheetFreeName(const std::vector<SheetAnimation>& animations, const std::string& name, int skip = -1) {
            std::set<std::string> taken;
            for (int i = 0; i < (int)animations.size(); i++) {
                if (i != skip) taken.insert(animations[i].name);
            }
            if (!taken.count(name)) return name;
            int n = 2;
            while (taken.count(name + "-" + std::to_string(n))) n++;
            return name + "-" + std::to_string(n);
        }

        // Border colours closer than this (OKLab) count as one; JPEG noise would split a flat colour (sheet.ts)
        constexpr double SAME_COLOR = 0.06;
        // A border colour is background when it covers at least this share of the border (sheet.ts)
        constexpr double COMMON_ENOUGH = 0.03;
        // A row (or column) is a painted line when the line's colour covers this share of it (sheet.ts)
        constexpr double LINE_SHARE = 0.25;
        // How far past a line its two sides are looked at, for blurred line edges (sheet.ts)
        constexpr int LINE_SIDE = 2;
        // A band shorter than this share of the tallest is a row of labels (sheet.ts)
        constexpr double LABEL_BAND = 0.4;
        // How many times a box is cut into rows again (sheet.ts)
        constexpr int DEPTH = 3;

        Rgba8 sheetPixel(const RgbaImage& image, int p) {
            const uint8_t* d = image.pixels().data() + static_cast<size_t>(p) * 4;
            return Rgba8{d[0], d[1], d[2], 255};
        }

        uint8_t sheetAlpha(const RgbaImage& image, int p) { return image.pixels()[static_cast<size_t>(p) * 4 + 3]; }

        uint32_t sheetKey(const RgbaImage& image, int p) {
            const uint8_t* d = image.pixels().data() + static_cast<size_t>(p) * 4;
            return (uint32_t(d[0]) << 16) | (uint32_t(d[1]) << 8) | d[2];
        }

        struct SheetRun {
            int from = 0;
            int to = 0;   // exclusive
        };

        // Runs of true, joined across gaps shorter than `bridge`
        std::vector<SheetRun> sheetRunsOf(const std::vector<bool>& occupied, int bridge) {
            std::vector<SheetRun> runs;
            for (int i = 0; i < (int)occupied.size(); i++) {
                if (!occupied[i]) continue;
                if (!runs.empty() && i - runs.back().to < bridge) runs.back().to = i + 1;
                else runs.push_back({i, i + 1});
            }
            return runs;
        }

        struct SheetCounts {
            std::vector<int> rows, columns;
        };

        // Opaque pixels per row and per column of a part of the picture
        SheetCounts sheetCounts(const RgbaImage& image, const FrameBox& area) {
            SheetCounts c;
            c.rows.assign(std::max(0, area.height), 0);
            c.columns.assign(std::max(0, area.width), 0);
            const uint8_t* data = image.pixels().data();
            for (int y = 0; y < area.height; y++) {
                for (int x = 0; x < area.width; x++) {
                    if (data[((static_cast<size_t>(area.y) + y) * image.width() + area.x + x) * 4 + 3] >= 128) {
                        c.rows[y]++;
                        c.columns[x]++;
                    }
                }
            }
            return c;
        }

        struct SheetBands {
            std::vector<SheetRun> bands, kept;
        };

        // The rows of art in one part of the sheet, and those tall enough to be frames
        SheetBands sheetBandsIn(const RgbaImage& image, const FrameBox& area) {
            const SheetCounts c = sheetCounts(image, area);
            // A row with a pixel or two in it is noise left by the clearing, not art
            const int noise = std::max(1, (int)jsRound(image.width() * 0.001));
            std::vector<bool> occupied(c.rows.size());
            for (size_t i = 0; i < c.rows.size(); i++) occupied[i] = c.rows[i] > noise;
            SheetBands out;
            for (const SheetRun& band : sheetRunsOf(occupied, std::max(2, (int)jsRound(image.height() * 0.01)))) {
                if (band.to - band.from >= 3) out.bands.push_back({band.from + area.y, band.to + area.y});
            }
            int tallest = 0;
            for (const SheetRun& band : out.bands) tallest = std::max(tallest, band.to - band.from);
            for (const SheetRun& band : out.bands) {
                if (band.to - band.from >= LABEL_BAND * tallest) out.kept.push_back(band);
            }
            return out;
        }

        // Which gaps (by index) separate frames: the widest ones, down to the biggest drop in width
        std::vector<int> sheetWidestGaps(const std::vector<int>& gaps, double least) {
            std::vector<std::pair<int, int>> order;   // size, index
            for (int i = 0; i < (int)gaps.size(); i++) order.push_back({gaps[i], i});
            std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<std::pair<int, int>> wide;
            for (const auto& gap : order) {
                if (gap.first >= least) wide.push_back(gap);
            }
            if (wide.empty()) return {};
            if (wide.size() == 1) return {wide[0].second};
            size_t cut = wide.size();
            double drop = 1;
            for (size_t k = 0; k + 1 < wide.size(); k++) {
                const double ratio = double(wide[k].first) / std::max(1, wide[k + 1].first);
                if (ratio > drop) {
                    drop = ratio;
                    cut = k + 1;
                }
            }
            // Gaps all about as wide split everywhere
            if (drop < 1.6) cut = wide.size();
            std::vector<int> out;
            for (size_t k = 0; k < cut; k++) out.push_back(wide[k].second);
            return out;
        }

        double sheetMedianOfSorted(std::vector<int> values) {
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        // The frames inside one part of the sheet, split only at gaps at least `least` wide; adds
        // the width of each gap it split frames at to `spacing`
        std::vector<FrameBox> sheetFramesIn(const RgbaImage& image, const FrameBox& area, double least, std::vector<int>& spacing) {
            const SheetBands found = sheetBandsIn(image, area);
            const std::vector<SheetRun>& bands = found.bands;
            if (bands.empty()) return {};
            const int bottomOf = area.y + area.height;

            std::vector<FrameBox> boxes;
            for (const SheetRun& band : found.kept) {
                // The band's own space, halfway to the bands around it (labels included)
                int index = 0;
                while (index < (int)bands.size() && (bands[index].from != band.from || bands[index].to != band.to)) index++;
                const SheetRun* above = index > 0 ? &bands[index - 1] : nullptr;
                const SheetRun* below = index + 1 < (int)bands.size() ? &bands[index + 1] : nullptr;
                const double height = band.to - band.from;
                const double aboveTo = above ? above->to : std::max<double>(area.y, band.from - height * 0.1);
                const double belowFrom = below ? below->from : std::min<double>(bottomOf, band.to + height * 0.1);
                const double marginTop = jsRound((band.from - aboveTo) / 2);
                const double marginBottom = jsRound((belowFrom - band.to) / 2);
                const int top = (int)std::max<double>(area.y, band.from - std::min(marginTop, jsRound(0.15 * height)));
                const int bottom = (int)std::min<double>(bottomOf, band.to + std::min(marginBottom, jsRound(0.15 * height)));
                const SheetCounts c = sheetCounts(image, {area.x, band.from, area.width, band.to - band.from});
                const std::vector<int>& columns = c.columns;
                std::vector<bool> occupied(columns.size());
                for (size_t i = 0; i < columns.size(); i++) occupied[i] = columns[i] > 0;
                std::vector<SheetRun> pieces = sheetRunsOf(occupied, 1);
                for (SheetRun& piece : pieces) {
                    piece.from += area.x;
                    piece.to += area.x;
                }
                if (pieces.empty()) continue;
                // Gaps between the pieces; the frames are split at the widest ones
                std::vector<int> gapSizes;
                for (size_t i = 1; i < pieces.size(); i++) gapSizes.push_back(pieces[i].from - pieces[i - 1].to);
                const std::vector<int> splits = sheetWidestGaps(gapSizes, least);
                std::set<int> cuts(splits.begin(), splits.end());
                for (int i : splits) spacing.push_back(gapSizes[i]);
                int start = pieces[0].from;
                std::vector<SheetRun> groups;
                for (int i = 0; i < (int)pieces.size(); i++) {
                    if (cuts.count(i)) {
                        groups.push_back({start, pieces[i].to});
                        start = pieces[i + 1].from;
                    }
                }
                groups.push_back({start, pieces.back().to});
                // Two frames close together make one group about twice as wide as the rest: split
                // it at its own widest gap
                for (size_t pass = 0; pass < pieces.size(); pass++) {
                    std::vector<int> widths;
                    for (const SheetRun& run : groups) widths.push_back(run.to - run.from);
                    const double median = sheetMedianOfSorted(widths);
                    int wideAt = -1;
                    for (int g = 0; g < (int)groups.size(); g++) {
                        if (groups[g].to - groups[g].from > 1.6 * median) {
                            wideAt = g;
                            break;
                        }
                    }
                    if (groups.size() < 2 || wideAt < 0) break;
                    const SheetRun run = groups[wideAt];
                    std::vector<SheetRun> inside;
                    for (const SheetRun& piece : pieces) {
                        if (piece.from >= run.from && piece.to <= run.to) inside.push_back(piece);
                    }
                    int at = -1;
                    for (int i = 1; i < (int)inside.size(); i++) {
                        if (at < 0 || inside[i].from - inside[i - 1].to > inside[at].from - inside[at - 1].to) at = i;
                    }
                    if (at < 0 || inside[at].from - inside[at - 1].to < least) break;
                    groups.erase(groups.begin() + wideAt);
                    groups.insert(groups.begin() + wideAt, {{run.from, inside[at - 1].to}, {inside[at].from, run.to}});
                }
                // A group with almost nothing in it is noise when it sits against a frame, and joins
                // it; set apart like a frame, it is one (the last wisp of smoke of an explosion)
                auto mass = [&](const SheetRun& run) {
                    long sum = 0;
                    for (int x = run.from - area.x; x < run.to - area.x; x++) sum += columns[x];
                    return sum;
                };
                long biggest = std::numeric_limits<long>::min();
                for (const SheetRun& run : groups) biggest = std::max(biggest, mass(run));
                std::vector<int> spaced;
                for (size_t i = 1; i < groups.size(); i++) spaced.push_back(groups[i].from - groups[i - 1].to);
                const double usualGap = spaced.empty() ? 0.0 : sheetMedianOfSorted(spaced);
                std::vector<SheetRun> solid;
                for (int i = 0; i < (int)groups.size(); i++) {
                    const SheetRun run = groups[i];
                    const double inf = std::numeric_limits<double>::infinity();
                    const double before = i > 0 ? run.from - groups[i - 1].to : inf;
                    const double after = i + 1 < (int)groups.size() ? groups[i + 1].from - run.to : inf;
                    if (mass(run) >= 0.02 * biggest || std::min(before, after) >= 0.5 * usualGap) {
                        solid.push_back(run);
                        continue;
                    }
                    // Joins the nearer neighbour: the one before when it is already kept, else the next
                    if (!solid.empty() && before <= after) solid.back() = {solid.back().from, run.to};
                    else if (i + 1 < (int)groups.size()) groups[i + 1] = {run.from, groups[i + 1].to};
                    else if (!solid.empty()) solid.back() = {solid.back().from, run.to};
                }
                double typicalGap;
                if (solid.size() > 1) {
                    double sum = 0;
                    for (size_t i = 1; i < solid.size(); i++) sum += solid[i].from - solid[i - 1].to;
                    typicalGap = sum / double(solid.size() - 1);
                } else {
                    typicalGap = jsRound(0.05 * height);
                }
                for (int i = 0; i < (int)solid.size(); i++) {
                    const SheetRun& run = solid[i];
                    const int left = i == 0 ? (int)std::max<double>(area.x, run.from - jsRound(typicalGap / 2))
                                            : (int)jsRound((solid[i - 1].to + run.from) / 2.0);
                    const int right = i == (int)solid.size() - 1 ? (int)std::min<double>(area.x + area.width, run.to + jsRound(typicalGap / 2))
                                                                 : (int)jsRound((run.to + solid[i + 1].from) / 2.0);
                    boxes.push_back({left, top, right - left, bottom - top});
                }
            }
            return boxes;
        }

        // ritmo's `counts` mass of a box clamped to the picture
        long sheetBoxMass(const RgbaImage& image, const FrameBox& box) {
            const int x0 = std::max(0, box.x);
            const int y0 = std::max(0, box.y);
            const FrameBox area{x0, y0, std::min(image.width(), box.x + box.width) - x0, std::min(image.height(), box.y + box.height) - y0};
            long sum = 0;
            for (int n : sheetCounts(image, area).columns) sum += n;
            return sum;
        }

    }

    SheetAnimation animationOf(const std::string& name, std::vector<int> frames) {
        SheetAnimation animation;
        animation.name = name;
        animation.frames = std::move(frames);
        return animation;
    }

    std::vector<FrameBox> readingOrder(const std::vector<FrameBox>& boxes) {
        std::vector<FrameBox> out;
        for (const auto& row : sheetRowsOf(boxes)) {
            for (int i : row) out.push_back(boxes[i]);
        }
        return out;
    }

    std::vector<SheetAnimation> groupFrames(const std::vector<FrameBox>& boxes) {
        const std::vector<std::vector<int>> rows = sheetRowsOf(boxes);
        auto left = [&](const std::vector<int>& row) {
            int v = std::numeric_limits<int>::max();
            for (int i : row) v = std::min(v, boxes[i].x);
            return v;
        };
        auto right = [&](const std::vector<int>& row) {
            int v = std::numeric_limits<int>::min();
            for (int i : row) v = std::max(v, boxes[i].x + boxes[i].width);
            return v;
        };
        auto spanTop = [&](const std::vector<int>& row) {
            int v = std::numeric_limits<int>::max();
            for (int i : row) v = std::min(v, boxes[i].y);
            return v;
        };
        auto spanBottom = [&](const std::vector<int>& row) {
            int v = std::numeric_limits<int>::min();
            for (int i : row) v = std::max(v, boxes[i].y + boxes[i].height);
            return v;
        };
        std::vector<int> widths;
        for (const FrameBox& box : boxes) widths.push_back(box.width);
        std::sort(widths.begin(), widths.end());
        const double reach = 0.5 * (widths.empty() ? 0 : widths[widths.size() / 2]);
        // Where two rows sit side by side: the blocks' meeting line, and how many frames each block's rows hold
        struct Seam {
            double x;
            size_t before, after;
        };
        std::vector<Seam> seams;
        for (size_t a = 0; a < rows.size(); a++) {
            for (size_t b = 0; b < rows.size(); b++) {
                if (a == b) continue;
                const int topA = spanTop(rows[a]), bottomA = spanBottom(rows[a]);
                const int topB = spanTop(rows[b]), bottomB = spanBottom(rows[b]);
                const int overlap = std::min(bottomA, bottomB) - std::max(topA, topB);
                if (overlap > 0.3 * std::min(bottomA - topA, bottomB - topB) && right(rows[a]) <= left(rows[b]) + reach) {
                    seams.push_back({(right(rows[a]) + left(rows[b])) / 2.0, rows[a].size(), rows[b].size()});
                }
            }
        }
        std::vector<std::vector<int>> parts;
        for (const auto& row : rows) {
            bool split = false;
            for (const Seam& seam : seams) {
                int at = -1;
                for (int k = 0; k < (int)row.size(); k++) {
                    if (boxes[row[k]].x + boxes[row[k]].width / 2.0 > seam.x) {
                        at = k;
                        break;
                    }
                }
                // ritmo compares findIndex (-1 when none) with the counts
                if (at != (int)seam.before || (int)row.size() - at != (int)seam.after) continue;
                const int edge = boxes[row[at]].x;
                if (std::abs(edge - seam.x) <= reach) {
                    parts.push_back(std::vector<int>(row.begin(), row.begin() + at));
                    parts.push_back(std::vector<int>(row.begin() + at, row.end()));
                    split = true;
                    break;
                }
            }
            if (!split) parts.push_back(row);
        }
        std::vector<SheetAnimation> out;
        for (size_t i = 0; i < parts.size(); i++) out.push_back(animationOf("animation-" + std::to_string(i + 1), parts[i]));
        return out;
    }

    Sheet withFrames(const Sheet& sheet, const std::vector<SheetFrame>& frames) {
        Sheet out = sheet;
        out.frames = frames;
        std::vector<FrameBox> boxes;
        for (const SheetFrame& frame : frames) boxes.push_back(frame.box);
        out.animations = groupFrames(boxes);
        return out;
    }

    Sheet insertFrame(const Sheet& sheet, int index, const SheetFrame& frame) {
        Sheet out = sheet;
        index = std::clamp(index, 0, (int)sheet.frames.size());
        out.frames.insert(out.frames.begin() + index, frame);
        for (SheetAnimation& animation : out.animations) {
            for (int& i : animation.frames) {
                if (i >= index) i++;
            }
        }
        if (out.animations.empty()) {
            out.animations = {animationOf("animation-1", {index})};
            return out;
        }
        const int before = index - 1;
        int owner = -1;
        for (int a = 0; a < (int)out.animations.size() && owner < 0; a++) {
            const auto& list = out.animations[a].frames;
            if (std::find(list.begin(), list.end(), before) != list.end()) owner = a;
        }
        std::vector<int>& list = out.animations[owner < 0 ? 0 : owner].frames;
        const auto at = owner < 0 ? list.begin() : std::find(list.begin(), list.end(), before) + 1;
        list.insert(at, index);
        return out;
    }

    Sheet removeFrame(const Sheet& sheet, int index) {
        Sheet out = sheet;
        out.frames.clear();
        for (int i = 0; i < (int)sheet.frames.size(); i++) {
            if (i != index) out.frames.push_back(sheet.frames[i]);
        }
        out.animations.clear();
        for (SheetAnimation animation : sheet.animations) {
            std::vector<int> frames;
            for (int i : animation.frames) {
                if (i != index) frames.push_back(i > index ? i - 1 : i);
            }
            animation.frames = std::move(frames);
            if (!animation.frames.empty()) out.animations.push_back(std::move(animation));
        }
        return out;
    }

    Sheet splitAnimation(const Sheet& sheet, int animation, int at) {
        if (animation < 0 || animation >= (int)sheet.animations.size()) return sheet;
        const SheetAnimation& current = sheet.animations[animation];
        if (at < 1 || at >= (int)current.frames.size()) return sheet;
        Sheet out = sheet;
        SheetAnimation first = current;
        first.frames.assign(current.frames.begin(), current.frames.begin() + at);
        SheetAnimation second = current;
        second.frames.assign(current.frames.begin() + at, current.frames.end());
        second.name = sheetFreeName(sheet.animations, current.name);
        out.animations[animation] = first;
        out.animations.insert(out.animations.begin() + animation + 1, second);
        return out;
    }

    Sheet joinAnimation(const Sheet& sheet, int animation) {
        if (animation < 0 || animation + 1 >= (int)sheet.animations.size()) return sheet;
        Sheet out = sheet;
        SheetAnimation joined = sheet.animations[animation];
        const auto& next = sheet.animations[animation + 1].frames;
        joined.frames.insert(joined.frames.end(), next.begin(), next.end());
        out.animations[animation] = joined;
        out.animations.erase(out.animations.begin() + animation + 1);
        return out;
    }

    bool isAnimationName(std::string_view name) {
        if (name.empty()) return false;
        for (char c : name) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    Sheet renameAnimation(const Sheet& sheet, int animation, const std::string& name) {
        if (animation < 0 || animation >= (int)sheet.animations.size()) return sheet;
        // ritmo: trim, then every run of white space becomes one "-"
        std::string wanted;
        size_t b = name.find_first_not_of(" \t\r\n\f\v");
        size_t e = name.find_last_not_of(" \t\r\n\f\v");
        const std::string trimmed = b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
        bool inSpace = false;
        for (char c : trimmed) {
            const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
            if (space) {
                if (!inSpace) wanted.push_back('-');
                inSpace = true;
            } else {
                wanted.push_back(c);
                inSpace = false;
            }
        }
        if (!isAnimationName(wanted)) return sheet;
        Sheet out = sheet;
        out.animations[animation].name = sheetFreeName(sheet.animations, wanted, animation);
        return out;
    }

    Sheet setAnimation(const Sheet& sheet, int animation, const AnimationChange& change) {
        if (animation < 0 || animation >= (int)sheet.animations.size()) return sheet;
        Sheet out = sheet;
        SheetAnimation& current = out.animations[animation];
        if (change.include) current.include = *change.include;
        if (change.loop) current.loop = *change.loop;
        current.fps = std::min(60.0, std::max(0.5, change.fps.value_or(current.fps)));
        return out;
    }

    std::vector<Rgba8> sheetBackground(const RgbaImage& image) {
        const int width = image.width(), height = image.height();
        struct Group {
            Lab lab;
            Rgba8 color;
            int n;
        };
        std::vector<Group> groups;
        int total = 0;
        auto count = [&](int x, int y) {
            const int p = y * width + x;
            if (sheetAlpha(image, p) < 128) return;
            total++;
            const Rgba8 color = sheetPixel(image, p);
            const Lab lab = oklabOf(color);
            for (Group& other : groups) {
                if (labDistance(other.lab, lab) <= SAME_COLOR) {
                    other.n++;
                    return;
                }
            }
            groups.push_back({lab, color, 1});
        };
        // Every border pixel, two deep: enough to see a painted grid's lines
        for (int depth = 0; depth < std::min(2, height); depth++) {
            for (int x = 0; x < width; x++) {
                count(x, depth);
                count(x, height - 1 - depth);
            }
        }
        for (int depth = 0; depth < std::min(2, width); depth++) {
            for (int y = 0; y < height; y++) {
                count(depth, y);
                count(width - 1 - depth, y);
            }
        }
        std::vector<Group> common;
        for (const Group& group : groups) {
            if (group.n >= COMMON_ENOUGH * total) common.push_back(group);
        }
        std::stable_sort(common.begin(), common.end(), [](const Group& a, const Group& b) { return b.n < a.n; });
        std::vector<Rgba8> out;
        for (size_t i = 0; i < common.size() && i < 4; i++) out.push_back(common[i].color);
        return out;
    }

    double fillNoise(const RgbaImage& image, Rgba8 fill) {
        const int width = image.width(), height = image.height();
        const Lab target = oklabOf(fill);
        std::vector<double> found;
        auto look = [&](int x, int y) {
            const int p = y * width + x;
            if (sheetAlpha(image, p) < 128) return;
            const double d = labDistance(oklabOf(sheetPixel(image, p)), target);
            if (d <= SAME_COLOR) found.push_back(d);
        };
        for (int x = 0; x < width; x++) {
            look(x, 0);
            look(x, height - 1);
        }
        for (int y = 0; y < height; y++) {
            look(0, y);
            look(width - 1, y);
        }
        if (found.empty()) return SAME_COLOR;
        std::sort(found.begin(), found.end());
        return found[(size_t)std::floor(found.size() * 0.95)];
    }

    Unlined removeGridLines(const RgbaImage& image, const std::vector<Rgba8>& colors, double tolerance) {
        if (colors.size() < 2) return {image, colors};
        const Rgba8 fill = colors[0];
        const int width = image.width(), height = image.height();
        RgbaImage out = image;
        uint8_t* data = out.editPixels().data();
        const Lab fillLab = oklabOf(fill);
        // Colours repeat a lot: convert each one once
        std::unordered_map<uint32_t, Lab> labs;
        auto near = [&](int p, const Lab& target) {
            const uint8_t* d = data + static_cast<size_t>(p) * 4;
            if (d[3] < 128) return false;
            const uint32_t key = (uint32_t(d[0]) << 16) | (uint32_t(d[1]) << 8) | d[2];
            auto it = labs.find(key);
            if (it == labs.end()) it = labs.emplace(key, oklabOf(Rgba8{d[0], d[1], d[2], 255})).first;
            return labDistance(it->second, target) <= tolerance;
        };
        std::vector<Rgba8> kept{fill};
        for (size_t c = 1; c < colors.size(); c++) {
            const Rgba8 color = colors[c];
            const Lab lab = oklabOf(color);
            std::vector<bool> rowLine(height), columnLine(width);
            for (int y = 0; y < height; y++) {
                int n = 0;
                for (int x = 0; x < width; x++) n += near(y * width + x, lab) ? 1 : 0;
                rowLine[y] = double(n) / width >= LINE_SHARE;
            }
            for (int x = 0; x < width; x++) {
                int n = 0;
                for (int y = 0; y < height; y++) n += near(y * width + x, lab) ? 1 : 0;
                columnLine[x] = double(n) / height >= LINE_SHARE;
            }
            // Lines are thin; a checkerboard's second tone fills half of every row, in one thick run
            auto thin = [](const std::vector<SheetRun>& runs, int size) {
                std::vector<SheetRun> outRuns;
                for (const SheetRun& run : runs) {
                    if (run.to - run.from <= std::max(4.0, 0.01 * size)) outRuns.push_back(run);
                }
                return outRuns;
            };
            const std::vector<SheetRun> lineRows = thin(sheetRunsOf(rowLine, 1), height);
            const std::vector<SheetRun> lineColumns = thin(sheetRunsOf(columnLine, 1), width);
            if (lineRows.size() + lineColumns.size() < 2) {
                kept.push_back(color);
                continue;
            }
            auto paint = [&](int p) {
                uint8_t* d = data + static_cast<size_t>(p) * 4;
                d[0] = fill.r;
                d[1] = fill.g;
                d[2] = fill.b;
                d[3] = 255;
            };
            // Across a line, it is background when both sides are the fill (or another line beside
            // it); rows first, then columns, so a crossing is painted once its row line is
            std::vector<uint8_t> onRow(height, 0), onColumn(width, 0);
            for (const SheetRun& run : lineRows) std::fill(onRow.begin() + run.from, onRow.begin() + run.to, 1);
            for (const SheetRun& run : lineColumns) std::fill(onColumn.begin() + run.from, onColumn.begin() + run.to, 1);
            auto side = [&](int p, bool beside) { return near(p, fillLab) || (beside && near(p, lab)); };
            for (const SheetRun& run : lineRows) {
                const int above = run.from - 1 - LINE_SIDE;
                const int below = run.to + LINE_SIDE;
                for (int x = 0; x < width; x++) {
                    // At the sheet's edge, the one side there is
                    if ((above >= 0 && !side(above * width + x, onRow[above] == 1)) ||
                        (below < height && !side(below * width + x, onRow[below] == 1))) {
                        continue;
                    }
                    for (int y = std::max(0, run.from - LINE_SIDE); y < std::min(height, run.to + LINE_SIDE); y++) paint(y * width + x);
                }
            }
            for (const SheetRun& run : lineColumns) {
                const int left = run.from - 1 - LINE_SIDE;
                const int right = run.to + LINE_SIDE;
                for (int y = 0; y < height; y++) {
                    if ((left >= 0 && !side(y * width + left, onColumn[left] == 1)) ||
                        (right < width && !side(y * width + right, onColumn[right] == 1))) {
                        continue;
                    }
                    for (int x = std::max(0, run.from - LINE_SIDE); x < std::min(width, run.to + LINE_SIDE); x++) paint(y * width + x);
                }
            }
        }
        return {std::move(out), kept};
    }

    Cleared clearSheet(const RgbaImage& image, const std::vector<Rgba8>& colors, double tolerance) {
        const int width = image.width(), height = image.height();
        std::vector<Lab> targets;
        for (Rgba8 c : colors) targets.push_back(oklabOf(c));
        // Colours repeat a lot: remember each one's answer
        std::unordered_map<uint32_t, bool> seen;
        auto like = [&](int p) {
            if (sheetAlpha(image, p) < 128) return true;
            const uint32_t key = sheetKey(image, p);
            auto it = seen.find(key);
            if (it == seen.end()) {
                const Lab lab = oklabOf(sheetPixel(image, p));
                bool answer = false;
                for (const Lab& target : targets) answer = answer || labDistance(lab, target) <= tolerance;
                it = seen.emplace(key, answer).first;
            }
            return it->second;
        };
        Cleared out;
        out.mask.assign(static_cast<size_t>(width) * height, 0);
        std::vector<int> stack;
        auto seed = [&](int p) {
            if (out.mask[p] == 1 || !like(p)) return;
            out.mask[p] = 1;
            stack.push_back(p);
        };
        for (int x = 0; x < width; x++) {
            seed(x);
            seed((height - 1) * width + x);
        }
        for (int y = 0; y < height; y++) {
            seed(y * width);
            seed(y * width + width - 1);
        }
        const int total = width * height;
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            const int x = p % width;
            if (x > 0) seed(p - 1);
            if (x < width - 1) seed(p + 1);
            if (p >= width) seed(p - width);
            if (p + width < total) seed(p + width);
        }
        out.image = image;
        uint8_t* data = out.image.editPixels().data();
        for (int p = 0; p < total; p++) {
            if (out.mask[p] == 1) data[static_cast<size_t>(p) * 4 + 3] = 0;
        }
        return out;
    }

    std::vector<FrameBox> findFrames(const RgbaImage& image) {
        const double least = std::max(2.0, 0.01 * image.width());
        std::vector<int> spacing;
        const std::vector<FrameBox> top = sheetFramesIn(image, {0, 0, image.width(), image.height()}, least, spacing);
        // Below the top, frames split only at gaps not much narrower than the narrowest gap that
        // already split two frames: one frame's own gaps (a body and its bomb) are narrower
        const double usual = spacing.empty() ? least : std::max(least, 0.6 * *std::min_element(spacing.begin(), spacing.end()));
        std::vector<FrameBox> all;
        auto again = [&](auto& self, const FrameBox& box, int depth) -> void {
            if (depth >= DEPTH || sheetBandsIn(image, box).kept.size() < 2) {
                all.push_back(box);
                return;
            }
            std::vector<int> unused;
            const std::vector<FrameBox> inner = sheetFramesIn(image, box, usual, unused);
            if (inner.size() <= 1) {
                all.push_back(box);
                return;
            }
            for (const FrameBox& part : inner) self(self, part, depth + 1);
        };
        for (const FrameBox& box : top) again(again, box, 0);
        return readingOrder(all);
    }

    RgbaImage cropImage(const RgbaImage& image, const FrameBox& box) {
        const int x0 = std::max(0, std::min(image.width(), box.x));
        const int y0 = std::max(0, std::min(image.height(), box.y));
        const int x1 = std::max(x0, std::min(image.width(), box.x + box.width));
        const int y1 = std::max(y0, std::min(image.height(), box.y + box.height));
        if (x1 <= x0 || y1 <= y0) return RgbaImage();
        return image.crop({x0, y0, x1 - x0, y1 - y0});
    }

    std::vector<int> bodyOf(const PixelArt& art) {
        const int width = art.width, height = art.height;
        const auto& indices = art.indices;
        std::vector<uint8_t> seen(indices.size(), 0);
        std::vector<int> best;
        std::vector<int> group;
        for (int start = 0; start < (int)indices.size(); start++) {
            if (seen[start] == 1 || indices[start] == TRANSPARENT) continue;
            group.assign(1, start);
            seen[start] = 1;
            for (size_t k = 0; k < group.size(); k++) {
                const int p = group[k];
                const int x = p % width;
                const int y = (p - x) / width;
                auto visit = [&](int n) {
                    if (seen[n] == 1 || indices[n] == TRANSPARENT) return;
                    seen[n] = 1;
                    group.push_back(n);
                };
                if (x > 0) visit(p - 1);
                if (x < width - 1) visit(p + 1);
                if (y > 0) visit(p - width);
                if (y < height - 1) visit(p + width);
            }
            if (group.size() > best.size()) best = group;
        }
        return best;
    }

    std::optional<Point> anchorOf(const PixelArt& art, Anchor anchor) {
        if (anchor == Anchor::Box) return std::nullopt;
        const std::vector<int> body = bodyOf(art);
        if (body.empty()) return std::nullopt;
        double sumX = 0, sumY = 0;
        int bottom = 0;
        for (int p : body) {
            const int x = p % art.width;
            const int y = (p - x) / art.width;
            sumX += x;
            sumY += y;
            bottom = std::max(bottom, y);
        }
        const double n = double(body.size());
        return Point{(int)jsRound(sumX / n), anchor == Anchor::Center ? (int)jsRound(sumY / n) : bottom};
    }

    std::vector<PixelArt> sharePalette(const std::vector<PixelArt>& arts) {
        std::vector<Rgba8> palette;
        std::unordered_map<uint32_t, int> slots;
        std::vector<PixelArt> out;
        for (const PixelArt& art : arts) {
            std::vector<int> remap;
            for (Rgba8 color : art.palette) {
                const uint32_t key = jasperengine::pixelart::packRgb(color);
                auto it = slots.find(key);
                if (it == slots.end()) {
                    it = slots.emplace(key, (int)palette.size()).first;
                    palette.push_back(Rgba8{color.r, color.g, color.b, 255});
                }
                remap.push_back(it->second);
            }
            PixelArt shared;
            shared.width = art.width;
            shared.height = art.height;
            shared.indices.resize(art.indices.size());
            for (size_t i = 0; i < art.indices.size(); i++) {
                shared.indices[i] = art.indices[i] == TRANSPARENT ? TRANSPARENT : remap[art.indices[i]];
            }
            out.push_back(std::move(shared));
        }
        // every picture holds the whole shared palette, as ritmo's share one array
        for (PixelArt& art : out) art.palette = palette;
        return out;
    }

    Layout layOut(const std::vector<PixelArt>& arts, const std::vector<Point>& offsets, int padding) {
        int left = std::numeric_limits<int>::max(), top = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min(), bottom = std::numeric_limits<int>::min();
        for (size_t i = 0; i < arts.size(); i++) {
            const PixelArt& art = arts[i];
            const Point offset = offsets[i];
            for (int p = 0; p < (int)art.indices.size(); p++) {
                if (art.indices[p] == TRANSPARENT) continue;
                const int x = p % art.width + offset.x;
                const int y = p / art.width + offset.y;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        if (right < left) {
            left = top = right = bottom = 0;
        }
        const int pad = std::max(0, padding);
        Layout out;
        out.width = right - left + 1 + 2 * pad;
        out.height = bottom - top + 1 + 2 * pad;
        for (size_t i = 0; i < arts.size(); i++) {
            const PixelArt& art = arts[i];
            const Point offset = offsets[i];
            Placed placed;
            placed.art.width = out.width;
            placed.art.height = out.height;
            placed.art.palette = art.palette;
            placed.art.indices.assign(static_cast<size_t>(out.width) * out.height, TRANSPARENT);
            const int dx = offset.x - left + pad;
            const int dy = offset.y - top + pad;
            for (int p = 0; p < (int)art.indices.size(); p++) {
                if (art.indices[p] == TRANSPARENT) continue;
                const int x = p % art.width + dx;
                const int y = p / art.width + dy;
                if (x >= 0 && y >= 0 && x < out.width && y < out.height) placed.art.indices[static_cast<size_t>(y) * out.width + x] = art.indices[p];
            }
            placed.anchor = {-left + pad, -top + pad};
            out.frames.push_back(std::move(placed));
        }
        return out;
    }

    std::vector<int> playOrder(int count, Loop loop) {
        std::vector<int> order;
        for (int i = 0; i < count; i++) order.push_back(i);
        if (loop == Loop::Forward || count < 3) return order;
        for (int i = count - 2; i >= 1; i--) order.push_back(i);
        return order;
    }

    bool looksAnimated(const RgbaImage& image, const std::vector<FrameBox>& boxes) {
        if (boxes.size() < 2) return false;
        std::vector<long> masses;
        for (const FrameBox& box : boxes) masses.push_back(sheetBoxMass(image, box));
        auto median = [](auto values) {
            std::sort(values.begin(), values.end());
            return double(values[values.size() / 2]);
        };
        const double typicalMass = median(masses);
        std::vector<int> widths;
        for (const FrameBox& box : boxes) widths.push_back(box.width);
        const double typicalWidth = median(widths);
        int alike = 0;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (masses[i] >= 0.2 * typicalMass && masses[i] <= 5 * typicalMass && boxes[i].width >= 0.4 * typicalWidth &&
                boxes[i].width <= 2.5 * typicalWidth) {
                alike++;
            }
        }
        return alike >= 2 && alike >= 0.75 * boxes.size();
    }

}
