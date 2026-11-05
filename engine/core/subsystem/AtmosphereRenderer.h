// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_RENDERER_H
#define ATMOSPHERE_RENDERER_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector4.h"
#include "math/Rect.h"
#include "render/AtmosphereQuality.h"
#include "render/AtmosphereMath.h"
#include "render/ObjectRender.h"
#include "render/CameraRender.h"
#include "render/TextureRender.h"
#include "render/ShaderRender.h"
#include "texture/Framebuffer.h"
#include "texture/Texture.h"
#include "texture/IBLEnvironment.h"
#include "ecs/Entity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class RenderSystem;
    class Scene;
    struct CameraComponent;
    struct Transform;
    struct FogComponent;
    struct VolumetricCloudsComponent;
    struct SkyboxComponent;

    // u_fs_heightFog (shaderlib/includes/height_fog.glsl): the height fog of a camera pass, read
    // by the fogged mesh variants (HAS_HEIGHT_FOG), the sky composite and the atmosphere passes
    typedef struct height_fog_t {
        Matrix4 invViewProjection;            // camera clip -> world, the camera's own matrices
        Vector4 screenToNdc;                  // camera ndc.xy = gl_FragCoord.xy * xy + zw
        Vector4 color_colorMode = Vector4(0.0f, 0.0f, 0.0f, -1.0f); // rgb linear colour, w 0 Color / 1 Sky / -1 not a height fog
        Vector4 extinction_base_H_maxOpacity;
        Vector4 start_volumetric_maxDist_inscatter;
        Vector4 sunDir_anisotropy;            // xyz towards the main light
        Vector4 sunColor_skyFillSin;          // rgb linear colour * intensity, w sin(sky fill angle) (0 = none)
        Vector4 horizonRing[4];               // rgb azimuths +Z, +X, -Z, -X; [0].w the sky fill's most
    } height_fog_t;
    static_assert(sizeof(height_fog_t) == 14 * 16, "height_fog_t is the 14 vectors of u_fs_heightFog");


    // u_fs_atmosphereDepth (shaderlib/atmosphere_depth.frag)
    typedef struct atmosphere_depth_t {
        Vector4 source;
        Vector4 factor_invFar;
        Vector4 inverseProjectionZ;
        Vector4 inverseProjectionW;
    } atmosphere_depth_t;

    // u_fs_volumetricFog (shaderlib/volumetric_fog.frag)
    typedef struct volumetric_fog_t {
        Vector4 target;
        Vector4 camera;
        Vector4 depthInfo;
        Vector4 noise;
        Vector4 noiseOffset;
        Vector4 volumeFrame[ATMOSPHERE_MAX_FOG_VOLUMES * 3];
        Vector4 volumeShape[ATMOSPHERE_MAX_FOG_VOLUMES];
        Vector4 volumeMedium[ATMOSPHERE_MAX_FOG_VOLUMES];
        Vector4 volumeAlbedo[ATMOSPHERE_MAX_FOG_VOLUMES];
        Vector4 volumeScroll[ATMOSPHERE_MAX_FOG_VOLUMES];
        Vector4 lightPosition[ATMOSPHERE_MAX_FOG_LIGHTS];
        Vector4 lightColor[ATMOSPHERE_MAX_FOG_LIGHTS];
        Vector4 lightSpot[ATMOSPHERE_MAX_FOG_LIGHTS];
        Vector4 lightCone[ATMOSPHERE_MAX_FOG_LIGHTS];
    } volumetric_fog_t;

    // u_fs_atmosphereDenoise (shaderlib/atmosphere_denoise.frag)
    typedef struct atmosphere_denoise_t {
        Vector4 target;
    } atmosphere_denoise_t;

    // u_fs_clouds (shaderlib/volumetric_clouds.frag)
    typedef struct clouds_t {
        Matrix4 invViewProjection;
        Vector4 target;
        Vector4 cameraPosition;
        Vector4 cameraForward;
        Vector4 layer;
        Vector4 shape;
        Vector4 erosion;
        Vector4 windOffset;
        Vector4 sunDir_steps;
        Vector4 sunColor_lightSteps;
        Vector4 ambientBottom_near;
        Vector4 ambientTop_fade;
        Vector4 fog;
    } clouds_t;

    // u_fs_skyComposite (shaderlib/sky_composite.frag)
    typedef struct sky_composite_t {
        Vector4 params;
    } sky_composite_t;

    // u_fs_volumetricComposite (shaderlib/volumetric_composite.frag)
    typedef struct volumetric_composite_t {
        Vector4 screenToUV;
        Vector4 fogSize;
        Vector4 cloudsSize;
        Vector4 params;
        Vector4 inverseProjectionZ;
        Vector4 inverseProjectionW;
        Vector4 depthInfo;
    } volumetric_composite_t;

    // What a view's last frame ran (get_atmosphere_info)
    struct AtmosphereInfo{
        AtmosphereQuality quality = AtmosphereQuality::OFF;
        AtmosphereQuality platformDefault = AtmosphereQuality::OFF;
        bool fog = false;
        bool heightFog = false;
        bool volumetric = false;           // asked for (Fog volumetric on a height fog)
        bool volumetricActive = false;     // ran last frame
        int fogWidth = 0;
        int fogHeight = 0;
        int fogSteps = 0;
        bool floatTargets = false;
        int volumesUsed = 0;
        int volumesCulled = 0;
        int localLights = 0;
        bool forcedDepthPrePass = false;
        bool clouds = false;
        bool cloudsActive = false;
        CloudsRendering cloudsMode = CloudsRendering::SKY_MAP;
        bool noiseReady = false;
        int mapSize = 0;
        int sliceFrames = 0;
        // where the Sky colour mode's colour comes from: "cubemap", "property", "fog_color"
        std::string horizonSource = "fog_color";
        std::vector<std::string> passes;
        size_t vramBytes = 0;
        // the part of vramBytes sized by the view (depth, fog and Screen clouds targets; the Sky
        // Map and the noise are not)
        size_t vramScreenBytes = 0;
        // the RenderSystem draw it was recorded in (which of two records is newer)
        uint64_t frame = 0;
    };

    // The atmosphere's targets of one view (RenderSystem's RenderView): reduced-resolution
    // linear depth (at 1/2 and 1/4), the volumetric fog and the Screen clouds (march and
    // denoised), the Sky Map (previous, current, building). Made on first use, kept while
    // their size stays within 8 pixels (the passes sample by camera UV, so a target a few
    // pixels off its ideal size draws the same), freed when the view stops using them.
    struct AtmosphereViewTargets{
        Framebuffer depth[2];
        Framebuffer fog[2];
        Framebuffer clouds[2];
        Framebuffer skyMap[3];
        bool used[4] = {};             // depth, fog, clouds, sky map this frame
        bool compressed = false;       // RGBA8 targets (no float colour targets)

        // the Sky Map's refresh: the index of the map composited as current and previous, the
        // one being marched, the rows done, the frames since the last swap (the crossfade)
        int skyMapSize = 0;
        int skyMapCurrent = -1;
        int skyMapPrevious = -1;
        int skyMapBuilding = 0;
        int skyMapRow = 0;
        int skyMapSlices = 1;
        int skyMapFadeFrames = 0;
        uint64_t skyMapFrame = 0;      // drawCount of its last refresh step (once per frame)
        uint64_t skyMapBuildingKey = 0;
        uint64_t skyMapCurrentKey = 0;

        // what the view's last frame ran (get_atmosphere_info)
        AtmosphereInfo info;

        size_t getBytes() const;
        size_t getScreenBytes() const;
        void releaseUnused();
        void destroy();
    };

    // The atmosphere of a scene (docs/specs/S12-atmosphere.md): the height fog's per-pass block,
    // the volumetric fog, the volumetric clouds and their composites, for RenderSystem.
    class JASPERENGINE_API AtmosphereRenderer{
    public:
        // what one camera pass draws
        struct PassPlan{
            bool mainCamera = false;      // the view's camera: it refreshes the Sky Map
            bool skyComposite = false;
            bool skyFill = false;
            bool skyMapClouds = false;
            bool volumetricFog = false;
            bool screenClouds = false;
            bool needsDepth() const { return volumetricFog || screenClouds; }
            bool any() const { return skyComposite || volumetricFog || screenClouds; }
        };

        explicit AtmosphereRenderer(RenderSystem* renderSystem);
        ~AtmosphereRenderer();
        AtmosphereRenderer(const AtmosphereRenderer&) = delete;
        AtmosphereRenderer& operator=(const AtmosphereRenderer&) = delete;

        // once per update(): the scene's fog, clouds and volumes, the noise bake
        void update(double dt);

        // the scene fog is a height fog (u_fs_heightFog says so to the fogged meshes); the meshes need depth
        // programs for the atmosphere's depth pre-pass
        bool hasHeightFog() const { return heightFogActive; }
        bool needsDepthPrograms() const { return depthPrograms; }

        // the per-pass block of the height fog (also the sky fill's and the passes'): the
        // camera's matrices, its viewport in a target targetHeight high and whether it draws
        // flipped (RenderSystem::fillSceneInputs' rule), the distance its volumetric fog marches
        // (0: none)
        void fillHeightFog(height_fog_t& block, const CameraComponent& camera, const Transform& cameraTransform,
                           const Rect& viewport, unsigned int targetHeight, bool flipped, float volumetricDistance) const;
        // the block of a pass without fog (the editor's Fog toggle): T 1, S 0 everywhere
        static void clearHeightFog(height_fog_t& block);

        // what a camera pass draws: the main camera of a view gets the volumetric fog and the
        // Screen clouds; every 3D pass the sky composite
        PassPlan planPass(const CameraComponent& camera, bool mainCamera, bool skyComposite) const;
        // before its colour pass of width x height pixels: the reduced-resolution passes of the
        // plan from the pass's packed depth (null: a plan without depth is drawn without them);
        // returns the distance the volumetric fog marched (0: it did not run)
        float renderPassTargets(PassPlan& plan, const CameraComponent& camera, const Transform& cameraTransform,
                                TextureRender* sceneDepth, bool forcedDepth, unsigned int width, unsigned int height);
        // inside the colour pass, after the opaque queue: the sky composite, then the
        // volumetric composite, for the pass's pipeline type and its viewport
        void drawComposites(const PassPlan& plan, PipelineType pipelineType, int msaaSamples, const Rect& viewport,
                            unsigned int targetHeight, bool flipped, const CameraComponent& camera, const Transform& cameraTransform);

        // the atmosphere passes a scene with a height fog (volumetric or not) and clouds can draw, as
        // exports ship them (ShaderVariantCollector)
        static std::vector<ShaderType> passShaderTypes(bool heightFog, bool volumetricFog, bool clouds);

        // the horizon ring of a sky's cube map (computed with its IBL maps, cached by texture)
        static void storeHorizonRing(const std::string& textureId, const IBLEnvironment::HorizonRing& ring);

        // frees the GPU resources (the scene's renderer is destroyed)
        void destroy();
        // the view's frame ended: what it ran (get_atmosphere_info), the targets it did not use freed
        void endFrame();

        // a pass the last frame planned waited for its shader or the clouds' noise bake (the
        // scene is still loading: RenderSystem::isAllLoaded)
        bool isLoading() const { return loading; }

        // the RGBA8 path even where float targets exist (tests; JASPERENGINE_ATMOSPHERE_RGBA8=1)
        static void setForceLowPrecisionTargets(bool force);
        static bool isForceLowPrecisionTargets();

    private:
        struct Program{
            std::shared_ptr<ShaderRender> shader;
            ObjectRender render;
            bool loaded = false;
            int blocks[3] = {-1, -1, -1};
        };
        enum ProgramKind{
            PROGRAM_DEPTH,
            PROGRAM_FOG,
            PROGRAM_DENOISE,
            PROGRAM_CLOUDS,
            PROGRAM_SKY_COMPOSITE,
            PROGRAM_VOLUMETRIC_COMPOSITE,
            PROGRAM_COUNT
        };

        RenderSystem* rs;
        Program programs[PROGRAM_COUNT];
        CameraRender passRender;

        // the scene, as update() found it
        bool fogPresent = false;
        bool heightFogActive = false;
        bool depthPrograms = false;
        FogComponent* fog = nullptr;
        VolumetricCloudsComponent* clouds = nullptr;
        std::vector<FogVolumeData> volumes;
        Vector3 fogNoiseOffset = Vector3(0.0f, 0.0f, 0.0f);
        Vector2 cloudShapeOffset = Vector2(0.0f, 0.0f);
        Vector2 cloudWeatherOffset = Vector2(0.0f, 0.0f);
        double noiseReadyTime = -1.0;
        // the main-thread bake: when its last slice ended (steady clock, ms)
        double bakeSliceEnd = -1.0;
        double time = 0.0;
        std::array<Vector3, 4> horizon;
        std::string horizonSource = "fog_color";
        // the clouds' weather map when it was authored with another wrap (weatherTexture), and
        // the map it was copied from
        Texture tiledWeatherMap;
        Texture tiledWeatherSource;

        // the noise textures (shared by every scene, AtmosphereNoise) and the blue noise
        static TextureRender noiseShape;
        static TextureRender noiseDetail;
        static TextureRender noiseWeather;
        static TextureRender blueNoise;
        static TextureRender empty3D;
        static bool forceLowPrecision;

        // the frame's per-pass data: the packed depth of the pass (the volumetric composite's
        // full-resolution depth) and the reduced depth level the fog marched against
        TextureRender* passSceneDepth = nullptr;
        int fogDepthLevel = 0;
        height_fog_t passFog;
        volumetric_fog_t fogBlock;
        clouds_t cloudsBlock;
        // what this frame's passes of the view being drawn ran
        AtmosphereInfo building;
        // a planned pass waited this frame (building) and the last one (loading)
        bool waiting = false;
        bool loading = false;

        bool loadProgram(ProgramKind kind);
        bool ensureTarget(Framebuffer& target, unsigned int width, unsigned int height, bool floatFormat, bool linear);
        bool useFloatTargets() const;
        AtmosphereQuality viewQuality() const;
        bool cloudsReady();
        TextureRender* weatherTexture();
        void updateHorizon();
        void fillClouds(clouds_t& block, const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier,
                        bool skyMap, float cameraAltitudeFade);
        void renderDepth(Framebuffer& target, TextureRender* sceneDepth, int factor, const CameraComponent& camera);
        void renderFog(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier);
        void renderScreenClouds(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier);
        void renderSkyMap(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier);
        void renderDenoise(Framebuffer& source, Framebuffer& target, Framebuffer& depth, const char* label);
        void bindPostColor(ObjectRender& render, ShaderData& shaderData);
        float cloudsFade() const;
        float cameraAltitudeFade(const Transform& cameraTransform) const;
        uint64_t skyMapKey(const Transform& cameraTransform, const AtmosphereTier& tier) const;
        void notePass(const char* label);
    };

}

#endif // ATMOSPHERE_RENDERER_H
