#version 450

// The library vertex path (includes/surface_vs.glsl), plus what sprite.frag needs:
// the world position for 2D lights, and with OUTLINE, OVERLAY or a Pixel FX feature (S48) the
// frame rect, the sprite's texture rect (times the instance's) in the texture's UV space, so
// effects can stay inside the current atlas frame. A grown quad (pixel outline, drop shadow) is
// built by the engine in the sprite's geometry (@expand), not here.

// as sprite.frag
#if defined(OUTLINE_STYLE_PIXEL4) || defined(OUTLINE_STYLE_PIXEL8) || defined(DROP_SHADOW) || defined(DISSOLVE) || defined(RAMP_RECOLOR) || defined(DITHER_FADE)
    #define SPRITE_FX_TEXELS
#endif

#if (defined(OUTLINE) || defined(OVERLAY) || defined(SPRITE_FX_TEXELS)) && defined(HAS_UV_SET1)
    #define SPRITE_FRAME_RECT
#endif

#ifdef USE_LIGHT2D
    #define SURFACE_POSITION
#endif

#ifdef SPRITE_FRAME_RECT
    // the hook runs once per vertex with the rect uniforms in scope; it does not move
    // the vertex, so the engine's depth program still matches
    #define SURFACE_DISPLACE
#endif

#include "includes/surface_vs.glsl"

#ifdef SPRITE_FRAME_RECT
    flat out vec4 v_frameRect; // xy origin, zw size (may be negative for a flipped rect)
    #if defined(DITHER_FADE) && defined(PIXEL_SNAP)
        // S41's grid for a screen-space dither: screen pixels per base pixel (frame.pixelSnap.w)
        flat out float v_pixelScale;
    #endif

    vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
        vec4 rect = vec4(0.0, 0.0, 1.0, 1.0);
        #ifdef HAS_UV_SET1
            #ifdef HAS_TEXTURERECT
                rect = spriteParams.textureRect;
            #endif
            #ifdef HAS_INSTANCING
                rect = vec4(rect.xy * i_textureRect.zw + i_textureRect.xy, rect.zw * i_textureRect.zw);
            #endif
        #endif
        v_frameRect = rect;
        #if defined(DITHER_FADE) && defined(PIXEL_SNAP)
            v_pixelScale = max(frame.pixelSnap.w, 1.0);
        #endif
        return objectPosition;
    }
#endif
