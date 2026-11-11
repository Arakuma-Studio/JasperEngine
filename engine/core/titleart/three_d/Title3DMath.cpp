// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Title3DMath.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::titleart;

namespace {
    constexpr double DEG = 3.14159265358979323846 / 180.0;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += at(row, k) * o.at(k, c);
            r.at(row, c) = s;
        }
    }
    return r;
}

std::array<float, 4> Mat4::apply(const std::array<float, 4>& v) const {
    std::array<float, 4> r{};
    for (int row = 0; row < 4; row++) r[row] = at(row, 0) * v[0] + at(row, 1) * v[1] + at(row, 2) * v[2] + at(row, 3) * v[3];
    return r;
}

Mat4 Mat4::translate(double x, double y, double z) {
    Mat4 r;
    r.at(0, 3) = (float)x;
    r.at(1, 3) = (float)y;
    r.at(2, 3) = (float)z;
    return r;
}

Mat4 Mat4::scale(double x, double y, double z) {
    Mat4 r;
    r.at(0, 0) = (float)x;
    r.at(1, 1) = (float)y;
    r.at(2, 2) = (float)z;
    return r;
}

Mat4 Mat4::rotateX(double degrees) {
    const float c = (float)std::cos(degrees * DEG), s = (float)std::sin(degrees * DEG);
    Mat4 r;
    r.at(1, 1) = c;
    r.at(1, 2) = -s;
    r.at(2, 1) = s;
    r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotateY(double degrees) {
    const float c = (float)std::cos(degrees * DEG), s = (float)std::sin(degrees * DEG);
    Mat4 r;
    r.at(0, 0) = c;
    r.at(0, 2) = s;
    r.at(2, 0) = -s;
    r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotateZ(double degrees) {
    const float c = (float)std::cos(degrees * DEG), s = (float)std::sin(degrees * DEG);
    Mat4 r;
    r.at(0, 0) = c;
    r.at(0, 1) = -s;
    r.at(1, 0) = s;
    r.at(1, 1) = c;
    return r;
}

Mat4 Mat4::affine2D(const geometry::Affine& a, double zScale) {
    Mat4 r;
    r.at(0, 0) = (float)a.a;
    r.at(1, 0) = (float)a.b;
    r.at(0, 1) = (float)a.c;
    r.at(1, 1) = (float)a.d;
    r.at(0, 3) = (float)a.e;
    r.at(1, 3) = (float)a.f;
    r.at(2, 2) = (float)zScale;
    return r;
}

Mat4 jasperengine::titleart::inverse(const Mat4& in) {
    // cofactor expansion in double precision
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = in.m[(size_t)i];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 out;
    if (std::abs(det) < 1e-30) return out;
    for (int i = 0; i < 16; i++) out.m[(size_t)i] = (float)(inv[i] / det);
    return out;
}

Mat4 jasperengine::titleart::normalMatrix(const Mat4& m) {
    Mat4 upper = m;
    upper.at(0, 3) = upper.at(1, 3) = upper.at(2, 3) = 0;
    const Mat4 inv = inverse(upper);
    Mat4 t;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) t.at(r, c) = inv.at(c, r);
    }
    return t;
}

Mat4 jasperengine::titleart::title3DLayerMatrix(const Json& transform, const geometry::Affine& parent) {
    const TitleTransform t = decodeTransform(transform);
    const bool object = transform.is_object();
    const double z = object ? transform.value("z", 0.0) : 0.0;
    const double rx = object ? transform.value("rotation_x", 0.0) : 0.0;
    const double ry = object ? transform.value("rotation_y", 0.0) : 0.0;
    // the groups' 2D placement keeps z at the scale of their uniform scale
    const double parentScale = std::sqrt(std::abs(parent.a * parent.d - parent.b * parent.c));
    const Mat4 root = Mat4::translate(t.position.x, t.position.y, z) * Mat4::rotateZ(-t.rotation) * Mat4::rotateY(ry) * Mat4::rotateX(rx) *
                      Mat4::scale(t.scale * (t.flipX ? -1 : 1), t.scale * (t.flipY ? -1 : 1), t.scale);
    return Mat4::affine2D(parent, parentScale) * root;
}

Mat4 jasperengine::titleart::title3DRigMatrix(const CharRig3D& rig, const std::array<float, 3>& pivot, double size) {
    const double u = size / 100.0;
    const double dx = rig.offset.x * u, dy = rig.offset.y * u, dz = rig.dz * u;
    return Mat4::translate(pivot[0] + dx, pivot[1] + dy, pivot[2] + dz) * Mat4::rotateZ(-rig.rotation) * Mat4::rotateY(rig.tiltY) *
           Mat4::rotateX(rig.tiltX) * Mat4::scale(rig.scale * rig.hScale, rig.scale * rig.vScale, rig.scale) *
           Mat4::translate(-pivot[0], -pivot[1], -pivot[2]);
}

TitleCamera3D jasperengine::titleart::title3DCamera(const Camera3D& cam, int width, int height, double pxScale, const TitleRectI& region,
                                                    bool flipY) {
    TitleCamera3D out;
    const double fov = std::clamp(cam.fov, 1.0, 120.0);
    const double halfTan = std::tan(fov * DEG / 2);
    out.focal = height / 2.0 / halfTan;
    out.distance = out.focal + cam.zOffset;
    out.nearPlane = std::max(1e-3, out.distance * 0.01);
    out.farPlane = out.distance * 20;
    const Mat4 placed = Mat4::translate(cam.x, cam.y, out.distance) * Mat4::rotateZ(-cam.rotation) * Mat4::rotateY(cam.rotationY) *
                        Mat4::rotateX(cam.rotationX);
    out.view = inverse(placed);
    out.position = {placed.at(0, 3), placed.at(1, 3), placed.at(2, 3)};
    // GL-style perspective (clip z in [-1, 1]; the shaders remap for Vulkan)
    const double aspect = (double)width / std::max(1, height);
    const double f = 1.0 / halfTan;
    Mat4 p;
    p.m.fill(0);
    p.at(0, 0) = (float)(f / aspect);
    p.at(1, 1) = (float)f;
    p.at(2, 2) = (float)((out.farPlane + out.nearPlane) / (out.nearPlane - out.farPlane));
    p.at(2, 3) = (float)(2 * out.farPlane * out.nearPlane / (out.nearPlane - out.farPlane));
    p.at(3, 2) = -1;
    // the crop: the region's NDC rectangle of the whole canvas onto [-1, 1]
    const double W = width * pxScale, H = height * pxScale;
    const double x0 = region.x / W * 2 - 1, x1 = (region.x + region.width) / W * 2 - 1;
    const double top = 1 - 2 * region.y / H, bottom = 1 - 2 * (region.y + region.height) / H;
    Mat4 crop;
    crop.at(0, 0) = (float)(2 / (x1 - x0));
    crop.at(0, 3) = (float)(-(x1 + x0) / (x1 - x0));
    crop.at(1, 1) = (float)(2 / (top - bottom));
    crop.at(1, 3) = (float)(-(top + bottom) / (top - bottom));
    if (flipY) {
        crop.at(1, 1) = -crop.at(1, 1);
        crop.at(1, 3) = -crop.at(1, 3);
    }
    out.projection = crop * p;
    return out;
}

bool jasperengine::titleart::title3DProject(const TitleCamera3D& camera, const std::array<float, 3>& world, int width, int height,
                                            double pxScale, std::array<double, 2>& out) {
    // the uncropped projection: the pinhole at the camera's focal length
    const std::array<float, 4> v = camera.view.apply({world[0], world[1], world[2], 1});
    if (v[2] >= -camera.nearPlane) return false;
    const double k = -camera.focal / v[2];
    const double cx = v[0] * k, cy = v[1] * k;
    out = {(cx + width / 2.0) * pxScale, (height / 2.0 - cy) * pxScale};
    return true;
}
