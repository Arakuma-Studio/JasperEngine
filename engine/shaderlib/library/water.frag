#version 450

// @shader mesh
// @name "Water"
// @category "Nature"
// @description "Stylised water: shallow to deep colour by the view angle, two scrolling normal layers and foam anchored to the world, optional sine or Gerstner waves; lit per pixel with a specular glint, distant waves and ripples fading so they do not shimmer. Optional features: BUILTIN_TEXTURES (the engine's seamless ripples and foam noise where the material sets no texture, so no image files are needed), GERSTNER (with WAVES: four Gerstner waves, sharp crests and the surface rolling forward), REFLECTION (the sky or the reflection probe around it by Schlick's fresnel, a GGX sun glint), PLANAR_REFLECTION (with REFLECTION: the scene mirrored in the water, from a camera pass per water height at the scene's Planar Reflections resolution, else the environment), UNDERSIDE (both faces drawn: seen from below, the sky only through Snell's window, the depths mirrored around it; with library:underwater in the post-processing chain the view under the surface fades into the water), and the scene-reading DEPTH_FOAM (foam along every shore and object crossing the surface) and ABSORPTION (colour by the water's thickness; with REFRACTION the bed seen through it also loses each colour at its own rate) reading the scene depth, REFRACTION (the bed and anything under the surface bent by the ripples) the scene colour and depth, CAUSTICS (with REFRACTION: the sunlight focused on the bed). For meshes that carry their water (a spline's river or lake): FLOW (rivers: the ripples and the foam carried along the flow the vertex colour gives) and MESH_DEPTH (shore foam and shallow to deep colour from the depth the mesh's first UV set gives, with no scene-depth pass: the mobile path). Mobile cost of those: the scene depth is an extra depth-only pass of the opaque meshes (none when SSAO or SSR already made one), the scene colour a copy of the screen (half resolution by default) and a split of the render pass; REFLECTION, CAUSTICS and MESH_DEPTH are a few instructions (one cube map read for REFLECTION), GERSTNER about thirty, FLOW doubles the four texture reads; PLANAR_REFLECTION draws the scene again (at half resolution by default) for each water height a camera sees. REFRACTION needs the Alpha or Premultiplied blend (Additive and Multiply draw it unrefracted). Limits of reading the depth: no shore foam where a vertical face meets the water seen from straight above (no depth gap there), and REFRACTION leaves a faint line along outlines crossing the water (samples in front are rejected). Floats rigidbodies with a Buoyancy (Water in Lua: the surface height and normal at a point, the same waves); the waves run on the scene clock, so they hold while the game is paused. Water Excluders (boats, cabins, dry tunnels) cut it, and Water Wakes leave foam and waves on it: engine variants only scenes with those components draw. Alpha blended; casts no shadows."
// @lighting none pixel
// @role water
// @require position normals
// @require scenedepth feature=DEPTH_FOAM
// @require scenedepth feature=ABSORPTION
// @require scenecolor scenedepth feature=REFRACTION
// @require environment feature=REFLECTION
// @require planarreflection feature=PLANAR_REFLECTION
// @require waterexclusion waterwake
// @require rawcolor feature=FLOW
// @require uv1 feature=MESH_DEPTH
// @state blend=alpha shadows=off
// @state[UNDERSIDE] cull=off
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
// @property waveSteepness range(0, 1) 0.5 "Crest sharpness" feature=GERSTNER
// @property reflectionColor color (0.9, 0.95, 1, 1) "Reflection colour (times the environment; alone without one)" feature=REFLECTION
// @property reflectionStrength range(0, 2) 1 "Reflection strength" feature=REFLECTION
// @property reflectionDistortion range(0, 0.2) 0.03 "Planar reflection distortion (screen UV per unit of ripple)" feature=PLANAR_REFLECTION
// @property shoreFoamWidth float 0.12 "Shore foam width (world units of depth)" feature=DEPTH_FOAM
// @property shoreFoamBreakup range(0, 1) 0.7 "Shore foam break-up (by the foam noise)" feature=DEPTH_FOAM
// @property absorptionDepth float 1.5 "Deep colour thickness (world units of water along the view)" feature=ABSORPTION
// @property shoreFade float 0.1 "Shore fade (world units of water)" feature=ABSORPTION
// @property refractionStrength range(0, 0.2) 0.04 "Refraction (screen UV per unit of ripple)" feature=REFRACTION
// @property refractionDepth float 0.5 "Full refraction from this thickness (world units)" feature=REFRACTION
// @property causticsStrength range(0, 4) 1.2 "Caustics brightness (in full sunlight)" feature=CAUSTICS
// @property causticsScale float 1.5 "Caustics size (world units per cell)" feature=CAUSTICS
// @property causticsSpeed float 0.7 "Caustics speed" feature=CAUSTICS
// @property causticsDepth float 3 "Caustics fade depth (world units below the surface)" feature=CAUSTICS
// @property flowSpeed float 1 "Flow speed (world units per second at the mesh's full speed)" feature=FLOW
// @property flowCycle float 1.5 "Flow cycle (seconds a ripple phase travels before it restarts)" feature=FLOW
// @property meshShoreFoamWidth float 0.3 "Shore foam width (world units of the mesh's depth)" feature=MESH_DEPTH
// @property meshShoreFoamBreakup range(0, 1) 0.7 "Shore foam break-up (by the foam noise)" feature=MESH_DEPTH
// @property meshAbsorptionDepth float 1.5 "Deep colour thickness (world units of water along the view)" feature=MESH_DEPTH
// @property meshShoreFade float 0.1 "Shore fade (world units of the mesh's depth)" feature=MESH_DEPTH
// @property buoyancy toggle 1 "Floats rigidbodies (those with a Buoyancy)"
// @property buoyancyDepth float 0 "Floating depth (world units below the surface; 0: no bottom)"
// @texture u_normalTexture normal "Ripple normal map" default[BUILTIN_TEXTURES]=ripples
// @texture u_noiseTexture gray "Foam noise (red)" default[BUILTIN_TEXTURES]=noise
// @feature WAVES "Waves"
// @feature GERSTNER "Gerstner waves (with WAVES: sharp crests, the surface rolling forward)"
// @feature BUILTIN_TEXTURES "Built-in ripples and foam noise (unset textures)"
// @feature REFLECTION "Reflection (sky or reflection probe, GGX glint)"
// @feature PLANAR_REFLECTION "Planar reflection (with REFLECTION: the scene mirrored; a camera pass per water height)"
// @feature UNDERSIDE "Seen from below (both faces, Snell's window)"
// @feature DEPTH_FOAM "Shore foam (reads the scene depth)"
// @feature ABSORPTION "Depth colour (reads the scene depth)"
// @feature REFRACTION "Refraction (reads the scene colour and depth)"
// @feature CAUSTICS "Caustics (with REFRACTION)"
// @feature FLOW "Flow (rivers: ripples advect along the mesh's flow)"
// @feature MESH_DEPTH "Depth from the mesh (shore foam and depth colour without a scene-depth pass; mobile)"
// @depth

// Made for a horizontal surface (a plane, tessellated for WAVES: the GameObject menu's
// 3D Object > Water is a 64 x 64 grid). Both layers sample the normal map and the foam noise
// at the world XZ divided by their size, so the pattern is the same size whatever the mesh's
// scale and continues across neighbouring tiles. Unset, the two textures are flat (no
// ripples) and mid grey (no noise); with BUILTIN_TEXTURES (the manifest's default[...]: it
// only changes what an unset slot binds, the shader is the same) they are the engine's
// built-in ripples (a seamless normal map of whole-wavenumber sines) and foam noise (seamless
// value noise), so a preset needs no image files. Foam shows where the two noise layers
// together exceed 1 - foamAmount (more on wave crests). Pixel lighting: Lambert from the ambient and the pixel lights on the water and
// foam colour, plus a Blinn-Phong glint of each pixel light (shadowed; GGX with REFLECTION),
// which also makes the water more opaque. Light units: includes/lights.glsl. The shadows it
// receives are looked up with the surface's normal without the ripples (offset along it), so
// a shadow's edge stays where the waves put it instead of following every ripple. It casts no
// shadows (@state shadows=off): the surface is see-through, so the bed below stays lit.
//
// Distant detail: the waves' slope fades out once a pixel covers 4% of a wave length (none
// left at 35%), the ripples to a third of their strength between 1% and 10% of a tile, so
// neither sparkles in the distance (a pixel's footprint on the surface, fwidth of the world
// position; GGX takes the lost detail over as roughness).
//
// REFLECTION: the environment in the reflected direction (@require environment,
// includes/environment.glsl: the reflection probe whose box holds the mesh, else the sky's
// prefiltered environment map; reflectionColor alone without either) times reflectionColor
// and reflectionStrength, over the water by Schlick's fresnel with f0 0.02 (water; the foam
// takes none); rays the ripples bend under the horizon are mirrored back over it. The pixel
// lights' glint is GGX (f0 0.02, roughness 1 - gloss) instead of Blinn-Phong.
//
// PLANAR_REFLECTION (with REFLECTION; @require planarreflection, includes/planar_reflection.glsl):
// the scene mirrored about the water's rest level instead of the environment, from a reflection
// pass the camera draws for each water height (the scene's Planar Reflections settings: the
// resolution, Off for none, the layers, shadows, far clip, clip plane offset and how many heights
// a camera reflects); the ripples bend where it is read by reflectionDistortion screen UV per
// unit, as REFRACTION's shift. Seen from below, or without a pass for this camera this frame
// (Off, more heights than it reflects, a camera image, a probe face, the eye under the surface),
// it is the environment, as REFLECTION alone. Made for flat water: a sloped mesh (a river)
// mirrors about one level.
//
// UNDERSIDE (cull off): a face seen from below the surface (its back, the camera under it) is
// lit as the underside: the sky only through Snell's window (the view within 48 degrees of
// straight up, softened by the ripples: there the water lets what is above through, tinted
// by the shallow colour; REFRACTION bends it), outside it the depths mirrored (the deep
// colour lit by the light reaching the water), the foam faint. The fog of the water between
// the camera and the surface is the post-processing pass library:underwater, which the
// engine switches on while the main camera is under a surface (engine/core/object/
// environment/Water.h). From below, ABSORPTION, CAUSTICS and the glint do not apply.
//
// Scene features (includes/scene.glsl; with all of them off the shader is the phase 1 water,
// pixel for pixel, but for the distant detail fade above). The water's thickness is the
// length of the view ray between the surface and the opaque scene behind it (the scene
// depth gap divided by the cosine of the ray to the camera's axis); its depth below the
// surface is that times |n.v|, n the surface's normal without the ripples.
//   DEPTH_FOAM: foam where the depth below the surface is under shoreFoamWidth, dense at
//     the contact line and thinning out, holes cut into it by the foam noise
//     (shoreFoamBreakup), so it hugs every shore and every object crossing the surface. Vertical faces seen from straight above leave no gap in
//     the depth, so no foam there (the limit of any depth-based foam).
//   ABSORPTION: shallow to deep colour by the thickness (1 - exp(-thickness /
//     absorptionDepth), at least the fresnel term of the phase 1 colour), and the water's
//     alpha fading in over the first shoreFade units, so the shore line is soft. With
//     REFRACTION the scene seen through it is also filtered per channel (Beer-Lambert): the
//     clearest channel of the shallow colour fades as the deepness above already fades it,
//     each other one at the rate its share of the shallow colour gives (log(c) / log(max c)
//     times faster), so a blue-green water turns the bed blue-green with depth.
//   REFRACTION: the scene colour behind the water at the screen position shifted by the
//     ripples' bend (view space, so it follows the camera), refractionStrength screen UV per
//     unit; the shift grows over the first refractionDepth units of thickness (none at the
//     shore line), never reaches past the screen's border (no smeared edge), and a shifted
//     sample landing on something in front of the water falls back to the straight one. The
//     water is drawn over that colour as the blend draws it (finalizeStraightColorOverScene:
//     display-encoded values mixed by its alpha; the fog on the water's own colour only, the
//     scene colour has its fog) and written with alpha 1 where the refraction is whole. The
//     scene colour is copied right before the water draws, after the transparent objects
//     the surface covers (the transparent queue draws them first, render/WaterLayering.h),
//     so they show through it bent too, and a refracting water over another one sees it
//     (a copy is taken again for it); where the shift is still under a couple of pixels (the
//     first bit of thickness at the shore line, refractionStrength 0) the water blends over
//     what the target holds instead, partly then fully, as without REFRACTION (at strength 0
//     it draws exactly the water without it, transparent objects behind included). With
//     ABSORPTION the thickness of the shifted sample sets the colour. The Additive and
//     Multiply blends cannot replace what is behind: they draw the water unrefracted.
//   CAUSTICS (with REFRACTION, else nothing): the bed seen through the water (the scene
//     position of the refracted sample) brightened by a web of light: two layers of the
//     zero line of three travelling cosines 120 degrees apart, their domain bent by two slow
//     sines (a dozen ALU instructions, no hash or texture), causticsScale world units per
//     cell drifting at causticsSpeed. In full sunlight (the main directional light, the
//     frame's mainLightColor times its intensity and its height above the horizon) it adds
//     causticsStrength times the pattern; it grows over the first 0.25 units below the
//     surface, fades with exp(-below / causticsDepth), and goes where its cells would be
//     smaller than a few pixels (no shimmer). Lit per pixel with shadows, the sunlight is
//     that light's shadow on the surface times as strong: no caustics under a shadowed surface.
//
// Mesh features: what the mesh carries instead of what the scene gives, for the water meshes
// of splines (docs/specs/S18-splines.md D9: a river ribbon, a lake or banks fill), whose
// vertices hold the water. Both off, neither the vertex colour nor the UVs are passed to
// this stage and the shader is the one before them, pixel for pixel.
//   FLOW (@require rawcolor: the vertex colour as the mesh has it, no instance tint): the
//     flow over world XZ, rg = 0.5 + 0.5 * direction (cut to unit length), b = the share of
//     flowSpeed (0 still, 1 full speed); a mesh without colours reads white (a full-speed
//     flow along +X+Z). Both ripple layers and the foam noise are carried along it, on top
//     of their drift, by the classic flow map: two phases half a cycle apart, each sampled
//     where the flow brought it from since it last restarted (at most flowSpeed * flowCycle
//     world units back, so the stretch where the flow varies stays small), the second at
//     another part of the tile; each weighs 1 mid-cycle and 0 where it restarts (a
//     triangle), so no restart shows, and the blend keeps its contrast (divided by the length
//     of the weights, as two independent patterns add up) so the ripples do not pulse.
//   MESH_DEPTH (@require uv1): the mesh's first UV set is (metres along the water, depth below
//     the surface in metres), as the spline water mesher writes it (0 at the banks); the ripples
//     and the foam keep world XZ. Shore foam as DEPTH_FOAM's where the depth is under
//     meshShoreFoamWidth (holes by meshShoreFoamBreakup); without ABSORPTION, shallow to deep
//     colour as ABSORPTION's by the water along the view down to a bed that deep (the depth
//     over |n.v|, n the surface's normal without the ripples, at most 5 times the depth) over
//     meshAbsorptionDepth, and the alpha fading in over the first meshShoreFade of depth. No
//     scene depth pass: what the water covers is not read, so objects crossing the surface
//     get no foam (DEPTH_FOAM adds it, both foams show). A mesh without UVs reads depth 0
//     (foam and faded out everywhere).
//
// buoyancy and buoyancyDepth are read by the physics, not by the shader (Water.h): a
// dynamic Rigidbody with a Buoyancy whose centre of mass is over this mesh's world bounds
// floats on its surface (the same waves), unless buoyancy is off or the centre of mass is
// more than buoyancyDepth below the surface; where waters overlap, the one whose bottom is
// highest wins (a pool over a lake).
//
// Water Excluders and Water Wakes (includes/water_interaction.glsl): in a scene with a Water
// Excluder the variant ('Wex') discards the pixels inside the excluders over the mesh in the
// camera's view (8 at most, nearest first), last, after every derivative read; in a scene with a
// Water Wake ('Wwk') the scene's wake map adds its foam (broken up by the foam noise, none where
// there is no wake) and bends the normal by its waves, calming the ripples under the foam.
// Without those components neither is compiled.

// the engine-written members of the Water Excluders and Wakes (none without them)
#include "includes/water_interaction_params.glsl"

uniform u_fs_customParams {
    vec4 shallowColor;
    vec4 deepColor;
    vec4 foamColor;
    vec4 specularColor;
    vec4 reflectionColor;
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
    float reflectionStrength;
    float shoreFoamWidth;
    float shoreFoamBreakup;
    float absorptionDepth;
    float shoreFade;
    float refractionStrength;
    float refractionDepth;
    float causticsStrength;
    float causticsScale;
    float causticsSpeed;
    float causticsDepth;
    float buoyancy;
    float buoyancyDepth;
    float flowSpeed;
    float flowCycle;
    float meshShoreFoamWidth;
    float meshShoreFoamBreakup;
    float meshAbsorptionDepth;
    float meshShoreFade;
    float waveSteepness;
    float reflectionDistortion;
    // written by the engine: the scene clock (Scene::getSceneTime), which the waves, the ripples,
    // the flow and the caustics run on, so they hold while the game is paused
    float sceneTime;
    WATER_INTERACTION_PARAMS
} customParams;

#define FRAME_FS
#include "includes/frame.glsl"

// the waves (WAVES) and the constants the caustics share
#include "library/water.glsl"

#if defined(DEPTH_FOAM) || defined(ABSORPTION) || defined(REFRACTION)
    #define WATER_THICKNESS
#endif

#if defined(CAUSTICS) && defined(REFRACTION)
    #define WATER_CAUSTICS
#endif

uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;
uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;

#include "includes/environment.glsl"
#include "includes/planar_reflection.glsl"
#include "includes/water_interaction.glsl"

in vec3 v_position;
in vec3 v_normal;
#if defined(WAVES) && defined(GERSTNER)
    // the world XZ the surface point rests at (library/water.vert): the Gerstner waves are lit there
    in vec2 v_waveRest;
#endif
#ifdef FLOW
    // the mesh's flow: rg its direction over world XZ (0.5 + 0.5 d), b the share of flowSpeed
    in vec4 v_rawColor;
#endif
#ifdef MESH_DEPTH
    #include "includes/surface_uv.glsl"
    // the mesh's water: x metres along it, y the depth below the surface (metres)
    SURFACE_UV_IN vec2 v_uv1;
#endif

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    // the lights' records from the frame's light data (two floats a light in the block): with
    // shadows and fog, sixteen lights stay within GLES3 / WebGL2's 224 fragment uniform vectors
    #define LIGHTS_FS_RECORDS
    #include "includes/lights.glsl"
#endif

out vec4 g_finalColor;

#include "includes/scene.glsl"
#include "includes/output.glsl"

// world XZ to a layer's UV: size in world units, drift in world units per second (the
// offset wraps every tile, so a long run keeps its precision)
vec2 waterLayerUV(vec2 xz, float size, vec2 drift){
    float scale = 1.0 / max(size, 1e-3);
    return xz * scale + fract(customParams.sceneTime * drift * scale);
}

#ifdef FLOW
// the mesh's flow at this pixel, world units per second over XZ
vec2 waterFlowVelocity(){
    vec2 direction = v_rawColor.rg * 2.0 - vec2(1.0);
    float size = length(direction);
    direction *= size > 1.0 ? 1.0 / size : 1.0;
    return direction * (clamp(v_rawColor.b, 0.0, 1.0) * customParams.flowSpeed);
}

// the two ripple layers' bend at one phase's UVs
vec2 waterRipple(vec2 uv1, vec2 uv2){
    return texture(sampler2D(u_normalTexture, u_normal_smp), uv1).xy +
           texture(sampler2D(u_normalTexture, u_normal_smp), uv2).xy - vec2(1.0);
}

// the foam noise at one phase's UVs
float waterFoamNoise(vec2 uv1, vec2 uv2){
    return 0.5 * (texture(sampler2D(u_noiseTexture, u_noise_smp), uv1).r +
                  texture(sampler2D(u_noiseTexture, u_noise_smp), uv2 * 1.37).r);
}
#endif

#ifdef WATER_CAUSTICS
// one layer of the caustics' web, in [0, 1]: the zero line of three cosines travelling 120
// degrees apart (rounded cells), their domain bent by two slow sines so it is no lattice
float waterCausticLayer(vec2 p, float t){
    p += 0.18 * sin(p.yx * vec2(2.3, 1.9) + vec2(0.9 * t, -0.7 * t));
    float s = cos(WATER_TWO_PI * p.x + t) +
              cos(WATER_TWO_PI * dot(p, vec2(-0.5, 0.8660)) - 1.1 * t) +
              cos(WATER_TWO_PI * dot(p, vec2(-0.5, -0.8660)) + 0.8 * t);
    return 1.0 - smoothstep(0.0, 0.7, abs(s));
}

// sunlight focused on the bed, about 0 to 2: two layers at other sizes and speeds hide the
// repetition, brightest where their webs cross
float waterCaustics(vec2 p, float t){
    float web = 0.5 * (waterCausticLayer(p, t) + waterCausticLayer(p * 1.37 + vec2(0.31, 0.77), 1.13 * t + 1.7));
    return web * web * 2.0;
}
#endif

void main() {
    vec2 xz = v_position.xz;
    vec3 n = normalize(v_normal);
    float crest = 0.0;

    // a pixel's footprint on the surface (world units), before any branch
    float footprint = length(fwidth(v_position.xz));
    // the share of the detail kept, 1 near, less where it would only sparkle
    float detail = 1.0;

    #if defined(WAVES) && defined(GERSTNER)
        // the Gerstner surface at the point's rest position, each wave fading where a pixel
        // covers a good part of it; the normal bent as the flat surface's would be
        vec4 waveWeights = vec4(1.0) - smoothstep(vec4(0.04), vec4(0.35),
            vec4(footprint) / (max(customParams.waveLength, 1e-3) * vec4(1.0, 0.6, 0.37, 0.23)));
        vec3 dPdx;
        vec3 dPdz;
        vec3 gerstner = waterGerstner(v_waveRest, customParams.sceneTime, customParams.waveDirection, customParams.waveHeight,
                                      customParams.waveLength, customParams.waveSpeed, customParams.waveSteepness,
                                      waveWeights, dPdx, dPdz);
        n = normalize(n + normalize(cross(dPdz, dPdx)) - vec3(0.0, 1.0, 0.0));
        // foam on the crests: high, or squeezed where the surface bunches up
        float compression = 1.0 - (dPdx.x * dPdz.z - dPdz.x * dPdx.z);
        crest = max(clamp(gerstner.y / max(customParams.waveHeight, 1e-4), 0.0, 1.0),
                    clamp(compression / max(customParams.waveSteepness, 1e-3), 0.0, 1.0)) * 0.15;
        detail = waveWeights.x;
    #elif defined(WAVES)
        // the waves' slope, as the vertex stage raised them, fading where a pixel covers a
        // good part of a wave
        vec3 wave = waterWaves(xz, customParams.sceneTime, customParams.waveDirection, customParams.waveHeight,
                               customParams.waveLength, customParams.waveSpeed);
        float waveDetail = 1.0 - smoothstep(0.04, 0.35, footprint / max(customParams.waveLength, 1e-3));
        n = normalize(n - vec3(wave.y, 0.0, wave.z) * waveDetail);
        crest = clamp(wave.x / max(customParams.waveHeight, 1e-4), 0.0, 1.0) * 0.15;
        detail = waveDetail;
    #endif

    vec3 v = frame.cameraPosition.w > 0.5 ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);

    #ifdef UNDERSIDE
        // its back seen from under the surface (not gl_FrontFacing, which flips with the
        // winding of offscreen passes; a grazing camera above also sees the backs of waves)
        bool fromBelow = dot(n, v) < 0.0 &&
                         (frame.cameraPosition.w > 0.5 ? frame.cameraDirection.y > 0.0 : frame.cameraPosition.y < v_position.y);
        if (fromBelow){
            n = -n;
        }
    #else
        const bool fromBelow = false;
    #endif

    // the surface's normal without the ripples
    vec3 surfaceNormal = n;

    vec2 uv1 = waterLayerUV(xz, customParams.layerScale, customParams.layerSpeed);
    vec2 uv2 = waterLayerUV(xz, customParams.secondLayerScale, customParams.secondLayerSpeed);

    #ifdef USE_WATER_WAKE
        // the wakes of the Water Wakes on this water: x foam, yz their waves' slope
        vec3 wake = waterWake(xz);
    #endif

    #ifdef FLOW
        // two phases half a cycle apart, each sampled where the flow brought the surface from
        // since the phase restarted; the second at another part of the tiles
        float cycle = max(customParams.flowCycle, 1e-3);
        float phaseA = fract(customParams.sceneTime / cycle);
        float phaseB = fract(phaseA + 0.5);
        vec2 flowCycleTravel = waterFlowVelocity() * cycle;
        vec2 layerScales = vec2(1.0 / max(customParams.layerScale, 1e-3), 1.0 / max(customParams.secondLayerScale, 1e-3));
        vec2 uv1A = uv1 - flowCycleTravel * (phaseA * layerScales.x);
        vec2 uv2A = uv2 - flowCycleTravel * (phaseA * layerScales.y);
        vec2 uv1B = uv1 - flowCycleTravel * (phaseB * layerScales.x) + vec2(0.5, 0.25);
        vec2 uv2B = uv2 - flowCycleTravel * (phaseB * layerScales.y) + vec2(0.25, 0.5);
        // phase A's weight: 1 mid-cycle, 0 where it restarts (phase B is then mid-cycle); the
        // blend of the two patterns scaled back to the contrast of one
        float flowWeight = 1.0 - abs(2.0 * phaseA - 1.0);
        float flowContrast = inversesqrt(flowWeight * flowWeight + (1.0 - flowWeight) * (1.0 - flowWeight));
    #endif

    // the two ripple layers bend the normal in the plane (map x along world X, y along Z),
    // down to a third of their strength where a pixel covers a good part of a tile
    #ifdef FLOW
        vec2 ripple = mix(waterRipple(uv1B, uv2B), waterRipple(uv1A, uv2A), flowWeight) * flowContrast;
    #else
        vec2 ripple = texture(sampler2D(u_normalTexture, u_normal_smp), uv1).xy +
                      texture(sampler2D(u_normalTexture, u_normal_smp), uv2).xy - vec2(1.0);
    #endif
    float rippleDetail = 1.0 - 0.7 * smoothstep(0.01, 0.1, footprint / max(min(customParams.layerScale, customParams.secondLayerScale), 1e-3));
    detail = min(detail, rippleDetail);
    #ifdef USE_WATER_WAKE
        // wake foam calms the ripples; the wake waves bend the surface as the ripples do
        float rippleCalm = 1.0 - 0.6 * clamp(wake.x * 3.0, 0.0, 1.0);
        n = normalize(n + (vec3(ripple.x, 0.0, ripple.y) * (2.0 * customParams.normalStrength * rippleDetail * rippleCalm) -
                           vec3(wake.y, 0.0, wake.z)) * (fromBelow ? -1.0 : 1.0));
    #else
        n = normalize(n + vec3(ripple.x, 0.0, ripple.y) * (2.0 * customParams.normalStrength * rippleDetail * (fromBelow ? -1.0 : 1.0)));
    #endif

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
        float reach = clamp(thickness / max(customParams.refractionDepth, 1e-3), 0.0, 1.0);
        shift *= reach;
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
        #ifdef WATER_CAUSTICS
            // the caustics' brightening of the bed before the sunlight reaching it (0: none)
            float causticsGain = 0.0;
        #endif
        // the refracted sample replaces what the target holds once a unit of ripple shifts
        // it by 2 pixels; before (strength 0, the shore line) the water blends over the
        // target as without REFRACTION (transparent objects behind it stay visible)
        float refractionWeight = clamp(customParams.refractionStrength * reach * frame.resolution.y * 0.5, 0.0, 1.0);

        #ifdef WATER_CAUSTICS
            if (!fromBelow){
                // the bed under the refracted sample, lit by the sun focused through the surface
                vec3 bed = sceneWorldPosition(refracted);
                float below = v_position.y - bed.y;
                float cellSize = max(customParams.causticsScale, 1e-3);
                // the bed's footprint per pixel, in cells: the surface's scaled by the distances
                // (the same for an orthographic camera)
                float bedFootprint = footprint * (frame.cameraPosition.w > 0.5 ? 1.0 :
                                     (surfaceDepth + seenThickness / rayScale) / max(surfaceDepth, 1e-3));
                float causticsFade = smoothstep(0.02, 0.25, below) * exp(-below / max(customParams.causticsDepth, 1e-3)) *
                                     (1.0 - smoothstep(0.15, 0.5, bedFootprint / cellSize));
                float pattern = waterCaustics(bed.xz / cellSize, customParams.sceneTime * customParams.causticsSpeed);
                // applied after the lights, which tell whether the surface over it is in shadow
                causticsGain = customParams.causticsStrength * pattern * causticsFade;
            }
        #endif

        #ifdef ABSORPTION
            if (!fromBelow){
                // each channel fades at its own rate, relative to the clearest one (whose fade
                // is the deepness below): Beer-Lambert tinting the bed by the shallow colour
                vec3 tint = clamp(customParams.shallowColor.rgb, vec3(0.001), vec3(0.999));
                vec3 extinction = log(tint) / log(max(tint.r, max(tint.g, tint.b)));
                behind *= exp(-(extinction - vec3(1.0)) * (seenThickness / max(customParams.absorptionDepth, 1e-3)));
            }
        #endif
    #endif

    #ifdef MESH_DEPTH
        // the mesh's depth below the surface (metres, 0 at the banks)
        float meshDepth = max(v_uv1.y, 0.0);
    #endif

    #ifdef ABSORPTION
        float deepness = max(1.0 - exp(-seenThickness / max(customParams.absorptionDepth, 1e-3)), fresnel);
        vec4 water = mix(customParams.shallowColor, customParams.deepColor, deepness);
        if (!fromBelow){
            water.a *= smoothstep(0.0, max(customParams.shoreFade, 1e-4), seenThickness);
        }
    #elif defined(MESH_DEPTH)
        // the water along the view down to a flat bed that deep, as ABSORPTION's thickness
        float meshThickness = meshDepth / max(abs(dot(v, surfaceNormal)), 0.2);
        float deepness = max(1.0 - exp(-meshThickness / max(customParams.meshAbsorptionDepth, 1e-3)), fresnel);
        vec4 water = mix(customParams.shallowColor, customParams.deepColor, deepness);
        if (!fromBelow){
            water.a *= smoothstep(0.0, max(customParams.meshShoreFade, 1e-4), meshDepth);
        }
    #else
        vec4 water = mix(customParams.shallowColor, customParams.deepColor, fresnel);
    #endif

    #ifdef FLOW
        // carried along with the ripples, around its mean of a half
        float foamNoise = (mix(waterFoamNoise(uv1B, uv2B), waterFoamNoise(uv1A, uv2A), flowWeight) - 0.5) * flowContrast + 0.5;
    #else
        float foamNoise = 0.5 * (texture(sampler2D(u_noiseTexture, u_noise_smp), uv1).r +
                                 texture(sampler2D(u_noiseTexture, u_noise_smp), uv2 * 1.37).r);
    #endif
    float noise = foamNoise + crest;
    float threshold = 1.0 - customParams.foamAmount;
    float foam = smoothstep(threshold, threshold + customParams.foamSoftness, noise) * step(0.001, customParams.foamAmount);
    #ifdef USE_WATER_WAKE
        // wake foam breaks up by the noise as it fades, and is none where there is no wake
        foam = max(foam, smoothstep(foamNoise - 0.15, foamNoise + 0.15, wake.x * 1.5) * min(0.35 + wake.x * 2.0, 1.0) *
                         smoothstep(0.0, 0.03, wake.x));
    #endif

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

    #ifdef MESH_DEPTH
        // DEPTH_FOAM's band from the mesh's depth: dense at the banks, none from meshShoreFoamWidth
        float meshDensity = 1.0 - clamp(meshDepth / max(customParams.meshShoreFoamWidth, 1e-4), 0.0, 1.0);
        meshDensity *= meshDensity;
        float meshCut = 0.9 * customParams.meshShoreFoamBreakup * clamp(foamNoise * 2.0 - 0.5, 0.0, 1.0);
        foam = max(foam, smoothstep(meshCut, meshCut + 0.08, meshDensity - 0.02));
    #endif

    vec4 albedo = mix(water, customParams.foamColor, foam);

    vec3 color = albedo.rgb;
    float alpha = albedo.a;

    // the share of the sunlight the surface gets: the main directional light's shadow there
    float causticsShadow = 1.0;

    #ifdef USE_PUNCTUAL
        // the underside is lit by the light reaching the water body (from above)
        vec3 lightNormal = fromBelow ? vec3(0.0, 1.0, 0.0) : n;
        // the shadow lookup offsets along the normal: the surface's without the ripples, or
        // the ripples would move the shadow's edge about (from below, up as the lighting)
        vec3 shadowNormal = fromBelow ? lightNormal : surfaceNormal;
        vec3 light = ambientLight(lightNormal);
        vec3 specular = vec3(0.0);
        #ifdef REFLECTION
            // the lost detail as roughness
            float roughness = mix(max(1.0 - customParams.gloss, 0.3), 1.0 - customParams.gloss, detail);
        #else
            float shininess = exp2(10.0 * customParams.gloss + 1.0);
        #endif
        for (int i = 0; i < LIGHT_CAPACITY; i++){
            if (i >= pixelLightCount()) break;
            LightSample lightSample = getLight(i, v_position);
            float shadow = 1.0;
            #ifdef USE_SHADOWS
                shadow = lightShadow(i, v_position, shadowNormal);
                if (getLightData(i).type == LightType_Directional && dot(lightSample.direction, frame.mainLightDirection.xyz) > 0.999){
                    causticsShadow = shadow;
                }
            #endif
            light += lambert(lightSample, lightNormal) * shadow;
            #ifdef REFLECTION
                specular += ggxSpecular(lightSample, n, v, roughness, 0.02) * shadow;
            #else
                specular += blinnPhong(lightSample, n, v, shininess) * shadow;
            #endif
        }
        specular *= customParams.specularColor.rgb * (1.0 - foam);
        if (fromBelow){
            specular = vec3(0.0);
        }
    #endif

    #ifdef WATER_CAUSTICS
        // the sun focused on the bed, as much of it as reaches the surface over it
        vec3 sunlight = frame.mainLightColor.rgb * (frame.mainLightDirection.w * max(frame.mainLightDirection.y, 0.0)) * causticsShadow;
        behind *= vec3(1.0) + causticsGain * min(sunlight * 0.5, vec3(1.0));
    #endif

    if (fromBelow){
        // Snell's window: the sky and what is above only within the critical angle (48.6
        // degrees from straight up), the ripples softening its edge; outside it the depths
        // are mirrored
        vec3 windowNormal = normalize(mix(vec3(0.0, -1.0, 0.0), n, 0.35));
        float window = smoothstep(0.62, 0.72, clamp(dot(windowNormal, v), 0.0, 1.0));
        vec3 depths = customParams.deepColor.rgb;
        color = mix(depths, customParams.shallowColor.rgb, window);
        color = mix(color, customParams.foamColor.rgb, foam * 0.3);
        // what is above shows through the window, tinted
        alpha = max(mix(1.0, customParams.shallowColor.a * 0.5, window), foam * 0.3);
    }

    #ifdef USE_PUNCTUAL
        color = color * light + specular;
    #endif

    #ifdef REFLECTION
        if (!fromBelow){
            // the environment over the water by Schlick's fresnel (f0 0.02), none on the foam
            vec3 reflected = reflect(-v, n);
            // ripples bend some rays under the horizon
            reflected.y = abs(reflected.y);
            vec3 reflection = customParams.reflectionColor.rgb * customParams.reflectionStrength;
            // the scene mirrored about the surface (PLANAR_REFLECTION, when the camera has a pass
            // for this height), bent by the ripples as REFRACTION bends the view; else the
            // environment
            bool planarDrawn = false;
            #ifdef SURFACE_PLANAR_REFLECTION
                if (planarReflectionPresent()){
                    vec2 bend = (frame.viewMatrix * vec4(n - surfaceNormal, 0.0)).xy * customParams.reflectionDistortion;
                    bend.x *= frame.resolution.y * frame.resolution.z;
                    reflection *= planarReflectionColor(v_position, bend);
                    planarDrawn = true;
                }
            #endif
            #ifdef SURFACE_ENVIRONMENT
                if (!planarDrawn && environmentPresent()){
                    reflection *= environmentColor(reflected, (1.0 - detail) * 0.3);
                }
            #endif
            float reflectance = (0.02 + 0.98 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0)) * (1.0 - foam);
            // straight alpha of the reflection over the water: premultiplied sum, then divided
            float coverage = alpha + (1.0 - alpha) * reflectance;
            color = (color * alpha * (1.0 - reflectance) + reflection * reflectance) / max(coverage, 1e-4);
            alpha = coverage;
        }
    #endif

    #ifdef USE_PUNCTUAL
        // a glint shows at full strength on clear water
        alpha = clamp(alpha + dot(specular, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    #endif

    #ifdef USE_WATER_EXCLUSION
        // no water inside a Water Excluder; last, after every derivative this pixel's quad needs
        if (waterExcluded(v_position))
            discard;
    #endif

    #ifdef REFRACTION
        // the water over what is under it, as the blend draws it; only the water's own
        // colour takes the fog
        g_finalColor = finalizeStraightColorOverScene(vec4(color, alpha), behind, refractionWeight);
    #else
        g_finalColor = finalizeStraightColor(vec4(color, alpha));
    #endif
}
