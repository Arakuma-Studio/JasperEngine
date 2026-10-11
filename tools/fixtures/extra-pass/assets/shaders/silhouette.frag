#version 450
// @shader mesh
// @name "Fixture silhouette"
// @description "The object's base colour texture as a flat-coloured shape (its alpha), shifted by offset: a drop shadow behind a sprite (BEFORE) or a tint over it (AFTER)."
// @require uv1
// @state blend=alpha zwrite=off shadows=off
// @property shadowColor color (0, 0, 0, 0.6) "Colour"
// @property offset vector3 (0, 0, 0) "Offset (object units)"

// the material's own base colour texture (the sprite's image), bound by the engine by name
uniform texture2D u_baseColorTexture;
uniform sampler u_baseColor_smp;

uniform u_fs_customParams {
    vec4 shadowColor;
} customParams;

// centroid on sprites, as the vertex stage writes it (GLSL ES links matching qualifiers only)
#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    float alpha = texture(sampler2D(u_baseColorTexture, u_baseColor_smp), v_uv1).a;
    if (alpha < 0.5){
        discard;
    }
    g_finalColor = finalizeStraightColor(customParams.shadowColor);
}
