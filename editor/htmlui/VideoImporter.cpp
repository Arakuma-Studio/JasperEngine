// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoImporter.h"
#include "util/FileUtils.h"

#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "util/CommandRunner.h"
#include "util/ShellEnv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
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

namespace {

    // an MP4 (or a WebM of another codec) one of the export platforms plays: kept as it is
    bool playsSomewhere(const jasperengine::VideoProbe& probe){
        using jasperengine::VideoSourceFactory;
        if (probe.playable)
            return true;
        if (!probe.readable || probe.encrypted || probe.container.empty())
            return false;
        for (int p = 0; p < VideoSourceFactory::PLATFORM_COUNT; p++){
            if (std::string(VideoSourceFactory::playableOn((VideoSourceFactory::Platform)p, probe)) != "no")
                return true;
        }
        return false;
    }

}

bool VideoImporter::isPlayable(const fs::path& path){
    return playsSomewhere(probeHost(path));
}

void VideoImporter::setFfmpegPath(const std::string& path){
    std::lock_guard<std::mutex> lock(ffmpegMutex);
    ffmpegOverride = path;
}

std::string VideoImporter::findFfmpeg(std::string* why, const std::string* overridePath){
    if (const char* env = std::getenv("JASPER_FFMPEG")){
        // set: it is the one (an empty or wrong value means "none", for tests and locked-down setups)
        if (isExecutable(env))
            return env;
        if (why)
            *why = std::string("JASPER_FFMPEG is '") + env + "', which is not an executable";
        return std::string();
    }
    std::string configured;
    if (overridePath){
        configured = *overridePath;
    }else{
        std::lock_guard<std::mutex> lock(ffmpegMutex);
        configured = ffmpegOverride;
    }
    if (isExecutable(configured))
        return configured;
    ShellEnv::bootstrapPath();
#ifdef _WIN32
    const std::string found = ShellEnv::findExecutable("ffmpeg.exe");
#else
    const std::string found = ShellEnv::findExecutable("ffmpeg");
#endif
    if (found.empty() && why)
        *why = configured.empty() ? "no ffmpeg on PATH (and JASPER_FFMPEG is not set)"
                                  : "the FFmpeg setting '" + configured + "' is not a file, and there is no ffmpeg on PATH";
    return found;
}

std::vector<std::string> VideoImporter::av1Encoders(const std::string& ffmpeg){
    std::vector<std::string> found;
    if (ffmpeg.empty())
        return found;
    std::string output;
    CommandRunner runner;
    runner.run({ffmpeg, "-hide_banner", "-encoders"}, fs::current_path(), [&output](const std::string& line){ output += line + "\n"; });
    for (const char* name : {"libsvtav1", "libaom-av1", "librav1e"}){
        if (output.find(std::string(" ") + name + " ") != std::string::npos)
            found.push_back(name);
    }
    return found;
}

std::vector<std::string> VideoImporter::conversionArgv(const std::string& ffmpeg, const std::string& encoder, const fs::path& input,
                                                       const fs::path& output, bool toneMap){
    std::vector<std::string> video;
    if (encoder == "libsvtav1")
        video = {"-c:v", "libsvtav1", "-preset", "6", "-crf", "32"};
    else if (encoder == "libaom-av1")
        video = {"-c:v", "libaom-av1", "-cpu-used", "6", "-row-mt", "1", "-crf", "30", "-b:v", "0"};
    else
        video = {"-c:v", "librav1e", "-speed", "6", "-qp", "100"};
    // keyframes every 2 s (seeks decode at most that much), at most 4096 px, even sizes for 4:2:0
    std::string chain = "scale='min(4096,iw)':'min(4096,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2";
    // HDR to SDR: linear light, BT.709 primaries, Hable's curve, back to BT.709 limited range
    if (toneMap)
        chain = "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv," + chain;
    std::vector<std::string> argv = { ffmpeg, "-nostdin", "-hide_banner", "-y", "-i", pathString(input),
                                      "-map", "0:v:0?", "-map", "0:a:0?", "-vf", chain };
    argv.insert(argv.end(), video.begin(), video.end());
    for (const char* a : {"-g", "60", "-keyint_min", "60", "-force_key_frames", "expr:gte(t,n_forced*2)",
                          "-pix_fmt", "yuv420p", "-c:a", "libopus", "-b:a", "128k", "-ac", "2", "-f", "webm"}) {
        argv.push_back(a);
    }
    argv.push_back(pathString(output));
    return argv;
}

std::string VideoImporter::conversionCommand(const std::string& ffmpeg, const std::string& encoder, const fs::path& input,
                                             const fs::path& output, bool toneMap){
    // a human-readable line (shown with HTMLUI-IMP-0010/0011 so a user can run it by hand); the
    // import itself runs the argv array, never this string
    std::string line;
    for (const std::string& a : conversionArgv(ffmpeg, encoder, input, output, toneMap)) {
        if (!line.empty()) line.push_back(' ');
        line += quote(a);
    }
    return line;
}

bool VideoImporter::isHdrInput(const std::string& ffmpeg, const fs::path& input){
    if (ffmpeg.empty())
        return false;
#ifdef _WIN32
    const fs::path ffprobe = fs::u8path(ffmpeg).parent_path() / "ffprobe.exe";
#else
    const fs::path ffprobe = fs::u8path(ffmpeg).parent_path() / "ffprobe";
#endif
    if (!isExecutable(pathString(ffprobe)))
        return false;
    std::string output;
    CommandRunner runner;
    runner.run({pathString(ffprobe), "-v", "error", "-select_streams", "v:0", "-show_entries",
                "stream=color_transfer", "-of", "csv=p=0", pathString(input)}, fs::current_path(),
               [&output](const std::string& line){ output += line + "\n"; });
    return output.find("smpte2084") != std::string::npos || output.find("arib-std-b67") != std::string::npos;
}

bool VideoImporter::hasToneMapping(const std::string& ffmpeg){
    if (ffmpeg.empty())
        return false;
    std::string output;
    CommandRunner runner;
    runner.run({ffmpeg, "-hide_banner", "-filters"}, fs::current_path(), [&output](const std::string& line){ output += line + "\n"; });
    return output.find(" zscale ") != std::string::npos && output.find(" tonemap ") != std::string::npos;
}

VideoImportResult VideoImporter::importFile(const fs::path& input, const fs::path& targetDir, bool overwrite,
                                            const std::function<void(const std::string&)>& onLine, bool convert){
    VideoImportResult result;
    std::error_code ec;
    const std::string name = pathString(input.filename());
    // (in a write scope, an MCP import: beneath its root; the copies too)
    jasperengine::editor::FileUtils::createDirectories(targetDir, ec);

    // already the engine's format: copied
    const jasperengine::VideoProbe probe = probeHost(input);
    const bool webmPlayable = probe.playable && probe.audioPlayable && probe.backend == "webm";
    const bool platformPlayable = !webmPlayable && playsSomewhere(probe) && probe.container != "webm" && probe.container != "matroska";
    if (!convert && (webmPlayable || platformPlayable)){
        result.output = targetDir / input.filename();
        if (fs::exists(result.output, ec) && !overwrite){
            result.message = pathString(result.output.filename()) + " already exists";
            return result;
        }
        jasperengine::editor::FileUtils::copyPath(input, result.output, true, ec);
        result.ok = !ec;
        result.copied = !ec;
        result.message = ec ? "could not copy " + name + ": " + ec.message() :
                         webmPlayable ? name + " is AV1/Opus WebM: copied" :
                         name + " (" + jasperengine::VideoFormats::codecName(probe.videoCodec) + " in " + probe.container +
                             ") plays through each platform's decoders: copied (Convert to WebM (AV1) in the Project window plays it in software everywhere)";
        return result;
    }
    // already AV1/Opus WebM: nothing to convert
    if (convert && webmPlayable){
        result.output = input;
        result.ok = true;
        result.message = name + " is AV1/Opus WebM already";
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
            jasperengine::editor::FileUtils::copyPath(input, copy, true, ec);
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
    // 8-bit 4:2:0 SDR (what every platform's decoder path is tested with); HDR input tone-mapped
    // when FFmpeg can (zimg), else converted as it is (the colours look washed out: said below)
    result.hdr = isHdrInput(result.ffmpeg, input);
    result.toneMapped = result.hdr && hasToneMapping(result.ffmpeg);
    result.command = conversionCommand(result.ffmpeg, result.encoder, input, output, result.toneMapped);
    std::string tail;
    const auto start = std::chrono::steady_clock::now();
    CommandRunner runner;
    const bool success = runner.run(conversionArgv(result.ffmpeg, result.encoder, input, output, result.toneMapped), targetDir, [&](const std::string& line){
        tail += line + "\n";
        if (tail.size() > 4000)
            tail.erase(0, tail.size() - 4000);
        if (onLine)
            onLine(line);
    });
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const bool playable = success && probeHost(output).playable;
    if (!playable){
        jasperengine::editor::FileUtils::removePath(output, ec);   // (in a write scope: beneath its root)
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
    result.message = name + " converted to " + pathString(output.filename()) + " (AV1 " + result.encoder + " + Opus, " + seconds + " s" +
                     (result.toneMapped ? ", HDR tone-mapped to SDR" : "") + ")";
    if (result.hdr && !result.toneMapped)
        result.message += "; it is HDR and this FFmpeg has no zscale/tonemap filters (zimg), so it was not tone-mapped: its colours will look washed out";
    return result;
}

// ---------------------------------------------------------------- downloaded videos

RemoteVideoImporter::RemoteVideoImporter(RemoteImporter& importer): importer(importer) {}

ImportedResource RemoteVideoImporter::run(const std::string& url, const std::string& body, const SniffedContent& sniffed, const ImportLocation& at){
    ImportedResource result;
    result.url = url;
    std::string host, path, query;
    splitUrl(url, host, path, query);
    std::string name = safeFileName(urlBaseName(url));
    if (name.empty())
        name = "video";
    const std::string stem = "videos/" + safeFileName(host) + "/" + urlHash(url, 12) + "-" + name;

    // FFmpeg reads and writes files: a scratch directory of this import (removed below)
    std::error_code ec;
    const fs::path scratch = fs::temp_directory_path(ec) / ("jasper-video-import-" + urlHash(url, 12) + "-" +
                                                          std::to_string(std::random_device{}()));
    fs::create_directories(scratch / "out", ec);
    const fs::path downloaded = scratch / (name + "." + sniffed.extension());
    {
        std::ofstream out(downloaded, std::ios::binary);
        out.write(body.data(), (std::streamsize)body.size());
    }
    importer.progress("[HTML UI] " + url + ": " + sniffed.name() + (VideoImporter::isPlayable(downloaded) ? "" : ", converting with FFmpeg"));
    const VideoImportResult converted = VideoImporter::importFile(downloaded, scratch / "out", true);

    std::string local, content;
    if (converted.ok){
        // played as it is (copied: its own extension, an MP4 stays one) or converted: the .webm
        std::string extension = pathString(converted.output.extension());
        if (extension.empty())
            extension = ".webm";
        local = importer.importPath(stem + extension);
        std::ifstream in(converted.output, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }else if (converted.copied){
        // no FFmpeg (or no AV1 encoder): the original, which the engine will not play
        local = importer.importPath(stem + "." + sniffed.extension());
        content = body;
    }
    // the messages name the saved file (asset-relative), not the scratch copy
    auto inProject = [&](std::string text){
        const std::string from = pathString(downloaded), to = local.empty() ? importer.importPath(stem + "." + sniffed.extension()) : local;
        for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
            text.replace(pos, from.size(), to);
        const std::string outDir = pathString(scratch / "out") + "/";
        const std::string toDir = to.substr(0, to.find_last_of('/') + 1);
        for (size_t pos = text.find(outDir); pos != std::string::npos; pos = text.find(outDir, pos + toDir.size()))
            text.replace(pos, outDir.size(), toDir);
        return text;
    };
    fs::remove_all(scratch, ec);
    for (const jasperengine::htmlui::compat::Diagnostic& d : converted.diagnostics)
        importer.report(d.code, at, url, inProject(d.message), inProject(d.suggestion));
    if (local.empty()){
        result.error = inProject(converted.message);
        return result;
    }

    ImportStaging staging;
    staging.add(local, std::move(content));
    jasperengine::htmlui::compat::ImportEntry entry;
    entry.url = url;
    entry.kind = "video";
    entry.path = local;
    entry.files = {local};
    entry.licenseName = "unknown";
    entry.source = host;
    if (!importer.finish(staging, {entry}, result))
        return result;
    importer.report(hc::codes::LicenseNote, at, url, "the license of the video '" + url + "' is unknown: verify that the game may ship it");
    return result;
}
