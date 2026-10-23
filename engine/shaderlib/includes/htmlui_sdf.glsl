// Copyright (c) 2026 Gamecrafters Guild Limited and contributors
// SPDX-License-Identifier: MIT
// roundedRectSDF, rectEdgeFootprint, sdfCoverage, erfApprox and the Gaussian shadow edge are
// OpenEngine's (Engine/Modules/Rendering/Shaders/UI/sdf_functions.glsl, MIT, NOTICE.md); the
// clip and border helpers below them are JasperEngine's (S16).
//
// The HTML UI's analytic shapes: rounded rectangles with elliptical corners, antialiased from the
// screen derivatives of the position they are evaluated at (any 2D, 3D or perspective transform),
// at any sample count. Rectangles are (x, y, width, height); radii are (top-left, top-right,
// bottom-right, bottom-left), horizontal and vertical.

#ifndef HTMLUI_SDF_GLSL
#define HTMLUI_SDF_GLSL

// Signed distance to a rounded rectangle with elliptical corners: positive inside, negative
// outside, zero on the boundary. Equal semi-axes keep the exact circular distance; unequal ones
// take the gradient-normalised first-order distance, exact along both axes.
float roundedRectSDF(vec2 p, vec4 rect, vec4 radX, vec4 radY){
    vec2 center = rect.xy + rect.zw * 0.5;
    vec2 halfExt = rect.zw * 0.5;
    vec2 q = abs(p - center) - halfExt;

    bool isLeft = p.x < center.x;
    bool isTop = p.y < center.y;
    vec2 r = vec2(isLeft ? (isTop ? radX.x : radX.w) : (isTop ? radX.y : radX.z),
                  isLeft ? (isTop ? radY.x : radY.w) : (isTop ? radY.y : radY.z));
    r = min(r, halfExt);
    if (r.x <= 0.0 || r.y <= 0.0)
        r = vec2(0.0);

    vec2 d = q + r;
    vec2 md = max(d, vec2(0.0));
    if (md.x <= 0.0 && md.y <= 0.0)
        return -max(q.x, q.y);
    if (r.x == r.y)
        return -(length(md) - r.x);

    float k1 = length(md / r);
    float k2 = length(md / (r * r));
    return -((k1 - 1.0) * k1 / max(k2, 1e-12));
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

// The analytic clip (S16 R6): the product of the coverage of up to two rounded rectangles at p
float htmluiClipShape(vec2 p, vec2 dpdx, vec2 dpdy, vec4 rect, vec4 radX, vec4 radY){
    return sdfCoverage(roundedRectSDF(p, rect, radX, radY), rectEdgeFootprint(p, rect, dpdx, dpdy));
}

#endif
