// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsPaint.h"

#include "SpriteMedia.h"
#include "SpriteNormalsField.h"
#include "SpriteNormalsRecipe.h"
#include "texsynth/SynthesisRecipe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include "util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::spritenormals {

    namespace {

        constexpr double kPi = 3.14159265358979323846;
        constexpr float kMinZ = 0.0175f;          // about 89 degrees: a blended normal never lies flatter

        bool transparent(const uint8_t* px, float alphaThreshold) { return px[3] / 255.0f <= alphaThreshold; }

        void setNeutral(uint8_t* p) {
            p[0] = 128;
            p[1] = 128;
            p[2] = 255;
        }

        uint8_t encodeByte(double v) {
            const double byte = std::round((v * 0.5 + 0.5) * 255.0);
            return static_cast<uint8_t>(byte <= 0.0 ? 0.0 : (byte >= 255.0 ? 255.0 : byte));
        }

        uint8_t coverage(double v) {
            const long rounded = std::lround(v);
            return static_cast<uint8_t>(std::clamp<long>(rounded, 0, 255));
        }

        // One composited pixel's RGB (OpenGL): generated `g`, overlay `p` (2.3 step 3)
        void compositePixel(const uint8_t* g, const uint8_t* p, uint8_t* out) {
            const int A = p[3];
            if (A == 0) {
                std::memcpy(out, g, 3);
                return;
            }
            if (A == 255) {
                std::memcpy(out, p, 3);
                return;
            }
            const double a = A / 255.0;
            double m[3];
            for (int k = 0; k < 3; k++) m[k] = (1.0 - a) * (g[k] / 127.5 - 1.0) + a * (p[k] / 127.5 - 1.0);
            double length = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            if (length < 1e-4) {
                std::memcpy(out, p, 3);
                return;
            }
            double n[3] = {m[0] / length, m[1] / length, m[2] / length};
            if (n[2] < kMinZ) {
                n[2] = kMinZ;
                length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (double& v : n) v /= length;
            }
            for (int k = 0; k < 3; k++) out[k] = encodeByte(n[k]);
        }

        std::string fieldOf(size_t i, const std::string& key = std::string()) {
            return "paint[" + std::to_string(i) + "]" + (key.empty() ? std::string() : "." + key);
        }

        void invalid(std::vector<Problem>& problems, const std::string& field, const std::string& message, const std::string& fix) {
            problems.push_back({"invalid_paint", field, message, fix});
        }

        std::string num(double v) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.4g", v);
            return buffer;
        }

        // ---- per-pixel cell ownership and islands

        std::vector<Cell> effectiveCells(const std::vector<Cell>& cells, int width, int height) {
            if (cells.empty()) return {Cell{0, 0, width, height}};
            return cells;
        }

        // The first cell holding each pixel, -1 for none
        std::vector<int> owners(const std::vector<Cell>& cells, int width, int height) {
            std::vector<int> owner(static_cast<size_t>(width) * height, cells.empty() ? 0 : -1);
            for (size_t c = 0; c < cells.size(); c++) {
                const Cell& cell = cells[c];
                for (int y = std::max(0, cell.y); y < std::min(height, cell.y + cell.height); y++) {
                    for (int x = std::max(0, cell.x); x < std::min(width, cell.x + cell.width); x++) {
                        int& o = owner[static_cast<size_t>(y) * width + x];
                        if (o < 0) o = static_cast<int>(c);
                    }
                }
            }
            return owner;
        }

        // 8-connected islands of opaque pixels within one owner; -1 for transparent pixels
        std::vector<int> islands(const Image& source, const std::vector<int>& owner, float alphaThreshold) {
            const int width = source.width, height = source.height;
            std::vector<int> label(owner.size(), -1);
            std::vector<int> queue;
            int next = 0;
            for (size_t start = 0; start < label.size(); start++) {
                if (label[start] >= 0 || transparent(source.data.data() + start * 4, alphaThreshold)) continue;
                queue.assign(1, static_cast<int>(start));
                label[start] = next;
                for (size_t q = 0; q < queue.size(); q++) {
                    const int i = queue[q];
                    const int x = i % width, y = i / width;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int nx = x + dx, ny = y + dy;
                            if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                            const size_t j = static_cast<size_t>(ny) * width + nx;
                            if (label[j] >= 0 || owner[j] != owner[static_cast<size_t>(i)] || transparent(source.data.data() + j * 4, alphaThreshold)) continue;
                            label[j] = next;
                            queue.push_back(static_cast<int>(j));
                        }
                    }
                }
                next++;
            }
            return label;
        }

        // ---- colours

        struct Lab {
            float L = 0.0f, a = 0.0f, b = 0.0f;
        };

        bool colorMatches(const uint8_t* px, const std::vector<std::array<uint8_t, 3>>& colors, const std::vector<Lab>& labs, float tolerance) {
            if (tolerance <= 0.0f) {
                for (const auto& c : colors) {
                    if (px[0] == c[0] && px[1] == c[1] && px[2] == c[2]) return true;
                }
                return false;
            }
            float L, a, b;
            detail::oklab(px[0], px[1], px[2], L, a, b);
            const float t2 = tolerance * tolerance;
            for (const Lab& c : labs) {
                const float dL = L - c.L, da = a - c.a, db = b - c.b;
                if (dL * dL + da * da + db * db <= t2) return true;
            }
            return false;
        }

        std::vector<Lab> labsOf(const std::vector<std::array<uint8_t, 3>>& colors) {
            std::vector<Lab> labs;
            for (const auto& c : colors) {
                Lab l;
                detail::oklab(c[0], c[1], c[2], l.L, l.a, l.b);
                labs.push_back(l);
            }
            return labs;
        }

        // ---- base64 (RFC 4648, standard alphabet, padded)

        const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string base64Encode(const std::vector<uint8_t>& bytes) {
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            size_t i = 0;
            for (; i + 2 < bytes.size(); i += 3) {
                const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
                out += kAlphabet[(v >> 18) & 63];
                out += kAlphabet[(v >> 12) & 63];
                out += kAlphabet[(v >> 6) & 63];
                out += kAlphabet[v & 63];
            }
            if (i < bytes.size()) {
                const bool two = i + 1 < bytes.size();
                const uint32_t v = (uint32_t(bytes[i]) << 16) | (two ? uint32_t(bytes[i + 1]) << 8 : 0u);
                out += kAlphabet[(v >> 18) & 63];
                out += kAlphabet[(v >> 12) & 63];
                out += two ? kAlphabet[(v >> 6) & 63] : '=';
                out += '=';
            }
            return out;
        }

        // Strict: a multiple of 4 characters of the alphabet, '=' only as the last one or two
        bool base64Decode(const std::string& text, std::vector<uint8_t>& out) {
            out.clear();
            if (text.size() % 4 != 0) return false;
            int8_t value[256];
            std::memset(value, -1, sizeof(value));
            for (int k = 0; k < 64; k++) value[static_cast<uint8_t>(kAlphabet[k])] = static_cast<int8_t>(k);
            out.reserve(text.size() / 4 * 3);
            for (size_t i = 0; i < text.size(); i += 4) {
                const bool last = i + 4 == text.size();
                int pad = 0;
                uint32_t v = 0;
                for (size_t k = 0; k < 4; k++) {
                    const uint8_t c = static_cast<uint8_t>(text[i + k]);
                    if (c == '=' && last && k >= 2) {
                        pad++;
                        v <<= 6;
                        continue;
                    }
                    if (pad > 0 || value[c] < 0) return false;
                    v = (v << 6) | static_cast<uint32_t>(value[c]);
                }
                out.push_back(static_cast<uint8_t>(v >> 16));
                if (pad < 2) out.push_back(static_cast<uint8_t>(v >> 8));
                if (pad < 1) out.push_back(static_cast<uint8_t>(v));
            }
            return true;
        }

        // ---- JSON readers

        bool isInteger(const json& v, int64_t lo, int64_t hi) {
            if (v.is_number_unsigned()) return v.get<uint64_t>() <= static_cast<uint64_t>(hi) && (lo <= 0 || v.get<uint64_t>() >= static_cast<uint64_t>(lo));
            if (v.is_number_integer()) return v.get<int64_t>() >= lo && v.get<int64_t>() <= hi;
            return false;
        }

        bool finiteNumber(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

        std::string dumped(const json& v) { return texsynth::jsonTextLossy(v); }

        const char* const kOpExample = R"({"op": "flatten", "where": {"colors": ["#ffffff"], "tolerance": 0.02}})";

        bool directionFromJson(const json& d, const std::string& field, std::array<float, 3>& out, std::vector<Problem>& problems) {
            const std::string fix = "[x, y, z] with z > 0 (OpenGL: +y is up the image), or {\"azimuth_deg\": 180, \"tilt_deg\": 45}";
            if (d.is_array()) {
                if (d.size() != 3 || !finiteNumber(d[0]) || !finiteNumber(d[1]) || !finiteNumber(d[2])) {
                    invalid(problems, field, field + " must be [x, y, z] numbers (got " + dumped(d) + ")", fix);
                    return false;
                }
                std::string error;
                if (!normaliseDirection({d[0].get<float>(), d[1].get<float>(), d[2].get<float>()}, out, error)) {
                    invalid(problems, field, field + " " + error + " (got " + dumped(d) + ")", fix);
                    return false;
                }
                return true;
            }
            if (!d.is_object()) {
                invalid(problems, field, field + " must be [x, y, z] or {azimuth_deg, tilt_deg} (got " + dumped(d) + ")", fix);
                return false;
            }
            for (auto it = d.begin(); it != d.end(); ++it) {
                if (it.key() != "azimuth_deg" && it.key() != "tilt_deg") {
                    invalid(problems, field, field + " has an unknown key \"" + texsynth::printableUtf8(it.key()) + "\"", "valid keys: azimuth_deg, tilt_deg");
                    return false;
                }
            }
            if (!d.contains("azimuth_deg") || !finiteNumber(d["azimuth_deg"])) {
                invalid(problems, field + ".azimuth_deg",
                        field + ".azimuth_deg must be a number of degrees (0 right, 90 up, 180 left, 270 down) (got " +
                            (d.contains("azimuth_deg") ? dumped(d["azimuth_deg"]) : std::string("nothing")) + ")",
                        fix);
                return false;
            }
            if (!d.contains("tilt_deg") || !finiteNumber(d["tilt_deg"]) || d["tilt_deg"].get<double>() < 0.0 || d["tilt_deg"].get<double>() > kMaxPaintTilt) {
                invalid(problems, field + ".tilt_deg",
                        field + ".tilt_deg must be between 0 (facing the viewer) and 85 degrees (got " +
                            (d.contains("tilt_deg") ? dumped(d["tilt_deg"]) : std::string("nothing")) + ")",
                        "e.g. 45");
                return false;
            }
            out = directionFromAngles(d["azimuth_deg"].get<float>(), d["tilt_deg"].get<float>());
            return true;
        }

        bool whereFromJson(const json& w, const std::string& field, PaintWhere& where, std::vector<Problem>& problems) {
            const std::string fix = "where: {rect: [x, y, w, h], pixels: [[x, y], ...], colors: [\"#rrggbb\"], tolerance, frame} (members intersect)";
            if (!w.is_object()) {
                invalid(problems, field, field + " must be an object (got " + dumped(w) + ")", fix);
                return false;
            }
            for (auto it = w.begin(); it != w.end(); ++it) {
                const std::string& key = it.key();
                if (key != "rect" && key != "pixels" && key != "colors" && key != "tolerance" && key != "frame") {
                    invalid(problems, field + "." + texsynth::printableUtf8(key), field + " has an unknown key \"" + texsynth::printableUtf8(key) + "\"",
                            "valid keys: rect, pixels, colors, tolerance, frame");
                    return false;
                }
            }
            if (w.contains("rect")) {
                const json& r = w["rect"];
                bool ok = r.is_array() && r.size() == 4;
                for (size_t k = 0; ok && k < 4; k++) ok = isInteger(r[k], k < 2 ? -1000000 : 1, 1000000);
                if (!ok) {
                    invalid(problems, field + ".rect", field + ".rect must be [x, y, w, h] in pixels with w and h >= 1 (got " + dumped(r) + ")",
                            "e.g. [10, 12, 4, 6]");
                    return false;
                }
                where.rect = std::array<int, 4>{r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>()};
            }
            if (w.contains("pixels")) {
                const json& p = w["pixels"];
                if (!p.is_array() || p.empty() || p.size() > static_cast<size_t>(kMaxPaintPixels)) {
                    invalid(problems, field + ".pixels",
                            field + ".pixels must be a list of 1 to " + std::to_string(kMaxPaintPixels) + " [x, y] pairs (got " +
                                (p.is_array() ? std::to_string(p.size()) + " entries" : dumped(p)) + ")",
                            "e.g. [[3, 4], [4, 4]]; a rect selects a block");
                    return false;
                }
                for (size_t k = 0; k < p.size(); k++) {
                    if (!p[k].is_array() || p[k].size() != 2 || !isInteger(p[k][0], -1000000, 1000000) || !isInteger(p[k][1], -1000000, 1000000)) {
                        invalid(problems, field + ".pixels[" + std::to_string(k) + "]",
                                field + ".pixels[" + std::to_string(k) + "] must be [x, y] (got " + dumped(p[k]) + ")", "e.g. [3, 4]");
                        return false;
                    }
                    where.pixels.push_back({p[k][0].get<int>(), p[k][1].get<int>()});
                }
            }
            if (w.contains("colors")) {
                const json& c = w["colors"];
                if (!c.is_array() || c.empty() || c.size() > static_cast<size_t>(kMaxPaintColors)) {
                    invalid(problems, field + ".colors",
                            field + ".colors must be a list of 1 to " + std::to_string(kMaxPaintColors) + " \"#rrggbb\" colours (got " +
                                (c.is_array() ? std::to_string(c.size()) + " entries" : dumped(c)) + ")",
                            "e.g. [\"#ffffff\"]");
                    return false;
                }
                for (size_t k = 0; k < c.size(); k++) {
                    uint32_t rgb = 0;
                    if (!c[k].is_string() || !parseColor(c[k].get<std::string>(), rgb)) {
                        invalid(problems, field + ".colors[" + std::to_string(k) + "]",
                                field + ".colors[" + std::to_string(k) + "] must be a colour \"#rrggbb\" (got " + dumped(c[k]) + ")", "e.g. \"#ffffff\"");
                        return false;
                    }
                    where.colors.push_back({static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)});
                }
            }
            if (w.contains("tolerance")) {
                const json& t = w["tolerance"];
                if (!finiteNumber(t) || t.get<double>() < 0.0 || t.get<double>() > 0.5) {
                    invalid(problems, field + ".tolerance", field + ".tolerance must be between 0 (exact) and 0.5 (Oklab distance) (got " + dumped(t) + ")",
                            "e.g. 0.02");
                    return false;
                }
                if (!w.contains("colors")) {
                    invalid(problems, field + ".tolerance", field + ".tolerance applies to colors, and there are none", "add colors, or leave tolerance out");
                    return false;
                }
                where.tolerance = t.get<float>();
            }
            if (w.contains("frame")) {
                const json& f = w["frame"];
                if (!isInteger(f, 0, 1000000)) {
                    invalid(problems, field + ".frame", field + ".frame must be a frame index >= 0 (got " + dumped(f) + ")", "frames count from 0, row-major");
                    return false;
                }
                where.frame = f.get<int>();
            }
            if (!where.rect && where.pixels.empty() && where.colors.empty() && !where.frame) {
                invalid(problems, field, field + " selects nothing: give at least one of rect, pixels, colors, frame", fix);
                return false;
            }
            return true;
        }

        bool opFromJson(const json& o, size_t i, PaintOp& op, std::vector<Problem>& problems) {
            const std::string at = fieldOf(i);
            if (!o.is_object()) {
                invalid(problems, at, at + " must be an operation object (got " + dumped(o) + ")", std::string("e.g. ") + kOpExample);
                return false;
            }
            for (auto it = o.begin(); it != o.end(); ++it) {
                const std::string& key = it.key();
                if (key != "op" && key != "where" && key != "direction" && key != "strength" && key != "radius") {
                    invalid(problems, at + "." + texsynth::printableUtf8(key), at + " has an unknown key \"" + texsynth::printableUtf8(key) + "\"",
                            "valid keys: op, where, direction, strength, radius");
                    return false;
                }
            }
            if (!o.contains("op") || !o["op"].is_string() || !parsePaintOpKind(o["op"].get<std::string>(), op.kind)) {
                invalid(problems, at + ".op",
                        at + ".op must be angle, flatten, smooth or erase (got " + (o.contains("op") ? dumped(o["op"]) : std::string("nothing")) + ")",
                        "angle: a direction; flatten: facing the viewer; smooth: blur the result; erase: remove paint");
                return false;
            }
            if (!o.contains("where")) {
                invalid(problems, at + ".where", at + ".where is required (the pixels to paint)", "where: {rect: [x, y, w, h]} or {colors: [\"#rrggbb\"]} or {frame: 0}");
                return false;
            }
            if (!whereFromJson(o["where"], at + ".where", op.where, problems)) return false;
            if (op.kind == PaintOpKind::Angle) {
                if (!o.contains("direction")) {
                    invalid(problems, at + ".direction", at + ".direction is required for angle",
                            "[x, y, z] with z > 0, or {\"azimuth_deg\": 180, \"tilt_deg\": 45}");
                    return false;
                }
                if (!directionFromJson(o["direction"], at + ".direction", op.direction, problems)) return false;
            } else if (o.contains("direction")) {
                invalid(problems, at + ".direction", at + ".direction applies to angle only (op is " + toString(op.kind) + ")", "leave direction out, or use op angle");
                return false;
            }
            if (o.contains("strength")) {
                const json& s = o["strength"];
                if (!finiteNumber(s) || s.get<double>() < 0.0 || s.get<double>() > 1.0) {
                    invalid(problems, at + ".strength", at + ".strength must be between 0 and 1 (got " + dumped(s) + ")", "1 paints fully, 0.5 blends half");
                    return false;
                }
                op.strength = s.get<float>();
            }
            if (o.contains("radius")) {
                const json& r = o["radius"];
                if (op.kind != PaintOpKind::Smooth) {
                    invalid(problems, at + ".radius", at + ".radius applies to smooth only (op is " + toString(op.kind) + ")", "leave radius out, or use op smooth");
                    return false;
                }
                if (!isInteger(r, 1, 4)) {
                    invalid(problems, at + ".radius", at + ".radius must be a whole number from 1 to 4 (got " + dumped(r) + ")", "1 averages 3x3 pixels, 2 averages 5x5");
                    return false;
                }
                op.radius = r.get<int>();
            }
            return true;
        }

        json roundedNumber(double v) {
            const double r = std::round(v * 1e6) / 1e6;
            if (r == std::floor(r) && std::abs(r) < 1e15) return static_cast<int64_t>(r);
            return r;
        }

        // ---- PNG chunks: the text after the paint keyword

        std::string paintSizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        // The chunk JSON's head checks shared by readPaint and readPaintSummary
        bool readHead(const json& j, PaintSummary& summary, std::string& problem) {
            if (!j.is_object()) {
                problem = std::string("the ") + kPaintKeyword + " chunk is not a JSON object";
                return false;
            }
            if (!j.contains("format") || !j["format"].is_string() || j["format"].get<std::string>() != kPaintFormat) {
                problem = std::string("the paint chunk's format is not \"") + kPaintFormat + "\"";
                return false;
            }
            if (!j.contains("version") || !isInteger(j["version"], 1, 1000000)) {
                problem = "the paint chunk has no valid version";
                return false;
            }
            if (j["version"].get<int64_t>() > kPaintVersion) {
                problem = "the paint chunk is version " + std::to_string(j["version"].get<int64_t>()) + ", newer than this editor reads (" +
                          std::to_string(kPaintVersion) + ")";
                return false;
            }
            const json size = j.contains("size") ? j["size"] : json();
            if (!size.is_array() || size.size() != 2 || !isInteger(size[0], 1, 65535) || !isInteger(size[1], 1, 65535) ||
                size[0].get<int64_t>() * size[1].get<int64_t>() > kMaxPixels) {
                problem = "the paint chunk's size is not [width, height] (got " + dumped(size) + ")";
                return false;
            }
            summary.width = size[0].get<int>();
            summary.height = size[1].get<int>();
            if (!j.contains("painted") || !isInteger(j["painted"], 0, kMaxPixels)) {
                problem = "the paint chunk's painted count is not a number of pixels";
                return false;
            }
            summary.painted = j["painted"].get<int>();
            return true;
        }

    }

    // ------------------------------------------------------------------ overlay

    PaintOverlay PaintOverlay::blank(int w, int h) {
        PaintOverlay o;
        if (w <= 0 || h <= 0) return o;
        o.width = w;
        o.height = h;
        o.rgba.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < o.rgba.size(); i += 4) {
            o.rgba[i] = 128;
            o.rgba[i + 1] = 128;
            o.rgba[i + 2] = 255;
            o.rgba[i + 3] = 0;
        }
        return o;
    }

    bool PaintOverlay::empty() const { return paintedCount() == 0; }

    size_t PaintOverlay::paintedCount() const {
        size_t n = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) n += rgba[i] > 0;
        return n;
    }

    bool PaintOverlay::sized(int w, int h) const {
        return width == w && height == h && w > 0 && h > 0 && rgba.size() == static_cast<size_t>(w) * h * 4;
    }

    void PaintOverlay::normalise() {
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            if (rgba[i + 3] == 0) setNeutral(&rgba[i]);
        }
    }

    // ------------------------------------------------------------------ names

    const char* toString(PaintOpKind kind) {
        switch (kind) {
            case PaintOpKind::Angle: return "angle";
            case PaintOpKind::Flatten: return "flatten";
            case PaintOpKind::Smooth: return "smooth";
            case PaintOpKind::Erase: return "erase";
        }
        return "flatten";
    }

    const char* toString(PaintMode mode) {
        switch (mode) {
            case PaintMode::Keep: return "keep";
            case PaintMode::Replace: return "replace";
            case PaintMode::Clear: return "clear";
        }
        return "keep";
    }

    bool parsePaintOpKind(const std::string& text, PaintOpKind& out) {
        for (PaintOpKind k : {PaintOpKind::Angle, PaintOpKind::Flatten, PaintOpKind::Smooth, PaintOpKind::Erase}) {
            if (text == toString(k)) {
                out = k;
                return true;
            }
        }
        return false;
    }

    bool parsePaintMode(const std::string& text, PaintMode& out) {
        for (PaintMode m : {PaintMode::Keep, PaintMode::Replace, PaintMode::Clear}) {
            if (text == toString(m)) {
                out = m;
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------ directions

    std::array<float, 3> directionFromAngles(float azimuthDeg, float tiltDeg) {
        const double az = std::fmod(static_cast<double>(azimuthDeg), 360.0) * kPi / 180.0;
        const double tilt = static_cast<double>(tiltDeg) * kPi / 180.0;
        // cos(90 degrees) is 6e-17, not 0: snapped, so 90 and 270 encode x as 128 alike
        auto snap = [](double v) { return static_cast<float>(std::abs(v) < 1e-6 ? 0.0 : v); };
        return {snap(std::sin(tilt) * std::cos(az)), snap(std::sin(tilt) * std::sin(az)), snap(std::cos(tilt))};
    }

    void anglesOf(const std::array<float, 3>& d, float& azimuthDeg, float& tiltDeg) {
        const double length = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
        if (length <= 0.0) {
            azimuthDeg = tiltDeg = 0.0f;
            return;
        }
        tiltDeg = static_cast<float>(std::acos(std::clamp(d[2] / length, -1.0, 1.0)) * 180.0 / kPi);
        double az = std::atan2(static_cast<double>(d[1]), static_cast<double>(d[0])) * 180.0 / kPi;
        if (az < 0.0) az += 360.0;
        if (az >= 360.0) az -= 360.0;
        azimuthDeg = std::hypot(d[0], d[1]) < 1e-7 ? 0.0f : static_cast<float>(az);
    }

    bool normaliseDirection(const std::array<float, 3>& in, std::array<float, 3>& out, std::string& error) {
        const double length = std::sqrt(double(in[0]) * in[0] + double(in[1]) * in[1] + double(in[2]) * in[2]);
        if (!std::isfinite(length) || length <= 0.0) {
            error = "has length 0";
            return false;
        }
        const std::array<float, 3> n = {static_cast<float>(in[0] / length), static_cast<float>(in[1] / length), static_cast<float>(in[2] / length)};
        if (!(n[2] > 0.0f)) {
            error = "must face the viewer (z > 0 after normalising)";
            return false;
        }
        out = n;
        return true;
    }

    std::array<uint8_t, 3> encodeDirection(const std::array<float, 3>& n) { return {encodeByte(n[0]), encodeByte(n[1]), encodeByte(n[2])}; }

    std::array<float, 3> decodeDirection(const uint8_t* rgb) {
        return {static_cast<float>(rgb[0] / 127.5 - 1.0), static_cast<float>(rgb[1] / 127.5 - 1.0), static_cast<float>(rgb[2] / 127.5 - 1.0)};
    }

    // ------------------------------------------------------------------ JSON

    bool paintOpsFromJson(const json& array, std::vector<PaintOp>& ops, std::vector<Problem>& problems) {
        ops.clear();
        const size_t before = problems.size();
        if (!array.is_array()) {
            invalid(problems, "paint", "paint must be an array of operations (got " + dumped(array) + ")", std::string("e.g. [") + kOpExample + "]");
            return false;
        }
        if (array.size() > static_cast<size_t>(kMaxPaintOps)) {
            invalid(problems, "paint", "paint has " + std::to_string(array.size()) + " operations; at most " + std::to_string(kMaxPaintOps) + " run in one call",
                    "split them over several calls (paint_mode keep adds to the paint already there)");
            return false;
        }
        for (size_t i = 0; i < array.size(); i++) {
            PaintOp op;
            if (opFromJson(array[i], i, op, problems)) ops.push_back(op);
        }
        if (problems.size() != before) {
            ops.clear();
            return false;
        }
        return true;
    }

    json paintOpsToJson(const std::vector<PaintOp>& ops) {
        json list = json::array();
        for (const PaintOp& op : ops) {
            json where = json::object();
            if (op.where.rect) where["rect"] = json::array({(*op.where.rect)[0], (*op.where.rect)[1], (*op.where.rect)[2], (*op.where.rect)[3]});
            if (!op.where.pixels.empty()) {
                json pixels = json::array();
                for (const auto& p : op.where.pixels) pixels.push_back(json::array({p[0], p[1]}));
                where["pixels"] = pixels;
            }
            if (!op.where.colors.empty()) {
                json colors = json::array();
                for (const auto& c : op.where.colors) colors.push_back(colorText((uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2]));
                where["colors"] = colors;
                where["tolerance"] = roundedNumber(op.where.tolerance);
            }
            if (op.where.frame) where["frame"] = *op.where.frame;
            json j = {{"op", toString(op.kind)}, {"where", where}, {"strength", roundedNumber(op.strength)}};
            if (op.kind == PaintOpKind::Angle) {
                j["direction"] = json::array({roundedNumber(op.direction[0]), roundedNumber(op.direction[1]), roundedNumber(op.direction[2])});
            }
            if (op.kind == PaintOpKind::Smooth) j["radius"] = op.radius;
            list.push_back(j);
        }
        return list;
    }

    // ------------------------------------------------------------------ validation and selection

    std::vector<Problem> validatePaintOps(const std::vector<PaintOp>& ops, int cellCount) {
        std::vector<Problem> problems;
        if (ops.size() > static_cast<size_t>(kMaxPaintOps)) {
            invalid(problems, "paint", "paint has " + std::to_string(ops.size()) + " operations; at most " + std::to_string(kMaxPaintOps) + " run in one call",
                    "split them over several calls (paint_mode keep adds to the paint already there)");
            return problems;
        }
        const int frames = cellCount < 0 ? -1 : std::max(1, cellCount);
        for (size_t i = 0; i < ops.size(); i++) {
            const PaintOp& op = ops[i];
            const PaintWhere& w = op.where;
            const std::string at = fieldOf(i);
            if (!w.rect && w.pixels.empty() && w.colors.empty() && !w.frame) {
                invalid(problems, at + ".where", at + ".where selects nothing: give at least one of rect, pixels, colors, frame", "where: {rect: [x, y, w, h]}");
            }
            if (w.rect && ((*w.rect)[2] < 1 || (*w.rect)[3] < 1)) {
                invalid(problems, at + ".where.rect", at + ".where.rect must have w and h >= 1 (got " + std::to_string((*w.rect)[2]) + "x" + std::to_string((*w.rect)[3]) + ")",
                        "e.g. [10, 12, 4, 6]");
            }
            if (w.pixels.size() > static_cast<size_t>(kMaxPaintPixels)) {
                invalid(problems, at + ".where.pixels", at + ".where.pixels has " + std::to_string(w.pixels.size()) + " pairs; at most " + std::to_string(kMaxPaintPixels),
                        "use a rect or colours for large areas");
            }
            if (w.colors.size() > static_cast<size_t>(kMaxPaintColors)) {
                invalid(problems, at + ".where.colors", at + ".where.colors has " + std::to_string(w.colors.size()) + " colours; at most " + std::to_string(kMaxPaintColors),
                        "split the colours over several operations");
            }
            if (!std::isfinite(w.tolerance) || w.tolerance < 0.0f || w.tolerance > 0.5f) {
                invalid(problems, at + ".where.tolerance", at + ".where.tolerance must be between 0 and 0.5 (got " + num(w.tolerance) + ")", "e.g. 0.02");
            }
            if (w.frame && (*w.frame < 0 || (frames >= 0 && *w.frame >= frames))) {
                invalid(problems, at + ".where.frame",
                        at + ".where.frame " + std::to_string(*w.frame) + " is not a frame" +
                            (frames >= 0 ? " (the output has " + std::to_string(frames) + (frames == 1 ? " frame: 0)" : " frames: 0 to " + std::to_string(frames - 1) + ")") : std::string()),
                        "frames count from 0, row-major (the recipe's frames.cells order)");
            }
            if (op.kind == PaintOpKind::Angle) {
                const auto& d = op.direction;
                const double length = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
                if (!std::isfinite(length) || std::abs(length - 1.0) > 1e-3 || !(d[2] > 0.0f)) {
                    invalid(problems, at + ".direction", at + ".direction must be a unit vector facing the viewer (z > 0)", "[x, y, z] with z > 0, or {azimuth_deg, tilt_deg}");
                }
            }
            if (!std::isfinite(op.strength) || op.strength < 0.0f || op.strength > 1.0f) {
                invalid(problems, at + ".strength", at + ".strength must be between 0 and 1 (got " + num(op.strength) + ")", "1 paints fully, 0.5 blends half");
            }
            if (op.kind == PaintOpKind::Smooth && (op.radius < 1 || op.radius > 4)) {
                invalid(problems, at + ".radius", at + ".radius must be from 1 to 4 (got " + std::to_string(op.radius) + ")", "1 averages 3x3 pixels");
            }
        }
        return problems;
    }

    int cellAt(const std::vector<Cell>& cells, int width, int height, int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return -1;
        if (cells.empty()) return 0;
        for (size_t c = 0; c < cells.size(); c++) {
            const Cell& cell = cells[c];
            if (x >= cell.x && y >= cell.y && x < cell.x + cell.width && y < cell.y + cell.height) return static_cast<int>(c);
        }
        return -1;
    }

    bool selectPixels(const PaintWhere& where, const PaintContext& ctx, std::vector<int>& pixels, std::string* error) {
        pixels.clear();
        const Image& source = ctx.source;
        const int width = source.width, height = source.height;
        if (width <= 0 || height <= 0) return true;
        int x0 = 0, y0 = 0, x1 = width, y1 = height;   // the region: [x0, x1) x [y0, y1)
        int ox = 0, oy = 0;                            // what rect and pixels are relative to
        if (where.frame) {
            const std::vector<Cell> cells = effectiveCells(ctx.cells, width, height);
            if (*where.frame < 0 || *where.frame >= static_cast<int>(cells.size())) {
                if (error) *error = "frame " + std::to_string(*where.frame) + " is not a frame (" + std::to_string(cells.size()) + " frames)";
                return false;
            }
            const Cell& c = cells[static_cast<size_t>(*where.frame)];
            x0 = std::max(x0, c.x);
            y0 = std::max(y0, c.y);
            x1 = std::min(x1, c.x + c.width);
            y1 = std::min(y1, c.y + c.height);
            ox = c.x;
            oy = c.y;
        }
        if (where.rect) {
            const auto& r = *where.rect;
            const int64_t rx = int64_t(ox) + r[0], ry = int64_t(oy) + r[1];
            x0 = static_cast<int>(std::max<int64_t>(x0, rx));
            y0 = static_cast<int>(std::max<int64_t>(y0, ry));
            x1 = static_cast<int>(std::min<int64_t>(x1, rx + r[2]));
            y1 = static_cast<int>(std::min<int64_t>(y1, ry + r[3]));
        }
        if (x0 >= x1 || y0 >= y1) return true;
        const std::vector<Lab> labs = labsOf(where.colors);
        auto accept = [&](int x, int y) {
            const uint8_t* px = source.pixel(x, y);
            if (transparent(px, ctx.alphaThreshold)) return false;
            return where.colors.empty() || colorMatches(px, where.colors, labs, where.tolerance);
        };
        if (!where.pixels.empty()) {
            std::vector<uint8_t> seen(static_cast<size_t>(width) * height, 0);
            for (const auto& p : where.pixels) {
                const int64_t x = int64_t(ox) + p[0], y = int64_t(oy) + p[1];
                if (x < x0 || y < y0 || x >= x1 || y >= y1) continue;
                const size_t i = static_cast<size_t>(y) * width + static_cast<size_t>(x);
                if (seen[i] || !accept(static_cast<int>(x), static_cast<int>(y))) continue;
                seen[i] = 1;
                pixels.push_back(static_cast<int>(i));
            }
            std::sort(pixels.begin(), pixels.end());
            return true;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                if (accept(x, y)) pixels.push_back(y * width + x);
            }
        }
        return true;
    }

    // ------------------------------------------------------------------ operations

    PaintTopology paintTopology(const Image& source, const std::vector<Cell>& cells, float alphaThreshold) {
        PaintTopology t;
        if (source.channels != 4 || source.width <= 0 || source.height <= 0) return t;
        t.width = source.width;
        t.height = source.height;
        t.owner = owners(cells, source.width, source.height);
        t.island = islands(source, t.owner, alphaThreshold);
        return t;
    }

    PaintStats::Op applyToPixels(PaintOverlay& overlay, PaintOpKind kind, const std::vector<int>& pixels, const std::array<float, 3>& direction,
                                 float strength, int radius, const PaintContext& ctx, const std::vector<std::array<float, 3>>* directions) {
        PaintStats::Op counts;
        const Image& source = ctx.source;
        const int width = source.width, height = source.height;
        if (!overlay.sized(width, height)) overlay = PaintOverlay::blank(width, height);
        if (pixels.empty() || width <= 0 || height <= 0) return counts;
        const float s = std::clamp(std::isfinite(strength) ? strength : 1.0f, 0.0f, 1.0f);
        const uint8_t target = coverage(255.0 * s);
        const size_t total = static_cast<size_t>(width) * height;

        // Smooth: the cells and islands (the context's cached ones when given) and the composite as
        // it stands (OpenGL), over the selection's bounding box grown by the radius only
        const int r = std::clamp(radius, 1, 4);
        PaintTopology local;
        const PaintTopology* field = ctx.topology && ctx.topology->width == width && ctx.topology->height == height ? ctx.topology : nullptr;
        int bx0 = width, by0 = height, bx1 = 0, by1 = 0;
        std::vector<uint8_t> current;   // RGB of [bx0, bx1) x [by0, by1)
        if (kind == PaintOpKind::Smooth) {
            if (!field) {
                local = paintTopology(source, ctx.cells, ctx.alphaThreshold);
                field = &local;
            }
            for (int i : pixels) {
                if (i < 0 || static_cast<size_t>(i) >= total) continue;
                bx0 = std::min(bx0, i % width - r);
                by0 = std::min(by0, i / width - r);
                bx1 = std::max(bx1, i % width + r + 1);
                by1 = std::max(by1, i / width + r + 1);
            }
            bx0 = std::max(0, bx0);
            by0 = std::max(0, by0);
            bx1 = std::min(width, bx1);
            by1 = std::min(height, by1);
            if (bx0 < bx1 && by0 < by1) {
                current.resize(static_cast<size_t>(bx1 - bx0) * (by1 - by0) * 3);
                for (int y = by0; y < by1; y++) {
                    for (int x = bx0; x < bx1; x++) {
                        const size_t i = static_cast<size_t>(y) * width + x;
                        // transparent pixels are never averaged: their value does not matter
                        compositePixel(ctx.generatedGL.data.data() + i * 4, overlay.rgba.data() + i * 4,
                                       &current[(static_cast<size_t>(y - by0) * (bx1 - bx0) + (x - bx0)) * 3]);
                    }
                }
            }
        }
        auto composed = [&](size_t j) { return &current[((j / width - by0) * static_cast<size_t>(bx1 - bx0) + (j % width - bx0)) * 3]; };
        // every new value from the state before this op (order never matters within one op)
        std::vector<std::array<uint8_t, 4>> values(pixels.size());
        std::vector<uint8_t> keep(pixels.size(), 0);
        for (size_t k = 0; k < pixels.size(); k++) {
            const int i = pixels[k];
            if (i < 0 || static_cast<size_t>(i) >= total || transparent(source.data.data() + static_cast<size_t>(i) * 4, ctx.alphaThreshold)) continue;
            keep[k] = 1;
            counts.selected++;
            const uint8_t* p = &overlay.rgba[static_cast<size_t>(i) * 4];
            std::array<uint8_t, 4> v = {p[0], p[1], p[2], p[3]};
            switch (kind) {
                case PaintOpKind::Angle: {
                    const std::array<uint8_t, 3> rgb = encodeDirection(directions && k < directions->size() ? (*directions)[k] : direction);
                    v = {rgb[0], rgb[1], rgb[2], std::max(p[3], target)};
                    break;
                }
                case PaintOpKind::Flatten:
                    v = {128, 128, 255, std::max(p[3], target)};
                    break;
                case PaintOpKind::Smooth: {
                    const int x = i % width, y = i / width;
                    double sum[3] = {0.0, 0.0, 0.0};
                    for (int yy = std::max(0, y - r); yy <= std::min(height - 1, y + r); yy++) {
                        for (int xx = std::max(0, x - r); xx <= std::min(width - 1, x + r); xx++) {
                            const size_t j = static_cast<size_t>(yy) * width + xx;
                            if (field->island[j] < 0 || field->island[j] != field->island[static_cast<size_t>(i)] || field->owner[j] != field->owner[static_cast<size_t>(i)]) continue;
                            const uint8_t* c = composed(j);
                            for (int ch = 0; ch < 3; ch++) sum[ch] += c[ch] / 127.5 - 1.0;
                        }
                    }
                    const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    const uint8_t* own = composed(static_cast<size_t>(i));
                    if (length < 1e-9) v = {own[0], own[1], own[2], std::max(p[3], target)};
                    else v = {encodeByte(sum[0] / length), encodeByte(sum[1] / length), encodeByte(sum[2] / length), std::max(p[3], target)};
                    break;
                }
                case PaintOpKind::Erase:
                    v[3] = coverage(p[3] * (1.0 - s));
                    break;
            }
            if (v[3] == 0) v = {128, 128, 255, 0};
            values[k] = v;
        }
        for (size_t k = 0; k < pixels.size(); k++) {
            if (!keep[k]) continue;
            uint8_t* p = &overlay.rgba[static_cast<size_t>(pixels[k]) * 4];
            if (std::memcmp(p, values[k].data(), 4) == 0) continue;
            std::memcpy(p, values[k].data(), 4);
            counts.changed++;
        }
        return counts;
    }

    std::vector<Problem> applyPaintOps(PaintOverlay& overlay, const std::vector<PaintOp>& ops, const PaintContext& ctx, PaintStats* stats) {
        const int cellCount = static_cast<int>(ctx.cells.size());
        std::vector<Problem> problems = validatePaintOps(ops, cellCount);
        if (!problems.empty()) return problems;
        const int width = ctx.source.width, height = ctx.source.height;
        if (!overlay.sized(width, height)) {
            if (!overlay.empty()) {
                problems.push_back({"paint_size_mismatch", "paint",
                                    "The paint is " + paintSizeText(overlay.width, overlay.height) + " but the output is " + paintSizeText(width, height) + ".",
                                    "Pass paint_mode: \"clear\" to drop the paint, or restore the source size."});
                return problems;
            }
            overlay = PaintOverlay::blank(width, height);
        }
        PaintStats local;
        PaintStats& out = stats ? *stats : local;
        out = PaintStats();
        // the islands once for every Smooth of the list (the context's when it has them)
        PaintTopology topology;
        PaintContext withTopology = ctx;
        bool smooths = false;
        for (const PaintOp& op : ops) smooths = smooths || op.kind == PaintOpKind::Smooth;
        if (smooths && !ctx.topology) {
            topology = paintTopology(ctx.source, ctx.cells, ctx.alphaThreshold);
            withTopology.topology = &topology;
        }
        for (size_t i = 0; i < ops.size(); i++) {
            const PaintOp& op = ops[i];
            std::vector<int> pixels;
            std::string error;
            selectPixels(op.where, ctx, pixels, &error);
            const PaintStats::Op counts = applyToPixels(overlay, op.kind, pixels, op.direction, op.strength, op.radius, withTopology);
            out.ops.push_back(counts);
            if (counts.selected == 0) {
                problems.push_back({"paint_empty_selection", fieldOf(i),
                                    fieldOf(i) + " (" + toString(op.kind) + ") selected no pixels: they are transparent, outside the image or frame, or of other colours.",
                                    "Check where: rect and pixels are relative to the frame when frame is given; colors match the source's colours."});
            }
        }
        for (size_t i = 0; i + 3 < overlay.rgba.size(); i += 4) {
            if (overlay.rgba[i + 3] == 0) continue;
            out.painted++;
            if (transparent(ctx.source.data.data() + i, ctx.alphaThreshold)) out.hidden++;
        }
        return problems;
    }

    // ------------------------------------------------------------------ compositing

    void flipGreen(Image& normal, const Image& source, float alphaThreshold) {
        if (normal.channels != 4 || normal.bitDepth != 8 || source.channels != 4 || source.width != normal.width || source.height != normal.height) return;
        const size_t n = normal.pixelCount();
        for (size_t i = 0; i < n; i++) {
            if (transparent(source.data.data() + i * 4, alphaThreshold)) continue;
            uint8_t& g = normal.data[i * 4 + 1];
            g = static_cast<uint8_t>(255 - g);
        }
    }

    void compositeRect(Image& out, const Image& generatedGL, const PaintOverlay& overlay, const Image& source, float alphaThreshold, bool directx, int x0,
                       int y0, int x1, int y1) {
        if (generatedGL.channels != 4 || generatedGL.bitDepth != 8 || source.channels != 4 || source.bitDepth != 8 || generatedGL.width != source.width ||
            generatedGL.height != source.height || out.width != source.width || out.height != source.height || out.channels != 4 || out.bitDepth != 8) {
            return;
        }
        const bool painted = overlay.sized(source.width, source.height);
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(source.width, x1);
        y1 = std::min(source.height, y1);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                const size_t i = static_cast<size_t>(y) * source.width + x;
                const uint8_t* s = source.data.data() + i * 4;
                uint8_t* o = out.data.data() + i * 4;
                std::memcpy(o, generatedGL.data.data() + i * 4, 3);
                o[3] = s[3];
                if (transparent(s, alphaThreshold)) {
                    // transparent pixels stay neutral (their paint is kept in the overlay)
                    setNeutral(o);
                    continue;
                }
                if (painted) compositePixel(generatedGL.data.data() + i * 4, overlay.rgba.data() + i * 4, o);
                if (directx) o[1] = static_cast<uint8_t>(255 - o[1]);
            }
        }
    }

    Image composite(const Image& generatedGL, const PaintOverlay& overlay, const Image& source, float alphaThreshold, bool directx) {
        if (generatedGL.channels != 4 || generatedGL.bitDepth != 8 || source.channels != 4 || source.bitDepth != 8 || generatedGL.width != source.width ||
            generatedGL.height != source.height) {
            return Image();
        }
        Image out = generatedGL;
        compositeRect(out, generatedGL, overlay, source, alphaThreshold, directx, 0, 0, source.width, source.height);
        return out;
    }

    Result generatePainted(const Request& request, PaintLayer& layer, const Control& control) {
        if (!layer.active()) {
            layer.stats = PaintStats();
            return generate(request, control);
        }
        Result result = generate(request, control);
        if (result.status != Status::Ok) return result;
        const Settings& s = request.settings;
        // the generation in OpenGL green: a DirectX one flipped (exact: unpainted pixels flip back
        // to the very bytes generate() made)
        Image gl = result.normal;
        if (s.directx) flipGreen(gl, request.color, s.alphaThreshold);
        const PaintContext ctx{request.color, gl, request.cells, s.alphaThreshold};
        const std::vector<Problem> problems = applyPaintOps(layer.overlay, layer.ops, ctx, &layer.stats);
        for (const Problem& p : problems) {
            if (p.kind == "invalid_paint" || p.kind == "paint_size_mismatch") {
                result.errors.push_back(p);
            } else {
                result.warnings.push_back(p);
            }
        }
        if (!result.errors.empty()) {
            result.status = Status::InvalidRequest;
            result.normal = Image();
            return result;
        }
        if (layer.stats.hidden > 0) {
            result.warnings.push_back({"paint_hidden_pixels", "paint",
                                       std::to_string(layer.stats.hidden) + " painted pixels are transparent now, so they don't show (the paint is kept for them).",
                                       "Erase them, or pass paint_mode: \"replace\" to start the paint over."});
        }
        result.normal = composite(gl, layer.overlay, request.color, s.alphaThreshold, s.directx);
        return result;
    }

    // ------------------------------------------------------------------ storage

    bool encodePaintText(const PaintOverlay& overlay, std::string& text, Problem& problem) {
        problem = Problem();
        text.clear();
        if (overlay.empty() || !overlay.sized(overlay.width, overlay.height)) return false;
        PaintOverlay normal = overlay;
        normal.normalise();
        Image image = Image::make(normal.width, normal.height, 4, 8);
        image.data = normal.rgba;
        std::vector<uint8_t> png;
        std::string error;
        if (!encodePng(image, png, error)) {
            problem = {"write_failed", "paint", "The paint layer could not be encoded (" + error + ").", "Try again; report it if it persists."};
            return false;
        }
        if (png.size() > kMaxPaintPngBytes) {
            char size[32];
            std::snprintf(size, sizeof(size), "%.1f", png.size() / (1024.0 * 1024.0));
            problem = {"paint_too_large", "paint",
                       std::string("The paint layer's image is ") + size + " MiB; at most 8 MiB fits in the normal map.",
                       "Paint fewer pixels, or split the sheet."};
            return false;
        }
        text = std::string("{\"format\":\"") + kPaintFormat + "\",\"version\":" + std::to_string(kPaintVersion) + ",\"size\":[" +
               std::to_string(normal.width) + "," + std::to_string(normal.height) + "],\"painted\":" + std::to_string(normal.paintedCount()) +
               ",\"encoding\":\"png-base64\",\"data\":\"" + base64Encode(png) + "\"}";
        return true;
    }

    bool encodeWithRecipeAndPaint(const Image& image, const json& recipe, const PaintOverlay* overlay, std::vector<uint8_t>& png, Problem& problem) {
        problem = Problem();
        std::string error;
        std::vector<uint8_t> plain;
        if (!encodePng(image, plain, error)) {
            problem = {"write_failed", "output", error, "Try again; report it if it persists."};
            return false;
        }
        // the paint first, then the recipe: both go right after IHDR, so IHDR, recipe, paint
        if (overlay && !overlay->empty()) {
            std::string text;
            if (!encodePaintText(*overlay, text, problem)) return false;
            std::vector<uint8_t> painted;
            if (!texsynth::insertRecipeChunk(plain, text, painted, error, kPaintKeyword)) {
                problem = {"write_failed", "paint", error, "Try again; report it if it persists."};
                return false;
            }
            plain.swap(painted);
        }
        std::string text;
        if (!recipeText(recipe, text, error) || !texsynth::insertRecipeChunk(plain, text, png, error, kRecipeKeyword)) {
            problem = {"write_failed", "output", error, "Rename the file (UTF-8 name), then try again."};
            return false;
        }
        return true;
    }

    std::optional<PaintOverlay> readPaint(const uint8_t* bytes, size_t size, std::string* problem) {
        if (problem) problem->clear();
        std::string error;
        const std::optional<std::string> text = texsynth::readRecipeTextFromPng(bytes, size, &error, kPaintKeyword);
        auto fail = [&](const std::string& why) {
            if (problem) *problem = why;
            return std::optional<PaintOverlay>();
        };
        if (!text) return error.empty() ? std::optional<PaintOverlay>() : fail(error);
        const json j = json::parse(*text, nullptr, false);
        PaintSummary head;
        std::string why;
        if (j.is_discarded()) return fail(std::string("the ") + kPaintKeyword + " chunk is not JSON");
        if (!readHead(j, head, why)) return fail(why);
        if (!j.contains("encoding") || !j["encoding"].is_string() || j["encoding"].get<std::string>() != "png-base64") {
            return fail("the paint chunk's encoding is not \"png-base64\"");
        }
        if (!j.contains("data") || !j["data"].is_string()) return fail("the paint chunk has no data");
        // the 8 MiB limit, checked on the text before anything is decoded
        if (j["data"].get_ref<const std::string&>().size() > 4 * ((kMaxPaintPngBytes + 2) / 3)) return fail("the paint chunk's image is larger than 8 MiB");
        std::vector<uint8_t> png;
        if (!base64Decode(j["data"].get_ref<const std::string&>(), png)) return fail("the paint chunk's data is not base64");
        if (png.size() > kMaxPaintPngBytes) return fail("the paint chunk's image is larger than 8 MiB");
        texsynth::Image image;
        if (!texsynth::loadImageFromMemory(png.data(), png.size(), image, error, 4096)) return fail("the paint chunk's image does not decode (" + error + ")");
        if (image.width != head.width || image.height != head.height) {
            return fail("the paint chunk says " + paintSizeText(head.width, head.height) + " but its image is " + paintSizeText(image.width, image.height));
        }
        const Image rgba = texsynth::toRgba8(image);
        PaintOverlay overlay;
        overlay.width = rgba.width;
        overlay.height = rgba.height;
        overlay.rgba = rgba.data;
        overlay.normalise();
        return overlay;
    }

    std::optional<PaintOverlay> readPaint(const fs::path& png, std::string* problem) {
        if (problem) problem->clear();
        std::ifstream file(png, std::ios::binary);
        if (!file) {
            if (problem) *problem = "cannot open " + jasperengine::editor::FileUtils::pathToUtf8(png);
            return std::nullopt;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return readPaint(bytes.data(), bytes.size(), problem);
    }

    std::optional<PaintSummary> readPaintSummary(const fs::path& png, std::string* problem) {
        if (problem) problem->clear();
        std::string error;
        const std::optional<std::string> text = texsynth::readRecipeTextHead(png, &error, kPaintKeyword);
        if (!text) {
            if (problem) *problem = error;
            return std::nullopt;
        }
        // the summary keys come before "data" (canonical order): parse only the head when it is there
        json j;
        const size_t data = text->find(",\"data\":");
        if (data != std::string::npos) j = json::parse(text->substr(0, data) + "}", nullptr, false);
        if (data == std::string::npos || j.is_discarded()) j = json::parse(*text, nullptr, false);
        PaintSummary summary;
        std::string why;
        if (j.is_discarded()) why = std::string("the ") + kPaintKeyword + " chunk is not JSON";
        else readHead(j, summary, why);
        if (!why.empty()) {
            if (problem) *problem = why;
            return std::nullopt;
        }
        return summary;
    }

    // ------------------------------------------------------------------ brush geometry

    std::vector<std::array<int, 2>> brushFootprint(PaintBrush::Shape shape, int size, int x, int y) {
        size = std::clamp(size, 1, 64);
        std::vector<std::array<int, 2>> out;
        const int lo = size / 2;   // odd: centred; even: the cursor pixel is the lower-right of the centre
        // the centre in pixel-corner coordinates: odd sizes the cursor pixel's centre, even its up-left corner
        const double cx = (size % 2) ? x + 0.5 : x;
        const double cy = (size % 2) ? y + 0.5 : y;
        const double r2 = (size / 2.0) * (size / 2.0);
        for (int py = y - lo; py < y - lo + size; py++) {
            for (int px = x - lo; px < x - lo + size; px++) {
                if (shape == PaintBrush::Shape::Round) {
                    const double dx = px + 0.5 - cx, dy = py + 0.5 - cy;
                    if (dx * dx + dy * dy > r2 + 1e-9) continue;
                }
                out.push_back({px, py});
            }
        }
        return out;
    }

    std::vector<std::array<int, 2>> linePixels(std::array<int, 2> a, std::array<int, 2> b) {
        std::vector<std::array<int, 2>> out;
        int x = a[0], y = a[1];
        const int dx = std::abs(b[0] - x), dy = -std::abs(b[1] - y);
        const int sx = x < b[0] ? 1 : -1, sy = y < b[1] ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            out.push_back({x, y});
            if (x == b[0] && y == b[1]) break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
        return out;
    }

    std::vector<int> wandSelect(const PaintContext& ctx, int x, int y, float tolerance, bool contiguous, bool allFrames) {
        std::vector<int> out;
        const Image& source = ctx.source;
        const int width = source.width, height = source.height;
        if (x < 0 || y < 0 || x >= width || y >= height) return out;
        const uint8_t* clicked = source.pixel(x, y);
        if (transparent(clicked, ctx.alphaThreshold)) return out;
        const std::vector<std::array<uint8_t, 3>> colors = {{clicked[0], clicked[1], clicked[2]}};
        const std::vector<Lab> labs = labsOf(colors);
        const std::vector<int> owner = owners(ctx.cells, width, height);
        const int cell = owner[static_cast<size_t>(y) * width + x];
        auto matches = [&](size_t i) {
            const uint8_t* px = source.data.data() + i * 4;
            if (transparent(px, ctx.alphaThreshold)) return false;
            if (!allFrames && owner[i] != cell) return false;
            return colorMatches(px, colors, labs, tolerance);
        };
        if (!contiguous) {
            for (size_t i = 0; i < owner.size(); i++) {
                if (matches(i)) out.push_back(static_cast<int>(i));
            }
            return out;
        }
        std::vector<uint8_t> seen(owner.size(), 0);
        std::vector<int> queue = {y * width + x};
        seen[static_cast<size_t>(queue[0])] = 1;
        for (size_t q = 0; q < queue.size(); q++) {
            const int i = queue[q];
            out.push_back(i);
            const int px = i % width, py = i / width;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = px + dx, ny = py + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const size_t j = static_cast<size_t>(ny) * width + nx;
                    if (seen[j] || !matches(j)) continue;
                    seen[j] = 1;
                    queue.push_back(static_cast<int>(j));
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
}
