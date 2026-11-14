// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S93 P2 (R8-R10, AC11): drag scrolling with uGUI ScrollRect physics. The scroll container
// properties scroll-drag, scroll-movement, scroll-elasticity, scroll-inertia and
// scroll-deceleration-rate, through the whole HTML pipeline into a context drawn by a recording
// render interface. The UI clock is fixed and stepped by 1/60 s, so every trace is exact.

#include "TestScriptCommon.h"
#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    constexpr double kFrame = 1.0 / 60.0;

    class CountingListener : public Rml::EventListener {
    public:
        int count = 0;
        void ProcessEvent(Rml::Event&) override{ count++; }
    };

    // A 300 x 300 list at (100, 100) of 100 rows of 50 px (5000 px of content), the row 'r<i>'
    // a button; `listStyle` adds the declarations under test to the list
    struct ScrollPage {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string path;
        double time = 1.0;
        CountingListener clicks, cancels;

        explicit ScrollPage(const std::string& name, const std::string& listStyle)
            : ok(HtmlUI::acquire()), path("@htmlui/test/scroll-physics/" + name + ".html"){
            if (!ok)
                return;
            HtmlUIClock::setFixedTime(time);
            context = Rml::CreateContext("scroll-physics", Rml::Vector2i(800, 600), &recording);
            std::string rows;
            for (int i = 0; i < 100; i++)
                rows += "<button class='row' id='r" + std::to_string(i) + "'>row " + std::to_string(i) + "</button>";
            VirtualFiles::add(path, "<!DOCTYPE html><html><head><style>html, body { margin: 0; width: 800px; height: 600px; } "
                                    "#list { position: absolute; left: 100px; top: 100px; width: 300px; height: 300px; overflow-y: auto; "
                                    "background: #222; " + listStyle + " } "
                                    ".row { display: block; box-sizing: border-box; width: 100%; height: 50px; margin: 0; border: 0; padding: 0; } "
                                    "</style></head><body><div id='list'>" + rows + "</div></body></html>");
            CompatOptions options;
            options.viewport = CompatSize{800, 600};
            options.fork = ForkFeatures::present();
            document = DocumentLoader::load(context, path, options);
            if (document){
                document->Show();
                document->AddEventListener("click", &clicks, true);
                document->AddEventListener("pointercancel", &cancels, true);
                frame();
            }
        }
        ~ScrollPage(){
            if (document){
                document->RemoveEventListener("click", &clicks, true);
                document->RemoveEventListener("pointercancel", &cancels, true);
                document->Close();
            }
            if (context){
                context->Update();
                Rml::RemoveContext("scroll-physics");
            }
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            VirtualFiles::remove(path);
            DocumentLoader::clearCache();
            HtmlUIClock::setFixedTime(std::nullopt);
            if (ok)
                HtmlUI::release();
        }

        // the next frame, 1/60 s of UI time later
        void frame(){
            HtmlUIClock::setFixedTime(time);
            context->Update();
            context->Render();
        }
        void advance(int frames = 1){
            for (int i = 0; i < frames; i++){
                time += kFrame;
                frame();
            }
        }

        Rml::Element* list() const{ return document->GetElementById("list"); }
        Rml::Element* row(int i) const{ return document->GetElementById("r" + std::to_string(i)); }
        float scrollTop() const{ return list()->GetScrollTop(); }
        // where the content is drawn relative to where scrollTop puts it: > 0 pulled down past the top
        // (an elastic overscroll), < 0 pushed up past the end
        float displacement() const{
            return row(0)->GetAbsoluteOffset(Rml::BoxArea::Border).y - (100.0f - list()->GetScrollTop());
        }

        static Rml::PointerInput finger(float x, float y){
            Rml::PointerInput input;
            input.id = 2;
            input.type = Rml::PointerType::Touch;
            input.position = Rml::Vector2f(x, y);
            return input;
        }
        void touchDown(float x, float y){ context->ProcessPointerDown(finger(x, y)); }
        void touchMove(float x, float y){ context->ProcessPointerMove(finger(x, y)); }
        void touchUp(float x, float y){ context->ProcessPointerUp(finger(x, y)); }
        void mouseMove(float x, float y){ context->ProcessMouseMove((int)x, (int)y, 0); }
        void mouseDown(){ context->ProcessMouseButtonDown(0, 0); }
        void mouseUp(){ context->ProcessMouseButtonUp(0, 0); }

        // a finger (or the mouse) from y0 to y1 at x 250 in `steps` moves, one a frame
        void drag(bool mouse, float y0, float y1, int steps){
            if (mouse){
                mouseMove(250, y0);
                mouseDown();
            }else{
                touchDown(250, y0);
            }
            for (int i = 1; i <= steps; i++){
                advance();
                const float y = y0 + (y1 - y0) * (float)i / (float)steps;
                if (mouse)
                    mouseMove(250, y);
                else
                    touchMove(250, y);
            }
        }
    };

    // Unity's Mathf.SmoothDamp (Game Programming Gems 4, 1.10): the reference of the elastic return
    float smoothDamp(float current, float target, float& velocity, float smoothTime, float deltaTime){
        smoothTime = std::max(0.0001f, smoothTime);
        const float omega = 2.0f / smoothTime;
        const float x = omega * deltaTime;
        const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float change = current - target;
        const float temp = (velocity + omega * change) * deltaTime;
        velocity = (velocity - omega * temp) * exp;
        float output = target + (change + temp) * exp;
        if ((target - current > 0.0f) == (output > target)){
            output = target;
            velocity = 0.0f;
        }
        return output;
    }

}

// The default (no scroll physics property): a touch fling ends with today's inertia, frame by
// frame. The trace was recorded on the base of S93 (6e3924918) before the change.
TEST(ScrollPhysics, ClampedDefaultUnchanged){
    ScrollPage page("clamped", "");
    ASSERT_TRUE(page.document);
    page.drag(false, 350, 150, 10);
    page.touchUp(250, 150);
    std::vector<float> trace;
    for (int i = 0; i < 40; i++){
        page.advance();
        trace.push_back(page.scrollTop());
    }
    const std::vector<float> recorded = {220, 238, 255, 270, 284, 297, 309, 320, 330, 339, 347, 355, 362, 368, 374, 379, 384, 389, 393, 397,
                                         401, 404, 407, 410, 412, 414, 416, 418, 420, 422, 423, 424, 425, 426, 427, 428, 429, 430, 431, 432};
    std::string text;
    for (float top : trace)
        text += std::to_string((int)top) + ", ";
    std::printf("[ scroll trace ] %s\n", text.c_str());
    ASSERT_EQ(trace.size(), recorded.size()) << text;
    for (size_t i = 0; i < trace.size(); i++)
        EXPECT_EQ(trace[i], recorded[i]) << "frame " << i << ": " << text;
    EXPECT_EQ(page.displacement(), 0.0f) << "clamped content never leaves its bounds";
}

// scroll-drag: pointer makes the mouse drag-pan the list; without it the mouse does not.
TEST(ScrollPhysics, MouseDragPansWithPointerScrollDrag){
    {
        ScrollPage page("mouse-default", "");
        ASSERT_TRUE(page.document);
        page.drag(true, 350, 150, 10);
        page.mouseUp();
        page.advance(5);
        EXPECT_EQ(page.scrollTop(), 0.0f) << "the mouse does not pan by default (scroll-drag: touch)";
    }
    ScrollPage page("mouse-pointer", "scroll-drag: pointer;");
    ASSERT_TRUE(page.document);
    page.drag(true, 350, 150, 10);
    EXPECT_EQ(page.scrollTop(), 200.0f) << "the content follows the mouse";
    page.mouseUp();
    page.advance(30);
    EXPECT_GT(page.scrollTop(), 200.0f) << "a mouse fling ends with inertia";
    EXPECT_EQ(page.clicks.count, 0) << "the panned press clicked nothing";
}

// A pan past the slop cancels the press: pointercancel, no click, ':active' removed; the mouse
// included under scroll-drag: pointer. A press that does not pan still clicks.
TEST(ScrollPhysics, DragCancelsClick){
    ScrollPage page("cancel", "scroll-drag: pointer;");
    ASSERT_TRUE(page.document);
    // a click without a pan
    page.mouseMove(250, 220);
    page.mouseDown();
    page.advance();
    page.mouseUp();
    EXPECT_EQ(page.clicks.count, 1) << "a still press clicks";

    page.mouseMove(250, 320);
    page.mouseDown();
    page.advance();
    Rml::Element* pressed = page.row(4);
    EXPECT_TRUE(pressed->IsPseudoClassSet("active")) << "the pressed row is :active";
    for (int i = 1; i <= 8; i++){
        page.advance();
        page.mouseMove(250, 320.0f - 10.0f * (float)i);
    }
    EXPECT_FALSE(pressed->IsPseudoClassSet("active")) << "the pan removed :active";
    EXPECT_EQ(page.cancels.count, 1) << "the pan sent pointercancel";
    page.mouseUp();
    EXPECT_EQ(page.clicks.count, 1) << "the panned press clicked nothing";
    EXPECT_GT(page.scrollTop(), 0.0f);

    // a finger
    const int cancels = page.cancels.count;
    page.advance(60);
    page.drag(false, 350, 250, 6);
    page.touchUp(250, 250);
    EXPECT_EQ(page.cancels.count, cancels + 1) << "the finger's pan sent pointercancel";
    EXPECT_EQ(page.clicks.count, 1) << "the finger's pan clicked nothing";
}

// scroll-movement: elastic: a pull past the top follows uGUI's RubberDelta while dragging; the
// release (at 120 px over, still) springs back with a SmoothDamp over scroll-elasticity (0.1 s),
// within 0.5 px of Mathf.SmoothDamp each frame, and is back within 6 x 0.1 s.
TEST(ScrollPhysics, ElasticOverscrollReturns){
    ScrollPage page("elastic", "scroll-movement: elastic;");
    ASSERT_TRUE(page.document);
    // RubberDelta(excess, 300) = (1 - 1 / (excess * 0.55 / 300 + 1)) * 300 = 120 at excess 363.64 px
    const float excess = 300.0f * (1.0f / (1.0f - 120.0f / 300.0f) - 1.0f) / 0.55f;
    page.drag(false, 120, 120 + excess, 20);
    EXPECT_EQ(page.scrollTop(), 0.0f);
    EXPECT_NEAR(page.displacement(), 120.0f, 1.0f) << "the rubber band at the release";
    // held still past the inertia window: the release has no velocity
    page.advance(12);
    page.touchUp(250, 120 + excess);

    float reference = page.displacement();
    float velocity = 0.0f;
    for (int i = 1; i <= 36; i++){
        page.advance();
        reference = smoothDamp(reference, 0.0f, velocity, 0.1f, (float)kFrame);
        EXPECT_NEAR(page.displacement(), reference, 0.5f) << "frame " << i;
    }
    EXPECT_LT(std::fabs(page.displacement()), 0.5f) << "back within 0.6 s";
    page.advance(10);
    EXPECT_EQ(page.displacement(), 0.0f) << "at rest";
    EXPECT_EQ(page.scrollTop(), 0.0f);
}

// scroll-deceleration-rate: 0.135: a fling keeps 13.5 % of its velocity after 1 s (uGUI: v *= rate^dt each
// frame), within 1 %; 6.3's curve (v *= 1 - 5 dt) would keep 0.5 %
TEST(ScrollPhysics, DecelerationRateMatchesUGUI){
    ScrollPage page("deceleration", "scroll-deceleration-rate: 0.135;");
    ASSERT_TRUE(page.document);
    page.context->SetScrollVelocity(page.list(), Rml::Vector2f(0.0f, 1000.0f));
    const float v0 = page.context->GetScrollVelocity(page.list()).y;
    EXPECT_EQ(v0, 1000.0f);
    page.advance(60);
    const float v1 = page.context->GetScrollVelocity(page.list()).y;
    EXPECT_NEAR(v1, v0 * 0.135f, v0 * 0.135f * 0.01f) << "the velocity after 1 s";
    // it moved as the model does frame by frame (the velocity decelerated, then applied), on an exact
    // position that scrollTop rounds
    float velocity = v0, expected = 0.0f;
    for (int i = 0; i < 60; i++){
        velocity *= std::pow(0.135f, (float)kFrame);
        expected += velocity * (float)kFrame;
    }
    EXPECT_NEAR(page.scrollTop(), expected, 1.0f);
    // and stops below 1 px/s
    page.advance(600);
    EXPECT_EQ(page.context->GetScrollVelocity(page.list()).y, 0.0f);
}

namespace {

    class ScrollPhysicsLua : public ScriptTest {
    protected:
        const char* documentFile() const override { return "s93/scroll_velocity.html"; }
        void TearDown() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            ScriptTest::TearDown();
        }
        void frame(double time){
            HtmlUIClock::setFixedTime(time);
            update();
        }
    };

}

// R10: el.scrollVelocity reads and sets the velocity (px/s, a Vector2), el:stopScrolling() stops it
TEST_F(ScrollPhysicsLua, ScrollVelocityAndStopScrolling){
    frame(1.0);
    EXPECT_LUA("el = doc:getElementById('list')");
    EXPECT_EQ(luaString("el.scrollVelocity.x == 0 and el.scrollVelocity.y == 0"), "true") << "at rest";
    EXPECT_LUA("el.scrollVelocity = Vector2(0, 1200)");
    EXPECT_EQ(luaString("math.floor(el.scrollVelocity.y + 0.5)"), "1200");
    frame(1.0 + 1.0 / 60.0);
    frame(1.0 + 2.0 / 60.0);
    EXPECT_EQ(luaString("el.scrollTop > 0 and el.scrollVelocity.y > 0 and el.scrollVelocity.y < 1200"), "true") << "flung, decelerating";
    EXPECT_LUA("el:stopScrolling()");
    EXPECT_EQ(luaString("el.scrollVelocity.y"), luaString("0.0"));
    const std::string top = luaString("el.scrollTop");
    frame(1.0 + 3.0 / 60.0);
    EXPECT_EQ(luaString("el.scrollTop"), top) << "stopped";
}
