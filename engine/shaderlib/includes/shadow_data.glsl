// The 3D shadow maps a fragment shader samples, as the engine fills them for the camera pass
// being drawn (RenderSystem::fillShadowData, docs/rendering/shadows.md "Shader interface"). The
// engine fills these blocks and binds these textures for any shader that declares them, by
// name, so a pass of its own can sample shadows too: define the parts it reads, include
// includes/shadow_sampling.glsl, and call its functions (an unread block or texture breaks the
// bind slots on GL, so declare only what is read):
//
//   SHADOW_DATA_POINT   the point-light block and atlas (lightShadow of a point light)
//   SHADOW_DATA_RAW     the raw-depth view of the atlas (PCSS; one texture and sampler more)
//   SHADOW_SAMPLING_HQ  (includes/shadow_sampling.glsl) the High filters, the Vogel disk and
//                       PCSS; without it a soft light filters with the Medium 5x5 grid
//
// includes/lights.glsl declares all of them under USE_SHADOWS (Lit and the shader library).
// A volumetric pass needs only the default part and sampleDirectionalShadow1(worldPos).
//
// Layout (vec4 members only: GL uploads a block typed after its first member). The slots are
// those of the projective atlas: a directional light's cascades are consecutive slots from its
// first, a spot light has one. Depths are window depths in [0, 1] (nearer the light smaller).

#ifndef SHADOW_DATA_GLSL
#define SHADOW_DATA_GLSL

uniform u_fs_shadow_data {
    // x first slot of the main directional light (-1: none), y its cascades,
    // z flags: 1 receiver-plane bias, 2 dithered cascade transition, 4 blended transition,
    //          8 PCSS allowed (the variant has the raw-depth view),
    // w the debug view (0 none, 1 cascades, 2 shadow factor, 3 texel density, 4 cache)
    vec4 header;
    vec4 fitCameraPosition;   // xyz the camera the cascades are fitted to (view depth is along its forward)
    vec4 fitCameraForward;    // xyz its forward (normalised)
    mat4 lightVP[MAX_SHADOW_ATLAS_SLOTS];   // world to the slot's clip space: the matrix it was rendered with
    vec4 atlasRect[MAX_SHADOW_ATLAS_SLOTS]; // xy offset, zw scale of the slot in atlas UV (bottom-left origin)
    // directional: x depth bias (world, along the light), y normal offset (world), z texel size
    // (world), w ortho depth span (world); spot: the same per metre of distance from the light
    // (scaled by it), w far plane
    vec4 bias[MAX_SHADOW_ATLAS_SLOTS];
    // directional: x near, y far view depth the cascade is selected for, z view depth where the
    // blend into the next starts; spot: x near, y far plane; w the slot's resolution (texels)
    vec4 split[MAX_SHADOW_ATLAS_SLOTS];
    // x filter (0 hard, 1 grid, 2 Vogel disk, 3 PCSS), y radius in texels (grid: 1 = 3x3,
    // 2 = 5x5; Vogel of a directional light; 0: from the source radius per fragment),
    // z tan(half angular diameter) of a directional light, source radius (m) of a spot light,
    // w strength (0 no shadow .. 1 full)
    vec4 filtering[MAX_SHADOW_ATLAS_SLOTS];
    // x view depth where the directional shadow starts fading, y where it ends (lit past it),
    // z the cache path of this frame (debug: 0 skipped, 1 restored, 2 static redrawn, 3 redrawn,
    // 4 uncached), w unused
    vec4 fade[MAX_SHADOW_ATLAS_SLOTS];
} shadowData;

uniform texture2D u_shadowAtlas;
uniform samplerShadow u_shadowAtlas_smp;   // LESS_EQUAL comparison, bilinear, white border (lit)

#ifdef SHADOW_DATA_POINT
uniform u_fs_point_shadow_data {
    // per cube face (point slot): xy offset, zw scale of the face tile in atlas UV
    vec4 faceRect[MAX_POINT_SHADOW_ATLAS_SLOTS];
    // per point light (first slot / 6): x tan(half the face field of view) (1 + guard band),
    // y, z window depth = y + z / distance along the face axis, w texel size per metre of distance
    vec4 projection[MAX_POINT_SHADOW_LIGHTS];
    // per point light: x depth bias per metre (world, towards the light), y normal offset per
    // metre, z unused, w face resolution (texels)
    vec4 bias[MAX_POINT_SHADOW_LIGHTS];
    // per point light: x filter (0 hard, 1 grid, 2 Vogel disk), y grid radius (1, 2), z source
    // radius (m), w strength
    vec4 filtering[MAX_POINT_SHADOW_LIGHTS];
    // per point light: x the cache path of this frame (debug, as fade.z), yzw unused
    vec4 state[MAX_POINT_SHADOW_LIGHTS];
} pointShadowData;

uniform texture2D u_shadowPointAtlas;      // sampled with u_shadowAtlas_smp
#endif

#ifdef SHADOW_DATA_RAW
// the same image as u_shadowAtlas through a second, unfiltered view: the depths themselves
// @image_sample_type u_shadowAtlasRaw unfilterable_float
// @sampler_type u_shadowAtlasRaw_smp nonfiltering
uniform texture2D u_shadowAtlasRaw;
uniform sampler u_shadowAtlasRaw_smp;
#endif

#endif // SHADOW_DATA_GLSL
