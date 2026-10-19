#version 450

// HTML UI effects, scatter form (phase 6): draw(0, 6 * W * H, 1) of a placement state (W x H
// texels, one per agent): vertex v belongs to texel v / 6, whose value is the quad's centre in
// target texels (xy) and its half extents (zw; not positive: no quad). The fragment stage gets the
// placement, the texel and its offset from the centre; the pass blends its quads additively.
// The placement image holds texel positions: it is read with texelFetch, never filtered.
// @image_sample_type u_scatterPlace unfilterable_float
// @sampler_type u_scatterPlace_smp nonfiltering
uniform u_vs_effectScatter {
    vec4 target;    // xy target size texels, zw 1/size
    vec4 place;     // xy placement image size texels
} sc;

uniform texture2D u_scatterPlace;
uniform sampler u_scatterPlace_smp;

// varyings in the same order in both stages (jfx_state.glsl): their locations follow declaration order
flat out vec4 v_scatterPlace;
flat out vec3 v_scatterCell;      // the placement texel, and its index

void main() {
    int index = gl_VertexIndex / 6;
    int corner = gl_VertexIndex - index * 6;
    int width = max(int(sc.place.x), 1);
    ivec2 cell = ivec2(index - (index / width) * width, index / width);
    vec4 p = texelFetch(sampler2D(u_scatterPlace, u_scatterPlace_smp), cell, 0);
    // two triangles: (-,-) (+,-) (-,+) and (-,+) (+,-) (+,+)
    vec2 k = vec2((corner == 1 || corner == 4 || corner == 5) ? 1.0 : -1.0,
                        (corner == 2 || corner == 3 || corner == 5) ? 1.0 : -1.0);
    bool live = p.z > 0.0 && p.w > 0.0;
    vec2 pos = p.xy + k * p.zw;
    vec2 ndc = pos * sc.target.zw * 2.0 - 1.0;
    #ifndef IS_GLSL
        // only GL stores memory row 0 at clip y = -1
        ndc.y = -ndc.y;
    #endif
    // a skipped agent: a degenerate triangle outside the clip volume
    gl_Position = live ? vec4(ndc, 0.0, 1.0) : vec4(-2.0, -2.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
    v_scatterPlace = p;
    v_scatterCell = vec3(vec2(cell), float(index));
}
