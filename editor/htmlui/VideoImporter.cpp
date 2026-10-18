// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoImporter.h"

#include "htmlui/media/VideoFormats.h"
#include "util/CommandRunner.h"
#include "util/ShellEnv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;
namespace hc = jasperengine::htmlui::compat;
using namespace jasperengine::editor::htmlui;

namespace {

    std::mutex ffmpegMutex;
    std::string ffmpegOverride;

    // one argument for the platform shell CommandRunner uses (/bin/sh, cmd.exe)
    std::string quote(const std::string& value){
#ifdef _WIN32
        std::string out = "\"";
        for (char c : value)
            out += c == '"' ? std::string("\\\"") : std::string(1, c);
        return out + "\"";
#else
        std::string out = "'";
        for (char c : value)
            out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return out + "'";
#endif
    }

    std::string pathString(const fs::path& path){
        return path.u8string();
    }

    hc::Diagnostic diagnostic(hc::DiagCode code, hc::Severity severity, const std::string& file, const std::string& message, const std::string& fix){
        hc::Diagnostic d;
        d.code = code;
        d.severity = severity;
        d.file = file;
        d.message = message;
        d.suggestion = fix;
        d.subject = file;
        return d;
    }

    // the probe over the host file (fopen, not the engine's FileData: that resolves asset paths
    // through the engine's System, which the importer does not need)
    jasperengine::VideoProbe probeHost(const fs::path& path){
#ifdef _WIN32
        FILE* handle = _wfopen(path.wstring().c_str(), L"rb");
#else
        FILE* handle = std::fopen(path.c_str(), "rb");
#endif
        return jasperengine::VideoFormats::probeStdio(handle, pathString(path.filename()));
    }

    bool isExecutable(const std::string& path){
        std::error_code ec;
        return !path.empty() && fs::is_regular_file(fs::u8path(path), ec);
    }

}

bool VideoImporter::isVideoFile(const fs::path& path){
    return jasperengine::VideoFormats::isVideoPath(pathString(path));
}

bool VideoImporter::isPlayable(const fs::path& path){
    return probeHost(path).playable;
}

void VideoImporter::setFfmpegPath(const std::string& path){
    std::lock_guard<std::mutex> lock(ffmpegMutex);
    ffmpegOverride = path;
}

std::string VideoImporter::findFfmpeg(std::string* why){
    if (const char* env = std::getenv("JASPER_FFMPEG")){
        // set: it is the one (an empty or wrong value means "none", for tests and locked-down setups)
        if (isExecutable(env))
            return env;
        if (why)
            *why = std::string("JASPER_FFMPEG is '") + env + "', which is not an executable";
        return std::string();
    }
    {
        std::lock_guard<std::mutex> lock(ffmpegMutex);
        if (isExecutable(ffmpegOverride))
            return ffmpegOverride;
    }
    ShellEnv::bootstrapPath();
#ifdef _WIN32
    const std::string found = ShellEnv::findExecutable("ffmpeg.exe");
#else
    const std::string found = ShellEnv::findExecutable("ffmpeg");
#endif
    if (found.empty() && why)
        *why = "no ffmpeg on PATH (and JASPER_FFMPEG is not set)";
    return found;
}

std::vector<std::string> VideoImporter::av1Encoders(const std::string& ffmpeg){
    std::vector<std::string> found;
    if (ffmpeg.empty())
        return found;
    std::string output;
    CommandRunner runner;
    runner.run(quote(ffmpeg) + " -hide_banner -encoders", fs::current_path(), [&output](const std::string& line){ output += line + "\n"; });
    for (const char* name : {"libsvtav1", "libaom-av1", "librav1e"}){
        if (output.find(std::string(" ") + name + " ") != std::string::npos)
            found.push_back(name);
    }
    return found;
}

std::string VideoImporter::conversionCommand(const std::string& ffmpeg, const std::string& encoder, const fs::path& input,
                                             const fs::path& output, bool tenBit){
    std::string video;
    if (encoder == "libsvtav1")
        video = "-c:v libsvtav1 -preset 6 -crf 32";
    else if (encoder == "libaom-av1")
        video = "-c:v libaom-av1 -cpu-used 6 -row-mt 1 -crf 30 -b:v 0";
    else
        video = "-c:v librav1e -speed 6 -qp 100";
    // keyframes every 2 s (seeks decode at most that much), at most 4096 px, even sizes for 4:2:0
    const std::string filter = "-vf " + quote("scale='min(4096,iw)':'min(4096,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2");
    return quote(ffmpeg) + " -nostdin -hide_banner -y -i " + quote(pathString(input)) + " -map 0:v:0? -map 0:a:0? " + filter + " " + video +
           " -g 60 -keyint_min 60 -force_key_frames " + quote("expr:gte(t,n_forced*2)") + " -pix_fmt " +
           (tenBit ? "yuv420p10le" : "yuv420p") + " -c:a libopus -b:a 128k -ac 2 -f webm " + quote(pathString(output));
}

VideoImportResult VideoImporter::importFile(const fs::path& input, const fs::path& targetDir, bool overwrite,
                                            const std::function<void(const std::string&)>& onLine){
    VideoImportResult result;
    std::error_code ec;
    const std::string name = pathString(input.filename());
    fs::create_directories(targetDir, ec);

    // already the engine's format: copied
    const jasperengine::VideoProbe probe = probeHost(input);
    if (probe.playable && probe.audioPlayable){
        result.output = targetDir / input.filename();
        if (fs::exists(result.output, ec) && !overwrite){
            result.message = pathString(result.output.filename()) + " already exists";
            return result;
        }
        fs::copy_file(input, result.output, fs::copy_options::overwrite_existing, ec);
        result.ok = !ec;
        result.copied = !ec;
        result.message = ec ? "could not copy " + name + ": " + ec.message() : name + " is AV1/Opus WebM: copied";
        return result;
    }

    fs::path output = targetDir / input.filename();
    output.replace_extension(".webm");
    // a VP9 .webm converted in place would overwrite its own source name
    if (fs::equivalent(output, input, ec))
        output = targetDir / (pathString(input.stem()) + ".av1.webm");
    std::string why;
    result.ffmpeg = findFfmpeg(&why);
    const std::vector<std::string> encoders = av1Encoders(result.ffmpeg);
    if (!encoders.empty())
        result.encoder = encoders.front();
    result.command = conversionCommand(result.ffmpeg.empty() ? "ffmpeg" : result.ffmpeg, result.encoder.empty() ? "libsvtav1" : result.encoder,
                                       input, output, false);

    if (result.ffmpeg.empty() || result.encoder.empty()){
        // copied as it is; the engine will not play it until it is converted
        const fs::path copy = targetDir / input.filename();
        if (!fs::exists(copy, ec) || overwrite)
            fs::copy_file(input, copy, fs::copy_options::overwrite_existing, ec);
        result.output = copy;
        result.copied = !ec;
        result.ok = false;
        const std::string reason = result.ffmpeg.empty() ? why : "the FFmpeg at " + result.ffmpeg + " has no AV1 encoder (libsvtav1, libaom-av1 or librav1e)";
        result.message = name + " is not AV1/Opus WebM and was copied unconverted: " + reason;
        result.diagnostics.push_back(diagnostic(hc::codes::VideoNotConverted, hc::Severity::Warning, name,
                                                "'" + name + "' is " + (probe.container.empty() ? std::string("not WebM") :
                                                    jasperengine::VideoFormats::codecName(probe.videoCodec) + " in " + probe.container) +
                                                    ": the engine does not play it, and it was not converted (" + reason + ")",
                                                "install FFmpeg with SVT-AV1 and libopus (or set JASPER_FFMPEG) and import it again, or run: " +
                                                    result.command));
        return result;
    }

    if (fs::exists(output, ec) && !overwrite){
        result.message = pathString(output.filename()) + " already exists; pass overwrite to replace it";
        result.output = output;
        return result;
    }
    // 8-bit 4:2:0 SDR (what every platform's decoder path is tested with)
    result.command = conversionCommand(result.ffmpeg, result.encoder, input, output, false);
    std::string tail;
    const auto start = std::chrono::steady_clock::now();
    CommandRunner runner;
    const bool success = runner.run(result.command, targetDir, [&](const std::string& line){
        tail += line + "\n";
        if (tail.size() > 4000)
            tail.erase(0, tail.size() - 4000);
        if (onLine)
            onLine(line);
    });
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const bool playable = success && probeHost(output).playable;
    if (!playable){
        fs::remove(output, ec);
        // the last lines say what went wrong
        std::string last;
        size_t lines = 0;
        for (size_t at = tail.size(); at > 0 && lines < 6; at--){
            if (tail[at - 1] == '\n')
                lines++;
            last.insert(last.begin(), tail[at - 1]);
        }
        result.message = "FFmpeg could not convert " + name;
        result.diagnostics.push_back(diagnostic(hc::codes::VideoConversionFailed, hc::Severity::Error, name,
                                                "FFmpeg could not convert '" + name + "' to AV1 + Opus WebM: " + last,
                                                "check the input plays (ffprobe), or convert it by hand: " + result.command));
        return result;
    }
    result.ok = true;
    result.converted = true;
    result.output = output;
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", result.seconds);
    result.message = name + " converted to " + pathString(output.filename()) + " (AV1 " + result.encoder + " + Opus, " + seconds + " s)";
    return result;
}
