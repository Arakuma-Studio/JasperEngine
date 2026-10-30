// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsPreview.h"

#include "SpriteNormalsField.h"
#include "SpriteNormalsPaint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::editor::spritenormals {

    namespace {

        constexpr double kPi = 3.14159265358979323846;
        constexpr int kGap = 4;               // between the tiles of a contact sheet
        constexpr int kChecker = 8;           // checkerboard square, sheet pixels

        bool isRgba8(const Image& image) { return !image.empty() && image.channels == 4 && image.bitDepth == 8; }

        // The decoded unit normal of an RGBA8 texel (green flipped back for DirectX; a zero
        // vector counts as +Z)
        void decodeNormal(const uint8_t* p, bool directx, float n[3]) {
            n[0] = p[0] / 255.0f * 2.0f - 1.0f;
            n[1] = p[1] / 255.0f * 2.0f - 1.0f;
            n[2] = p[2] / 255.0f * 2.0f - 1.0f;
            if (directx) n[1] = -n[1];
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length < 1e-6f) {
                n[0] = 0.0f;
                n[1] = 0.0f;
                n[2] = 1.0f;
                return;
            }
            n[0] /= length;
            n[1] /= length;
            n[2] /= length;
        }

        double degrees(double radians) { return radians * 180.0 / kPi; }

        // The pixels of `cell` as their own RGBA8 image
        Image crop(const Image& image, const Cell& cell) {
            Image out = Image::make(cell.width, cell.height, 4, 8);
            for (int y = 0; y < cell.height; y++) {
                const uint8_t* src = image.pixel(cell.x, cell.y + y);
                std::copy(src, src + static_cast<size_t>(cell.width) * 4, out.pixel(0, y));
            }
            return out;
        }

        // Every k-th pixel (nearest): ceil(w / k) x ceil(h / k)
        Image decimate(const Image& image, int k) {
            if (k <= 1) return image;
            const int w = (image.width + k - 1) / k;
            const int h = (image.height + k - 1) / k;
            Image out = Image::make(w, h, 4, 8);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) std::copy(image.pixel(x * k, y * k), image.pixel(x * k, y * k) + 4, out.pixel(x, y));
            }
            return out;
        }

        // `tile` scaled by `s` (nearest) composited "over" the sheet at (x0, y0), clipped
        void blit(Image& sheet, const Image& tile, int x0, int y0, int s) {
            for (int y = 0; y < tile.height * s; y++) {
                const int sy = y0 + y;
                if (sy < 0 || sy >= sheet.height) continue;
                for (int x = 0; x < tile.width * s; x++) {
                    const int sx = x0 + x;
                    if (sx < 0 || sx >= sheet.width) continue;
                    const uint8_t* src = tile.pixel(x / s, y / s);
                    uint8_t* dst = sheet.pixel(sx, sy);
                    const int a = src[3];
                    for (int c = 0; c < 3; c++) dst[c] = static_cast<uint8_t>((src[c] * a + dst[c] * (255 - a) + 127) / 255);
                    dst[3] = 255;
                }
            }
        }

        // The magenta outline of the painted visible pixels of `frame` around its normal tile (at
        // x0, y0; decimated by k, scaled by s): drawn on the sheet pixels next to painted ones that
        // are not painted themselves (the gap around the tile included), so painted pixels stay visible
        void outlinePainted(Image& sheet, const PaintOverlay& paint, const Image& color, const Cell& frame, float alphaThreshold, int k, int s, int x0,
                            int y0) {
            const int w = (frame.width + k - 1) / k, h = (frame.height + k - 1) / k;
            std::vector<uint8_t> painted(static_cast<size_t>(w) * h, 0);
            bool any = false;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const int fx = frame.x + x * k, fy = frame.y + y * k;
                    const size_t i = static_cast<size_t>(fy) * color.width + fx;
                    const bool on = paint.rgba[i * 4 + 3] > 0 && color.data[i * 4 + 3] / 255.0f > alphaThreshold;
                    painted[static_cast<size_t>(y) * w + x] = on;
                    any = any || on;
                }
            }
            if (!any) return;
            auto paintedAt = [&](int tx, int ty) {
                if (tx < 0 || ty < 0 || tx >= w * s || ty >= h * s) return false;
                return painted[static_cast<size_t>(ty / s) * w + tx / s] != 0;
            };
            for (int ty = -1; ty <= h * s; ty++) {
                for (int tx = -1; tx <= w * s; tx++) {
                    const int sx = x0 + tx, sy = y0 + ty;
                    if (sx < 0 || sy < 0 || sx >= sheet.width || sy >= sheet.height || paintedAt(tx, ty)) continue;
                    if (!paintedAt(tx - 1, ty) && !paintedAt(tx + 1, ty) && !paintedAt(tx, ty - 1) && !paintedAt(tx, ty + 1)) continue;
                    uint8_t* p = sheet.pixel(sx, sy);
                    p[0] = 255;
                    p[1] = 0;
                    p[2] = 255;
                    p[3] = 255;
                }
            }
        }

        std::string integer(double v) { return std::to_string(std::lround(v)); }

        // ---- roughness and creases (hires design spec, rule 6)

        // k of the metric: the median over the cells (the image without cells) of the largest
        // island's shorter side, over 128
        double metricScale(const Image& normal, const std::vector<Cell>& frames, float alphaThreshold) {
            std::vector<double> sides;
            for (const Cell& cell : frames) {
                std::vector<uint8_t> mask(static_cast<size_t>(cell.width) * cell.height, 0);
                for (int y = 0; y < cell.height; y++) {
                    for (int x = 0; x < cell.width; x++) {
                        mask[static_cast<size_t>(y) * cell.width + x] = normal.pixel(cell.x + x, cell.y + y)[3] / 255.0f > alphaThreshold;
                    }
                }
                const std::vector<detail::Component> islands = detail::components(mask, cell.width, cell.height);
                if (islands.empty()) continue;
                size_t largest = 0;
                for (size_t k = 1; k < islands.size(); k++) {
                    if (islands[k].pixels.size() > islands[largest].pixels.size()) largest = k;
                }
                const detail::Component& big = islands[largest];
                sides.push_back(std::min(big.maxX - big.minX + 1, big.maxY - big.minY + 1));
            }
            if (sides.empty()) return 1.0;
            std::sort(sides.begin(), sides.end());
            const size_t n = sides.size();
            const double median = n % 2 ? sides[n / 2] : (sides[n / 2 - 1] + sides[n / 2]) * 0.5;
            return std::max(1.0, median / kHiresSide);
        }

        void measureSmoothness(Metrics& m, const Image& normal, const std::vector<float>& normals, const std::vector<Cell>& cells,
                               float alphaThreshold) {
            std::vector<Cell> frames = cells;
            if (frames.empty()) frames.push_back({0, 0, normal.width, normal.height});
            for (Cell& c : frames) {
                // clipped to the image
                const int x0 = std::clamp(c.x, 0, normal.width), y0 = std::clamp(c.y, 0, normal.height);
                c.width = std::clamp(c.x + c.width, 0, normal.width) - x0;
                c.height = std::clamp(c.y + c.height, 0, normal.height) - y0;
                c.x = x0;
                c.y = y0;
            }
            frames.erase(std::remove_if(frames.begin(), frames.end(), [](const Cell& c) { return c.width <= 0 || c.height <= 0; }), frames.end());
            m.scale = metricScale(normal, frames, alphaThreshold);
            // pixel art (128 px or less): not measured (an 8 px inset leaves the apex of a 32 px
            // sprite, and its 1-2 px creases are the pixel-art look)
            if (m.scale <= 1.0) return;
            const float sigma = static_cast<float>(4.0 * m.scale);
            const float inset = static_cast<float>(8.0 * m.scale);
            const double creaseCos = std::cos(kCreaseDeg * kPi / 180.0);
            double sum = 0.0;
            int64_t inner = 0, creased = 0;
            for (const Cell& cell : frames) {
                const int w = cell.width, h = cell.height;
                // the alpha-0.5 silhouette in a field padded by 1 (the cell's border is outside)
                const int pw = w + 2, ph = h + 2;
                std::vector<uint8_t> solid(static_cast<size_t>(pw) * ph, 0);
                bool any = false;
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const bool on = normal.pixel(cell.x + x, cell.y + y)[3] >= 128;
                        solid[static_cast<size_t>(y + 1) * pw + x + 1] = on;
                        any = any || on;
                    }
                }
                if (!any) continue;
                const detail::Field padded = detail::edt(solid, pw, ph, detail::Border::Zero);
                detail::Field depth = detail::Field::zero(w, h);
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) depth.data[static_cast<size_t>(y) * w + x] = padded.data[static_cast<size_t>(y + 1) * pw + x + 1];
                }
                bool anyInner = false;
                for (float d : depth.data) anyInner = anyInner || (std::isfinite(d) && d > inset);
                if (!anyInner) continue;
                // the cell's normals blurred (the border continues: outside the image they are
                // the edge's), each channel alone
                detail::Field channel[3];
                for (int k = 0; k < 3; k++) {
                    detail::Field f = detail::Field::zero(w, h);
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            const size_t i = static_cast<size_t>(cell.y + y) * normal.width + (cell.x + x);
                            const uint8_t* p = normal.data.data() + i * 4;
                            // transparent pixels count as their encoded normal (flat)
                            f.data[static_cast<size_t>(y) * w + x] = p[3] / 255.0f > alphaThreshold ? normals[i * 3 + k] : (k == 2 ? 1.0f : 0.0f);
                        }
                    }
                    channel[k] = detail::gaussian(f, sigma, detail::Border::Clamp);
                }
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const size_t j = static_cast<size_t>(y) * w + x;
                        const float d = depth.data[j];
                        if (!(std::isfinite(d) && d > inset)) continue;
                        const size_t i = static_cast<size_t>(cell.y + y) * normal.width + (cell.x + x);
                        const double b[3] = {channel[0].data[j], channel[1].data[j], channel[2].data[j]};
                        const double length = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                        if (!(length > 1e-9)) continue;
                        const double c = std::clamp((normals[i * 3] * b[0] + normals[i * 3 + 1] * b[1] + normals[i * 3 + 2] * b[2]) / length, -1.0, 1.0);
                        sum += degrees(std::acos(c));
                        inner++;
                        if (c < creaseCos) creased++;
                    }
                }
            }
            m.innerPixels = static_cast<int>(inner);
            if (inner > 0) {
                m.roughnessDeg = sum / static_cast<double>(inner);
                m.creasePct = 100.0 * static_cast<double>(creased) / static_cast<double>(inner);
            }
        }

        std::string oneDecimal(double v) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.1f", std::round(v * 10.0) / 10.0);
            return buffer;
        }

    }

    // ------------------------------------------------------------------ lit render

    LightSetup orbitLight(int width, int height, float angleDegrees, bool directx, float alphaThreshold) {
        const float size = static_cast<float>(std::max(width, height));
        const float angle = static_cast<float>(angleDegrees * kPi / 180.0);
        PreviewLight light;
        // counter-clockwise from the right, 90 = up the image (image y grows downward)
        light.x = width * 0.5f + std::cos(angle) * 0.75f * size;
        light.y = height * 0.5f - std::sin(angle) * 0.75f * size;
        light.height = 0.5f * size;
        light.range = 3.0f * size;
        LightSetup setup;
        setup.lights.push_back(light);
        setup.directx = directx;
        setup.alphaThreshold = alphaThreshold;
        return setup;
    }

    Image renderLit(const Image& normal, const Image& color, const LightSetup& setup) {
        if (!isRgba8(normal) || !isRgba8(color) || normal.width != color.width || normal.height != color.height) return Image();
        Image out = Image::make(color.width, color.height, 4, 8);
        const float ambient[3] = {setup.ambientRed * setup.ambientIntensity, setup.ambientGreen * setup.ambientIntensity,
                                  setup.ambientBlue * setup.ambientIntensity};
        for (int y = 0; y < color.height; y++) {
            for (int x = 0; x < color.width; x++) {
                const uint8_t* c = color.pixel(x, y);
                uint8_t* o = out.pixel(x, y);
                if (c[3] / 255.0f <= setup.alphaThreshold) continue;   // stays (0,0,0,0)
                float n[3];
                decodeNormal(normal.pixel(x, y), setup.directx, n);
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                // getLight2DIntensity: ambient (pre-multiplied by its intensity) + each light
                float light[3] = {ambient[0], ambient[1], ambient[2]};
                for (const PreviewLight& l : setup.lights) {
                    // image y down -> world y up, so a green-up normal faces lights above it
                    const float tx = l.x - px;
                    const float ty = -(l.y - py);
                    const float d = std::sqrt(tx * tx + ty * ty);
                    if (d >= l.range) continue;
                    const float atten = std::pow(std::clamp(1.0f - d / l.range, 0.0f, 1.0f), l.falloff);
                    float ndotl = 1.0f;
                    if (l.height > 0.0f) {
                        const float length = std::sqrt(tx * tx + ty * ty + l.height * l.height);
                        ndotl = std::clamp((n[0] * tx + n[1] * ty + n[2] * l.height) / length, 0.0f, 1.0f);
                    }
                    const float k = l.intensity * atten * ndotl;
                    light[0] += l.red * k;
                    light[1] += l.green * k;
                    light[2] += l.blue * k;
                }
                // mesh.frag's 2D path: linear albedo x light, gamma 2.2 back
                for (int ch = 0; ch < 3; ch++) {
                    const float linear = std::pow(c[ch] / 255.0f, 2.2f) * light[ch];
                    const float srgb = std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f);
                    o[ch] = static_cast<uint8_t>(std::lround(srgb * 255.0f));
                }
                o[3] = c[3];
            }
        }
        return out;
    }

    // ------------------------------------------------------------------ contact sheet

    std::vector<int> sheetFrames(int frameCount, int maxFrames) {
        std::vector<int> frames;
        if (frameCount <= 0) return frames;
        maxFrames = std::max(1, maxFrames);
        if (frameCount <= maxFrames) {
            for (int i = 0; i < frameCount; i++) frames.push_back(i);
            return frames;
        }
        if (maxFrames == 1) return {0};
        for (int i = 0; i < maxFrames; i++) {
            frames.push_back(static_cast<int>(std::lround(static_cast<double>(i) * (frameCount - 1) / (maxFrames - 1))));
        }
        return frames;
    }

    Image contactSheet(const Image& color, const Image& normal, const std::vector<Cell>& cells, bool directx, float alphaThreshold,
                       const SheetOptions& options, const PaintOverlay* paint) {
        if (!isRgba8(color) || !isRgba8(normal) || color.width != normal.width || color.height != normal.height) return Image();
        std::vector<Cell> frames;
        if (cells.empty()) {
            frames.push_back({0, 0, color.width, color.height});
        } else {
            for (int i : sheetFrames(static_cast<int>(cells.size()), options.maxFrames)) {
                const Cell& c = cells[static_cast<size_t>(i)];
                // a cell outside the image is clipped to it (nothing to show when it is empty)
                Cell clipped = c;
                clipped.x = std::clamp(c.x, 0, color.width);
                clipped.y = std::clamp(c.y, 0, color.height);
                clipped.width = std::clamp(c.x + c.width, 0, color.width) - clipped.x;
                clipped.height = std::clamp(c.y + c.height, 0, color.height) - clipped.y;
                if (clipped.width > 0 && clipped.height > 0) frames.push_back(clipped);
            }
        }
        if (frames.empty()) return Image();
        const int rows = static_cast<int>(frames.size());
        int w = 0, h = 0;
        for (const Cell& c : frames) {
            w = std::max(w, c.width);
            h = std::max(h, c.height);
        }
        const int maxWidth = std::max(1, options.maxWidth);
        const int maxHeight = std::max(1, options.maxHeight);
        auto fits = [&](int k) {
            const int tw = (w + k - 1) / k, th = (h + k - 1) / k;
            return 6 * tw + 5 * kGap <= maxWidth && rows * th + (rows - 1) * kGap <= maxHeight;
        };
        // frames too large even at scale 1: every k-th pixel
        int k = 1;
        while (!fits(k) && k < std::max(w, h)) k++;
        const int tw = (w + k - 1) / k, th = (h + k - 1) / k;
        int s = std::min({std::max(1, options.maxScale), (maxWidth - 5 * kGap) / (6 * tw), (maxHeight - (rows - 1) * kGap) / (rows * th)});
        s = std::max(1, s);
        const int sheetWidth = std::min(maxWidth, 6 * tw * s + 5 * kGap);
        const int sheetHeight = std::min(maxHeight, rows * th * s + (rows - 1) * kGap);

        Image sheet = Image::make(sheetWidth, sheetHeight, 4, 8);
        for (int y = 0; y < sheetHeight; y++) {
            for (int x = 0; x < sheetWidth; x++) {
                uint8_t* p = sheet.pixel(x, y);
                const bool odd = ((x / kChecker) + (y / kChecker)) % 2 != 0;
                p[0] = odd ? 0x22 : 0x2a;
                p[1] = odd ? 0x22 : 0x2a;
                p[2] = odd ? 0x27 : 0x30;
                p[3] = 255;
            }
        }
        // colour | normal | lit from the upper left, upper right, lower right, lower left
        const float angles[4] = {135.0f, 45.0f, 315.0f, 225.0f};
        for (int r = 0; r < rows; r++) {
            const Image frameColor = crop(color, frames[static_cast<size_t>(r)]);
            const Image frameNormal = crop(normal, frames[static_cast<size_t>(r)]);
            std::vector<Image> tiles = {frameColor, frameNormal};
            for (float angle : angles) {
                tiles.push_back(renderLit(frameNormal, frameColor, orbitLight(frameColor.width, frameColor.height, angle, directx, alphaThreshold)));
            }
            const int y0 = r * (th * s + kGap);
            for (size_t t = 0; t < tiles.size(); t++) {
                blit(sheet, decimate(tiles[t], k), static_cast<int>(t) * (tw * s + kGap), y0, s);
            }
            if (paint && paint->sized(color.width, color.height)) outlinePainted(sheet, *paint, color, frames[static_cast<size_t>(r)], alphaThreshold, k, s, tw * s + kGap, y0);
        }
        return sheet;
    }

    // ------------------------------------------------------------------ quality

    Metrics measure(const Image& normal, const std::vector<Cell>& cells, bool directx, float alphaThreshold, const PaintOverlay* paint,
                    const Image* shape) {
        Metrics m;
        if (!isRgba8(normal)) return m;
        const int width = normal.width, height = normal.height;
        // which cell each pixel belongs to (-1: none, not measured); the whole image without cells
        std::vector<int> owner(normal.pixelCount(), cells.empty() ? 0 : -1);
        for (size_t c = 0; c < cells.size(); c++) {
            const Cell& cell = cells[c];
            for (int y = std::max(0, cell.y); y < std::min(height, cell.y + cell.height); y++) {
                for (int x = std::max(0, cell.x); x < std::min(width, cell.x + cell.width); x++) {
                    int& o = owner[static_cast<size_t>(y) * width + x];
                    if (o < 0) o = static_cast<int>(c);
                }
            }
        }
        std::vector<float> normals(normal.pixelCount() * 3, 0.0f);
        std::vector<uint8_t> opaque(normal.pixelCount(), 0);
        int64_t considered = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const size_t i = static_cast<size_t>(y) * width + x;
                if (owner[i] < 0) continue;
                considered++;
                const uint8_t* p = normal.pixel(x, y);
                if (p[3] / 255.0f <= alphaThreshold) {
                    m.hasTransparency = true;
                    continue;
                }
                opaque[i] = 1;
                decodeNormal(p, directx, &normals[i * 3]);
            }
        }
        const bool painted = paint && paint->sized(width, height);
        double sum[3] = {0.0, 0.0, 0.0};
        double tiltSum = 0.0;
        int64_t flat = 0, steep = 0, soft = 0, noisy = 0;
        const double noiseCos = std::cos(35.0 * kPi / 180.0);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const size_t i = static_cast<size_t>(y) * width + x;
                if (!opaque[i]) continue;
                m.opaquePixels++;
                const float* n = &normals[i * 3];
                const double tilt = degrees(std::acos(std::clamp(static_cast<double>(n[2]), -1.0, 1.0)));
                tiltSum += tilt;
                if (tilt < 2.0) flat++;
                if (tilt > 60.0) steep++;
                if (normal.pixel(x, y)[3] < 250) soft++;
                if (painted && paint->rgba[i * 4 + 3] > 0) m.paintedPixels++;
                for (int k = 0; k < 3; k++) sum[k] += n[k];
                // the opaque 8-neighbours of the same cell
                double mean[3] = {0.0, 0.0, 0.0};
                int neighbours = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        const size_t j = static_cast<size_t>(ny) * width + nx;
                        if (!opaque[j] || owner[j] != owner[i]) continue;
                        for (int k = 0; k < 3; k++) mean[k] += normals[j * 3 + k];
                        neighbours++;
                    }
                }
                if (neighbours >= 3) {
                    const double length = std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
                    if (length > 1e-9 && (n[0] * mean[0] + n[1] * mean[1] + n[2] * mean[2]) / length < noiseCos) noisy++;
                }
            }
        }
        if (considered > 0) m.coveragePct = 100.0 * m.opaquePixels / static_cast<double>(considered);
        if (m.opaquePixels > 0) {
            const double count = m.opaquePixels;
            m.flatPct = 100.0 * flat / count;
            m.steepPct = 100.0 * steep / count;
            m.meanTiltDeg = tiltSum / count;
            m.noisePct = 100.0 * noisy / count;
            m.softEdgePct = 100.0 * soft / count;
            m.paintedPct = 100.0 * m.paintedPixels / count;
            const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
            if (length > 1e-12) m.leanDeg = degrees(std::acos(std::clamp(sum[2] / length, -1.0, 1.0)));
        }
        if (shape && isRgba8(*shape) && shape->width == width && shape->height == height) {
            std::vector<float> shapeNormals(normal.pixelCount() * 3, 0.0f);
            for (size_t i = 0; i < normal.pixelCount(); i++) {
                if (opaque[i]) decodeNormal(shape->data.data() + i * 4, directx, &shapeNormals[i * 3]);
            }
            measureSmoothness(m, normal, shapeNormals, cells, alphaThreshold);
        } else {
            measureSmoothness(m, normal, normals, cells, alphaThreshold);
        }
        for (const Cell& cell : cells) {
            bool touches = false;
            for (int y = std::max(0, cell.y); y < std::min(height, cell.y + cell.height) && !touches; y++) {
                for (int x = std::max(0, cell.x); x < std::min(width, cell.x + cell.width); x++) {
                    const bool border = x == cell.x || y == cell.y || x == cell.x + cell.width - 1 || y == cell.y + cell.height - 1;
                    if (border && opaque[static_cast<size_t>(y) * width + x]) {
                        touches = true;
                        break;
                    }
                }
            }
            if (touches) m.framesTouchingEdges++;
        }
        return m;
    }

    Assessment assess(const Settings& settings, const Metrics& m, int cellCount, const Result* result) {
        Assessment a;
        std::string profile = toString(settings.profile);
        if (settings.profile == Profile::Auto && result && result->profilesUsed) {
            std::string used;
            for (Profile p : {Profile::Rounded, Profile::Dome, Profile::Bevel, Profile::Inflate}) {
                if (result->profilesUsed & (1u << static_cast<int>(p))) used += (used.empty() ? "" : ", ") + std::string(toString(p));
            }
            profile += " -> " + used;
        }
        a.lines.push_back("Shape: " + profile + " (mean tilt " + integer(m.meanTiltDeg) + "°, " + integer(m.flatPct) + "% flat)");
        a.lines.push_back("Steep pixels: " + integer(m.steepPct) + "%");
        a.lines.push_back("Noise: " + integer(m.noisePct) + "%");
        a.lines.push_back("Lean: " + integer(m.leanDeg) + "°");
        if (m.innerPixels > 0) a.lines.push_back("Roughness: " + oneDecimal(m.roughnessDeg) + "° (creases " + oneDecimal(m.creasePct) + "%)");
        a.lines.push_back("Coverage: " + integer(m.coveragePct) + "%");
        if (cellCount > 0) {
            a.lines.push_back("Frames: " + std::to_string(cellCount) + " (" + std::to_string(m.framesTouchingEdges) + " touch their cell's edge)");
        }
        if (m.paintedPixels > 0) a.lines.push_back("Painted: " + std::to_string(m.paintedPixels) + " px (" + oneDecimal(m.paintedPct) + "%)");
        auto warn = [&](const char* kind, const std::string& message, const std::string& fix) { a.warnings.push_back({kind, "", message, fix}); };
        const bool transparentEdges = settings.edges == Edges::Transparent;
        if (!m.hasTransparency && transparentEdges) {
            warn("no_alpha", "The image has no transparent pixels: volume only bevels its border.", "For tiles use edges wrap or extend.");
        }
        if (m.flatPct > 60.0) {
            warn("mostly_flat", integer(m.flatPct) + "% of the sprite is flat.", "Raise strength or volume, or detail if it is painted with shading.");
        }
        if (m.steepPct > 25.0) {
            warn("too_steep", integer(m.steepPct) + "% of the pixels are steeper than 60°.",
                 "Lower strength (try " + oneDecimal(settings.strength * 0.6) + "), or raise smoothing.");
        }
        if (m.noisePct > 15.0) {
            warn("noisy", integer(m.noisePct) + "% of the pixels disagree with their neighbours (dithering or hard shading bands).",
                 "Raise smoothing to 1.5-2 or lower detail.");
        }
        if (m.creasePct > kCreasedPct) {
            warn("creased", oneDecimal(m.creasePct) + "% of the inside lies on sharp creases (ridges down the middle of the shapes).",
                 "Use profile inflate (round art) or dome (flat-faced props), or the painted or props preset, or raise softness.");
        } else if (m.roughnessDeg > kStreakyDeg) {
            warn("streaky", "The light is streaky inside the sprite (roughness " + oneDecimal(m.roughnessDeg) + "°): fans from the outline or bumps from the brushwork.",
                 "Raise softness (outline fans) or smoothing (brushwork), or use profile inflate.");
        }
        if (m.leanDeg > 8.0) {
            warn("leaning", "The map leans " + integer(m.leanDeg) + "° on average: the painted light direction became a slope.",
                 "Lower detail, or add a region with detail 0 for the highlight colours.");
        }
        if (m.framesTouchingEdges > 0 && transparentEdges) {
            warn("frames_touch_edges", std::to_string(m.framesTouchingEdges) + " frames touch their cell's edge, which gets a rim there.",
                 "Use edges extend if the sprite continues past the cell.");
        }
        // (version 2 above 128 px: the soft pixels reachable from outside are already outside
        // the shape, and enclosed ones are glass, rightly solid)
        if (m.softEdgePct > 10.0 && !(settings.version >= 2 && m.scale > 1.0)) {
            warn("soft_edges", integer(m.softEdgePct) + "% of the opaque pixels are semi-transparent; they count as solid.",
                 "Raise alpha_threshold to ignore glows and soft edges.");
        }
        a.hint = a.warnings.empty() ? "Looks fine." : a.warnings.front().fix;
        return a;
    }

}
