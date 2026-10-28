// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Float-exact ports of spritefusion-sprite-normals' field.rs, distance.rs, components.rs and
// color.rs (commit d14b067, MIT, LICENSE-spritefusion-sprite-normals.txt). Compiled with
// -ffp-contract=off (CMakeLists.txt): every a*b+c below is a separately rounded multiply and
// add, as in Rust; std::fma is called exactly where upstream calls mul_add.

#include "SpriteNormalsField.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace jasperengine::editor::spritenormals::detail {

    namespace {

        // Index of `i` (any integer) on a line of `n` samples read through `border`; -1 when it
        // reads nothing (Border::Zero outside the line)
        inline int borderIndex(int i, int n, Border border) {
            if (i >= 0 && i < n) return i;
            switch (border) {
                case Border::Zero: return -1;
                case Border::Wrap: {
                    const int m = i % n;
                    return m < 0 ? m + n : m;
                }
                case Border::Clamp: return i < 0 ? 0 : n - 1;
            }
            return -1;
        }

        // upstream distance.rs squared_line: the lower envelope of the parabolas rooted at the
        // finite samples of `source` (Felzenszwalb-Huttenlocher), sampled at every position
        void squaredLine(const float* source, float* destination, int n, std::vector<int>& sites, std::vector<double>& starts) {
            sites.clear();
            starts.clear();
            for (int q = 0; q < n; q++) {
                const float height = source[q];
                if (!std::isfinite(height)) continue;
                double intersection = -std::numeric_limits<double>::infinity();
                while (!sites.empty()) {
                    const int p = sites.back();
                    const double qd = static_cast<double>(q);
                    const double pd = static_cast<double>(p);
                    intersection = ((static_cast<double>(height) + qd * qd) - static_cast<double>(source[p]) - pd * pd) /
                                   (2.0 * static_cast<double>(q - p));
                    if (intersection > starts.back()) break;
                    sites.pop_back();
                    starts.pop_back();
                }
                if (sites.empty()) intersection = -std::numeric_limits<double>::infinity();
                sites.push_back(q);
                starts.push_back(intersection);
            }
            if (sites.empty()) {
                std::fill(destination, destination + n, std::numeric_limits<float>::infinity());
                return;
            }
            size_t active = 0;
            for (int q = 0; q < n; q++) {
                while (active + 1 < starts.size() && starts[active + 1] < static_cast<double>(q)) active++;
                const float delta = static_cast<float>(q) - static_cast<float>(sites[active]);
                destination[q] = std::fma(delta, delta, source[sites[active]]);
            }
        }

        // One 1-D pass of the EDT on a line of n samples. Wrap: the line repeated three times
        // (sites from every copy), the middle copy kept: the exact distance on the circle.
        void edtLine(const float* source, float* destination, int n, Border border, std::vector<float>& scratch,
                     std::vector<int>& sites, std::vector<double>& starts) {
            if (border != Border::Wrap) {
                // Zero and Clamp alike: no site beyond the line (Clamp: outside counts as mask)
                squaredLine(source, destination, n, sites, starts);
                return;
            }
            scratch.resize(static_cast<size_t>(n) * 6);
            float* tripled = scratch.data();
            float* out = tripled + 3 * static_cast<size_t>(n);
            for (int k = 0; k < 3; k++) std::copy(source, source + n, tripled + static_cast<size_t>(k) * n);
            squaredLine(tripled, out, 3 * n, sites, starts);
            std::copy(out + n, out + 2 * static_cast<size_t>(n), destination);
        }

        // The normalized kernel of upstream field.rs gaussian: taps -r..r, summed in that order
        std::vector<float> gaussianKernel(float sigma, int& radius) {
            radius = static_cast<int>(std::ceil(sigma * 3.0f));
            std::vector<float> kernel(static_cast<size_t>(2 * radius + 1));
            for (int o = -radius; o <= radius; o++) {
                const float t = static_cast<float>(o) / sigma;
                kernel[static_cast<size_t>(o + radius)] = std::exp(-0.5f * (t * t));
            }
            float total = 0.0f;
            for (float v : kernel) total += v;
            for (float& v : kernel) v /= total;
            return kernel;
        }
    }

    Field Field::zero(int width, int height) {
        Field field;
        field.width = width;
        field.height = height;
        field.data.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0f);
        return field;
    }

    const float* srgbToLinearTable() {
        static const std::array<float, 256> table = [] {
            std::array<float, 256> t{};
            for (int i = 0; i < 256; i++) {
                const float v = static_cast<float>(i) / 255.0f;
                t[static_cast<size_t>(i)] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();
        return table.data();
    }

    namespace {
        // The cube root rounded correctly to float. The upstream binary does not take cbrtf from
        // the C library (glibc's is up to 1 ulp off) but from Rust's own libm, whose cbrtf is
        // correctly rounded; glibc's double cbrt (within 1 ulp of 2^-52) rounded once to float
        // gives the same value for every float input (checked exhaustively over 0..1, the range
        // the LMS terms of an sRGB colour fall in; the report of Phase 2A has the check).
        inline float cbrtFloat(float x) { return static_cast<float>(std::cbrt(static_cast<double>(x))); }

        inline void lmsRoots(uint8_t r8, uint8_t g8, uint8_t b8, float& l, float& m, float& s) {
            const float* linear = srgbToLinearTable();
            const float r = linear[r8], g = linear[g8], b = linear[b8];
            l = cbrtFloat(0.41222146f * r + 0.53633255f * g + 0.051445995f * b);
            m = cbrtFloat(0.2119035f * r + 0.6806995f * g + 0.10739696f * b);
            s = cbrtFloat(0.08830246f * r + 0.28171885f * g + 0.6299787f * b);
        }
    }

    float oklabL(uint8_t r, uint8_t g, uint8_t b) {
        float l, m, s;
        lmsRoots(r, g, b, l, m, s);
        return 0.21045426f * l + 0.7936178f * m - 0.004072047f * s;
    }

    void oklab(uint8_t r, uint8_t g, uint8_t b, float& L, float& a, float& bb) {
        float l, m, s;
        lmsRoots(r, g, b, l, m, s);
        L = 0.21045426f * l + 0.7936178f * m - 0.004072047f * s;
        a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
        bb = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    }

    Field edt(const std::vector<uint8_t>& mask, int width, int height, Border border) {
        Field result = Field::zero(width, height);
        if (width <= 0 || height <= 0) return result;
        const int longest = std::max(width, height);
        std::vector<float> source(static_cast<size_t>(longest)), destination(static_cast<size_t>(longest)), scratch;
        std::vector<int> sites;
        std::vector<double> starts;
        sites.reserve(static_cast<size_t>(longest) * 3);
        starts.reserve(static_cast<size_t>(longest) * 3);
        const float inf = std::numeric_limits<float>::infinity();
        for (int y = 0; y < height; y++) {
            const size_t row = static_cast<size_t>(y) * width;
            for (int x = 0; x < width; x++) source[static_cast<size_t>(x)] = mask[row + x] ? inf : 0.0f;
            edtLine(source.data(), destination.data(), width, border, scratch, sites, starts);
            std::copy(destination.begin(), destination.begin() + width, result.data.begin() + static_cast<std::ptrdiff_t>(row));
        }
        for (int x = 0; x < width; x++) {
            for (int y = 0; y < height; y++) source[static_cast<size_t>(y)] = result.data[static_cast<size_t>(y) * width + x];
            edtLine(source.data(), destination.data(), height, border, scratch, sites, starts);
            for (int y = 0; y < height; y++) result.data[static_cast<size_t>(y) * width + x] = std::sqrt(destination[static_cast<size_t>(y)]);
        }
        return result;
    }

    Field gaussian(const Field& input, float sigma, Border border) {
        if (sigma < 0.05f) return input;
        int radius = 0;
        const std::vector<float> kernel = gaussianKernel(sigma, radius);
        const int width = input.width, height = input.height;
        Field horizontal = Field::zero(width, height);
        // Every output accumulates out += source * weight in tap order -r..r; taps outside the
        // field are skipped (Zero) or read through the border. The tap loop is outermost so the
        // inner loop runs over contiguous samples, like upstream's add_scaled.
        for (int y = 0; y < height; y++) {
            const float* src = input.data.data() + static_cast<size_t>(y) * width;
            float* out = horizontal.data.data() + static_cast<size_t>(y) * width;
            for (int o = -radius; o <= radius; o++) {
                const float weight = kernel[static_cast<size_t>(o + radius)];
                if (border == Border::Zero) {
                    const int begin = std::max(0, -o), end = std::min(width, width - o);
                    for (int x = begin; x < end; x++) out[x] += src[x + o] * weight;
                } else {
                    for (int x = 0; x < width; x++) out[x] += src[borderIndex(x + o, width, border)] * weight;
                }
            }
        }
        Field result = Field::zero(width, height);
        for (int y = 0; y < height; y++) {
            float* out = result.data.data() + static_cast<size_t>(y) * width;
            for (int o = -radius; o <= radius; o++) {
                const int row = borderIndex(y + o, height, border);
                if (row < 0) continue;
                const float weight = kernel[static_cast<size_t>(o + radius)];
                const float* src = horizontal.data.data() + static_cast<size_t>(row) * width;
                for (int x = 0; x < width; x++) out[x] += src[x] * weight;
            }
        }
        return result;
    }

    Field maskedGaussian(const Field& input, const Field& weight, float sigma, Border border) {
        Field weighted = input;
        for (size_t i = 0; i < weighted.data.size(); i++) weighted.data[i] *= weight.data[i];
        Field result = gaussian(weighted, sigma, border);
        const Field denominator = gaussian(weight, sigma, border);
        for (size_t i = 0; i < result.data.size(); i++) result.data[i] /= std::fmax(denominator.data[i], 1e-8f);
        return result;
    }

    void sobel(const Field& input, int x, int y, float& dx, float& dy, Border border) {
        const float* d = input.data.data();
        const int w = input.width, h = input.height;
        float ul, u, ur, l, r, dl, dn, dr;
        if (border == Border::Zero) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const size_t up = i - w, down = i + w;
            ul = d[up - 1]; u = d[up]; ur = d[up + 1];
            l = d[i - 1]; r = d[i + 1];
            dl = d[down - 1]; dn = d[down]; dr = d[down + 1];
        } else {
            const int x0 = borderIndex(x - 1, w, border), x2 = borderIndex(x + 1, w, border);
            const size_t r0 = static_cast<size_t>(borderIndex(y - 1, h, border)) * w;
            const size_t r1 = static_cast<size_t>(y) * w;
            const size_t r2 = static_cast<size_t>(borderIndex(y + 1, h, border)) * w;
            ul = d[r0 + x0]; u = d[r0 + x]; ur = d[r0 + x2];
            l = d[r1 + x0]; r = d[r1 + x2];
            dl = d[r2 + x0]; dn = d[r2 + x]; dr = d[r2 + x2];
        }
        dx = (((((ur + 2.0f * r) + dr) - ul) - 2.0f * l) - dl) * 0.125f;
        dy = (((((dl + 2.0f * dn) + dr) - ul) - 2.0f * u) - ur) * 0.125f;
    }

    std::vector<Component> components(const std::vector<uint8_t>& mask, int width, int height) {
        std::vector<Component> result;
        if (width <= 0 || height <= 0) return result;
        std::vector<uint8_t> visited(mask.size(), 0);
        for (size_t start = 0; start < mask.size(); start++) {
            if (!mask[start] || visited[start]) continue;
            Component part;
            part.pixels.push_back(static_cast<int>(start));
            part.minX = part.maxX = static_cast<int>(start % width);
            part.minY = part.maxY = static_cast<int>(start / width);
            visited[start] = 1;
            for (size_t cursor = 0; cursor < part.pixels.size(); cursor++) {
                const int index = part.pixels[cursor];
                const int x = index % width, y = index / width;
                part.minX = std::min(part.minX, x);
                part.maxX = std::max(part.maxX, x);
                part.minY = std::min(part.minY, y);
                part.maxY = std::max(part.maxY, y);
                for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, height - 1); ny++) {
                    for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, width - 1); nx++) {
                        const size_t neighbor = static_cast<size_t>(ny) * width + nx;
                        if (mask[neighbor] && !visited[neighbor]) {
                            visited[neighbor] = 1;
                            part.pixels.push_back(static_cast<int>(neighbor));
                        }
                    }
                }
            }
            result.push_back(std::move(part));
        }
        return result;
    }

    void encodeNormal(float dx, float dy, float strength, bool directx, uint8_t out[3]) {
        const float x = -dx * strength;
        float y = dy * strength;
        if (directx) y = -y;
        const float length = std::sqrt(std::fma(x, x, std::fma(y, y, 1.0f)));
        const float v[3] = {x / length, y / length, 1.0f / length};
        for (int k = 0; k < 3; k++) {
            const float byte = std::round((v[k] * 0.5f + 0.5f) * 255.0f);
            out[k] = static_cast<uint8_t>(byte <= 0.0f ? 0.0f : (byte >= 255.0f ? 255.0f : byte));
        }
    }

    void slopeNormal(float dx, float dy, float strength, float v[3]) {
        const float x = -dx * strength;
        const float y = dy * strength;
        const float length = std::sqrt(std::fma(x, x, std::fma(y, y, 1.0f)));
        v[0] = x / length;
        v[1] = y / length;
        v[2] = 1.0f / length;
    }

    void encodeUnit(const float v[3], bool directx, uint8_t out[3]) {
        const float n[3] = {v[0], directx ? -v[1] : v[1], v[2]};
        for (int k = 0; k < 3; k++) {
            const float byte = std::round((n[k] * 0.5f + 0.5f) * 255.0f);
            out[k] = static_cast<uint8_t>(byte <= 0.0f ? 0.0f : (byte >= 255.0f ? 255.0f : byte));
        }
    }
}
