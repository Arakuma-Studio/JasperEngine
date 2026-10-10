#version 450

// @shader mesh
// @name "Water"
// @category "Nature"
// @description "Stylised water without scene depth: shallow to deep colour by the view angle, two scrolling normal layers and foam anchored to the world, optional sine waves; lit per pixel with a specular glint. Alpha blended."
// @lighting none pixel
// @require position normals
// @state blend=alpha
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
// @texture u_normalTexture normal "Ripple normal map"
// @texture u_noiseTexture gray "Foam noise (red)"
// @feature WAVES "Waves"
// @depth

// Made for a horizontal surface (a plane, tessellated for WAVES). Both layers sample the
// normal map and the foam noise at the world XZ divided by their size, so the pattern is
// the same size whatever the mesh's scale and continues across neighbouring tiles. Foam
// shows where the two noise layers together exceed 1 - foamAmount (more on wave crests).
// Without scene depth there is no shoreline foam (phase 3). Pixel lighting: Lambert from
// the ambient and the pixel lights on the water and foam colour, plus a Blinn-Phong glint
// of each pixel light (shadowed), which also makes the water more opaque. Light units:
// includes/lights.glsl.

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
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

#ifdef WAVES
    #include "library/water.glsl"
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
        crest = clamp(wave.x / max(customParams.waveHeight, 1e-4), 0.0, 1.0) * 0.3;
    #endif

    vec2 uv1 = waterLayerUV(xz, customParams.layerScale, customParams.layerSpeed);
    vec2 uv2 = waterLayerUV(xz, customParams.secondLayerScale, customParams.secondLayerSpeed);

    // the two ripple layers bend the normal in the plane (map x along world X, y along Z)
    vec2 ripple = texture(sampler2D(u_normalTexture, u_normal_smp), uv1).xy +
                  texture(sampler2D(u_normalTexture, u_normal_smp), uv2).xy - vec2(1.0);
    n = normalize(n + vec3(ripple.x, 0.0, ripple.y) * (2.0 * customParams.normalStrength));

    vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    float fresnel = pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), customParams.fresnelPower);
    vec4 water = mix(customParams.shallowColor, customParams.deepColor, fresnel);

    float noise = 0.5 * (texture(sampler2D(u_noiseTexture, u_noise_smp), uv1).r +
                         texture(sampler2D(u_noiseTexture, u_noise_smp), uv2 * 1.37).r) + crest;
    float threshold = 1.0 - customParams.foamAmount;
    float foam = smoothstep(threshold, threshold + customParams.foamSoftness, noise) * step(0.001, customParams.foamAmount);
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

    g_finalColor = finalizeColor(vec4(color, alpha));
}
