// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The resolution and bitrate presets follow OpenEngine's Apps/Editor/Source/
// MovieRecorderController.cpp and the H.264 encoder preference list its Engine/Source/Engine/
// Video/VideoWriterFFmpeg.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License. See NOTICE.md.

#include "recorder/RecorderSettings.h"

#include "io/AtomicFile.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::recorder;

namespace fs = std::filesystem;

namespace {

    template <typename E>
    struct Token {
        E value;
        const char* name;
    };

    constexpr Token<Kind> kKinds[] = {{Kind::ImageSequence, "image_sequence"}, {Kind::Movie, "movie"}};
    constexpr Token<Resolution> kResolutions[] = {{Resolution::MatchGameView, "match_game_view"}, {Resolution::P480, "480p"},
                                                  {Resolution::P720, "720p"}, {Resolution::P1080, "1080p"}, {Resolution::P1440, "1440p"},
                                                  {Resolution::P2160, "2160p"}, {Resolution::Custom, "custom"}};
    constexpr Token<Mode> kModes[] = {{Mode::Manual, "manual"}, {Mode::SingleFrame, "single_frame"}, {Mode::FrameInterval, "frame_interval"},
                                      {Mode::TimeInterval, "time_interval"}};
    constexpr Token<Format> kFormats[] = {{Format::Mp4, "mp4"}, {Format::Mov, "mov"}};
    constexpr Token<Quality> kQualities[] = {{Quality::Low, "low"}, {Quality::Medium, "medium"}, {Quality::High, "high"}};

    template <typename E, size_t N>
    const char* tokenOf(const Token<E> (&table)[N], E value) {
        for (const Token<E>& t : table) {
            if (t.value == value) return t.name;
        }
        return table[0].name;
    }

    template <typename E, size_t N>
    void readToken(const YAML::Node& node, const Token<E> (&table)[N], E& value) {
        if (!node || !node.IsScalar()) return;
        const std::string text = node.as<std::string>();
        for (const Token<E>& t : table) {
            if (text == t.name) value = t.value;
        }
    }

    template <typename T>
    void readScalar(const YAML::Node& node, T& value) {
        if (!node || !node.IsScalar()) return;
        try {
            value = node.as<T>();
        } catch (const YAML::Exception&) {
        }
    }

    std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
        return text;
    }

}

bool Settings::operator==(const Settings& o) const {
    return kind == o.kind && resolution == o.resolution && customWidth == o.customWidth && customHeight == o.customHeight &&
           frameRate == o.frameRate && mode == o.mode && singleFrame == o.singleFrame && frameStart == o.frameStart && frameEnd == o.frameEnd &&
           timeStart == o.timeStart && timeEnd == o.timeEnd && exitPlayMode == o.exitPlayMode && recordOnPlay == o.recordOnPlay &&
           outputFolder == o.outputFolder && movieName == o.movieName && sequenceName == o.sequenceName && take == o.take &&
           format == o.format && quality == o.quality && captureAudio == o.captureAudio;
}

fs::path recorder::settingsFile(const fs::path& projectPath) {
    return projectPath / ".jasperengine" / "user" / "recorder.yaml";
}

bool recorder::load(const fs::path& file, Settings& settings, std::string& error) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;   // defaults
    YAML::Node root;
    try {
        root = YAML::LoadFile(file.string());
    } catch (const YAML::Exception& e) {
        error = file.string() + ": " + e.what();
        return false;
    }
    if (!root.IsMap()) return true;
    readToken(root["recorder"], kKinds, settings.kind);
    readToken(root["resolution"], kResolutions, settings.resolution);
    readScalar(root["custom_width"], settings.customWidth);
    readScalar(root["custom_height"], settings.customHeight);
    readScalar(root["frame_rate"], settings.frameRate);
    readToken(root["mode"], kModes, settings.mode);
    readScalar(root["single_frame"], settings.singleFrame);
    readScalar(root["frame_start"], settings.frameStart);
    readScalar(root["frame_end"], settings.frameEnd);
    readScalar(root["time_start"], settings.timeStart);
    readScalar(root["time_end"], settings.timeEnd);
    readScalar(root["exit_play_mode"], settings.exitPlayMode);
    readScalar(root["record_on_play"], settings.recordOnPlay);
    readScalar(root["output_folder"], settings.outputFolder);
    readScalar(root["movie_name"], settings.movieName);
    readScalar(root["sequence_name"], settings.sequenceName);
    readScalar(root["take"], settings.take);
    readToken(root["format"], kFormats, settings.format);
    readToken(root["quality"], kQualities, settings.quality);
    readScalar(root["capture_audio"], settings.captureAudio);
    settings.frameRate = std::clamp(settings.frameRate, kMinFrameRate, kMaxFrameRate);
    settings.customWidth = std::clamp(settings.customWidth, kMinSize, kMaxSize);
    settings.customHeight = std::clamp(settings.customHeight, kMinSize, kMaxSize);
    settings.take = std::max(settings.take, 1);
    return true;
}

bool recorder::save(const fs::path& file, const Settings& s, std::string& error) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "recorder" << YAML::Value << tokenOf(kKinds, s.kind);
    out << YAML::Key << "resolution" << YAML::Value << tokenOf(kResolutions, s.resolution);
    out << YAML::Key << "custom_width" << YAML::Value << s.customWidth;
    out << YAML::Key << "custom_height" << YAML::Value << s.customHeight;
    out << YAML::Key << "frame_rate" << YAML::Value << s.frameRate;
    out << YAML::Key << "mode" << YAML::Value << tokenOf(kModes, s.mode);
    out << YAML::Key << "single_frame" << YAML::Value << s.singleFrame;
    out << YAML::Key << "frame_start" << YAML::Value << s.frameStart;
    out << YAML::Key << "frame_end" << YAML::Value << s.frameEnd;
    out << YAML::Key << "time_start" << YAML::Value << s.timeStart;
    out << YAML::Key << "time_end" << YAML::Value << s.timeEnd;
    out << YAML::Key << "exit_play_mode" << YAML::Value << s.exitPlayMode;
    out << YAML::Key << "record_on_play" << YAML::Value << s.recordOnPlay;
    out << YAML::Key << "output_folder" << YAML::Value << s.outputFolder;
    out << YAML::Key << "movie_name" << YAML::Value << s.movieName;
    out << YAML::Key << "sequence_name" << YAML::Value << s.sequenceName;
    out << YAML::Key << "take" << YAML::Value << s.take;
    out << YAML::Key << "format" << YAML::Value << tokenOf(kFormats, s.format);
    out << YAML::Key << "quality" << YAML::Value << tokenOf(kQualities, s.quality);
    out << YAML::Key << "capture_audio" << YAML::Value << s.captureAudio;
    out << YAML::EndMap;
    const io::WriteResult written = io::writeFileAtomic(file, std::string(out.c_str()) + "\n");
    if (!written.ok()) {
        error = written.message;
        return false;
    }
    return true;
}

void recorder::presetSize(Resolution resolution, int& width, int& height) {
    switch (resolution) {
        case Resolution::P480: width = 854; height = 480; return;
        case Resolution::P720: width = 1280; height = 720; return;
        case Resolution::P1080: width = 1920; height = 1080; return;
        case Resolution::P1440: width = 2560; height = 1440; return;
        case Resolution::P2160: width = 3840; height = 2160; return;
        case Resolution::MatchGameView:
        case Resolution::Custom: break;
    }
    width = height = 0;
}

const char* recorder::resolutionLabel(Resolution resolution) {
    switch (resolution) {
        case Resolution::MatchGameView: return "Match Game View";
        case Resolution::P480: return "480p (854 x 480)";
        case Resolution::P720: return "720p (1280 x 720)";
        case Resolution::P1080: return "1080p (1920 x 1080)";
        case Resolution::P1440: return "1440p (2560 x 1440)";
        case Resolution::P2160: return "2160p (3840 x 2160)";
        case Resolution::Custom: return "Custom";
    }
    return "Custom";
}

void recorder::outputSize(const Settings& settings, int gameWidth, int gameHeight, int maxTextureSize, int& width, int& height) {
    if (settings.resolution == Resolution::MatchGameView) {
        width = gameWidth;
        height = gameHeight;
    } else if (settings.resolution == Resolution::Custom) {
        width = settings.customWidth;
        height = settings.customHeight;
    } else {
        presetSize(settings.resolution, width, height);
    }
    const int limit = maxTextureSize > 0 ? std::min(kMaxSize, maxTextureSize) : kMaxSize;
    width = std::clamp(width, kMinSize, limit);
    height = std::clamp(height, kMinSize, limit);
    // 4:2:0 needs even sizes
    if (settings.kind == Kind::Movie) {
        width -= width % 2;
        height -= height % 2;
    }
}

void recorder::frameRange(const Settings& settings, int64_t& first, int64_t& last) {
    switch (settings.mode) {
        case Mode::Manual:
            first = 0;
            last = -1;
            return;
        case Mode::SingleFrame:
            first = last = std::max(0, settings.singleFrame);
            return;
        case Mode::FrameInterval:
            first = std::max(0, settings.frameStart);
            last = std::max<int64_t>(first, settings.frameEnd);
            return;
        case Mode::TimeInterval: {
            // the frames whose time k / fps lies in [timeStart, timeEnd)
            const double fps = std::clamp(settings.frameRate, kMinFrameRate, kMaxFrameRate);
            first = (int64_t)std::ceil(std::max(0.0f, settings.timeStart) * fps - 1e-9);
            last = std::max<int64_t>(first, (int64_t)std::ceil(std::max(settings.timeStart, settings.timeEnd) * fps - 1e-9) - 1);
            return;
        }
    }
    first = 0;
    last = -1;
}

std::string recorder::expandName(const std::string& pattern, const NameContext& c) {
    char take[16], frame[24], resolution[32];
    std::snprintf(take, sizeof(take), "%03d", c.take);
    std::snprintf(frame, sizeof(frame), "%04lld", (long long)std::max<int64_t>(c.frame, 0));
    std::snprintf(resolution, sizeof(resolution), "%dx%d", c.width, c.height);
    std::string name = replaceAll(pattern, "<Scene>", c.scene.empty() ? "Scene" : c.scene);
    name = replaceAll(name, "<Take>", take);
    name = replaceAll(name, "<Date>", c.date);
    name = replaceAll(name, "<Time>", c.time);
    name = replaceAll(name, "<Resolution>", resolution);
    name = replaceAll(name, "<Frame>", c.frame >= 0 ? frame : "");
    return name;
}

fs::path recorder::outputFolder(const Settings& settings, const fs::path& projectPath) {
    const fs::path folder = settings.outputFolder.empty() ? fs::path("Recordings") : fs::path(settings.outputFolder);
    return folder.is_absolute() ? folder : projectPath / folder;
}

fs::path recorder::outputFile(const Settings& settings, const fs::path& projectPath, const NameContext& context) {
    const bool movie = settings.kind == Kind::Movie;
    const std::string name = expandName(movie ? settings.movieName : settings.sequenceName, context);
    const char* extension = movie ? (settings.format == Format::Mov ? ".mov" : ".mp4") : ".png";
    return outputFolder(settings, projectPath) / fs::path(name + extension);
}

std::vector<std::string> recorder::parseEncoders(const std::string& output) {
    // lines " V....D libx264              libx264 H.264 / AVC ..." after the "------" line
    std::vector<std::string> names;
    std::istringstream lines(output);
    std::string line;
    bool listed = false;
    while (std::getline(lines, line)) {
        if (line.find("------") != std::string::npos) {
            listed = true;
            continue;
        }
        if (!listed) continue;
        std::istringstream words(line);
        std::string flags, name;
        if (words >> flags >> name && flags.size() == 6) names.push_back(name);
    }
    return names;
}

std::string recorder::pickH264(const std::vector<std::string>& encoders) {
    for (const char* name : {"libx264", "h264_videotoolbox", "h264_mf", "h264_nvenc", "h264_qsv", "h264_amf", "libopenh264"}) {
        if (std::find(encoders.begin(), encoders.end(), name) != encoders.end()) return name;
    }
    return std::string();
}

bool recorder::hasProRes(const std::vector<std::string>& encoders) {
    return std::find(encoders.begin(), encoders.end(), "prores_ks") != encoders.end();
}

std::vector<std::string> recorder::movieArguments(const std::string& encoder, Format format, Quality quality, int width, int height,
                                                  int frameRate, const fs::path& output) {
    std::vector<std::string> args = {"-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba",
                                     "-s", std::to_string(width) + "x" + std::to_string(height), "-framerate", std::to_string(frameRate),
                                     "-i", "pipe:0"};
    // BT.709, limited range: the conversion matrix and the tags
    const char* pixelFormat = format == Format::Mov ? "yuv422p10le" : "yuv420p";
    args.insert(args.end(), {"-vf", std::string("scale=out_color_matrix=bt709:out_range=tv,format=") + pixelFormat});
    if (format == Format::Mov) {
        args.insert(args.end(), {"-c:v", "prores_ks", "-profile:v", "3"});
    } else if (encoder == "libx264") {
        const int crf = quality == Quality::Low ? 28 : (quality == Quality::Medium ? 23 : 18);
        args.insert(args.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", std::to_string(crf)});
    } else {
        // hardware and other encoders: a bitrate of pixels x fps x {0.05, 0.1, 0.2} bits
        const double bits = quality == Quality::Low ? 0.05 : (quality == Quality::Medium ? 0.1 : 0.2);
        const long long bitrate = (long long)((double)width * (double)height * (double)frameRate * bits);
        args.insert(args.end(), {"-c:v", encoder, "-b:v", std::to_string(bitrate)});
    }
    args.insert(args.end(), {"-pix_fmt", pixelFormat, "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
                             "-color_range", "tv"});
    if (format == Format::Mp4) args.insert(args.end(), {"-movflags", "+faststart"});
    args.push_back(output.string());
    return args;
}

std::vector<std::string> recorder::muxArguments(Format format, const fs::path& video, const fs::path& audio, const fs::path& output) {
    std::vector<std::string> args = {"-y", "-hide_banner", "-loglevel", "error", "-i", video.string(), "-i", audio.string(),
                                     "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy"};
    if (format == Format::Mov) {
        args.insert(args.end(), {"-c:a", "pcm_s16le"});
    } else {
        args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart"});
    }
    args.push_back(output.string());
    return args;
}

uint32_t recorder::audioFramesOf(int64_t frame, int frameRate, int sampleRate) {
    const auto at = [&](int64_t k) { return (int64_t)std::llround((double)k * (double)sampleRate / (double)frameRate); };
    return (uint32_t)(at(frame + 1) - at(frame));
}
