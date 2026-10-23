// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The lattice deformer's GPU path (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md
// §5) drawn by the engine into the harness target: a mesh and a lattice made in code, an
// orthographic camera looking down -z at 50 pixels per world unit. A lattice at rest draws
// exactly what no lattice draws; moved points bend the silhouette where the CPU solver puts
// it; the shadow map and the depth pre-pass (depth.vert) follow the colour pass (mesh.vert);
// a vertex colour mask, skinned meshes (before and after skinning) and instanced meshes.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "lattice/LatticeSolver.h"
#include "object/Camera.h"
#include "object/Lattice.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int screenWidth = 640;
    const int screenHeight = 360;
    const float pixelsPerUnit = 50.0f;

    // world (x, y) to the target: the camera looks down -z at the origin, y up
    float toScreenX(float x){ return screenWidth * 0.5f + x * pixelsPerUnit; }
    int toScreenRow(float y){ return int(std::lround(screenHeight * 0.5f - y * pixelsPerUnit)); }

    // A scene drawn by an orthographic camera, unlit unless a test adds a light
    struct LatticeScene {
        SceneFixture fixture;
        Camera camera;

        LatticeScene(): camera(&fixture.getScene()){
            camera.setOrtho(-6.4f, 6.4f, -3.6f, 3.6f, 0.1f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
        }

        Scene& scene(){ return fixture.getScene(); }

        // a box at the origin, subdivided so a curved deformation shows in its outline
        Primitive& addBox(float size, float r = 1.0f, float g = 1.0f, float b = 1.0f){
            Primitive& box = fixture.add<Primitive>();
            box.createBox(size, size, size, 16);
            box.setColor(r, g, b, 1.0f);
            return box;
        }

        // a 2x2x2 lattice (resolution 2: every interpolation reproduces the linear field of
        // its corners exactly) at a position, scaled to size
        Lattice& addLattice(float x, float y, float z, float size, LatticeInterpolation interpolation = LatticeInterpolation::CUBIC){
            Lattice& lattice = fixture.add<Lattice>();
            lattice.setPosition(x, y, z);
            lattice.setScale(size);
            LatticeComponent& component = lattice.getComponent<LatticeComponent>();
            component.resolutionX = 2;
            component.resolutionY = 2;
            component.resolutionZ = 2;
            component.offsets.assign(8, Vector3(0.0f, 0.0f, 0.0f));
            component.interpolation = interpolation;
            component.revision++;
            return lattice;
        }

        // the four points of the lattice's +y face moved along x (local units): a shear along x
        // that grows with y, which keeps every row of the outline at its height
        void shearTop(Lattice& lattice, float offsetX){
            LatticeComponent& component = lattice.getComponent<LatticeComponent>();
            for (int k = 0; k < 2; k++){
                for (int i = 0; i < 2; i++){
                    component.offsets[lattice::pointIndex(2, 2, i, 1, k)] = Vector3(offsetX, 0.0f, 0.0f);
                }
            }
            component.revision++;
        }

        void bind(Entity target, Entity lattice, LatticeStage stage = LatticeStage::AFTER_SKINNING,
                  LatticeMaskChannel mask = LatticeMaskChannel::NONE, bool includeChildren = false){
            LatticeModifierComponent modifier;
            LatticeBinding binding;
            binding.lattice = lattice;
            binding.stage = stage;
            binding.mask = mask;
            modifier.bindings.push_back(binding);
            modifier.includeChildren = includeChildren;
            scene().addComponent<LatticeModifierComponent>(target, modifier);
        }

        // frames until everything is loaded and every mesh draws the variant its bindings ask
        // for (a lattice appearing or going reloads the mesh once its new variant is built)
        bool settle(){
            Scene* current = &scene();
            return GpuEnvironment::runFrames([current](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.numSubmeshes == 0)
                        continue;
                    if (!mesh.loaded || mesh.needReload || mesh.needVariantReload || mesh.lattice.variantActive != (mesh.lattice.count > 0))
                        return false;
                }
                return true;
            }, 600);
        }
    };

    bool sameImage(const Screen& a, const Screen& b, int& differing){
        differing = 0;
        if (a.width != b.width || a.height != b.height)
            return false;
        for (size_t p = 0; p + 3 < a.rgba.size(); p += 4){
            if (a.rgba[p] != b.rgba[p] || a.rgba[p + 1] != b.rgba[p + 1] || a.rgba[p + 2] != b.rgba[p + 2])
                differing++;
        }
        return differing == 0;
    }

    // the left and right ends of what is drawn over the background in one row, within [x0, x1)
    InkBox rowInk(const Screen& screen, float worldY, int x0 = 0, int x1 = screenWidth){
        const int row = toScreenRow(worldY);
        return screen.inkOnBackground({x0, row, x1, row + 1}, 24);
    }

    // where the CPU solver moves a world point
    Vector3 solverPoint(Lattice& lattice, const Vector3& world){
        LatticeComponent& component = lattice.getComponent<LatticeComponent>();
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(component, padded);
        return lattice::deformWorldPoint(component, padded, lattice.getComponent<Transform>().modelMatrix, world);
    }

    // the last dark pixel of a row scanning left from the right edge down to x0: the right end
    // of a shadow on a lit wall (dark: under halfway between the wall's lit and shadowed values)
    int shadowRightEdge(const Screen& screen, float worldY, int x0, int litX){
        const int row = toScreenRow(worldY);
        auto luminance = [&](int x){
            const uint8_t* p = screen.at(x, row);
            return int(p[0]) + int(p[1]) + int(p[2]);
        };
        const int lit = luminance(litX);
        int dark = lit;
        for (int x = x0; x < litX; x++)
            dark = std::min(dark, luminance(x));
        const int threshold = (lit + dark) / 2;
        for (int x = litX; x >= x0; x--){
            if (luminance(x) < threshold)
                return x;
        }
        return -1;
    }

    class LatticeRenderTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
        }
    };

}

TEST_F(LatticeRenderTest, AtRestDrawsExactlyWhatNoLatticeDraws){
    LatticeScene s;
    Primitive& box = s.addBox(2.0f);
    ASSERT_TRUE(s.settle());
    Screen plain;
    ASSERT_TRUE(plain.read());
    const uint32_t plainProperties = box.getComponent<MeshComponent>().submeshes[0].shaderProperties;
    EXPECT_EQ(plainProperties & (1u << 5), 0u) << "no lattice: no 'Ltc' bit";

    Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f);
    s.bind(box.getEntity(), lattice.getEntity());
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = box.getComponent<MeshComponent>();
    ASSERT_EQ(mesh.lattice.count, 1);
    EXPECT_TRUE(mesh.lattice.variantActive);
    EXPECT_EQ(mesh.submeshes[0].shaderProperties, plainProperties | (1u << 5)) << "the same variant plus 'Ltc'";
    EXPECT_EQ(mesh.lattice.movingBindings, 0u);
    EXPECT_EQ(mesh.lattice.worldPadding, 0.0f);
    // record: 12 header texels and the 4x4x4 padded grid, in whole rows of 64
    EXPECT_EQ(mesh.lattice.texels.size(), size_t(LATTICE_TEXTURE_WIDTH * 2));

    Screen rest;
    ASSERT_TRUE(rest.read());
    int differing = 0;
    EXPECT_TRUE(sameImage(plain, rest, differing)) << differing << " pixels differ at rest";
    EXPECT_SCREEN_GOLDEN(rest, "lattice_rest");

    // the binding removed: the mesh goes back to its own variant, the state is reset
    s.scene().removeComponent<LatticeModifierComponent>(box.getEntity());
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(mesh.lattice.count, 0);
    EXPECT_FALSE(mesh.lattice.variantActive);
    EXPECT_TRUE(mesh.lattice.texels.empty());
    EXPECT_EQ(mesh.submeshes[0].shaderProperties, plainProperties);
    Screen unbound;
    ASSERT_TRUE(unbound.read());
    EXPECT_TRUE(sameImage(plain, unbound, differing)) << differing << " pixels differ unbound";
}

TEST_F(LatticeRenderTest, MovedPointsBendTheSilhouetteWhereTheSolverDoes){
    const LatticeInterpolation modes[] = {LatticeInterpolation::SHARP, LatticeInterpolation::SMOOTH, LatticeInterpolation::CUBIC};
    for (LatticeInterpolation mode : modes){
        SCOPED_TRACE("interpolation " + std::to_string(int(mode)));
        LatticeScene s;
        Primitive& box = s.addBox(2.0f);
        Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f, mode);
        s.shearTop(lattice, 0.4f);
        s.bind(box.getEntity(), lattice.getEntity());
        ASSERT_TRUE(s.settle());

        Screen screen;
        ASSERT_TRUE(screen.read());
        for (float y : {0.8f, 0.4f, 0.0f, -0.4f, -0.8f}){
            const InkBox ink = rowInk(screen, y);
            ASSERT_FALSE(ink.empty()) << "row at y " << y;
            const float left = toScreenX(solverPoint(lattice, Vector3(-1.0f, y, 1.0f)).x);
            const float right = toScreenX(solverPoint(lattice, Vector3(1.0f, y, 1.0f)).x);
            EXPECT_NEAR(ink.x0, left, 1.5f) << "left edge at y " << y;
            EXPECT_NEAR(ink.x1 + 1, right, 1.5f) << "right edge at y " << y;
        }
        // the top leans right by about 0.82 units, the bottom by 0.18
        EXPECT_GT(rowInk(screen, 0.8f).x0 - rowInk(screen, -0.8f).x0, 25);

        // the bounds grow by the largest padded offset (the ghost row above the top face
        // extrapolates it to 0.8 local units) at the lattice's scale 2.5
        const MeshComponent& mesh = box.getComponent<MeshComponent>();
        EXPECT_EQ(mesh.lattice.movingBindings, 1u);
        EXPECT_NEAR(mesh.lattice.worldPadding, 0.8f * 2.5f, 1e-4f);
        const Vector3 corner = solverPoint(lattice, Vector3(1.0f, 1.0f, 1.0f));
        EXPECT_LE(corner.x, mesh.worldAABB.getMaximum().x);

        if (mode == LatticeInterpolation::CUBIC){
            EXPECT_SCREEN_GOLDEN(screen, "lattice_shear");
        }
    }
}

TEST_F(LatticeRenderTest, ShadowsAndTheDepthPrePassFollow){
    // a wall behind the box takes its shadow from a light leaning to +x: the shadow's right end
    // is the box's right edge moved 0.3 units per unit of depth (4 from the front face)
    auto build = [](LatticeScene& s, bool shear, LatticeMaskChannel mask, bool prepass){
        s.scene().setDepthPrepassEnabled(prepass);
        s.scene().setGlobalIllumination(0.2f);
        Light& light = s.fixture.add<Light>();
        light.setType(LightType::DIRECTIONAL);
        light.setDirection(0.3f, 0.0f, -1.0f);
        light.setIntensity(4.0f);
        light.setShadows(true);
        Primitive& wall = s.fixture.add<Primitive>();
        wall.createWall(14.0f, 8.0f);
        wall.setPosition(0.0f, 0.0f, -3.0f);
        Primitive& box = s.addBox(2.0f, 0.9f, 0.3f, 0.2f);
        if (shear || mask != LatticeMaskChannel::NONE){
            Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f);
            if (shear)
                s.shearTop(lattice, 0.4f);
            s.bind(box.getEntity(), lattice.getEntity(), LatticeStage::AFTER_SKINNING, mask);
        }
    };

    Screen plain, sheared, masked, kept, prepass;
    {
        LatticeScene s;
        build(s, false, LatticeMaskChannel::NONE, false);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(plain.read());
    }
    {
        LatticeScene s;
        build(s, true, LatticeMaskChannel::NONE, false);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(sheared.read());
        EXPECT_SCREEN_GOLDEN(sheared, "lattice_shadow");
    }
    {
        // the box's vertex colour is white: RED masks nothing, INVERTED_RED everything, in the
        // shadow pass too (the depth variant reads the colour)
        LatticeScene s;
        build(s, true, LatticeMaskChannel::RED, false);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(masked.read());
    }
    {
        LatticeScene s;
        build(s, true, LatticeMaskChannel::INVERTED_RED, false);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(kept.read());
    }
    {
        LatticeScene s;
        build(s, true, LatticeMaskChannel::NONE, true);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(prepass.read());
    }

    const int litX = screenWidth - 4;
    const float shiftTop = 0.4f * (0.8f / 2.5f + 0.5f) * 2.5f;     // 0.82
    const float shiftBottom = 0.4f * (-0.8f / 2.5f + 0.5f) * 2.5f; // 0.18
    const int x0 = int(toScreenX(1.0f + shiftTop)) + 3;              // right of the sheared box
    const int plainTop = shadowRightEdge(plain, 0.8f, int(toScreenX(1.0f)) + 3, litX);
    const int plainBottom = shadowRightEdge(plain, -0.8f, int(toScreenX(1.0f)) + 3, litX);
    ASSERT_GT(plainTop, 0);
    EXPECT_NEAR(plainTop, toScreenX(1.0f + 0.3f * 4.0f), 4.0f) << "the unbound box's shadow";
    EXPECT_NEAR(plainBottom, toScreenX(1.0f + 0.3f * 4.0f), 4.0f);

    const int shearedTop = shadowRightEdge(sheared, 0.8f, x0, litX);
    const int shearedBottom = shadowRightEdge(sheared, -0.8f, int(toScreenX(1.0f + shiftBottom)) + 3, litX);
    EXPECT_NEAR(shearedTop - plainTop, shiftTop * pixelsPerUnit, 4.0f) << "the shadow map is drawn deformed";
    EXPECT_NEAR(shearedBottom - plainBottom, shiftBottom * pixelsPerUnit, 4.0f);

    int differing = 0;
    EXPECT_TRUE(sameImage(sheared, masked, differing)) << differing << " pixels differ with a mask of 1";
    EXPECT_TRUE(sameImage(plain, kept, differing)) << differing << " pixels differ with a mask of 0";
    // the pre-pass depth is the colour pass's, bit for bit: nothing of the box is lost to the test
    EXPECT_TRUE(sameImage(sheared, prepass, differing)) << differing << " pixels differ with the depth pre-pass";
}

TEST_F(LatticeRenderTest, SkinnedMeshesBeforeAndAfterSkinning){
    // skinned_column: 0.5 x 2 x 0.5 from y 0, its bottom on the Root joint and its top on Tip
    // (Root's child). Root moved to (2.5, -1): the column stands at x 2.25..2.75, y -1..1
    auto build = [](LatticeScene& s, LatticeStage stage, bool aroundBindPose) -> Model& {
        Model& model = s.fixture.add<Model>();
        EXPECT_TRUE(model.loadModel("lattice/skinned_column.gltf"));
        model.getBone("Root").setPosition(2.5f, -1.0f, 0.0f);
        // before skinning the lattice stands around the bind pose (the column at the origin)
        Lattice& lattice = aroundBindPose ? s.addLattice(0.0f, 1.0f, 0.0f, 2.5f) : s.addLattice(2.5f, 0.0f, 0.0f, 2.5f);
        s.shearTop(lattice, 0.4f);
        s.bind(model.getEntity(), lattice.getEntity(), stage, LatticeMaskChannel::NONE, true);
        return model;
    };
    const float shiftTop = 0.4f * (0.8f / 2.5f + 0.5f) * 2.5f;
    const float shiftBottom = 0.4f * (-0.8f / 2.5f + 0.5f) * 2.5f;

    {
        SCOPED_TRACE("after skinning, around the posed column");
        LatticeScene s;
        build(s, LatticeStage::AFTER_SKINNING, false);
        ASSERT_TRUE(s.settle());
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_NEAR(rowInk(screen, 0.8f).x0, toScreenX(2.25f + shiftTop), 1.5f);
        EXPECT_NEAR(rowInk(screen, -0.8f).x0, toScreenX(2.25f + shiftBottom), 1.5f);
        EXPECT_SCREEN_GOLDEN(screen, "lattice_skinned");
    }
    {
        SCOPED_TRACE("before skinning, around the bind pose");
        LatticeScene s;
        build(s, LatticeStage::BEFORE_SKINNING, true);
        ASSERT_TRUE(s.settle());
        Screen screen;
        ASSERT_TRUE(screen.read());
        // the same rows of the bind pose (y 1.8 and 0.2 there), then carried by the joints
        EXPECT_NEAR(rowInk(screen, 0.8f).x0, toScreenX(2.25f + shiftTop), 1.5f);
        EXPECT_NEAR(rowInk(screen, -0.8f).x0, toScreenX(2.25f + shiftBottom), 1.5f);
    }
    {
        SCOPED_TRACE("after skinning, around the bind pose: the posed column is outside it");
        LatticeScene s;
        build(s, LatticeStage::AFTER_SKINNING, true);
        ASSERT_TRUE(s.settle());
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_NEAR(rowInk(screen, 0.8f).x0, toScreenX(2.25f), 1.5f);
        EXPECT_NEAR(rowInk(screen, -0.8f).x0, toScreenX(2.25f), 1.5f);
    }
}

TEST_F(LatticeRenderTest, InstancesAreDeformedWhereTheyStand){
    LatticeScene s;
    Primitive& box = s.addBox(1.0f);
    box.createInstancedMesh();
    box.addInstance(-2.0f, 0.0f, 0.0f);
    box.addInstance(2.0f, 0.0f, 0.0f);
    // a lattice around the right instance only (the left one is 2 local units outside it,
    // past the fade)
    Lattice& lattice = s.addLattice(2.0f, 0.0f, 0.0f, 2.0f);
    s.shearTop(lattice, 0.4f);
    s.bind(box.getEntity(), lattice.getEntity());
    ASSERT_TRUE(s.settle());
    EXPECT_NE(box.getComponent<MeshComponent>().submeshes[0].shaderProperties & (1u << 18), 0u) << "'Ist'";

    Screen screen;
    ASSERT_TRUE(screen.read());
    const int middle = int(toScreenX(0.0f));
    for (float y : {0.4f, -0.4f}){
        const InkBox left = rowInk(screen, y, 0, middle);
        const InkBox right = rowInk(screen, y, middle, screenWidth);
        EXPECT_NEAR(left.x0, toScreenX(-2.5f), 1.5f) << "the left instance is not deformed, y " << y;
        EXPECT_NEAR(left.x1 + 1, toScreenX(-1.5f), 1.5f);
        EXPECT_NEAR(right.x0, toScreenX(solverPoint(lattice, Vector3(1.5f, y, 0.5f)).x), 1.5f) << "y " << y;
        EXPECT_NEAR(right.x1 + 1, toScreenX(solverPoint(lattice, Vector3(2.5f, y, 0.5f)).x), 1.5f);
    }
    EXPECT_SCREEN_GOLDEN(screen, "lattice_instanced");
}
