#version 450

// Copyright 2023 Sony Interactive Entertainment.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modified by Eduardo Doria and contributors: Bend Studio's screen-space shadows (bend_sss_gpu.h,
// WriteScreenSpaceShadow) ported from a compute shader to a fragment shader for contact shadows
// (docs/specs/S57-forge-lighting.md R3). A fragment has no wavefront to share depth reads with,
// so each pixel reads its own samples, and a few of them (8 to 16) spread over the light's
// Length with blue-noise jitter instead of one per pixel; the perspective-corrected depth
// comparison, the edge-aware two-tap minor-axis reads, the hard, averaged and fading sample
// groups and the contrast are Bend's. Its output is the R8 mask the opaque pass multiplies into
// the light's shadow term (includes/light_occlusion.glsl).

in vec2 v_texcoord;
out vec4 frag_color;

// the depth pre-pass's packed depth (includes/depth_util.glsl), point-sampled
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
// a 64x64 blue-noise threshold map (render/BlueNoise64.h), repeated
uniform texture2D u_noiseTexture;
uniform sampler u_noise_smp;

uniform u_fs_contactShadows {
    mat4 projection;      // the depth pre-pass's (logical) projection
    mat4 invProjection;
    vec4 lightCoordinate; // xy the light in NDC, z its packed depth, w +1 in front of the camera, -1 behind (ContactShadows::lightCoordinate)
    vec4 lightView;       // xyz towards the light in view space (w 0, directional) or its view position (w 1, spot)
    vec4 depthSize;       // xy the depth texture's size, zw its inverse
    vec4 params;          // x Length (world), y Thickness (world), z sample count, w hard samples
    vec4 params2;         // x fade samples, y Bend's bilinear threshold, z shadow contrast, w noise tiling (1 / 64 a pixel)
} contact;

#include "includes/depth_util.glsl"

#define CONTACT_MAX_SAMPLES 16

// the packed depth at a texel, the far depth outside the texture (Bend's border sampler)
float contactDepth(vec2 texel){
    if (texel.x < 0.0 || texel.y < 0.0 || texel.x >= contact.depthSize.x || texel.y >= contact.depthSize.y){
        return 1.0;
    }
    return decodeDepth(texelFetch(sampler2D(u_depthTexture, u_depth_smp), ivec2(texel), 0));
}

// a packed depth's view position at a texel centre
vec3 contactViewPosition(vec2 texel, float depth01){
    vec2 uv = texel * contact.depthSize.zw;
    vec4 view = contact.invProjection * vec4(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth01 * 2.0 - 1.0, 1.0);
    return view.xyz / view.w;
}

// a view position's texel in the depth texture
vec2 contactTexelOf(vec3 viewPosition){
    vec4 clip = contact.projection * vec4(viewPosition, 1.0);
    vec2 ndc = clip.xy / max(abs(clip.w), 1e-6) * sign(clip.w);
    return vec2(ndc.x * 0.5 + 0.5, ndcYToDepthUV(ndc.y)) * contact.depthSize.xy;
}

// the packed depth of a view depth (z negative in front)
float contactPackedDepth(float viewZ){
    vec4 clip = contact.projection * vec4(0.0, 0.0, viewZ, 1.0);
    return 0.5 * clip.z / clip.w + 0.5;
}

void main(){
    // the pixel this mask texel covers, at its centre (Bend: major axis coordinates at pixel centres)
    vec2 pixel = floor(v_texcoord * contact.depthSize.xy) + 0.5;
    float startDepth = contactDepth(pixel - 0.5);
    // the background (cleared to the far depth) is never shadowed
    if (startDepth >= 1.0){
        frag_color = vec4(1.0);
        return;
    }

    // the light's coordinate in the depth texture's texels (its own orientation)
    vec2 lightXY = vec2(contact.lightCoordinate.x * 0.5 + 0.5, ndcYToDepthUV(contact.lightCoordinate.y)) * contact.depthSize.xy;
    float lightZ = contact.lightCoordinate.z;
    // Bend: direction = -LightCoordinate.w; the ray towards the light runs to the light's
    // coordinate when it is in front of the camera, away from it when behind
    float direction = -contact.lightCoordinate.w;

    // the ray's end Length away towards the light, projected: the march's extent on screen
    vec3 viewPosition = contactViewPosition(pixel, startDepth);
    vec3 towardsLight = (contact.lightView.w > 0.5) ? (contact.lightView.xyz - viewPosition) : contact.lightView.xyz;
    float lightDistance = length(towardsLight);
    float rayLength = contact.params.x;
    if (contact.lightView.w > 0.5){
        rayLength = min(rayLength, lightDistance);
    }
    vec2 endXY = contactTexelOf(viewPosition + towardsLight / max(lightDistance, 1e-6) * rayLength);
    vec2 march = endXY - pixel;

    // the major axis is the one the line from the light to the pixel runs along most (Bend)
    vec2 fromLight = pixel - lightXY;
    bool xAxisMajor = abs(fromLight.x) > abs(fromLight.y);
    float majorSign = ((xAxisMajor ? fromLight.x : fromLight.y) >= 0.0) ? 1.0 : -1.0;
    float marchMajor = abs(xAxisMajor ? march.x : march.y);
    if (marchMajor < 1.0){
        // the ray runs along the view: it covers less than a pixel
        frag_color = vec4(1.0);
        return;
    }

    int samples = int(contact.params.z + 0.5);
    int hardSamples = int(contact.params.w + 0.5);
    int fadeSamples = int(contact.params2.x + 0.5);
    // at least a pixel between samples along the major axis
    float stepMajor = max(marchMajor / float(samples), 1.0);
    vec2 stepXY = march * (stepMajor / marchMajor);

    // the distance of a pixel from the light along the major axis, positive on the pixel's side
    // (Bend's sample_distance)
    float startDistance = abs(xAxisMajor ? fromLight.x : fromLight.y);

    // Depth thresholds (bilinear/shadow thickness) are based on a fractional ratio of the
    // difference between sampled depth and the far clip depth
    float startThicknessScale = abs(1.0 - startDepth);
    // Bend's SurfaceThickness: the share of that depth a surface is assumed to be thick, from
    // the light's Thickness in metres at this pixel's depth
    float thicknessShare = clamp((contactPackedDepth(viewPosition.z - contact.params.y) - startDepth) / max(startThicknessScale, 1e-7), 1e-5, 1.0);

    // perspective correct the depth: in this space all light rays are parallel
    float corrected = (startDepth - lightZ) / startDistance;
    // the inverse of how large the shadowing window is for the projected sample data (Bend); the
    // min() keeps a minimum window very close to the light
    float depthScale = min(startDistance + direction, 1.0 / thicknessShare) * startDistance / startThicknessScale;
    // z_sign: the near depth (0) is below the far one (1)
    float start = corrected * depthScale - 1.0;

    // blue-noise jitter of the samples along the ray, a share of a step
    float jitter = texture(sampler2D(u_noiseTexture, u_noise_smp), gl_FragCoord.xy * contact.params2.w).r;

    vec4 shadowValue = vec4(1.0);
    float hardShadow = 1.0;
    float bilinearThreshold = contact.params2.y;

    for (int i = 0; i < CONTACT_MAX_SAMPLES; i++){
        if (i >= samples) break;
        vec2 sampleXY = pixel + stepXY * (float(i) + 0.5 + jitter);

        // We sample depth twice per pixel per sample, and interpolate with an edge detect filter.
        // Interpolation should only occur on the minor axis of the ray - major axis coordinates
        // should be at pixel centers
        vec2 readXY = floor(sampleXY);
        float minorAxis = xAxisMajor ? sampleXY.y : sampleXY.x;
        float bilinear = fract(minorAxis) - 0.5;
        float bias = (bilinear > 0.0) ? 1.0 : -1.0;
        vec2 offsetXY = vec2(xAxisMajor ? 0.0 : bias, xAxisMajor ? bias : 0.0);
        vec2 depths = vec2(contactDepth(readXY), contactDepth(readXY + offsetXY));

        float depthThicknessScale = abs(1.0 - depths.x);
        // If depth variance is more than a specific threshold, then just use point filtering
        bool usePointFilter = abs(depths.x - depths.y) > depthThicknessScale * bilinearThreshold;
        // Any sample is possibly interpolated towards the bilinear sample, so use a shadowing
        // depth that is further away, based on the difference between the two samples
        float shadowingDepth = usePointFilter ? depths.x : (depths.x + abs(depths.x - depths.y));

        // the sample's distance from the light along the major axis, at its pixel's centre
        float sampleDistance = ((xAxisMajor ? readXY.x : readXY.y) + 0.5 - (xAxisMajor ? lightXY.x : lightXY.y)) * majorSign;
        // samples that overshoot the light coordinate are ignored
        float storedDepth = (sampleDistance > 0.0) ? (shadowingDepth - lightZ) / sampleDistance : 1e10;
        float depthDelta = abs(start - storedDepth * depthScale);

        if (i < hardSamples){
            // the first samples: a single pixel can produce a full shadow
            hardShadow = min(hardShadow, depthDelta);
        }else if (i < samples - fadeSamples){
            // the main samples, averaged in to a set of 4 shadow values
            shadowValue[i & 3] = min(shadowValue[i & 3], depthDelta);
        }else{
            // the final samples fade out
            float fadeOut = float(i + 1 - (samples - fadeSamples)) / float(fadeSamples + 1) * 0.75;
            shadowValue[i & 3] = min(shadowValue[i & 3], depthDelta + fadeOut);
        }
    }

    // Apply the contrast value. A value of 0 indicates a sample was exactly matched to the
    // reference depth (and the result is fully shadowed)
    float contrast = contact.params2.z;
    shadowValue = clamp(shadowValue * contrast + (1.0 - contrast), 0.0, 1.0);
    hardShadow = clamp(hardShadow * contrast + (1.0 - contrast), 0.0, 1.0);

    // Take the average of 4 samples, this is useful to reduces aliasing noise in the source
    // depth, especially with long shadows; the first samples' hard shadow separately
    float result = min(hardShadow, dot(shadowValue, vec4(0.25)));
    frag_color = vec4(result);
}
