// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImageDiff.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    void channelsOf(const texsynth::Image& image, int x, int y, bool premultiplied, int out[4]) {
        const uint8_t* p = image.pixel(x, y);
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = p[3];
        if (premultiplied) {
            for (int k = 0; k < 3; k++) out[k] = static_cast<int>(std::lround(out[k] * out[3] / 255.0));
        }
    }

    ImageDiffRect region(const texsynth::Image& image, const ImageDiffOptions& options) {
        ImageDiffRect r{0, 0, image.width, image.height};
        if (!options.crop) return r;
        const int x0 = std::clamp(options.crop->x, 0, image.width), y0 = std::clamp(options.crop->y, 0, image.height);
        const int x1 = std::clamp(options.crop->x + options.crop->width, 0, image.width);
        const int y1 = std::clamp(options.crop->y + options.crop->height, 0, image.height);
        return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    }

}

texsynth::Image editor::toRgba8Image(const texsynth::Image& image) {
    if (image.channels == 4 && image.bitDepth == 8) return image;
    return texsynth::toRgba8(image);
}

ImageDiffResult editor::diffImages(const texsynth::Image& a8, const texsynth::Image& b8, const ImageDiffOptions& options) {
    const texsynth::Image a = toRgba8Image(a8), b = toRgba8Image(b8);
    ImageDiffResult result;
    result.widthA = a.width;
    result.heightA = a.height;
    result.widthB = b.width;
    result.heightB = b.height;
    result.sameSize = a.width == b.width && a.height == b.height;
    if (!result.sameSize) {
        result.error = "the images differ in size: " + std::to_string(a.width) + " x " + std::to_string(a.height) + " and " +
                       std::to_string(b.width) + " x " + std::to_string(b.height);
        return result;
    }
    const ImageDiffRect r = region(a, options);
    uint64_t sum = 0, over = 0, changed = 0;
    int minX = r.x + r.width, minY = r.y + r.height, maxX = -1, maxY = -1;
    for (int y = r.y; y < r.y + r.height; y++) {
        for (int x = r.x; x < r.x + r.width; x++) {
            int ca[4], cb[4];
            channelsOf(a, x, y, options.premultiplied, ca);
            channelsOf(b, x, y, options.premultiplied, cb);
            int d = 0;
            for (int k = 0; k < 4; k++) {
                const int dk = std::abs(ca[k] - cb[k]);
                sum += static_cast<uint64_t>(dk);
                d = std::max(d, dk);
            }
            result.max = std::max(result.max, d);
            if (d > 0) changed++;
            if (d > options.tolerance) {
                over++;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    const uint64_t pixels = static_cast<uint64_t>(r.width) * r.height;
    if (pixels > 0) {
        result.mean = static_cast<double>(sum) / (pixels * 4.0);
        result.fractionOver = static_cast<double>(over) / pixels;
        result.changedPct = 100.0 * static_cast<double>(changed) / pixels;
    }
    if (maxX >= 0) result.bbox = {minX, minY, maxX - minX + 1, maxY - minY + 1};
    result.ok = result.max <= options.maxDiff && result.mean <= options.maxMean && result.fractionOver <= options.maxFraction;
    return result;
}

texsynth::Image editor::diffImage(const texsynth::Image& a8, const texsynth::Image& b8, const ImageDiffOptions& options) {
    const texsynth::Image a = toRgba8Image(a8), b = toRgba8Image(b8);
    const int w = std::min(a.width, b.width), h = std::min(a.height, b.height);
    texsynth::Image out = texsynth::Image::make(std::max(w, 1), std::max(h, 1), 4, 8);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int ca[4], cb[4];
            channelsOf(a, x, y, options.premultiplied, ca);
            channelsOf(b, x, y, options.premultiplied, cb);
            int d = 0;
            for (int k = 0; k < 4; k++) d = std::max(d, std::abs(ca[k] - cb[k]));
            // faint luminance of a (over black) so the eye finds the place, the difference x8 in red
            const double luma = (0.2126 * ca[0] + 0.7152 * ca[1] + 0.0722 * ca[2]) * (options.premultiplied ? 1.0 : ca[3] / 255.0);
            const int base = static_cast<int>(luma * 0.25);
            uint8_t* p = out.pixel(x, y);
            p[0] = static_cast<uint8_t>(std::min(255, base + d * 8));
            p[1] = static_cast<uint8_t>(base);
            p[2] = static_cast<uint8_t>(base);
            p[3] = 255;
        }
    }
    return out;
}

texsynth::Image editor::diffPanel(const texsynth::Image& a8, const texsynth::Image& b8, const ImageDiffOptions& options) {
    const texsynth::Image a = toRgba8Image(a8), b = toRgba8Image(b8);
    const texsynth::Image d = diffImage(a, b, options);
    const int gap = 4;
    const int h = std::max({a.height, b.height, d.height});
    const int w = a.width + b.width + d.width + gap * 2;
    texsynth::Image out = texsynth::Image::make(std::max(w, 1), std::max(h, 1), 4, 8);
    int x0 = 0;
    for (const texsynth::Image* panel : {&a, &b, &d}) {
        for (int y = 0; y < panel->height; y++) {
            for (int x = 0; x < panel->width; x++) {
                // the panels over a mid grey checker so real alpha shows
                const uint8_t* s = panel->pixel(x, y);
                const int check = ((x / 8 + y / 8) % 2) ? 96 : 160;
                uint8_t* p = out.pixel(x0 + x, y);
                const int alpha = s[3];
                for (int k = 0; k < 3; k++) p[k] = static_cast<uint8_t>((s[k] * alpha + check * (255 - alpha)) / 255);
                p[3] = 255;
            }
        }
        x0 += panel->width + gap;
    }
    return out;
}
