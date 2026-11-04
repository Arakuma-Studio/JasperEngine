// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEMLIGHTING_H
#define RENDERSYSTEMLIGHTING_H

// The renderer's state of S57's lighting features (docs/specs/S57-forge-lighting.md):
// screen-tile light lists, capsule and contact shadows and weighted blended OIT. Their code is
// RenderSystemLighting.cpp; RenderSystem.h holds one of each struct here where they belong.

#include "math/Vector4.h"
#include "render/CameraRender.h"
#include "render/CapsuleShadows.h"
#include "render/FramebufferRender.h"
#include "render/LightingTiers.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "render/TiledLights.h"
#include "jobs/Task.h"

#include <memory>
#include <vector>

namespace jasperengine{

    // u_fs_objectLights of a Tiled variant (includes/tiled_lights.glsl)
    typedef struct tiled_object_lights_t {
        Vector4 counts;      // y vertex lights, z the object's layer, w 1: check the lights' masks
        Vector4 sh[3];
        Vector4 tileGrid;    // xy origin (gl_FragCoord), zw tile size (pixels)
        Vector4 tileCount;   // x columns, y rows
    } tiled_object_lights_t;

    // u_fs_lightOcclusion (includes/light_occlusion.glsl)
    typedef struct light_occlusion_t {
        Vector4 contactViewport;   // xy origin (gl_FragCoord), zw 1 / size
        Vector4 contactParams;     // x 1: flip the mask's rows
        Vector4 capsuleParams;     // x capsules, y intensity, z softness
        Vector4 capsuleA[CapsuleShadows::MAX_CAPSULES];
        Vector4 capsuleB[CapsuleShadows::MAX_CAPSULES];
    } light_occlusion_t;

    // u_fs_contactShadows (contact_shadows.frag)
    typedef struct fs_contact_shadows_t {
        Matrix4 projection;
        Matrix4 invProjection;
        Vector4 lightCoordinate;
        Vector4 lightView;
        Vector4 depthSize;
        Vector4 params;
        Vector4 params2;
    } fs_contact_shadows_t;

    // The screen-tile lists of one camera's passes (a CameraShadows'): built by a job from the
    // frame's lights, uploaded before the colour pass. Made the first time a Tiled pass needs it.
    struct TiledLightsView{
        TiledLights builder;
        std::vector<TiledLights::Light> lights;  // the job's input, snapshot on the main thread
        std::vector<float> texels;               // its output, the texture's whole content
        TiledLights::View view;
        TiledLights::Stats stats;
        jobs::Task<void> job;
        bool building = false;
        TextureRender texture;
        int textureRows = 0;
        // the frame its texture was last written in (once a frame: sokol)
        uint64_t uploadedFrame = 0;
        bool uploaded = false;
        Vector4 tileGrid;
        Vector4 tileCount;
        // some light's culling mask leaves a layer out, or one is shaded per vertex: the
        // shaders read the lights' masks
        bool masks = false;

        TiledLightsView() = default;
        TiledLightsView(const TiledLightsView&) = delete;
        TiledLightsView& operator=(const TiledLightsView&) = delete;
        // a build still running reads the lights and writes the texels this owns
        ~TiledLightsView(){
            if (building)
                job.wait();
            texture.destroyTexture();
        }
    };

    // What a view's main camera draws S57's passes into: the contact shadows' mask and the OIT
    // accumulation targets, sized to its colour pass, freed when unused
    struct LightingViewTargets{
        FramebufferRender contactMask;
        CameraRender contactPass;
        unsigned int contactWidth = 0;
        unsigned int contactHeight = 0;
        uint64_t contactUsed = 0;
        FramebufferRender oit;
        unsigned int oitWidth = 0;
        unsigned int oitHeight = 0;
        int oitSamples = 1;
        uint32_t oitDepthView = 0;   // the colour pass's depth attachment it shares
        uint64_t oitUsed = 0;
        CameraRender oitPass;
    };

    // The lighting features of a scene this update (RenderSystem::updateLightingFeatures)
    struct LightingFeatureState{
        LightingTier tier = LightingTier::HIGH;
        // what the mesh variants take: a change reloads them
        bool tiledLights = false;
        bool capsuleShadows = false;
        bool contactShadows = false;
        bool oit = false;
        int tileColumns = TiledLights::GRID_COLUMNS;
        int tileRows = TiledLights::GRID_ROWS;
        int contactDivisor = 0;
        // the scene lights (RenderSystem::sceneLights indices) taking capsule and contact shadows
        int capsuleLight = -1;
        int contactLight = -1;
        // the capsules of the casters nearest the main camera, world space, and the direction
        // towards their light
        std::vector<CapsuleShadows::World> capsules;
        int capsuleCasters = 0;
        Vector3 capsuleTowardsLight = Vector3(0.0f, 1.0f, 0.0f);
        // u_fs_oit of the OIT camera being drawn (x its weight scale)
        Vector4 oitParams = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
        // a camera asked for Order Independent where the device cannot (warned once)
        bool oitFallbackWarned = false;
        bool oitFallback = false;

        // the passes' programs, made on first need and freed when the feature goes
        std::shared_ptr<ShaderRender> contactShader;
        ObjectRender contactRender;
        int contactSlot = -1;
        bool contactLoaded = false;
        TextureRender noiseTexture;
        std::shared_ptr<ShaderRender> resolveShader;
        ObjectRender resolveRender;
        bool resolveLoaded = false;
        // what a Tiled variant reads where no pass built lists (an empty list everywhere)
        TextureRender emptyTiles;

        // the last frame's figures (get_render_stats)
        TiledLights::Stats tileStats;
        bool tileStatsValid = false;
        int contactWidth = 0;
        int contactHeight = 0;
        int oitDraws = 0;
        int oitDrawsCounting = 0;
    };

}

#endif // RENDERSYSTEMLIGHTING_H
