// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R4, R16, R40)
// - Purpose: the 3D title's transforms as column-major 4x4 matrices (GLSL's layout): the layer
//   root (position and z, R = Rz(-rotation) Ry(rotation_y) Rx(rotation_x), uniform scale x flip,
//   inside its groups' 2D placement), the rigid matrix of a rigged character
//   (T(pivot + d) Rz Ry Rx S T(-pivot)), the comp camera (fov 30, dist = H / 2 / tan(fov / 2), so
//   one world unit is one comp px on z = 0; near dist x 0.01, far dist x 20) and the projection
//   cropped to a render region of the canvas, flipped where the framebuffer origin is
//   bottom-left so row 0 is the image's top on every backend.
// - World space is document space: canvas px, origin at the canvas centre, +y up, +z toward the
//   viewer.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/geometry/Path.h"
#include "titleart/three_d/Title3DStyle.h"

#include <array>

namespace jasperengine::titleart {

    struct Mat4 {
        std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // column-major

        float& at(int row, int col) { return m[(size_t)(col * 4 + row)]; }
        float at(int row, int col) const { return m[(size_t)(col * 4 + row)]; }
        Mat4 operator*(const Mat4& o) const;
        std::array<float, 4> apply(const std::array<float, 4>& v) const;

        static Mat4 translate(double x, double y, double z);
        static Mat4 scale(double x, double y, double z);
        // counter-clockwise looking down the axis toward the origin (right-handed)
        static Mat4 rotateX(double degrees);
        static Mat4 rotateY(double degrees);
        static Mat4 rotateZ(double degrees);
        // a 2D affine in the xy plane (z scaled by `zScale`)
        static Mat4 affine2D(const geometry::Affine& a, double zScale);
    };
    // the inverse of a 4x4 (general); identity when singular
    JASPERENGINE_API Mat4 inverse(const Mat4& m);
    // the inverse transpose of the upper 3x3 (in a 4x4), for normals
    JASPERENGINE_API Mat4 normalMatrix(const Mat4& m);

    // the layer root inside `parent` (its groups' placement, layer space -> document space)
    JASPERENGINE_API Mat4 title3DLayerMatrix(const Json& transform, const geometry::Affine& parent);
    // a rigged character's matrix in layer space (pivot: em-cell centre and mid-depth; `size`
    // the layer's source size for the % members)
    JASPERENGINE_API Mat4 title3DRigMatrix(const CharRig3D& rig, const std::array<float, 3>& pivot, double size);

    struct TitleCamera3D {
        Mat4 view, projection;
        std::array<float, 3> position{0, 0, 0};
        double distance = 0, nearPlane = 0, farPlane = 0;
        double focal = 0;           // comp px per world unit at unit view depth (H / 2 / tan(fov / 2))
    };
    // the camera of a canvas `width` x `height` (comp px) and the projection cropped to `region`
    // (output px of the canvas at `pxScale`, rows down); `flipY` for bottom-left framebuffers
    JASPERENGINE_API TitleCamera3D title3DCamera(const Camera3D& camera, int width, int height, double pxScale, const TitleRectI& region,
                                                 bool flipY);
    // a world point to output px of the canvas at `pxScale` (rows down); false behind the camera
    JASPERENGINE_API bool title3DProject(const TitleCamera3D& camera, const std::array<float, 3>& world, int width, int height, double pxScale,
                                         std::array<double, 2>& out);

}
