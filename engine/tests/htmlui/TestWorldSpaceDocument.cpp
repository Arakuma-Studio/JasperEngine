// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S93 R36 / AC41: a UI Document in Render Mode World Space. Its quad (texture size / Pixels Per
// Unit, at the GameObject, Billboard), drawn depth-tested among the scene's objects (hidden where a
// cube covers it: golden on glcore and Vulkan), and pointer input on the quad that a mesh in front
// of it takes.

#include "TestGoldenCommon.h"

#include "Input.h"
#include "UIWorldRaycast.h"
#include "component/CameraComponent.h"
#include "component/UIDocumentComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/ui/UIDocument.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class WorldClicks : public Rml::EventListener {
    public:
        int count = 0;
        void ProcessEvent(Rml::Event&) override{ count++; }
    };

    // the canvas point of a world point through the scene camera
    Vector2 worldToCanvas(Scene& scene, const Vector3& world){
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const float x = clip.x / clip.w, y = clip.y / clip.w;
        return Vector2((x + 1.0f) * 0.5f * Engine::getCanvasWidth(), (1.0f - y) * 0.5f * Engine::getCanvasHeight());
    }

    class WorldSpaceDocumentTest : public FullPathGoldenTest {
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

        // the two-quadrant document (red top-left, blue bottom-right) as a 2 x 2 quad at the origin
        UIDocument worldDocument(){
            UIDocument document = fixture->addDocument("p1/texture_input.rml");
            UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::WORLD;
            component.textureWidth = 256;
            component.textureHeight = 256;
            component.pixelsPerUnit = 128.0f;
            return document;
        }

        Rml::Element* element(Entity entity, const std::string& id){
            Rml::ElementDocument* document = system().getDocument(entity);
            return document ? document->GetElementById(id) : nullptr;
        }

        bool click(const Vector3& world){
            const Vector2 point = worldToCanvas(scene(), world);
            const bool down = system().dispatchWorldPointer(0, D_MOUSE_BUTTON_LEFT, point.x, point.y, 0, PointerPhase::DOWN, false);
            system().dispatchWorldPointer(0, D_MOUSE_BUTTON_LEFT, point.x, point.y, 0, PointerPhase::UP, false);
            return down;
        }

        // the canvas pixel of a world point (the harness target is the canvas)
        static PixelRect around(const Vector2& point, int radius){
            return PixelRect{(int)point.x - radius, (int)point.y - radius, (int)point.x + radius, (int)point.y + radius};
        }
    };

}

TEST_F(WorldSpaceDocumentTest, QuadIsTextureSizeOverPixelsPerUnit){
    UIDocument document = worldDocument();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();

    // its own context and target, as a texture-mode document
    ASSERT_NE(system().getTargetTexture(entity), nullptr);
    Matrix4 quad;
    ASSERT_TRUE(system().getWorldQuad(entity, nullptr, quad));
    EXPECT_NEAR((quad * Vector3(-1, 1, 0)).x, -1.0f, 1e-5f);
    EXPECT_NEAR((quad * Vector3(-1, 1, 0)).y, 1.0f, 1e-5f);
    EXPECT_NEAR((quad * Vector3(1, -1, 0)).x, 1.0f, 1e-5f);

    // 64 px per unit: twice as large; the GameObject's transform places it
    document.getComponent<UIDocumentComponent>().pixelsPerUnit = 64.0f;
    document.setPosition(3, 0, 0);
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(system().getWorldQuad(entity, nullptr, quad));
    EXPECT_NEAR((quad * Vector3(1, 1, 0)).x, 5.0f, 1e-4f);
    EXPECT_NEAR((quad * Vector3(1, 1, 0)).y, 2.0f, 1e-4f);

    // the uv a ray hits: top-left texel at the top-left corner
    const UIWorldHit hit = raycastQuadUV(quad, Ray(Vector3(1.5f, 1.5f, 5), Vector3(0, 0, -10)));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.uv.x, 0.125f, 1e-4f);
    EXPECT_NEAR(hit.uv.y, 0.125f, 1e-4f);
    EXPECT_NEAR(hit.distance, 5.0f, 1e-4f);
    EXPECT_FALSE(raycastQuadUV(quad, Ray(Vector3(-0.5f, 0, 5), Vector3(0, 0, -10))).hit) << "beside the quad";

    // not World Space: no quad
    document.getComponent<UIDocumentComponent>().renderMode = UIDocumentRenderMode::TEXTURE;
    EXPECT_FALSE(system().getWorldQuad(entity, nullptr, quad));
}

TEST_F(WorldSpaceDocumentTest, PointerHitsInFrontMissesBehind){
    UIDocument document = worldDocument();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    Rml::Element* tl = element(entity, "tl");
    Rml::Element* br = element(entity, "br");
    ASSERT_NE(tl, nullptr);
    ASSERT_NE(br, nullptr);
    WorldClicks tlClicks, brClicks;
    tl->AddEventListener(Rml::EventId::Click, &tlClicks);
    br->AddEventListener(Rml::EventId::Click, &brClicks);

    // no mesh: the quad takes the pointer
    EXPECT_TRUE(click(Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(tlClicks.count, 1);
    EXPECT_TRUE(click(Vector3(0.5f, -0.5f, 0)));
    EXPECT_EQ(brClicks.count, 1);

    // a cube behind the document does not block it
    Primitive& behind = fixture->add<Primitive>();
    behind.createBox(1, 1, 1);
    behind.setPosition(-0.5f, 0.5f, -2.0f);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_TRUE(click(Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(tlClicks.count, 2);

    // a cube in front of #tl takes the ray: no click there, #br still gets its own
    Primitive& front = fixture->add<Primitive>();
    front.createBox(1, 1, 1);
    front.setPosition(-0.5f, 0.5f, 1.5f);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_FALSE(click(Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(tlClicks.count, 2);
    EXPECT_TRUE(click(Vector3(0.5f, -0.5f, 0)));
    EXPECT_EQ(brClicks.count, 2);

    tl->RemoveEventListener(Rml::EventId::Click, &tlClicks);
    br->RemoveEventListener(Rml::EventId::Click, &brClicks);
}

TEST_F(WorldSpaceDocumentTest, BillboardFacesTheCamera){
    UIDocument document = worldDocument();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = document.getEntity();
    Rml::Element* tl = element(entity, "tl");
    ASSERT_NE(tl, nullptr);
    WorldClicks clicks;
    tl->AddEventListener(Rml::EventId::Click, &clicks);

    // turned edge-on: nothing to hit, nothing drawn
    document.setRotation(0, 90, 0);
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(click(Vector3(-0.5f, 0.5f, 0)));
    Screen edgeOn;
    ASSERT_TRUE(capture(edgeOn));
    EXPECT_EQ(edgeOn.countColor(around(worldToCanvas(scene(), Vector3(-0.5f, 0.5f, 0)), 6), 255, 0, 0), 0u);

    // Billboard: it faces the camera whatever its rotation
    document.getComponent<UIDocumentComponent>().billboard = true;
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(click(Vector3(-0.5f, 0.5f, 0)));
    EXPECT_EQ(clicks.count, 1);
    Screen facing;
    ASSERT_TRUE(capture(facing));
    EXPECT_GT(facing.countColor(around(worldToCanvas(scene(), Vector3(-0.5f, 0.5f, 0)), 6), 255, 0, 0), 100u);

    tl->RemoveEventListener(Rml::EventId::Click, &clicks);
}

TEST_F(WorldSpaceDocumentTest, WorldSpaceDocumentIsOccluded){
    UIDocument document = worldDocument();
    // a cube in front of the top-left quadrant's centre, one behind the bottom-right one
    Primitive& front = fixture->add<Primitive>();
    front.createBox(0.5f, 0.5f, 0.5f);
    front.setPosition(-0.5f, 0.5f, 1.0f);
    Primitive& behind = fixture->add<Primitive>();
    behind.createBox(0.5f, 0.5f, 0.5f);
    behind.setPosition(0.5f, -0.5f, -1.0f);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen, 2));
    // the cube covers the red quadrant's centre (and, nearer the camera, its top-left part); its
    // inner corner shows red
    const Vector2 tlCentre = worldToCanvas(scene(), Vector3(-0.5f, 0.5f, 0));
    EXPECT_EQ(screen.countColor(around(tlCentre, 4), 255, 0, 0), 0u) << "the cube in front hides the document";
    const Vector2 tlInner = worldToCanvas(scene(), Vector3(-0.08f, 0.08f, 0));
    EXPECT_GT(screen.countColor(around(tlInner, 3), 255, 0, 0), 30u);
    // the cube behind is hidden by the blue quadrant
    const Vector2 brCentre = worldToCanvas(scene(), Vector3(0.5f, -0.5f, 0));
    EXPECT_GT(screen.countColor(around(brCentre, 4), 0, 0, 255), 60u) << "the document hides the cube behind it";
    // the transparent body shows the scene
    EXPECT_TRUE(screen.isBackground((int)worldToCanvas(scene(), Vector3(0.6f, 0.6f, 0)).x, (int)worldToCanvas(scene(), Vector3(0.6f, 0.6f, 0)).y));

    EXPECT_SCREEN_GOLDEN(screen, "p6/world_space_occluded");
}
