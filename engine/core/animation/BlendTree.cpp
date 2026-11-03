// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BlendTree.h"

#include "animation/AnimatorAssetIO.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    constexpr float kDrop = 1e-3f;
    constexpr float kTiny = 1e-6f;

    float param(const float* parameters, int index){
        return index >= 0 ? parameters[index] : 0.0f;
    }

    void normalizeWeights(float* weights, uint32_t n){
        float total = 0.0f;
        for (uint32_t i = 0; i < n; i++) total += weights[i];
        if (!(total > 0.0f)) return;
        for (uint32_t i = 0; i < n; i++) weights[i] /= total;
        // drop the slivers and renormalise
        float kept = 0.0f;
        for (uint32_t i = 0; i < n; i++){
            if (weights[i] < kDrop) weights[i] = 0.0f;
            kept += weights[i];
        }
        if (kept > 0.0f){
            for (uint32_t i = 0; i < n; i++) weights[i] /= kept;
        }
    }

    void weights1D(const CompiledTreeChild* children, uint32_t n, float x, float* weights){
        for (uint32_t i = 0; i < n; i++) weights[i] = 0.0f;
        if (n == 0) return;
        if (n == 1 || x <= children[0].threshold){
            weights[0] = 1.0f;
            return;
        }
        if (x >= children[n - 1].threshold){
            weights[n - 1] = 1.0f;
            return;
        }
        for (uint32_t i = 0; i + 1 < n; i++){
            const float a = children[i].threshold;
            const float b = children[i + 1].threshold;
            if (x >= a && x < b){
                const float f = b > a ? (x - a) / (b - a) : 0.0f;
                weights[i] = 1.0f - f;
                weights[i + 1] = f;
                return;
            }
        }
        weights[n - 1] = 1.0f;
    }

    void weightsDirectional(const CompiledTree& tree, const CompiledTreeChild* children, float x, float y, float* weights){
        const uint32_t n = tree.childCount;
        for (uint32_t i = 0; i < n; i++) weights[i] = 0.0f;
        if (n == 0) return;
        const float length = std::sqrt(x * x + y * y);
        if (length < kTiny){
            if (tree.centre >= 0){
                weights[tree.centre] = 1.0f;
            }else{
                for (uint32_t i = 0; i < n; i++) weights[i] = 1.0f / (float)n;
            }
            return;
        }
        const float angle = std::atan2(y, x);
        // the angular neighbours: the nearest child counterclockwise before and after the angle
        int before = -1, after = -1;
        float bestBefore = 10.0f, bestAfter = 10.0f;
        const float twoPi = 6.283185307179586f;
        for (uint32_t i = 0; i < n; i++){
            if ((int)i == tree.centre) continue;
            const float cx = children[i].x, cy = children[i].y;
            if (std::fabs(cx) < kTiny && std::fabs(cy) < kTiny) continue;
            float d = std::atan2(cy, cx) - angle;
            while (d < 0.0f) d += twoPi;
            while (d >= twoPi) d -= twoPi;
            // after: smallest positive gap (0 = on the child's direction)
            if (d < bestAfter){
                bestAfter = d;
                after = (int)i;
            }
            const float back = d == 0.0f ? 0.0f : twoPi - d;
            if (back < bestBefore){
                bestBefore = back;
                before = (int)i;
            }
        }
        if (after < 0){
            if (tree.centre >= 0) weights[tree.centre] = 1.0f;
            return;
        }
        if (before < 0) before = after;
        float wa = 0.0f, wb = 0.0f;
        const float ax = children[after].x, ay = children[after].y;
        const float bx = children[before].x, by = children[before].y;
        const float det = ax * by - ay * bx;
        if (before == after || std::fabs(det) < kTiny){
            const float len2 = ax * ax + ay * ay;
            wa = std::max(0.0f, (x * ax + y * ay) / len2);
        }else{
            wa = (x * by - y * bx) / det;
            wb = (ax * y - ay * x) / det;
            wa = std::max(0.0f, wa);
            wb = std::max(0.0f, wb);
        }
        const float sum = wa + wb;
        if (!(sum > 0.0f)){
            weights[after] = 1.0f;
            return;
        }
        if (tree.centre >= 0 && sum < 1.0f){
            weights[after] += wa;
            weights[before] += wb;
            weights[tree.centre] = 1.0f - sum;
        }else{
            weights[after] += wa / sum;
            weights[before] += wb / sum;
        }
    }

    void weightsGradientBand(const CompiledController& controller, const CompiledTree& tree, const CompiledTreeChild* children,
                             float x, float y, float* weights){
        const uint32_t n = tree.childCount;
        const float* pairs = controller.pairs.data() + tree.pairBegin;
        const bool polar = tree.type == BlendTreeType::FreeformDirectional2D;
        const float length = std::sqrt(x * x + y * y);
        for (uint32_t i = 0; i < n; i++){
            const float px = children[i].x, py = children[i].y;
            float w = 1.0f;
            float angle = 0.0f, magnitude = 0.0f;
            if (polar){
                magnitude = std::sqrt(px * px + py * py);
                if (magnitude > kTiny && length > kTiny) angle = std::atan2(px * y - py * x, px * x + py * y) * 2.0f;
            }
            for (uint32_t j = 0; j < n; j++){
                if (i == j) continue;
                const float* pair = pairs + ((size_t)i * n + j) * 4;
                if (pair[2] == 0.0f) continue;
                float vx, vy;
                if (polar){
                    vx = (length - magnitude) / pair[3];
                    vy = angle;
                }else{
                    vx = x - px;
                    vy = y - py;
                }
                const float h = 1.0f - (vx * pair[0] + vy * pair[1]) * pair[2];
                if (h < w) w = h;
            }
            weights[i] = std::max(0.0f, w);
        }
    }

}

void anim::blendTreeWeights(const CompiledController& controller, const CompiledTree& tree, const float* parameters, float* weights){
    const uint32_t n = tree.childCount;
    const CompiledTreeChild* children = controller.children.data() + tree.childBegin;
    if (n == 0) return;
    switch (tree.type){
        case BlendTreeType::Simple1D:
            weights1D(children, n, param(parameters, tree.parameterX), weights);
            break;
        case BlendTreeType::SimpleDirectional2D:
            weightsDirectional(tree, children, param(parameters, tree.parameterX), param(parameters, tree.parameterY), weights);
            break;
        case BlendTreeType::FreeformDirectional2D:
        case BlendTreeType::FreeformCartesian2D:
            weightsGradientBand(controller, tree, children, param(parameters, tree.parameterX), param(parameters, tree.parameterY), weights);
            break;
        case BlendTreeType::Direct:
            for (uint32_t i = 0; i < n; i++) weights[i] = std::max(0.0f, param(parameters, children[i].directParameter));
            return;
    }
    normalizeWeights(weights, n);
}

size_t anim::motionLeaves(const CompiledController& controller, const CompiledMotion& motion, const float* parameters,
                          float weight, MotionLeaf* out, size_t count, size_t capacity){
    if (!(weight > 0.0f)) return count;
    if (motion.kind == CompiledMotion::Kind::Clip){
        if (count < capacity) out[count++] = {motion.index, weight, 1.0f, false};
        return count;
    }
    if (motion.kind != CompiledMotion::Kind::Tree) return count;
    const CompiledTree& tree = controller.trees[(size_t)motion.index];
    float weights[MAX_BLEND_CHILDREN];
    blendTreeWeights(controller, tree, parameters, weights);
    for (uint32_t c = 0; c < tree.childCount; c++){
        if (!(weights[c] > 0.0f)) continue;
        const CompiledTreeChild& child = controller.children[tree.childBegin + c];
        const size_t first = count;
        count = motionLeaves(controller, child.motion, parameters, weight * weights[c], out, count, capacity);
        for (size_t l = first; l < count; l++){
            out[l].timeScale *= child.timeScale;
            out[l].mirror = out[l].mirror != child.mirror;
        }
    }
    return count;
}
