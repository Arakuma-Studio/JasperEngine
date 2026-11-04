#version 450

// @shader postprocess
// @name "Posterize"
// @category "Post-processing"
// @description "Fewer colours: each channel rounded to a few levels, with an optional ordered dither that trades the bands for a fine pattern; for an exact palette use Post-processing ▸ Palette Quantize."
// @property levels range(2, 32) 5 "Levels per channel"
// @property dither range(0, 1) 0.5 "Dither"
// @property saturation range(0, 2) 1 "Saturation"

// Rounded on the display-encoded colour (perceptually even steps); the 4x4 Bayer pattern
// moves each pixel's rounding threshold by up to one level. Display colour in and out.

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
    float levels;
    float dither;
    float saturation;
} postParams;

const mediump float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                          3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main(){
    mediump vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    mediump float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = clamp(mix(vec3(luma), color, postParams.saturation), 0.0, 1.0);

    mediump float steps = max(postParams.levels, 2.0) - 1.0;
    ivec2 pixel = ivec2(mod(gl_FragCoord.xy, 4.0));
    mediump float threshold = (bayer[pixel.y * 4 + pixel.x] + 0.5) / 16.0 - 0.5;
    color = floor(color * steps + 0.5 + threshold * postParams.dither) / steps;

    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
