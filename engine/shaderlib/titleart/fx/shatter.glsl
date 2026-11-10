// S83's Shatter (§6.6.17), after Pachislop src/effects/b/shatter.ts by SafEight, used with the
// author's permission: the layer cut into glass shards (kinked spokes from the impact crossed by
// chord rings, denser toward the impact), Voronoi cells of jittered-grid seeds or triangles of a
// jittered grid, laid out over the layer's box padded and snapped to 32 canvas px; the pieces near
// the impact are thrown outward, turned, dropped and faded, each keeping its texture (sampled at its
// rest position) and catching a highlight on its cut edges.
//
// Pachislop builds the polygons on the CPU and draws a mesh; here every piece is a function of its
// index (its geometry and its five randoms from the seed), so one fragment pass finds, for each
// pixel, the pieces that may have moved over it (the cells within reach of their largest
// displacement), maps the pixel back to each one's rest position, tests it against the piece and
// keeps the two most displaced that cover it (Pachislop draws the most displaced last). The
// distance to the cut edges is exact: the distance to the piece's polygon edges, or for a Voronoi
// cell to its bisectors.
//
// Coordinates: canvas px from the canvas centre, y up (Pachislop's comp space).

#ifndef TITLEART_FX_SHATTER_GLSL
#define TITLEART_FX_SHATTER_GLSL

const float TFX_FAR = 1.0e5;

// the block: p2 = (pattern 0 glass 1 voronoi 2 triangles, pieces, impact canvas px (y up)),
// p3 = (radius canvas px, strength canvas px, rotation rad, fall canvas px), p4 = (progress, fade,
// gap canvas px, lost 0..1), p5 = (edge colour linear, edge highlight 0..3), p6 = (seed, cracks only,
// canvas centre target px), p7.x = the highlight width canvas px

float tfx_sh(float a, float b) {
    return hash12(vec2(a * 1.731 + ta.p[6].x * 0.917, b * 3.117 + ta.p[6].x * 0.311));
}

// the layout box: the layer box, canvas px y up, padded by 16 and snapped outward to 32
vec4 tfx_shatterBox() {
    vec4 b = fx_layerBox();
    vec2 c = ta.p[6].zw;
    vec2 lo = vec2(b.x - c.x, c.y - b.w) / TA_PX, hi = vec2(b.z - c.x, c.y - b.y) / TA_PX;
    return vec4(floor((lo - 16.0) / 32.0) * 32.0, ceil((hi + 16.0) / 32.0) * 32.0);
}

// the signed distance from p to the line a -> b (positive on its left, the inside of a CCW polygon)
float tfx_lineSide(vec2 p, vec2 a, vec2 b) {
    vec2 e = b - a;
    float l = length(e);
    return l > 1e-6 ? (e.x * (p.y - a.y) - e.y * (p.x - a.x)) / l : 1e9;
}

// inside a convex polygon of 3 or 4 points (either winding): the distance to its nearest edge,
// negative outside
float tfx_polyInside(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d, bool quad) {
    float s = sign(tfx_lineSide(c, a, b) + (quad ? tfx_lineSide(d, a, b) : 0.0));
    s = s == 0.0 ? 1.0 : s;
    float e = s * tfx_lineSide(p, a, b);
    e = min(e, s * tfx_lineSide(p, b, c));
    if (quad) {
        e = min(e, s * tfx_lineSide(p, c, d));
        e = min(e, s * tfx_lineSide(p, d, a));
    } else {
        e = min(e, s * tfx_lineSide(p, c, a));
    }
    return e;
}

vec2 tfx_polyCentroid(vec2 a, vec2 b, vec2 c, vec2 d, bool quad) {
    if (!quad) return (a + b + c) / 3.0;
    float a1 = abs(tfx_lineSide(c, a, b) * length(b - a)), a2 = abs(tfx_lineSide(d, a, c) * length(c - a));
    vec2 c1 = (a + b + c) / 3.0, c2 = (a + c + d) / 3.0;
    return (c1 * a1 + c2 * a2) / max(a1 + a2, 1e-6);
}

// ---- the partitions ----

// Voronoi: cols x rows seeds over the box
ivec2 tfx_vGrid(vec4 B, float n) {
    vec2 s = max(B.zw - B.xy, vec2(1.0));
    int cols = max(1, int(floor(sqrt(n * s.x / s.y) + 0.5)));
    int rows = max(1, int(floor(n / float(cols) + 0.5)));
    return ivec2(cols, rows);
}
vec2 tfx_vSeed(vec4 B, ivec2 g, ivec2 c) {
    vec2 cell = (B.zw - B.xy) / vec2(g);
    return B.xy + (vec2(c) + 0.5 + (vec2(tfx_sh(float(c.x), float(c.y) + 0.5), tfx_sh(float(c.x) + 0.25, float(c.y) + 7.5)) - 0.5) * 0.9) * cell;
}
// x inside the cell of seed c: the distance to the cell's nearest bisector (negative outside)
float tfx_vInside(vec4 B, ivec2 g, ivec2 c, vec2 x) {
    vec2 s = tfx_vSeed(B, g, c);
    float ds = dot(x - s, x - s);
    float e = 1e9;
    for (int j = -2; j <= 2; j++) {
        for (int i = -2; i <= 2; i++) {
            ivec2 o = c + ivec2(i, j);
            if ((i == 0 && j == 0) || o.x < 0 || o.y < 0 || o.x >= g.x || o.y >= g.y) continue;
            vec2 t = tfx_vSeed(B, g, o);
            float l = length(t - s);
            e = min(e, (dot(x - t, x - t) - ds) / max(2.0 * l, 1e-6));
        }
    }
    return e;
}

// triangles: a (cols + 1) x (rows + 1) jittered grid (box-edge points slide along their edge only,
// indices -1 and past the last at FAR), each quad cut along a random diagonal
ivec2 tfx_tGrid(vec4 B, float n) {
    vec2 s = max(B.zw - B.xy, vec2(1.0));
    int cols = max(1, int(floor(sqrt(n * 0.5 * s.x / s.y) + 0.5)));
    int rows = max(1, int(floor(n * 0.5 / float(cols) + 0.5)));
    return ivec2(cols, rows);
}
vec2 tfx_tPoint(vec4 B, ivec2 g, ivec2 v) {
    vec2 cell = (B.zw - B.xy) / vec2(g);
    vec2 j = (vec2(tfx_sh(float(v.x) + 0.5, float(v.y) + 11.0), tfx_sh(float(v.x) + 9.5, float(v.y) + 3.0)) - 0.5) * 0.7 * cell;
    float x = v.x < 0 ? -TFX_FAR : v.x > g.x ? TFX_FAR : B.x + float(v.x) * cell.x + (v.x > 0 && v.x < g.x ? j.x : 0.0);
    float y = v.y < 0 ? -TFX_FAR : v.y > g.y ? TFX_FAR : B.y + float(v.y) * cell.y + (v.y > 0 && v.y < g.y ? j.y : 0.0);
    return vec2(x, y);
}
// the corners of triangle `tri` (0, 1) of cell c
void tfx_tTriangle(vec4 B, ivec2 g, ivec2 c, int tri, out vec2 a, out vec2 b, out vec2 d) {
    vec2 p0 = tfx_tPoint(B, g, c), p1 = tfx_tPoint(B, g, c + ivec2(1, 0)), p2 = tfx_tPoint(B, g, c + ivec2(1, 1)), p3 = tfx_tPoint(B, g, c + ivec2(0, 1));
    bool ac = tfx_sh(float(c.x) + 3.3, float(c.y) + 17.0) < 0.5;
    if (ac) {
        a = p0;
        b = tri == 0 ? p1 : p2;
        d = tri == 0 ? p2 : p3;
    } else {
        a = tri == 0 ? p0 : p1;
        b = tri == 0 ? p1 : p2;
        d = p3;
    }
}

// glass: K spokes and M rings (Pachislop glassCells), the impact inside the box or outside it
struct TfxGlass {
    vec2 impact;
    int K, M;
    bool inside;
    float sp, start, rNear, rc;
};
TfxGlass tfx_glass(vec4 B, float n) {
    TfxGlass g;
    g.impact = ta.p[2].zw;
    g.K = int(min(40.0, max(5.0, floor(sqrt(n * 0.9) + 0.5))));
    g.M = max(1, int(floor(n / float(g.K) + 0.5)));
    vec2 ip = g.impact;
    g.inside = ip.x > B.x && ip.x < B.z && ip.y > B.y && ip.y < B.w;
    g.rc = 1.0;
    g.rc = max(g.rc, length(B.xy - ip));
    g.rc = max(g.rc, length(B.zy - ip));
    g.rc = max(g.rc, length(B.xw - ip));
    g.rc = max(g.rc, length(B.zw - ip));
    g.rNear = g.inside ? 0.0 : length(vec2(max(max(B.x - ip.x, 0.0), ip.x - B.z), max(max(B.y - ip.y, 0.0), ip.y - B.w)));
    if (g.inside) {
        g.sp = 2.0 * TA_PI / float(g.K);
        g.start = tfx_sh(91.0, 3.0) * 2.0 * TA_PI;
    } else {
        // the corner angles about the direction of the box centre (all within +-90 degrees)
        float toC = atan(0.5 * (B.y + B.w) - ip.y, 0.5 * (B.x + B.z) - ip.x);
        float lo = 0.0, hi = 0.0;
        vec2 corners[4];
        corners[0] = B.xy;
        corners[1] = B.zy;
        corners[2] = B.xw;
        corners[3] = B.zw;
        for (int i = 0; i < 4; i++) {
            float d = atan(corners[i].y - ip.y, corners[i].x - ip.x) - toC;
            float w = atan(sin(d), cos(d));
            lo = min(lo, w);
            hi = max(hi, w);
        }
        lo -= 0.02;
        hi += 0.02;
        g.sp = (hi - lo) / float(max(g.K - 1, 1));
        g.start = toC + lo;
    }
    return g;
}
float tfx_gBase(TfxGlass g, int k) {
    float j = (tfx_sh(float(k) + 0.5, 21.0) - 0.5) * 0.5;
    if (!g.inside && (k == 0 || k == g.K - 1)) j = 0.0;
    return g.start + (float(k) + j) * g.sp;
}
float tfx_gRad(TfxGlass g, int j) {
    if (j <= 0) return g.rNear;
    if (j >= g.M) return TFX_FAR;
    return g.rNear + (g.rc - g.rNear) * pow((float(j) + (tfx_sh(float(j) + 0.5, 33.0) - 0.5) * 0.5) / float(g.M), 1.5);
}
// spoke k's crossing of ring j (j >= 1), kinked and wobbled
vec2 tfx_gPoint(TfxGlass g, int k, int j) {
    if (j <= 0) return g.impact;
    int kk = g.inside ? k - (k / g.K) * g.K : k;
    bool outer = j >= g.M;
    bool straight = !g.inside && (kk == 0 || kk == g.K - 1);
    float a = tfx_gBase(g, kk) + (k >= g.K ? 2.0 * TA_PI : 0.0) + (outer || straight ? 0.0 : (tfx_sh(float(kk) + 0.5, float(j) + 41.0) - 0.5) * 0.2 * g.sp);
    float r0 = tfx_gRad(g, j - 1), r1 = tfx_gRad(g, j);
    float rr = outer ? TFX_FAR : r1 + (tfx_sh(float(kk) + 7.5, float(j) + 53.0) - 0.5) * 0.35 * (r1 - r0);
    return g.impact + vec2(cos(a), sin(a)) * rr;
}
// the corners of glass piece (k, j, tri): a triangle at the impact for j = 0, else a quad or, cut
// on the diagonal, one of its two triangles
bool tfx_gPiece(TfxGlass g, int k, int j, int tri, out vec2 a, out vec2 b, out vec2 c, out vec2 d) {
    if (j == 0) {
        a = g.impact;
        b = tfx_gPoint(g, k, 1);
        c = tfx_gPoint(g, k + 1, 1);
        d = c;
        return false;
    }
    vec2 A0 = tfx_gPoint(g, k, j), A1 = tfx_gPoint(g, k, j + 1), B1 = tfx_gPoint(g, k + 1, j + 1), B0 = tfx_gPoint(g, k + 1, j);
    bool split = tfx_sh(float(k) + 5.5, float(j) + 61.0) < 0.25;
    if (!split) {
        a = A0;
        b = A1;
        c = B1;
        d = B0;
        return true;
    }
    a = A0;
    b = tri == 0 ? A1 : B1;
    c = tri == 0 ? B1 : B0;
    d = c;
    return false;
}

// ---- the pieces ----

// a piece's fate: (displacement, rotation, fade, highlight factor); lost pieces come back with w < 0
vec4 tfx_pieceMotion(vec2 centre, float id, out vec2 t) {
    float r1 = tfx_sh(id, 101.0), r2 = tfx_sh(id, 102.0), r3 = tfx_sh(id, 103.0), r4 = tfx_sh(id, 104.0), r5 = tfx_sh(id, 105.0);
    vec2 d = centre - ta.p[2].zw;
    float dist = length(d);
    vec2 dir = dist > 1e-3 ? d / dist : vec2(cos(r5 * 2.0 * TA_PI), sin(r5 * 2.0 * TA_PI));
    float f0 = max(0.0, 1.0 - dist / max(ta.p[3].x, 1e-3));
    float f = f0 * f0 * (3.0 - 2.0 * f0);
    bool cracks = ta.p[6].y > 0.5;
    float progress = ta.p[4].x;
    float strength = cracks ? 0.0 : ta.p[3].y * progress;
    float push = strength * f * (0.6 + 0.8 * r2);
    float side = strength * f * (r3 - 0.5) * 0.5;
    float rot = cracks ? 0.0 : (2.0 * r4 - 1.0) * ta.p[3].z * f * progress;
    float fall = cracks ? 0.0 : ta.p[3].w * progress * progress * f;
    t = vec2(dir.x * push - dir.y * side, dir.y * push + dir.x * side - fall);
    float moved = length(t) / max(ta.p[3].y + ta.p[3].w, 1e-3);
    float fade = 1.0 - ta.p[4].y * clamp(moved, 0.0, 1.0);
    // the lost pieces were blown out of the frame, most often right at the impact
    if (r1 < ta.p[4].w * 2.5 * f) fade = -1.0;
    return vec4(t, rot, fade);
}

// the pixel at canvas point X carried back to piece-local rest coordinates
vec2 tfx_restPoint(vec2 X, vec2 centre, vec2 t, float rot) {
    vec2 q = X - centre - t;
    float c = cos(-rot), s = sin(-rot);
    return centre + vec2(c * q.x - s * q.y, s * q.x + c * q.y);
}

// one candidate piece's contribution: its colour at X (premultiplied) and how far it moved
void tfx_shatterTry(vec2 X, vec2 centre, float id, int pattern, ivec4 key, vec4 B, ivec2 grid, TfxGlass gl,
                    inout vec4 top, inout float topMoved, inout vec4 second) {
    vec2 t;
    vec4 m = tfx_pieceMotion(centre, id, t);
    if (m.w < 0.0) return;
    vec2 x = tfx_restPoint(X, centre, t, m.z);
    float e;
    if (pattern == 1) {
        e = tfx_vInside(B, grid, key.xy, x);
    } else if (pattern == 2) {
        vec2 a, b, d;
        tfx_tTriangle(B, grid, key.xy, key.z, a, b, d);
        e = tfx_polyInside(x, a, b, d, d, false);
    } else {
        vec2 a, b, c, d;
        bool quad = tfx_gPiece(gl, key.x, key.y, key.z, a, b, c, d);
        e = tfx_polyInside(x, a, b, c, d, quad);
    }
    // the gap insets every cut edge by half its width
    e -= 0.5 * ta.p[4].z;
    float edgePx = e * TA_PX;
    float cov = clamp(edgePx + 0.5, 0.0, 1.0);
    if (cov <= 0.0) return;
    // the rest position in target px
    vec2 c0 = ta.p[6].zw;
    vec2 src = vec2(c0.x + x.x * TA_PX, c0.y - x.y * TA_PX);
    vec4 s = textureLod(TA_TEX0, src / TA_SIZE, 0.0);
    float w = max(ta.p[7].x * TA_PX, 0.5);
    float hi = 0.8 * exp(-edgePx / w) + 0.3 * exp(-edgePx / (4.0 * w));
    float factor = 0.55 + 0.45 * tfx_sh(id, 105.0);
    vec4 col = vec4(s.rgb + ta.p[5].rgb * (hi * ta.p[5].w * factor * s.a * 1.6), s.a) * cov * m.w;
    float moved = length(t) + abs(m.z) * 1e-3;
    if (moved >= topMoved) {
        second = top;
        top = col;
        topMoved = moved;
    } else {
        second = tfx_over(col, second);
    }
}

// Shatter: every piece that may cover this pixel, the two most displaced composited.
// TA_TEX0 the input, TA_RAW1 its bounds
vec4 tfx_shatter() {
    vec2 c0 = ta.p[6].zw;
    vec2 X = vec2(gl_FragCoord.x - c0.x, c0.y - gl_FragCoord.y) / TA_PX;
    int pattern = int(ta.p[2].x + 0.5);
    float n = ta.p[2].y;
    vec4 B = tfx_shatterBox();
    // how far a piece may have come: the push with its sideways scatter, the fall and a turned corner
    float progress = ta.p[4].x;
    float reach = ta.p[6].y > 0.5 ? 0.0 : 1.45 * ta.p[3].y * progress + ta.p[3].w * progress * progress;
    vec4 top = vec4(0.0), second = vec4(0.0);
    float topMoved = -1.0;
    TfxGlass gl = tfx_glass(B, n);
    if (pattern == 1 || pattern == 2) {
        ivec2 grid = pattern == 1 ? tfx_vGrid(B, n) : tfx_tGrid(B, n);
        vec2 cell = (B.zw - B.xy) / vec2(grid);
        reach += 0.75 * ta.p[3].z * max(cell.x, cell.y);
        ivec2 c = ivec2(floor((X - B.xy) / cell));
        ivec2 r = ivec2(min(ceil(vec2(reach) / cell) + 1.0, vec2(10.0)));
        int lo = pattern == 1 ? 0 : -1;
        for (int j = -11; j <= 11; j++) {
            if (j < -r.y || j > r.y) continue;
            for (int i = -11; i <= 11; i++) {
                if (i < -r.x || i > r.x) continue;
                ivec2 k = c + ivec2(i, j);
                if (k.x < lo || k.y < lo || k.x >= grid.x + (pattern == 1 ? 0 : 1) || k.y >= grid.y + (pattern == 1 ? 0 : 1)) continue;
                if (pattern == 1) {
                    tfx_shatterTry(X, tfx_vSeed(B, grid, k), float(k.y * 1031 + k.x), 1, ivec4(k, 0, 0), B, grid, gl, top, topMoved, second);
                } else {
                    for (int h = 0; h < 2; h++) {
                        vec2 a, b, d;
                        tfx_tTriangle(B, grid, k, h, a, b, d);
                        if (max(max(abs(a.x), abs(b.x)), abs(d.x)) > 0.5 * TFX_FAR) continue;
                        if (max(max(abs(a.y), abs(b.y)), abs(d.y)) > 0.5 * TFX_FAR) continue;
                        tfx_shatterTry(X, (a + b + d) / 3.0, float((k.y + 1) * 2063 + (k.x + 1) * 2 + h), 2, ivec4(k, h, 0), B, grid, gl, top, topMoved, second);
                    }
                }
            }
        }
    } else {
        // glass: the spoke sector and ring the pixel's rest lies in, and the ones within reach
        vec2 d = X - gl.impact;
        float rho = length(d);
        float th = atan(d.y, d.x);
        int k0 = 0;
        float best = 1e9;
        for (int k = 0; k < 40; k++) {
            if (k >= gl.K) break;
            float da = th - tfx_gBase(gl, k) - 0.5 * gl.sp;
            da = abs(atan(sin(da), cos(da)));
            if (da < best) {
                best = da;
                k0 = k;
            }
        }
        int j0 = 0;
        for (int j = 1; j < 40; j++) {
            if (j >= gl.M) break;
            if (rho > tfx_gRad(gl, j)) j0 = j;
        }
        float ringW = (gl.rc - gl.rNear) / float(gl.M);
        int dj = int(min(ceil(reach / max(ringW, 1.0)) + 1.0, 8.0));
        int dk = int(min(ceil(reach / max(rho * gl.sp, 1.0)) + 1.0, 8.0));
        dk = min(dk, gl.K);
        int kmax = gl.inside ? gl.K : gl.K - 1;
        for (int jj = -8; jj <= 8; jj++) {
            if (jj < -dj || jj > dj) continue;
            int j = j0 + jj;
            if (j < 0 || j >= gl.M) continue;
            for (int kk = -8; kk <= 8; kk++) {
                if (kk < -dk || kk > dk) continue;
                int k = k0 + kk;
                if (gl.inside) k = k - int(floor(float(k) / float(gl.K))) * gl.K;
                else if (k < 0 || k >= kmax) continue;
                for (int h = 0; h < 2; h++) {
                    vec2 a, b, c, e;
                    bool quad = tfx_gPiece(gl, k, j, h, a, b, c, e);
                    if (quad && h == 1) continue;
                    if (j == 0 && h == 1) continue;
                    float id = float(j * 97 + k) * 2.0 + float(h);
                    tfx_shatterTry(X, tfx_polyCentroid(a, b, c, e, quad), id, 0, ivec4(k, j, h, 0), B, ivec2(1), gl, top, topMoved, second);
                }
            }
        }
        // an impact outside the box: Pachislop's wedges fill the rest of the circle, outside the
        // padded box, where the layer has no ink: nothing of them is drawn
    }
    return tfx_over(top, second);
}

#endif // TITLEART_FX_SHATTER_GLSL
