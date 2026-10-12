#version 450

// @shader mesh
// @name "Glass"
// @category "Effects"
// @description "Glass and clear plastic: the scene behind it bent by its normals (index of refraction and thickness, optional normal map), tinted and absorbed along the path through it, a fresnel reflection of the scene's environment (the reflection probe around it, else the sky's environment map, else the reflection colour; REFLECTION_MAP gives it a cubemap of its own instead), a glint of the per-pixel lights; BLUR frosts it. Reads the scene colour and depth: on mobile a copy of the screen (half resolution by default), a split of the render pass and an extra depth-only pass of the opaque meshes (none when SSAO or SSR already made one). Alpha blended (transparent queue), no depth write, casts no shadows. Transparent objects behind the glass are not seen through it (it shows the scene colour, which holds only opaque objects and the sky): keep them in front of it or make them opaque."
// @lighting none pixel
// @require normals position scenecolor scenedepth
// @require uv1 feature=NORMAL_MAP
// @require environment feature=!REFLECTION_MAP
// @state blend=alpha zwrite=off shadows=off
// @property tint color (0.92, 0.97, 1, 1) "Tint"
// @property ior range(1, 2.5) 1.45 "Index of refraction"
// @property thickness float 0.25 "Thickness (world units the view travels inside)"
// @property absorptionColor color (0.45, 0.8, 0.75, 1) "Absorption colour (thick glass turns to it)"
// @property absorption float 0 "Absorption (per world unit of path)"
// @property reflectionColor color (0.8, 0.86, 0.95, 1) "Reflection colour (times the environment or the cubemap; alone without either)"
// @property reflectionStrength range(0, 2) 1 "Reflection strength"
// @property specularColor color (1, 1, 1, 1) "Specular colour (per pixel lights)"
// @property gloss range(0, 1) 0.9 "Glossiness (per pixel lights)"
// @property normalStrength range(0, 2) 1 "Normal strength" feature=NORMAL_MAP
// @property tiling vector4 (1, 1, 0, 0) "Normal map tiling (xy) and offset (zw)" feature=NORMAL_MAP
// @property blurRadius range(0, 4) 1.5 "Blur radius (texels of the scene colour copy)" feature=BLUR
// @texture u_normalTexture normal "Normal map" feature=NORMAL_MAP
// @texture u_reflectionTexture black "Reflection cubemap" cube feature=REFLECTION_MAP
// @feature NORMAL_MAP "Normal map"
// @feature REFLECTION_MAP "Reflection cubemap (instead of the environment)"
// @feature BLUR "Frosted (blurred)"

// Refraction: the view ray I (camera to surface) bent by refract(I, n, 1 / ior) travels
// `thickness` world units inside; the screen shift of where it ends (the bend times the
// thickness, projected with the camera's projection at the fragment's depth, so it shrinks
// with distance and holds for orthographic cameras) moves the scene colour sample. A pane
// with flat faces shifts the view only at an angle; a sphere's curved front works as a lens
// (the image turns over). The shift never reaches past the screen's border (no smeared
// edge), and a shifted sample landing on something in front of the glass falls back to the
// straight one (scene depth).
// Transmission: scene colour * tint, times mix(1, absorptionColor, 1 - exp(-absorption *
// path)), the path being thickness / |n.v| (the rim of a ball and a pane seen at an angle
// absorb more). Reflection: Schlick's fresnel with f0 from the index of refraction (0.034
// for 1.45), reflectionStrength times reflectionColor times the environment in the
// reflected direction (@require environment, includes/environment.glsl: the reflection
// probe whose box holds the mesh, else the sky's prefiltered environment map, as the
// built-in Lit reflects; sharp, roughness 0); without an environment (no probe, a sky
// without a cube map) reflectionColor alone. REFLECTION_MAP replaces the environment by a
// cubemap of the material's own (linear from sRGB) and binds no environment. Pixel lights
// add a Blinn-Phong glint (shadowed). The result is written with alpha 1:
// the glass composes what is behind in the shader, as the engine's blends would draw three
// layers over it (display-encoded values, includes/output.glsl encodeColor): the
// transmission filter multiplied, the reflection mixed by the fresnel, the glint added. The
// fog: the reflection takes it; the filter fades to white and the glint to black with it
// (the scene colour has its fog), as a multiply and an additive layer do. The scene colour
// holds the opaque scene and the sky only, so transparent objects behind the glass are not
// seen through it (a tinted, bent view cannot be blended over the target instead). With the
// Additive blend the glass draws its reflection and glint only, with Multiply its
// transmission filter only (neither can replace what is behind). BLUR: four bilinear taps of the scene colour copy
// around the sample, blurRadius copy texels out (at the default half-resolution copy each
// tap already averages 2 x 2 pixels). NORMAL_MAP: a tangent-space normal map at uv1
// (tiling), its frame from screen-space derivatives (no tangents needed). Which side is seen
// (a back face, with cull off, is seen from inside) is decided by the mesh's normal before
// the map perturbs it, and a mapped normal tilting past the silhouette is pulled back to
// face the camera, so grazing views do not flip it pixel by pixel.

uniform u_fs_customParams {
    vec4 tint;
    vec4 absorptionColor;
    vec4 reflectionColor;
    vec4 specularColor;
    vec4 tiling;
    float ior;
    float thickness;
    float absorption;
    float reflectionStrength;
    float gloss;
    float normalStrength;
    float blurRadius;
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

#ifdef NORMAL_MAP
    uniform texture2D u_normalTexture;
    uniform sampler u_normal_smp;
    #include "includes/surface_uv.glsl"
    SURFACE_UV_IN vec2 v_uv1;
#endif

#ifdef REFLECTION_MAP
    uniform textureCube u_reflectionTexture;
    uniform sampler u_reflection_smp;
#endif

#include "includes/environment.glsl"

in vec3 v_position;
in vec3 v_normal;

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

#ifdef NORMAL_MAP
// the normal map's normal in world space, the tangent frame from the screen-space
// derivatives of the position and UV (as simple_lit)
vec3 perturbNormal(vec3 normal, vec2 uv){
    vec3 mapped = texture(sampler2D(u_normalTexture, u_normal_smp), uv).rgb * 2.0 - vec3(1.0);
    mapped.xy *= customParams.normalStrength;

    vec3 dpx = dFdx(v_position);
    vec3 dpy = dFdy(v_position);
    vec2 duvx = dFdx(uv);
    vec2 duvy = dFdy(uv);
    float det = duvx.x * duvy.y - duvy.x * duvx.y;
    if (abs(det) < 1e-12){
        return normal;
    }
    vec3 tangent = (duvy.y * dpx - duvx.y * dpy) / det;
    tangent = tangent - normal * dot(normal, tangent);
    float tangentLength = length(tangent);
    if (tangentLength < 1e-8){
        return normal;
    }
    tangent /= tangentLength;
    vec3 bitangent = cross(normal, tangent);
    return normalize(mat3(tangent, bitangent, normal) * mapped);
}
#endif

void main() {
    vec3 n = normalize(v_normal);
    bool orthographic = frame.cameraPosition.w > 0.5;
    vec3 v = orthographic ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
    // a back face (a material with cull off) is seen from inside: decided by the mesh's
    // normal, not the mapped one (which tilts past the silhouette at grazing views)
    float side = dot(n, v) < 0.0 ? -1.0 : 1.0;
    #ifdef NORMAL_MAP
        n = side * perturbNormal(n, v_uv1 * customParams.tiling.xy + customParams.tiling.zw);
        // a mapped normal tilting away from the camera is pulled back just past the
        // silhouette (continuous, no flip)
        n = normalize(n + v * max(0.01 - dot(n, v), 0.0));
    #else
        n *= side;
    #endif
    float nv = clamp(dot(n, v), 0.0, 1.0);

    vec2 screen = screenUV();
    float surfaceDepth = fragmentLinearDepth();

    // the bent view ray's end, thickness inside the glass, in view space, projected to a
    // screen UV shift (|P00|, |P11|: a flipped render-to-texture projection keeps screen UV y up)
    float eta = 1.0 / max(customParams.ior, 1e-3);
    vec3 bend = (refract(-v, n, eta) + v) * customParams.thickness;
    vec2 viewBend = (frame.viewMatrix * vec4(bend, 0.0)).xy;
    vec2 projection = vec2(abs(frame.projectionMatrix[0][0]), abs(frame.projectionMatrix[1][1]));
    vec2 shift = viewBend * projection * (0.5 / (orthographic ? 1.0 : max(surfaceDepth, 1e-3)));
    // never past the screen's border, where the copy would repeat its edge
    vec2 room = min(screen, vec2(1.0) - screen);
    shift *= clamp(min(room.x, room.y) / max(2.0 * length(shift), 1e-5), 0.0, 1.0);
    vec2 refracted = screen + shift;
    if (sceneLinearDepth(refracted) < surfaceDepth){
        // something in front of the glass is not behind it
        refracted = screen;
    }

    #ifdef BLUR
        // four taps on a rotated square around the sample
        vec2 texel = sceneInputs.colorParams.zw * (customParams.blurRadius / 1.58);
        vec3 behind = 0.25 * (sceneColor(refracted + texel * vec2(0.5, 1.5)) +
                              sceneColor(refracted + texel * vec2(-1.5, 0.5)) +
                              sceneColor(refracted + texel * vec2(-0.5, -1.5)) +
                              sceneColor(refracted + texel * vec2(1.5, -0.5)));
    #else
        vec3 behind = sceneColor(refracted);
    #endif

    // transmission filter: tint, then absorption along the path through the glass; it
    // filters the fogged scene colour, so the fog in front of the glass fades it to white
    float path = customParams.thickness / max(nv, 0.2);
    vec3 transmission = fadeByFog(customParams.tint.rgb *
                                  mix(vec3(1.0), customParams.absorptionColor.rgb, 1.0 - exp(-customParams.absorption * path)),
                                  vec3(1.0));

    // reflection, Schlick's fresnel with the index of refraction's f0
    float f0 = (customParams.ior - 1.0) / (customParams.ior + 1.0);
    f0 *= f0;
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0);
    vec3 reflection = customParams.reflectionColor.rgb * customParams.reflectionStrength;
    #if defined(REFLECTION_MAP)
        reflection *= sRGBToLinear(texture(samplerCube(u_reflectionTexture, u_reflection_smp), reflect(-v, n)).rgb);
    #elif defined(SURFACE_ENVIRONMENT)
        if (environmentPresent()){
            reflection *= environmentColor(reflect(-v, n), 0.0);
        }
    #endif

    vec3 glint = vec3(0.0);
    #ifdef USE_PUNCTUAL
        vec3 specular = vec3(0.0);
        float shininess = exp2(10.0 * customParams.gloss + 1.0);
        for (int i = 0; i < LIGHT_CAPACITY; i++){
            if (i >= pixelLightCount()) break;
            LightSample lightSample = getLight(i, v_position);
            float shadow = 1.0;
            #ifdef USE_SHADOWS
                shadow = lightShadow(i, v_position, n);
            #endif
            specular += blinnPhong(lightSample, n, v, shininess) * shadow;
        }
        // the fog fades the glint to black (it adds light)
        glint = fadeByFog(specular * customParams.specularColor.rgb, vec3(0.0));
    #endif

    // as the blends would draw them, in display values: the scene times the transmission
    // filter, the reflection over it by the fresnel (it replaces what the glass lets
    // through), the glint added
    vec3 reflected = encodeColor(applyFog(reflection));
    #if defined(BLEND_ADDITIVE)
        // what the glass adds: its reflection and glint
        g_finalColor = vec4(reflected * fresnel + encodeColor(glint), 1.0);
    #elif defined(BLEND_MULTIPLY)
        // what the glass multiplies by: its transmission filter
        g_finalColor = vec4(encodeColor(transmission), 1.0);
    #else
        vec3 color = mix(encodeColor(behind) * encodeColor(transmission), reflected, fresnel) + encodeColor(glint);
        g_finalColor = vec4(color, 1.0);
    #endif
}
