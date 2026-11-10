// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/materials.ts by SafEight, used with the author's permission.

#include "TitlePicture3D.h"

#include "titleart/render/TitleContent.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr int FACE_TEX_MAX = 4096;
    constexpr int EDGE_TEX_MAX = 1024;

    // a box downscale of straight RGBA by an integer factor (colour weighted by alpha)
    std::vector<uint8_t> downscale(const std::vector<uint8_t>& rgba, int w, int h, int f, int& ow, int& oh) {
        ow = std::max(1, w / f);
        oh = std::max(1, h / f);
        std::vector<uint8_t> out((size_t)ow * oh * 4, 0);
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                double c[4] = {0, 0, 0, 0};
                int n = 0;
                for (int dy = 0; dy < f; dy++) {
                    for (int dx = 0; dx < f; dx++) {
                        const int sx = std::min(w - 1, x * f + dx), sy = std::min(h - 1, y * f + dy);
                        const uint8_t* p = &rgba[((size_t)sy * w + sx) * 4];
                        const double a = p[3] / 255.0;
                        for (int k = 0; k < 3; k++) c[k] += p[k] * a;
                        c[3] += p[3];
                        n++;
                    }
                }
                uint8_t* o = &out[((size_t)y * ow + x) * 4];
                const double a = c[3] / n;
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)std::lround(a > 0 ? std::min(255.0, c[k] / (c[3] / 255.0)) : 0.0);
                o[3] = (uint8_t)std::lround(a);
            }
        }
        return out;
    }

}

std::vector<uint8_t> titleart::nearestInsideColours(const std::vector<uint8_t>& px, int w, int h, int threshold) {
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    // per column: the row of the nearest site in that column (-1 = none)
    std::vector<int> near((size_t)w * h, -1);
    for (int x = 0; x < w; x++) {
        int last = -1;
        for (int y = 0; y < h; y++) {
            if (px[((size_t)y * w + x) * 4 + 3] >= threshold) last = y;
            near[(size_t)y * w + x] = last;
        }
        last = -1;
        for (int y = h - 1; y >= 0; y--) {
            const size_t i = (size_t)y * w + x;
            if (px[i * 4 + 3] >= threshold) last = y;
            if (last >= 0 && (near[i] < 0 || last - y < y - near[i])) near[i] = last;
        }
    }
    // per row: the lower envelope of the parabolas (x - q)^2 + dy(q)^2 over the columns with a site
    std::vector<double> f((size_t)w), z((size_t)w + 1);
    std::vector<int> v((size_t)w);
    const double inf = std::numeric_limits<double>::infinity();
    for (int y = 0; y < h; y++) {
        const size_t row = (size_t)y * w;
        int k = -1;
        for (int q = 0; q < w; q++) {
            const int ny = near[row + q];
            if (ny < 0) continue;
            f[(size_t)q] = (double)(y - ny) * (y - ny) + (double)q * q;
            double s = -inf;
            while (k >= 0) {
                const int p = v[(size_t)k];
                s = (f[(size_t)q] - f[(size_t)p]) / (2.0 * (q - p));
                if (s > z[(size_t)k]) break;
                k--;
            }
            k++;
            v[(size_t)k] = q;
            z[(size_t)k] = k == 0 ? -inf : s;
            z[(size_t)k + 1] = inf;
        }
        if (k < 0) {
            for (int x = 0; x < w; x++) out[(row + x) * 4 + 3] = 255;
            continue;
        }
        for (int x = 0, j = 0; x < w; x++) {
            while (z[(size_t)j + 1] < x) j++;
            const int q = v[(size_t)j];
            const size_t src = ((size_t)near[row + q] * w + q) * 4;
            const size_t dst = (row + x) * 4;
            out[dst] = px[src];
            out[dst + 1] = px[src + 1];
            out[dst + 2] = px[src + 2];
            out[dst + 3] = 255;
        }
    }
    return out;
}

std::shared_ptr<TitlePicture3D> titleart::makeTitlePicture3D(const Json& layer, const TitleArtDoc& doc, TitleContentProvider& content,
                                                             const AssetResolver* assets, double pxPerUnit) {
    const geometry::LayerElements els = content.elements(layer, doc, assets, TitleElementFilter{});
    const TitleRect b = els.baseBounds;
    if (b.empty()) return nullptr;
    // a margin of 2 %, so the clamped edge never shows the picture's last row
    const double mx = b.width * 0.02, my = b.height * 0.02;
    const double x0 = b.x - mx, x1 = b.x + b.width + mx, y0 = b.y - my, y1 = b.y + b.height + my;
    double m = std::max(1e-3, pxPerUnit);
    m = std::min(m, FACE_TEX_MAX / std::max(x1 - x0, y1 - y0));
    geometry::MaskFrame frame;
    frame.width = std::max(1, (int)std::ceil((x1 - x0) * m));
    frame.height = std::max(1, (int)std::ceil((y1 - y0) * m));
    frame.scale = m;
    frame.origin = {x0, y1};
    TitleImageData image;
    if (!content.picture(layer, doc, assets, frame, image) || image.width != frame.width || image.height != frame.height) return nullptr;
    auto out = std::make_shared<TitlePicture3D>();
    out->width = image.width;
    out->height = image.height;
    out->x0 = x0;
    out->x1 = x0 + frame.width / m;
    out->y1 = y1;
    out->y0 = y1 - frame.height / m;
    // the face: premultiplied
    out->face = image.rgba;
    for (size_t i = 0; i + 3 < out->face.size(); i += 4) {
        const int a = out->face[i + 3];
        for (int c = 0; c < 3; c++) out->face[i + c] = (uint8_t)((out->face[i + c] * a + 127) / 255);
    }
    // the edge: the nearest inside colours at <= 1024 px
    const Json& trace = layer.contains("trace") && layer["trace"].is_object() ? layer["trace"] : Json::object();
    const int threshold = std::clamp(trace.value("threshold", 128), 1, 255);
    out->edgeAlpha = threshold / 255.0;
    const int factor = std::max(1, (int)std::ceil(std::max(image.width, image.height) / (double)EDGE_TEX_MAX));
    std::vector<uint8_t> small = factor > 1 ? downscale(image.rgba, image.width, image.height, factor, out->edgeWidth, out->edgeHeight) : image.rgba;
    if (factor == 1) {
        out->edgeWidth = image.width;
        out->edgeHeight = image.height;
    }
    out->edge = nearestInsideColours(small, out->edgeWidth, out->edgeHeight, threshold);
    return out;
}
