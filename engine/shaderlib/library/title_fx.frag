#version 450

// @shader ui
// @name "Title FX"
// @category "2D"
// @description "The Title Art Renderer's live layers on its Image: a keyable sweep band, a glow pulse from the title's glow output, a tint, glint stars at the title's highlight anchors and the crossfade between two renders. Driven by the component (its Live Time, never engine time); not for hand use."
// @feature SWEEP "Sweep"
// @feature GLOW "Glow Pulse"
// @feature TINT "Tint"
// @feature GLINTS "Glints"
// @feature CROSSFADE "Crossfade"
// @feature ANIM "Animation"
// @property sweepParams vector4 (0, 0.15, 25, 0.6) "Sweep: position 0..1, width (fraction of the frame), angle (degrees clockwise), softness" feature=SWEEP
// @property sweepColor hdrcolor (1, 1, 1, 0.8) "Sweep colour (alpha: strength)" feature=SWEEP
// @property glowColor hdrcolor (1, 0.85, 0.4, 1) "Glow colour" feature=GLOW
// @property glowParams vector4 (0, 0, 0, 0) "Glow: pulse" feature=GLOW
// @property tintColor color (1, 1, 1, 1) "Tint colour (overlay blend)" feature=TINT
// @property tintParams vector4 (0, 0, 0, 0) "Tint: amount" feature=TINT
// @property glintColor hdrcolor (1, 1, 1, 1) "Glint colour" feature=GLINTS
// @property glintParams vector4 (1, 0.06, 0.5, 0) "Glints: amount, size (fraction of the frame height), rate, seed" feature=GLINTS
// @property glintInfo vector4 (0, 0, 1, 0) "Glints: anchor count, live time, frame aspect" feature=GLINTS
// @property crossfadeParams vector4 (1, 0, 0, 0) "Crossfade: the new image's share" feature=CROSSFADE
// @property animAffine0 vector4 (1, 0, 0, 1) "Animation: the fragment affine row 0 (identity: S85 moves the vertices), opacity" feature=ANIM
// @property animAffine1 vector4 (0, 1, 0, 0) "Animation: affine row 1, flipbook frame blend" feature=ANIM
// @property animColor vector4 (1, 1, 1, 0) "Animation: flash colour, flash amount" feature=ANIM
// @property frameRect vector4 (0, 0, 1, 1) "Animation: the frame's texture rect" feature=ANIM
// @property frameRect2 vector4 (0, 0, 1, 1) "Animation: the next frame's texture rect" feature=ANIM
// @property animVertex0 vector4 (1, 0, 0, 0) "Animation: the vertex affine row 0 (frame-normalised)" feature=ANIM
// @property animVertex1 vector4 (0, 1, 0, 0) "Animation: the vertex affine row 1" feature=ANIM
// @property animSpace vector4 (0, 0, 1, 1) "Animation: the frame in the element's own space (origin, size)" feature=ANIM
// @texture u_glowTexture black "The title's glow output" feature=GLOW
// @texture u_prevTexture black "The previous render (crossfade)" feature=CROSSFADE

// The title's image (lent by TitleArtSystem as the Image's texture: straight-alpha sRGB, the
// bake's encoding) through the live layers of includes/title_fx.glsl. The frame is the image's
// 0..1 UV range; anchors and sizes are frame-normalised, so they follow the Image's size.

#if defined(SWEEP) || defined(GLOW) || defined(TINT) || defined(GLINTS) || defined(CROSSFADE) || defined(ANIM)
// (no engine-written time: the live layers read the component's Live Time, R38)
uniform u_fs_customParams {
    #ifdef SWEEP
        vec4 sweepParams;
        vec4 sweepColor;
    #endif
    #ifdef GLOW
        vec4 glowColor;
        vec4 glowParams;
    #endif
    #ifdef TINT
        vec4 tintColor;
        vec4 tintParams;
    #endif
    #ifdef GLINTS
        vec4 glintColor;
        vec4 glintParams;
        vec4 glintInfo;
        vec4 anchors[16];
    #endif
    #ifdef CROSSFADE
        vec4 crossfadeParams;
    #endif
    #ifdef ANIM
        vec4 animAffine0;
        vec4 animAffine1;
        vec4 animColor;
        vec4 frameRect;
        vec4 frameRect2;
    #endif
} customParams;
#endif

#ifdef GLOW
    uniform texture2D u_glowTexture;
    uniform sampler u_glow_smp;
#endif
#ifdef CROSSFADE
    uniform texture2D u_prevTexture;
    uniform sampler u_prev_smp;
#endif

out vec4 g_finalColor;

#include "includes/ui_fs.glsl"
#include "includes/title_fx.glsl"
#include "includes/output.glsl"

vec4 titleTexel(vec2 uv){
    #ifdef HAS_TEXTURE
        return texture(sampler2D(u_uiTexture, u_ui_smp), uv);
    #else
        return vec4(1.0);
    #endif
}

void main() {
    #ifdef HAS_TEXTURE
        vec2 uv = v_uv1;
    #else
        vec2 uv = vec2(0.5);
    #endif
    // the frame in image pixels, for the aspect of the sweep and the stars
    #ifdef HAS_TEXTURE
        vec2 frameSize = vec2(textureSize(sampler2D(u_uiTexture, u_ui_smp), 0));
    #else
        vec2 frameSize = vec2(1.0);
    #endif

    vec4 texel;
    #ifdef ANIM
        vec2 source = titleFxAnimUV(uv);
        float inside = step(0.0, source.x) * step(source.x, 1.0) * step(0.0, source.y) * step(source.y, 1.0);
        vec4 a = titleTexel(customParams.frameRect.xy + source * customParams.frameRect.zw);
        vec4 b = titleTexel(customParams.frameRect2.xy + source * customParams.frameRect2.zw);
        texel = mix(a, b, customParams.animAffine1.w) * inside;
    #else
        texel = titleTexel(uv);
    #endif

    vec4 color = vec4(sRGBToLinear(texel.rgb), texel.a);
    #ifdef CROSSFADE
        // R17: old to new in premultiplied space
        vec4 previous = texture(sampler2D(u_prevTexture, u_prev_smp), uv);
        vec4 prev = vec4(sRGBToLinear(previous.rgb) * previous.a, previous.a);
        vec4 next = vec4(color.rgb * color.a, color.a);
        vec4 mixed = mix(prev, next, customParams.crossfadeParams.x);
        color = vec4(mixed.rgb / max(mixed.a, 1e-5), mixed.a);
    #endif

    #ifdef TINT
        color.rgb = titleFxTint(color.rgb);
    #endif
    #ifdef SWEEP
        color.rgb = titleFxSweep(uv, frameSize, color.rgb);
    #endif
    #ifdef ANIM
        // S85's flash: colour x amount added (a negative amount darkens), the opacity after
        color.rgb = max(color.rgb + customParams.animColor.rgb * customParams.animColor.a, vec3(0.0));
    #endif

    TitleFxLight light;
    light.color = vec3(0.0);
    light.coverage = 0.0;
    #ifdef GLOW
        titleFxGlow(texture(sampler2D(u_glowTexture, u_glow_smp), uv), light);
    #endif
    #ifdef GLINTS
        titleFxGlints(uv, frameSize, light);
    #endif
    color = titleFxCompose(color, light);
    #ifdef ANIM
        color.a *= customParams.animAffine0.w;
    #endif

    // the element's colour (a Canvas Group's fade, ui.color) and its vertex colour, as ui.frag
    color *= uiParams.color;
    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb *= v_color;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC4
        color *= v_color;
    #endif
    g_finalColor = finalizeColor(color);
}
