// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Render-to-texture documents and world-space input (phase-1 spec §3.12, §5.1):
// raycastMeshUV on primitives, the texture-mode document lifecycle (own context, target,
// mode switches, removal) and pointer events reaching a document through a mesh.

#include "TestGoldenCommon.h"
#include "component/CameraComponent.h"

#include "HtmlUIClock.h"
#include "HtmlUIContext.h"
#include "RmlRenderer.h"
#include "Input.h"
#include "UIWorldRaycast.h"
#include "component/MeshComponent.h"
#include "component/UIDocumentComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"
#include "object/ui/UIDataTransfer.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIItemsView.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // counts the events of one type on an element
    class CountingListener : public Rml::EventListener {
    public:
        int count = 0;
        void ProcessEvent(Rml::Event&) override{ count++; }
    };

    // the canvas point a world point projects to through the scene camera (inverse of
    // Camera::screenToRay)
    Vector2 project(Scene& scene, const Vector3& world){
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const float x = clip.x / clip.w, y = clip.y / clip.w;
        return Vector2((x + 1.0f) * 0.5f * Engine::getCanvasWidth(), (1.0f - y) * 0.5f * Engine::getCanvasHeight());
    }

    class TextureModeTest : public FullPathGoldenTest {
    protected:
        std::unique_ptr<SceneFixture> fixture;
        Camera* camera = nullptr;

        void SetUp() override{
            FullPathGoldenTest::SetUp();
            fixture.reset(new SceneFixture());
            // a 3D camera at z = 5 looking at the origin
            camera = &fixture->add<Camera>();
            camera->setPosition(0, 0, 5);
            camera->setTarget(0, 0, 0);
            camera->activate();
        }

        void TearDown() override{
            fixture.reset();
            FullPathGoldenTest::TearDown();
        }

        Scene& scene(){ return fixture->getScene(); }
        UIDocumentSystem& system(){ return *scene().getSystem<UIDocumentSystem>(); }

        // a 2x2 wall (XY plane, facing +Z) at a position
        Primitive& wall(float x, float y, float z){
            Primitive& primitive = fixture->add<Primitive>();
            primitive.createWall(2, 2);
            primitive.setPosition(x, y, z);
            return primitive;
        }

        // a TEXTURE-mode document hit-tested on a surface
        UIDocument textureDocument(const std::string& file, Entity surface, int width = 256, int height = 256, float ratio = 0.0f){
            UIDocument document = fixture->addDocument(file);
            UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = width;
            component.textureHeight = height;
            component.pixelRatio = ratio;
            component.inputSurface = surface;
            return document;
        }

        Rml::Element* element(Entity entity, const std::string& id){
            Rml::ElementDocument* document = system().getDocument(entity);
            return document ? document->GetElementById(id) : nullptr;
        }

        bool pointer(PointerPhase phase, const Vector3& world, int id = 0, bool touch = false){
            const Vector2 point = project(scene(), world);
            return system().dispatchWorldPointer(id, touch ? -1 : D_MOUSE_BUTTON_LEFT, point.x, point.y, 0, phase, touch);
        }
    };

}

TEST_F(TextureModeTest, RaycastPlaneCenterAndCorners){
    // the horizontal plane primitive, seen from above
    Primitive& plane = fixture->add<Primitive>();
    plane.createPlane(2, 2);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    UIWorldHit hit = raycastMeshUV(&scene(), plane.getEntity(), Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.uv.x, 0.5f, 1e-5f);
    EXPECT_NEAR(hit.uv.y, 0.5f, 1e-5f);
    EXPECT_NEAR(hit.distance, 5.0f, 1e-4f);

    // the corners (createPlane: (-1, -1) is uv (0, 0), +z is v = 1, +x is u = 1)
    struct Corner { float x, z, u, v; };
    for (Corner c : {Corner{-1, -1, 0, 0}, Corner{-1, 1, 0, 1}, Corner{1, 1, 1, 1}, Corner{1, -1, 1, 0}}){
        hit = raycastMeshUV(&scene(), plane.getEntity(), Ray(Vector3(c.x, 5, c.z), Vector3(0, -10, 0)));
        ASSERT_TRUE(hit.hit) << c.x << ", " << c.z;
        EXPECT_NEAR(hit.uv.x, c.u, 1e-4f);
        EXPECT_NEAR(hit.uv.y, c.v, 1e-4f);
    }

    // a wall's top-left is uv (0, 0): the texture shows upright
    Primitive& upright = wall(5, 0, 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    hit = raycastMeshUV(&scene(), upright.getEntity(), Ray(Vector3(4.5f, 0.5f, 5), Vector3(0, 0, -10)));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.uv.x, 0.25f, 1e-4f);
    EXPECT_NEAR(hit.uv.y, 0.25f, 1e-4f);
}

TEST_F(TextureModeTest, RaycastRotatedScaled){
    Primitive& target = wall(1, 2, -3);
    target.setRotation(30, 45, 10);
    target.setScale(Vector3(3, 0.5f, 1));
    ASSERT_TRUE(fixture->waitUntilLoaded());

    // a local point with a known uv, through the model matrix, approached along the normal
    const Matrix4 model = target.getModelMatrix();
    const Vector3 local(0.5f, -0.5f, 0.0f);       // wall uv (0.75, 0.75)
    const Vector3 world = model * local;
    const Vector3 normal = ((model * Vector3(0, 0, 1)) - (model * Vector3(0, 0, 0))).normalize();

    UIWorldHit hit = raycastMeshUV(&scene(), target.getEntity(), Ray(world + normal * 4.0f, normal * -8.0f));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.uv.x, 0.75f, 1e-4f);
    EXPECT_NEAR(hit.uv.y, 0.75f, 1e-4f);
    EXPECT_NEAR(hit.distance, 4.0f, 1e-3f);

    // a ray that stops short of the surface
    EXPECT_FALSE(raycastMeshUV(&scene(), target.getEntity(), Ray(world + normal * 4.0f, normal * -3.0f)).hit);
}

TEST_F(TextureModeTest, RaycastMissBackFaceDisabled){
    Primitive& target = wall(0, 0, 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    // beside the wall
    EXPECT_FALSE(raycastMeshUV(&scene(), target.getEntity(), Ray(Vector3(1.5f, 0, 5), Vector3(0, 0, -10))).hit);
    // pointing away
    EXPECT_FALSE(raycastMeshUV(&scene(), target.getEntity(), Ray(Vector3(0, 0, 5), Vector3(0, 0, 10))).hit);

    // from behind: the test is two-sided, the uv the same texel
    UIWorldHit back = raycastMeshUV(&scene(), target.getEntity(), Ray(Vector3(-0.5f, 0.5f, -5), Vector3(0, 0, 10)));
    ASSERT_TRUE(back.hit);
    EXPECT_NEAR(back.uv.x, 0.25f, 1e-4f);
    EXPECT_NEAR(back.uv.y, 0.25f, 1e-4f);

    // no mesh, a disabled one, a hidden one
    EXPECT_FALSE(raycastMeshUV(&scene(), camera->getEntity(), Ray(Vector3(0, 0, 5), Vector3(0, 0, -10))).hit);
    EXPECT_FALSE(raycastMeshUV(&scene(), NULL_ENTITY, Ray(Vector3(0, 0, 5), Vector3(0, 0, -10))).hit);
    target.getComponent<MeshComponent>().enabled = false;
    EXPECT_FALSE(raycastMeshUV(&scene(), target.getEntity(), Ray(Vector3(0, 0, 5), Vector3(0, 0, -10))).hit);
    target.getComponent<MeshComponent>().enabled = true;
    target.setVisible(false);
    EXPECT_FALSE(raycastMeshUV(&scene(), target.getEntity(), Ray(Vector3(0, 0, 5), Vector3(0, 0, -10))).hit);
}

TEST_F(TextureModeTest, RaycastNearestOfTwo){
    Primitive& far = wall(0, 0, 0);
    Primitive& near = wall(0, 0, 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    const Ray ray(Vector3(0.2f, 0.2f, 5), Vector3(0, 0, -10));
    UIWorldHit farHit = raycastMeshUV(&scene(), far.getEntity(), ray);
    UIWorldHit nearHit = raycastMeshUV(&scene(), near.getEntity(), ray);
    ASSERT_TRUE(farHit.hit);
    ASSERT_TRUE(nearHit.hit);
    EXPECT_NEAR(farHit.distance, 5.0f, 1e-4f);
    EXPECT_NEAR(nearHit.distance, 4.0f, 1e-4f);

    // a box: the nearest of its faces (the front one)
    Primitive& box = fixture->add<Primitive>();
    box.createBox(2, 2, 2);
    box.setPosition(10, 0, 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    UIWorldHit boxHit = raycastMeshUV(&scene(), box.getEntity(), Ray(Vector3(10, 0, 5), Vector3(0, 0, -10)));
    ASSERT_TRUE(boxHit.hit);
    EXPECT_NEAR(boxHit.distance, 4.0f, 1e-4f);
}

TEST_F(TextureModeTest, OwnContextAndTarget){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_input.rml", surface.getEntity(), 512, 256, 2.0f);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    const Entity entity = document.getEntity();
    HtmlUIContext* context = system().getTextureContext(entity);
    ASSERT_NE(context, nullptr);
    EXPECT_NE(context, system().getContext());
    // layout in CSS px: the texture size over the pixel ratio
    EXPECT_EQ(context->getDimensions(), Rml::Vector2i(256, 128));
    EXPECT_FLOAT_EQ(context->getRmlContext()->GetDensityIndependentPixelRatio(), 2.0f);
    EXPECT_EQ(std::string(context->getRmlContext()->GetName()).rfind("tex:", 0), 0u);

    // the element API reaches it like a screen document
    EXPECT_TRUE(document.isLoaded());
    ASSERT_NE(element(entity, "tl"), nullptr);
    EXPECT_EQ(element(entity, "tl")->GetContext(), context->getRmlContext());
    EXPECT_EQ(system().getContext()->getRmlContext()->GetNumDocuments(), 0);

    Framebuffer* target = system().getTargetTexture(entity);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(document.getTargetTexture(), target);
    // a screen document has none
    UIDocument screen = fixture->addDocument("p1/texture_input.rml");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_EQ(system().getTargetTexture(screen.getEntity()), nullptr);
    EXPECT_EQ(system().getTextureContext(screen.getEntity()), nullptr);

    // size and ratio follow the component without a reload
    UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
    component.textureWidth = 300;
    component.pixelRatio = 1.0f;
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(context->getDimensions(), Rml::Vector2i(300, 256));

    // SCREEN again (a reload): into the screen context, the texture context goes
    component.renderMode = UIDocumentRenderMode::SCREEN;
    component.needReload = true;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_EQ(system().getTextureContext(entity), nullptr);
    EXPECT_EQ(system().getTargetTexture(entity), nullptr);
    ASSERT_NE(element(entity, "tl"), nullptr);
    EXPECT_EQ(element(entity, "tl")->GetContext(), system().getContext()->getRmlContext());

    // and TEXTURE again
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.needReload = true;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    ASSERT_NE(system().getTextureContext(entity), nullptr);
    EXPECT_EQ(element(entity, "tl")->GetContext(), system().getTextureContext(entity)->getRmlContext());
    EXPECT_EQ(system().getContext()->getRmlContext()->GetNumDocuments(), 1);   // the screen document only

    // removing the component drops the context
    document.removeComponent<UIDocumentComponent>();
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(system().getTextureContext(entity), nullptr);
    EXPECT_FALSE(system().isDocumentLoaded(entity));
}

TEST_F(TextureModeTest, WorldPointerReachesDocument){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_input.rml", surface.getEntity());
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();

    Rml::Element* tl = element(entity, "tl");
    Rml::Element* br = element(entity, "br");
    ASSERT_NE(tl, nullptr);
    ASSERT_NE(br, nullptr);
    CountingListener tlClicks, brClicks;
    tl->AddEventListener(Rml::EventId::Click, &tlClicks);
    br->AddEventListener(Rml::EventId::Click, &brClicks);

    // the top-left quadrant of the wall is #tl
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_TRUE(tl->IsPseudoClassSet("hover"));
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(system().getTextureFocus(), entity);
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(tlClicks.count, 1);
    EXPECT_EQ(brClicks.count, 0);

    // bottom-right
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(0.5f, -0.5f, 0)));
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(0.5f, -0.5f, 0)));
    EXPECT_EQ(brClicks.count, 1);
    EXPECT_FALSE(tl->IsPseudoClassSet("hover"));

    // the transparent body (top-right) does not consume: the game gets it
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(0.5f, 0.5f, 0)));
    EXPECT_FALSE(pointer(PointerPhase::DOWN, Vector3(0.5f, 0.5f, 0)));
    EXPECT_EQ(system().getTextureFocus(), NULL_ENTITY);
    EXPECT_FALSE(pointer(PointerPhase::UP, Vector3(0.5f, 0.5f, 0)));

    // off the wall: the hover goes (mouse leave)
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(0.5f, -0.5f, 0)));
    EXPECT_TRUE(br->IsPseudoClassSet("hover"));
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(3.0f, 0.0f, 0)));
    EXPECT_FALSE(br->IsPseudoClassSet("hover"));

    // not interactive: nothing reaches it
    document.getComponent<UIDocumentComponent>().interactive = false;
    EXPECT_FALSE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_FALSE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(tlClicks.count, 1);
    document.getComponent<UIDocumentComponent>().interactive = true;

    // hidden: neither
    document.hide();
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_FALSE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0)));
    document.show();
    GpuEnvironment::drawFrames(1);

    // own entity as the surface when inputSurface is not set: the document entity has no
    // mesh, so nothing is hit
    document.getComponent<UIDocumentComponent>().inputSurface = NULL_ENTITY;
    EXPECT_FALSE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_FALSE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0)));

    tl->RemoveEventListener(Rml::EventId::Click, &tlClicks);
    br->RemoveEventListener(Rml::EventId::Click, &brClicks);
}

TEST_F(TextureModeTest, WorldPointerCapture){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_input.rml", surface.getEntity());
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    Rml::Element* tl = element(entity, "tl");
    ASSERT_NE(tl, nullptr);
    CountingListener clicks;
    tl->AddEventListener(Rml::EventId::Click, &clicks);

    // down on #tl, dragged off the wall: still captured (consumed), uv clamped to the edge
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_TRUE(tl->IsPseudoClassSet("active"));
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0)));
    EXPECT_TRUE(tl->IsPseudoClassSet("hover"));   // clamped to the top-left corner, still on #tl
    // a touch pointer is not captured by the mouse
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0), 1, true));
    EXPECT_TRUE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    // released off-surface, over the clamped corner: a click on #tl, then nothing captured
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-4.0f, 4.0f, 0)));
    EXPECT_EQ(clicks.count, 1);
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0)));

    EXPECT_FALSE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));

    // touches: each pointer captured on its own; a cancel releases without a click
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0), 2, true));
    EXPECT_TRUE(system().hasPointerCapture(2, -1, true));
    EXPECT_FALSE(system().hasPointerCapture(3, -1, true));
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0), 2, true));
    EXPECT_TRUE(system().dispatchWorldPointer(2, -1, 0, 0, 0, PointerPhase::CANCEL, true));
    EXPECT_FALSE(system().hasPointerCapture(2, -1, true));
    EXPECT_EQ(clicks.count, 1);
    // a tap: start and end on #tl clicks it
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0), 3, true));
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0), 3, true));
    EXPECT_EQ(clicks.count, 2);
    // a touch on the transparent body is not taken, and not tracked
    EXPECT_FALSE(pointer(PointerPhase::DOWN, Vector3(0.5f, 0.5f, 0), 4, true));
    EXPECT_FALSE(system().getTextureContext(document.getEntity())->hasTouch(4));

    tl->RemoveEventListener(Rml::EventId::Click, &clicks);
}

TEST_F(TextureModeTest, WorldPointerNearestFirst){
    // two documents on overlapping walls: the nearer gets the pointer where it has content,
    // its transparent body passes the pointer to the one behind
    Primitive& farWall = wall(0, 0, 0);
    Primitive& nearWall = wall(0, 0, 1);
    UIDocument farDocument = textureDocument("p1/texture_input.rml", farWall.getEntity());
    UIDocument nearDocument = textureDocument("p1/texture_input.rml", nearWall.getEntity());
    ASSERT_TRUE(fixture->waitUntilLoaded());

    Rml::Element* farTl = element(farDocument.getEntity(), "tl");
    Rml::Element* nearTl = element(nearDocument.getEntity(), "tl");
    Rml::Element* farBr = element(farDocument.getEntity(), "br");
    ASSERT_TRUE(farTl && nearTl && farBr);

    // the near wall's top-left quadrant covers the far one's
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 1)));
    EXPECT_TRUE(nearTl->IsPseudoClassSet("active"));
    EXPECT_FALSE(farTl->IsPseudoClassSet("active"));
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 1)));
    EXPECT_EQ(system().getTextureFocus(), nearDocument.getEntity());

    // move the near wall so its transparent top-right quadrant lies over the far #tl: the
    // near document does not consume there and passes the pointer on
    nearWall.setPosition(-1, 0, 1);
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0)));
    EXPECT_TRUE(farTl->IsPseudoClassSet("active"));
    EXPECT_FALSE(nearTl->IsPseudoClassSet("active"));
    EXPECT_EQ(system().getTextureFocus(), farDocument.getEntity());
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-0.5f, 0.5f, 0)));
    // the near document saw the pointer (no content under it); both lose it off the walls
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(6.0f, 0.0f, 0)));
    EXPECT_FALSE(farTl->IsPseudoClassSet("hover"));
    EXPECT_FALSE(farBr->IsPseudoClassSet("hover"));
}

namespace {

    // a 2D UI scene (the fixture's own camera) with one texture-mode document
    class TextureTargetTest : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            HtmlUIClock::setFixedTime(0.0);
        }
        void TearDown() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            FullPathGoldenTest::TearDown();
        }

        static UIDocument textureDocument(SceneFixture& fixture, int width, int height, float ratio){
            UIDocument document = fixture.addDocument("p1/texture_mode.rml");
            UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = width;
            component.textureHeight = height;
            component.pixelRatio = ratio;
            return document;
        }

        struct Texels {
            std::vector<uint8_t> rgba;
            int width = 0, height = 0;
            const uint8_t* at(int x, int y) const{ return &rgba[(size_t(y) * width + x) * 4]; }
            ::testing::AssertionResult is(int x, int y, int r, int g, int b, int a, int tolerance = 1) const{
                const uint8_t* p = at(x, y);
                if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance &&
                    std::abs(p[3] - a) <= tolerance)
                    return ::testing::AssertionSuccess();
                return ::testing::AssertionFailure() << "texel (" << x << ", " << y << ") is (" << int(p[0]) << ", " << int(p[1]) << ", "
                                                     << int(p[2]) << ", " << int(p[3]) << "), expected (" << r << ", " << g << ", " << b
                                                     << ", " << a << ") +-" << tolerance;
            }
        };

        static bool read(Framebuffer& target, Texels& texels){
            texels.width = (int)target.getWidth();
            texels.height = (int)target.getHeight();
            return readTexture(target.getRender().getColorTexture(), texels.width, texels.height, texels.rgba);
        }
    };

}

TEST_F(TextureTargetTest, StraightAlphaTopLeft){
    SceneFixture fixture;
    UIDocument document = textureDocument(fixture, 256, 256, 0.0f);
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Framebuffer* target = document.getTargetTexture();
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->isCreated());
    EXPECT_EQ(target->getWidth(), 256u);
    EXPECT_EQ(target->getHeight(), 256u);
    EXPECT_EQ(target->getRender().getSampleCount(), 1);

    Texels texels;
    ASSERT_TRUE(read(*target, texels));
    // an opaque red div; the top-left square is at texel (0, 0): top-left origin
    EXPECT_TRUE(texels.is(0, 0, 255, 0, 0, 255));
    EXPECT_TRUE(texels.is(32, 32, 255, 0, 0, 255));
    EXPECT_TRUE(texels.is(63, 63, 255, 0, 0, 255));
    // 50% white: straight (255, 255, 255, 128), not premultiplied (128, 128, 128, 128)
    EXPECT_TRUE(texels.is(96, 32, 255, 255, 255, 128));
    // 25% blue
    EXPECT_TRUE(texels.is(160, 32, 0, 0, 255, 64));
    // the transparent body
    EXPECT_EQ(texels.at(220, 32)[3], 0);
    EXPECT_EQ(texels.at(128, 80)[3], 0);

    // the screen context draws nothing of it: the scene stays the background
    Screen screen;
    ASSERT_TRUE(capture(screen));
    EXPECT_TRUE(screen.isBackground(32, 32));

    // G25
    CompareResult result = compareWithGolden("p1/texture_mode", texels.rgba, texels.width, texels.height);
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(TextureTargetTest, PixelRatioAndConsumers){
    SceneFixture fixture;
    // 512 texels for the 256 CSS px layout
    UIDocument document = textureDocument(fixture, 512, 512, 2.0f);
    // an Image showing the texture: upright
    Image& image = fixture.add<Image>();
    image.setPosition(100, 50);
    image.setSize(128, 128);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Framebuffer* target = document.getTargetTexture();
    ASSERT_NE(target, nullptr);
    image.setTexture(target);
    GpuEnvironment::drawFrames(2);

    Texels texels;
    ASSERT_TRUE(read(*target, texels));
    ASSERT_EQ(texels.width, 512);
    // the red square spans 128 texels
    EXPECT_TRUE(texels.is(120, 120, 255, 0, 0, 255));
    EXPECT_TRUE(texels.is(136, 60, 255, 255, 255, 128));

    // the image's top-left quarter (64 texels of 512 -> 16 px) is red, its top right corner empty
    Screen screen;
    ASSERT_TRUE(capture(screen));
    EXPECT_TRUE(screen.is(100 + 4, 50 + 4, 255, 0, 0, 8));
    EXPECT_TRUE(screen.isBackground(100 + 124, 50 + 4));

    // hidden: the texture is cleared
    const unsigned long version = target->getVersion();
    document.hide();
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(read(*target, texels));
    EXPECT_EQ(texels.at(120, 120)[3], 0);
    // shown again: drawn again, the same Framebuffer (consumers keep it)
    document.show();
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(read(*target, texels));
    EXPECT_TRUE(texels.is(120, 120, 255, 0, 0, 255));
    EXPECT_EQ(document.getTargetTexture(), target);

    // a new size recreates the texture (a version bump rebinds consumers)
    document.getComponent<UIDocumentComponent>().textureWidth = 256;
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(target->getWidth(), 256u);
    EXPECT_GT(target->getVersion(), version);
    ASSERT_TRUE(read(*target, texels));
    EXPECT_TRUE(texels.is(60, 60, 255, 0, 0, 255));   // 256 x 512 texels: 128 x 256 CSS px
}

TEST_F(TextureTargetTest, NoLeaksOverReloads){
    SceneFixture fixture;
    UIDocument document = textureDocument(fixture, 128, 128, 1.0f);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = *fixture.getScene().getSystem<UIDocumentSystem>();
    HtmlUIContext* context = system.getTextureContext(document.getEntity());
    ASSERT_NE(context, nullptr);
    const size_t textures = context->getRenderer().getLiveTextureCount();
    const size_t geometry = context->getRenderer().getLiveGeometryCount();

    for (int i = 0; i < 20; i++){
        document.getComponent<UIDocumentComponent>().needReload = true;
        GpuEnvironment::drawFrames(1);
    }
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(system.getTextureContext(document.getEntity()), context);
    EXPECT_EQ(context->getRenderer().getLiveTextureCount(), textures);
    EXPECT_EQ(context->getRenderer().getLiveGeometryCount(), geometry);
}

TEST_F(TextureModeTest, KeysTextAndDataModels){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_form.rml", surface.getEntity());
    // a screen document too: keys go to whichever took the last press
    UIDocument screen = fixture->addDocument("p1/texture_input.rml");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    // the screen document is out of the way until its press below
    screen.hide();
    GpuEnvironment::drawFrames(1);

    // the data model placeholder lives in the texture context: no error, empty binding
    Rml::Element* bound = element(entity, "bound");
    ASSERT_NE(bound, nullptr);

    // a press on the text field through the engine's pointer path (p1-input -> world)
    const Vector2 field = project(scene(), Vector3(-0.5f, 0.75f, 0));   // uv (0.25, 0.125): #name
    EXPECT_TRUE(system().eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    EXPECT_TRUE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    EXPECT_TRUE(system().eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    EXPECT_FALSE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    EXPECT_EQ(system().getTextureFocus(), entity);
    EXPECT_TRUE(system().hasFocusedTextInput());

    // text and keys reach the texture document's field
    EXPECT_TRUE(system().eventOnTextInput("h\xc3\xa9llo"));
    GpuEnvironment::drawFrames(1);
    Rml::Element* name = element(entity, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->GetAttribute<Rml::String>("value", ""), "h\xc3\xa9llo");
    EXPECT_TRUE(system().eventOnKeyDown(D_KEY_BACKSPACE, false, 0));
    EXPECT_TRUE(system().eventOnKeyUp(D_KEY_BACKSPACE, 0));
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(name->GetAttribute<Rml::String>("value", ""), "h\xc3\xa9ll");
    // Escape is never a text field's
    EXPECT_FALSE(system().eventOnKeyDown(D_KEY_ESCAPE, false, 0));

    // a press on the screen document's #tl takes the keys back to the screen context
    screen.show();
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(system().eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, 10, 10, 0, false));
    EXPECT_TRUE(system().eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, 10, 10, 0, false));
    EXPECT_EQ(system().getTextureFocus(), NULL_ENTITY);
    EXPECT_FALSE(system().hasFocusedTextInput());
    EXPECT_FALSE(system().eventOnTextInput("x"));
    EXPECT_EQ(name->GetAttribute<Rml::String>("value", ""), "h\xc3\xa9ll");
    screen.hide();

    // a data model made by the document's script binds in its own context
    UIValue initial = UIValue::object();
    initial["label"] = "Bound";
    UIDataModel model = document.createDataModel("form", initial);
    EXPECT_TRUE(model.isValid());
    ASSERT_TRUE(fixture->waitUntilLoaded());
    bound = element(entity, "bound");
    ASSERT_NE(bound, nullptr);
    EXPECT_EQ(bound->GetInnerRML(), "Bound");
    EXPECT_EQ(system().getContext()->getRmlContext()->GetDataModels().count("form"), 0u);
    EXPECT_EQ(system().getTextureContext(entity)->getRmlContext()->GetDataModels().count("form"), 1u);
}

// p4-input: gamepad keys follow the keyboard's rule: the texture document that took the last
// press gets them; a press on the screen document takes them back
TEST_F(TextureModeTest, GamepadKeysFollowTheKeys){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_form.rml", surface.getEntity());
    UIDocument screen = fixture->addDocument("p1/texture_input.rml");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    screen.hide();
    GpuEnvironment::drawFrames(1);

    Rml::Element* name = element(entity, "name");
    ASSERT_NE(name, nullptr);
    CountingListener keys;
    name->AddEventListener(Rml::EventId::Keydown, &keys);
    EXPECT_FALSE(system().hasGamepadFocus());
    EXPECT_FALSE(system().eventOnGamepadKeyDown(0, GamepadUIKey::A, false)) << "nothing focused: the game's";
    EXPECT_FALSE(system().eventOnGamepadKeyUp(0, GamepadUIKey::A));

    const Vector2 field = project(scene(), Vector3(-0.5f, 0.75f, 0));   // #name
    EXPECT_TRUE(system().eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    EXPECT_TRUE(system().eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    ASSERT_EQ(system().getTextureFocus(), entity);
    EXPECT_TRUE(system().hasGamepadFocus());

    // A on the texture document's text field: consumed, and the field edits nothing
    EXPECT_TRUE(system().eventOnGamepadKeyDown(0, GamepadUIKey::A, false));
    EXPECT_TRUE(system().eventOnGamepadKeyUp(0, GamepadUIKey::A));
    EXPECT_EQ(keys.count, 1);
    EXPECT_EQ(name->GetAttribute<Rml::String>("value", ""), "");
    // the d-pad is the document's even with nothing else to focus
    EXPECT_TRUE(system().eventOnGamepadKeyDown(0, GamepadUIKey::DPAD_DOWN, false));
    EXPECT_TRUE(system().eventOnGamepadKeyUp(0, GamepadUIKey::DPAD_DOWN));
    EXPECT_EQ(keys.count, 2);

    // a press on the screen document takes the keys back to the screen context, where the clicked
    // panel has the focus but is not in the Tab order: the gamepad is the game's
    screen.show();
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(system().eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, 10, 10, 0, false));
    EXPECT_TRUE(system().eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, 10, 10, 0, false));
    EXPECT_EQ(system().getTextureFocus(), NULL_ENTITY);
    EXPECT_FALSE(system().hasGamepadFocus());
    EXPECT_FALSE(system().eventOnGamepadKeyDown(0, GamepadUIKey::A, false));
    EXPECT_FALSE(system().eventOnGamepadKeyUp(0, GamepadUIKey::A));
    EXPECT_EQ(keys.count, 2);
    name->RemoveEventListener(Rml::EventId::Keydown, &keys);
}

TEST_F(TextureModeTest, StatePreservingReload){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("p1/texture_form.rml", surface.getEntity());
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    HtmlUIContext* context = system().getTextureContext(entity);
    ASSERT_NE(context, nullptr);
    Framebuffer* target = system().getTargetTexture(entity);

    // a value typed into the field, then p1-editor's full reload (hot reload of the .html)
    const Vector2 field = project(scene(), Vector3(-0.5f, 0.75f, 0));
    EXPECT_TRUE(system().eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    EXPECT_TRUE(system().eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, field.x, field.y, 0, false));
    EXPECT_TRUE(system().eventOnTextInput("kept"));
    GpuEnvironment::drawFrames(1);
    Rml::ElementDocument* before = system().getDocument(entity);
    const uint32_t loads = system().getLoadCount(entity);

    system().reloadDocument(entity, true);
    GpuEnvironment::drawFrames(2);

    Rml::ElementDocument* after = system().getDocument(entity);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after, before);
    EXPECT_GT(system().getLoadCount(entity), loads);
    // in its own context still, with its texture, and the value kept
    EXPECT_EQ(system().getTextureContext(entity), context);
    EXPECT_EQ(after->GetContext(), context->getRmlContext());
    EXPECT_EQ(context->getRmlContext()->GetNumDocuments(), 1);
    EXPECT_EQ(system().getTargetTexture(entity), target);
    Rml::Element* name = after->GetElementById("name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->GetAttribute<Rml::String>("value", ""), "kept");
    EXPECT_EQ(system().getContext()->getRmlContext()->GetNumDocuments(), 0);

    // a state-preserving reload that switches to SCREEN moves it, the texture context goes
    document.getComponent<UIDocumentComponent>().renderMode = UIDocumentRenderMode::SCREEN;
    system().reloadDocument(entity, true);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(system().getTextureContext(entity), nullptr);
    ASSERT_NE(system().getDocument(entity), nullptr);
    EXPECT_EQ(system().getDocument(entity)->GetContext(), system().getContext()->getRmlContext());
    EXPECT_EQ(system().getDocument(entity)->GetElementById("name")->GetAttribute<Rml::String>("value", ""), "kept");
}

// S17 R32: a TEXTURE-mode document has tooltips, drags and virtual views through world-space input
TEST_F(TextureModeTest, WidgetsThroughWorldPointer){
    Primitive& surface = wall(0, 0, 0);
    UIDocument document = textureDocument("s17/texture_widgets.html", surface.getEntity());
    ASSERT_TRUE(fixture->waitUntilLoaded());
    HtmlUIClock::setFixedTime(50.0);

    // the list view's rows, bound by a provider
    UIItemsView list = document.getElementById("list").getItemsView();
    ASSERT_TRUE(list.isValid());
    UIItemsProvider provider;
    provider.count = 100;
    provider.bind = [](const UIElement& row, int index){ row.querySelector(".row").setTextContent("item " + std::to_string(index)); };
    list.setProvider(provider);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(list.getVisibleRange().first, 1);
    EXPECT_EQ(list.getRow(1).getTextContent(), "item 1");

    // the title's tooltip after the delay, inside the texture
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(-0.5f, 0.5f, 0)));
    for (double t = 50.1; t <= 51.0; t += 0.1){
        HtmlUIClock::setFixedTime(t);
        GpuEnvironment::drawFrames(1);
    }
    UITooltipInfo tip = document.getTooltip();
    ASSERT_TRUE(tip.visible);
    EXPECT_EQ(tip.text, "World tip");
    EXPECT_GE(tip.rect.x, 0.0f);
    EXPECT_LE(tip.rect.x + tip.rect.width, 256.0f);
    EXPECT_LE(tip.rect.y + tip.rect.height, 256.0f);

    // a drag from the top-right quadrant to the bottom-right one
    int drops = 0;
    UIElement source = document.getElementById("src");
    source.addEventListener("dragstart", [](UIEvent& event){ event.getDataTransfer().setData("item", "axe"); });
    document.getElementById("bin").addEventListener("drop", [&drops](UIEvent& event){
        drops += event.getDataTransfer().getData("item") == "axe" ? 1 : 0;
    });
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(0.5f, 0.5f, 0)));
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(0.5f, 0.5f, 0)));
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(0.45f, 0.4f, 0)));
    HtmlUIClock::setFixedTime(51.1);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(document.getTooltip().visible) << "a press hides the tooltip";
    EXPECT_TRUE(pointer(PointerPhase::MOVE, Vector3(0.5f, -0.5f, 0)));
    HtmlUIClock::setFixedTime(51.2);
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(0.5f, -0.5f, 0)));
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(drops, 1);
    HtmlUIClock::reset();
}
