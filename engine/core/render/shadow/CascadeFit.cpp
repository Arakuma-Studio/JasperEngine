// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// sliceBoundingSphereRadius/Center, the 0.99 up switch of lightRotation and the freeze guard
// bands are adapted from OpenEngine's ShadowMapRenderFeature.cpp (SliceBoundingSphereRadius,
// CascadeLightUp, GetCascadeFitTuning):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "CascadeFit.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {
    // a 4x4 matrix in double, [col][row] as Matrix4
    struct DMat4 {
        double m[4][4];
    };

    DMat4 toDouble(const Matrix4& source){
        DMat4 d;
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                d.m[c][r] = (double)source[c][r];
            }
        }
        return d;
    }

    // the inverse by Gauss-Jordan elimination with partial pivoting; false when singular
    bool invert(const DMat4& source, DMat4& out){
        double a[4][8];
        for (int r = 0; r < 4; r++){
            for (int c = 0; c < 4; c++){
                a[r][c] = source.m[c][r];
                a[r][c + 4] = (r == c) ? 1.0 : 0.0;
            }
        }
        for (int col = 0; col < 4; col++){
            int pivot = col;
            for (int r = col + 1; r < 4; r++){
                if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
                    pivot = r;
            }
            if (std::abs(a[pivot][col]) < 1e-300)
                return false;
            if (pivot != col){
                for (int k = 0; k < 8; k++)
                    std::swap(a[col][k], a[pivot][k]);
            }
            const double scale = 1.0 / a[col][col];
            for (int k = 0; k < 8; k++)
                a[col][k] *= scale;
            for (int r = 0; r < 4; r++){
                if (r == col)
                    continue;
                const double factor = a[r][col];
                if (factor == 0.0)
                    continue;
                for (int k = 0; k < 8; k++)
                    a[r][k] -= factor * a[col][k];
            }
        }
        for (int r = 0; r < 4; r++){
            for (int c = 0; c < 4; c++){
                out.m[c][r] = a[r][c + 4];
            }
        }
        return true;
    }

    void transformPoint(const DMat4& matrix, const double in[3], double out[3]){
        for (int r = 0; r < 3; r++){
            out[r] = matrix.m[0][r] * in[0] + matrix.m[1][r] * in[1] + matrix.m[2][r] * in[2] + matrix.m[3][r];
        }
    }

    // a point of world space in the light basis (rotation only)
    void toLight(const Matrix4& rotation, const double world[3], double light[3]){
        for (int r = 0; r < 3; r++){
            light[r] = (double)rotation[0][r] * world[0] + (double)rotation[1][r] * world[1] + (double)rotation[2][r] * world[2];
        }
    }

    // the light-space box of a world box; false when it has none
    bool lightSpaceBounds(const Matrix4& rotation, const ShadowBounds& bounds, double minOut[3], double maxOut[3]){
        if (!bounds.valid)
            return false;
        for (int k = 0; k < 3; k++){
            minOut[k] = 1e300;
            maxOut[k] = -1e300;
        }
        for (int i = 0; i < 8; i++){
            const double corner[3] = {
                (i & 1) ? bounds.max.x : bounds.min.x,
                (i & 2) ? bounds.max.y : bounds.min.y,
                (i & 4) ? bounds.max.z : bounds.min.z};
            double light[3];
            toLight(rotation, corner, light);
            for (int k = 0; k < 3; k++){
                minOut[k] = std::min(minOut[k], light[k]);
                maxOut[k] = std::max(maxOut[k], light[k]);
            }
        }
        return std::isfinite(minOut[0]) && std::isfinite(maxOut[0]);
    }

    // P * R in double, stored once as float
    Matrix4 composeViewProjection(const double proj[4][4], const Matrix4& rotation){
        Matrix4 result;
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                double sum = 0.0;
                for (int k = 0; k < 4; k++){
                    sum += proj[k][r] * (double)rotation[c][k];
                }
                result[c][r] = (float)sum;
            }
        }
        return result;
    }
}

Matrix4 CascadeFit::lightRotation(const Vector3& lightDir){
    Vector3 forward = lightDir;
    const float length = forward.length();
    forward = (length > 1e-12f) ? forward / length : Vector3(0.0f, -1.0f, 0.0f);
    const Vector3 up = (std::abs(forward.y) > 0.99f) ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f);
    return Matrix4::lookAtMatrix(Vector3(0.0f, 0.0f, 0.0f), forward, up);
}

float CascadeFit::sliceBoundingSphereRadius(float nearDist, float farDist, float tanHalfY, float aspect){
    // a slice corner at depth d sits at (d * aspect * tanHalfY, d * tanHalfY, d): its squared
    // lateral offset is d^2 * k2
    const float k2 = tanHalfY * tanHalfY * (1.0f + aspect * aspect);
    const float n = std::max(nearDist, 0.0f);
    const float f = std::max(farDist, n + 1e-4f);

    // the centre equidistant from the near and far corner rings sits at (n + f)(k2 + 1) / 2;
    // beyond the far plane the far ring alone bounds the slice, from a centre on the far plane
    if (k2 >= (f - n) / (f + n))
        return f * std::sqrt(k2);

    const float c = 0.5f * (n + f) * (k2 + 1.0f);
    return std::sqrt(f * f * k2 + (f - c) * (f - c));
}

float CascadeFit::sliceBoundingSphereCenter(float nearDist, float farDist, float tanHalfY, float aspect){
    const float k2 = tanHalfY * tanHalfY * (1.0f + aspect * aspect);
    const float n = std::max(nearDist, 0.0f);
    const float f = std::max(farDist, n + 1e-4f);
    if (k2 >= (f - n) / (f + n))
        return f;
    return 0.5f * (n + f) * (k2 + 1.0f);
}

void CascadeFit::computeSplits(const CascadeFitInput& input, float splitFar[CASCADE_FIT_MAX]){
    const unsigned int cascades = input.orthographic ? 1u : std::clamp(input.cascades, 1u, CASCADE_FIT_MAX);
    const float n = std::max(input.nearClip, 1e-4f);
    const float f = std::max(input.shadowDistance, n * 1.001f);

    for (unsigned int i = 0; i < CASCADE_FIT_MAX; i++){
        splitFar[i] = f;
    }

    float previous = n;
    for (unsigned int i = 0; i + 1 < cascades; i++){
        float split;
        if (input.mode == CascadeSplitMode::MANUAL){
            split = input.splits[i] * f;
        }else{
            const float fraction = (float)(i + 1) / (float)cascades;
            const float uniform = n + fraction * (f - n);
            const float logarithmic = n * std::pow(f / n, fraction);
            split = uniform + (logarithmic - uniform) * CASCADE_SPLIT_LAMBDA;
        }
        // strictly increasing, inside (near, distance): a bad manual set still gives every
        // cascade some depth
        const float room = (f - previous) / (float)(cascades - i);
        const float lowest = previous + room * 0.01f;
        const float highest = f - room * 0.01f * (float)(cascades - i - 1);
        if (!std::isfinite(split))
            split = previous + room;
        split = std::clamp(split, lowest, std::max(lowest, highest));
        splitFar[i] = split;
        previous = split;
    }
}

float CascadeFit::freezeGuardFraction(unsigned int cascade){
    return (cascade == 0) ? (1.0f / 16.0f) : (1.0f / 8.0f);
}

float CascadeFit::quantizeRadius(float radius){
    if (!(radius > 0.0f) || !std::isfinite(radius))
        return 0.0f;
    int exponent = 0;
    const float mantissa = std::frexp(radius, &exponent);   // radius = mantissa * 2^exponent, mantissa in [0.5, 1)
    // the step: 2^ceil(log2 radius) / 64
    const int ceilLog2 = (mantissa == 0.5f) ? exponent - 1 : exponent;
    const float step = std::ldexp(1.0f, ceilLog2 - 6);
    return std::ceil(radius / step) * step;
}

CascadeFitResult CascadeFit::fit(const CascadeFitInput& input, const CascadeFitResult* previous){
    CascadeFitResult result;
    Vector3 lightDir = input.lightDir;
    const float lightLength = lightDir.length();
    lightDir = (lightLength > 1e-12f) ? lightDir / lightLength : Vector3(0.0f, -1.0f, 0.0f);
    result.lightDir = lightDir;
    result.resolution = std::max(input.resolution, 1u);
    result.view = lightRotation(lightDir);

    const float nearClip = std::max(input.nearClip, 1e-4f);
    if (!(input.shadowDistance > nearClip) || !std::isfinite(input.shadowDistance))
        return result;

    DMat4 cameraToWorld;
    if (!invert(toDouble(input.cameraView), cameraToWorld))
        return result;

    const unsigned int cascades = input.orthographic ? 1u : std::clamp(input.cascades, 1u, CASCADE_FIT_MAX);
    result.cascades = cascades;
    for (unsigned int c = 0; c < CASCADE_FIT_MAX; c++){
        result.cascadeResolution[c] = (input.cascadeResolution[c] > 0) ? input.cascadeResolution[c] : result.resolution;
    }

    float splitFar[CASCADE_FIT_MAX];
    computeSplits(input, splitFar);
    const float blend = std::clamp(input.blendFraction, 0.0f, 0.5f);
    for (unsigned int c = 0; c < cascades; c++){
        result.splitFar[c] = splitFar[c];
        result.splitNear[c] = (c == 0) ? nearClip
            : splitFar[c - 1] - blend * (splitFar[c - 1] - result.splitNear[c - 1]);
    }
    for (unsigned int c = 0; c < cascades; c++){
        result.blendStart[c] = (c + 1 < cascades) ? result.splitNear[c + 1] : result.splitFar[c];
    }

    double casterMin[3] = {0.0, 0.0, 0.0};
    double casterMax[3] = {0.0, 0.0, 0.0};
    const bool hasCasters = lightSpaceBounds(result.view, input.casterBounds, casterMin, casterMax);

    // a previous fit the frozen cascades can be taken from: same light, resolution and count
    const bool canFreeze = input.freeze && previous && previous->cascades == cascades &&
        previous->resolution == result.resolution && previous->lightDir == lightDir;

    // the slice spheres, in light space
    double sphereX[CASCADE_FIT_MAX], sphereY[CASCADE_FIT_MAX], sphereZ[CASCADE_FIT_MAX];
    float sphereR[CASCADE_FIT_MAX];
    for (unsigned int c = 0; c < cascades; c++){
        const float n = result.splitNear[c];
        const float f = result.splitFar[c];
        float centerDepth;
        float radius;
        if (input.orthographic){
            const float halfDepth = 0.5f * (f - n);
            centerDepth = 0.5f * (n + f);
            radius = std::sqrt(input.orthoHalfWidth * input.orthoHalfWidth + input.orthoHalfHeight * input.orthoHalfHeight + halfDepth * halfDepth);
        }else{
            centerDepth = sliceBoundingSphereCenter(n, f, input.tanHalfY, input.aspect);
            radius = sliceBoundingSphereRadius(n, f, input.tanHalfY, input.aspect);
        }
        const double viewCenter[3] = {0.0, 0.0, -(double)centerDepth};
        double world[3];
        transformPoint(cameraToWorld, viewCenter, world);
        double light[3];
        toLight(result.view, world, light);
        sphereX[c] = light[0];
        sphereY[c] = light[1];
        sphereZ[c] = light[2];
        sphereR[c] = quantizeRadius(radius);
    }

    // which cascades keep their frozen fit: the slice sphere still inside the frozen box,
    // with the guard consumed below half (a refit is requested from half on, forced at all)
    bool keep[CASCADE_FIT_MAX] = {};
    if (canFreeze){
        int requested = -1;
        float requestedUse = 0.0f;
        for (unsigned int c = 0; c < cascades; c++){
            if (previous->sphereRadius[c] != sphereR[c] || previous->splitFar[c] != result.splitFar[c] ||
                    previous->splitNear[c] != result.splitNear[c] || previous->cascadeResolution[c] != result.cascadeResolution[c]){
                continue; // the slice changed (field of view, aspect, splits): refit
            }
            const double guard = (double)previous->radius[c] - (double)sphereR[c];
            const double travel = std::max(std::abs(sphereX[c] - previous->centerX[c]), std::abs(sphereY[c] - previous->centerY[c]));
            const double slackNear = previous->nearZ[c] - (sphereZ[c] + sphereR[c]);
            const double slackFar = (sphereZ[c] - sphereR[c]) - previous->farZ[c];
            double used = 1.0;
            if (guard > 0.0){
                used = travel / guard;
                used = std::max(used, 1.0 - slackFar / guard);
                used = std::max(used, 1.0 - slackNear / guard);
            }else if (travel == 0.0 && slackNear >= 0.0 && slackFar >= 0.0){
                used = 0.0;
            }
            // a caster that rose above the frozen near plane would be clipped
            if (hasCasters && casterMax[2] > previous->nearZ[c] &&
                    casterMax[0] >= previous->centerX[c] - previous->radius[c] && casterMin[0] <= previous->centerX[c] + previous->radius[c] &&
                    casterMax[1] >= previous->centerY[c] - previous->radius[c] && casterMin[1] <= previous->centerY[c] + previous->radius[c]){
                used = 1.0;
            }
            used = std::max(used, 0.0);
            result.guardUsed[c] = (float)std::min(used, 1.0);
            if (used >= 1.0)
                continue; // forced
            keep[c] = true;
            if (used >= 0.5 && used > requestedUse){
                requested = (int)c;
                requestedUse = (float)used;
            }
        }
        if (requested >= 0){
            keep[requested] = false;
        }
    }

    for (unsigned int c = 0; c < cascades; c++){
        if (keep[c]){
            result.proj[c] = previous->proj[c];
            result.viewProj[c] = previous->viewProj[c];
            result.radius[c] = previous->radius[c];
            result.sphereRadius[c] = previous->sphereRadius[c];
            result.texelWorld[c] = previous->texelWorld[c];
            result.depthSpan[c] = previous->depthSpan[c];
            result.centerX[c] = previous->centerX[c];
            result.centerY[c] = previous->centerY[c];
            result.nearZ[c] = previous->nearZ[c];
            result.farZ[c] = previous->farZ[c];
            result.refit[c] = false;
            continue;
        }

        const float guardFraction = input.freeze ? freezeGuardFraction(c) : 0.0f;
        const float radius = input.freeze ? quantizeRadius(sphereR[c] * (1.0f + guardFraction)) : sphereR[c];
        const double texel = 2.0 * (double)radius / (double)result.cascadeResolution[c];
        const double centerX = std::round(sphereX[c] / texel) * texel;
        const double centerY = std::round(sphereY[c] / texel) * texel;

        // depth: from the sphere's back to its front, the front pulled towards the light up to the
        // casters over the box (off-screen casters still cast into it), quantised to radius / 8
        double front = sphereZ[c] + radius;
        if (hasCasters &&
                casterMax[0] >= centerX - radius && casterMin[0] <= centerX + radius &&
                casterMax[1] >= centerY - radius && casterMin[1] <= centerY + radius){
            front = std::max(front, casterMax[2]);
        }
        const double back = sphereZ[c] - radius;
        const double quantum = (double)radius / 8.0;
        const double nearZ = std::ceil(front / quantum) * quantum;
        const double farZ = std::floor(back / quantum) * quantum;

        // orthographic box [centre - radius, centre + radius] x [-nearZ, -farZ] along -Z
        const double nearDistance = -nearZ;
        const double farDistance = -farZ;
        double proj[4][4] = {};
        proj[0][0] = 1.0 / (double)radius;
        proj[1][1] = 1.0 / (double)radius;
        proj[2][2] = -2.0 / (farDistance - nearDistance);
        proj[3][0] = -centerX / (double)radius;
        proj[3][1] = -centerY / (double)radius;
        proj[3][2] = -(farDistance + nearDistance) / (farDistance - nearDistance);
        proj[3][3] = 1.0;

        Matrix4 projection;
        for (int col = 0; col < 4; col++){
            for (int row = 0; row < 4; row++){
                projection[col][row] = (float)proj[col][row];
            }
        }
        result.proj[c] = projection;
        result.viewProj[c] = composeViewProjection(proj, result.view);
        result.radius[c] = radius;
        result.sphereRadius[c] = sphereR[c];
        result.texelWorld[c] = (float)texel;
        result.depthSpan[c] = (float)(farDistance - nearDistance);
        result.centerX[c] = centerX;
        result.centerY[c] = centerY;
        result.nearZ[c] = nearZ;
        result.farZ[c] = farZ;
        result.refit[c] = true;
        if (!canFreeze)
            result.guardUsed[c] = 0.0f;
    }

    return result;
}
