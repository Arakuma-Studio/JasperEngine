// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_FRAME_H
#define NAV_FRAME_H

// A surface's nav space (docs/navigation.md): its GameObject's world position and rotation,
// scale ignored, with Recast's Y up. On a 2D surface (plane XY) world (x, y) maps to nav
// (x, 0, y) and back, the world z kept by the caller.

#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "navigation/NavTypes.h"

namespace jasperengine::nav {

    struct NavFrame {
        Vector3 origin;
        Quaternion rotation;
        NavPlane plane = NavPlane::XZ;

        Vector3 toNav(const Vector3& world) const {
            const Vector3 local = rotation.inverse() * (world - origin);
            if (plane == NavPlane::XY)
                return Vector3(local.x, 0.0f, local.y);
            return local;
        }
        // a direction (no translation)
        Vector3 directionToNav(const Vector3& world) const {
            const Vector3 local = rotation.inverse() * world;
            if (plane == NavPlane::XY)
                return Vector3(local.x, 0.0f, local.y);
            return local;
        }
        // planeZ: the local z a 2D point gets (the surface's plane is 0)
        Vector3 toWorld(const Vector3& nav, float planeZ = 0.0f) const {
            const Vector3 local = plane == NavPlane::XY ? Vector3(nav.x, nav.z, planeZ) : nav;
            return origin + rotation * local;
        }
        Vector3 directionToWorld(const Vector3& nav) const {
            const Vector3 local = plane == NavPlane::XY ? Vector3(nav.x, nav.z, 0.0f) : nav;
            return rotation * local;
        }
        // the local z of a world point (a 2D agent keeps its depth)
        float planeZOf(const Vector3& world) const {
            return (rotation.inverse() * (world - origin)).z;
        }
        // nav from world as a matrix (plane XZ; on XY the projection drops z)
        Matrix4 navFromWorld() const {
            Matrix4 m = rotation.inverse().getRotationMatrix() * Matrix4::translateMatrix(-origin);
            if (plane == NavPlane::XY) {
                // rows: x stays, y <- 0, z <- local y
                Matrix4 axis;
                axis.set(0, 0, 1.0f); axis.set(1, 0, 0.0f); axis.set(2, 0, 0.0f);
                axis.set(0, 1, 0.0f); axis.set(1, 1, 0.0f); axis.set(2, 1, 0.0f);
                axis.set(0, 2, 0.0f); axis.set(1, 2, 1.0f); axis.set(2, 2, 0.0f);
                m = axis * m;
            }
            return m;
        }
        // the world up of nav Y (XZ) or the plane's normal (XY)
        Vector3 up() const {
            return rotation * (plane == NavPlane::XY ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f));
        }
    };

}

#endif // NAV_FRAME_H
