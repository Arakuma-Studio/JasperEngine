#version 450

// @shader mesh
// @name "Toon"
// @category "Stylized"
// @description "Cel shading: the light falls in hard bands (or follows a ramp texture), the unlit side takes a shadow colour, with optional hard specular highlight and rim light. Receives shadows."
// @lighting pixel mixed vertex
// @require uv1 color
// @require position normals feature=RIM
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property shadowColor color (0.42, 0.38, 0.62, 1) "Shadow colour (unlit side fill)"
// @property bands range(1, 8) 2 "Bands"
// @property bandSmoothness range(0, 1) 0.08 "Band edge width"
// @property specularColor hdrcolor (1, 1, 1, 1) "Specular colour" feature=SPECULAR
// @property specularSize range(0, 1) 0.3 "Specular size" feature=SPECULAR
// @property specularSmoothness range(0, 1) 0.15 "Specular edge width" feature=SPECULAR
// @property rimColor hdrcolor (0.75, 0.75, 0.75, 1) "Rim colour" feature=RIM
// @property rimPower range(0.5, 16) 4 "Rim power" feature=RIM
// @property rimSmoothness range(0, 1) 0.1 "Rim edge width" feature=RIM
// @property rimLitOnly toggle 1 "Rim on the lit side only" feature=RIM
// @property stochasticCellSize range(0.25, 4) 1 "Cell Size" feature=STOCHASTIC
// @property stochasticBlendSharpness range(0, 1) 0.4 "Blend Sharpness" feature=STOCHASTIC
// @property stochasticContrast range(0, 1) 0.45 "Contrast" feature=STOCHASTIC
// @property stochasticRotation range(0, 1) 0 "Rotation" feature=STOCHASTIC
// @texture u_baseTexture white "Base map"
// @texture u_rampTexture white "Ramp (left unlit, right lit)" feature=RAMP
// @feature RAMP "Ramp texture"
// @feature SPECULAR "Specular highlight"
// @feature RIM "Rim light"
// @feature STOCHASTIC "Stochastic Tiling"

// Per pixel light (pixel, mixed), with x = max(n.l, 0) * shadow:
//     light += radiance / PI * band(x)
// band(x) steps x into `bands` levels (any lit x reaches the first one) with smooth edges
// `bandSmoothness` of a band wide, or with RAMP is the ramp's colour at x. The shadow
// colour fills the unlit side: albedo * shadowColor where no light reaches the first band.
// Which side is lit follows n.l and the light's reach (its range window and spot cone),
// not its distance falloff: a point light's bands stay as crisp far from it as near, only
// dimmer (the radiance falls off). The SH ambient adds smoothly; vertex lights (mixed) add
// their smooth diffuse.
// In vertex mode the vertex stage (toon.vert) also sums each vertex light's full radiance
// (as if facing it) and reach; their interpolated ratio is the vertex lights' n.l, banded
// as above and times that radiance, so one light is as bright per vertex as per pixel.
// SPECULAR is a hard Blinn highlight of the pixel lights; RIM adds rimColor on
// silhouettes, optionally only on the lit side. Without scene lights the variant is
// unlit: the base colour (and rim). Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 shadowColor;
    vec4 specularColor;
    vec4 rimColor;
    float bands;
    float bandSmoothness;
    float specularSize;
    float specularSmoothness;
    float rimPower;
    float rimSmoothness;
    float rimLitOnly;
    #ifdef STOCHASTIC
        float stochasticCellSize;
        float stochasticBlendSharpness;
        float stochasticContrast;
        float stochasticRotation;
    #endif
} customParams;

#ifdef STOCHASTIC
    #include "library/stochastic_surface.glsl"
#endif

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#ifdef RAMP
    uniform texture2D u_rampTexture;
    uniform sampler u_ramp_smp;
#endif

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the instance colour

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
    #ifndef USE_PUNCTUAL
        in vec3 v_vertexAmbient;
        // rgb the vertex lights' radiance / PI facing them, a their reach (toon.vert)
        in vec4 v_toonPeak;
    #endif
#endif

#if defined(USE_PUNCTUAL) || defined(RIM)
    // the world position and normal, for the pixel lights (always written for a pixel-lit
    // variant) and the rim (@require position normals feature=RIM)
    #define TOON_SURFACE
    in vec3 v_position;
    in vec3 v_normal;
#endif
#if defined(RIM) || (defined(USE_PUNCTUAL) && defined(SPECULAR))
    // the direction to the camera, for the highlight and the rim
    #define TOON_VIEW
    #define FRAME_FS
    #include "includes/frame.glsl"
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

#if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
float bandCount(){
    return max(floor(customParams.bands + 0.5), 1.0);
}

// how much of a light with lambert term x in [0, 1] the surface takes
vec3 toonBand(float x){
    #ifdef RAMP
        // level 0: no derivatives inside the light loop (HLSL unrolls it otherwise)
        return sRGBToLinear(textureLod(sampler2D(u_rampTexture, u_ramp_smp), vec2(clamp(x, 0.01, 0.99), 0.5), 0.0).rgb);
    #else
        float count = bandCount();
        float t = clamp(x, 0.0, 1.0) * count;
        float level = floor(t) + smoothstep(0.0, max(customParams.bandSmoothness, 1e-3), fract(t));
        return vec3(min(level / count, 1.0));
    #endif
}

// 0 where x is unlit, 1 from the first band on
float toonLit(float x){
    return smoothstep(0.0, max(customParams.bandSmoothness, 1e-3) / bandCount(), x);
}
#endif

#ifdef USE_PUNCTUAL
// how far pixel light i reaches the position, 0 to 1: its attenuation without the inverse
// square falloff (the range window, the spot cone or mask), 1 for a directional light
float toonReach(int i, float attenuation){
    Light light = getLightData(i);
    if (light.type == LightType_Directional){
        return attenuation;
    }
    vec3 toLight = light.position - v_position;
    return min(attenuation * dot(toLight, toLight), 1.0);
}
#endif

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    #ifdef STOCHASTIC
        // the base map's taps set the weights the material's other maps reuse
        StochasticTiling stochastic = stochasticSetup(uv, stochasticCustomParams(), 0u);
        vec4 base = customParams.baseColor * sRGBToLinear(stochasticTextureLuminance(u_baseTexture, u_base_smp, stochastic)) * v_color;
    #else
        vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    #endif

    #ifdef TOON_SURFACE
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing){
            n = -n;
        }
    #endif
    #ifdef TOON_VIEW
        vec3 toCamera = (frame.cameraPosition.w > 0.5) ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    #endif

    vec3 color = base.rgb;
    float lit = 1.0;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 light = vec3(0.0);
        vec3 specular = vec3(0.0);
        lit = 0.0;

        #ifdef USE_PUNCTUAL
            light += ambientLight(n);
            #ifdef SPECULAR
                float specularCos = 1.0 - 0.2 * customParams.specularSize * customParams.specularSize;
                float specularEdge = max(customParams.specularSmoothness * (1.0 - specularCos), 1e-5);
            #endif
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample s = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                float x = max(dot(n, s.direction), 0.0) * shadow;
                float onLit = toonLit(x);
                vec3 radiance = s.radiance * LIGHT_INV_PI;
                light += radiance * toonBand(x);
                lit += onLit * toonReach(i, s.attenuation);
                #ifdef SPECULAR
                    float NdotH = dot(n, normalize(s.direction + toCamera));
                    specular += radiance * (smoothstep(specularCos - specularEdge, specularCos + specularEdge, NdotH) * onLit);
                #endif
            }
            #ifdef USE_VERTEX_LIGHTS
                // mixed: the vertex lights' smooth diffuse
                light += v_vertexLight;
            #endif
        #else
            // vertex mode: the vertex lights' n.l is their diffuse over their radiance
            // facing them; banded as per pixel, times that radiance
            const vec3 luminance = vec3(0.2126, 0.7152, 0.0722);
            float x = clamp(dot(v_vertexLight, luminance) / max(dot(v_toonPeak.rgb, luminance), 1e-6), 0.0, 1.0);
            light = v_vertexAmbient + v_toonPeak.rgb * toonBand(x);
            lit = toonLit(x) * v_toonPeak.a;
        #endif

        lit = clamp(lit, 0.0, 1.0);
        color = base.rgb * (light + customParams.shadowColor.rgb * (1.0 - lit));
        #ifdef SPECULAR
            color += customParams.specularColor.rgb * specular;
        #endif
    #endif

    #ifdef RIM
        float rimEdge = 0.5 * customParams.rimSmoothness + 1e-3;
        float rim = smoothstep(0.5 - rimEdge, 0.5 + rimEdge, pow(max(1.0 - dot(n, toCamera), 0.0), customParams.rimPower));
        rim *= mix(1.0, lit, customParams.rimLitOnly);
        color += customParams.rimColor.rgb * rim;
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
