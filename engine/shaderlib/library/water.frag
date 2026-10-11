#version 450

// @shader mesh
// @name "Water"
// @category "Nature"
// @description "Stylised water: shallow to deep colour by the view angle, two scrolling normal layers and foam anchored to the world, optional sine waves; lit per pixel with a specular glint. Optional scene-reading features: DEPTH_FOAM (foam along every shore and object crossing the surface) and ABSORPTION (colour by the water's thickness) read the scene depth, REFRACTION (the bed and anything under the surface bent by the ripples) the scene colour and depth. Mobile cost of those: the scene depth is an extra depth-only pass of the opaque meshes (none when SSAO or SSR already made one), the scene colour a copy of the screen (half resolution by default) and a split of the render pass. Alpha blended; casts no shadows."
// @lighting none pixel
// @require position normals
// @require scenedepth feature=DEPTH_FOAM
// @require scenedepth feature=ABSORPTION
// @require scenecolor scenedepth feature=REFRACTION
// @state blend=alpha shadows=off
// @property shallowColor color (0.25, 0.8, 0.85, 0.5) "Shallow colour (looking down)"
// @property deepColor color (0.03, 0.25, 0.45, 0.92) "Deep colour (grazing view)"
// @property fresnelPower range(0.5, 8) 3 "Fresnel power"
// @property normalStrength range(0, 2) 0.6 "Ripple strength"
// @property layerScale float 4 "Ripple size (world units per tile)"
// @property layerSpeed vector2 (0.12, 0.05) "Ripple drift (world units per second)"
// @property secondLayerScale float 2.6 "Second ripple size (world units per tile)"
// @property secondLayerSpeed vector2 (-0.05, 0.1) "Second ripple drift (world units per second)"
// @property foamColor color (1, 1, 1, 1) "Foam colour"
// @property foamAmount range(0, 1) 0.2 "Foam amount"
// @property foamSoftness range(0.001, 0.5) 0.06 "Foam softness"
// @property specularColor color (1, 1, 1, 1) "Specular colour (per pixel lights)"
// @property gloss range(0, 1) 0.85 "Glossiness (per pixel lights)"
// @property waveDirection vector2 (1, 0.3) "Wave direction (world XZ)" feature=WAVES
// @property waveHeight float 0.06 "Wave height (world units)" feature=WAVES
// @property waveLength float 2.5 "Wave length (world units)" feature=WAVES
// @property waveSpeed float 0.8 "Wave speed (world units per second)" feature=WAVES
// @property shoreFoamWidth float 0.12 "Shore foam width (world units of depth)" feature=DEPTH_FOAM
// @property shoreFoamBreakup range(0, 1) 0.7 "Shore foam break-up (by the foam noise)" feature=DEPTH_FOAM
// @property absorptionDepth float 1.5 "Deep colour thickness (world units of water along the view)" feature=ABSORPTION
// @property shoreFade float 0.1 "Shore fade (world units of water)" feature=ABSORPTION
// @property refractionStrength range(0, 0.2) 0.04 "Refraction (screen UV per unit of ripple)" feature=REFRACTION
// @property refractionDepth float 0.5 "Full refraction from this thickness (world units)" feature=REFRACTION
// @texture u_normalTexture normal "Ripple normal map"
// @texture u_noiseTexture gray "Foam noise (red)"
// @feature WAVES "Waves"
// @feature DEPTH_FOAM "Shore foam (reads the scene depth)"
// @feature ABSORPTION "Depth colour (reads the scene depth)"
// @feature REFRACTION "Refraction (reads the scene colour and depth)"
// @depth

// Made for a horizontal surface (a plane, tessellated for WAVES). Both layers sample the
// normal map and the foam noise at the world XZ divided by their size, so the pattern is
// the same size whatever the mesh's scale and continues across neighbouring tiles. Foam
// shows where the two noise layers together exceed 1 - foamAmount (more on wave crests).
// Pixel lighting: Lambert from the ambient and the pixel lights on the water and foam
// colour, plus a Blinn-Phong glint of each pixel light (shadowed), which also makes the
// water more opaque. Light units: includes/lights.glsl. No shadows (@state shadows=off):
// the surface is see-through, so the bed below stays lit.
//
// Scene features (includes/scene.glsl; with all three off the shader is the phase 1 water,
// pixel for pixel). The water's thickness is the length of the view ray between the
// surface and the opaque scene behind it (the scene depth gap divided by the cosine of the
// ray to the camera's axis); its depth below the surface is that times |n.v|, n the
// surface's normal without the ripples.
//   DEPTH_FOAM: foam where the depth below the surface is under shoreFoamWidth, dense at
//     the contact line and thinning out, holes cut into it by the foam noise
//     (shoreFoamBreakup), so it hugs every shore and every object crossing the surface. Vertical faces seen from straight above leave no gap in
//     the depth, so no foam there (the limit of any depth-based foam).
//   ABSORPTION: shallow to deep colour by the thickness (1 - exp(-thickness /
//     absorptionDepth), at least the fresnel term of the phase 1 colour), and the water's
//     alpha fading in over the first shoreFade units, so the shore line is soft.
//   REFRACTION: the scene colour behind the water at the screen position shifted by the
//     ripples' bend (view space, so it follows the camera), refractionStrength screen UV per
//     unit; the shift grows over the first refractionDepth units of thickness (none at the
//     shore line), never reaches past the screen's border (no smeared edge), and a shifted
//     sample landing on something in front of the water falls back to the straight one. The
//     water is drawn over that colour (its alpha used to mix) and written with alpha 1, the
//     fog applied to the water's own colour only (the scene colour has its fog already).
//     With ABSORPTION the thickness of the shifted sample sets the colour.

uniform u_fs_customParams {
    vec4 shallowColor;
    vec4 deepColor;
    vec4 foamColor;
    vec4 specularColor;
    vec2 layerSpeed;
    vec2 secondLayerSpeed;
    vec2 waveDirection;
    float fresnelPower;
    float normalStrength;
    float layerScale;
    float secondLayerScale;
    float foamAmount;
    float foamSoftness;
    float gloss;
    float waveHeight;
    float waveLength;
    float waveSpeed;
    float shoreFoamWidth;
    float shoreFoamBreakup;
    float absorptionDepth;
    float shoreFade;
    float refractionStrength;
    float refractionDepth;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

#ifdef WAVES
    #include "library/water.glsl"
#endif

#if defined(DEPTH_FOAM) || defined(ABSORPTION) || defined(REFRACTION)
    #define WATER_THICKNESS
#endif

uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;
uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;

in vec3 v_position;
in vec3 v_normal;

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

// world XZ to a layer's UV: size in world units, drift in world units per second (the
// offset wraps every tile, so a long run keeps its precision)
vec2 waterLayerUV(vec2 xz, float size, vec2 drift){
    float scale = 1.0 / max(size, 1e-3);
    return xz * scale + fract(frame.time.x * drift * scale);
}

void main() {
    vec2 xz = v_position.xz;
    vec3 n = normalize(v_normal);
    float crest = 0.0;

    #ifdef WAVES
        // the waves' slope, as the vertex stage raised them
        vec3 wave = waterWaves(xz, frame.time.x, customParams.waveDirection, customParams.waveHeight,
                               customParams.waveLength, customParams.waveSpeed);
        n = normalize(n - vec3(wave.y, 0.0, wave.z));
        crest = clamp(wave.x / max(customParams.waveHeight, 1e-4), 0.0, 1.0) * 0.15;
    #endif

    #if defined(WATER_THICKNESS)
        // the surface's normal without the ripples
        vec3 surfaceNormal = n;
    #endif

    vec2 uv1 = waterLayerUV(xz, customParams.layerScale, customParams.layerSpeed);
    vec2 uv2 = waterLayerUV(xz, customParams.secondLayerScale, customParams.secondLayerSpeed);

    // the two ripple layers bend the normal in the plane (map x along world X, y along Z)
    vec2 ripple = texture(sampler2D(u_normalTexture, u_normal_smp), uv1).xy +
                  texture(sampler2D(u_normalTexture, u_normal_smp), uv2).xy - vec2(1.0);
    n = normalize(n + vec3(ripple.x, 0.0, ripple.y) * (2.0 * customParams.normalStrength));

    vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    float fresnel = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), customParams.fresnelPower);

    #ifdef WATER_THICKNESS
        vec2 screen = screenUV();
        float surfaceDepth = fragmentLinearDepth();
        // view-ray length per unit of view depth (1 for an orthographic camera)
        float rayScale = 1.0 / max(dot(-v, frame.cameraDirection.xyz), 0.05);
        // the water along the view ray, between the surface and the opaque scene behind it
        float thickness = max(sceneLinearDepth(screen) - surfaceDepth, 0.0) * rayScale;
        float seenThickness = thickness;
    #endif

    #ifdef REFRACTION
        // the ripples' bend seen from the camera: view-space x and y are the screen's
        // directions (screen UV is y up); x scaled by the aspect so the shift is round
        vec2 shift = (frame.viewMatrix * vec4(n - surfaceNormal, 0.0)).xy * customParams.refractionStrength;
        shift.x *= frame.resolution.y * frame.resolution.z;
        // none at the shore line, full from refractionDepth of water
        shift *= clamp(thickness / max(customParams.refractionDepth, 1e-3), 0.0, 1.0);
        // never past the screen's border, where the copy would repeat its edge
        vec2 room = min(screen, vec2(1.0) - screen);
        shift *= clamp(min(room.x, room.y) / max(2.0 * length(shift), 1e-5), 0.0, 1.0);
        vec2 refracted = screen + shift;
        float refractedDepth = sceneLinearDepth(refracted);
        if (refractedDepth < surfaceDepth){
            // something in front of the water: it is not under the surface
            refracted = screen;
        }else{
            seenThickness = (refractedDepth - surfaceDepth) * rayScale;
        }
        vec3 behind = sceneColor(refracted);
    #endif

    #ifdef ABSORPTION
        float deepness = max(1.0 - exp(-seenThickness / max(customParams.absorptionDepth, 1e-3)), fresnel);
        vec4 water = mix(customParams.shallowColor, customParams.deepColor, deepness);
        water.a *= smoothstep(0.0, max(customParams.shoreFade, 1e-4), seenThickness);
    #else
        vec4 water = mix(customParams.shallowColor, customParams.deepColor, fresnel);
        #ifdef REFRACTION
            // the tint grows in with the refraction, so the shore line is soft
            water.a *= clamp(thickness / max(customParams.refractionDepth, 1e-3), 0.0, 1.0);
        #endif
    #endif

    float foamNoise = 0.5 * (texture(sampler2D(u_noiseTexture, u_noise_smp), uv1).r +
                             texture(sampler2D(u_noiseTexture, u_noise_smp), uv2 * 1.37).r);
    float noise = foamNoise + crest;
    float threshold = 1.0 - customParams.foamAmount;
    float foam = smoothstep(threshold, threshold + customParams.foamSoftness, noise) * step(0.001, customParams.foamAmount);

    #ifdef DEPTH_FOAM
        // depth below the surface along its normal, in foam widths: 0 at the contact line
        float below = thickness * abs(dot(v, surfaceNormal)) / max(customParams.shoreFoamWidth, 1e-4);
        // dense at the contact line, thinning out to none at the width; the noise (its
        // contrast doubled) cuts holes into the thin part, so the band's edge breaks up
        float density = 1.0 - clamp(below, 0.0, 1.0);
        density *= density;
        float cut = 0.9 * customParams.shoreFoamBreakup * clamp(foamNoise * 2.0 - 0.5, 0.0, 1.0);
        foam = max(foam, smoothstep(cut, cut + 0.08, density - 0.02));
    #endif

    vec4 albedo = mix(water, customParams.foamColor, foam);

    vec3 color = albedo.rgb;
    float alpha = albedo.a;

    #ifdef USE_PUNCTUAL
        vec3 light = ambientLight(n);
        vec3 specular = vec3(0.0);
        float shininess = exp2(10.0 * customParams.gloss + 1.0);
        for (int i = 0; i < LIGHT_CAPACITY; i++){
            if (i >= pixelLightCount()) break;
            LightSample lightSample = getLight(i, v_position);
            float shadow = 1.0;
            #ifdef USE_SHADOWS
                shadow = lightShadow(i, v_position, n);
            #endif
            light += lambert(lightSample, n) * shadow;
            specular += blinnPhong(lightSample, n, v, shininess) * shadow;
        }
        specular *= customParams.specularColor.rgb * (1.0 - foam);
        color = color * light + specular;
        // a glint shows at full strength on clear water
        alpha = clamp(alpha + dot(specular, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    #endif

    #ifdef REFRACTION
        // the water over what is under it; only the water's own colour takes the fog
        g_finalColor = finalizeFoggedColor(vec4(mix(behind, applyFog(color), alpha), 1.0));
    #else
        g_finalColor = finalizeStraightColor(vec4(color, alpha));
    #endif
}
