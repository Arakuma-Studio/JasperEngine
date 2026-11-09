// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The lattice deformer's GPU path (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md
// §5) drawn by the engine into the harness target: a mesh and a lattice made in code, an
// orthographic camera looking down -z at 50 pixels per world unit. A lattice at rest draws
// exactly what no lattice draws; moved points bend the silhouette where the CPU solver puts
// it; the shadow map and the depth pre-pass (depth.vert) follow the colour pass (mesh.vert);
// a vertex colour mask, skinned meshes (before and after skinning) and instanced meshes.

#include "TestGoldenCommon.h"
#include "component/Transform.h"

#include "Scene.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "lattice/LatticeSolver.h"
#include "object/Camera.h"
#include "object/GameObject.h"
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

        // a box at the origin (one quad per side)
        Primitive& addBox(float size, float r = 1.0f, float g = 1.0f, float b = 1.0f){
            Primitive& box = fixture.add<Primitive>();
            box.createBox(size, size, size);
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
    const uint64_t plainProperties = box.getComponent<MeshComponent>().submeshes[0].shaderProperties;
    EXPECT_EQ(plainProperties & shaderPropertyBit(32), 0u) << "no lattice: no 'Ltc' bit";

    Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f);
    s.bind(box.getEntity(), lattice.getEntity());
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = box.getComponent<MeshComponent>();
    ASSERT_EQ(mesh.lattice.count, 1);
    EXPECT_TRUE(mesh.lattice.variantActive);
    EXPECT_EQ(mesh.submeshes[0].shaderProperties, plainProperties | shaderPropertyBit(32)) << "the same variant plus 'Ltc'";
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
        // the box's sides are single quads: the GPU moves their corners (where the solver
        // does) and the rasterizer draws the straight edges between them
        const Vector3 bottomLeft = solverPoint(lattice, Vector3(-1.0f, -1.0f, 1.0f));
        const Vector3 topLeft = solverPoint(lattice, Vector3(-1.0f, 1.0f, 1.0f));
        const Vector3 bottomRight = solverPoint(lattice, Vector3(1.0f, -1.0f, 1.0f));
        const Vector3 topRight = solverPoint(lattice, Vector3(1.0f, 1.0f, 1.0f));
        for (float y : {0.8f, 0.4f, 0.0f, -0.4f, -0.8f}){
            const InkBox ink = rowInk(screen, y);
            ASSERT_FALSE(ink.empty()) << "row at y " << y;
            const float t = (y + 1.0f) * 0.5f;
            const float left = toScreenX(bottomLeft.x + (topLeft.x - bottomLeft.x) * t);
            const float right = toScreenX(bottomRight.x + (topRight.x - bottomRight.x) * t);
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

TEST_F(LatticeRenderTest, NormalsFollowTheJacobian){
    // the right face's points of a lattice around the box pushed toward the camera tilt its
    // front face: z' = z + 0.4 u_x (local units), a slope of 0.4, so its normal (the cofactor
    // of the Jacobian) is (-0.4, 0, 1) normalised. The same box rotated about y to that normal,
    // lit by the same light and seen at the same place, shades its front face the same
    const float angle = std::atan2(-0.4f, 1.0f) * 180.0f / 3.14159265f;
    enum class Box { DEFORMED, ROTATED, PLAIN };
    auto render = [angle](Box kind, Screen& screen){
        LatticeScene s;
        s.scene().setGlobalIllumination(0.1f);
        Light& light = s.fixture.add<Light>();
        light.setType(LightType::DIRECTIONAL);
        light.setDirection(0.5f, -0.3f, -1.0f);
        light.setIntensity(3.0f);
        Primitive& box = s.addBox(2.0f, 0.8f, 0.8f, 0.8f);
        if (kind == Box::DEFORMED){
            Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f, LatticeInterpolation::SHARP);
            LatticeComponent& component = lattice.getComponent<LatticeComponent>();
            for (int k = 0; k < 2; k++){
                for (int j = 0; j < 2; j++){
                    component.offsets[lattice::pointIndex(2, 2, 1, j, k)] = Vector3(0.0f, 0.0f, 0.4f);
                }
            }
            component.revision++;
            s.bind(box.getEntity(), lattice.getEntity());
        }else if (kind == Box::ROTATED){
            box.setRotation(0.0f, angle, 0.0f);
        }
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE(screen.read());
    };
    Screen deformed, rotated, plain;
    render(Box::DEFORMED, deformed);
    render(Box::ROTATED, rotated);
    render(Box::PLAIN, plain);

    const int x = int(toScreenX(0.0f));
    const int row = toScreenRow(0.2f);
    const uint8_t* a = deformed.at(x, row);
    const uint8_t* b = rotated.at(x, row);
    const uint8_t* f = plain.at(x, row);
    for (int c = 0; c < 3; c++){
        EXPECT_NEAR(int(a[c]), int(b[c]), 1) << "channel " << c;
    }
    EXPECT_GT(std::abs(int(f[0]) - int(a[0])), 2) << "the tilt changes the shading";
    EXPECT_SCREEN_GOLDEN(deformed, "lattice_normals");
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

namespace {

    // a deterministic offset in [-amount, amount]^3 per point (a fixed integer hash)
    Vector3 hashedOffset(int index, int salt, float amount){
        auto unit = [](uint32_t h){
            h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
            return float(h & 0xFFFFFF) / float(0xFFFFFF) * 2.0f - 1.0f;
        };
        const uint32_t base = uint32_t(index) * 3u + uint32_t(salt) * 7919u;
        return Vector3(unit(base), unit(base + 1u), unit(base + 2u)) * amount;
    }

}

// The GPU against the CPU solver at many points of a non-trivial lattice (4 x 3 x 5 points,
// hashed offsets, rotated and non-uniformly scaled) for every interpolation and outside mode,
// inside and outside the cage (the fade band and past it), with a stacked second lattice and
// a weight: small boxes, children of a rotated and scaled parent whose Lattice Modifier hands
// its bindings down, each drawn where lattice::deformWorldPoint puts its centre (the
// centroid of its silhouette: the deformation is affine at the scale of a box).
TEST_F(LatticeRenderTest, GpuMatchesTheSolverAtManyPoints){
    // 100 pixels per world unit
    const float zoom = 100.0f;
    auto screenX = [&](float x){ return screenWidth * 0.5f + x * zoom; };
    auto screenY = [&](float y){ return screenHeight * 0.5f - y * zoom; };

    struct Config {
        const char* name;
        LatticeInterpolation interpolation;
        LatticeOutside outside;
        float fadeDistance;
        bool stacked;
        bool mirrored = false; // negative scales on the lattice and the markers' parent
    };
    const Config configs[] = {
        {"sharp extend", LatticeInterpolation::SHARP, LatticeOutside::EXTEND, 0.25f, false},
        {"smooth extend", LatticeInterpolation::SMOOTH, LatticeOutside::EXTEND, 0.25f, false},
        {"cubic extend", LatticeInterpolation::CUBIC, LatticeOutside::EXTEND, 0.25f, false},
        {"sharp fade", LatticeInterpolation::SHARP, LatticeOutside::FADE, 0.5f, false},
        {"smooth fade", LatticeInterpolation::SMOOTH, LatticeOutside::FADE, 0.5f, false},
        {"cubic fade", LatticeInterpolation::CUBIC, LatticeOutside::FADE, 0.5f, false},
        {"cubic fade 0", LatticeInterpolation::CUBIC, LatticeOutside::FADE, 0.0f, false},
        {"cubic fade, stacked", LatticeInterpolation::CUBIC, LatticeOutside::FADE, 0.5f, true},
        {"smooth fade, mirrored", LatticeInterpolation::SMOOTH, LatticeOutside::FADE, 0.5f, false, true},
    };

    for (const Config& config : configs){
        SCOPED_TRACE(config.name);
        LatticeScene s;
        s.camera.setOrtho(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 100.0f);

        Lattice& lattice = s.fixture.add<Lattice>();
        lattice.setPosition(0.1f, -0.05f, 0.0f);
        lattice.setRotation(12.0f, -18.0f, 9.0f);
        lattice.setScale(Vector3(config.mirrored ? -3.4f : 3.4f, 2.2f, 2.6f));
        LatticeComponent& component = lattice.getComponent<LatticeComponent>();
        component.resolutionX = 4;
        component.resolutionY = 3;
        component.resolutionZ = 5;
        component.offsets.resize(4 * 3 * 5);
        for (int p = 0; p < 4 * 3 * 5; p++)
            component.offsets[p] = hashedOffset(p, 1, 0.07f);
        component.interpolation = config.interpolation;
        component.outside = config.outside;
        component.fadeDistance = config.fadeDistance;
        component.revision++;

        // the second lattice: smaller, over the right half, a sharp 3 x 3 x 3
        Lattice* second = nullptr;
        if (config.stacked){
            second = &s.fixture.add<Lattice>();
            second->setPosition(0.9f, 0.2f, 0.0f);
            second->setRotation(0.0f, 0.0f, -25.0f);
            second->setScale(Vector3(2.0f, 1.6f, 2.0f));
            LatticeComponent& c2 = second->getComponent<LatticeComponent>();
            c2.resolutionX = 3;
            c2.resolutionY = 3;
            c2.resolutionZ = 3;
            c2.offsets.resize(27);
            for (int p = 0; p < 27; p++)
                c2.offsets[p] = hashedOffset(p, 2, 0.06f);
            c2.interpolation = LatticeInterpolation::SHARP;
            c2.outside = LatticeOutside::FADE;
            c2.fadeDistance = 0.3f;
            c2.revision++;
        }

        // the markers: children of a rotated, scaled parent, bound through its modifier
        GameObject& rig = s.fixture.add<GameObject>();
        rig.setPosition(0.05f, 0.0f, 0.0f);
        rig.setRotation(0.0f, 0.0f, 8.0f);
        rig.setScale(config.mirrored ? Vector3(1.1f, -1.1f, 1.1f) : Vector3(1.1f, 1.1f, 1.1f));
        std::vector<Primitive*> markers;
        std::vector<Vector3> localCentres;
        for (int row = 0; row < 4; row++){
            for (int col = 0; col < 5; col++){
                Primitive& marker = s.fixture.add<Primitive>();
                marker.createBox(0.07f, 0.07f, 0.07f);
                // z alternates so the rotated lattice's z axis takes part
                const Vector3 local(-2.1f + col * 1.05f, -1.15f + row * 0.75f, ((row + col) % 2) ? 0.35f : -0.4f);
                marker.setPosition(local);
                rig.addChild(&marker);
                markers.push_back(&marker);
                localCentres.push_back(local);
            }
        }
        {
            LatticeModifierComponent modifier;
            LatticeBinding binding;
            binding.lattice = lattice.getEntity();
            binding.weight = config.stacked ? 1.3f : 1.0f;
            modifier.bindings.push_back(binding);
            if (second){
                LatticeBinding b2;
                b2.lattice = second->getEntity();
                b2.weight = 0.8f;
                modifier.bindings.push_back(b2);
            }
            modifier.includeChildren = true;
            s.scene().addComponent<LatticeModifierComponent>(rig.getEntity(), modifier);
        }
        ASSERT_TRUE(s.settle());

        Screen screen;
        ASSERT_TRUE(screen.read());

        std::vector<Vector3> padded, padded2;
        lattice::buildPaddedGrid(component, padded);
        if (second)
            lattice::buildPaddedGrid(second->getComponent<LatticeComponent>(), padded2);
        const Matrix4& latticeModel = lattice.getComponent<Transform>().modelMatrix;

        int moved = 0;
        for (size_t m = 0; m < markers.size(); m++){
            SCOPED_TRACE("marker " + std::to_string(m));
            const Transform& transform = markers[m]->getComponent<Transform>();
            const Vector3 rest = transform.modelMatrix * Vector3(0.0f, 0.0f, 0.0f);
            Vector3 expected = lattice::deformWorldPoint(component, padded, latticeModel, rest, config.stacked ? 1.3f : 1.0f);
            if (second)
                expected = lattice::deformWorldPoint(second->getComponent<LatticeComponent>(), padded2,
                                                     second->getComponent<Transform>().modelMatrix, expected, 0.8f);
            // without a fade band the field jumps at the cage's faces: a box across one is
            // drawn torn, its centroid is not where its centre goes
            if (config.fadeDistance <= 0.0f){
                const Matrix4 toLattice = latticeModel.inverse();
                int inside = 0;
                for (int c = 0; c < 8; c++){
                    const Vector3 corner(c & 1 ? 0.035f : -0.035f, c & 2 ? 0.035f : -0.035f, c & 4 ? 0.035f : -0.035f);
                    const Vector3 u = toLattice * (transform.modelMatrix * corner) + Vector3(0.5f, 0.5f, 0.5f);
                    if (u.x >= 0.0f && u.x <= 1.0f && u.y >= 0.0f && u.y <= 1.0f && u.z >= 0.0f && u.z <= 1.0f)
                        inside++;
                }
                if (inside != 0 && inside != 8)
                    continue;
            }
            if (std::fabs(expected.x - rest.x) * zoom > 2.0f || std::fabs(expected.y - rest.y) * zoom > 2.0f)
                moved++;

            // the coverage-weighted centroid of what is drawn around the expected place
            const int cx = int(std::lround(screenX(expected.x)));
            const int cy = int(std::lround(screenY(expected.y)));
            const int radius = 28;
            double sum = 0.0, sx = 0.0, sy = 0.0;
            for (int y = std::max(cy - radius, 0); y <= std::min(cy + radius, screenHeight - 1); y++){
                for (int x = std::max(cx - radius, 0); x <= std::min(cx + radius, screenWidth - 1); x++){
                    const uint8_t* p = screen.at(x, y);
                    const int ink = std::max(int(p[0]), std::max(int(p[1]), int(p[2]))) - goldenBackground;
                    if (ink <= 4)
                        continue;
                    sum += ink;
                    sx += ink * (x + 0.5);
                    sy += ink * (y + 0.5);
                }
            }
            ASSERT_GT(sum, 0.0) << "nothing drawn near (" << expected.x << ", " << expected.y << ")";
            const float drawnX = float(sx / sum);
            const float drawnY = float(sy / sum);
            EXPECT_NEAR(drawnX, screenX(expected.x), 0.6f) << "rest (" << rest.x << ", " << rest.y << ", " << rest.z << ")";
            EXPECT_NEAR(drawnY, screenY(expected.y), 0.6f) << "rest (" << rest.x << ", " << rest.y << ", " << rest.z << ")";
        }
        // not a test of nothing: most markers move by more than 2 pixels (outside the fade
        // band of "cubic fade 0" fewer do)
        EXPECT_GE(moved, 6) << "markers moved";
        if (config.interpolation == LatticeInterpolation::CUBIC && config.outside == LatticeOutside::FADE && !config.stacked && config.fadeDistance > 0.0f){
            EXPECT_SCREEN_GOLDEN(screen, "lattice_markers");
        }
    }
}

// The Jacobian's other outputs against the solver at points of a curved field (cubic and
// smooth, 4 x 3 x 5 points, the lattice rotated and non-uniformly scaled, so A^T and A^-1
// differ): the volume ratio (library:lattice_stretch with both colours black: the linear
// output is 1 - |log2 s| / log2 range) and the normal (the front face of each box lit by a
// directional light shades as a box rotated to lattice::deformVertex's normal, drawn where
// the solver puts the face's centre).
TEST_F(LatticeRenderTest, StretchAndNormalsMatchTheSolverInACurvedField){
    const float zoom = 100.0f;
    const float half = 0.045f; // the boxes' half size (small: the face's centre interpolates its corners)
    const float range = 4.0f;

    auto setupLattice = [](Lattice& lattice, LatticeInterpolation interpolation){
        lattice.setPosition(0.0f, 0.0f, 0.0f);
        lattice.setRotation(14.0f, -20.0f, 11.0f);
        lattice.setScale(Vector3(3.6f, 2.4f, 2.0f));
        LatticeComponent& component = lattice.getComponent<LatticeComponent>();
        component.resolutionX = 4;
        component.resolutionY = 3;
        component.resolutionZ = 5;
        component.offsets.resize(4 * 3 * 5);
        for (int p = 0; p < 4 * 3 * 5; p++)
            component.offsets[p] = hashedOffset(p, 3, 0.15f);
        component.interpolation = interpolation;
        component.revision++;
    };
    std::vector<Vector3> centres;
    for (int row = 0; row < 2; row++){
        for (int col = 0; col < 4; col++)
            centres.push_back(Vector3(-1.35f + col * 0.9f, -0.45f + row * 0.85f, (col % 2) ? 0.25f : -0.2f));
    }
    // and in the fade band (fade distance 0.25 local units), where its gradient takes part
    centres.push_back(Vector3(2.0f, -0.35f, 0.0f));
    centres.push_back(Vector3(-2.0f, 0.3f, 0.1f));
    centres.push_back(Vector3(0.4f, 1.4f, 0.0f));

    for (LatticeInterpolation mode : {LatticeInterpolation::SMOOTH, LatticeInterpolation::CUBIC}){
        SCOPED_TRACE("interpolation " + std::to_string(int(mode)));

        // The solver at the corners of the first triangle of each box's front face (corners
        // (-,-), (+,-), (+,+): MeshSystem::createBox), interpolated as the rasterizer does at the
        // pixel nearest the triangle's centroid (an orthographic camera: linear on the screen):
        // the field changes fast in the fade band, the face's centre is not enough there
        struct Expected { int x, y; Vector3 face; Vector3 normal; float stretch; };
        std::vector<Expected> expected;
        {
            LatticeScene s; // only for the matrices the engine computes
            Lattice& lattice = s.fixture.add<Lattice>();
            setupLattice(lattice, mode);
            ASSERT_TRUE(s.settle());
            const LatticeComponent& component = lattice.getComponent<LatticeComponent>();
            std::vector<Vector3> padded;
            lattice::buildPaddedGrid(component, padded);
            const Matrix4& latticeModel = lattice.getComponent<Transform>().modelMatrix;
            for (const Vector3& centre : centres){
                const Matrix4 model = Matrix4::translateMatrix(centre);
                lattice::ObjectSpaceLattice binding;
                binding.lattice = &component;
                binding.padded = &padded;
                binding.objectToNormalized = lattice::objectToNormalized(latticeModel, model);
                binding.normalizedToObject = binding.objectToNormalized.inverse();
                const Vector3 corners[3] = {Vector3(-half, -half, half), Vector3(half, -half, half), Vector3(half, half, half)};
                Vector3 position[3], normal[3];
                float stretch[3];
                float sx[3], sy[3];
                for (int c = 0; c < 3; c++){
                    position[c] = corners[c];
                    normal[c] = Vector3(0.0f, 0.0f, 1.0f);
                    stretch[c] = 1.0f;
                    lattice::deformVertex(binding, position[c], &normal[c], nullptr, &stretch[c], nullptr);
                    position[c] = model * position[c];
                    sx[c] = screenWidth * 0.5f + position[c].x * zoom;
                    sy[c] = screenHeight * 0.5f - position[c].y * zoom;
                }
                Expected e;
                e.x = int(std::floor((sx[0] + sx[1] + sx[2]) / 3.0f));
                e.y = int(std::floor((sy[0] + sy[1] + sy[2]) / 3.0f));
                // barycentric coordinates of the pixel's centre
                const float px = e.x + 0.5f, py = e.y + 0.5f;
                const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                const float b1 = ((px - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (py - sy[0])) / area;
                const float b2 = ((sx[1] - sx[0]) * (py - sy[0]) - (px - sx[0]) * (sy[1] - sy[0])) / area;
                const float b0 = 1.0f - b1 - b2;
                EXPECT_TRUE(b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f) << "the pixel is in the triangle";
                e.face = position[0] * b0 + position[1] * b1 + position[2] * b2;
                e.normal = normal[0] * b0 + normal[1] * b1 + normal[2] * b2;
                e.normal.normalizeL();
                e.stretch = stretch[0] * b0 + stretch[1] * b1 + stretch[2] * b2;
                expected.push_back(e);
            }
        }

        // the volume ratio
        {
            LatticeScene s;
            s.camera.setOrtho(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 100.0f);
            Lattice& lattice = s.fixture.add<Lattice>();
            setupLattice(lattice, mode);
            for (const Vector3& centre : centres){
                Primitive& box = s.addBox(2.0f * half);
                box.setPosition(centre);
                box.setCustomShader("library:lattice_stretch");
                box.setShaderUniform("squashColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
                box.setShaderUniform("stretchColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
                box.setShaderUniform("range", range);
                s.bind(box.getEntity(), lattice.getEntity());
            }
            ASSERT_TRUE(s.settle());
            Screen screen;
            ASSERT_TRUE(screen.read());
            int far = 0;
            for (size_t m = 0; m < centres.size(); m++){
                SCOPED_TRACE("box " + std::to_string(m));
                const float t = std::min(std::fabs(std::log2(expected[m].stretch)) / std::log2(range), 1.0f);
                const int value = int(std::lround(std::pow(1.0f - t, 1.0f / 2.2f) * 255.0f));
                if (std::fabs(expected[m].stretch - 1.0f) > 0.05f)
                    far++;
                EXPECT_TRUE(screen.is(expected[m].x, expected[m].y, value, value, value, 2))
                    << "volume ratio " << expected[m].stretch;
            }
            EXPECT_GE(far, 4) << "boxes whose volume changes by more than 5%";
        }

        // the normal
        Screen deformed, reference;
        auto light = [](LatticeScene& s){
            s.camera.setOrtho(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 100.0f);
            s.scene().setGlobalIllumination(0.1f);
            Light& light = s.fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(0.45f, -0.35f, -1.0f);
            light.setIntensity(3.0f);
        };
        {
            LatticeScene s;
            light(s);
            Lattice& lattice = s.fixture.add<Lattice>();
            setupLattice(lattice, mode);
            for (const Vector3& centre : centres){
                Primitive& box = s.addBox(2.0f * half, 0.8f, 0.8f, 0.8f);
                box.setPosition(centre);
                s.bind(box.getEntity(), lattice.getEntity());
            }
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(deformed.read());
        }
        {
            LatticeScene s;
            light(s);
            for (const Expected& e : expected){
                // +z turned to the normal, the front face's centre where the solver puts it
                const Vector3 z = e.normal;
                Vector3 x = Vector3(0.0f, 1.0f, 0.0f).crossProduct(z);
                x.normalizeL();
                const Vector3 y = z.crossProduct(x);
                Primitive& box = s.addBox(2.0f * half, 0.8f, 0.8f, 0.8f);
                box.setRotation(Quaternion(x, y, z));
                box.setPosition(e.face - z * half);
            }
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(reference.read());
        }
        int tilted = 0;
        for (size_t m = 0; m < centres.size(); m++){
            SCOPED_TRACE("box " + std::to_string(m));
            const int x = expected[m].x;
            const int y = expected[m].y;
            const uint8_t* r = reference.at(x, y);
            EXPECT_TRUE(deformed.is(x, y, r[0], r[1], r[2], 2)) << "normal (" << expected[m].normal.x << ", "
                                                                << expected[m].normal.y << ", " << expected[m].normal.z << ")";
            if (expected[m].normal.z < 0.995f)
                tilted++;
        }
        EXPECT_GE(tilted, 4) << "boxes whose front face tilts by more than 5.7 degrees";
    }
}

namespace {

    // what the packing must follow from frame to frame (LatticeRender::prepare): one box bound
    // to lattice A (and B, stacked, in either order)
    struct PackingConfig {
        Vector3 latticePosition = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 latticeRotation = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 latticeScale = Vector3(2.6f, 2.2f, 2.4f);
        int resolution[3] = {4, 3, 5};
        int salt = 5;
        LatticeInterpolation interpolation = LatticeInterpolation::CUBIC;
        bool enabled = true;
        float weight = 1.0f;
        Vector3 target = Vector3(0.0f, 0.0f, 0.0f);
        bool second = false;
        bool secondFirst = false;
    };

    void applyLattice(Lattice& lattice, const PackingConfig& config){
        lattice.setPosition(config.latticePosition);
        lattice.setRotation(config.latticeRotation.x, config.latticeRotation.y, config.latticeRotation.z);
        lattice.setScale(config.latticeScale);
        LatticeComponent& component = lattice.getComponent<LatticeComponent>();
        component.resolutionX = config.resolution[0];
        component.resolutionY = config.resolution[1];
        component.resolutionZ = config.resolution[2];
        const int points = config.resolution[0] * config.resolution[1] * config.resolution[2];
        component.offsets.assign(size_t(points), Vector3(0.0f, 0.0f, 0.0f));
        for (int p = 0; p < points; p++)
            component.offsets[p] = hashedOffset(p, config.salt, 0.12f);
        component.interpolation = config.interpolation;
        component.enabled = config.enabled;
        component.revision++;
    }

    void applyBindings(LatticeScene& s, Entity box, Entity a, Entity b, const PackingConfig& config){
        LatticeModifierComponent& modifier = s.scene().getComponent<LatticeModifierComponent>(box);
        modifier.bindings.clear();
        LatticeBinding first;
        first.lattice = a;
        first.weight = config.weight;
        LatticeBinding second;
        second.lattice = b;
        second.weight = 0.9f;
        if (config.second && config.secondFirst)
            modifier.bindings.push_back(second);
        modifier.bindings.push_back(first);
        if (config.second && !config.secondFirst)
            modifier.bindings.push_back(second);
    }

    struct PackingScene {
        LatticeScene s;
        Primitive* box = nullptr;
        Lattice* a = nullptr;
        Lattice* b = nullptr;

        explicit PackingScene(const PackingConfig& config){
            s.camera.setOrtho(-3.2f, 3.2f, -1.8f, 1.8f, 0.1f, 100.0f);
            box = &s.addBox(1.6f, 0.9f, 0.6f, 0.3f);
            a = &s.fixture.add<Lattice>();
            b = &s.fixture.add<Lattice>();
            b->setPosition(0.8f, 0.3f, 0.0f);
            b->setRotation(0.0f, 0.0f, 30.0f);
            b->setScale(Vector3(1.8f, 1.6f, 2.0f));
            PackingConfig other;
            other.resolution[0] = 3;
            other.resolution[1] = 3;
            other.resolution[2] = 3;
            other.salt = 9;
            other.latticePosition = Vector3(0.8f, 0.3f, 0.0f);
            other.latticeRotation = Vector3(0.0f, 0.0f, 30.0f);
            other.latticeScale = Vector3(1.8f, 1.6f, 2.0f);
            applyLattice(*b, other);
            s.scene().addComponent<LatticeModifierComponent>(box->getEntity(), LatticeModifierComponent());
            apply(config);
        }

        void apply(const PackingConfig& config){
            box->setPosition(config.target);
            applyLattice(*a, config);
            applyBindings(s, box->getEntity(), a->getEntity(), b->getEntity(), config);
        }
    };

}

// Changes from frame to frame draw what the same state drawn from scratch draws: the lattice
// moved with the same offsets, the target moved, a resolution change that keeps the texel
// count (4x3x5 and 5x3x4 both pad to 210), records growing to more rows of the texture and
// shrinking back, the lattice disabled and enabled, the weight, a second binding and the
// bindings' order, the interpolation.
TEST_F(LatticeRenderTest, EveryChangeDrawsWhatAFreshSceneDraws){
    std::vector<std::pair<std::string, PackingConfig>> steps;
    PackingConfig c;
    steps.push_back({"initial", c});
    c.latticePosition = Vector3(0.3f, -0.2f, 0.1f);
    c.latticeRotation = Vector3(0.0f, 0.0f, 15.0f);
    steps.push_back({"lattice moved", c});
    c.target = Vector3(-0.4f, 0.25f, 0.0f);
    steps.push_back({"target moved", c});
    c.resolution[0] = 5; c.resolution[1] = 3; c.resolution[2] = 4;
    steps.push_back({"resolution, same texel count", c});
    c.resolution[0] = 16; c.resolution[1] = 16; c.resolution[2] = 16;
    steps.push_back({"resolution grows", c});
    c.resolution[0] = 2; c.resolution[1] = 2; c.resolution[2] = 3;
    steps.push_back({"resolution shrinks", c});
    c.enabled = false;
    steps.push_back({"disabled", c});
    c.enabled = true;
    steps.push_back({"enabled", c});
    c.weight = 0.5f;
    steps.push_back({"weight", c});
    c.second = true;
    steps.push_back({"second binding", c});
    c.secondFirst = true;
    steps.push_back({"bindings reordered", c});
    c.interpolation = LatticeInterpolation::SHARP;
    steps.push_back({"interpolation", c});
    c.second = false;
    steps.push_back({"second binding removed", c});

    std::vector<Screen> fresh(steps.size());
    for (size_t i = 0; i < steps.size(); i++){
        PackingScene scene(steps[i].second);
        ASSERT_TRUE(scene.s.settle()) << steps[i].first;
        ASSERT_TRUE(fresh[i].read());
    }
    int differing = 0;
    for (size_t i = 1; i < steps.size(); i++)
        EXPECT_FALSE(sameImage(fresh[i - 1], fresh[i], differing)) << "'" << steps[i].first << "' changes the image";

    PackingScene live(steps[0].second);
    for (size_t i = 0; i < steps.size(); i++){
        SCOPED_TRACE(steps[i].first);
        live.apply(steps[i].second);
        ASSERT_TRUE(live.s.settle());
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_TRUE(sameImage(fresh[i], screen, differing)) << differing << " pixels differ";
    }
}

// Inherited bindings follow the hierarchy as it changes (LatticeRender::prepare's walk down
// the Transform array): own bindings first, then the ancestors' with includeChildren, nearest
// first; reparenting in and out of a bound branch; a deleted lattice; a deleted ancestor; more
// bindings than LATTICE_MAX_PER_MESH (the first ones kept, the warning flag cleared when it
// fits again).
TEST_F(LatticeRenderTest, InheritedBindingsFollowTheHierarchy){
    LatticeScene s;
    Scene& scene = s.scene();
    Lattice& translate = s.addLattice(0, 0, 0, 4);
    translate.setOutside(LatticeOutside::EXTEND);
    for (int i = 0; i < translate.getPointCount(); i++)
        translate.setOffsetAt(i, Vector3(0.25f, 0, 0)); // one world unit right
    Lattice& stretch = s.addLattice(0, 0, 0, 4);
    stretch.setOutside(LatticeOutside::EXTEND);
    for (int i = 0; i < stretch.getPointCount(); i++){
        const Vector3 rest = stretch.getRestPosition(i % 2, (i / 2) % 2, i / 4);
        stretch.setOffsetAt(i, Vector3(rest.x, 0, 0)); // double world x
    }
    GameObject& rig = s.fixture.add<GameObject>();
    Primitive& child = s.addBox(0.5f);
    rig.addChild(&child);
    s.bind(rig.getEntity(), translate.getEntity(), LatticeStage::AFTER_SKINNING, LatticeMaskChannel::NONE, true);
    auto expectBox = [&](float center, float halfWidth, float y = 0){
        ASSERT_TRUE(s.settle());
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_TRUE(screen.is(int(toScreenX(center)), toScreenRow(y), 255, 255, 255));
        EXPECT_TRUE(screen.is(int(toScreenX(center + halfWidth - 0.1f)), toScreenRow(y), 255, 255, 255));
        EXPECT_TRUE(screen.isBackground(int(toScreenX(center + halfWidth + 0.1f)), toScreenRow(y)));
    };
    expectBox(1, 0.25f);

    // Own bindings run before inherited ones: stretch then translate centers at x=1;
    // the reverse order would center at x=2.
    s.bind(child.getEntity(), stretch.getEntity());
    expectBox(1, 0.5f);
    scene.getComponent<LatticeModifierComponent>(rig.getEntity()).includeChildren = false;
    expectBox(0, 0.5f);
    scene.getComponent<LatticeModifierComponent>(rig.getEntity()).includeChildren = true;
    scene.addEntityChild(NULL_ENTITY, child.getEntity(), true);
    expectBox(0, 0.5f);
    scene.addEntityChild(rig.getEntity(), child.getEntity(), true);
    expectBox(1, 0.5f);

    // The nearest binding to the same lattice wins, including an explicit zero weight.
    ASSERT_TRUE(translate.bind(&child, 0));
    expectBox(0, 0.5f);
    ASSERT_TRUE(translate.unbind(&child));
    expectBox(1, 0.5f);
    // Nearest ancestors apply first. The outer stretch must double the inner
    // parent's translation; reversing the ancestor walk would leave center x=1.
    Lattice& outerStretch = s.addLattice(0, 0, 0, 4);
    outerStretch.setOutside(LatticeOutside::EXTEND);
    for (int i = 0; i < outerStretch.getPointCount(); i++){
        const Vector3 rest = outerStretch.getRestPosition(i % 2, (i / 2) % 2, i / 4);
        outerStretch.setOffsetAt(i, Vector3(rest.x, 0, 0));
    }
    GameObject& outer = s.fixture.add<GameObject>();
    outer.addChild(&rig);
    s.bind(outer.getEntity(), outerStretch.getEntity(), LatticeStage::AFTER_SKINNING, LatticeMaskChannel::NONE, true);
    expectBox(2, 1);
    scene.addEntityChild(NULL_ENTITY, rig.getEntity(), true);

    // A child's own modifier is inherited only when it opts into descendants.
    Primitive& grandchild = s.addBox(0.5f);
    grandchild.setPosition(0, 1, 0);
    child.addChild(&grandchild);
    expectBox(1, 0.25f, 1);
    scene.getComponent<LatticeModifierComponent>(child.getEntity()).includeChildren = true;
    expectBox(1, 0.5f, 1);
    scene.getComponent<LatticeModifierComponent>(child.getEntity()).includeChildren = false;
    grandchild.setVisible(false);

    scene.destroyEntity(translate.getEntity());
    expectBox(0, 0.5f);

    // Only the first four configured bindings affect a mesh. The fifth would move
    // the box upward, so the horizontal silhouette also detects a cap regression.
    LatticeModifierComponent& own = scene.getComponent<LatticeModifierComponent>(child.getEntity());
    own.bindings.clear();
    for (int n = 0; n < 5; n++){
        Lattice& shift = s.addLattice(0, 0, 0, 4);
        shift.setOutside(LatticeOutside::EXTEND);
        for (int i = 0; i < shift.getPointCount(); i++)
            shift.setOffsetAt(i, n < 4 ? Vector3(0.125f, 0, 0) : Vector3(0, 0.125f, 0));
        LatticeBinding binding;
        binding.lattice = shift.getEntity();
        own.bindings.push_back(binding);
    }
    expectBox(2, 0.25f);
}

// The bounds hold every vertex a lattice moves (spec 5.5): a before-skinning lattice's offset
// is carried by the bones and the mesh's model matrix, so a non-uniformly scaled skinned mesh
// with a turned bone moves its vertices further than the lattice's scale times the bone's
// (model * bone * model^-1 stretches a direction the scale does not, here x into y three
// times). The public world bounds must contain every posed, deformed bind-pose point.
TEST_F(LatticeRenderTest, BoundsHoldBeforeSkinningOffsetsOfAScaledMesh){
    LatticeScene s;
    Model& model = s.fixture.add<Model>();
    ASSERT_TRUE(model.loadModel("lattice/skinned_column.gltf"));
    model.setScale(Vector3(1.0f, 3.0f, 1.0f));
    model.getBone("Root").setRotation(0.0f, 0.0f, 90.0f);
    // around the bind pose as it stands at the model's transform: 0.5 x 6 x 0.5 from y 0
    Lattice& lattice = s.addLattice(0.0f, 3.0f, 0.0f, 7.0f);
    s.shearTop(lattice, 0.4f);
    s.bind(model.getEntity(), lattice.getEntity(), LatticeStage::BEFORE_SKINNING, LatticeMaskChannel::NONE, true);
    ASSERT_TRUE(s.settle());

    // Root turns both authored cuboids by 90 degrees; Tip's rest translation is
    // canceled by its inverse bind. The lattice's linear shear is 0 at world
    // y=-0.5 and 2.8 at y=6.5. At bind point (x,y,z), the model first scales y
    // by 3, so before skinning the shift along x is 0.4 * (3*y + 0.5).
    // Rotation then carries x into y, which the model scales by 3 again.
    // These eight extrema contain the authored column's complete linear field;
    // neither expected positions nor the oracle use the renderer's bone cache.
    const AABB bounds = model.getWorldAABB();
    for (float x : {-0.25f, 0.25f}){
        for (float y : {0.0f, 2.0f}){
            for (float z : {-0.25f, 0.25f}){
                const float shiftX = 0.4f * (3.0f * y + 0.5f);
                const Vector3 expected(-y, 3.0f * (x + shiftX), z);
                EXPECT_TRUE(bounds.contains(expected))
                        << "the public world bounds must contain the posed, deformed mesh";
            }
        }
    }
}

// A lattice whose transform is singular (a zero scale) deforms nothing and the mesh draws as
// without it; scaled back it deforms again. A target with a zero scale is skipped too.
TEST_F(LatticeRenderTest, SingularTransformsAreSkipped){
    LatticeScene s;
    Primitive& box = s.addBox(2.0f);
    ASSERT_TRUE(s.settle());
    Screen plain;
    ASSERT_TRUE(plain.read());

    Lattice& lattice = s.addLattice(0.0f, 0.0f, 0.0f, 2.5f);
    s.shearTop(lattice, 0.4f);
    s.bind(box.getEntity(), lattice.getEntity());
    lattice.setScale(Vector3(2.5f, 0.0f, 2.5f));
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = box.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.lattice.count, 0);
    Screen flat;
    ASSERT_TRUE(flat.read());
    int differing = 0;
    EXPECT_TRUE(sameImage(plain, flat, differing)) << differing << " pixels differ";

    lattice.setScale(2.5f);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(mesh.lattice.count, 1);
    Screen sheared;
    ASSERT_TRUE(sheared.read());
    EXPECT_FALSE(sameImage(plain, sheared, differing));

    box.setScale(Vector3(1.0f, 0.0f, 1.0f));
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(mesh.lattice.count, 0);
    box.setScale(1.0f);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(mesh.lattice.count, 1);
    Screen again;
    ASSERT_TRUE(again.read());
    EXPECT_TRUE(sameImage(sheared, again, differing)) << differing << " pixels differ";
}
