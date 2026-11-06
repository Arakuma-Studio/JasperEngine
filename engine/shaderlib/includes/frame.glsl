// Engine frame block: per-camera-pass data, filled by name for any shader that declares it
// (frame_t in RenderSystem.h). Define FRAME_VS (in a .vert) or FRAME_FS (in a .frag) before
// including, only where the stage reads it: GL drivers strip an unused block, which breaks
// the bind slots. The members up to ambient are a contract with the library shaders; the
// engine may only append members after it, identically in both blocks (today the shadow
// PCF radius lights.glsl reads and the fog fog.glsl reads when this block is declared
// first; the built-in Lit has its own smaller camera block, includes/lit_frame.glsl), but for
// the post-processing colour's vectors, which only the fragment block has (USE_POST_COLOR), and
// the pixel-snap grid after them, which only a pixel-snapped vertex stage declares (PIXEL_SNAP,
// includes/pixel_snap.glsl; S41): a program without either uploads the bytes it always did.

#if defined(FRAME_VS) && !defined(FRAME_VS_DECLARED)
#define FRAME_VS_DECLARED
uniform u_vs_frame {
    mat4 viewMatrix;
    mat4 projectionMatrix;
    mat4 viewProjectionMatrix;
    vec4 cameraPosition;     // xyz world position, w = 1 orthographic / 0 perspective
    vec4 cameraDirection;    // xyz world forward
    vec4 time;               // x seconds since startup, y sin(x), z cos(x), w frame delta
    vec4 resolution;         // xy render target size in pixels, zw 1/size
    vec4 mainLightDirection; // xyz towards the brightest directional light, w intensity (0 = none)
    vec4 mainLightColor;     // rgb linear
    vec4 ambient;            // rgb scene ambient (global illumination colour * intensity)
    // engine-internal, not part of the library contract
    vec4 lightingInfo;           // x 3D shadow PCF tap radius (Scene shadowQuality)
    vec4 fogColor_type;          // rgb fog colour, w type: 0 linear, 1 exponential, 2 exponential squared
    vec4 fogDensity_start_end;   // x density, z linear start, w linear end
    #ifdef PIXEL_SNAP
        // the fragment block's post-processing colour (frame_t::postColor), unread here: the
        // pixel-snap grid comes after it
        vec4 frameFragmentOnly[4];
        // the pixel-snap grid of the pass (S41, includes/pixel_snap.glsl; zero without a Pixel
        // Perfect Camera): xy T in base pixels, z flipY, w k; xy the smoothing offset, z 1 on-grid view
        vec4 pixelSnap;
        vec4 pixelSnapOffset;
    #endif
} frame;
#endif

#if defined(FRAME_FS) && !defined(FRAME_FS_DECLARED)
#define FRAME_FS_DECLARED
uniform u_fs_frame {
    mat4 viewMatrix;
    mat4 projectionMatrix;
    mat4 viewProjectionMatrix;
    vec4 cameraPosition;     // xyz world position, w = 1 orthographic / 0 perspective
    vec4 cameraDirection;    // xyz world forward
    vec4 time;               // x seconds since startup, y sin(x), z cos(x), w frame delta
    vec4 resolution;         // xy render target size in pixels, zw 1/size
    vec4 mainLightDirection; // xyz towards the brightest directional light, w intensity (0 = none)
    vec4 mainLightColor;     // rgb linear
    vec4 ambient;            // rgb scene ambient (global illumination colour * intensity)
    // engine-internal, not part of the library contract
    vec4 lightingInfo;           // x 3D shadow PCF tap radius (Scene shadowQuality)
    vec4 fogColor_type;          // rgb fog colour, w type: 0 linear, 1 exponential, 2 exponential squared
    vec4 fogDensity_start_end;   // x density, z linear start, w linear end
    #ifdef USE_POST_COLOR
        // the post-processing colour's parameters (includes/post_color.glsl, frame_t::postColor);
        // the fragment stage only, so its block is larger than the vertex stage's
        vec4 postColor_vignetteScaleOffset;
        vec4 postColor_vignetteColor;
        vec4 postColor_params;
        vec4 postColor_shaper;
    #endif
} frame;
#endif
