// The four FX slots of a batchable sprite (docs/specs/S52-sprite-instancing.md R19, R21), for the
// fragment stage of a manifest shader whose @property claims one with instance=fx.<slot>:
//
//   fx.x  Palette Swap row (S43)      fx.y  FLASH amount
//   fx.z  DITHER_FADE amount (S48)    fx.w  DISSOLVE progress (S48)
//
// Read a claimed amount as `customParams.<member> + spriteFx(<slot>)`. Every neutral value is 0:
// drawn alone the uniform holds the value and the slot is 0; batched (the variant's 'Sfx',
// HAS_SPRITE_FX) the slot holds it (baked per vertex, or a Sprite Batch's per instance) and the
// run's uniform is 0. Without 'Sfx' the slot reads 0. The vertex stage is includes/surface_vs.glsl.

#ifndef SPRITE_FX_GLSL
#define SPRITE_FX_GLSL

#ifdef HAS_SPRITE_FX
    flat in vec4 v_spriteFx;
    #define spriteFx(slot) (v_spriteFx.slot)
#else
    #define spriteFx(slot) (0.0)
#endif

#endif
