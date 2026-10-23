#version 450

// Atmosphere/CloudsScreen and Atmosphere/CloudsSkyMap: the volumetric clouds raymarched through
// their layer, lit by the sun and the sky ambient. Two modes of the same march:
//   Screen   (target.z 0) a ray per pixel of a quarter-resolution target, stopped at the scene's
//            surface (clouds in front of far terrain), composited by the volumetric composite;
//   Sky Map  (target.z 1) a ray per texel of a hemi-octahedral map of world directions from the
//            camera position, composited over the sky pixels by the sky composite.
// Output (S, T) of the clouds, with the height fog's aerial perspective at the clouds' mean
// distance (weighted by what they add), so clouds and fog agree.
//
// The density (after SebLague/Clouds as ported by OpenEngine, MIT; with the weather map, cloud
// types and coverage of Schneider's "Real-time volumetric cloudscapes"): a weather map (R
// coverage, G type) and a height profile by type place the cloud; Perlin-Worley shape noise
// carves it (shapeFactor); detail Worley noise erodes its edges, weighted by (1 - shape)^3. The
// layer bends down with the earth's curvature. The light: the sun through a short march towards
// it (Beer's law, three multiple-scattering octaves, a dual-lobe Henyey-Greenstein phase and a
// powder term), plus the ambient from the cloud base to its top. Empty space (no coverage) is
// stepped over without 3D noise taps. The march starts at a static blue-noise offset.

#define ATMOSPHERE_BLUE_NOISE
#define MAX_CLOUD_STEPS 96
#define MAX_CLOUD_LIGHT_STEPS 6

in vec2 v_texcoord;
out vec4 frag_color;

// the Screen mode's quarter-resolution linear depth (atmosphere_depth.frag); unread in Sky Map
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
uniform texture3D u_cloudShape;
uniform sampler u_cloudShape_smp;
uniform texture3D u_cloudDetail;
uniform sampler u_cloudDetail_smp;
uniform texture2D u_weatherMap;
uniform sampler u_weatherMap_smp;

uniform u_fs_clouds {
    mat4 invViewProjection;     // camera clip -> world (Screen mode rays)
    vec4 target;                // xy 1 / target size, z 0 Screen / 1 Sky Map, w 1: RGBA8 (compressed) output
    vec4 cameraPosition;        // xyz the rays' origin in Sky Map mode, w far clip
    vec4 cameraForward;         // xyz world forward, w 1 orthographic; near clip in ... (see depth below)
    vec4 layer;                 // x bottom altitude, y altitude range, z curvature k / (2 R), w max distance (m)
    vec4 shape;                 // x coverage, y extinction (1/m), z shape tiling (1/m), w shape factor
    vec4 erosion;               // x erosion factor, y erosion tiling (1/m), z 1: erosion, w weather tiling (1/m)
    vec4 windOffset;            // xy shape offset (m, world xz), zw weather offset (m)
    vec4 sunDir_steps;          // xyz towards the sun, w primary steps
    vec4 sunColor_lightSteps;   // rgb sun light (linear, dimmed, faded below the horizon), w light steps
    vec4 ambientBottom_near;    // rgb ambient at the cloud base, w near clip (orthographic surface distance)
    vec4 ambientTop_fade;       // rgb ambient at the top, w the clouds' opacity factor (bake fade-in, altitude fade)
    vec4 fog;                   // x 1: aerial perspective by the height fog
} clouds;

#include "includes/depth_util.glsl"
#include "includes/atmosphere.glsl"
#include "includes/height_fog.glsl"

float remap(float v, float a, float b, float c, float d){
    return c + (v - a) * (d - c) / (b - a);
}

// the layer's altitude of a point: its height plus the earth's curvature's drop at its
// horizontal distance from the camera
float cloudAltitude(vec3 p){
    vec2 d = p.xz - clouds.cameraPosition.xz;
    return p.y + clouds.layer.z * dot(d, d);
}

// the smaller and larger roots of a t^2 + b t + c (x > y when there is none)
vec2 cloudRoots(float a, float b, float c){
    if (abs(a) < 1e-12){
        if (abs(b) < 1e-9)
            return vec2(1.0, -1.0);
        float t = -c / b;
        return vec2(t, t);
    }
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0)
        return vec2(1.0, -1.0);
    float q = -0.5 * (b + (b >= 0.0 ? 1.0 : -1.0) * sqrt(disc));
    float r1 = q / a;
    float r2 = (abs(q) > 1e-12) ? c / q : r1;
    return vec2(min(r1, r2), max(r1, r2));
}

// the first part [enter, exit] of the ray inside the layer (exit < enter: none). The altitude
// along the ray is y0 + t dy + a t^2, a convex parabola (a = curvature times the horizontal
// part), or a line without curvature: it crosses an altitude at most twice, going down then up.
vec2 cloudLayerInterval(vec3 origin, vec3 dir){
    float bottom = clouds.layer.x;
    float top = clouds.layer.x + clouds.layer.y;
    float y0 = cloudAltitude(origin);
    float a = clouds.layer.z * max(1.0 - dir.y * dir.y, 0.0);
    bool curved = a > 1e-12;
    vec2 rb = cloudRoots(a, dir.y, y0 - bottom);
    vec2 rt = cloudRoots(a, dir.y, y0 - top);
    bool hasBottom = rb.x <= rb.y;
    bool hasTop = rt.x <= rt.y;
    const float NONE = 1e30;
    if (y0 < bottom){
        // below: in through the bottom going up (its later crossing), out through the top
        if (!hasBottom || rb.y <= 0.0)
            return vec2(1.0, 0.0);
        float enter = rb.y;
        float exit = (hasTop && rt.y > enter) ? rt.y : NONE;
        return vec2(enter, exit);
    }
    if (y0 <= top){
        // inside: out through the top going up or the bottom going down, whichever is first
        float exit = NONE;
        if (hasTop && rt.y > 0.0)
            exit = rt.y;
        if (hasBottom && rb.x > 0.0)
            exit = min(exit, rb.x);
        return vec2(0.0, exit);
    }
    // above: in through the top going down, out through the bottom or back up through the top
    if (!hasTop || rt.x <= 0.0)
        return vec2(1.0, 0.0);
    float enter = rt.x;
    float exit = curved ? rt.y : NONE;
    if (hasBottom && rb.x > enter)
        exit = min(exit, rb.x);
    return vec2(enter, exit);
}

// the cloud's vertical profile at height fraction h for a cloud type (0 stratus .. 1 towering)
float cloudProfile(float h, float type){
    float top = mix(0.35, 1.0, type);
    return smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(top * 0.65, top, h));
}

// the coverage of the weather map at a point (0 clear .. 1 covered) and its cloud type
vec2 cloudWeather(vec3 p){
    vec2 uv = (p.xz + clouds.windOffset.zw) * clouds.erosion.w;
    vec2 weather = textureLod(sampler2D(u_weatherMap, u_weatherMap_smp), uv, 0.0).rg;
    float c = clouds.shape.x;
    float cover = smoothstep(1.0 - c - 0.1, 1.0 - c + 0.1, weather.r) * step(0.001, c);
    return vec2(cover, weather.g);
}

// the cloud's extinction (1/m) at a point of height fraction h with coverage and profile known
float cloudDensity(vec3 p, float h, float cover, float profile, bool erode){
    vec3 uvw = (p + vec3(clouds.windOffset.x, 0.0, clouds.windOffset.y)) * clouds.shape.z;
    vec4 s = textureLod(sampler3D(u_cloudShape, u_cloudShape_smp), uvw, 0.0);
    float fbm = dot(s.gba, vec3(0.625, 0.25, 0.125));
    // Perlin-Worley eroded by the Worley octaves
    float lowFreq = clamp(remap(s.r, fbm - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
    float carved = clamp(remap(lowFreq * profile, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
    float base = mix(cover * profile, carved, clouds.shape.w);
    if (erode && base > 0.0){
        vec3 d = textureLod(sampler3D(u_cloudDetail, u_cloudDetail_smp), (p + vec3(clouds.windOffset.x, 0.0, clouds.windOffset.y) * 2.0) * clouds.erosion.y, 0.0).rgb;
        float detail = dot(d, vec3(0.625, 0.25, 0.125));
        // wispy at the base, billowy at the top; the shapes' edges erode most
        detail = mix(detail, 1.0 - detail, clamp(h * 4.0, 0.0, 1.0));
        float edge = 1.0 - lowFreq;
        base -= (1.0 - detail) * edge * edge * edge * clouds.erosion.x;
    }
    return max(base, 0.0) * clouds.shape.y;
}

// the cloud's extinction at a point of the layer, everything from the weather map on
float cloudDensityAt(vec3 p, bool erode){
    float h = (cloudAltitude(p) - clouds.layer.x) / max(clouds.layer.y, 1.0);
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    vec2 weather = cloudWeather(p);
    float profile = cloudProfile(h, weather.y);
    if (weather.x * profile <= 0.0)
        return 0.0;
    return cloudDensity(p, h, weather.x, profile, erode);
}

float hg(float g, float cosTheta){
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-5);
    return (1.0 - g2) / (d * sqrt(d));
}

// dual-lobe Henyey-Greenstein (4 pi normalised): a strong forward lobe and a weak back lobe,
// their eccentricity scaled for the multiple-scattering octaves
float cloudPhase(float cosTheta, float scale){
    return mix(hg(0.8 * scale, cosTheta), hg(-0.3 * scale, cosTheta), 0.3);
}

void main(){
    bool skyMap = clouds.target.z > 0.5;
    vec2 uv = atmosphereFragmentUV(clouds.target.xy);
    vec3 origin;
    vec3 dir;
    float surface = 1e9;
    if (skyMap){
        // the hemi-octahedral map (AtmosphereMath::hemiOctahedralDecode)
        vec2 st = uv * 2.0 - 1.0;
        vec2 xz = 0.5 * vec2(st.x + st.y, st.x - st.y);
        dir = normalize(vec3(xz.x, 1.0 - abs(xz.x) - abs(xz.y), xz.y));
        origin = clouds.cameraPosition.xyz;
    }else{
        atmosphereCameraRay(clouds.invViewProjection, uv, origin, dir);
        float linear = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), gl_FragCoord.xy * clouds.target.xy, 0.0));
        if (linear < 0.999999){
            float viewDistance = linear * clouds.cameraPosition.w;
            surface = (clouds.cameraForward.w > 0.5) ? viewDistance - clouds.ambientBottom_near.w
                                                     : viewDistance / max(dot(dir, clouds.cameraForward.xyz), 1e-4);
        }
    }

    vec2 interval = cloudLayerInterval(origin, dir);
    float t0 = interval.x;
    float t1 = min(min(interval.y, clouds.layer.w), surface);
    float fade = clouds.ambientTop_fade.w;
    if (t1 <= t0 || fade <= 0.0){
        frag_color = atmospherePack(vec3(0.0), 1.0, clouds.target.w);
        return;
    }

    int steps = int(clouds.sunDir_steps.w);
    int lightSteps = int(clouds.sunColor_lightSteps.w);
    bool erode = clouds.erosion.z > 0.5;
    float dt = (t1 - t0) / float(steps);
    float t = t0 + atmosphereBlueNoise() * dt;
    vec3 sunDir = clouds.sunDir_steps.xyz;
    vec3 sunLight = clouds.sunColor_lightSteps.rgb;
    float cosTheta = dot(dir, sunDir);
    float phases[3] = float[3](cloudPhase(cosTheta, 1.0), cloudPhase(cosTheta, 0.5), cloudPhase(cosTheta, 0.25));
    // the light march's reach: half the layer's depth, at most 1 km
    float lightLength = min(0.5 * clouds.layer.y, 1000.0);
    float lightStep = lightLength / float(max(lightSteps, 1));

    vec3 scattering = vec3(0.0);
    float transmittance = 1.0;
    float weightedDistance = 0.0;
    float weightSum = 0.0;
    for (int i = 0; i < MAX_CLOUD_STEPS; i++){
        if (i >= steps || t >= t1)
            break;
        vec3 p = origin + dir * t;
        float h = (cloudAltitude(p) - clouds.layer.x) / max(clouds.layer.y, 1.0);
        vec2 weather = cloudWeather(p);
        float profile = cloudProfile(h, weather.y);
        // empty space: no 3D noise taps
        if (h > 0.0 && h < 1.0 && weather.x * profile > 0.0){
            float sigma = cloudDensity(p, h, weather.x, profile, erode);
            if (sigma > 0.0){
                float lightDepth = 0.0;
                for (int j = 0; j < MAX_CLOUD_LIGHT_STEPS; j++){
                    if (j >= lightSteps)
                        break;
                    lightDepth += cloudDensityAt(p + sunDir * ((float(j) + 0.5) * lightStep), false) * lightStep;
                }
                // three octaves of multiple scattering (Wrenninge): each attenuates and
                // contributes half as much, with a wider phase
                float sun = 0.0;
                float octave = 1.0;
                for (int o = 0; o < 3; o++){
                    sun += octave * phases[o] * exp(-octave * lightDepth);
                    octave *= 0.5;
                }
                // powder: the sun-facing edges of dense clouds darken (Schneider)
                sun *= mix(1.0, 2.0 * (1.0 - exp(-2.0 * lightDepth)), 0.5);
                vec3 ambient = mix(clouds.ambientBottom_near.rgb, clouds.ambientTop_fade.rgb, h);
                vec3 light = sunLight * sun + ambient;
                float extinction = exp(-sigma * dt);
                float weight = transmittance * (1.0 - extinction);
                scattering += light * weight;
                weightedDistance += t * weight;
                weightSum += weight;
                transmittance *= extinction;
                if (transmittance < 0.01)
                    break;
            }
        }
        t += dt;
    }

    // aerial perspective: the height fog in front of the clouds, at their mean distance
    if (clouds.fog.x > 0.5 && weightSum > 1e-5){
        vec3 fogScattering;
        float fogTransmittance;
        heightFogSegment(origin, dir, 0.0, weightedDistance / weightSum, fogScattering, fogTransmittance);
        scattering = scattering * fogTransmittance + fogScattering * (1.0 - transmittance);
    }

    scattering *= fade;
    transmittance = 1.0 - (1.0 - transmittance) * fade;
    frag_color = atmospherePack(scattering, transmittance, clouds.target.w);
}
