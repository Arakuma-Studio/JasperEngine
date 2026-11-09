#version 450

// @shader mesh
// @name "Sprite"
// @category "2D"
// @description "Sprite, tilemap and polygon effects on the sprite's own texture and colour: outline (smooth, or pixel with 4 or 8 neighbours, an inline and a colour from a ramp or the art, sel-out), hue shift, hit flash, grayscale, colour replace, overlay, glow, palette swap and cycling, and the Pixel FX dither fade, dissolve, ramp recolour and drop shadow; lit by 2D lights, with the material's Normal Texture as the normal map. Fixed order: palette swap, colour replace, hue shift, ramp recolour, grayscale, overlay, flash, dissolve, inline, outline (behind), drop shadow (behind the outline), alpha mode, 2D lights, glow, dither fade. A pixel outline or a drop shadow grows the sprite's quad so nothing is cut at the frame edge. Alpha blended, both faces, no shadows; with the Premultiplied blend mode the texture is read as premultiplied."
// @require uv1 color normalmap
// @state blend=alpha zwrite=off cull=off shadows=off
// @batchable framerect=OUTLINE,OVERLAY,OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8,DROP_SHADOW,DISSOLVE,RAMP_RECOLOR,DITHER_FADE
// @feature OUTLINE "Outline"
// @feature HUE_SHIFT "Hue shift"
// @feature FLASH "Hit flash"
// @feature GRAYSCALE "Grayscale"
// @feature COLOR_REPLACE "Colour replace"
// @feature OVERLAY "Overlay (hard light)"
// @feature GLOW "Glow"
// @feature PALETTE_SWAP "Palette Swap"
// @feature PALETTE_CYCLE "Palette Cycling"
// @property outlineColor color (1, 1, 1, 1) "Outline colour" feature=OUTLINE
// @property outlineWidth range(0, 8) 1 "Outline width (texels)" feature=OUTLINE
// @property hueShift range(0, 360) 120 "Hue shift (degrees)" feature=HUE_SHIFT
// @property hueSpeed float 0 "Hue cycle speed (degrees per second)" feature=HUE_SHIFT
// @property flashColor color (1, 1, 1, 1) "Flash colour" feature=FLASH
// @property flashAmount range(0, 1) 0.75 "Flash amount" feature=FLASH instance=fx.y
// @property grayscaleAmount range(0, 1) 1 "Grayscale amount" feature=GRAYSCALE
// @property replaceColorR color (0.9, 0.2, 0.2, 1) "Replacement for the mask's red (alpha: strength)" feature=COLOR_REPLACE
// @property replaceColorG color (0.2, 0.8, 0.3, 1) "Replacement for the mask's green (alpha: strength)" feature=COLOR_REPLACE
// @property replaceColorB color (0.25, 0.4, 0.95, 1) "Replacement for the mask's blue (alpha: strength)" feature=COLOR_REPLACE
// @property overlayColor color (1, 0.78, 0.3, 1) "Overlay colour (times the overlay texture)" feature=OVERLAY
// @property overlayTiling vector4 (1, 1, 0, 0) "Overlay tiling (xy) and offset (zw), over the frame" feature=OVERLAY
// @property overlayAmount range(0, 1) 1 "Overlay amount" feature=OVERLAY
// @property glowColor hdrcolor (2, 1.5, 0.8, 1) "Glow colour (HDR)" feature=GLOW
// @property glowScale range(0, 4) 1 "Glow alpha scale" feature=GLOW
// @property glowPower range(0.1, 8) 2 "Glow falloff power" feature=GLOW
// @texture u_replaceMaskTexture black "Replace mask (RGB selects the replacement colour)" feature=COLOR_REPLACE
// @texture u_overlayTexture white "Overlay texture" feature=OVERLAY
// @palette u_palette "Palette" feature=PALETTE_SWAP,PALETTE_CYCLE
// @property paletteRow int 0 "Active palette (0: original)" feature=PALETTE_SWAP instance=fx.x
// @property paletteCycleSpeed float 1 "Cycling speed" feature=PALETTE_CYCLE
// @feature DITHER_FADE "Dither Fade"
// @feature DISSOLVE "Dissolve"
// @feature RAMP_RECOLOR "Ramp Recolour"
// @feature DROP_SHADOW "Drop Shadow"
// @keywords OUTLINE_STYLE "Outline style" SMOOTH PIXEL4 PIXEL8 feature=OUTLINE
// @property outlineColorFrom choice("Colour", "Ramp darkest", "Selective") 0 "Outline colour from" feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8
// @property outlineRampSteps range(1, 4) 1 "Selective: ramp steps darker" feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8
// @property inlineWidth range(0, 2) 0 "Inline width (texels)" feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8
// @property inlineColor color (0, 0, 0, 1) "Inline colour" feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8
// @property ditherFadeAmount range(0, 1) 0 "Dither fade (1: gone)" feature=DITHER_FADE instance=fx.z
// @property ditherMatrix choice("Bayer 4x4", "Bayer 8x8") 0 "Dither matrix" feature=DITHER_FADE
// @property ditherSpace choice("Sprite texels", "Screen pixels") 0 "Dither space" feature=DITHER_FADE
// @property dissolveProgress range(0, 1) 0 "Dissolve progress (1: gone)" feature=DISSOLVE instance=fx.w
// @property dissolveDirection vector2 (0, 1) "Dissolve direction (texels, y up; 0: noise only)" feature=DISSOLVE
// @property dissolveNoise range(0, 1) 0.35 "Dissolve noise" feature=DISSOLVE
// @property dissolveNoiseScale range(1, 32) 4 "Dissolve noise cell (texels)" feature=DISSOLVE
// @property dissolveJitter range(0, 0.3) 0.08 "Dissolve jitter" feature=DISSOLVE
// @property dissolveEdgeWidth range(0, 8) 2 "Edge band (whole texels)" feature=DISSOLVE
// @property dissolveScorchWidth range(0, 16) 3 "Scorch band (texels)" feature=DISSOLVE
// @property dissolveEdgeGlow hdrcolor (0, 0, 0, 1) "Edge glow (HDR, black: off)" feature=DISSOLVE
// @property dissolveSeed int 0 "Dissolve seed" feature=DISSOLVE
// @property rampAmount range(0, 1) 1 "Ramp amount (dithered)" feature=RAMP_RECOLOR
// @property rampLow float 1 "Ramp step for luma 0 (negative: from the end)" feature=RAMP_RECOLOR
// @property rampHigh float -1 "Ramp step for luma 1 (negative: from the end)" feature=RAMP_RECOLOR
// @property rampBevel range(0, 1) 0 "Ramp bevel" feature=RAMP_RECOLOR
// @property rampPulse float 0 "Ramp pulse (steps per second)" feature=RAMP_RECOLOR
// @property shadowOffset vector2 (1, -1) "Shadow offset (texels, y up)" feature=DROP_SHADOW
// @property shadowColor color (0, 0, 0, 0.5) "Shadow colour" feature=DROP_SHADOW
// @property shadowColorFrom choice("Colour", "Ramp darkest") 0 "Shadow colour from" feature=DROP_SHADOW
// @texture u_fxRampTexture ramp_grey "Ramp (an N x 1 strip, or <palette>.palette#ramp:<name>)" feature=DISSOLVE,RAMP_RECOLOR,OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8,DROP_SHADOW
// @expand outlineWidth feature=OUTLINE_STYLE_PIXEL4,OUTLINE_STYLE_PIXEL8
// @expand shadowOffset feature=DROP_SHADOW

// Sprites keep the engine's own material: the texture and colour of the Sprite (or Tilemap,
// MeshPolygon), its texture rect (atlas frame), the vertex and instance colours. Palette
// Swap and Palette Cycling come first, on the raw texel (its exact bytes, before the tint:
// includes/palette.glsl); the other effects run on straight-alpha linear colour in this order: colour replace, hue shift,
// grayscale, overlay, flash, outline (behind the sprite), the material's alpha mode, 2D
// lights (shaped by the material's Normal Texture when it has one: @require normalmap), glow. With OUTLINE or OVERLAY the texture is read inside the current frame only.
// The Pixel FX features (docs/specs/S48-pixel-fx.md) slot into that order: ramp recolour after
// the hue shift, dissolve after the flash, then inline, outline, drop shadow, and the dither
// fade last of all. They work on whole texels of the frame (x right, y up, from its corner),
// so they stay on the art pixel grid, and pick palette colours from the ramp (u_fxRampTexture,
// an N x 1 strip read at texel centres) instead of blending. A pixel outline or a drop shadow
// draws outside the art: the engine grows the sprite's quad for them (@expand), and the texels
// beyond the frame read transparent. Each animated amount (flash, dither fade, dissolve, ramp)
// skips its body on a uniform branch while it is 0; the per-instance FX slots of S52's batches
// add to them (spriteFx: fx.y flash, fx.z dither fade, fx.w dissolve; 0 without a batch).
// The blend mode is the material's (alpha by default): with Premultiplied (BLEND_PREMULTIPLIED)
// the texture is premultiplied, un-premultiplied for the effects and premultiplied again on
// output (finalizeStraightColor), as are the other modes' outputs. No shadows by default:
// the sprite's shape is its texture's alpha, and without a depth program a shadow would be
// the whole quad (as Unity's sprites, which cast none either).
// Dynamic Batching draws it (@batchable): the Palette Swap row, the flash, dither fade and dissolve
// amounts are read with their FX slot added (includes/sprite_fx.glsl), and the frame rect of
// OUTLINE, OVERLAY and the Pixel FX features comes per vertex (sprite.vert).

#if defined(OUTLINE_STYLE_PIXEL4) || defined(OUTLINE_STYLE_PIXEL8)
    #define OUTLINE_PIXEL
#endif
// texel-exact work inside the frame (as sprite.vert)
#if defined(OUTLINE_PIXEL) || defined(DROP_SHADOW) || defined(DISSOLVE) || defined(RAMP_RECOLOR) || defined(DITHER_FADE)
    #define SPRITE_FX_TEXELS
#endif

#if (defined(OUTLINE) || defined(OVERLAY) || defined(SPRITE_FX_TEXELS)) && defined(HAS_UV_SET1)
    #define SPRITE_FRAME_RECT
#endif

#if defined(SPRITE_FX_TEXELS) && defined(SPRITE_FRAME_RECT)
    #define SPRITE_FX
    // the features that read the ramp
    #if defined(DISSOLVE) || defined(RAMP_RECOLOR) || defined(OUTLINE_PIXEL) || defined(DROP_SHADOW)
        #define SPRITE_FX_RAMP
    #endif
    // the quad may be grown (@expand): what lies beyond the frame is transparent
    #if defined(OUTLINE_PIXEL) || defined(DROP_SHADOW)
        #define SPRITE_FX_GROWN
    #endif
#endif

#ifdef USE_LIGHT2D
    #define SURFACE_POSITION
#endif

#if defined(HUE_SHIFT) || defined(FLASH) || defined(GRAYSCALE) || defined(OUTLINE) || defined(COLOR_REPLACE) || defined(OVERLAY) || defined(GLOW) || defined(PALETTE_SWAP) || defined(PALETTE_CYCLE) || defined(DITHER_FADE) || defined(DISSOLVE) || defined(RAMP_RECOLOR) || defined(DROP_SHADOW)
uniform u_fs_customParams {
    float time; // engine-written seconds
    #ifdef OUTLINE
        vec4 outlineColor;
        float outlineWidth;
    #endif
    #ifdef HUE_SHIFT
        float hueShift;
        float hueSpeed;
    #endif
    #ifdef FLASH
        vec4 flashColor;
        float flashAmount;
    #endif
    #ifdef GRAYSCALE
        float grayscaleAmount;
    #endif
    #ifdef COLOR_REPLACE
        vec4 replaceColorR;
        vec4 replaceColorG;
        vec4 replaceColorB;
    #endif
    #ifdef OVERLAY
        vec4 overlayColor;
        vec4 overlayTiling;
        float overlayAmount;
    #endif
    #ifdef GLOW
        vec4 glowColor;
        float glowScale;
        float glowPower;
    #endif
    #ifdef PALETTE_SWAP
        float paletteRow;
    #endif
    #ifdef PALETTE_CYCLE
        float paletteCycleSpeed;
        float sceneTime; // engine-written: the scene clock, which holds while the game is paused
    #endif
    // S48's members last, so the blocks of the features before keep their layout
    #ifdef OUTLINE_PIXEL
        vec4 inlineColor;
        float outlineColorFrom;
        float outlineRampSteps;
        float inlineWidth;
    #endif
    #ifdef DITHER_FADE
        float ditherFadeAmount;
        float ditherMatrix;
        float ditherSpace;
    #endif
    #ifdef DISSOLVE
        vec4 dissolveEdgeGlow;
        vec2 dissolveDirection;
        float dissolveProgress;
        float dissolveNoise;
        float dissolveNoiseScale;
        float dissolveJitter;
        float dissolveEdgeWidth;
        float dissolveScorchWidth;
        float dissolveSeed;
    #endif
    #ifdef RAMP_RECOLOR
        float rampAmount;
        float rampLow;
        float rampHigh;
        float rampBevel;
        float rampPulse;
    #endif
    #ifdef DROP_SHADOW
        vec4 shadowColor;
        vec2 shadowOffset;
        float shadowColorFrom;
    #endif
} customParams;
#endif

#ifdef COLOR_REPLACE
    uniform texture2D u_replaceMaskTexture;
    uniform sampler u_replaceMask_smp;
#endif
#ifdef OVERLAY
    uniform texture2D u_overlayTexture;
    uniform sampler u_overlay_smp;
#endif
#ifdef SPRITE_FX_RAMP
    uniform texture2D u_fxRampTexture;
    uniform sampler u_fxRamp_smp;
#endif

#ifdef SURFACE_POSITION
    in vec3 v_position;
#endif
#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // vertex x instance colour
#ifdef SPRITE_FRAME_RECT
    flat in vec4 v_frameRect;
    #if defined(DITHER_FADE) && defined(PIXEL_SNAP)
        flat in float v_pixelScale;
    #endif
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"
#include "includes/color_fx.glsl"
#include "includes/base_color_fs.glsl"
#include "includes/palette.glsl"
#include "includes/sprite_fx.glsl"
#ifdef USE_LIGHT2D
    #include "includes/lighting2d.glsl"
#endif
#ifdef SPRITE_FX
    #include "includes/pixel_fx.glsl"
    #include "includes/bayer.glsl"
#endif

// the texture as straight-alpha (sRGB-encoded colour, alpha)
vec4 spriteTexel(vec2 uv){
    #ifdef HAS_UV_SET1
        vec4 texel = sampleBaseColorTexture(uv);
        #ifdef BLEND_PREMULTIPLIED
            texel.rgb /= max(texel.a, 1.0 / 255.0);
        #endif
        return texel;
    #else
        return vec4(1.0);
    #endif
}

#ifdef SPRITE_FRAME_RECT
    // the frame rect as min/max corners, whatever the sign of its size
    vec4 frameBounds(){
        vec2 a = v_frameRect.xy;
        vec2 b = v_frameRect.xy + v_frameRect.zw;
        return vec4(min(a, b), max(a, b));
    }

    // the frame shrunk by half a texel: the UVs whose filtering stays inside the frame
    vec4 frameInset(vec2 texelSize){
        vec4 bounds = frameBounds();
        vec2 lo = bounds.xy + texelSize * 0.5;
        return vec4(lo, max(bounds.zw - texelSize * 0.5, lo));
    }
#endif

#ifdef SPRITE_FX
    vec2 fxTexSize;   // the texture, texels
    vec2 fxOrigin;    // the frame's top-left corner, UV
    ivec2 fxSize;     // the frame, texels
    #ifdef DISSOLVE
        float fxProgress; // dissolveProgress plus the instance's slot
        float fxFront;    // the front the keys burn against (pfxDissolveFront)
    #endif

    // a texel of the frame (x right, y down from its top, as the image's rows) as the frame
    // texel the effects count in (y up from its bottom)
    ivec2 fxUp(ivec2 t){
        return ivec2(t.x, fxSize.y - 1 - t.y);
    }

    bool fxInside(ivec2 t){
        return all(greaterThanEqual(t, ivec2(0))) && all(lessThan(t, fxSize));
    }

    // a texel of the frame as straight alpha, sampled at its centre; transparent outside it
    vec4 fxTexel(ivec2 t){
        if (!fxInside(t))
            return vec4(0.0);
        vec4 texel = textureLod(sampler2D(u_baseColorTexture, u_baseColor_smp), fxOrigin + (vec2(t) + 0.5) / fxTexSize, 0.0);
        #ifdef BLEND_PREMULTIPLIED
            texel.rgb /= max(texel.a, 1.0 / 255.0);
        #endif
        return texel;
    }

    #ifdef DISSOLVE
        // a texel of the frame against the dissolve's front (pfxDissolveDistance: negative burnt,
        // below the edge width in the edge band, below edge + scorch scorched)
        float fxDissolveDistanceAt(ivec2 t){
            return pfxDissolveDistance(fxUp(t), fxSize, customParams.dissolveDirection, customParams.dissolveNoise,
                                       customParams.dissolveNoiseScale, customParams.dissolveJitter,
                                       uint(max(customParams.dissolveSeed, 0.0)), fxProgress,
                                       floor(max(customParams.dissolveEdgeWidth, 0.0) + 0.5), max(customParams.dissolveScorchWidth, 0.0));
        }

        // whether a texel of the frame has burnt: its key against the front alone
        bool fxBurnt(ivec2 t){
            if (fxProgress >= 1.0)
                return true;
            float key = pfxDissolveKey(fxUp(t), fxSize, customParams.dissolveDirection, customParams.dissolveNoise,
                                       customParams.dissolveNoiseScale, customParams.dissolveJitter, uint(max(customParams.dissolveSeed, 0.0)));
            return key < fxFront;
        }
    #endif

    // 1 where a texel of the frame is opaque (alpha from one half) and not burnt away
    float fxOpaque(ivec2 t){
        float on = fxTexel(t).a >= 0.5 ? 1.0 : 0.0;
        #ifdef DISSOLVE
            if (on > 0.0 && fxProgress > 0.0 && fxBurnt(t))
                on = 0.0;
        #endif
        return on;
    }

    #ifdef SPRITE_FX_RAMP
        // the ramp's steps (its width, 1 to 32)
        int fxRampCount(){
            return clamp(textureSize(sampler2D(u_fxRampTexture, u_fxRamp_smp), 0).x, 1, 32);
        }

        // a ramp step as stored (sRGB), clamped to the ramp
        vec3 fxRampTexel(int index){
            return texelFetch(sampler2D(u_fxRampTexture, u_fxRamp_smp), ivec2(clamp(index, 0, fxRampCount() - 1), 0), 0).rgb;
        }

        // a ramp step as a linear colour
        vec3 fxRamp(int index){
            return sRGBToLinear(fxRampTexel(index));
        }

        // the step whose display luma is nearest a display (sRGB) colour's: one pass over the ramp
        int fxRampNearest(vec3 display){
            float luma = colorLuminance(display);
            int count = fxRampCount();
            int best = 0;
            float bestDistance = 2.0;
            for (int i = 0; i < 32; i++){
                if (i >= count)
                    break;
                float gap = abs(colorLuminance(fxRampTexel(i)) - luma);
                if (gap < bestDistance){
                    bestDistance = gap;
                    best = i;
                }
            }
            return best;
        }
    #endif

    #ifdef DITHER_FADE
        // R2's threshold for a texel cell (sprite space) or the screen (screen space)
        float fxDitherThreshold(ivec2 spriteCell){
            ivec2 cell = spriteCell;
            if (customParams.ditherSpace > 0.5){
                #ifdef PIXEL_SNAP
                    // S41's base pixels
                    cell = ivec2(floor(gl_FragCoord.xy / v_pixelScale));
                #else
                    cell = ivec2(floor(gl_FragCoord.xy));
                #endif
            }
            return bayerThreshold(customParams.ditherMatrix > 0.5 ? 8 : 4, cell);
        }
    #endif
#endif

#if defined(OUTLINE) && defined(SPRITE_FRAME_RECT) && !defined(OUTLINE_PIXEL)
    // Alpha of the 8 neighbours at outlineWidth texels (diagonals on the circle), each
    // inside the current frame only: a tap outside the frame inset reads 0 and samples a
    // clamped, in-frame position, so an atlas neighbour never bleeds in.
    float outlineAlpha(vec2 uv, vec2 texelSize, vec4 inset){
        vec2 lo = inset.xy;
        vec2 hi = inset.zw;
        vec2 step1 = texelSize * customParams.outlineWidth;
        vec2 stepD = step1 * 0.70710678;
        vec2 offsets[8] = vec2[8](
            vec2(step1.x, 0.0), vec2(-step1.x, 0.0), vec2(0.0, step1.y), vec2(0.0, -step1.y),
            stepD, -stepD, vec2(stepD.x, -stepD.y), vec2(-stepD.x, stepD.y));
        float alpha = 0.0;
        for (int i = 0; i < 8; i++){
            vec2 p = uv + offsets[i];
            vec2 clamped = clamp(p, lo, hi);
            float inside = (p == clamped) ? 1.0 : 0.0;
            #if defined(DISSOLVE) && defined(SPRITE_FX)
                // the outline follows the dissolving shape
                if (inside > 0.0 && fxProgress > 0.0)
                    inside = fxOpaque(ivec2(floor((clamped - fxOrigin) * fxTexSize))) > 0.0 ? 1.0 : 0.0;
            #endif
            alpha = max(alpha, sampleBaseColorTexture(clamped).a * inside);
        }
        return alpha;
    }
#endif

void main() {
    vec2 uv = v_uv1;
    #ifdef SPRITE_FRAME_RECT
        // filtering at the frame's edge would blend in the neighbouring atlas frame
        vec2 texelSize = 1.0 / baseColorTextureSize();
        vec4 inset = frameInset(texelSize);
        uv = clamp(uv, inset.xy, inset.zw);
    #endif

    #ifdef SPRITE_FX
        // the frame in texels and the fragment's texel in it (image rows, from the frame's top)
        fxTexSize = baseColorTextureSize();
        vec4 fxBounds = frameBounds();
        fxOrigin = fxBounds.xy;
        fxSize = max(ivec2(floor((fxBounds.zw - fxBounds.xy) * fxTexSize + 0.5)), ivec2(1));
        ivec2 fxPos = ivec2(floor((v_uv1 - fxOrigin) * fxTexSize));
        #ifdef SPRITE_FX_GROWN
            bool fxInFrame = fxInside(fxPos);
        #else
            fxPos = clamp(fxPos, ivec2(0), fxSize - 1);
        #endif
        // the Bayer cell of the texel: non-negative, a multiple of 8 away so the pattern stays
        ivec2 fxCell = fxUp(fxPos) + ivec2(64);
        #ifdef DISSOLVE
            fxProgress = customParams.dissolveProgress + spriteFx(w);
            fxFront = 2.0;
            if (fxProgress > 0.0){
                // the frame's straight front: its span is the bands' width in key units
                vec2 gradient;
                float range;
                float extent;
                pfxDissolveSmooth(vec2(0.5), vec2(fxSize), customParams.dissolveDirection, customParams.dissolveNoise,
                                  customParams.dissolveNoiseScale, uint(max(customParams.dissolveSeed, 0.0)), gradient, range, extent);
                float nominal = 1.0 / ((range + max(customParams.dissolveJitter, 0.0)) * extent);
                fxFront = pfxDissolveFront(fxProgress, (floor(max(customParams.dissolveEdgeWidth, 0.0) + 0.5) +
                                                        max(customParams.dissolveScorchWidth, 0.0)) * nominal);
            }
        #endif
    #endif

    vec4 texel = spriteTexel(uv);
    #ifdef SPRITE_FX_GROWN
        // the grown border around the art is empty
        if (!fxInFrame)
            texel = vec4(0.0);
    #endif
    #if defined(PALETTE_SWAP) || defined(PALETTE_CYCLE)
        // the authored bytes, before the tint: a white tint keeps the exact palette
        #ifdef PALETTE_SWAP
            float paletteRow = customParams.paletteRow + spriteFx(x);
        #else
            float paletteRow = 0.0;
        #endif
        #ifdef PALETTE_CYCLE
            float paletteCycleSpeed = customParams.paletteCycleSpeed;
            float paletteTime = customParams.sceneTime;
        #else
            float paletteCycleSpeed = 0.0;
            float paletteTime = 0.0;
        #endif
        texel = paletteSwapTexel(texel, paletteRow, paletteTime, paletteCycleSpeed);
    #endif
    vec4 tint = pbrParams.baseColorFactor * v_color;
    vec4 color = vec4(sRGBToLinear(texel.rgb), texel.a) * tint;

    #ifdef COLOR_REPLACE
        // the masked texels' brightness times the replacement colours
        vec3 mask = texture(sampler2D(u_replaceMaskTexture, u_replaceMask_smp), uv).rgb;
        float shade = colorLuminance(color.rgb);
        color.rgb = mix(color.rgb, customParams.replaceColorR.rgb * shade, mask.r * customParams.replaceColorR.a);
        color.rgb = mix(color.rgb, customParams.replaceColorG.rgb * shade, mask.g * customParams.replaceColorG.a);
        color.rgb = mix(color.rgb, customParams.replaceColorB.rgb * shade, mask.b * customParams.replaceColorB.a);
    #endif

    #ifdef HUE_SHIFT
        // wrapped to a turn, so the rotation's sine and cosine stay precise in a long run
        color.rgb = hueRotate(color.rgb, radians(mod(customParams.hueShift + customParams.hueSpeed * customParams.time, 360.0)));
    #endif

    #if defined(RAMP_RECOLOR) && defined(SPRITE_FX)
        // the display luma picks a ramp rampStep; an amount below 1 is a dithered choice per texel
        if (customParams.rampAmount > 0.0 && color.a > 0.0 && bayer4(fxCell) < customParams.rampAmount){
            float count = float(fxRampCount());
            float low = customParams.rampLow < 0.0 ? count + customParams.rampLow : customParams.rampLow;
            float high = customParams.rampHigh < 0.0 ? count + customParams.rampHigh : customParams.rampHigh;
            float luma = clamp(colorLuminance(linearTosRGB(max(color.rgb, vec3(0.0)))), 0.0, 1.0);
            float rampStep = floor(low + (high - low) * luma + 0.5);
            if (customParams.rampBevel > 0.0 && bayer4(fxCell) < customParams.rampBevel){
                // lit from the top left: an edge facing the light one rampStep lighter, one facing away darker
                if (fxTexel(fxPos + ivec2(-1, -1)).a < 0.5)
                    rampStep += 1.0;
                else if (fxTexel(fxPos + ivec2(1, 1)).a < 0.5)
                    rampStep -= 1.0;
            }
            if (customParams.rampPulse != 0.0){
                // whole steps a second, cycling within low..high
                float first = min(low, high);
                float span = abs(high - low) + 1.0;
                rampStep = first + mod(rampStep - first + floor(customParams.time * customParams.rampPulse), span);
            }
            color.rgb = fxRamp(int(rampStep));
        }
    #endif

    #ifdef GRAYSCALE
        color.rgb = grayscale(color.rgb, customParams.grayscaleAmount);
    #endif

    #ifdef OVERLAY
        // hard light of texture x colour over the sprite's grey value (the "gold skin"),
        // the texture laid over the frame so atlas animation does not swim
        #ifdef SPRITE_FRAME_RECT
            vec4 bounds = frameBounds();
            vec2 frameUV = (v_uv1 - bounds.xy) / max(bounds.zw - bounds.xy, vec2(1e-6));
        #else
            vec2 frameUV = v_uv1;
        #endif
        vec2 overlayUV = frameUV * customParams.overlayTiling.xy + customParams.overlayTiling.zw;
        vec3 layer = texture(sampler2D(u_overlayTexture, u_overlay_smp), overlayUV).rgb * linearTosRGB(customParams.overlayColor.rgb);
        vec3 grey = linearTosRGB(vec3(colorLuminance(color.rgb)));
        color.rgb = mix(color.rgb, sRGBToLinear(hardLight(grey, layer)), customParams.overlayAmount * customParams.overlayColor.a);
    #endif

    #ifdef FLASH
        color.rgb = mix(color.rgb, customParams.flashColor.rgb, (customParams.flashAmount + spriteFx(y)) * customParams.flashColor.a);
    #endif

    #if defined(DISSOLVE) && defined(SPRITE_FX)
        // colour bands along the front, then what it passed is gone
        if (fxProgress > 0.0 && color.a > 0.0){
            float dist = fxProgress >= 1.0 ? -1.0 : fxDissolveDistanceAt(fxPos);
            float edge = floor(max(customParams.dissolveEdgeWidth, 0.0) + 0.5);
            if (dist < 0.0){
                color.a = 0.0;
            }else if (dist < edge){
                // the edge band: the ramp's hot end at the front, a step cooler per texel inward
                int count = fxRampCount();
                int band = int(floor(dist));
                color.rgb = fxRamp(count - 1 - (band * (count - 1)) / max(int(edge), 1)) + customParams.dissolveEdgeGlow.rgb;
            }else if (dist < edge + max(customParams.dissolveScorchWidth, 0.0)){
                // scorched: the art's own step (by luma) one darker
                float luma = clamp(colorLuminance(linearTosRGB(max(color.rgb, vec3(0.0)))), 0.0, 1.0);
                color.rgb = fxRamp(int(floor(luma * float(fxRampCount() - 1) + 0.5)) - 1);
            }
        }
    #endif

    float spriteAlpha = color.a;

    #if defined(OUTLINE_PIXEL) && defined(SPRITE_FX)
        if (color.a >= 0.5 && customParams.inlineWidth >= 0.5){
            // the inline: opaque texels with a transparent 4-neighbour within its width
            int width = int(clamp(floor(customParams.inlineWidth + 0.5), 1.0, 2.0));
            bool edgeTexel = false;
            for (int s = 1; s <= 2; s++){
                if (s > width || edgeTexel)
                    break;
                edgeTexel = fxOpaque(fxPos + ivec2(s, 0)) < 0.5 || fxOpaque(fxPos + ivec2(-s, 0)) < 0.5 ||
                            fxOpaque(fxPos + ivec2(0, s)) < 0.5 || fxOpaque(fxPos + ivec2(0, -s)) < 0.5;
            }
            if (edgeTexel){
                // Selective: its own colour a step darker on the ramp
                color.rgb = customParams.outlineColorFrom > 1.5 ? fxRamp(fxRampNearest(linearTosRGB(max(color.rgb, vec3(0.0)))) - 1)
                                                                 : customParams.inlineColor.rgb;
            }
        }
        if (color.a < 0.5){
            // the outline: a transparent texel with an opaque neighbour at whole-texel steps
            // 1..width, in the 4 (PIXEL4) or 8 (PIXEL8) directions, behind the sprite
            int width = int(clamp(floor(customParams.outlineWidth + 0.5), 1.0, 2.0));
            #ifdef OUTLINE_STYLE_PIXEL8
                const int directions = 8;
            #else
                const int directions = 4;
            #endif
            ivec2 steps[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
                                      ivec2(1, 1), ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1));
            bool hit = false;
            ivec2 touched = ivec2(0);
            for (int s = 1; s <= 2; s++){
                if (s > width || hit)
                    break;
                for (int i = 0; i < directions; i++){
                    ivec2 q = fxPos + steps[i] * s;
                    if (fxOpaque(q) > 0.5){
                        hit = true;
                        touched = q;
                        break;
                    }
                }
            }
            if (hit){
                vec4 outlineColor = customParams.outlineColor;
                if (customParams.outlineColorFrom > 1.5){
                    // sel-out: the touched texel's colour, outlineRampSteps darker on the ramp
                    int down = int(floor(customParams.outlineRampSteps + 0.5));
                    outlineColor = vec4(fxRamp(fxRampNearest(fxTexel(touched).rgb) - down), 1.0);
                }else if (customParams.outlineColorFrom > 0.5){
                    outlineColor = vec4(fxRamp(0), 1.0);
                }
                float outline = outlineColor.a * tint.a;
                float alpha = color.a + outline * (1.0 - color.a);
                color.rgb = (color.rgb * color.a + outlineColor.rgb * outline * (1.0 - color.a)) / max(alpha, 1e-5);
                color.a = alpha;
            }
        }
    #endif

    #if defined(OUTLINE) && defined(SPRITE_FRAME_RECT) && !defined(OUTLINE_PIXEL)
        // the outline sits behind the sprite: sprite over outline
        #ifdef SPRITE_FX_GROWN
            // the drop shadow's grown border: the taps from where the fragment really is
            float outline = outlineAlpha(fxInFrame ? uv : v_uv1, texelSize, inset) * customParams.outlineColor.a * tint.a;
        #else
        float outline = outlineAlpha(uv, texelSize, inset) * customParams.outlineColor.a * tint.a;
        #endif
        float alpha = color.a + outline * (1.0 - color.a);
        color.rgb = (color.rgb * color.a + customParams.outlineColor.rgb * outline * (1.0 - color.a)) / max(alpha, 1e-5);
        color.a = alpha;
    #endif

    #if defined(DROP_SHADOW) && defined(SPRITE_FX)
        if (color.a < 1.0){
            // the sprite's shape (after the dissolve) moved by the offset, behind it and its outline
            ivec2 offset = ivec2(floor(customParams.shadowOffset + 0.5));
            if (fxOpaque(fxPos - ivec2(offset.x, -offset.y)) > 0.5){
                vec4 shadow = customParams.shadowColorFrom > 0.5 ? vec4(fxRamp(0), 1.0) : customParams.shadowColor;
                float shade = shadow.a * tint.a;
                #ifdef DITHER_FADE
                    // dithered, not blended
                    shade = shade > fxDitherThreshold(fxCell) ? 1.0 : 0.0;
                #endif
                float alpha = color.a + shade * (1.0 - color.a);
                color.rgb = (color.rgb * color.a + shadow.rgb * shade * (1.0 - color.a)) / max(alpha, 1e-5);
                color.a = alpha;
            }
        }
    #endif

    // the material's alpha mask cuts what is left, the outline included
    color = applyMaterialAlphaMode(color);

    #ifdef USE_LIGHT2D
        // the normal map (the material's Normal Texture, OpenGL green) at the colour's UV,
        // in-frame clamped with OUTLINE or OVERLAY
        color.rgb *= getLight2DIntensity(v_position, getNormal2DAt(uv), true);
    #endif

    #ifdef GLOW
        // emission: not darkened by the 2D lights
        color.rgb += alphaGlow(spriteAlpha, customParams.glowScale, customParams.glowPower, customParams.glowColor.rgb);
    #endif

    #if defined(DITHER_FADE) && defined(SPRITE_FX)
        // last, so the whole look fades together: kept where alpha x (1 - amount) is above the
        // texel's threshold, else discarded (never a partial alpha)
        float fade = customParams.ditherFadeAmount + spriteFx(z);
        if (fade > 0.0 && color.a * (1.0 - fade) <= fxDitherThreshold(fxCell))
            discard;
    #endif

    g_finalColor = finalizeStraightColor(color);
}
