// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorInternal.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::vertexcolor::detail {

float length3(const Vector3& v) {
    return std::sqrt(dot3(v, v));
}

Vector3 normalize3(const Vector3& v) {
    const float len = length3(v);
    if (!(len > 0.0f) || !std::isfinite(len)) return Vector3(0.0f, 0.0f, 0.0f);
    return Vector3(v.x / len, v.y / len, v.z / len);
}

double detSin(double x) {
    if (!std::isfinite(x)) return 0.0;
    const double twoPi = 2.0 * kPi;
    double r = x - std::floor(x / twoPi + 0.5) * twoPi;
    if (r > kPi / 2.0) r = kPi - r;
    else if (r < -kPi / 2.0) r = -kPi - r;
    // Taylor series on [-pi/2, pi/2]: the 25th power's term is below 1e-20
    const double r2 = r * r;
    double term = r;
    double sum = r;
    for (int n = 1; n <= 12; n++) {
        term = -term * r2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double detCos(double x) {
    return detSin(x + kPi / 2.0);
}

double detPow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (!(x > 0.0) || !std::isfinite(x) || !std::isfinite(y)) return 0.0;
    if (x == 1.0) return 1.0;
    int exponent = 0;
    double m = std::frexp(x, &exponent);
    const double sqrtHalf = 0.70710678118654752440;
    if (m < sqrtHalf) {
        m *= 2.0;
        exponent--;
    }
    // ln(m) = 2 atanh(s), |s| <= 0.172
    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    double term = s;
    double series = s;
    for (int n = 1; n <= 16; n++) {
        term *= s2;
        series += term / (double)(2 * n + 1);
    }
    const double ln2 = 0.69314718055994530942;
    double t = y * (2.0 * series + (double)exponent * ln2);
    if (t > 709.0) t = 709.0;
    if (t < -745.0) return 0.0;
    // e^t = 2^k e^r, |r| <= ln2 / 2
    const double k = std::floor(t / ln2 + 0.5);
    const double r = t - k * ln2;
    double e = 1.0;
    double et = 1.0;
    for (int n = 1; n <= 22; n++) {
        et = et * r / (double)n;
        e += et;
    }
    return std::ldexp(e, (int)k);
}

namespace {

    // atan on [0, 1]: two half-angle steps bring x under 0.2, where 20 terms of the series reach
    // double precision
    double detAtanUnit(double x) {
        for (int i = 0; i < 2; i++) x = x / (1.0 + std::sqrt(1.0 + x * x));
        const double x2 = x * x;
        double term = x;
        double sum = x;
        for (int n = 1; n <= 20; n++) {
            term *= -x2;
            sum += term / (double)(2 * n + 1);
        }
        return 4.0 * sum;
    }

}

double detAtan2(double y, double x) {
    if (!std::isfinite(x) || !std::isfinite(y) || (x == 0.0 && y == 0.0)) return 0.0;
    const double ax = std::fabs(x), ay = std::fabs(y);
    double angle = ay <= ax ? detAtanUnit(ay / ax) : kPi / 2.0 - detAtanUnit(ax / ay);
    if (x < 0.0) angle = kPi - angle;
    return y < 0.0 ? -angle : angle;
}

Space Space::of(const Matrix4& model) {
    Space space;
    const float* p = model;
    std::memcpy(space.m, p, sizeof(space.m));
    space.identity = true;
    for (int i = 0; i < 16; i++) {
        const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
        if (space.m[i] != expected) space.identity = false;
    }
    const Vector3 a0(space.m[0], space.m[1], space.m[2]);
    const Vector3 a1(space.m[4], space.m[5], space.m[6]);
    const Vector3 a2(space.m[8], space.m[9], space.m[10]);
    Vector3 c0 = cross3(a1, a2);
    Vector3 c1 = cross3(a2, a0);
    Vector3 c2 = cross3(a0, a1);
    // a mirroring model turns the cofactor round: normals must keep pointing out
    if (dot3(a0, c0) < 0.0f) {
        c0 = -c0;
        c1 = -c1;
        c2 = -c2;
    }
    const Vector3 cols[3] = {c0, c1, c2};
    for (int c = 0; c < 3; c++) {
        space.normal[c * 3 + 0] = cols[c].x;
        space.normal[c * 3 + 1] = cols[c].y;
        space.normal[c * 3 + 2] = cols[c].z;
    }
    return space;
}

Space Space::local() {
    return Space::of(Matrix4());
}

Vector3 Space::point(const Vector3& p) const {
    if (identity) return p;
    return Vector3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                   m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                   m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

Vector3 Space::direction(const Vector3& n) const {
    if (identity) return normalize3(n);
    return normalize3(Vector3(normal[0] * n.x + normal[3] * n.y + normal[6] * n.z,
                              normal[1] * n.x + normal[4] * n.y + normal[7] * n.z,
                              normal[2] * n.x + normal[5] * n.y + normal[8] * n.z));
}

namespace {

    void quatRotationRows(const Quaternion& input, float r[9]) {
        float w = input.w, x = input.x, y = input.y, z = input.z;
        const float n = w * w + x * x + y * y + z * z;
        if (!(n > 0.0f)) {
            w = 1.0f;
            x = y = z = 0.0f;
        } else if (n != 1.0f) {
            const float len = std::sqrt(n);
            w /= len;
            x /= len;
            y /= len;
            z /= len;
        }
        r[0] = 1.0f - 2.0f * (y * y + z * z);
        r[1] = 2.0f * (x * y - w * z);
        r[2] = 2.0f * (x * z + w * y);
        r[3] = 2.0f * (x * y + w * z);
        r[4] = 1.0f - 2.0f * (x * x + z * z);
        r[5] = 2.0f * (y * z - w * x);
        r[6] = 2.0f * (x * z - w * y);
        r[7] = 2.0f * (y * z + w * x);
        r[8] = 1.0f - 2.0f * (x * x + y * y);
    }

    Quaternion quatMultiply(const Quaternion& a, const Quaternion& b) {
        return Quaternion(a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                          a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
    }

    Quaternion quatAxisAngle(int axis, float degrees) {
        const double half = (double)degrees * kPi / 360.0;
        const float s = (float)detSin(half);
        const float c = (float)detCos(half);
        return Quaternion(c, axis == 0 ? s : 0.0f, axis == 1 ? s : 0.0f, axis == 2 ? s : 0.0f);
    }

    float displayEncode(float linear) {
        return pixelart::engineLinearToSrgb(linear);
    }

    float displayDecode(float encoded) {
        return pixelart::engineSrgbToLinear(encoded);
    }

}

Vector3 rotate(const Quaternion& q, const Vector3& v) {
    float r[9];
    quatRotationRows(q, r);
    return Vector3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z, r[6] * v.x + r[7] * v.y + r[8] * v.z);
}

Vector3 rotateInverse(const Quaternion& q, const Vector3& v) {
    float r[9];
    quatRotationRows(q, r);
    return Vector3(r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z, r[2] * v.x + r[5] * v.y + r[8] * v.z);
}

Quaternion eulerDegrees(const Vector3& degrees) {
    return quatMultiply(quatAxisAngle(1, degrees.y), quatMultiply(quatAxisAngle(0, degrees.x), quatAxisAngle(2, degrees.z)));
}

Hsv toHsv(const Vector4& linear) {
    const float r = displayEncode(linear.x), g = displayEncode(linear.y), b = displayEncode(linear.z);
    const float maxC = std::max(r, std::max(g, b));
    const float minC = std::min(r, std::min(g, b));
    const float delta = maxC - minC;
    Hsv hsv;
    hsv.v = maxC;
    hsv.s = maxC > 0.0f ? delta / maxC : 0.0f;
    if (delta > 0.0f) {
        float h;
        if (maxC == r) h = (g - b) / delta;
        else if (maxC == g) h = 2.0f + (b - r) / delta;
        else h = 4.0f + (r - g) / delta;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
        if (h >= 1.0f) h -= 1.0f;
        hsv.h = h;
    }
    return hsv;
}

Vector4 fromHsv(const Hsv& hsv, float alpha) {
    float h = hsv.h - std::floor(hsv.h);
    const float s = std::max(0.0f, std::min(1.0f, hsv.s));
    const float v = std::max(0.0f, hsv.v);
    h *= 6.0f;
    const int sector = std::min(5, (int)std::floor(h));
    const float f = h - (float)sector;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    float r = v, g = t, b = p;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return Vector4(displayDecode(r), displayDecode(g), displayDecode(b), alpha);
}

bool colorMatches(const Vector4& value, const ColorMatch& match) {
    // a hair over the tolerance: an exact tolerance of 1/255 must match a one-step difference
    const float tolerance = std::max(0.0f, match.tolerance) + 1e-6f;
    if (!match.ignoreAlpha && std::fabs(value.w - match.value.w) > tolerance) return false;
    if (!match.ignoreH && !match.ignoreS && !match.ignoreV) {
        return std::fabs(value.x - match.value.x) <= tolerance && std::fabs(value.y - match.value.y) <= tolerance &&
               std::fabs(value.z - match.value.z) <= tolerance;
    }
    const Hsv a = toHsv(value);
    const Hsv b = toHsv(match.value);
    if (!match.ignoreH) {
        // a gray has no hue: it matches only another gray
        if ((a.s > 0.0f) != (b.s > 0.0f)) return false;
        float dh = std::fabs(a.h - b.h);
        if (dh > 0.5f) dh = 1.0f - dh;
        if (a.s > 0.0f && dh > tolerance) return false;
    }
    if (!match.ignoreS && std::fabs(a.s - b.s) > tolerance) return false;
    if (!match.ignoreV && std::fabs(a.v - b.v) > tolerance) return false;
    return true;
}

void denseValues(const TargetData& data, const MeshView& mesh, std::vector<Vector4>& base, std::vector<Vector4>& corners) {
    const uint32_t vertexCount = mesh.vertexCount();
    const Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);
    if (data.base.size() == vertexCount) base = data.base;
    else base.assign(vertexCount, white);
    corners.resize(mesh.indices.size());
    for (size_t c = 0; c < mesh.indices.size(); c++) corners[c] = base[mesh.indices[c]];
    for (const CornerValue& entry : data.corners) {
        if (entry.corner < corners.size()) corners[entry.corner] = entry.value;
    }
}

Vector4 logicalMean(const MeshView& mesh, const std::vector<Vector4>& base, const std::vector<Vector4>& corners, uint32_t logical) {
    double sum[4] = {0.0, 0.0, 0.0, 0.0};
    uint32_t count = 0;
    for (uint32_t m = mesh.memberStart[logical]; m < mesh.memberStart[logical + 1]; m++) {
        const uint32_t vertex = mesh.members[m];
        for (uint32_t k = mesh.vertexCornerStart[vertex]; k < mesh.vertexCornerStart[vertex + 1]; k++) {
            const Vector4& v = corners[mesh.vertexCorners[k]];
            sum[0] += v.x;
            sum[1] += v.y;
            sum[2] += v.z;
            sum[3] += v.w;
            count++;
        }
    }
    if (count == 0) {
        for (uint32_t m = mesh.memberStart[logical]; m < mesh.memberStart[logical + 1]; m++) {
            const Vector4& v = base[mesh.members[m]];
            sum[0] += v.x;
            sum[1] += v.y;
            sum[2] += v.z;
            sum[3] += v.w;
            count++;
        }
    }
    if (count == 0) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    return Vector4((float)(sum[0] / count), (float)(sum[1] / count), (float)(sum[2] / count), (float)(sum[3] / count));
}

uint64_t fnvU32(uint64_t hash, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        hash ^= (uint64_t)((value >> (8 * i)) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t fnvFloat(uint64_t hash, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return fnvU32(hash, bits);
}

}
