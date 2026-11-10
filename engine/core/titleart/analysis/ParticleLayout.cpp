// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Ported from Pachislop src/generators/particles.ts and src/generators/inkSplatter.ts by SafEight, used with the author's permission.

#include "ParticleLayout.h"

#include "titleart/effects/TitleFx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;

    // mixed-in glints' radius, x the particle's own
    constexpr double GLINT_MIX_SCALE = 3.5;
    // length / width of confetti strips and sakura petals
    constexpr double CONFETTI_ASPECT = 2.4;
    constexpr double PETAL_ASPECT = 1.25;
    // the depth focus plane (0 far .. 1 near): the sharp band sits just in front of the middle
    constexpr double FOCUS = 0.6;
    constexpr int LUT_N = 64;
    // tries per particle before giving up on a sparse emitter (thin edges of small text)
    constexpr int MAX_TRIES = 400;
    // cells stepped inward when picking a particle's colour from the ink
    constexpr int INK_COLOR_STEPS = 8;
    // grid region snap and readback budget (cells per canvas-px area), Pachislop's contentGrid
    constexpr double SNAP = 64.0;
    constexpr double GRID_CELLS = 50000.0;
    // Ink Splatter's main drops at Amount 100 %
    constexpr int INK_MAX_DROPS = 80;

    using Rgba = std::array<double, 4>;

    double clamp01(double v) {
        return std::min(1.0, std::max(0.0, v));
    }

    double mixLinear(double a, double b, double t) {
        return a + (b - a) * t;
    }

    // JavaScript's Math.round: half up
    double jsRound(double v) {
        return std::floor(v + 0.5);
    }

    double jsSign(double v) {
        return v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0;
    }

    double fract(double v) {
        return v - std::floor(v);
    }

    double smoothstep(double e0, double e1, double x) {
        const double t = clamp01((x - e0) / (e1 - e0));
        return t * t * (3.0 - 2.0 * t);
    }

    // shapes that tumble in 3D: foreshortened along their width, the back face shown when flipped
    bool tumbling(ParticleShape s) {
        return s == ParticleShape::Square || s == ParticleShape::Confetti || s == ParticleShape::Petal || s == ParticleShape::Star ||
               s == ParticleShape::Heart;
    }

    // shapes the Glint mix applies to (gold dust: dots with a few big 4-point glints)
    bool glintMixable(ParticleShape s) {
        return s == ParticleShape::Dot || s == ParticleShape::Square;
    }

    // signed -1..1 with density falling off toward +-1 (falloff 0 = uniform)
    double axis(double u, double falloff) {
        const double v = 2.0 * u - 1.0;
        return jsSign(v) * std::pow(std::abs(v), 1.0 + 2.0 * falloff);
    }

    void unit(double x, double y, double fx, double fy, double& ox, double& oy) {
        const double l = std::hypot(x, y);
        if (l > 1e-6) {
            ox = x / l;
            oy = y / l;
        } else {
            ox = fx;
            oy = fy;
        }
    }

    Rgba linearRgba(const TitleGradient& g, double pos) {
        float c[4];
        fx::sampleGradient(g, pos, c);
        return {srgbToLinear(c[0]), srgbToLinear(c[1]), srgbToLinear(c[2]), c[3]};
    }

    // where a particle ends up and how it got there (drawing space px)
    struct Birth {
        double x = 0.0, y = 0.0;
        double dx = 1.0, dy = 0.0;      // unit motion direction (streak orientation)
        double t = 0.0;                 // 0..1 distance measure: gradient position and depth bias
        bool hasSpeed = false;
        double speed = 0.0;             // 0..1 (streak length); default the particle's nearness
        double fade = 1.0;              // opacity thinning out along the travel
        bool hasRgb = false;
        double rgb[3] = {0.0, 0.0, 0.0};        // linear, sampled from the layer
        double mx = 0.0, my = 0.0;      // the displacement included in x, y
    };

    using BirthFn = std::function<bool(gen::Rng& r, Birth& b)>;

    // a start point on the analytic emitters with its outward direction and 0..1 radius
    struct Start {
        double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0, t = 0.0;
    };

    // Pachislop buildParticles: `birth` places particle i from its placement stream; everything
    // else comes from its look stream, drawn in a fixed order whether used or not
    std::vector<ParticleRecord> buildParticles(const ParticleLook& look, const BirthFn& birth) {
        std::vector<ParticleRecord> out;
        out.reserve((size_t)std::max(0, look.count));
        const double d = look.depth;
        std::vector<Rgba> lut;
        if (look.colorMode == ParticleColorMode::Radial) {
            for (int k = 0; k < LUT_N; k++) lut.push_back(linearRgba(look.gradient, (double)k / (LUT_N - 1)));
        }
        std::vector<Rgba> palette;
        for (const TitleGradientStop& s : look.gradient.stops) {
            const TitleColor c = fx::linearOf(s.color);
            palette.push_back({c.r, c.g, c.b, s.alpha});
        }
        const Rgba solid = {look.color.r, look.color.g, look.color.b, 1.0};
        for (int i = 0; i < look.count; i++) {
            gen::Rng placement = particleStream(look.seed, i, 0);
            Birth b;
            if (!birth(placement, b)) continue;
            gen::Rng a = particleStream(look.seed, i, 1);
            const double z0 = a();
            const double su = a();
            const double ru = a();
            const double phi = a() * 2.0 * PI;
            const double ou = a();
            const double pu = a();
            const double gu = a();
            const double side = a();
            gen::Rng motion = particleStream(look.seed, i, 2);
            const double motionPhase = motion();

            // most particles far and small, a few near and big; with Depth they also sit further out
            const double z = clamp01(mixLinear(std::pow(z0, 1.7), b.t, 0.5 * d));
            const double nearness = 1.0 + d * (2.4 * z - 0.7);
            const double bright = 1.0 + d * (0.8 * z - 0.35);
            const bool glint = look.glintMix > 0.0 && gu < look.glintMix;
            const ParticleShape shape = glint ? ParticleShape::Glint4 : look.shape;
            double blur = d * (z < FOCUS ? (0.8 * (FOCUS - z)) / FOCUS : (2.4 * (z - FOCUS)) / (1.0 - FOCUS));
            if (particleShapeGlint(shape)) blur = std::min(blur, 0.5);     // glints stay crisp
            else if (shape == ParticleShape::Bokeh) blur *= 0.3;           // already a defocused disc
            const double r = (look.size / 2.0) * (1.0 - look.sizeRandom * su) * nearness * (glint ? GLINT_MIX_SCALE : 1.0);
            if (!(r > 0.0)) continue;
            const double speed = b.hasSpeed ? b.speed : z;
            double aspect = 1.0;
            double rot = (2.0 * ru - 1.0) * look.rotation;
            if (particleShapeStreaked(shape)) {
                const double len = look.streak * (shape == ParticleShape::Ember ? 0.35 + 0.65 * speed : 0.5 + 0.5 * speed);
                aspect = 1.0 + len / (2.0 * r);
                rot = std::atan2(b.dy, b.dx);
            } else if (shape == ParticleShape::Confetti) {
                aspect = CONFETTI_ASPECT;
            } else if (shape == ParticleShape::Petal) {
                aspect = PETAL_ASPECT;
            }
            const bool tumbles = tumbling(shape) && look.tumble > 0.0;
            const double fs = tumbles ? mixLinear(1.0, std::max(0.12, std::abs(std::cos(phi))), look.tumble) : 1.0;

            Rgba c;
            if (look.colorMode == ParticleColorMode::Sample && b.hasRgb) c = {b.rgb[0], b.rgb[1], b.rgb[2], 1.0};
            else if (look.colorMode == ParticleColorMode::Radial) c = lut[(size_t)jsRound(clamp01(b.t) * (LUT_N - 1))];
            else if (look.colorMode == ParticleColorMode::Palette && !palette.empty())
                c = palette[(size_t)std::min((double)palette.size() - 1.0, std::floor(pu * (double)palette.size()))];
            else c = solid;
            double shade = 1.0;
            if (tumbles && std::cos(phi) < 0.0) shade = shape == ParticleShape::Confetti ? 0.45 : 0.85;    // back face: two-tone
            if (shape == ParticleShape::Confetti) shade *= 0.65 + 0.35 * std::abs(std::cos(phi));      // foil turning from the light
            const double alpha = look.opacity * (1.0 - look.opacityRandom * ou) * b.fade * c[3];
            if (!(alpha > 0.0)) continue;
            const double k = alpha * shade * (particleShapeSolid(shape) ? 1.0 : bright);

            ParticleRecord q;
            q.index = (uint32_t)i;
            q.x = (float)b.x;
            q.y = (float)b.y;
            q.mx = (float)b.mx;
            q.my = (float)b.my;
            q.w = (float)(2.0 * r * (aspect + blur));
            q.h = (float)(2.0 * r * (1.0 + blur) * fs);
            q.rot = (float)rot;
            q.color[0] = (float)(c[0] * k);
            q.color[1] = (float)(c[1] * k);
            q.color[2] = (float)(c[2] * k);
            q.color[3] = (float)alpha;
            q.blur = (float)blur;
            q.aspect = (float)aspect;
            q.phase = (float)phi;
            q.z = (float)z;
            q.side = (float)side;
            q.motionPhase = (float)motionPhase;
            q.glint = glint;
            out.push_back(q);
        }
        return out;
    }

    // the unpremultiplied linear colour of a particle born in cell i: the most vivid ink cell up
    // to INK_COLOR_STEPS cells inward along the normal (titles are rimmed with dark keylines and
    // crossed by white highlights; the sparks should carry the face colour)
    void inkColor(const EmitMap& m, int i, double rgb[3]) {
        const Grid& g = *m.grid;
        const int w = g.w, h = g.h;
        const int x0 = i % w;
        const int y0 = (i - x0) / w;
        int best = i;
        double bestScore = -1.0;
        for (int k = 0; k <= INK_COLOR_STEPS; k++) {
            const double x = jsRound(x0 - (double)m.nx[(size_t)i] * k);
            const double y = jsRound(y0 - (double)m.ny[(size_t)i] * k);
            if (x < 0.0 || y < 0.0 || x >= w || y >= h) break;
            const int j = (int)y * w + (int)x;
            const float* c = &g.rgba[(size_t)j * 4];
            if (c[3] < 0.5f) continue;
            const double mx = std::max({(double)c[0], (double)c[1], (double)c[2]});
            const double mn = std::min({(double)c[0], (double)c[1], (double)c[2]});
            // brightness x (some + saturation): a vivid face beats a white highlight and a dark rim
            const double score = mx * (0.4 + (mx > 0.0 ? (mx - mn) / mx : 0.0));
            if (score > bestScore) {
                bestScore = score;
                best = j;
            }
        }
        const float* c = &g.rgba[(size_t)best * 4];
        const double a = std::max(1e-4, (double)c[3]);
        rgb[0] = c[0] / a;
        rgb[1] = c[1] / a;
        rgb[2] = c[2] / a;
    }

}

ParticleShape analysis::particleShapeOf(std::string_view name) {
    static const char* const NAMES[] = {"dot", "glint4", "glint6", "ember", "square", "confetti", "petal", "star", "heart", "bokeh", "rain"};
    for (int i = 0; i < (int)(sizeof(NAMES) / sizeof(NAMES[0])); i++) {
        if (name == NAMES[i]) return (ParticleShape)i;
    }
    return ParticleShape::Dot;
}

bool analysis::particleShapeSolid(ParticleShape s) {
    return s == ParticleShape::Confetti || s == ParticleShape::Petal || s == ParticleShape::Heart;
}

bool analysis::particleShapeStreaked(ParticleShape s) {
    return s == ParticleShape::Ember || s == ParticleShape::Rain;
}

bool analysis::particleShapeGlint(ParticleShape s) {
    return s == ParticleShape::Glint4 || s == ParticleShape::Glint6;
}

ParticleLook analysis::readParticleLook(const ParamValues& p, double seed) {
    ParticleLook look;
    look.shape = particleShapeOf(p.string("shape"));
    look.glintMix = glintMixable(look.shape) ? p.number("glint_mix") / 100.0 : 0.0;
    look.count = (int)std::min((double)PARTICLES_MAX, std::max(0.0, jsRound(p.number("count"))));
    look.size = std::max(0.1, p.number("size"));
    look.sizeRandom = p.number("size_random") / 100.0;
    look.depth = p.number("depth") / 100.0;
    look.streak = std::max(0.0, p.number("streak"));
    look.ring = p.number("ring") / 100.0;
    look.rotation = p.number("rotation_random") * DEG;
    look.tumble = p.number("tumble") / 100.0;
    look.sheen = p.number("sheen") / 100.0;
    const std::string mode = p.string("color_mode");
    look.colorMode = mode == "radial"    ? ParticleColorMode::Radial
                     : mode == "palette" ? ParticleColorMode::Palette
                     : mode == "sample"  ? ParticleColorMode::Sample
                                         : ParticleColorMode::Solid;
    look.color = fx::linearOf(p.color("color"));
    look.gradient = p.gradient("gradient");
    look.opacity = p.number("opacity") / 100.0;
    look.opacityRandom = p.number("opacity_random") / 100.0;
    look.seed = (int)jsRound(seed);
    return look;
}

gen::Rng analysis::particleStream(int seed, int index, int salt) {
    // Math.imul(seed + 0x632be5ab, 0x9e3779b1) ^ Math.imul(i + 1, 0x85ebca6b) ^ Math.imul(salt + 1, 0xc2b2ae35)
    const int32_t a = gen::imul((int32_t)(uint32_t)((int64_t)seed + 0x632be5abLL), (int32_t)0x9e3779b1u);
    const int32_t b = gen::imul((int32_t)(uint32_t)((int64_t)index + 1), (int32_t)0x85ebca6bu);
    const int32_t c = gen::imul((int32_t)(uint32_t)((int64_t)salt + 1), (int32_t)0xc2b2ae35u);
    const uint32_t k = (uint32_t)(a ^ b ^ c);
    // common.ts rng floors seed x 4096 before hashing it: this hands it the 32-bit key exactly
    return gen::Rng((double)k / 4096.0);
}

bool analysis::buildEmitMap(const Grid& grid, bool edge, double bandPx, EmitMap& out) {
    const int w = grid.w, h = grid.h;
    const size_t n = (size_t)w * (size_t)h;
    if (w <= 0 || h <= 0 || grid.alpha.size() < n || grid.rgba.size() < n * 4) return false;
    const double cell = grid.cellComp;
    std::vector<float> alpha(n), outside(n);
    double sx = 0.0, sy = 0.0;
    int cnt = 0;
    int ix0 = w, iy0 = h, ix1 = -1, iy1 = -1;
    for (size_t i = 0; i < n; i++) {
        const float a = grid.alpha[i];
        alpha[i] = a;
        outside[i] = a >= 0.5f ? 0.0f : 1.0f;
        if (a >= 0.5f) {
            const int x = (int)(i % (size_t)w);
            const int y = (int)(i / (size_t)w);
            sx += x;
            sy += y;
            cnt++;
            ix0 = std::min(ix0, x);
            iy0 = std::min(iy0, y);
            ix1 = std::max(ix1, x);
            iy1 = std::max(iy1, y);
        }
    }
    if (cnt == 0) return false;
    out.grid = &grid;
    out.cx = grid.x0 + (sx / cnt + 0.5) * cell;
    out.cy = grid.y0 + (sy / cnt + 0.5) * cell;
    out.radius = 0.5 * std::hypot((double)(ix1 - ix0 + 1), (double)(iy1 - iy0 + 1)) * cell;
    // cells to the nearest non-ink cell (chamfer), for the edge band
    const std::vector<float> dOut = gridDistance(w, h, outside);
    const double band = std::max(1.0, bandPx / cell);
    out.mask.assign(n, 0);
    for (size_t i = 0; i < n; i++) out.mask[i] = alpha[i] >= 0.5f && (!edge || dOut[i] <= band) ? 1 : 0;
    // normals: -gradient of the 3 x 3 smoothed alpha; radial from the centroid where it is flat
    std::vector<float> sm(n);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double s = 0.0;
            for (int j = -1; j <= 1; j++) {
                for (int k = -1; k <= 1; k++) s += alpha[(size_t)std::min(h - 1, std::max(0, y + j)) * w + std::min(w - 1, std::max(0, x + k))];
            }
            sm[(size_t)y * w + x] = (float)(s / 9.0);
        }
    }
    out.nx.assign(n, 0.0f);
    out.ny.assign(n, 0.0f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = (size_t)y * w + x;
            const double gx = (double)sm[(size_t)y * w + std::min(w - 1, x + 1)] - (double)sm[(size_t)y * w + std::max(0, x - 1)];
            const double gy = (double)sm[(size_t)std::min(h - 1, y + 1) * w + x] - (double)sm[(size_t)std::max(0, y - 1) * w + x];
            double vx = -gx;
            double vy = -gy;
            double len = std::hypot(vx, vy);
            if (len < 0.02) {
                vx = grid.x0 + (x + 0.5) * cell - out.cx;
                vy = grid.y0 + (y + 0.5) * cell - out.cy;
                len = std::hypot(vx, vy);
            }
            out.nx[i] = (float)(len > 0.0 ? vx / len : 1.0);
            out.ny[i] = (float)(len > 0.0 ? vy / len : 0.0);
        }
    }
    out.box[0] = std::floor((grid.x0 + ix0 * cell) / SNAP) * SNAP;
    out.box[1] = std::floor((grid.y0 + iy0 * cell) / SNAP) * SNAP;
    out.box[2] = std::ceil((grid.x0 + (ix1 + 1) * cell) / SNAP) * SNAP;
    out.box[3] = std::ceil((grid.y0 + (iy1 + 1) * cell) / SNAP) * SNAP;
    return true;
}

bool analysis::sampleEmitMap(const EmitMap& m, gen::Rng& r, MapSample& out) {
    const Grid& g = *m.grid;
    // uniform tries over the snapped box (not a list of cells): a borderline cell flipping
    // between render scales then moves only the particles that land on it
    for (int k = 0; k < MAX_TRIES; k++) {
        const double x = m.box[0] + r() * (m.box[2] - m.box[0]);
        const double y = m.box[1] + r() * (m.box[3] - m.box[1]);
        const double gx = std::floor((x - g.x0) / g.cellComp);
        const double gy = std::floor((y - g.y0) / g.cellComp);
        if (gx < 0.0 || gy < 0.0 || gx >= g.w || gy >= g.h) continue;
        const int i = (int)gy * g.w + (int)gx;
        if (m.mask[(size_t)i]) {
            out.x = x;
            out.y = y;
            out.cell = i;
            return true;
        }
    }
    return false;
}

std::vector<ParticleRecord> analysis::layoutParticles(const ParamValues& p, const ParticleLook& look, const ParticleSpace& space,
                                                      const EmitMap* map) {
    const std::string em = p.string("emitter");
    if (em == "layer") {
        if (!map || !map->grid) return {};
        // births bursting from the layer's pixels: along the edge normal, away from the point or
        // at the heading, Distance x (1 - Distance random x sqrt(u)) out, so most stay close
        const std::string mode = p.string("direction");
        const TitleVec2 pt = p.vec2("point");
        // Pachislop's heading turns counter-clockwise (y-up); ours is clockwise on screen (D16)
        const double ang = -p.number("heading") * DEG;
        const double distance = std::max(0.0, p.number("travel"));
        const double dr = p.number("distance_random") / 100.0;
        const double cone = p.number("cone") * DEG;
        return buildParticles(look, [&](gen::Rng& r, Birth& b) {
            MapSample s;
            if (!sampleEmitMap(*map, r, s)) return false;
            double a;
            if (mode == "angle") a = ang;
            else if (mode == "point" && std::hypot(s.x - pt.x, s.y - pt.y) > 1e-3) a = std::atan2(s.y - pt.y, s.x - pt.x);
            else a = std::atan2((double)map->ny[(size_t)s.cell], (double)map->nx[(size_t)s.cell]);
            a += (r() - 0.5) * cone;
            const double tf = 1.0 - dr * std::sqrt(r());
            b.dx = std::cos(a);
            b.dy = std::sin(a);
            b.mx = b.dx * distance * tf;
            b.my = b.dy * distance * tf;
            b.x = s.x + b.mx;
            b.y = s.y + b.my;
            // gradient by radius: 0 for the nearest particles, 1 for the farthest
            b.t = dr > 0.0 ? (tf - 1.0 + dr) / dr : 1.0;
            b.hasSpeed = true;
            b.speed = tf;
            b.fade = 1.0 - 0.5 * tf * tf;
            b.hasRgb = true;
            inkColor(*map, s.cell, b.rgb);
            return true;
        });
    }

    const std::string dist = p.string("distribution");
    const double falloff = p.number("falloff") / 100.0;
    const double travel = std::max(0.0, p.number("travel"));
    const double cone = p.number("cone") * DEG;
    const double ang = -p.number("angle") * DEG;
    const TitleVec2 radius = p.vec2("radius");
    const double rx = std::max(0.0, radius.x);
    const double ry = std::max(0.0, radius.y);

    std::function<void(gen::Rng&, Start&)> start;
    double R = 0.0;     // emitter radius (gradient by radius spans R + travel)
    if (em == "full") {
        const double W = space.canvasWidth / 2.0;
        const double H = space.canvasHeight / 2.0;
        const TitleVec2 o = space.documentToSpace.apply({0.0, 0.0});
        const TitleVec2 ax = space.documentToSpace.apply({W, 0.0});
        const TitleVec2 ay = space.documentToSpace.apply({0.0, H});
        R = std::hypot(W, H);
        start = [=](gen::Rng& r, Start& s) {
            const double fx = axis(r(), falloff);
            const double fy = axis(r(), falloff);
            s.x = o.x + fx * (ax.x - o.x) + fy * (ay.x - o.x);
            s.y = o.y + fx * (ax.y - o.y) + fy * (ay.y - o.y);
            const double a = r() * 2.0 * PI;
            unit(s.x - o.x, s.y - o.y, std::cos(a), std::sin(a), s.dx, s.dy);
            s.t = std::max(std::abs(fx), std::abs(fy));
        };
    } else {
        R = em == "point" ? 0.0 : em == "line" ? rx : std::max(rx, ry);
        start = [=](gen::Rng& r, Start& s) {
            const double a = r() * 2.0 * PI;
            const double ca = std::cos(a);
            const double sa = std::sin(a);
            if (em == "point") {
                s = {0.0, 0.0, ca, sa, 0.0};
            } else if (em == "ellipse") {
                const double rho = std::pow(r(), 0.5 + 1.5 * falloff);
                s.x = rx * rho * ca;
                s.y = ry * rho * sa;
                unit(rx * ca, ry * sa, ca, sa, s.dx, s.dy);
                s.t = rho;
            } else if (em == "rect") {
                const double fx = axis(r(), falloff);
                const double fy = axis(r(), falloff);
                s.x = rx * fx;
                s.y = ry * fy;
                unit(rx * fx, ry * fy, ca, sa, s.dx, s.dy);
                s.t = std::max(std::abs(fx), std::abs(fy));
            } else if (em == "ring") {
                // falloff tightens the band around the ring line
                const double u1 = r();
                const double u2 = r();
                const double rho = 1.0 + (u1 + u2 - 1.0) * 0.25 * (1.0 - 0.85 * falloff);
                s = {rx * rho * ca, ry * rho * sa, ca * ry, sa * rx, std::min(1.0, rho)};
            } else {
                const double fx = axis(r(), falloff);
                s = {rx * fx, 0.0, 0.0, sa < 0.0 ? -1.0 : 1.0, std::abs(fx)};
            }
        };
    }

    return buildParticles(look, [&](gen::Rng& r, Birth& b) {
        Start s;
        start(r, s);
        double ux, uy;
        unit(s.dx, s.dy, 1.0, 0.0, ux, uy);
        const double jitter = (r() - 0.5) * cone;
        if (dist == "burst") {
            const double tf = std::pow(r(), 0.6 + 1.6 * falloff);
            const double a = std::atan2(uy, ux) + jitter;
            b.dx = std::cos(a);
            b.dy = std::sin(a);
            const double go = travel * tf;
            b.mx = b.dx * go;
            b.my = b.dy * go;
            b.x = s.x + b.mx;
            b.y = s.y + b.my;
            b.t = (s.t * R + go) / std::max(1.0, R + travel);
            b.hasSpeed = true;
            b.speed = tf;
            b.fade = 1.0 - 0.5 * tf * tf;
            return true;
        }
        const double a = ang + jitter;
        b.dx = std::cos(a);
        b.dy = std::sin(a);
        if (dist == "stream") {
            const double tf = std::pow(r(), 0.6 + 1.6 * falloff);
            b.mx = b.dx * travel * tf;
            b.my = b.dy * travel * tf;
            b.x = s.x + b.mx;
            b.y = s.y + b.my;
            b.t = tf;
            b.fade = 1.0 - 0.6 * tf * tf;
            return true;
        }
        if (em == "point") {
            // a point has no area to scatter over: Distance is the cloud's radius
            const double rho = std::pow(r(), 0.5 + 1.5 * falloff);
            b.x = ux * travel * rho;
            b.y = uy * travel * rho;
            b.t = rho;
            return true;
        }
        b.x = s.x;
        b.y = s.y;
        b.t = s.t;
        return true;
    });
}

ParticleMotion analysis::readParticleMotion(const ParamValues& p) {
    ParticleMotion m;
    m.time = clamp01(p.number("time", 1.0));
    m.loop = p.flag("loop");
    m.gravity = p.number("gravity");
    m.spin = p.number("spin");
    m.twinkle = clamp01(p.number("twinkle"));
    m.fadeIn = clamp01(p.number("fade_in"));
    m.fadeOut = clamp01(p.number("fade_out"));
    return m;
}

ParticleState analysis::particleAt(const ParticleRecord& r, const ParticleMotion& m, bool spins, bool glint) {
    ParticleState s;
    const double phase = r.motionPhase;
    const double tau = m.loop ? fract(m.time + phase) : m.time;
    // eased out: fast at birth, at rest where Pachislop's still has it at time 1
    const double ease = 1.0 - (1.0 - tau) * (1.0 - tau);
    s.x = r.x + r.mx * (ease - 1.0);
    s.y = r.y + r.my * (ease - 1.0) - m.gravity * tau * tau;
    s.rot = r.rot;
    // our spin turns clockwise on screen; the record's rotation counter-clockwise (+y up)
    if (spins) s.rot += -m.spin * DEG * tau * (0.5 + fract(phase * 64.0));
    double f = 1.0;
    if (m.fadeIn > 0.0) f *= smoothstep(0.0, m.fadeIn, tau);
    if (m.fadeOut > 0.0) f *= 1.0 - smoothstep(1.0 - m.fadeOut, 1.0, tau);
    if (glint && m.twinkle > 0.0) {
        // peaks of 3.2 whose mean over a cycle is 1: a twinkle keeps the field's brightness
        const double c = 0.5 + 0.5 * std::cos(2.0 * PI * (PARTICLE_TWINKLE_CYCLES * m.time + phase));
        f *= mixLinear(1.0, 3.2 * c * c * c, m.twinkle);
    }
    s.colorScale = f;
    return s;
}

bool analysis::gridInkBox(const Grid& g, float threshold, double box[4]) {
    int ix0 = g.w, iy0 = g.h, ix1 = -1, iy1 = -1;
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) {
            if (!(g.alpha[(size_t)y * g.w + x] > threshold)) continue;
            ix0 = std::min(ix0, x);
            iy0 = std::min(iy0, y);
            ix1 = std::max(ix1, x);
            iy1 = std::max(iy1, y);
        }
    }
    if (ix1 < ix0) return false;
    box[0] = g.x0 + ix0 * g.cellComp;
    box[1] = g.y0 + iy0 * g.cellComp;
    box[2] = g.x0 + (ix1 + 1) * g.cellComp;
    box[3] = g.y0 + (iy1 + 1) * g.cellComp;
    return true;
}

GridSpec analysis::particleGridSpec(const double b[4]) {
    // the ink box +- 8 canvas px grown outward to SNAP multiples: the lattice stays identical
    // when a measured box wobbles by a pixel between scales
    const double x0 = std::floor((b[0] - 8.0) / SNAP) * SNAP;
    const double y0 = std::floor((b[1] - 8.0) / SNAP) * SNAP;
    const double x1 = std::ceil((b[2] + 8.0) / SNAP) * SNAP;
    const double y1 = std::ceil((b[3] + 8.0) / SNAP) * SNAP;
    // the cell from the canvas-space area: the same at every render scale
    const double cell = std::max(3.0, std::ceil(std::sqrt(std::max(1.0, (x1 - x0) * (y1 - y0)) / GRID_CELLS)));
    GridSpec s;
    s.x0 = x0;
    s.y0 = y0;
    s.cellComp = cell;
    s.w = std::max(1, (int)std::ceil((x1 - x0) / cell));
    s.h = std::max(1, (int)std::ceil((y1 - y0) / cell));
    s.maxTaps = 8;
    s.ceilTaps = true;
    s.packing = GridPacking::Color;
    return s;
}

std::vector<InkBlot> analysis::layoutInkSplatter(const Grid& g, const ParamValues& p, double seed) {
    std::vector<InkBlot> blots;
    if (g.empty() || g.alpha.size() < (size_t)g.w * g.h) return blots;
    const double spread = std::max(1.0, p.number("spread"));
    const double size = p.number("size");
    const std::vector<float> dist = gridDistance(g.w, g.h, g.alpha);
    const double cell = g.cellComp;
    const double INF = std::numeric_limits<double>::infinity();
    double x0 = INF, y0 = INF, x1 = -INF, y1 = -INF;
    // canvas px about the canvas centre: the same at every canvas size and render scale
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) {
            if (!(g.alpha[(size_t)y * g.w + x] >= 0.5f)) continue;
            x0 = std::min(x0, g.x0 + x * cell);
            y0 = std::min(y0, g.y0 + y * cell);
            x1 = std::max(x1, g.x0 + (x + 1) * cell);
            y1 = std::max(y1, g.y0 + (y + 1) * cell);
        }
    }
    const int count = (int)jsRound(p.number("amount") / 100.0 * INK_MAX_DROPS);
    if (x0 > x1 || count <= 0) return blots;
    const double cx = 0.5 * (x0 + x1);
    const double cy = 0.5 * (y0 + y1);
    // Pachislop's angle turns counter-clockwise (y-up); ours is clockwise on screen (D16)
    const double fixedDir = -p.number("angle") * DEG;
    const bool radial = p.string("direction") != "directional";
    const double streakShare = p.number("streaks") / 100.0;
    const double streakLen = p.number("streak_length");
    const double satAmount = p.number("satellites") / 100.0;
    auto push = [&](double ax, double ay, double bx, double by, double ra, double rb, double s) { blots.push_back({ax, ay, bx, by, ra, rb, s}); };
    gen::Rng r(seed * 977.0 + 3.0, gen::Rng::Seeding::EffectsB);
    int placed = 0;
    for (int tries = 0; placed < count && tries < count * 60; tries++) {
        const double px = x0 - spread + r() * (x1 - x0 + 2.0 * spread);
        const double py = y0 - spread + r() * (y1 - y0 + 2.0 * spread);
        const double gx = std::floor((px - g.x0) / cell);
        const double gy = std::floor((py - g.y0) / cell);
        const double d = gx >= 0.0 && gy >= 0.0 && gx < g.w && gy < g.h ? (double)dist[(size_t)gy * g.w + (size_t)gx] * cell : INF;
        if (d > spread) continue;
        // dense and heavy at the edge, thinning out with distance; few drops land on the letters
        const double nearness = 1.0 - d / spread;
        if (gen::hashN({seed, (double)tries, 0.0}) > (d == 0.0 ? 0.25 : std::pow(nearness, 1.2))) continue;
        placed++;
        // everything below is keyed by the try, so a cell flipping between render scales
        // changes only this drop instead of re-rolling every one after it
        auto h = [&](double n) { return gen::hashN({seed, (double)tries, n}); };
        const double rad = size * (0.2 + 0.8 * std::pow(h(1), 2.0)) * (0.45 + 0.55 * nearness);
        const double base = radial ? std::atan2(py - cy, px - cx) : fixedDir;
        const double dir = base + (h(2) - 0.5) * (radial ? 0.5 : 0.3);
        const double ux = std::cos(dir);
        const double uy = std::sin(dir);
        double hx = px;
        double hy = py;
        if (h(3) < streakShare && streakLen > 0.0) {
            // drag streak: a thin tail at the landing point, the drop's bulk piled up at the far end
            const double len = streakLen * (0.35 + 0.65 * h(4)) * (0.5 + 0.5 * nearness);
            hx = px + ux * len;
            hy = py + uy * len;
            push(px, py, hx, hy, std::max(0.6, rad * 0.22), rad * 0.95, h(5));
        } else {
            push(px, py, px, py, rad, rad, h(5));
        }
        // satellites: a few droplets around the drop, thrown further along its direction
        const int sats = (int)jsRound(satAmount * (1.0 + 5.0 * h(6)) * (0.4 + 0.6 * nearness));
        for (int j = 0; j < sats; j++) {
            auto hs = [&](double n) { return gen::hashN({seed, (double)tries, 10.0 + j * 4 + n}); };
            const double a = dir + (hs(0) - 0.5) * 2.4;
            const double reach = rad * (1.4 + 2.2 * std::pow(hs(1), 1.5));
            const double sr = std::max(0.6, rad * (0.06 + 0.2 * std::pow(hs(2), 2.0)));
            const double sx = hx + std::cos(a) * reach;
            const double sy = hy + std::sin(a) * reach;
            push(sx, sy, sx, sy, sr, sr, hs(3));
        }
    }
    return blots;
}
