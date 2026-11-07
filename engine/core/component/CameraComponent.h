// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CAMERA_COMPONENT_H
#define CAMERA_COMPONENT_H

#include "camera/CameraType.h"
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Plane.h"
#include "texture/Framebuffer.h"
#include "render/CameraRender.h"
#include "render/OrderIndependentTransparency.h"
#include "registry/TagManager.h"

#include <cmath>

#define DEFAULT_ORTHO_NEAR          -10
#define DEFAULT_ORTHO_FAR           10
#define DEFAULT_PERSPECTIVE_NEAR    0.2
#define DEFAULT_PERSPECTIVE_FAR     200

namespace jasperengine{

    enum FrustumPlane{
        FRUSTUM_PLANE_NEAR   = 0,
        FRUSTUM_PLANE_FAR    = 1,
        FRUSTUM_PLANE_LEFT   = 2,
        FRUSTUM_PLANE_RIGHT  = 3,
        FRUSTUM_PLANE_TOP    = 4,
        FRUSTUM_PLANE_BOTTOM = 5
    };

    struct JASPERENGINE_API CameraComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;


        CameraType type = CameraType::CAMERA_UI;

        // Unity Camera.cullingMask: bit N set draws GameObjects on layer N (and lets
        // them cast shadows when this is the main camera)
        uint32_t cullingMask = LAYER_MASK_EVERYTHING;

        // runtime, editor: the Scene view's own camera, through which the GameObjects hidden
        // in the Scene view (EntityMetadata::editorFlags) are not drawn. Never saved.
        bool editorView = false;

        Matrix4 projectionMatrix;
        Matrix4 viewMatrix;
        Matrix4 viewProjectionMatrix;

        Vector3 target = Vector3(0, 0, 0);
        Vector3 worldTarget;

        Vector3 up = Vector3(0, 1, 0);
        Vector3 direction = Vector3(0, 0, 1);
        Vector3 right = Vector3(1, 0, 0);

        Vector3 worldUp;
        Vector3 worldDirection;
        Vector3 worldRight;

        float leftClip = 0;
        float rightClip = 10;
        float bottomClip = 0;
        float topClip = 10;

        float yfov = 0.75;
        float aspect = 1.0;

        float nearClip = DEFAULT_ORTHO_NEAR;
        float farClip = DEFAULT_ORTHO_FAR;

        Plane frustumPlanes[6];

        CameraRender render;

        bool renderToTexture = false;
        // need to be a pointer to not lost reference when component changes position
        Framebuffer* framebuffer = new Framebuffer();

        // authored framebuffer settings (source of truth, serialized/editable);
        // RenderSystem syncs these into the Framebuffer above and recreates it when
        // they change. Defaults must match the Framebuffer() constructor defaults.
        unsigned int framebufferWidth = 512;
        unsigned int framebufferHeight = 512;
        TextureFilter framebufferFilter = TextureFilter::LINEAR;

        // draw transparent renderables back to front (needs depthTest)
        bool transparentSort = true;
        // how alpha-blended materials draw (spec S57 R4): Sorted back to front, or Order
        // Independent (weighted blended OIT, Sorted where the device cannot), whose depth weight
        // takes view distances times oitWeightScale (lower for large scenes)
        TransparencyMode transparencyMode = TransparencyMode::SORTED;
        float oitWeightScale = 1.0f;

        // false makes the camera ignore the depth buffer and composite in submission
        // order, like a 2D canvas. Instance and point batches follow the main camera
        bool depthTest = true;

        // when true (the default, and re-enabled by Camera::setTarget) updateCamera
        // builds the view from lookAt(position, target, up) and ignores the Transform
        // rotation; when false the rotation drives it (forward = rotation * -Z)
        bool useTarget = true;
        bool autoResize = true;

        // engine-driven override (planar reflection): when set, updateCamera uses
        // customViewMatrix instead of position/target, and invertCulling flips
        // triangle winding for the reflected (handedness-mirrored) pass
        bool hasCustomViewMatrix = false;
        Matrix4 customViewMatrix;
        bool invertCulling = false;

        // Camera Shaker's view offset (CameraControlSystem; runtime only, never serialized or
        // cloned): a pose in camera space applied after the view matrix is built, so the
        // shake is seen by everything drawn and picked through this camera while the
        // Transform, direction, right and the audio listener stay steady. The rotation turns
        // about viewOffsetPivot (camera space: the eye, or a 2D view's centre)
        bool hasViewOffset = false;
        Vector3 viewOffsetPosition = Vector3(0, 0, 0);
        Quaternion viewOffsetRotation;
        Vector3 viewOffsetPivot = Vector3(0, 0, 0);
        // the world up of the view before that offset (updateCamera): the audio listener's, so a
        // shake never turns what the player hears. worldUp / worldRight / worldDirection are the
        // drawn (shaken) view's
        Vector3 steadyWorldUp = Vector3(0, 1, 0);

        bool needUpdate = true;
    };

    // The up vector a camera looking along viewDirection uses: preferredUp, unless the two are parallel
    // (lookAt would get a zero side vector). Then an up perpendicular to the view: looking along Y the
    // Scene view's Top/Bottom axis-view convention (+Z up on the screen looking down, -Z looking up),
    // else world Y. Only truly parallel vectors: an orbit clamped near vertical keeps its own up.
    inline Vector3 cameraUpFor(const Vector3& viewDirection, const Vector3& preferredUp){
        const float dirLength = viewDirection.length();
        const float upLength = preferredUp.length();
        if (!(dirLength > 0.0f)){
            return preferredUp;
        }
        const Vector3 direction = viewDirection / dirLength;
        if (upLength > 0.0f && direction.crossProduct(preferredUp / upLength).length() > 1e-6f){
            return preferredUp;
        }
        if (std::fabs(direction.y) > 0.5f){
            return Vector3(0.0f, 0.0f, direction.y < 0.0f ? 1.0f : -1.0f);
        }
        return Vector3(0.0f, 1.0f, 0.0f);
    }

}

#endif //CAMERA_COMPONENT_H