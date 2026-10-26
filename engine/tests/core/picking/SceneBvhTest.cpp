// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SceneBvh (docs/specs/S26-scene-view.md R7, R8, R15, AC6): the tree over entity bounds
// answers rays and frusta as a linear scan does while its boxes move, refits in parallel as it
// does serially, and the scene-level BVH follows entities, layers and editor flags. Cases
// adapted from OpenEngine's SceneTlasTests.cpp (Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License, NOTICE.md).

#include "picking/SceneBvh.h"
#include "picking/MeshRaycast.h"
#include "object/Primitive.h"
#include "picking/PickingTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<AABB> randomBoxes(size_t count, std::mt19937& rng, float extent){
        std::uniform_real_distribution<float> position(-extent, extent);
        std::uniform_real_distribution<float> size(0.05f, 1.5f);
        std::vector<AABB> boxes(count);
        for (AABB& box : boxes){
            const Vector3 c(position(rng), position(rng), position(rng));
            const Vector3 h(size(rng), size(rng), size(rng));
            box = AABB(c - h, c + h);
        }
        return boxes;
    }

    bool slabHit(const AABB& box, const Vector3& origin, const Vector3& direction, float tMax, float& tEnter){
        float t0 = 0.0f, t1 = tMax;
        for (unsigned a = 0; a < 3; a++){
            const float inv = 1.0f / (std::fabs(direction[a]) < 1e-30f ? (direction[a] < 0 ? -1e-30f : 1e-30f) : direction[a]);
            float tn = (box.getMinimum()[a] - origin[a]) * inv;
            float tf = (box.getMaximum()[a] - origin[a]) * inv;
            if (tn > tf) std::swap(tn, tf);
            // the tree's widening of the exit (MeshBvhSimd.h SLAB_ROBUST)
            tf *= 1.0000008f;
            t0 = std::max(t0, tn);
            t1 = std::min(t1, tf);
            if (t0 > t1) return false;
        }
        tEnter = t0;
        return true;
    }

    // the leaves a ray meets, by a linear scan and by the tree (visit keeps going)
    std::set<uint32_t> linearRay(const std::vector<AABB>& boxes, const Vector3& o, const Vector3& d, float tMax){
        std::set<uint32_t> found;
        for (uint32_t i = 0; i < boxes.size(); i++){
            float t;
            if (slabHit(boxes[i], o, d, tMax, t)) found.insert(i);
        }
        return found;
    }

    std::set<uint32_t> treeRay(const SceneBvhTree& tree, const Vector3& o, const Vector3& d, float tMax){
        std::set<uint32_t> found;
        tree.raycast(o, d, tMax, [&](uint32_t leaf, float){ found.insert(leaf); return tMax; });
        return found;
    }

    // a perspective-like frustum looking down -Z from z = 20 over [-w, w]^2 at the near plane
    void makeFrustum(Plane planes[6], float halfWidth){
        planes[0] = Plane(Vector3(0, 0, -1), Vector3(0, 0, 19.0f));   // near: inside below z = 19
        planes[1] = Plane(Vector3(0, 0, 1), Vector3(0, 0, -30.0f));   // far
        planes[2] = Plane(Vector3(1, 0, 0.15f).normalized(), Vector3(-halfWidth, 0, 0));
        planes[3] = Plane(Vector3(-1, 0, 0.15f).normalized(), Vector3(halfWidth, 0, 0));
        planes[4] = Plane(Vector3(0, -1, 0.15f).normalized(), Vector3(0, halfWidth, 0));
        planes[5] = Plane(Vector3(0, 1, 0.15f).normalized(), Vector3(0, -halfWidth, 0));
    }

    bool linearInFrustum(const AABB& box, const Plane planes[6]){
        for (int p = 0; p < 6; p++){
            if (planes[p].getSide(box.getCenter(), box.getHalfSize()) == Plane::NEGATIVE_SIDE) return false;
        }
        return true;
    }

}

TEST(SceneBvh, MatchesLinearScan){
    // 10k moving boxes over 100 frames, refitted every frame (rebuilt every 64th, as the
    // scene does): the tree meets exactly the boxes a linear scan does
    std::mt19937 rng(7);
    std::vector<AABB> boxes = randomBoxes(10000, rng, 50.0f);
    SceneBvhTree tree;
    tree.build(boxes);
    std::uniform_real_distribution<float> step(-0.4f, 0.4f);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_int_distribution<size_t> pick(0, boxes.size() - 1);
    for (int frame = 0; frame < 100; frame++){
        std::vector<uint32_t> moved;
        for (int m = 0; m < 300; m++){
            const uint32_t leaf = (uint32_t)pick(rng);
            const Vector3 delta(step(rng), step(rng), step(rng));
            boxes[leaf] = AABB(boxes[leaf].getMinimum() + delta, boxes[leaf].getMaximum() + delta);
            tree.setLeafBox(leaf, boxes[leaf]);
            moved.push_back(leaf);
        }
        if (frame % 64 == 63){
            tree.build(boxes);
        }else{
            tree.refit(moved);
        }
        for (int r = 0; r < 20; r++){
            Vector3 d(unit(rng), unit(rng), unit(rng));
            d.normalize();
            const Vector3 o = Vector3(unit(rng), unit(rng), unit(rng)) * 40.0f - d * 100.0f;
            ASSERT_EQ(treeRay(tree, o, d, 300.0f), linearRay(boxes, o, d, 300.0f)) << "frame " << frame;
        }
    }

    // closest: visit returning the box's entry prunes what is behind it
    for (int r = 0; r < 200; r++){
        Vector3 d(unit(rng), unit(rng), unit(rng));
        d.normalize();
        const Vector3 o = Vector3(unit(rng), unit(rng), unit(rng)) * 40.0f - d * 100.0f;
        float best = 1e30f;
        uint32_t bestLeaf = UINT32_MAX;
        for (uint32_t i = 0; i < boxes.size(); i++){
            float t;
            if (slabHit(boxes[i], o, d, 300.0f, t) && t < best){ best = t; bestLeaf = i; }
        }
        float treeBest = 300.0f;
        uint32_t treeLeaf = UINT32_MAX;
        tree.raycast(o, d, 300.0f, [&](uint32_t leaf, float tEnter){
            if (tEnter < treeBest || (tEnter == treeBest && leaf < treeLeaf)){ treeBest = tEnter; treeLeaf = leaf; }
            return treeBest;
        });
        if (bestLeaf == UINT32_MAX){
            EXPECT_EQ(treeLeaf, UINT32_MAX);
        }else{
            EXPECT_NEAR(treeBest, best, 1e-3f);
        }
    }
}

TEST(SceneBvh, FrustumMatchesCornerRule){
    std::mt19937 rng(11);
    const std::vector<AABB> boxes = randomBoxes(5000, rng, 40.0f);
    SceneBvhTree tree;
    tree.build(boxes);
    for (float halfWidth : {2.0f, 8.0f, 25.0f}){
        Plane planes[6];
        makeFrustum(planes, halfWidth);
        std::set<uint32_t> expected, found;
        for (uint32_t i = 0; i < boxes.size(); i++){
            if (linearInFrustum(boxes[i], planes)) expected.insert(i);
        }
        tree.frustum(planes, [&](uint32_t leaf){ found.insert(leaf); });
        // the broadphase never drops a box the linear rule keeps, and adds none outside
        EXPECT_EQ(found, expected) << halfWidth;
    }
}

TEST(SceneBvh, ParallelRefitEqualsSerial){
    std::mt19937 rng(13);
    std::vector<AABB> boxes = randomBoxes(60000, rng, 200.0f);
    SceneBvhTree serial, parallel;
    serial.build(boxes);
    parallel.build(boxes);
    std::uniform_real_distribution<float> step(-2.0f, 2.0f);
    for (size_t i = 0; i < boxes.size(); i++){
        const Vector3 delta(step(rng), step(rng), step(rng));
        boxes[i] = AABB(boxes[i].getMinimum() + delta, boxes[i].getMaximum() + delta * 1.5f);
        serial.setLeafBox((uint32_t)i, boxes[i]);
        parallel.setLeafBox((uint32_t)i, boxes[i]);
    }
    serial.refitAll(false);
    parallel.refitAll(true);
    const auto& a = serial.getNodes();
    const auto& b = parallel.getNodes();
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++){
        for (int k = 0; k < 3; k++){
            ASSERT_EQ(a[i].mn[k], b[i].mn[k]) << i;
            ASSERT_EQ(a[i].mx[k], b[i].mx[k]) << i;
        }
    }
    // and the root holds every box
    const AABB root = parallel.getBounds();
    for (const AABB& box : boxes){
        ASSERT_TRUE(root.contains(box));
    }
}

TEST(SceneBvh, BuildRefitRebuild){
    Scene scene;
    Primitive a(&scene);
    a.createBox(1, 1, 1);
    a.setPosition(0, 0, 0);
    Primitive b(&scene);
    b.createBox(1, 1, 1);
    b.setPosition(5, 0, 0);
    settleScene(scene);

    SceneBvh& bvh = scene.getPickBvh();
    PickFilter filter;
    std::vector<PickHit> hits;
    bvh.raycast(Ray(Vector3(5, 0, 10), Vector3(0, 0, -20)), filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, b.getEntity());
    EXPECT_EQ(bvh.getStats().builds, 1u);

    // moved: refitted, not rebuilt
    b.setPosition(-5, 0, 0);
    settleScene(scene);
    hits.clear();
    bvh.raycast(Ray(Vector3(5, 0, 10), Vector3(0, 0, -20)), filter, hits, false);
    EXPECT_TRUE(hits.empty());
    hits.clear();
    bvh.raycast(Ray(Vector3(-5, 0, 10), Vector3(0, 0, -20)), filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, b.getEntity());
    EXPECT_EQ(bvh.getStats().builds, 1u);
    EXPECT_GE(bvh.getStats().refits, 1u);

    // a new mesh: rebuilt
    Primitive c(&scene);
    c.createBox(1, 1, 1);
    c.setPosition(0, 5, 0);
    settleScene(scene);
    hits.clear();
    bvh.raycast(Ray(Vector3(0, 5, 10), Vector3(0, 0, -20)), filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, c.getEntity());
    EXPECT_EQ(bvh.getStats().builds, 2u);

    // after 64 refits, a rebuild
    for (int i = 0; i < (int)SceneBvh::REBUILD_AFTER_REFITS + 1; i++){
        a.setPosition(0, 0, (float)(i + 1) * 0.01f);
        settleScene(scene);
        hits.clear();
        bvh.raycast(Ray(Vector3(0, 0, 10), Vector3(0, 0, -20)), filter, hits, false);
    }
    EXPECT_EQ(bvh.getStats().builds, 3u);
}

TEST(SceneBvh, LayerMask){
    Scene scene;
    Primitive front(&scene);
    front.createBox(1, 1, 1);
    front.setPosition(0, 0, 2);
    Primitive back(&scene);
    back.createBox(1, 1, 1);
    back.setPosition(0, 0, -2);
    scene.setEntityLayer(front.getEntity(), 8);
    settleScene(scene);

    const Ray ray(Vector3(0, 0, 10), Vector3(0, 0, -20));
    PickFilter filter;
    std::vector<PickHit> hits;
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, front.getEntity());

    filter.layerMask = ~(1u << 8);
    hits.clear();
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, back.getEntity());

    // the layer moved back: the filter follows without a rebuild being asked for
    scene.setEntityLayer(front.getEntity(), 0);
    hits.clear();
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, front.getEntity());

    // MeshRaycast's default skips Ignore Raycast
    scene.setEntityLayer(front.getEntity(), IGNORE_RAYCAST_LAYER_INDEX);
    const MeshRaycastHit hit = MeshRaycast::raycast(&scene, ray);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.entity, back.getEntity());
}

TEST(SceneBvh, EditorFlags){
    Scene scene;
    Primitive front(&scene);
    front.createBox(1, 1, 1);
    front.setPosition(0, 0, 2);
    Primitive back(&scene);
    back.createBox(1, 1, 1);
    back.setPosition(0, 0, -2);
    settleScene(scene);

    scene.setEntityEditorFlags(front.getEntity(), EDITOR_PICK_LOCKED);
    EXPECT_TRUE(scene.hasEntityEditorFlags());
    const Ray ray(Vector3(0, 0, 10), Vector3(0, 0, -20));
    PickFilter filter;
    std::vector<PickHit> hits;
    // the runtime ignores the editor's flags
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, front.getEntity());
    // the Scene view skips locked ones
    filter.excludeEditorFlags = EDITOR_PICK_LOCKED | EDITOR_HIDDEN;
    hits.clear();
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, back.getEntity());

    scene.setEntityEditorFlags(front.getEntity(), 0);
    EXPECT_FALSE(scene.hasEntityEditorFlags());
    hits.clear();
    scene.getPickBvh().raycast(ray, filter, hits, false);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, front.getEntity());

    // the frustum broadphase honours them too
    scene.setEntityEditorFlags(back.getEntity(), EDITOR_HIDDEN);
    Plane planes[6];
    makeFrustum(planes, 10.0f);
    std::vector<Entity> inside;
    scene.getPickBvh().frustum(planes, filter, inside);
    EXPECT_EQ(inside, std::vector<Entity>{front.getEntity()});
}
