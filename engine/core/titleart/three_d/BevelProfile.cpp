// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/bevel.ts by SafEight, used with the author's permission.

#include "BevelProfile.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double QUARTER = PI / 2;
    // how far inside a piece a singular end's tangent is taken (parameter units)
    constexpr double SINGULAR_NUDGE = 1e-6;

    struct Sample {
        double o, h, to, th;
    };

    // a piece: its point and analytic tangent at parameter s in 0..1
    struct Piece {
        std::function<Sample(double)> f;
        int segments = 1;
        bool smooth = false;
    };

    bool usable(double a, double b) {
        return std::isfinite(a) && std::isfinite(b) && (std::abs(a) > 1e-14 || std::abs(b) > 1e-14);
    }

    UnitPiece samplePiece(const Piece& piece) {
        UnitPiece out;
        for (int k = 0; k <= piece.segments; k++) {
            const double s = (double)k / piece.segments;
            Sample v = piece.f(s);
            UnitPoint p;
            p.o = v.o;
            p.h = v.h;
            if (piece.smooth) {
                if (!usable(v.to, v.th)) {
                    // singular at an end: the limit direction from just inside
                    const double inside = k == 0 ? SINGULAR_NUDGE : k == piece.segments ? 1 - SINGULAR_NUDGE : s;
                    const Sample n = piece.f(inside);
                    v.to = n.to;
                    v.th = n.th;
                }
                if (usable(v.to, v.th)) {
                    p.smooth = true;
                    p.to = v.to;
                    p.th = v.th;
                }
            }
            out.push_back(p);
        }
        return out;
    }

    const std::vector<std::string> NAMES = {"flat", "chamfer", "round", "cove", "step", "inset", "tube", "custom"};

}

const char* jasperengine::titleart::three_d::bevelProfileName(BevelProfileKind kind) {
    return NAMES[(size_t)kind].c_str();
}

bool jasperengine::titleart::three_d::bevelProfileFromName(std::string_view name, BevelProfileKind& out) {
    for (size_t i = 0; i < NAMES.size(); i++) {
        if (NAMES[i] == name) {
            out = (BevelProfileKind)i;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& jasperengine::titleart::three_d::bevelProfileNames() {
    return NAMES;
}

int jasperengine::titleart::three_d::bevelSegments(int segments) {
    return std::max(1, std::min(BEVEL_MAX_SEGMENTS, segments));
}

int jasperengine::titleart::three_d::effectiveSegments(int segments, double outputScale) {
    return std::min(BEVEL_MAX_SEGMENTS, (int)std::ceil(bevelSegments(segments) * std::sqrt(std::max(1.0, outputScale))));
}

std::vector<UnitPiece> jasperengine::titleart::three_d::unitBevel(const BevelShape& shape, int segments) {
    const int n = bevelSegments(segments);
    const double p = std::pow(4.0, std::clamp(shape.curve, -1.0, 1.0));
    const bool bent = std::abs(p - 1) > 1e-6;
    // the bend of a drop d in 0..1 and its derivative
    auto bend = [=](double d) { return bent ? std::pow(std::max(0.0, d), p) : d; };
    auto bendSlope = [=](double d) { return bent ? p * std::pow(std::max(0.0, d), p - 1) : 1.0; };
    // straight piece; a riser (both o and h change) bends into a smooth curve when curve != 0
    auto line = [&](double o0, double h0, double o1, double h1) {
        const bool riser = bent && o0 != o1 && h0 != h1;
        Piece piece;
        piece.segments = riser ? n : 1;
        piece.smooth = riser;
        piece.f = [=](double s) -> Sample {
            if (!riser) return {o0 + (o1 - o0) * s, h0 + (h1 - h0) * s, o1 - o0, h1 - h0};
            return {o0 + (o1 - o0) * s, h0 + (h1 - h0) * bend(s), o1 - o0, (h1 - h0) * bendSlope(s)};
        };
        return piece;
    };
    // quarter arc parametrised by angle t = s x pi/2; the bend applies to the drop
    auto arc = [&](std::function<double(double)> o, std::function<double(double)> dO, std::function<double(double)> h,
                   std::function<double(double)> dH) {
        Piece piece;
        piece.segments = n;
        piece.smooth = true;
        piece.f = [=](double s) -> Sample {
            const double t = s * QUARTER;
            const double hv = h(t);
            return {o(t), bend(hv), dO(t) * QUARTER, bendSlope(hv) * dH(t) * QUARTER};
        };
        return piece;
    };
    std::vector<Piece> pieces;
    switch (shape.profile) {
        case BevelProfileKind::Flat:
            // a flat ring at the inner height, then a vertical step down at the outer edge
            pieces = {line(0, 0, 1, 0), line(1, 0, 1, 1)};
            break;
        case BevelProfileKind::Chamfer:
            pieces = {line(0, 0, 1, 1)};
            break;
        case BevelProfileKind::Round:
            // convex quarter round: flat where it meets the inner part, vertical at the outer edge
            pieces = {arc([](double t) { return std::sin(t); }, [](double t) { return std::cos(t); },
                          [](double t) { return 1 - std::cos(t); }, [](double t) { return std::sin(t); })};
            break;
        case BevelProfileKind::Cove:
            // concave quarter: drops steeply from the inner edge and runs out flat
            pieces = {arc([](double t) { return 1 - std::cos(t); }, [](double t) { return std::sin(t); },
                          [](double t) { return std::sin(t); }, [](double t) { return std::cos(t); })};
            break;
        case BevelProfileKind::Step:
            // two risers with a front-facing ledge between them, ending on a ledge
            pieces = {line(0, 0, 0.15, 0.5), line(0.15, 0.5, 0.5, 0.5), line(0.5, 0.5, 0.65, 1), line(0.65, 1, 1, 1)};
            break;
        case BevelProfileKind::Inset:
            // a raised rim: rises from the inner edge, flat on top (the inner part sits recessed)
            pieces = {line(0, 0, 0.4, -1), line(0.4, -1, 1, -1)};
            break;
        case BevelProfileKind::Tube: {
            // a half-round tube rising to a crest `depth` high at mid-width and back down
            Piece tube;
            tube.segments = 2 * n;
            tube.smooth = true;
            tube.f = [=](double s) -> Sample {
                const double a = s * PI;
                const double sn = std::max(0.0, std::sin(a));
                const double e = 1 / p;
                // singular at the ends when the bend sharpens or rounds the crest (samplePiece nudges)
                const double dh = -e * std::pow(sn, e - 1) * std::cos(a) * PI;
                return {(1 - std::cos(a)) / 2, -std::pow(sn, e), std::sin(a) * PI / 2, dh};
            };
            pieces = {tube};
            break;
        }
        case BevelProfileKind::Custom: {
            // the curve's keys split it into runs; a corner key starts a new piece (a crease)
            const TitleCurve& c = shape.custom;
            std::vector<double> stops;
            for (int k = 0; k <= n; k++) stops.push_back((double)k / n);
            for (const TitleCurvePoint& key : c.points) stops.push_back(std::clamp(key.x, 0.0, 1.0));
            std::sort(stops.begin(), stops.end());
            stops.erase(std::unique(stops.begin(), stops.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), stops.end());
            std::vector<double> creases = {0.0};
            for (const TitleCurvePoint& key : c.points) {
                if (key.corner && key.x > 1e-9 && key.x < 1 - 1e-9) creases.push_back(key.x);
            }
            creases.push_back(1.0);
            std::vector<UnitPiece> out;
            for (size_t r = 0; r + 1 < creases.size(); r++) {
                const double x0 = creases[r], x1 = creases[r + 1];
                UnitPiece piece;
                for (double x : stops) {
                    if (x < x0 - 1e-9 || x > x1 + 1e-9) continue;
                    const double hv = std::clamp(c.evaluate(x), 0.0, 1.0);
                    // one-sided slopes at the run's ends, so each side of a crease keeps its own
                    const bool left = x >= x1 - 1e-9 && x > x0 + 1e-9;
                    const double dh = c.derivative(x, left);
                    UnitPoint pt;
                    pt.o = x;
                    pt.h = bend(hv);
                    double th = bendSlope(hv) * dh;
                    if (!std::isfinite(th)) th = bendSlope(std::max(hv, SINGULAR_NUDGE)) * dh;
                    pt.smooth = true;
                    pt.to = 1.0;
                    pt.th = th;
                    piece.push_back(pt);
                }
                if (piece.size() >= 2) out.push_back(std::move(piece));
            }
            return out;
        }
    }
    std::vector<UnitPiece> out;
    out.reserve(pieces.size());
    for (const Piece& piece : pieces) out.push_back(samplePiece(piece));
    return out;
}
