// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// drawImageToFrame ports the image draw of Pachislop src/engine/layer2d/Title2DRenderer.ts
// (drawImage: the picture over its bounds at the frame's resolution) by SafEight, used with the
// author's permission.

#include "TitleContent.h"

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::mutex& factoryMutex() {
        static std::mutex m;
        return m;
    }

    TitleContentFactory& factory() {
        static TitleContentFactory f;
        return f;
    }

}

void titleart::setDefaultTitleContentProvider(TitleContentFactory f) {
    std::lock_guard<std::mutex> lock(factoryMutex());
    factory() = std::move(f);
}

std::shared_ptr<TitleContentProvider> titleart::makeDefaultTitleContentProvider() {
    std::lock_guard<std::mutex> lock(factoryMutex());
    return factory() ? factory()() : nullptr;
}

void titleart::drawImageToFrame(const TitleImageData& image, const TitleRect& bounds, const geometry::MaskFrame& frame, TitleImageData& out) {
    out.width = frame.width;
    out.height = frame.height;
    out.rgba.assign((size_t)frame.width * frame.height * 4, 0);
    if (image.width <= 0 || image.height <= 0 || bounds.empty() || frame.scale <= 0.0) return;
    // a frame pixel's centre in layer space (rows top first from the frame's top-left corner)
    for (int y = 0; y < frame.height; y++) {
        const double ly = frame.origin.y - (y + 0.5) / frame.scale;
        const double v = (bounds.y + bounds.height - ly) / bounds.height * image.height - 0.5;
        if (v < -0.5 || v > image.height - 0.5) continue;
        for (int x = 0; x < frame.width; x++) {
            const double lx = frame.origin.x + (x + 0.5) / frame.scale;
            const double u = (lx - bounds.x) / bounds.width * image.width - 0.5;
            if (u < -0.5 || u > image.width - 0.5) continue;
            const int x0 = std::clamp((int)std::floor(u), 0, image.width - 1), y0 = std::clamp((int)std::floor(v), 0, image.height - 1);
            const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
            const double fx = std::clamp(u - x0, 0.0, 1.0), fy = std::clamp(v - y0, 0.0, 1.0);
            // premultiplied while filtering, so transparent texels add no colour; straight again after
            double acc[4] = {0, 0, 0, 0};
            const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
            const double wx[2] = {1.0 - fx, fx}, wy[2] = {1.0 - fy, fy};
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    const uint8_t* t = &image.rgba[((size_t)ys[j] * image.width + xs[i]) * 4];
                    const double w = wx[i] * wy[j];
                    const double a = t[3] / 255.0;
                    acc[0] += t[0] * a * w;
                    acc[1] += t[1] * a * w;
                    acc[2] += t[2] * a * w;
                    acc[3] += a * w;
                }
            }
            uint8_t* o = &out.rgba[((size_t)y * frame.width + x) * 4];
            if (acc[3] > 0.0) {
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)std::lround(std::clamp(acc[c] / acc[3], 0.0, 255.0));
            }
            o[3] = (uint8_t)std::lround(std::clamp(acc[3] * 255.0, 0.0, 255.0));
        }
    }
}
