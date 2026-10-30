// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Blend tree weights (docs/specs/S21-animation.md AC2, R6, D6): every 2D type gives weights >= 0
// summing to 1 over a 64x64 grid, a parameter on a child gives it weight 1, 1D is exact at its
// thresholds, a 20-child tree uses every child (OpenEngine's 16-sample cut), Direct passes the
// parameters through, nested trees multiply down, and phase-synced children do not drift.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"
#include "animation/BlendTree.h"
#include "animation/CompiledController.h"

#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    ControllerDef treeController(BlendTreeType type, const std::vector<Vector2>& positions, const std::vector<float>& thresholds = {}){
        ControllerDef def;
        def.parameters = {{"X", AnimatorParameterType::Float, 0}, {"Y", AnimatorParameterType::Float, 0}};
        LayerDef layer;
        layer.name = "Base Layer";
        StateDef s;
        s.name = "Tree";
        BlendTreeDef tree;
        tree.type = type;
        tree.parameter = "X";
        tree.parameterY = "Y";
        const size_t count = std::max(positions.size(), thresholds.size());
        for (size_t i = 0; i < count; i++){
            BlendChildDef child;
            child.motion.clip.path = "c" + std::to_string(i) + ".anim";
            if (i < positions.size()) child.position = positions[i];
            if (i < thresholds.size()) child.threshold = thresholds[i];
            child.directParameter = i % 2 ? "Y" : "X";
            tree.children.push_back(child);
        }
        s.motion.tree.push_back(tree);
        layer.machine.states.push_back(s);
        def.layers.push_back(layer);
        normalizeController(def);
        return def;
    }

    std::vector<float> weights(const CompiledController& c, float x, float y){
        const CompiledTree& tree = c.trees[0];
        std::vector<float> w(tree.childCount);
        const float parameters[2] = {x, y};
        blendTreeWeights(c, tree, parameters, w.data());
        return w;
    }

    const std::vector<Vector2> kRing = {Vector2(0, 0), Vector2(0, 1), Vector2(1, 0), Vector2(0, -1), Vector2(-1, 0),
                                        Vector2(0, 2), Vector2(2, 0), Vector2(-2, 0), Vector2(0.7f, 0.7f)};

}

TEST(AnimatorBlendTree, TwoDWeightsArePositiveAndSumToOne){
    for (BlendTreeType type : {BlendTreeType::SimpleDirectional2D, BlendTreeType::FreeformDirectional2D, BlendTreeType::FreeformCartesian2D}){
        std::vector<Vector2> positions = kRing;
        if (type == BlendTreeType::SimpleDirectional2D) positions = {Vector2(0, 0), Vector2(0, 1), Vector2(1, 0), Vector2(0, -1), Vector2(-1, 0)};
        auto compiled = compileController(treeController(type, positions));
        for (int i = 0; i < 64; i++){
            for (int j = 0; j < 64; j++){
                const float x = -3.0f + 6.0f * (float)i / 63.0f;
                const float y = -3.0f + 6.0f * (float)j / 63.0f;
                const std::vector<float> w = weights(*compiled, x, y);
                float sum = 0.0f;
                for (float v : w){
                    EXPECT_GE(v, 0.0f);
                    sum += v;
                }
                EXPECT_NEAR(sum, 1.0f, 1e-6f) << blendTreeTypeName(type) << " at " << x << ", " << y;
            }
        }
    }
}

TEST(AnimatorBlendTree, ParameterOnAChildGivesItWeightOne){
    for (BlendTreeType type : {BlendTreeType::SimpleDirectional2D, BlendTreeType::FreeformDirectional2D, BlendTreeType::FreeformCartesian2D}){
        std::vector<Vector2> positions = kRing;
        if (type == BlendTreeType::SimpleDirectional2D) positions = {Vector2(0, 0), Vector2(0, 1), Vector2(1, 0), Vector2(0, -1), Vector2(-1, 0)};
        auto compiled = compileController(treeController(type, positions));
        for (size_t c = 0; c < positions.size(); c++){
            const std::vector<float> w = weights(*compiled, positions[c].x, positions[c].y);
            EXPECT_NEAR(w[c], 1.0f, 1e-6f) << blendTreeTypeName(type) << " child " << c;
        }
    }
}

TEST(AnimatorBlendTree, OneDimensionalIsExactAtThresholds){
    auto compiled = compileController(treeController(BlendTreeType::Simple1D, {}, {0.0f, 0.5f, 2.0f, 4.0f}));
    const float thresholds[] = {0.0f, 0.5f, 2.0f, 4.0f};
    for (int c = 0; c < 4; c++){
        const std::vector<float> w = weights(*compiled, thresholds[c], 0);
        for (int k = 0; k < 4; k++) EXPECT_EQ(w[k], k == c ? 1.0f : 0.0f);
    }
    const std::vector<float> half = weights(*compiled, 1.25f, 0);
    EXPECT_NEAR(half[1], 0.5f, 1e-6f);
    EXPECT_NEAR(half[2], 0.5f, 1e-6f);
    EXPECT_EQ(weights(*compiled, -5.0f, 0)[0], 1.0f) << "clamped below";
    EXPECT_EQ(weights(*compiled, 50.0f, 0)[3], 1.0f) << "clamped above";
    // children are sorted by threshold on load
    auto unsorted = compileController(treeController(BlendTreeType::Simple1D, {}, {3.0f, 1.0f, 2.0f}));
    EXPECT_EQ(unsorted->children[0].threshold, 1.0f);
}

TEST(AnimatorBlendTree, TwentyChildrenAreAllUsed){
    std::vector<Vector2> positions;
    for (int i = 0; i < 20; i++){
        const float angle = (float)i * 6.2831853f / 20.0f;
        const float radius = 1.0f + (float)(i % 3);
        positions.push_back(Vector2(std::cos(angle) * radius, std::sin(angle) * radius));
    }
    for (BlendTreeType type : {BlendTreeType::FreeformDirectional2D, BlendTreeType::FreeformCartesian2D}){
        auto compiled = compileController(treeController(type, positions));
        ASSERT_EQ(compiled->trees[0].childCount, 20u);
        std::set<size_t> used;
        for (int i = 0; i < 64; i++){
            for (int j = 0; j < 64; j++){
                const std::vector<float> w = weights(*compiled, -3.5f + 7.0f * i / 63.0f, -3.5f + 7.0f * j / 63.0f);
                for (size_t c = 0; c < w.size(); c++) if (w[c] > 0.0f) used.insert(c);
            }
        }
        for (size_t c = 0; c < positions.size(); c++){
            const std::vector<float> w = weights(*compiled, positions[c].x, positions[c].y);
            if (w[c] > 0.0f) used.insert(c);
        }
        EXPECT_EQ(used.size(), 20u) << blendTreeTypeName(type);
    }
}

TEST(AnimatorBlendTree, DirectPassesClampedParameters){
    auto compiled = compileController(treeController(BlendTreeType::Direct, {Vector2(), Vector2(), Vector2()}));
    const std::vector<float> w = weights(*compiled, 0.25f, -1.0f);
    EXPECT_EQ(w[0], 0.25f);
    EXPECT_EQ(w[1], 0.0f) << "negative weights clamp to 0";
    EXPECT_EQ(w[2], 0.25f);
}

TEST(AnimatorBlendTree, NestedTreesMultiplyWeights){
    ControllerDef def = treeController(BlendTreeType::Simple1D, {}, {0.0f, 1.0f});
    BlendTreeDef inner;
    inner.type = BlendTreeType::Simple1D;
    inner.parameter = "Y";
    for (int i = 0; i < 2; i++){
        BlendChildDef child;
        child.motion.clip.path = "inner" + std::to_string(i) + ".anim";
        child.threshold = (float)i;
        child.timeScale = 2.0f;
        inner.children.push_back(child);
    }
    def.layers[0].machine.states[0].motion.tree[0].children[1].motion = MotionDef();
    def.layers[0].machine.states[0].motion.tree[0].children[1].motion.tree.push_back(inner);
    auto compiled = compileController(def);
    MotionLeaf leaves[8];
    const float parameters[2] = {0.5f, 0.25f};
    const size_t count = motionLeaves(*compiled, compiled->states[0].motion, parameters, 1.0f, leaves, 0, 8);
    ASSERT_EQ(count, 3u);
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) sum += leaves[i].weight;
    EXPECT_NEAR(sum, 1.0f, 1e-6f);
    EXPECT_NEAR(leaves[1].weight, 0.5f * 0.75f, 1e-6f);
    EXPECT_NEAR(leaves[2].weight, 0.5f * 0.25f, 1e-6f);
    EXPECT_EQ(leaves[1].timeScale, 2.0f);
    EXPECT_EQ(compiled->maxTreeDepth, 2);
}

// The children of a tree play phase-synced (one normalised time): after 10 s of blended playback
// of two clips of different lengths their normalised times still agree
TEST(AnimatorBlendTree, PhaseSyncedChildrenDoNotDrift){
    AnimatorFixture f;
    const std::string walk = f.clip("walk", slide("Hips", 1.0f, ClipWrapMode::Loop, 1.0f));
    const std::string run = f.clip("run", slide("Hips", 2.0f, ClipWrapMode::Loop, 0.7f));
    ControllerDef def;
    def.parameters = {{"Speed", AnimatorParameterType::Float, 0.5f}};
    LayerDef layer;
    layer.name = "Base Layer";
    StateDef s;
    s.name = "Move";
    BlendTreeDef tree;
    tree.type = BlendTreeType::Simple1D;
    tree.parameter = "Speed";
    BlendChildDef a;
    a.motion.clip.path = walk;
    a.threshold = 0.0f;
    BlendChildDef b;
    b.motion.clip.path = run;
    b.threshold = 1.0f;
    tree.children = {a, b};
    s.motion.tree.push_back(tree);
    layer.machine.states.push_back(s);
    def.layers.push_back(layer);
    f.animator(f.controllerFile("tree", def));
    f.step(0.0);
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 600; i++){
        f.step(dt);
        // the hips' x is walk(t * 1) * 0.5 + run(t * 0.7) * 0.5 with one normalised t: both clips
        // are at the same phase, so x / (0.5 * 1 + 0.5 * 2) is the phase itself
        const anim::AnimatorInstance& instance = f.instance();
        const double normalized = instance.layers[0].current.time;
        const double phase = normalized - std::floor(normalized);
        const float expected = (float)phase * 1.5f;
        EXPECT_NEAR(f.position(f.hips).x, expected, 1e-4f) << "frame " << i;
    }
    // the tree's length is the weighted average of its children's
    EXPECT_NEAR(f.instance().layers[0].current.length, 0.85f, 1e-5f);
}
