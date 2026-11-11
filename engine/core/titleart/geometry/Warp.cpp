// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/warp.ts by SafEight, used with the author's permission.
//
// The warp mesh constructions are ported from Patchy (https://github.com/shabanmohd/patchy,
// docs/warp.md and src/core/warp_mesh.cpp), which reverse-engineered them from Photoshop 2026
// COM captures (control points match to about 1e-6 px):
//
// MIT License
//
// Copyright (c) 2026 Seth A. Robinson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Warp.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double PI = 3.14159265358979323846;

    const char* const STYLE_NAMES[WARP_STYLE_COUNT] = {
        "none", "arc", "arc_lower", "arc_upper", "arch", "bulge", "shell_lower", "shell_upper", "flag", "wave", "fish",
        "rise", "fisheye", "inflate", "squeeze", "twist"};

    WarpMesh identityMesh(double w, double h, int nu, int nv) {
        WarpMesh m;
        m.nu = nu;
        m.nv = nv;
        for (int r = 0; r < nv; r++) {
            for (int c = 0; c < nu; c++) {
                m.xs.push_back(w * c / (nu - 1));
                m.ys.push_back(h * r / (nv - 1));
            }
        }
        return m;
    }

    // one row of the classic cubic circle-arc approximation: the arc over the chord (ax, y)-(bx, y)
    // with central angle 2 theta, bulging toward -y (up in content space) when `up`
    void arcRow(WarpMesh& m, double ax, double bx, double y, double theta, bool up) {
        const double s = std::sin(theta), c = std::cos(theta);
        const double radius = (bx - ax) / (2 * s);
        const double handle = 4.0 / 3.0 * std::tan(theta / 2) * radius;
        const double dy = up ? -handle * s : handle * s;
        m.xs.insert(m.xs.end(), {ax, ax + handle * c, bx - handle * c, bx});
        m.ys.insert(m.ys.end(), {y, y + dy, y + dy, y});
    }

    void flatRow(WarpMesh& m, double w, double y) {
        m.xs.insert(m.xs.end(), {0.0, w / 3, 2 * w / 3, w});
        m.ys.insert(m.ys.end(), {y, y, y, y});
    }

    // Photoshop's negative arc is the positive arc flipped: y -> h - y, rows reversed
    WarpMesh mirrorRows(const WarpMesh& m, double h) {
        WarpMesh out;
        out.nu = m.nu;
        out.nv = m.nv;
        for (int r = m.nv - 1; r >= 0; r--) {
            for (int c = 0; c < m.nu; c++) {
                out.xs.push_back(m.xs[r * m.nu + c]);
                out.ys.push_back(h - m.ys[r * m.nu + c]);
            }
        }
        return out;
    }

    // the control mesh of a style at `bend` (-100..100) over a w x h content box
    WarpMesh styleMesh(WarpStyle style, double bend, double w, double h) {
        const double b = std::clamp(bend, -100.0, 100.0);
        if (style == WarpStyle::None || b == 0) return identityMesh(w, h, 4, 2);
        const double theta = std::abs(b) * PI / 200;
        const double d = 2 * h * b / 100;   // flag / wave / fish / rise displacement (height based)
        WarpMesh m;
        m.nu = 4;
        m.nv = 2;
        switch (style) {
            case WarpStyle::Arc: {
                // top corners swing out about the bottom corners (radius h); both edges arc with
                // the same central angle, so the band keeps its thickness
                const double s = std::sin(theta);
                arcRow(m, -h * s, w + h * s, h * (1 - std::cos(theta)), theta, true);
                arcRow(m, 0, w, h, theta, true);
                return b < 0 ? mirrorRows(m, h) : m;
            }
            case WarpStyle::ArcLower:
            case WarpStyle::ArcUpper:
                // one edge stays straight; the other arcs away from the box (+) or into it (-)
                flatRow(m, w, 0);
                arcRow(m, 0, w, h, theta, b < 0);
                return style == WarpStyle::ArcUpper ? mirrorRows(m, h) : m;
            case WarpStyle::Arch:
            case WarpStyle::Bulge:
                // arch: both edges bow the same way; bulge: the bottom bows the opposite way
                arcRow(m, 0, w, 0, theta, b > 0);
                arcRow(m, 0, w, h, theta, style == WarpStyle::Bulge ? b < 0 : b > 0);
                return m;
            case WarpStyle::ShellLower:
            case WarpStyle::ShellUpper: {
                // two flat rows; the 2h/3 row's end points rotate by theta about the anchor
                // corners, and the shell edge fans out (+) or arcs into the box (-)
                const double s = std::sin(theta), c = std::cos(theta);
                m.nv = 4;
                flatRow(m, w, 0);
                flatRow(m, w, h / 3);
                if (b > 0) {
                    const double r = 2 * h / 3;
                    m.xs.insert(m.xs.end(), {-r * s, w / 3, 2 * w / 3, w + r * s});
                    m.ys.insert(m.ys.end(), {r * c, 2 * h / 3, 2 * h / 3, r * c});
                    arcRow(m, -h * s, w + h * s, h * c, theta, false);
                } else {
                    const double r = h / 3;
                    m.xs.insert(m.xs.end(), {-r * s, w / 3, 2 * w / 3, w + r * s});
                    m.ys.insert(m.ys.end(), {h - r * c, 2 * h / 3, 2 * h / 3, h - r * c});
                    arcRow(m, 0, w, h, theta, true);
                }
                return style == WarpStyle::ShellUpper ? mirrorRows(m, h) : m;
            }
            case WarpStyle::Flag:
                flatRow(m, w, 0);
                flatRow(m, w, h);
                m.ys[1] -= d;
                m.ys[2] += d;
                m.ys[5] -= d;
                m.ys[6] += d;
                return m;
            case WarpStyle::Wave:
                // edges stay flat; only the middle row waves (the S runs opposite to flag's)
                m.nv = 3;
                flatRow(m, w, 0);
                flatRow(m, w, h / 2);
                flatRow(m, w, h);
                m.ys[5] += d;
                m.ys[6] -= d;
                return m;
            case WarpStyle::Fish:
                // flag's S on the top edge, the inverted S on the bottom: pinches head and tail
                flatRow(m, w, 0);
                flatRow(m, w, h);
                m.ys[1] -= d;
                m.ys[2] += d;
                m.ys[5] += d;
                m.ys[6] -= d;
                return m;
            case WarpStyle::Rise:
                // rigid column ramp: the left half drops, the right half stays
                flatRow(m, w, 0);
                flatRow(m, w, h);
                m.ys[0] += d;
                m.ys[1] += d;
                m.ys[4] += d;
                m.ys[5] += d;
                return m;
            case WarpStyle::Fisheye: {
                // interior points move toward their nearest corner by bend/50 (on the corners at 50)
                WarpMesh f = identityMesh(w, h, 4, 4);
                const double t = b / 50;
                for (int r = 1; r <= 2; r++) {
                    for (int c = 1; c <= 2; c++) {
                        const int i = r * 4 + c;
                        f.xs[i] += t * ((c == 1 ? 0 : w) - f.xs[i]);
                        f.ys[i] += t * ((r == 1 ? 0 : h) - f.ys[i]);
                    }
                }
                return f;
            }
            case WarpStyle::Inflate:
            case WarpStyle::Squeeze: {
                // quadratic 3 x 3: edge midpoints slide a quarter of the extent per 50 %. Inflate
                // pushes all four out; squeeze pushes top / bottom out and pinches the sides in
                WarpMesh q = identityMesh(w, h, 3, 3);
                const double dx = w * b / 200, dy = h * b / 200;
                q.ys[1] -= dy;
                q.ys[7] += dy;
                q.xs[3] += style == WarpStyle::Inflate ? -dx : dx;
                q.xs[5] += style == WarpStyle::Inflate ? dx : -dx;
                return q;
            }
            case WarpStyle::Twist: {
                // the interior ring circulates by the full extent per 100 % (clockwise on screen for +)
                WarpMesh t = identityMesh(w, h, 4, 4);
                const double mx = w * std::abs(b) / 100, my = h * std::abs(b) / 100;
                if (b > 0) {
                    t.xs[5] += mx;
                    t.ys[6] += my;
                    t.xs[10] -= mx;
                    t.ys[9] -= my;
                } else {
                    t.ys[5] += my;
                    t.xs[6] -= mx;
                    t.ys[10] -= my;
                    t.xs[9] += mx;
                }
                return t;
            }
            case WarpStyle::None:
                break;
        }
        return identityMesh(w, h, 4, 2);
    }

    // Photoshop's distortion: vertical first scales every ROW about the midpoint of its two edge
    // points by 1 + (2v - 1) vertical, then horizontal scales every COLUMN likewise by
    // 1 + (2u - 1) horizontal (Photoshop's order and pivot rule)
    void distortMesh(WarpMesh& m, double horizontal, double vertical) {
        const double hz = std::clamp(horizontal, -100.0, 100.0) / 100;
        const double vt = std::clamp(vertical, -100.0, 100.0) / 100;
        const auto scale = [&](const std::vector<int>& idx, double k) {
            const int a = idx.front(), z = idx.back();
            const double mx = (m.xs[a] + m.xs[z]) / 2, my = (m.ys[a] + m.ys[z]) / 2;
            for (int i : idx) {
                m.xs[i] = mx + (m.xs[i] - mx) * k;
                m.ys[i] = my + (m.ys[i] - my) * k;
            }
        };
        if (vt != 0) {
            for (int r = 0; r < m.nv; r++) {
                std::vector<int> idx;
                for (int c = 0; c < m.nu; c++) idx.push_back(r * m.nu + c);
                scale(idx, 1 + (2.0 * r / (m.nv - 1) - 1) * vt);
            }
        }
        if (hz != 0) {
            for (int c = 0; c < m.nu; c++) {
                std::vector<int> idx;
                for (int r = 0; r < m.nv; r++) idx.push_back(r * m.nu + c);
                scale(idx, 1 + (2.0 * c / (m.nu - 1) - 1) * hz);
            }
        }
    }

    // Bernstein weights of degree n - 1 at t (n = 2..4)
    void bernstein(int n, double t, double out[4]) {
        const double s = 1 - t;
        if (n == 2) {
            out[0] = s;
            out[1] = t;
        } else if (n == 3) {
            out[0] = s * s;
            out[1] = 2 * s * t;
            out[2] = t * t;
        } else {
            out[0] = s * s * s;
            out[1] = 3 * s * s * t;
            out[2] = 3 * s * t * t;
            out[3] = t * t * t;
        }
    }

    TitleVec2 evalMesh(const WarpMesh& m, double u, double v) {
        double wu[4], wv[4];
        bernstein(m.nu, u, wu);
        bernstein(m.nv, v, wv);
        double x = 0, y = 0;
        for (int r = 0; r < m.nv; r++) {
            for (int c = 0; c < m.nu; c++) {
                const double k = wu[c] * wv[r];
                x += k * m.xs[r * m.nu + c];
                y += k * m.ys[r * m.nu + c];
            }
        }
        return {x, y};
    }

}

const char* jasperengine::titleart::geometry::warpStyleName(WarpStyle style) {
    const int i = (int)style;
    return i >= 0 && i < WARP_STYLE_COUNT ? STYLE_NAMES[i] : "none";
}

bool jasperengine::titleart::geometry::warpStyleFromName(const std::string& name, WarpStyle& out) {
    for (int i = 0; i < WARP_STYLE_COUNT; i++) {
        if (name == STYLE_NAMES[i]) {
            out = (WarpStyle)i;
            return true;
        }
    }
    return false;
}

WarpParams jasperengine::titleart::geometry::decodeWarp(const Json& warp) {
    WarpParams p;
    if (!warp.is_object()) return p;
    warpStyleFromName(warp.value("style", std::string("none")), p.style);
    p.bend = warp.value("bend", 0.0);
    p.horizontal = warp.value("horizontal", 0.0);
    p.vertical = warp.value("vertical", 0.0);
    return p;
}

bool jasperengine::titleart::geometry::warpIsIdentity(const WarpParams& warp) {
    return (warp.style == WarpStyle::None || warp.bend == 0) && warp.horizontal == 0 && warp.vertical == 0;
}

WarpMesh jasperengine::titleart::geometry::warpMesh(const WarpParams& warp, double w, double h) {
    WarpMesh m = styleMesh(warp.style, warp.bend, w, h);
    distortMesh(m, warp.horizontal, warp.vertical);
    return m;
}

std::optional<Warper> jasperengine::titleart::geometry::makeWarper(const TitleRect& box, const WarpParams& warp) {
    if (warpIsIdentity(warp)) return std::nullopt;
    const double w = box.width, h = box.height;
    if (!(w > 1e-6) || !(h > 1e-6)) return std::nullopt;
    const WarpMesh m = warpMesh(warp, w, h);
    // re-centre: the warped frame's bounds (sampled along the four box edges) keep the box centre
    double fx0 = std::numeric_limits<double>::infinity(), fy0 = fx0;
    double fx1 = -fx0, fy1 = -fx0;
    constexpr int N = 64;
    for (int i = 0; i <= N; i++) {
        const double t = (double)i / N;
        for (const TitleVec2& p : {evalMesh(m, t, 0), evalMesh(m, t, 1), evalMesh(m, 0, t), evalMesh(m, 1, t)}) {
            fx0 = std::min(fx0, p.x);
            fx1 = std::max(fx1, p.x);
            fy0 = std::min(fy0, p.y);
            fy1 = std::max(fy1, p.y);
        }
    }
    const double sx = w / 2 - (fx0 + fx1) / 2;
    const double sy = h / 2 - (fy0 + fy1) / 2;
    const double size = std::max({w, h, fx1 - fx0, fy1 - fy0});
    const double bx0 = box.x, by1 = box.y + box.height;
    Warper out;
    out.map.map = [m, bx0, by1, w, h, sx, sy](const TitleVec2& p) {
        const TitleVec2 q = evalMesh(m, (p.x - bx0) / w, (by1 - p.y) / h);
        return TitleVec2{bx0 + q.x + sx, by1 - q.y - sy};
    };
    out.map.tol = std::max(size * 1.5e-4, 1e-3);
    out.map.span = std::max(w, h) / 8;
    out.frame = {bx0 + fx0 + sx, by1 - fy1 - sy, fx1 - fx0, fy1 - fy0};
    return out;
}

Path jasperengine::titleart::geometry::warpPath(const Path& path, const TitleRect& box, const WarpParams& warp) {
    const std::optional<Warper> warper = makeWarper(box, warp);
    return warper ? remapPath(path, warper->map) : path;
}

TitleRect jasperengine::titleart::geometry::warpFrame(const TitleRect& box, const WarpParams& warp) {
    const std::optional<Warper> warper = makeWarper(box, warp);
    return warper ? warper->frame : box;
}
