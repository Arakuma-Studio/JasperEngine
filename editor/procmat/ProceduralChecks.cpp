// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralChecks.h"

#include "texsynth/TextureSynthesis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace jasperengine::editor;
using namespace jasperengine::editor::procmat;

namespace {

    using Json = qa::Json;

    // the side the image-wide statistics (repetition, spectrum) work at: enough for a pattern's period,
    // cheap at any bake size
    constexpr int STAT_SIDE = 64;
    constexpr double PI = 3.14159265358979323846;

    double round3(double value) {
        return std::round(value * 1000.0) / 1000.0;
    }

    double round1(double value) {
        return std::round(value * 10.0) / 10.0;
    }

    const OutputImage* find(const MaterialImages& images, const char* name) {
        auto it = images.outputs.find(name);
        if (it == images.outputs.end() || it->second.width <= 0 || it->second.height <= 0 ||
            it->second.rgba.size() != (size_t)it->second.width * (size_t)it->second.height * 4) {
            return nullptr;
        }
        return &it->second;
    }

    size_t texels(const OutputImage& image) {
        return (size_t)image.width * (size_t)image.height;
    }

    // sRGB-weighted luma of 8-bit sRGB values, 0..255
    double luma(const uint8_t* p) {
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    }

    // what a block reads of an output: a channel, or the luma of a colour
    std::vector<uint8_t> values(const OutputImage& image, int channel) {
        std::vector<uint8_t> out(texels(image));
        for (size_t i = 0; i < out.size(); i++) {
            const uint8_t* p = &image.rgba[i * 4];
            out[i] = channel < 0 ? (uint8_t)std::clamp((int)std::lround(luma(p)), 0, 255) : p[channel];
        }
        return out;
    }

    struct Histogram {
        std::array<size_t, 256> counts{};
        size_t total = 0;

        explicit Histogram(const std::vector<uint8_t>& data) {
            for (uint8_t v : data) counts[v]++;
            total = data.size();
        }
        // the value below which `share` of the texels lie, 0..1
        double percentile(double share) const {
            const size_t target = (size_t)std::ceil(share * (double)total);
            size_t seen = 0;
            for (int v = 0; v < 256; v++) {
                seen += counts[(size_t)v];
                if (seen >= std::max<size_t>(target, 1)) return v / 255.0;
            }
            return 1.0;
        }
        double percent(size_t count) const {
            return total ? 100.0 * (double)count / (double)total : 0.0;
        }
    };

    Json histogramJson(const std::vector<uint8_t>& data) {
        const Histogram h(data);
        Json bins = Json::array();
        for (int b = 0; b < 16; b++) {
            size_t count = 0;
            for (int v = b * 16; v < b * 16 + 16; v++) count += h.counts[(size_t)v];
            bins.push_back(round1(h.percent(count)));
        }
        return {{"bins_percent", bins}, {"min", round3(h.percentile(0.0))}, {"p1", round3(h.percentile(0.01))}, {"p50", round3(h.percentile(0.5))},
                {"p99", round3(h.percentile(0.99))}, {"max", round3(h.percentile(1.0))},
                {"clipped_low_percent", round1(h.percent(h.counts[0]))}, {"clipped_high_percent", round1(h.percent(h.counts[255]))}};
    }

    // a box-filtered STAT_SIDE square of the values, zero mean
    std::vector<double> reduced(const std::vector<uint8_t>& data, int width, int height) {
        std::vector<double> out((size_t)STAT_SIDE * STAT_SIDE, 0.0);
        std::vector<int> counts(out.size(), 0);
        for (int y = 0; y < height; y++) {
            const int ry = y * STAT_SIDE / height;
            for (int x = 0; x < width; x++) {
                const size_t cell = (size_t)ry * STAT_SIDE + (size_t)(x * STAT_SIDE / width);
                out[cell] += data[(size_t)y * width + x];
                counts[cell]++;
            }
        }
        double mean = 0.0;
        for (size_t i = 0; i < out.size(); i++) {
            out[i] = counts[i] ? out[i] / counts[i] : 0.0;
            mean += out[i];
        }
        mean /= (double)out.size();
        for (double& v : out) v -= mean;
        return out;
    }

    // The strongest peak of the circular autocorrelation away from the origin, normalized to -1..1, and where.
    // Only local maxima count: a smooth pattern correlates with itself a few cells away, a repeat is a second
    // peak.
    double autocorrelationPeak(const std::vector<double>& field, int& atX, int& atY) {
        const int n = STAT_SIDE;
        double zero = 0.0;
        for (double v : field) zero += v * v;
        atX = atY = 0;
        if (zero <= 1e-9) return 0.0;
        std::vector<double> ac((size_t)n * n, 0.0);
        for (int dy = 0; dy < n; dy++) {
            for (int dx = 0; dx < n; dx++) {
                double sum = 0.0;
                for (int y = 0; y < n; y++) {
                    const double* row = &field[(size_t)y * n];
                    const double* other = &field[(size_t)((y + dy) % n) * n];
                    for (int x = 0; x < n; x++) sum += row[x] * other[(x + dx) % n];
                }
                ac[(size_t)dy * n + dx] = sum / zero;
            }
        }
        double best = 0.0;
        for (int dy = 0; dy < n; dy++) {
            for (int dx = 0; dx < n; dx++) {
                if (dx == 0 && dy == 0) continue;
                const double value = ac[(size_t)dy * n + dx];
                bool peak = true;
                for (int oy = -1; oy <= 1 && peak; oy++) {
                    for (int ox = -1; ox <= 1; ox++) {
                        if ((ox || oy) && ac[(size_t)((dy + oy + n) % n) * n + (size_t)((dx + ox + n) % n)] > value) {
                            peak = false;
                            break;
                        }
                    }
                }
                if (peak && value > best) {
                    best = value;
                    atX = dx;
                    atY = dy;
                }
            }
        }
        return best;
    }

    texsynth::Image asSynthImage(const OutputImage& image) {
        texsynth::Image out;
        out.width = image.width;
        out.height = image.height;
        out.channels = 4;
        out.bitDepth = 8;
        out.data = image.rgba;
        return out;
    }

    // The bytes a texture of this output takes once cooked (docs/textures.md, Normal quality), with its
    // mips: desktop BC7 (BC5 for normals, BC4 for one channel), mobile ASTC 6x6, web UASTC
    double cookedBytes(const std::string& name, size_t count, const std::string& platform) {
        double perTexel = 1.0;
        if (platform == "desktop") perTexel = name == "height" ? 0.5 : 1.0;
        else if (platform == "mobile") perTexel = 16.0 / 36.0;
        return perTexel * (double)count * 4.0 / 3.0;
    }

    Json tileabilityRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("tileability", "content");
        row["method"] = "synthesize_texture's seam score: the mean RGBA step across the wrap edge over the steps within 8 texels of it";
        row["threshold"] = qa.seamFail;
        row["threshold_source"] = "materials.qa.seam_fail";
        if (m.is_null()) return row;
        double worst = 0.0;
        std::string where;
        Json measured = Json::object();
        for (auto it = m.begin(); it != m.end(); ++it) {
            const double x = it.value().value("seam_x", 1.0), y = it.value().value("seam_y", 1.0);
            measured[it.key()] = std::max(x, y);
            if (std::max(x, y) > worst) {
                worst = std::max(x, y);
                where = it.key() + (x >= y ? " across its left and right edges" : " across its top and bottom edges");
            }
        }
        row["measured"] = measured;
        if (worst > qa.seamFail) {
            row["status"] = "FAIL";
            row["limited_by"] = "seam " + std::to_string(round3(worst)).substr(0, 4) + " on " + where + ".";
            row["fix_hint"] = "A node feeding it does not tile: give its scale a whole number of repeats, or turn its tiling on.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json repetitionRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("repetition", "content");
        row["method"] = "the strongest peak of the circular autocorrelation of the base colour's luma (else the height's) at 64 x 64, away from the origin";
        row["threshold"] = qa.repetitionWarn;
        row["threshold_source"] = "materials.qa.repetition_warn";
        if (m.is_null()) return row;
        row["measured"] = m["peak"];
        if (m.value("peak", 0.0) > qa.repetitionWarn) {
            row["status"] = "WARN";
            row["limited_by"] = "the pattern repeats every " + m["period"].dump() + " (of 64 cells): tiled, the repeat shows.";
            row["fix_hint"] = "Turn on Stochastic Tiling on the material (stochasticTiling), or mix a larger-scale noise into the pattern.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json histogramRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("histogram", "content");
        row["method"] = "the base colour's luma texels at 0 or 255";
        row["threshold"] = qa.clippedPercent;
        row["threshold_source"] = "materials.qa.clipped_percent";
        if (m.is_null() || !m.contains("base_color")) return row;
        const Json& h = m["base_color"];
        const double clipped = h.value("clipped_low_percent", 0.0) + h.value("clipped_high_percent", 0.0);
        row["measured"] = {{"clipped_low_percent", h["clipped_low_percent"]}, {"clipped_high_percent", h["clipped_high_percent"]}};
        if (clipped > qa.clippedPercent) {
            row["status"] = "WARN";
            row["limited_by"] = "the base colour is clipped to black or white on " + std::to_string((int)std::lround(clipped)) + "% of its texels.";
            row["fix_hint"] = "Narrow the colour ramp or the levels feeding Base Color so no texel reaches 0 or 255.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json baseColorRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("base_color", "content");
        row["method"] = "the non-metal texels (metallic under 0.5) whose sRGB luma is outside 30..240, the PBR range of real dielectrics";
        row["threshold"] = qa.dielectricOutsidePercent;
        row["threshold_source"] = "materials.qa.dielectric_outside_percent";
        if (m.is_null()) return row;
        row["measured"] = m["dielectric_outside_percent"];
        if (m.value("dielectric_outside_percent", 0.0) > qa.dielectricOutsidePercent) {
            row["status"] = "WARN";
            row["limited_by"] = "dielectric base colour outside sRGB 30..240 on " + m["dielectric_outside_percent"].dump() + "% of the non-metal texels.";
            row["fix_hint"] = "Keep a non-metal's albedo between sRGB 30 (charcoal) and 240 (fresh snow); light and shadow come from the lighting.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json normalRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("normal", "content");
        row["method"] = "decoded normals: the mean error of their length, the share facing away (z < 0) and, with a height, the green against the height's slope";
        row["threshold"] = qa.normalLengthError;
        row["threshold_source"] = "materials.qa.normal_length_error";
        if (m.is_null()) return row;
        row["measured"] = {{"length_error", m["length_error"]}, {"z_negative_percent", m["z_negative_percent"]}};
        if (m.contains("green_slope_correlation")) row["measured"]["green_slope_correlation"] = m["green_slope_correlation"];
        if (m.value("length_error", 0.0) > qa.normalLengthError || m.value("z_negative_percent", 0.0) > 1.0) {
            row["status"] = "FAIL";
            row["limited_by"] = m.value("z_negative_percent", 0.0) > 1.0 ? "the normal points away from the surface on " + m["z_negative_percent"].dump() + "% of its texels."
                                                                         : "decoded normals are not unit length (mean error " + m["length_error"].dump() + ").";
            row["fix_hint"] = "Feed Normal from a normal-map node, or leave it unconnected: the bake then makes it from Height.";
        } else if (m.contains("green_slope_correlation") && m.value("green_slope_correlation", 0.0) < -0.2) {
            row["status"] = "WARN";
            row["limited_by"] = "the green channel runs against the height's slope: a DirectX (Y-down) normal map.";
            row["fix_hint"] = "Invert the green of the normal fed to Normal, or leave Normal unconnected so the bake makes it from Height.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json roughnessRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("roughness", "content");
        row["method"] = "the ORM's green: p99 - p1";
        row["threshold"] = qa.roughnessMinRange;
        row["threshold_source"] = "materials.qa.roughness_min_range";
        if (m.is_null()) return row;
        row["measured"] = m["range"];
        if (m.value("range", 0.0) < qa.roughnessMinRange) {
            row["status"] = "WARN";
            row["limited_by"] = "roughness is flat (" + m["p1"].dump() + " to " + m["p99"].dump() + "): the surface reads as plastic.";
            row["fix_hint"] = "Drive Roughness from the pattern (a mask or the height through a ramp) so worn and recessed parts differ.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json metallicRow(const Json& m, const ProcMatQaSettings& qa) {
        Json row = qa::row("metallic", "content");
        row["method"] = "the ORM's blue: the share of texels between 0.1 and 0.9";
        row["threshold"] = qa.metallicMidPercent;
        row["threshold_source"] = "materials.qa.metallic_mid_percent";
        if (m.is_null()) return row;
        row["measured"] = m["mid_percent"];
        if (m.value("mid_percent", 0.0) > qa.metallicMidPercent) {
            row["status"] = "WARN";
            row["limited_by"] = m["mid_percent"].dump() + "% of the texels are neither metal nor dielectric.";
            row["fix_hint"] = "Make Metallic near 0 or 1 (a sharp threshold of a mask); blend only at the edges between metal and paint, rust or dirt.";
        } else {
            row["status"] = "PASS";
        }
        return row;
    }

    Json height8bitRow(const MaterialImages& images, const Json& histogram) {
        Json row = qa::row("height_8bit", "content");
        row["method"] = "a baked height is 8-bit: under parallax or displacement its 256 levels show as steps";
        row["threshold"] = 0;
        row["threshold_source"] = "a shader of the material samples the height";
        const OutputImage* height = find(images, "height");
        if (!height) return row;
        std::array<bool, 256> used{};
        for (size_t i = 0; i < texels(*height); i++) used[height->rgba[i * 4]] = true;
        const int levels = (int)std::count(used.begin(), used.end(), true);
        row["measured"] = {{"levels", levels}};
        if (histogram.contains("height")) row["measured"]["range"] = round3(histogram["height"].value("max", 0.0) - histogram["height"].value("min", 0.0));
        if (images.heightReader.empty()) {
            row["status"] = "PASS";
            row["limited_by"] = "no shader of the material samples the height.";
            return row;
        }
        row["status"] = "WARN";
        row["limited_by"] = images.heightReader + " samples the 8-bit height (" + std::to_string(levels) + " levels): its steps show under parallax.";
        row["fix_hint"] = "Keep the parallax depth small, or use the height only for masks: a 16-bit height is not baked (S90).";
        return row;
    }

    // dominant colours: the 8 most common of a 4-bit-per-channel quantization
    std::vector<std::array<double, 3>> palette(const OutputImage& image) {
        std::map<int, size_t> counts;
        for (size_t i = 0; i < texels(image); i++) {
            const uint8_t* p = &image.rgba[i * 4];
            counts[(p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4)]++;
        }
        std::vector<std::pair<size_t, int>> ranked;
        for (const auto& [key, count] : counts) ranked.push_back({count, key});
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        std::vector<std::array<double, 3>> out;
        for (size_t i = 0; i < ranked.size() && i < 8; i++) {
            const int key = ranked[i].second;
            const double srgb[3] = {((key >> 8 & 15) * 16 + 8) / 255.0, ((key >> 4 & 15) * 16 + 8) / 255.0, ((key & 15) * 16 + 8) / 255.0};
            double lin[3];
            for (int c = 0; c < 3; c++) lin[c] = srgb[c] <= 0.04045 ? srgb[c] / 12.92 : std::pow((srgb[c] + 0.055) / 1.055, 2.4);
            const double x = (0.4124 * lin[0] + 0.3576 * lin[1] + 0.1805 * lin[2]) / 0.95047;
            const double y = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
            const double z = (0.0193 * lin[0] + 0.1192 * lin[1] + 0.9505 * lin[2]) / 1.08883;
            const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
            out.push_back({116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))});
        }
        return out;
    }

    // the radially averaged power spectrum of the reduced luma, 32 bands, summing to 1
    std::vector<double> spectrum(const OutputImage& image) {
        const std::vector<double> field = reduced(values(image, -1), image.width, image.height);
        const int n = STAT_SIDE;
        std::vector<double> bands(n / 2, 0.0);
        std::vector<double> cosines((size_t)n), sines((size_t)n);
        for (int i = 0; i < n; i++) {
            cosines[(size_t)i] = std::cos(-2.0 * PI * i / n);
            sines[(size_t)i] = std::sin(-2.0 * PI * i / n);
        }
        for (int ky = 0; ky < n; ky++) {
            for (int kx = 0; kx < n; kx++) {
                const int fx = kx <= n / 2 ? kx : kx - n, fy = ky <= n / 2 ? ky : ky - n;
                const int band = (int)std::sqrt((double)(fx * fx + fy * fy));
                if (band == 0 || band >= n / 2) continue;
                double re = 0.0, im = 0.0;
                for (int y = 0; y < n; y++) {
                    for (int x = 0; x < n; x++) {
                        const size_t turn = (size_t)((kx * x + ky * y) % n);
                        re += field[(size_t)y * n + x] * cosines[turn];
                        im += field[(size_t)y * n + x] * sines[turn];
                    }
                }
                bands[(size_t)band] += re * re + im * im;
            }
        }
        double total = 0.0;
        for (double b : bands) total += b;
        if (total > 0.0) for (double& b : bands) b /= total;
        return bands;
    }

    // the share of texels whose luma Sobel magnitude passes 32 levels
    double edgeDensity(const OutputImage& image) {
        const std::vector<uint8_t> l = values(image, -1);
        const int w = image.width, h = image.height;
        size_t edges = 0;
        const auto at = [&](int x, int y) { return (double)l[(size_t)((y + h) % h) * w + (size_t)((x + w) % w)]; };
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const double gx = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1);
                const double gy = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1);
                if (std::sqrt(gx * gx + gy * gy) > 32.0 * 4.0) edges++;
            }
        }
        return (double)edges / (double)texels(image);
    }

    // a reference image at most STAT_SIDE * 4 on a side is plenty for these, and an imported photo is large
    OutputImage boxDown(const OutputImage& image, int side) {
        if (image.width <= side && image.height <= side) return image;
        OutputImage out;
        out.name = image.name;
        out.width = std::min(image.width, side);
        out.height = std::min(image.height, side);
        out.rgba.assign((size_t)out.width * out.height * 4, 0);
        for (int y = 0; y < out.height; y++) {
            for (int x = 0; x < out.width; x++) {
                const int x0 = x * image.width / out.width, x1 = std::max(x0 + 1, (x + 1) * image.width / out.width);
                const int y0 = y * image.height / out.height, y1 = std::max(y0 + 1, (y + 1) * image.height / out.height);
                for (int c = 0; c < 4; c++) {
                    double sum = 0.0;
                    for (int sy = y0; sy < y1; sy++) {
                        for (int sx = x0; sx < x1; sx++) sum += image.rgba[((size_t)sy * image.width + sx) * 4 + c];
                    }
                    out.rgba[((size_t)y * out.width + x) * 4 + c] = (uint8_t)std::lround(sum / ((x1 - x0) * (y1 - y0)));
                }
            }
        }
        return out;
    }

}

const std::vector<std::string>& ProceduralChecks::checkIds() {
    static const std::vector<std::string> ids = {"tileability", "repetition", "histogram", "base_color", "normal", "roughness", "metallic", "height_8bit"};
    return ids;
}

Json ProceduralChecks::metrics(const MaterialImages& images) {
    Json m = Json::object();
    const OutputImage* base = find(images, "base_color");
    const OutputImage* normal = find(images, "normal");
    const OutputImage* orm = find(images, "orm");
    const OutputImage* height = find(images, "height");

    Json seams = Json::object();
    for (const auto& [name, image] : images.outputs) {
        if (!find(images, name.c_str())) continue;
        double x = 1.0, y = 1.0;
        texsynth::seamScores(asSynthImage(image), x, y);
        seams[name] = {{"seam_x", round3(x)}, {"seam_y", round3(y)}};
    }
    m["tileability"] = seams.empty() ? Json() : seams;

    const OutputImage* patterned = base ? base : height;
    if (patterned) {
        int atX = 0, atY = 0;
        const double peak = autocorrelationPeak(reduced(values(*patterned, base ? -1 : 0), patterned->width, patterned->height), atX, atY);
        m["repetition"] = {{"output", patterned == base ? "base_color" : "height"}, {"peak", round3(peak)}, {"period", {atX, atY}}};
    } else {
        m["repetition"] = nullptr;
    }

    Json histograms = Json::object();
    if (base) histograms["base_color"] = histogramJson(values(*base, -1));
    if (const OutputImage* emission = find(images, "emission")) histograms["emission"] = histogramJson(values(*emission, -1));
    if (orm) {
        histograms["occlusion"] = histogramJson(values(*orm, 0));
        histograms["roughness"] = histogramJson(values(*orm, 1));
        histograms["metallic"] = histogramJson(values(*orm, 2));
    }
    if (height) histograms["height"] = histogramJson(values(*height, 0));
    m["histogram"] = histograms.empty() ? Json() : histograms;

    if (base) {
        const bool ormFits = orm && orm->width == base->width && orm->height == base->height;
        size_t dielectric = 0, outside = 0;
        double sum = 0.0, low = 255.0, high = 0.0;
        for (size_t i = 0; i < texels(*base); i++) {
            const double l = luma(&base->rgba[i * 4]);
            sum += l;
            low = std::min(low, l);
            high = std::max(high, l);
            if (ormFits && orm->rgba[i * 4 + 2] >= 128) continue;
            dielectric++;
            if (l < 30.0 || l > 240.0) outside++;
        }
        m["base_color"] = {{"luma_min", (int)std::lround(low)}, {"luma_max", (int)std::lround(high)}, {"luma_mean", round1(sum / (double)texels(*base))},
                           {"dielectric_outside_percent", dielectric ? round1(100.0 * (double)outside / (double)dielectric) : 0.0},
                           {"metal_from", ormFits ? "orm" : "none: every texel counts as a dielectric"}};
    } else {
        m["base_color"] = nullptr;
    }

    if (normal) {
        double lengthError = 0.0, xy = 0.0, dot = 0.0, normalVar = 0.0, slopeVar = 0.0;
        size_t away = 0;
        const bool slope = height && height->width == normal->width && height->height == normal->height;
        const int w = normal->width, h = normal->height;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint8_t* p = &normal->rgba[((size_t)y * w + x) * 4];
                const double nx = p[0] / 127.5 - 1.0, ny = p[1] / 127.5 - 1.0, nz = p[2] / 127.5 - 1.0;
                lengthError += std::fabs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0);
                xy += std::sqrt(nx * nx + ny * ny);
                if (nz < 0.0) away++;
                if (slope) {
                    // rows go down the image, as v does: a surface rising toward the bottom faces up (+Y, green)
                    const double dv = (double)height->rgba[((size_t)((y + 1) % h) * w + x) * 4] - (double)height->rgba[((size_t)((y + h - 1) % h) * w + x) * 4];
                    dot += ny * dv;
                    normalVar += ny * ny;
                    slopeVar += dv * dv;
                }
            }
        }
        const double count = (double)texels(*normal);
        m["normal"] = {{"length_error", round3(lengthError / count)}, {"z_negative_percent", round1(100.0 * (double)away / count)}, {"mean_xy", round3(xy / count)}};
        if (slope && normalVar > 1e-9 && slopeVar > 1e-9) m["normal"]["green_slope_correlation"] = round3(dot / std::sqrt(normalVar * slopeVar));
    } else {
        m["normal"] = nullptr;
    }

    if (orm) {
        const Histogram r(values(*orm, 1));
        m["roughness"] = {{"p1", round3(r.percentile(0.01))}, {"p50", round3(r.percentile(0.5))}, {"p99", round3(r.percentile(0.99))},
                          {"range", round3(r.percentile(0.99) - r.percentile(0.01))}};
        size_t mid = 0;
        for (size_t i = 0; i < texels(*orm); i++) {
            const uint8_t v = orm->rgba[i * 4 + 2];
            if (v > 25 && v < 230) mid++;
        }
        m["metallic"] = {{"mid_percent", round1(100.0 * (double)mid / (double)texels(*orm))}};
    } else {
        m["roughness"] = nullptr;
        m["metallic"] = nullptr;
    }

    Json cost = {{"method", "an estimate: docs/textures.md's formats at Normal quality, mips included; not a cook"}, {"textures", 0}};
    Json perPlatform = {{"desktop", 0.0}, {"mobile", 0.0}, {"web", 0.0}};
    int textures = 0;
    for (const auto& [name, image] : images.outputs) {
        if (!find(images, name.c_str())) continue;
        textures++;
        for (const char* platform : {"desktop", "mobile", "web"}) {
            perPlatform[platform] = perPlatform[platform].get<double>() + cookedBytes(name, texels(image), platform);
        }
    }
    cost["textures"] = textures;
    for (const char* platform : {"desktop", "mobile", "web"}) cost[std::string(platform) + "_mb"] = round3(perPlatform[platform].get<double>() / (1024.0 * 1024.0));
    m["cost"] = cost;
    return m;
}

Json ProceduralChecks::rows(const Json& m, const MaterialImages& images, const ProcMatQaSettings& qa) {
    Json rows = Json::array();
    rows.push_back(tileabilityRow(m["tileability"], qa));
    rows.push_back(repetitionRow(m["repetition"], qa));
    rows.push_back(histogramRow(m["histogram"], qa));
    rows.push_back(baseColorRow(m["base_color"], qa));
    rows.push_back(normalRow(m["normal"], qa));
    rows.push_back(roughnessRow(m["roughness"], qa));
    rows.push_back(metallicRow(m["metallic"], qa));
    rows.push_back(height8bitRow(images, m["histogram"].is_object() ? m["histogram"] : Json::object()));
    return rows;
}

Json ProceduralChecks::thresholds(const ProcMatQaSettings& qa) {
    qa::ThresholdAudit audit;
    const ProcMatQaSettings defaults;
    for (const ProcMatQaField& field : procMatQaFields()) {
        if (field.real) audit.add(field.key, qa.*field.real, defaults.*field.real);
        else audit.add(field.key, qa.*field.whole, defaults.*field.whole);
    }
    return audit.json();
}

Json ProceduralChecks::reference(const MaterialImages& images, const OutputImage& referenceImage) {
    const OutputImage* own = find(images, "base_color");
    if (!own) {
        for (const auto& [name, image] : images.outputs) {
            if ((own = find(images, name.c_str()))) break;
        }
    }
    Json out = {{"advisory", "no status: a reference compares, it does not judge"}};
    if (!own || referenceImage.width <= 0 || referenceImage.height <= 0) return out;
    const OutputImage ref = boxDown(referenceImage, STAT_SIDE * 4);
    const OutputImage mine = boxDown(*own, STAT_SIDE * 4);
    const std::vector<std::array<double, 3>> a = palette(ref), b = palette(mine);
    double distance = 0.0;
    for (const auto& colour : a) {
        double best = 1e9;
        for (const auto& other : b) {
            best = std::min(best, std::sqrt((colour[0] - other[0]) * (colour[0] - other[0]) + (colour[1] - other[1]) * (colour[1] - other[1]) +
                                            (colour[2] - other[2]) * (colour[2] - other[2])));
        }
        distance += best;
    }
    const std::vector<double> sa = spectrum(ref), sb = spectrum(mine);
    double l1 = 0.0;
    for (size_t i = 0; i < sa.size(); i++) l1 += std::fabs(sa[i] - sb[i]);
    out["output"] = own->name;
    out["palette_delta_e"] = round1(a.empty() ? 0.0 : distance / (double)a.size());
    out["spectrum"] = round3(l1);
    out["edges"] = round3(std::fabs(edgeDensity(ref) - edgeDensity(mine)));
    return out;
}

Json ProceduralChecks::report(const MaterialImages& images, const CheckRequest& request) {
    Json out;
    out["metrics"] = metrics(images);
    Json all = rows(out["metrics"], images, request.qa);
    Json checks = Json::array();
    for (Json& row : all) {
        const std::string id = row.value("id", "");
        if (request.only.empty() || std::find(request.only.begin(), request.only.end(), id) != request.only.end()) checks.push_back(std::move(row));
    }
    const bool stop = qa::countRounds(checks, request.rounds, request.qa.fixRounds);
    out["checks"] = checks;
    out["verdict"] = qa::verdict(checks);
    out["thresholds"] = thresholds(request.qa);
    out["stop"] = stop;
    out["guidance"] = qa::guidance(stop, out["verdict"] == "FAIL", request.qa.fixRounds, "preview again and check again", CLOSING);
    return out;
}
