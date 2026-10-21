// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase 6 across its packages (p6-accept, docs/html-ui/phase-6-acceptance.md): what p6-video, p6-stateful-fx,
// p6-justify and p6-line-clamp could not test on their own branches.
// - <video> under RmlRenderer's command recording (phase-1 spec 3.2.10): a page of videos with a phase-5
//   filter effect, a stateful filter effect, a backdrop effect and a justified, clamped caption is
//   byte-identical recorded and drawn call by call; a video removed while its frame is recorded is drawn by
//   the replay with its released texture, then is gone (element, stream, texture).
// - Stateful effects on a <video> (filter form: the effect reads the element's content, which holds the
//   converted frame): pinned, byte-identical and replayed; live, the simulations step with the playing video.
// - Justified, indented, clamped text inside elements with paint, filter, stateful, gradient-text and backdrop
//   effects (the effects change no box; the ellipsis takes the text fill), and in a texture-mode document
//   (the same pixels as the page drawn as a screen document).

#include "effects/EffectTestCommon.h"
#include "video/VideoTestCommon.h"

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/SceneFixture.h"

#include "HtmlUIClock.h"
#include "RmlRenderer.h"
#include "component/UIDocumentComponent.h"
#include "media/UIVideoElement.h"
#include "media/VideoStream.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core.h>

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    constexpr uint8_t BACKGROUND[3] = {0x10, 0x12, 0x18};   // the pages' #101218

    // recording on or off for a scope (read by beginFrame)
    struct RecordingMode {
        bool previous;
        explicit RecordingMode(bool on): previous(RmlRenderer::isCommandRecording()){
            RmlRenderer::setCommandRecordingForTesting(on);
        }
        ~RecordingMode(){
            RmlRenderer::setCommandRecordingForTesting(previous);
        }
    };

    struct FixedEffectTime {
        explicit FixedEffectTime(double t){ EffectClock::setFixedTime(t); }
        ~FixedEffectTime(){ EffectClock::setFixedTime(std::nullopt); }
    };

    std::string firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width){
        if (a.size() != b.size())
            return "sizes " + std::to_string(a.size()) + " and " + std::to_string(b.size());
        for (size_t i = 0; i < a.size(); i++){
            if (a[i] != b[i]){
                const size_t pixel = i / 4;
                return "first difference at (" + std::to_string(pixel % (size_t)width) + ", " + std::to_string(pixel / (size_t)width) +
                       "), channel " + std::to_string(i % 4) + ": " + std::to_string(a[i]) + " and " + std::to_string(b[i]);
            }
        }
        return "identical";
    }

    bool warmUp(std::initializer_list<const char*> names){
        for (const char* name : names){
            const EffectManifest* m = EffectRegistry::get().find(name);
            if (!m || !warmUpEffect(*m)){
                ADD_FAILURE() << name << " does not compile";
                return false;
            }
        }
        return true;
    }

    UIVideoElement* videoOf(EffectScene& scene, const char* id){
        return rmlui_dynamic_cast<UIVideoElement*>(scene.element(id));
    }

    void seekAll(EffectScene& scene, std::initializer_list<const char*> ids, double seconds){
        for (const char* id : ids){
            UIVideoElement* v = videoOf(scene, id);
            ASSERT_NE(v, nullptr) << id;
            v->setCurrentTime(seconds);
        }
    }

    // the test card's frame index drawn at (x, y) at half its size (the pages' 160 x 90 videos)
    int halfCardIndex(const std::vector<uint8_t>& rgba, int width, int x, int y){
        return cardIndexOfRgba(rgba.data(), width * 4, x, y, 0.5);
    }

    bool isBackground(const std::vector<uint8_t>& rgba, int width, int x, int y){
        const uint8_t* p = &rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
        return p[0] == BACKGROUND[0] && p[1] == BACKGROUND[1] && p[2] == BACKGROUND[2];
    }

    // pixels of an area that are not the background, and of those how many are pure white
    struct Ink {
        size_t painted = 0;
        size_t white = 0;
    };

    Ink inkIn(const TestImage& image, int x0, int y0, int x1, int y1){
        Ink ink;
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const uint8_t* p = &image.rgba[((size_t)y * (size_t)image.width + (size_t)x) * 4];
                if (std::abs(p[0] - BACKGROUND[0]) + std::abs(p[1] - BACKGROUND[1]) + std::abs(p[2] - BACKGROUND[2]) <= 12)
                    continue;
                ink.painted++;
                if (p[0] == 255 && p[1] == 255 && p[2] == 255)
                    ink.white++;
            }
        }
        return ink;
    }

    void expectGolden(const std::string& name, const TestImage& image){
        const CompareResult result = compareWithGolden(name, image.rgba, image.width, image.height);
        EXPECT_TRUE(result.passed) << name << ": " << result.message;
    }

    const char* const kRecordedVideos[] = {"plain", "fx", "trail", "under"};

    struct VideoShot {
        TestImage image;
        size_t commands = 0;
        bool trailSimulated = false;
    };

    // fixtures/p6/video-recorded.html at frame 21 of the card, recording on or off, in a scene of its own
    VideoShot shootRecordedPage(bool recording){
        RecordingMode mode(recording);
        VideoShot shot;
        EffectScene scene(360, 240);
        if (!scene.valid())
            return shot;
        std::string error;
        EXPECT_TRUE(scene.loadFile("fixtures/p6/video-recorded.html", &error)) << error;
        FixtureRunner& runner = scene.fixture();
        runner.frameAt(0.0);
        runner.frameAt(0.0);
        for (const char* id : kRecordedVideos){
            UIVideoElement* v = videoOf(scene, id);
            EXPECT_NE(v, nullptr) << id;
            if (v)
                v->setCurrentTime(cardTime(21));
        }
        runner.frameAt(runner.time(), true);
        EXPECT_TRUE(scene.capture(shot.image));
        shot.commands = runner.renderer() ? runner.renderer()->getLastFrameCommandCount() : 0;
        shot.trailSimulated = EffectRuntime::get().simulationInfo(scene.element("trail"), "time-trail", 0).exists;
        EffectRuntime::get().clearTestInput();
        return shot;
    }

}

TEST(Phase6Cross, VideoFramesRecordedMatchImmediateOnes){
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    TestSystem::get().clearLogs();
    ASSERT_TRUE(warmUp({"chromatic-aberration", "time-trail", "frosted-glass"}));
    FixedEffectTime clock(1.0);
    const VideoShot recorded = shootRecordedPage(true);
    const VideoShot immediate = shootRecordedPage(false);
    ASSERT_FALSE(recorded.image.empty());
    ASSERT_FALSE(immediate.image.empty());
    EXPECT_GT(recorded.commands, 0u) << "the frame was recorded";
    EXPECT_EQ(immediate.commands, 0u) << "the frame was drawn at once";
    EXPECT_TRUE(recorded.trailSimulated) << "time-trail simulates on the video";
    EXPECT_TRUE(immediate.trailSimulated);
    // the plain video shows the seeked frame in both
    EXPECT_EQ(halfCardIndex(recorded.image.rgba, recorded.image.width, 10, 10), 21);
    EXPECT_EQ(halfCardIndex(immediate.image.rgba, immediate.image.width, 10, 10), 21);
    ASSERT_EQ(recorded.image.width, immediate.image.width);
    ASSERT_EQ(recorded.image.height, immediate.image.height);
    EXPECT_TRUE(recorded.image.rgba == immediate.image.rgba) << firstDifference(recorded.image.rgba, immediate.image.rgba, recorded.image.width);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(Phase6Cross, AVideoRemovedWhileItsFrameIsRecorded){
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    ASSERT_TRUE(warmUp({"chromatic-aberration", "time-trail", "frosted-glass"}));
    FixedEffectTime clock(1.0);
    std::vector<uint8_t> layers[2];
    for (int recording = 1; recording >= 0; recording--){
        TestSystem::get().clearLogs();
        RecordingMode mode(recording != 0);
        EffectScene scene(360, 240, 1);
        ASSERT_TRUE(scene.valid());
        std::string error;
        ASSERT_TRUE(scene.loadFile("fixtures/p6/video-recorded.html", &error)) << error;
        FixtureRunner& runner = scene.fixture();
        runner.frameAt(0.0);
        runner.frameAt(0.0);
        seekAll(scene, {"plain", "fx", "trail", "under"}, cardTime(10));
        TestImage before;
        ASSERT_TRUE(scene.capture(before));
        ASSERT_EQ(halfCardIndex(before.rgba, before.width, 10, 10), 10);
        RmlRenderer* renderer = runner.renderer();
        ASSERT_NE(renderer, nullptr);
        const size_t textures = renderer->getLiveTextureCount();
        const int elements = UIVideoElement::getLiveCount();
        const int streams = VideoStream::getLiveCount();
        ASSERT_EQ(elements, 4);

        // one frame by hand, as HtmlUIContext draws it; #under goes between its Render and endFrame
        RmlRenderTarget target;
        ASSERT_TRUE(target.ensure(360, 240, 1));
        beginTestFrame();
        scene.context()->Update();
        renderer->runVideoConversions();
        ASSERT_TRUE(renderer->beginFrame(target, Rml::Vector2i(360, 240)));
        scene.context()->Render();
        {
            Rml::Element* under = scene.element("under");
            ASSERT_NE(under, nullptr);
            Rml::ElementPtr removed = under->GetParentNode()->RemoveChild(under);
            // destroyed now: its texture (and its planes) and its geometry go back to the renderer mid-frame
            removed.reset();
        }
        EXPECT_EQ(UIVideoElement::getLiveCount(), elements - 1);
        EXPECT_TRUE(renderer->endFrame());
        SystemRender::commit();
        EXPECT_EQ(renderer->getLastFrameCommandCount() > 0, recording != 0);
        ASSERT_TRUE(readTexture(target.getComposited()->getColorTexture(), 360, 240, layers[recording]));
        target.destroy();
        // the removed video was drawn by that frame (its picture's top-left corner, outside the glass)
        EXPECT_FALSE(isBackground(layers[recording], 360, 200, 136)) << "the frame drew the video it recorded";

        // the next frames: gone with its stream and its texture
        TestImage after;
        ASSERT_TRUE(scene.capture(after));
        EXPECT_TRUE(isBackground(after.rgba, after.width, 200, 136));
        EXPECT_EQ(halfCardIndex(after.rgba, after.width, 10, 10), 10);
        EXPECT_EQ(VideoStream::getLiveCount(), streams - 1);
        EXPECT_EQ(renderer->getLiveTextureCount(), textures - 1);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
    }
    EXPECT_TRUE(layers[1] == layers[0]) << firstDifference(layers[1], layers[0], 360);
}

TEST(Phase6Cross, StatefulEffectsOnAVideo){
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    TestSystem::get().clearLogs();
    ASSERT_TRUE(warmUp({"time-trail", "data-mosh"}));
    const char* const videos[] = {"plain", "trail", "mosh", "both"};

    // pinned: the state recomputed for the effect time from a replay over the frame the videos show
    TestImage pinned, again, reloaded;
    for (int load = 0; load < 2; load++){
        FixedEffectTime clock(1.5);
        EffectScene scene(360, 240);
        ASSERT_TRUE(scene.valid());
        std::string error;
        ASSERT_TRUE(scene.loadFile("fixtures/p6/video-stateful.html", &error)) << error;
        scene.fixture().frameAt(0.0);
        scene.fixture().frameAt(0.0);
        for (const char* id : videos){
            UIVideoElement* v = videoOf(scene, id);
            ASSERT_NE(v, nullptr) << id;
            v->setCurrentTime(cardTime(33));
        }
        scene.fixture().frameAt(0.0, true);
        TestImage& image = load == 0 ? pinned : reloaded;
        ASSERT_TRUE(scene.capture(image));
        EXPECT_EQ(halfCardIndex(image.rgba, image.width, 10, 10), 33);
        for (const auto& [id, effect] : {std::pair<const char*, const char*>{"trail", "time-trail"}, {"mosh", "data-mosh"}, {"both", "time-trail"}}){
            const EffectSimulationInfo info = EffectRuntime::get().simulationInfo(scene.element(id), effect, 0);
            EXPECT_TRUE(info.exists) << id;
            EXPECT_EQ(info.steps, 90) << id << ": 1.5 s of 1/60 s steps";
        }
        if (load == 0){
            ASSERT_TRUE(scene.capture(again));
            EXPECT_EQ(maxDifference(pinned, again), 0) << "the same pinned frame";
        }
    }
    EXPECT_EQ(maxDifference(pinned, reloaded), 0) << "the pinned state depends on what ran before";
    expectGolden("video/stateful.pinned", pinned);

    // live: the effect clock follows the UI clock, the videos play; each drawn frame steps the simulations once
    {
        EffectScene scene(360, 240);
        ASSERT_TRUE(scene.valid());
        std::string error;
        ASSERT_TRUE(scene.loadFile("fixtures/p6/video-stateful.html", &error)) << error;
        FixtureRunner& runner = scene.fixture();
        for (int frame = 0; frame <= 30; frame++)
            runner.frameAt(frame / 60.0, true);
        TestImage live;
        ASSERT_TRUE(scene.capture(live));
        for (const char* id : videos){
            UIVideoElement* v = videoOf(scene, id);
            ASSERT_NE(v, nullptr) << id;
            EXPECT_FALSE(v->isPaused()) << id << " autoplays";
            EXPECT_EQ(v->getShownFrameIndex(), 15) << id << ": 0.5 s at 30 fps";
        }
        EXPECT_EQ(halfCardIndex(live.rgba, live.width, 10, 10), 15);
        const EffectSimulationInfo trail = EffectRuntime::get().simulationInfo(scene.element("trail"), "time-trail", 0);
        EXPECT_TRUE(trail.exists);
        // a step a drawn 1/60 s frame (the first drawn frame starts it; the UI's own shaders may drop the first frames)
        EXPECT_GE(trail.steps, 20);
        EXPECT_LE(trail.steps, 30);
        EXPECT_FALSE(trail.paused);
        // the trail of the moving square: the trail video differs from the plain one at the same frame
        size_t differing = 0;
        for (int y = 0; y < 90; y++){
            for (int x = 0; x < 160; x++){
                const uint8_t* a = &live.rgba[((size_t)(10 + y) * live.width + (size_t)(10 + x)) * 4];
                const uint8_t* b = &live.rgba[((size_t)(10 + y) * live.width + (size_t)(190 + x)) * 4];
                if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 24)
                    differing++;
            }
        }
        EXPECT_GT(differing, 200u) << "time-trail sheds a trail behind the card's moving square";
        expectGolden("video/stateful.live", live);
    }
    EffectRuntime::get().clearTestInput();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(Phase6Cross, ClampedJustifiedTextInsideEffects){
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    TestSystem::get().clearLogs();
    ASSERT_TRUE(warmUp({"stripes", "chromatic-aberration", "time-trail", "holographic", "frosted-glass"}));
    FixedEffectTime clock(1.0);
    EffectScene scene(480, 320);
    std::string error;
    ASSERT_TRUE(scene.loadFile("fixtures/p6/clamp-effects.html", &error)) << error;
    TestImage image;
    ASSERT_TRUE(scene.capture(image));

    // two clamped lines each (Ahem 10px on 12px lines, 20px on 24px), plus the padding; the effects change no box
    struct Expected { const char* id; float height; };
    for (const Expected& e : {Expected{"paint", 24.f + 12.f}, Expected{"filter", 24.f}, Expected{"trail", 24.f}, Expected{"fill", 48.f},
                              Expected{"glass", 24.f + 8.f}}){
        const Rml::Rectanglef box = scene.box(e.id);
        const Rml::Rectanglef twin = scene.box(std::string(e.id) + "-twin");
        EXPECT_FLOAT_EQ(box.Height(), e.height) << e.id;
        EXPECT_FLOAT_EQ(box.Width(), twin.Width()) << e.id;
        EXPECT_FLOAT_EQ(box.Height(), twin.Height()) << e.id;
        Rml::Element* element = scene.element(e.id);
        ASSERT_NE(element, nullptr) << e.id;
        EXPECT_GT(element->GetScrollHeight(), element->GetClientHeight() + 1.f) << e.id << " is clamped: lines after the clamp point";
    }
    EXPECT_TRUE(EffectRuntime::get().simulationInfo(scene.element("trail"), "time-trail", 0).exists);

    // gradient text: the second line's "filled" and the ellipsis after it (Ahem 20px: x 130-190) are drawn by the
    // effect's fill, never in the white the text is drawn in for the fill
    const Ink word = inkIn(image, 12, 96, 128, 116);
    const Ink ellipsis = inkIn(image, 132, 96, 188, 116);
    EXPECT_GT(word.painted, 1500u);
    EXPECT_GT(ellipsis.painted, 600u) << "the ellipsis of the clamp line is drawn";
    EXPECT_EQ(word.white + ellipsis.white, 0u) << "the text and its ellipsis take the effect's fill";
    // and the twin's ellipsis, in the block's colour, at the same place 160px lower
    EXPECT_GT(inkIn(image, 132, 256, 188, 276).painted, 600u);

    expectGolden("p6/clamp_effects", image);
    EffectRuntime::get().clearTestInput();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(Phase6Cross, ClampedJustifiedTextInATextureDocument){
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    TestSystem::get().clearLogs();
    ASSERT_TRUE(warmUp({"stripes"}));
    FixedEffectTime clock(1.0);
    HtmlUIClock::setFixedTime(0.0);
    struct Expected { const char* id; float height; };
    const Expected heights[] = {{"a", 28.f}, {"b", 42.f}, {"c", 28.f}, {"p", 56.f}};

    // the page as a texture-mode document (its own context and target, 320 x 200 texels for 320 x 200 px)
    TestImage texture;
    {
        SceneFixture fixture;
        UIDocument document = fixture.addDocument("p6/clamp-texture.html");
        UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
        component.renderMode = UIDocumentRenderMode::TEXTURE;
        component.textureWidth = 320;
        component.textureHeight = 200;
        component.pixelRatio = 1.0f;
        ASSERT_TRUE(fixture.waitUntilLoaded());
        GpuEnvironment::drawFrames(3);
        UIDocumentSystem& system = *fixture.getScene().getSystem<UIDocumentSystem>();
        Rml::ElementDocument* rml = system.getDocument(document.getEntity());
        ASSERT_NE(rml, nullptr);
        for (const Expected& e : heights){
            Rml::Element* element = rml->GetElementById(e.id);
            ASSERT_NE(element, nullptr) << e.id;
            EXPECT_FLOAT_EQ(element->GetBox().GetSize(Rml::BoxArea::Border).y, e.height) << e.id << " in the texture document";
        }
        Framebuffer* target = document.getTargetTexture();
        ASSERT_NE(target, nullptr);
        ASSERT_EQ(target->getWidth(), 320u);
        ASSERT_EQ(target->getHeight(), 200u);
        texture.width = 320;
        texture.height = 200;
        ASSERT_TRUE(readTexture(target->getRender().getColorTexture(), 320, 200, texture.rgba));
    }

    // the same page as a screen document of the same size
    TestImage screen;
    {
        EffectScene scene(320, 200);
        ASSERT_TRUE(scene.valid());
        std::string error;
        ASSERT_TRUE(scene.loadFile("fixtures/p6/clamp-texture.html", &error)) << error;
        ASSERT_TRUE(scene.capture(screen));
        for (const Expected& e : heights)
            EXPECT_FLOAT_EQ(scene.box(e.id).Height(), e.height) << e.id << " in the screen document";
    }
    HtmlUIClock::setFixedTime(std::nullopt);

    // an opaque page: the texture's straight alpha is the screen layer's premultiplied colour
    EXPECT_LE(maxDifference(texture, screen), 1) << firstDifference(texture.rgba, screen.rgba, 320);
    expectGolden("p6/clamp_texture", texture);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
