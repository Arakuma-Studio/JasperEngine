// Area Lights (spec S99 R33, USE_AREA_LIGHTS): a Rectangle or a Disc emitting from its front,
// in the built-in Lit's pixel-light loop. The CPU form of every function here is
// render/AreaLightMath.cpp, whose unit tests hold them to numeric integrals.
//
//   - Diffuse, every tier: the shape's vector form factor (its edges' integrals, a rational fit in
//     place of acos; a disc is an octagon of its area, sixteen sides for the LTC specular), clipped by the receiver's horizon as a
//     sphere of the same vector (closed form): exact where the light is whole above the horizon.
//   - Specular, Standard and lower: the most representative point (the reflection ray's hit
//     clamped to the shape) lit as a point light with the irradiance of a disc of the shape's area
//     (A / (d^2 + A / pi), finite at contact), its GGX energy normalised (Karis 2013).
//   - Specular, High (USE_LIT_QUALITY, AREA_LIGHTS_LTC): the linearly transformed cosine of the GGX
//     lobe (Heitz et al. 2016) from the LTC tables in u_lightProfiles.
//   - A material with extended lobes (PBR_EXT_LOBES) lights them from the diffuse's
//     representative direction with the irradiance the form factor gives.
//
// Include after brdf.glsl, pbr_ext.glsl and lights.glsl: it reads MaterialInfo, BRDF_lambertian,
// BRDF_specularGGX, clampedDot and the light records (getLightData, lightPackedBits).

#ifndef AREA_LIGHTS_GLSL
#define AREA_LIGHTS_GLSL

struct AreaLight {
    vec3 center;
    vec3 normal;      // forward, the emitting side
    vec3 right;
    vec3 up;
    float halfWidth;  // a disc's radius, with halfHeight
    float halfHeight;
    bool disc;
    vec3 luminance;   // linear colour * intensity (the face's)
    float window;     // the range's window at the receiver
};

const float AREA_PI = 3.14159265358979;
// a disc is a regular polygon of its area (AreaLightMath::corners): 8 sides for the diffuse, 16 for
// the LTC specular. Per side count: the circumradius scale sqrt(2 pi / (n sin(2 pi / n))) and the
// step's cos and sin
const vec3 AREA_DISC_8 = vec3(1.05390736, 0.70710678, 0.70710678);
const vec3 AREA_DISC_16 = vec3(1.01300156, 0.92387953, 0.38268343);
const vec2 AREA_RECTANGLE[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

// the light of the object's list as an Area Light, false for any other light (includes/lights.glsl
// has the records' form)
bool getAreaLight(int index, Light light, vec3 worldPosition, out AreaLight area){
    area = AreaLight(vec3(0.0), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), 0.0, 0.0, false, vec3(0.0), 0.0);
    if (light.type == LightType_Directional || light.innerConeCos < 1.5){
        return false;
    }
    area.center = light.position;
    area.normal = normalize(light.direction);
    vec3 up = light.spotUp - area.normal * dot(light.spotUp, area.normal);
    area.up = (length(up) > 0.0001) ? normalize(up) : normalize(cross(area.normal, (abs(area.normal.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    area.right = cross(area.normal, area.up);
    area.halfWidth = light.innerConeCos - 2.0;
    area.halfHeight = -light.spotMaskAspect;
    area.disc = ((lightPackedBits(index) >> 14) & 1) == 1;
    float size = area.disc ? AREA_PI * area.halfWidth * area.halfHeight : 4.0 * area.halfWidth * area.halfHeight;
    area.luminance = light.color * (light.intensity / max(size, 1e-6));
    float distance = length(worldPosition - area.center);
    float ratio = distance / max(light.range, 1e-6);
    area.window = (light.range <= 0.0) ? 1.0 : clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return true;
}

vec3 areaCorner(AreaLight area, vec2 c){
    return area.center + area.right * (c.x * area.halfWidth) + area.up * (c.y * area.halfHeight);
}

// an edge's share of the vector form factor between two unit vectors (Hill and Heitz's fit of
// theta / sin(theta), with Lambert's 1 / 2 pi)
vec3 areaEdgeFormFactor(vec3 v1, vec3 v2){
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float thetaSinTheta = (x > 0.0) ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSinTheta;
}

// the shape's vector form factor from p, each corner through m first, facing the light; a disc
// as the polygon of disc (AREA_DISC_8 or AREA_DISC_16), its corners stepped by a rotation
vec3 areaVectorFormFactor(AreaLight area, vec3 p, mat3 m, vec3 disc){
    int count = area.disc ? ((disc.y > 0.8) ? 16 : 8) : 4;
    vec2 corner = area.disc ? vec2(disc.x, 0.0) : AREA_RECTANGLE[0];
    vec3 first = normalize(m * (areaCorner(area, corner) - p));
    vec3 previous = first;
    vec3 sum = vec3(0.0);
    for (int k = 1; k < 16; k++){
        if (k >= count) break;
        corner = area.disc ? vec2(corner.x * disc.y - corner.y * disc.z, corner.x * disc.z + corner.y * disc.y) : AREA_RECTANGLE[k];
        vec3 current = normalize(m * (areaCorner(area, corner) - p));
        sum += areaEdgeFormFactor(previous, current);
        previous = current;
    }
    sum += areaEdgeFormFactor(previous, first);
    if (dot(sum, m * (area.center - p)) < 0.0){
        sum = -sum;
    }
    return sum;
}

// the horizon-clipped form factor of a spherical cap of vector length sinSigmaSqr, its direction
// cosTheta from the normal
float areaSphereFormFactor(float cosTheta, float sinSigmaSqr){
    sinSigmaSqr = clamp(sinSigmaSqr, 1e-6, 1.0);
    if (cosTheta * cosTheta > sinSigmaSqr){
        return sinSigmaSqr * max(cosTheta, 0.0);
    }
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 1e-7));
    float x = sqrt(max(1.0 / sinSigmaSqr - 1.0, 1e-7));
    float y = clamp(-x * cosTheta / sinTheta, -1.0, 1.0);
    float sinThetaSqrtY = sinTheta * sqrt(max(1.0 - y * y, 0.0));
    return max(((cosTheta * acos(y) - x * sinThetaSqrtY) * sinSigmaSqr + atan(sinThetaSqrtY / x)) / AREA_PI, 0.0);
}

// The light's diffuse and specular at the receiver p (normal n, view v) into the loop's sums,
// shadow its shadow term
void areaLightShade(AreaLight area, MaterialInfo info, vec3 p, vec3 n, vec3 v, float shadow, inout vec3 diffuse, inout vec3 specular){
    if (dot(p - area.center, area.normal) <= 0.0){
        return;
    }
    vec3 luminance = area.luminance * (shadow * area.window);
    vec3 f = areaVectorFormFactor(area, p, mat3(1.0), AREA_DISC_8);
    float len = length(f);
    if (len <= 1e-9){
        return;
    }
    vec3 ld = f / len;
    float formFactor = areaSphereFormFactor(dot(ld, n), len);
    float NdotV = clampedDot(n, v);
    vec3 hd = normalize(ld + v);

    #ifdef PBR_EXT_LOBES
        float NdotLd = clampedDot(n, ld);
        vec3 irradiance = luminance * (AREA_PI * formFactor / max(NdotLd, 1e-4));
        pbrExtDirect(info, irradiance, area.center - p, n, v, ld, hd, NdotLd, NdotV, clampedDot(n, hd), clampedDot(v, hd), diffuse, specular);
    #else
        diffuse += luminance * (AREA_PI * formFactor) * BRDF_lambertian(info.f0, info.f90, info.albedoColor, clampedDot(v, hd));

        #ifdef AREA_LIGHTS_LTC
            // the LTC tables: rows 0-63 of u_lightProfiles, the inverse matrix's terms in the first
            // 64 columns, the albedo and Fresnel share in the next
            vec2 uv = vec2(info.perceptualRoughness, sqrt(1.0 - NdotV)) * (63.0 / 64.0) + 0.5 / 64.0;
            float rowsScale = LIGHT_PROFILE_LUT_ROWS / float(textureSize(sampler2D(u_lightProfiles, LIGHT_PROFILES_SAMPLER), 0).y);
            vec4 t1 = textureLod(sampler2D(u_lightProfiles, LIGHT_PROFILES_SAMPLER), vec2(uv.x * 0.5, uv.y * rowsScale), 0.0);
            vec4 t2 = textureLod(sampler2D(u_lightProfiles, LIGHT_PROFILES_SAMPLER), vec2(0.5 + uv.x * 0.5, uv.y * rowsScale), 0.0);
            vec3 t1v = v - n * dot(v, n);
            t1v = (dot(t1v, t1v) > 1e-10) ? normalize(t1v) : normalize(cross(n, (abs(n.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
            vec3 t2v = cross(n, t1v);
            mat3 minv = mat3(vec3(t1.x, 0.0, t1.y), vec3(0.0, 1.0, 0.0), vec3(t1.z, 0.0, t1.w));
            vec3 fs = areaVectorFormFactor(area, p, minv * transpose(mat3(t1v, t2v, n)), AREA_DISC_16);
            float lenS = length(fs);
            float ltc = (lenS > 1e-9) ? areaSphereFormFactor(fs.z / lenS, lenS) : 0.0;
            specular += luminance * ltc * (info.f0 * t2.x + (info.f90 - info.f0) * t2.y) * litEnergyCompensation;
        #else
            // the reflection ray's hit on the light's plane, clamped to the shape (a ray leaving the
            // plane takes the plane's point under it at the centre's distance)
            vec3 r = reflect(-v, n);
            vec3 toCenter = area.center - p;
            float denom = dot(r, area.normal);
            float t = dot(toCenter, area.normal) / min(denom, -1e-4);
            vec3 hit = p + r * ((denom < -1e-4 && t > 0.0) ? t : length(toCenter));
            hit -= area.normal * dot(hit - area.center, area.normal);
            vec2 xy = vec2(dot(hit - area.center, area.right), dot(hit - area.center, area.up));
            if (area.disc){
                float rr = length(xy / vec2(area.halfWidth, area.halfHeight));
                if (rr > 1.0) xy /= rr;
            }else{
                xy = clamp(xy, -vec2(area.halfWidth, area.halfHeight), vec2(area.halfWidth, area.halfHeight));
            }
            vec3 toPoint = area.center + area.right * xy.x + area.up * xy.y - p;
            float distanceSqr = max(dot(toPoint, toPoint), 1e-8);
            vec3 ls = toPoint * inversesqrt(distanceSqr);
            float size = area.disc ? AREA_PI * area.halfWidth * area.halfHeight : 4.0 * area.halfWidth * area.halfHeight;
            float irradiance = size * max(dot(area.normal, -ls), 0.0) / (distanceSqr + size / AREA_PI);
            float alpha = max(info.alphaRoughness, 1e-4);
            float widened = clamp(alpha + sqrt(size / AREA_PI) / (2.0 * sqrt(distanceSqr)), 0.0, 1.0);
            float normalisation = (alpha / widened) * (alpha / widened);
            vec3 hs = normalize(ls + v);
            float NdotLs = clampedDot(n, ls);
            specular += luminance * (irradiance * normalisation * NdotLs) *
                BRDF_specularGGX(info.f0, info.f90, info.alphaRoughness, clampedDot(v, hs), NdotLs, NdotV, clampedDot(n, hs));
        #endif
    #endif
}

#endif // AREA_LIGHTS_GLSL
