// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PaletteUsage.h"

#include "pixelart/PixelColor.h"

#include <algorithm>

namespace jasperengine::editor::pixelart {
namespace {

    namespace pxu = jasperengine::pixelart;

    // murmur3's finaliser: every key bit reaches the low bits the tables mask with (a plain
    // multiplicative hash leaves them to the blue channel alone)
    uint32_t usageMix32(uint32_t h) {
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
        return h;
    }

    size_t usageTableSize(size_t entries) {
        size_t size = 16;
        while (size < entries * 2) size *= 2;
        return size;
    }

    void usageFnvBytes(uint64_t& hash, const uint8_t* bytes, size_t count) {
        for (size_t i = 0; i < count; i++) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
    }

    void usageFnvInt32(uint64_t& hash, int32_t value) {
        const uint32_t v = (uint32_t)value;
        const uint8_t bytes[4] = {(uint8_t)(v & 255u), (uint8_t)((v >> 8) & 255u), (uint8_t)((v >> 16) & 255u), (uint8_t)(v >> 24)};
        usageFnvBytes(hash, bytes, 4);
    }

}

    ExactColorIndex::ExactColorIndex(std::span<const pxu::Rgba8> colors, bool skipTransparent) {
        bool anyVisible = false;
        for (const pxu::Rgba8& color : colors) anyVisible = anyVisible || color.a != 0;
        const bool skip = skipTransparent && anyVisible;
        slots.assign(usageTableSize(colors.size()), Slot{});
        const size_t mask = slots.size() - 1;
        for (size_t i = 0; i < colors.size(); i++) {
            if (skip && colors[i].a == 0) continue;
            const uint32_t key = pxu::packRgb(colors[i]) + 1u;
            size_t at = usageMix32(key) & mask;
            while (slots[at].key != 0 && slots[at].key != key) at = (at + 1) & mask;
            // a duplicate keeps its first index
            if (slots[at].key == 0) slots[at] = Slot{key, (int)i};
        }
    }

    int ExactColorIndex::find(pxu::Rgba8 color) const {
        const uint32_t key = pxu::packRgb(color) + 1u;
        const size_t mask = slots.size() - 1;
        size_t at = usageMix32(key) & mask;
        while (slots[at].key != 0) {
            if (slots[at].key == key) return slots[at].index;
            at = (at + 1) & mask;
        }
        return -1;
    }

    void ColorTally::grow() {
        std::vector<Slot> old = std::move(slots);
        slots.assign(old.empty() ? 1024 : old.size() * 2, Slot{});
        const size_t mask = slots.size() - 1;
        for (const Slot& slot : old) {
            if (slot.key == 0) continue;
            size_t at = usageMix32((uint32_t)(slot.key - 1u)) & mask;
            while (slots[at].key != 0) at = (at + 1) & mask;
            slots[at] = slot;
        }
    }

    void ColorTally::add(uint32_t key, int64_t count) {
        if ((used + 1) * 2 > slots.size()) grow();
        const uint64_t stored = (uint64_t)key + 1u;
        const size_t mask = slots.size() - 1;
        size_t at = usageMix32(key) & mask;
        while (slots[at].key != 0 && slots[at].key != stored) at = (at + 1) & mask;
        if (slots[at].key == 0) {
            slots[at].key = stored;
            used++;
        }
        slots[at].count += count;
    }

    std::vector<std::pair<uint32_t, int64_t>> ColorTally::entries() const {
        std::vector<std::pair<uint32_t, int64_t>> list;
        list.reserve(used);
        for (const Slot& slot : slots) {
            if (slot.key != 0) list.emplace_back((uint32_t)(slot.key - 1u), slot.count);
        }
        std::sort(list.begin(), list.end());
        return list;
    }

    PaletteUsageReport measureUsage(const RgbaImage& image, std::span<const pxu::Rgba8> palette,
                                    int alphaThreshold, int maxOffColors) {
        PaletteUsageReport report;
        report.counts.assign(palette.size(), 0);
        report.contentHash = imageContentHash(image);
        if (!image.valid()) return report;

        const ExactColorIndex exact(palette, true);
        ColorTally off;
        const int threshold = std::max(alphaThreshold, 0);
        // neighbouring pixels mostly repeat a colour: the last answer saves the lookup
        uint32_t lastRgb = 0xffffffffu;
        int lastIndex = -1;
        for (const pxu::Rgba8& pixel : image.pixels) {
            if (pixel.a < threshold) continue;
            const uint32_t rgb = pxu::packRgb(pixel);
            if (rgb != lastRgb) {
                lastRgb = rgb;
                lastIndex = exact.find(pixel);
            }
            if (lastIndex >= 0) {
                report.counts[lastIndex]++;
            } else {
                off.add(rgb);
                report.offPalettePixels++;
            }
        }

        std::vector<std::pair<uint32_t, int64_t>> colors = off.entries();
        report.offColorCount = (int64_t)colors.size();
        // sorted by RGB already, so equal counts stay in RGB order
        std::stable_sort(colors.begin(), colors.end(), [](const std::pair<uint32_t, int64_t>& x, const std::pair<uint32_t, int64_t>& y) {
            return x.second > y.second;
        });
        if (maxOffColors >= 0 && colors.size() > (size_t)maxOffColors) colors.resize((size_t)maxOffColors);

        const pxu::PaletteMatcher matcher(palette, pxu::ColorMetric::OkLab);
        report.offColors.reserve(colors.size());
        for (const std::pair<uint32_t, int64_t>& entry : colors) {
            PaletteUsageReport::OffColor offColor;
            offColor.color = pxu::unpackRgb(entry.first);
            offColor.count = entry.second;
            const std::pair<int, float> nearest = matcher.nearestUncached(offColor.color);
            offColor.nearest = nearest.first;
            offColor.deltaE = nearest.first >= 0 ? nearest.second * 100.0f : 0.0f;
            report.offColors.push_back(offColor);
        }
        return report;
    }

    uint64_t imageContentHash(const RgbaImage& image) {
        uint64_t hash = 0xcbf29ce484222325ull;
        usageFnvInt32(hash, image.width);
        usageFnvInt32(hash, image.height);
        for (const pxu::Rgba8& pixel : image.pixels) {
            const uint8_t bytes[4] = {pixel.r, pixel.g, pixel.b, pixel.a};
            usageFnvBytes(hash, bytes, 4);
        }
        return hash;
    }

}
