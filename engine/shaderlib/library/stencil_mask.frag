#version 450

// @shader mesh
// @name "Stencil mask"
// @category "Outline and stencil"
// @description "Writes stencil 16 (bits 0xF0) where it draws, and no colour or depth: a portal, window or hole. Objects with a stencil test show only inside it (Visible inside mask preset: equal 16) or only outside it (Visible outside mask: not equal 16). Masks must draw before their readers: in the opaque queue they precede blended readers; give opaque readers a higher sort priority or the mask a lower one (the Stencil mask preset sets -1). Several masks: refs 16, 32 ... 240 under read/write mask 0xF0. Alpha clip cuts the mask's shape from a texture's alpha. Turn colour writes on to see it (Colour)."
// @require uv1 feature=ALPHA_CLIP
// @state blend=opaque zwrite=off shadows=off colorwrite=none
// @state stencil=always ref=16 writemask=0xf0 pass=replace
// @property color color (1, 0, 1, 1) "Colour (drawn only with colour writes on)"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff" feature=ALPHA_CLIP
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)" feature=ALPHA_CLIP
// @texture u_maskTexture white "Mask shape (alpha)" feature=ALPHA_CLIP
// @feature ALPHA_CLIP "Alpha clip shape"

// The depth test stays on (a mask behind a wall masks nothing there) and the depth write
// off, so the mask hides nothing and is left out of the depth pre-pass. The colour exists
// to debug a mask's placement: it reaches the screen only if the material's colour writes
// are turned on.

uniform u_fs_customParams {
    vec4 color;
    #ifdef ALPHA_CLIP
        vec4 tiling;
        float alphaCutoff;
    #endif
} customParams;

#ifdef ALPHA_CLIP
    uniform texture2D u_maskTexture;
    uniform sampler u_mask_smp;

    in vec2 v_uv1;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    #ifdef ALPHA_CLIP
        vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
        if (texture(sampler2D(u_maskTexture, u_mask_smp), uv).a < customParams.alphaCutoff){
            discard;
        }
    #endif
    g_finalColor = finalizeColor(customParams.color);
}
