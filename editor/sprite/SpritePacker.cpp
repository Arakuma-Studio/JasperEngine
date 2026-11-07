// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The free-rectangle bin below is a port of MaxRectsBinPack by Jukka Jylänki, public domain
// (https://github.com/juj/RectangleBinPack): its Best Short Side Fit search, SplitFreeNode,
// InsertNewFreeRectangle and PruneFreeList, with rotation removed and score ties broken by the
// smallest (y, x) so the layout does not depend on the order of the free list.

#include "sprite/SpritePacker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <utility>

namespace jasperengine::editor::sprite {

    using pixelart::IntPoint;
    using pixelart::IntRect;
    using pixelart::IntSize;
    using pixelart::PixelImage;
    using pixelart::PixelProblem;

    namespace {

        int roundUp4(int value) { return (value + 3) & ~3; }

        int roundUpPow2(int value) {
            int pow2 = 1;
            while (pow2 < value) pow2 <<= 1;
            return pow2;
        }

        int floorPow2(int value) {
            int pow2 = 1;
            while (pow2 <= value / 2) pow2 <<= 1;
            return pow2;
        }

        bool isContainedIn(const IntRect& a, const IntRect& b) {
            return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
        }

        class MaxRectsBin {
        public:
            MaxRectsBin(int origin, int width, int height) {
                if (width > origin && height > origin) freeRects.push_back({origin, origin, width - origin, height - origin});
            }

            bool find(int width, int height, IntPoint& out) const {
                int bestShort = INT32_MAX, bestLong = INT32_MAX;
                bool found = false;
                for (const IntRect& free : freeRects) {
                    if (free.w < width || free.h < height) continue;
                    const int leftoverH = free.w - width;
                    const int leftoverV = free.h - height;
                    const int shortSide = std::min(leftoverH, leftoverV);
                    const int longSide = std::max(leftoverH, leftoverV);
                    const bool better = shortSide < bestShort || (shortSide == bestShort && (longSide < bestLong ||
                                        (longSide == bestLong && (free.y < out.y || (free.y == out.y && free.x < out.x)))));
                    if (!found || better) {
                        found = true;
                        bestShort = shortSide;
                        bestLong = longSide;
                        out = {free.x, free.y};
                    }
                }
                return found;
            }

            void place(const IntRect& node) {
                for (size_t i = 0; i < freeRects.size();) {
                    if (splitFreeNode(freeRects[i], node)) {
                        freeRects[i] = freeRects.back();
                        freeRects.pop_back();
                    } else {
                        ++i;
                    }
                }
                pruneFreeList();
                usedRight = std::max(usedRight, node.x + node.w);
                usedBottom = std::max(usedBottom, node.y + node.h);
            }

            int right() const { return usedRight; }
            int bottom() const { return usedBottom; }

        private:
            std::vector<IntRect> freeRects;
            std::vector<IntRect> newFreeRects;
            size_t newFreeRectsLastSize = 0;
            int usedRight = 0, usedBottom = 0;

            bool splitFreeNode(const IntRect freeNode, const IntRect& usedNode) {
                if (usedNode.x >= freeNode.x + freeNode.w || usedNode.x + usedNode.w <= freeNode.x ||
                    usedNode.y >= freeNode.y + freeNode.h || usedNode.y + usedNode.h <= freeNode.y) {
                    return false;
                }
                // the up to four pieces of one free rectangle never contain each other
                newFreeRectsLastSize = newFreeRects.size();
                if (usedNode.x < freeNode.x + freeNode.w && usedNode.x + usedNode.w > freeNode.x) {
                    if (usedNode.y > freeNode.y && usedNode.y < freeNode.y + freeNode.h) {
                        IntRect piece = freeNode;
                        piece.h = usedNode.y - piece.y;
                        insertNewFreeRect(piece);
                    }
                    if (usedNode.y + usedNode.h < freeNode.y + freeNode.h) {
                        IntRect piece = freeNode;
                        piece.y = usedNode.y + usedNode.h;
                        piece.h = freeNode.y + freeNode.h - (usedNode.y + usedNode.h);
                        insertNewFreeRect(piece);
                    }
                }
                if (usedNode.y < freeNode.y + freeNode.h && usedNode.y + usedNode.h > freeNode.y) {
                    if (usedNode.x > freeNode.x && usedNode.x < freeNode.x + freeNode.w) {
                        IntRect piece = freeNode;
                        piece.w = usedNode.x - piece.x;
                        insertNewFreeRect(piece);
                    }
                    if (usedNode.x + usedNode.w < freeNode.x + freeNode.w) {
                        IntRect piece = freeNode;
                        piece.x = usedNode.x + usedNode.w;
                        piece.w = freeNode.x + freeNode.w - (usedNode.x + usedNode.w);
                        insertNewFreeRect(piece);
                    }
                }
                return true;
            }

            void insertNewFreeRect(const IntRect& rect) {
                for (size_t i = 0; i < newFreeRectsLastSize;) {
                    if (isContainedIn(rect, newFreeRects[i])) return;
                    if (isContainedIn(newFreeRects[i], rect)) {
                        // keeps the pieces of the free rectangle being split after the older ones
                        newFreeRects[i] = newFreeRects[--newFreeRectsLastSize];
                        newFreeRects[newFreeRectsLastSize] = newFreeRects.back();
                        newFreeRects.pop_back();
                    } else {
                        ++i;
                    }
                }
                newFreeRects.push_back(rect);
            }

            void pruneFreeList() {
                // the old free rectangles can never be inside a new one (the new ones only shrink)
                for (const IntRect& old : freeRects) {
                    for (size_t j = 0; j < newFreeRects.size();) {
                        if (isContainedIn(newFreeRects[j], old)) {
                            newFreeRects[j] = newFreeRects.back();
                            newFreeRects.pop_back();
                        } else {
                            ++j;
                        }
                    }
                }
                freeRects.insert(freeRects.end(), newFreeRects.begin(), newFreeRects.end());
                newFreeRects.clear();
            }
        };

        // The page limit the rounding can never push past, and where the free area starts
        struct Geometry {
            int side = 0;
            int origin = 0;
        };

        Geometry geometry(const PackSettings& settings) {
            Geometry g;
            g.side = settings.maxSize;
            if (settings.blockAlign) g.side &= ~3;
            if (settings.powerOfTwo) g.side = floorPow2(g.side);
            g.origin = settings.blockAlign ? roundUp4(settings.padding) : settings.padding;
            return g;
        }

        bool validate(const PackSettings& settings, PixelProblem& problem) {
            const auto fail = [&](const char* field, std::string message, const char* fix) {
                problem = {"invalid_settings", field, std::move(message), fix};
                return false;
            };
            if (settings.padding < 0 || settings.padding > PixelImage::kMaxSide) {
                return fail("texture.padding", "padding " + std::to_string(settings.padding) + " is out of [0, " +
                            std::to_string(PixelImage::kMaxSide) + "]", "use a padding of 0 or more pixels");
            }
            if (settings.extrude < 0 || settings.extrude > PixelImage::kMaxSide) {
                return fail("texture.extrude", "extrude " + std::to_string(settings.extrude) + " is out of [0, " +
                            std::to_string(PixelImage::kMaxSide) + "]", "use an extrusion of 0 or more pixels");
            }
            if (settings.maxSize < 1 || settings.maxSize > PixelImage::kMaxSide) {
                return fail("texture.maxSize", "max size " + std::to_string(settings.maxSize) + " is out of [1, " +
                            std::to_string(PixelImage::kMaxSide) + "]", "use a max size such as 2048");
            }
            return true;
        }

        struct Group {
            int member = 0;
            const std::string* path = nullptr;
            int largestSide = 0;
            int64_t largestArea = 0;
            std::vector<int> items;
        };

        int maxSide(const IntSize& size) { return std::max(size.w, size.h); }
        int64_t area(const IntSize& size) { return static_cast<int64_t>(size.w) * size.h; }

        // Repeats the edge pixels of rect outward by `extrude` (the cell holds them)
        void writeExtrusion(PixelImage& page, std::span<uint8_t> pixels, const IntRect& rect, int extrude) {
            const int pageW = page.width(), pageH = page.height();
            for (int y = rect.y - extrude; y < rect.y + rect.h + extrude; y++) {
                if (y < 0 || y >= pageH) continue;
                const int sy = std::clamp(y, rect.y, rect.y + rect.h - 1);
                const bool frameRow = y == sy;
                for (int x = rect.x - extrude; x < rect.x + rect.w + extrude; x++) {
                    if (frameRow && x == rect.x) {
                        x = rect.x + rect.w - 1;   // the frame's own pixels stay
                        continue;
                    }
                    if (x < 0 || x >= pageW) continue;
                    const int sx = std::clamp(x, rect.x, rect.x + rect.w - 1);
                    std::memcpy(&pixels[(static_cast<size_t>(y) * pageW + x) * 4],
                                &pixels[(static_cast<size_t>(sy) * pageW + sx) * 4], 4);
                }
            }
        }

        int64_t ceilSqrt(int64_t value) {
            int64_t root = static_cast<int64_t>(std::sqrt(static_cast<double>(value)));
            while (root * root < value) root++;
            while (root > 0 && (root - 1) * (root - 1) >= value) root--;
            return root;
        }

        // The bins a page may shrink into, smallest area first (then max side, then width): powers
        // of two from the one past the frames' area up to the page limit (square and 2:1 each way),
        // and squares from the area's side in steps of 1/32 of it. The page limit itself is left out:
        // it is the page as first laid out.
        std::vector<IntSize> candidateBins(const std::vector<IntSize>& pageCells, const Geometry& g, bool blockAlign) {
            int64_t occupied = 0;
            int widest = 0, tallest = 0;
            for (const IntSize& cell : pageCells) {
                occupied += area(cell);
                widest = std::max(widest, cell.w);
                tallest = std::max(tallest, cell.h);
            }
            const int64_t start = ceilSqrt(occupied) + g.origin;
            std::vector<IntSize> out;
            const auto add = [&](int64_t w, int64_t h) {
                if (w > g.side || h > g.side || (w == g.side && h == g.side)) return;
                if (w < g.origin + widest || h < g.origin + tallest) return;
                if ((w - g.origin) * (h - g.origin) < occupied) return;
                if (blockAlign && (w % 4 != 0 || h % 4 != 0)) return;
                out.push_back({static_cast<int>(w), static_cast<int>(h)});
            };
            int64_t pow2 = 1;
            while (pow2 < std::max<int64_t>(start, std::min(widest, tallest) + g.origin)) pow2 <<= 1;
            for (; pow2 <= g.side; pow2 <<= 1) {
                add(pow2, pow2);
                add(pow2, pow2 / 2);
                add(pow2 / 2, pow2);
            }
            const int64_t base = (start + 3) / 4 * 4;
            const int64_t step = std::max<int64_t>(4, ((base + 31) / 32 + 3) / 4 * 4);
            for (int k = 0; k <= 24; k++) add(base + k * step, base + k * step);
            std::sort(out.begin(), out.end(), [](const IntSize& a, const IntSize& b) {
                if (area(a) != area(b)) return area(a) < area(b);
                if (maxSide(a) != maxSide(b)) return maxSide(a) < maxSide(b);
                if (a.w != b.w) return a.w < b.w;
                return a.h < b.h;
            });
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

    }

    bool layout(const std::vector<LayoutInput>& items, const PackSettings& settings, std::vector<int>& pageOut,
                std::vector<IntPoint>& positionOut, std::vector<IntSize>& pageSizes, PixelProblem* error) {
        const size_t count = items.size();
        pageOut.assign(count, 0);
        positionOut.assign(count, IntPoint{});
        pageSizes.clear();
        const auto failWith = [&](PixelProblem problem) {
            pageOut.clear();
            positionOut.clear();
            if (error) *error = std::move(problem);
            return false;
        };

        PixelProblem problem;
        if (!validate(settings, problem)) return failWith(std::move(problem));
        const Geometry g = geometry(settings);
        const int extrude = settings.extrude;

        std::vector<IntSize> cells(count);
        std::map<int, size_t> groupIndex;
        std::vector<Group> groups;
        for (size_t i = 0; i < count; i++) {
            const LayoutInput& item = items[i];
            if (item.size.w <= 0 || item.size.h <= 0) continue;
            const int64_t grow = 2 * static_cast<int64_t>(extrude) + settings.padding;
            int64_t cellW = item.size.w + grow, cellH = item.size.h + grow;
            if (settings.blockAlign) {
                cellW = (cellW + 3) & ~int64_t(3);
                cellH = (cellH + 3) & ~int64_t(3);
            }
            if (g.origin + cellW > g.side || g.origin + cellH > g.side) {
                return failWith({"too_large", "texture.maxSize",
                                 "frame '" + item.name + "' of '" + item.memberPath + "' is " + std::to_string(item.size.w) +
                                     "x" + std::to_string(item.size.h) + " and needs " + std::to_string(g.origin + cellW) + "x" +
                                     std::to_string(g.origin + cellH) + " with padding and extrusion, more than the " +
                                     std::to_string(g.side) + " pixel page limit",
                                 "raise Max Size or split the frame"});
            }
            cells[i] = {static_cast<int>(cellW), static_cast<int>(cellH)};

            auto [it, added] = groupIndex.emplace(item.member, groups.size());
            if (added) {
                groups.push_back({});
                groups.back().member = item.member;
                groups.back().path = &item.memberPath;
            }
            Group& group = groups[it->second];
            const int side = maxSide(item.size);
            const int64_t itemArea = area(item.size);
            if (side > group.largestSide || (side == group.largestSide && itemArea > group.largestArea)) {
                group.largestSide = side;
                group.largestArea = itemArea;
            }
            group.items.push_back(static_cast<int>(i));
        }

        // every key fully ordered, so std::sort is as deterministic as a stable sort
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
            if (a.largestSide != b.largestSide) return a.largestSide > b.largestSide;
            if (a.largestArea != b.largestArea) return a.largestArea > b.largestArea;
            if (*a.path != *b.path) return *a.path > *b.path;
            return a.member < b.member;
        });
        for (Group& group : groups) {
            std::sort(group.items.begin(), group.items.end(), [&](int a, int b) {
                const LayoutInput& ia = items[a];
                const LayoutInput& ib = items[b];
                if (maxSide(ia.size) != maxSide(ib.size)) return maxSide(ia.size) > maxSide(ib.size);
                if (area(ia.size) != area(ib.size)) return area(ia.size) > area(ib.size);
                if (ia.name != ib.name) return ia.name > ib.name;
                return a < b;
            });
        }

        std::vector<MaxRectsBin> bins;
        const auto placeOn = [&](size_t page, int item) {
            IntPoint at;
            const IntSize cell = cells[item];
            if (!bins[page].find(cell.w, cell.h, at)) return false;
            bins[page].place({at.x, at.y, cell.w, cell.h});
            pageOut[item] = static_cast<int>(page);
            positionOut[item] = {at.x + extrude, at.y + extrude};
            return true;
        };
        // a fresh page always holds one frame: the size check above
        const auto flow = [&](int item, size_t firstPage) {
            for (size_t page = firstPage; page < bins.size(); page++) {
                if (placeOn(page, item)) return;
            }
            bins.emplace_back(g.origin, g.side, g.side);
            placeOn(bins.size() - 1, item);
        };
        const auto placeWhole = [&](size_t page, const Group& group) {
            const MaxRectsBin saved = bins[page];
            for (int item : group.items) {
                if (!placeOn(page, item)) {
                    bins[page] = saved;
                    return false;
                }
            }
            return true;
        };

        for (const Group& group : groups) {
            if (!settings.keepMembersTogether) {
                for (int item : group.items) flow(item, 0);
                continue;
            }
            bool placed = false;
            for (size_t page = 0; page < bins.size() && !placed; page++) placed = placeWhole(page, group);
            if (placed) continue;
            bins.emplace_back(g.origin, g.side, g.side);
            if (placeWhole(bins.size() - 1, group)) continue;
            // larger than one page: it spreads over consecutive new pages, still apart from the others
            const size_t first = bins.size() - 1;
            for (int item : group.items) flow(item, first);
        }

        // A page laid out in the limit's bin runs along its top edge and leaves a long strip when
        // its frames need far less, so each page is laid out again, with the same frames in the same
        // order, in the smallest candidate bin that holds all of them; none holding them keeps the
        // first layout. Which frames go on which page does not change.
        std::vector<std::vector<int>> pageItems(bins.size());
        for (const Group& group : groups) {
            for (int item : group.items) pageItems[pageOut[item]].push_back(item);
        }
        std::vector<IntSize> pageCells;
        std::vector<IntPoint> tight;
        for (size_t page = 0; page < bins.size(); page++) {
            pageCells.clear();
            for (int item : pageItems[page]) pageCells.push_back(cells[item]);
            for (const IntSize& candidate : candidateBins(pageCells, g, settings.blockAlign)) {
                MaxRectsBin bin(g.origin, candidate.w, candidate.h);
                tight.clear();
                for (const IntSize& cell : pageCells) {
                    IntPoint at;
                    if (!bin.find(cell.w, cell.h, at)) break;
                    bin.place({at.x, at.y, cell.w, cell.h});
                    tight.push_back(at);
                }
                if (tight.size() != pageCells.size()) continue;
                bins[page] = std::move(bin);
                for (size_t k = 0; k < tight.size(); k++) positionOut[pageItems[page][k]] = {tight[k].x + extrude, tight[k].y + extrude};
                break;
            }
        }

        pageSizes.reserve(bins.size());
        for (const MaxRectsBin& bin : bins) {
            int w = bin.right(), h = bin.bottom();
            if (settings.blockAlign) {
                w = roundUp4(w);
                h = roundUp4(h);
            }
            if (settings.powerOfTwo) {
                w = roundUpPow2(w);
                h = roundUpPow2(h);
            }
            if (settings.square) w = h = std::max(w, h);
            pageSizes.push_back({w, h});
        }
        return true;
    }

    PackResult pack(const std::vector<PackMember>& members, const PackSettings& settings) {
        PackResult result;

        // packing order: members by path (stable for equal paths); the result stays in sheet order
        std::vector<size_t> order(members.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return members[a].path < members[b].path; });
        std::vector<size_t> firstFlat(members.size());
        size_t total = 0;
        for (size_t m = 0; m < members.size(); m++) {
            firstFlat[m] = total;
            total += members[m].frames.size();
        }
        result.layout.frames.resize(total);

        std::vector<LayoutInput> items;
        std::vector<PixelImage> itemImages;
        std::vector<int> itemFlat;
        std::vector<int> flatItem(total, -1);
        std::map<uint64_t, std::vector<int>> byHash;
        for (size_t c = 0; c < order.size(); c++) {
            const PackMember& member = members[order[c]];
            for (size_t f = 0; f < member.frames.size(); f++) {
                const PackInputFrame& frame = member.frames[f];
                const int flat = static_cast<int>(firstFlat[order[c]] + f);
                PackedFrame& out = result.layout.frames[flat];
                if (!frame.image) continue;
                const PixelImage& source = *frame.image;
                out.sourceSize = {source.width(), source.height()};
                if (source.empty()) continue;
                const IntRect keep = settings.trim && !frame.noTrim ? source.opaqueBounds(0) : source.bounds();
                if (keep.empty()) continue;
                out.offset = {keep.x, keep.y};

                PixelImage trimmed = keep == source.bounds() ? source : source.crop(keep);
                // duplicates compare by RGBA, whatever index plane a frame came with
                trimmed.dropIndices();
                std::vector<int>& sameHash = byHash[trimmed.hash()];
                int match = -1;
                for (int candidate : sameHash) {
                    const PixelImage& other = itemImages[candidate];
                    if (other.width() == trimmed.width() && other.height() == trimmed.height() &&
                        std::equal(other.pixels().begin(), other.pixels().end(), trimmed.pixels().begin())) {
                        match = candidate;
                        break;
                    }
                }
                if (match >= 0) {
                    out.duplicateOf = itemFlat[match];
                    flatItem[flat] = match;
                    continue;
                }
                const int item = static_cast<int>(items.size());
                items.push_back({static_cast<int>(c), member.path, frame.name, {trimmed.width(), trimmed.height()}});
                itemImages.push_back(std::move(trimmed));
                itemFlat.push_back(flat);
                sameHash.push_back(item);
                flatItem[flat] = item;
            }
        }

        std::vector<int> pageOf;
        std::vector<IntPoint> positions;
        if (!layout(items, settings, pageOf, positions, result.layout.pages, &result.error)) {
            result.layout = {};
            return result;
        }

        for (size_t flat = 0; flat < total; flat++) {
            const int item = flatItem[flat];
            if (item < 0) continue;
            PackedFrame& out = result.layout.frames[flat];
            out.page = pageOf[item];
            out.rect = {positions[item].x, positions[item].y, items[item].size.w, items[item].size.h};
        }

        result.pages.reserve(result.layout.pages.size());
        for (const IntSize& size : result.layout.pages) result.pages.emplace_back(size.w, size.h);
        for (size_t item = 0; item < items.size(); item++) {
            result.pages[pageOf[item]].blit(itemImages[item], positions[item].x, positions[item].y);
        }
        // after every frame is down, so extrusion only ever reads frame pixels
        if (settings.extrude > 0) {
            std::vector<std::span<uint8_t>> pagePixels;
            pagePixels.reserve(result.pages.size());
            for (PixelImage& page : result.pages) pagePixels.push_back(page.editPixels());
            for (size_t item = 0; item < items.size(); item++) {
                const IntRect rect{positions[item].x, positions[item].y, items[item].size.w, items[item].size.h};
                writeExtrusion(result.pages[pageOf[item]], pagePixels[pageOf[item]], rect, settings.extrude);
            }
        }

        int64_t usedArea = 0, pageArea = 0;
        for (const LayoutInput& item : items) usedArea += area(item.size);
        for (const IntSize& size : result.layout.pages) pageArea += area(size);
        result.usage = pageArea > 0 ? static_cast<double>(usedArea) / static_cast<double>(pageArea) : 0.0;
        return result;
    }

}
