// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lattice runtime on the CPU side (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md
// §6.4, §12 WP-ECS EntityCloner): what a copy of a lattice target keeps.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/LatticeModifierComponent.h"
#include "object/Lattice.h"
#include "object/Sprite.h"
#include "registry/EntityCloner.h"

#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class LatticeRuntimeTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
        }
    };

}

// Copying a deformed sprite preserves its visible curved texture from its first
// rendered frame; an interim untessellated quad would distort the checker differently.
TEST_F(LatticeRuntimeTest, CopiedSpriteKeepsItsLatticeGrid){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(100, 100);
    sprite.setPivotPreset(PivotPreset::CENTER);
    sprite.setPosition(160, 160, 0);
    sprite.setTexture("common/checker.png");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen plain;
    ASSERT_TRUE(plain.read());
    ASSERT_GT(plain.countColor({0, 0, plain.width, plain.height}, 255, 0, 0, 20), 0u);

    Lattice& lattice = fixture.add<Lattice>();
    lattice.setPosition(160, 160, 0);
    lattice.setScale(200);
    lattice.setResolution(3, 3, 3);
    lattice.setOffset(1, 1, 1, Vector3(0.3f, 0, 0));
    ASSERT_TRUE(lattice.bind(sprite.getEntity()));
    scene.getComponent<LatticeModifierComponent>(sprite.getEntity()).subdivisions = 6;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen expected;
    ASSERT_TRUE(expected.read());
    ASSERT_GT(expected.countColor({0, 0, expected.width, expected.height}, 255, 0, 0, 20), 0u);
    ASSERT_NE(expected.rgba, plain.rgba) << "the curved lattice must visibly distort the checker";

    const std::vector<Entity> copies = EntityCloner::clone(&scene, {sprite.getEntity()}, &scene);
    ASSERT_EQ(copies.size(), size_t(1));
    sprite.setVisible(false);
    int visibleFrames = 0;
    ASSERT_TRUE(GpuEnvironment::runFrames([&](){
        Screen actual;
        if (!actual.read())
            return false;
        if (actual.countColor({0, 0, 640, 360}, 255, 0, 0, 20) == 0)
            return false; // copy has not produced its first rendered frame yet
        EXPECT_EQ(actual.rgba, expected.rgba) << "visible frame " << visibleFrames;
        return ++visibleFrames == 4;
    }, 300));
    scene.destroyEntity(copies[0]);
}
