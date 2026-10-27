// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 2D normal maps drawn by the engine into the harness target (sprite normals phase 1,
// docs/superpowers/specs/2026-10-27-sprite-normals-phase1-engine-design.md section 5.2): an
// OpenGL normal map (green up the image) lights the side of a 2D object that faces a Light 2D
// on every 2D mesh (sprite quad and grid, MeshPolygon, Tilemap), under every mirroring
// (negative scale, mirrored texture rects, manual flipY, instances), rotated, with the built-in
// shader and library:sprite; objects without a normal map are lit the same at any light height;
// the Lit path's tangent frame follows mirroring too; a normal map that fails to load is flat.
//
// The scene: a 640x360 target, an orthographic world camera at one world unit per pixel looking
// down -z (y up), 2D ambient 0, one Light 2D (intensity 1, range 2000, falloff 0: constant
// attenuation, no shadows). The textures are made in code: `white` and `dome`, a hemisphere
// normal map in the OpenGL convention (and its DirectX twin, green inverted).

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "math/Quaternion.h"
#include "object/Camera.h"
#include "object/Lattice.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/MeshPolygon.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int screenWidth = 640;
    const int screenHeight = 360;
    const int size = 64;

    // RGBA8 texels of the test textures: static, a TextureData does not own its data
    std::vector<uint8_t>& whitePixels(){
        static std::vector<uint8_t> pixels(size_t(size) * size * 4, 255);
        return pixels;
    }

    // a hemisphere: n = (nx, ny, sqrt(1 - r2)) inside the disc, +Z outside; ny is up the image
    // (OpenGL) or, with directX, down it
    std::vector<uint8_t> makeDome(bool directX){
        std::vector<uint8_t> pixels(size_t(size) * size * 4);
        const float half = size * 0.5f;
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                const float nx = (x + 0.5f - half) / half;
                const float ny = (half - (y + 0.5f)) / half;
                const float r2 = nx * nx + ny * ny;
                float n[3] = {0.0f, 0.0f, 1.0f};
                if (r2 < 1.0f){
                    n[0] = nx;
                    n[1] = directX ? -ny : ny;
                    n[2] = std::sqrt(1.0f - r2);
                }
                uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
                for (int c = 0; c < 3; c++)
                    p[c] = uint8_t(std::lround((n[c] * 0.5f + 0.5f) * 255.0f));
                p[3] = 255;
            }
        }
        return pixels;
    }

    std::vector<uint8_t>& domePixels(){
        static std::vector<uint8_t> pixels = makeDome(false);
        return pixels;
    }

    std::vector<uint8_t>& domeDirectXPixels(){
        static std::vector<uint8_t> pixels = makeDome(true);
        return pixels;
    }

    TextureData textureData(std::vector<uint8_t>& pixels){
        return TextureData(size, size, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    void setColorTexture(Submesh& submesh){
        submesh.material.baseColorTexture.setData("light2dnormals|white", textureData(whitePixels()));
        submesh.needUpdateTexture = true;
    }

    void setNormalMap(Submesh& submesh, bool directX = false){
        if (directX)
            submesh.material.normalTexture.setData("light2dnormals|dome-dx", textureData(domeDirectXPixels()));
        else
            submesh.material.normalTexture.setData("light2dnormals|dome", textureData(domePixels()));
        submesh.needUpdateTexture = true;
    }

    // mean luminance of the four quarters of a box (x1/y1 exclusive)
    struct Quarters {
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    };

    float meanLuminance(const Screen& screen, int x0, int y0, int x1, int y1){
        double sum = 0.0;
        int count = 0;
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const uint8_t* p = screen.at(x, y);
                sum += (p[0] + p[1] + p[2]) / 3.0;
                count++;
            }
        }
        return count ? float(sum / count) : 0.0f;
    }

    Quarters quarters(const Screen& screen, const PixelRect& box){
        const int qw = (box.x1 - box.x0) / 4;
        const int qh = (box.y1 - box.y0) / 4;
        Quarters q;
        q.left = meanLuminance(screen, box.x0, box.y0, box.x0 + qw, box.y1);
        q.right = meanLuminance(screen, box.x1 - qw, box.y0, box.x1, box.y1);
        q.top = meanLuminance(screen, box.x0, box.y0, box.x1, box.y0 + qh);
        q.bottom = meanLuminance(screen, box.x0, box.y1 - qh, box.x1, box.y1);
        return q;
    }

    std::string describe(const Quarters& q){
        return "left " + std::to_string(q.left) + ", right " + std::to_string(q.right) +
               ", top " + std::to_string(q.top) + ", bottom " + std::to_string(q.bottom);
    }

    ::testing::AssertionResult brighter(float lit, float dark, const char* what, const Quarters& q){
        if (lit > 1.3f * dark && lit > 40.0f)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << what << " is not the lit side (" << describe(q) << ")";
    }

    // the 64x64 box of an object centred at world (x, y) (one unit per pixel, y up)
    PixelRect boxAt(float x, float y){
        const int cx = screenWidth / 2 + int(std::lround(x));
        const int cy = screenHeight / 2 - int(std::lround(y));
        return {cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2};
    }

    enum class LightSide { LEFT, TOP };

    // A world (y up) or UI (y down) scene drawn into the target, lit by one Light 2D
    struct NormalScene {
        SceneFixture fixture;
        Camera camera;
        Light2D* light = nullptr;
        bool ui;

        explicit NormalScene(bool ui = false): camera(&fixture.getScene()), ui(ui){
            Scene& s = scene();
            s.setAmbientLight2D(0.0f);
            if (ui){
                camera.setType(CameraType::CAMERA_UI);
            }else{
                camera.setOrtho(-screenWidth * 0.5f, screenWidth * 0.5f, -screenHeight * 0.5f, screenHeight * 0.5f, -100.0f, 100.0f);
                camera.setPosition(0.0f, 0.0f, 10.0f);
                camera.setTarget(0.0f, 0.0f, 0.0f);
            }
            camera.activate();
        }

        Scene& scene(){ return fixture.getScene(); }

        // the object's centre in world units: the screen centre
        Vector3 centre() const{
            return ui ? Vector3(screenWidth * 0.5f, screenHeight * 0.5f, 0.0f) : Vector3(0.0f, 0.0f, 0.0f);
        }

        // 300 units to the object's left or above it (up the screen), at a height
        Light2D& addLight(LightSide side, float height = 64.0f){
            light = &fixture.add<Light2D>();
            light->setIntensity(1.0f);
            light->setRange(2000.0f);
            light->setFalloff(0.0f);
            light->setHeight(height);
            light->setShadows(false);
            placeLight(side);
            return *light;
        }

        void placeLight(LightSide side){
            const Vector3 c = centre();
            if (side == LightSide::LEFT)
                light->setPosition(c.x - 300.0f, c.y, 0.0f);
            else
                light->setPosition(c.x, ui ? c.y - 300.0f : c.y + 300.0f, 0.0f);
        }

        Sprite& addSprite(bool normalMap = true, bool directX = false){
            Sprite& sprite = fixture.add<Sprite>();
            sprite.setSize(size, size);
            sprite.setPivotPreset(PivotPreset::CENTER);
            const Vector3 c = centre();
            sprite.setPosition(c.x, c.y, 0.0f);
            MeshComponent& mesh = sprite.getComponent<MeshComponent>();
            setColorTexture(mesh.submeshes[0]);
            if (normalMap)
                setNormalMap(mesh.submeshes[0], directX);
            return sprite;
        }

        // frames until every mesh is loaded with the variant it asks for
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
                    if (!mesh.loaded || mesh.needReload || mesh.needVariantReload ||
                            mesh.lattice.variantActive != (mesh.lattice.count > 0))
                        return false;
                    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                        if (mesh.submeshes[s].needUpdateTexture && mesh.submeshes[s].vertexCount > 0)
                            return false;
                    }
                }
                return true;
            }, 600);
        }

        // the quarters of the 64x64 box at the screen centre
        Quarters measure(PixelRect box = boxAt(0.0f, 0.0f)){
            Screen screen;
            EXPECT_TRUE(screen.read());
            return quarters(screen, box);
        }

        // lit from the left, then from the top
        void expectLitFromLightSide(const std::string& what, PixelRect box = boxAt(0.0f, 0.0f)){
            placeLight(LightSide::LEFT);
            ASSERT_TRUE(settle()) << what;
            const Quarters left = measure(box);
            EXPECT_TRUE(brighter(left.left, left.right, (what + ": light on the left, the left quarter").c_str(), left));
            placeLight(LightSide::TOP);
            ASSERT_TRUE(settle()) << what;
            const Quarters top = measure(box);
            EXPECT_TRUE(brighter(top.top, top.bottom, (what + ": light above, the top quarter").c_str(), top));
        }
    };

    ::testing::AssertionResult sameImage(const Screen& a, const Screen& b, int tolerance){
        if (a.width != b.width || a.height != b.height)
            return ::testing::AssertionFailure() << "the sizes differ";
        int worst = 0;
        size_t differing = 0;
        for (size_t p = 0; p + 3 < a.rgba.size(); p += 4){
            int d = 0;
            for (int c = 0; c < 3; c++)
                d = std::max(d, std::abs(int(a.rgba[p + c]) - int(b.rgba[p + c])));
            worst = std::max(worst, d);
            if (d > tolerance)
                differing++;
        }
        if (differing == 0)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << differing << " pixels differ by more than " << tolerance << " (at most " << worst << ")";
    }

    class Light2DNormalMapTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
        }
    };

}

// R1, R2: the built-in mesh shader's unlit 2D path
TEST_F(Light2DNormalMapTest, SpriteLitFromLeftAndTop){
    NormalScene s;
    s.addSprite();
    s.addLight(LightSide::LEFT);
    s.expectLitFromLightSide("sprite");
}

// R3: the sprite grid a lattice asks for (at rest), through mesh.vert's lattice path
TEST_F(Light2DNormalMapTest, SpriteGrid){
    NormalScene s;
    Sprite& sprite = s.addSprite();
    s.addLight(LightSide::LEFT);
    Lattice& lattice = s.fixture.add<Lattice>();
    lattice.setPosition(0.0f, 0.0f, 0.0f);
    lattice.setScale(200.0f);
    ASSERT_TRUE(lattice.bind(sprite.getEntity()));
    s.scene().getComponent<LatticeModifierComponent>(sprite.getEntity()).subdivisions = 4;
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(sprite.getComponent<MeshComponent>().buffer.getCount(), 25u) << "a 4 x 4 grid";
    s.expectLitFromLightSide("sprite grid");
}

// R4: a MeshPolygon square
TEST_F(Light2DNormalMapTest, Polygon){
    NormalScene s;
    MeshPolygon& polygon = s.fixture.add<MeshPolygon>();
    const float h = size * 0.5f;
    polygon.addVertex(Vector3(-h, -h, 0.0f));
    polygon.addVertex(Vector3(h, -h, 0.0f));
    polygon.addVertex(Vector3(-h, h, 0.0f));
    polygon.addVertex(Vector3(h, h, 0.0f));
    MeshComponent& mesh = polygon.getComponent<MeshComponent>();
    setColorTexture(mesh.submeshes[0]);
    setNormalMap(mesh.submeshes[0]);
    s.addLight(LightSide::LEFT);
    s.expectLitFromLightSide("polygon");
}

// R5: one 64x64 tile, the tileset's normal map on its submesh
TEST_F(Light2DNormalMapTest, Tilemap){
    NormalScene s;
    Tilemap& tilemap = s.fixture.add<Tilemap>();
    tilemap.addRect(0, "tile", Rect(0.0f, 0.0f, 1.0f, 1.0f));
    tilemap.addTile(0, Vector2(0.0f, 0.0f), float(size), float(size));
    tilemap.setPosition(-size * 0.5f, -size * 0.5f, 0.0f);
    MeshComponent& mesh = tilemap.getComponent<MeshComponent>();
    // the tileset texture made in code: submesh 0, as addRect with a texture file makes it
    setColorTexture(mesh.submeshes[0]);
    setNormalMap(mesh.submeshes[0]);
    mesh.numSubmeshes = 1;
    s.addLight(LightSide::LEFT);
    s.expectLitFromLightSide("tilemap");
}

// R6-R8: a negative Transform scale on either axis or both (the dome is symmetric: the lit
// side follows the light)
TEST_F(Light2DNormalMapTest, MirroredScale){
    const Vector3 scales[] = {Vector3(-1.0f, 1.0f, 1.0f), Vector3(1.0f, -1.0f, 1.0f), Vector3(-1.0f, -1.0f, 1.0f)};
    for (const Vector3& scale : scales){
        NormalScene s;
        Sprite& sprite = s.addSprite();
        sprite.setScale(scale);
        s.addLight(LightSide::LEFT);
        s.expectLitFromLightSide("scale (" + std::to_string(int(scale.x)) + ", " + std::to_string(int(scale.y)) + ")");
    }
}

// R9: texture rects mirrored horizontally and vertically
TEST_F(Light2DNormalMapTest, MirroredFrameRect){
    const Rect rects[] = {Rect(1.0f, 0.0f, -1.0f, 1.0f), Rect(0.0f, 1.0f, 1.0f, -1.0f)};
    for (const Rect& rect : rects){
        NormalScene s;
        Sprite& sprite = s.addSprite();
        sprite.setTextureRect(rect);
        s.addLight(LightSide::LEFT);
        s.expectLitFromLightSide(rect.getWidth() < 0 ? "texture rect mirrored in x" : "texture rect mirrored in y");
    }
}

// R10: flipY off on a world camera (the image upside down)
TEST_F(Light2DNormalMapTest, ManualFlipY){
    NormalScene s;
    Sprite& sprite = s.addSprite();
    sprite.setFlipY(false);
    s.addLight(LightSide::LEFT);
    s.expectLitFromLightSide("flipY off");
}

// R11: the frame turns with the sprite: rotated 90 degrees, the image's up points to world -x
TEST_F(Light2DNormalMapTest, Rotated90){
    NormalScene s;
    Sprite& sprite = s.addSprite();
    sprite.setRotation(Quaternion(90.0f, Vector3(0.0f, 0.0f, 1.0f)));
    s.addLight(LightSide::LEFT);
    s.expectLitFromLightSide("rotated 90 degrees");
}

// R12: instances, one as is, one with a negative scale, one with a mirrored instance rect
TEST_F(Light2DNormalMapTest, Instanced){
    NormalScene s;
    Sprite& sprite = s.addSprite();
    sprite.setPosition(0.0f, 0.0f, 0.0f);
    sprite.createInstancedMesh();
    const Quaternion none;
    sprite.addInstance(Vector3(-128.0f, 0.0f, 0.0f), none, Vector3(1.0f, 1.0f, 1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), Rect(0.0f, 0.0f, 1.0f, 1.0f));
    sprite.addInstance(Vector3(0.0f, 0.0f, 0.0f), none, Vector3(-1.0f, 1.0f, 1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), Rect(0.0f, 0.0f, 1.0f, 1.0f));
    sprite.addInstance(Vector3(128.0f, 0.0f, 0.0f), none, Vector3(1.0f, 1.0f, 1.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), Rect(1.0f, 0.0f, -1.0f, 1.0f));
    s.addLight(LightSide::LEFT);
    // each instance lit from its own left and top: the light 300 units from the middle one is
    // on the left of all three, and above all three
    const char* names[] = {"instance as is", "instance with scale (-1, 1)", "instance with a mirrored rect"};
    const float xs[] = {-128.0f, 0.0f, 128.0f};
    for (int i = 0; i < 3; i++)
        s.expectLitFromLightSide(names[i], boxAt(xs[i], 0.0f));
}

// R13: library:sprite reads the material's Normal Texture, also with OUTLINE (the clamped UV)
TEST_F(Light2DNormalMapTest, LibrarySprite){
    for (bool outline : {false, true}){
        NormalScene s;
        Sprite& sprite = s.addSprite();
        Material& material = sprite.getComponent<MeshComponent>().submeshes[0].material;
        material.shader = "library:sprite";
        if (outline)
            material.shaderFeatures = {"OUTLINE"};
        s.addLight(LightSide::LEFT);
        s.expectLitFromLightSide(outline ? "library:sprite with OUTLINE" : "library:sprite");
        EXPECT_NE(sprite.getComponent<MeshComponent>().submeshes[0].shaderProperties & (1u << 7), 0u)
            << "the variant samples the normal map ('Nmp')";
    }
}

// R13 (all features): every library:sprite feature on compiles with the normal map and keeps
// the lit side (glow adds the same emission to both sides)
TEST_F(Light2DNormalMapTest, LibrarySpriteAllFeatures){
    NormalScene s;
    Sprite& sprite = s.addSprite();
    Material& material = sprite.getComponent<MeshComponent>().submeshes[0].material;
    material.shader = "library:sprite";
    material.shaderFeatures = {"OUTLINE", "HUE_SHIFT", "FLASH", "GRAYSCALE", "COLOR_REPLACE", "OVERLAY", "GLOW"};
    s.addLight(LightSide::LEFT);
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    EXPECT_NE(mesh.submeshes[0].shaderProperties & (1u << 7), 0u) << "the variant samples the normal map ('Nmp')";
    const Quarters q = s.measure();
    EXPECT_GT(q.left, q.right + 10.0f) << "every feature on: " << describe(q);
}

// R14: a sprite without a normal map is lit the same whatever the light's height
TEST_F(Light2DNormalMapTest, FlatIgnoresHeight){
    NormalScene s;
    s.addSprite(false);
    Light2D& light = s.addLight(LightSide::LEFT, 64.0f);
    ASSERT_TRUE(s.settle());
    Screen high;
    ASSERT_TRUE(high.read());
    light.setHeight(0.0f);
    ASSERT_TRUE(s.settle());
    Screen flat;
    ASSERT_TRUE(flat.read());
    EXPECT_TRUE(sameImage(high, flat, 1)) << "height 64 vs 0, no normal map";
    EXPECT_GT(meanLuminance(flat, 288, 148, 352, 212), 200.0f) << "the full light";
}

// R15: the same with library:sprite
TEST_F(Light2DNormalMapTest, LibraryFlatIgnoresHeight){
    NormalScene s;
    Sprite& sprite = s.addSprite(false);
    sprite.getComponent<MeshComponent>().submeshes[0].material.shader = "library:sprite";
    Light2D& light = s.addLight(LightSide::LEFT, 64.0f);
    ASSERT_TRUE(s.settle());
    Screen high;
    ASSERT_TRUE(high.read());
    light.setHeight(0.0f);
    ASSERT_TRUE(s.settle());
    Screen flat;
    ASSERT_TRUE(flat.read());
    EXPECT_TRUE(sameImage(high, flat, 1)) << "height 64 vs 0, library:sprite, no normal map";
    EXPECT_GT(meanLuminance(flat, 288, 148, 352, 212), 200.0f) << "the full light";
}

// R16: a UI camera (y down): the image's up is the screen's up there too
TEST_F(Light2DNormalMapTest, UICamera){
    NormalScene s(true);
    Sprite& sprite = s.addSprite();
    s.addLight(LightSide::LEFT);
    ASSERT_TRUE(s.settle());
    EXPECT_FALSE(sprite.getComponent<SpriteComponent>().flipY) << "a UI camera leaves flipY off";
    s.expectLitFromLightSide("UI camera");
}

// R17: the Lit path (a directional light from the left, then from above, no Light 2D): the
// tangent frame follows a negative scale. Sprites are double-sided and a runtime negative scale
// does not swap the winding (section 3.8), so the mirrored one is wound clockwise to keep its
// front face front.
TEST_F(Light2DNormalMapTest, LitMirrored3D){
    for (float scaleX : {1.0f, -1.0f}){
        NormalScene s;
        Sprite& sprite = s.addSprite();
        sprite.setScale(Vector3(scaleX, 1.0f, 1.0f));
        if (scaleX < 0.0f)
            sprite.setWindingOrder(WindingOrder::CW);
        sprite.getComponent<MeshComponent>().submeshes[0].material.lighting = MaterialLighting::PIXEL;
        s.scene().setGlobalIllumination(0.0f);
        Light& sun = s.fixture.add<Light>();
        sun.setType(LightType::DIRECTIONAL);
        sun.setIntensity(2.0f);
        const std::string what = scaleX < 0.0f ? "lit, scale (-1, 1)" : "lit";

        sun.setDirection(1.0f, 0.0f, -1.0f);
        ASSERT_TRUE(s.settle());
        const Quarters left = s.measure();
        EXPECT_TRUE(brighter(left.left, left.right, (what + ": light from the left, the left quarter").c_str(), left));

        sun.setDirection(0.0f, -1.0f, -1.0f);
        ASSERT_TRUE(s.settle());
        const Quarters top = s.measure();
        EXPECT_TRUE(brighter(top.top, top.bottom, (what + ": light from above, the top quarter").c_str(), top));
    }
}

// R18: a normal map that fails to load shades flat, as no normal map does (Lit path)
TEST_F(Light2DNormalMapTest, MissingNormalFallsBackFlat){
    Screen missing, none;
    for (bool missingMap : {true, false}){
        NormalScene s;
        Sprite& sprite = s.addSprite(false);
        Submesh& submesh = sprite.getComponent<MeshComponent>().submeshes[0];
        submesh.material.lighting = MaterialLighting::PIXEL;
        if (missingMap){
            submesh.material.normalTexture.setPath("light2dnormals/does-not-exist.png");
            submesh.needUpdateTexture = true;
        }
        s.scene().setGlobalIllumination(0.0f);
        Light& sun = s.fixture.add<Light>();
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(1.0f, 0.0f, -1.0f);
        sun.setIntensity(2.0f);
        ASSERT_TRUE(s.settle());
        ASSERT_TRUE((missingMap ? missing : none).read());
        if (missingMap)
            EXPECT_NE(submesh.shaderProperties & (1u << 7), 0u) << "the variant samples a normal map ('Nmp')";
    }
    EXPECT_TRUE(sameImage(missing, none, 2)) << "a missing normal map vs none";
}

// R19: a DirectX map (green inverted) still lights from the left, but upside down: the top
// quarter is the dark one under a light above (the test tells the conventions apart)
TEST_F(Light2DNormalMapTest, DirectXMapReadsUpsideDown){
    NormalScene s;
    s.addSprite(true, true);
    s.addLight(LightSide::LEFT);
    ASSERT_TRUE(s.settle());
    const Quarters left = s.measure();
    EXPECT_TRUE(brighter(left.left, left.right, "DirectX map, light on the left: the left quarter", left));
    s.placeLight(LightSide::TOP);
    ASSERT_TRUE(s.settle());
    const Quarters top = s.measure();
    EXPECT_TRUE(brighter(top.bottom, top.top, "DirectX map, light above: the bottom quarter", top));
}
