// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SceneBvh (docs/specs/S26-scene-view.md R7, R8, R15, AC6): the tree over entity bounds
// answers rays and frusta as a linear scan does while its boxes move, refits in parallel as it
// does serially, and the scene-level BVH follows entities, layers and editor flags. Cases
// adapted from OpenEngine's SceneTlasTests.cpp (Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License, NOTICE.md).

#include "picking/SceneBvh.h"
#include "picking/MeshRaycast.h"
#include "picking/PickCache.h"
#include "object/Primitive.h"
#include "picking/PickingTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <random>
#include <set>
#include <vector>

namespace SceneBvhTest_cpp {

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
        // moved and grown (a valid box: Debug builds assert min <= max)
        const Vector3 grow(std::fabs(delta.x) * 0.5f, std::fabs(delta.y) * 0.5f, std::fabs(delta.z) * 0.5f);
        boxes[i] = AABB(boxes[i].getMinimum() + delta, boxes[i].getMaximum() + delta + grow);
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

// R31: a scene nobody queries has no pick BVH and its frames bind no mesh tree
TEST(SceneBvh, ZeroWhenUnused){
    const size_t entriesBefore = PickCache::get().stats().entries;
    Scene scene;
    std::vector<std::unique_ptr<Primitive>> spheres;
    for (int i = 0; i < 20; i++){
        auto sphere = std::make_unique<Primitive>(&scene);
        sphere->createSphere(1.0f, 32, 16);
        sphere->setPosition((float)i * 3.0f, 0.0f, 0.0f);
        spheres.push_back(std::move(sphere));
    }
    for (int frame = 0; frame < 5; frame++){
        spheres[0]->setPosition(0.0f, (float)frame, 0.0f);
        settleScene(scene);
    }
    EXPECT_EQ(scene.findPickBvh(), nullptr) << "no pick BVH without a query";
    EXPECT_EQ(PickCache::get().stats().entries, entriesBefore) << "no mesh tree was built or queued";
    // the first query builds it
    MeshRaycast::raycast(&scene, Vector3(3, 0, 10), Vector3(0, 0, -1), 100.0f);
    EXPECT_NE(scene.findPickBvh(), nullptr);
}

namespace {

    bool scanSphereTouches(const AABB& box, const Vector3& c, float r){
        float d2 = 0.0f;
        for (unsigned a = 0; a < 3; a++){
            const float v = c[a];
            if (v < box.getMinimum()[a]) d2 += (box.getMinimum()[a] - v) * (box.getMinimum()[a] - v);
            else if (v > box.getMaximum()[a]) d2 += (v - box.getMaximum()[a]) * (v - box.getMaximum()[a]);
        }
        return d2 <= r * r;
    }

    // the oriented box against the leaf box: their 8 corners' projections on all 15 axes
    bool scanBoxTouches(const AABB& box, const Vector3& c, const Vector3& half, const Quaternion& rotation){
        const Vector3 axes[3] = {rotation * Vector3(1, 0, 0), rotation * Vector3(0, 1, 0), rotation * Vector3(0, 0, 1)};
        const Vector3 world[3] = {Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)};
        Vector3 a[8], b[8];
        for (int i = 0; i < 8; i++){
            a[i] = c + axes[0] * ((i & 1) ? half.x : -half.x) + axes[1] * ((i & 2) ? half.y : -half.y) + axes[2] * ((i & 4) ? half.z : -half.z);
            b[i] = Vector3((i & 1) ? box.getMaximum().x : box.getMinimum().x, (i & 2) ? box.getMaximum().y : box.getMinimum().y,
                           (i & 4) ? box.getMaximum().z : box.getMinimum().z);
        }
        std::vector<Vector3> candidates(axes, axes + 3);
        candidates.insert(candidates.end(), world, world + 3);
        for (const Vector3& x : axes) for (const Vector3& y : world) candidates.push_back(x.crossProduct(y));
        for (const Vector3& axis : candidates){
            if (axis.squaredLength() < 1e-12f) continue;
            float minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
            for (int i = 0; i < 8; i++){
                minA = std::min(minA, a[i].dotProduct(axis)); maxA = std::max(maxA, a[i].dotProduct(axis));
                minB = std::min(minB, b[i].dotProduct(axis)); maxB = std::max(maxB, b[i].dotProduct(axis));
            }
            if (maxA < minB || maxB < minA) return false;
        }
        return true;
    }

}

// spatial-queries AC10: sphere and oriented-box overlaps of the scene's leaves equal a scan of
// every leaf (5,000 boxes, 1,000 queries)
TEST(SceneBvh, OverlapMatchesLinearScan){
    Scene scene;
    std::mt19937 rng(17);
    std::uniform_real_distribution<float> position(-60.0f, 60.0f);
    std::uniform_real_distribution<float> size(0.2f, 3.0f);
    std::vector<std::unique_ptr<Primitive>> boxes;
    for (int i = 0; i < 5000; i++){
        auto box = std::make_unique<Primitive>(&scene);
        box->createBox(size(rng), size(rng), size(rng));
        box->setPosition(position(rng), position(rng), position(rng));
        boxes.push_back(std::move(box));
    }
    settleScene(scene);
    SceneBvh& bvh = scene.getPickBvh();
    PickFilter filter;
    filter.kinds = MeshRaycast::OVERLAP_KINDS;
    const std::vector<SceneBvh::Leaf> leaves = bvh.getLeaves();
    ASSERT_EQ(leaves.size(), 5000u);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> reach(0.5f, 12.0f);
    std::vector<MeshOverlapHit> hits;
    size_t found = 0;
    for (int q = 0; q < 1000; q++){
        SceneBvh::OverlapShape shape;
        shape.box = q % 2 == 1;
        shape.center = Vector3(position(rng), position(rng), position(rng));
        shape.radius = reach(rng);
        shape.halfExtents = Vector3(reach(rng), reach(rng) * 0.5f, reach(rng));
        Quaternion rotation(unit(rng) + 1.5f, unit(rng), unit(rng), unit(rng));
        shape.rotation = rotation.normalize();
        hits.clear();
        bvh.overlap(shape, filter, hits, false);
        std::set<Entity> got;
        for (const MeshOverlapHit& hit : hits) got.insert(hit.entity);
        ASSERT_EQ(got.size(), hits.size()) << "each leaf once";
        ASSERT_TRUE(std::is_sorted(hits.begin(), hits.end(), [](const MeshOverlapHit& a, const MeshOverlapHit& b){ return a.entity < b.entity; }));
        std::set<Entity> expected;
        for (const SceneBvh::Leaf& leaf : leaves){
            const bool touches = shape.box ? scanBoxTouches(leaf.bounds, shape.center, shape.halfExtents, shape.rotation)
                                           : scanSphereTouches(leaf.bounds, shape.center, shape.radius);
            if (touches) expected.insert(leaf.entity);
        }
        ASSERT_EQ(got, expected) << "query " << q << (shape.box ? " (box)" : " (sphere)");
        found += hits.size();
    }
    EXPECT_GT(found, 1000u);
}

}  // namespace SceneBvhTest_cpp
