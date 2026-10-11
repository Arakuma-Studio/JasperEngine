// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEM_H
#define RENDERSYSTEM_H

#include "SubSystem.h"
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
#include "component/LineRendererComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "render/ObjectRender.h"
#include "render/CameraRender.h"
#include "render/BufferRender.h"
#include "render/FramebufferRender.h"
#include "buffer/ExternalBuffer.h"
#include "shader/ShaderBuildTypes.h"
#include "shader/ShaderManifest.h"
#include "Engine.h"
#include <map>
#include <array>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

namespace jasperengine{
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
	} frame_t;

	// u_vs_objectLights / u_fs_objectLights (shaderlib/includes/lights.glsl) at the largest
	// capacity. A variant of capacity C reads, in order: counts, position_type[C],
	// direction_inner[C], color_outer[C] (the vertex stage stops here, in mixed mode), the
	// L1 SH ambient (3, the vertex stage stops here in vertex mode), spot_shadow[C]: 4 + 4 C
	// vectors in the fragment stage (RenderSystem::buildObjectLights).
	typedef struct object_lights_t {
		Vector4 data[4 + 4 * MAX_LIGHTS_PER_OBJECT];
	} object_lights_t;

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

	typedef struct fs_terrain_layers_t {
		Vector4 colorFactor[MAX_TERRAIN_LAYERS]; // rgb = linear tint, w = 1 on a PBR layer
		Vector4 uvTransform[MAX_TERRAIN_LAYERS]; // xy = detail tiling scale, zw = offset
		Vector4 surface[MAX_TERRAIN_LAYERS];     // roughness, metallic, normal strength, occlusion strength
		Vector4 slices[MAX_TERRAIN_LAYERS];      // color, normal and packed surface slice (-1 = none)
	} fs_terrain_layers_t;

	typedef struct fs_reflection_probe_t {
		Vector4 position_weight; // xyz = capture position, w = local probe blend weight
		Vector4 boxMin_intensity; // xyz = influence AABB min, w = intensity
		Vector4 boxMax_lod;       // xyz = influence AABB max, w = max available mip LOD
	} fs_reflection_probe_t;

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
		Vector4 params;             // x = roughness, y = metallic, z = IBL source, w = alpha cutout
		Vector4 baseColorFactor;
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

	typedef struct fs_composite_t {
		Matrix4 invProjection;
		Matrix4 invView;
		Vector4 params;   // x=intensity, y=flip G-buffer Y, z=debug, w=unused
		Vector4 envColor; // rgb = env color (linear), w = env rotation (radians)
	} fs_composite_t;

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
	} scene_inputs_t;

	typedef struct vs_points_params_t {
		Matrix4 mvpMatrix;
		float pointScale;
		float padding[3];
	} vs_points_params_t;

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
		MaterialAlphaMode alphaMode = MaterialAlphaMode::AUTO;
		// as the material asks; the variant resolves it against the shader (resolveLighting)
		MaterialLighting lighting = MaterialLighting::AUTO;
		bool textureShadow = false;
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
		// shader library: the manifest of the shader the submesh draws with (null for the
		// built-in shader and forks without one) and whether its depth pass draws with the
		// library shader's own depth program (@depth)
		std::shared_ptr<const ShaderManifest> manifest;
		bool depthProgram = false;
		// the manifest's defines the material enables (ShaderReference::getEnabledFeatures),
		// which decide its feature= @require inputs; empty: the unconditional ones only
		std::vector<std::string> features;
	};

	// The shaders one submesh draws with
	struct SubmeshShaderVariant {
		uint32_t properties = 0;        // ShaderType::MESH
		bool depthPass = false;         // drawn into shadow maps and depth pre-passes
		bool depth = false;             // has a depth shader: a depth pass, or a forked depth shader
		uint32_t depthProperties = 0;   // ShaderType::DEPTH
		bool gbuffer = false;           // drawn into the SSR G-buffer
		uint32_t gbufferProperties = 0; // ShaderType::GBUFFER
	};

	class JASPERENGINE_API RenderSystem : public SubSystem {
	private:
		enum class TransparentRenderType{
			MESH,
			POINTS
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
		};

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
		};

		// One merged run of tilemap indices to draw, in elements relative to the
		// submesh index range (see selectTilemapChunks).
		struct TilemapDrawRange{
			unsigned int offset;
			unsigned int count;
		};

		// draw order of the transparent queue: the lowest sort priority, then the farthest, then
		// the submission order (a total order, so std::sort is deterministic)
		struct TransparentRenderComparison{
			bool operator()(const TransparentRenderData& lhs, const TransparentRenderData& rhs) const{
				if (lhs.sortPriority != rhs.sortPriority)
					return lhs.sortPriority < rhs.sortPriority;
				if (lhs.distanceToCamera != rhs.distanceToCamera)
					return lhs.distanceToCamera > rhs.distanceToCamera;
				return lhs.order < rhs.order;
			}
		};

		// draw order of the opaque queue of a 3D pass: the lowest sort priority, then the
		// nearest, then the submission order
		struct OpaqueRenderComparison{
			bool operator()(const OpaqueRenderData& lhs, const OpaqueRenderData& rhs) const{
				if (lhs.sortPriority != rhs.sortPriority)
					return lhs.sortPriority < rhs.sortPriority;
				if (lhs.distanceToCamera != rhs.distanceToCamera)
					return lhs.distanceToCamera < rhs.distanceToCamera;
				return lhs.order < rhs.order;
			}
		};

		// the queues of the camera pass being drawn (renderCameraPass, the reflection probe
		// face), kept between passes so a pass allocates nothing once they have grown
		std::vector<OpaqueRenderData> opaqueQueue;
		std::vector<TransparentRenderData> transparentQueue;

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

		// editor-only, draws as if the scene had no fog
		bool disableFog = false;

		// Editor-only override that suppresses the scene's fixed game resolution
		// (edit-mode viewports render native; play mode re-enables it). Defaults
		// to false and is never set at runtime, so exported games are unaffected.
		bool disableFixedResolution = false;

		static uint32_t pixelsWhite[64];
		static uint32_t pixelsBlack[64];
		static uint32_t pixelsNormal[64];

		static TextureRender emptyWhite;
		static TextureRender emptyArrayWhite;
		static TextureRender emptyBlack;
		static TextureRender emptyCubeBlack;
		static TextureRender emptyCubeWhite;
		static TextureRender emptyNormal;
		static TextureRender emptyShadowDepth;
		// shader library @texture defaults besides white and black
		static uint32_t pixelsGray[64];
		static uint32_t pixelsFlatNormal[64];
		static TextureRender emptyGray;
		static TextureRender emptyFlatNormal;
		static TextureRender emptyCubeGray;

		static bool emptyTexturesCreated;
		
		bool hasLights;
		bool hasShadows;
		bool hasFog;
		bool hasIBL;
		bool hasReflectionProbes;
		bool hasMultipleCameras;
		// extra cameras the last frame drew, to catch the switch back to main only
		bool lastMultiCameraDraw;
		bool capturingReflectionProbe;
		// batch order currently baked into the shared points/instance arrays
		bool lastBatchSort;
		// scene mesh LOD switch the levels were built (or freed) for
		bool lastMeshLodEnabled = false;
		// pipelines the objects were loaded with, to catch a destination change
		uint16_t loadedPipelines;

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
		};

		std::unordered_map<Entity, std::unique_ptr<ReflectionProbeRuntime>> reflectionProbeRuntimes;
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
			bool needUpload = false;
		};
		bool instanceViewsDirty;

		// error a view hides at a distance: threshold * (perUnit * distance + constant)
		struct LodView{
			Vector3 origin;
			float perUnit = 0;   // perspective: units per pixel at distance 1
			float constant = 0;  // orthographic: units per pixel
			float threshold = 1;
			bool enabled = false;
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
			vs_shadows_t vs_shadows;
			fs_shadows_t fs_shadows;
			fs_point_shadows_t fs_point_shadows;

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
			// its own, created on first need, resized with the frame's layout (the point atlas
			// keeps only the rows the point lights use), freed when unused
			FramebufferRender ownProjective;
			FramebufferRender ownPoint;
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
		};

		// one camera's color pass (see renderCameraPass)
		struct CameraPass{
			CameraComponent* camera = nullptr;
			Transform* cameraTransform = nullptr;
			// the view camera: culled instances, detail levels, SSAO, SSR, post-process, fixed resolution
			bool isMain = false;
			// draws into camera->framebuffer
			bool renderToTexture = false;
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
		struct RenderView{
			InstanceView instanceViews[1 + MAX_SHADOW_ATLAS_SLOTS];
			LodView lodView;

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
			Framebuffer gbufferFramebuffer;    // MRT: color[0] packed depth, color[1] view-space normal/roughness/metallic
			Framebuffer sceneColorFramebuffer; // offscreen opaque scene color (top-left)
			Framebuffer ssrFramebuffer;        // reflection color + mask (logical orientation)
			Framebuffer ssrBlurFramebuffer;    // glossy-blurred reflection

			unsigned int fixedResWidth = 0;
			unsigned int fixedResHeight = 0;
			Framebuffer fixedResFramebuffer;   // offscreen scene color at the fixed size

			unsigned int postProcessWidth = 0;
			unsigned int postProcessHeight = 0;
			Framebuffer postProcessFramebuffer[2]; // ping-pong targets

			// scene inputs (shader library phase 3) of the camera passes drawn in this view:
			// packed depth and colour copies by size, reused across passes and frames, freed
			// once unused for a while (releaseSceneInputTargets)
			std::vector<std::unique_ptr<SceneInputTarget>> sceneInputTargets;
			// the scene camera's colour pass when only the scene colour needs it offscreen
			// (exported builds drawing to the swapchain): blitted to the swapchain after
			Framebuffer sceneColorRedirect;

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

		// what one pass draws of a mesh
		struct MeshDraw{
			unsigned int instanceCount = 1;
			unsigned int instanceFirst = 0;
			unsigned int lod = 0;
		};
		Entity activeReflectionProbe = NULL_ENTITY;
		// the scene's sky: its first SkyboxComponent, as last loaded (another one leads the
		// array after a removal or a reorder, see update)
		Entity activeSky = NULL_ENTITY;
		fs_reflection_probe_t fs_reflection_probe;

		// 2D light path (Light2DComponent). Deliberately independent of
		// scene->getLightState(): the editor forces LightState::OFF in 2D scenes
		// to keep 3D lights out, but 2D lights must still work there.
		bool hasLights2D;
		bool hasShadows2D;
		int numLights2D;

		// Directional + spot atlas keeps up to 3x3 logical slots, but its physical
		// grid is compacted to the slots used by the current light set.
		FramebufferRender shadowAtlasFramebuffer;
		CameraRender shadowAtlasPassRender;
		unsigned int shadowAtlasSlotResolution;
		int shadowAtlasCols;
		int shadowAtlasRows;
		int shadowAtlasUsedSlots;
		bool needUpdateShadowAtlas;
		bool hasShadowAtlas;

		// separate atlas for point-light cube faces
		FramebufferRender shadowPointAtlasFramebuffer;
		CameraRender shadowPointAtlasPassRender;
		unsigned int shadowPointAtlasSlotResolution;
		bool needUpdateShadowPointAtlas;
		bool hasShadowPointAtlas;

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
		// per-draw scratch of drawMesh
		object_lights_t objectLightsBlock;
		fs_lighting_t legacyLightingBlock;
		fs_lighting2d_t fs_lighting2d;
		fs_fog_t fs_fog;

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

		// engine-written custom uniforms: seconds since startup, sampled once per draw(),
		// and the size of the target being drawn (shadow slot, SSAO depth or camera color)
		float frameTime;
		Vector2 passResolution;

		// the engine frame block (u_vs_frame / u_fs_frame) of the camera pass being drawn,
		// filled once per pass and uploaded to every draw whose shader declares it, and the
		// built-in Lit's part of it (u_fs_litFrame)
		frame_t passFrame;
		lit_frame_t passLitFrame;

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
		};
		bool postProcessLoaded;
		bool postProcessNeedReload;
		bool postProcessNeedUpdateUniforms;    // rewrite pass values, no chain rebuild
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
		std::unordered_map<Entity, uint32_t> sceneInputPoints;
		uint32_t sceneInputUsage = 0;          // the bits of both maps
		bool sceneDepthPrograms = false;       // MeshShaderEnvironment::sceneDepth the meshes load with
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
			uint32_t targetsCreated = 0;   // since the scene started (reuse keeps it constant)
		};
		SceneInputStats sceneInputStats;
		SceneInputStats lastSceneInputStats;

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
		void orderObjectLights(const MeshComponent& mesh, uint32_t properties, int order[MAX_LIGHTS_PER_OBJECT], int& pixelCount, int& vertexCount) const;
		// the per-object lights block of a submesh variant for the camera pass being drawn
		void buildObjectLights(const MeshComponent& mesh, uint32_t properties, object_lights_t& block) const;
		int passLightCascades(const SceneLight& light) const;
		// the legacy u_fs_lighting of an older fork: the object's first pixel lights
		void buildLegacyLighting(const MeshComponent& mesh, uint32_t properties, fs_lighting_t& block) const;
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
		void releaseReflectionProbeMaps(ReflectionProbeRuntime& runtime);
		void destroyReflectionProbe(Entity entity, ReflectionProbeComponent& probe);
		bool selectReflectionProbe(const Vector3& worldPosition, fs_reflection_probe_t& params, TextureRender*& texture);
		void initShadowAtlasRects();
		void initShadowPointAtlasRects();
		unsigned int clampShadowAtlasSlotResolution(unsigned int requestedResolution, int atlasCols, int atlasRows) const;
		bool ensureShadowAtlas(unsigned int slotResolution, int usedSlots);
		bool ensureShadowPointAtlas(unsigned int slotResolution);
		Rect getShadowAtlasSlotRect(int slotIndex) const;
		Rect getShadowPointAtlasSlotRect(int slotIndex) const;
		void configureLightShadowNearFar(LightComponent& light, const CameraComponent& camera);
		Matrix4 getDirLightProjection(const Matrix4& viewMatrix, const Matrix4& sceneCameraInv, float shadowMaxDistance, const Vector3& lightDirection, const Vector3& cameraPosition);
		// shader library: the frame block of a camera pass (fillFrameBlock fills the members
		// library shaders read; engine-internal members appended after them get their own
		// fill functions, called after it), uploaded by applyFrameBlock. updatePassFrame
		// fills passFrame once per camera pass, before its shadow, depth and colour draws.
		void fillFrameBlock(frame_t& frame, const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution);
		void updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform);
		void updatePassFrame(const CameraComponent& camera, const Transform& cameraTransform, const Vector2& resolution);
		// the size of the target a camera's colour pass renders into
		Vector2 passFrameResolution(const CameraComponent& camera) const;
		void applyFrameBlock(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// the scene inputs of the pass being drawn, to a colour draw whose shader declares them
		// (the far-plane depth and black where the pass has none)
		void applySceneInputs(ObjectRender& render, const ShaderLibraryBindings& bindings);
		// resolves the frame block slots and the custom textures of a loaded shader, then
		// binds the textures (false while one is still loading)
		bool loadShaderLibraryBindings(ShaderLibraryBindings& bindings, ShaderData& shaderData, ObjectRender& render,
		                               std::vector<std::pair<std::string, Texture>>& textures, const ShaderManifest* manifest);
		// binds the resolved custom textures again (they changed or a render target was redrawn)
		bool bindShaderTextures(const ShaderLibraryBindings& bindings, ObjectRender& render, std::vector<std::pair<std::string, Texture>>& textures);
		static bool shaderTexturesOutdated(const ShaderLibraryBindings& bindings, const std::vector<std::pair<std::string, Texture>>& textures);
		TextureRender* getDefaultShaderTexture(int fallback, bool cube);

		bool checkPBRFrabebufferUpdate(Material& material);
		bool checkPBRTextures(Material& material, bool receiveLights);
		bool loadPBRTextures(Material& material, ShaderData& shaderData, ObjectRender& render, bool receiveLights);
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
		unsigned int bindSubmeshAttributes(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh,
		                                   BufferMap& buffers, BufferRenderMap& bufferNameToRender, BufferStrideMap& bufferStride,
		                                   InstancedMeshComponent* instmesh);
		// a_smoothNormal for a render whose shader declares it: the mesh's generated buffer
		// of the submesh's positions, made when missing; false when the mesh cannot have one
		bool bindSmoothNormals(ObjectRender& render, ShaderData& shaderData, MeshComponent& mesh, Submesh& submesh, BufferMap& buffers);
		void releaseSmoothNormals(MeshComponent& mesh, bool keepData);
		// forgets the generated buffers no submesh of the finished load uses
		void trimSmoothNormals(MeshComponent& mesh);
		static bool releaseSubmeshLod(Submesh& submesh);
		void releaseMeshLods(MeshComponent& mesh);
		static bool isMeshLodPending(Submesh& submesh);
		void updateMeshLodErrors(MeshComponent& mesh);
		LodView computeLodView(const CameraComponent& camera, const Transform& cameraTransform) const;
		void updateMainLodView(CameraComponent& camera, Transform& cameraTransform);
		unsigned int selectMeshLod(const MeshComponent& mesh, const LodView* view, float distance, float worldScale) const;
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
		bool ensureOwnShadowAtlas(CameraShadows& target, bool projective);
		// rows of point atlas slots the point lights use (a camera's own point atlas holds no more)
		int ownPointAtlasRows() const;
		// frees the camera shadows unused for a while
		void releaseUnusedCameraShadows();
		// the CDLOD cut camera draws (and its shadow passes) into terrain view terrainView
		void selectCameraTerrain(CameraComponent& camera, Transform& cameraTransform, int terrainView);

		// instance views
		void buildInstanceViews(CameraComponent& mainCamera, Transform& mainCameraTransform);
		// limitDistance drops instances past the batch's cull distance from eye
		void buildInstanceView(InstanceView& view, InstanceViewRange& range, const InstanceRenderData* instances, const InstanceBounds* instanceBounds, unsigned int count, const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const LodView& lodView, const Vector3& eye, const Vector3& origin, const Vector3& direction, InstanceOrder order, bool limitDistance = true);
		void uploadInstanceViews(InstanceView* views, size_t count);
		bool selectMeshDraw(Entity entity, MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh, bool ownLod, int instanceView, const LodView* lodView, MeshDraw& draw);
		void bindInstances(ObjectRender& render, InstancedMeshComponent& instmesh, int instanceView, unsigned int first);
		void drawSubmeshGeometry(ObjectRender& render, Entity entity, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView, unsigned int lod, unsigned int instanceCount);

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
		Vector3 instanceFadeEye(Entity entity, const InstancedMeshComponent& instmesh, int instanceView) const;
		bool isBatchVisible(Entity entity, const MeshComponent& mesh, const Transform& transform, const InstancedMeshComponent* instmesh, int instanceView, const float cameraFar, const Plane frustumPlanes[6]);
		TerrainView& terrainViewData(Entity entity, TerrainComponent& terrain, int terrainView);
		// the terrain cut of the view camera, which its depth, G-buffer and shadow passes draw
		int mainTerrainView() const;
		// model, normal and mvp matrices of an object, in one block (scratch when copied)
		const Matrix4* objectMatrices(Transform& transform, Matrix4 (&scratch)[3]);
		const Matrix4& objectMVP(Transform& transform) const;
		const Matrix4& objectModel(Transform& transform) const;
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

	protected:

		void updateMeshBuffers(MeshComponent& mesh);
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
		TextureRender* renderSSAO(CameraComponent& camera, bool fixedResolution, TextureRender* sharedDepth = nullptr);

		// SSR
		void loadSSR();
		void destroySSR();
		bool ensureSSRFramebuffers(unsigned int width, unsigned int height);
		void renderDepthPrePass(CameraComponent& camera); // shared depth for SSAO/SSR
		// the opaque meshes' packed depth (PIP_DEPTH) of a camera into target: the SSAO,
		// post-process and scene depth pre-passes. perCameraTransform: the camera's, when the
		// pass makes its own per-object matrices (renderCameraPass's perCameraMVP), else null
		void renderOpaqueDepth(CameraComponent& camera, Framebuffer& target, unsigned int width, unsigned int height,
		                       int instanceView, const LodView* lodView, Transform* perCameraTransform);

		// scene inputs (phase 3)
		// records what a loaded mesh or points component reads of the scene (0: nothing)
		void setSceneInputUser(Entity entity, bool points, uint32_t inputs);
		// the scene inputs the visible draws of a camera pass require
		uint32_t findPassSceneInputs(CameraComponent& camera, int instanceView);
		// whether the entity has a draw requiring the scene colour (the 2D pass copies before it)
		bool readsSceneColor(Entity entity, Signature signature) const;
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
		bool drawMeshGBuffer(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_gbuffer_t vsGBufferParams, bool hasLocalProbe, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int instanceView = -1, const LodView* lodView = nullptr);
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
		void renderPostProcess(FramebufferRender* destination);

		bool drawUI(UIComponent& ui, Transform& transform, PipelineType pipType);
		void destroyUI(Entity entity, UIComponent& ui);

		bool drawPoints(Entity entity, PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		float getPointsViewportHeight(const CameraComponent& camera) const;
		float computePointsScale(const CameraComponent& camera, float viewportHeight) const;
		void destroyPoints(Entity entity, PointsComponent& points);

		bool drawLines(LineRendererComponent& lines, Transform& transform, Transform& camTransform, PipelineType pipType);
		void destroyLines(Entity entity, LineRendererComponent& lines);

		bool drawSky(SkyboxComponent& sky, PipelineType pipType, const Matrix4& viewProjection);
		void destroySky(Entity entity, SkyboxComponent& sky);

		void destroyLight(LightComponent& light);
		void destroyCamera(CameraComponent& camera, bool entityDestroyed);
		
		void updateSkyViewProjection(SkyboxComponent& sky, CameraComponent& camera);
		Matrix4 skyViewProjection(const SkyboxComponent& sky, const CameraComponent& camera) const;
		void updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent& camera, Transform& cameraTransform);
		void computeLightCameras(const LightComponent& light, const Transform& transform, const CameraComponent& camera, const Transform& cameraTransform, const Vector2& shadowCameraNearFar, LightCamera* cameras, unsigned int& numCascades);
		void updatePoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void updateTerrain(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainView& view);
		void updateCameraFrustumPlanes(const Matrix4 viewProjectionMatrix, Plane* frustumPlanes);
		void updateInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);

		void sortPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void sortInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);

	public:

		RenderSystem(Scene* scene);
		virtual ~RenderSystem();

		// Editor-only viewport debug override (see member declaration). Free to toggle:
		// meshes draw with no-culling variants of their pipelines, built once on first use.
		void setDisableFaceCulling(bool disableFaceCulling);

		void setDisableFog(bool disableFog);

		// Editor-only override (see member declaration). No reload needed: the
		// editor always renders through Engine::getFramebuffer(), so PIP_RTT is
		// already baked either way.
		void setDisableFixedResolution(bool disableFixedResolution);

		// the depth pass alpha-tests the submesh (MASK, or the legacy AUTO + textureShadow);
		// the editor derives export keys from it, so the rule lives in one place
		static bool usesAlphaMask(const Material& material, bool textureShadow);

		// Shader variants, derived the same way by loadMesh and by the editor collecting the
		// variants a build needs (from content, without loading anything on the GPU).
		// Sets the submesh's vertex attribute flags (hasTexCoord1, hasNormal, ...) from the
		// buffers it draws from and its own attributes, and hasNormalMap from its material.
		static void scanSubmeshAttributes(Submesh& submesh, const std::vector<const Buffer*>& buffers);
		// sceneMeshShader: the scene's default mesh shader (Scene::getDefaultMeshShader)
		static SubmeshShaderInputs getSubmeshShaderInputs(const MeshComponent& mesh, const Submesh& submesh, const TerrainComponent* terrain, const InstancedMeshComponent* instmesh, bool mirror,
		                                                  const std::string& sceneMeshShader = std::string());
		static SubmeshShaderVariant getSubmeshShaderVariant(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs);
		// The lighting mode a submesh draws with: the material's when the shader lists it,
		// else the shader's default (the first of its manifest's @lighting, NONE without one;
		// the built-in Lit and forks without a manifest: PIXEL, listing PIXEL, VERTEX, MIXED).
		// Never AUTO. manifest: the one the submesh draws with (SubmeshShaderInputs::manifest).
		static MaterialLighting resolveLighting(MaterialLighting requested, const ShaderManifest* manifest);
		// whether the shader lists the mode (AUTO: always)
		static bool supportsLighting(MaterialLighting mode, const ShaderManifest* manifest);
		// Whether the lit or unlit shader samples the material's textures
		static bool checkPBRTextures(const MeshShaderEnvironment& environment, const SubmeshShaderInputs& inputs);
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
		static std::string getSubmeshShaderReference(const MeshComponent& mesh, const Submesh& submesh, const std::string& sceneMeshShader);
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
		// The same for UI, points, lines and sky (AUTO: alpha blend and depth write, legacyCulling)
		static RenderState resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite, const ShaderManifest* manifest,
		                                               const std::vector<std::string>& shaderFeatures, bool legacyCulling);
		// with the component's stencil and colour write (phase 2; AUTO: the manifest's)
		static RenderState resolveComponentRenderState(MaterialBlendMode blendMode, MaterialDepthWrite depthWrite,
		                                               const MaterialStencil& stencil, int colorWrite, const ShaderManifest* manifest,
		                                               const std::vector<std::string>& shaderFeatures, bool legacyCulling);
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
		void needReloadLines();
		void needReloadMeshes();
		void needReloadPostProcess();
		void needUpdatePostProcessUniforms();
		void needReloadUIs();
		void needReloadSky();
		void prepareMeshForDataReload(Entity entity, MeshComponent& mesh);

		bool isAllLoaded() const;
		void getLoadCount(size_t& loaded, size_t& total) const;
		bool hasPendingMeshLods() const;
	
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
			// the view camera's passes cull no face (the Scene view's debug toggle)
			bool disableFaceCulling = false;
			// canvas an auto-resize camera covers in this view (0: as the camera is)
			float canvasWidth = 0.0f;
			float canvasHeight = 0.0f;
		};
		void drawView(uint32_t viewId, const ViewDesc& desc);
		// frees what a view created
		void releaseView(uint32_t viewId);
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
