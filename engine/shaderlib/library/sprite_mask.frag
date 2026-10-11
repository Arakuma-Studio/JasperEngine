#version 450

// @shader mesh
// @name "Sprite mask"
// @category "2D"
// @description "Unity-style Sprite Mask: writes stencil 16 (bits 0xF0) in the shape of the sprite's own image (its texture rect or atlas frame; texels below the alpha cutoff write nothing), and no colour or depth. Put it on a Sprite (Sprite mask preset); sprites with the Visible inside mask preset (stencil equal 16) show only inside it, Visible outside mask (not equal) only outside. In a 3D or distance-sorted camera the mask (opaque queue) draws before the blended sprites; in a 2D camera that draws in hierarchy order, place the mask above the sprites it masks. Turn colour writes on to see it."
// @require uv1
// @state blend=opaque zwrite=off cull=off shadows=off colorwrite=none
// @state stencil=always ref=16 writemask=0xf0 pass=replace
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff"

// The image is the material's base colour texture (a Sprite's, Tilemap's or MeshPolygon's,
// includes/base_color_fs.glsl), at v_uv1, which the shared vertex path already maps into
// the sprite's texture rect and the instance's rect, so atlas frames and animations mask
// with their current frame. A mesh without UVs masks its whole shape.

#include "includes/output.glsl"
#include "includes/base_color_fs.glsl"

uniform u_fs_customParams {
    float alphaCutoff;
} customParams;

// as the vertex stage declares it (includes/surface_vs.glsl): GLSL ES 3.00 links only matching
// qualifiers, and a sprite always has a texture rect
#ifdef HAS_TEXTURERECT
    centroid in vec2 v_uv1;
#else
    in vec2 v_uv1;
#endif

out vec4 g_finalColor;

void main(){
    #ifdef HAS_UV_SET1
        vec4 texel = sampleBaseColorTexture(v_uv1);
    #else
        // the whole shape (v_uv1 is still read: the vertex stage writes it)
        vec4 texel = vec4(1.0, 1.0, 1.0, 1.0 + 0.0 * v_uv1.x);
    #endif
    if (texel.a < customParams.alphaCutoff){
        discard;
    }
    g_finalColor = finalizeColor(vec4(sRGBToLinear(texel.rgb), texel.a));
}
