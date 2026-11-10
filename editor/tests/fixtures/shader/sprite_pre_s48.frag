#version 450

// @shader mesh
// @name "Sprite"
// @category "2D"
// @description "Sprite, tilemap and polygon effects on the sprite's own texture and colour: outline, hue shift, hit flash, grayscale, colour replace, overlay and glow; lit by 2D lights, with the material's Normal Texture as the normal map. Alpha blended, both faces, no shadows; with the Premultiplied blend mode the texture is read as premultiplied."
// @require uv1 color normalmap
// @state blend=alpha zwrite=off cull=off shadows=off
// @batchable framerect=OUTLINE,OVERLAY
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

// Sprites keep the engine's own material: the texture and colour of the Sprite (or Tilemap,
// MeshPolygon), its texture rect (atlas frame), the vertex and instance colours. Palette
// Swap and Palette Cycling come first, on the raw texel (its exact bytes, before the tint:
// includes/palette.glsl); the other effects run on straight-alpha linear colour in this order: colour replace, hue shift,
// grayscale, overlay, flash, outline (behind the sprite), the material's alpha mode, 2D
// lights (shaped by the material's Normal Texture when it has one: @require normalmap), glow. With OUTLINE or OVERLAY the texture is read inside the current frame only.
// The blend mode is the material's (alpha by default): with Premultiplied (BLEND_PREMULTIPLIED)
// the texture is premultiplied, un-premultiplied for the effects and premultiplied again on
// output (finalizeStraightColor), as are the other modes' outputs. No shadows by default:
// the sprite's shape is its texture's alpha, and without a depth program a shadow would be
// the whole quad (as Unity's sprites, which cast none either).
// Dynamic Batching draws it (@batchable): the Palette Swap row and the flash amount are read with
// their FX slot added (includes/sprite_fx.glsl), the frame rect of OUTLINE and OVERLAY comes per
// vertex (sprite.vert).

#if (defined(OUTLINE) || defined(OVERLAY)) && defined(HAS_UV_SET1)
    #define SPRITE_FRAME_RECT
#endif

#ifdef USE_LIGHT2D
    #define SURFACE_POSITION
#endif

#if defined(HUE_SHIFT) || defined(FLASH) || defined(GRAYSCALE) || defined(OUTLINE) || defined(COLOR_REPLACE) || defined(OVERLAY) || defined(GLOW) || defined(PALETTE_SWAP) || defined(PALETTE_CYCLE)
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

#ifdef SURFACE_POSITION
    in vec3 v_position;
#endif
#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // vertex x instance colour
#ifdef SPRITE_FRAME_RECT
    flat in vec4 v_frameRect;
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

#if defined(OUTLINE) && defined(SPRITE_FRAME_RECT)
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

    vec4 texel = spriteTexel(uv);
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

    float spriteAlpha = color.a;

    #if defined(OUTLINE) && defined(SPRITE_FRAME_RECT)
        // the outline sits behind the sprite: sprite over outline
        float outline = outlineAlpha(uv, texelSize, inset) * customParams.outlineColor.a * tint.a;
        float alpha = color.a + outline * (1.0 - color.a);
        color.rgb = (color.rgb * color.a + customParams.outlineColor.rgb * outline * (1.0 - color.a)) / max(alpha, 1e-5);
        color.a = alpha;
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

    g_finalColor = finalizeStraightColor(color);
}
