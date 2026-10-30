// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R6, D6)
// - Purpose: the weights of a compiled blend tree's children for the current parameters, and the
//   flattening of a state's motion (clip, or nested trees) into weighted clip leaves.
// - Weights: Simple1D piecewise linear between the neighbouring thresholds (exact at a
//   threshold); SimpleDirectional2D barycentric over the two angular neighbours of the parameter
//   and the centre child (the one at the origin, when there is one); Freeform Cartesian and
//   Directional gradient-band interpolation (Johansen 2009; Directional in polar space with the
//   angle term doubled) over the pair data of CompiledController::pairs; Direct the clamped (>= 0)
//   parameters, not normalised. Every other type gives weights >= 0 that sum to 1, drops weights
//   below 1e-3 and renormalises. A parameter exactly on a child gives that child weight 1.
// - Thread-safety: pure; allocation-free (children at most MAX_BLEND_CHILDREN).

#ifndef BLEND_TREE_H
#define BLEND_TREE_H

#include "Export.h"
#include "animation/CompiledController.h"

#include <cstddef>

namespace jasperengine::anim{

    // the weights of `tree`'s children (tree.childCount values) for `parameters`
    JASPERENGINE_API void blendTreeWeights(const CompiledController& controller, const CompiledTree& tree,
                                           const float* parameters, float* weights);

    struct MotionLeaf{
        int clip = -1;              // controller clip slot
        float weight = 0.0f;        // the product of the weights down the tree
        float timeScale = 1.0f;     // the product of the children's time scales
    };

    // The clip leaves of `motion` (weights scaled by `weight`) appended to `out`, at most
    // `capacity` in all; returns the new count. Leaves of weight 0 are skipped.
    JASPERENGINE_API size_t motionLeaves(const CompiledController& controller, const CompiledMotion& motion, const float* parameters,
                                         float weight, MotionLeaf* out, size_t count, size_t capacity);

}

#endif //BLEND_TREE_H
