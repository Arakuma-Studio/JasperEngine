#version 450

// @shader postprocess
// @name "Pixelate"
// @category "Post-processing"
// @description "Big square pixels: the picture in blocks of a few output pixels, each the colour at its centre. Pair it with a pass scale below 1 to make it cheaper still."
// @property pixelSize range(1, 64) 8 "Block size (pixels)"
// @property roundCorners range(0, 1) 0 "Rounded blocks (dark gaps)"

// Each block reads the source once at its centre (nearest-looking even with bilinear
// filtering, since the tap lands on the block centre); the optional gap darkens the block's
// corners into rounded "LED" dots. Display colour in and out.

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
    float roundCorners;
} postParams;

void main(){
    highp vec2 block = max(postParams.pixelSize, 1.0) * postParams.resolution.zw;
    highp vec2 cell = v_texcoord / block;
    highp vec2 uv = (floor(cell) + 0.5) * block;
    mediump vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb;

    // the distance from the block's centre, 0 at the centre and 1 at its edge midpoints
    mediump vec2 d = abs(fract(cell) - 0.5) * 2.0;
    mediump float corner = smoothstep(1.0, 0.75, length(d) * 0.85);
    color *= mix(1.0, corner, postParams.roundCorners);

    frag_color = vec4(color, 1.0);
}
