#version 450

out vec4 frag_color;

in vec2 v_segA;
in vec2 v_segB;
in float v_angle;
in vec4 v_lightPosRange;

#include "includes/depth_util.glsl"

void main(){
    // The map X axis is angle, but a straight segment does not sweep linearly in
    // angle, so any linearly-interpolated position/distance bows the stored
    // profile into arcs between the vertices. Instead intersect this fragment's
    // polar ray (the angle interpolates exactly, since x IS the angle) with the
    // segment line analytically: dist = true occluder distance at this angle.
    vec2 lightPos = v_lightPosRange.xy;
    vec2 rayDir = vec2(cos(v_angle), sin(v_angle));
    vec2 segDir = v_segB - v_segA;
    vec2 rel = v_segA - lightPos;

    float denom = rayDir.x * segDir.y - rayDir.y * segDir.x;
    float s = rel.x * segDir.y - rel.y * segDir.x;

    float dist;
    if (abs(denom) > 1e-6 && s * denom > 0.0){
        dist = s / denom;
    }else{
        // ray nearly parallel to the segment (edge-on, negligible angular span):
        // fall back to the nearest endpoint distance
        dist = min(length(v_segA - lightPos), length(v_segB - lightPos));
    }

    // capped below 1.0 because encodeDepth cannot represent exactly 1.0 (the
    // clear color decodes > 1 = unoccluded)
    dist = clamp(dist / v_lightPosRange.w, 0.0, 0.999);

    gl_FragDepth = dist;
    frag_color = encodeDepth(dist);
}
