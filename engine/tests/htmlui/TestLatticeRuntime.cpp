// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lattice runtime on the CPU side (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md
// §6.4, §12 WP-ECS EntityCloner): what a copy of a lattice target keeps.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "object/Lattice.h"
#include "object/Sprite.h"
#include "registry/EntityCloner.h"
#include "subsystem/RenderSystem.h"

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

// A copied sprite keeps the grid its vertices were built for: the copy's sprite rebuild makes
// the same grid again, never the quad (which it would draw bilinearly bent for a frame, then
// rebuild and reload as a grid).
TEST_F(LatticeRuntimeTest, CopiedSpriteKeepsItsLatticeGrid){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    Sprite& sprite = fixture.add<Sprite>();
    sprite.setSize(100, 100);
    Lattice& lattice = fixture.add<Lattice>();
    lattice.setScale(Vector3(200.0f, 200.0f, 200.0f));
    ASSERT_TRUE(lattice.bind(sprite.getEntity()));
    scene.getComponent<LatticeModifierComponent>(sprite.getEntity()).subdivisions = 6;

    const int gridVertices = 7 * 7;
    ASSERT_TRUE(GpuEnvironment::runFrames([&](){
        const MeshComponent& mesh = sprite.getComponent<MeshComponent>();
        return mesh.loaded && !mesh.needReload && mesh.buffer.getCount() == (unsigned int)gridVertices;
    }, 300)) << "the bound sprite becomes a 6 x 6 grid";

    const std::vector<Entity> copies = EntityCloner::clone(&scene, {sprite.getEntity()}, &scene);
    ASSERT_EQ(copies.size(), size_t(1));
    const Entity copy = copies[0];

    {
        const MeshComponent& copied = scene.getComponent<MeshComponent>(copy);
        EXPECT_TRUE(copied.lattice.wantsTessellation) << "the copy keeps the grid request";
        EXPECT_EQ(copied.lattice.tessellation, 6);
        EXPECT_EQ(copied.lattice.count, 0) << "the records are packed anew for the copy";
        EXPECT_TRUE(copied.lattice.texels.empty());
    }

    // frame by frame until the copy is loaded: its vertices stay the grid's
    std::vector<unsigned int> counts;
    bool reloaded = false;
    ASSERT_TRUE(GpuEnvironment::runFrames([&](){
        const MeshComponent& copied = scene.getComponent<MeshComponent>(copy);
        counts.push_back(copied.buffer.getCount());
        reloaded = reloaded || copied.needReload;
        return copied.loaded && copied.lattice.count == 1 && counts.size() >= 4;
    }, 300));
    for (size_t frame = 0; frame < counts.size(); frame++){
        EXPECT_EQ(counts[frame], (unsigned int)gridVertices) << "frame " << frame << " after the copy";
    }
    EXPECT_FALSE(reloaded) << "the copy is loaded once, as the grid";

    scene.destroyEntity(copy);
}
