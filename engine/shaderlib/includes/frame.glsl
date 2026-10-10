// Engine frame block: per-camera-pass data, filled by name for any shader that declares it
// (frame_t in RenderSystem.h). Define FRAME_VS (in a .vert) or FRAME_FS (in a .frag) before
// including, only where the stage reads it: GL drivers strip an unused block, which breaks
// the bind slots. The members up to ambient are a contract with the library shaders; the
// engine may only append members after it, identically in both blocks.

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
    vec4 environment;            // rgb environment colour (linear), w environment rotation (radians)
    vec4 viewportInfo;           // xy 1 / viewport size, z SSAO debug view, w SSAO Y flip
    vec4 fogColor_type;          // rgb fog colour, w type: 0 linear, 1 exponential, 2 exponential squared
    vec4 fogDensity_start_end;   // x density, z linear start, w linear end
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
    vec4 environment;            // rgb environment colour (linear), w environment rotation (radians)
    vec4 viewportInfo;           // xy 1 / viewport size, z SSAO debug view, w SSAO Y flip
    vec4 fogColor_type;          // rgb fog colour, w type: 0 linear, 1 exponential, 2 exponential squared
    vec4 fogDensity_start_end;   // x density, z linear start, w linear end
} frame;
#endif
