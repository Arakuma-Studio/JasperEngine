// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Recorder's settings and the pure parts of a recording (docs/specs/S27-editor-tools.md
// R24-R30): Unity Recorder's options (Image Sequence / Movie, Output Resolution, Frame Rate,
// Recording Mode, Exit Play Mode, Output and file name wildcards, Take), stored per project in
// .jasperengine/user/recorder.yaml; the output size, the recorded frame range, the file names,
// the H.264 encoder pick and the FFmpeg command lines (the editor pipes raw frames to the user's
// FFmpeg executable: no libav is linked and no FFmpeg ships). The resolution and bitrate presets
// follow OpenEngine's MovieRecorderController.cpp and the encoder preference list its
// VideoWriterFFmpeg.cpp (MIT, NOTICE.md).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::recorder {

    enum class Kind : uint8_t { ImageSequence, Movie };
    enum class Resolution : uint8_t { MatchGameView, P480, P720, P1080, P1440, P2160, Custom };
    enum class Mode : uint8_t { Manual, SingleFrame, FrameInterval, TimeInterval };
    enum class Format : uint8_t { Mp4, Mov };
    enum class Quality : uint8_t { Low, Medium, High };

    struct Settings {
        Kind kind = Kind::ImageSequence;
        Resolution resolution = Resolution::P1080;
        int customWidth = 1920;
        int customHeight = 1080;
        int frameRate = 30;               // 24, 25, 30, 50, 60 or a custom 1-240
        Mode mode = Mode::Manual;
        int singleFrame = 0;
        int frameStart = 0;
        int frameEnd = 99;
        float timeStart = 0.0f;
        float timeEnd = 5.0f;
        bool exitPlayMode = true;
        bool recordOnPlay = false;
        std::string outputFolder = "Recordings";   // relative to the project, or absolute
        std::string movieName = "<Scene>_<Take>";
        std::string sequenceName = "<Scene>_<Take>/frame_<Frame>";
        int take = 1;
        Format format = Format::Mp4;
        Quality quality = Quality::High;
        bool captureAudio = false;

        bool operator==(const Settings& other) const;
    };

    constexpr int kMinSize = 16;
    constexpr int kMaxSize = 8192;
    constexpr int kMinFrameRate = 1;
    constexpr int kMaxFrameRate = 240;
    constexpr int kAudioRate = 48000;
    constexpr int kAudioChannels = 2;
    constexpr size_t kQueueFrames = 8;

    std::filesystem::path settingsFile(const std::filesystem::path& projectPath);
    bool load(const std::filesystem::path& file, Settings& settings, std::string& error);
    bool save(const std::filesystem::path& file, const Settings& settings, std::string& error);

    // the size of a preset (0 for Match Game View and Custom)
    void presetSize(Resolution resolution, int& width, int& height);
    const char* resolutionLabel(Resolution resolution);

    // the recorded size: the preset's, the Game view's, or the custom one, kept within 16-8192 and
    // the device's largest texture, even on both sides for a movie
    void outputSize(const Settings& settings, int gameWidth, int gameHeight, int maxTextureSize, int& width, int& height);

    // the game frames recorded, first and last (last -1: until stopped); frame 0 is the first
    // game frame of the recording (of Play when it starts with Play)
    void frameRange(const Settings& settings, int64_t& first, int64_t& last);

    struct NameContext {
        std::string scene;
        int take = 1;
        std::string date;    // "2026-10-30"
        std::string time;    // "14-05-09"
        int width = 0;
        int height = 0;
        int64_t frame = -1;  // the sequence's <Frame> (from the first recorded frame); -1 for a movie
    };
    // <Scene>, <Take> (3 digits), <Date>, <Time>, <Resolution> (1920x1080) and <Frame> (4 digits)
    std::string expandName(const std::string& pattern, const NameContext& context);
    // the folder of a recording and its file (".png" or ".mp4" / ".mov" appended)
    std::filesystem::path outputFolder(const Settings& settings, const std::filesystem::path& projectPath);
    std::filesystem::path outputFile(const Settings& settings, const std::filesystem::path& projectPath, const NameContext& context);

    // the encoders of `ffmpeg -hide_banner -encoders` output, by name
    std::vector<std::string> parseEncoders(const std::string& encodersOutput);
    // the first present of libx264, h264_videotoolbox, h264_mf, h264_nvenc, h264_qsv, h264_amf, libopenh264
    std::string pickH264(const std::vector<std::string>& encoders);
    bool hasProRes(const std::vector<std::string>& encoders);

    // The arguments (argv after the executable) streaming raw RGBA frames from stdin into
    // `output`: BT.709 tagged, yuv420p H.264 with +faststart, or yuv422p10le ProRes 422 HQ
    std::vector<std::string> movieArguments(const std::string& encoder, Format format, Quality quality, int width, int height,
                                            int frameRate, const std::filesystem::path& output);
    // the second pass muxing `audio` (16-bit WAV) into `video`: AAC 192 kb/s (MP4) or PCM (MOV)
    std::vector<std::string> muxArguments(Format format, const std::filesystem::path& video, const std::filesystem::path& audio,
                                          const std::filesystem::path& output);
    // the audio sample frames of game frame k: round((k+1) rate / fps) - round(k rate / fps)
    uint32_t audioFramesOf(int64_t frame, int frameRate, int sampleRate = kAudioRate);

}
