// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/psStyle.ts (colorHex, mixHex) by SafEight, used with the author's
// permission.

#include "PsColor.h"

#include "PsdDescriptor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine::editor::psd;
using jasperengine::titleart::Json;

namespace {

    double clamp01(double v) {
        return std::min(1.0, std::max(0.0, v));
    }

}

std::string jasperengine::editor::psd::rgbHex(double r, double g, double b) {
    char out[8];
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", (int)std::lround(clamp01(r) * 255), (int)std::lround(clamp01(g) * 255),
                  (int)std::lround(clamp01(b) * 255));
    return out;
}

void jasperengine::editor::psd::hexRgb(const std::string& hex, double& r, double& g, double& b) {
    r = g = b = 0;
    if (hex.size() != 7 || hex[0] != '#') return;
    const long v = std::strtol(hex.c_str() + 1, nullptr, 16);
    r = ((v >> 16) & 255) / 255.0;
    g = ((v >> 8) & 255) / 255.0;
    b = (v & 255) / 255.0;
}

std::string jasperengine::editor::psd::mixHex(const std::string& a, const std::string& b, double t) {
    double ar, ag, ab, br, bg, bb;
    hexRgb(a, ar, ag, ab);
    hexRgb(b, br, bg, bb);
    return rgbHex(ar + (br - ar) * t, ag + (bg - ag) * t, ab + (bb - ab) * t);
}

std::string jasperengine::editor::psd::psColorHex(const Json& c, const std::string& fallback) {
    if (!c.is_object()) return fallback;
    if (c.contains("redFloat")) return rgbHex(descNumber(c, "redFloat", 0), descNumber(c, "greenFloat", 0), descNumber(c, "blueFloat", 0));
    const std::string cls = c.value("_class", std::string());
    if (cls == "RGBC" || c.contains("Rd  ")) {
        return rgbHex(descNumber(c, "Rd  ", 0) / 255, descNumber(c, "Grn ", 0) / 255, descNumber(c, "Bl  ", 0) / 255);
    }
    if (cls == "HSBC" || c.contains("H   ")) {
        const double h = descNumber(c, "H   ", 0), s = descNumber(c, "Strt", 0) / 100, v = descNumber(c, "Brgh", 0) / 100;
        const auto f = [&](double n) {
            const double k = std::fmod(n + h / 60, 6);
            return v - v * s * std::max(0.0, std::min({k, 4 - k, 1.0}));
        };
        return rgbHex(f(5), f(3), f(1));
    }
    if (cls == "CMYC" || c.contains("Cyn ")) {
        const double k = 1 - descNumber(c, "Blck", 0) / 100;
        return rgbHex((1 - descNumber(c, "Cyn ", 0) / 100) * k, (1 - descNumber(c, "Mgnt", 0) / 100) * k, (1 - descNumber(c, "Ylw ", 0) / 100) * k);
    }
    if (cls == "LbCl" || c.contains("Lmnc")) {
        // Lab (D50) -> XYZ -> linear sRGB (Bradford-adapted matrix) -> sRGB
        const double fy = (descNumber(c, "Lmnc", 0) + 16) / 116;
        const double fx = fy + descNumber(c, "A   ", 0) / 500;
        const double fz = fy - descNumber(c, "B   ", 0) / 200;
        const auto inv = [](double t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116) / 7.787; };
        const double X = 0.9642 * inv(fx), Y = inv(fy), Z = 0.8249 * inv(fz);
        const double lin[3] = {3.1339 * X - 1.6169 * Y - 0.4906 * Z, -0.9788 * X + 1.9161 * Y + 0.0335 * Z, 0.0719 * X - 0.229 * Y + 1.4052 * Z};
        double out[3];
        for (int i = 0; i < 3; i++) out[i] = lin[i] <= 0.0031308 ? 12.92 * lin[i] : 1.055 * std::pow(std::max(0.0, lin[i]), 1 / 2.4) - 0.055;
        return rgbHex(out[0], out[1], out[2]);
    }
    if (cls == "Grsc" || c.contains("Gry ")) {
        const double g = 1 - descNumber(c, "Gry ", 0) / 100;
        return rgbHex(g, g, g);
    }
    return fallback;
}
