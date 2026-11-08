// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The <video> media core without RmlUi (docs/html-ui/phase-6-video.md §5.1): formats and probe,
// the WebM index, AV1 decoding (every frame shows its own index; 10-bit, 4:4:4, odd sizes, thread
// counts), Opus (the tone bursts at their samples, the 5.1 down-mix), the decode pipeline
// (frame-exact seeks in any order, loops as one stream, inline mode, real time never blocking,
// errors), the playback state machine and the audio ring. Element lifecycle tests own leak coverage.

#include "VideoTestCommon.h"

#include "media/OpusAudioDecoder.h"
#include "media/VideoAudio.h"
#include "media/VideoDecoder.h"
#include "media/VideoDemuxer.h"
#include "media/VideoFormats.h"
#include "media/VideoPlayback.h"
#include "media/VideoStream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    using Ev = VideoPlayback::Event;

    std::vector<VideoFrame> decodeAll(const std::string& name, int threads = 0){
        Av1Decoder::setThreadCountForTesting(threads);
        VideoDemuxer demuxer;
        EXPECT_TRUE(demuxer.open(videoFixture(name))) << demuxer.getError().message;
        Av1Decoder decoder;
        std::string error;
        EXPECT_TRUE(decoder.open(demuxer.getVideoTrack(), error)) << error;
        std::vector<VideoFrame> frames;
        std::vector<uint8_t> data;
        for (const VideoPacketRef& packet : demuxer.getVideoPackets()){
            EXPECT_TRUE(demuxer.read(packet, data));
            VideoFrame frame;
            bool got = false;
            EXPECT_TRUE(decoder.decode(data.data(), data.size(), packet.ptsNs, frame, got, error)) << error;
            EXPECT_TRUE(got);
            if (!got)
                break;
            frames.push_back(std::move(frame));
        }
        Av1Decoder::setThreadCountForTesting(0);
        return frames;
    }

    // left/right channel onsets (first sample above 0.1) of the bursts in [from, from + window)
    int onsetIn(const std::vector<float>& pcm, int channels, int channel, size_t from, size_t window){
        for (size_t i = from; i < from + window && i * channels + channel < pcm.size(); i++){
            if (std::fabs(pcm[i * channels + channel]) > 0.1f)
                return (int)(i - from);
        }
        return -1;
    }

    int zeroCrossings(const std::vector<float>& pcm, int channels, int channel, size_t from, size_t count){
        int crossings = 0;
        for (size_t i = from + 1; i < from + count; i++){
            const float a = pcm[(i - 1) * channels + channel], b = pcm[i * channels + channel];
            if ((a < 0.0f) != (b < 0.0f))
                crossings++;
        }
        return crossings;
    }

}

// ---------------------------------------------------------------- formats

TEST(VideoFormats, CanPlayType){
    using C = VideoFormats::CanPlay;
    EXPECT_EQ(VideoFormats::canPlayType("video/webm"), C::MAYBE);
    EXPECT_EQ(VideoFormats::canPlayType("VIDEO/WebM"), C::MAYBE);
    EXPECT_EQ(VideoFormats::canPlayType("audio/webm"), C::MAYBE);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs=\"av01.0.05M.08, opus\""), C::PROBABLY);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm;codecs=av01"), C::PROBABLY);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs='opus'"), C::PROBABLY);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs=\"vp9\""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs=\"vp09.00.10.08, opus\""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs=\"av01, vorbis\""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/webm; codecs=\"\""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/mp4"), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/mp4; codecs=\"avc1.42E01E\""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("video/ogg"), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType(""), C::NO);
    EXPECT_EQ(VideoFormats::canPlayType("nonsense"), C::NO);
    EXPECT_STREQ(VideoFormats::toString(C::PROBABLY), "probably");
    EXPECT_STREQ(VideoFormats::toString(C::MAYBE), "maybe");
    EXPECT_STREQ(VideoFormats::toString(C::NO), "");
}

TEST(VideoFormats, Extensions){
    EXPECT_TRUE(VideoFormats::isPlayableExtension("ui/clip.webm"));
    EXPECT_TRUE(VideoFormats::isPlayableExtension("ui/CLIP.WEBM?x=1#t=2"));
    EXPECT_TRUE(VideoFormats::isPlayableExtension("a.mkv"));
    // MP4s play where the platform decodes them (docs/superpowers/specs/2026-10-31-video-mp4-design.md)
    EXPECT_TRUE(VideoFormats::isPlayableExtension("a.mp4"));
    EXPECT_FALSE(VideoFormats::isRejectedExtension("a.mp4"));
    EXPECT_FALSE(VideoFormats::isRejectedExtension("dir.v/a.MOV"));
    EXPECT_TRUE(VideoFormats::isMp4Extension("dir.v/a.MOV"));
    EXPECT_TRUE(VideoFormats::isRejectedExtension("a.avi"));
    EXPECT_TRUE(VideoFormats::isRejectedExtension("a.ogv"));
    EXPECT_FALSE(VideoFormats::isRejectedExtension("a.webm"));
    EXPECT_FALSE(VideoFormats::isVideoPath("a.png"));
    EXPECT_FALSE(VideoFormats::isVideoPath("dir.webm/file"));
    EXPECT_TRUE(VideoFormats::isVideoPath("a.m4v"));
    EXPECT_EQ(VideoFormats::codecName("V_VP9"), "VP9");
    EXPECT_EQ(VideoFormats::codecName("V_MPEG4/ISO/AVC"), "H.264");
    EXPECT_EQ(VideoFormats::codecName("A_AAC/MPEG4/LC"), "AAC");
}

TEST(VideoFormats, ProbeFixtures){
    VideoProbe card = VideoFormats::probeFile(videoFixture("testcard.webm"));
    EXPECT_TRUE(card.playable) << card.error.message;
    EXPECT_EQ(card.container, "webm");
    EXPECT_EQ(card.videoCodec, "V_AV1");
    EXPECT_EQ(card.audioCodec, "A_OPUS");
    EXPECT_EQ(card.width, CARD_WIDTH);
    EXPECT_EQ(card.height, CARD_HEIGHT);
    EXPECT_EQ(card.audioChannels, 2);
    EXPECT_NEAR(card.duration, 2.008, 0.002);

    VideoProbe silent = VideoFormats::probeFile(videoFixture("testcard-noaudio.webm"));
    EXPECT_TRUE(silent.playable);
    EXPECT_TRUE(silent.audioCodec.empty());

    VideoProbe vp9 = VideoFormats::probeFile(videoFixture("testcard-vp9.webm"));
    EXPECT_FALSE(vp9.playable);
    EXPECT_TRUE(vp9.readable);
    EXPECT_EQ(vp9.videoCodec, "V_VP9");
    EXPECT_EQ(vp9.error.code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_NE(vp9.error.message.find("V_VP9"), std::string::npos) << vp9.error.message;

    VideoProbe png = VideoFormats::probeFile(videoFixture("poster.png"));
    EXPECT_FALSE(png.playable);
    EXPECT_TRUE(png.container.empty());
    EXPECT_NE(png.error.message.find("not a WebM file"), std::string::npos) << png.error.message;

    VideoProbe missing = VideoFormats::probeFile(videoFixture("missing.webm"));
    EXPECT_FALSE(missing.playable);
    EXPECT_FALSE(missing.readable);
    EXPECT_EQ(missing.error.code, MediaErrorCode::SRC_NOT_SUPPORTED);
}

// ---------------------------------------------------------------- demuxer

TEST(VideoDemuxer, IndexOfTheTestCard){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("testcard.webm"))) << demuxer.getError().message;
    ASSERT_TRUE(demuxer.hasVideo());
    ASSERT_TRUE(demuxer.hasAudio());
    const std::vector<VideoPacketRef>& video = demuxer.getVideoPackets();
    ASSERT_EQ((int)video.size(), CARD_FRAMES);
    for (int k = 0; k < CARD_FRAMES; k++){
        EXPECT_EQ(video[(size_t)k].ptsNs, cardPtsNs(k)) << k;
        EXPECT_EQ(video[(size_t)k].key, k % CARD_FPS == 0) << k;
    }
    const VideoTrackInfo& track = demuxer.getVideoTrack();
    EXPECT_EQ(track.width, CARD_WIDTH);
    EXPECT_EQ(track.displayWidth, CARD_WIDTH);
    EXPECT_EQ(track.color.matrix, VideoColor::Matrix::BT709);
    EXPECT_FALSE(track.color.fullRange);
    const AudioTrackInfo& audio = demuxer.getAudioTrack();
    EXPECT_EQ(audio.channels, 2);
    EXPECT_EQ(audio.sampleRate, 48000.0);
    EXPECT_EQ(audio.codecDelayNs, 6500000);
    // 20 ms packets over 2 s plus the codec delay
    EXPECT_GE(demuxer.getAudioPackets().size(), 100u);
    EXPECT_NEAR(demuxer.getDuration(), 2.008, 0.002);
}

TEST(VideoDemuxer, FrameLookup){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("testcard-noaudio.webm")));
    for (int k = 0; k < CARD_FRAMES; k++){
        EXPECT_EQ(demuxer.videoFrameAt(cardPtsNs(k)), k);
        EXPECT_EQ(demuxer.videoFrameAt(cardPtsNs(k) + 16000000), k);
        if (k > 0){
            EXPECT_EQ(demuxer.videoFrameAt(cardPtsNs(k) - 1), k - 1);
        }
    }
    EXPECT_EQ(demuxer.videoFrameAt(-5), 0);
    EXPECT_EQ(demuxer.videoFrameAt(10000000000LL), CARD_FRAMES - 1);
    EXPECT_EQ(demuxer.keyframeAtOrBefore(29), 0);
    EXPECT_EQ(demuxer.keyframeAtOrBefore(30), 30);
    EXPECT_EQ(demuxer.keyframeAtOrBefore(59), 30);
    EXPECT_EQ(demuxer.keyframeAfter(0), 30);
    EXPECT_EQ(demuxer.keyframeAfter(30), -1);
    // no audio track: the duration is the end of the last frame
    EXPECT_NEAR(demuxer.getDuration(), 2.0, 0.002);
}

TEST(VideoDemuxer, HeadersOnlyReadsNoCluster){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("testcard.webm"), true));
    EXPECT_TRUE(demuxer.getVideoPackets().empty());
    EXPECT_NEAR(demuxer.getDuration(), 2.008, 0.002);
}

TEST(VideoDemuxer, RejectsWhatItCannotPlay){
    VideoDemuxer demuxer;
    EXPECT_FALSE(demuxer.open(videoFixture("testcard-vp9.webm")));
    EXPECT_EQ(demuxer.getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_FALSE(demuxer.open(videoFixture("poster.png")));
    EXPECT_FALSE(demuxer.open(videoFixture("nothing-here.webm")));
    EXPECT_NE(demuxer.getError().message.find("could not be read"), std::string::npos);

    // a file cut inside its (only) cluster: the cluster cannot be read, an error and no crash
    std::ifstream in(videoFixture("testcard-noaudio.webm"), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string cut = ::testing::TempDir() + "p6video-truncated.webm";
    {
        std::ofstream out(cut, std::ios::binary);
        out.write(bytes.data(), (std::streamsize)(bytes.size() * 2 / 3));
    }
    EXPECT_FALSE(demuxer.open(cut));
    EXPECT_EQ(demuxer.getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_NE(demuxer.getError().message.find("without frames"), std::string::npos) << demuxer.getError().message;
    // cut inside the headers
    {
        std::ofstream out(cut, std::ios::binary);
        out.write(bytes.data(), 60);
    }
    EXPECT_FALSE(demuxer.open(cut));
    EXPECT_EQ(demuxer.getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    std::remove(cut.c_str());
}

// ---------------------------------------------------------------- AV1

// A WebM whose headers are whole but that has no frame (the test card cut before its first
// Cluster): not playable, MEDIA_ERR_SRC_NOT_SUPPORTED, no crash
TEST(VideoDemuxer, AFileWithoutFramesIsRejected){
    std::ifstream in(videoFixture("testcard-noaudio.webm"), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const uint8_t clusterId[] = {0x1F, 0x43, 0xB6, 0x75};
    const auto cluster = std::search(bytes.begin(), bytes.end(), std::begin(clusterId), std::end(clusterId));
    ASSERT_NE(cluster, bytes.end());
    const std::vector<uint8_t> headers(bytes.begin(), cluster);
    VideoDemuxer demuxer;
    EXPECT_FALSE(demuxer.openMemory(headers.data(), headers.size(), "no-frames.webm"));
    EXPECT_EQ(demuxer.getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
    EXPECT_NE(demuxer.getError().message.find("no-frames.webm"), std::string::npos) << demuxer.getError().message;
}

TEST(Av1Decoder, EveryFrameShowsItsIndex){
    const std::vector<VideoFrame> frames = decodeAll("testcard-noaudio.webm");
    ASSERT_EQ((int)frames.size(), CARD_FRAMES);
    for (int k = 0; k < CARD_FRAMES; k++){
        const VideoFrame& frame = frames[(size_t)k];
        EXPECT_EQ(frame.width, CARD_WIDTH);
        EXPECT_EQ(frame.height, CARD_HEIGHT);
        EXPECT_EQ(frame.chromaShiftX, 1);
        EXPECT_EQ(frame.chromaShiftY, 1);
        EXPECT_EQ(frame.y.size(), (size_t)CARD_WIDTH * CARD_HEIGHT);
        EXPECT_EQ(frame.u.size(), (size_t)(CARD_WIDTH / 2) * (CARD_HEIGHT / 2));
        EXPECT_EQ(frame.ptsNs, cardPtsNs(k));
        EXPECT_EQ(frame.bitDepth, 8);
        EXPECT_EQ(frame.color.matrix, VideoColor::Matrix::BT709);
        EXPECT_FALSE(frame.color.fullRange);
        EXPECT_EQ(cardIndexOf(frame), k);
    }
    // the bars: white at 75 % is Y 180 in BT.709 limited range ((191/255) x 219 + 16)
    EXPECT_NEAR(frames[0].y[20 * CARD_WIDTH + 20], 180, 3);
}

TEST(Av1Decoder, TenBitMatchesEightBit){
    const std::vector<VideoFrame> eight = decodeAll("testcard-noaudio.webm");
    const std::vector<VideoFrame> ten = decodeAll("testcard-10bit.webm");
    ASSERT_EQ(ten.size(), eight.size());
    for (size_t k = 0; k < ten.size(); k++){
        EXPECT_EQ(ten[k].bitDepth, 10);
        EXPECT_EQ(cardIndexOf(ten[k]), (int)k);
        // the same picture within the coding noise
        EXPECT_NEAR(ten[k].y[20 * CARD_WIDTH + 100], eight[k].y[20 * CARD_WIDTH + 100], 3);
    }
}

TEST(Av1Decoder, ThreadCountsGiveIdenticalPlanes){
    const std::vector<VideoFrame> one = decodeAll("testcard-noaudio.webm", 1);
    const std::vector<VideoFrame> four = decodeAll("testcard-noaudio.webm", 4);
    ASSERT_EQ(one.size(), four.size());
    for (size_t k = 0; k < one.size(); k++){
        EXPECT_EQ(one[k].y, four[k].y) << k;
        EXPECT_EQ(one[k].u, four[k].u) << k;
        EXPECT_EQ(one[k].v, four[k].v) << k;
    }
}

TEST(Av1Decoder, OddSizeAndFullChroma){
    const std::vector<VideoFrame> odd = decodeAll("odd-size.webm");
    ASSERT_FALSE(odd.empty());
    EXPECT_EQ(odd[0].width, 97);
    EXPECT_EQ(odd[0].height, 55);
    EXPECT_EQ(odd[0].chromaWidth(), 49);
    EXPECT_EQ(odd[0].chromaHeight(), 28);
    EXPECT_EQ(odd[0].u.size(), (size_t)49 * 28);

    const std::vector<VideoFrame> full = decodeAll("testcard-444.webm");
    ASSERT_EQ(full.size(), 15u);
    EXPECT_EQ(full[0].chromaShiftX, 0);
    EXPECT_EQ(full[0].chromaShiftY, 0);
    EXPECT_EQ(full[0].u.size(), full[0].y.size());
    for (size_t k = 0; k < full.size(); k++)
        EXPECT_EQ(cardIndexOf(full[k]), (int)k);
}

TEST(Av1Decoder, DamagedFrameFails){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("corrupt.webm")));
    Av1Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(demuxer.getVideoTrack(), error));
    std::vector<uint8_t> data;
    int failedAt = -1;
    for (int k = 0; k < (int)demuxer.getVideoPackets().size(); k++){
        ASSERT_TRUE(demuxer.read(demuxer.getVideoPackets()[(size_t)k], data));
        VideoFrame frame;
        bool got = false;
        if (!decoder.decode(data.data(), data.size(), 0, frame, got, error)){
            failedAt = k;
            break;
        }
        ASSERT_TRUE(got) << k;
        EXPECT_EQ(cardIndexOf(frame), k);
    }
    EXPECT_EQ(failedAt, 40);
    EXPECT_NE(error.find("damaged"), std::string::npos) << error;
}

// dav1d's asm (x86-64 NASM, aarch64/armv7 NEON, the CPU features detected at run time) decodes
// every clip bit for bit as its C functions do: every frame of the test clips (8-bit, 10-bit,
// 4:4:4, an odd size, the example's loop) and of the clips in JASPER_VIDEO_MEASURE (':'-separated,
// e.g. 720p and 1080p encodes), with the CPU feature mask off (C only) and on. Prints the decoder's
// time per frame for the C reference, single-thread asm and the engine's default thread count.
TEST(Av1Decoder, AsmDecodesBitExactlyAsC){
    struct Decoded {
        std::vector<VideoFrame> frames;
        double msPerFrame = 0.0;
        int threads = 0;
    };
    auto decodeFile = [](const std::string& path, unsigned mask, int threads) -> Decoded {
        Decoded out;
        Av1Decoder::setCpuFlagsMaskForTesting(mask);
        Av1Decoder::setThreadCountForTesting(threads);
        VideoDemuxer demuxer;
        EXPECT_TRUE(demuxer.open(path)) << path << ": " << demuxer.getError().message;
        Av1Decoder decoder;
        std::string error;
        EXPECT_TRUE(decoder.open(demuxer.getVideoTrack(), error)) << error;
        out.threads = decoder.getThreadCount();
        std::vector<uint8_t> data;
        double seconds = 0.0;
        for (const VideoPacketRef& packet : demuxer.getVideoPackets()){
            EXPECT_TRUE(demuxer.read(packet, data));
            VideoFrame frame;
            bool got = false;
            const auto start = std::chrono::steady_clock::now();
            EXPECT_TRUE(decoder.decode(data.data(), data.size(), packet.ptsNs, frame, got, error)) << error;
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!got)
                break;
            out.frames.push_back(std::move(frame));
        }
        out.msPerFrame = out.frames.empty() ? 0.0 : seconds * 1000.0 / (double)out.frames.size();
        Av1Decoder::setThreadCountForTesting(0);
        Av1Decoder::setCpuFlagsMaskForTesting(~0u);
        return out;
    };

    std::vector<std::string> clips;
    for (const char* name : {"testcard-noaudio.webm", "testcard-10bit.webm", "testcard-444.webm", "odd-size.webm"})
        clips.push_back(videoFixture(name));
    clips.push_back(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/../../../docs/mcp/guide/html-ui-examples/video_menu/assets/menu-loop.webm");
    if (const char* extra = std::getenv("JASPER_VIDEO_MEASURE")){
        const std::string list = extra;
        for (size_t at = 0; at <= list.size();){
            const size_t end = std::min(list.find(':', at), list.size());
            if (end > at)
                clips.push_back(list.substr(at, end - at));
            at = end + 1;
        }
    }

    for (const std::string& clip : clips){
        const std::string name = clip.substr(clip.find_last_of('/') + 1);
        const Decoded c = decodeFile(clip, 0u, 1);
        const Decoded asmOne = decodeFile(clip, ~0u, 1);
        ASSERT_FALSE(c.frames.empty()) << name;
        ASSERT_EQ(asmOne.frames.size(), c.frames.size()) << name;
        for (size_t i = 0; i < c.frames.size(); i++){
            const VideoFrame& a = asmOne.frames[i];
            const VideoFrame& b = c.frames[i];
            ASSERT_EQ(a.width, b.width) << name << " frame " << i;
            ASSERT_EQ(a.height, b.height) << name << " frame " << i;
            ASSERT_TRUE(a.y == b.y && a.u == b.u && a.v == b.v) << name << ": frame " << i << " differs between the asm and C";
        }
        // the engine's thread count (frame threads change nothing in the output either)
        const Decoded asmMany = decodeFile(clip, ~0u, 0);
        ASSERT_EQ(asmMany.frames.size(), c.frames.size()) << name;
        for (size_t i = 0; i < c.frames.size(); i++){
            ASSERT_TRUE(asmMany.frames[i].y == c.frames[i].y && asmMany.frames[i].u == c.frames[i].u && asmMany.frames[i].v == c.frames[i].v)
                << name << ": frame " << i << " differs (asm, " << asmMany.threads << " threads)";
        }
        std::printf("[asm] %s %dx%d, %zu frames: C %.2f ms/frame, asm %.2f; default (%d threads) %.2f\n",
                    name.c_str(), c.frames[0].width, c.frames[0].height, c.frames.size(), c.msPerFrame, asmOne.msPerFrame,
                    asmMany.threads, asmMany.msPerFrame);
    }
    std::printf("[asm] dav1d CPU features: 0x%x\n", Av1Decoder::getCpuFlags());
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
    // the engine's test builds assemble it (the build images have NASM): the decoder found features
    EXPECT_NE(Av1Decoder::getCpuFlags(), 0u) << "dav1d was built without its asm (CMake: \"dav1d: asm none\"; NASM missing?)";
#endif
}


// ---------------------------------------------------------------- Opus

TEST(OpusAudioDecoder, ToneBurstsAtTheirSamples){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("testcard.webm")));
    OpusAudioDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(demuxer.getAudioTrack(), error)) << error;
    EXPECT_EQ(decoder.getStreamChannels(), 2);
    EXPECT_EQ(decoder.getOutputChannels(), 2);
    EXPECT_EQ(decoder.getPreSkip(), 312);

    std::vector<float> all, pcm;
    std::vector<uint8_t> data;
    for (const VideoPacketRef& packet : demuxer.getAudioPackets()){
        ASSERT_TRUE(demuxer.read(packet, data));
        const int frames = decoder.decode(data.data(), data.size(), pcm, error);
        ASSERT_GT(frames, 0) << error;
        all.insert(all.end(), pcm.begin(), pcm.end());
    }
    // drop the pre-skip: sample 0 is time 0
    all.erase(all.begin(), all.begin() + 2 * decoder.getPreSkip());
    for (int second = 0; second < 2; second++){
        const size_t at = (size_t)second * 48000;
        const int left = onsetIn(all, 2, 0, at, 4800);
        const int right = onsetIn(all, 2, 1, at, 4800);
        EXPECT_GE(left, 0);
        EXPECT_LE(left, 24) << "left burst " << second;     // 0.5 ms
        EXPECT_GE(right, 0);
        EXPECT_LE(right, 24) << "right burst " << second;
        // 1 kHz left, 2 kHz right: 100 and 200 zero crossings in the 50 ms burst
        EXPECT_NEAR(zeroCrossings(all, 2, 0, at + 240, 1920), 80, 3);
        EXPECT_NEAR(zeroCrossings(all, 2, 1, at + 240, 1920), 160, 4);
        // silence after the burst
        EXPECT_EQ(onsetIn(all, 2, 0, at + 4800, 30000), -1);
    }
}

TEST(OpusAudioDecoder, SurroundIsDownmixedToStereo){
    VideoDemuxer demuxer;
    ASSERT_TRUE(demuxer.open(videoFixture("testcard-51.webm")));
    EXPECT_EQ(demuxer.getAudioTrack().channels, 6);
    OpusAudioDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(demuxer.getAudioTrack(), error)) << error;
    EXPECT_EQ(decoder.getStreamChannels(), 6);
    EXPECT_EQ(decoder.getOutputChannels(), 2);
    std::vector<float> pcm;
    std::vector<uint8_t> data;
    float peak = 0.0f;
    for (const VideoPacketRef& packet : demuxer.getAudioPackets()){
        ASSERT_TRUE(demuxer.read(packet, data));
        ASSERT_GT(decoder.decode(data.data(), data.size(), pcm, error), 0) << error;
        for (float s : pcm)
            peak = std::max(peak, std::fabs(s));
    }
    EXPECT_GT(peak, 0.1f);
    EXPECT_LE(peak, 1.0f);
}

TEST(OpusAudioDecoder, RejectsABadHeader){
    AudioTrackInfo track;
    track.codecPrivate = {'N', 'o', 't', 'O', 'p', 'u', 's'};
    OpusAudioDecoder decoder;
    std::string error;
    EXPECT_FALSE(decoder.open(track, error));
    EXPECT_NE(error.find("OpusHead"), std::string::npos);
}

// ---------------------------------------------------------------- the pipeline

TEST(VideoStream, FrameExactSeeksInAnyOrder){
    VideoStream stream(videoFixture("testcard-noaudio.webm"), true, false);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    EXPECT_EQ(stream.getInfo().frameCount, CARD_FRAMES);
    // a fixed shuffle: forwards, backwards, across the keyframe at 30
    int order[CARD_FRAMES];
    for (int i = 0; i < CARD_FRAMES; i++)
        order[i] = (i * 37 + 11) % CARD_FRAMES;
    int64_t shown = -1;
    for (int i = 0; i < CARD_FRAMES; i++){
        const int k = order[i];
        for (double offset : {0.0, 0.5 / CARD_FPS}){
            VideoFrame frame;
            const VideoStream::FrameResult result = stream.acquireFrame(0, cardTime(k) + offset, true, shown, frame);
            if (offset > 0.0 && shown == k){
                EXPECT_EQ(result, VideoStream::FrameResult::SAME);
                continue;
            }
            ASSERT_EQ(result, VideoStream::FrameResult::NEW) << "frame " << k;
            EXPECT_EQ(frame.index, k);
            EXPECT_EQ(cardIndexOf(frame), k);
            shown = frame.sequence;
        }
    }
    // just before a frame's time: the frame before it
    VideoFrame frame;
    ASSERT_EQ(stream.acquireFrame(0, cardTime(45) - 1e-6, true, shown, frame), VideoStream::FrameResult::NEW);
    EXPECT_EQ(cardIndexOf(frame), 44);
    EXPECT_DOUBLE_EQ(stream.keyframeTimeAtOrBefore(cardTime(45)), 1.0);
    EXPECT_DOUBLE_EQ(stream.keyframeTimeAtOrBefore(cardTime(29)), 0.0);
}

TEST(VideoStream, PlaybackInOrderNeverSeeks){
    VideoStream stream(videoFixture("testcard-noaudio.webm"), true, false);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    int64_t shown = -1;
    for (int k = 0; k < CARD_FRAMES; k++){
        VideoFrame frame;
        ASSERT_EQ(stream.acquireFrame(0, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
        EXPECT_EQ(cardIndexOf(frame), k);
        shown = frame.sequence;
    }
    const VideoStream::Stats stats = stream.getStats();
    EXPECT_EQ(stats.seeks, 0u);
    EXPECT_EQ(stats.framesDecoded, (uint64_t)CARD_FRAMES);
    EXPECT_EQ(stats.framesDropped, 0u);
}

TEST(VideoStream, LoopIsOneContinuousStream){
    VideoStream stream(videoFixture("testcard-noaudio.webm"), true, false);
    stream.setLoop(true);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    int64_t shown = -1;
    for (int iteration = 0; iteration < 3; iteration++){
        for (int k = 0; k < CARD_FRAMES; k++){
            VideoFrame frame;
            ASSERT_EQ(stream.acquireFrame(iteration, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
            EXPECT_EQ(frame.sequence, (int64_t)iteration * CARD_FRAMES + k);
            EXPECT_EQ(cardIndexOf(frame), k);
            shown = frame.sequence;
        }
    }
    EXPECT_EQ(stream.getStats().seeks, 0u);
}

TEST(VideoStream, LoopTurnedOnAtTheEndGoesOn){
    VideoStream stream(videoFixture("testcard-noaudio.webm"), true, false);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    int64_t shown = -1;
    VideoFrame frame;
    for (int k = 0; k < CARD_FRAMES; k++){
        ASSERT_EQ(stream.acquireFrame(0, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
        shown = frame.sequence;
    }
    stream.setLoop(true);
    for (int k = 0; k < 5; k++){
        ASSERT_EQ(stream.acquireFrame(1, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
        EXPECT_EQ(frame.sequence, CARD_FRAMES + k);
        EXPECT_EQ(cardIndexOf(frame), k);
        shown = frame.sequence;
    }
}

TEST(VideoStream, InlineModeGivesTheSameFrames){
    VideoStream stream(videoFixture("testcard-noaudio.webm"), false, false);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    int64_t shown = -1;
    for (int k : {0, 5, 59, 31, 30, 29, 2}){
        VideoFrame frame;
        ASSERT_EQ(stream.acquireFrame(0, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
        EXPECT_EQ(cardIndexOf(frame), k);
        shown = frame.sequence;
    }
    // real time inline: a call does at most a little work and never waits
    VideoFrame frame;
    const VideoStream::FrameResult result = stream.acquireFrame(0, cardTime(50), false, shown, frame);
    EXPECT_TRUE(result == VideoStream::FrameResult::NOT_READY || result == VideoStream::FrameResult::STALE ||
                result == VideoStream::FrameResult::NEW);
}

TEST(VideoStream, RealTimeNeverBlocks){
    // the decoder needs 200 ms a frame (5 fps) against the clock's 30 frames a second: it is behind
    // from the second frame on, whatever the machine's load (a slower machine only widens the gap)
    VideoStream::setDecodeDelayForTesting(200);
    {
        VideoStream stream(videoFixture("testcard-noaudio.webm"), true, false);
        ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
        int64_t shown = -1;
        int newest = -1;
        bool late = false;
        const auto start = std::chrono::steady_clock::now();
        for (int step = 0; step < 60; step++){
            // the clock runs at 60 fps
            const double t = step / 60.0;
            VideoFrame frame;
            const auto before = std::chrono::steady_clock::now();
            const VideoStream::FrameResult result = stream.acquireFrame(0, t, false, shown, frame);
            const auto took = std::chrono::steady_clock::now() - before;
            EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), 20) << "a real-time request blocked";
            if (result == VideoStream::FrameResult::NEW || result == VideoStream::FrameResult::STALE){
                EXPECT_GT(frame.sequence, shown);
                shown = frame.sequence;
                newest = cardIndexOf(frame);
                if (result == VideoStream::FrameResult::STALE)
                    late = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        (void)start;
        EXPECT_GT(newest, 0);
        // behind: an older frame was shown late, or the decoder skipped ahead to a keyframe
        EXPECT_TRUE(late || stream.getStats().seeks > 0);
    }
    VideoStream::setDecodeDelayForTesting(0);
}

TEST(VideoStream, Errors){
    {
        VideoStream stream(videoFixture("missing.webm"), true, false);
        EXPECT_EQ(stream.waitOpen(), VideoStream::State::FAILED);
        EXPECT_EQ(stream.getError().code, MediaErrorCode::SRC_NOT_SUPPORTED);
        VideoFrame frame;
        EXPECT_EQ(stream.acquireFrame(0, 0.0, true, -1, frame), VideoStream::FrameResult::FAILED);
    }
    {
        VideoStream stream(videoFixture("testcard-vp9.webm"), true, false);
        EXPECT_EQ(stream.waitOpen(), VideoStream::State::FAILED);
        EXPECT_NE(stream.getError().message.find("V_VP9"), std::string::npos);
    }
    {
        VideoStream stream(videoFixture("corrupt.webm"), true, false);
        ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
        int64_t shown = -1;
        for (int k = 0; k < 40; k++){
            VideoFrame frame;
            ASSERT_EQ(stream.acquireFrame(0, cardTime(k), true, shown, frame), VideoStream::FrameResult::NEW);
            EXPECT_EQ(cardIndexOf(frame), k);
            shown = frame.sequence;
        }
        VideoFrame frame;
        EXPECT_EQ(stream.acquireFrame(0, cardTime(40), true, shown, frame), VideoStream::FrameResult::FAILED);
        EXPECT_EQ(stream.getError().code, MediaErrorCode::DECODE);
        EXPECT_NE(stream.getError().message.find("frame 40"), std::string::npos) << stream.getError().message;
    }
}

TEST(VideoStream, AudioRingFollowsTheMediaClock){
    VideoStream stream(videoFixture("testcard.webm"), true, true);
    ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
    std::shared_ptr<VideoAudioRing> ring = stream.getAudioRing();
    ASSERT_TRUE(ring);
    EXPECT_EQ(ring->getChannels(), 2);
    // let the worker fill it, then read 2 s in 10 ms blocks as a mixer does
    std::vector<float> all, block(480 * 2);
    for (int i = 0; i < 200; i++){
        for (int wait = 0; wait < 500 && ring->getBufferedFrames() < 480 && ring->getWritePosition() < 96000; wait++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ring->read(block.data(), 480);
        all.insert(all.end(), block.begin(), block.end());
    }
    EXPECT_EQ(ring->getReadPosition(), 96000);
    EXPECT_LE(onsetIn(all, 2, 0, 0, 4800), 24);
    EXPECT_LE(onsetIn(all, 2, 0, 48000, 4800), 24);
    EXPECT_GE(onsetIn(all, 2, 0, 48000, 4800), 0);

    // from 0.5 s: the next burst is 0.5 s into what is read
    stream.syncAudio(24000);
    for (int wait = 0; wait < 2000 && ring->getReadPosition() != 24000; wait++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(ring->getReadPosition(), 24000);
    all.clear();
    for (int i = 0; i < 60; i++){
        for (int wait = 0; wait < 500 && ring->getBufferedFrames() < 480; wait++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ring->read(block.data(), 480);
        all.insert(all.end(), block.begin(), block.end());
    }
    // After a sync the first packet is placed by its block timestamp, which WebM keeps in
    // milliseconds (FFmpeg's muxer also rounds pts minus the codec delay): up to about 1 ms off,
    // then samples are counted. From the start (above) the placement is exact.
    const int onset = onsetIn(all, 2, 0, 0, 28800);
    EXPECT_GE(onset, 24000 - 96);
    EXPECT_LE(onset, 24000 + 96);
    RecordProperty("sync_placement_frames", onset - 24000);
}


// The cost and memory of a played clip (the phase record's numbers): the test card (320x180), the
// video_menu example's loop (640x360) and any clips listed in JASPER_VIDEO_MEASURE (paths separated
// by ':', e.g. 720p and 1080p encodes made for a measurement). Each is played frame by frame on the
// worker in frame-exact mode; the decoder's time per frame, its picture pool and the queue are
// recorded. The queue never holds more than QUEUE_FRAMES packed frames, and everything is freed.
TEST(VideoStream, CostAndMemoryPerResolution){
    std::vector<std::string> clips = {videoFixture("testcard-noaudio.webm"),
                                      std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/../../../docs/mcp/guide/html-ui-examples/video_menu/assets/menu-loop.webm"};
    if (const char* extra = std::getenv("JASPER_VIDEO_MEASURE")){
        std::string list = extra;
        for (size_t at = 0; at <= list.size();){
            const size_t end = std::min(list.find(':', at), list.size());
            if (end > at)
                clips.push_back(list.substr(at, end - at));
            at = end + 1;
        }
    }
    const size_t livePictures = Av1Decoder::getLivePictureBytes();
    for (const std::string& clip : clips){
        size_t peakPictures = 0, peakQueue = 0, frameBytes = 0;
        double msPerFrame = 0.0;
        int width = 0, height = 0, threads = 0, frames = 0;
        {
            VideoStream stream(clip, true, false);
            ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY) << clip << ": " << stream.getError().message;
            const VideoStream::Info& info = stream.getInfo();
            width = info.width;
            height = info.height;
            threads = info.decoderThreads;
            frames = info.frameCount;
            const double fps = info.frameCount / info.duration;
            int64_t shown = -1;
            for (int i = 0; i < info.frameCount; i++){
                VideoFrame frame;
                ASSERT_EQ(stream.acquireFrame(0, (i + 0.5) / fps, true, shown, frame), VideoStream::FrameResult::NEW) << clip << " frame " << i;
                shown = frame.sequence;
                frameBytes = frame.bytes();
                const VideoStream::Stats stats = stream.getStats();
                peakPictures = std::max(peakPictures, stats.pictureBytes);
                peakQueue = std::max(peakQueue, stats.queueBytes);
            }
            const VideoStream::Stats stats = stream.getStats();
            EXPECT_EQ(stats.seeks, 0u) << clip << ": played in order";
            EXPECT_EQ(stats.framesDropped, 0u) << clip;
            msPerFrame = stats.framesDecoded ? stats.decodeNs / 1e6 / (double)stats.framesDecoded : 0.0;
        }
        // 4:2:0 8-bit packed: 1.5 bytes a pixel
        EXPECT_EQ(frameBytes, (size_t)width * height + 2 * (size_t)((width + 1) / 2) * ((height + 1) / 2)) << clip;
        EXPECT_LE(peakQueue, VideoStream::QUEUE_FRAMES * frameBytes) << clip;
        EXPECT_GT(peakPictures, 0u) << clip;
        const std::string name = clip.substr(clip.find_last_of('/') + 1);
        std::printf("[measure] %s %dx%d, %d frames, %d decoder thread(s): %.2f ms/frame, pictures %.2f MiB, queue %.2f MiB, frame %.2f MiB\n",
                    name.c_str(), width, height, frames, threads, msPerFrame, peakPictures / 1048576.0, peakQueue / 1048576.0,
                    frameBytes / 1048576.0);
        ::testing::Test::RecordProperty(name + "_ms_per_frame", std::to_string(msPerFrame));
        ::testing::Test::RecordProperty(name + "_picture_bytes", std::to_string(peakPictures));
    }
    EXPECT_EQ(Av1Decoder::getLivePictureBytes(), livePictures);
}

// ---------------------------------------------------------------- playback state machine

TEST(VideoPlayback, PlayPauseAndTime){
    VideoPlayback playback;
    std::vector<Ev> events;
    EXPECT_TRUE(std::isnan(playback.getDuration()));
    playback.setDuration(2.0);
    EXPECT_TRUE(playback.isPaused());
    playback.update(5.0, events);
    EXPECT_EQ(playback.getPosition(), 0.0);
    playback.play(10.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::PLAY}));
    events.clear();
    playback.notifyReady(10.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::PLAYING}));
    events.clear();
    playback.notifyReady(10.0, events);
    EXPECT_TRUE(events.empty());
    playback.update(10.1, events);
    EXPECT_NEAR(playback.getPosition(), 0.1, 1e-12);
    EXPECT_TRUE(events.empty());
    playback.update(10.25, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::TIMEUPDATE}));
    events.clear();
    playback.pause(10.5, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::TIMEUPDATE, Ev::PAUSE}));
    EXPECT_NEAR(playback.getPosition(), 0.5, 1e-12);
    events.clear();
    playback.update(20.0, events);
    EXPECT_NEAR(playback.getPosition(), 0.5, 1e-12);
    playback.play(30.0, events);
    playback.update(30.25, events);
    EXPECT_NEAR(playback.getPosition(), 0.75, 1e-12);
}

TEST(VideoPlayback, EndAndPlayAgain){
    VideoPlayback playback;
    std::vector<Ev> events;
    playback.setDuration(2.0);
    playback.play(0.0, events);
    playback.notifyReady(0.0, events);
    events.clear();
    playback.update(2.5, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::TIMEUPDATE, Ev::PAUSE, Ev::ENDED}));
    EXPECT_TRUE(playback.isEnded());
    EXPECT_TRUE(playback.isPaused());
    EXPECT_EQ(playback.getPosition(), 2.0);
    events.clear();
    playback.play(3.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::SEEKING, Ev::PLAY}));
    EXPECT_EQ(playback.getPosition(), 0.0);
    EXPECT_FALSE(playback.isEnded());
    events.clear();
    playback.completeSeek(3.0, events);
    playback.notifyReady(3.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::TIMEUPDATE, Ev::SEEKED, Ev::PLAYING}));
}

TEST(VideoPlayback, LoopWraps){
    VideoPlayback playback;
    std::vector<Ev> events;
    playback.setDuration(2.0);
    playback.setLoop(true);
    playback.play(0.0, events);
    events.clear();
    playback.update(2.5, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::SEEKING, Ev::TIMEUPDATE, Ev::SEEKED}));
    EXPECT_NEAR(playback.getPosition(), 0.5, 1e-12);
    EXPECT_EQ(playback.getIteration(), 1);
    EXPECT_FALSE(playback.isEnded());
    EXPECT_FALSE(playback.isPaused());
    events.clear();
    playback.update(7.0, events);   // two more wraps in one step
    EXPECT_NEAR(playback.getPosition(), 1.0, 1e-12);
    EXPECT_EQ(playback.getIteration(), 3);
    // a seek starts the count again
    playback.seek(1.5, 7.0, events);
    EXPECT_EQ(playback.getIteration(), 0);
}

TEST(VideoPlayback, SeekClampsAndHoldsTheClock){
    VideoPlayback playback;
    std::vector<Ev> events;
    playback.setDuration(2.0);
    playback.play(0.0, events);
    playback.notifyReady(0.0, events);
    events.clear();
    playback.seek(5.0, 1.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::SEEKING}));
    EXPECT_EQ(playback.getPosition(), 2.0);
    EXPECT_TRUE(playback.isSeeking());
    playback.update(1.5, events);
    EXPECT_EQ(playback.getPosition(), 2.0);
    events.clear();
    // completing a seek to the end of media that does not loop ends it
    playback.completeSeek(1.5, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::TIMEUPDATE, Ev::SEEKED, Ev::PAUSE, Ev::ENDED}));
    playback.seek(-1.0, 2.0, events);
    EXPECT_EQ(playback.getPosition(), 0.0);
    // before the metadata the position is kept as the start position
    VideoPlayback early;
    early.seek(1.25, 0.0, events);
    EXPECT_EQ(early.getPosition(), 1.25);
}

TEST(VideoPlayback, Rates){
    VideoPlayback playback;
    std::vector<Ev> events;
    playback.setDuration(10.0);
    EXPECT_FALSE(playback.setPlaybackRate(-1.0, 0.0, events));
    EXPECT_FALSE(playback.setPlaybackRate(17.0, 0.0, events));
    EXPECT_FALSE(playback.setPlaybackRate(0.01, 0.0, events));
    EXPECT_FALSE(playback.setPlaybackRate(std::nan(""), 0.0, events));
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(playback.setPlaybackRate(2.0, 0.0, events));
    EXPECT_EQ(events, (std::vector<Ev>{Ev::RATECHANGE}));
    events.clear();
    EXPECT_TRUE(playback.setPlaybackRate(2.0, 0.0, events));
    EXPECT_TRUE(events.empty());
    playback.play(0.0, events);
    playback.update(1.0, events);
    EXPECT_NEAR(playback.getPosition(), 2.0, 1e-12);
    ASSERT_TRUE(playback.setPlaybackRate(0.0, 1.0, events));
    playback.update(2.0, events);
    EXPECT_NEAR(playback.getPosition(), 2.0, 1e-12);
    EXPECT_FALSE(playback.isPaused());
    ASSERT_TRUE(playback.setPlaybackRate(0.5, 2.0, events));
    playback.update(3.0, events);
    EXPECT_NEAR(playback.getPosition(), 2.5, 1e-12);
    // the default rate comes back at the next resource
    ASSERT_TRUE(playback.setDefaultPlaybackRate(1.5, events));
    playback.reset();
    EXPECT_EQ(playback.getPlaybackRate(), 1.5);
}

TEST(VideoPlayback, StallStopsTheClock){
    VideoPlayback playback;
    std::vector<Ev> events;
    playback.setDuration(10.0);
    playback.play(0.0, events);
    playback.notifyReady(0.0, events);
    events.clear();
    playback.setStalled(true, 1.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::WAITING}));
    playback.update(3.0, events);
    EXPECT_NEAR(playback.getPosition(), 1.0, 1e-12);
    events.clear();
    playback.setStalled(false, 3.0, events);
    playback.notifyReady(3.0, events);
    EXPECT_EQ(events, (std::vector<Ev>{Ev::PLAYING}));
    playback.update(3.5, events);
    EXPECT_NEAR(playback.getPosition(), 1.5, 1e-12);
}

// ---------------------------------------------------------------- audio ring and sync

TEST(VideoAudioRing, PositionsSilenceAndCorrections){
    VideoAudioRing ring(2, 1000);
    std::vector<float> data(200 * 2, 0.5f), out(100 * 2);
    EXPECT_EQ(ring.write(data.data(), 200), 200u);
    EXPECT_EQ(ring.getWritePosition(), 200);
    ring.read(out.data(), 100);
    EXPECT_EQ(out[0], 0.5f);
    EXPECT_EQ(ring.getReadPosition(), 100);
    // insert 50 frames of silence: the position stands while they play
    ring.requestCorrection(50);
    ring.read(out.data(), 100);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[2 * 49 + 1], 0.0f);
    EXPECT_EQ(out[2 * 50], 0.5f);
    EXPECT_EQ(ring.getReadPosition(), 150);
    // drop 30: the position jumps
    ring.requestCorrection(-30);
    ring.read(out.data(), 10);
    EXPECT_EQ(ring.getReadPosition(), 190);
    // nothing left: silence, and the position moves on
    ring.read(out.data(), 100);
    EXPECT_EQ(out[2 * 50], 0.0f);
    EXPECT_EQ(ring.getReadPosition(), 290);
    EXPECT_EQ(ring.getUnderrunFrames(), 90u);
    // late data for positions already read is dropped
    EXPECT_EQ(ring.write(data.data(), 200), 200u);
    EXPECT_EQ(ring.getBufferedFrames(), 110u);
    ring.reset(5000);
    EXPECT_EQ(ring.getReadPosition(), 5000);
    EXPECT_EQ(ring.getBufferedFrames(), 0u);
}

TEST(VideoAudioSync, Decisions){
    using A = VideoAudioSync::Action;
    EXPECT_EQ(VideoAudioSync::decide(48000, 0, 48000, 0).action, A::NONE);
    EXPECT_EQ(VideoAudioSync::decide(48000 + 700, 0, 48000, 0).action, A::NONE);    // 14.6 ms
    VideoAudioSync::Decision ahead = VideoAudioSync::decide(48000 + 1440, 0, 48000, 0);  // 30 ms ahead
    EXPECT_EQ(ahead.action, A::CORRECT);
    EXPECT_EQ(ahead.frames, 1440);
    EXPECT_NEAR(ahead.drift, 0.03, 1e-9);
    VideoAudioSync::Decision behind = VideoAudioSync::decide(48000 - 1440, 0, 48000, 0);
    EXPECT_EQ(behind.frames, -1440);
    // the latency counts: what is heard lags what was read
    EXPECT_EQ(VideoAudioSync::decide(48000 + 1024, 0, 48000, 1024).action, A::NONE);
    // a pending correction counts as done
    EXPECT_EQ(VideoAudioSync::decide(48000 + 1440, 1440, 48000, 0).action, A::NONE);
    VideoAudioSync::Decision far = VideoAudioSync::decide(0, 0, 48000, 512);
    EXPECT_EQ(far.action, A::RESYNC);
    EXPECT_EQ(far.frames, 48000 + 512);
}

// A/V sync on the fixture clock: the media clock steps 1/60 s, a consumer reads 10 ms blocks at
// its own rate (exact, 3 % fast, 3 % slow), the controller corrects. The drift (what is read
// against the media clock) stays within one block, or one block plus the dead band.
TEST(VideoAudioSync, DriftStaysBoundedOnTheFixtureClock){
    for (double speed : {1.0, 1.03, 0.97}){
        VideoStream stream(videoFixture("testcard.webm"), true, true);
        stream.setLoop(true);
        ASSERT_EQ(stream.waitOpen(), VideoStream::State::READY);
        std::shared_ptr<VideoAudioRing> ring = stream.getAudioRing();
        ASSERT_TRUE(ring);
        std::vector<float> block(480 * 2);
        double owed = 0.0;
        double worst = 0.0;
        for (int step = 1; step <= 600; step++){           // 10 s on the fixture clock
            const double media = step / 60.0;
            owed += 800.0 * speed;                         // 48000 / 60 frames at the consumer's speed
            while (owed >= 480.0){
                for (int wait = 0; wait < 500 && ring->getBufferedFrames() < 480; wait++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ring->read(block.data(), 480);
                owed -= 480.0;
            }
            const int64_t mediaFrames = (int64_t)std::llround(media * 48000.0);
            const VideoAudioSync::Decision decision = VideoAudioSync::decide(ring->getReadPosition(), ring->getPendingCorrection(), mediaFrames, 0);
            if (decision.action == VideoAudioSync::Action::CORRECT)
                ring->requestCorrection(decision.frames);
            else if (decision.action == VideoAudioSync::Action::RESYNC)
                stream.syncAudio(decision.frames);
            const double drift = (double)(ring->getReadPosition() - mediaFrames) / 48000.0;
            if (step > 1)
                worst = std::max(worst, std::fabs(drift));
            EXPECT_NE(decision.action, VideoAudioSync::Action::RESYNC) << "speed " << speed << " step " << step;
        }
        const double bound = speed == 1.0 ? 0.0101 : 0.025;
        EXPECT_LE(worst, bound) << "speed " << speed;
        RecordProperty(speed == 1.0 ? "drift_exact_ms" : (speed > 1.0 ? "drift_fast_ms" : "drift_slow_ms"), (int)std::lround(worst * 1000.0));
    }
}
