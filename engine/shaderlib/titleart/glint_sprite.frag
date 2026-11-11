#version 450
precision highp float;
precision highp int;

// The Low tier's sprite glints (glint_sprite.vert): a star of `rays` arms at `angle` (memory
// rows), each falling to 2 % of its start at the ray length (Pachislop's STREAK_TAIL), a
// hairline across, over a round bloom of the point's light; written as added light (alpha 0,
// additive blending into the glint output).

in vec2 v_local;
flat in vec3 v_light;
out vec4 frag_color;

uniform u_fs_glintSprite {
    vec4 shape;     // x: rays, y: angle (rad, memory rows), z: ray length (output texels), w: hairline width (output texels)
    vec4 bloom;     // x: the bloom's sigma (output texels), y: its gain, z: the rays' start gain
} gs;

void main() {
    int rays = int(gs.shape.x + 0.5);
    float fall = log(50.0) / max(gs.shape.z, 1e-3);
    float s = 0.0;
    for (int r = 0; r < 8; r++) {
        if (r >= rays) break;
        float a = gs.shape.y + 6.2831853 * float(r) / float(rays);
        vec2 d = vec2(cos(a), sin(a));
        float along = dot(v_local, d);
        if (along < 0.0) continue;
        float across = abs(v_local.x * d.y - v_local.y * d.x) / max(gs.shape.w, 1e-3);
        s += exp(-along * fall) * exp(-across * across);
    }
    float b = exp(-dot(v_local, v_local) / (2.0 * gs.bloom.x * gs.bloom.x));
    frag_color = vec4(v_light * (s * gs.bloom.z + b * gs.bloom.y), 0.0);
}
