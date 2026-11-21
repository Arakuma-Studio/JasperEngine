#version 450

// Cavity (docs/specs/S92-cavity.md): Blender-viewport-style ridge and valley shading of the
// opaque 3D scene. One fullscreen triangle inside the colour pass, after the opaque queue and
// the SSR composite, multiplying what is there (blend DST_COLOR, SRC_COLOR: out = dst x 2 src,
// so 0.5 leaves a pixel as it is). It reads the scene's packed depth and the view-space normals
// of the same pass: SSR's G-buffer (16-bit octahedral over two textures) or the depth pre-pass's
// normals attachment (includes/depth_normals.glsl).
//
//   CAVITY_SCREEN  thin lines from the normals of four neighbours (Blender's "curvature")
//   CAVITY_WORLD   a soft bevel and crease shade from depth taps within a world radius
//                  (Blender's "cavity"), the obscurance term of McGuire et al., "The Alchemy
//                  Screen-Space Ambient Obscurance Algorithm" (HPG 2011)
//
// factor = clamp((1 - valley)(1 + ridge)(1 + curvature), 0, 2) is a multiply of linear light;
// the colour target holds display-encoded colour (each mesh shader applies the tone curve), so
// the draw multiplies by factor^(1 / 2.2): for a power-law encoding that is exactly the encoded
// value of the linear product, and close to it under the sRGB curve and the tone curves.
// Cavity fades with the analytic distance fog already inside the meshes' colour, so a fogged
// far object shows none of it (the volumetric and height fogs are drawn over it after).
//
// The fragment also writes its depth (the pre-pass's, a hair nearer) for a LESS_EQUAL test
// against the colour pass's depth: where the colour pass drew something nearer than the
// pre-pass saw (a stencil-tested submesh, which no pre-pass draws: S92 R6), the test fails and
// that pixel takes no foreign cavity.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
// the pre-pass's normals (one texture), or the G-buffer's coarse (.rg) and fine (.gb) halves
uniform texture2D u_cavityNormalTexture;
uniform sampler u_cavityNormal_smp;
uniform texture2D u_cavityNormalFineTexture;
uniform sampler u_cavityNormalFine_smp;

uniform u_fs_cavity {
    mat4 projection;     // the camera's (logical: unflipped)
    mat4 invProjection;
    vec4 screen;         // x Ridge, y Valley, z tap distance in pixels (1 and up), w depth step threshold
    vec4 world;          // x Radius (view space), y Ridge, z Valley, w samples (4, 8 or 16)
    vec4 worldLimits;    // x the most on-screen radius in pixels (0: none), y attenuation, z gain
    vec4 texel;          // xy the colour pass's size in pixels, zw 1 / size
    vec4 depthTexel;     // xy the depth target's size, zw 1 / size
    vec4 flags;          // x flip Y (a GL offscreen pass drawn flipped), y G-buffer normals, z debug view
    vec4 fog;            // x type (0 linear, 1 exponential, 2 squared; < 0 none), y density, z start, w end
} cavity;

#include "includes/depth_util.glsl"
// (and includes/octahedral.glsl, which has no guard of its own)
#include "includes/depth_normals.glsl"

// the depth target's v of a step up the screen: +1 where its row 0 is the bottom (GL)
#ifdef IS_GLSL
    const float UP_V = 1.0;
#else
    const float UP_V = -1.0;
#endif

float cavityDepth(vec2 uv){
    return decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), uv, 0.0));
}

// the depth buffer packs 0.5 z/w + 0.5 before the backend's clip-space fixup: [-1, 1] matches
// invProjection on every backend; Y takes the backend's row order (depth_util.glsl)
vec3 cavityViewPosition(vec2 uv, float depth01){
    vec4 view = cavity.invProjection * vec4(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth01 * 2.0 - 1.0, 1.0);
    return view.xyz / view.w;
}

// xyz the view-space normal, w 1 where the pass gave one
vec4 cavityNormal(vec2 uv){
    vec4 coarse = textureLod(sampler2D(u_cavityNormalTexture, u_cavityNormal_smp), uv, 0.0);
    if (cavity.flags.y > 0.5){
        vec2 fine = textureLod(sampler2D(u_cavityNormalFineTexture, u_cavityNormalFine_smp), uv, 0.0).gb;
        return vec4(octDecode16(coarse.rg, fine), 1.0);
    }
    return depthNormalsDecode(coarse);
}

// the point of view-space z on the view ray through uv
vec3 cavityPointAtZ(vec2 uv, float z){
    vec3 R = cavityViewPosition(uv, 0.5);
    // perspective rays meet at the eye, orthographic ones are parallel
    if (abs(cavity.projection[2][3]) > 0.5)
        return R * (z / min(R.z, -1e-6));
    return vec3(R.xy, z);
}

// a face normal from the depth of the four neighbours, from the nearer side of each axis so it
// never spans a silhouette (as ssao.frag)
vec3 cavityDepthNormal(vec2 uv, vec3 P){
    vec2 t = cavity.depthTexel.zw;
    vec3 Pl = cavityViewPosition(uv - vec2(t.x, 0.0), cavityDepth(uv - vec2(t.x, 0.0)));
    vec3 Pr = cavityViewPosition(uv + vec2(t.x, 0.0), cavityDepth(uv + vec2(t.x, 0.0)));
    vec3 Pd = cavityViewPosition(uv - vec2(0.0, t.y), cavityDepth(uv - vec2(0.0, t.y)));
    vec3 Pu = cavityViewPosition(uv + vec2(0.0, t.y), cavityDepth(uv + vec2(0.0, t.y)));
    vec3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 dy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 n = normalize(cross(dx, dy));
    return dot(n, P) > 0.0 ? -n : n;
}

// Blender-like soft ceiling: x (1 - c x) up to its top 0.25 / c at x = 0.5 / c
float cavitySoftClamp(float x, float c){
    return x < 0.5 / c ? x * (1.0 - c * x) : 0.25 / c;
}

#ifdef CAVITY_SCREEN
// the curvature from the normals of the four taps a tap distance away (0 across a depth step,
// the background, or where a tap has no normal)
float cavityScreen(vec2 uv, float depthCentre){
    vec2 o = cavity.screen.z * cavity.texel.zw;
    vec2 uvUp = uv + vec2(0.0, o.y * UP_V);
    vec2 uvDown = uv - vec2(0.0, o.y * UP_V);
    vec2 uvRight = uv + vec2(o.x, 0.0);
    vec2 uvLeft = uv - vec2(o.x, 0.0);

    float du = cavityDepth(uvUp);
    float dd = cavityDepth(uvDown);
    float dr = cavityDepth(uvRight);
    float dl = cavityDepth(uvLeft);

    // a plane's 1 / view z is affine on screen: its second difference across opposite taps is
    // zero on any surface without a step, relative to the centre's
    float invC = 1.0 / cavityViewPosition(uv, depthCentre).z;
    float invU = 1.0 / cavityViewPosition(uvUp, du).z;
    float invD = 1.0 / cavityViewPosition(uvDown, dd).z;
    float invR = 1.0 / cavityViewPosition(uvRight, dr).z;
    float invL = 1.0 / cavityViewPosition(uvLeft, dl).z;
    float threshold = cavity.screen.w * abs(invC);
    bool vertical = du < 1.0 && dd < 1.0 && abs(invU + invD - 2.0 * invC) < threshold;
    bool horizontal = dr < 1.0 && dl < 1.0 && abs(invR + invL - 2.0 * invC) < threshold;

    vec4 nu = cavityNormal(uvUp);
    vec4 nd = cavityNormal(uvDown);
    vec4 nr = cavityNormal(uvRight);
    vec4 nl = cavityNormal(uvLeft);
    vertical = vertical && nu.w > 0.5 && nd.w > 0.5;
    horizontal = horizontal && nr.w > 0.5 && nl.w > 0.5;

    float d = (vertical ? nu.y - nd.y : 0.0) + (horizontal ? nr.x - nl.x : 0.0);
    float ridge = cavity.screen.x;
    float valley = cavity.screen.y;
    if (d >= 0.0)
        return ridge > 0.0 ? 2.0 * cavitySoftClamp(d, 0.5 / max(ridge * ridge, 1e-4)) : 0.0;
    return valley > 0.0 ? -2.0 * cavitySoftClamp(-d, 0.7 / max(valley * valley, 1e-4)) : 0.0;
}
#endif

#ifdef CAVITY_WORLD
// x valley, y ridge: the obscurance of the taps above the surface's plane and the openness of
// those below it, on a fixed disk of rings of four arms whose radius is the world radius on
// screen (its RMS radius 1 / sqrt(3) of it whatever the count, so the count changes smoothness,
// not width), the 4n arms of n rings evenly spaced in angle
vec2 cavityWorld(vec2 uv, vec3 P, vec3 n){
    float radius = cavity.world.x;
    // the radius's pixels at this depth (clip w: -z in perspective, 1 in orthographic)
    float clipW = cavity.projection[2][3] * P.z + cavity.projection[3][3];
    float pixels = radius * cavity.projection[1][1] * 0.5 * cavity.texel.y / max(clipW, 1e-6);
    // under about a pixel the term fades to neutral: a distant object never goes noisy or black
    float fade = smoothstep(0.75, 2.0, pixels);
    if (fade <= 0.0)
        return vec2(0.0);
    float cap = cavity.worldLimits.x;
    float scale = cap > 0.0 ? min(1.0, cap / pixels) : 1.0;
    float diskPixels = pixels * scale;
    float behind = radius * scale;

    int samples = int(cavity.world.w + 0.5);
    float rings = float(samples / 4);
    float norm = 1.0 / sqrt(4.0 * rings * rings - 1.0);
    float attenuation = cavity.worldLimits.y;

    float valley = 0.0;
    float ridge = 0.0;
    float weights = 0.0;
    for (int i = 0; i < 16; i++){
        if (i >= samples)
            break;
        float ring = float(i / 4);
        float arm = float(i - (i / 4) * 4);
        // rotated off the screen axes, ring by ring between the previous ring's arms
        float angle = 1.5707963 * (arm + ring / rings) + 0.3926991;
        float reach = diskPixels * (2.0 * ring + 1.0) * norm;
        vec2 tapUV = uv + vec2(cos(angle), sin(angle)) * reach * cavity.texel.zw;
        // a tap under a pixel away reads the centre: it fades out
        float weight = smoothstep(0.5, 1.0, reach);
        if (weight <= 0.0 || tapUV.x < 0.0 || tapUV.x > 1.0 || tapUV.y < 0.0 || tapUV.y > 1.0)
            continue;
        float tapDepth = cavityDepth(tapUV);
        vec3 S;
        if (tapDepth >= 1.0){
            // the background: a point the radius behind the centre's depth, so a silhouette
            // gets its ridge
            S = cavityPointAtZ(tapUV, P.z - behind);
        }else{
            S = cavityViewPosition(tapUV, tapDepth);
        }
        vec3 v = S - P;
        float L = length(v);
        float h = dot(v, n);
        float bias = 0.05 * L + 1e-4;
        float falloff = 1.0 / (max(L, 1e-5) * (1.0 + attenuation * L * L));
        valley += weight * max(h - bias, 0.0) * falloff;
        ridge += weight * max(-h - bias, 0.0) * falloff;
        weights += weight;
    }
    if (weights <= 0.0)
        return vec2(0.0);
    float gain = cavity.worldLimits.z * fade / weights;
    return vec2(clamp(valley * gain * cavity.world.z, 0.0, 1.0), max(ridge * gain * cavity.world.y, 0.0));
}
#endif

void main(){
    vec2 uv = v_texcoord;
    if (cavity.flags.x > 0.5)
        uv.y = 1.0 - uv.y;

    float depth01 = cavityDepth(uv);
    bool debugView = cavity.flags.z > 0.5;
    // the background: neutral (the debug view's grey, else nothing drawn)
    if (depth01 >= 1.0){
        if (!debugView)
            discard;
        gl_FragDepth = 1.0;
        frag_color = vec4(0.5, 0.5, 0.5, 1.0);
        return;
    }

    vec3 P = cavityViewPosition(uv, depth01);
    float factor = 1.0;

    #ifdef CAVITY_SCREEN
        factor *= 1.0 + cavityScreen(uv, depth01);
    #endif
    #ifdef CAVITY_WORLD
        vec4 n = cavityNormal(uv);
        vec3 normal = n.w > 0.5 ? n.xyz : cavityDepthNormal(uv, P);
        vec2 term = cavityWorld(uv, P, normal);
        factor *= (1.0 - term.x) * (1.0 + term.y);
    #endif

    // the analytic distance fog the meshes' colour already holds (includes/fog.glsl's: window
    // depth times clip w)
    if (cavity.fog.x >= 0.0){
        float clipW = cavity.projection[2][3] * P.z + cavity.projection[3][3];
        float fogDistance = depth01 * clipW;
        float visible = 1.0;
        if (cavity.fog.x < 0.5)
            visible = (cavity.fog.w - fogDistance) / max(cavity.fog.w - cavity.fog.z, 1e-5);
        else if (cavity.fog.x < 1.5)
            visible = exp2(-cavity.fog.y * fogDistance * 1.442695);
        else
            visible = exp2(-cavity.fog.y * cavity.fog.y * fogDistance * fogDistance * 1.442695);
        factor = mix(1.0, factor, clamp(visible, 0.0, 1.0));
    }
    factor = clamp(factor, 0.0, 2.0);

    // a hair nearer than the pre-pass's depth, for the LESS_EQUAL test (see the top)
    gl_FragDepth = max(depth01 - 1e-6, 0.0);
    if (debugView){
        frag_color = vec4(vec3(0.5 * factor), 1.0);
        return;
    }
    // exactly neutral: nothing to write
    if (abs(factor - 1.0) < 1.0 / 1024.0)
        discard;
    frag_color = vec4(vec3(0.5 * pow(factor, 1.0 / 2.2)), 1.0);
}
