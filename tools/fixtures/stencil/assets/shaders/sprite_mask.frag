#version 450
// @shader mesh
// @name "Sprite mask (fixture)"
// @category "2D"
// @description "Writes stencil 4 where the sprite's texture alpha passes the cutoff; draws no colour. A stand-in for the library's sprite_mask, taking its stencil state from the manifest."
// @require uv1
// @state blend=alpha zwrite=off cull=off shadows=off
// @state stencil=always ref=4 pass=replace colorwrite=none
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff"

#include "includes/output.glsl"
#include "includes/base_color_fs.glsl"

uniform u_fs_customParams {
    float alphaCutoff;
} customParams;

in vec2 v_uv1;
out vec4 g_finalColor;

void main(){
    #ifdef HAS_UV_SET1
        vec4 texel = sampleBaseColorTexture(v_uv1);
    #else
        // no texture: the whole quad (v_uv1 is still read, the vertex stage writes it)
        vec4 texel = vec4(1.0, 1.0, 1.0, 1.0 + 0.0 * v_uv1.x);
    #endif
    // the mask's shape: transparent texels write no stencil
    if (texel.a < customParams.alphaCutoff){
        discard;
    }
    g_finalColor = finalizeColor(texel);
}
