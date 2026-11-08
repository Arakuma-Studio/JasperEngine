// Capsule shadows (USE_CAPSULE_SHADOWS, docs/specs/S57-forge-lighting.md R2): the soft occlusion
// of the main directional light by the capsules of the Capsule Shadow Casters nearest the camera,
// per receiver pixel, without a shadow map. render/CapsuleShadows.cpp (occlusion) is its CPU twin.
// Re-derived from the geometry, after The Forge's UT28: the closest approach of the ray from the
// receiver towards the light to each capsule's segment, smoothstepped over a penumbra that widens
// along the ray (Softness), the capsule hiding all of the light while the penumbra is narrower
// than the capsule and its share of the light's disc past that.

#ifndef CAPSULE_SHADOWS_GLSL
#define CAPSULE_SHADOWS_GLSL

// a hard capsule still blends a tenth of its radius
const float CAPSULE_PENUMBRA_BASE = 0.1;

// the visibility (1 lit .. 0 occluded) of point towards a light (unit towardsLight) past the
// capsule a..b of radius r
float capsuleVisibility(vec3 point, vec3 towardsLight, vec3 a, vec3 b, float r, float softness){
    vec3 d = b - a;
    vec3 w = point - a;
    float dd = dot(d, d);
    float ld = dot(towardsLight, d);
    float lw = dot(towardsLight, w);
    float dw = dot(d, w);

    // the closest approach of the ray point + t * towardsLight (t >= 0) and the segment
    // a + s * d (0 <= s <= 1): the lines' closest points, s clamped, then t for that s
    float denominator = dd - ld * ld;
    float s = (denominator > 1e-8) ? clamp((dw - lw * ld) / denominator, 0.0, 1.0) : 0.0;
    float along = s * ld - lw;
    // a capsule behind the point (the ray leaves it): a surface facing the light never shadows itself
    if (along <= 0.0){
        return 1.0;
    }
    s = (dd > 1e-8) ? clamp((dw + along * ld) / dd, 0.0, 1.0) : 0.0;
    float t = max(s * ld - lw, 0.0);
    float dist = length(w + towardsLight * t - d * s);

    float penumbra = CAPSULE_PENUMBRA_BASE * r + softness * t;
    float hidden = min(1.0, (r * r) / max(penumbra * penumbra, 1e-12));
    float occluded = hidden * (1.0 - smoothstep(max(r - penumbra, 0.0), r + penumbra, dist)) * smoothstep(0.0, r, along);
    return 1.0 - occluded;
}

// the capsules' visibility of a point towards the main light: the most occluding capsule's
// (overlapping capsules at a joint hide the light once), faded by the scene's intensity
float capsuleShadow(vec3 worldPosition, vec3 towardsLight){
    float visibility = 1.0;
    float softness = lightOcclusionParams.capsuleParams.z;
#ifdef LIGHT_DATA_OCCLUSION
    // the frame's capsules that reach the object, a bit each, in the frame's order (the block
    // form's list order): rows of u_lightRecords from lightDataBases.x
    int reachLow = int(lightOcclusionParams.capsuleParams.x + 0.5);
    int reachHigh = int(lightOcclusionParams.capsuleParams.w + 0.5);
    int first = int(objectLights.lightDataBases.x + 0.5);
    for (int i = 0; i < CAPSULE_SHADOWS_MAX; i++){
        int reach = (i < 16) ? (reachLow >> i) : (reachHigh >> (i - 16));
        if ((reach & 1) == 0) continue;
        vec4 a = lightRecordTexel(first + i, 0);
        vec4 b = lightRecordTexel(first + i, 1);
        visibility = min(visibility, capsuleVisibility(worldPosition, towardsLight, a.xyz, b.xyz, a.w, softness));
    }
#else
    int count = int(lightOcclusionParams.capsuleParams.x + 0.5);
    for (int i = 0; i < CAPSULE_SHADOWS_MAX; i++){
        if (i >= count) break;
        vec4 a = lightOcclusionParams.capsuleA[i];
        vec4 b = lightOcclusionParams.capsuleB[i];
        visibility = min(visibility, capsuleVisibility(worldPosition, towardsLight, a.xyz, b.xyz, a.w, softness));
    }
#endif
    return mix(1.0, visibility, lightOcclusionParams.capsuleParams.y);
}

#endif // CAPSULE_SHADOWS_GLSL
