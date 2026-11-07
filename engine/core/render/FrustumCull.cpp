// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrustumCull.h"

#include "component/CameraComponent.h"

#include <cmath>
#include <cstring>

using namespace jasperengine;

void FrustumCull::set(const Plane newPlanes[6], float newFarClip){
    bool changed = serial == 0 || newFarClip != farClip;
    for (int p = 0; p < 6 && !changed; p++){
        changed = newPlanes[p].normal != planes[p].normal || newPlanes[p].d != planes[p].d;
    }
    if (!changed)
        return;

    serial = serial + 1 ? serial + 1 : 1;
    farClip = newFarClip;
    axisAligned = true;
    for (int p = 0; p < 6; p++){
        planes[p] = newPlanes[p];
        const Vector3& n = planes[p].normal;
        const int nonZero = (n.x != 0.0f) + (n.y != 0.0f) + (n.z != 0.0f);
        axis[p] = (nonZero != 1) ? -1 : (n.x != 0.0f) ? 0 : (n.y != 0.0f) ? 1 : 2;
        if (axis[p] < 0 && !(p == FRUSTUM_PLANE_FAR && farClip == 0.0f))
            axisAligned = false;
    }
}

bool FrustumCull::inside(const float lo[3], const float hi[3]) const{
    const float centre[3] = {(hi[0] + lo[0]) * 0.5f, (hi[1] + lo[1]) * 0.5f, (hi[2] + lo[2]) * 0.5f};
    const float half[3] = {(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    for (int p = 0; p < 6; p++){
        if (p == FRUSTUM_PLANE_FAR && farClip == 0.0f)
            continue;
        const Plane& plane = planes[p];
        if (axis[p] >= 0){
            const int a = axis[p];
            const float n = plane.normal[a];
            if (n * centre[a] + plane.d < -std::fabs(n * half[a]))
                return false;
        }else{
            const float distance = plane.normal.x * centre[0] + plane.normal.y * centre[1] + plane.normal.z * centre[2] + plane.d;
            const float reach = std::fabs(plane.normal.x * half[0]) + std::fabs(plane.normal.y * half[1]) + std::fabs(plane.normal.z * half[2]);
            if (distance < -reach)
                return false;
        }
    }
    return true;
}
