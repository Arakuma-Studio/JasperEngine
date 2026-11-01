// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Recorder's pure parts (docs/specs/S27-editor-tools.md R24-R30)

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
