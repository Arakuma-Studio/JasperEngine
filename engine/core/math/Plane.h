// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Plane_h
#define Plane_h

#include "Vector3.h"
#include "AABB.h"

namespace jasperengine{

    class OBB;
    
    class JASPERENGINE_API Plane{

    public:

        enum Side : int
        {
            NO_SIDE,
            POSITIVE_SIDE,
            NEGATIVE_SIDE,
            BOTH_SIDE
        };

        Vector3 normal;
        float d;

        Plane ();
        Plane (const Plane& rhs);
        // declared with the user-provided copy constructor (an implicit one is deprecated)
        Plane& operator= (const Plane& rhs) = default;
        Plane (const Vector3& rkNormal, float fConstant);
        Plane (float a, float b, float c, float d);
        Plane (const Vector3& rkNormal, const Vector3& rkPoint);
        Plane (const Vector3& rkPoint0, const Vector3& rkPoint1, const Vector3& rkPoint2);

        Plane operator - () const;
        bool operator==(const Plane& rhs) const;
        bool operator!=(const Plane& rhs) const;

        Side getSide (const Vector3& rkPoint) const;
        Side getSide (const Vector3& centre, const Vector3& halfSize) const;
        Side getSide (const AABB& rkBox) const;
        Side getSide (const OBB& obb) const;

        float getDistance (const Vector3& rkPoint) const;

        void redefine(const Vector3& rkPoint0, const Vector3& rkPoint1, const Vector3& rkPoint2);
        void redefine(const Vector3& rkNormal, const Vector3& rkPoint);

        Vector3 projectVector(const Vector3& v) const;

        Plane& normalize(void);
        Plane normalized() const;

    };

}

namespace jasperengine::math{

    // A sphere is in a camera's frustum unless it lies wholly behind one of its six planes (the
    // order of CameraComponent::frustumPlanes); the far plane (index 1) is skipped when the
    // camera's far clip is 0. RenderSystem::isInsideCamera and the Culling Groups share it.
    inline bool sphereInFrustum(const Plane planes[6], float farClip, const Vector3& center, float radius){
        for (int plane = 0; plane < 6; ++plane){
            if (plane == 1 && farClip == 0)
                continue;
            if (planes[plane].getDistance(center) < -radius)
                return false;
        }
        return true;
    }

}


#endif /* Plane_h */
