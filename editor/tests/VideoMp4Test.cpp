// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// MP4 video through the platform decoders (feature video-mp4,
// docs/superpowers/specs/2026-10-31-video-mp4-design.md §9): the ISO-BMFF probe, growing-file
// reads, the Linux FFmpeg backend (skipped with a message where the system has no FFmpeg),
// the source factory's choices and the URL media cache against the in-process HTTP server.
// The clips (engine/tests/media/clips, tools/media/make_test_clips.sh) show their frame number
// as coloured quadrants: base-4 digits of N in the top-left, top-right and bottom-left quarters.

#include "ScriptDataTestSupport.h"
#include "Stream.h"
#include "util/TestHttpServer.h"

#include "media/GrowingFileReader.h"
#include "media/MediaByteSource.h"
#include "media/MediaCache.h"
#include "media/Mp4Probe.h"
#include "media/PlatformVideoSource.h"
#include "media/VideoAudio.h"
#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "media/VideoStream.h"
#include "net/Http.h"
#include "net/HttpBackend.h"
#include "thread/MainThreadQueue.h"
#include "FFmpegBackend.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

using namespace jasperengine;
using jaspertest::TestHttpServer;

namespace {

    namespace fs = std::filesystem;

    std::string clip(const std::string& name){
        return std::string(JASPER_MEDIA_TEST_CLIPS) + "/" + name;
    }

    std::string webmFixture(const std::string& name){
        return std::string(JASPER_HTMLUI_VIDEO_FIXTURES) + "/" + name;
    }

    std::string readBytes(const std::string& path){
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool ffmpeg(){
        std::string reason;
        const bool available = FFmpegBackend::isAvailable(&reason);
        if (!available)
            std::printf("[  SKIPPED ] no FFmpeg libraries: %s\n", reason.c_str());
        return available;
    }

    // the quadrant digit at a pixel: black 0, red 1, green 2, blue 3 (BT.601 limited range)
    int digitAt(const VideoFrame& frame, int x, int y){
        const double Y = frame.y[(size_t)y * frame.width + x];
        const size_t c = (size_t)(y >> frame.chromaShiftY) * frame.chromaWidth() + (size_t)(x >> frame.chromaShiftX);
        const double U = frame.u[c] - 128.0, V = frame.v[c] - 128.0;
        const double r = 1.164 * (Y - 16) + 1.596 * V;
        const double g = 1.164 * (Y - 16) - 0.392 * U - 0.813 * V;
        const double b = 1.164 * (Y - 16) + 2.017 * U;
        const double colours[4][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        int best = 0;
        double bestDistance = 1e18;
        for (int i = 0; i < 4; i++){
            const double d = (r - colours[i][0]) * (r - colours[i][0]) + (g - colours[i][1]) * (g - colours[i][1]) + (b - colours[i][2]) * (b - colours[i][2]);
            if (d < bestDistance){
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    // the frame number a 160x90 clip's picture shows
    int frameNumber(const VideoFrame& frame){
        if (frame.width != 160 || frame.height != 90 || !frame.hasChroma())
            return -1;
        return digitAt(frame, 40, 22) + 4 * digitAt(frame, 120, 22) + 16 * digitAt(frame, 40, 67);
    }

    bool isWhite(const VideoFrame& frame, int x, int y){
        return frame.y[(size_t)y * frame.width + x] > 200;
    }

    class VideoMp4Test : public ::testing::Test {
    protected:
        scriptdata::TestSystem system;
        System* previous = nullptr;
        fs::path root;

        void SetUp() override{
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("jasper-video-mp4-" + std::to_string(::getpid()) + "-" + info->name());
            fs::remove_all(root);
            fs::create_directories(root / "data");
            fs::create_directories(root / "assets");
            fs::create_directories(root / "lua");
            system.root = root.string();
            previous = SystemTestAccess::swap(&system);
            VideoSourceFactory::setPlatformDecodersEnabledForTesting(true);
            VideoSourceFactory::setPlatformBackendForTesting(nullptr, nullptr, nullptr);
            VideoSourceFactory::setSourceOverrideForTesting(nullptr);
        }

        void TearDown() override{
            MediaCache::shutdown();
            Http::shutdown();
            MainThreadQueue::clear();
            VideoSourceFactory::setPlatformDecodersEnabledForTesting(true);
            SystemTestAccess::swap(previous);
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // frames' worth of main-thread deliveries until `done`
        bool waitFor(const std::function<bool()>& done, double seconds = 10.0){
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end){
                MainThreadQueue::drain();
                if (done())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        std::unique_ptr<PlatformVideoSource> platformSource(const std::string& path, bool audio = false){
            std::string error;
            std::shared_ptr<MediaByteSource> bytes = MediaByteSource::openFile(path, error);
            EXPECT_TRUE(bytes) << error;
            if (!bytes)
                return nullptr;
            const Mp4ProbeResult probe = Mp4Probe::probe(*bytes);
            return std::make_unique<PlatformVideoSource>(bytes, probe.ok ? &probe : nullptr, []{ return FFmpegBackend::create(); }, "ffmpeg", audio);
        }

        // the frame shown at media time (frame index + a third of a frame) of an iteration
        int frameAt(IVideoSource& source, int iteration, int index, int64_t& shown){
            VideoFrame frame;
            const IVideoSource::FrameResult result = source.acquireFrame(iteration, (index + 0.33) / 30.0, true, shown, frame);
            if (result != IVideoSource::FrameResult::NEW)
                return -100 - (int)result;
            shown = frame.sequence;
            return frameNumber(frame);
        }
    };

}

// ---------------------------------------------------------------- the probe

TEST_F(VideoMp4Test, Mp4ProbeReadsTracks){
    std::string error;
    std::unique_ptr<MediaByteSource> bytes = MediaByteSource::openFile(clip("h264_aac_160x90.mp4"), error);
    ASSERT_TRUE(bytes) << error;
    const Mp4ProbeResult probe = Mp4Probe::probe(*bytes);
    ASSERT_TRUE(probe.ok) << probe.error;
    EXPECT_EQ(probe.container, "mp4");
    EXPECT_TRUE(probe.fastStart);
    EXPECT_FALSE(probe.fragmented);
    EXPECT_NEAR(probe.duration, 2.0, 0.05);
    EXPECT_EQ(probe.video.fourcc, "avc1");
    EXPECT_EQ(probe.video.codecString.rfind("avc1.42", 0), 0u) << probe.video.codecString;   // Baseline
    EXPECT_EQ(probe.video.width, 160);
    EXPECT_EQ(probe.video.height, 90);
    EXPECT_NEAR(probe.video.frameRate, 30.0, 0.01);
    EXPECT_EQ(probe.video.sampleCount, 60u);
    EXPECT_EQ(probe.video.rotation, 0);
    EXPECT_EQ(probe.audio.fourcc, "mp4a");
    EXPECT_EQ(probe.audio.codecString, "mp4a.40.2");
    EXPECT_EQ(probe.audio.channels, 1);
    EXPECT_EQ(probe.audio.sampleRate, 48000);

    Mp4VideoIndex index;
    ASSERT_TRUE(Mp4Probe::buildVideoIndex(*bytes, probe, index, error)) << error;
    ASSERT_EQ(index.ptsNs.size(), 60u);
    for (size_t i = 0; i < index.ptsNs.size(); i++)
        EXPECT_NEAR((double)index.ptsNs[i], i * 1e9 / 30.0, 1e6) << i;
    EXPECT_EQ(index.keyframes, (std::vector<int>{0, 30}));
    EXPECT_EQ(index.frameAt(0), 0);
    EXPECT_EQ(index.frameAt((int64_t)(1.25e9)), 37);
    EXPECT_EQ(index.keyframeAtOrBefore(37), 30);
    EXPECT_EQ(index.keyframeAfter(10), 30);
    EXPECT_EQ(index.keyframeAfter(30), -1);
}

TEST_F(VideoMp4Test, Mp4ProbeFastStartRotationAndCodecs){
    std::string error;
    auto probeOf = [&](const std::string& name){
        std::unique_ptr<MediaByteSource> bytes = MediaByteSource::openFile(clip(name), error);
        EXPECT_TRUE(bytes) << error;
        return bytes ? Mp4Probe::probe(*bytes) : Mp4ProbeResult();
    };
    const Mp4ProbeResult end = probeOf("h264_aac_160x90_moov_end.mp4");
    ASSERT_TRUE(end.ok) << end.error;
    EXPECT_FALSE(end.fastStart);
    EXPECT_GT(end.moovOffset, end.mdatOffset);

    const Mp4ProbeResult hevc = probeOf("hevc_aac_160x90.mp4");
    ASSERT_TRUE(hevc.ok) << hevc.error;
    EXPECT_EQ(hevc.video.fourcc, "hvc1");
    EXPECT_EQ(hevc.video.bitDepth, 8);

    const Mp4ProbeResult turned = probeOf("h264_rotated_160x90.mp4");
    ASSERT_TRUE(turned.ok) << turned.error;
    EXPECT_EQ(turned.video.rotation, 90);
    EXPECT_EQ(turned.audio.kind, Mp4Track::Kind::NONE);
    // the display size is the turned one
    VideoSourceFactory::setPlatformDecodersEnabledForTesting(false);
    const VideoProbe shown = VideoFormats::probeFile(clip("h264_rotated_160x90.mp4"));
    EXPECT_EQ(shown.width, 90);
    EXPECT_EQ(shown.height, 160);
    EXPECT_EQ(shown.rotation, 90);
    EXPECT_EQ(shown.container, "mp4");
    EXPECT_EQ(shown.videoCodec, "avc1");
    EXPECT_FALSE(shown.playable) << "no platform decoders: not playable here";
    EXPECT_EQ(shown.error.code, MediaErrorCode::SRC_NOT_SUPPORTED);

    // WebM is not ISO-BMFF; neither is garbage
    const std::string webm = readBytes(webmFixture("testcard.webm"));
    EXPECT_FALSE(Mp4Probe::sniff((const uint8_t*)webm.data(), webm.size()));
    const uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT_FALSE(Mp4Probe::sniff(garbage, sizeof(garbage)));
    std::unique_ptr<MediaByteSource> junk = MediaByteSource::openMemory(std::vector<uint8_t>(garbage, garbage + 16), "junk");
    const Mp4ProbeResult nothing = Mp4Probe::probe(*junk);
    EXPECT_FALSE(nothing.ok);
    EXPECT_FALSE(nothing.isoBmff);
}

TEST_F(VideoMp4Test, Mp4ProbeTruncatedFiles){
    const std::string whole = readBytes(clip("h264_aac_160x90.mp4"));
    // the moov box cut short
    std::unique_ptr<MediaByteSource> cut = MediaByteSource::openMemory(std::vector<uint8_t>(whole.begin(), whole.begin() + 400), "cut.mp4");
    const Mp4ProbeResult shortMoov = Mp4Probe::probe(*cut);
    EXPECT_TRUE(shortMoov.isoBmff);
    EXPECT_FALSE(shortMoov.ok);
    EXPECT_FALSE(shortMoov.error.empty());
    // moov at the end, the file cut in its media data: no moov
    const std::string end = readBytes(clip("h264_aac_160x90_moov_end.mp4"));
    std::unique_ptr<MediaByteSource> head = MediaByteSource::openMemory(std::vector<uint8_t>(end.begin(), end.begin() + 3000), "head.mp4");
    const Mp4ProbeResult noMoov = Mp4Probe::probe(*head);
    EXPECT_FALSE(noMoov.ok);
    EXPECT_GE(noMoov.mdatOffset, 0);
    // a box claiming more than the file
    std::vector<uint8_t> bad(whole.begin(), whole.begin() + 64);
    bad[0] = 0x7F;
    std::unique_ptr<MediaByteSource> damaged = MediaByteSource::openMemory(bad, "damaged.mp4");
    EXPECT_FALSE(Mp4Probe::probe(*damaged).ok);
}

TEST_F(VideoMp4Test, OptimizeForStreamingMovesTheMoov){
    const std::string output = (root / "fast.mp4").string();
    std::string error;
    ASSERT_TRUE(Mp4Probe::rewriteFastStart(clip("h264_aac_160x90_moov_end.mp4"), output, error)) << error;
    std::unique_ptr<MediaByteSource> bytes = MediaByteSource::openFile(output, error);
    ASSERT_TRUE(bytes) << error;
    const Mp4ProbeResult probe = Mp4Probe::probe(*bytes);
    ASSERT_TRUE(probe.ok) << probe.error;
    EXPECT_TRUE(probe.fastStart);
    EXPECT_EQ(fs::file_size(output), fs::file_size(clip("h264_aac_160x90_moov_end.mp4")));
    Mp4VideoIndex index;
    EXPECT_TRUE(Mp4Probe::buildVideoIndex(*bytes, probe, index, error)) << error;
    EXPECT_EQ(index.ptsNs.size(), 60u);
    if (!ffmpeg())
        return;
    // the chunk offsets moved with the data: every frame still decodes to its number
    std::unique_ptr<PlatformVideoSource> source = platformSource(output);
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    int64_t shown = -1;
    for (int k = 0; k < 60; k += 7)
        EXPECT_EQ(frameAt(*source, 0, k, shown), k);
}

TEST_F(VideoMp4Test, OptimizeForStreamingWritesTheSizeOfAToTheEndMoov){
    // the moov is the last box and says size 0 ("to the end of the file"), as some muxers write it
    std::ifstream in(clip("h264_aac_160x90_moov_end.mp4"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::unique_ptr<MediaByteSource> original = MediaByteSource::openMemory(bytes, "moov_end.mp4");
    const Mp4ProbeResult before = Mp4Probe::probe(*original);
    ASSERT_TRUE(before.ok) << before.error;
    ASSERT_GT(before.moovOffset, 0);
    const size_t at = (size_t)before.moovOffset;
    bytes[at] = bytes[at + 1] = bytes[at + 2] = bytes[at + 3] = 0;
    const std::string input = (root / "zero.mp4").string();
    {
        std::ofstream out(input, std::ios::binary);
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }
    const std::string output = (root / "zero-fast.mp4").string();
    std::string error;
    ASSERT_TRUE(Mp4Probe::rewriteFastStart(input, output, error)) << error;
    std::unique_ptr<MediaByteSource> fast = MediaByteSource::openFile(output, error);
    ASSERT_TRUE(fast) << error;
    const Mp4ProbeResult probe = Mp4Probe::probe(*fast);
    ASSERT_TRUE(probe.ok) << probe.error;
    EXPECT_TRUE(probe.fastStart);
    Mp4VideoIndex index;
    EXPECT_TRUE(Mp4Probe::buildVideoIndex(*fast, probe, index, error)) << error;
    EXPECT_EQ(index.ptsNs.size(), 60u);
}

// ---------------------------------------------------------------- growing files

TEST_F(VideoMp4Test, GrowingFileReaderBlocksUntilTheBytesArrive){
    const std::string part = (root / "data" / "clip.part").string();
    const std::string final = (root / "data" / "clip.mp4").string();
    {
        std::ofstream out(part, std::ios::binary);
        out << std::string(1000, 'a');
    }
    auto state = std::make_shared<GrowingFileState>(part, final);
    state->update(1000, 3000);
    GrowingFileReader reader(state, "http://example/clip.mp4");
    EXPECT_TRUE(reader.isGrowing());
    EXPECT_EQ(reader.size(), 3000);
    char buffer[2048];
    EXPECT_EQ(reader.readAt(0, buffer, 1000), 1000);

    // a read past the bytes waits; more bytes wake it
    std::atomic<int64_t> got{-2};
    std::thread waiting([&]{ got = reader.readAt(1000, buffer, 1000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(got.load(), -2) << "the read waits for the download";
    {
        std::ofstream out(part, std::ios::binary | std::ios::app);
        out << std::string(1000, 'b');
    }
    state->update(2000, 3000);
    waiting.join();
    EXPECT_EQ(got.load(), 1000);
    EXPECT_EQ(buffer[0], 'b');

    // the end of a finished file
    {
        std::ofstream out(part, std::ios::binary | std::ios::app);
        out << std::string(1000, 'c');
    }
    fs::rename(part, final);
    state->complete(3000);
    EXPECT_FALSE(reader.isGrowing());
    EXPECT_EQ(reader.readAt(2500, buffer, 1000), 500);
    EXPECT_EQ(reader.readAt(3000, buffer, 10), 0);
    // a snapshot never blocks
    std::unique_ptr<MediaByteSource> snapshot = GrowingFileReader::snapshot(state, "snapshot");
    EXPECT_EQ(snapshot->readAt(2900, buffer, 500), 100);
}

TEST_F(VideoMp4Test, GrowingFileReaderFailsWithTheDownloadOrAbort){
    const std::string part = (root / "data" / "f.part").string();
    std::ofstream(part, std::ios::binary) << std::string(100, 'x');
    auto state = std::make_shared<GrowingFileState>(part, part + ".done");
    state->update(100, -1);
    char buffer[64];
    {
        GrowingFileReader reader(state, "u");
        std::atomic<int64_t> got{-2};
        std::thread waiting([&]{ got = reader.readAt(100, buffer, 10); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        reader.abort();
        waiting.join();
        EXPECT_EQ(got.load(), -1) << "abort ends a waiting read";
    }
    GrowingFileReader second(state, "u");
    std::atomic<int64_t> got{-2};
    std::thread waiting([&]{ got = second.readAt(100, buffer, 10); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    state->fail("connection lost");
    waiting.join();
    EXPECT_EQ(got.load(), -1);
    EXPECT_EQ(second.readAt(0, buffer, 10), -1) << "a failed download fails every read";
}

// ---------------------------------------------------------------- FFmpeg

TEST_F(VideoMp4Test, FFmpegDecodesEveryFrame){
    if (!ffmpeg())
        GTEST_SKIP();
    std::unique_ptr<PlatformVideoSource> source = platformSource(clip("h264_aac_160x90.mp4"));
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    const VideoInfo& info = source->getInfo();
    EXPECT_TRUE(source->hasExactIndex());
    EXPECT_EQ(info.frameCount, 60);
    EXPECT_NEAR(info.frameRate, 30.0, 0.01);
    EXPECT_NEAR(info.duration, 2.0, 0.05);
    EXPECT_EQ(info.width, 160);
    EXPECT_EQ(info.displayWidth, 160);
    EXPECT_EQ(info.videoCodec, "avc1");
    EXPECT_EQ(info.container, "mp4");
    EXPECT_STREQ(source->backendName(), "ffmpeg");
    int64_t shown = -1;
    for (int k = 0; k < 60; k++)
        ASSERT_EQ(frameAt(*source, 0, k, shown), k) << "frame " << k;
    EXPECT_EQ(source->getStats().seeks, 0u) << "played forward: no seek";
}

TEST_F(VideoMp4Test, FFmpegSeeksExactly){
    if (!ffmpeg())
        GTEST_SKIP();
    std::unique_ptr<PlatformVideoSource> source = platformSource(clip("h264_aac_160x90.mp4"));
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    int64_t shown = -1;
    EXPECT_EQ(frameAt(*source, 0, 37, shown), 37);
    EXPECT_EQ(source->keyframeTimeAtOrBefore(37 / 30.0), 1.0);
    // back to before the keyframe, then forward again
    EXPECT_EQ(frameAt(*source, 0, 3, shown), 3);
    EXPECT_EQ(frameAt(*source, 0, 59, shown), 59);
    EXPECT_GE(source->getStats().seeks, 2u);
}

TEST_F(VideoMp4Test, FFmpegLoopsWithoutSkippingAFrame){
    if (!ffmpeg())
        GTEST_SKIP();
    std::unique_ptr<PlatformVideoSource> source = platformSource(clip("h264_aac_160x90.mp4"));
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    source->setLoop(true);
    int64_t shown = -1;
    for (int k = 0; k < 150; k++){
        const int iteration = k / 60;
        ASSERT_EQ(frameAt(*source, iteration, k % 60, shown), k % 60) << "iteration " << iteration << " frame " << k % 60;
        EXPECT_EQ(shown, k);
    }
    EXPECT_EQ(source->getStats().seeks, 0u) << "a loop decodes on, it does not seek";
}

TEST_F(VideoMp4Test, FFmpegAudioIsTheLengthOfTheClip){
    if (!ffmpeg())
        GTEST_SKIP();
    std::unique_ptr<PlatformVideoSource> source = platformSource(clip("h264_aac_160x90.mp4"), true);
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    EXPECT_TRUE(source->getInfo().hasAudio);
    EXPECT_EQ(source->getInfo().audioCodec, "mp4a");
    std::shared_ptr<VideoAudioRing> ring = source->getAudioRing();
    ASSERT_TRUE(ring);
    EXPECT_EQ(ring->getChannels(), 2);
    source->syncAudio(0);
    source->waitAudioSync();
    // read as the mixer would until nothing more is written
    std::vector<float> block(4800 * 2);
    int64_t written = -1;
    int still = 0;
    double peak = 0.0;
    for (int i = 0; i < 2000 && still < 40; i++){
        ring->read(block.data(), 4800);
        for (float sample : block)
            peak = std::max(peak, (double)std::fabs(sample));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const int64_t now = ring->getWritePosition();
        still = now == written ? still + 1 : 0;
        written = now;
    }
    EXPECT_NEAR((double)written, 2.0 * 48000, 0.01 * 2.0 * 48000) << "48 kHz x the duration";
    EXPECT_GT(peak, 0.1) << "the tone is heard";
}

TEST_F(VideoMp4Test, FFmpegTurnsRotatedVideo){
    if (!ffmpeg())
        GTEST_SKIP();
    std::unique_ptr<PlatformVideoSource> source = platformSource(clip("h264_rotated_160x90.mp4"));
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    EXPECT_EQ(source->getInfo().rotation, 90);
    EXPECT_EQ(source->getInfo().displayWidth, 90);
    EXPECT_EQ(source->getInfo().displayHeight, 160);
    VideoFrame frame;
    ASSERT_EQ(source->acquireFrame(0, 0.01, true, -1, frame), IVideoSource::FrameResult::NEW);
    EXPECT_EQ(frame.width, 90);
    EXPECT_EQ(frame.height, 160);
    // the white reference quarter (bottom-right when upright) is at the top-left once turned clockwise
    EXPECT_TRUE(isWhite(frame, 22, 120));
    EXPECT_FALSE(isWhite(frame, 67, 40));
}

TEST_F(VideoMp4Test, FFmpegPlaysHevcAndVp9WhereItHasTheDecoders){
    if (!ffmpeg())
        GTEST_SKIP();
    if (FFmpegBackend::hasDecoder("hevc")){
        std::unique_ptr<IVideoSource> hevc = VideoSourceFactory::create(clip("hevc_aac_160x90.mp4"), VideoSourceFactory::Options{false, true});
        ASSERT_EQ(hevc->waitOpen(), IVideoSource::State::READY) << hevc->getError().message;
        EXPECT_STREQ(hevc->backendName(), "ffmpeg");
        int64_t shown = -1;
        EXPECT_EQ(frameAt(*hevc, 0, 12, shown), 12);
    }
    if (FFmpegBackend::hasDecoder("vp9")){
        // VP9 WebM: the platform's decoders (the software path is AV1 only)
        std::unique_ptr<IVideoSource> vp9 = VideoSourceFactory::create(clip("vp9_opus_160x90.webm"), VideoSourceFactory::Options{false, true});
        ASSERT_EQ(vp9->waitOpen(), IVideoSource::State::READY) << vp9->getError().message;
        EXPECT_STREQ(vp9->backendName(), "ffmpeg");
        EXPECT_GE(vp9->getInfo().frameCount, 59);
        int64_t shown = -1;
        EXPECT_EQ(frameAt(*vp9, 0, 20, shown), 20);
    }
}

// ---------------------------------------------------------------- the factory

TEST_F(VideoMp4Test, FactoryKeepsWebmAv1InSoftware){
    std::unique_ptr<IVideoSource> source = VideoSourceFactory::create(webmFixture("testcard.webm"));
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    EXPECT_STREQ(source->backendName(), "webm") << "AV1/Opus WebM plays in software everywhere, for identical frames";
    VideoSourceFactory::setPlatformDecodersEnabledForTesting(false);
    std::unique_ptr<IVideoSource> again = VideoSourceFactory::create(webmFixture("testcard.webm"));
    EXPECT_STREQ(again->backendName(), "webm");
}

TEST_F(VideoMp4Test, FactoryPicksThePlatformForMp4){
    if (ffmpeg()){
        std::unique_ptr<IVideoSource> source = VideoSourceFactory::create(clip("h264_aac_160x90.mp4"));
        ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
        EXPECT_STREQ(source->backendName(), "ffmpeg");
        const VideoProbe probe = VideoFormats::probeFile(clip("h264_aac_160x90.mp4"));
        EXPECT_TRUE(probe.playable) << probe.error.message;
        EXPECT_EQ(probe.backend, "ffmpeg");
        EXPECT_TRUE(probe.fastStart);
        EXPECT_EQ(VideoFormats::canPlayType("video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\""), VideoFormats::CanPlay::PROBABLY);
        EXPECT_EQ(VideoFormats::canPlayType("video/mp4"), VideoFormats::CanPlay::MAYBE);
        EXPECT_EQ(VideoFormats::canPlayExtension("https://cdn.example.com/a.mp4?x=1"), VideoFormats::CanPlay::PROBABLY);
    }
    // a fake platform backend: the factory's choice by content, not extension
    struct Fake : PlatformDecoderBackend {
        bool open(MediaByteSource&, Stream, VideoInfo& info, MediaError&) override{ info.width = info.height = 16; return true; }
        bool seek(double) override{ return true; }
        Step decodeNext(VideoFrame&, std::vector<float>&, double&, MediaError&) override{ return Step::END; }
        void close() override{}
    };
    VideoSourceFactory::setPlatformBackendForTesting([]{ return std::make_unique<Fake>(); }, "fake",
        [](const std::string& codec){ return codec == "h264" ? VideoSourceFactory::Support::Yes : VideoSourceFactory::Support::No; });
    std::unique_ptr<IVideoSource> faked = VideoSourceFactory::create(clip("h264_aac_160x90.mp4"));
    EXPECT_STREQ(faked->backendName(), "fake");
    // HEVC: the fake has no decoder for it
    std::unique_ptr<IVideoSource> hevc = VideoSourceFactory::create(clip("hevc_aac_160x90.mp4"));
    EXPECT_EQ(hevc->getState(), IVideoSource::State::FAILED);
    EXPECT_EQ(hevc->getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_NE(hevc->getError().message.find("HEVC video does not play on Linux"), std::string::npos) << hevc->getError().message;
    VideoSourceFactory::setPlatformBackendForTesting(nullptr, nullptr, nullptr);
}

TEST_F(VideoMp4Test, FactoryWithoutFFmpegSaysWhatToInstall){
    const char* saved = std::getenv("JASPERENGINE_FFMPEG_DIR");
    const std::string previousDir = saved ? saved : "";
    ::setenv("JASPERENGINE_FFMPEG_DIR", (root / "no-ffmpeg-here").string().c_str(), 1);
    FFmpegBackend::reloadForTesting();
    std::unique_ptr<IVideoSource> source = VideoSourceFactory::create(clip("h264_aac_160x90.mp4"));
    EXPECT_EQ(source->getState(), IVideoSource::State::FAILED);
    EXPECT_EQ(source->getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_NE(source->getError().message.find("install libavcodec, libavformat and libswresample"), std::string::npos) << source->getError().message;
    EXPECT_NE(source->getError().message.find("Fedora"), std::string::npos);
    EXPECT_EQ(VideoFormats::canPlayType("video/mp4; codecs=avc1.42E01E"), VideoFormats::CanPlay::NO);
    // WebM AV1 is not affected
    std::unique_ptr<IVideoSource> webm = VideoSourceFactory::create(webmFixture("testcard.webm"));
    EXPECT_EQ(webm->waitOpen(), IVideoSource::State::READY);
    if (saved)
        ::setenv("JASPERENGINE_FFMPEG_DIR", previousDir.c_str(), 1);
    else
        ::unsetenv("JASPERENGINE_FFMPEG_DIR");
    FFmpegBackend::reloadForTesting();
}

TEST_F(VideoMp4Test, PlatformMatrix){
    VideoProbe mp4;
    mp4.readable = true;
    mp4.container = "mp4";
    mp4.videoCodec = "avc1";
    mp4.audioCodec = "mp4a";
    using P = VideoSourceFactory::Platform;
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Linux, mp4), "needs_ffmpeg");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Windows, mp4), "yes");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::IOS, mp4), "yes");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Android, mp4), "yes");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Web, mp4), "yes");
    VideoProbe hevc = mp4;
    hevc.videoCodec = "hvc1";
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Windows, hevc), "needs_extension");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::MacOS, hevc), "yes");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Android, hevc), "device_dependent");
    VideoProbe webm;
    webm.readable = true;
    webm.container = "webm";
    webm.videoCodec = "V_AV1";
    webm.audioCodec = "A_OPUS";
    for (int p = 0; p < VideoSourceFactory::PLATFORM_COUNT; p++)
        EXPECT_STREQ(VideoSourceFactory::playableOn((P)p, webm), "yes") << VideoSourceFactory::platformId((P)p);
    VideoProbe vp9 = webm;
    vp9.videoCodec = "V_VP9";
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::IOS, vp9), "no");
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Android, vp9), "yes");
    mp4.encrypted = true;
    EXPECT_STREQ(VideoSourceFactory::playableOn(P::Windows, mp4), "no");
}

// ---------------------------------------------------------------- the media cache

namespace {

    class MediaCacheTest : public VideoMp4Test {
    protected:
        std::unique_ptr<TestHttpServer> server;

        void SetUp() override{
            VideoMp4Test::SetUp();
            server = std::make_unique<TestHttpServer>();
            ASSERT_TRUE(server->started());
            MediaCache::setLimit(MediaCache::DEFAULT_LIMIT);
        }
        void TearDown() override{
            MediaCache::shutdown();
            Http::shutdown();
            waitFor([]{ return HttpTesting::runningTransfers() == 0; }, 5.0);
            server.reset();
            VideoMp4Test::TearDown();
        }
    };

}

TEST_F(MediaCacheTest, DownloadsIntoTheCache){
    const std::string bytes = readBytes(clip("h264_aac_160x90.mp4"));
    server->addFile("ad.mp4", bytes, "\"v1\"");
    const std::string url = server->url("/file/ad.mp4");
    EXPECT_EQ(MediaCache::pathFor(url).rfind((root / "data" / ".cache" / "media").string() + "/", 0), 0u) << MediaCache::pathFor(url);
    EXPECT_EQ(MediaCache::pathFor(url).substr(MediaCache::pathFor(url).size() - 4), ".mp4");
    std::shared_ptr<MediaDownload> download = MediaCache::acquire(url);
    ASSERT_TRUE(waitFor([&]{ return download->getStatus() != MediaDownload::Status::DOWNLOADING; }));
    ASSERT_EQ(download->getStatus(), MediaDownload::Status::COMPLETE) << download->getError().message;
    EXPECT_FALSE(download->isFromCache());
    EXPECT_EQ(download->getProgress(), 1.0);
    EXPECT_EQ(readBytes(download->getFilePath()), bytes);
    MediaCache::release(download);

    // again: from the cache at once, revalidated with the ETag (304 keeps it)
    std::shared_ptr<MediaDownload> cached = MediaCache::acquire(url);
    EXPECT_EQ(cached->getStatus(), MediaDownload::Status::COMPLETE);
    EXPECT_TRUE(cached->isFromCache());
    ASSERT_TRUE(waitFor([&]{ return server->requests("/file/ad.mp4") == 2; }));
    waitFor([]{ return HttpTesting::runningTransfers() == 0; }, 5.0);
    waitFor([]{ return false; }, 0.1);
    EXPECT_EQ(readBytes(cached->getFilePath()), bytes);
    MediaCache::release(cached);

    // offline with the complete copy: plays from the cache (the failed check is no error)
    Http::setOfflineSimulated(true);
    std::shared_ptr<MediaDownload> offline = MediaCache::acquire(url);
    EXPECT_EQ(offline->getStatus(), MediaDownload::Status::COMPLETE);
    waitFor([]{ return false; }, 0.1);
    EXPECT_EQ(offline->getStatus(), MediaDownload::Status::COMPLETE);
    MediaCache::release(offline);
    Http::setOfflineSimulated(false);
    EXPECT_GE(MediaCache::clear(url), 2) << "the file and its meta";
    EXPECT_FALSE(fs::exists(MediaCache::pathFor(url)));
}

TEST_F(MediaCacheTest, ResumesAPartialDownload){
    const std::string bytes = readBytes(clip("h264_aac_160x90.mp4"));
    server->addFile("resume.mp4", bytes, "\"r\"");
    server->cutNext("resume.mp4", 5000);
    const std::string url = server->url("/file/resume.mp4");
    std::shared_ptr<MediaDownload> first = MediaCache::acquire(url);
    ASSERT_TRUE(waitFor([&]{ return first->getStatus() != MediaDownload::Status::DOWNLOADING; }));
    EXPECT_EQ(first->getStatus(), MediaDownload::Status::FAILED);
    EXPECT_EQ(first->getError().code, MediaErrorCode::NETWORK);
    MediaCache::release(first);
    const std::string part = MediaCache::pathFor(url) + ".part";
    ASSERT_TRUE(fs::exists(part)) << "the partial download is kept";
    EXPECT_GT(fs::file_size(part), 0u);
    // the next try asks for the rest (Range) and ends with the whole file
    std::shared_ptr<MediaDownload> second = MediaCache::acquire(url);
    ASSERT_TRUE(waitFor([&]{ return second->getStatus() != MediaDownload::Status::DOWNLOADING; }));
    ASSERT_EQ(second->getStatus(), MediaDownload::Status::COMPLETE) << second->getError().message;
    EXPECT_EQ(readBytes(second->getFilePath()), bytes);
    MediaCache::release(second);
}

TEST_F(MediaCacheTest, EvictsTheLeastRecentlyUsed){
    const std::string bytes = readBytes(clip("h264_aac_160x90.mp4"));
    for (const char* name : {"a.mp4", "b.mp4", "c.mp4"})
        server->addFile(name, bytes, std::string("\"") + name + "\"");
    MediaCache::setLimit(bytes.size() * 2 + 1000);
    std::vector<std::string> paths;
    for (const char* name : {"a.mp4", "b.mp4", "c.mp4"}){
        std::shared_ptr<MediaDownload> download = MediaCache::acquire(server->url(std::string("/file/") + name));
        ASSERT_TRUE(waitFor([&]{ return download->getStatus() != MediaDownload::Status::DOWNLOADING; }));
        ASSERT_EQ(download->getStatus(), MediaDownload::Status::COMPLETE);
        paths.push_back(download->getFilePath());
        MediaCache::release(download);
        // distinct use times
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_FALSE(fs::exists(paths[0])) << "the oldest went";
    EXPECT_TRUE(fs::exists(paths[1]));
    EXPECT_TRUE(fs::exists(paths[2]));
    EXPECT_LE(MediaCache::getUsedBytes(), MediaCache::getLimit());
}

TEST_F(MediaCacheTest, CancelledWithItsLastOwner){
    const std::string bytes = readBytes(clip("h264_aac_160x90.mp4"));
    server->addFile("slow.mp4", bytes, "");
    const std::string url = server->url("/file/slow.mp4?trickle=200");
    std::shared_ptr<MediaDownload> a = MediaCache::acquire(url);
    std::shared_ptr<MediaDownload> b = MediaCache::acquire(url);
    EXPECT_EQ(a, b) << "one download per URL";
    ASSERT_TRUE(waitFor([&]{ return a->getReceived() > 0; }));
    MediaCache::release(a);
    EXPECT_EQ(MediaCache::getActiveDownloads(), 1) << "another player still wants it";
    MediaCache::release(b);
    EXPECT_EQ(MediaCache::getActiveDownloads(), 0);
    EXPECT_EQ(b->getStatus(), MediaDownload::Status::FAILED);
    EXPECT_EQ(b->getError().code, MediaErrorCode::ABORTED);
    waitFor([]{ return HttpTesting::runningTransfers() == 0; }, 5.0);
    EXPECT_TRUE(fs::exists(MediaCache::pathFor(url) + ".part")) << "kept for a resume";
}

TEST_F(MediaCacheTest, FastStartPlaysWhileDownloading){
    if (!ffmpeg())
        GTEST_SKIP();
    const std::string bytes = readBytes(clip("h264_aac_160x90.mp4"));
    server->addFile("fast.mp4", bytes, "");
    std::unique_ptr<IVideoSource> source = VideoSourceFactory::create(server->url("/file/fast.mp4?trickle=250"), VideoSourceFactory::Options{false, true});
    EXPECT_EQ(source->getState(), IVideoSource::State::BUFFERING);
    bool earlyFrame = false;
    double bufferedAtStart = -1.0;
    ASSERT_TRUE(waitFor([&]{
        if (source->getState() != IVideoSource::State::READY)
            return false;
        VideoFrame frame;
        if (source->acquireFrame(0, 0.01, false, -1, frame) == IVideoSource::FrameResult::NEW){
            earlyFrame = source->getDownloadProgress() < 1.0;
            bufferedAtStart = source->getBufferedSeconds();
            return true;
        }
        return false;
    }, 15.0));
    EXPECT_TRUE(earlyFrame) << "the first frame came before the download completed";
    EXPECT_LT(bufferedAtStart, source->getInfo().duration);
    std::printf("[ progressive ] first frame at %.0f%% of the download\n", 100.0 * bufferedAtStart / source->getInfo().duration);
    // and it plays to the end once the rest arrives
    int64_t shown = -1;
    ASSERT_TRUE(waitFor([&]{ return source->getDownloadProgress() >= 1.0; }, 15.0));
    EXPECT_EQ(frameAt(*source, 0, 59, shown), 59);
}

TEST_F(MediaCacheTest, MoovAtTheEndPlaysOnceDownloaded){
    if (!ffmpeg())
        GTEST_SKIP();
    const std::string bytes = readBytes(clip("h264_aac_160x90_moov_end.mp4"));
    server->addFile("end.mp4", bytes, "");
    std::unique_ptr<IVideoSource> source = VideoSourceFactory::create(server->url("/file/end.mp4?trickle=100"), VideoSourceFactory::Options{false, true});
    ASSERT_TRUE(waitFor([&]{
        const IVideoSource::State state = source->getState();
        if (state != IVideoSource::State::BUFFERING)
            return true;
        EXPECT_LT(source->getDownloadProgress(), 1.0 + 1e-9);
        return false;
    }, 15.0));
    EXPECT_EQ(source->getDownloadProgress(), 1.0) << "no progressive start without fast start";
    ASSERT_EQ(source->waitOpen(), IVideoSource::State::READY) << source->getError().message;
    int64_t shown = -1;
    EXPECT_EQ(frameAt(*source, 0, 40, shown), 40);
}

TEST_F(MediaCacheTest, UrlErrors){
    std::unique_ptr<IVideoSource> missing = VideoSourceFactory::create(server->url("/file/nothing.mp4"));
    ASSERT_TRUE(waitFor([&]{ return missing->getState() == IVideoSource::State::FAILED; }));
    EXPECT_EQ(missing->getError().code, MediaErrorCode::NETWORK);
    EXPECT_NE(missing->getError().message.find("404"), std::string::npos) << missing->getError().message;
}
