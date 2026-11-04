// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The bounded frame queue (the main thread waits rather than drop a frame) and the bounded audio
// buffer follow OpenEngine's Apps/Editor/Source/GameViewMovieCapture.cpp, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md. The audio here comes
// from the deterministic driver's tap, mixed in step with the fixed timestep, not from the device.

#include "recorder/RecorderSession.h"

#include "Engine.h"
#include "Out.h"
#include "audio/AudioTap.h"
#include "htmlui/VideoImporter.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "Scene.h"
#include "object/ui/UIDocument.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "render/ObjectRender.h"
#include "render/SystemRender.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UISystem.h"
#include "texture/Framebuffer.h"
#include "texture/Texture.h"
#include "util/FileUtils.h"
#include "util/GraphicUtils.h"
#include "vcs/ProcessExec.h"

#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

    bool isDiskFull(const std::error_code& code) {
#ifdef EDQUOT
        if (code.default_error_condition() == std::error_condition(EDQUOT, std::generic_category())) return true;
#endif
        return code == std::errc::no_space_on_device;
    }

    // the wait for a frame's loads: what is loading (a shader, a texture, a model) may take this
    // long with nothing finishing before the recording fails; the rest (objects that report loaded
    // late, or never) this long before the recording stops waiting for them (a screenshot's settle)
    constexpr auto kLoadLimit = std::chrono::seconds(120);
    constexpr auto kSettleLimit = std::chrono::seconds(6);

}

mcp::Json Result::sidecarJson() const {
    mcp::Json j = {{"frames", frames}, {"fps", fps}, {"width", width}, {"height", height}, {"encoder", encoder}, {"scene", scene},
                   {"take", take}, {"startFrame", startFrame}, {"warnings", warnings}, {"wallSeconds", wallSeconds},
                   {"maxQueued", maxQueued}, {"output", output.generic_string()}};
    if (!error.empty()) j["error"] = error;
    if (!failedStep.empty()) j["failed_step"] = failedStep;
    if (!partial.empty()) {
        j["partial"] = partial.generic_string();
        j["partial_frames"] = partialFrames;
    }
    if (estimatedBytes > 0) j["estimated_bytes"] = estimatedBytes;
    if (freeBytes >= 0) j["free_bytes"] = freeBytes;
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
    TestFaults faults;

    // the first failure: what failed and why, and the flag the main thread polls every frame
    std::string failedStep;
    std::string failure;
    std::atomic<bool> failedFlag{false};

    // the image sequence: each frame complete in a hidden file next to its name, all of them
    // renamed at the end (R3)
    struct Encoded {
        std::string error;
        std::error_code code;
    };
    struct Frame {
        fs::path file;
        fs::path staged;
    };
    std::deque<jobs::Task<Encoded>> encodes;
    std::vector<Frame> frames;
    size_t framesWritten = 0;         // the frames from the first on known to be written
    fs::path sequenceFolder;
    bool folderExisted = false;

    // the movie
    std::string ffmpeg;
    std::string encoder;
    fs::path movie;          // the final file: nothing is written under this name but the whole movie
    fs::path video;          // what FFmpeg writes
    fs::path muxed;          // what the second pass writes
    vcs::ProcessStream process;
    std::thread writer;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<uint8_t>> queue;
    bool closing = false;
    bool writerFailed = false;
    int64_t framesSent = 0;
    std::deque<std::string> stderrTail;

    // the audio
    bool audio = false;
    fs::path wavFile;
    std::ofstream wav;
    uint64_t audioBytes = 0;
    int64_t audioChunks = 0;
    // what the tap delivered since the last game frame
    std::vector<float> tapped;
    std::vector<float> mix;
    std::vector<int16_t> pcm;

    bool failed() const { return failedFlag.load(std::memory_order_acquire); }

    void fail(const char* step, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failure.empty()) {
                failedStep = step;
                failure = reason;
            }
        }
        failedFlag.store(true, std::memory_order_release);
        changed.notify_all();
    }

    // what the output's disk has left (an empty folder's nearest existing parent); -1: unknown
    int64_t freeBytes() const {
        if (faults.freeBytes >= 0) return faults.freeBytes;
        std::error_code ec;
        fs::path folder = settings.kind == Kind::Movie ? movie.parent_path() : outputFolder(settings, project);
        while (!folder.empty() && !fs::exists(folder, ec)) {
            if (folder == folder.parent_path()) return -1;
            folder = folder.parent_path();
        }
        const fs::space_info space = fs::space(folder, ec);
        return ec ? -1 : (int64_t)space.available;
    }

    std::string diskFull() const {
        return diskFullReason((uint64_t)std::max<int64_t>(freeBytes(), 0));
    }

    // the "why" of a failed write
    std::string writeReason(const std::error_code& code, const std::string& fallback) const {
        if (isDiskFull(code)) return diskFull();
        if (!fallback.empty()) return fallback;
        return code ? code.message() : std::string("the write failed");
    }

    // the "why" of FFmpeg's failure, in its own words: its last line of stderr
    std::string ffmpegReason(const std::string& fallback) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const std::string& line : stderrTail) {
            if (isDiskFullMessage(line)) return diskFull();
        }
        return stderrTail.empty() ? fallback : fallback + ": " + stderrTail.back();
    }

    bool begin(std::string& error) {
        std::error_code ec;
        if (settings.kind == Kind::ImageSequence) {
            // the level spec R28 asks for; set before any encode job runs
            stbi_write_png_compression_level = 3;
            NameContext first = names;
            first.frame = 0;
            sequenceFolder = outputFile(settings, project, first).parent_path();
            folderExisted = fs::exists(sequenceFolder, ec);
            fs::create_directories(sequenceFolder, ec);
            if (ec) {
                error = "the output folder cannot be created (" + sequenceFolder.string() + "): " + ec.message();
                return false;
            }
            return true;
        }
        movie = uniquePath(outputFile(settings, project, names));
        fs::create_directories(movie.parent_path(), ec);
        if (ec) {
            error = "the output folder cannot be created (" + movie.parent_path().string() + "): " + ec.message();
            return false;
        }
        video = io::makeTemporarySiblingPath(movie);
        if (audio) {
            fs::path wavName = movie;
            wavName += ".audio.wav";
            wavFile = io::makeTemporarySiblingPath(wavName);
            wav.open(wavFile, std::ios::binary | std::ios::trunc);
            if (wav) writeWavHeader(wav, 0);
            wav.flush();
            if (!wav) {
                const std::error_code code(errno, std::generic_category());
                error = "the audio cannot be written (" + wavFile.string() + "): " + writeReason(code, std::string());
                wav.close();
                fs::remove(wavFile, ec);
                return false;
            }
        }
        vcs::ProcessSpec spec;
        spec.exe = ffmpeg;
        spec.args = movieArguments(encoder, settings.format, settings.quality, width, height, settings.frameRate, video);
        vcs::ProcessStream::Callbacks callbacks;
        callbacks.stderrLine = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            stderrTail.push_back(line);
            if (stderrTail.size() > 20) stderrTail.pop_front();
        };
        if (!process.start(spec, callbacks, error)) {
            if (audio) {
                wav.close();
                fs::remove(wavFile, ec);
            }
            return false;
        }
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
                sendFrame(frame);
            }
            process.closeStdin();
        });
        return true;
    }

    // a frame to the encoder (the writer thread): after a failure of the encoder's the rest goes
    // nowhere; after one of the audio's it still goes, so the video can be kept
    void sendFrame(const std::vector<uint8_t>& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (writerFailed) return;
        }
        const int64_t number = framesSent++;
        bool sent = true;
        std::string reason = "FFmpeg stopped taking frames";
        if (number == faults.videoWrite) {
            sent = false;
            reason = diskFull();
        } else if (number == faults.encoderKill) {
            process.kill();
            sent = false;
        } else {
            sent = process.writeStdin(frame.data(), frame.size());
        }
        if (sent) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            writerFailed = true;
        }
        fail("video", reason);
    }

    // a frame, blocking while the queue is full (the frames are never dropped); false once the
    // recording has failed
    bool push(std::vector<uint8_t>&& rgba, int64_t frame) {
        if (settings.kind == Kind::ImageSequence) {
            // what has finished is looked at now: the first failure stops the next frame
            while (!encodes.empty() && (encodes.size() >= kQueueFrames || encodes.front().isReady())) {
                collect(encodes.front());
                encodes.pop_front();
            }
            if (failed()) return false;
            NameContext context = names;
            context.frame = frame;
            Frame staged;
            staged.file = outputFile(settings, project, context);
            staged.staged = io::makeTemporarySiblingPath(staged.file);
            const fs::path target = staged.staged;
            const bool injected = (int64_t)frames.size() == faults.pngWrite;
            frames.push_back(staged);
            const int w = width, h = height;
            auto pixels = std::make_shared<std::vector<uint8_t>>(std::move(rgba));
            encodes.push_back(jobs::submit([pixels, target, w, h, injected]() -> Encoded {
                Encoded done;
                if (injected) {
                    done.code = std::make_error_code(std::errc::no_space_on_device);
                    return done;
                }
                std::string png;
                if (!stbi_write_png_to_func(&appendPng, &png, w, h, 4, pixels->data(), w * 4)) {
                    done.error = "PNG encoding failed: " + target.string();
                    return done;
                }
                const io::WriteResult written = io::writeFileAtomic(target, png);
                if (!written.ok()) {
                    done.error = written.message;
                    done.code = written.code;
                }
                return done;
            }, {jobs::Lane::Background, jobs::GateClass::Default, "recorder-png"}));
            maxQueued = std::max(maxQueued, encodes.size());
            return true;
        }
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return queue.size() < kQueueFrames || failed(); });
        if (failed()) return false;
        queue.push_back(std::move(rgba));
        maxQueued = std::max(maxQueued, queue.size());
        lock.unlock();
        changed.notify_all();
        return true;
    }

    void collect(jobs::Task<Encoded>& task) {
        try {
            const Encoded done = task.get();
            if (done.error.empty() && !done.code) {
                if (!failed()) framesWritten++;
            } else {
                fail("png", writeReason(done.code, done.error));
            }
        } catch (const std::exception& e) {
            fail("png", std::string("PNG job: ") + e.what());
        }
    }

    // The audio of game frame k (written only for recorded frames). The pump mixed it before this
    // frame ended: exactly this count, the same accumulator. A paused Play mixes nothing while the
    // frames go on, so silence keeps the two in step; frames that ran unrecorded give their audio up.
    void mixAudio(int64_t frame, bool write) {
        if (!audio) return;
        const uint32_t count = audioFramesOf(frame, settings.frameRate);
        mix.assign((size_t)count * kAudioChannels, 0.0f);
        const size_t taken = std::min(mix.size(), tapped.size());
        std::copy(tapped.end() - (std::ptrdiff_t)taken, tapped.end(), mix.begin());
        tapped.clear();
        if (!write || failed()) return;
        pcm.resize(mix.size());
        for (size_t i = 0; i < mix.size(); i++) pcm[i] = (int16_t)std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f);
        const size_t bytes = pcm.size() * sizeof(int16_t);
        // the sizes in a WAV header are 32 bits: past them no reader would find the end
        if (audioBytes + bytes > (uint64_t)UINT32_MAX - 36) {
            fail("audio", "the audio reached the 4 GB a WAV can hold");
            return;
        }
        if (audioChunks++ == faults.audioWrite) {
            fail("audio", diskFull());
            return;
        }
        wav.write(reinterpret_cast<const char*>(pcm.data()), (std::streamsize)bytes);
        // now, not when the buffer next fills: the first failed chunk stops the recording
        wav.flush();
        if (!wav) {
            fail("audio", writeReason(std::error_code(errno, std::generic_category()), std::string()));
            return;
        }
        audioBytes += bytes;
    }

    // everything written: off the main thread
    void finish(Result& result) {
        if (settings.kind == Kind::ImageSequence) finishSequence(result);
        else finishMovie(result);
        {
            std::lock_guard<std::mutex> lock(mutex);
            result.failedStep = failedStep;
            if (!failure.empty()) {
                result.error = (failedStep == "png" ? "the image write failed" :
                                failedStep == "audio" ? "the audio write failed" :
                                failedStep == "video" ? "the video write failed" :
                                failedStep == "mux" ? "muxing the audio in failed" :
                                failedStep == "publish" ? "the finished file could not take its name" :
                                                          "the recording stopped") + std::string(": ") + failure;
            }
        }
        if (result.failedStep.empty()) return;
        // what stays of it, said once where every report reads it
        if (!result.partial.empty()) {
            result.error += "; " + std::to_string(result.partialFrames) + " frames were kept as " + FileUtils::pathToUtf8(result.partial);
            if (settings.kind == Kind::Movie && settings.captureAudio) result.error += " (no audio)";
        } else {
            result.error += "; nothing was kept";
        }
    }

    void finishSequence(Result& result) {
        while (!encodes.empty()) {
            collect(encodes.front());
            encodes.pop_front();
        }
        result.encoder = "png";
        result.output = frames.empty() ? sequenceFolder : frames.front().file.parent_path();
        std::error_code ec;
        size_t published = 0;
        if (!failed()) {
            if (faults.publish && !frames.empty()) fail("publish", diskFull());
            for (const Frame& frame : frames) {
                if (failed()) break;
                if (!io::renameWithRetry(frame.staged, frame.file, ec)) {
                    fail("publish", writeReason(ec, frame.file.string() + ": " + ec.message()));
                    break;
                }
                published++;
                result.files.push_back(frame.file);
            }
        }
        if (failed()) {
            result.files.clear();
            keepPartialSequence(result, published);
            if (!folderExisted) fs::remove(sequenceFolder, ec);    // (only an empty one goes)
        }
    }

    // the frames written before the first failure, under <folder>.partial; those after it (the
    // jobs under way) have a gap before them and go
    void keepPartialSequence(Result& result, size_t published) {
        std::error_code ec;
        fs::path partialName = sequenceFolder;
        partialName += ".partial";
        const fs::path partial = uniquePath(partialName);
        size_t kept = 0;
        for (size_t i = 0; i < frames.size(); i++) {
            const fs::path& from = i < published ? frames[i].file : frames[i].staged;
            if (i >= framesWritten) {
                fs::remove(from, ec);
                continue;
            }
            if (kept == 0) {
                fs::create_directories(partial, ec);
                if (ec) {
                    Out::warning("Recorder: the partial folder cannot be created (%s): %s", partial.string().c_str(), ec.message().c_str());
                    framesWritten = 0;
                    fs::remove(from, ec);
                    continue;
                }
            }
            if (io::renameWithRetry(from, partial / frames[i].file.filename(), ec)) kept++;
            else fs::remove(from, ec);
        }
        if (kept > 0) {
            result.partial = partial;
            result.partialFrames = (int64_t)kept;
        }
    }

    void finishMovie(Result& result) {
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
            fail("video", "FFmpeg did not finish within 120 s");
        }
        // a failure while the frames went: now FFmpeg's last lines say what it was; and one only
        // its exit says (the write of the trailer on a full disk). A file with no frames in it
        // fails on its own: nothing is published (the session reports "no frame was recorded")
        std::string writerReason;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (writerFailed && failedStep == "video") writerReason = failure;
        }
        const bool anyFrames = framesSent > 0 && result.frames > 0;
        std::error_code ec;
        if (!writerReason.empty() && faults.videoWrite < 0) {
            const std::string better = ffmpegReason(writerReason);
            std::lock_guard<std::mutex> lock(mutex);
            failure = better;
        } else if (!failed() && anyFrames && (exitCode != 0 || fs::file_size(video, ec) == 0 || ec)) {
            fail("video", ffmpegReason("FFmpeg failed (exit " + std::to_string(exitCode) + ")"));
        } else if (!failed() && anyFrames) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& line : stderrTail) {
                // (FFmpeg can end with 0 after a full disk took its trailer)
                if (isDiskFullMessage(line) || line.find("Error writing trailer") != std::string::npos) {
                    failedStep = "video";
                    failure = isDiskFullMessage(line) ? diskFull() : line;
                    failedFlag.store(true, std::memory_order_release);
                    break;
                }
            }
        }
        result.encoder = encoder;
        result.output = movie;
        if (audio) closeWav();
        if (!failed() && anyFrames && audio) muxAudio();
        const fs::path finished = audio ? muxed : video;
        if (!failed() && anyFrames) publishMovie(finished, result);
        if (failed()) {
            keepPartialMovie(result);
            std::lock_guard<std::mutex> lock(mutex);
            for (const std::string& line : stderrTail) Out::error("FFmpeg: %s", line.c_str());
        }
        for (const fs::path* temporary : {&video, &muxed, &wavFile}) {
            if (!temporary->empty()) fs::remove(*temporary, ec);
        }
    }

    void closeWav() {
        if (!failed()) {
            if (faults.audioHeader) {
                fail("audio", diskFull());
            } else {
                wav.seekp(0);
                writeWavHeader(wav, (uint32_t)audioBytes);
                wav.flush();
                if (!wav) fail("audio", writeReason(std::error_code(errno, std::generic_category()), std::string()));
            }
        }
        wav.close();
        if (wav.fail() && !failed()) fail("audio", writeReason(std::error_code(errno, std::generic_category()), std::string()));
    }

    void muxAudio() {
        if (faults.mux) {
            fail("mux", diskFull());
            return;
        }
        muxed = io::makeTemporarySiblingPath(movie);
        vcs::ProcessSpec spec;
        spec.exe = ffmpeg;
        spec.args = muxArguments(settings.format, video, wavFile, muxed);
        spec.timeoutMs = 120000;
        const vcs::ProcessResult result = vcs::runProcess(spec);
        std::error_code ec;
        const bool written = fs::file_size(muxed, ec) > 0 && !ec;
        if (result.ok() && written && !isDiskFullMessage(result.err)) return;
        fail("mux", isDiskFullMessage(result.err) ? diskFull() : result.message());
    }

    // the only write under the movie's name: S03's publish of a complete file
    void publishMovie(const fs::path& finished, Result& result) {
        if (faults.publish) {
            fail("publish", diskFull());
            return;
        }
        // (it was free at the start; something else may have taken it meanwhile)
        movie = uniquePath(movie);
        const io::WriteResult published = io::publishFile(finished, movie);
        if (!published.ok()) {
            fail("publish", writeReason(published.code, published.message));
            return;
        }
        result.output = movie;
        result.files = {movie};
    }

    // the video so far under <name>.partial.<ext> when it decodes to the end, without an error
    // (an MP4 FFmpeg did not get to finish has no index, and a full disk leaves just that)
    void keepPartialMovie(Result& result) {
        std::error_code ec;
        if (video.empty() || !fs::exists(video, ec) || fs::file_size(video, ec) == 0) return;
        vcs::ProcessSpec spec;
        spec.exe = ffmpeg;
        spec.args = decodeCheckArguments(video);
        spec.timeoutMs = 600000;
        const vcs::ProcessResult decoded = vcs::runProcess(spec);
        const int64_t count = parseDecodedFrames(decoded.out);
        if (!decoded.ok() || !decoded.err.empty() || count <= 0) return;
        const fs::path partial = uniquePath(partialPath(movie));
        if (!io::renameWithRetry(video, partial, ec)) {
            Out::warning("Recorder: the partial movie cannot be kept (%s): %s", partial.string().c_str(), ec.message().c_str());
            return;
        }
        result.partial = partial;
        result.partialFrames = count;
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
    sink->faults = parseTestFaults(std::getenv("JASPER_TEST_RECORDER_FAIL"));
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
    // audio is the deterministic driver's, from the scene's start: only a recording that starts with Play has it
    const bool playing = host.isPlaying() || (host.isPlayStarting && host.isPlayStarting());
    audio = current.kind == Kind::Movie && current.captureAudio;
    if (audio && playing && !fromPlayStart) {
        working.warnings.push_back("Capture Audio needs the recording to start with Play: this one has no audio");
        audio = false;
    }
    if (audio && !AudioSystem::beginDeterministic()) {
        AudioSystem::endDeterministic();
        working.warnings.push_back("the deterministic audio driver did not start: no audio");
        audio = false;
    }
    sink->audio = audio;
    std::string sinkError;
    if (!sink->begin(sinkError)) {
        if (audio) AudioSystem::endDeterministic();
        audio = false;
        error = sinkError;
        sink.reset();
        return false;
    }
    if (audio) {
        Sink* tapSink = sink.get();
        audioTap = AudioTap::subscribe("recorder", [tapSink](const float* interleaved, uint32_t frames, uint64_t) {
            tapSink->tapped.insert(tapSink->tapped.end(), interleaved, interleaved + (size_t)frames * kAudioChannels);
        });
    }
    warnAboutFreeSpace();
    // the game at the output size and the fixed timestep; the recorder wins over MCP's fixed_fps
    previousFixedDelta = Engine::getFixedDeltatime();
    if (previousFixedDelta > 0.0 && std::fabs(1.0 / previousFixedDelta - current.frameRate) > 1e-6) working.overriddenFixedFps = 1.0 / previousFixedDelta;
    Engine::setFixedDeltatime(1.0 / current.frameRate);
    if (host.setGameRenderSize) host.setGameRenderSize(width, height);
    recorded = 0;
    frameIndex = -1;
    settleGivenUp = false;
    drawn = FrameMarks();
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

void RecorderSession::warnAboutFreeSpace() {
    Settings estimated = current;
    estimated.captureAudio = audio;
    const bool knownLength = last >= 0;
    const uint64_t needs = estimateRecordingBytes(estimated, width, height, knownLength ? last - first + 1 : -1);
    const int64_t available = sink->freeBytes();
    working.estimatedBytes = needs;
    working.freeBytes = available;
    startWarning.clear();
    if (available < 0 || needs <= (uint64_t)available) return;
    startWarning = "This recording needs about " + formatSize(needs) + (knownLength ? "" : " a minute") + " and the disk has " + formatSize((uint64_t)available) +
                   " free: it may stop with a full disk";
    working.warnings.push_back(startWarning);
    Out::warning("Recorder: %s", startWarning.c_str());
}

void RecorderSession::beginRecording() {
    currentState = State::Recording;
}

bool RecorderSession::assetsLoading() const {
    if (!ShaderPool::isCreationIdle() || TextureDataPool::getLoadingCount() > 0) return true;
    Scene* scene = host.gameScene ? host.gameScene() : nullptr;
    MeshSystem* meshes = scene ? scene->getSystem<MeshSystem>().get() : nullptr;
    return meshes && meshes->hasPendingAsyncModelLoads();
}

bool RecorderSession::contentSettled() const {
    if (UIDocument::getPendingEffectReplays() > 0 || !Engine::areScenesLoaded(true)) return false;
    Scene* scene = host.gameScene ? host.gameScene() : nullptr;
    UISystem* ui = scene ? scene->getSystem<UISystem>().get() : nullptr;
    return !ui || !ui->hasPendingImages();
}

std::string RecorderSession::describeLoading() const {
    std::string out = Engine::areScenesLoaded(true) ? std::string() : Engine::describeNotLoaded();
    if (!ShaderPool::isCreationIdle() && out.find("shader") == std::string::npos) out += (out.empty() ? "" : "; ") + std::string("shaders or pipelines being made");
    Scene* scene = host.gameScene ? host.gameScene() : nullptr;
    MeshSystem* meshes = scene ? scene->getSystem<MeshSystem>().get() : nullptr;
    if (meshes && meshes->hasPendingAsyncModelLoads()) out += (out.empty() ? "" : "; ") + std::string("models loading");
    return out;
}

RecorderSession::FrameMarks RecorderSession::readMarks() const {
    FrameMarks marks;
    marks.assetsIdle = !assetsLoading();
    marks.settled = settleGivenUp || contentSettled();
    marks.waitingDraws = ObjectRender::getWaitingDrawCount();
    marks.standIns = Texture::getStandInCount();
    marks.loadingAnswers = Texture::getLoadingAnswerCount();
    marks.textureEpoch = TexturePool::getReadyEpoch();
    const GpuCreationStats stats = SystemRender::getGpuCreationStats();
    marks.created = stats.createdShaders + stats.createdPipelines + stats.warmDraws;
    return marks;
}

bool RecorderSession::drawnWithAllReady(const FrameMarks& before, const FrameMarks& after) const {
    if (!before.assetsIdle || !after.assetsIdle) return false;
    if (settleGivenUp) return true;
    return before.settled && after.settled && before.waitingDraws == after.waitingDraws && before.standIns == after.standIns &&
           before.loadingAnswers == after.loadingAnswers && before.textureEpoch == after.textureEpoch && before.created == after.created;
}

void RecorderSession::captureFrame() {
    if (!recording() || !sink) return;
    // What this call reads was drawn since the last one. The frame is good only if everything was
    // ready before it drew, nothing waited or stood in while it drew and nothing has come in since:
    // being idle now says nothing of a frame drawn before the creation that just ended, the very
    // frame the last load shows up late in
    const FrameMarks before = drawn;
    drawn = readMarks();
    const bool drawnReady = drawnWithAllReady(before, drawn);
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
    if (holdingFrame) {
        // the game is held (it draws, it does not advance) until a whole frame is drawn so
        if (drawnReady) {
            releaseHold();
            recordFrame();
            return;
        }
        checkHold(before);
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
    if (sink->failed()) {
        finish();
        return;
    }
    if (!inRange) return;
    if (!drawnReady) {
        holdingFrame = true;
        heldBeforeHold = Engine::isSimulationHeld();
        Engine::setSimulationHeld(true);
        holdStart = settleStart = Clock::now();
        return;
    }
    recordFrame();
}

void RecorderSession::releaseHold() {
    if (!holdingFrame) return;
    holdingFrame = false;
    Engine::setSimulationHeld(heldBeforeHold);
}

// The limits of a hold. Loads may take long, but one that finishes nothing for kLoadLimit fails the
// recording naming it (a screenshot does the same: never a stand-in). The rest stops being waited
// for after kSettleLimit, with a warning: objects that report loaded late or never do not stop a
// recording, and the frames after this one do not wait for them again.
void RecorderSession::checkHold(const FrameMarks& before) {
    const Clock::time_point now = Clock::now();
    if (before.created != drawn.created || before.textureEpoch != drawn.textureEpoch) holdStart = now;
    if (!drawn.assetsIdle || !before.assetsIdle) {
        settleStart = now;
        const auto limit = sink->faults.holdLimitMs >= 0 ? std::chrono::milliseconds(sink->faults.holdLimitMs)
                                                         : std::chrono::duration_cast<std::chrono::milliseconds>(kLoadLimit);
        if (now - holdStart > limit) {
            sink->fail("loading", "frame " + std::to_string(frameIndex) + " was still waiting after " + std::to_string(limit.count() / 1000) +
                                      " s, and a stand-in is never recorded: " + describeLoading());
            finish();
        }
        return;
    }
    if (now - settleStart > kSettleLimit) {
        std::string what = describeLoading();
        const auto add = [&what](const std::string& part) { what += (what.empty() ? "" : "; ") + part; };
        if (before.waitingDraws != drawn.waitingDraws) add("draws waiting for their pipelines");
        if (before.standIns != drawn.standIns || before.loadingAnswers != drawn.loadingAnswers) add("stand-in textures bound");
        if (before.textureEpoch != drawn.textureEpoch) add("textures becoming ready");
        if (before.created != drawn.created) add("shaders or pipelines being made");
        working.warnings.push_back("frame " + std::to_string(frameIndex) + " on: recorded without waiting for what does not settle: " + what);
        settleGivenUp = true;
    }
}

void RecorderSession::recordFrame() {
    if (!sink) return;
    std::vector<uint8_t> rgba;
    Framebuffer* framebuffer = host.gameFramebuffer();
    if (!GraphicUtils::readFramebufferRGBA(framebuffer, rgba)) {
        // (a movie with a frame missing is a damaged one)
        sink->fail("readback", "frame " + std::to_string(frameIndex) + " could not be read back");
        finish();
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
                Out::error("Recording failed: %s", result.error.c_str());
            }
            startWarning.clear();
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
    // a write failed off the main thread: Play goes on, the recording ends here
    if (sink && sink->failed()) {
        finish();
        return;
    }
    // (a frame held for what it waits for is not recorded yet)
    const bool done = last >= 0 && frameIndex >= last && !holdingFrame;
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
    releaseHold();
    Engine::setFixedDeltatime(previousFixedDelta);
    if (host.setGameRenderSize) host.setGameRenderSize(0, 0);
    if (audio) {
        AudioTap::unsubscribe(audioTap);
        AudioSystem::endDeterministic();
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
            working.maxQueued = s->maxQueued;
        }
        working.wallSeconds = std::chrono::duration<double>(Clock::now() - started).count();
        working.ok = working.error.empty() && working.frames > 0;
        if (working.error.empty() && working.frames == 0) working.error = "no frame was recorded";
        // the sidecar next to what stayed: the output, or the partial (nothing is written at the
        // output's path of a failed recording)
        const fs::path& described = working.ok ? working.output : working.partial;
        if (!described.empty()) {
            const fs::path sidecar = described.parent_path() / (described.filename().string() + ".recording.json");
            const io::WriteResult written = io::writeFileAtomic(sidecar, working.sidecarJson().dump(2) + "\n");
            if (written.ok()) working.sidecar = sidecar;
            else working.warnings.push_back("the sidecar could not be written: " + written.message);
        }
    });
}
