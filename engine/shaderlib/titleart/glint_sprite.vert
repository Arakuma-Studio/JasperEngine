#version 450
precision highp float;
precision highp int;

// The Low tier's sprite glints (docs/specs/S86-title-3d.md R32, §6.6;
// ShaderType::TITLE_GLINT_SPRITE): one instanced draw of a quad per texel of the detected points
// (the glint's point detector on the stars' mip), with no readback. An instance keeps its quad
// only where its texel is a local maximum over its 8 neighbours and above the floor; the others
// collapse outside the clip volume. The quad sits on the glint output lattice (memory rows) and
// carries the point's light to the star fragment stage (glint_sprite.frag).

uniform texture2D u_gsPeaks;
uniform sampler u_gsPeaks_smp;

uniform u_vs_glintSprite {
    vec4 grid;      // xy: the points texture's size (texels)
    vec4 map;       // xy: output texels per point texel, zw: the output texel of point (0, 0)'s centre
    vec4 frame;     // xy: the output's size (texels), z: the sprite's half size (output texels), w: 1 = origin top left
    vec4 gain;      // rgb: tint x intensity, a: the floor a point's light must pass
} gs;

out vec2 v_local;
flat out vec3 v_light;

vec3 gsPeak(ivec2 t, ivec2 size) {
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) return vec3(0.0);
    return texelFetch(sampler2D(u_gsPeaks, u_gsPeaks_smp), t, 0).rgb;
}

void main() {
    ivec2 size = ivec2(gs.grid.xy + 0.5);
    int id = gl_InstanceIndex;
    ivec2 t = ivec2(id - (id / size.x) * size.x, id / size.x);
    vec3 p = gsPeak(t, size);
    float l = max(p.r, max(p.g, p.b));
    bool keep = l > gs.gain.a;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            vec3 n = gsPeak(t + ivec2(dx, dy), size);
            float ln = max(n.r, max(n.g, n.b));
            // one sprite per plateau: ties go to the lower index
            if (ln > l || (ln == l && (dy < 0 || (dy == 0 && dx < 0)))) keep = false;
        }
    }
    int v = gl_VertexIndex;
    vec2 corner = vec2((v == 1 || v == 2 || v == 4) ? 1.0 : -1.0, (v == 2 || v == 4 || v == 5) ? 1.0 : -1.0);
    v_local = corner * gs.frame.z;
    v_light = p * gs.gain.rgb;
    if (!keep) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec2 centre = gs.map.zw + vec2(t) * gs.map.xy;
    vec2 at = (centre + v_local) / gs.frame.xy;
    gl_Position = vec4(at.x * 2.0 - 1.0, gs.frame.w > 0.5 ? 1.0 - at.y * 2.0 : at.y * 2.0 - 1.0, 0.0, 1.0);
}
