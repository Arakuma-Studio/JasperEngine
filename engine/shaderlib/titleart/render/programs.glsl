// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The numbers of the Title Art renderer's programs (ShaderType::TITLE_ART's property bits 0-6,
// TA_PROGRAM), member for member render/TitlePrograms.h's TitleRenderProgram
// (TitleArtShadersTest checks both). 1..17 are the effect and generator programs of
// titleart.frag (render/TitlePasses.h); the renderer's run from TA_PROGRAM_RENDERER_FIRST to
// TA_PROGRAM_RENDERER_LAST in titleart_render.frag.

#define TA_PROGRAM_RENDERER_FIRST 18
#define TA_PROGRAM_COPY 18          // a source through a uv map, opacity, an optional mask
#define TA_PROGRAM_BLEND 19         // a blend mode of §6.4 over a backdrop copy (blending off)
#define TA_PROGRAM_ADJUST_MIX 20    // an adjustment layer mixed over what is below it
#define TA_PROGRAM_MASK_VALUE 21    // a mask source's value, invert and density, times the masks so far
#define TA_PROGRAM_MASK_GRADIENT 22 // an analytic gradient mask
#define TA_PROGRAM_BLUR 23          // separable Gaussian of a filterable target
#define TA_PROGRAM_BLUR_RAW 24      // separable Gaussian of a scalar field (texelFetch)
#define TA_PROGRAM_OUTPUT 25        // straight sRGB with dither, premultiplied sRGB, or the grey mask
#define TA_PROGRAM_SEED 26          // jump-flood seeds at sub-pixel edge points of a coverage mask
#define TA_PROGRAM_FLOOD 27         // one jump-flood step
#define TA_PROGRAM_RESOLVE 28       // seeds to the signed distance
#define TA_PROGRAM_HEIGHT 29        // bevel height from the distance through the profile
#define TA_PROGRAM_COVER_INIT 30    // Fit bevel: every inside pixel seeds its inscribed circle
#define TA_PROGRAM_COVER_FLOOD 31   // Fit bevel: the largest covering circle, one jump step
#define TA_PROGRAM_COVER_RADIUS 32  // Fit bevel: the covering circle's radius
#define TA_PROGRAM_REDUCE_MAX 33    // 8 x 8 max reduction of the inside distance (shape burst)
#define TA_PROGRAM_SPAN 34          // the nearest glyph of every frame pixel (line / char spans)
#define TA_PROGRAM_ELEMENT 35       // one element shaded (or punched) into the stack
#define TA_PROGRAM_PLACE 36         // the stack placed into the node target
#define TA_PROGRAM_ERODE 37         // the dry-brush keep-mask
#define TA_PROGRAM_MASK_KEEP 38     // an element mask times the keep-mask
#define TA_PROGRAM_GROW 39          // a mask grown through its distance field (image rims)
#define TA_PROGRAM_SEED_ALPHA 40    // jump-flood seeds at the alpha edge of a layer target
#define TA_PROGRAM_RESOLVE_ALPHA 41 // seeds to the signed distance of a layer target's alpha
#define TA_PROGRAM_RENDERER_LAST 63
