// Exact production TextShaper before/after, linked to real bundled dependencies.
#include "TextShaper.h"
#include "BaselineTextShaper.h"
#include "FontFace.h"
#include "DefaultFont.h"
#include "DefaultFontArabic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
namespace {
std::vector<const FontFace*> faces;
std::vector<float> scales;
volatile float sink = 0.0f;
size_t checks = 0;

void compare(const std::vector<uint32_t>& points, size_t offset, size_t length, ShapeOptions options) {
    ShapedGlyph sentinel{0, true, 123, 1, 2, 3, 4, 5};
    std::vector<ShapedGlyph> before{sentinel}, after{sentinel};
    BaselineTextShaper::shapeRange(faces, scales, points, offset, length, 7, options, before);
    TextShaper::shapeRange(faces, scales, points, offset, length, 7, options, after);
    if (before.size() != after.size()) throw std::runtime_error("glyph count mismatch");
    for (size_t i = 0; i < before.size(); ++i) {
        const auto& a = before[i]; const auto& b = after[i];
        if (a.face != b.face || a.rtl != b.rtl || a.glyphIndex != b.glyphIndex || a.cluster != b.cluster ||
            a.xOffset != b.xOffset || a.yOffset != b.yOffset || a.xAdvance != b.xAdvance || a.line != b.line)
            throw std::runtime_error("glyph mismatch at " + std::to_string(i));
    }
    ++checks;
}

std::vector<uint32_t> decode(const std::string& text) {
    std::vector<uint32_t> points;
    TextShaper::decodeUtf8(text.data(), text.size(), points);
    return points;
}

void verify() {
    // Invalid ranges are rejected before constructing iterators. Overflowed
    // bounds were undefined in the baseline, so check that contract directly.
    std::vector<ShapedGlyph> invalid;
    for (const auto range : {std::pair<size_t, size_t>{std::numeric_limits<size_t>::max(), 2},
                            {1, std::numeric_limits<size_t>::max()}, {4, 1}}) {
        TextShaper::shapeRange(faces, scales, {65, 66, 67}, range.first, range.second, 0, ShapeOptions(), invalid);
        if (!invalid.empty()) throw std::runtime_error("invalid range appended glyphs");
    }
    for (int direction : {0, 1, 2, 3}) {
        ShapeOptions options;
        options.baseDirection = direction;
        compare({}, 0, 0, options);
        for (uint32_t a = 0x20; a <= 0x7e; ++a) {
            compare({a}, 0, 1, options);
            for (uint32_t b = 0x20; b <= 0x7e; ++b)
                compare({a, b}, 0, 2, options);
        }
        // Representative UAX #9 classes: L, EN, ES, ET, CS, ON, WS.
        const std::string classes = "aZ09+-#$%,.:()[] ";
        for (uint32_t a : classes)
            for (uint32_t b : classes)
                for (uint32_t c : classes)
                    compare({a, b, c}, 0, 3, options);

        for (const std::string text : {"office AV To fi ffi", "  123,456.78 ($90) ",
                "Hello العربية 123", "العربية (123) hello", "עברית 123 abc", "A\nB\tC\rD",
                "á 👩‍👩‍👧‍👦 한글", "a\xE2\x80\xAE" "b123", "\xF0\x28\x8C\x28", ""}) {
            const auto points = decode(text);
            for (bool kern : {false, true}) {
                options.kerning = kern;
                for (float spacing : {0.0f, 1.25f}) {
                    options.letterSpacing = spacing;
                    options.wordSpacing = spacing * 2;
                    options.language = "en";
                    compare(points, 0, points.size(), options);
                    if (points.size() > 3) compare(points, 1, points.size() - 2, options);
                    options.justifyAdvance.assign(points.size(), 0.5f);
                    options.justifyOffset.assign(points.size(), 0.25f);
                    compare(points, 0, points.size(), options);
                    options.justifyAdvance.clear(); options.justifyOffset.clear();
                }
            }
        }
    }
    std::mt19937 generator(123456);
    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<uint32_t> points(1 + generator() % 128);
        for (auto& c : points) c = 0x20 + generator() % 95;
        ShapeOptions options;
        options.baseDirection = trial % 3;
        options.kerning = trial % 2;
        options.letterSpacing = trial % 4 == 0 ? 0.7f : 0;
        compare(points, 0, points.size(), options);
        points.insert(points.begin(), 0x627); points.push_back(0x644);
        compare(points, 1, points.size() - 2, options);
    }
    for (uint32_t prefix : {0x627u, 0x202Eu, 0x2067u, 0x5D0u}) {
        auto points = decode(" (123) abc +45.6 ");
        points.insert(points.begin(), prefix);
        points.push_back(0x2069);
        for (int direction : {0, 1, 2}) {
            ShapeOptions options;
            options.baseDirection = direction;
            compare(points, 1, points.size() - 2, options);
        }
    }
    // ASCII falls back and splits font items when the Arabic font leads.
    std::reverse(faces.begin(), faces.end());
    std::reverse(scales.begin(), scales.end());
    for (const char* text : {" office AV fi", "123,456.78 ($90)", "The quick brown fox."}) {
        auto points = decode(text);
        for (int direction : {0, 1, 2}) {
            ShapeOptions options;
            options.baseDirection = direction;
            compare(points, 0, points.size(), options);
        }
    }
    std::reverse(faces.begin(), faces.end());
    std::reverse(scales.begin(), scales.end());
    std::printf("Behavior checks passed: %zu exact glyph-vector comparisons\n", checks);
}

double cpuNs() {
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time)) std::abort();
    return double(time.tv_sec) * 1e9 + double(time.tv_nsec);
}

double run(bool baseline, const std::vector<uint32_t>& points, const ShapeOptions& options, int iterations) {
    std::vector<ShapedGlyph> glyphs;
    const double start = cpuNs();
    for (int i = 0; i < iterations; ++i) {
        glyphs.clear();
        if (baseline) BaselineTextShaper::shapeRange(faces, scales, points, 0, points.size(), 0, options, glyphs);
        else TextShaper::shapeRange(faces, scales, points, 0, points.size(), 0, options, glyphs);
        sink = glyphs.empty() ? 0.0f : glyphs.back().xAdvance;
    }
    return (cpuNs() - start) / iterations;
}

void benchmark(const char* name, const std::string& text, int direction = 0) {
    const auto points = decode(text);
    ShapeOptions options;
    options.baseDirection = direction;
    run(true, points, options, 100); run(false, points, options, 100);
    const int iterations = std::max(100, 150000 / int(std::max(size_t(1), points.size())));
    for (int sample = 0; sample < 11; ++sample) {
        double before, after;
        if (sample % 2) { after = run(false, points, options, iterations); before = run(true, points, options, iterations); }
        else { before = run(true, points, options, iterations); after = run(false, points, options, iterations); }
        std::printf("%s,%d,%.3f,%.3f,%.6f\n", name, sample, before, after, after / before);
    }
}
}

int main() {
    std::string error;
    auto latin = FontFace::loadMemory(roboto_regular_ttf, roboto_regular_ttf_len, 0, false, "Roboto", &error);
    auto arabic = FontFace::loadMemory(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, 0, false, "Arabic", &error);
    if (!latin || !arabic) throw std::runtime_error(error);
    faces = {latin.get(), arabic.get()};
    scales = {latin->scaleForEmSize(16), arabic->scaleForEmSize(16)};
    verify();
#ifndef TEXT_SHAPER_CHECK_ONLY
    benchmark("ascii-label", "Inspector");
    benchmark("ascii-numbers", "123,456.78 ($90)");
    std::string paragraph;
    for (int i = 0; i < 20; ++i) paragraph += "The quick brown fox jumps over the lazy dog. ";
    benchmark("ascii-paragraph", paragraph);
    benchmark("arabic-mixed", "Hello العربية 123");
    benchmark("ascii-rtl-context", "Inspector 123", 2);
    benchmark("non-ascii-at-end", paragraph + "é");
#endif
}
