// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The <video> API in Lua (docs/html-ui/phase-6-video.md §3.9): HTMLMediaElement's properties and
// methods on UIElement, the constants on its class table, `error` as nil or a table, events with
// addEventListener in order, and the error once on an element that is not a video. The document is
// an HTML page loaded by a UIDocument of a scene (compat, UIDocumentSystem), on the fixture clock.

#include "TestScriptCommon.h"

#include "HtmlUIClock.h"
#include "render/SystemRender.h"
#include "media/UIVideoElement.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class VideoLuaTest : public ScriptTest {
    protected:
        const char* documentFile() const override{
            return "video/pages/lua.html";
        }

        void SetUp() override{
            HtmlUIClock::reset();
            HtmlUIClock::setFixedTime(0.0);
            ScriptTest::SetUp();
        }

        void TearDown() override{
            ScriptTest::TearDown();
            HtmlUIClock::reset();
        }

        // one engine frame at `seconds`: the documents' update, then the end of the frame (the
        // events of a load wait for a later frame, as in the engine)
        void at(double seconds){
            HtmlUIClock::setFixedTime(seconds);
            update();
            SystemRender::commit();
        }
    };

}

TEST_F(VideoLuaTest, PropertiesAndMethods){
    at(0.0);
    at(0.0);
    EXPECT_LUA(R"(
        el = doc:getElementById("v")
        assert(el.readyState == UIElement.HAVE_ENOUGH_DATA, "readyState " .. tostring(el.readyState))
        assert(el.networkState == UIElement.NETWORK_IDLE)
        assert(el.videoWidth == 320 and el.videoHeight == 180)
        assert(math.abs(el.duration - 2.0) < 0.002)
        assert(el.paused == true and el.ended == false and el.seeking == false)
        assert(el.currentSrc:find("testcard%-noaudio%.webm"), el.currentSrc)
        assert(el.currentTime == 0)
        assert(el.error == nil)
        assert(el:canPlayType("video/mp4") == "")
        assert(el:canPlayType("video/webm") == "maybe")
        assert(el:canPlayType('video/webm; codecs="av01.0.05M.08, opus"') == "probably")

        el.currentTime = 1.5
        assert(el.currentTime == 1.5)
        el.volume = 0.5
        assert(el.volume == 0.5)
        el.volume = 2          -- outside [0, 1]: a warning, unchanged
        assert(el.volume == 0.5)
        el.muted = true
        assert(el.muted and not el.defaultMuted)
        el.defaultMuted = true
        assert(el:hasAttribute("muted"))
        el.loop = true
        assert(el.loop and el:hasAttribute("loop"))
        el.loop = false
        assert(not el:hasAttribute("loop"))
        el.playbackRate = 2
        assert(el.playbackRate == 2)
        el.playbackRate = -1   -- refused
        assert(el.playbackRate == 2)
        el.defaultPlaybackRate = 0.5
        assert(el.defaultPlaybackRate == 0.5)
        el.preload = "metadata"
        assert(el.preload == "metadata")
        el.autoplay = true
        assert(el.autoplay and el:hasAttribute("autoplay"))
        el.poster = "../poster.png"
        assert(el.poster == "../poster.png")
        assert(el.preservesPitch == false)
        assert(UIElement.MEDIA_ERR_SRC_NOT_SUPPORTED == 4 and UIElement.HAVE_NOTHING == 0 and UIElement.NETWORK_NO_SOURCE == 3)
    )");
}

TEST_F(VideoLuaTest, EventsInOrder){
    at(0.0);
    at(0.0);
    EXPECT_LUA(R"(
        el = doc:getElementById("v")
        log = {}
        for _, t in ipairs({"play", "playing", "pause", "seeking", "seeked", "ended", "ratechange", "volumechange"}) do
            el:addEventListener(t, function(e) log[#log + 1] = e.type end)
        end
        el:play()
    )");
    at(0.5);
    EXPECT_LUA(R"(el:pause())");
    at(0.6);
    EXPECT_LUA(R"(el.currentTime = 1.9; el:play())");
    at(0.7);
    at(1.0);
    EXPECT_EQ(luaString("table.concat(log, ',')"), "play,playing,pause,seeking,seeked,play,playing,pause,ended");
    EXPECT_LUA(R"(
        assert(el.ended and el.paused)
        assert(math.abs(el.currentTime - el.duration) < 1e-9)
        assert(el:isPlaying() == false)
    )");
}

TEST_F(VideoLuaTest, ErrorTableAndListener){
    at(0.0);
    EXPECT_LUA(R"(
        bad = doc:getElementById("missing")
        message = nil
        bad:addEventListener("error", function(e) message = e.detail.message end)
    )");
    at(0.0);
    at(0.0);
    EXPECT_LUA(R"(
        assert(bad.error ~= nil)
        assert(bad.error.code == UIElement.MEDIA_ERR_SRC_NOT_SUPPORTED)
        assert(bad.error.message:find("could not be read"), bad.error.message)
        assert(bad.networkState == UIElement.NETWORK_NO_SOURCE)
    )");
    // the listener was added after the load: the load's events still reach it (a later frame)
    EXPECT_NE(luaString("message"), "nil");
}

TEST_F(VideoLuaTest, NotAVideoLogsOnce){
    at(0.0);
    TestSystem::get().clearLogs();
    EXPECT_LUA(R"(
        d = doc:getElementById("d")
        for i = 1, 3 do
            assert(d.currentTime == 0)
            d.volume = 0.5
        end
        assert(d.paused == true)
    )");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "UIElement::getCurrentTime: <div> is not a <video> element"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "UIElement::setVolume: <div> is not a <video> element"), 1u);
    EXPECT_LUA(R"(d = nil)");
}
