// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEM_H
#define RENDERSYSTEM_H

#include "SubSystem.h"
#include <functional>
#include "render/ViewDrawMode.h"
#include <string>
#include <vector>
#include "AtmosphereRenderer.h"
#include "RenderSystemLighting.h"
#include "component/MeshComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SkyboxComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/RectTransformComponent.h"
#include "component/UIComponent.h"
#include "component/ImageComponent.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/Light2DComponent.h"
#include "component/PointsComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/LineRendererComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "render/WaterLayering.h"
#include "render/ObjectRender.h"
#include "render/VisibleList.h"
#include "render/CameraRender.h"
#include "render/BufferRender.h"
#include "render/FramebufferRender.h"
#include "render/shadow/CascadeFit.h"
#include "render/shadow/ShadowCache.h"
#include "render/shadow/ShadowCasterRegistry.h"
#include "render/shadow/ShadowSlotPlanner.h"
#include "render/shadow/ShadowTiers.h"
#include "buffer/ExternalBuffer.h"
#include "shader/ShaderBuildTypes.h"
#include "shader/ShaderManifest.h"
#include "util/PostColorLut.h"
#include "lattice/LatticeRender.h"
#include "render/DynamicResolutionController.h"
#include "render/DrawBatcher.h"
#include "render/FrustumCull.h"
#include "render/SSRProbeSlots.h"
#include "Engine.h"
#include <map>
#include <array>
#include <mutex>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace jasperengine{

	namespace particles { struct CompiledEffect; }
	// Legacy u_fs_lighting, declared by forks written before per-object lights: filled per
	// draw with the object's first MAX_LIGHTS pixel lights (the camera part per camera)
	typedef struct fs_lighting_t {
	    Vector4 direction_range[MAX_LIGHTS];
	    Vector4 color_intensity[MAX_LIGHTS];
	    Vector4 position_type[MAX_LIGHTS];
	    Vector4 inCon_ouCon_shadows_cascades[MAX_LIGHTS];
	    Vector4 spotUp_maskAspect[MAX_LIGHTS]; // world up.xyz; w > 0 means a ready mask and stores its width/height
		Vector4 eyePos; // w = spot mask atlas tile of each light, 3 bits per light
		Vector4 cameraDir; // xyz = camera backward axis, w = 3D shadow PCF tap radius
		Vector4 globalIllum; //global illumination
		Vector4 envColor; //environment color.rgb (linear) and environment rotation.w (radians)
		Vector4 viewportInfo; //1.0/viewportSize.xy in .xy (used by USE_SSAO)
	} fs_lighting_t;

	typedef struct fs_lighting2d_t {
	    Vector4 position_range[MAX_LIGHTS_2D];   // xy = world pos, z = height, w = range
	    Vector4 color_intensity[MAX_LIGHTS_2D];  // rgb = color (linear), w = intensity
	    Vector4 falloff_shadow[MAX_LIGHTS_2D];   // x = falloff exp, y = shadow atlas row (-1 = none), z = softness (texels), w = bias
		Vector4 ambient;                         // rgb = ambient2D (linear) * intensity, w = numLights2D
		Vector4 atlasInfo;                       // x = 1/atlasWidth, y = 1/MAX_LIGHTS_2D, z = atlasWidth, w = PCF tap radius
	} fs_lighting2d_t;

	// The post-processing colour's parameters (phase 4, shaderlib/includes/post_color.glsl),
	// per camera pass: the end of u_fs_frame, the start of u_fs_litFrame, or u_fs_postColor
	typedef struct post_color_t {
		Vector4 vignetteScaleOffset; // p = gl_FragCoord.xy * xy - zw (centred, 1 at the edges)
		Vector4 vignetteColor;       // rgb display colour, a intensity (0 = none)
		Vector4 params;              // x, y vignette falloff, z dither amplitude, w LUT side (0 = no LUT)
		Vector4 shaper;              // x the LUT shaper's k (s = sqrt(x / (x + k))), y how a built-in shader's
		                             // blend takes its colour (PostColorBlend: 0 own, 1 added, 2 premultiplied)
	} post_color_t;

	// How the blend of a draw takes the post-processing colour of a shader without a manifest
	// (built-in mesh, points, lines and their forks: no BLEND_* define), the index of its pass
	// block (RenderSystem::passPostColors): its own colour (opaque, alpha), an added one
	// (black adds nothing, includes/post_color.glsl), a plain factor (multiply: neutral data),
	// or a premultiplied colour (the straight colour's, times alpha). Library shaders follow
	// their BLEND_* define and always take POST_BLEND_OWN.
	enum PostColorBlend : uint8_t {
		POST_BLEND_OWN = 0,
		POST_BLEND_ADDITIVE = 1,
		POST_BLEND_PREMULTIPLIED = 2,
		POST_BLEND_MULTIPLY = 3,
		POST_BLEND_COUNT = 4
	};

	// u_vs_frame / u_fs_frame (shaderlib/includes/frame.glsl): per-camera-pass data for any
	// shader that declares the block. The layout up to ambient is a contract with the
	// library shaders; engine-internal members may only be appended after it, in the
	// same order as in frame.glsl.
	typedef struct frame_t {
		Matrix4 viewMatrix;
		Matrix4 projectionMatrix;
		Matrix4 viewProjectionMatrix;
		Vector4 cameraPosition;     // xyz world position, w = 1 orthographic / 0 perspective
		Vector4 cameraDirection;    // xyz world forward
		Vector4 time;               // x seconds since startup, y sin(x), z cos(x), w frame delta
		Vector4 resolution;         // xy render target size in pixels, zw 1/size
		Vector4 mainLightDirection; // xyz towards the brightest directional light, w intensity (0 = none)
		Vector4 mainLightColor;     // rgb linear
		Vector4 ambient;            // rgb scene ambient (global illumination colour * intensity)
		// engine-internal (RenderSystem::fillFrameLighting)
		Vector4 lightingInfo;           // x 3D shadow PCF tap radius
		Vector4 fogColor_type;          // rgb fog colour, w type (0 linear, 1 exponential, 2 exponential squared)
		Vector4 fogDensity_start_end;   // x density, z linear start, w linear end
		// the fragment block of a variant with the post-processing colour (USE_POST_COLOR)
		post_color_t postColor;
	} frame_t;

	// u_vs_objectLights / u_fs_objectLights (shaderlib/includes/lights.glsl) at the largest
	// capacity. A variant of capacity C reads, in order: counts, position_type[C],
	// direction_inner[C], color_outer[C] (the vertex stage stops here, in mixed mode), the
	// L1 SH ambient (3, the vertex stage stops here in vertex mode), spot_shadow[C]: 4 + 4 C
	// vectors in the fragment stage (RenderSystem::buildObjectLights).
	typedef struct object_lights_t {
		Vector4 data[4 + 4 * MAX_LIGHTS_PER_OBJECT];
	} object_lights_t;

	// u_fs_objectLights of the records form (includes/lights.glsl, LIGHTS_FS_RECORDS): the
	// pixel lights' records are rows of the frame's u_lightRecords (RenderSystem::syncLightRecords)
	typedef struct object_light_records_t {
		Vector4 counts;      // x pixel lights, y vertex lights
		Vector4 sh[3];
		// per pixel light, two a vector: its scene light, its spot_shadow.w
		Vector4 records[(MAX_LIGHTS_PER_OBJECT + 1) / 2];
	} object_light_records_t;

	// u_fs_litFrame (shaderlib/includes/lit_frame.glsl): what the built-in Lit's lit variants
	// read of the camera pass, filled with the frame block (RenderSystem::updatePassFrame). A
	// variant declares a prefix of it (2 to 6 vectors, by fog, shadows, SSAO and IBL).
	typedef struct lit_frame_t {
		Vector4 cameraPosition_fogEnd;     // xyz camera world position, w fog linear end
		Vector4 ambient_fogType;           // rgb scene ambient, w fog type
		Vector4 fogColor_param;            // rgb fog colour, w fog linear start (linear) or density
		Vector4 cameraForward_pcf;         // xyz camera world forward, w 3D shadow PCF tap radius
		Vector4 viewport_ssao_envRotation; // xy 1 / viewport size, z SSAO flags (1 debug, 2 Y flip), w environment rotation
		Vector4 environmentColor;          // rgb environment colour (linear)
	} lit_frame_t;

	// u_fs_litFrame of a variant with the post-processing colour: its parameters first, then the
	// prefix of lit_frame_t the variant declares
	typedef struct lit_frame_block_t {
		post_color_t postColor;
		lit_frame_t lit;
	} lit_frame_block_t;

	typedef struct fs_terrain_layers_t {
		Vector4 colorFactor[MAX_TERRAIN_LAYERS]; // rgb = linear tint, w = 1 on a PBR layer
		Vector4 uvTransform[MAX_TERRAIN_LAYERS]; // xy = detail tiling scale, zw = offset
		Vector4 surface[MAX_TERRAIN_LAYERS];     // roughness, metallic, normal strength, occlusion strength
		Vector4 slices[MAX_TERRAIN_LAYERS];      // color, normal and packed surface slice (-1 = none)
		Vector4 stochastic[MAX_TERRAIN_LAYERS];  // Stochastic Tiling: cell size (0 = off), blend sharpness, contrast, rotation
	} fs_terrain_layers_t;

	// u_fs_planarReflection (includes/planar_reflection.glsl)
	typedef struct fs_planar_reflection_t {
		Matrix4 viewProjection; // the reflection camera's, logical
		Vector4 params;         // x the plane's height, y 1 with a reflection
	} fs_planar_reflection_t;

	// WATER_INTERACTION_PARAMS (includes/water_interaction_params.glsl), the engine-written members
	// at the end of a water shader's u_fs_customParams: the Water Excluders over a draw and the
	// scene's wake map
	constexpr int WATER_EXCLUDERS_PER_DRAW = 8;
	typedef struct fs_water_interaction_t {
		Vector4 exclusion;      // x the excluders tested; y, z, w their rows in the data texture, three bytes each
		Vector4 wakeArea;       // xy the map's world corner, z 1 / its world size, w 1 while it holds a wake
		Vector4 wakeParams;     // x slope per unit of crest over a texel, y 1 / texels, z the body tag / 255
	} fs_water_interaction_t;
	static_assert(sizeof(fs_water_interaction_t) == 3 * 16, "WATER_INTERACTION_PARAMS is 3 vec4");

	// includes/environment.glsl (u_fs_environment)
	typedef struct fs_environment_t {
		Vector4 colorRotation; // rgb linear tint, w rotation about +Y (radians)
		Vector4 params;        // x highest mip level, y 1 with an environment
	} fs_environment_t;

	// u_fs_environment of a shader reading the planar reflection too: both in one block
	typedef struct fs_environment_planar_t {
		fs_environment_t environment;
		fs_planar_reflection_t planar;
	} fs_environment_planar_t;

	typedef struct fs_reflection_probe_t {
		Vector4 position_weight; // xyz = capture position, w = local probe blend weight
		Vector4 boxMin_intensity; // xyz = influence AABB min, w = intensity
		Vector4 boxMax_lod;       // xyz = influence AABB max, w = max available mip LOD
	} fs_reflection_probe_t;

	// probe_filter.frag
	typedef struct fs_probe_filter_t {
		Vector4 params; // x = roughness, y = cube face, z = samples (taps per side when downsampling), w = 1 to downsample
		Vector4 source; // x = destination level size, y = source base size, z = source mip count, w = source level when downsampling
	} fs_probe_filter_t;

	typedef struct vs_shadow2d_t {
		Vector4 lightPos_range; // xy = light world pos, w = range
		Vector4 offset;         // x = NDC x offset for the polar seam duplication (-2, 0, +2)
	} vs_shadow2d_t;

	typedef struct fs_fog_t {
		Vector4 color_type;
		Vector4 density_start_end;
	} fs_fog_t;

	typedef struct vs_shadows_t {
	    Matrix4 lightViewProjectionMatrix[MAX_SHADOW_ATLAS_SLOTS];
	    Vector4 shadowParams[MAX_SHADOW_ATLAS_SLOTS]; // normalBias in .x
	} vs_shadows_t;

	typedef struct fs_shadows_t {
        Vector4 bias_texSize_nearFar[MAX_SHADOW_ATLAS_SLOTS];
        // xy = atlas origin, zw = atlas scale (directional/spot)
        Vector4 atlasRect[MAX_SHADOW_ATLAS_SLOTS];
	} fs_shadows_t;

	typedef struct fs_point_shadows_t {
        Vector4 bias_texSize_nearFar[MAX_POINT_SHADOW_ATLAS_SLOTS];
        // xy = atlas origin, zw = atlas scale (point cube faces)
        Vector4 atlasRect[MAX_POINT_SHADOW_ATLAS_SLOTS];
	} fs_point_shadows_t;

	// includes/shadow_data.glsl u_fs_shadow_data, member for member (the layout is documented
	// there and in docs/rendering/shadows.md): the shadows a fragment stage samples
	typedef struct fs_shadow_data_t {
		Vector4 header;               // main directional slot, its cascades, flags, debug view
		Vector4 fitCameraPosition;
		Vector4 fitCameraForward;
		Matrix4 lightVP[MAX_SHADOW_ATLAS_SLOTS];
		Vector4 atlasRect[MAX_SHADOW_ATLAS_SLOTS];
		Vector4 bias[MAX_SHADOW_ATLAS_SLOTS];
		Vector4 split[MAX_SHADOW_ATLAS_SLOTS];
		Vector4 filtering[MAX_SHADOW_ATLAS_SLOTS];
		Vector4 fade[MAX_SHADOW_ATLAS_SLOTS];
	} fs_shadow_data_t;
	static_assert(sizeof(fs_shadow_data_t) == 16 * (3 + 9 * MAX_SHADOW_ATLAS_SLOTS), "fs_shadow_data_t must match u_fs_shadow_data");

	// includes/shadow_data.glsl u_fs_point_shadow_data
	typedef struct fs_point_shadow_data_t {
		Vector4 faceRect[MAX_POINT_SHADOW_ATLAS_SLOTS];
		Vector4 projection[MAX_POINT_SHADOW_LIGHTS];
		Vector4 bias[MAX_POINT_SHADOW_LIGHTS];
		Vector4 filtering[MAX_POINT_SHADOW_LIGHTS];
		Vector4 state[MAX_POINT_SHADOW_LIGHTS];
	} fs_point_shadow_data_t;
	static_assert(sizeof(fs_point_shadow_data_t) == 16 * (MAX_POINT_SHADOW_ATLAS_SLOTS + 4 * MAX_POINT_SHADOW_LIGHTS), "fs_point_shadow_data_t must match u_fs_point_shadow_data");

	// shadow_restore.frag u_fs_shadowRestore
	typedef struct fs_shadow_restore_t {
		Vector4 sourceRect;
		Vector4 params;
	} fs_shadow_restore_t;

	typedef struct vs_depth_t {
		Matrix4 modelMatrix;
		Matrix4 lightVPMatrix;
		Matrix4 mvpMatrix; // lightVPMatrix * modelMatrix
	} vs_depth_t;

	typedef struct vs_gbuffer_t {
		Matrix4 modelMatrix;
		Matrix4 viewProjectionMatrix;
		Matrix4 normalMatrix;       // view-space normal matrix: transpose(inverse(view*model))
	} vs_gbuffer_t;

	typedef struct fs_gbuffer_material_t {
		Vector4 params;             // x = roughness, y = metallic, z = reflection source code / 255, w = alpha cutout
		Vector4 baseColorFactor;
		Vector4 stochastic;         // Stochastic Tiling ('Sto' variants; padding in 'Uv2' ones): cell size, blend sharpness, contrast, rotation
		Vector4 texCoordSets;       // 'Uv2' variants only: the UV set (0, 1) of x base colour, y metallic-roughness, z occlusion
	} fs_gbuffer_material_t;

	typedef struct fs_ssao_t {
		Matrix4 projection;
		Matrix4 invProjection;
		Vector4 kernel[SSAO_KERNEL_SIZE];
		Vector4 params;     // radius.x, bias.y, intensity.z
		Vector4 noiseScale; // screenSize/noiseSize in .xy
	} fs_ssao_t;

	typedef struct fs_ssao_blur_t {
		Vector4 texelSize; // 1.0/textureSize in .xy
	} fs_ssao_blur_t;

	typedef struct fs_ssr_t {
		Matrix4 projection;
		Matrix4 invProjection;
		Vector4 params; // x=maxDistance, y=thickness, z=intensity, w=maxSteps
		Vector4 misc;   // xy=1/depthTextureSize, z=flip depth<->scene-color Y, w=glossy-blur amount
	} fs_ssr_t;

	typedef struct fs_ssr_blur_t {
		Vector4 params; // xy=1/textureSize, z=max radius(pixels), w=flip G-buffer Y
	} fs_ssr_blur_t;

	// mirrored by u_fs_compositeParams of composite.frag
	typedef struct fs_composite_t {
		Matrix4 invProjection;
		Matrix4 invView;
		Vector4 params;   // x=intensity, y=flip G-buffer Y, z=debug, w=flip destination Y
		Vector4 envColor; // rgb = sky colour (linear), w = sky rotation (radians): environmentInfo()
		Vector4 ambient;  // rgb = the scene ambient Lit adds, w = fog: 0 none, 1 distance, 2 height
		Vector4 projectionW; // the projection's 4th row: the clip w of a view position
		Vector4 probePosition[SSR_PROBE_SLOTS]; // xyz = capture position
		Vector4 probeBoxMin[SSR_PROBE_SLOTS];   // xyz = influence AABB min, w = intensity
		Vector4 probeBoxMax[SSR_PROBE_SLOTS];   // xyz = influence AABB max, w = max mip LOD (0 = unfiltered)
	} fs_composite_t;

	// What the SSR G-buffer pass of a view last drew (RenderSystem::getSSRStats): the reflection
	// probes of its drawn meshes and the slots the composite rebuilds them in
	struct SSRViewStats {
		struct Probe {
			Entity entity = NULL_ENTITY;
			int slot = -1;          // -1: no slot, its surfaces keep the probe reflection without SSR
			float coverage = 0.0f;  // the screen fraction of the meshes it lights (summed)
		};
		bool valid = false;         // the G-buffer pass ran in the last frame of the view
		unsigned int meshesDrawn = 0;
		std::vector<Probe> probes;  // by coverage, largest first
		unsigned int probesWithoutSlot() const;
	};

	// the uber pass's parameters (shaderlib/uber.vert, uber.frag)
	typedef struct vs_uber_t {
		Vector4 uvScaleOffset;       // source UV = position * xz + yw
		Vector4 sourceTexel;         // xy 1 / source size, zw source size
	} vs_uber_t;
	typedef struct fs_uber_t {
		Vector4 uvScaleOffset;
		Vector4 sourceTexel;
		Vector4 outputTexel;         // xy 1 / output viewport size, zw its size
		Vector4 effects;             // x chromatic aberration, y pixel block (output pixels), z grain, w grain response
		Vector4 crt;                 // x scanlines, y mask, z curvature, w tube vignette
		Vector4 edgeSharpenTime;     // x edge threshold, y edge intensity, z sharpen, w time
		Vector4 edgeColorDither;     // rgb edge colour (display), a dither amplitude
		Vector4 vignetteScaleOffset; // as post_color_t
		Vector4 vignetteColor;
		Vector4 vignetteLutScan;     // x, y vignette falloff, z grade LUT side, w scanline period (output pixels)
		Vector4 bloomBlur;           // rgb bloom tint * intensity (linear), a blur mix
		Vector4 sampling;            // x 1: the source is a NEAREST target, its UV snaps to texel centres
		Vector4 vhs0;                // x intensity, y wobble, z tracking, w colour bleed
		Vector4 vhs1;                // x tape noise, y chroma streaks, z dropouts, w scanlines
		Vector4 vhs2;                // x interference, y glitches, z tape time (s)
		Vector4 halation;            // rgb tint * intensity (linear), a radius
		Vector4 film;                // x grain size, y coloured grain, z film frame, w dust size
		Vector4 damage;              // x strength, y hair, z scratch, w dust candidates
		Vector4 gateWeave;           // xy offset (output pixels), z cos, w sin of the rotation
		Vector4 gateMask;            // xy half size, z corner radius, w feather (output pixels); x 0: none
	} fs_uber_t;

	// the bloom and blur chain's passes (shaderlib/bloom.vert, bloom.frag)
	typedef struct vs_bloom_t {
		Vector4 offset;              // xy tap distance in source UV
	} vs_bloom_t;
	typedef struct fs_bloom_t {
		Vector4 curve;               // threshold: x threshold - knee, y 2 knee, z 0.25 / knee, w threshold (linear)
		Vector4 params;              // x scatter (the upsample's mix with its level)
	} fs_bloom_t;

	// u_fs_blurredScene (shaderlib/includes/blurred_scene.glsl): where a draw reads the blurred
	// scene of the frame (PostProcessingSettings::blurEnabled), for frosted UI panels
	typedef struct blurred_scene_t {
		Vector4 screenToUV;          // blurred texture UV = gl_FragCoord.xy * xy + zw
		Vector4 params;              // x 1 when the blurred scene is bound (0: black, none this frame)
	} blurred_scene_t;

	typedef struct fs_blit_t {
		// source UV = destination UV * xz + yw: (1, 0, 1, 0) copies, (1, 0, -1, 1) flips Y (a GL
		// swapchain destination), anything else reads a part of the source (a scene-colour copy
		// of a colour pass whose viewport is a part of its target)
		Vector4 params;
	} fs_blit_t;

	// u_fs_sceneInputs (shaderlib/includes/scene.glsl): what shaders requiring scenedepth /
	// scenecolor read of the camera pass, filled once per pass that has such a draw
	// (RenderSystem::fillSceneInputs)
	typedef struct scene_inputs_t {
		Matrix4 inverseViewProjection; // clip -> world, the camera's unflipped matrices
		Vector4 inverseProjectionZ;    // -(row z) of the inverse projection (view distance numerator)
		Vector4 inverseProjectionW;    // row w of the inverse projection
		Vector4 screenParams;          // screen UV (y up) = fragment coordinate * xy + zw
		Vector4 colorParams;           // colour copy v = uv.y * x + y, zw = 1 / copy size
		Vector4 depthParams;           // depth texture uv = screen uv * xy + zw
		Matrix4 viewProjection;        // world -> clip, unflipped (SCENE_VIEW_PROJECTION: the built-in Lit's transmission)
	} scene_inputs_t;

	typedef struct vs_points_params_t {
		Matrix4 mvpMatrix;
		float pointScale;
		float padding[3];
	} vs_points_params_t;

	// u_vs_linesParams of a ribbon: a thick line or a trail (includes/lines_vs.glsl, LINE_RIBBON)
	typedef struct vs_lines_ribbon_params_t {
		Matrix4 mvpMatrix;
		Matrix4 modelMatrix;
		Matrix4 viewProjection;
		// xyz world position (w 0), or the direction towards the camera (w 1, orthographic)
		Vector4 cameraPosition;
		// xyz world normal of TransformZ alignment, w 1 TransformZ / 0 View
		Vector4 alignAxis;
		// xy render target size in pixels
		Vector4 viewport;
	} vs_lines_ribbon_params_t;

	typedef struct check_load_t {
		Scene* scene;
		Entity entity;
	} check_load_t;

	// What the shader variant of a mesh depends on in its scene: the render system's state
	// when the mesh loads. The editor fills it from a scene's content to know every variant
	// a build has to ship.
	struct MeshShaderEnvironment {
		bool lights = false;            // punctual lights (Light components, or lighting forced on)
		int lightCapacity = 8;          // per-object lights tier (RenderSystem::getLightCapacity)
		bool shadows = false;           // a light casts shadows
		bool shadowsHQ = false;         // the shadow tier filters with the High filters ('Shq' variants)
		bool ibl = false;               // the skybox's environment maps are ready
		bool reflectionProbes = false;
		bool fog = false;
		bool lights2D = false;
		bool shadows2D = false;         // a shadow-casting 2D light and an occluder
		bool ssao = false;
		bool ssr = false;
		bool depthPrepass = false;
		// a loaded object reads the scene depth (@require scenedepth) or a post pass samples the
		// depth: every mesh has a depth program, so the opaque depth pre-pass can draw it
		bool sceneDepth = false;
		// the post-processing colour (phase 4, USE_POST_COLOR): one global bit, see
		// RenderSystem::getPostProcessingTiers
		bool postColor = false;
		// the scene's Lit Quality is High (SceneSettings::litQuality): every lit variant of the
		// built-in Lit, and of forks of it, takes USE_LIT_QUALITY (in its builtin:lit reference)
		bool litQualityHigh = false;
		// the lobes the build draws (MaterialPbr::getRuntimeTier; the exporter: the target's)
		PbrTier pbrTier = PbrTier::FULL;
		// the atmosphere's depth pre-pass needs every opaque mesh's depth program (volumetric
		// fog, volumetric clouds)
		bool atmosphereDepth = false;
		// S57 (RenderSystemLighting.cpp): the pixel lights come from screen tiles ('Til'), the
		// shadowed lit variants take capsule ('Cap') and contact ('Cts') shadows (contact shadows
		// need every opaque mesh's depth program), alpha-blended submeshes an OIT render ('Oit')
		bool tiledLights = false;
		bool capsuleShadows = false;
		bool contactShadows = false;
		bool oit = false;
		// every Light component of the scene is Directional (enabled or not, so a flashlight
		// switched on keeps the variant): the built-in Lit's 'Dlo' variant
		bool directionalLightsOnly = false;
		// the scene has a Water Excluder / a Water Wake (enabled or not): a shader requiring
		// waterexclusion / waterwake takes 'Wex' / 'Wwk' (includes/water_interaction.glsl)
		bool waterExclusion = false;
		bool waterWake = false;
	};

	// The uber pass's compile-time features (shader library phase 4): the property bits of a
	// ShaderType::BLIT variant (shaderlib/uber.vert / uber.frag; properties 0 is the plain
	// blit, blit.frag). Appended only: the bits are part of saved shader keys.
	enum UberFeature : uint32_t {
		UBER_FXAA         = 1u << 0,  // 'Fxa' FXAA 3.11 console, luma from green
		UBER_CHROMATIC    = 1u << 1,  // 'Cab' chromatic aberration (red and blue taps)
		UBER_PIXELATE     = 1u << 2,  // 'Pix' pixelate (UV snapped to blocks)
		UBER_CRT          = 1u << 3,  // 'Crt' CRT curvature, scanlines, mask, tube vignette
		UBER_EDGE         = 1u << 4,  // 'Edg' Sobel edges on luma
		UBER_GRAIN        = 1u << 5,  // 'Grn' film grain
		UBER_SHARPEN      = 1u << 6,  // 'Shp' sharpen
		UBER_LUT          = 1u << 7,  // 'Lut' the colour grade's LUT (display domain)
		UBER_VIGNETTE     = 1u << 8,  // 'Vig' vignette
		UBER_DITHER       = 1u << 9,  // 'Dth' dither
		UBER_BLOOM        = 1u << 10, // 'Blm' bloom composite (u_bloomTexture, the bloom chain's)
		UBER_BLUR         = 1u << 11, // 'Blr' blur composite (u_blurTexture, the blur chain's)
		UBER_UPSCALE      = 1u << 12, // 'Scl' none of the above: the plain upscale of a render scale below 1
		UBER_VHS          = 1u << 13, // 'Vhs' VHS tape (replaces the scene tap; never with FXAA or sharpen)
		UBER_HALATION     = 1u << 14, // 'Hal' halation composite (two levels of the bloom chain, or its own)
		UBER_FILM_DAMAGE  = 1u << 15, // 'Fdm' film damage: hairs, scratches, dust, gate weave and mask
		UBER_GRAIN_LENS   = 1u << 16, // 'Gln' the Lens film grain (with UBER_GRAIN)
		UBER_GRAIN_FILMIC = 1u << 17, // 'Gfm' the Filmic film grain (with UBER_GRAIN)
		UBER_FEATURE_COUNT = 18
	};

	// What a PostProcessingSettings turns on (shader library phase 4): the in-shader colour
	// (tier T0, USE_POST_COLOR in mesh, sky, points and lines variants), the uber pass (T1/T2)
	// and its features. RenderSystem::getPostProcessingTiers.
	struct PostProcessingTiers {
		// the shaders' variants apply the post-processing colour (exposure, curve, and without
		// the uber pass the grade, vignette and dither): the scene's variant bit
		bool tailColor = false;
		// the tail's LUT holds the grade too and the tail does vignette and dither (no uber pass)
		bool tailGrade = false;
		// the uber pass runs after the scene (one fullscreen pass, the fixed-resolution blit's)
		bool uber = false;
		// its BLIT variant bits (RenderSystem::UberFeature)
		uint32_t uberFeatures = 0;
		// the 3D scene's resolution scale (uber only) and MSAA samples (1, 2, 4); with dynamic
		// resolution on, renderScale is its upper bound and the applied scale the controller's
		// (RenderSystem::getAppliedRenderScale)
		float renderScale = 1.0f;
		int msaaSamples = 1;
		// halation without bloom runs its own two-pass chain (with bloom it reads bloom's levels)
		bool halationChain = false;
		// dynamic resolution drives the scale (S13): the uber pass stays on at any scale
		bool dynamicResolution = false;
	};

	// Dynamic resolution as the renderer applies it now (RenderSystem::getDynamicResolutionStatus,
	// MCP get_post_processing_status, Lua scene:getDynamicResolution())
	struct DynamicResolutionStatus {
		bool enabled = false;          // the settings ask for it
		bool running = false;          // the controller drives the scale (Play / the game, no fixed resolution)
		const char* idle = "off";      // why it does not run ("" while it does): off, edit mode, fixed resolution...
		bool overridden = false;       // a pinned scale (setDynamicResolutionOverride) replaces it
		float scale = 1.0f;            // the render scale the main camera draws at
		float minScale = 1.0f;         // its range
		float maxScale = 1.0f;
		const char* source = "none";   // the law of the last measurement: "gpu", "frame" or "none"
		float smoothedMs = 0.0f;       // its smoothed cost (GPU time or frame period), ms
		float targetMs = 0.0f;
		bool saturatedLow = false;     // at the min scale and still over the target
		bool ineffective = false;      // the scale buys too little for the content (GPU law)
		uint32_t acceptedChanges = 0;
		float probeSeconds = 0.0f;     // frame law: seconds until it probes one rung up (0: none)
	};

	// What the shader variant of a submesh depends on in its mesh and GameObject
	struct SubmeshShaderInputs {
		// vertex attributes, see RenderSystem::scanSubmeshAttributes
		bool texCoord1 = false;
		bool texCoord2 = false;
		bool normal = false;
		bool tangent = false;
		bool vertexColor3 = false;
		bool vertexColor4 = false;
		bool textureRect = false;
		bool skinning = false;
		bool morphTarget = false;
		bool morphNormal = false;
		bool morphTangent = false;
		// material
		bool baseColorTexture = false;
		bool metallicRoughnessTexture = false;
		bool normalTexture = false;
		bool occlusionTexture = false;
		bool emissiveTexture = false;
		// the UV set (0 = UV1, 1 = UV2) each map is read on (Material::*TexCoord), which the
		// G-buffer reads them on too
		int baseColorTexCoord = 0;
		int metallicRoughnessTexCoord = 0;
		int occlusionTexCoord = 0;
		MaterialAlphaMode alphaMode = MaterialAlphaMode::AUTO;
		// as the material asks; the variant resolves it against the shader (resolveLighting)
		MaterialLighting lighting = MaterialLighting::AUTO;
		bool textureShadow = false;
		// Material::stochasticTiling, as the material asks; the variant decides where it applies
		bool stochasticTiling = false;
		// the colour shader samples Lit's Stochastic Tiling (RenderSystem::supportsStochasticTiling:
		// the built-in Lit, a fork of it defining LIT_STOCHASTIC_PARAMS); a fork made before it
		// samples plainly, so its depth and G-buffer passes must too
		bool stochasticShader = true;
		// the shader is the engine's Lit (empty, or a builtin:lit reference; not a fork or a
		// library shader): the one 'Dlo' applies to
		bool builtinLit = true;
		// mesh
		bool receiveLights = true;
		bool receiveIBL = true;
		bool receiveShadows = true;
		bool castShadows = true;
		bool customDepthShader = false;
		// GameObject
		bool terrain = false;
		bool terrainBlendMaps = false;
		bool terrainSurface = false;
		bool instanced = false;
		bool instanceFade = false;
		bool mirror = false;
		// bound to lattices (LatticeMeshState::count > 0, never a terrain): the 'Ltc' variants,
		// with the vertex colour a mask channel reads
		bool lattice = false;
		// played by a Vertex Animator (VatMeshState::variant, never a terrain): the 'Vat' / 'Vab'
		// variants, without skinning or morph targets
		VatVariant vat = VatVariant::NONE;
		// shader library: the manifest of the shader the submesh draws with (null for the
		// built-in shader and forks without one) and whether its depth pass draws with the
		// library shader's own depth program (@depth)
		std::shared_ptr<const ShaderManifest> manifest;
		bool depthProgram = false;
		// the manifest's defines the material enables (ShaderReference::getEnabledFeatures),
		// which decide its feature= @require inputs; empty: the unconditional ones only
		std::vector<std::string> features;
		// the built-in Lit's extended lobes and hex tiling (MaterialPbr::getPbrVariant): the
		// submesh's material while the inputs are used, null for none (an extra pass, a shader
		// with a manifest)
		const Material* material = nullptr;
		// the material's transmission lobe on a shader without a manifest: drawn in the
		// transparent queue after the scene colour copy it refracts (no SSAO there)
		bool pbrTransmission = false;
		// a painted layer of the Terrain hex tiles (TERRAIN_HEX_TILING)
		bool terrainHex = false;
		// and a layer takes Stochastic Tiling (TERRAIN_HEX_STOCHASTIC, with terrainHex only)
		bool terrainHexStochastic = false;
	};

	// The shaders one submesh draws with
	struct SubmeshShaderVariant {
		uint64_t properties = 0;        // ShaderType::MESH
		bool depthPass = false;         // drawn into shadow maps and depth pre-passes
		bool depth = false;             // has a depth shader: a depth pass, or a forked depth shader
		uint64_t depthProperties = 0;   // ShaderType::DEPTH
		bool gbuffer = false;           // drawn into the SSR G-buffer
		uint64_t gbufferProperties = 0; // ShaderType::GBUFFER
		// the environment specular its colour pass adds, which the SSR composite swaps
		SSRSource ssrSource = SSRSource::SKIP;
		// The built-in Lit's defines beyond the property bits, sorted: the material's lobes,
		// maps and hex tiling (pbr), TERRAIN_HEX_TILING and USE_LIT_QUALITY. They travel in the
		// "builtin:lit#..." reference (a fork's "<fork>#..."), getSubmeshShaderReference; none:
		// the variant it always was (customId 0)
		std::vector<std::string> litDefines;
		PbrVariant pbr;
	};

	// The automatic GPU instancing variants of a submesh (spec S55 R1): its colour and depth
	// variants with HAS_AUTO_INSTANCING ('Aui'), each 0 when the submesh draws alone in those passes
	struct SubmeshBatchVariant {
		uint64_t properties = 0;        // ShaderType::MESH
		uint64_t depthProperties = 0;   // ShaderType::DEPTH
	};

	class FluidSystem;


	class JASPERENGINE_API RenderSystem : public SubSystem {
		// the scene's fluids, whose MATERIAL outputs the draws show (set by drawFramePasses;
		// null until then or without one)
		FluidSystem* frameFluids = nullptr;


		friend class AtmosphereRenderer;
	private:
		enum class TransparentRenderType{
			MESH,
			POINTS,
			PARTICLES,
			// a thick Line Renderer, a Trail Renderer (blended ribbons)
			LINES,
			TRAIL
		};

		struct TransparentRenderData{
			TransparentRenderType type;
			// the terrain layer uniforms are looked up by entity, so the sorted draw keeps it
			Entity entity;
			MeshComponent* mesh;
			PointsComponent* points;
			InstancedMeshComponent* instmesh;
			TerrainComponent* terrain;
			TilemapComponent* tilemap;
			Transform* transform;
			float distanceToCamera;
			// transparent queue order (MeshComponent::transparentSortPriority), lower first
			int sortPriority;
			// submission order: equal priority and distance draw in it, deterministically
			uint32_t order;
			ParticleSystemComponent* particles = nullptr;
			LineRendererComponent* lines = nullptr;
			TrailRendererComponent* trail = nullptr;
			// its rank among the pass's waters (render/WaterLayering.h): what a water covers
			// draws before it, what is in front of it after; and the distance it sorts at (its
			// own, or its waters' group's: waterPlace)
			int8_t waterRank = 0;
			float waterSortDistance = 0.0f;
		};

		// which of the LOD cross-fade's draws a pass makes (spec S54 R6-R8)
		enum class LodFadePass : uint8_t{
			ALL,    // all: single meshes as planned, an instance view's plain and dominant copies
			PLAIN,  // no fading draw: the depth pre-pass, an instanced mesh's first queue entry
			FADE    // only the fading draws: an instanced mesh's second queue entry (its fade copies)
		};

	public:
		// (public: their order is tested, RenderQueueOrderTest)
		// opaque mesh of a 3D pass, drawn front to back so early depth rejects covered fragments
		struct OpaqueRenderData{
			Entity entity;
			MeshComponent* mesh;
			InstancedMeshComponent* instmesh;
			TerrainComponent* terrain;
			TilemapComponent* tilemap;
			Transform* transform;
			float distanceToCamera;
			// MeshComponent::opaqueSortPriority, honoured before the distance (lower first)
			int sortPriority;
			// submission (transform) order: equal priority and distance draw in it
			uint32_t order;
			// cross-fading detail levels (spec S54 R6): drawn after the others, with the discard
			// variant (the plan of the view's last frame: only the order depends on it)
			bool fading = false;
			// an instanced mesh with fade copies has two entries: its plain draws, its fading ones
			LodFadePass lodPass = LodFadePass::ALL;
			// with GPU Instancing (spec S55): the submeshes this entry draws (one each for a mesh
			// that can batch; the rest of a mesh in one entry) and the batch key its draws group by
			// (VisibleList::groupBatches; 0: it batches with nothing); all and 0 without
			uint32_t submeshMask = 0xFFFFFFFFu;
			uint64_t batchKey = 0;
			// its bounds were culled by the visible list (drawMesh does not test them again)
			bool preCulled = false;
			// a colour pass's visible list sorted by state (buildVisibleQueue): its distance band
			// (distanceBand) and its render state (pipeline, material and mesh of its first draw),
			// so draws of one state run together inside a band and skip applying it again
			uint8_t band = 0;
			uint64_t stateKey = 0;
		};

		// One merged run of tilemap indices to draw, in elements relative to the
		// submesh index range (see selectTilemapChunks).
		struct TilemapDrawRange{
			unsigned int offset;
			unsigned int count;
		};

		// draw order of the transparent queue: the lowest sort priority, then around the waters
		// (waterSortOrder: the farthest sort distance, the higher rank, the farthest), then the
		// submission order (a total order, so std::sort is deterministic)
		// (Sorting Layers and Order in Layer, pixel-art S52, go before sortPriority; the water
		// order stays right after it)
		struct TransparentRenderComparison{
			bool operator()(const TransparentRenderData& lhs, const TransparentRenderData& rhs) const{
				if (lhs.sortPriority != rhs.sortPriority)
					return lhs.sortPriority < rhs.sortPriority;
				const int water = waterSortOrder(lhs.waterSortDistance, lhs.waterRank, lhs.distanceToCamera,
				                                 rhs.waterSortDistance, rhs.waterRank, rhs.distanceToCamera);
				if (water != 0)
					return water < 0;
				return lhs.order < rhs.order;
			}
		};

		// draw order of the opaque queue of a 3D pass: the lowest sort priority, then the meshes
		// that do not fade (the discard of fading ones turns off early depth and hidden surface
		// removal for their draws only, so they come after: Apple, PowerVR and Arm's order), then
		// the nearest distance band (one per doubling of the distance: early depth keeps most of
		// its use), then the render state (equal states in a run apply once), then the nearest,
		// then the submission order (a total order, so std::sort is deterministic)
		struct OpaqueRenderComparison{
			bool operator()(const OpaqueRenderData& lhs, const OpaqueRenderData& rhs) const{
				if (lhs.sortPriority != rhs.sortPriority)
					return lhs.sortPriority < rhs.sortPriority;
				if (lhs.fading != rhs.fading)
					return !lhs.fading;
				if (lhs.band != rhs.band)
					return lhs.band < rhs.band;
				if (lhs.stateKey != rhs.stateKey)
					return lhs.stateKey < rhs.stateKey;
				if (lhs.distanceToCamera != rhs.distanceToCamera)
					return lhs.distanceToCamera < rhs.distanceToCamera;
				return lhs.order < rhs.order;
			}
		};

	private:
		// the queues of the camera pass being drawn (renderCameraPass, the reflection probe
		// face), kept between passes so a pass allocates nothing once they have grown
		std::vector<OpaqueRenderData> opaqueQueue;
		std::vector<TransparentRenderData> transparentQueue;
		// what the mesh draws of the pass being drawn make of the LOD cross-fade
		LodFadePass passLodFade = LodFadePass::ALL;
		// a pass's visible list (spec S55 R4): its candidates and their results, kept between
		// passes like the queues
		std::vector<VisibleCandidate> visibleCandidates;
		std::vector<VisibleResult> visibleResults;
		std::vector<OpaqueRenderData> visibleEntries;
		std::vector<int> visiblePriorities;
		std::vector<uint64_t> visibleKeys;
		std::vector<uint32_t> visibleOrder;
		// the depth passes' lists (renderOpaqueDepth, a shadow slot's casters)
		std::vector<OpaqueRenderData> depthQueue;

		struct SpotMaskAtlasEntry{
			std::string sourceId;
			const std::array<TextureData, 6>* sourceOwner = nullptr;
			const void* sourcePixels = nullptr;
			int width = 0;
			int height = 0;
			float aspect = 1.0f;

			bool empty() const{
				return sourceId.empty();
			}

			bool operator==(const SpotMaskAtlasEntry& other) const{
				return sourceId == other.sourceId &&
					sourceOwner == other.sourceOwner &&
					sourcePixels == other.sourcePixels &&
					width == other.width &&
					height == other.height;
			}
		};

		Scene* scene;

		// visible ranges of the tilemap submesh being drawn, refilled by
		// selectTilemapChunks and consumed by the draw right after it
		std::vector<TilemapDrawRange> tilemapDrawRanges;

		// Editor-only viewport debug override: the scene camera's passes of draw() cull no
		// face (the frame is the Scene view in edit mode; a secondary view has its own,
		// see ViewDesc). Defaults to false and is never set at runtime, so exported games
		// are unaffected.
		bool disableFaceCulling = false;

		// Editor-only override that suppresses the scene's fixed game resolution
		// (edit-mode viewports render native; play mode re-enables it). Defaults
		// to false and is never set at runtime, so exported games are unaffected.
		bool disableFixedResolution = false;


		static uint32_t pixelsWhite[64];
		static uint32_t pixelsBlack[64];

		static TextureRender emptyWhite;
		static TextureRender emptyArrayWhite;
		static TextureRender emptyBlack;
		static TextureRender emptyTransparent;
		static uint32_t pixelsTransparent[64];
		static TextureRender emptyCubeBlack;
		static TextureRender emptyCubeWhite;
		static TextureRender emptyNormal;
		static TextureRender emptyShadowDepth;
		// shader library @texture defaults besides white and black
		static uint32_t pixelsGray[64];
		static uint32_t pixelsFlatNormal[64];
		static TextureRender emptyGray;
		static TextureRender emptyFlatNormal;
		// shader library @texture defaults "ripples" and "noise" (Water::makeRipplesTexture,
		// makeNoiseTexture)
		static constexpr int DEFAULT_PATTERN_SIZE = 128;
		static uint32_t pixelsRipples[DEFAULT_PATTERN_SIZE * DEFAULT_PATTERN_SIZE];
		static uint32_t pixelsNoise[DEFAULT_PATTERN_SIZE * DEFAULT_PATTERN_SIZE];
		static TextureRender defaultRipples;
		static TextureRender defaultNoise;
		// @texture default "particle" (Particle Systems' default quad): a soft white disc
		static constexpr int DEFAULT_PARTICLE_SIZE = 64;
		static uint32_t pixelsParticle[DEFAULT_PARTICLE_SIZE * DEFAULT_PARTICLE_SIZE];
		static TextureRender defaultParticle;
		static TextureRender emptyCubeGray;
		// zeros for the morph inputs a mesh's shader declares and the mesh has none of
		// (bindUnfedMorphInputs): one vertex buffer per size asked for, shared by every
		// RenderSystem (a scene may load on another thread: under the mutex) and released
		// with the static textures (bindings hold their handles)
		struct ZeroVertexBuffer {
			unsigned int size = 0;
			BufferRender render;
		};
		static std::vector<ZeroVertexBuffer> zeroVertexBuffers;
		static std::mutex zeroVertexBuffersMutex;
		// a buffer of at least `size` zero bytes into `buffer`, made when missing; false when
		// it cannot be
		static bool getZeroVertexBuffer(unsigned int size, BufferRender& buffer);

		static bool emptyTexturesCreated;
		// the RenderSystems drawing with the static textures above (their bindings hold the
		// handles), and whether this one is one of them: the last to go destroys them
		static int emptyTextureUsers;
		// the variant checks a texture edit ran (getTextureVariantChecks)
		static uint64_t textureVariantChecks;
		bool usesEmptyTextures = false;
		
		// what LatticeRender::prepare keeps between frames
		LatticeRenderState latticeState;
		// the meshes updateVertexAnimations set a Vertex Animation variant on last frame
		std::vector<Entity> vatMeshes;

		// (false until load() or the first light check: updateTransform reads hasLights, also
		// for a scene whose systems never loaded, as the physics tests resolve their transforms)
		bool hasLights = false;
		bool hasShadows = false;
		bool hasFog = false;
		bool hasIBL = false;
		bool hasReflectionProbes = false;
		// the scene has a Water Excluder / a Water Wake (enabled or not): the water variants'
		// 'Wex' / 'Wwk' (MeshShaderEnvironment), and the GPU side of WaterInteractionSystem
		bool hasWaterExcluders = false;
		bool hasWaterWakes = false;
		TextureRender waterExclusionMaps;
		int waterExclusionMapLayers = 0;
		// the frame's excluders (RGBA32F, 4 texels a row: includes/water_interaction.glsl), the
		// rows uploaded this frame and the frame they were
		TextureRender waterExclusionData;
		std::vector<Vector4> waterExclusionTexels;
		size_t waterExclusionRows = 0;
		uint64_t waterExclusionDataFrame = UINT64_MAX;
		uint64_t waterExclusionMapsVersion = 0;
		TextureRender waterWakeTexture;
		uint64_t waterWakeUploadFrame = UINT64_MAX;
		bool waterExcluderCapWarned = false;
		bool hasMultipleCameras = false;
		// extra cameras the last frame drew, to catch the switch back to main only
		bool lastMultiCameraDraw = false;
		bool capturingReflectionProbe = false;
		// batch order currently baked into the shared points/instance arrays
		bool lastBatchSort = true;
		// scene mesh LOD switch the levels were built (or freed) for
		bool lastMeshLodEnabled = false;
		// updateObjectLights zeroed every mesh's pick while the scene has no lights
		bool objectLightsCleared = false;
		// pipelines the objects were loaded with, to catch a destination change
		uint16_t loadedPipelines = 0;

		// Projected spotlight masks share one horizontal R8 atlas of SPOT_MASK_ATLAS_TILES
		// fixed-size tiles, given to the masked spot lights in scene order
		// (LightComponent::spotMaskTile).
		TextureRender spotMaskAtlas;
		std::array<SpotMaskAtlasEntry, SPOT_MASK_ATLAS_TILES> spotMaskAtlasEntries;
		std::vector<unsigned char> spotMaskAtlasPixels;
		bool spotMaskAtlasCreated;

		struct ReflectionProbeRuntime{
			FramebufferRender captureFramebuffer;
			CameraRender capturePass;
			std::shared_ptr<TextureRender> irradianceMap;
			std::shared_ptr<TextureRender> prefilteredMap;
			unsigned int resolution = 0;
			int nextFace = 0;
			float elapsed = 0.0f;
			float retryDelay = 0.0f;
			bool ready = false;
			bool captureInProgress = false;
			bool modeInitialized = false;
			ReflectionProbeMode lastMode = ReflectionProbeMode::STATIC;
			unsigned int observedCaptureRevision = 0;
			Vector3 capturePosition;
			Vector3 capturedPosition;
			// the capture prefiltered into one GGX roughness per mip level (filterReflectionProbes),
			// sampled instead of the capture once made
			TextureRender filteredMap;
			bool needFilter = false;
			// the image the authored maps were made of, and its TextureDataPool::generation
			std::string environmentId;
			uint32_t environmentGeneration = 0;
		};

		std::unordered_map<Entity, std::unique_ptr<ReflectionProbeRuntime>> reflectionProbeRuntimes;
		uint32_t probeImagesEpochSeen = 0;

		// the prefilter of finished runtime captures (probe_filter.frag), shared by the probes
		bool probeFilterLoaded = false;
		TextureRender probeSourceMap;  // box-filtered mip chain of the capture
		TextureRender probeReduceMap;  // scratch target while building that chain
		int probeSourceSize = 0;
		CameraRender probeFilterPassRender;
		ObjectRender probeFilterRender;
		std::shared_ptr<ShaderRender> probeFilterShader;
		fs_probe_filter_t fs_probe_filter;
		int probeFilterSlotParams = -1;
		// Each mirror's reflection camera. Kept here so the authored component stays copiable.
		std::unordered_map<Entity, Entity> mirrorCameras;
		// Main camera position for the instance distance fade: the colour and depth passes must
		// fade against the same point or a shadow outlives its instance.
		Vector3 fadeEyePosition;

		// culled instances of every instanced mesh for one view, uploaded once per frame
		struct InstanceView{
			std::vector<InstanceRenderData> data;
			ExternalBuffer buffer;
			size_t capacity = 0; // bytes of the GPU buffer
			// the Vertex Animation state of the instances of VAT batches (InstanceViewRange::
			// vatOffset), in a buffer of their own
			std::vector<InstanceVatRenderData> vatData;
			ExternalBuffer vatBuffer;
			size_t vatCapacity = 0;
			bool needUpload = false;
			// the GPU buffers hold data: false while an upload was refused (past the frame's stream
			// budget, tried again next frame), when its instances are not drawn rather than drawn
			// from what the buffer held before (spec S55 R6)
			bool current = true;
		};
		bool instanceViewsDirty;

		// error a view hides at a distance: threshold * (perUnit * distance + constant) / bias
		struct LodView{
			Vector3 origin;
			float perUnit = 0;   // perspective: units per pixel at distance 1
			float constant = 0;  // orthographic: units per pixel
			float threshold = 1;
			float bias = 1;      // the project's and device tier's LOD Bias (QualityResolver), times each mesh's
			float viewHeight = 1; // pixels (Cull Below Screen Height)
			bool enabled = false;
			// a camera view whose plan fades (planMeshLod): the main camera of draw() or of an
			// editor view; shadow, probe and other cameras select one level, statelessly
			bool fades = false;
			bool crossFade = false;  // the scene's Cross Fade
			float fadeDuration = 0.5f;
			float fadeDeltaTime = 0;  // the fade clock's seconds since the view's last frame
			uint64_t frame = 0;       // the view's frame, from 1
		};

		// where a pass keeps a UI element's clip rect: the RectTransform's own in draw()
		struct PassScissor{
			Rect* rect;
			bool* active;
		};

		// how a culled instance list is ordered
		enum class InstanceOrder{
			NEAREST,       // nearest first (along a direction when given), grouped by detail level
			KEEP,          // list order, full detail: the batch order is the layering
			BACK_TO_FRONT  // ascending along a direction, full detail: blended batches
		};

		// shadow cameras of one light fitted to a camera (directional cascades, near/far)
		struct LightCameraSet{
			LightCamera cameras[6];
			unsigned int numCascades = 1;
			// a directional light's cascade fit, kept for the fit freeze
			CascadeFitResult fit;
			bool hasFit = false;
		};

		static constexpr int SHADOW_KEY_SLOTS = (MAX_SHADOW_ATLAS_SLOTS > MAX_POINT_SHADOW_ATLAS_SLOTS) ? MAX_SHADOW_ATLAS_SLOTS : MAX_POINT_SHADOW_ATLAS_SLOTS;

		// What a shadow atlas holds: the shadow camera of each slot and the layers whose
		// casters went into it. Two atlases drawn after the same update() with the same key
		// hold the same depths, so a camera wanting one can sample the other.
		struct ShadowAtlasKey{
			// update() the atlas was drawn after, 0 when it holds nothing
			uint64_t epoch = 0;
			int slots = 0;
			Matrix4 viewProjection[SHADOW_KEY_SLOTS];
			uint32_t casterMask[SHADOW_KEY_SLOTS] = {};
			// the camera whose terrain cut, billboards and instance lists the casters were
			// drawn with, when such camera-dependent casters are in the masks
			const void* cameraCasters = nullptr;

			bool sameDepths(const ShadowAtlasKey& other) const;
		};

		// The lighting a camera's passes read and the shadow maps they sample: the scene
		// camera's, a render-to-texture, mirror or probe face camera's, an editor view's.
		// Directional cascades are fitted to the camera; spot and point shadow cameras are
		// the lights' own, so only the layers the camera draws can tell its maps apart. A
		// camera samples another camera's maps drawn after the same update() when they hold
		// the same depths, and draws its own otherwise.
		struct CameraShadows{
			// the camera part of the legacy u_fs_lighting (eyePos, cameraDir, globalIllum,
			// envColor, viewportInfo); its lights come from each object's list. The frame block
			// (includes/frame.glsl) of the camera's passes is RenderSystem::passFrame.
			fs_lighting_t fs_lighting;
			// the shadows its fragment stages sample (includes/shadow_data.glsl)
			fs_shadow_data_t fs_shadow_data;
			fs_point_shadow_data_t fs_point_shadow_data;
			// the legacy blocks of forks written before the fragment-stage shadows
			vs_shadows_t vs_shadows;
			fs_shadows_t fs_shadows;
			fs_point_shadows_t fs_point_shadows;
			// the camera its cascades are fitted to (set_shadow_debug moves it, not the view)
			Vector3 fitCameraPosition;
			Vector3 fitCameraForward = Vector3(0.0f, 0.0f, -1.0f);

			// the cascades below, else the lights' own (the scene camera); by the directional
			// light's base shadow slot
			bool ownLightCameras = false;
			std::array<LightCameraSet, MAX_SHADOW_ATLAS_SLOTS> lightCameras;

			// layers the camera draws, so the layers that cast into its maps
			uint32_t cameraMask = LAYER_MASK_EVERYTHING;
			// terrain cut its shadow passes draw
			int terrainView = 0;

			// what its own atlases hold (the frame's atlases for the scene camera)
			ShadowAtlasKey projectiveKey;
			ShadowAtlasKey pointKey;
			// update() projectiveKey and pointKey were last computed for
			uint64_t keysEpoch = 0;

			// atlases its passes sample: its own, another camera's, or null for the frame's
			FramebufferRender* projectiveAtlas = nullptr;
			FramebufferRender* pointAtlas = nullptr;
			// the raw-depth view of projectiveAtlas (PCSS)
			TextureRender* projectiveAtlasRaw = nullptr;
			// its own, created on first need, resized with the frame's layout (the point atlas
			// keeps only the rows the point lights use), freed when unused
			FramebufferRender ownProjective;
			FramebufferRender ownPoint;
			// the raw depths of ownProjective (PCSS)
			TextureRender ownProjectiveRaw;
			uint64_t ownProjectiveGeneration = 0;
			uint64_t ownPointGeneration = 0;
			// the shadow cache of each slot of its atlases (ShadowCache); the frame's caches
			// with a static layer, the other cameras' only skip or redraw
			std::array<ShadowSlotState, MAX_SHADOW_ATLAS_SLOTS> projectiveSlots;
			std::array<ShadowSlotState, MAX_POINT_SHADOW_ATLAS_SLOTS> pointSlots;
			// how far into the caster change log (RenderSystem::casterChangeLog) its atlases'
			// slots were evaluated: the changes after it are the ones they have not seen
			uint64_t projectiveChangesSeen = 0;
			uint64_t pointChangesSeen = 0;
			int ownProjectiveWidth = 0;
			int ownProjectiveHeight = 0;
			int ownPointWidth = 0;
			int ownPointHeight = 0;
			// its own atlases are drawn for this update()
			bool drawProjective = false;
			bool drawPoint = false;

			// culled casters of each projective slot, when its own maps are drawn from the
			// frame's instance lists (render-to-texture, mirror and probe cameras)
			bool ownShadowInstances = false;
			InstanceView instanceViews[MAX_SHADOW_ATLAS_SLOTS];
			struct InstanceRanges{
				std::array<InstanceViewRange, MAX_SHADOW_ATLAS_SLOTS> slots;
				uint64_t build = 0;
			};
			std::unordered_map<Entity, InstanceRanges> instanceRanges;
			uint64_t instanceBuild = 0;
			uint64_t instanceRevision = 0;
			uint64_t instanceShadowHash = 0;

			// its screen-tile light lists (Light Mode Tiled; null until a Tiled pass draws)
			std::unique_ptr<TiledLightsView> tiledLights;

			// draw count it was last prepared in (unused ones are freed)
			uint64_t lastUsed = 0;

			CameraShadows() = default;
			~CameraShadows();
			CameraShadows(const CameraShadows&) = delete;
			CameraShadows& operator=(const CameraShadows&) = delete;

			void releaseProjective();
			void releasePoint();
			void releaseInstances();
			// every GPU resource it holds
			void release();
		};

		// a target of the scene inputs (shader library phase 3): the opaque depth pre-pass
		// (packed depth, nearest) or the scene-colour copy (linear) of a camera pass
		struct SceneInputTarget{
			Framebuffer framebuffer;
			bool color = false;
			unsigned int width = 0;
			unsigned int height = 0;
			uint64_t lastUsed = 0; // drawCount
			uint64_t lastViewDraw = 0; // RenderView::sceneInputDraws
		};

		// one camera's color pass (see renderCameraPass)
		struct CameraPass{
			CameraComponent* camera = nullptr;
			Transform* cameraTransform = nullptr;
			// the camera's entity (NULL_ENTITY: a mirror's or a view's own camera)
			Entity entity = NULL_ENTITY;
			// the view camera: culled instances, detail levels, SSAO, SSR, post-process, fixed resolution
			bool isMain = false;
			// draws into camera->framebuffer
			bool renderToTexture = false;
			// a water reflection pass (renderWaterReflections): no planar reflection reader,
			// no scene input reader, the viewer's detail levels
			bool reflection = false;
			// terrain node cut the pass draws (TERRAIN_PASS_VIEW: the secondary pass's own)
			int terrainView = 0;
		};

#ifdef JASPERENGINE_EDITOR
		static constexpr int TERRAIN_PASS_VIEW = -1;

		struct PassScissorState{
			Rect rect;
			bool active = false;
		};

		// What a camera pass of a secondary view reads per object in place of the
		// Transform, which keeps the frame's own camera. Indexed like the Transform array.
		struct PassObjects{
			// the Transform array the indices refer to, as it was when they were built
			const Transform* base = nullptr;
			size_t count = 0;
			std::vector<Matrix4> mvp;
			std::vector<float> distance;
			// a real billboard turns to this pass's camera, so it and its children get
			// their own model and normal matrices; -1 keeps the Transform's
			std::vector<int> override;
			std::vector<Matrix4> overrideModel;
			std::vector<Matrix4> overrideNormal;
			// UI clip rects in this pass's target
			std::vector<PassScissorState> scissors;
			// terrain node cut of this pass's camera, and the bytes its buffers hold
			std::unordered_map<Entity, TerrainView> terrainViews;
			std::unordered_map<Entity, size_t> terrainCapacity;
			Matrix4 skyViewProjection;
		};

		// instances a view culls from: its own list when they depend on the camera
		// (billboards, distance fade), else the component's
		struct ViewInstanceSource{
			std::vector<InstanceRenderData> instances;
			std::vector<InstanceBounds> bounds;
			std::vector<InstanceVatRenderData> vat; // parallel to instances for a VAT batch
			AABB worldAABB;
			bool own = false;
			Vector3 fadeEyeLocal;
		};

		// transparent points sorted for a view's camera
		struct PointsView{
			std::vector<PointRenderData> points;
			ExternalBuffer buffer;
			size_t capacity = 0; // bytes of the GPU buffer
		};

		// a mirror's reflection seen from a view's camera
		struct MirrorView{
			Framebuffer framebuffer;
			CameraComponent camera;
			Transform transform;
			PassObjects objects;
			Matrix4 viewProjection; // oblique, for the mirror surface's projective lookup
			// lighting and shadow maps of the reflection camera
			CameraShadows shadows;
			bool ready = false;

			MirrorView();
			~MirrorView();
			MirrorView(const MirrorView&) = delete;
			MirrorView& operator=(const MirrorView&) = delete;
		};
#endif

		// Everything a frame draws that depends on the view it draws: the camera's
		// derived data (culled instances, detail levels, lighting and shadow uniforms)
		// and the targets sized to the view (SSAO, SSR, fixed resolution, post-process).
		// draw() uses primaryView; the editor's secondary views keep their own, so a
		// second view in the same frame reallocates nothing and disturbs nothing.
		// The bloom or the blur chain of a view (phase 4, T2): SRGB8A8 targets without depth at
		// 1/4, 1/8, 1/16 and 1/32 of the uber pass's source, the way down (down[i]) and up
		// (up[i], the size of down[i]); made for the levels in use, resized in place
		struct PostChainTargets{
			static const int LEVELS = 4;
			Framebuffer down[LEVELS];
			Framebuffer up[LEVELS - 1];
			unsigned int sourceWidth = 0;
			unsigned int sourceHeight = 0;
			int levels = -1;              // the down levels below the first that exist
			bool upLevels = true;         // the way up exists (halation's chain has none)
			void destroy();
			bool isCreated() const{ return levels >= 0; }
		};

		// A water reflection pass of a view (renderWaterReflections): the view's camera mirrored
		// about the plane of the waters at one height, drawn into its own target; reused from
		// frame to frame, freed once unused for a while
		struct WaterReflectionPass{
			float level = 0.0f;
			// drawn this frame for the view's camera, and the frame (drawCount) it last was
			bool active = false;
			uint64_t lastUsed = 0;
			Framebuffer framebuffer;
			CameraComponent camera;
			Transform transform;
			// the viewer's lighting and shadow maps (strengths 0 without the scene's shadows)
			CameraShadows shadows;
#ifdef JASPERENGINE_EDITOR
			PassObjects objects;
#endif

			WaterReflectionPass();
			~WaterReflectionPass();
			WaterReflectionPass(const WaterReflectionPass&) = delete;
			WaterReflectionPass& operator=(const WaterReflectionPass&) = delete;
		};

		// what one pass draws of a mesh
		struct MeshDraw{
			unsigned int instanceCount = 1;
			unsigned int instanceFirst = 0;
			unsigned int vatFirst = 0;     // the first instance's Vertex Animation state
			// the level a pass drawing one level draws (the plan's dominant one)
			unsigned int lod = 0;
			// the view's plan (planMeshLod): a fading one draws both levels in the colour pass
			LodDraw plan;
		};

		// a mesh the SSR G-buffer pass draws (renderGBufferPass collects them first, so only
		// drawn meshes take a probe slot)
		struct GBufferDraw{
			Entity entity = NULL_ENTITY;
			MeshComponent* mesh = nullptr;
			Transform* transform = nullptr;
			InstancedMeshComponent* instmesh = nullptr;
			TerrainComponent* terrain = nullptr;
			TilemapComponent* tilemap = nullptr;
			MeshDraw draw;
			Matrix4 normalMatrix;
			Entity probe = NULL_ENTITY;   // the reflection probe it is under (its ENVIRONMENT submeshes)
			float probeWeight = 0.0f;
			TextureRender* probeTexture = nullptr;
			fs_reflection_probe_t probeParams;
		};

		// the SSR state of a view: the G-buffer pass's draws and probe slots, read by its composite
		struct SSRView{
			std::vector<GBufferDraw> draws;
			std::vector<SSRProbeCandidate> candidates;
			SSRProbeAssignment slots;
			SSRProbeAssignment previousSlots;
			TextureRender* slotTextures[SSR_PROBE_SLOTS] = {nullptr, nullptr, nullptr, nullptr};
			fs_reflection_probe_t slotParams[SSR_PROBE_SLOTS];
			SSRViewStats stats;
		};

		struct RenderView{
			InstanceView instanceViews[1 + MAX_SHADOW_ATLAS_SLOTS];
			LodView lodView;
			// the detail levels this view drew: the frame being drawn, and the last one done
			LodFrameStats lodStats;
			LodFrameStats lodStatsDone;
			// the fade clock's reading at the view's last frame (planMeshLod)
			double lodClock = -1.0;
#ifdef JASPERENGINE_EDITOR
			// the fades of a secondary view (the main view's live in MeshComponent::lodFade)
			std::unordered_map<Entity, MeshLodFade> lodFades;
#endif

			// lighting and shadow maps of the view camera
			CameraShadows shadows;

			unsigned int ssaoWidth = 0;
			unsigned int ssaoHeight = 0;
			Framebuffer ssaoDepthFramebuffer; // color = packed depth, depth = z-test
			Framebuffer ssaoFramebuffer;      // raw AO
			Framebuffer ssaoBlurFramebuffer;  // blurred AO (sampled by meshes)
			TextureRender* currentSSAOTexture = nullptr; // AO bound to meshes this camera (or empty white)

			unsigned int ssrWidth = 0;
			unsigned int ssrHeight = 0;
			Framebuffer gbufferFramebuffer;    // MRT: color[0] packed depth, color[1] view-space normal/roughness/metallic,
			                                   // color[2] albedo + reflection source, color[3] occlusion
			SSRView ssr;
			// the contact shadows' mask and the OIT targets of its main camera (null: none drawn)
			std::unique_ptr<LightingViewTargets> lightingTargets;
			Framebuffer sceneColorFramebuffer; // offscreen opaque scene color (top-left)
			Framebuffer ssrFramebuffer;        // reflection color + mask (logical orientation)
			Framebuffer ssrBlurFramebuffer;    // glossy-blurred reflection

			unsigned int fixedResWidth = 0;
			unsigned int fixedResHeight = 0;
			Framebuffer fixedResFramebuffer;   // offscreen scene color at the fixed size

			// the post-processed camera's scene, at the render scale, that the uber pass draws to
			// the destination (phase 4; with fixed resolution the uber pass reads fixedResFramebuffer)
			unsigned int postSceneWidth = 0;
			unsigned int postSceneHeight = 0;
			Framebuffer postSceneFramebuffer;

			unsigned int postProcessWidth = 0;
			unsigned int postProcessHeight = 0;
			// the user chain's full-size targets: 0 takes the scene (depth-stencil), 1 and 2 the
			// passes (2 only while a pass after the first reads the original scene, which then
			// stays in 0)
			Framebuffer postProcessFramebuffer[3];
			// the chain's passes below full size (PostProcessPass::scale): two targets per size,
			// made when a size is used, freed in the frame it is not
			struct PostScaledTargets{
				unsigned int width = 0;
				unsigned int height = 0;
				Framebuffer framebuffer[2];
				bool used = false;
			};
			std::vector<std::unique_ptr<PostScaledTargets>> postScaledTargets;

			// the bloom and blur chains (phase 4), and halation's own two levels without bloom (S13)
			PostChainTargets bloomTargets;
			PostChainTargets blurTargets;
			PostChainTargets halationTargets;
			// the multisampled scene target of the post-processed camera (PostProcessingSettings::
			// msaaSamples): its colour pass draws into it and resolves into its usual target
			Framebuffer msaaFramebuffer;
			unsigned int msaaWidth = 0;
			unsigned int msaaHeight = 0;
			int msaaSamples = 1;
			// what the multisampled scene resolves into when the camera would draw to the
			// swapchain (exported builds without another offscreen effect): blitted to it after
			Framebuffer msaaResolveFramebuffer;

			// scene inputs (shader library phase 3) of the camera passes drawn in this view:
			// packed depth and colour copies by size, reused across passes and frames, freed
			// once unused for a while (releaseSceneInputTargets)
			std::vector<std::unique_ptr<SceneInputTarget>> sceneInputTargets;
			uint64_t sceneInputDraws = 0;   // the drawCameras() of this view, the current one included
			// the scene camera's colour pass when only the scene colour needs it offscreen
			// (exported builds drawing to the swapchain): blitted to the swapchain after
			Framebuffer sceneColorRedirect;
			// the water reflection passes of the view's camera, and which one each reflecting mesh
			// reads this frame (none: its fallback)
			std::vector<std::unique_ptr<WaterReflectionPass>> waterReflections;
			std::unordered_map<Entity, int> waterReflectionOf;
			// the camera they were drawn for (its pass alone reads them), and the view whose
			// passes waterReflectionOf indexes (null: this one; an editor view through the
			// frame's own camera reads the frame's)
			const CameraComponent* waterReflectionCamera = nullptr;
			RenderView* waterReflectionSource = nullptr;
			// the scene's post-processing applies to the view camera (ViewDesc::postProcessing;
			// always for the frame's own view)
			bool postProcessing = true;
			// the atmosphere's targets of the view (volumetric fog, clouds) and what it ran
			AtmosphereViewTargets atmosphere;
			// editor: the view draws without the volumetric clouds, and at this atmosphere
			// quality (the Game view's preview; -1 the project's)
			bool disableClouds = false;
			int atmosphereQuality = -1;

#ifdef JASPERENGINE_EDITOR
			bool secondary = false;
			// where the view draws
			Framebuffer* target = nullptr;
			Rect viewRect;
			// the scene under the others: clears the target and runs the post-process chain
			bool baseLayer = true;
			bool editorOverlays = false;
			bool fixedResolution = false;
			// draws as if the scene had no fog (the fog variant stays, its factor is 1)
			bool disableFog = false;
			// its camera passes cull no face (the Scene view's debug toggle)
			bool disableFaceCulling = false;
			// the Scene view's Draw Mode (ViewDesc::drawMode)
			ViewDrawMode drawMode = ViewDrawMode::SHADED;
			// the view's copy of its camera, sized for the view: the scene's camera
			// component is never touched (it may be drawn by the frame or another view)
			CameraComponent camera;
			// drawn through the camera draw() used this frame: culling, detail levels,
			// terrain cut, shadow cascades and mirrors are the frame's own
			bool sharesPrimaryCamera = false;

			PassObjects objects;

			std::unordered_map<Entity, std::array<InstanceViewRange, 1 + MAX_SHADOW_ATLAS_SLOTS>> instanceRanges;
			std::unordered_map<Entity, ViewInstanceSource> instanceSources;
			// what the instance views were last built from
			bool instancesBuilt = false;
			uint64_t instanceRevision = 0;
			Matrix4 instanceViewProjection;
			Vector3 instanceEye;
			LodView instanceLodView;
			uint64_t instanceShadowHash = 0;

			std::unordered_map<Entity, PointsView> points;
			std::unordered_map<Entity, std::unique_ptr<MirrorView>> mirrors;
#endif

#ifdef JASPERENGINE_EDITOR
			// the camera copy borrows the scene camera's framebuffer, it owns none
			RenderView(){
				delete camera.framebuffer;
				camera.framebuffer = nullptr;
			}
#else
			RenderView() = default;
#endif
			RenderView(const RenderView&) = delete;
			RenderView& operator=(const RenderView&) = delete;
		};

		RenderView primaryView;

		// lighting and shadow maps of each render-to-texture, mirror and probe face camera
		// draw() renders, by camera (or probe) entity; freed some draws after its last use
		std::unordered_map<Entity, std::unique_ptr<CameraShadows>> cameraShadows;
		// what the camera pass being drawn reads in place of the view's (null: rv().shadows)
		CameraShadows* passShadows = nullptr;
		// bumped by update(): maps drawn after the same one hold the same scene
		uint64_t shadowEpoch = 1;
		// drawCameras() calls, the clock of cameraShadows' lastUsed
		uint64_t drawCount = 0;
		// layers holding shadow casters this epoch, and those holding casters that depend on
		// the camera drawing them (terrain cut, billboards, instance lists culled from it)
		uint32_t shadowCasterLayers = 0;
		uint32_t cameraCasterLayers = 0;
		// by light: layers holding casters that can reach its maps (all casters for a
		// directional light, those inside its shadow frustums for a spot or point light)
		std::vector<uint32_t> lightCasterLayers;
		uint64_t casterLayersEpoch = 0;
		// what a camera being prepared wants its atlases to hold
		ShadowAtlasKey wantedProjectiveKey;
		ShadowAtlasKey wantedPointKey;
		// bumped each time the frame's instance views are rebuilt
		uint64_t instanceRevision = 0;

#ifdef JASPERENGINE_EDITOR
		RenderView* activeView = &primaryView;
		std::unordered_map<uint32_t, std::unique_ptr<RenderView>> secondaryViews;
		// per-object data of the secondary camera pass being drawn, null in draw()
		PassObjects* passObjects = nullptr;
		// the mirror whose reflection a secondary view is drawing (it cannot show itself)
		Entity passExcludedEntity = NULL_ENTITY;
		// editor gizmos and icons living in this scene, by entity
		std::vector<uint8_t> editorOverlayEntities;
		// whether draw() shows them; each secondary view chooses its own
		bool drawEditorOverlays = true;
		// the pass being drawn shows them
		bool passDrawsOverlays = true;
		// pipelines the secondary views' cameras need
		uint16_t viewPipelines = 0;
		// draw() ran since the last update(): the frame's shared passes are done
		bool frameDrawn = false;
		// the camera pass being drawn culls no face (a Scene view's debug toggle)
		bool passDisablesFaceCulling = false;
#endif
		// the camera pass being drawn is the editor's Scene view (CameraComponent::editorView)
		// with GameObjects hidden there (EDITOR_HIDDEN): isLayerVisible leaves those out
		bool passHidesEditorHidden = false;

		// whether the pass being drawn culls no face (always false outside the editor)
		bool passNoFaceCulling() const{
#ifdef JASPERENGINE_EDITOR
			return passDisablesFaceCulling;
#else
			return false;
#endif
		}

		// the view being drawn: primaryView in draw(), a secondary view in drawView()
		RenderView& rv(){
#ifdef JASPERENGINE_EDITOR
			return *activeView;
#else
			return primaryView;
#endif
		}
		const RenderView& rv() const{
#ifdef JASPERENGINE_EDITOR
			return *activeView;
#else
			return primaryView;
#endif
		}

		// the lighting and shadow maps of the camera pass being drawn
		CameraShadows& ps(){
			return passShadows ? *passShadows : rv().shadows;
		}
		const CameraShadows& ps() const{
			return passShadows ? *passShadows : rv().shadows;
		}

		struct ShadowSlot{
			int slot;
			LightComponent* light;
			int lightIndex;
			int cameraIndex;
		};
		// collectShadowSlots' list, kept so collecting allocates nothing once warm
		std::vector<ShadowSlot> shadowSlots;

		Entity activeReflectionProbe = NULL_ENTITY;
		// the scene's sky: its first SkyboxComponent, as last loaded (another one leads the
		// array after a removal or a reorder, see update)
		Entity activeSky = NULL_ENTITY;
		fs_reflection_probe_t fs_reflection_probe;

		// 2D light path (Light2DComponent). Deliberately independent of
		// scene->getLightState(): the editor forces LightState::OFF in 2D scenes
		// to keep 3D lights out, but 2D lights must still work there.
		bool hasLights2D = false;
		bool hasShadows2D = false;
		int numLights2D = 0;

		// Shadows (RenderSystemShadows.cpp, docs/rendering/shadows.md). The projective atlas
		// (directional cascades and spot lights) and the point atlas (cube faces) are depth-only
		// D32 comparison atlases laid out by the slot planner, each slot at its own resolution;
		// the static caches hold the static casters' depths of the slots that have a static
		// layer, copied back under the dynamic casters (ShadowCache).
		FramebufferRender shadowAtlasFramebuffer;
		TextureRender shadowAtlasRaw;               // its raw depths (PCSS)
		CameraRender shadowAtlasPassRender;
		bool hasShadowAtlas;

		FramebufferRender shadowPointAtlasFramebuffer;
		TextureRender shadowPointAtlasRaw;          // its raw depths (the Atlas debug view)
		CameraRender shadowPointAtlasPassRender;
		bool hasShadowPointAtlas;

		FramebufferRender shadowStaticAtlas;
		TextureRender shadowStaticAtlasRaw;
		FramebufferRender shadowStaticPointAtlas;
		TextureRender shadowStaticPointAtlasRaw;
		CameraRender shadowStaticPassRender;
		// bumped when an atlas is recreated: what its slots held is gone
		uint64_t shadowAtlasGeneration = 1;
		uint64_t shadowPointAtlasGeneration = 1;
		uint64_t shadowStaticGeneration = 1;
		uint64_t shadowStaticPointGeneration = 1;
		// update() a static cache was last used in (freed after STATIC_CACHE_KEEP_UPDATES)
		uint64_t shadowStaticUsed = 0;
		uint64_t shadowStaticPointUsed = 0;
		static constexpr uint64_t STATIC_CACHE_KEEP_UPDATES = 300;

		// the slots and atlas layouts of this update (the planner's)
		ShadowSlotPlanner shadowPlanner;
		ShadowPlan shadowPlan;
		std::vector<ShadowLightRequest> shadowRequests;
		std::vector<int> shadowRequestLight;        // scene light index of each request
		// the shadow casters and what changed in them this update
		ShadowCasterRegistry shadowCasters;
		// the caster changes of the updates since the oldest the frame's atlases have not
		// evaluated (update() can run more than once per draw: the editor updates the scene
		// before its frame does); casterChangeBase is the log position of its first entry.
		// An atlas that falls behind what the log keeps redraws its slots.
		std::vector<ShadowCasterChange> casterChangeLog;
		uint64_t casterChangeBase = 0;
		static constexpr size_t CASTER_CHANGE_LOG_MAX = 16384;
		// update() clock of the shadow caches
		uint64_t shadowFrame = 0;
		// counts the updates (never 0 after the first), the clock of immutableChanges
		uint32_t updateSerial = 0;
		// the update a content-shared immutable buffer of the entity's mesh last took new bytes in
		// (refreshImmutableBuffer); a second change within the window goes private
		std::unordered_map<Entity, uint32_t> immutableChanges;
		static constexpr uint32_t IMMUTABLE_CHANGE_WINDOW = 30;
		// the update a dynamic shadow caster was last seen in (static layers are kept a while after)
		uint64_t shadowDynamicSeen = 0;
		// the quality tier this update (Scene Quality, else the project's tier of the platform)
		ShadowTier shadowTier = ShadowTier::HIGH;
		bool shadowHardOnly = false;
		// the mesh variants receiving shadows have the High filters (MeshShaderEnvironment::shadowsHQ)
		bool shadowHQVariants = false;
		// the requested tier was capped by the shaders this build cooked (warned once)
		bool shadowTierCapWarned = false;
		// the shadow cache's copy and clear program (and the atlas debug view's)
		ObjectRender shadowRestoreRender;
		std::shared_ptr<ShaderRender> shadowRestoreShader;
		int shadowRestoreSlotParams = -1;
		std::pair<int, int> shadowRestoreSourceSlot = {-1, -1};
		bool shadowRestoreLoaded = false;
		static TextureRender emptyShadowDepthRaw;

	public:
		// set_shadow_debug (editor, transient): moves only the camera the frame's cascades are
		// fitted to (the stability tests), holds every slot as drawn, or redraws every slot
		struct ShadowDebugSettings{
			Vector3 fitCameraOffset = Vector3(0.0f, 0.0f, 0.0f);   // world units
			Vector3 fitCameraEuler = Vector3(0.0f, 0.0f, 0.0f);    // pitch, yaw, roll (degrees)
			bool freezeCache = false;
			bool forceRerender = false;
		};

		// get_shadow_stats: what the last update's shadow passes did
		struct ShadowSlotStats{
			int slot = -1;
			bool point = false;
			ShadowRect rect;
			unsigned int resolution = 0;
			ShadowSlotPath path = ShadowSlotPath::SKIP;            // this update
			ShadowDirtyCause cause = ShadowDirtyCause::NONE;
			ShadowSlotPath renderPath = ShadowSlotPath::SKIP;      // its last draw
			ShadowDirtyCause renderCause = ShadowDirtyCause::NONE;
			ShadowRect dirtyRect;                                  // what its last draw wrote
			uint64_t framesSinceRender = 0;
			bool staticLayer = false;
		};
		struct ShadowLightStats{
			Entity entity = NULL_ENTITY;
			LightType type = LightType::DIRECTIONAL;
			Vector3 direction = Vector3(0.0f, -1.0f, 0.0f);   // world direction the light points
			int firstSlot = -1;
			unsigned int cascades = 0;
			float shadowDistance = 0.0f;
			float splitFar[MAX_SHADOWCASCADES] = {};
			float radius[MAX_SHADOWCASCADES] = {};
			float texelWorld[MAX_SHADOWCASCADES] = {};
			unsigned int resolution[6] = {};
			std::vector<ShadowSlotStats> slots;
		};
		struct ShadowStats{
			ShadowTier tier = ShadowTier::HIGH;
			bool hardOnly = false;
			unsigned int projectiveWidth = 0, projectiveHeight = 0;
			unsigned int pointWidth = 0, pointHeight = 0;
			unsigned int staticWidth = 0, staticHeight = 0;
			unsigned int staticPointWidth = 0, staticPointHeight = 0;
			uint64_t memoryBytes = 0;
			uint64_t frame = 0;
			// draw calls of the frame atlas's passes this update, by slot path (index: ShadowSlotPath)
			uint32_t drawCalls[5] = {};
			uint32_t passes = 0;            // render passes of the frame's shadow maps this update
			std::vector<ShadowLightStats> lights;
		};
		void setShadowDebug(const ShadowDebugSettings& settings);
		const ShadowDebugSettings& getShadowDebug() const{ return shadowDebug; }
		const ShadowStats& getShadowStats() const{ return shadowStats; }

		// S57's lighting this update and its last frame (get_render_stats "lighting")
		struct LightingStats{
			LightMode lightMode = LightMode::AUTOMATIC;   // the project's
			LightingTier tier = LightingTier::HIGH;
			bool tiledLights = false;
			int tileColumns = 0;
			int tileRows = 0;
			bool tileStatsValid = false;
			TiledLights::Stats tiles;
			bool capsuleShadows = false;
			int capsuleCasters = 0;
			int capsules = 0;
			bool contactShadows = false;
			int contactWidth = 0;
			int contactHeight = 0;
			bool oit = false;
			bool oitFallback = false;
			int oitDraws = 0;
		};
		LightingStats getLightingStats() const;
		// the shadow quality tier the scene renders with
		ShadowTier getEffectiveShadowTier() const{ return shadowTier; }
		// the atlases as a colour image (the Atlas debug view): null unless that view is on
		TextureRender* getShadowAtlasDebugTexture();

	private:
		ShadowDebugSettings shadowDebug;
		ShadowStats shadowStats;
		FramebufferRender shadowAtlasDebugTarget;
		CameraRender shadowAtlasDebugPassRender;
		ObjectRender shadowAtlasViewRender;
		std::shared_ptr<ShaderRender> shadowAtlasViewShader;
		bool shadowAtlasViewLoaded = false;

		// 1D polar shadow atlas for 2D lights: width x MAX_LIGHTS_2D, one row per
		// shadow-casting Light2D. ShadowCaster2D segments of the whole scene are merged
		// into one world-space line-list buffer and drawn once per light row.
		FramebufferRender shadow2DAtlasFramebuffer;
		CameraRender shadow2DAtlasPassRender;
		unsigned int shadow2DAtlasWidth;
		bool hasShadow2DAtlas;

		bool shadowCaster2DLoaded;
		ExternalBuffer shadowCaster2DBuffer;
		std::vector<float> shadowCaster2DSegments; // 4 floats per vertex: endpoint xy + other-endpoint xy
		ObjectRender shadowCaster2DRender;
		std::shared_ptr<ShaderRender> shadow2DShader;
		int shadow2DSlotParams;
		unsigned int shadowCaster2DBufferCapacity; // in vertices
		unsigned int shadowCaster2DVertexCount;    // vertices built this frame
		// sokol allows one sg_update_buffer per buffer per frame, but the same scene
		// can be drawn twice in a frame (e.g. open as a tab AND layered as a child
		// scene), so the upload is flagged in update() and consumed by the first draw
		bool shadowCaster2DNeedUpdateBuffer;

		bool needUpdateShadowBindings;

		// Unified lighting: every light of the scene in the form the per-object lists copy
		// (lights.glsl layout), rebuilt each update() by updateSceneLights. The lights whose
		// data changed since the previous update are in changedLights; each mesh picks the
		// lights reaching it in updateObjectLights (MeshComponent::lights), honouring the
		// lights' culling masks, and drawMesh uploads them per draw.
		struct SceneLight{
			Vector4 position_type;    // xyz world position, w type (0 directional, 1 point, 2 spot)
			Vector4 direction_range;  // xyz world direction, w range
			Vector4 color_intensity;  // rgb linear colour, w intensity
			Vector4 cone_shadow;      // inner, outer cone cos, shadow slot, cascades
			Vector4 spot_mask;        // xyz projection up, w mask aspect (0 = none)
			// as the per-object lights block holds it: position_type, direction_inner,
			// color_outer, spot_shadow (its w filled per draw)
			Vector4 packed[4];
			uint32_t cullingMask = LAYER_MASK_EVERYTHING;
			int spotMaskTile = -1;
			LightShading shading = LightShading::BOTH;
			bool active = false;      // enabled, on an active entity, giving light

			bool operator==(const SceneLight& other) const;
		};
		std::vector<SceneLight> sceneLights;
		std::vector<SceneLight> previousSceneLights;
		std::vector<Entity> sceneLightEntities;
		std::vector<int> changedLights;
		// bumped when every mesh has to pick its lights again: lights added, removed or
		// reordered, the scene ambient or the cap changed
		uint64_t lightListEpoch;
		Vector3 lightListAmbient;
		int lightListCap;
		// capacity tier the meshes loaded their variants with
		int lightCapacityTier;
		// every Light component is Directional (checkLightsAndShadow), and as the meshes loaded
		bool directionalLightsOnly = false;
		bool loadedDirectionalLightsOnly = false;
		bool directionalLightsOnlyKnown = false;
		// a point or spot Light was in the scene since load(): the general Lit stays until the
		// next load, so lights that come and go never reload every mesh again
		bool nonDirectionalLightSeen = false;
		// the Lit Quality and the lobe tier the meshes loaded their variants with
		LitQuality loadedLitQuality;
		PbrTier loadedPbrTier;
		// per-draw scratch of drawMesh
		object_lights_t objectLightsBlock;
		fs_lighting_t legacyLightingBlock;
		// S57: the fragment block of a Tiled variant, the capsule and contact shadows' block and
		// what the scene's lighting features are this update
		tiled_object_lights_t tiledObjectLightsBlock;
		// the fragment block of the lights' records form, the frame's records (a row of 512
		// texels holds 128 scene lights) and the meshes drawing with them (none: no texture)
		static constexpr int LIGHT_RECORDS_WIDTH = 512;
		object_light_records_t lightRecordsBlock;
		// a lights' block with the records form's tail, and where the frame's capsules and 2D
		// lights start among the records (after the scene lights)
		Vector4 lightBlockScratch[24];
		int lightDataCapsuleBase = 0;
		int lightData2DBase = 0;
		TextureRender lightRecords;
		// what the texture holds (uploaded only when a record changed) and the next frame's
		std::vector<Vector4> lightRecordTexels;
		std::vector<Vector4> lightRecordScratch;
		uint64_t lightRecordsFrame = UINT64_MAX;
		bool lightRecordsUploaded = false;
		std::unordered_set<Entity> lightRecordMeshes;
		light_occlusion_t lightOcclusionBlock;
		LightingFeatureState lighting;
		// the camera pass being drawn: its contact mask (null: white), whether it draws OIT
		TextureRender* passContactMask = nullptr;
		bool passOit = false;
		// a Capsule Shadow Caster set to Capsules Only was applied to the meshes
		bool capsuleShadowsOnlyMarked = false;
		fs_lighting2d_t fs_lighting2d;
		fs_fog_t fs_fog;
		// the height fog of the camera pass being drawn (u_fs_heightFog), and the atmosphere:
		// volumetric fog, clouds and their composites (AtmosphereRenderer)
		height_fog_t passHeightFog;
		AtmosphereRenderer atmosphere;
#ifdef JASPERENGINE_EDITOR
		std::unordered_map<uint32_t, AtmosphereInfo> keptAtmosphereInfo;
#endif
		// the scene fog is a height fog (its fogged meshes evaluate it: u_fs_heightFog)
		bool heightFogActive = false;
		// the variant state the meshes loaded with (needReloadMeshVariants on a change)
		bool atmosphereDepthPrograms = false;

		// SSAO: per-frame fullscreen passes for the main camera. The depth pre-pass
		// renders camera-space packed depth, then the ssao + blur fullscreen passes
		// produce an AO texture sampled by lit meshes (USE_SSAO).
		bool ssaoLoaded;
		CameraRender ssaoPassRender;      // drives the offscreen SSAO passes
		ObjectRender ssaoRender;          // fullscreen ssao.frag draw
		ObjectRender ssaoBlurRender;      // fullscreen ssao_blur.frag draw
		TextureRender ssaoNoiseTexture;
		std::shared_ptr<ShaderRender> ssaoShader;
		std::shared_ptr<ShaderRender> ssaoBlurShader;
		fs_ssao_t fs_ssao; // kernel filled once in loadSSAO; matrices/params per frame
		fs_ssao_blur_t fs_ssao_blur;
		int ssaoSlotParams;
		int ssaoBlurSlotParams;

		// engine-written custom uniforms: seconds since startup and the scene clock
		// (Scene::getSceneTime), sampled once per draw(), and the size of the target being
		// drawn (shadow slot, SSAO depth or camera color)
		float frameTime;
		float frameSceneTime = 0.0f;
		Vector2 passResolution;

		// the engine frame block (u_vs_frame / u_fs_frame) of the camera pass being drawn,
		// filled once per pass and uploaded to every draw whose shader declares it, and the
		// built-in Lit's part of it (u_fs_litFrame)
		frame_t passFrame;
		// (its lit member is the prefix every Lit variant declares; a variant with the post
		// colour uploads from postColor), one per PostColorBlend (built-in Lit meshes take the
		// one of their blend)
		lit_frame_block_t passLitFrameBlocks[POST_BLEND_COUNT];
		// the post-processing colour's parameters of the camera pass (post_color_t): the
		// scene's in the main camera's colour pass, neutral elsewhere (updatePassFrame), as
		// each PostColorBlend takes them (u_fs_postColor)
		post_color_t passPostColors[POST_BLEND_COUNT];

		// SSR: per-frame fullscreen passes for the main camera. The opaque color pass
		// renders into sceneColorFramebuffer, the ssr pass marches the depth pre-pass
		// and samples that color, then the composite pass blends reflections into the
		// real render target.
		bool ssrLoaded;
		CameraRender gbufferPassRender;    // drives the MRT G-buffer geometry pass
		CameraRender ssrPassRender;        // drives the offscreen ssr + composite passes
		ObjectRender ssrRender;            // fullscreen ssr.frag draw
		ObjectRender ssrBlurRender;        // fullscreen ssr_blur.frag draw
		ObjectRender compositeRender;      // fullscreen composite.frag draw
		std::shared_ptr<ShaderRender> ssrShader;
		std::shared_ptr<ShaderRender> ssrBlurShader;
		std::shared_ptr<ShaderRender> compositeShader;
		fs_ssr_t fs_ssr;
		fs_ssr_blur_t fs_ssr_blur;
		fs_composite_t fs_composite;
		int ssrSlotParams;
		int ssrBlurSlotParams;
		int compositeSlotParams;
		int compositeSlotFog;              // u_fs_fog (the distance fog the swap is attenuated by)
		int compositeSlotHeightFog;        // u_fs_heightFog
		std::pair<int, int> compositeProbeSlots[SSR_PROBE_SLOTS]; // u_probeTexture0..3
		std::pair<int, int> compositeOcclusionSlot;               // u_occlusionGBufferTexture
		std::pair<int, int> compositeSSAOSlot;                    // u_ssaoTexture
		std::pair<int, int> ssrAlbedoSlot;                        // ssr.frag's u_albedoTexture
		bool ssrProbeOverflowWarned = false; // the overflow warning, once until the next scene load
		bool swapchainRedirect;            // exported builds: the last pass targets the swapchain
		// exported builds, a scene with a scene-colour reader (phase 3): the scene camera may draw
		// offscreen (allowed: its pipelines exist), does this frame (a reader in view, or seen
		// in the last frames: hold), and the frames its target went unused
		bool sceneColorRedirectAllowed;
		bool sceneColorRedirect;
		int sceneColorRedirectHold;
		int sceneColorRedirectIdle;
		bool mvpFlipped = false;           // the Y flip the MVP matrices were made with

		// Fixed game resolution: when enabled on the Engine main scene (Scene
		// fixedResolution settings), the main camera renders into
		// fixedResFramebuffer and a fullscreen blit pass upscales it to the
		// real destination (view rect).
		bool blitLoaded;
		// the uber pass (phase 4): a BLIT variant with the features of postTiers, replacing the
		// fixed-resolution blit when that is on, else one pass after the scene
		bool uberLoaded = false;
		uint32_t uberFeatures = 0;         // the variant loaded
		CameraRender uberPassRender;
		ObjectRender uberRender;
		std::shared_ptr<ShaderRender> uberShader;
		int uberSlotVSParams = -1;
		int uberSlotFSParams = -1;
		std::pair<int, int> uberSlotSource = {-1, -1};
		std::pair<int, int> uberSlotLut = {-1, -1};
		std::pair<int, int> uberSlotBloom = {-1, -1};
		std::pair<int, int> uberSlotBlur = {-1, -1};
		std::pair<int, int> uberSlotHalation = {-1, -1};
		std::pair<int, int> uberSlotHalationWide = {-1, -1};
		vs_uber_t vs_uber;
		fs_uber_t fs_uber;
		// what the uber pass composites (the bloom chain's output and its blurred copy, phase 4
		// T2): null binds black (bloom) and the scene itself (blur), so a variant with the
		// feature compiles and draws without them
		TextureRender* uberBloomTexture = nullptr;
		TextureRender* uberBlurTexture = nullptr;
		// halation's two levels (1/4 and 1/8: the bloom chain's way down, or its own chain)
		TextureRender* uberHalationTexture = nullptr;
		TextureRender* uberHalationWideTexture = nullptr;
		// The bloom and blur chain (phase 4, T2; shaderlib/bloom.frag): one program per pass kind
		enum BloomProgramKind{
			BLOOM_PREFILTER_THRESHOLD,   // 'Pre,Thr' the bloom's prefilter
			BLOOM_PREFILTER_PLAIN,       // 'Pre' the blur's prefilter
			BLOOM_DOWN,                  // dual-filter downsample
			BLOOM_UP_MIX,                // 'Ups,Mix' the bloom's upsample (with its level by scatter)
			BLOOM_UP,                    // 'Ups' the blur's upsample
			BLOOM_PROGRAMS
		};
		struct BloomProgram{
			std::shared_ptr<ShaderRender> shader;
			ObjectRender render;
			bool loaded = false;
			int slotVSParams = -1;
			int slotFSParams = -1;
			std::pair<int, int> slotSource = {-1, -1};
			std::pair<int, int> slotLevel = {-1, -1};
		};
		BloomProgram bloomPrograms[BLOOM_PROGRAMS];
		// MSAA of the post-processed camera (PostProcessingSettings::msaaSamples, as the device
		// has it): the sample count the objects' PIP_MSAA pipelines are made with (1: none)
		int msaaPipelineSamples = 1;
		// exported builds drawing to the swapchain: the multisampled scene resolves offscreen
		// and is blitted (updateSwapchainRedirect)
		bool msaaRedirect = false;
		CameraRender bloomPassRender;
		vs_bloom_t vs_bloom;
		fs_bloom_t fs_bloom;
		// the blurred scene of the frame's last base-layer post-processed camera, for the draws
		// after it in the same view (layer scenes: frosted UI panels; includes/blurred_scene.glsl),
		// null while there is none; shared by every scene's RenderSystem
		static TextureRender* frameBlurredScene;
		static blurred_scene_t frameBlurredSceneBlock;
		CameraRender fixedResPassRender;   // drives the upscale blit pass
		ObjectRender blitRender;           // fullscreen blit.frag draw
		std::shared_ptr<ShaderRender> blitShader;
		fs_blit_t fs_blit;
		int blitSlotParams;

		// User post-process chain: forked fullscreen shaders run after the color (and
		// SSR) pass, ping-ponging between two buffers into the real destination.
		struct PostProcessRuntime{
			std::shared_ptr<ShaderRender> shader;
			ObjectRender render;
			int passIndex;                         // index in the scene chain (disabled passes skipped)
			uint16_t customId;                     // 0 = built-in passthrough
			CustomUniformBlock params;             // u_fs_postParams, slot -1 when the fork has none
			std::pair<int, int> slotSceneColor;
			std::pair<int, int> slotDepth;
			std::pair<int, int> slotGBuffer;
			std::pair<int, int> slotSSAO;
			// the scene before the chain (phase 4: u_originalSceneTexture)
			std::pair<int, int> slotOriginal = {-1, -1};
			// its manifest (a library shader's or a fork's, for its @property defaults), its
			// textures and what it reads of the engine's shader-library data (frame block...)
			std::shared_ptr<const ShaderManifest> manifest;
			std::vector<std::pair<std::string, Texture>> textures;
			ShaderLibraryBindings bindings;
		};
		bool postProcessLoaded;
		bool postProcessNeedReload;
		bool postProcessNeedUpdateUniforms;    // rewrite pass values, no chain rebuild
		uint64_t postProcessGlobalsVersion = 0; // ShaderGlobals version the pass values bind
		// the scene's PostProcessingSettings changed since they were applied (phase 4)
		bool postProcessingDirty = true;
		// the QualityResolver revision the post-processing tiers, the LOD picks and the SSAO
		// variants follow (a device tier, a thermal step)
		uint32_t qualityRevisionSeen = 0;
		bool qualitySSAO = false;
		// what the applied settings turn on (getPostProcessingTiers), and their colour LUTs
		PostProcessingTiers postTiers;
		// MeshShaderEnvironment::postColor the objects load with (the variant bit)
		bool postColorVariants = false;
		// the bit changed: the loaded points, lines and sky keep drawing their variant until the
		// new ones are built, then reload (as meshes do, needReloadMeshVariants)
		bool postColorReloadPending = false;
		// the in-shader LUT (shaper domain, includes/post_color.glsl), side postTailLutSize
		TextureRender postTailLut;
		int postTailLutSize = 0;
		// its shaper's k (getPostColorShaper), post_color_t::shaper.x
		float postTailShaper = 1.0f;
		// linearSampler's texture
		TextureRender postLinearSampler;
		// the uber pass's grade LUT (display domain), side postGradeLutSize
		TextureRender postGradeLut;
		int postGradeLutSize = 0;
		// Dynamic resolution (S13): the controller of the post-processed main camera's render
		// scale, ticked once per frame (drawFramePasses) while the settings enable it and fixed
		// resolution is off, in the frame's own view only; its scale replaces the tiers' render
		// scale there (getAppliedRenderScale) and is never written to the settings
		DynamicResolutionController dynamicResolution;
		bool dynamicResolutionRunning = false;
		// the game's events were paused at the last tick: the editor's edit mode (and Pause), where
		// the controller does not run and a pinned scale does not apply
		bool dynamicResolutionEditPaused = false;
		// why the controller did not run this frame ("" when it did), DynamicResolutionStatus::idle
		const char* dynamicResolutionIdle = "off";
		float dynamicResolutionScale = 1.0f;
		// what the controller is configured with, and the view it measured (a change resets it)
		DynamicResolutionConfig dynamicResolutionConfig;
		unsigned int dynamicResolutionViewWidth = 0;
		unsigned int dynamicResolutionViewHeight = 0;
		uint64_t dynamicResolutionFrame = 0;
		// the last frame's uber pass rendered the scene at the applied scale (the sample is valid)
		bool dynamicResolutionConnected = false;
		// > 0: the applied scale pinned (Lua setDynamicResolutionOverride, MCP force_render_scale)
		float dynamicResolutionOverride = 0.0f;
		// > 0: GPU milliseconds given to the controller in place of a measurement (MCP fake_gpu_ms)
		float dynamicResolutionFakeGpuMs = 0.0f;
		// running, it keeps the GPU timer on for its GPU law (S15 R5)
		bool dynamicResolutionGpuTiming = false;
		void setDynamicResolutionGpuTiming(bool on);
		// >= 0: the uber pass's time pinned, seconds (MCP set_post_processing_debug time_seconds)
		double postDebugTime = -1.0;
		// the controller's tick for the frame
		void updateDynamicResolution();

		// the user's LUT (PostProcessingSettings::lutTexture: a .cube, its cooked .jlut or a PNG
		// strip), shared through the CubeLut cache by path; null when none or it failed to load
		std::string postLutPath;
		std::shared_ptr<const CubeLut> postLut;
		// the bake buffer, kept between bakes
		std::vector<uint8_t> postLutPixels;
		bool postProcessKeepsOriginal = false; // a pass after the first reads the original scene
		bool postProcessNeedsDepth;            // a pass samples the depth texture
		bool postProcessNeedsGBuffer;          // a pass samples the G-buffer (needs SSR)
		CameraRender postProcessPassRender;    // drives the offscreen post passes
		std::vector<PostProcessRuntime> postProcessPasses;

		// Scene inputs (shader library phase 3, includes/scene.glsl). The loaded objects whose
		// shaders (main draw or extra pass) require scenedepth / scenecolor, by entity, with
		// those ShaderRequire bits: empty in a scene without any, and then a camera pass does
		// nothing for them. A pass with a visible requiring draw gets the opaque depth (the
		// pre-pass SSAO, SSR or a post pass already ran, else its own) and, for scenecolor, one
		// copy of its colour target after the opaque queue and the sky.
		std::unordered_map<Entity, uint32_t> sceneInputMeshes;
		// the meshes drawn with a water (library:water, @role water) and those reading a planar
		// reflection (@require planarreflection), as their last load resolved them: empty in a
		// scene without, so the passes skip the water layering and the reflection passes at once
		std::unordered_set<Entity> waterMeshes;
		std::unordered_set<Entity> planarReflectionMeshes;
		std::unordered_map<Entity, uint32_t> sceneInputPoints;
		std::unordered_map<Entity, uint32_t> sceneInputParticles;
		uint32_t sceneInputUsage = 0;          // the bits of both maps
		bool sceneDepthPrograms = false;       // MeshShaderEnvironment::sceneDepth the meshes load with
		// updates since the last reader of the depth / colour went: the depth programs and the
		// exported scene camera's offscreen pipelines stay a while (a reader spawned again soon
		// reloads nothing)
		uint32_t sceneDepthIdleUpdates = 0;
		uint32_t sceneColorIdleUpdates = 0;
		bool sceneColorReaders = false;        // the colour readers kept (sceneColorRedirectAllowed's)
		// the loaded opaque meshes whose depth the SSR G-buffer does not hold as the depth
		// pre-pass draws it (a library @depth program or depth fork moving or clipping them,
		// the opaque submeshes of a split mesh): a pass reading the scene depth with one of
		// them visible makes its own pre-pass instead of taking the G-buffer's
		std::unordered_set<Entity> gbufferDepthMismatch;
		// which submeshes a drawMesh / drawMeshDepth call draws: a split mesh
		// (MeshComponent::splitQueues) draws its opaque ones in the opaque queue and the
		// depth pre-passes, its blended ones in the transparent queue
		// OIT: an OIT camera's accumulation pass (its submeshes with an OIT render; the other
		// queues leave them out while passOit)
		enum class SubmeshQueue { ALL, OPAQUE, TRANSPARENT, OIT };
		SubmeshQueue passSubmeshQueue = SubmeshQueue::ALL;
		// the submeshes the draw being made draws (a visible list's entry: OpaqueRenderData::
		// submeshMask), and whether that list culled it already (spec S55 R4: drawMesh and
		// drawMeshDepth test the frustum again otherwise)
		uint32_t passSubmeshMask = 0xFFFFFFFFu;
		bool passPreCulled = false;
		// the GPU Instancing setting the meshes were loaded with (a change reloads them: the
		// batch variants are only loaded while it is on)
		bool loadedGpuInstancing = true;
		// the camera pass being drawn: its block and textures (null: the neutral fallbacks)
		scene_inputs_t passSceneBlock;
		TextureRender* passSceneDepth = nullptr;
		TextureRender* passSceneColor = nullptr;
		CameraRender sceneInputPassRender;     // drives the scene depth pre-pass and the colour copy
		ObjectRender sceneColorCopyRender;     // blit.frag into a copy target (PIP_EFFECT)
		std::shared_ptr<ShaderRender> sceneColorCopyShader;
		bool sceneColorCopyLoaded = false;
		int sceneColorCopySlotParams = -1;
		// what the passes of the last drawCameras() did for them (diagnostics, logRenderObjects)
		struct SceneInputStats{
			uint32_t passes = 0;           // camera passes with a requiring draw
			uint32_t depthPrePasses = 0;   // scene depth pre-passes run for them
			uint32_t sharedDepth = 0;      // passes that reused the SSAO / SSR / post-process depth
			uint32_t colorCopies = 0;
			uint32_t colorRecopies = 0;    // copies taken again for readers over other readers
			uint32_t targetsCreated = 0;   // since the scene started (reuse keeps it constant)
		};
		SceneInputStats sceneInputStats;
		SceneInputStats lastSceneInputStats;

		// FLATTEN Canvas Groups (ui canvas group spec 7.10): before a camera's colour pass, each
		// layered group's subtree (its UI elements and UI particles) is drawn into a pooled
		// target the size of the pass target, cleared to transparent (innermost groups first,
		// so a nested layer is composited into its parent's). The colour pass composites the
		// layer at the group's place in the Hierarchy order, premultiplied at layerAlpha,
		// scissored to the subtree's bounds, and skips the UI it holds
		struct CanvasLayer{
			Framebuffer framebuffer;
			uint64_t lastUsedFrame = 0;
			// used by the camera pass being drawn, for the layers at these bounds (siblings
			// elsewhere on screen share a target)
			bool inUse = false;
			std::vector<Rect> usedRects;
		};
		struct PassCanvasLayer{
			size_t layer = 0;           // index in canvasLayers
			Rect bounds;                // pass target pixels the subtree covers
			bool hasBounds = false;
		};
		std::vector<std::unique_ptr<CanvasLayer>> canvasLayers;
		std::unordered_map<Entity, PassCanvasLayer> passCanvasLayers;
		CameraRender canvasLayerPass;
		ObjectRender canvasCompositeObject;
		std::shared_ptr<ShaderRender> canvasCompositeShader;
		int canvasCompositeVsSlot = -1;
		int canvasCompositeFsSlot = -1;
		std::pair<int, int> canvasCompositeTextureSlot = {-1, -1};
		bool canvasCompositeLoaded = false;
		uint64_t canvasLayerFrame = 0;
		uint64_t canvasLayerTargetsCreated = 0;
		bool loadCanvasComposite();
		void destroyCanvasLayers();
		// the layers of one camera pass (passCanvasLayers), drawn outside any pass
		void renderCanvasGroupLayers(CameraComponent& camera, Transform& cameraTransform, const Rect& colorPassViewport,
		                             unsigned int targetWidth, unsigned int targetHeight, bool perCameraMVP, uint32_t cullingMask);
		// composites a pass layer into the current pass (the pass's pipeline type and samples)
		void drawCanvasLayer(Entity group, const PassCanvasLayer& layer, float alpha, PipelineType pipelineType, int msaaSamples,
		                     const Rect& colorPassViewport, unsigned int targetWidth, unsigned int targetHeight, CameraRender& render);
		// releases the layers unused for 120 frames
		void trimCanvasLayers();
		// the render points of Points whose uiGroupAlpha changed since the update
		void refreshGroupFadedPoints();
		// a Points GameObject in the UI hierarchy (a RectTransform on it or above): a UI
		// particle, which Canvas Groups fade and FLATTEN layers hold
		bool isUIParticle(Entity entity) const;

		static void changeLoaded(void* data);
		static void changeDestroy(void* data);

		static bool samplesCameraTarget(const CameraComponent& camera, const MeshComponent& mesh);
		static bool samplesCameraTarget(const CameraComponent& camera, const Texture& texture);
		// the entity is active in hierarchy and none of its renderer components
		// (mesh, sprite, model, tilemap, terrain, instances, particles, points, lines, UI) is disabled
		bool isRenderActive(Entity entity, Signature signature) const;
		bool isRenderActive(Entity entity) const;
		// the entity's layer is in a camera, light or probe culling mask; Everything costs one compare
		bool isLayerVisible(Entity entity, uint32_t cullingMask) const;
		// enabled light on an entity active in hierarchy
		bool isLightActive(Entity entity, const LightComponent& light) const;
		bool isLight2DActive(Entity entity, const Light2DComponent& light) const;
		bool isRenderingFlipped(const CameraComponent& camera) const;
		bool isFixedResolutionActive() const;
		void updateSwapchainRedirect();
		void updateSceneColorRedirect(CameraComponent& mainCamera);
		void updateMVP(size_t index, Transform& transform, CameraComponent& camera, Transform& cameraTransform);

		void createEmptyTextures();
		static void destroyEmptyTextures();
		int checkLightsAndShadow();
		bool loadLights(int numLights);
		// the camera part of the lighting (frame block, legacy block) and the shadow uniforms of target for camera
		void processLights(CameraShadows& target, int numLights, const CameraComponent& camera, const Transform& cameraTransform);
		void updateSpotMaskAtlas(int numLights);
		// unified lighting: the scene's lights, then the lights each mesh is lit by
		void updateSceneLights();
		void updateObjectLights();
		void selectMeshLights(Entity entity, MeshComponent& mesh, const Transform& transform);
		// the object's lights a submesh variant (its lighting mode and capacity) takes, pixel
		// lights then vertex lights, as scene light indices
		void orderObjectLights(const MeshComponent& mesh, uint64_t properties, int order[MAX_LIGHTS_PER_OBJECT], int& pixelCount, int& vertexCount) const;
		// the per-object lights block of a submesh variant for the camera pass being drawn
		void buildObjectLights(const MeshComponent& mesh, uint64_t properties, object_lights_t& block) const;
		int passLightCascades(const SceneLight& light) const;
		// the legacy u_fs_lighting of an older fork: the object's first pixel lights
		void buildLegacyLighting(const MeshComponent& mesh, uint64_t properties, fs_lighting_t& block) const;

		//-----S57 lighting (RenderSystemLighting.cpp)-----
		// what the variants take this update (Light Mode, capsule and contact shadows, OIT): a
		// change reloads them
		void updateLightingFeatures();
		// after the scene lights: the Capsules Only meshes, the nearest casters' capsules and the
		// lights taking capsule and contact shadows (their spot_shadow.w bits 12 and 13)
		void updateLightingOcclusion(const Transform& mainCameraTransform);
		int lightOcclusionBits(int sceneLight) const;
		// the object's lights of a variant into its blocks (the Tiled form for 'Til')
		void applyObjectLights(ObjectRender& render, int slotVS, unsigned int sizeVS, int slotFS, unsigned int sizeFS,
		                       std::pair<int, int> slotLightRecords, const LightDataTail& tail, const MeshComponent& mesh,
		                       uint64_t properties, uint32_t& builtLightBits);
		// the records form's tail of a variant's lights' block (includes/lights.glsl LIGHT_DATA_TAIL)
		static LightDataTail resolveLightDataTail(ShaderData& shaderData);
		// its values for a draw of the mesh in the pass being drawn, into the block's bytes
		void writeLightDataTail(const LightDataTail& tail, const MeshComponent& mesh, uint64_t properties, unsigned char* block, size_t size) const;
		// the records form's block (includes/lights.glsl, LIGHTS_FS_RECORDS): the object's pixel
		// lights as rows of u_lightRecords
		void buildObjectLightRecords(const MeshComponent& mesh, uint64_t properties, object_light_records_t& block) const;
		// a pixel light's spot_shadow.w in the pass being drawn: its shadow slot, cascades, mask
		// tile and occlusions (vertex lights: none)
		float packObjectLightShadow(int sceneLight, bool perPixel) const;
		// the frame's light records, once a frame while a mesh draws with them; a float texture
		// of at least the rows given (false: none)
		void syncLightRecords();
		bool ensureLightRecords(int rows);
		void setLightRecordsMesh(Entity entity, bool records);
	public:
		// the light records' uploads so far (tests: none while no light changes)
		uint64_t lightRecordUploads = 0;
		const TextureRender& getLightRecordsTexture() const { return lightRecords; }
	private:
		// a submesh variant's S57 blocks and textures: resolved at load, bound per draw
		void loadLightingFeatureSlots(Submesh& submesh, ShaderData& shaderData);
		void applyLightingFeatures(Submesh& submesh, ObjectRender& render, const MeshComponent& mesh);
		// the Hair shading model's values into a lobe variant's u_fs_pbrParams
		void writePbrHairBlock(const Submesh& submesh, unsigned char* block, size_t size) const;
		// the screen-tile lists of the pass being drawn: the job started (begin) once its view is
		// known, the texture uploaded before the colour pass (finish; synchronous: both)
		void beginTiledLights(CameraShadows& target, const CameraComponent& camera, const Transform& cameraTransform,
		                      const Rect& viewport, unsigned int targetHeight, bool flipped);
		void finishTiledLights(CameraShadows& target);
		// the contact shadows' mask of the main camera from its packed depth (null: none)
		TextureRender* renderContactShadows(CameraComponent& camera, TextureRender* depth, unsigned int width, unsigned int height);
		// the block of the pass's capsule and contact shadows, before its colour pass
		void fillLightOcclusion(const Rect& colorViewport, unsigned int targetHeight, bool flipped);
		// OIT: whether this colour pass draws it (supported, a target with depth to share), the
		// accumulation pass of its OIT submeshes and its resolve over the colour pass
		bool usesOit(const CameraComponent& camera, int msaaSamples) const;
		bool hasOitDraws(const std::vector<TransparentRenderData>& renders);
		bool renderOitAccumulation(CameraComponent& camera, Transform& cameraTransform, FramebufferRender* colorTarget, int msaaSamples,
		                           const Rect& viewport, bool viewportApplied, const std::vector<TransparentRenderData>& renders,
		                           int terrainView, int instanceView, const LodView* lodView);
		void drawOitResolve(PipelineType colorPip, int msaaSamples);
		// an alpha-blended submesh's OIT render (loadMesh, after its colour render), and its release
		bool loadSubmeshOit(MeshComponent& mesh, unsigned int submeshIndex, uint64_t colorProperties, uint16_t customShaderId,
		                    const RenderState& colorState, bool transparentQueue);
		void finishSubmeshOit(Submesh& submesh);
		void releaseSubmeshOit(Submesh& submesh);
		bool drawsOit(const Submesh& submesh) const;
		// frees what the features left unused (their targets, passes and programs)
		void releaseLightingFeatures(bool all);
		// asks for the fullscreen programs of the features on (contact shadows, the OIT resolve)
		// the frame they turn on, so the scene's load waits for them (lightingPassesLoading)
		void requestLightingPasses();
		bool lightingPassesLoading() const;
		// the engine-internal members of the frame block (lighting, environment, viewport, fog),
		// filled by updatePassFrame after the contract members (fillFrameBlock)
		void fillFrameLighting(frame_t& frame, lit_frame_t& litFrame, const CameraComponent& camera) const;
		// a material asks for a lighting mode its shader does not list (logged once per pair)
		static void warnUnsupportedLighting(const Material& material, const std::string& shader, const ShaderManifest* manifest,
		                                    const std::string& entityName, Entity entity, unsigned int submesh);
		// PCF tap radius of the 3D shadow filter (Scene shadowQuality)
		float shadowPCFRadius() const;
		// rgb environment colour (linear), w environment rotation (radians)
		Vector4 environmentInfo() const;
		void loadSpotMaskTexture(ShaderData& shaderData, ObjectRender& render);
		bool loadLights2D();
		void processLights2D();
		bool ensureShadow2DAtlas(unsigned int width);
		void loadShadow2DTexture(ShaderData& shaderData, ObjectRender& render, bool receiveShadows2D);
		unsigned int buildShadowCaster2DSegments();
		bool loadShadowCaster2DPass(unsigned int vertexCapacity);
		void destroyShadowCaster2DPass();
		bool loadAndProcessFog();
		void releaseSkyEnvironment(SkyboxComponent& sky);
		void updateSkyEnvironment(SkyboxComponent& sky);
		void updateReflectionProbes(double dt);
		void renderReflectionProbeCapture();
		void loadProbeFilter();
		void destroyProbeFilter();
		void filterReflectionProbes();
		void releaseReflectionProbeMaps(ReflectionProbeRuntime& runtime);
		void destroyReflectionProbe(Entity entity, ReflectionProbeComponent& probe);
		bool selectReflectionProbe(const Vector3& worldPosition, fs_reflection_probe_t& params, TextureRender*& texture, Entity* selectedProbe = nullptr);
		// the sky environment Lit's IBL reads: its colour and rotation (environmentInfo) and the
		// prefiltered map the colour pass binds (emptyCubeBlack without one)
		Vector4 skyEnvironment(TextureRender*& prefiltered);
		// the quality tier of this update and its settings
		void updateShadowTier();
		const ShadowTierSettings& shadowTierSettings() const;
		// the slots of the shadow-casting lights (importance, sticky, packed) and their atlases
		void planShadowSlots(int numLights);
		bool ensureShadowAtlases();
		void releaseShadowAtlases();
		// a light setting its shadow maps depend on changed: its slots are redrawn
		void updateLightShadowRevision(LightComponent& light, const Transform& transform);
		Rect getShadowAtlasSlotRect(int slotIndex) const;
		Rect getShadowPointAtlasSlotRect(int slotIndex) const;
		// the cascades a directional light draws, its directional shadow distance
		unsigned int lightShadowCascades(const LightComponent& light, const CameraComponent& camera) const;
		float lightShadowDistance(const LightComponent& light, const CameraComponent& camera) const;
		// a light's map resolution at mapResolution 0, by type
		unsigned int lightShadowResolution(const LightComponent& light) const;
		// the shadow casters of this update (ShadowCasterRegistry), from the meshes
		void updateShadowCasters();
		void appendCasterChanges();
		// the shadow cameras of every shadowed light for the scene camera
		void updateLightCameras(CameraComponent& camera, Transform& cameraTransform);
		// the camera the cascades are fitted to: the camera, moved by set_shadow_debug for the frame's
		void shadowFitCamera(const CameraComponent& camera, const Transform& cameraTransform, bool frame, Matrix4& view, Vector3& position, Vector3& forward) const;
		// the copy / clear program of the shadow cache
		bool loadShadowRestore();
		void drawShadowRestore(CameraRender& pass, TextureRender* source, const ShadowRect& sourceRect, unsigned int sourceWidth, unsigned int sourceHeight,
		                       const ShadowRect& slotRect, const ShadowRect& scissor, bool clear);
		void renderShadowAtlasDebug();
		void fillShadowStats();
		// shader library: the frame block of a camera pass (fillFrameBlock fills the members
		// library shaders read; engine-internal members appended after them get their own
		// fill functions, called after it), uploaded by applyFrameBlock. updatePassFrame
		// fills passFrame once per camera pass, before its shadow, depth and colour draws.
		void fillFrameBlock(frame_t& frame, const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution);
		void updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform);
		void updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution);
		// the post-processing (phase 4): applies changed settings (LUT bakes, variant bit);
		// the colour pass's post colour parameters (neutral unless active: the main camera of
		// the base layer, post-processing on in the view), for its viewport in a target of
		// targetHeight pixels; the post colour block (tiny block, frame and Lit's) and LUT of a draw
		void applyPostProcessing();
		void bakePostColorLut(TextureRender& texture, int& textureSize, PostColorLut::Stage stage);
		void setPassPostColor(const post_color_t& postColor);
		void fillPassPostColor(bool active, const Rect& viewport, float targetHeight);
		void applyPostColor(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// the vignette's parameters for a viewport of a target targetHeight high (gl_FragCoord)
		void fillVignette(const PostProcessingSettings& settings, const Rect& viewport, float targetHeight,
		                  Vector4& scaleOffset, Vector4& color, float& falloffScale, float& falloffBias) const;
		// the uber pass: loads the variant of features (false while its shader builds), its
		// scene target, and the pass from source (sourceWidth x sourceHeight) into destination
		// (null: the view's target or the swapchain) at viewport
		bool loadUber(uint32_t features);
		// the S13 effects' uber parameters (VHS, halation, film grain and damage) at that time,
		// for an output viewport of that size
		void fillUberFilm(const PostProcessingSettings& settings, double time, float viewportWidth, float viewportHeight);
		void destroyUber();
		bool ensurePostSceneFramebuffer(unsigned int width, unsigned int height);
		// whether the variants the loaded points, lines and sky take with the current colour bit
		// are built (or failed: the load falls back), asking for them
		bool postColorVariantsReady();
		// the bloom chain's programs (BloomProgramKind) loaded; false while one is building
		bool loadBloomProgram(BloomProgramKind kind);
		void destroyBloomPrograms();
		// the chain's targets for a source size and down levels (0..3), made or resized
		bool ensurePostChainTargets(PostChainTargets& targets, unsigned int sourceWidth, unsigned int sourceHeight, int levels, bool upLevels = true);
		// one pass of the chain: source (and level, a mixed upsample's) into target; the source
		// through samplerOf's sampler when given (a NEAREST target, see linearSampler)
		void renderBloomPass(BloomProgramKind kind, const char* label, TextureRender* source, unsigned int sourceWidth,
		                     unsigned int sourceHeight, TextureRender* level, Framebuffer& target, const Vector4& offset,
		                     TextureRender* samplerOf = nullptr);
		// The bloom chain (threshold, 3 levels down and up with the scatter mix) or the blur chain
		// (no threshold, levels from the blur radius) of the uber pass's source: its result at a
		// quarter of the source's size, or null while its programs build. nearestSource: the source
		// is a NEAREST target, which the prefilter's 2x2 averages read through a linear sampler
		TextureRender* renderBloomChain(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight, bool nearestSource);
		TextureRender* renderBlurChain(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight, bool nearestSource);
		// Halation without bloom (S13): its threshold prefilter to 1/4 and one dual-filter level
		// to 1/8 (two passes) into halationTargets; false while its programs build. With bloom on
		// it reads bloom's down[0] and down[1] instead
		bool renderHalationChain(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight, bool nearestSource);
		// a texture whose sampler is linear and clamped (its image is never read), made when a
		// pass first reads a NEAREST target bilinear (the fixed resolution's, NEAREST by default)
		TextureRender* linearSampler();
	public:
		// the blur chain's dual-filter levels for a radius (0..3: 1 + 2 x levels passes)
		static int getBlurLevels(float radius);
	private:
		// the multisampled scene target for a colour target of this size (made or resized)
		bool ensureMsaaFramebuffer(unsigned int width, unsigned int height, int samples);
		// the target the multisampled scene resolves into before its blit to the swapchain
		bool ensureMsaaResolveFramebuffer(unsigned int width, unsigned int height);
		// binds the frame's blurred scene to a draw that reads it (includes/blurred_scene.glsl)
		void applyBlurredScene(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// nearestSource: the source is a NEAREST target (fixed resolution): read through a linear
		// sampler at its texel centres (uber.frag's sampling), so the upscale stays nearest
		void renderUber(TextureRender* source, unsigned int sourceWidth, unsigned int sourceHeight,
		                FramebufferRender* destination, const Rect& viewport, bool nearestSource);
		// the post colour bit of a shader type's variants while the scene's objects load with it
		uint64_t postColorProperty(ShaderType type) const;
		// the size of the target a camera's colour pass renders into
		Vector2 passFrameResolution(const CameraComponent& camera) const;
		// the frame block of the pass (and the post colour, the blurred scene and the globals)
		void applyFrameBlock(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// the global shader values a draw's shader declares (ShaderGlobals: its globals blocks,
		// repacked when a value changed, and its texture globals)
		void applyGlobals(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// its texture globals only (at load, outside a pass)
		void applyGlobalTextures(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// the scene inputs of the pass being drawn, to a colour draw whose shader declares them
		// (the far-plane depth and black where the pass has none)
		void applySceneInputs(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// the reflection environment around a mesh, to a draw whose shader declares it
		// (includes/environment.glsl): its reflection probe, else the sky's prefiltered map
		void applyEnvironment(ObjectRender& render, const ShaderLibraryBindings& bindings, Entity entity, const MeshComponent& mesh, const Transform& transform);
		// resolves the frame block slots and the custom textures of a loaded shader, then
		// binds the textures (false while one is still loading); enabledFeatures (the variant's,
		// ShaderReference::getEnabledFeatures) pick a @texture's default[<DEFINE>]= default
		bool loadShaderLibraryBindings(ShaderLibraryBindings& bindings, ShaderData& shaderData, ObjectRender& render,
		                               std::vector<std::pair<std::string, Texture>>& textures, const ShaderManifest* manifest,
		                               const std::vector<std::string>& enabledFeatures = {});
		// binds the resolved custom textures again (they changed or a render target was redrawn)
		bool bindShaderTextures(const ShaderLibraryBindings& bindings, ObjectRender& render, std::vector<std::pair<std::string, Texture>>& textures);
		static bool shaderTexturesOutdated(const ShaderLibraryBindings& bindings, const std::vector<std::pair<std::string, Texture>>& textures);
		TextureRender* getDefaultShaderTexture(int fallback, bool cube);

		bool checkPBRFrabebufferUpdate(Material& material);
		bool checkPBRTextures(Material& material, bool receiveLights, bool receiveIBL);
		bool loadPBRTextures(Material& material, ShaderData& shaderData, ObjectRender& render, bool receiveLights);
		// A material's binding takes a stand-in for an image it names that has no GPU texture yet
		// (Texture::getStandInCount): taken around the binding, noted on the submesh, which binds
		// again once an image could be made since (TexturePool::getReadyEpoch)
		struct TextureBindMark{
			uint32_t standIns = 0;
			uint32_t readyEpoch = 0;
		};
		static TextureBindMark markTextureBinding();
		static void noteTextureBinding(Submesh& submesh, const TextureBindMark& mark);
		// A material bound to an image whose pixels were replaced, or whose GPU texture was made
		// again in place (Texture::isRenderOutdated), binds again: looked at when
		// TexturePool::getRebindEpoch moved (rebindEpochSeen), never otherwise
		static bool materialRendersOutdated(const Submesh& submesh);
		static bool shaderTexturesRenderOutdated(const std::vector<std::pair<std::string, Texture>>& textures);
		uint32_t rebindEpochSeen = 0;
		// LightComponent::resolvedColor and resolvedIntensity of every light (photometric units,
		// colour temperature, the scene's EV100), where they changed
		void resolveLights();
		// the extension maps the submesh's variant binds (Submesh::pbrMaps), false while one loads
		bool loadPbrExtTextures(Submesh& submesh, ShaderData& shaderData, ObjectRender& render);
		// u_fs_pbrParams of a lit variant: the material's prefix, the emissive factor times
		// Material::emissiveStrength
		static void applyLitMaterialBlock(ObjectRender& render, int slot, const Material& material, unsigned int size);
		void loadShadowTextures(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool receiveShadow);
		void updateShadowBindings();
		bool loadDepthTexture(Material& material, ShaderData& shaderData, ObjectRender& render);
		bool loadGBufferTextures(Material& material, ShaderData& shaderData, ObjectRender& render);
		// Every map of every layer is a slice of one array texture, so the whole terrain
		// costs a single bind slot. Kept here, not on the component, which stays copiable.
		struct TerrainDetailArray{
			TextureRender render;
			// what each slice was built from, in array order
			std::vector<std::string> sliceKeys;
			// how many leading slices hold layer colors (rounded up to whole blend maps)
			int colorSlices = 0;
			// where a layer's normal and packed surface slices landed, -1 when it has none
			std::array<int, MAX_TERRAIN_LAYERS> normalSlice;
			std::array<int, MAX_TERRAIN_LAYERS> surfaceSlice;
			// one sampler serves every slice, so its settings are part of what the cache holds
			TextureFilter minFilter = TextureFilter::LINEAR;
			TextureFilter magFilter = TextureFilter::LINEAR;
			TextureWrap wrapU = TextureWrap::REPEAT;
			TextureWrap wrapV = TextureWrap::REPEAT;
			bool failed = false;
		};
		std::unordered_map<Entity, TerrainDetailArray> terrainDetailArrays;

		TerrainDetailArray* getTerrainDetailArray(Entity entity, TerrainComponent& terrain);
		void destroyTerrainDetailArray(Entity entity);
		void applyTerrainLayersUniform(ObjectRender& render, int slot, Entity entity, TerrainComponent& terrain);
		bool loadTerrainTextures(Entity entity, TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData);
		bool loadTerrainHeightTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData);
		bool updateTerrainRenderTextures(Entity entity, TerrainComponent& terrain, MeshComponent& mesh);
		void updateAllTerrainRenderTextures();
		void updateInstanceBuffers();
		void uploadInstanceViews(RenderView& renderView);
		Rect getScissorRect(RectTransformComponent& layout, ImageComponent& img, Transform& transform, const Rect& passViewport);
		// The UI scissor of a pass entity (a RectTransform): its parent's (unless ignoreScissor)
		// is applied, its own (an Image's rect) is kept for its children. True when the
		// parent's is empty: the entity draws nothing
		bool applyPassScissor(Entity entity, Signature signature, Transform& transform, CameraRender& render, const Rect& colorPassViewport, bool& hasActiveScissor);
		// the pixel rect (pass target pixels, within passViewport) a local rectangle covers
		Rect projectLocalRect(const Matrix4& mvp, float localLeft, float localTop, float localRight, float localBottom, const Rect& passViewport);

		// mesh LOD
		struct LodAttribute{
			Buffer* buffer = nullptr;
			Attribute* attribute = nullptr;
		};
		struct LodAttributes{
			LodAttribute position;
			LodAttribute index;
			LodAttribute normal;
			LodAttribute uv;
			LodAttribute skinWeights;
		};
		void updateMeshLods(Entity entity, MeshComponent& mesh, bool rehash);
		static LodAttribute findSubmeshAttribute(Submesh& submesh, std::map<std::string, Buffer*>& buffers, AttributeType type);
		static bool findLodAttributes(Submesh& submesh, std::map<std::string, Buffer*>& buffers, LodAttributes& attributes);
		static uint64_t attributeHash(uint64_t seed, const LodAttribute& attribute);
		static bool readLodSource(const LodAttributes& attributes, MeshLodSource& source);
		void pollMeshLod(MeshComponent& mesh, Submesh& submesh);

		// shader library phase 2: the extra pass of a submesh (Material::extraPass) and the
		// generated a_smoothNormal (@require smoothnormals)
		using BufferMap = std::map<std::string, Buffer*>;
		using BufferRenderMap = std::map<std::string, BufferRender*>;
		using BufferStrideMap = std::map<std::string, unsigned int>;
		// builds (or, without an extra pass, releases) the submesh's extra pass; false while
		// its shader or textures are not ready
		bool loadSubmeshExtraPass(Entity entity, MeshComponent& mesh, unsigned int submeshIndex, const SubmeshShaderInputs& submeshInputs,
		                          const MeshShaderEnvironment& environment, uint16_t pipelines, BufferMap& buffers,
		                          BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride, InstancedMeshComponent* instmesh);
		void releaseSubmeshExtraPass(Submesh& submesh, bool keepShader);
		// draws it, with the instances, detail level and tilemap ranges of the submesh's draw
		void drawSubmeshExtraPass(Entity entity, MeshComponent& mesh, unsigned int submeshIndex, Transform& transform, PipelineType pipType,
		                          InstancedMeshComponent* instmesh, int instanceView, const MeshDraw& draw, unsigned int instanceCount,
		                          const Matrix4* objectBlock, TilemapComponent* tilemap, uint32_t& builtLightBits);
		// the vertex attributes of the submesh (its buffers, its own attributes, the instance
		// buffer) bound to a render; the index count when indexed, else the mesh's vertex count
		// The batch variants (spec S55 R1): their shaders asked for with the submesh's others
		// (their builds start together; a load does not wait for them), then loaded on render's
		// and depthRender's vertex layouts and linked to them; pending: a shader still building
		// (the mesh reloads when it is ready, needVariantReload)
		void requestSubmeshBatchShaders(MeshComponent& mesh, Submesh& submesh, const SubmeshShaderInputs& inputs,
		                                const SubmeshShaderVariant& variant, const std::string& meshShaderSrc,
		                                const std::string& depthShaderSrc);
		void loadSubmeshBatchVariants(MeshComponent& mesh, Submesh& submesh, uint16_t pipelines, const RenderState& colorState,
		                              uint16_t depthPipelines, const RenderState& depthState, bool allDepthAttributes, int msaaSamples,
		                              BufferMap& buffers, BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride, bool& pending);
		void releaseSubmeshBatchVariants(Submesh& submesh, bool keepShaders);
		// the bounds of the vertices a submesh draws (Submesh::localBounds), from its float
		// positions and indices; shared by the submeshes over the same buffers
		void updateSubmeshBounds(MeshComponent& mesh);
		// the submesh's shadow indices (Submesh::shadowIndices), made or found shared
		std::shared_ptr<ShadowIndexBuffer> shadowIndexBuffer(MeshComponent& mesh, Submesh& submesh);
		// the shadow indices the depth draw being made draws level 0 with (null: its own)
		ShadowIndexBuffer* passShadowIndices = nullptr;
		// The terrain nodes a shadow slot draws (spec S55 R5), as the records of TerrainView's
		// buffers (position, size, range, resolution) and the eye they morph from
		struct ShadowTerrainCut{
			std::vector<float> nodes[2];
			Vector3 eye;
		};
		ShadowTerrainCut shadowTerrainScratch;
		// the cut of the terrain the shadow caster being drawn has (null: the camera's whole cut)
		const ShadowTerrainCut* passShadowTerrain = nullptr;
		// A dynamic terrain caster's cut is the camera's without the nodes outside the slot's
		// frustum; a static caster's (MeshComponent::staticShadowCaster) is its own from the slot's
		// light camera, which then depends on the slot alone: the S10 static layer keeps it while
		// the camera moves
		ShadowTerrainCut& shadowTerrainCut(Entity entity, TerrainComponent& terrain, Transform& transform, const MeshComponent& mesh,
		                                   const LightCamera& shadowCamera);
		// CDLOD selection (terrainNodeLODSelect) from eye inside the frustum, into cut, leaving the
		// nodes' own state (the camera's selection) as it is
		bool selectShadowTerrainNode(TerrainComponent& terrain, const Transform& transform, const Vector3& eye, float cameraFar,
		                             const Plane frustumPlanes[6], TerrainNode& node, int lodLevel, ShadowTerrainCut& cut);
		// the detail levels of a point light's cube face (as shadowLodView for its atlas)
		LodView pointFaceLodView(const LightComponent& light, int lightIndex, int face) const;
		bool usesShadowIndices(const MeshComponent& mesh, const Submesh& submesh, const InstancedMeshComponent* instmesh, const TerrainComponent* terrain) const;
		// what the next draw of this object may join (ObjectRender::setNextDrawBatch), when the
		// draw can batch: the plain pipeline of a batch-variant submesh
		void offerDrawBatch(ObjectRender& render, ObjectRender& variant, int objectSlot, bool mirrored);
		// The opaque queue of a pass with GPU Instancing (spec S55 R2, R4), in the order it has: the
		// entries culled at once (on jobs) against the pass's frustum, mesh then submesh, those of
		// a mesh that can batch split one per submesh, the draws of one batch key brought together
		// within their priority (VisibleList::groupBatches); the culled ones counted. sortByState
		// (a colour pass's opaque queue): the visible entries are then sorted by
		// OpaqueRenderComparison with their band and state, before the batches are grouped
		void buildVisibleQueue(std::vector<OpaqueRenderData>& queue, const VisibleFrustum& frustum, PipelineType pipType, bool depthPipelines,
		                       bool sortByState = false);
		unsigned int bindSubmeshAttributes(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh,
		                                   BufferMap& buffers, BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride,
		                                   InstancedMeshComponent* instmesh);
		// a_smoothNormal for a render whose shader declares it: the mesh's generated buffer
		// of the submesh's positions, made when missing; false when the mesh cannot have one
		bool bindSmoothNormals(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh, BufferMap& buffers);
		// zeros for the morph inputs a render's shader declares (a fixed count, see
		// includes/morphtarget.glsl) and the submesh has no target for; false when the
		// buffer of zeros cannot be made
		bool bindUnfedMorphInputs(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh, BufferMap& buffers);
		void releaseSmoothNormals(MeshComponent& mesh, bool keepData);
		// forgets the generated buffers no submesh of the finished load uses
		void trimSmoothNormals(MeshComponent& mesh);
		static bool releaseSubmeshLod(Submesh& submesh);
		void releaseMeshLods(MeshComponent& mesh);
		static bool isMeshLodPending(Submesh& submesh);
		void updateMeshLodErrors(MeshComponent& mesh);
		LodView computeLodView(const CameraComponent& camera, const Transform& cameraTransform, float viewHeight = 0.0f, bool shadowBias = false) const;
		void updateMainLodView(CameraComponent& camera, Transform& cameraTransform);
		// a camera view's next frame: its frame number and fade clock (planMeshLod)
		void advanceLodView(LodView& view, double& clock);
		unsigned int selectMeshLod(const MeshComponent& mesh, const LodView* view, float distance, float worldScale) const;
		// the projected height (pixels) of world bounds in a view, at that distance
		static float lodScreenHeight(const LodView& view, const AABB& bounds, float distance);
		// the mesh's plan in a view that fades (once per view frame, every pass of the frame
		// draws it): its level, a time or distance cross-fade, culled for its size
		const LodDraw& planMeshLod(Entity entity, MeshComponent& mesh, const LodView& view, float distance, float worldScale);
		// the fade state of the mesh in the view being drawn
		MeshLodFade& lodFadeState(Entity entity, MeshComponent& mesh);
		// whether the mesh may cross-fade at all (the scene's and its settings, a built-in
		// shader), and whether its fade pipelines are ready now
		bool canLodFade(const MeshComponent& mesh) const;
		// a submesh's fade variant (MESH 'Lfd') asked for and checked against its base
		// variant's bindings (spec S54 §6); false while not ready, or never for this submesh
		bool prepareSubmeshLodFade(Submesh& submesh, PipelineType pipType, bool mirrored);
	public:
		// a LOD fade variant's reflection binds what its base variant's does (checked at load)
		static bool sameLodFadeBindings(const ShaderData& base, const ShaderData& fade);
		// a loaded submesh's positions (mesh space) and the triangles of one of its detail levels
		// over them (the level's indices, or the submesh's own for 0 or a level it lacks); false
		// when the submesh has no CPU geometry the levels read (the editor's wireframe, captures)
		static bool readSubmeshLevel(MeshComponent& mesh, unsigned int submeshIndex, unsigned int level,
		                             std::vector<Vector3>& positions, std::vector<uint32_t>& indices);
		// what the level builder reads of a loaded submesh (its positions, indices and the
		// attributes that guide it, decoded to floats); false when it has none to build from
		static bool readSubmeshLodSource(MeshComponent& mesh, unsigned int submeshIndex, MeshLodSource& source);
	private:
		void releaseSubmeshLodFade(Submesh& submesh);
		LodView shadowLodView(const LightComponent& light, int lightIndex, int cameraIndex, int slot) const;
		// projective atlas slots the shadow pass of the camera being drawn fills, into shadowSlots
		const std::vector<ShadowSlot>& collectShadowSlots();

		// per-camera shadows
		CameraShadows& getCameraShadows(Entity entity);
		// fits target's cascades to camera, fills its lighting and shadow uniforms and picks
		// the atlases it samples: maps holding the same depths drawn after this update(),
		// else its own (drawn by renderCameraShadows)
		void prepareCameraShadows(CameraShadows& target, const CameraComponent& camera, const Transform& cameraTransform, int terrainView, bool ownInstances);
		void renderCameraShadows(CameraShadows& target);
		// culled casters of target's projective slots, from the frame's instance lists
		void buildCameraShadowInstances(CameraShadows& target);
		void ensureCasterLayers();
		void computeShadowKeys(const CameraShadows& target, ShadowAtlasKey& projective, ShadowAtlasKey& point);
		FramebufferRender* findShadowSource(CameraShadows& target, const ShadowAtlasKey& wanted, bool projective);
		// the raw-depth view of an atlas findShadowSource gave
		TextureRender* shadowSourceRaw(FramebufferRender* atlas);
		bool ensureOwnShadowAtlas(CameraShadows& target, bool projective);
		// frees the camera shadows unused for a while
		void releaseUnusedCameraShadows();
		// the CDLOD cut camera draws (and its shadow passes) into terrain view terrainView
		void selectCameraTerrain(CameraComponent& camera, Transform& cameraTransform, int terrainView);

		// instance views
		void buildInstanceViews(CameraComponent& mainCamera, Transform& mainCameraTransform);
		// limitDistance drops instances past the batch's cull distance from eye
		// vat: the instances' Vertex Animation state (parallel to instances), null unless the
		// batch draws with a VAT variant
		void buildInstanceView(InstanceView& view, InstanceViewRange& range, const InstanceRenderData* instances, const InstanceVatRenderData* vat, const InstanceBounds* instanceBounds, unsigned int count, const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const LodView& lodView, const Vector3& eye, const Vector3& origin, const Vector3& direction, InstanceOrder order, bool limitDistance = true);
		void uploadInstanceViews(InstanceView* views, size_t count);
		bool selectMeshDraw(Entity entity, MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh, bool ownLod, int instanceView, const LodView* lodView, MeshDraw& draw);
		void bindInstances(ObjectRender& render, InstancedMeshComponent& instmesh, int instanceView, unsigned int first, unsigned int vatFirst);
		BufferRender* instanceVatBuffer(int instanceView);
		// the Vertex Animator of every mesh: its variant, payload, u_vs_vat and bounds (once a frame)
		void updateVertexAnimations();
		// fade: the colour pass's fading draws (the plan's two levels, or an instance view's fade
		// buckets), else the plan's dominant level and the instance view's plain and dominant copies
		// beforeLevel (the editor's LOD Level Draw Mode) runs before each level's draw
		void drawSubmeshGeometry(ObjectRender& render, Entity entity, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView, unsigned int lod, unsigned int instanceCount,
		                         const std::function<void(unsigned int)>* beforeLevel = nullptr);
		void drawSubmeshFade(ObjectRender& render, Entity entity, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView,
		                     const MeshDraw& draw, int slotVSParams, const Matrix4* objectBlock, const std::function<void(unsigned int)>* beforeLevel = nullptr);

		// terrain
		bool terrainNodeLODSelect(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainNode& terrainNode, int lodLevel, TerrainView& view);
		AABB getTerrainNodeAABB(const Transform& transform, TerrainNode& terrainNode);
		bool isTerrainNodeInSphere(Vector3 position, float radius, const AABB& box);

		// tilemap
		bool selectTilemapChunks(TilemapComponent& tilemap, unsigned int submeshIndex, const float cameraFar, const Plane frustumPlanes[6]);

		float lerp(float a, float b, float fraction);

		// draw() in parts: the work done once per frame, then the cameras
		void drawFramePasses();
		void renderShadowMaps(bool projective, bool point);
		void render2DShadows();
		void drawCameras(bool includeMain);
		void renderCameraPass(const CameraPass& pass);

		// what the pass being drawn reads: the Engine's framebuffer, view rect and main
		// scene in draw(), the view's own in a secondary view
		Framebuffer* targetFramebuffer() const;
		Rect targetViewRect() const;
		bool isBaseLayer() const;
		// shadow cameras of the pass being drawn, or of shadows
		const LightCamera& lightCamera(int lightIndex, const LightComponent& light, int cameraIndex) const;
		unsigned int lightCascades(int lightIndex, const LightComponent& light) const;
		const LightCamera& lightCamera(const CameraShadows& shadows, int lightIndex, const LightComponent& light, int cameraIndex) const;
		unsigned int lightCascades(const CameraShadows& shadows, int lightIndex, const LightComponent& light) const;
		const InstanceViewRange& instanceRange(Entity entity, const InstancedMeshComponent& instmesh, int instanceView) const;
		BufferRender* instanceBuffer(int instanceView);
		// the view whose buffer instanceBuffer gives
		const InstanceView& instanceViewOf(int instanceView);
		Vector3 instanceFadeEye(Entity entity, const InstancedMeshComponent& instmesh, int instanceView) const;
		bool isBatchVisible(Entity entity, const MeshComponent& mesh, const Transform& transform, const InstancedMeshComponent* instmesh, int instanceView, const float cameraFar, const Plane frustumPlanes[6]);
		TerrainView& terrainViewData(Entity entity, TerrainComponent& terrain, int terrainView);
		// the terrain cut of the view camera, which its depth, G-buffer and shadow passes draw
		int mainTerrainView() const;
		// model, normal and mvp matrices of an object, in one block (scratch when copied)
		const Matrix4* objectMatrices(Transform& transform, Matrix4 (&scratch)[3]);
		const Matrix4& objectMVP(Transform& transform) const;
		const Matrix4& objectModel(Transform& transform) const;
	public:
		// Whether a draw of the mesh with this model matrix reverses the face winding of its
		// pipelines (ObjectRender::beginDraw mirrored): a negative determinant turns its triangles
		// over on screen, so its front faces would cull and shade (gl_FrontFacing) as backs,
		// unless the mesh's winding order already compensates that mirror
		// (MeshComponent::mirroredWinding). Per draw, so a mirrored instance of an instanced mesh
		// is not covered (its instance matrix is the GPU's)
		static bool isMirroredDraw(const MeshComponent& mesh, const Matrix4& modelMatrix);
	private:
		float objectDistance(Transform& transform) const;
		PassScissor passScissor(Transform& transform, RectTransformComponent& layout);
		const Matrix4& passSkyViewProjection(const SkyboxComponent& sky) const;
		FramebufferRender& shadowAtlasTarget();
		FramebufferRender& shadowPointAtlasTarget();
		// binds the shadow maps the pass being drawn samples to a lit submesh when they are
		// not the frame's (restore puts the frame's back); false when they are the frame's
		bool bindViewShadowMaps(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool restore);
		// 1/viewport size, SSAO debug and flip flags of fs_lighting for camera in the view being drawn
		Vector4 lightingViewportInfo(const CameraComponent& camera) const;

#ifdef JASPERENGINE_EDITOR
		// index of transform in the secondary pass's objects, false outside them
		bool passObjectIndex(const Transform& transform, size_t& index) const;
		bool isObjectOverridden(const Transform& transform) const;
		MirrorView* passMirror(Entity entity);
		bool bindViewPointsOrder(Entity entity, PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& cameraTransform);

		RenderView& getSecondaryView(uint32_t viewId);
		void destroyRenderView(RenderView& view);
		void buildPassObjects(PassObjects& objects, CameraComponent& camera, Transform& cameraTransform);
		void selectPassTerrain(PassObjects& objects, CameraComponent& camera, Transform& cameraTransform);
		void buildViewInstances(RenderView& view, CameraComponent& camera, Transform& cameraTransform);
		void buildInstanceList(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Transform& transform, const CameraComponent& camera, const Vector3& fadeEyeLocal, ViewInstanceSource& source);
		void renderViewMirrors(RenderView& view, CameraComponent& camera);
		void forgetEntityInViews(Entity entity, ComponentId componentId);
#endif

	//-----Dynamic Batching (RenderSystemBatch.cpp, docs/rendering/batching.md)-----
	public:
		// the counters and breaks of the last frame of a view (SokolSystem view tag)
		struct BatchFrame{
			BatchStats stats;
			std::vector<BatchBreakEvent> breaks;
		};
		const BatchFrame* getBatchFrame(uint32_t viewTag) const;
		// the entity has a batch record (its baked vertices are kept between frames: tests, tools)
		bool hasBatchRecord(Entity entity) const;
		// the batch form of the entity's draws or why it has none
		BatchBreak getBatchEligibility(Entity entity);
		// Pixel-art S52 hook: per-vertex attributes beyond the ones the bake knows (position,
		// normal, tangent, UV sets, colour) are copied as they are; a type set here is baked like
		// a position (an object-space point: transformed to the batch's space) or a direction (a
		// normal-like vector: transformed by the normal matrix, normalized). A per-object value
		// that must not break batches lives in such an attribute, the same at every vertex.
		enum class BatchAttributeRole : uint8_t { VALUE, POINT, DIRECTION };
		static void setBatchAttributeRole(AttributeType type, BatchAttributeRole role);
		static BatchAttributeRole getBatchAttributeRole(AttributeType type);

	private:
		// what a batch draws with in place of its first object's own (passBatchGeometry)
		struct BatchGeometry{
			BufferRender* vertexBuffer = nullptr;
			size_t vertexOffset = 0;
			BufferRender* indexBuffer = nullptr;
			size_t indexOffset = 0;
			uint32_t indexCount = 0;
			// model (the cell's translation), normal (identity: normals are baked) and mvp
			Matrix4 objectBlock[3];
			bool mirrored = false;
			// the colour factor is in the vertex colours: the draw's is white
			bool neutralColor = false;
		};
		// the baked vertices of an object, kept while its inputs do not change (settled)
		struct BatchRecord{
			Entity entity = NULL_ENTITY;
			bool ui = false;
			unsigned int stride = 0;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			int positionOffset = -1;
			int normalOffset = -1;
			int tangentOffset = -1;
			int uv1Offset = -1;
			int uv2Offset = -1;
			int colorOffset = -1;
			// other attributes baked by role (setBatchAttributeRole)
			std::vector<std::pair<int, BatchAttributeRole>> extraAttributes;
			// object space, the texture rect and colour applied
			std::vector<unsigned char> local;
			// placed: positions relative to the origin cell, normals and tangents in world space
			std::vector<unsigned char> placed;
			std::vector<uint16_t> indices;
			// what the bake read (a difference bakes again)
			const unsigned char* source = nullptr;
			size_t sourceSize = 0;
			// the vertex and index buffers' write counts (Buffer::noteWrite)
			uint64_t sourceWrites = 0;
			uint64_t indexWrites = 0;
			Rect rect;
			bool hasRect = false;
			Vector4 color;
			bool bakesColor = false;
			bool bakesNormals = false;
			bool localValid = false;
			// what the placement read
			Matrix4 placedWith;
			bool placedValid = false;
			int32_t cell[3] = {0, 0, 0};
			float lo[3] = {0.0f, 0.0f, 0.0f};
			float hi[3] = {0.0f, 0.0f, 0.0f};
			uint32_t cullSerial = 0;
			bool inside = false;
			// RenderSystem::update left its Transform's MVP as it was (ensureBatchMVP)
			bool mvpStale = false;
		};
		struct BatchPassContext{
			BatchPass pass = BatchPass::COLOR;
			CameraComponent* camera = nullptr;
			Transform* cameraTransform = nullptr;
			PipelineType pipType = PIP_DEFAULT;
			// the camera pre-pass: the colour pass's pipeline type (what decides the batch form)
			PipelineType colorPipType = PIP_DEFAULT;
			// what the pass's MVPs are made of (Y-flipped like updateMVP when the target is)
			Matrix4 viewProjection;
			float farClip = 0.0f;
			const Plane* frustumPlanes = nullptr;
			bool forSSAO = false;
			bool culls = true;
			int terrainView = 0;
			int instanceView = -1;
			const LodView* lodView = nullptr;
			bool inCanvasLayer = false;
			// (cell, mvp) of the last cell asked for in the pass
			int32_t mvpCell[3] = {INT32_MIN, 0, 0};
			Matrix4 cellMVP;
		};
		struct BatchChunk{
			BufferRender buffer;
			size_t capacity = 0;
			size_t used = 0;
		};

		static constexpr float BATCH_CELL_SIZE = 4096.0f;
		static constexpr uint32_t BATCH_MAX_MESH_VERTICES = 64;
		static constexpr uint32_t BATCH_MAX_UI_VERTICES = 256;
		static constexpr uint32_t BATCH_QUADS = 16384;
		static constexpr uint32_t BATCH_SHRINK_FRAMES = 300;

		DrawBatcher batcher;
		BatchPassContext batchPass;
		bool batchPassActive = false;
		const BatchGeometry* passBatchGeometry = nullptr;
		// the camera passes' planes (colour, pre-pass: their verdicts are kept between frames) and
		// the other depth passes' (shadow slots, scene depth: tested each time)
		FrustumCull batchCull;
		FrustumCull batchDepthCull;
		std::vector<BatchRecord> batchRecords;
		// entity -> record index + 1 (0: none), in pages of BATCH_PAGE entities freed when empty:
		// entity ids only grow
		static constexpr Entity BATCH_PAGE = 1024;
		struct BatchPage{
			uint32_t index[BATCH_PAGE];
			uint32_t count = 0;
		};
		std::vector<std::unique_ptr<BatchPage>> batchPages;
		std::map<uint32_t, BatchFrame> batchFrames;
		std::vector<BatchChunk> batchVertexChunks;
		std::vector<BatchChunk> batchIndexChunks;
		size_t batchVertexDemand = 0;
		size_t batchIndexDemand = 0;
		size_t batchVertexCursor = 0;
		size_t batchIndexCursor = 0;
		// frames the chunks were mostly unused, each kind its own
		uint32_t batchSmallVertexFrames = 0;
		uint32_t batchSmallIndexFrames = 0;
		uint64_t batchChunkCommit = 0;
		BufferRender batchQuadIndices;
		bool batchLazyMVP = false;
		// objects whose record went while their MVP was stale: made by their next draw alone
		// (ensureBatchMVP), as a record's is
		std::unordered_set<Entity> batchStaleMVPs;
		bool batchingSeen = true;

		BatchRecord* findBatchRecord(Entity entity);
		BatchRecord& addBatchRecord(Entity entity);
		void dropBatchRecord(Entity entity);
		void dropBatchRecords();

		// NONE when the mesh draws in batch form with pipType (its depth program's with depthPass),
		// else why it is drawn alone
		BatchBreak meshBatchForm(Entity entity, Signature signature, MeshComponent& mesh, Transform& transform, PipelineType pipType, bool depthPass);
		BatchBreak uiBatchForm(UIComponent& ui, TextureRender* textureOverride);
		bool bakeMeshRecord(BatchRecord& record, MeshComponent& mesh);
		bool bakeUIRecord(BatchRecord& record, UIComponent& ui, const Vector4& drawColor, bool bakesColor);
		void placeBatchRecord(BatchRecord& record, const Matrix4& model, const Matrix4* normalMatrix);
		const Matrix4& batchCellMVP(const int32_t cell[3]);
		bool cullBatchRecord(BatchRecord& record);
		void appendBatchItem(BatchRun& run, const BatchRecord& record);
		void beginBatchPass(BatchPass pass, BatchMode mode, CameraComponent* camera, Transform* cameraTransform, PipelineType pipType,
		                    const Matrix4& viewProjection, float farClip, const Plane* frustumPlanes);
		void endBatchPass();
		// draws what the batcher holds now (as the pass's end would), inside a batch pass
		void flushHeldBatchDraws();
		bool batchingActive() const { return batchPassActive; }
		// colour pass: drawn alone (drawMesh) or held for a batch
		bool submitMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType,
		                InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView, int instanceView, const LodView* lodView);
		bool submitUI(Entity entity, UIComponent& ui, Transform& transform, PipelineType pipType, TextureRender* textureOverride);
		// depth passes (pre-pass, scene depth, shadow casters): params are what drawMeshDepth takes alone
		bool submitMeshDepth(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const vs_depth_t& params,
		                     InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO, PipelineType pipelineType,
		                     int instanceView, const LodView* lodView);
		bool fillMeshKey(BatchKey& key, Entity entity, MeshComponent& mesh, const BatchRecord& record, bool mirrored);
		void drawBatchRun(BatchRun& run);
		static void flushBatches(void* owner);
		static void drawBatchRunCallback(void* owner, BatchRun& run);
		bool uploadBatch(const void* data, size_t size, bool index, BufferRender*& buffer, size_t& offset);
		void prepareBatchChunks();
		void destroyBatchChunks();
		void drawBatchGeometry(ObjectRender& render, Buffer& vertices, const BatchGeometry& batch);
		// the MVP update() left as it was for a batched mesh, made before a draw of it alone reads it
		void ensureBatchMVP(Entity entity, Transform& transform);
		void beginBatchFrame();
		void endBatchFrame();

		// the texture (re)binding drawMesh and the batcher do before a draw
		void refreshSubmeshTextures(Submesh& submesh, bool receiveLights, bool pbrFramebufferOutdated);
		void refreshDepthTextures(Submesh& submesh);
		void refreshUITexture(UIComponent& ui);
		void uploadUIBuffers(UIComponent& ui);
		// the draw colour of a UI element: its colour (a Text Effect's alpha only), faded by its
		// Canvas Groups
		Vector4 uiElementColor(const UIComponent& ui, bool inCanvasLayer) const;
		// the material's u_fs_pbrParams prefix (lit 12 floats with the emission intensity, unlit
		// 4 or 8), its colour factor white when neutralColor
		static void applyMaterialPrefix(ObjectRender& render, int slot, const Material& material, unsigned int size, bool lit, bool neutralColor);

	protected:

		void updateMeshBuffers(Entity entity, MeshComponent& mesh);
		// an immutable buffer shared by content (MeshBufferPool) whose bytes changed takes the
		// shared buffer of its new bytes, or after a second change within
		// IMMUTABLE_CHANGE_WINDOW updates a dynamic buffer of its own; every render of the mesh
		// is retargeted to it
		void refreshImmutableBuffer(Entity entity, MeshComponent& mesh, Buffer& buffer);
		void retargetMeshBuffer(MeshComponent& mesh, uint32_t oldId, BufferRender* replacement, bool index);
		void updateTerrainNodesBuffer(TerrainView& view);
		// instanceView -1 draws every instance from the component buffer; lodView null is full detail
		bool drawMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView = 0, int instanceView = -1, const LodView* lodView = nullptr);
		bool drawMeshDepth(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_depth_t vsDepthParams, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO = false, PipelineType pipelineType = PIP_DEPTH, int instanceView = -1, const LodView* lodView = nullptr);
		void destroyMesh(Entity entity, MeshComponent& mesh, bool clearAssets = false);

		// SSAO
		void loadSSAO();
		void destroySSAO();
		bool ensureSSAOFramebuffers(unsigned int width, unsigned int height);
		// sharedDepth != null reuses an existing packed-depth texture (the SSR G-buffer),
		// skipping the SSAO depth pre-pass; null runs the pre-pass
		// fixedResolution: the color pass it feeds renders at the scene's fixed size
		// returns the packed depth it read (null when it did not run)
		// perCameraTransform: as renderOpaqueDepth's
		TextureRender* renderSSAO(CameraComponent& camera, bool fixedResolution, TextureRender* sharedDepth = nullptr,
		                          Transform* perCameraTransform = nullptr);

		// SSR
		void loadSSR();
		void destroySSR();
		bool ensureSSRFramebuffers(unsigned int width, unsigned int height);
		void renderDepthPrePass(CameraComponent& camera, Transform* perCameraTransform); // shared depth for SSAO/SSR
		// the opaque meshes' packed depth (PIP_DEPTH) of a camera into target: the SSAO,
		// post-process and scene depth pre-passes. perCameraTransform: the camera's, when the
		// pass makes its own per-object matrices (renderCameraPass's perCameraMVP), else null
		void renderOpaqueDepth(CameraComponent& camera, Framebuffer& target, unsigned int width, unsigned int height,
		                       int instanceView, const LodView* lodView, Transform* perCameraTransform);

		// scene inputs (phase 3)
		// records what a loaded mesh or points component reads of the scene (0: nothing)
		void setSceneInputUser(Entity entity, bool points, uint32_t inputs);
		// waterMeshes and planarReflectionMeshes for a mesh as its load resolved it
		void setWaterMesh(Entity entity, bool water, bool planarReflection);
		// the pass's waters a transparent queue layers around (WaterLayering.h): the visible
		// transparent meshes of waterMeshes in the queue, with their levels and wave bands
		void gatherPassWaters(const std::vector<TransparentRenderData>& queue, std::vector<WaterLayer>& waters) const;
		// The water reflection passes of a view's camera (spec water-reflections 6.4): the
		// heights of the visible meshes reading a planar reflection, nearest first up to the
		// scene's cap, each drawn mirrored about its plane before the camera's own pass;
		// terrainView: the terrain cut the passes draw (the primary view's next free slot)
		void renderWaterReflections(RenderView& view, CameraComponent& viewer, Transform& viewerTransform, int terrainView);
		// whether the camera sees a mesh reading a planar reflection from above it, with the
		// scene's resolution on (its water reflection passes draw this frame)
		bool planarReflectionVisible(Entity cameraEntity);
		// the view's reflection passes unused for a while freed (all of them: everything)
		void releaseWaterReflections(RenderView& view, bool everything);
		// what a reflection camera takes of the camera it mirrors (copied: making a camera entity
		// may move the camera arrays)
		struct ReflectionViewer{
			Matrix4 viewMatrix;
			Vector3 eye;
			CameraType type = CameraType::CAMERA_PERSPECTIVE;
			float yfov = 0.0f, aspect = 1.0f, nearClip = 0.1f, farClip = 1000.0f;
			float leftClip = 0.0f, rightClip = 0.0f, bottomClip = 0.0f, topClip = 0.0f;
			uint32_t cullingMask = LAYER_MASK_EVERYTHING;

			static ReflectionViewer of(const CameraComponent& camera);
		};
		// a reflection camera: the viewer mirrored about a plane (its normal, a point of it), its
		// near plane moved onto the plane raised by clipOffset along the normal, so nothing
		// behind it shows (mirrors and water reflections)
		void driveReflectionCamera(CameraComponent& reflectionCamera, Transform& reflectionTransform, const ReflectionViewer& viewer,
		                           const Vector3& normal, const Vector3& point, float clipOffset);
		// target takes source's lighting and shadow maps (its own camera's viewport), without
		// shadows at strength 0
		void shareCameraShadows(CameraShadows& target, const CameraShadows& source, const CameraComponent& camera, bool shadows);
		// the camera of the camera pass being drawn (renderCameraPass)
		const CameraComponent* passCamera = nullptr;
		// u_fs_planarReflection and its texture for a draw of a mesh reading one
		void applyPlanarReflection(ObjectRender& render, const ShaderLibraryBindings& bindings, Entity entity);
		// the planar reflection a draw of the mesh reads in the pass being drawn (none: params.y 0, black)
		void getPlanarReflection(Entity entity, fs_planar_reflection_t& block, TextureRender*& texture);
		// the eye of the camera pass being drawn (its camera's Transform)
		Vector3 passEye;
		// the Water Excluders' and Wakes' members of a draw's custom block and their textures: the
		// excluders over the mesh in the pass camera's view, nearest first, and the wake map with the
		// mesh's body tag
		void applyWaterInteraction(ObjectRender& render, const ShaderLibraryBindings& bindings, Entity entity, const MeshComponent& mesh,
		                           const Material& material, CustomUniformBlock& customParams);
		// the world box a transparent draw covers (null when it has none to tell)
		AABB transparentBounds(const TransparentRenderData& renderData) const;
		// whether a draw of the transparent queue samples the scene colour (and is drawn)
		bool readsSceneColor(const TransparentRenderData& renderData, CameraComponent& camera, int instanceView);
		// the box's extent in normalized device coordinates (xmin, ymin, xmax, ymax): the whole
		// screen when it is unknown or reaches behind the eye
		static Vector4 screenBoundsOf(const AABB& bounds, const Matrix4& viewProjection);
		// the scene colour copies a camera pass may take (the first and the re-copies for readers
		// drawn over other readers)
		static constexpr unsigned int MAX_SCENE_COLOR_COPIES = 4;
		// registers what the meshes and points about to load will read, before update() decides
		// from the readers what every mesh loads with (depth programs, offscreen pipelines)
		void predictSceneInputUsers();
		// whether the scene depth keeps depth programs on every mesh, and the exported scene
		// camera its offscreen pipelines for a colour reader (with their hold after the last)
		void updateSceneInputPrograms();
		void setGBufferDepthMismatch(Entity entity, bool mismatch);
		// whether a visible opaque mesh of this camera is drawn differently by the G-buffer
		bool hasVisibleGBufferDepthMismatch(CameraComponent& camera, int instanceView);
		// whether the entity has a draw requiring the scene colour this pass draws now
		bool drawsSceneColorReader(Entity entity, Signature signature, Transform& transform, CameraComponent& camera, int instanceView);
		// the scene inputs the visible draws of a camera pass require
		uint32_t findPassSceneInputs(CameraComponent& camera, int instanceView);
		// a depth or colour target of the view being drawn, made or reused
		SceneInputTarget* ensureSceneInputTarget(bool color, unsigned int width, unsigned int height);
		// frees the targets of a view unused for a while (all: every one)
		void releaseSceneInputTargets(RenderView& view, bool all);
		bool ensureSceneColorRedirect(unsigned int width, unsigned int height);
		void loadSceneColorCopy();
		// copies the viewport of a colour target (targetHeight: its height) into a copy target
		// at the scene's Scene Color Scale; the copy, or null
		TextureRender* copySceneColor(TextureRender* source, const Rect& viewport, unsigned int targetWidth, unsigned int targetHeight);
		// passSceneBlock for a camera pass: its viewport in its target, whether its rows run
		// flipped (isRenderingFlipped) and the target height (rows of a non-GL viewport)
		void fillSceneInputs(const CameraComponent& camera, const Rect& viewport, unsigned int targetHeight, bool flipped, unsigned int copyWidth, unsigned int copyHeight);
		// G-buffer geometry pass for SSR: MRT packed depth + view-space normal/roughness/metallic
		bool ensureGBufferFramebuffer(unsigned int width, unsigned int height);
		void renderGBufferPass(CameraComponent& camera);
		// the SSR G-buffer: a mesh's draw record, when it draws (visible, loaded, a submesh to draw)
		bool collectGBufferDraw(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, GBufferDraw& record);
		void drawMeshGBuffer(GBufferDraw& record, const float cameraFar, const Plane frustumPlanes[6], vs_gbuffer_t vsGBufferParams);
		// destination == nullptr renders the composite to the swapchain (backbuffer)
		void renderSSR(CameraComponent& camera, FramebufferRender* destination);

		// fixed game resolution
		void loadBlit();
		void destroyBlit();
		bool ensureFixedResFramebuffer(unsigned int width, unsigned int height, TextureFilter filter);
		// draws source over the real destination (Engine framebuffer in the editor,
		// swapchain in exported builds)
		void renderBlit(TextureRender* source, Rect viewport);
		// upscales fixedResFramebuffer to the view rect
		void renderFixedResolutionBlit();

		// user post-process chain
		void loadPostProcess();
		void updatePostProcessUniforms();
		void destroyPostProcess();
		bool ensurePostProcessFramebuffers(unsigned int width, unsigned int height);
		// runs the chain from ping-pong buffer 0 into destination (swapchain when null)
		// the chain into the destination (null: the swapchain, or with keepResult its last
		// output is returned, the uber pass's source)
		TextureRender* renderPostProcess(FramebufferRender* destination, bool keepResult = false);
		// a full-size or scaled target of the user chain, not the one it reads (input) nor the
		// original scene while kept
		Framebuffer* postProcessOutput(unsigned int width, unsigned int height, TextureRender* input);
		// a copy of a chain target into a destination framebuffer (the last pass below full size)
		void renderPostProcessCopy(TextureRender* source, FramebufferRender* destination);

		// submesh 0 shows a transparent FluidSimulation output (straight alpha) under the legacy
		// blending: drawn in the transparent queue, as with a transparent texture
		bool lendsTransparentFluid(Entity entity, const Submesh& submesh) const;
		// textureOverride: shown instead of the UI's texture (a FluidSimulation's MATERIAL output)
		// inCanvasLayer: drawn into a FLATTEN Canvas Group layer (its alpha relative to the layer)
		bool drawUI(UIComponent& ui, Transform& transform, PipelineType pipType, TextureRender* textureOverride = nullptr, bool inCanvasLayer = false);
		void destroyUI(Entity entity, UIComponent& ui);

		bool drawPoints(Entity entity, PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);

		// ---- particle systems (RenderSystemParticles.cpp): their GPU objects, their per-view
		// packing into the frame's streams (sg_append_buffer) and their instanced draws
		struct ParticleStream{
			// the pages appended to this frame; the older ones go two frames after they filled
			std::vector<std::unique_ptr<BufferRender>> pages;
			std::vector<std::pair<std::unique_ptr<BufferRender>, uint64_t>> retired;
			size_t capacity = 0;
			size_t used = 0;
			uint64_t frame = 0;
			BufferType type = BufferType::VERTEX_BUFFER;
		};
		ParticleStream particleInstanceStream;
		ParticleStream particleRotationStream;
		ParticleStream particleCustomStream;
		ParticleStream particleTrailStream;
		// the quad's corners and triangles, and what each stream is bound to at load (a draw
		// swaps it for this frame's page at its offset)
		BufferRender particleCornerBuffer;
		BufferRender particleQuadIndexBuffer;
		BufferRender particleInstancePlaceholder;
		BufferRender particleRotationPlaceholder;
		BufferRender particleCustomPlaceholder;
		BufferRender particleTrailPlaceholder;
		bool particleStaticBuffers = false;
		bool ensureParticleBuffers();
		void destroyParticleBuffers();
		bool appendParticleStream(ParticleStream& stream, const void* data, size_t bytes, BufferRender*& buffer, size_t& offset);
		bool loadParticles(Entity entity, ParticleSystemComponent& particles, uint16_t pipelines);
		void destroyParticles(ParticleSystemComponent& particles);
		// load, reload and the scene inputs of every particle system (update)
		void updateParticleRenders(uint16_t pipelines);
		// what the draw tests besides the component's checkbox: the GameObject active, the
		// layer, the editor's own views (a disabled MeshComponent it takes its mesh from does not hide it)
		bool isParticleRenderActive(Entity entity, uint32_t cullingMask) const;
		bool particleBoundsVisible(const particles::ParticleSystemState& state, CameraComponent& camera);
		void setParticleSceneInputUser(Entity entity, uint32_t inputs);
		uint64_t particleRenderKey(const particles::ParticleSystemState& state) const;
		void drawParticles(Entity entity, ParticleSystemComponent& particles, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		float getPointsViewportHeight(const CameraComponent& camera) const;
		float computePointsScale(const CameraComponent& camera, float viewportHeight) const;
		void destroyPoints(Entity entity, PointsComponent& points);

		bool drawLines(Entity entity, LineRendererComponent& lines, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		void destroyLines(Entity entity, LineRendererComponent& lines);
		static void changeTrailLoaded(void* data);
		static void changeTrailDestroy(void* data);
		// a thick line's or a trail's draw: the ribbon params for this camera, the blocks, textures
		template<typename T>
		bool drawRibbon(T& component, const Matrix4& modelMatrix, const Vector3& alignAxis, bool transformZ,
		                CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		bool drawTrail(Entity entity, TrailRendererComponent& trail, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		// scratch of updateTrail: a trail's centre line and its points' parameters
		std::vector<Vector3> trailPoints;
		std::vector<float> trailPointT;
		void destroyTrail(Entity entity, TrailRendererComponent& trail);
		// the GPU side of a ribbon (thick line or trail): shader variant, buffers sized to its
		// capacity, bindings and pipelines; `ribbonVariant` the variant properties without the
		// post colour bit
		template<typename T>
		bool loadRibbonRender(Entity entity, T& component, uint16_t pipelines, uint64_t ribbonVariant, BufferUsage usage);
		template<typename T>
		void destroyRibbonRender(Entity entity, T& component);
		// CPU: the ribbon of a thick line or the segments of a hairline polyline, when dirty;
		// needReload when the variant or the capacity the data needs changed
		void updateLinesGeometry(Entity entity, LineRendererComponent& lines, float dt);
		// a trail's activity for this pass (its own enabled, not the GameObject's other renderers)
		bool isTrailRenderActive(Entity entity, const TrailRendererComponent& trail, uint32_t cullingMask) const;
		// the world bounds of a line's ribbon (with its width), null when not culled (pixel widths)
		AABB linesWorldBounds(const LineRendererComponent& lines, const Transform& transform) const;

		bool drawSky(SkyboxComponent& sky, PipelineType pipType, const Matrix4& viewProjection);
		void destroySky(Entity entity, SkyboxComponent& sky);

		void destroyLight(LightComponent& light);
		void destroyCamera(CameraComponent& camera, bool entityDestroyed);
		
		void updateSkyViewProjection(SkyboxComponent& sky, CameraComponent& camera);
		Matrix4 skyViewProjection(const SkyboxComponent& sky, const CameraComponent& camera) const;
		void updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent& camera, Transform& cameraTransform);
		// Shadow cameras of light seen through camera: the directional cascades fitted to its
		// frustum (fit: in the previous fit, out this one; frame: the scene camera's, which
		// set_shadow_debug moves), or the spot / point cameras.
		void computeLightCameras(const LightComponent& light, const Transform& transform, const CameraComponent& camera, const Transform& cameraTransform,
		                         LightCamera* cameras, unsigned int& numCascades, CascadeFitResult& fit, bool& hasFit, bool frame,
		                         Vector3& fitPosition, Vector3& fitForward);
		void updatePoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void updateTerrain(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainView& view);
		static void updateCameraFrustumPlanes(const Matrix4 viewProjectionMatrix, Plane* frustumPlanes);
		void updateInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);

		void sortPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void sortInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);
		// sortInstancedMesh's buffers, kept between frames (a distance-sorted batch sorts every frame)
		std::vector<unsigned int> sortOrderScratch;
		std::vector<float> sortDepthScratch;
		std::vector<InstanceRenderData> sortInstancesScratch;
		std::vector<InstanceBounds> sortBoundsScratch;
		std::vector<InstanceVatRenderData> sortVatScratch;

	public:
		// what the FLATTEN Canvas Groups drew in the last frame (tests, diagnostics)
		struct CanvasGroupRenderStats{
			uint32_t layers = 0;            // layers drawn and composited
			uint32_t skippedBranches = 0;   // FLATTEN groups at alpha 0: their UI was skipped whole
			uint32_t fallbacks = 0;         // layered groups drawn Per Element (an element not ready for the layer)
			uint32_t pooled = 0;            // layer targets in the pool
			uint32_t sharedTargets = 0;     // layers drawn into a target another layer of the pass uses
			uint64_t targetsCreated = 0;    // layer targets created or resized since the scene started
		};
		const CanvasGroupRenderStats& getCanvasGroupRenderStats() const { return lastCanvasGroupRenderStats; }

	private:
		CanvasGroupRenderStats canvasGroupRenderStats;
		CanvasGroupRenderStats lastCanvasGroupRenderStats;

	public:

		RenderSystem(Scene* scene);
		virtual ~RenderSystem();

		// Editor-only viewport debug override (see member declaration). Free to toggle:
		// meshes draw with no-culling variants of their pipelines, built once on first use.
		void setDisableFaceCulling(bool disableFaceCulling);
		// the Draw Mode of the frame's view (the Scene view in Edit mode; editor only)
		void setDrawMode(ViewDrawMode drawMode);

		// editor-only: the frame's own view draws as if the scene had no fog (the Scene view's
		// Fog toggle; the fog variants stay, their fog is neutral), as a secondary view's
		// ViewDesc::disableFog
		void setDisableFog(bool disableFog);
		// editor-only: the frame's own view draws without the volumetric clouds (the Scene
		// view's Clouds toggle), and at this atmosphere quality (the Game view's preview while
		// playing; -1 the project's)
		void setDisableClouds(bool disableClouds);
		void setPreviewAtmosphereQuality(int quality);
		// what the atmosphere ran in the last frame of a view (0: the frame's own)
		const AtmosphereInfo& getAtmosphereInfo(uint32_t viewId = 0);
#ifdef JASPERENGINE_EDITOR
		// the frame's own view drew for an editor view (the Game view drawn as the frame):
		// what its atmosphere ran counts for that view too (getAtmosphereInfo(viewId))
		void keepAtmosphereInfo(uint32_t viewId);
#endif

		// Editor-only override (see member declaration). No reload needed: the
		// editor always renders through Engine::getFramebuffer(), so PIP_RTT is
		// already baked either way.
		void setDisableFixedResolution(bool disableFixedResolution);

		// Dynamic resolution (S13, PostProcessingSettings::dynamicResolution*): the render scale
		// the post-processed main camera draws at (the controller's while it runs, the pinned
		// one, else the settings'), whether the controller runs, and its state
		float getAppliedRenderScale() const;
		DynamicResolutionStatus getDynamicResolutionStatus() const;
		bool isDynamicResolutionRunning() const { return dynamicResolutionRunning; }
		const DynamicResolutionController& getDynamicResolution() const { return dynamicResolution; }
		// pins the applied scale (0.5..1) while the uber pass runs; 0 or below releases it
		void setDynamicResolutionOverride(float scale);
		float getDynamicResolutionOverride() const { return dynamicResolutionOverride; }
		// GPU milliseconds the controller takes as each frame's measurement (tests); 0 releases
		void setDynamicResolutionFakeGpuMs(float milliseconds);
		float getDynamicResolutionFakeGpuMs() const { return dynamicResolutionFakeGpuMs; }
		// pins the time the uber pass's effects animate with (grain, VHS, film damage), seconds;
		// below 0 releases it (deterministic captures)
		void setPostProcessingDebugTime(double seconds);
		double getPostProcessingDebugTime() const { return postDebugTime; }
		// the frame's own view draws without the scene's post-processing (the editor's edit-mode
		// Scene view): the colour passes bypass the post colour (no variant change), no uber pass
		void setDisablePostProcessing(bool disablePostProcessing);

		// the depth pass alpha-tests the submesh (MASK, or the legacy AUTO + textureShadow);
		// the editor derives export keys from it, so the rule lives in one place
		static bool usesAlphaMask(const Material& material, bool textureShadow);
		// a material drawn by the built-in Lit (or a fork of it) with the transmission lobe: in
		// the transparent queue, opaque over the scene colour copy it refracts
		static bool usesPbrTransmission(const Material& material);
		// The extended lobes, maps and hex tiling the submesh's material asks for with the
		// lighting of its loaded variant (unlit: hex tiling only), for a tier: what the
		// inspector and get_material_variant report (the maps it binds, the dropped ones)
		static PbrVariant getLoadedPbrVariant(const Submesh& submesh, PbrTier tier);

		// the lattice bindings packed into the meshes (LatticeRender::prepare, once a frame)
		LatticeRenderState& getLatticeState();

		// the bounds culling and the world AABB take for a mesh's vertices: a Vertex Animator's
		// animatedBounds when set, else verticesAABB
		static const AABB& getMeshLocalBounds(const MeshComponent& mesh);
		// the distance band of an opaque draw (its squared distance): 0 up to 1, then one per
		// doubling of the distance (NaN 0, infinity 63); the opaque queues sort by it before the state
		static uint8_t distanceBand(float squaredDistance);

		// Shader variants, derived the same way by loadMesh and by the editor collecting the
		// variants a build needs (from content, without loading anything on the GPU).
		// Sets the submesh's vertex attribute flags (hasTexCoord1, hasNormal, ...) from the
		// buffers it draws from and its own attributes, and hasNormalMap from its material.
		static void scanSubmeshAttributes(Submesh& submesh, const std::vector<const Buffer*>& buffers);
		// sceneMeshShader: the scene's default mesh shader (Scene::getDefaultMeshShader)
		static SubmeshShaderInputs getSubmeshShaderInputs(const MeshComponent& mesh, const Submesh& submesh, const TerrainComponent* terrain, const InstancedMeshComponent* instmesh, bool mirror,
		                                                  const std::string& sceneMeshShader = std::string());
		static SubmeshShaderVariant getSubmeshShaderVariant(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs);
		// Its automatic GPU instancing variants (spec S55 R1) while GPU Instancing is on and the
		// mesh allows it: an opaque submesh of the built-in or a library shader without
		// skinning, morph targets, a lattice, a Vertex Animation, an extra pass, a texture rect,
		// a mirror, a terrain or an InstancedMesh, whose vertex inputs leave room for the
		// instance's vectors (a fork, which keeps its own object block, never batches)
		// the vertices a submesh's positions hold (its own attribute's, else the mesh buffer's)
		static unsigned int submeshVertexCount(const MeshComponent& mesh, const Submesh& submesh);
		// the most vertices of a mesh dynamic batching takes (sprite-batching), which is never instanced
		static constexpr unsigned int DYNAMIC_BATCH_MAX_VERTICES = 64;
		static SubmeshBatchVariant getSubmeshBatchVariant(const MeshComponent& mesh, const Submesh& submesh, const SubmeshShaderInputs& inputs,
		                                                  const SubmeshShaderVariant& variant, const std::string& meshShaderSrc,
		                                                  const std::string& depthShaderSrc);
		// The lighting mode a submesh draws with: the material's when the shader lists it,
		// else the shader's default (the first of its manifest's @lighting, NONE without one;
		// the built-in Lit and forks without a manifest: PIXEL, listing PIXEL, VERTEX, MIXED,
		// NONE).
		// Never AUTO. manifest: the one the submesh draws with (SubmeshShaderInputs::manifest).
		static MaterialLighting resolveLighting(MaterialLighting requested, const ShaderManifest* manifest);
		// whether the shader lists the mode (AUTO: always)
		static bool supportsLighting(MaterialLighting mode, const ShaderManifest* manifest);
		// Whether the lit or unlit shader samples the material's textures
		static bool checkPBRTextures(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs);
		// Whether a mesh colour shader ("" built-in, a fork, a library reference) samples the
		// material's Stochastic Tiling (Material::stochasticTiling): the built-in Lit does; a fork
		// without manifest lines does when its .frag defines LIT_STOCHASTIC_PARAMS
		// (ShaderManifest::litStochasticParams; a fork made before the feature does not, nor one
		// whose files cannot be read); a shader with a manifest never does (a library shader has
		// its own STOCHASTIC feature). Read from the fork's entry point at runtime and by the
		// editor alike, so a build ships the variants the game asks for.
		static bool supportsStochasticTiling(const std::string& shader);
		// the shader reference is the engine's Lit: empty, or builtin:lit with its variant features
		static bool isBuiltinLit(const std::string& shader);
		MeshShaderEnvironment getMeshShaderEnvironment() const;
		// capacity tier (4, 8 or 16 lights) of the per-object lights blocks for a scene's
		// maxLightsPerObject: the light-capacity bits of every lit mesh variant
		static int getLightCapacity(int maxLightsPerObject);

		// ---- shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md)
		// The shader a submesh draws with: its material's, else the mesh's fork, else the
		// scene's default mesh shader; "" is the built-in Lit shader
		static const std::string& getSubmeshShader(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader);
		// The custom shader strings (with the "#FEATURES" suffix the material's features
		// resolve to) its color and depth variants are keyed by; "" for built-in. The depth
		// pass draws with the library shader's @depth program, else the mesh's depth fork.
		// A colour variant of a shader with a manifest also carries its resolved blend
		// mode's BLEND_* define (ShaderReference::getColorVariantReference).
		// The colour variant's reference takes the built-in Lit's own defines too (litDefines, a
		// SubmeshShaderVariant's): "builtin:lit#..." for the built-in shader, "<fork>#..." for a
		// fork of it. Without them: the reference of the shader the material names.
		static std::string getSubmeshShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader);
		static std::string getSubmeshShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader,
		                                             const std::vector<std::string>& litDefines);
		static std::string getSubmeshDepthShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader);
		// ---- shader library phase 2 (docs/superpowers/specs/2026-10-10-shader-library-phase2-design.md)
		// Whether the submesh draws an extra pass: its material names one (a non-empty
		// MaterialPass::shader) and it is not a Terrain's (terrain: the submesh's inputs)
		static bool hasExtraPass(const Submesh& submesh, bool terrain);
		// The custom shader string the extra pass's colour variant is keyed by: its shader with
		// its features and the BLEND_* define of its resolved blend mode
		static std::string getSubmeshExtraPassReference(const Submesh& submesh);
		// The inputs the extra pass's variant comes from: the submesh's (the same mesh variant
		// bits: skinning, morph targets, instancing, texture rect, vertex colour, fog, from
		// getSubmeshShaderInputs) with its own shader's manifest and features, its default
		// lighting mode, and no received shadows, IBL or mirror
		static SubmeshShaderInputs getExtraPassShaderInputs(const SubmeshShaderInputs& submeshInputs, const MaterialPass& pass);
		// Its colour pipeline state, resolved as the submesh's (resolveSubmeshRenderState):
		// the pass's values, else its shader's @state with the rules of its features
		// (manifestState, null: none), else the submesh's legacy behaviour
		static RenderState resolveExtraPassRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifestState* manifestState);
		// The color pipeline state of a submesh: the material's value, else the manifest's
		// @state with the @state[...] rules of the material's features, else the legacy
		// behaviour. blended: not opaque; transparentQueue: drawn sorted after the opaque objects
		static RenderState resolveSubmeshRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifest* manifest,
		                                             bool& blended, bool& transparentQueue);
		// The same with a manifest state already resolved (Submesh::manifestState; null: none)
		static RenderState resolveSubmeshRenderState(const MeshComponent& mesh, const Submesh& submesh, const ShaderManifestState* manifestState,
		                                             bool& blended, bool& transparentQueue);
		// Scene inputs (phase 3) a submesh's shaders (its own, else the mesh's or the scene's,
		// and its extra pass) read with its material's features, ShaderRequire_SceneDepth /
		// _SceneColor; opaqueQueue: whether the submesh resolves to the opaque queue, where
		// they read black and its own depth (a material error the inspector and checks report)
		static uint32_t getSubmeshSceneInputs(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader,
		                                      bool& opaqueQueue);
		// The scene inputs this scene's last frame produced: bit 0 the scene depth, bit 1 the colour
		uint32_t getSceneInputUsage() const;
		// The same for UI, points, lines and sky (AUTO: alpha blend and depth write, legacyCulling)
		static RenderState resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite, const ShaderManifest* manifest,
		                                               const std::vector<std::string>& shaderFeatures, bool legacyCulling);
		// with the component's stencil and colour write (phase 2; AUTO: the manifest's)
		static RenderState resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite,
		                                               const MaterialStencil& stencil, int colorWrite, const ShaderManifest* manifest,
		                                               const std::vector<std::string>& shaderFeatures, bool legacyCulling);
		// near and far as rendered: a perspective camera keeps 0 < near < far (0.001 apart at least),
		// whatever its component says; orthographic and UI ones as they are
		static Vector2 getCameraNearFar(const CameraComponent& camera);
		// A Points component's: Depth Write Auto (with a manifest's zwrite= Auto too) follows the
		// queue, written when opaque and not when transparent (Unity's transparent ZWrite Off).
		// depthFollowsQueue: whether it did, so a change of transparency needs new pipelines
		static RenderState resolvePointsRenderState(const PointsComponent& points, bool& depthFollowsQueue);
		// The variants a particle effect draws with (without the post-processing colour bit): its
		// quads, meshes and trails, the shader reference (its features and blend define)
		struct ParticleVariantSet{
			bool quads = false;
			bool meshes = false;
			bool trails = false;
			uint64_t quadProperties = 0;
			uint64_t meshProperties = 0;
			uint64_t trailProperties = 0;
			std::string reference;   // empty: the built-in shader
		};
		static ParticleVariantSet getParticleVariants(const particles::CompiledEffect& effect, bool fog);
		// the shader-library custom shader string of a UI/points/lines/sky shader with its
		// features and the BLEND_* define of its blend mode (Skybox: AUTO)
		static std::string getComponentShaderReference(const std::string& customShader, const std::vector<std::string>& shaderFeatures,
		                                               MaterialBlendMode blendMode);

		// The shader variants an entity draws with as it is now, in this scene: its mesh
		// (with the shadow/depth and SSR G-buffer passes), UI, points, lines or sky. A
		// forked shader also lists the built-in variant it falls back to. For warming
		// them up before the entity shows (ShaderVariantCollection::addEntityVariants).
		void getShaderVariants(Entity entity, std::vector<ShaderKey>& keys);

		// Logs what the loaded meshes hold on the GPU: submeshes, extra passes (none for a
		// submesh without one), generated smoothed-normal buffers and the live pipelines of
		// every object (Engine's ready signal prints it, for tests)
		void logRenderObjects();

		// copies the stacked scene composite to the swapchain (Engine::endCompositeFramebuffer)
		void presentFramebufferToSwapchain(Framebuffer* source);

		// pipelines this scene's cameras need; objects reload when it changes
		uint16_t getScenePipelines() const;

		bool loadMesh(Entity entity, MeshComponent& mesh, uint16_t pipelines, InstancedMeshComponent* instmesh, TerrainComponent* terrain);
		bool loadPoints(Entity entity, PointsComponent& points, uint16_t pipelines);
		bool loadLines(Entity entity, LineRendererComponent& lines, uint16_t pipelines);
		bool loadTrail(Entity entity, TrailRendererComponent& trail, uint16_t pipelines);
		bool loadUI(Entity entity, UIComponent& ui, uint16_t pipelines, bool isText);
		bool loadSky(Entity entity, SkyboxComponent& sky, uint16_t pipelines);

		void updateFramebuffer(CameraComponent& camera);
		void updateTransform(Transform& transform, Entity entity);
		void updateCamera(CameraComponent& camera, Transform& transform);
		void updateMirrors(Entity mainCameraEntity);
		Entity createMirrorCamera(Entity mirrorEntity);
		Entity getMirrorCamera(Entity mirrorEntity) const;
		void destroyMirrorCamera(Entity entity);

		// camera
		void updateCameraSize(Entity entity);
		// sizes an auto-resize camera for a canvas of canvasWidth x canvasHeight, the
		// Engine canvas in updateCameraSize(entity); a render-to-texture camera keeps its framebuffer's
		void updateCameraSizeForCanvas(Entity entity, float canvasWidth, float canvasHeight);
		bool isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const AABB& box);
		bool isInsideCamera(CameraComponent& camera, const AABB& box);
		bool isInsideCamera(CameraComponent& camera, const Vector3& point);
		bool isInsideCamera(CameraComponent& camera, const Vector3& center, const float& radius);
		bool isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const Vector3& center, const float& radius);

		void needReloadPoints();
		// Line Renderers and Trail Renderers
		void needReloadLines();

		// Trail Renderers (TrailSystem calls it once per trail per update, after simulating):
		// builds the ribbon when its points or settings changed, loads or reloads it
		void updateTrail(Entity entity, TrailRendererComponent& trail, float dt);
		// the shader variant properties (without the post colour bit) a Line Renderer / Trail
		// Renderer loads: the hairline (Vc4) or the ribbon (Rib, Tex with a base texture on a
		// shader without manifest, Pxw for pixel widths). Shader warmup and the editor's
		// variant collection use them too
		static uint64_t getLinesVariantProperties(const Scene* scene, const LineRendererComponent& lines);
		static uint64_t getTrailVariantProperties(const Scene* scene, const TrailRendererComponent& trail);
		// the base texture (shaderTextures' u_baseTexture), empty without one
		static const Texture* getLinesBaseTexture(const std::vector<std::pair<std::string, Texture>>& shaderTextures);
		void needReloadMeshes();
		// a change of the scene's shader environment that leaves every resource the loaded
		// variants bind alive (light cap, fog, lights added or removed, reflection probes, 2D
		// shadows, scene depth programs): a loaded mesh keeps drawing its variant until the new
		// one's shaders are built (meshVariantShadersReady), then reloads in that update, so a
		// runtime compile shows no gap; with precompiled shaders it reloads in the same update
		// as needReloadMeshes does. Once they are built, the loaded objects make their pipelines
		// on the new variants and warm them (spec S32 R4, R8), and the reload waits for those too
		void needReloadMeshVariants();
		// the meshes with a submesh whose shader can ask for the Water Excluders or Wakes
		void needReloadWaterVariants();
		// the hull map array and the wake map (once a frame, before the passes draw)
		void syncWaterInteraction();
		void destroyWaterInteraction();
		// the excluders' data texture with room for that many rows (made again only to grow)
		bool ensureWaterExclusionData(size_t rows);
		bool meshVariantShadersReady(Entity entity, MeshComponent& mesh, const InstancedMeshComponent* instmesh, const TerrainComponent* terrain);
		// the inputs a load of the submesh would resolve now, from its attributes of the last load
		SubmeshShaderInputs getCurrentSubmeshInputs(Entity entity, const MeshComponent& mesh, unsigned int index, const TerrainComponent* terrain,
		                                            const InstancedMeshComponent* instmesh, bool hasMirror, const std::string& sceneMeshShader) const;
		// whether the loaded submesh draws that variant in every pass it builds (colour with its
		// Lit defines and extension maps, depth, G-buffer, the material's extra pass)
		static bool loadedVariantMatches(const Submesh& submesh, const SubmeshShaderVariant& variant, const SubmeshShaderVariant* extraPass);
		// how many times a submesh's texture edit resolved its variant again (tests: a rebind of
		// the same images runs none)
		static uint64_t getTextureVariantChecks();
		void needReloadPostProcess();
		void needUpdatePostProcessUniforms();
		// the scene's values for a default shader changed: the components drawing with it
		// rewrite their custom blocks at their next draw (Scene::setDefaultShaderUniform)
		void needUpdateDefaultShaderUniforms(ShaderType type);
		// the scene's PostProcessingSettings changed (Scene::setPostProcessing)
		void needUpdatePostProcessing();
		// an image of a terrain's painted layers changed on disk (the editor's file pick-up): the
		// array read from the files is built again
		void invalidateTerrainDetailArray(Entity entity);
		// the images post-processing reads changed on disk (the editor's file pick-up): the LUT
		// strip (read by path, cached while the path stays) is read again and the user chain
		// reloads, taking its pass textures again from the scene
		void invalidatePostProcessTextures();
		// what a set of post-processing settings turns on (the export collector and the editor
		// use it to pick the same variants), and what the renderer applies now
		// The user LUT is read through the CubeLut cache to know its stage (a scene-stage .cube
		// changes the curve, not the grade); the second form takes the loaded one (null: none)
		static PostProcessingTiers getPostProcessingTiers(const PostProcessingSettings& settings);
		static PostProcessingTiers getPostProcessingTiers(const PostProcessingSettings& settings, const CubeLut* lut);
		// the k of the in-shader LUT's shaper s = sqrt(x / (x + k)) for a LUT of that side
		// (PostColorLut::bake, includes/post_color.glsl): 1, or with tone mapping NONE the
		// one putting the clip point (2^-exposure) on a texel, so whites reach 255 at any side
		// (a scene-stage LUT moves the clip, so it keeps k = 1 too)
		static float getPostColorShaper(const PostProcessingSettings& settings, int lutSide, const CubeLut* lut = nullptr);
		const PostProcessingTiers& getActivePostProcessingTiers() const { return postTiers; }
		void needReloadUIs();
		void needReloadSky();
		void prepareMeshForDataReload(Entity entity, MeshComponent& mesh);

		// the SSR G-buffer pass's reflection probes and slots in the frame's view (get_render_stats)
		const SSRViewStats& getSSRStats() const;

		bool isAllLoaded() const;
		void getLoadCount(size_t& loaded, size_t& total) const;
		// what is not loaded yet, as "<entity name> (<what>)": what keeps isAllLoaded() false
		std::vector<std::string> describeNotLoaded() const;
		// every load isAllLoaded() waits for: loaded or not, its entity (NULL_ENTITY: the
		// scene's own) and what it is
		void forEachLoad(const std::function<void(bool loaded, Entity entity, const char* what)>& visit) const;

		// Scene inputs (shader library phase 3) in detail: what the loaded objects read, and
		// what the last frame's camera passes made for them (getSceneInputUsage: the short
		// form). For the editor and diagnostics; no per-frame work.
		struct SceneInputDetails{
			uint32_t reads = 0;            // ShaderRequire_SceneDepth | _SceneColor of the loaded readers
			uint32_t readers = 0;          // loaded meshes and points components reading the scene
			uint32_t passes = 0;           // last frame: camera passes with a visible reader
			uint32_t depthPrePasses = 0;   // last frame: scene depth pre-passes run for them
			uint32_t sharedDepth = 0;      // last frame: passes that reused the SSAO / SSR / post depth
			uint32_t colorCopies = 0;      // last frame: colour copies
			uint32_t colorRecopies = 0;    // last frame: of them, taken again for readers over other readers
			uint32_t targets = 0;          // depth and copy targets alive (the editor's views included)
			bool depthPrograms = false;    // every mesh keeps a depth program for the scene depth
			bool colorRedirect = false;    // the exported scene camera draws offscreen for the copy now
		};

		// what the last frame's water reflection passes were (get_render_stats, the scene
		// settings' In Use row)
		struct PlanarReflectionStats{
			uint32_t planes = 0;     // water heights seen by the views' cameras
			uint32_t passes = 0;     // reflection passes drawn
			uint32_t fallbacks = 0;  // heights over the cap (their waters show the environment)
			uint32_t width = 0;      // the last pass's target
			uint32_t height = 0;
		};

		SceneInputDetails getSceneInputDetails();
		// what the last frame's water reflection passes were
		PlanarReflectionStats getPlanarReflectionStats() const{ return lastPlanarReflectionStats; }
	private:
		PlanarReflectionStats planarReflectionStats;
		PlanarReflectionStats lastPlanarReflectionStats;
	public:
		bool hasPendingMeshLods() const;

		// the detail levels the main view (draw()) drew in its last frame
		LodFrameStats getLodStats() const;
	
#ifdef JASPERENGINE_EDITOR
		// Editor gizmos and icons that live in the scene they annotate. Only the passes
		// of an editor view draw them: never the game, a camera image or a shadow map.
		void setEditorOverlay(Entity entity, bool overlay);
		bool isEditorOverlay(Entity entity) const;
		// whether draw() is an editor view (the editor turns it off while the scene plays)
		void setDrawEditorOverlays(bool draw);

		// A secondary view: this scene drawn again after the frame's draw(), through
		// another camera into another target. Nothing is updated and nothing done once
		// per frame runs twice: the view reuses the frame's uploads, keeps its own
		// camera-dependent state (object matrices, lighting, shadow cascades and maps,
		// culled instances, detail levels, terrain cut, sorted points, mirrors) and its
		// own targets sized to it, created on first use and resized only with the view.
		struct ViewDesc{
			Entity camera = NULL_ENTITY;
			// where it draws; viewRect is the part of it the camera covers
			Framebuffer* target = nullptr;
			Rect viewRect;
			// the scene under the others: clears the target and runs the post-process chain
			bool baseLayer = true;
			// draws this scene's editor gizmos and icons
			bool editorOverlays = false;
			// renders at the scene's fixed game resolution when it has one
			bool fixedResolution = false;
			// draws as if the scene had no fog
			bool disableFog = false;
			// the scene's post-processing applies to the view camera (the Game view; the Scene
			// view leaves it off by default): with it off the colour passes draw as if the scene
			// had none (its variants keep the post colour bit, bypassed)
			bool postProcessing = true;
			// the view camera's passes cull no face (the Scene view's debug toggle)
			bool disableFaceCulling = false;
			// draws without the volumetric clouds; at this atmosphere quality (-1 the project's)
			bool disableClouds = false;
			int atmosphereQuality = -1;
			// canvas an auto-resize camera covers in this view (0: as the camera is)
			float canvasWidth = 0.0f;
			float canvasHeight = 0.0f;
			// off: no shadow maps are drawn for the view and its fragments read no shadow
			bool shadows = true;
			ViewDrawMode drawMode = ViewDrawMode::SHADED;
		};
		void drawView(uint32_t viewId, const ViewDesc& desc);
		// the level a mesh drew in an editor view's last frame (its plan's dominant level;
		// LOD_CULLED: culled for its size), 0 before it drew there
		unsigned int getViewMeshLod(uint32_t viewId, Entity entity) const;
		// frees what a view created
		void releaseView(uint32_t viewId);
		// the detail levels an editor view drew in its last frame (its Stats overlay)
		LodFrameStats getViewLodStats(uint32_t viewId) const;
#endif

		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //RENDERSYSTEM_H
