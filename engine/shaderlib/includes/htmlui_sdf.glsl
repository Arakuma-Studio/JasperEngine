// Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// SPDX-License-Identifier: MIT
// roundedRectSDF (adapted), rectEdgeFootprint, sdfCoverage, erfApprox and the Gaussian edge are OpenEngine's
// (Engine/Modules/Rendering/Shaders/UI/sdf_functions.glsl, MIT, NOTICE.md); the clip, shadow,
// border and distance-field text helpers below them are JasperEngine's (S16).
//
// The HTML UI's analytic shapes: rounded rectangles with elliptical corners, antialiased from the
// screen derivatives of the position they are evaluated at (any 2D, 3D or perspective transform),
// at any sample count. Rectangles are (x, y, width, height); radii are (top-left, top-right,
// bottom-right, bottom-left), horizontal and vertical.

#ifndef HTMLUI_SDF_GLSL
#define HTMLUI_SDF_GLSL

// The analytic clip, the texture's kind and the distance-field text parameters of a draw (S16 R6,
// R12, R16, R17); RmlRenderer's ClipParams
uniform u_fs_htmluiClip {
    vec4 rect[2];       // x, y, width, height: document px before the element transform
    vec4 radiiX[2];     // top-left, top-right, bottom-right, bottom-left
    vec4 radiiY[2];
    vec4 params;        // x: clip shapes (0, 1 or 2); y: texture kind (0 RGBA, 1 R8 coverage, 2 MTSDF)
    vec4 text;          // MTSDF: x: effect (0 fill, 1 dilated, 2 blurred), y: dilation, z: blur sigma
                        // (document px), w: the atlas distance range (texels each side of the outline)
} clip;

// Signed distance to an elliptical corner whose ellipse is centred at c with semi-axes r, for a
// point d = p - c already turned to the corner's quadrant (both positive): equal semi-axes keep
// the exact circular distance; unequal ones take the gradient-normalised first-order distance,
// exact along both axes. Positive inside.
float ellipseCornerSDF(vec2 d, vec2 r){
    if (r.x == r.y)
        return -(length(d) - r.x);
    float k1 = length(d / r);
    float k2 = length(d / (r * r));
    return -((k1 - 1.0) * k1 / max(k2, 1e-12));
}

// Signed distance to a rounded rectangle with elliptical corners: positive inside, negative
// outside, zero on the boundary. Each corner's ellipse applies where p is past its centre on both
// axes, whatever half of the rectangle that is: a CSS radius may reach past the middle of a side
// (`border-radius: 20px / 50px` on a 60 px box, `100% 0`), the shape is the rectangle cut by every
// corner that reaches p (adapted from OpenEngine's, which took the corner of p's quadrant and
// limited radii to half the side).
float roundedRectSDF(vec2 p, vec4 rect, vec4 radX, vec4 radY){
    vec2 p0 = rect.xy;
    vec2 p1 = rect.xy + rect.zw;
    vec2 q = abs(p - (rect.xy + rect.zw * 0.5)) - rect.zw * 0.5;
    float sd = -max(q.x, q.y);
    // top-left, top-right, bottom-right, bottom-left
    vec2 r0 = vec2(radX.x, radY.x);
    if (r0.x > 0.0 && r0.y > 0.0){
        vec2 d = (p0 + r0) - p;
        if (d.x > 0.0 && d.y > 0.0)
            sd = min(sd, ellipseCornerSDF(d, r0));
    }
    vec2 r1 = vec2(radX.y, radY.y);
    if (r1.x > 0.0 && r1.y > 0.0){
        vec2 d = vec2(p.x - (p1.x - r1.x), (p0.y + r1.y) - p.y);
        if (d.x > 0.0 && d.y > 0.0)
            sd = min(sd, ellipseCornerSDF(d, r1));
    }
    vec2 r2 = vec2(radX.z, radY.z);
    if (r2.x > 0.0 && r2.y > 0.0){
        vec2 d = p - (p1 - r2);
        if (d.x > 0.0 && d.y > 0.0)
            sd = min(sd, ellipseCornerSDF(d, r2));
    }
    vec2 r3 = vec2(radX.w, radY.w);
    if (r3.x > 0.0 && r3.y > 0.0){
        vec2 d = vec2((p0.x + r3.x) - p.x, p.y - (p1.y - r3.y));
        if (d.x > 0.0 && d.y > 0.0)
            sd = min(sd, ellipseCornerSDF(d, r3));
    }
    return sd;
}

// How far along the normal of the edge of rect nearest p one screen pixel reaches, in the units
// of p, from p's screen derivatives (exact under non-uniform scales and skews too)
float rectEdgeFootprint(vec2 p, vec4 rect, vec2 dpdx, vec2 dpdy){
    vec2 halfSize = rect.zw * 0.5;
    vec2 q = abs(p - (rect.xy + halfSize)) - halfSize;
    vec2 n = (q.x > 0.0 && q.y > 0.0) ? normalize(q) : (q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return max(length(vec2(dot(dpdx, n), dot(dpdy, n))), 1e-4);
}

// Box-filter coverage of the edge at signed distance sd (positive inside) for a pixel footprint
// of w: exact for a straight edge at every sub-pixel phase, and area-conserving for a band taken
// as coverage(outer) - coverage(inner)
float sdfCoverage(float sd, float w){
    return clamp(sd / w + 0.5, 0.0, 1.0);
}

// Abramowitz & Stegun approximation of erf(x), max error ~1.5e-7
float erfApprox(float x){
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

// The Gaussian edge of a CSS blur (sigma = blur radius / 2) at signed distance sd, widened by the
// pixel's own footprint (pixelSigma, in the units of sd)
float gaussianEdge(float sd, float sigma, float pixelSigma){
    float sigmaEff = sqrt(sigma * sigma + pixelSigma * pixelSigma);
    return 0.5 + 0.5 * erfApprox(sd / (max(sigmaEff, 1e-4) * 1.4142135));
}

// The analytic clip (S16 R6): the coverage of a rounded rectangle at p
float htmluiClipShape(vec2 p, vec2 dpdx, vec2 dpdy, vec4 rect, vec4 radX, vec4 radY){
    return sdfCoverage(roundedRectSDF(p, rect, radX, radY), rectEdgeFootprint(p, rect, dpdx, dpdy));
}

// The clip of u_fs_htmluiClip at p (1 without one)
float htmluiClipCoverage(vec2 p, vec2 dpdx, vec2 dpdy){
    float coverage = 1.0;
    if (clip.params.x > 0.5){
        coverage = htmluiClipShape(p, dpdx, dpdy, clip.rect[0], clip.radiiX[0], clip.radiiY[0]);
        if (clip.params.x > 1.5)
            coverage *= htmluiClipShape(p, dpdx, dpdy, clip.rect[1], clip.radiiX[1], clip.radiiY[1]);
    }
    return coverage;
}

// The horizontal extent [left, right] of a rounded rectangle on the row y (inside its height)
vec2 roundedRectRow(float y, vec4 rect, vec4 radX, vec4 radY){
    float y0 = rect.y;
    float y1 = rect.y + rect.w;
    float left = rect.x;
    float right = rect.x + rect.z;
    // the corner of each side the row passes, its distance into the curve
    float rxl = (y - y0 < radY.x) ? radX.x : ((y1 - y < radY.w) ? radX.w : 0.0);
    float ryl = (y - y0 < radY.x) ? radY.x : ((y1 - y < radY.w) ? radY.w : 1.0);
    float dyl = (y - y0 < radY.x) ? (radY.x - (y - y0)) : ((y1 - y < radY.w) ? (radY.w - (y1 - y)) : 0.0);
    float rxr = (y - y0 < radY.y) ? radX.y : ((y1 - y < radY.z) ? radX.z : 0.0);
    float ryr = (y - y0 < radY.y) ? radY.y : ((y1 - y < radY.z) ? radY.z : 1.0);
    float dyr = (y - y0 < radY.y) ? (radY.y - (y - y0)) : ((y1 - y < radY.z) ? (radY.z - (y1 - y)) : 0.0);
    if (rxl > 0.0 && ryl > 0.0){
        float t = clamp(dyl / ryl, 0.0, 1.0);
        left += rxl - rxl * sqrt(1.0 - t * t);
    }
    if (rxr > 0.0 && ryr > 0.0){
        float t = clamp(dyr / ryr, 0.0, 1.0);
        right -= rxr - rxr * sqrt(1.0 - t * t);
    }
    return vec2(left, right);
}

// The share of a row [left, right] blurred horizontally by sigma that reaches x
float blurredRow(float x, vec2 row, float sigma){
    float s = 1.0 / (sigma * 1.4142135);
    return max(0.5 * (erfApprox((x - row.x) * s) - erfApprox((x - row.y) * s)), 0.0);
}

// The integral of a row's blur over the band [a, b] of rows, Gaussian-weighted around p.y:
// sampled at band midpoints (a band of the curved corners, cut to 3 sigma around p.y)
float blurredBand(vec2 p, float a, float b, float sigma, vec4 rect, vec4 radX, vec4 radY){
    a = max(a, p.y - 3.0 * sigma);
    b = min(b, p.y + 3.0 * sigma);
    if (b <= a)
        return 0.0;
    const int SAMPLES = 6;
    float step = (b - a) / float(SAMPLES);
    float sum = 0.0;
    for (int i = 0; i < SAMPLES; i++){
        float y = a + (float(i) + 0.5) * step;
        float t = (y - p.y) / sigma;
        sum += exp(-0.5 * t * t) * blurredRow(p.x, roundedRectRow(y, rect, radX, radY), sigma);
    }
    return sum * step / (sigma * 2.5066283);
}

// A rounded rectangle's coverage blurred by a Gaussian of sigma (the CSS box-shadow blur, sigma =
// blur radius / 2) at p: separable and exact over the rows between the corners, integrated
// numerically over the rows of the curves
float roundedRectShadow(vec2 p, vec4 rect, vec4 radX, vec4 radY, float sigma){
    float y0 = rect.y;
    float y1 = rect.y + rect.w;
    float top = min(y0 + max(radY.x, radY.y), y1);
    float bottom = max(y1 - max(radY.z, radY.w), top);
    float s = 1.0 / (sigma * 1.4142135);
    // the straight rows: their extent does not change, the vertical blur is an erf
    vec2 row = vec2(rect.x, rect.x + rect.z);
    float middle = blurredRow(p.x, row, sigma) * max(0.5 * (erfApprox((bottom - p.y) * s) - erfApprox((top - p.y) * s)), 0.0);
    return clamp(middle + blurredBand(p, y0, top, sigma, rect, radX, radY) + blurredBand(p, bottom, y1, sigma, rect, radX, radY), 0.0, 1.0);
}

float htmluiMedian(float r, float g, float b){
    return max(min(r, g), min(max(r, g), b));
}

// MTSDF text (S16 R16, R17): the glyph's coverage, or of its effect (dilated: an outline, blurred:
// a glow, a font-effect blur or a blurred text-shadow), at the fragment. texel is the sample, uvTexels
// its position in atlas texels, docPos the document position: their derivatives give the screen
// size of a texel and of a document px
float htmluiDistanceText(vec4 texel, vec2 uvTexels, vec2 docPos){
    // a texel's value v is the distance (v - 0.5) * 2 * range, in texels
    float range = 2.0 * clip.text.w;
    vec2 texelsX = dFdx(uvTexels);
    vec2 texelsY = dFdy(uvTexels);
    // atlas texels per screen pixel, and document px per atlas texel
    float texelsPerPixel = max(0.5 * (length(texelsX) + length(texelsY)), 1e-5);
    float docPerTexel = (length(dFdx(docPos)) + length(dFdy(docPos))) / max(length(texelsX) + length(texelsY), 1e-5);
    float effect = clip.text.x;
    if (effect < 0.5){
        // the glyph: the median's distance, in screen pixels
        float sd = (htmluiMedian(texel.r, texel.g, texel.b) - 0.5) * range / texelsPerPixel;
        return clamp(sd + 0.5, 0.0, 1.0);
    }
    // effects from the true distance (alpha), in document px, footprint of a screen pixel
    float sd = (texel.a - 0.5) * range * docPerTexel + clip.text.y;
    float pixel = texelsPerPixel * docPerTexel;
    if (effect < 1.5)
        return sdfCoverage(sd, pixel);
    return gaussianEdge(sd, clip.text.z, 0.5 * pixel);
}

#endif
