// QuadBatch's vertex stage (gpu/QuadBatch.h), ported from Pachislop src/generators/common.ts
// (QUAD_VERT, drawInstancedQuads) by SafEight, used with the author's permission.
//
// The block u_vs_titleart mirrors p[0..5] of the fragment's (generators/TitleGen.h genPass):
// p[0].xy the target size, p[4] and p[5].xy the drawing space -> target px affine, p[5].z min_px.
// Quads: 4 RGBA32F texels a record (centre and size; rotation and three values of the program's
// own, passed on as v_more; premultiplied colour; extra), 6 vertices a quad. Meshes: 2 texels a
// vertex (x, y, u, v; extra). A program that moves its quads here (Particles' closed-form time,
// particles_motion.glsl) sees its whole block, p[6..15] too, and defines tfx_quadMotion.
//
// @image_sample_type u_taVertexData unfilterable_float
// @sampler_type u_taVertexData_smp nonfiltering

#ifndef TITLEART_GEN_TFX_QUADS_GLSL
#define TITLEART_GEN_TFX_QUADS_GLSL

#if TA_PROGRAM == TA_GEN_QUADS_PARTICLES
    #define TFX_QUAD_MOTION 1
uniform u_vs_titleart {
    vec4 p[16];
} tv;
#else
uniform u_vs_titleart {
    vec4 p[6];
} tv;
#endif

uniform texture2D u_taVertexData;
uniform sampler u_taVertexData_smp;

out vec2 v_quad;
out vec4 v_color;
out vec4 v_extra;
out vec2 v_sizePx;
out vec3 v_more;

const int TFX_DATA_WIDTH = 1024;

vec4 tfx_record(int texel) {
    return texelFetch(sampler2D(u_taVertexData, u_taVertexData_smp), ivec2(texel % TFX_DATA_WIDTH, texel / TFX_DATA_WIDTH), 0);
}

#ifdef TFX_QUAD_MOTION
    #include "titleart/gen/particles_motion.glsl"
#endif

// a drawing-space point to clip space: only GL stores destination row 0 at clip y = -1
vec4 tfx_toClip(vec2 space) {
    vec2 t = vec2(tv.p[4].x * space.x + tv.p[4].y * space.y, tv.p[4].z * space.x + tv.p[4].w * space.y) + tv.p[5].xy;
    vec2 ndc = t / tv.p[0].xy * 2.0 - 1.0;
    #ifndef IS_GLSL
        ndc.y = -ndc.y;
    #endif
    vec4 position = vec4(ndc, 0.0, 1.0);
    #ifdef IS_VULKAN
        position.z = (position.z + position.w) * 0.5;
    #endif
    return position;
}

void main() {
#if TA_GEN_VERTICES == 1
    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex - quad * 6;
    // two triangles: (-1,-1) (1,-1) (1,1), (-1,-1) (1,1) (-1,1)
    vec2 c = vec2(corner == 1 || corner == 2 || corner == 4 ? 1.0 : -1.0, corner == 2 || corner == 4 || corner == 5 ? 1.0 : -1.0);
    vec4 cs = tfx_record(quad * 4);
    vec4 turn = tfx_record(quad * 4 + 1);
    float rot = turn.x;
    vec4 color = tfx_record(quad * 4 + 2);
    vec4 extra = tfx_record(quad * 4 + 3);
    #ifdef TFX_QUAD_MOTION
        tfx_quadMotion(cs, rot, color, turn.yzw);
    #endif
    // target px per drawing-space px (Pachislop's layerPxScale)
    float k = length(vec2(tv.p[4].x, tv.p[4].z));
    vec2 px = abs(cs.zw) * k;
    if (k <= 0.0 || px.x * px.y <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_quad = vec2(0.0);
        v_color = vec4(0.0);
        v_extra = vec4(0.0);
        v_sizePx = vec2(1.0);
        v_more = vec3(0.0);
        return;
    }
    // sub-min_px quads grow to min_px with their colour scaled by the area ratio (energy kept)
    vec2 grown = max(px, vec2(tv.p[5].z));
    vec2 hs = 0.5 * grown / k;
    vec2 l = c * hs;
    float cr = cos(rot), sr = sin(rot);
    gl_Position = tfx_toClip(cs.xy + vec2(cr * l.x - sr * l.y, sr * l.x + cr * l.y));
    v_quad = c;
    v_color = color * (px.x * px.y / (grown.x * grown.y));
    v_extra = extra;
    v_sizePx = grown;
    v_more = turn.yzw;
#else
    vec4 pos = tfx_record(gl_VertexIndex * 2);
    v_extra = tfx_record(gl_VertexIndex * 2 + 1);
    gl_Position = tfx_toClip(pos.xy);
    v_quad = pos.zw;
    v_color = vec4(1.0);
    v_sizePx = vec2(length(vec2(tv.p[4].x, tv.p[4].z)));
    v_more = vec3(0.0);
#endif
}

#endif // TITLEART_GEN_TFX_QUADS_GLSL
