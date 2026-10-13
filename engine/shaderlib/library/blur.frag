#version 450

// @shader postprocess
// @name "Blur"
// @category "Post-processing"
// @description "A soft blur in one pass: 13 taps on two rings, averaged in linear light. For a wide or full-screen blur the engine's post-processing Blur (a quarter-resolution chain) is much cheaper; this one is for a chain of your own (with a pass scale below 1 it widens for free)."
// @property radius range(0, 16) 3 "Radius (source pixels)"
// @property amount range(0, 1) 1 "Amount (0: the source as it is)"

// The centre, 8 taps on a ring of the radius and 4 on a ring of half of it (rotated between
// the others), each decoded with the gamma-2 approximation (c * c) so the average is in
// linear light, then encoded back. Display colour in and out.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_postParams {
    vec4 resolution; // engine-written: xy output size, zw 1 / size
    float radius;
    float amount;
} postParams;

mediump vec3 linearTap(highp vec2 uv){
    mediump vec3 c = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb;
    return c * c;
}

void main(){
    highp vec2 texel = 1.0 / vec2(textureSize(sampler2D(u_sceneColorTexture, u_sceneColor_smp), 0));
    highp vec2 r = texel * postParams.radius;
    mediump vec3 centre = linearTap(v_texcoord);
    mediump vec3 sum = centre * 2.0;
    // the outer ring (weight 1 each)
    sum += linearTap(v_texcoord + vec2(r.x, 0.0)) + linearTap(v_texcoord - vec2(r.x, 0.0));
    sum += linearTap(v_texcoord + vec2(0.0, r.y)) + linearTap(v_texcoord - vec2(0.0, r.y));
    highp vec2 d = r * 0.7071;
    sum += linearTap(v_texcoord + d) + linearTap(v_texcoord - d);
    sum += linearTap(v_texcoord + vec2(d.x, -d.y)) + linearTap(v_texcoord + vec2(-d.x, d.y));
    // the inner ring, on the diagonals of the outer one's axes (weight 2 each)
    highp vec2 h = r * 0.5;
    sum += (linearTap(v_texcoord + vec2(h.x, h.y) * 0.7071) + linearTap(v_texcoord - vec2(h.x, h.y) * 0.7071)) * 2.0;
    sum += (linearTap(v_texcoord + vec2(h.x, -h.y) * 0.7071) + linearTap(v_texcoord + vec2(-h.x, h.y) * 0.7071)) * 2.0;
    mediump vec3 blurred = sum / 18.0;
    frag_color = vec4(sqrt(mix(centre, blurred, postParams.amount)), 1.0);
}
