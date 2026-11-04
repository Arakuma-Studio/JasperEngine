// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The numbers of Title Art's programs (ShaderType::TITLE_ART's property bits, TA_PROGRAM),
// member for member render/TitlePrograms.h's TitleProgram (TitleArtShadersTest checks both).
// 1..63 are the renderer's (S80); 64..255 are the effect and generator programs of the
// registry's definitions (S80's core effects, S83's catalogue), which declare theirs in
// titleart/effects.glsl.

#define TA_PROGRAM_COPY 1           // a source through a uv map, opacity, an optional mask
#define TA_PROGRAM_BLEND 2          // a blend mode of §6.4 over a backdrop copy (blending off)
#define TA_PROGRAM_ADJUST 3         // one of the fourteen adjustment operations
#define TA_PROGRAM_ADJUST_MIX 4     // an adjustment layer mixed over what is below it
#define TA_PROGRAM_MASK_VALUE 5     // a mask source's value, invert and density, times the masks so far
#define TA_PROGRAM_MASK_GRADIENT 6  // an analytic gradient mask
#define TA_PROGRAM_BLUR 7           // separable Gaussian of a filterable target
#define TA_PROGRAM_BLUR_RAW 8       // separable Gaussian of a scalar field (texelFetch)
#define TA_PROGRAM_OUTPUT 9         // straight sRGB with dither, premultiplied sRGB, or the grey mask
#define TA_PROGRAM_SEED 10          // jump-flood seeds at sub-pixel edge points of a coverage mask
#define TA_PROGRAM_FLOOD 11         // one jump-flood step
#define TA_PROGRAM_RESOLVE 12       // seeds to the signed distance
#define TA_PROGRAM_HEIGHT 13        // bevel height from the distance through the profile
#define TA_PROGRAM_COVER_INIT 14    // Fit bevel: every inside pixel seeds its inscribed circle
#define TA_PROGRAM_COVER_FLOOD 15   // Fit bevel: the largest covering circle, one jump step
#define TA_PROGRAM_COVER_RADIUS 16  // Fit bevel: the covering circle's radius
#define TA_PROGRAM_REDUCE_MAX 17    // 8 x 8 max reduction of the inside distance (shape burst)
#define TA_PROGRAM_SPAN 18          // the nearest glyph of every frame pixel (line / char spans)
#define TA_PROGRAM_ELEMENT 19       // one element shaded (or punched) into the stack
#define TA_PROGRAM_PLACE 20         // the stack placed into the node target
#define TA_PROGRAM_ERODE 21         // the dry-brush keep-mask
#define TA_PROGRAM_MASK_KEEP 22     // an element mask times the keep-mask
#define TA_PROGRAM_GROW 23          // a mask grown through its distance field (image rims)
#define TA_PROGRAM_SEED_ALPHA 24    // jump-flood seeds at the alpha edge of a layer target
#define TA_PROGRAM_RESOLVE_ALPHA 25 // seeds to the signed distance of a layer target's alpha
#define TA_PROGRAM_RENDERER_LAST 63
