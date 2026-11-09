// The fragment stage of particle systems (shader type particles): the inputs
// includes/particles_vs.glsl writes, the particle texture with frame blending, the lighting
// (LIT per vertex, SIXWAY the two response maps with the main light per pixel), the main
// light's shadow (RECEIVE_SHADOWS) and the emission. particleSurfaceColor() is the linear,
// straight-alpha colour of the fragment (its coverage in alpha); a library shader adds its effects to it and writes it
// through includes/output.glsl's finalizeStraightColor (it has a BLEND_* define); the built-in
// particles.frag shapes it for the blend in particlesFsParams.
//
// Six-way lighting follows OpenEngine's particle_six_way.glsl (Engine/Modules/Rendering/Shaders/
// Includes/particle_six_way.glsl), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License (see NOTICE.md): the three map layouts and the squared-direction
// response.

#ifndef PARTICLES_FS_GLSL
#define PARTICLES_FS_GLSL

#define FRAME_FS
#include "includes/frame.glsl"
#include "includes/srgb.glsl"

in vec2 v_uv;
in vec4 v_color;
#ifdef FRAME_BLEND
    in vec2 v_uvNext;
    in float v_frameBlend;
#endif
#if defined(LIT) || defined(SIXWAY)
    in vec3 v_light;
#endif
#if defined(LIT) && !defined(SIXWAY)
    in vec3 v_mainLight;
#endif
#ifdef SIXWAY
    in vec3 v_basisRight;
    in vec3 v_basisUp;
    in vec3 v_basisNormal;
#endif
in float v_emissive;
#ifdef CUSTOM_DATA
    in vec4 v_custom;
#endif
#ifdef RECEIVE_SHADOWS
    in vec3 v_worldPosition;
#endif

uniform u_fs_particlesParams {
    vec4 blend;           // x the blend of the built-in shader: 0 alpha, 1 premultiplied, 2 additive, 3 multiply; y a soft disc without texture
    vec4 emissionColor;   // rgb the emission colour (linear), w 1 with an emission map
    vec4 sixWay;          // x layout (0 RGBA, 1 top/left/right + bottom/back/front, 2 positive/negative), y contrast
    #ifdef PARTICLE_PIXEL
        // pixel particles (S49, includes/particles_pixel.glsl)
        vec4 pixelFade;       // x fade (0 Dither, 1 Cutout, 2 Alpha), y dither size, z dither space (0 Screen, 1 Particle), w secondary (0 Ramp Offset, 1 Color)
        vec4 pixelSecondary;  // rgb the secondary colour (sRGB), w the ramp offset (signed, after Reverse)
        vec4 pixelShape;      // x the shape (-1: a billboard), y Ring Width, z Glow Bands, w flags (1 Rim, 2 Shade Bottom)
        vec4 pixelStreak;     // x Head Fraction, y Width
        vec4 pixelInfo;       // x the ramp's colours (0: none)
        vec4 pixelRamp[32];   // the ramp, sRGB
    #endif
} particlesFsParams;

#ifdef PARTICLE_PIXEL
    #include "includes/particles_pixel.glsl"
#endif

#ifdef HAS_TEXTURE
    uniform texture2D u_particleTexture;
    uniform sampler u_particle_smp;
#endif
uniform texture2D u_particleEmission;
uniform sampler u_particleEmission_smp;
#ifdef SIXWAY
    uniform texture2D u_particleSixWayA;
    uniform sampler u_particleSixWayA_smp;
    uniform texture2D u_particleSixWayB;
    uniform sampler u_particleSixWayB_smp;
#endif
#ifdef RECEIVE_SHADOWS
    #include "includes/shadow_sampling.glsl"
#endif

// the particle texture at the fragment, the next frame blended in (linear, straight alpha)
vec4 particleTexel(){
    #ifdef HAS_TEXTURE
        vec4 texel = sRGBToLinear(texture(sampler2D(u_particleTexture, u_particle_smp), v_uv));
        #ifdef FRAME_BLEND
            vec4 next = sRGBToLinear(texture(sampler2D(u_particleTexture, u_particle_smp), v_uvNext));
            texel = mix(texel, next, v_frameBlend);
        #endif
        return texel;
    #else
        #if !defined(PARTICLE_MESH) && !defined(PARTICLE_TRAIL)
            // no texture: Unity's default particle, a soft disc (blend.y; the old points' square without it)
            if (particlesFsParams.blend.y > 0.5){
                float edge = clamp(1.0 - length(v_uv * 2.0 - 1.0), 0.0, 1.0);
                return vec4(1.0, 1.0, 1.0, edge * edge * (3.0 - 2.0 * edge));
            }
        #endif
        return vec4(1.0);
    #endif
}

// the main light's shadow at the fragment (1 lit)
float particleShadow(){
    #ifdef RECEIVE_SHADOWS
        return sampleDirectionalShadow1(v_worldPosition);
    #else
        return 1.0;
    #endif
}

#ifdef SIXWAY
// the six responses of the two maps: +X, +Y, +Z and -X, -Y, -Z of the sprite (+Z its front)
void particleSixWayDecode(vec4 a, vec4 b, out vec3 positive, out vec3 negative, out float opacity, out float emission){
    int mapLayout = int(particlesFsParams.sixWay.x + 0.5);
    if (mapLayout == 1){
        // map A top, left, right; map B bottom, back, front
        positive = vec3(a.b, a.r, b.b);
        negative = vec3(a.g, b.r, b.g);
        opacity = a.a;
        emission = 0.0;
    }else if (mapLayout == 2){
        positive = a.rgb;
        negative = b.rgb;
        opacity = a.a;
        emission = b.a;
    }else{
        // packed: back in A.b, front in B.b
        positive = vec3(a.r, a.g, b.b);
        negative = vec3(b.r, b.g, a.b);
        opacity = a.a;
        emission = b.a;
    }
    positive = max(positive, vec3(0.0));
    negative = max(negative, vec3(0.0));
}

// the response to a light from a direction in the sprite's frame: the squared direction weights
// sum to one, each signed axis picks one map channel
float particleSixWayResponse(vec3 positive, vec3 negative, vec3 direction){
    vec3 d = direction * inversesqrt(max(dot(direction, direction), 1e-12));
    vec3 response = mix(negative, positive, vec3(greaterThanEqual(d, vec3(0.0))));
    return dot(response, d * d);
}
#endif

// the fragment's colour, linear and straight alpha: texture times particle colour, lit, plus
// its emission; its alpha is the coverage (the texture's, six-way: the opacity map's), which
// the shader multiplies by the particle's alpha (v_color.a) or dissolves by it
vec4 particleSurfaceColor(){
    #ifdef PARTICLE_PIXEL
        // Render Mode Pixel: the shape per base pixel, no texture, unlit (S49 R9)
        if (particlesFsParams.pixelShape.x > -0.5){
            vec4 shaped = pixelShapeColor(v_color.rgb);
            shaped.rgb += particlesFsParams.emissionColor.rgb * v_emissive;
            return shaped;
        }
    #endif
    vec4 texel = particleTexel();
    vec4 color = vec4(texel.rgb * v_color.rgb, texel.a);
    vec3 emission = particlesFsParams.emissionColor.rgb * v_emissive;
    if (particlesFsParams.emissionColor.w > 0.5)
        emission *= sRGBToLinear(texture(sampler2D(u_particleEmission, u_particleEmission_smp), v_uv).rgb);

    #ifdef SIXWAY
        vec4 mapA = texture(sampler2D(u_particleSixWayA, u_particleSixWayA_smp), v_uv);
        vec4 mapB = texture(sampler2D(u_particleSixWayB, u_particleSixWayB_smp), v_uv);
        vec3 positive, negative;
        float opacity, mapEmission;
        particleSixWayDecode(mapA, mapB, positive, negative, opacity, mapEmission);
        float contrast = max(particlesFsParams.sixWay.y, 0.0001);
        positive = pow(positive, vec3(contrast));
        negative = pow(negative, vec3(contrast));
        // the main light per pixel, in the sprite's frame
        vec3 l = frame.mainLightDirection.xyz;
        vec3 local = vec3(dot(l, v_basisRight), dot(l, v_basisUp), dot(l, v_basisNormal));
        vec3 lighting = frame.mainLightColor.rgb * frame.mainLightDirection.w * particleSixWayResponse(positive, negative, local) * particleShadow();
        // the ambient and the other lights: the mean response
        lighting += v_light * (dot(positive + negative, vec3(1.0)) / 6.0);
        color.rgb = v_color.rgb * lighting;
        color.a = opacity;
        emission += particlesFsParams.emissionColor.rgb * mapEmission * v_emissive;
    #elif defined(LIT)
        color.rgb *= v_light + v_mainLight * particleShadow();
    #endif

    color.rgb += emission;
    #ifdef PARTICLE_PIXEL
        // a billboard of the Pixel Art module: its coverage through the dither or the cutout (R4)
        color = pixelTexelColor(color);
    #endif
    return color;
}

#endif
