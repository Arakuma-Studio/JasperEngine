// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from the Adobe Photoshop File Formats Specification, "Adjustment layers" and the
// adjustment file formats (Levels, Curves, Hue/Saturation, Color Balance...).

#include "PsdAdjustments.h"

#include "PsColor.h"
#include "PsdBytes.h"
#include "PsdDescriptor.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::editor::psd;
using jasperengine::titleart::Json;

namespace {

    Json levelsChannel(ByteReader& r) {
        const double inBlack = r.u16(), inWhite = r.u16(), outBlack = r.u16(), outWhite = r.u16(), gamma = r.u16();
        return Json{{"in_black", inBlack}, {"in_white", inWhite}, {"gamma", std::clamp(gamma / 100.0, 0.1, 9.99)},
                    {"out_black", outBlack}, {"out_white", outWhite}};
    }

    Json curvePoints(ByteReader& r) {
        const uint16_t count = r.u16();
        Json points = Json::array();
        for (uint16_t i = 0; i < count && r.ok() && i < 64; i++) {
            const double output = r.u16(), input = r.u16();
            points.push_back(Json{{"x", std::clamp(input / 255.0, 0.0, 1.0)}, {"y", std::clamp(output / 255.0, 0.0, 1.0)}, {"corner", false}});
        }
        return Json{{"points", points}};
    }

    Json descriptorOf(const std::vector<uint8_t>& data, std::string& error) {
        ByteReader r(data.data(), data.size());
        Json d;
        if (!readVersionedDescriptor(r, d)) {
            error = r.failure();
            return Json();
        }
        return d;
    }

    Json gradientStops(ByteReader& r) {
        Json stops = Json::array();
        const uint16_t colors = r.u16();
        std::vector<std::pair<double, std::string>> c;
        std::vector<double> mids;
        for (uint16_t i = 0; i < colors && r.ok() && i < 32; i++) {
            const double location = r.u32() / 4096.0;
            const double midpoint = r.u32() / 100.0;
            const uint16_t space = r.u16();
            double v[4];
            for (double& x : v) x = r.u16();
            r.u16();
            std::string hex = "#000000";
            if (space == 0) hex = rgbHex(v[0] / 65535, v[1] / 65535, v[2] / 65535);
            else if (space == 8) hex = rgbHex(1 - v[0] / 10000, 1 - v[0] / 10000, 1 - v[0] / 10000);
            c.emplace_back(location, hex);
            mids.push_back(midpoint);
        }
        const uint16_t transparencies = r.u16();
        std::vector<std::pair<double, double>> alpha;
        for (uint16_t i = 0; i < transparencies && r.ok() && i < 32; i++) {
            const double location = r.u32() / 4096.0;
            r.u32();
            alpha.emplace_back(location, r.u16() / 100.0);
        }
        for (size_t i = 0; i < c.size(); i++) {
            double a = 1;
            if (!alpha.empty()) {
                a = alpha.front().second;
                for (size_t k = 0; k + 1 < alpha.size(); k++) {
                    if (c[i].first >= alpha[k].first && c[i].first <= alpha[k + 1].first && alpha[k + 1].first > alpha[k].first) {
                        const double t = (c[i].first - alpha[k].first) / (alpha[k + 1].first - alpha[k].first);
                        a = alpha[k].second + (alpha[k + 1].second - alpha[k].second) * t;
                    }
                }
                if (c[i].first > alpha.back().first) a = alpha.back().second;
            }
            const double mid = i + 1 < c.size() ? std::clamp(mids[i + 1], 0.05, 0.95) : 0.5;
            stops.push_back(Json{{"pos", std::clamp(c[i].first, 0.0, 1.0)}, {"color", c[i].second}, {"alpha", std::clamp(a, 0.0, 1.0)}, {"mid", mid}});
        }
        if (stops.size() < 2) {
            stops = Json::array({Json{{"pos", 0}, {"color", "#000000"}, {"alpha", 1}, {"mid", 0.5}}, Json{{"pos", 1}, {"color", "#ffffff"}, {"alpha", 1}, {"mid", 0.5}}});
        }
        return Json{{"stops", stops}, {"interp", "rgb"}};
    }

}

bool jasperengine::editor::psd::psdAdjustmentToTitle(const std::string& key, const std::vector<uint8_t>& data, Json& out, std::string& error) {
    ByteReader r(data.data(), data.size());
    if (key == "levl") {
        r.u16();
        Json channels = Json::object();
        const char* names[] = {"rgb", "r", "g", "b"};
        for (const char* n : names) channels[n] = levelsChannel(r);
        out = Json{{"type", "levels"}, {"channels", channels}};
    } else if (key == "curv") {
        r.u8();
        r.u16();
        const uint32_t present = r.u32();
        Json channels = Json::object();
        const char* names[] = {"rgb", "r", "g", "b"};
        for (int bit = 0; bit < 32 && r.ok(); bit++) {
            if (!(present & (1u << bit))) continue;
            const Json curve = curvePoints(r);
            if (bit < 4 && curve["points"].size() >= 2) channels[names[bit]] = curve;
        }
        out = Json{{"type", "curves"}, {"channels", channels}};
    } else if (key == "hue2" || key == "hue ") {
        r.u16();
        const bool colorize = r.u8() != 0;
        r.u8();
        const double ch = r.i16(), cs = r.i16(), cl = r.i16();
        const double mh = r.i16(), ms = r.i16(), ml = r.i16();
        out = Json{{"type", "hue_saturation"}, {"hue", colorize ? ch : mh}, {"saturation", colorize ? cs : ms},
                   {"lightness", colorize ? cl : ml}, {"colorize", colorize}};
    } else if (key == "brit") {
        const double b = r.i16(), c = r.i16();
        out = Json{{"type", "brightness_contrast"}, {"brightness", b}, {"contrast", c}};
    } else if (key == "CgEd") {
        const Json d = descriptorOf(data, error);
        if (d.is_null()) return false;
        out = Json{{"type", "brightness_contrast"}, {"brightness", descNumber(d, "Brgh", 0)}, {"contrast", descNumber(d, "Cntr", 0)}};
    } else if (key == "expA") {
        r.u16();
        const double exposure = r.f32(), offset = r.f32(), gamma = r.f32();
        out = Json{{"type", "exposure"}, {"exposure", exposure}, {"offset", offset}, {"gamma", gamma}};
    } else if (key == "vibA") {
        const Json d = descriptorOf(data, error);
        if (d.is_null()) return false;
        out = Json{{"type", "vibrance"}, {"vibrance", descNumber(d, "vibrance", 0)}, {"saturation", descNumber(d, "Strt", 0)}};
    } else if (key == "blnc") {
        Json o = Json{{"type", "color_balance"}};
        for (const char* range : {"shadows", "midtones", "highlights"}) {
            const double cr = r.i16(), mg = r.i16(), yb = r.i16();
            o[range] = Json{{"cyan_red", cr}, {"magenta_green", mg}, {"yellow_blue", yb}};
        }
        o["preserve_luminosity"] = r.remaining() ? r.u8() != 0 : true;
        out = o;
    } else if (key == "blwh") {
        const Json d = descriptorOf(data, error);
        if (d.is_null()) return false;
        out = Json{{"type", "black_white"}, {"reds", descNumber(d, "Rd  ", 40)}, {"yellows", descNumber(d, "Yllw", 60)},
                   {"greens", descNumber(d, "Grn ", 40)}, {"cyans", descNumber(d, "Cyn ", 60)}, {"blues", descNumber(d, "Bl  ", 20)},
                   {"magentas", descNumber(d, "Mgnt", 80)}, {"tint", descBool(d, "useTint", false)}};
        if (const Json* tint = descObject(d, "tintColor")) out["tint_color"] = psColorHex(*tint, "#e1d3b3");
    } else if (key == "phfl") {
        const uint16_t version = r.u16();
        std::string hex = "#ec8a00";
        if (version == 3) {
            // CIE XYZ as 32-bit fixed values / 65536 -> sRGB (D65 matrix, as Photoshop's own swatches)
            const double X = r.i32() / 65536.0 / 100, Y = r.i32() / 65536.0 / 100, Z = r.i32() / 65536.0 / 100;
            const double lin[3] = {3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z, 0.0557 * X - 0.2040 * Y + 1.0570 * Z};
            double c[3];
            for (int i = 0; i < 3; i++) c[i] = lin[i] <= 0.0031308 ? 12.92 * lin[i] : 1.055 * std::pow(std::max(0.0, lin[i]), 1 / 2.4) - 0.055;
            hex = rgbHex(c[0], c[1], c[2]);
        } else {
            const uint16_t space = r.u16();
            double v[4];
            for (double& x : v) x = r.u16();
            if (space == 0) hex = rgbHex(v[0] / 65535, v[1] / 65535, v[2] / 65535);
        }
        const double density = r.u32();
        const bool preserve = r.u8() != 0;
        out = Json{{"type", "photo_filter"}, {"color", hex}, {"density", std::clamp(density, 0.0, 100.0)}, {"preserve_luminosity", preserve}};
    } else if (key == "grdm") {
        r.u16();
        const bool reverse = r.u8() != 0;
        r.u8();
        r.unicode();
        out = Json{{"type", "gradient_map"}, {"gradient", gradientStops(r)}, {"reverse", reverse}};
    } else if (key == "nvrt") {
        out = Json{{"type", "invert"}};
    } else if (key == "post") {
        out = Json{{"type", "posterize"}, {"levels", std::clamp<int>(r.u16(), 2, 255)}};
    } else if (key == "thrs") {
        out = Json{{"type", "threshold"}, {"level", std::clamp<int>(r.u16(), 1, 255)}};
    } else {
        error = "adjustment '" + key + "' has no Title Art equivalent";
        return false;
    }
    if (!r.ok()) {
        error = "adjustment '" + key + "': " + r.failure();
        return false;
    }
    return true;
}

bool jasperengine::editor::psd::titleAdjustmentToPsd(const Json& adjustment, std::string& key, std::vector<uint8_t>& data) {
    ByteWriter w;
    const std::string type = adjustment.value("type", std::string());
    const auto num = [](const Json& o, const char* k, double fallback) {
        auto it = o.find(k);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    };
    if (type == "levels") {
        key = "levl";
        w.u16(2);
        const Json channels = adjustment.value("channels", Json::object());
        for (int i = 0; i < 29; i++) {
            const char* names[] = {"rgb", "r", "g", "b"};
            const Json c = i < 4 ? channels.value(names[i], Json::object()) : Json::object();
            w.u16((uint16_t)std::lround(num(c, "in_black", 0)));
            w.u16((uint16_t)std::lround(num(c, "in_white", 255)));
            w.u16((uint16_t)std::lround(num(c, "out_black", 0)));
            w.u16((uint16_t)std::lround(num(c, "out_white", 255)));
            w.u16((uint16_t)std::lround(num(c, "gamma", 1) * 100));
        }
    } else if (type == "curves") {
        // Photoshop curves are smooth, 2..16 points with rising inputs (Pachislop psd.ts psCurve);
        // anything else is written baked
        const Json channels = adjustment.value("channels", Json::object());
        const char* names[] = {"rgb", "r", "g", "b"};
        std::vector<std::vector<std::pair<int, int>>> curves(4);
        uint32_t present = 0;
        for (int i = 0; i < 4; i++) {
            if (!channels.contains(names[i])) continue;
            const Json points = channels[names[i]].value("points", Json::array());
            if (points.size() < 2 || points.size() > 16) return false;
            for (size_t k = 0; k < points.size(); k++) {
                if (k > 0 && k + 1 < points.size() && points[k].value("corner", false)) return false;
                const int input = (int)std::lround(std::clamp(num(points[k], "x", 0), 0.0, 1.0) * 255);
                const int output = (int)std::lround(std::clamp(num(points[k], "y", 0), 0.0, 1.0) * 255);
                if (!curves[i].empty() && input <= curves[i].back().first) return false;
                curves[i].push_back({input, output});
            }
            const bool identity = curves[i].size() == 2 && curves[i][0] == std::make_pair(0, 0) && curves[i][1] == std::make_pair(255, 255);
            // unchanged colour channels are left out, as Photoshop stores them
            if (i > 0 && identity) continue;
            present |= 1u << i;
        }
        key = "curv";
        w.u8(0);
        w.u16(1);
        w.u32(present);
        for (int i = 0; i < 4; i++) {
            if (!(present & (1u << i))) continue;
            w.u16((uint16_t)curves[i].size());
            for (const auto& p : curves[i]) {
                w.u16((uint16_t)p.second);
                w.u16((uint16_t)p.first);
            }
        }
    } else if (type == "hue_saturation") {
        key = "hue2";
        const bool colorize = adjustment.value("colorize", false);
        int16_t h = (int16_t)std::lround(num(adjustment, "hue", 0)), s = (int16_t)std::lround(num(adjustment, "saturation", 0));
        const int16_t l = (int16_t)std::lround(num(adjustment, "lightness", 0));
        // colorize: Photoshop's hue 0..360 and saturation 0..100 from the same fields
        if (colorize) {
            h = (int16_t)((h % 360 + 360) % 360);
            s = std::clamp<int16_t>(s, 0, 100);
        }
        w.u16(2);
        w.u8(colorize ? 1 : 0);
        w.u8(0);
        w.i16(colorize ? h : 0);
        w.i16(colorize ? s : 25);
        w.i16(colorize ? l : 0);
        w.i16(colorize ? 0 : h);
        w.i16(colorize ? 0 : s);
        w.i16(colorize ? 0 : l);
        // the six hue ranges, untouched (their default bounds, no change)
        const int16_t ranges[6][4] = {{315, 345, 15, 45}, {15, 45, 75, 105}, {75, 105, 135, 165},
                                      {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345}};
        for (const auto& range : ranges) {
            for (int16_t v : range) w.i16(v);
            w.i16(0);
            w.i16(0);
            w.i16(0);
        }
    } else if (type == "brightness_contrast") {
        // the modern algorithm and ranges (-150..150, -50..100): CgEd with useLegacy false
        key = "CgEd";
        w.u32(16);
        DescriptorWriter d(w);
        d.begin("null", 7);
        d.longItem("Vrsn", 1);
        d.longItem("Brgh", (int32_t)std::lround(num(adjustment, "brightness", 0)));
        d.longItem("Cntr", (int32_t)std::lround(num(adjustment, "contrast", 0)));
        d.longItem("means", 127);
        d.boolean("Lab ", false);
        d.boolean("useLegacy", false);
        d.boolean("Auto", false);
    } else if (type == "invert") {
        key = "nvrt";
    } else if (type == "posterize") {
        key = "post";
        w.u16((uint16_t)std::clamp<long>(std::lround(num(adjustment, "levels", 4)), 2, 255));
        w.u16(0);
    } else if (type == "threshold") {
        key = "thrs";
        w.u16((uint16_t)std::clamp<long>(std::lround(num(adjustment, "level", 128)), 1, 255));
        w.u16(0);
    } else {
        return false;
    }
    data = std::move(w.out);
    return true;
}
