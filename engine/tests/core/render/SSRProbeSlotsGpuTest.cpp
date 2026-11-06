// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SSR G-buffer pass's reflection probe slots on the headless device
// (docs/superpowers/specs/2026-11-04-ssr-probes-design.md 6.5, 10.3 #9-#10): only the probes of
// drawn meshes with IBL count, the largest on screen get the composite's slots, and a probe left
// without one is reported (RenderSystem::getSSRStats) and logged once.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "object/environment/ReflectionProbe.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class SSRProbeSlotsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::vector<std::unique_ptr<Primitive>> tiles;
        std::vector<std::unique_ptr<ReflectionProbe>> probes;

        void SetUp() override{
            scene.setBackgroundColor(0.2f, 0.3f, 0.5f, 1.0f);
            scene.setSSREnabled(true);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 8.0f, 12.0f);
            camera.setTarget(0.0f, 0.0f, -4.0f);
            camera.setFarClip(300.0f);
            camera.activate();
            Engine::setScene(&scene);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.3f, -1.0f, 0.4f);
            sun.setShadows(false);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        // a metal tile of `size` at `position`, in a probe whose box holds just it
        ReflectionProbe& addTile(const Vector3& position, float size, bool lit = true){
            tiles.push_back(std::make_unique<Primitive>(&scene));
            Primitive& tile = *tiles.back();
            tile.createPlane(size, size);
            tile.setPosition(position);
            Material material = tile.getMaterial();
            material.metallicFactor = 1.0f;
            material.roughnessFactor = 0.1f;
            if (!lit)
                material.lighting = MaterialLighting::NONE;
            tile.setMaterial(material);
            tile.setReceiveIBL(true);

            probes.push_back(std::make_unique<ReflectionProbe>(&scene));
            ReflectionProbe& probe = *probes.back();
            probe.setPosition(position + Vector3(0.0f, 1.0f, 0.0f));
            probe.setBoxSize(size + 0.5f, 4.0f, size + 0.5f);
            probe.setResolution(32);
            return probe;
        }

        // frames until every probe captured (its stats list `expected` probes) or the budget ends
        const SSRViewStats& renderUntil(size_t expected){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }));
            const SSRViewStats& stats = scene.getSystem<RenderSystem>()->getSSRStats();
            for (int frame = 0; frame < 240 && stats.probes.size() < expected; frame++)
                GpuEnvironment::drawFrames(1);
            GpuEnvironment::drawFrames(2);
            return stats;
        }
    };

}

TEST_F(SSRProbeSlotsGpu, ProbesBeyondTheSlotsKeepTheirReflection){
    JASPER_REQUIRE_GPU();
    LogCapture log;
    std::vector<Entity> near;
    for (int i = 0; i < 4; i++)
        near.push_back(addTile(Vector3(-7.5f + 5.0f * (float)i, 0.0f, 0.0f), 4.0f).getEntity());
    // farther and small: the least screen, well inside the view
    const Entity far = addTile(Vector3(0.0f, 0.0f, -30.0f), 1.0f).getEntity();

    const SSRViewStats& stats = renderUntil(5);
    ASSERT_TRUE(stats.valid);
    std::string listed;
    for (const SSRViewStats::Probe& probe : stats.probes)
        listed += " " + std::to_string(probe.entity) + "(slot " + std::to_string(probe.slot) + ", " + std::to_string(probe.coverage) + ")";
    ASSERT_EQ(stats.probes.size(), 5u) << "far " << far << ", listed:" << listed;
    EXPECT_EQ(stats.probesWithoutSlot(), 1u);
    // largest first: the far probe is the last, without a slot
    EXPECT_EQ(stats.probes.back().entity, far);
    EXPECT_EQ(stats.probes.back().slot, -1);
    for (const SSRViewStats::Probe& probe : stats.probes){
        if (probe.entity != far){
            EXPECT_GE(probe.slot, 0);
            EXPECT_NE(std::find(near.begin(), near.end(), probe.entity), near.end());
        }
    }

    // reported once, not every frame
    GpuEnvironment::drawFrames(5);
    EXPECT_EQ(log.count(D_LOG_WARN, "reflection probes in view"), 1u);
}

TEST_F(SSRProbeSlotsGpu, CulledAndNonIBLMeshesTakeNoSlot){
    JASPER_REQUIRE_GPU();
    // behind the camera
    addTile(Vector3(0.0f, 0.0f, 40.0f), 4.0f);
    // in view, unlit: it reflects no environment
    addTile(Vector3(-6.0f, 0.0f, -2.0f), 4.0f, false);
    // in view and lit: the only one with a slot
    const Entity lit = addTile(Vector3(4.0f, 0.0f, -2.0f), 4.0f).getEntity();

    renderUntil(1);
    // more frames: every probe has captured by now, still only the drawn lit tile's counts
    GpuEnvironment::drawFrames(60);
    const SSRViewStats& stats = scene.getSystem<RenderSystem>()->getSSRStats();
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.probes.size(), 1u);
    EXPECT_EQ(stats.probes[0].entity, lit);
    EXPECT_EQ(stats.probes[0].slot, 0);
    EXPECT_GT(stats.probes[0].coverage, 0.0f);
}
