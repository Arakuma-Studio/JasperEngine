// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LOD cross-fade (USE_LOD_FADE, MESH 'Lfd'; spec S54 R6). The two detail levels of a
// fading mesh draw with one screen-space 8x8 Bayer rank and complementary shares of one fade:
// the level faded from (a negative fade, -k/64) keeps the pixels ranked below k, the level faded
// to (positive, (64-k)/64) the others, so each pixel keeps exactly one level, opaque and depth
// writing (RenderSystem planMeshLod, render/LodFade.h). Only fading draws use this variant: every
// other draw keeps its pipeline without a discard, and early depth.
//
// The fade comes from the vertex stage (mesh.vert): the object block's normalMatrix[0].w for a
// single draw, or the instance's first matrix column's w for an Instanced Mesh's fading copy;
// both lanes are 0 in what any other reader takes (a direction, an affine matrix).

#ifdef USE_LOD_FADE

#ifdef LOD_FADE_VS
    flat out float v_lodFade;
#else
    flat in float v_lodFade;

    // the rank 0..63 of a pixel in the 8x8 Bayer matrix: the bits of x^y and y, interleaved and reversed
    int lodFadeRank(ivec2 pixel){
        ivec2 p = pixel & ivec2(7);
        int v = p.x ^ p.y;
        return ((v & 1) << 5) | ((p.y & 1) << 4) | ((v & 2) << 2) |
               ((p.y & 2) << 1) | ((v & 4) >> 1) | ((p.y & 4) >> 2);
    }

    bool lodFadeKeeps(){
        float fade = v_lodFade;
        int k = int(abs(fade) * 64.0 + 0.5);
        int rank = lodFadeRank(ivec2(gl_FragCoord.xy));
        return (fade < 0.0) ? (rank < k) : (rank >= 64 - k);
    }

    // last, right before the colour is written: the dither is not uniform within a quad, and
    // derivatives after a non-uniform discard are undefined (GLSL ES 3.00 6.4). No semicolon after
    // it: without the fade it is nothing at all, and the variants without it stay the same programs
    #define LOD_FADE_CLIP() if (!lodFadeKeeps()) discard;
#endif

#else
    #define LOD_FADE_CLIP()
#endif
