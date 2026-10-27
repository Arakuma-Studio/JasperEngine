// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generator: upstream spritefusion-sprite-normals lib.rs generate_normal + relief.rs generate
// (commit d14b067, MIT, LICENSE-spritefusion-sprite-normals.txt), extended with JasperEngine's
// profiles, edge modes, auto radius, outline and colour regions, height map and threads. The
// parity path (Rounded, Transparent, no regions) evaluates exactly upstream's float operations
// in upstream's order; compiled with -ffp-contract=off (CMakeLists.txt).

#include "SpriteNormals.h"

#include "SpriteNormalsField.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

namespace jasperengine::editor::spritenormals {

    using detail::Border;
    using detail::Field;

    namespace {

        std::string fmt(double v) {
            if (std::isnan(v)) return "nan";
            if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", v);
            return buffer;
        }

        std::string sizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        std::string lower(const std::string& s) {
            std::string out = s;
            for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            return out;
        }

        void checkRange(std::vector<Problem>& problems, const std::string& field, float value, float min, float max) {
            if (std::isfinite(value) && value >= min && value <= max) return;
            const std::string range = fmt(min) + " and " + fmt(max);
            problems.push_back({"invalid_settings", field,
                                field + " must be " + (std::isfinite(value) ? "" : "a number ") + "between " + range + " (got " + fmt(value) + ")",
                                "Use a value between " + range + "."});
        }

        // ---- the effective regions: the detected outline first, then the user's

        struct ColorLab {
            float L = 0.0f, a = 0.0f, b = 0.0f;
        };

        struct EffectiveRegion {
            std::vector<ColorLab> colors;
            float tolerance = 0.03f;
            float detail = 1.0f;
            float lift = 0.0f;
            bool flat = false;
            int userIndex = -1;   // index in settings.regions, -1 for the outline
        };

        ColorLab labOf(uint32_t rgb) {
            ColorLab c;
            detail::oklab(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), c.L, c.a, c.b);
            return c;
        }

        // The first region whose colours include `px` within tolerance, or -1
        int matchRegion(const std::vector<EffectiveRegion>& regions, const uint8_t* px) {
            float L, a, b;
            detail::oklab(px[0], px[1], px[2], L, a, b);
            for (size_t r = 0; r < regions.size(); r++) {
                const EffectiveRegion& region = regions[r];
                const float t2 = region.tolerance * region.tolerance;
                for (const ColorLab& c : region.colors) {
                    const float dL = L - c.L, da = a - c.a, db = b - c.b;
                    if (dL * dL + da * da + db * db <= t2) return static_cast<int>(r);
                }
            }
            return -1;
        }

        // ---- work items

        struct Item {
            int cell = 0;                 // index into the (deduplicated) cells
            detail::Component island;     // Transparent: the island, cell-local pixel indices
            float radius = 4.0f;
        };

        struct Context {
            const Image* color = nullptr;
            const Settings* settings = nullptr;
            const std::vector<Cell>* cells = nullptr;
            std::vector<EffectiveRegion> regions;
            bool gain = false;            // some region's detail != 1
            bool lift = false;            // some region lifts
            bool flat = false;            // some region is flat
            Image* normal = nullptr;
            std::vector<float>* heights = nullptr;   // settings.height: strength * combined per image pixel
            std::vector<uint8_t>* heightMask = nullptr;
        };

        // contract section 3 / Phase 2 spec 3.5: height of the silhouette at inset t (d = +INF:
        // no transparent pixel anywhere, a constant 0)
        inline float profileHeight(Profile profile, float distance, float S, float R) {
            if (std::isinf(distance)) return 0.0f;
            const float t = std::fmax(distance - 0.5f, 0.0f);
            switch (profile) {
                case Profile::Rounded: return (S * R) * std::log1p(t / R);
                case Profile::Dome: return t >= R ? S * R : S * std::sqrt(std::fmax(R * R - (R - t) * (R - t), 0.0f));
                case Profile::Bevel: return S * std::fmin(t, R);
            }
            return 0.0f;
        }

        // The Sobel of the plateau profiles (dome, bevel): the same slopes as detail::sobel but
        // summed as differences, so a plateau of any height gives exactly 0 (upstream's order,
        // kept for Rounded's parity, leaves a 1-ulp residue on a constant: 127 instead of 128)
        void balancedSobel(const Field& f, int x, int y, float& dx, float& dy, Border border) {
            auto at = [&](int px, int py) {
                if (border == Border::Wrap) {
                    px = (px % f.width + f.width) % f.width;
                    py = (py % f.height + f.height) % f.height;
                } else if (border == Border::Clamp) {
                    px = std::min(std::max(px, 0), f.width - 1);
                    py = std::min(std::max(py, 0), f.height - 1);
                }
                return f.data[static_cast<size_t>(py) * f.width + px];
            };
            dx = (((at(x + 1, y - 1) - at(x - 1, y - 1)) + 2.0f * (at(x + 1, y) - at(x - 1, y))) + (at(x + 1, y + 1) - at(x - 1, y + 1))) * 0.125f;
            dy = (((at(x - 1, y + 1) - at(x - 1, y - 1)) + 2.0f * (at(x, y + 1) - at(x, y - 1))) + (at(x + 1, y + 1) - at(x + 1, y - 1))) * 0.125f;
        }

        // Relief of one work item (upstream relief::generate, generalised): the field is the
        // island's box padded by 1 (Transparent) or the whole cell (Wrap / Extend). Returns the
        // bit set of the effective regions that matched a pixel.
        uint32_t runItem(const Item& item, const Context& ctx) {
            const Settings& s = *ctx.settings;
            const Cell& cell = (*ctx.cells)[static_cast<size_t>(item.cell)];
            const Image& color = *ctx.color;
            const bool transparent = s.edges == Edges::Transparent;
            const Border border = s.edges == Edges::Wrap ? Border::Wrap : (s.edges == Edges::Extend ? Border::Clamp : Border::Zero);

            // field geometry: field (fx, fy) = cell-local (fx + ox, fy + oy)
            int width, height, ox, oy;
            if (transparent) {
                width = item.island.maxX - item.island.minX + 3;
                height = item.island.maxY - item.island.minY + 3;
                ox = item.island.minX - 1;
                oy = item.island.minY - 1;
            } else {
                width = cell.width;
                height = cell.height;
                ox = oy = 0;
            }
            const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
            std::vector<uint8_t> mask(n, 0);
            std::vector<int> fieldPixels;   // field indices of the mask pixels
            Field opacity = Field::zero(width, height);
            Field lightness = Field::zero(width, height);
            const bool anyRegion = !ctx.regions.empty();
            std::vector<int> region;
            if (anyRegion) region.assign(n, -1);
            uint32_t matched = 0;

            auto addPixel = [&](int lx, int ly) {
                const size_t i = static_cast<size_t>(ly - oy) * width + static_cast<size_t>(lx - ox);
                mask[i] = 1;
                fieldPixels.push_back(static_cast<int>(i));
            };
            if (transparent) {
                // (upstream walks the island image row-major: only min/max below depend on the
                // pixels, so the BFS order gives the same values)
                for (int p : item.island.pixels) addPixel(p % cell.width, p / cell.width);
            } else {
                for (int ly = 0; ly < cell.height; ly++) {
                    for (int lx = 0; lx < cell.width; lx++) {
                        const uint8_t a = color.pixel(cell.x + lx, cell.y + ly)[3];
                        if (static_cast<float>(a) / 255.0f > s.alphaThreshold) addPixel(lx, ly);
                    }
                }
            }
            float low = 1.0f, high = 0.0f;
            for (int i : fieldPixels) {
                const int lx = i % width + ox, ly = i / width + oy;
                const uint8_t* px = color.pixel(cell.x + lx, cell.y + ly);
                opacity.data[static_cast<size_t>(i)] = static_cast<float>(px[3]) / 255.0f;
                const float value = detail::oklabL(px[0], px[1], px[2]);
                lightness.data[static_cast<size_t>(i)] = value;
                low = std::min(low, value);
                high = std::max(high, value);
                if (anyRegion) {
                    const int r = matchRegion(ctx.regions, px);
                    region[static_cast<size_t>(i)] = r;
                    if (r >= 0) matched |= 1u << r;
                }
            }
            // A constant height offset has no normal; centering avoids cancellation noise on flat art
            for (int i : fieldPixels) lightness.data[static_cast<size_t>(i)] -= (low + high) * 0.5f;

            Field shape = detail::edt(mask, width, height, border);
            for (float& value : shape.data) value = profileHeight(s.profile, value, s.volume, item.radius);
            Field combined = detail::gaussian(shape, s.softness, border);
            shape = Field();

            if (s.detail != 0.0f) {
                if (!ctx.gain) {
                    const Field fine = detail::maskedGaussian(lightness, opacity, s.smoothing, border);
                    const Field broad = detail::maskedGaussian(lightness, opacity, s.smoothing * 2.0f, border);
                    for (size_t i = 0; i < n; i++) combined.data[i] += s.detail * (0.7f * fine.data[i] + 0.3f * broad.data[i]);
                } else {
                    // excluded colours (detail 0) neither feed nor receive their own lightness: their
                    // height is interpolated from their neighbours; the gain blends the multipliers
                    Field g = Field::zero(width, height);
                    Field fed = opacity;
                    for (int i : fieldPixels) {
                        const int r = region[static_cast<size_t>(i)];
                        const float gi = r >= 0 ? ctx.regions[static_cast<size_t>(r)].detail : 1.0f;
                        g.data[static_cast<size_t>(i)] = gi;
                        if (!(gi > 0.0f)) fed.data[static_cast<size_t>(i)] = 0.0f;
                    }
                    const Field fine = detail::maskedGaussian(lightness, fed, s.smoothing, border);
                    const Field broad = detail::maskedGaussian(lightness, fed, s.smoothing * 2.0f, border);
                    const Field gain = detail::maskedGaussian(g, opacity, s.smoothing, border);
                    for (size_t i = 0; i < n; i++) {
                        combined.data[i] += s.detail * gain.data[i] * (0.7f * fine.data[i] + 0.3f * broad.data[i]);
                    }
                }
            }
            if (ctx.lift) {
                Field lift = Field::zero(width, height);
                for (int i : fieldPixels) {
                    const int r = region[static_cast<size_t>(i)];
                    if (r >= 0) lift.data[static_cast<size_t>(i)] = ctx.regions[static_cast<size_t>(r)].lift;
                }
                const Field blurred = detail::gaussian(lift, s.softness, border);
                for (size_t i = 0; i < n; i++) combined.data[i] += blurred.data[i];
            }

            Image& normal = *ctx.normal;
            for (int i : fieldPixels) {
                const int fx = i % width, fy = i / width;
                const int x = cell.x + fx + ox, y = cell.y + fy + oy;
                uint8_t* out = normal.pixel(x, y);
                const int r = anyRegion ? region[static_cast<size_t>(i)] : -1;
                if (ctx.flat && r >= 0 && ctx.regions[static_cast<size_t>(r)].flat) {
                    out[0] = 128;
                    out[1] = 128;
                    out[2] = 255;
                } else {
                    float dx, dy;
                    if (s.profile == Profile::Rounded) {
                        detail::sobel(combined, fx, fy, dx, dy, border);
                    } else {
                        balancedSobel(combined, fx, fy, dx, dy, border);
                    }
                    detail::encodeNormal(dx, dy, s.strength, s.directx, out);
                }
                if (ctx.heights) {
                    const size_t p = static_cast<size_t>(y) * color.width + x;
                    (*ctx.heights)[p] = s.strength * combined.data[static_cast<size_t>(i)];
                    (*ctx.heightMask)[p] = 1;
                }
            }
            return matched;
        }

        // The mask of a cell (alpha strictly above the cutoff), cell-local
        std::vector<uint8_t> cellMask(const Image& color, const Cell& cell, float threshold) {
            std::vector<uint8_t> mask(static_cast<size_t>(cell.width) * cell.height, 0);
            for (int y = 0; y < cell.height; y++) {
                for (int x = 0; x < cell.width; x++) {
                    mask[static_cast<size_t>(y) * cell.width + x] = static_cast<float>(color.pixel(cell.x + x, cell.y + y)[3]) / 255.0f > threshold;
                }
            }
            return mask;
        }

        inline float autoRadius(float side) { return std::min(std::max(0.15f * side, 4.0f), 512.0f); }

        // ignore_outline: the most frequent exact colour of the silhouette rim (mask pixels with a
        // 4-neighbour outside the mask or their cell) when it covers half of the rim and is dark
        std::optional<uint32_t> detectOutline(const Image& color, const std::vector<Cell>& cells, float threshold) {
            std::unordered_map<uint32_t, int64_t> counts;
            int64_t rim = 0;
            for (const Cell& cell : cells) {
                const std::vector<uint8_t> mask = cellMask(color, cell, threshold);
                auto inside = [&](int x, int y) {
                    return x >= 0 && y >= 0 && x < cell.width && y < cell.height && mask[static_cast<size_t>(y) * cell.width + x];
                };
                for (int y = 0; y < cell.height; y++) {
                    for (int x = 0; x < cell.width; x++) {
                        if (!inside(x, y)) continue;
                        if (inside(x - 1, y) && inside(x + 1, y) && inside(x, y - 1) && inside(x, y + 1)) continue;
                        const uint8_t* px = color.pixel(cell.x + x, cell.y + y);
                        counts[(uint32_t(px[0]) << 16) | (uint32_t(px[1]) << 8) | px[2]]++;
                        rim++;
                    }
                }
            }
            if (rim == 0) return std::nullopt;
            uint32_t best = 0;
            int64_t bestCount = -1;
            float bestL = 0.0f;
            for (const auto& [rgb, count] : counts) {
                const float L = detail::oklabL(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
                if (count > bestCount || (count == bestCount && (L < bestL || (L == bestL && rgb < best)))) {
                    best = rgb;
                    bestCount = count;
                    bestL = L;
                }
            }
            if (bestCount * 2 < rim || bestL > 0.35f) return std::nullopt;
            return best;
        }

        double secondsSince(std::chrono::steady_clock::time_point start) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    // ---- settings

    Settings presetSettings(Preset preset) {
        Settings s;
        s.preset = preset;
        switch (preset) {
            case Preset::Soft: s.strength = 1.0f; s.detail = 2.0f; s.smoothing = 1.0f; s.volume = 1.15f; break;
            case Preset::Default: s.strength = 1.5f; s.detail = 3.0f; s.smoothing = 1.0f; s.volume = 1.15f; break;
            case Preset::Crisp: s.strength = 1.5f; s.detail = 3.0f; s.smoothing = 0.6f; s.volume = 1.15f; break;
            case Preset::Detailed: s.strength = 1.5f; s.detail = 6.0f; s.smoothing = 0.6f; s.volume = 1.15f; break;
            case Preset::Chunky: s.strength = 2.5f; s.detail = 4.0f; s.smoothing = 0.6f; s.volume = 2.0f; break;
        }
        return s;
    }

    const char* toString(Preset preset) {
        switch (preset) {
            case Preset::Soft: return "soft";
            case Preset::Default: return "default";
            case Preset::Crisp: return "crisp";
            case Preset::Detailed: return "detailed";
            case Preset::Chunky: return "chunky";
        }
        return "default";
    }

    const char* toString(Profile profile) {
        switch (profile) {
            case Profile::Rounded: return "rounded";
            case Profile::Dome: return "dome";
            case Profile::Bevel: return "bevel";
        }
        return "rounded";
    }

    const char* toString(Edges edges) {
        switch (edges) {
            case Edges::Transparent: return "transparent";
            case Edges::Wrap: return "wrap";
            case Edges::Extend: return "extend";
        }
        return "transparent";
    }

    bool parsePreset(const std::string& name, Preset& out) {
        const std::string n = lower(name);
        for (Preset p : {Preset::Soft, Preset::Default, Preset::Crisp, Preset::Detailed, Preset::Chunky}) {
            if (n == toString(p)) {
                out = p;
                return true;
            }
        }
        return false;
    }

    bool parseProfile(const std::string& name, Profile& out) {
        const std::string n = lower(name);
        for (Profile p : {Profile::Rounded, Profile::Dome, Profile::Bevel}) {
            if (n == toString(p)) {
                out = p;
                return true;
            }
        }
        return false;
    }

    bool parseEdges(const std::string& name, Edges& out) {
        const std::string n = lower(name);
        for (Edges e : {Edges::Transparent, Edges::Wrap, Edges::Extend}) {
            if (n == toString(e)) {
                out = e;
                return true;
            }
        }
        return false;
    }

    bool parseColor(const std::string& text, uint32_t& out) {
        const size_t start = !text.empty() && text[0] == '#' ? 1 : 0;
        if (text.size() != start + 6) return false;
        uint32_t value = 0;
        for (size_t i = start; i < text.size(); i++) {
            const char c = text[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = value * 16 + static_cast<uint32_t>(digit);
        }
        out = value;
        return true;
    }

    std::string colorText(uint32_t rgb) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "#%06x", static_cast<unsigned>(rgb & 0xFFFFFFu));
        return buffer;
    }

    std::vector<Problem> validateSettings(const Settings& settings) {
        std::vector<Problem> problems;
        checkRange(problems, "strength", settings.strength, 0.0f, 8.0f);
        checkRange(problems, "detail", settings.detail, 0.0f, 8.0f);
        checkRange(problems, "smoothing", settings.smoothing, 0.0f, 8.0f);
        checkRange(problems, "volume", settings.volume, 0.0f, 8.0f);
        if (settings.radius) checkRange(problems, "radius", *settings.radius, 0.5f, 512.0f);
        checkRange(problems, "softness", settings.softness, 0.0f, 8.0f);
        checkRange(problems, "alpha_threshold", settings.alphaThreshold, 0.0f, 0.99f);
        if (settings.regions.size() > 16) {
            problems.push_back({"invalid_settings", "regions",
                                "regions has " + std::to_string(settings.regions.size()) + " entries; at most 16 are allowed",
                                "Merge regions: one region can list up to 16 colours."});
        }
        for (size_t r = 0; r < settings.regions.size(); r++) {
            const Region& region = settings.regions[r];
            const std::string field = "regions[" + std::to_string(r) + "]";
            if (region.colors.empty() || region.colors.size() > 16) {
                problems.push_back({"invalid_settings", field + ".colors",
                                    field + ".colors must have between 1 and 16 colours (got " + std::to_string(region.colors.size()) + ")",
                                    region.colors.empty() ? "List the colours as \"#rrggbb\"." : "Split the colours into several regions."});
            }
            for (size_t c = 0; c < region.colors.size(); c++) {
                if (region.colors[c] > 0xFFFFFFu) {
                    problems.push_back({"invalid_settings", field + ".colors[" + std::to_string(c) + "]",
                                        field + ".colors[" + std::to_string(c) + "] is not an RGB colour (got 0x" + [&] {
                                            char b[16];
                                            std::snprintf(b, sizeof(b), "%x", static_cast<unsigned>(region.colors[c]));
                                            return std::string(b);
                                        }() + ")",
                                        "Use \"#rrggbb\"."});
                }
            }
            checkRange(problems, field + ".tolerance", region.tolerance, 0.0f, 0.5f);
            checkRange(problems, field + ".detail", region.detail, 0.0f, 8.0f);
            checkRange(problems, field + ".lift", region.lift, -16.0f, 16.0f);
        }
        return problems;
    }

    // ---- cells

    bool sliceGrid(const Grid& grid, int imageWidth, int imageHeight, std::vector<Cell>& cells, std::string& error) {
        cells.clear();
        const double availW = static_cast<double>(imageWidth) - grid.offsetX;
        const double availH = static_cast<double>(imageHeight) - grid.offsetY;
        int cols = grid.columns, rows = grid.rows;
        double cw = grid.tileWidth, ch = grid.tileHeight;
        if (cw > 0 && ch > 0) {
            if (cols <= 0) cols = static_cast<int>(std::floor((availW + grid.paddingX) / (cw + grid.paddingX)));
            if (rows <= 0) rows = static_cast<int>(std::floor((availH + grid.paddingY) / (ch + grid.paddingY)));
        } else if (cols > 0 && rows > 0) {
            cw = std::floor((availW - static_cast<double>(grid.paddingX) * (cols - 1)) / cols);
            ch = std::floor((availH - static_cast<double>(grid.paddingY) * (rows - 1)) / rows);
        } else {
            error = "Give the grid a tile size [w, h] (pixels), or columns and rows.";
            return false;
        }
        const std::string empty = "The grid is empty: the image is " + sizeText(imageWidth, imageHeight) + " pixels.";
        if (cols <= 0 || rows <= 0 || cw < 1.0 || ch < 1.0 || grid.offsetX < 0 || grid.offsetY < 0 || grid.paddingX < 0 || grid.paddingY < 0) {
            error = empty;
            return false;
        }
        const int64_t all = static_cast<int64_t>(cols) * rows;
        const int64_t count = grid.count > 0 ? std::min<int64_t>(grid.count, all) : all;
        if (count > kMaxCells) {
            error = "The grid makes " + std::to_string(count) + " cells; at most " + std::to_string(kMaxCells) + " are allowed (pass count).";
            return false;
        }
        for (int64_t i = 0; i < count; i++) {
            const int r = static_cast<int>(i / cols), c = static_cast<int>(i % cols);
            Cell cell;
            cell.x = grid.offsetX + c * static_cast<int>(cw + grid.paddingX);
            cell.y = grid.offsetY + r * static_cast<int>(ch + grid.paddingY);
            cell.width = std::min(static_cast<int>(cw), imageWidth - cell.x);
            cell.height = std::min(static_cast<int>(ch), imageHeight - cell.y);
            // cells beyond the image (explicit columns / rows too many) are dropped
            if (cell.width >= 1 && cell.height >= 1) cells.push_back(cell);
        }
        if (cells.empty()) {
            error = empty;
            return false;
        }
        return true;
    }

    // ---- generation

    const char* toString(Status status) {
        switch (status) {
            case Status::Ok: return "ok";
            case Status::InvalidRequest: return "invalid_request";
            case Status::Cancelled: return "cancelled";
            case Status::Failed: return "failed";
        }
        return "failed";
    }

    Result generate(const Request& request, const Control& control) {
        const auto start = std::chrono::steady_clock::now();
        Result result;
        try {
            const Settings& s = request.settings;
            const Image& color = request.color;
            result.errors = validateSettings(s);

            // ---- the image and the cells
            bool imageOk = false;
            if (color.empty()) {
                result.errors.push_back({"invalid_source", "source", "The image is empty.", "Give an image with pixels."});
            } else if (color.channels != 4 || color.bitDepth != 8 ||
                       color.data.size() != color.pixelCount() * 4) {
                result.errors.push_back({"invalid_source", "source", "The image must be 8-bit RGBA (got " + texsynth::describeFormat(color) + ").",
                                         "Convert it with texsynth::toRgba8."});
            } else if (static_cast<int64_t>(color.width) * color.height > kMaxPixels) {
                result.errors.push_back({"too_large", "source",
                                         "The image is " + sizeText(color.width, color.height) + " (" +
                                             std::to_string(static_cast<int64_t>(color.width) * color.height) + " pixels); at most " +
                                             std::to_string(kMaxPixels) + " pixels (4096 x 4096) are allowed.",
                                         "Scale the image down or split it."});
            } else {
                imageOk = true;
            }

            std::vector<Cell> cells;
            if (imageOk) {
                if (request.cells.empty()) {
                    cells.push_back({0, 0, color.width, color.height});
                } else if (request.cells.size() > static_cast<size_t>(kMaxCells)) {
                    result.errors.push_back({"frames_mismatch", "frames",
                                             "There are " + std::to_string(request.cells.size()) + " cells; at most " + std::to_string(kMaxCells) + " are allowed.",
                                             "Use fewer, larger frames."});
                } else {
                    std::vector<size_t> original;   // the request index of each kept cell
                    for (size_t i = 0; i < request.cells.size(); i++) {
                        const Cell& c = request.cells[i];
                        if (c.width < 1 || c.height < 1 || c.x < 0 || c.y < 0 || c.x > color.width - c.width || c.y > color.height - c.height) {
                            result.errors.push_back({"frames_mismatch", "frames",
                                                     "Cell " + std::to_string(i) + " (" + std::to_string(c.x) + ", " + std::to_string(c.y) + ", " +
                                                         sizeText(c.width, c.height) + ") is not inside the " + sizeText(color.width, color.height) + " image.",
                                                     "Give cells inside the image, at least 1x1 pixel."});
                            continue;
                        }
                        bool duplicate = false;
                        for (size_t k = 0; k < cells.size() && !duplicate; k++) {
                            if (cells[k] == c) {
                                duplicate = true;
                            } else if (c.x < cells[k].x + cells[k].width && cells[k].x < c.x + c.width && c.y < cells[k].y + cells[k].height &&
                                       cells[k].y < c.y + c.height) {
                                result.errors.push_back({"frames_mismatch", "frames",
                                                         "Cells " + std::to_string(original[k]) + " and " + std::to_string(i) + " overlap.",
                                                         "Give cells that share no pixel."});
                                duplicate = true;   // reported once
                            }
                        }
                        if (!duplicate) {
                            cells.push_back(c);
                            original.push_back(i);
                        }
                    }
                }
            }
            if (!result.errors.empty()) {
                result.status = Status::InvalidRequest;
                result.seconds = secondsSince(start);
                return result;
            }

            // ---- output and regions
            result.normal = Image::make(color.width, color.height, 4, 8);
            const size_t pixels = color.pixelCount();
            for (size_t i = 0; i < pixels; i++) {
                uint8_t* out = result.normal.data.data() + i * 4;
                out[0] = 128;
                out[1] = 128;
                out[2] = 255;
                out[3] = color.data[i * 4 + 3];
            }

            Context ctx;
            ctx.color = &color;
            ctx.settings = &s;
            ctx.cells = &cells;
            ctx.normal = &result.normal;
            if (s.ignoreOutline) {
                result.outlineColor = detectOutline(color, cells, s.alphaThreshold);
                if (result.outlineColor) {
                    EffectiveRegion outline;
                    outline.colors.push_back(labOf(*result.outlineColor));
                    outline.detail = 0.0f;
                    ctx.regions.push_back(outline);
                } else {
                    result.warnings.push_back({"outline_not_found", "ignore_outline",
                                               "No outline colour covers half of the silhouette rim; nothing was excluded.", ""});
                }
            }
            for (size_t r = 0; r < s.regions.size(); r++) {
                EffectiveRegion region;
                for (uint32_t c : s.regions[r].colors) region.colors.push_back(labOf(c));
                region.tolerance = s.regions[r].tolerance;
                region.detail = s.regions[r].detail;
                region.lift = s.regions[r].lift;
                region.flat = s.regions[r].flat;
                region.userIndex = static_cast<int>(r);
                ctx.regions.push_back(region);
            }
            for (const EffectiveRegion& region : ctx.regions) {
                ctx.gain = ctx.gain || region.detail != 1.0f;
                ctx.lift = ctx.lift || region.lift != 0.0f;
                ctx.flat = ctx.flat || region.flat;
            }
            std::vector<float> heights;
            std::vector<uint8_t> heightMask;
            if (s.height) {
                heights.assign(pixels, 0.0f);
                heightMask.assign(pixels, 0);
                ctx.heights = &heights;
                ctx.heightMask = &heightMask;
            }

            // ---- work items, in deterministic order, and the radii
            std::vector<Item> items;
            const bool transparent = s.edges == Edges::Transparent;
            const bool autoR = !s.radius.has_value();
            std::vector<float> largestSides;   // sequence: per cell with an island
            for (size_t c = 0; c < cells.size(); c++) {
                const Cell& cell = cells[c];
                std::vector<detail::Component> islands;
                if (transparent || (autoR && request.sequence)) {
                    islands = detail::components(cellMask(color, cell, s.alphaThreshold), cell.width, cell.height);
                }
                if (autoR && request.sequence && !islands.empty()) {
                    size_t largest = 0;
                    for (size_t k = 1; k < islands.size(); k++) {
                        if (islands[k].pixels.size() > islands[largest].pixels.size()) largest = k;
                    }
                    const detail::Component& big = islands[largest];
                    largestSides.push_back(static_cast<float>(std::min(big.maxX - big.minX + 1, big.maxY - big.minY + 1)));
                }
                if (transparent) {
                    for (detail::Component& island : islands) {
                        Item item;
                        item.cell = static_cast<int>(c);
                        item.radius = autoR ? autoRadius(static_cast<float>(std::min(island.maxX - island.minX + 1, island.maxY - island.minY + 1)))
                                            : *s.radius;
                        item.island = std::move(island);
                        items.push_back(std::move(item));
                    }
                } else {
                    Item item;
                    item.cell = static_cast<int>(c);
                    item.radius = autoR ? autoRadius(static_cast<float>(std::min(cell.width, cell.height))) : s.radius.value_or(4.0f);
                    items.push_back(std::move(item));
                }
            }
            if (autoR && request.sequence) {
                float radius = 4.0f;
                if (!largestSides.empty()) {
                    std::sort(largestSides.begin(), largestSides.end());
                    const size_t m = largestSides.size();
                    const float median = m % 2 ? largestSides[m / 2] : (largestSides[m / 2 - 1] + largestSides[m / 2]) * 0.5f;
                    radius = autoRadius(median);
                }
                for (Item& item : items) item.radius = radius;
                result.radiusMin = result.radiusMax = radius;
            } else if (autoR) {
                result.radiusPerIsland = true;
                result.radiusMin = result.radiusMax = 4.0f;
                for (size_t k = 0; k < items.size(); k++) {
                    result.radiusMin = k ? std::min(result.radiusMin, items[k].radius) : items[k].radius;
                    result.radiusMax = k ? std::max(result.radiusMax, items[k].radius) : items[k].radius;
                }
            } else {
                result.radiusMin = result.radiusMax = *s.radius;
            }
            result.islands = static_cast<int>(items.size());

            // ---- run the items on the worker threads (disjoint output pixels: any schedule
            // writes the same bytes)
            int threads = request.threads;
            if (threads <= 0) {
                const unsigned hw = std::thread::hardware_concurrency();
                threads = static_cast<int>(std::min(hw == 0 ? 1u : hw, 16u));
            }
            threads = std::max(1, std::min(threads, static_cast<int>(std::max<size_t>(items.size(), 1))));
            result.threads = threads;

            std::vector<uint32_t> matched(items.size(), 0);
            std::atomic<size_t> next{0}, done{0};
            std::atomic<bool> cancelled{false}, outOfMemory{false}, failed{false};
            std::mutex progressMutex;
            double lastReport = -std::numeric_limits<double>::infinity();
            const size_t total = items.size();
            auto work = [&]() {
                try {
                    for (;;) {
                        if (outOfMemory || failed || cancelled) return;
                        if (control.cancel && control.cancel->load()) {
                            cancelled = true;
                            return;
                        }
                        const size_t k = next++;
                        if (k >= total) return;
                        matched[k] = runItem(items[k], ctx);
                        const size_t finished = ++done;
                        if (control.onProgress) {
                            std::lock_guard<std::mutex> lock(progressMutex);
                            const double now = secondsSince(start);
                            if (finished == total || now - lastReport >= control.progressIntervalSeconds) {
                                lastReport = now;
                                Progress progress;
                                progress.fraction = static_cast<double>(finished) / static_cast<double>(total);
                                progress.elapsedSeconds = now;
                                control.onProgress(progress);
                            }
                        }
                    }
                } catch (const std::bad_alloc&) {
                    outOfMemory = true;
                } catch (...) {
                    failed = true;
                }
            };
            if (threads == 1) {
                work();
            } else {
                std::vector<std::thread> pool;
                pool.reserve(static_cast<size_t>(threads));
                try {
                    for (int t = 0; t < threads; t++) pool.emplace_back(work);
                } catch (...) {
                    // fewer threads than asked: the ones started (or this one) finish the items
                    if (pool.empty()) work();
                }
                for (std::thread& thread : pool) thread.join();
            }
            if (outOfMemory || failed) {
                Result failure;
                failure.status = Status::Failed;
                failure.errors.push_back({"failed", "", outOfMemory ? "Out of memory while generating the normal map." : "Generating the normal map failed.",
                                          outOfMemory ? "Use a smaller image or fewer threads." : ""});
                failure.seconds = secondsSince(start);
                return failure;
            }
            if (cancelled || (control.cancel && control.cancel->load() && done < total)) {
                Result stopped;
                stopped.status = Status::Cancelled;
                stopped.errors.push_back({"cancelled", "", "The generation was cancelled.", ""});
                stopped.seconds = secondsSince(start);
                return stopped;
            }

            // ---- warnings and the height map
            uint32_t any = 0;
            for (uint32_t m : matched) any |= m;
            for (size_t r = 0; r < ctx.regions.size(); r++) {
                const EffectiveRegion& region = ctx.regions[r];
                if (region.userIndex < 0 || (any & (1u << r))) continue;
                const Region& user = s.regions[static_cast<size_t>(region.userIndex)];
                result.warnings.push_back({"region_unmatched", "regions[" + std::to_string(region.userIndex) + "]",
                                           "regions[" + std::to_string(region.userIndex) + "] (" + colorText(user.colors.front()) +
                                               ") matched no pixel; widen its tolerance (" + fmt(user.tolerance) + ").",
                                           "Raise regions[" + std::to_string(region.userIndex) + "].tolerance or pick the colour from the image."});
            }
            if (s.height) {
                float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
                for (size_t i = 0; i < pixels; i++) {
                    if (!heightMask[i]) continue;
                    lo = std::min(lo, heights[i]);
                    hi = std::max(hi, heights[i]);
                }
                result.height = Image::make(color.width, color.height, 1, 16);
                if (lo > hi) lo = hi = 0.0f;
                result.heightMin = lo;
                result.heightMax = hi;
                uint16_t* out = reinterpret_cast<uint16_t*>(result.height.data.data());
                for (size_t i = 0; i < pixels; i++) {
                    if (!heightMask[i]) {
                        out[i] = 0;
                    } else if (hi == lo) {
                        out[i] = 32768;
                    } else {
                        const double v = std::round((static_cast<double>(heights[i]) - lo) / (static_cast<double>(hi) - lo) * 65535.0);
                        out[i] = static_cast<uint16_t>(std::min(std::max(v, 0.0), 65535.0));
                    }
                }
            }
            result.status = Status::Ok;
            result.seconds = secondsSince(start);
            return result;
        } catch (const std::bad_alloc&) {
            Result failure;
            failure.status = Status::Failed;
            failure.errors.push_back({"failed", "", "Out of memory while generating the normal map.", "Use a smaller image."});
            failure.seconds = secondsSince(start);
            return failure;
        } catch (...) {
            Result failure;
            failure.status = Status::Failed;
            failure.errors.push_back({"failed", "", "Generating the normal map failed.", ""});
            failure.seconds = secondsSince(start);
            return failure;
        }
    }
}
