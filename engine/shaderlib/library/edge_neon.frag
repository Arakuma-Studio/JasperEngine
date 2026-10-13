#version 450

// @shader postprocess
// @name "Edge Neon"
// @category "Post-processing"
// @description "Glowing outlines: the picture's edges (Sobel on brightness) lit in a neon colour that can cycle through the hues, over a darkened scene."
// @property edgeColor hdrcolor (0.1, 1, 0.9, 1) "Edge colour"
// @property threshold range(0, 1) 0.1 "Edge threshold"
// @property intensity range(0, 4) 1.5 "Edge brightness"
// @property darken range(0, 1) 0.7 "Darken the scene"
// @property hueSpeed float 0 "Hue cycle (turns per second)"

// 8 taps around the pixel (one source texel apart) give the Sobel gradients of the luma; the
// edge strength is shaped by the threshold. The edge colour is linear (a colour property),
// added in linear light over the darkened scene and encoded back. Display colour in and out.

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
    float time;      // engine-written seconds
    vec4 edgeColor;
    float threshold;
    float intensity;
    float darken;
    float hueSpeed;
} postParams;

mediump float lumaAt(highp vec2 uv){
    return dot(texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb, vec3(0.2126, 0.7152, 0.0722));
}

// rotates a colour's hue by a turn fraction (about the grey axis)
mediump vec3 hueTurn(mediump vec3 color, mediump float turns){
    mediump float angle = 6.2831853 * turns;
    const mediump vec3 k = vec3(0.57735);
    mediump float c = cos(angle);
    return color * c + cross(k, color) * sin(angle) + k * dot(k, color) * (1.0 - c);
}

void main(){
    highp vec2 texel = 1.0 / vec2(textureSize(sampler2D(u_sceneColorTexture, u_sceneColor_smp), 0));
    mediump float tl = lumaAt(v_texcoord + vec2(-texel.x, texel.y));
    mediump float t  = lumaAt(v_texcoord + vec2(0.0, texel.y));
    mediump float tr = lumaAt(v_texcoord + texel);
    mediump float l  = lumaAt(v_texcoord - vec2(texel.x, 0.0));
    mediump float r  = lumaAt(v_texcoord + vec2(texel.x, 0.0));
    mediump float bl = lumaAt(v_texcoord - texel);
    mediump float b  = lumaAt(v_texcoord - vec2(0.0, texel.y));
    mediump float br = lumaAt(v_texcoord + vec2(texel.x, -texel.y));
    mediump float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    mediump float gy = (tl + 2.0 * t + tr) - (bl + 2.0 * b + br);
    mediump float edge = smoothstep(postParams.threshold, postParams.threshold * 2.0 + 0.1, length(vec2(gx, gy)));

    mediump vec3 scene = sRGBToLinear(texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb);
    mediump vec3 neon = max(hueTurn(postParams.edgeColor.rgb, fract(postParams.time * postParams.hueSpeed)), 0.0);
    mediump vec3 color = scene * (1.0 - postParams.darken) + neon * edge * postParams.intensity;
    frag_color = vec4(linearTosRGB(color), 1.0);
}
