// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CPU ports of Pixelorama's pixel-art rotations and the AdvMAME scalers (S47 R21, R22, D9).
//
// Ported from Pixelorama (https://github.com/Orama-Interactive/Pixelorama):
//   src/Autoload/DrawingAlgos.gd            rotxel(), nn_rotate(), similar_colors()
//   src/Shaders/Effects/Rotation/cleanEdge.gdshader
//   src/Shaders/Effects/Rotation/OmniScale.gdshader
//
//   MIT License
//   Copyright (c) 2019-present Orama Interactive and contributors
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// cleanEdge (the shader Pixelorama ships):
//   MIT LICENSE
//   Copyright (c) 2022 torcado
//   (the same permission notice and warranty disclaimer as above)
//
// OmniScale (no-AA version from https://github.com/deakcor/godot-omniscale, edited by Overloaded
// for Pixelorama's rotation; original GLSL by hunterk via RetroArch, ported to Godot by Nobuyuki):
//   MIT License
//   Copyright (c) 2015-2016 Lior Halphon
//   (the same permission notice and warranty disclaimer as above)
//
// What differs from the sources, on purpose:
// - Every method maps output pixels to source pixels, so the result is made of source colours.
//   The shaders' texture() reads are nearest, clamp-to-edge lookups that remember the pixel they
//   read; where OmniScale's no-AA version still blends a few corner terms (w0 * 0.375 + ...), the
//   blend resolves to the nearest of its terms (ties: the centre pixel w4, else the first term).
// - Pixelorama's mix_rotated_and_original (the selection texture and the blend with the image
//   under it) is not ported: the caller composites. An output pixel whose rotated position lies
//   outside the source rectangle is transparent (-1), for every method.
// - One convention for all methods: positive degrees turn the image clockwise on screen (y down);
//   output pixel o takes the source at pivot + R(-angle) (o - pivot). Pixelorama's CPU rotxel and
//   nn_rotate turn the other way for a positive angle.
// - The pivot is exact in pixel-index coordinates for every size; rotxel's odd-size "ox += 1, oy
//   += 1" compensated Pixelorama's half-pixel-off pivot on odd sizes (size / 2 uncorrected) and is
//   dropped. The shaders' "+0.0001" size fudge (a GPU texel lookup fix) is replaced by reading the
//   pixel that contains the position.
// - Multiples of 90 degrees are exact permutations for every method.
// - Scale3x follows the published rules (scale2x.sourceforge.net); Pixelorama's scale_3x writes
//   each 3x3 block one pixel up and left of where it belongs.

#include "SpriteRotate.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        constexpr int32_t kNoSlice = -2;

        struct RotPx {
            int32_t idx = -1;
            uint8_t r = 0, g = 0, b = 0, a = 0;
        };

        struct RotSource {
            int w = 0, h = 0;
            std::span<const uint8_t> rgba;

            // nearest, clamp-to-edge
            RotPx at(int x, int y) const {
                x = x < 0 ? 0 : (x >= w ? w - 1 : x);
                y = y < 0 ? 0 : (y >= h ? h - 1 : y);
                const int32_t i = y * w + x;
                const uint8_t* p = rgba.data() + static_cast<size_t>(i) * 4;
                return RotPx{i, p[0], p[1], p[2], p[3]};
            }
        };

        struct RotVec {
            double x = 0.0, y = 0.0;
        };

        double roundHalfAway(double v) { return v < 0.0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5); }

        bool samePx(const RotPx& a, const RotPx& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

        // the source position (continuous: pixel i covers [i, i + 1)) an output pixel centre reads
        RotVec rotSourcePoint(int x, int y, double cosA, double sinA, double pivotX, double pivotY) {
            const double dx = (x + 0.5) - (pivotX + 0.5);
            const double dy = (y + 0.5) - (pivotY + 0.5);
            return RotVec{pivotX + 0.5 + dx * cosA + dy * sinA, pivotY + 0.5 - dx * sinA + dy * cosA};
        }

        bool rotInside(const RotVec& p, int w, int h) { return p.x >= 0.0 && p.y >= 0.0 && p.x < w && p.y < h; }

        // ---- 90-degree steps: exact permutations

        std::vector<int32_t> rotateQuarter(int w, int h, int quarter, double pivotX, double pivotY) {
            std::vector<int32_t> map(static_cast<size_t>(w) * h, -1);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const double dx = x - pivotX, dy = y - pivotY;
                    double sx = 0.0, sy = 0.0;
                    switch (quarter) {
                        case 0: sx = dx; sy = dy; break;
                        case 1: sx = dy; sy = -dx; break;      // R(-90)
                        case 2: sx = -dx; sy = -dy; break;
                        default: sx = -dy; sy = dx; break;     // R(-270)
                    }
                    const double fx = roundHalfAway(pivotX + sx), fy = roundHalfAway(pivotY + sy);
                    if (fx < 0.0 || fy < 0.0 || fx >= w || fy >= h) continue;
                    map[static_cast<size_t>(y) * w + x] = static_cast<int32_t>(fy) * w + static_cast<int32_t>(fx);
                }
            }
            return map;
        }

        // ---- nearest (nn_rotate)

        std::vector<int32_t> rotateNearest(const RotSource& src, double cosA, double sinA, double pivotX, double pivotY) {
            std::vector<int32_t> map(static_cast<size_t>(src.w) * src.h, -1);
            for (int y = 0; y < src.h; y++) {
                for (int x = 0; x < src.w; x++) {
                    const double dx = x - pivotX, dy = y - pivotY;
                    const double ox = dx * cosA + dy * sinA + pivotX;
                    const double oy = -dx * sinA + dy * cosA + pivotY;
                    if (ox >= 0.0 && ox < src.w && oy >= 0.0 && oy < src.h) {
                        // Godot's get_pixel truncates the float coordinates (non-negative here)
                        map[static_cast<size_t>(y) * src.w + x] = static_cast<int32_t>(std::floor(oy)) * src.w + static_cast<int32_t>(std::floor(ox));
                    }
                }
            }
            return map;
        }

        // ---- rotxel (DrawingAlgos.gd rotxel)

        // similar_colors with its default tolerance 0.392157 (100 of 255) per channel
        bool rotxelSimilar(const RotPx& a, const RotPx& b) {
            auto close = [](uint8_t p, uint8_t q) { return (p > q ? p - q : q - p) <= 100; };
            return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b) && close(a.a, b.a);
        }

        std::vector<int32_t> rotateRotxel(const RotSource& src, double radians, double pivotX, double pivotY) {
            const int w = src.w, h = src.h;
            std::vector<int32_t> map(static_cast<size_t>(w) * h, -1);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const double dx = 3.0 * (x - pivotX);
                    const double dy = 3.0 * (y - pivotY);
                    bool found = false;
                    int ox = 0, oy = 0;
                    for (int k = 0; k < 9; k++) {
                        const int modk = -1 + k % 3;
                        const int divk = -1 + k / 3;
                        const double ex = dx + modk, ey = dy + divk;
                        // clockwise on screen: the source lies at the output turned back by the angle
                        const double dir = std::atan2(ey, ex) - radians;
                        const double mag = std::sqrt(ex * ex + ey * ey);
                        ox = static_cast<int>(roundHalfAway(pivotX * 3.0 + 1.0 + mag * std::cos(dir)));
                        oy = static_cast<int>(roundHalfAway(pivotY * 3.0 + 1.0 + mag * std::sin(dir)));
                        if (ox >= 0 && ox < w * 3 && oy >= 0 && oy < h * 3) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) continue;
                    const int index = ox % 3 + 3 * (oy % 3);
                    ox = static_cast<int>(roundHalfAway((ox - 1) / 3.0));
                    oy = static_cast<int>(roundHalfAway((oy - 1) / 3.0));
                    int32_t out = 0;
                    if (ox == 0 || ox == w - 1 || oy == 0 || oy == h - 1) {
                        out = src.at(ox, oy).idx;
                    } else {
                        const RotPx a = src.at(ox - 1, oy - 1), b = src.at(ox, oy - 1), c = src.at(ox + 1, oy - 1);
                        const RotPx d = src.at(ox - 1, oy), e = src.at(ox, oy), f = src.at(ox + 1, oy);
                        const RotPx g = src.at(ox - 1, oy + 1), hh = src.at(ox, oy + 1), i = src.at(ox + 1, oy + 1);
                        auto s = rotxelSimilar;
                        switch (index) {
                            case 0: out = (s(d, b) && !s(d, hh) && !s(b, f)) ? d.idx : e.idx; break;
                            case 1:
                                out = ((s(d, b) && !s(d, hh) && !s(b, f) && !s(e, c)) || (s(b, f) && !s(d, b) && !s(f, hh) && !s(e, a))) ? b.idx : e.idx;
                                break;
                            case 2: out = (s(b, f) && !s(d, b) && !s(f, hh)) ? f.idx : e.idx; break;
                            case 3:
                                out = ((s(d, hh) && !s(f, hh) && !s(d, b) && !s(e, a)) || (s(d, b) && !s(d, hh) && !s(b, f) && !s(e, g))) ? d.idx : e.idx;
                                break;
                            case 4: out = e.idx; break;
                            case 5:
                                out = ((s(b, f) && !s(d, b) && !s(f, hh) && !s(e, i)) || (s(f, hh) && !s(b, f) && !s(d, hh) && !s(e, c))) ? f.idx : e.idx;
                                break;
                            case 6: out = (s(d, hh) && !s(f, hh) && !s(d, b)) ? d.idx : e.idx; break;
                            case 7:
                                out = ((s(f, hh) && !s(f, b) && !s(d, hh) && !s(e, g)) || (s(d, hh) && !s(f, hh) && !s(d, b) && !s(e, i))) ? hh.idx : e.idx;
                                break;
                            default: out = (s(f, hh) && !s(f, b) && !s(d, hh)) ? f.idx : e.idx; break;
                        }
                    }
                    map[static_cast<size_t>(y) * w + x] = out;
                }
            }
            return map;
        }

        // ---- cleanEdge (SLOPE and CLEANUP defined, similarThreshold 0, lineWidth 1, highestColor white)

        bool ceSimilar(const RotPx& a, const RotPx& b) { return (a.a == 0 && b.a == 0) || samePx(a, b); }
        bool ceSimilar3(const RotPx& a, const RotPx& b, const RotPx& c) { return ceSimilar(a, b) && ceSimilar(b, c); }
        bool ceSimilar4(const RotPx& a, const RotPx& b, const RotPx& c, const RotPx& d) {
            return ceSimilar(a, b) && ceSimilar(b, c) && ceSimilar(c, d);
        }

        bool ceHigher(const RotPx& self, const RotPx& other) {
            if (ceSimilar(self, other)) return false;
            if (self.a == other.a) {
                // closer to the highest colour (white) wins; squared distances order as distances do
                auto toWhite = [](const RotPx& p) {
                    const int dr = 255 - p.r, dg = 255 - p.g, db = 255 - p.b;
                    return dr * dr + dg * dg + db * db;
                };
                return toWhite(self) < toWhite(other);
            }
            return self.a > other.a;
        }

        double ceDistance(const RotPx& a, const RotPx& b) {
            const double dr = (a.r - b.r) / 255.0, dg = (a.g - b.g) / 255.0, db = (a.b - b.b) / 255.0, da = (a.a - b.a) / 255.0;
            return std::sqrt(dr * dr + dg * dg + db * db + da * da);
        }

        RotVec ceAdd(RotVec a, RotVec b) { return {a.x + b.x, a.y + b.y}; }
        RotVec ceMul(RotVec a, RotVec b) { return {a.x * b.x, a.y * b.y}; }

        double ceDistToLine(RotVec testPt, RotVec pt1, RotVec pt2, RotVec dir) {
            const RotVec lineDir{pt2.x - pt1.x, pt2.y - pt1.y};
            const RotVec perpDir{lineDir.y, -lineDir.x};
            const RotVec dirToPt1{pt1.x - testPt.x, pt1.y - testPt.y};
            const double length = std::sqrt(perpDir.x * perpDir.x + perpDir.y * perpDir.y);
            const double sign = (perpDir.x * dir.x + perpDir.y * dir.y) > 0.0 ? 1.0 : -1.0;
            return sign * ((perpDir.x / length) * dirToPt1.x + (perpDir.y / length) * dirToPt1.y);
        }

        // sliceDist: the colour a slice from a neighbour paints at `point`, kNoSlice when none
        int32_t ceSlice(RotVec point, RotVec mainDir, RotVec pointDir, const RotPx& u, const RotPx& uf, const RotPx& uff, const RotPx& b,
                        const RotPx& c, const RotPx& f, const RotPx& ff, const RotPx& db, const RotPx& d, const RotPx& df, const RotPx& dff,
                        const RotPx& ddb, const RotPx& dd, const RotPx& ddf) {
            const double lineWidth = 1.0;   // max(0.44, min(1.142, lineWidth)) with SLOPE
            point = RotVec{mainDir.x * (point.x - 0.5) + 0.5, mainDir.y * (point.y - 0.5) + 0.5};
            const RotVec neg{-pointDir.x, -pointDir.y};

            const double distAgainst = 4.0 * ceDistance(f, d) + ceDistance(uf, c) + ceDistance(c, db) + ceDistance(ff, df) + ceDistance(df, dd);
            const double distTowards = 4.0 * ceDistance(c, df) + ceDistance(u, f) + ceDistance(f, dff) + ceDistance(b, d) + ceDistance(d, ddf);
            bool shouldSlice = (distAgainst < distTowards) || ((distAgainst < distTowards + 0.001) && !ceHigher(c, f));
            if (ceSimilar4(f, d, b, u) && ceSimilar3(uf, df, db) && !ceSimilar(c, f)) shouldSlice = false;   // checkerboard
            if (!shouldSlice) return kNoSlice;

            double dist = 1.0;
            bool flip = false;
            const RotVec center{0.5, 0.5};
            auto at = [&](double x, double y) { return ceAdd(center, ceMul(RotVec{x, y}, pointDir)); };
            auto pick = [&](const RotPx& from, const RotPx& p, const RotPx& q) {
                return dist <= 0.0 ? ((ceDistance(from, p) <= ceDistance(from, q)) ? p.idx : q.idx) : kNoSlice;
            };

            if (ceSimilar3(f, d, db) && !ceSimilar3(f, d, b) && !ceSimilar(uf, db)) {   // lower shallow 2:1 slant
                if (!(ceSimilar(c, df) && ceHigher(c, f))) {
                    if (ceHigher(c, f)) flip = true;
                    if (ceSimilar(u, f) && !ceSimilar(c, df) && !ceHigher(c, u)) flip = true;
                }
                if (flip) dist = lineWidth - ceDistToLine(point, at(1.5, -1.0), at(-0.5, 0.0), neg);
                else dist = ceDistToLine(point, at(1.5, 0.0), at(-0.5, 1.0), pointDir);
                if (!flip && ceSimilar(c, uf) && !(ceSimilar3(c, uf, uff) && !ceSimilar3(c, uf, ff) && !ceSimilar(d, uff))) {
                    dist = std::min(dist, ceDistToLine(point, at(2.0, -1.0), at(0.0, 1.0), pointDir));
                }
                dist -= lineWidth / 2.0;
                return pick(c, f, d);
            } else if (ceSimilar3(uf, f, d) && !ceSimilar3(u, f, d) && !ceSimilar(uf, db)) {   // forward steep 2:1 slant
                if (!(ceSimilar(c, df) && ceHigher(c, d))) {
                    if (ceHigher(c, d)) flip = true;
                    if (ceSimilar(b, d) && !ceSimilar(c, df) && !ceHigher(c, d)) flip = true;
                }
                if (flip) dist = lineWidth - ceDistToLine(point, at(0.0, -0.5), at(-1.0, 1.5), neg);
                else dist = ceDistToLine(point, at(1.0, -0.5), at(0.0, 1.5), pointDir);
                if (!flip && ceSimilar(c, db) && !(ceSimilar3(c, db, ddb) && !ceSimilar3(c, db, dd) && !ceSimilar(f, ddb))) {
                    dist = std::min(dist, ceDistToLine(point, at(1.0, 0.0), at(-1.0, 2.0), pointDir));
                }
                dist -= lineWidth / 2.0;
                return pick(c, f, d);
            } else if (ceSimilar(f, d)) {   // 45 diagonal
                if (ceSimilar(c, df) && ceHigher(c, f)) {
                    if (!ceSimilar(c, dd) && !ceSimilar(c, ff)) flip = true;   // line against a triple colour stripe
                } else {
                    if (ceHigher(c, f)) flip = true;
                    if (!ceSimilar(c, b) && ceSimilar4(b, f, d, u)) flip = true;
                }
                if (((ceSimilar(f, db) && ceSimilar3(u, f, df)) || (ceSimilar(uf, d) && ceSimilar3(b, d, df))) && !ceSimilar(c, df)) {
                    flip = true;   // single pixel 2:1 slope
                }
                if (flip) dist = lineWidth - ceDistToLine(point, at(1.0, -1.0), at(-1.0, 1.0), neg);
                else dist = ceDistToLine(point, at(1.0, 0.0), at(0.0, 1.0), pointDir);
                if (!flip && ceSimilar3(c, uf, uff) && !ceSimilar3(c, uf, ff) && !ceSimilar(d, uff)) {   // shallow
                    dist = std::max(dist, ceDistToLine(point, at(1.5, 0.0), at(-0.5, 1.0), pointDir));
                }
                if (!flip && ceSimilar3(ddb, db, c) && !ceSimilar3(dd, db, c) && !ceSimilar(ddb, f)) {   // steep
                    dist = std::max(dist, ceDistToLine(point, at(1.0, -0.5), at(0.0, 1.5), pointDir));
                }
                dist -= lineWidth / 2.0;
                return pick(c, f, d);
            } else if (ceSimilar3(ff, df, d) && !ceSimilar3(ff, df, c) && !ceSimilar(uff, d)) {   // far corner of a shallow slant
                if (!(ceSimilar(f, dff) && ceHigher(f, ff))) {
                    if (ceHigher(f, ff)) flip = true;
                    if (ceSimilar(uf, ff) && !ceSimilar(f, dff) && !ceHigher(f, uf)) flip = true;
                }
                if (flip) dist = lineWidth - ceDistToLine(point, at(1.5 + 1.0, -1.0), at(-0.5 + 1.0, 0.0), neg);
                else dist = ceDistToLine(point, at(1.5 + 1.0, 0.0), at(-0.5 + 1.0, 1.0), pointDir);
                dist -= lineWidth / 2.0;
                return pick(f, ff, df);
            } else if (ceSimilar3(f, df, dd) && !ceSimilar3(c, df, dd) && !ceSimilar(f, ddb)) {   // far corner of a steep slant
                if (!(ceSimilar(d, ddf) && ceHigher(d, dd))) {
                    if (ceHigher(d, dd)) flip = true;
                    if (ceSimilar(db, dd) && !ceSimilar(d, ddf) && !ceHigher(d, dd)) flip = true;
                }
                if (flip) dist = lineWidth - ceDistToLine(point, at(0.0, -0.5 + 1.0), at(-1.0, 1.5 + 1.0), neg);
                else dist = ceDistToLine(point, at(1.0, -0.5 + 1.0), at(0.0, 1.5 + 1.0), pointDir);
                dist -= lineWidth / 2.0;
                return pick(d, df, dd);
            }
            return kNoSlice;
        }

        int32_t cleanEdgeAt(const RotSource& src, RotVec p) {
            const int cx = static_cast<int>(std::floor(p.x)), cy = static_cast<int>(std::floor(p.y));
            const RotVec local{p.x - cx, p.y - cy};
            // round(local) * 2 - 1, GLSL round taken half away from zero
            const RotVec pd{local.x >= 0.5 ? 1.0 : -1.0, local.y >= 0.5 ? 1.0 : -1.0};
            const int sx = static_cast<int>(pd.x), sy = static_cast<int>(pd.y);
            auto t = [&](int ox, int oy) { return src.at(cx + ox * sx, cy + oy * sy); };

            const RotPx uub = t(-1, -2), uu = t(0, -2), uuf = t(1, -2);
            const RotPx ubb = t(-2, -2), ub = t(-1, -1), u = t(0, -1), uf = t(1, -1), uff = t(2, -1);
            const RotPx bb = t(-2, 0), b = t(-1, 0), c = t(0, 0), f = t(1, 0), ff = t(2, 0);
            const RotPx dbb = t(-2, 1), db = t(-1, 1), d = t(0, 1), df = t(1, 1), dff = t(2, 1);
            const RotPx ddb = t(-1, 2), dd = t(0, 2), ddf = t(1, 2);

            int32_t col = c.idx;
            // corner, back and up slices (slices from neighbour pixels only reach these three quadrants)
            const int32_t cCol = ceSlice(local, RotVec{1.0, 1.0}, pd, u, uf, uff, b, c, f, ff, db, d, df, dff, ddb, dd, ddf);
            const int32_t bCol = ceSlice(local, RotVec{-1.0, 1.0}, pd, u, ub, ubb, f, c, b, bb, df, d, db, dbb, ddf, dd, ddb);
            const int32_t uCol = ceSlice(local, RotVec{1.0, -1.0}, pd, d, df, dff, b, c, f, ff, ub, u, uf, uff, uub, uu, uuf);
            if (cCol != kNoSlice) col = cCol;
            if (bCol != kNoSlice) col = bCol;
            if (uCol != kNoSlice) col = uCol;
            return col;
        }

        // ---- OmniScale (no-AA)

        // distance(a, b) > 0.1 on 0-1 RGBA: 100 * d^2 > 255^2, exact in integers
        bool omniDifferent(const RotPx& a, const RotPx& b) {
            const int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b, da = a.a - b.a;
            return 100 * (dr * dr + dg * dg + db * db + da * da) > 255 * 255;
        }

        // mix(a, b, step) of the no-AA shader: a selection
        const RotPx& omniSel(const RotPx& a, const RotPx& b, bool takeB) { return takeB ? b : a; }

        // A weighted blend the no-AA version kept, resolved to the nearest of its terms (ties: the
        // term listed first; callers list the centre pixel first)
        RotPx omniBlend(std::initializer_list<std::pair<const RotPx*, double>> terms) {
            double r = 0, g = 0, b = 0, a = 0;
            for (const auto& [px, weight] : terms) {
                r += px->r * weight;
                g += px->g * weight;
                b += px->b * weight;
                a += px->a * weight;
            }
            const RotPx* best = nullptr;
            double bestDistance = 0.0;
            for (const auto& [px, weight] : terms) {
                const double dr = px->r - r, dg = px->g - g, db = px->b - b, da = px->a - a;
                const double distance = dr * dr + dg * dg + db * db + da * da;
                if (!best || distance < bestDistance) {
                    best = px;
                    bestDistance = distance;
                }
            }
            return *best;
        }

        int32_t omniScaleAt(const RotSource& src, RotVec pos) {
            const int cx = static_cast<int>(std::floor(pos.x)), cy = static_cast<int>(std::floor(pos.y));
            RotVec p{pos.x - cx, pos.y - cy};
            int ox = 1, oy = 1;
            // the top-left quarter is computed; another quarter flips the coordinates
            if (p.x > 0.5) {
                ox = -1;
                p.x = 1.0 - p.x;
            }
            if (p.y > 0.5) {
                oy = -1;
                p.y = 1.0 - p.y;
            }
            const RotPx w0 = src.at(cx - ox, cy - oy), w1 = src.at(cx, cy - oy), w2 = src.at(cx + ox, cy - oy);
            const RotPx w3 = src.at(cx - ox, cy), w4 = src.at(cx, cy), w5 = src.at(cx + ox, cy);
            const RotPx w6 = src.at(cx - ox, cy + oy), w7 = src.at(cx, cy + oy), w8 = src.at(cx + ox, cy + oy);

            int pattern = 0;
            if (omniDifferent(w0, w4)) pattern |= 1 << 0;
            if (omniDifferent(w1, w4)) pattern |= 1 << 1;
            if (omniDifferent(w2, w4)) pattern |= 1 << 2;
            if (omniDifferent(w3, w4)) pattern |= 1 << 3;
            if (omniDifferent(w5, w4)) pattern |= 1 << 4;
            if (omniDifferent(w6, w4)) pattern |= 1 << 5;
            if (omniDifferent(w7, w4)) pattern |= 1 << 6;
            if (omniDifferent(w8, w4)) pattern |= 1 << 7;
            auto P = [&](int mask, int value) { return (pattern & mask) == value; };

            const double pixelSize = std::sqrt(0.125);   // length(1 / (ScaleMultiplier 4 / 1))
            const double pixelSize5 = pixelSize * std::sqrt(5.0);

            // the corner colour of several branches
            auto corner = [&]() -> RotPx {
                if (omniDifferent(w0, w1) || omniDifferent(w0, w3)) return omniSel(w1, w3, p.y - p.x + 0.5 > 0.5);
                const RotPx blend = omniBlend({{&w1, 0.375}, {&w3, 0.375}, {&w0, 0.25}});
                return omniSel(omniSel(blend, w3, p.y * 2.0 > 0.5), w1, p.x * 2.0 > 0.5);
            };

            if ((P(191, 55) || P(219, 19)) && omniDifferent(w1, w5)) return omniSel(w4, w3, 0.5 - p.x > 0.5).idx;
            if ((P(219, 73) || P(239, 109)) && omniDifferent(w7, w3)) return omniSel(w4, w1, 0.5 - p.y > 0.5).idx;
            if ((P(11, 11) || P(254, 74) || P(254, 26)) && omniDifferent(w3, w1)) return w4.idx;
            if ((P(111, 42) || P(91, 10) || P(191, 58) || P(223, 90) || P(159, 138) || P(207, 138) || P(239, 78) || P(63, 14) ||
                 P(251, 90) || P(187, 138) || P(127, 90) || P(175, 138) || P(235, 138)) && omniDifferent(w3, w1)) {
                return omniSel(w4, omniSel(w4, w0, 0.5 - p.x > 0.5), 0.5 - p.y > 0.5).idx;
            }
            if (P(11, 8)) {
                const RotPx a = omniBlend({{&w4, 0.375}, {&w0, 0.375}, {&w1, 0.25}});
                const RotPx b = omniBlend({{&w4, 0.5}, {&w1, 0.5}});
                return omniSel(omniSel(a, b, p.x * 2.0 > 0.5), w4, p.y * 2.0 > 0.5).idx;
            }
            if (P(11, 2)) {
                const RotPx a = omniBlend({{&w4, 0.375}, {&w0, 0.375}, {&w3, 0.25}});
                const RotPx b = omniBlend({{&w4, 0.5}, {&w3, 0.5}});
                return omniSel(omniSel(a, b, p.y * 2.0 > 0.5), w4, p.x * 2.0 > 0.5).idx;
            }
            if (P(47, 47)) {
                const double dist = std::sqrt((p.x - 0.5) * (p.x - 0.5) + (p.y - 0.5) * (p.y - 0.5));
                if (dist < 0.5 - pixelSize / 2.0) return w4.idx;
                const RotPx r = corner();
                if (dist > 0.5 + pixelSize / 2.0) return r.idx;
                return omniSel(w4, r, (dist - 0.5 + pixelSize / 2.0) / pixelSize > 0.5).idx;
            }
            if (P(191, 55) || P(219, 19)) {
                const double dist = p.x - 2.0 * p.y;
                if (dist > pixelSize5 / 2.0) return w1.idx;
                const RotPx r = omniSel(w3, w4, p.x + 0.5 > 0.5);
                if (dist < -pixelSize5 / 2.0) return r.idx;
                return omniSel(r, w1, (dist + pixelSize5 / 2.0) / pixelSize5 > 0.5).idx;
            }
            if (P(219, 73) || P(239, 109)) {
                const double dist = p.y - 2.0 * p.x;
                if (dist > pixelSize5 / 2.0) return w3.idx;
                const RotPx r = omniSel(w1, w4, p.x + 0.5 > 0.5);
                if (dist < -pixelSize5 / 2.0) return r.idx;
                return omniSel(r, w3, (dist + pixelSize5 / 2.0) / pixelSize5 > 0.5).idx;
            }
            if (P(191, 143) || P(126, 14)) {
                const double dist = p.x + 2.0 * p.y;
                if (dist > 1.0 + pixelSize5 / 2.0) return w4.idx;
                const RotPx r = corner();
                if (dist < 1.0 - pixelSize5 / 2.0) return r.idx;
                return omniSel(r, w4, (dist + pixelSize5 / 2.0 - 1.0) / pixelSize5 > 0.5).idx;
            }
            if (P(126, 42) || P(239, 171)) {
                const double dist = p.y + 2.0 * p.x;
                if (dist > 1.0 + pixelSize5 / 2.0) return w4.idx;
                const RotPx r = corner();
                if (dist < 1.0 - pixelSize5 / 2.0) return r.idx;
                return omniSel(r, w4, (dist + pixelSize5 / 2.0 - 1.0) / pixelSize5 > 0.5).idx;
            }
            if (P(27, 3) || P(79, 67) || P(139, 131) || P(107, 67)) return omniSel(w4, w3, 0.5 - p.x > 0.5).idx;
            if (P(75, 9) || P(139, 137) || P(31, 25) || P(59, 25)) return omniSel(w4, w1, 0.5 - p.y > 0.5).idx;
            if (P(251, 106) || P(111, 110) || P(63, 62) || P(251, 250) || P(223, 222) || P(223, 30)) {
                return omniSel(w4, w0, (1.0 - p.x - p.y) / 2.0 > 0.5).idx;
            }
            if (P(79, 75) || P(159, 27) || P(47, 11) || P(190, 10) || P(238, 10) || P(126, 10) || P(235, 75) || P(59, 27)) {
                const double dist = p.x + p.y;
                if (dist > 0.5 + pixelSize / 2.0) return w4.idx;
                const RotPx r = corner();
                if (dist < 0.5 - pixelSize / 2.0) return r.idx;
                return omniSel(r, w4, (dist + pixelSize / 2.0 - 0.5) / pixelSize > 0.5).idx;
            }
            if (P(11, 1)) {
                const RotPx half = omniBlend({{&w1, 0.5}, {&w3, 0.5}});
                return omniSel(omniSel(w4, w3, 0.5 - p.x > 0.5), omniSel(w1, half, 0.5 - p.x > 0.5), 0.5 - p.y > 0.5).idx;
            }
            if (P(11, 0)) {
                return omniSel(omniSel(w4, w3, 0.5 - p.x > 0.5), omniSel(w1, w0, 0.5 - p.x > 0.5), 0.5 - p.y > 0.5).idx;
            }

            const double dist = p.x + p.y;
            if (dist > 0.5 + pixelSize / 2.0) return w4.idx;
            // more samples to solve this diagonal
            const RotPx x0 = src.at(cx - 2 * ox, cy - 2 * oy), x1 = src.at(cx - ox, cy - 2 * oy), x2 = src.at(cx, cy - 2 * oy);
            const RotPx x3 = src.at(cx + ox, cy - 2 * oy), x4 = src.at(cx - 2 * ox, cy - oy), x5 = src.at(cx - 2 * ox, cy);
            const RotPx x6 = src.at(cx - 2 * ox, cy + oy);
            if (omniDifferent(x0, w4)) pattern |= 1 << 8;
            if (omniDifferent(x1, w4)) pattern |= 1 << 9;
            if (omniDifferent(x2, w4)) pattern |= 1 << 10;
            if (omniDifferent(x3, w4)) pattern |= 1 << 11;
            if (omniDifferent(x4, w4)) pattern |= 1 << 12;
            if (omniDifferent(x5, w4)) pattern |= 1 << 13;
            if (omniDifferent(x6, w4)) pattern |= 1 << 14;
            int diagonalBias = -7;
            for (int bits = pattern; bits != 0; bits >>= 1) diagonalBias += bits & 1;
            if (diagonalBias <= 0) {
                const RotPx r = omniSel(w1, w3, p.y - p.x + 0.5 > 0.5);
                if (dist < 0.5 - pixelSize / 2.0) return r.idx;
                return omniSel(r, w4, (dist + pixelSize / 2.0 - 0.5) / pixelSize > 0.5).idx;
            }
            return w4.idx;
        }

        // ---- AdvMAME2x / AdvMAME3x over a map of source indices

        std::vector<int32_t> advMame(int w, int h, int factor, const std::vector<int32_t>& in, const PixelEqual& equal) {
            const int ow = w * factor;
            std::vector<int32_t> out(static_cast<size_t>(ow) * h * factor, -1);
            auto px = [&](int x, int y) {
                x = x < 0 ? 0 : (x >= w ? w - 1 : x);
                y = y < 0 ? 0 : (y >= h ? h - 1 : y);
                return in[static_cast<size_t>(y) * w + x];
            };
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const int32_t A = px(x - 1, y - 1), B = px(x, y - 1), C = px(x + 1, y - 1);
                    const int32_t D = px(x - 1, y), E = px(x, y), F = px(x + 1, y);
                    const int32_t G = px(x - 1, y + 1), H = px(x, y + 1), I = px(x + 1, y + 1);
                    auto eq = [&](int32_t a, int32_t b) { return a == b || equal(a, b); };
                    auto put = [&](int sx, int sy, int32_t v) { out[static_cast<size_t>(y * factor + sy) * ow + x * factor + sx] = v; };
                    const bool db = eq(D, B), bf = eq(B, F), dh = eq(D, H), hf = eq(H, F);
                    if (factor == 2) {
                        put(0, 0, (db && !bf && !dh) ? D : E);
                        put(1, 0, (bf && !db && !hf) ? F : E);
                        put(0, 1, (dh && !db && !hf) ? D : E);
                        put(1, 1, (hf && !dh && !bf) ? F : E);
                        continue;
                    }
                    put(0, 0, (db && !bf && !dh) ? D : E);
                    put(1, 0, ((db && !bf && !dh && !eq(E, C)) || (bf && !db && !hf && !eq(E, A))) ? B : E);
                    put(2, 0, (bf && !db && !hf) ? F : E);
                    put(0, 1, ((db && !bf && !dh && !eq(E, G)) || (dh && !db && !hf && !eq(E, A))) ? D : E);
                    put(1, 1, E);
                    put(2, 1, ((bf && !db && !hf && !eq(E, I)) || (hf && !dh && !bf && !eq(E, C))) ? F : E);
                    put(0, 2, (dh && !db && !hf) ? D : E);
                    put(1, 2, ((dh && !db && !hf && !eq(E, I)) || (hf && !dh && !bf && !eq(E, G))) ? H : E);
                    put(2, 2, (hf && !dh && !bf) ? F : E);
                }
            }
            return out;
        }

    }

    std::vector<int32_t> rotateMap(int w, int h, std::span<const uint8_t> rgba, double degrees, double pivotX, double pivotY,
                                   RotateMethod method) {
        if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return {};
        double turn = std::fmod(degrees, 360.0);
        if (turn < 0.0) turn += 360.0;
        const double quarters = turn / 90.0;
        const double nearest = roundHalfAway(quarters);
        if (std::fabs(quarters - nearest) * 90.0 < 1e-9) {
            return rotateQuarter(w, h, static_cast<int>(nearest) % 4, pivotX, pivotY);
        }
        const RotSource src{w, h, rgba};
        const double radians = turn * (3.14159265358979323846 / 180.0);
        const double cosA = std::cos(radians), sinA = std::sin(radians);
        switch (method) {
            case RotateMethod::Nearest: return rotateNearest(src, cosA, sinA, pivotX, pivotY);
            case RotateMethod::Rotxel: return rotateRotxel(src, radians, pivotX, pivotY);
            case RotateMethod::CleanEdge:
            case RotateMethod::OmniScale: break;
        }
        std::vector<int32_t> map(static_cast<size_t>(w) * h, -1);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const RotVec p = rotSourcePoint(x, y, cosA, sinA, pivotX, pivotY);
                if (!rotInside(p, w, h)) continue;
                map[static_cast<size_t>(y) * w + x] = method == RotateMethod::CleanEdge ? cleanEdgeAt(src, p) : omniScaleAt(src, p);
            }
        }
        return map;
    }

    std::vector<int32_t> scaleMap(int w, int h, int factor, const PixelEqual& equal) {
        if (w <= 0 || h <= 0 || factor < 2 || factor > 4) return {};
        std::vector<int32_t> identity(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < identity.size(); i++) identity[i] = static_cast<int32_t>(i);
        if (factor != 4) return advMame(w, h, factor, identity, equal);
        // AdvMAME4x: AdvMAME2x twice, the second pass comparing the first pass's source pixels
        const std::vector<int32_t> twice = advMame(w, h, 2, identity, equal);
        return advMame(w * 2, h * 2, 2, twice, equal);
    }

}
