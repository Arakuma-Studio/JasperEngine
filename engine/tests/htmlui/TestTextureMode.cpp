// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Render-to-texture documents and world-space input (phase-1 spec §3.12, §5.1):
// raycastMeshUV on primitives, the texture-mode document lifecycle (own context, target,
// mode switches, removal) and pointer events reaching a document through a mesh.

#include "TestGoldenCommon.h"

#include "HtmlUIContext.h"
#include "Input.h"
#include "UIWorldRaycast.h"
#include "component/MeshComponent.h"
#include "component/UIDocumentComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/Context.h>
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

        bool pointer(PointerPhase phase, const Vector3& world, int id = 0){
            const Vector2 point = project(scene(), world);
            return system().dispatchWorldPointer(id, D_MOUSE_BUTTON_LEFT, point.x, point.y, 0, phase, false);
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
    UIDocument document = textureDocument("p1/texture_input.html", surface.getEntity(), 512, 256, 2.0f);
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
    UIDocument screen = fixture->addDocument("p1/texture_input.html");
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
    UIDocument document = textureDocument("p1/texture_input.html", surface.getEntity());
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
    UIDocument document = textureDocument("p1/texture_input.html", surface.getEntity());
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
    // another pointer is not captured
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0), 1));
    // released off-surface, over the clamped corner: a click on #tl, then nothing captured
    EXPECT_TRUE(pointer(PointerPhase::UP, Vector3(-4.0f, 4.0f, 0)));
    EXPECT_EQ(clicks.count, 1);
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0)));

    // a cancel releases the capture
    EXPECT_TRUE(pointer(PointerPhase::DOWN, Vector3(-0.5f, 0.5f, 0), 2));
    EXPECT_TRUE(system().dispatchWorldPointer(2, 0, 0, 0, 0, PointerPhase::CANCEL, true));
    EXPECT_FALSE(pointer(PointerPhase::MOVE, Vector3(-4.0f, 4.0f, 0), 2));

    tl->RemoveEventListener(Rml::EventId::Click, &clicks);
}

TEST_F(TextureModeTest, WorldPointerNearestFirst){
    // two documents on overlapping walls: the nearer gets the pointer where it has content,
    // its transparent body passes the pointer to the one behind
    Primitive& farWall = wall(0, 0, 0);
    Primitive& nearWall = wall(0, 0, 1);
    UIDocument farDocument = textureDocument("p1/texture_input.html", farWall.getEntity());
    UIDocument nearDocument = textureDocument("p1/texture_input.html", nearWall.getEntity());
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
