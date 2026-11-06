// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Recorder's pure parts (docs/specs/S27-editor-tools.md R24-R30)

#include "audio/AudioClock.h"
#include "recorder/RecorderSettings.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine::editor;
using namespace jasperengine::editor::recorder;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(RecorderSettings, FrameRanges) {
    Settings s;
    int64_t first, last;
    frameRange(s, first, last);
    EXPECT_EQ(first, 0);
    EXPECT_EQ(last, -1);   // manual: until stopped
    s.mode = Mode::SingleFrame;
    s.singleFrame = 12;
    frameRange(s, first, last);
    EXPECT_EQ(first, 12);
    EXPECT_EQ(last, 12);
    s.mode = Mode::FrameInterval;
    s.frameStart = 30;
    s.frameEnd = 59;
    frameRange(s, first, last);
    EXPECT_EQ(last - first + 1, 30);
    // [1 s, 2 s) at 30 fps: frames 30-59
    s.mode = Mode::TimeInterval;
    s.timeStart = 1.0f;
    s.timeEnd = 2.0f;
    s.frameRate = 30;
    frameRange(s, first, last);
    EXPECT_EQ(first, 30);
    EXPECT_EQ(last, 59);
}

TEST(RecorderSettings, OutputSizes) {
    Settings s;
    int w, h;
    s.resolution = Resolution::P720;
    outputSize(s, 100, 100, 0, w, h);
    EXPECT_EQ(w, 1280);
    EXPECT_EQ(h, 720);
    s.resolution = Resolution::MatchGameView;
    s.kind = Kind::Movie;
    outputSize(s, 641, 361, 0, w, h);
    EXPECT_EQ(w, 640);   // even for 4:2:0
    EXPECT_EQ(h, 360);
    s.kind = Kind::ImageSequence;
    outputSize(s, 641, 361, 0, w, h);
    EXPECT_EQ(w, 641);
    s.resolution = Resolution::Custom;
    s.customWidth = 20000;
    s.customHeight = 4;
    outputSize(s, 0, 0, 4096, w, h);
    EXPECT_EQ(w, 4096);   // the device's largest texture
    EXPECT_EQ(h, kMinSize);
}

TEST(RecorderSettings, Names) {
    Settings s;
    NameContext c;
    c.scene = "Level1";
    c.take = 7;
    c.date = "2026-10-30";
    c.time = "14-05-09";
    c.width = 640;
    c.height = 360;
    EXPECT_EQ(expandName("<Scene>_<Take>", c), "Level1_007");
    c.frame = 42;
    EXPECT_EQ(expandName("<Scene>_<Take>/frame_<Frame>", c), "Level1_007/frame_0042");
    EXPECT_EQ(expandName("<Date> <Time> <Resolution>", c), "2026-10-30 14-05-09 640x360");
    const fs::path project = "/game";
    EXPECT_EQ(outputFile(s, project, c), fs::path("/game/Recordings/Level1_007/frame_0042.png"));
    s.kind = Kind::Movie;
    c.frame = -1;
    EXPECT_EQ(outputFile(s, project, c), fs::path("/game/Recordings/Level1_007.mp4"));
    s.format = Format::Mov;
    s.outputFolder = "/out";
    EXPECT_EQ(outputFile(s, project, c), fs::path("/out/Level1_007.mov"));
}

TEST(RecorderSettings, Encoders) {
    const std::string output =
        "Encoders:\n V..... = Video\n ------\n V....D a64multi             Multicolor charset for Commodore 64\n"
        " V....D libopenh264          OpenH264 H.264 / AVC\n V....D h264_nvenc           NVIDIA NVENC H.264 encoder\n"
        " V..... prores_ks            Apple ProRes (iCodec Pro)\n A....D aac                  AAC\n";
    const std::vector<std::string> encoders = parseEncoders(output);
    EXPECT_EQ(encoders.size(), 5u);
    EXPECT_EQ(pickH264(encoders), "h264_nvenc");   // before libopenh264 in the preference list
    EXPECT_TRUE(hasProRes(encoders));
    EXPECT_EQ(pickH264({"libx264", "h264_nvenc"}), "libx264");
    EXPECT_EQ(pickH264({"aac"}), "");
}

TEST(RecorderSettings, FfmpegArguments) {
    const std::vector<std::string> x264 = movieArguments("libx264", Format::Mp4, Quality::Medium, 640, 360, 30, "/o/a.mp4");
    auto has = [](const std::vector<std::string>& args, const std::string& a, const std::string& b) {
        for (size_t i = 0; i + 1 < args.size(); i++) {
            if (args[i] == a && args[i + 1] == b) return true;
        }
        return false;
    };
    EXPECT_TRUE(has(x264, "-f", "rawvideo"));
    EXPECT_TRUE(has(x264, "-pix_fmt", "rgba"));
    EXPECT_TRUE(has(x264, "-s", "640x360"));
    EXPECT_TRUE(has(x264, "-i", "pipe:0"));
    EXPECT_TRUE(has(x264, "-crf", "23"));
    EXPECT_TRUE(has(x264, "-movflags", "+faststart"));
    EXPECT_TRUE(has(x264, "-colorspace", "bt709"));
    EXPECT_TRUE(has(x264, "-pix_fmt", "yuv420p"));
    EXPECT_EQ(x264.back(), "/o/a.mp4");
    const std::vector<std::string> nvenc = movieArguments("h264_nvenc", Format::Mp4, Quality::High, 100, 100, 10, "/o/a.mp4");
    EXPECT_TRUE(has(nvenc, "-b:v", "20000"));   // 100 x 100 x 10 x 0.2
    const std::vector<std::string> prores = movieArguments("", Format::Mov, Quality::High, 64, 64, 24, "/o/a.mov");
    EXPECT_TRUE(has(prores, "-c:v", "prores_ks"));
    EXPECT_TRUE(has(prores, "-pix_fmt", "yuv422p10le"));
    const std::vector<std::string> mux = muxArguments(Format::Mp4, "/o/v.mp4", "/o/a.wav", "/o/out.mp4");
    EXPECT_TRUE(has(mux, "-c:v", "copy"));
    EXPECT_TRUE(has(mux, "-c:a", "aac"));
    EXPECT_TRUE(has(mux, "-b:a", "192k"));
    EXPECT_TRUE(has(muxArguments(Format::Mov, "v", "a", "o"), "-c:a", "pcm_s16le"));
    // the outputs have temporary names: the container is named
    EXPECT_TRUE(has(x264, "-f", "mp4"));
    EXPECT_TRUE(has(prores, "-f", "mov"));
    EXPECT_TRUE(has(mux, "-f", "mp4"));
    EXPECT_TRUE(has(muxArguments(Format::Mov, "v", "a", "o"), "-f", "mov"));
}

// what the deterministic driver's pump mixes in a game frame is what the Recorder writes for it
TEST(RecorderSettings, FramesMatchAudioClock) {
    for (int fps : {24, 30, 60, 144}) {
        jasperengine::AudioClock clock;
        clock.reset((unsigned int)kAudioRate);
        for (int64_t k = 0; k < fps * 600; k++) {
            ASSERT_EQ(clock.advance(1.0 / fps), audioFramesOf(k, fps)) << fps << " fps, frame " << k;
        }
    }
}

TEST(RecorderSettings, DecodeCheck) {
    const std::vector<std::string> args = decodeCheckArguments("/o/a.mp4");
    EXPECT_NE(std::find(args.begin(), args.end(), "/o/a.mp4"), args.end());
    EXPECT_EQ(args.back(), "-");
    EXPECT_EQ(parseDecodedFrames("frame=12\nfps=0.0\nprogress=continue\nframe=90\nfps=0.0\nprogress=end\n"), 90);
    EXPECT_EQ(parseDecodedFrames("progress=end\n"), -1);
    EXPECT_EQ(parseDecodedFrames(""), -1);
}

TEST(RecorderSettings, PartialPath) {
    EXPECT_EQ(partialPath("/o/take.mp4").generic_string(), "/o/take.partial.mp4");
    EXPECT_EQ(partialPath("/o/my.take.mov").generic_string(), "/o/my.take.partial.mov");
}

TEST(RecorderSettings, Estimates) {
    Settings settings;
    settings.frameRate = 30;
    settings.kind = Kind::ImageSequence;
    // 100 frames of RGBA at a third
    EXPECT_NEAR((double)estimateRecordingBytes(settings, 1000, 1000, 100), 100 * 1000 * 1000 * 4 * 0.35, 1000.0);
    settings.kind = Kind::Movie;
    settings.format = Format::Mp4;
    settings.quality = Quality::High;
    const uint64_t oneMinute = estimateRecordingBytes(settings, 1920, 1080, 1800);
    EXPECT_EQ(estimateRecordingBytes(settings, 1920, 1080, -1), oneMinute) << "no set end: its first minute";
    EXPECT_GT(oneMinute, 50ull * 1024 * 1024);
    EXPECT_LT(oneMinute, 500ull * 1024 * 1024);
    settings.quality = Quality::Low;
    EXPECT_LT(estimateRecordingBytes(settings, 1920, 1080, 1800), oneMinute);
    settings.format = Format::Mov;
    EXPECT_GT(estimateRecordingBytes(settings, 1920, 1080, 1800), oneMinute * 5) << "ProRes 422 HQ";
    settings.format = Format::Mp4;
    settings.quality = Quality::High;
    settings.captureAudio = true;
    // the video twice (its temporary and the mux), the WAV of 60 s and the AAC of 60 s
    EXPECT_NEAR((double)estimateRecordingBytes(settings, 1920, 1080, 1800), 2.0 * oneMinute + 60 * 192000 + 60 * 24000, 1000.0);
}

TEST(RecorderSettings, SizesAndReasons) {
    EXPECT_EQ(formatSize(950 * 1024), "950 KB");
    EXPECT_EQ(formatSize(340ull * 1024 * 1024), "340 MB");
    EXPECT_EQ(formatSize(1288490189ull), "1.2 GB");
    EXPECT_EQ(diskFullReason(3ull * 1024 * 1024 + 5), "disk full, 3 MB free");
    EXPECT_EQ(diskFullReason(0), "disk full, 0 MB free");
    EXPECT_TRUE(isDiskFullMessage("av_interleaved_write_frame(): No space left on device"));
    EXPECT_TRUE(isDiskFullMessage("Error writing trailer: There is not enough space on the disk"));
    EXPECT_TRUE(isDiskFullMessage("write failed: Disk quota exceeded"));
    EXPECT_FALSE(isDiskFullMessage("Invalid data found when processing input"));
}

TEST(RecorderSettings, TestFaults) {
    const TestFaults none = parseTestFaults(nullptr);
    EXPECT_EQ(none.audioWrite, -1);
    EXPECT_EQ(none.videoWrite, -1);
    EXPECT_EQ(none.encoderKill, -1);
    EXPECT_EQ(none.pngWrite, -1);
    EXPECT_EQ(none.holdLimitMs, -1);
    EXPECT_EQ(none.freeBytes, -1);
    EXPECT_FALSE(none.audioHeader || none.mux || none.publish);
    const TestFaults many = parseTestFaults("audio@5,video@7,encoder-kill@8,png@9,audio-header,mux,publish,hold-limit=1500,free=2");
    EXPECT_EQ(many.audioWrite, 5);
    EXPECT_EQ(many.videoWrite, 7);
    EXPECT_EQ(many.encoderKill, 8);
    EXPECT_EQ(many.pngWrite, 9);
    EXPECT_TRUE(many.audioHeader && many.mux && many.publish);
    EXPECT_EQ(many.holdLimitMs, 1500);
    EXPECT_EQ(many.freeBytes, 2 * 1024 * 1024);
}

TEST(RecorderSettings, AudioFramesSumExactly) {
    // 90 frames at 30 fps: exactly 3 s of 48 kHz, whatever the rounding per frame
    for (int fps : {24, 25, 30, 60, 7}) {
        uint64_t total = 0;
        for (int64_t k = 0; k < fps * 3; k++) total += audioFramesOf(k, fps);
        EXPECT_EQ(total, (uint64_t)(3 * kAudioRate)) << fps;
    }
    EXPECT_EQ(audioFramesOf(0, 30), 1600u);
}

TEST(RecorderSettings, Persist) {
    TestTempDir dir("recorder-settings");
    Settings s;
    s.kind = Kind::Movie;
    s.resolution = Resolution::Custom;
    s.customWidth = 800;
    s.frameRate = 60;
    s.mode = Mode::TimeInterval;
    s.timeEnd = 3.5f;
    s.take = 9;
    s.format = Format::Mov;
    s.captureAudio = true;
    s.movieName = "<Scene>-<Date>";
    const fs::path file = settingsFile(dir.path());
    EXPECT_EQ(file, dir.path() / ".jasperengine" / "user" / "recorder.yaml");
    std::string error;
    ASSERT_TRUE(save(file, s, error)) << error;
    Settings read;
    ASSERT_TRUE(load(file, read, error)) << error;
    EXPECT_TRUE(read == s);
    // a missing file: the defaults
    Settings defaults;
    ASSERT_TRUE(load(dir.path() / "none.yaml", defaults, error));
    EXPECT_TRUE(defaults == Settings());
}
