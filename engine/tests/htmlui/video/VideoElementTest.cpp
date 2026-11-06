// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The <video> element on the fixture clock (docs/html-ui/phase-6-video.md §5.1): HTML pages of
// fixtures/video/pages loaded through the compat layer by the FixtureRunner (the UI clock fixed,
// frame-exact mode), drawn on an RmlRenderer (the GPU Yuv conversion) and read back: load events in
// Chromium's order, the frame at fixed times, pause/resume, loops, the end, seeks, the <source>
// choice, errors, the poster, intrinsic sizes, four videos at once, the media state API, A/V sync
// through the element, real-time mode (never blocking), and 100 load/unload cycles.

#include "VideoTestCommon.h"

#include "FixtureRunner.h"
#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "Log.h"
#include "HtmlUITexture.h"
#include "RmlRenderer.h"
#include "media/UIVideoControls.h"
#include "media/UIVideoElement.h"
#include "media/VideoDecoder.h"
#include "media/VideoStream.h"
#include "media/VideoSourceFactory.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* const mediaEvents[] = {"loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "canplaythrough", "play",
                                       "playing", "pause", "waiting", "seeking", "seeked", "timeupdate", "ended", "ratechange",
                                       "volumechange", "emptied", "abort", "error", "suspend"};

    class EventLog : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            types.push_back(event.GetType());
            const Rml::String message = event.GetParameter<Rml::String>("message", "");
            if (!message.empty())
                messages.push_back(message);
        }
        size_t count(const std::string& type) const{
            return (size_t)std::count(types.begin(), types.end(), type);
        }
        // the log without timeupdate (its cadence is tested on its own)
        std::vector<std::string> withoutTimeupdate() const{
            std::vector<std::string> out;
            for (const std::string& type : types){
                if (type != "timeupdate")
                    out.push_back(type);
            }
            return out;
        }
        void clear(){
            types.clear();
            messages.clear();
        }
        std::vector<std::string> types;
        std::vector<std::string> messages;
    };

    class VideoElementTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            TestSystem::get().clearLogs();
            runner = std::make_unique<FixtureRunner>(640, 400, 1);
            if (!runner->valid())
                GTEST_SKIP() << "no HtmlUI or GPU";
        }

        void TearDown() override{
            UIVideoElement::setAudioConsumerForTesting(false);
            VideoStream::setDecodeDelayForTesting(0);
            runner.reset();
            VideoSourceFactory::setSourceOverrideForTesting(nullptr);
            HtmlUIClock::reset();
        }

        void useInlineDecoder(){
            VideoSourceFactory::setSourceOverrideForTesting([this](const std::string& path, const VideoSourceFactory::Options& options){
                auto source = std::make_unique<VideoStream>(path, false, options.audio);
                inlineDecoder = source.get();
                return source;
            });
        }

        void fillAudio(const std::shared_ptr<VideoAudioRing>& ring){
            ASSERT_NE(inlineDecoder, nullptr);
            while (ring->getBufferedFrames() < 480 && inlineDecoder->pump(1)){}
        }

        VideoStream* inlineDecoder = nullptr;

        // a page of fixtures/video/pages; listeners on every media event of `ids`
        void load(const std::string& page, std::initializer_list<const char*> ids = {"v"}){
            std::string error;
            ASSERT_TRUE(runner->load("fixtures/video/pages/" + page, &error)) << error;
            for (const char* id : ids){
                Rml::Element* element = runner->document()->GetElementById(id);
                ASSERT_NE(element, nullptr) << id;
                for (const char* type : mediaEvents)
                    element->AddEventListener(type, &logs[id]);
            }
        }

        UIVideoElement* video(const char* id = "v"){
            return rmlui_dynamic_cast<UIVideoElement*>(runner->document()->GetElementById(id));
        }

        // frames until `seconds` in 1/60 s steps (from the runner's clock)
        void playTo(double seconds, bool draw = false){
            double t = runner->time();
            while (t + 1e-9 < seconds){
                t = std::min(seconds, t + 1.0 / 60.0);
                runner->frameAt(t, draw);
            }
            runner->frameAt(seconds, draw);
        }

        // the frame index the drawn page shows at (x, y) for a test card at `scale`
        int drawnIndex(int x = 0, int y = 0, double scale = 1.0){
            FixtureRunner::Capture capture;
            EXPECT_TRUE(runner->capture(capture, true));
            if (capture.image.empty())
                return -1;
            return cardIndexOfRgba(capture.image.rgba.data(), capture.image.width * 4, x, y, scale);
        }

        std::unique_ptr<FixtureRunner> runner;
        std::map<std::string, EventLog> logs;
    };

    using Events = std::vector<std::string>;

}

TEST_F(VideoElementTest, LoadEventsInChromiumsOrder){
    load("single.html");
    UIVideoElement* v = video();
    ASSERT_NE(v, nullptr);
    // frame-exact: the metadata and the first frame are there from the first layout
    EXPECT_EQ(v->getVideoWidth(), CARD_WIDTH);
    EXPECT_EQ(v->getVideoHeight(), CARD_HEIGHT);
    EXPECT_NEAR(v->getDuration(), 2.0, 0.002);
    EXPECT_TRUE(v->isPaused());
    EXPECT_TRUE(logs["v"].types.empty()) << "load events wait for a later frame";
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    EXPECT_EQ(logs["v"].types, (Events{"loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "canplaythrough"}));
    EXPECT_EQ(v->getReadyState(), UIVideoElement::HAVE_ENOUGH_DATA);
    EXPECT_EQ(v->getNetworkState(), UIVideoElement::NETWORK_IDLE);
    EXPECT_TRUE(v->getCurrentSrc().find("testcard-noaudio.webm") != std::string::npos) << v->getCurrentSrc();
    EXPECT_FALSE(v->getError());
    // the first frame is drawn while paused at 0
    runner->frameAt(0.0, true);
    EXPECT_EQ(drawnIndex(), 0);
    EXPECT_GE(v->getStats().conversionsQueued, 1u);
    EXPECT_EQ(v->getStats().cpuConversions, 0u);
}

TEST_F(VideoElementTest, AutoplayPlaysOnTheUIClock){
    UIVideoElement::setAudioConsumerForTesting(true);
    load("autoplay.html");
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    EXPECT_EQ(logs["v"].withoutTimeupdate(), (Events{"loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay",
                                                    "canplaythrough", "play", "playing"}));
    UIVideoElement* v = video();
    EXPECT_FALSE(v->isPaused());
    playTo(0.5, false);
    EXPECT_NEAR(v->getCurrentTime(), 0.5, 1e-9);
    runner->frameAt(0.5, true);
    EXPECT_EQ(drawnIndex(), 15);
    playTo(1.0);
    runner->frameAt(1.0, true);
    EXPECT_EQ(drawnIndex(), 30);
    // timeupdate at most every 250 ms of UI time while playing: four in the first second
    EXPECT_GE(logs["v"].count("timeupdate"), 3u);
    EXPECT_LE(logs["v"].count("timeupdate"), 5u);
}

TEST_F(VideoElementTest, FrameExactAtEveryFrameTime){
    load("single.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    for (int k : {0, 1, 2, 29, 30, 31, 44, 59, 13, 0}){
        v->setCurrentTime(cardTime(k));
        runner->frameAt(runner->time(), true);
        EXPECT_EQ(v->getShownFrameIndex(), k);
        EXPECT_EQ(drawnIndex(), k) << "after a seek to frame " << k;
    }
    // between two frames: the earlier one
    v->setCurrentTime(cardTime(20) + 0.5 / CARD_FPS);
    runner->frameAt(runner->time(), true);
    EXPECT_EQ(drawnIndex(), 20);
    // fastSeek goes to the keyframe at or before
    v->fastSeek(cardTime(50));
    EXPECT_NEAR(v->getCurrentTime(), 1.0, 1e-9);
}

TEST_F(VideoElementTest, SeekEvents){
    load("single.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    logs["v"].clear();
    v->setCurrentTime(1.5);
    EXPECT_NEAR(v->getCurrentTime(), 1.5, 1e-9);
    runner->frameAt(0.0);
    EXPECT_EQ(logs["v"].types, (Events{"seeking", "timeupdate", "seeked"}));
    EXPECT_FALSE(v->isSeeking());
    // out of range: clamped to [0, duration]
    v->setCurrentTime(-3.0);
    EXPECT_EQ(v->getCurrentTime(), 0.0);
    v->setCurrentTime(99.0);
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9);
}

TEST_F(VideoElementTest, PauseAndResume){
    load("single.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    logs["v"].clear();
    v->play();
    playTo(0.5);
    v->pause();
    EXPECT_NEAR(v->getCurrentTime(), 0.5, 1e-9);
    playTo(1.25);
    EXPECT_NEAR(v->getCurrentTime(), 0.5, 1e-9) << "time stands while paused";
    runner->frameAt(1.25, true);
    EXPECT_EQ(drawnIndex(), 15);
    v->play();
    playTo(1.5);
    EXPECT_NEAR(v->getCurrentTime(), 0.75, 1e-9);
    const Events events = logs["v"].withoutTimeupdate();
    EXPECT_EQ(events, (Events{"play", "playing", "pause", "play", "playing"}));
}

TEST_F(VideoElementTest, LoopWrapsSeamlessly){
    load("loop.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    playTo(1.9);
    logs["v"].clear();
    playTo(2.5, false);
    EXPECT_NEAR(v->getCurrentTime(), 0.5, 1e-9);
    EXPECT_FALSE(v->isEnded());
    EXPECT_FALSE(v->isPaused());
    const Events events = logs["v"].withoutTimeupdate();
    EXPECT_EQ(events, (Events{"seeking", "seeked"}));
    runner->frameAt(2.5, true);
    EXPECT_EQ(drawnIndex(), 15);
    // the stream went on from frame 0 without a seek of its own
    EXPECT_EQ(v->getStreamStats().seeks, 0u);
    // several loops
    playTo(7.0);
    EXPECT_NEAR(v->getCurrentTime(), 1.0, 1e-9);
    runner->frameAt(7.0, true);
    EXPECT_EQ(drawnIndex(), 30);
}

TEST_F(VideoElementTest, EndOfMedia){
    load("single.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    v->play();
    playTo(1.9);
    logs["v"].clear();
    playTo(2.5);
    EXPECT_TRUE(v->isEnded());
    EXPECT_TRUE(v->isPaused());
    EXPECT_NEAR(v->getCurrentTime(), v->getDuration(), 1e-9);
    EXPECT_EQ(logs["v"].types.back(), "ended");
    const Events tail(logs["v"].types.end() - 3, logs["v"].types.end());
    EXPECT_EQ(tail, (Events{"timeupdate", "pause", "ended"}));
    runner->frameAt(2.5, true);
    EXPECT_EQ(drawnIndex(), 59);
    // play() after the end starts again from 0
    logs["v"].clear();
    v->play();
    runner->frameAt(2.5);
    EXPECT_NEAR(v->getCurrentTime(), 0.0, 1e-9);
    EXPECT_FALSE(v->isEnded());
    EXPECT_EQ(logs["v"].withoutTimeupdate(), (Events{"seeking", "play", "seeked", "playing"}));
}

TEST_F(VideoElementTest, SourceChoice){
    load("sources.html", {"typed", "untyped", "none"});
    Rml::Element* bad = runner->document()->GetElementById("bad");
    // (the fixture's logs outlive the document: a listener must not dangle at its teardown)
    EventLog& sourceLog = logs["bad"];
    bad->AddEventListener("error", &sourceLog);
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    // typed sources the engine does not play are skipped without being opened
    EXPECT_TRUE(video("typed")->getCurrentSrc().find("testcard-noaudio.webm") != std::string::npos) << video("typed")->getCurrentSrc();
    EXPECT_EQ(logs["typed"].count("error"), 0u);
    // an untyped VP9 source fails at open: an error at that <source>, then the next one
    EXPECT_TRUE(video("untyped")->getCurrentSrc().find("testcard-noaudio.webm") != std::string::npos);
    EXPECT_EQ(sourceLog.count("error"), 1u);
    EXPECT_EQ(logs["untyped"].count("error"), 0u) << "the error goes to the <source>, not the video";
    EXPECT_EQ(video("untyped")->getReadyState(), UIVideoElement::HAVE_ENOUGH_DATA);
    // nothing playable: no source, no error on the video (HTML), a warning in the Console
    EXPECT_EQ(video("none")->getNetworkState(), UIVideoElement::NETWORK_NO_SOURCE);
    EXPECT_FALSE(video("none")->getError());
    EXPECT_GT(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-HTML-0027"), 0u) << "the run-time diagnostic names its code";
}

TEST_F(VideoElementTest, Errors){
    load("errors.html", {"missing", "vp9", "mp4", "corrupt"});
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    for (const char* id : {"missing", "vp9", "mp4"}){
        UIVideoElement* v = video(id);
        EXPECT_EQ(v->getError().code, MediaErrorCode::SRC_NOT_SUPPORTED) << id;
        EXPECT_EQ(v->getNetworkState(), UIVideoElement::NETWORK_NO_SOURCE) << id;
        EXPECT_EQ(v->getReadyState(), UIVideoElement::HAVE_NOTHING) << id;
        EXPECT_EQ(logs[id].withoutTimeupdate(), (Events{"loadstart", "error"})) << id;
        EXPECT_FALSE(logs[id].messages.empty()) << id;
    }
    EXPECT_NE(video("vp9")->getError().message.find("V_VP9"), std::string::npos) << video("vp9")->getError().message;
    // damaged data at frame 40: a decode error there, the last good frame stays
    playTo(1.6);
    UIVideoElement* corrupt = video("corrupt");
    EXPECT_EQ(corrupt->getError().code, MediaErrorCode::DECODE);
    EXPECT_TRUE(corrupt->isPaused());
    EXPECT_EQ(corrupt->getShownFrameIndex(), 39);
    EXPECT_EQ(logs["corrupt"].count("error"), 1u);
}

TEST_F(VideoElementTest, PosterAndPreload){
    load("poster.html", {"p", "q", "r"});
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    UIVideoElement* p = video("p");
    UIVideoElement* q = video("q");
    UIVideoElement* r = video("r");
    // preload none: nothing opened, the poster's size (or 300x150) and the poster on screen
    EXPECT_EQ(p->getStats().streamsCreated, 0u);
    EXPECT_EQ(p->getNetworkState(), UIVideoElement::NETWORK_IDLE);
    EXPECT_EQ(p->getReadyState(), UIVideoElement::HAVE_NOTHING);
    EXPECT_TRUE(p->isShowingPoster());
    EXPECT_EQ(logs["p"].withoutTimeupdate(), (Events{"loadstart", "suspend"}));
    const Rml::Vector2f pSize = p->GetBox().GetSize(Rml::BoxArea::Content);
    EXPECT_EQ(pSize, Rml::Vector2f(320.0f, 180.0f));
    const Rml::Vector2f qSize = q->GetBox().GetSize(Rml::BoxArea::Content);
    EXPECT_EQ(qSize, Rml::Vector2f(300.0f, 150.0f));
    // the poster stays while the first frame is decoded, until playback starts
    EXPECT_TRUE(r->isShowingPoster());
    EXPECT_EQ(r->getReadyState(), UIVideoElement::HAVE_ENOUGH_DATA);
    // play(): the fetch starts, the poster goes once a frame is there
    p->play();
    runner->frameAt(0.0);
    runner->frameAt(0.0, true);
    EXPECT_EQ(p->getStats().streamsCreated, 1u);
    EXPECT_FALSE(p->isShowingPoster());
    EXPECT_FALSE(p->isPaused());
    // a seek hides the poster too
    r->setCurrentTime(1.0);
    runner->frameAt(0.0);
    EXPECT_FALSE(r->isShowingPoster());
}

TEST_F(VideoElementTest, FourVideosAtOnce){
    load("grid.html", {"a", "b", "c", "d"});
    UIVideoElement::setAudioConsumerForTesting(true);
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    video("b")->setCurrentTime(1.5);
    video("d")->setPlaybackRate(2.0);
    video("c")->setCurrentTime(1.75);
    playTo(0.5);
    // each at its own time: a 0.5 s, b paused at 1.5, c looped (1.75 + 0.5 = 0.25), d at twice the rate
    EXPECT_NEAR(video("a")->getCurrentTime(), 0.5, 1e-9);
    EXPECT_NEAR(video("b")->getCurrentTime(), 1.5, 1e-9);
    EXPECT_NEAR(video("c")->getCurrentTime(), 0.25, 1e-9);
    EXPECT_NEAR(video("d")->getCurrentTime(), 1.0, 1e-9);
    runner->frameAt(0.5, true);
    EXPECT_EQ(drawnIndex(0, 0, 0.5), 15);
    EXPECT_EQ(drawnIndex(160, 0, 0.5), 45);
    EXPECT_EQ(drawnIndex(0, 90, 0.5), 7);
    EXPECT_EQ(drawnIndex(160, 90, 0.5), 30);
}

TEST_F(VideoElementTest, VolumeRateAndMuted){
    load("loop.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    logs["v"].clear();
    // the muted attribute is the initial state
    EXPECT_TRUE(v->isMuted());
    EXPECT_TRUE(v->getDefaultMuted());
    v->setMuted(false);
    EXPECT_FALSE(v->isMuted());
    EXPECT_TRUE(v->getDefaultMuted()) << "the property does not change the attribute";
    EXPECT_TRUE(v->setVolume(0.25f));
    EXPECT_FALSE(v->setVolume(1.5f));
    EXPECT_FALSE(v->setVolume(-0.1f));
    EXPECT_EQ(v->getVolume(), 0.25f);
    EXPECT_TRUE(v->setPlaybackRate(0.5));
    EXPECT_FALSE(v->setPlaybackRate(-1.0));
    EXPECT_FALSE(v->setPlaybackRate(32.0));
    EXPECT_TRUE(v->setPlaybackRate(0.0));
    runner->frameAt(0.0);
    EXPECT_EQ(logs["v"].withoutTimeupdate(), (Events{"volumechange", "volumechange", "ratechange", "ratechange"}));
    // the reflected attributes
    EXPECT_TRUE(v->getLoop());
    v->setLoop(false);
    EXPECT_FALSE(v->HasAttribute("loop"));
    EXPECT_EQ(v->getPreload(), "auto");
    v->setPreload("bogus");
    EXPECT_EQ(v->getPreload(), "metadata");
    EXPECT_EQ(UIVideoElement::canPlayType("video/webm; codecs=\"av01.0.05M.08, opus\""), "probably");
    EXPECT_EQ(UIVideoElement::canPlayType("video/mp4"), "");
}

TEST_F(VideoElementTest, SrcChangeLoadsAgain){
    load("single.html");
    UIVideoElement* v = video();
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    logs["v"].clear();
    v->SetAttribute("src", "../testcard-10bit.webm");
    runner->frameAt(0.1);
    runner->frameAt(0.2);
    EXPECT_EQ(logs["v"].withoutTimeupdate(), (Events{"emptied", "loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay",
                                                    "canplaythrough"}));
    EXPECT_TRUE(v->getCurrentSrc().find("testcard-10bit.webm") != std::string::npos);
    v->setCurrentTime(cardTime(33));
    runner->frameAt(0.3, true);
    EXPECT_EQ(drawnIndex(), 33);
}

// A/V sync through the element on the fixture clock: the test reads the audio as the mixer would
// (10 ms blocks, 1024 frames of output latency; a frame's interval is played before the engine's
// update) while the clock steps 1/60 s; the drift stays within one block, and the tone burst of
// second 1 sits at its media position. With a consumer 3 % fast the controller keeps it within the
// dead band plus a block
TEST_F(VideoElementTest, AudioFollowsThePicture){
    useInlineDecoder();
    const int64_t latency = 1024;
    UIVideoElement::setAudioConsumerForTesting(true, latency);
    load("autoplay.html");
    runner->frameAt(0.0);
    UIVideoElement* v = video();
    std::shared_ptr<VideoAudioRing> ring = v->getAudioRing();
    ASSERT_TRUE(ring);
    std::vector<float> block(480 * 2);
    double owed = 0.0, worst = 0.0;
    int64_t burstAt = -1;      // the media frame (48 kHz) at which the second burst is heard
    for (int step = 1; step <= 120; step++){
        const double t = step / 60.0;
        // the mixer has played the frame's interval by the time the engine updates
        owed += 800.0;
        while (owed >= 480.0){
            ASSERT_NO_FATAL_FAILURE(fillAudio(ring));
            const int64_t start = ring->getReadPosition();
            ring->read(block.data(), 480);
            for (int i = 0; i < 480 && burstAt < 0; i++){
                if (start + i > 40000 && std::fabs(block[(size_t)i * 2]) > 0.1f)
                    burstAt = start + i;
            }
            owed -= 480.0;
        }
        runner->frameAt(t);
        if (step > 2){
            // what is heard (read position less the latency) against the picture
            const double heard = (double)(ring->getReadPosition() - latency) / 48000.0;
            worst = std::max(worst, std::fabs(heard - v->getCurrentTime()));
        }
    }
    EXPECT_LE(worst, 0.0101);
    EXPECT_EQ(ring->getPendingCorrection(), 0);
    // the burst sits at its media position (heard when the picture is at 1.0 s)
    ASSERT_GE(burstAt, 0);
    EXPECT_NEAR((double)burstAt / 48000.0, 1.0, 0.002);
    RecordProperty("element_drift_ms", (int)std::lround(worst * 1000.0));
}

// a mixer 3 % faster than the clock: the element drops samples; the drift stays within the dead
// band (15 ms), the 3 % accumulated over the controller's eight-update mean (4 ms) and one block
// (10 ms): 29 ms (no resync)
TEST_F(VideoElementTest, AudioCorrectsAFastMixer){
    useInlineDecoder();
    UIVideoElement::setAudioConsumerForTesting(true, 0);
    load("loop.html");
    UIVideoElement* v = video();
    v->SetAttribute("src", "../testcard.webm");
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    v->setMuted(false);
    v->play();
    runner->frameAt(0.0);
    std::shared_ptr<VideoAudioRing> ring = v->getAudioRing();
    ASSERT_TRUE(ring);
    std::vector<float> block(480 * 2);
    double owed = 0.0, worst = 0.0;
    for (int step = 1; step <= 600; step++){
        owed += 800.0 * 1.03;
        while (owed >= 480.0){
            ASSERT_NO_FATAL_FAILURE(fillAudio(ring));
            ring->read(block.data(), 480);
            owed -= 480.0;
        }
        runner->frameAt(step / 60.0);
        // the loop's media clock: iterations of the duration
        const double media = v->getCurrentTime() + std::floor((step / 60.0) / v->getDuration()) * v->getDuration();
        if (step > 2)
            worst = std::max(worst, std::fabs((double)ring->getReadPosition() / 48000.0 - media));
    }
    EXPECT_LE(worst, 0.029);
    EXPECT_GT(worst, 0.005) << "the consumer was fast: there was something to correct";
    RecordProperty("fast_mixer_drift_ms", (int)std::lround(worst * 1000.0));
}

TEST_F(VideoElementTest, ClockPinnedOrHeldFreezesIt){
    load("loop.html");
    UIVideoElement* v = video();
    playTo(0.5);
    const double at = v->getCurrentTime();
    // the same UI time frame after frame (a held simulation, a pinned clock): nothing moves
    for (int i = 0; i < 10; i++)
        runner->frameAt(0.5);
    EXPECT_EQ(v->getCurrentTime(), at);
    // the game's time scale does not reach the UI clock (a pause menu's video plays)
    Engine::setTimeScale(0.0f);
    playTo(1.0);
    EXPECT_NEAR(v->getCurrentTime(), 1.0, 1e-9);
    Engine::setTimeScale(1.0f);
}

TEST_F(VideoElementTest, RealTimeModeNeverBlocks){
    bool waitRequested = false;
    class DelayedSource : public VideoStream {
    public:
        DelayedSource(const std::string& path, bool audio, bool& waitRequested):
            VideoStream(path, false, audio), waitRequested(waitRequested){}
        FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shown, VideoFrame& frame) override{
            waitRequested = waitRequested || wait;
            if (frameIndexFor(seconds) >= 3) return FrameResult::NOT_READY;
            return VideoStream::acquireFrame(iteration, seconds, wait, shown, frame);
        }
    private:
        bool& waitRequested;
    };
    struct RestoreSource {
        std::unique_ptr<FixtureRunner>& runner;
        ~RestoreSource(){ runner.reset(); VideoSourceFactory::setSourceOverrideForTesting(nullptr); }
    } restore{runner};
    VideoSourceFactory::setSourceOverrideForTesting([&](const std::string& path, const VideoSourceFactory::Options& options){
        return std::make_unique<DelayedSource>(path, options.audio, waitRequested);
    });
    load("loop.html");
    runner->frameAt(0.0);
    runner->frameAt(0.0);
    HtmlUIClock::setFixedTime(std::nullopt);
    waitRequested = false;
    for (int i = 0; i < 40; i++){
        HtmlUIClock::advance(1.0 / 60.0);
        runner->context()->Update();
    }
    UIVideoElement* v = video();
    EXPECT_FALSE(v->isFrameExact());
    EXPECT_FALSE(waitRequested) << "real-time playback must request frames without waiting";
    EXPECT_EQ(v->getShownFrameIndex(), 2);
    EXPECT_GE(logs["v"].count("waiting"), 1u);
    EXPECT_LT(v->getCurrentTime(), 40.0 / 60.0 - 0.1);
}

TEST_F(VideoElementTest, HundredLoadUnloadCyclesLeakNothing){
    RmlRenderer* renderer = runner->renderer();
    ASSERT_NE(renderer, nullptr);
    std::string error;
    // the baseline: a page without video, drawn (the renderer's own textures exist)
    ASSERT_TRUE(runner->load("fixtures/video/pages/none.html", &error)) << error;
    runner->frameAt(0.0, true);
    const size_t textures = renderer->getLiveTextureCount();
    std::vector<uint32_t> buffers, images;
    renderer->getGpuHandles(buffers, images);
    const size_t imageCount = images.size();
    const size_t pictures = Av1Decoder::getLivePictureBytes();
    const int elements = UIVideoElement::getLiveCount();
    const int streams = VideoStream::getLiveCount();
    EXPECT_EQ(elements, 0);
    EXPECT_EQ(streams, 0);
    for (int cycle = 0; cycle < 100; cycle++){
        const char* page = cycle % 3 == 0 ? "grid.html" : (cycle % 3 == 1 ? "autoplay.html" : "poster.html");
        ASSERT_TRUE(runner->load(std::string("fixtures/video/pages/") + page, &error)) << error;
        runner->frameAt(runner->time() + 0.1, true);
        runner->frameAt(runner->time() + 0.1, true);
        ASSERT_TRUE(runner->load("fixtures/video/pages/none.html", &error)) << error;
        runner->frameAt(runner->time() + 0.01, true);
    }
    EXPECT_EQ(UIVideoElement::getLiveCount(), elements);
    EXPECT_EQ(VideoStream::getLiveCount(), streams);
    EXPECT_EQ(Av1Decoder::getLivePictureBytes(), pictures);
    // the poster images stay in RmlUi's file texture cache (as any <img> source): the video
    // textures and their planes are gone
    renderer->getGpuHandles(buffers, images);
    EXPECT_LE(renderer->getLiveTextureCount(), textures + 1);
    EXPECT_LE(images.size(), imageCount + 1);
    EXPECT_EQ(renderer->getVideoConversionCount() > 0, true);
}

// 100 `src` swaps on one element (the load algorithm each time: the old stream, its pictures and
// its conversion jobs released): one stream at the end, the renderer's textures and images as
// after the first load, the decoder's pictures as for that first load (same clip, same frame)
TEST_F(VideoElementTest, HundredSrcSwapsLeakNothing){
    RmlRenderer* renderer = runner->renderer();
    ASSERT_NE(renderer, nullptr);
    std::string error;
    ASSERT_TRUE(runner->load("fixtures/video/pages/autoplay.html", &error)) << error;
    runner->frameAt(0.0, true);
    runner->frameAt(0.1, true);
    const size_t textures = renderer->getLiveTextureCount();
    std::vector<uint32_t> buffers, images;
    renderer->getGpuHandles(buffers, images);
    const size_t imageCount = images.size();
    const size_t pictures = Av1Decoder::getLivePictureBytes();
    EXPECT_EQ(VideoStream::getLiveCount(), 1);
    UIVideoElement* v = video();
    for (int swap = 1; swap <= 100; swap++){
        v->SetAttribute("src", swap % 2 ? "../testcard-noaudio.webm" : "../testcard.webm");
        runner->frameAt(0.1, true);
        runner->frameAt(0.1, true);
        ASSERT_EQ(VideoStream::getLiveCount(), 1) << "swap " << swap;
    }
    // back on the first clip (swap 100), at the same time
    EXPECT_TRUE(v->getCurrentSrc().find("testcard.webm") != std::string::npos) << v->getCurrentSrc();
    EXPECT_EQ(UIVideoElement::getLiveCount(), 1);
    renderer->getGpuHandles(buffers, images);
    EXPECT_EQ(renderer->getLiveTextureCount(), textures);
    EXPECT_EQ(images.size(), imageCount);
    EXPECT_LE(Av1Decoder::getLivePictureBytes(), pictures) << "no picture of an earlier stream is kept";
}
