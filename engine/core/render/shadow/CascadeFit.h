// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CASCADEFIT_H
#define CASCADEFIT_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "render/shadow/ShadowTypes.h"

namespace jasperengine {

    // Stable cascaded shadow fit (spec S10 R1-R8): every cascade is the bounding sphere of its
    // camera frustum slice, in a rotation-only light basis, with its centre snapped to the shadow
    // texel grid and its radius quantised. A camera that rotates in place, or moves by less than a
    // texel, gets bit-identical shadow matrices, so shadow edges do not crawl. Pure CPU, no sokol.

    static constexpr unsigned int CASCADE_FIT_MAX = 4;
    // the blend band of a cascade: its last fraction before the split, where the receiver mixes
    // it with the next one; the next slice starts that much before the split so its box holds
    // the band (shadow_cascade_blend.glsl, OpenEngine)
    static constexpr float CASCADE_BLEND_FRACTION = 0.175f;
    // the log/uniform mix of the Auto split mode
    static constexpr float CASCADE_SPLIT_LAMBDA = 0.7f;

    struct CascadeFitInput {
        Matrix4 cameraView;                 // world to camera view space (looking down -Z)
        bool orthographic = false;
        float tanHalfY = 0.0f;              // perspective: tan(vertical fov / 2)
        float aspect = 1.0f;                // width / height
        float orthoHalfWidth = 0.0f;        // orthographic: half extents of the view
        float orthoHalfHeight = 0.0f;
        float nearClip = 0.1f;
        float shadowDistance = 100.0f;      // view depth where the last cascade ends (clamped to the far clip)
        Vector3 lightDir = Vector3(0.0f, -1.0f, 0.0f);   // world direction the light points
        unsigned int cascades = 1;          // 1..CASCADE_FIT_MAX (orthographic cameras: 1)
        unsigned int resolution = 1024;     // texels per side of each cascade's map
        float splits[CASCADE_FIT_MAX - 1] = {0.067f, 0.2f, 0.467f};   // MANUAL: fractions of shadowDistance
        CascadeSplitMode mode = CascadeSplitMode::AUTO;
        float blendFraction = CASCADE_BLEND_FRACTION;    // 0: slices only touch (no blend band)
        bool freeze = false;                // keep a fit while the camera stays inside it (R5)
        ShadowBounds casterBounds;          // world bounds of the shadow casters (pull the near plane to them)
    };

    struct CascadeFitResult {
        unsigned int cascades = 0;
        Matrix4 view;                                   // the rotation-only light view, shared by every cascade
        Matrix4 proj[CASCADE_FIT_MAX];
        Matrix4 viewProj[CASCADE_FIT_MAX];
        float splitNear[CASCADE_FIT_MAX] = {};          // view depth range the cascade is selected for
        float splitFar[CASCADE_FIT_MAX] = {};
        float blendStart[CASCADE_FIT_MAX] = {};         // view depth where the blend into the next cascade starts (splitFar of the last)
        float radius[CASCADE_FIT_MAX] = {};             // ortho half extent (the slice sphere, guard band included)
        float sphereRadius[CASCADE_FIT_MAX] = {};       // the slice sphere's quantised radius
        float texelWorld[CASCADE_FIT_MAX] = {};         // world size of a shadow texel (2 radius / resolution)
        float depthSpan[CASCADE_FIT_MAX] = {};          // world depth of the ortho box
        double centerX[CASCADE_FIT_MAX] = {};           // light-space centre of the box (snapped)
        double centerY[CASCADE_FIT_MAX] = {};
        double nearZ[CASCADE_FIT_MAX] = {};             // light-view z of the box's near (towards the light) and far planes
        double farZ[CASCADE_FIT_MAX] = {};
        float guardUsed[CASCADE_FIT_MAX] = {};          // fraction of the freeze guard band consumed (0 when not frozen)
        bool refit[CASCADE_FIT_MAX] = {};               // fitted this call (false: the previous fit kept, frozen)
        Vector3 lightDir;                               // the light direction fitted to
        unsigned int resolution = 0;
    };

    class JASPERENGINE_API CascadeFit {
    public:
        // The cascades of a light for a camera. previous is the result of the last call for the
        // same light and camera (null for none): with freeze, its cascades are kept while the
        // slice spheres stay inside them, a refit is requested at half the guard band consumed
        // (at most one such refit per call, the most consumed first) and forced when coverage
        // would be lost.
        static CascadeFitResult fit(const CascadeFitInput& input, const CascadeFitResult* previous);

        // the rotation-only light view of a direction: up is world Y, or world Z when the light
        // is within ~8 degrees of vertical
        static Matrix4 lightRotation(const Vector3& lightDir);

        // the radius of the smallest sphere holding the frustum slice [nearDist, farDist] of a
        // perspective camera, and the view depth of its centre
        static float sliceBoundingSphereRadius(float nearDist, float farDist, float tanHalfY, float aspect);
        static float sliceBoundingSphereCenter(float nearDist, float farDist, float tanHalfY, float aspect);

        // the split distances (view depth of each cascade's far plane before the blend overlap)
        static void computeSplits(const CascadeFitInput& input, float splitFar[CASCADE_FIT_MAX]);

        // the guard band of cascade c under freeze, as a fraction of its radius (1/16 for the
        // first, 1/8 for the others)
        static float freezeGuardFraction(unsigned int cascade);

        // r rounded up to a multiple of 2^ceil(log2 r) / 64, so float noise in r never changes it
        static float quantizeRadius(float radius);
    };

}

#endif // CASCADEFIT_H
