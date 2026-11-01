// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The bounded frame queue (the main thread waits rather than drop a frame) and the bounded audio
// buffer follow OpenEngine's Apps/Editor/Source/GameViewMovieCapture.cpp, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md. The audio here is
// mixed offline in step with the fixed timestep instead of tapped from the device.

#include "recorder/RecorderSession.h"

#include "Engine.h"
#include "Out.h"
#include "htmlui/VideoImporter.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "render/SystemRender.h"
#include "subsystem/AudioSystem.h"
#include "texture/Framebuffer.h"
#include "util/GraphicUtils.h"
#include "vcs/ProcessExec.h"

#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::recorder;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

    void appendPng(void* context, void* data, int size) {
        auto* out = static_cast<std::string*>(context);
        out->append(static_cast<const char*>(data), (size_t)size);
    }

    // 16-bit PCM WAV: the header written first with sizes patched at the end
    void writeWavHeader(std::ofstream& out, uint32_t dataBytes) {
        auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + dataBytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        u32(16);
        u16(1);
        u16((uint16_t)kAudioChannels);
        u32((uint32_t)kAudioRate);
        u32((uint32_t)(kAudioRate * kAudioChannels * 2));
        u16((uint16_t)(kAudioChannels * 2));
        u16(16);
        out.write("data", 4);
        u32(dataBytes);
    }

    std::string localStamp(const char* format) {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), format, &local);
        return text;
    }

    // a file name not taken yet ("x.mp4", "x (2).mp4"...)
    fs::path uniquePath(const fs::path& path) {
        std::error_code ec;
        if (!fs::exists(path, ec)) return path;
        for (int n = 2;; n++) {
            fs::path candidate = path.parent_path() / (path.stem().string() + " (" + std::to_string(n) + ")" + path.extension().string());
            if (!fs::exists(candidate, ec)) return candidate;
        }
    }

}

mcp::Json Result::sidecarJson() const {
    mcp::Json j = {{"frames", frames}, {"fps", fps}, {"width", width}, {"height", height}, {"encoder", encoder}, {"scene", scene},
                   {"take", take}, {"startFrame", startFrame}, {"warnings", warnings}, {"wallSeconds", wallSeconds},
                   {"maxQueued", maxQueued}, {"output", output.generic_string()}};
    if (!error.empty()) j["error"] = error;
    return j;
}

// ---- the sinks

struct RecorderSession::Sink {
    Settings settings;
    NameContext names;
    fs::path project;
    int width = 0;
    int height = 0;
    size_t maxQueued = 0;
    std::vector<std::string> warnings;
    std::string failure;

    // the image sequence
    std::deque<jobs::Task<std::string>> encodes;
    std::vector<fs::path> files;

    // the movie
    std::string ffmpeg;
    std::string encoder;
    fs::path movie;          // the final file
    fs::path video;          // what FFmpeg writes (a temporary one when audio is muxed after)
    vcs::ProcessStream process;
    std::thread writer;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<uint8_t>> queue;
    bool closing = false;
    bool writeFailed = false;
    std::deque<std::string> stderrTail;

    // the audio
    bool audio = false;
    fs::path wavFile;
    std::ofstream wav;
    uint64_t audioBytes = 0;
    std::vector<float> mix;
    std::vector<int16_t> pcm;

    bool begin(std::string& error) {
        if (settings.kind == Kind::ImageSequence) {
            // the level spec R28 asks for; set before any encode job runs
            stbi_write_png_compression_level = 3;
            return true;
        }
        std::error_code ec;
        movie = uniquePath(outputFile(settings, project, names));
        fs::create_directories(movie.parent_path(), ec);
        video = audio ? movie.parent_path() / ("." + movie.stem().string() + ".video" + movie.extension().string()) : movie;
        vcs::ProcessSpec spec;
        spec.exe = ffmpeg;
        spec.args = movieArguments(encoder, settings.format, settings.quality, width, height, settings.frameRate, video);
        vcs::ProcessStream::Callbacks callbacks;
        callbacks.stderrLine = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            stderrTail.push_back(line);
            if (stderrTail.size() > 20) stderrTail.pop_front();
        };
        if (!process.start(spec, callbacks, error)) return false;
        writer = std::thread([this]() {
            while (true) {
                std::vector<uint8_t> frame;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [this] { return !queue.empty() || closing; });
                    if (queue.empty()) break;
                    frame = std::move(queue.front());
                    queue.pop_front();
                }
                changed.notify_all();
                if (!writeFailed && !process.writeStdin(frame.data(), frame.size())) writeFailed = true;
            }
            process.closeStdin();
        });
        if (audio) {
            wavFile = movie.parent_path() / ("." + movie.stem().string() + ".audio.wav");
            wav.open(wavFile, std::ios::binary | std::ios::trunc);
            if (!wav) {
                warnings.push_back("the audio could not be written (" + wavFile.string() + "): the movie has none");
                audio = false;
            } else {
                writeWavHeader(wav, 0);
            }
        }
        return true;
    }

    // a frame, blocking while the queue is full (the frames are never dropped)
    bool push(std::vector<uint8_t>&& rgba, int64_t frame) {
        if (settings.kind == Kind::ImageSequence) {
            while (encodes.size() >= kQueueFrames) {
                collect(encodes.front());
                encodes.pop_front();
            }
            NameContext context = names;
            context.frame = frame;
            const fs::path file = outputFile(settings, project, context);
            files.push_back(file);
            const int w = width, h = height;
            auto pixels = std::make_shared<std::vector<uint8_t>>(std::move(rgba));
            encodes.push_back(jobs::submit([pixels, file, w, h]() -> std::string {
                std::string png;
                if (!stbi_write_png_to_func(&appendPng, &png, w, h, 4, pixels->data(), w * 4)) return "PNG encoding failed: " + file.string();
                const io::WriteResult written = io::writeFileAtomic(file, png);
                return written.ok() ? std::string() : written.message;
            }, {jobs::Lane::Background, jobs::GateClass::Default, "recorder-png"}));
            maxQueued = std::max(maxQueued, encodes.size());
            return failure.empty();
        }
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return queue.size() < kQueueFrames || writeFailed; });
        if (writeFailed) return false;
        queue.push_back(std::move(rgba));
        maxQueued = std::max(maxQueued, queue.size());
        lock.unlock();
        changed.notify_all();
        return true;
    }

    void collect(jobs::Task<std::string>& task) {
        try {
            const std::string error = task.get();
            if (!error.empty() && failure.empty()) failure = error;
        } catch (const std::exception& e) {
            if (failure.empty()) failure = std::string("PNG job: ") + e.what();
        }
    }

    // the audio of game frame k (written only for recorded frames; mixed for every frame so the
    // sounds advance with the game)
    void mixAudio(int64_t frame, bool write) {
        if (!AudioSystem::isOfflineCapture()) return;
        const uint32_t count = audioFramesOf(frame, settings.frameRate);
        mix.assign((size_t)count * kAudioChannels, 0.0f);
        AudioSystem::mixOffline(mix.data(), count);
        if (!write || !audio) return;
        pcm.resize(mix.size());
        for (size_t i = 0; i < mix.size(); i++) pcm[i] = (int16_t)std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f);
        wav.write(reinterpret_cast<const char*>(pcm.data()), (std::streamsize)(pcm.size() * sizeof(int16_t)));
        audioBytes += pcm.size() * sizeof(int16_t);
    }

    // everything written: off the main thread
    void finish(Result& result) {
        if (settings.kind == Kind::ImageSequence) {
            while (!encodes.empty()) {
                collect(encodes.front());
                encodes.pop_front();
            }
            result.encoder = "png";
            result.files = files;
            result.output = files.empty() ? outputFolder(settings, project) : files.front().parent_path();
            if (!failure.empty()) result.error = failure;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        changed.notify_all();
        if (writer.joinable()) writer.join();
        int exitCode = -1;
        // FFmpeg's end: 120 s, then it is killed
        if (!process.wait(120000, exitCode)) {
            process.kill();
            process.wait(-1, exitCode);
            failure = "FFmpeg did not finish within 120 s";
        }
        if (failure.empty() && (writeFailed || exitCode != 0)) failure = "FFmpeg failed (exit " + std::to_string(exitCode) + ")";
        result.encoder = encoder;
        if (audio) {
            wav.seekp(0);
            writeWavHeader(wav, (uint32_t)audioBytes);
            wav.close();
        }
        std::error_code ec;
        if (failure.empty() && audio) {
            vcs::ProcessSpec spec;
            spec.exe = ffmpeg;
            spec.args = muxArguments(settings.format, video, wavFile, movie);
            spec.timeoutMs = 120000;
            const vcs::ProcessResult muxed = vcs::runProcess(spec);
            if (!muxed.ok()) {
                failure = "the audio could not be muxed in: " + muxed.message();
                fs::rename(video, movie, ec);
            } else {
                fs::remove(video, ec);
            }
        }
        if (!wavFile.empty()) fs::remove(wavFile, ec);
        result.output = movie;
        result.files = {movie};
        if (!failure.empty()) {
            // the partial file stays, renamed; FFmpeg's last lines go to the Console
            const fs::path failed = movie.parent_path() / (movie.stem().string() + ".failed" + movie.extension().string());
            if (fs::exists(video, ec) && video != movie) fs::rename(video, failed, ec);
            else if (fs::exists(movie, ec)) fs::rename(movie, failed, ec);
            result.output = failed;
            result.files = {failed};
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& line : stderrTail) Out::error("FFmpeg: %s", line.c_str());
            result.error = failure;
        }
    }
};

// ---- the session

RecorderSession& RecorderSession::get() {
    static RecorderSession session;
    return session;
}

void RecorderSession::setHost(Host newHost) {
    host = std::move(newHost);
}

const FfmpegInfo& RecorderSession::ffmpeg() {
    if (!ffmpegInfo.probed && !ffmpegProbe.valid()) refreshFfmpeg();
    if (ffmpegProbe.valid() && ffmpegProbe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) ffmpegInfo = ffmpegProbe.get();
    return ffmpegInfo;
}

void RecorderSession::refreshFfmpeg() {
    if (ffmpegProbe.valid()) return;
    // runProcess never runs on the main thread (S29 R24): the probe is a job of its own
    ffmpegProbe = std::async(std::launch::async, []() {
        FfmpegInfo info;
        info.path = htmlui::VideoImporter::findFfmpeg(&info.why);
        if (!info.path.empty()) {
            vcs::ProcessSpec spec;
            spec.exe = info.path;
            spec.args = {"-hide_banner", "-encoders"};
            spec.timeoutMs = 20000;
            const vcs::ProcessResult probed = vcs::runProcess(spec);
            const std::vector<std::string> encoders = parseEncoders(probed.out);
            info.h264 = pickH264(encoders);
            info.proRes = hasProRes(encoders);
            if (info.h264.empty()) info.why = "this FFmpeg has no H.264 encoder (libx264, h264_videotoolbox, h264_mf, h264_nvenc, ...)";
        }
        info.probed = true;
        return info;
    });
}

bool RecorderSession::audioMuted() const {
    return recording() && audio;
}

std::string RecorderSession::overlayText() const {
    if (!recording()) return std::string();
    if (currentState == State::Arming) return "REC (waiting for Play)";
    const double seconds = current.frameRate > 0 ? (double)std::max<int64_t>(frameIndex, 0) / current.frameRate : 0.0;
    char text[64];
    std::snprintf(text, sizeof(text), "REC %02d:%02d \xc2\xb7 frame %lld", (int)(seconds / 60.0), (int)std::fmod(seconds, 60.0),
                  (long long)std::max<int64_t>(frameIndex, 0));
    return text;
}

bool RecorderSession::start(const Settings& settings, std::string& error) {
    if (active()) {
        error = "a recording is already running (stop it first)";
        return false;
    }
    return arm(settings, false, error);
}

void RecorderSession::onPlayStarting(const Settings& settings) {
    if (active() || !settings.recordOnPlay) return;
    std::string error;
    if (!arm(settings, true, error)) Out::warning("Record on Play: %s", error.c_str());
}

bool RecorderSession::arm(const Settings& settings, bool fromPlayStart, std::string& error) {
    if (!host.isPlaying || !host.gameFramebuffer) {
        error = "the editor is not ready";
        return false;
    }
    current = settings;
    current.frameRate = std::clamp(current.frameRate, kMinFrameRate, kMaxFrameRate);
    working = Result();
    int gw = 0, gh = 0;
    if (host.gameViewSize) host.gameViewSize(gw, gh);
    outputSize(current, gw > 0 ? gw : 1920, gh > 0 ? gh : 1080, host.maxTextureSize ? host.maxTextureSize() : 0, width, height);
    frameRange(current, first, last);
    sink = std::make_unique<Sink>();
    sink->settings = current;
    sink->project = host.projectPath ? host.projectPath() : fs::path();
    sink->width = width;
    sink->height = height;
    sink->names.scene = host.sceneName ? host.sceneName() : std::string("Scene");
    sink->names.take = current.take;
    sink->names.date = localStamp("%Y-%m-%d");
    sink->names.time = localStamp("%H-%M-%S");
    sink->names.width = width;
    sink->names.height = height;
    if (current.kind == Kind::Movie) {
        const FfmpegInfo& info = ffmpeg();
        if (!info.probed) {
            // a movie needs the probe's answer: it runs at most a few seconds, once
            if (ffmpegProbe.valid()) ffmpegInfo = ffmpegProbe.get();
        }
        if (ffmpegInfo.path.empty() || ffmpegInfo.h264.empty()) {
            error = "Movie needs FFmpeg with an H.264 encoder (" + ffmpegInfo.why + "): set it in Preferences > External Tools > FFmpeg, "
                    "or record an Image Sequence";
            sink.reset();
            return false;
        }
        if (current.format == Format::Mov && !ffmpegInfo.proRes) {
            error = "MOV (ProRes 422 HQ) needs FFmpeg with prores_ks";
            sink.reset();
            return false;
        }
        sink->ffmpeg = ffmpegInfo.path;
        sink->encoder = current.format == Format::Mov ? std::string("prores_ks") : ffmpegInfo.h264;
    }
    // audio is offline, from the scene's start: only a recording that starts with Play has it
    const bool playing = host.isPlaying() || (host.isPlayStarting && host.isPlayStarting());
    audio = current.kind == Kind::Movie && current.captureAudio;
    if (audio && playing && !fromPlayStart) {
        working.warnings.push_back("Capture Audio needs the recording to start with Play: this one has no audio");
        audio = false;
    }
    if (audio && !AudioSystem::beginOfflineCapture((unsigned)kAudioRate, (unsigned)kAudioChannels)) {
        working.warnings.push_back("the offline audio mixer did not start: no audio");
        audio = false;
    }
    sink->audio = audio;
    std::string sinkError;
    if (!sink->begin(sinkError)) {
        if (audio) AudioSystem::endOfflineCapture();
        error = sinkError;
        sink.reset();
        return false;
    }
    // the game at the output size and the fixed timestep; the recorder wins over MCP's fixed_fps
    previousFixedDelta = Engine::getFixedDeltatime();
    if (previousFixedDelta > 0.0 && std::fabs(1.0 / previousFixedDelta - current.frameRate) > 1e-6) working.overriddenFixedFps = 1.0 / previousFixedDelta;
    Engine::setFixedDeltatime(1.0 / current.frameRate);
    if (host.setGameRenderSize) host.setGameRenderSize(width, height);
    recorded = 0;
    frameIndex = -1;
    // the game frames from now on are the recording's (frame 0: the first one Play runs)
    gameFramesAtStart = Engine::getGameFrames();
    gameFramesSeen = gameFramesAtStart;
    startTime = Clock::now();
    startedPlay = false;
    currentState = State::Arming;
    if (!playing && !fromPlayStart) {
        std::string playError;
        if (!host.startPlay || !host.startPlay(playError)) {
            error = "Play did not start: " + playError;
            finish();
            return false;
        }
        startedPlay = true;
    } else if (fromPlayStart) {
        startedPlay = true;
    }
    if (host.isPlaying()) beginRecording();
    return true;
}

void RecorderSession::beginRecording() {
    currentState = State::Recording;
}

void RecorderSession::captureFrame() {
    if (!recording() || !sink) return;
    const uint64_t frames = Engine::getGameFrames();
    // frames run while Play is not running (none should) are not the recording's
    if (!host.isPlaying()) {
        if (currentState == State::Arming) {
            gameFramesAtStart += frames - gameFramesSeen;
            gameFramesSeen = frames;
        }
        return;
    }
    if (currentState == State::Arming) beginRecording();
    if (shaderHold != ShaderHold::None) {
        // a frame drawn while shaders and pipelines were still being created (S32) shows their
        // stand-ins: the game is held (it draws, it does not advance) until creation is idle and
        // one whole frame has been drawn since, then that frame is the recorded one
        const bool idle = SystemRender::isGpuCreationIdle();
        const bool timedOut = Clock::now() - shaderHoldStart > std::chrono::seconds(30);
        if (!idle && !timedOut) {
            shaderHold = ShaderHold::Waiting;
            return;
        }
        if (shaderHold == ShaderHold::Waiting && !timedOut) {
            shaderHold = ShaderHold::DrawnOnce;   // the next frame's start reads what this one draws
            return;
        }
        if (timedOut && !idle) {
            working.warnings.push_back("frame " + std::to_string(frameIndex) + " was recorded with shaders still being created (30 s)");
        }
        shaderHold = ShaderHold::None;
        Engine::setSimulationHeld(heldBeforeShaders);
        recordFrame();
        return;
    }
    if (frames <= gameFramesSeen) return;
    if (frames - gameFramesSeen > 1) {
        working.warnings.push_back(std::to_string(frames - gameFramesSeen - 1) + " game frame(s) ran within one editor frame and were not recorded");
    }
    gameFramesSeen = frames;
    frameIndex = (int64_t)(frames - gameFramesAtStart) - 1;
    const bool inRange = frameIndex >= first && (last < 0 || frameIndex <= last);
    sink->mixAudio(frameIndex, inRange);
    if (!inRange) return;
    if (!SystemRender::isGpuCreationIdle()) {
        heldBeforeShaders = Engine::isSimulationHeld();
        Engine::setSimulationHeld(true);
        shaderHold = ShaderHold::Waiting;
        shaderHoldStart = Clock::now();
        return;
    }
    recordFrame();
}

void RecorderSession::recordFrame() {
    if (!sink) return;
    std::vector<uint8_t> rgba;
    Framebuffer* framebuffer = host.gameFramebuffer();
    if (!GraphicUtils::readFramebufferRGBA(framebuffer, rgba)) {
        working.warnings.push_back("frame " + std::to_string(frameIndex) + " could not be read back");
        return;
    }
    const int fw = (int)framebuffer->getWidth(), fh = (int)framebuffer->getHeight();
    if (fw != width || fh != height) {
        // the view had not taken the output size yet (the first frame of a recording started
        // mid-play): the frame scaled to it
        std::vector<uint8_t> scaled((size_t)width * height * 4);
        stbir_resize_uint8_linear(rgba.data(), fw, fh, fw * 4, scaled.data(), width, height, width * 4, STBIR_RGBA);
        rgba.swap(scaled);
    }
    if (!sink->push(std::move(rgba), frameIndex)) {
        working.error = "the output stopped taking frames";
        finish();
        return;
    }
    recorded++;
}

void RecorderSession::update() {
    if (currentState == State::Finalising) {
        if (finalising.valid() && finalising.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            finalising.get();
            result = working;
            sink.reset();
            serial++;
            currentState = State::Idle;
            if (result.ok) {
                Out::success("Recorded %lld frames to %s", (long long)result.frames, result.output.string().c_str());
            } else {
                Out::error("Recording failed: %s (%s)", result.error.c_str(), result.output.string().c_str());
            }
            if (onFinished) {
                auto callback = std::move(onFinished);
                onFinished = nullptr;
                callback(result);
            }
        }
        return;
    }
    if (currentState == State::Arming) {
        if (host.isPlaying()) return;   // captureFrame starts the frames
        if (!(host.isPlayStarting && host.isPlayStarting()) && Clock::now() - startTime > std::chrono::seconds(1)) {
            working.error = "Play stopped before the recording started";
            finish();
        }
        return;
    }
    if (currentState != State::Recording) return;
    // (a frame held for its shaders is not recorded yet)
    const bool done = last >= 0 && frameIndex >= last && shaderHold == ShaderHold::None;
    if (done || !host.isPlaying()) {
        if (done && current.exitPlayMode && host.isPlaying() && host.stopPlay) host.stopPlay();
        finish();
    }
}

void RecorderSession::stop() {
    if (currentState == State::Arming || currentState == State::Recording) {
        if (current.exitPlayMode && startedPlay && host.isPlaying() && host.stopPlay) host.stopPlay();
        finish();
    }
}

void RecorderSession::restoreEngine() {
    if (shaderHold != ShaderHold::None) {
        shaderHold = ShaderHold::None;
        Engine::setSimulationHeld(heldBeforeShaders);
    }
    Engine::setFixedDeltatime(previousFixedDelta);
    if (host.setGameRenderSize) host.setGameRenderSize(0, 0);
    if (audio) {
        AudioSystem::endOfflineCapture();
        audio = false;
    }
}

void RecorderSession::finish() {
    restoreEngine();
    currentState = State::Finalising;
    working.frames = recorded;
    working.fps = current.frameRate;
    working.width = width;
    working.height = height;
    working.scene = sink ? sink->names.scene : std::string();
    working.take = current.take;
    working.startFrame = first;
    const Clock::time_point started = startTime;
    Sink* s = sink.get();
    // the take after this one
    current.take++;
    finalising = std::async(std::launch::async, [this, s, started]() {
        if (s) {
            s->finish(working);
            for (const std::string& w : s->warnings) working.warnings.push_back(w);
            working.maxQueued = s->maxQueued;
        }
        working.wallSeconds = std::chrono::duration<double>(Clock::now() - started).count();
        working.ok = working.error.empty() && working.frames > 0;
        if (working.error.empty() && working.frames == 0) working.error = "no frame was recorded";
        // the sidecar next to the output
        const fs::path sidecar = working.output.empty() ? fs::path()
                                 : working.output.parent_path() / (working.output.filename().string() + ".recording.json");
        if (!sidecar.empty()) {
            const io::WriteResult written = io::writeFileAtomic(sidecar, working.sidecarJson().dump(2) + "\n");
            if (written.ok()) working.sidecar = sidecar;
        }
    });
}
