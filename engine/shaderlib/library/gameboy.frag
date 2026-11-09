#version 450

// @shader postprocess
// @name "Game Boy"
// @category "Post-processing"
// @description "A four-shade handheld screen: big pixels, the brightness mapped to four greens with an ordered dither between them; for an exact palette use Post-processing ▸ Palette Quantize."
// @property pixelSize range(1, 16) 4 "Pixel size (output pixels)"
// @property dither range(0, 1) 0.6 "Dither"
// @property contrast range(0.5, 2) 1.1 "Contrast"
// @property shade0 color (0.06, 0.22, 0.06, 1) "Darkest"
// @property shade1 color (0.19, 0.38, 0.19, 1) "Dark"
// @property shade2 color (0.55, 0.67, 0.06, 1) "Light"
// @property shade3 color (0.61, 0.74, 0.06, 1) "Lightest"

// The source read at the centre of each pixel block, its display luma (with contrast) rounded
// to one of four shades, the 4x4 Bayer pattern moving the rounding per block. The shades are
// colour properties (linear), encoded for the display. Display colour in and out.

#include "includes/srgb.glsl"

// the defaults Vulkan GLSL has anyway, stated: glslang warns about implicit ones in a shader
// using precision qualifiers (the mediump below reaches GLES as RelaxedPrecision)
precision highp float;
precision highp int;

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_postParams {
    vec4 resolution; // engine-written: xy output size, zw 1 / size
    float pixelSize;
    float dither;
    float contrast;
    vec4 shade0;
    vec4 shade1;
    vec4 shade2;
    vec4 shade3;
} postParams;

const mediump float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                          3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main(){
    highp vec2 blockSize = max(postParams.pixelSize, 1.0) * postParams.resolution.zw;
    highp vec2 cell = floor(v_texcoord / blockSize);
    mediump vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), (cell + 0.5) * blockSize).rgb;
    mediump float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    luma = clamp((luma - 0.5) * postParams.contrast + 0.5, 0.0, 1.0);

    ivec2 pixel = ivec2(mod(cell, 4.0));
    mediump float threshold = (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0 - 0.5;
    mediump float shade = clamp(floor(luma * 3.0 + 0.5 + threshold * postParams.dither), 0.0, 3.0);

    mediump vec3 linear = shade < 0.5 ? postParams.shade0.rgb : (shade < 1.5 ? postParams.shade1.rgb : (shade < 2.5 ? postParams.shade2.rgb : postParams.shade3.rgb));
    frag_color = vec4(linearTosRGB(linear), 1.0);
}
