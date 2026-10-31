// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Linux (and FreeBSD) video decoders: the system's FFmpeg through dlopen. See FFmpegBackend.h.

#include "FFmpegBackend.h"

#include "Log.h"
#include "media/PlatformVideoBackend.h"

#include <dlfcn.h>

#include <cstdlib>
#include <map>
#include <mutex>

using namespace jasperengine;

namespace {

    // the sets a distribution ships together, newest first: avcodec/avformat, avutil, swresample, swscale
    struct FFmpegSet {
        int avcodec;
        int avutil;
        int swresample;
        int swscale;
        const char* release;
    };
    constexpr FFmpegSet FFMPEG_SETS[] = {
        {62, 60, 6, 9, "8.x"},
        {61, 59, 5, 8, "7.x"},
        {60, 58, 4, 7, "6.x"},
        {59, 57, 4, 6, "5.x"},
        {58, 56, 3, 5, "4.x"},
    };

    struct Loaded {
        std::once_flag once;
        bool ok = false;
        std::string reason;
        FFmpegLibraries libs;
        std::mutex decodersMutex;
        std::map<std::string, bool> decoders;
    };

    // replaced (never freed: a backend may still use its libraries) by reloadForTesting
    Loaded*& loadedSlot(){
        static Loaded* value = new Loaded();
        return value;
    }

    Loaded* loadedState(){
        return loadedSlot();
    }

    std::string libraryPath(const char* name, int major){
        std::string file = std::string("lib") + name + ".so." + std::to_string(major);
        const char* dir = std::getenv("JASPERENGINE_FFMPEG_DIR");
        if (dir && *dir)
            return std::string(dir) + "/" + file;
        return file;
    }

    void* openLibrary(const char* name, int major){
        return dlopen(libraryPath(name, major).c_str(), RTLD_NOW | RTLD_LOCAL);
    }

    void closeAll(FFmpegLibraries& libs){
        for (void** handle : {&libs.avformat, &libs.avcodec, &libs.swscale, &libs.swresample, &libs.avutil}){
            if (*handle)
                dlclose(*handle);
            *handle = nullptr;
        }
    }

    unsigned versionOf(void* library, const char* symbol){
        using VersionFn = unsigned (*)();
        VersionFn fn = library ? reinterpret_cast<VersionFn>(dlsym(library, symbol)) : nullptr;
        return fn ? fn() : 0u;
    }

    void load(Loaded& state){
        for (const FFmpegSet& set : FFMPEG_SETS){
            FFmpegLibraries libs;
            // dependencies first: avformat's and avcodec's DT_NEEDED resolve to these
            libs.avutil = openLibrary("avutil", set.avutil);
            if (!libs.avutil)
                continue;
            libs.swresample = openLibrary("swresample", set.swresample);
            libs.avcodec = openLibrary("avcodec", set.avcodec);
            libs.avformat = openLibrary("avformat", set.avcodec);
            if (!libs.swresample || !libs.avcodec || !libs.avformat){
                closeAll(libs);
                continue;
            }
            libs.swscale = openLibrary("swscale", set.swscale);
            libs.avcodecMajor = set.avcodec;
            const unsigned version = versionOf(libs.avcodec, "avcodec_version");
            if ((int)(version >> 16) != set.avcodec){
                closeAll(libs);
                continue;
            }
            libs.version = std::string(set.release) + " (avcodec " + std::to_string(version >> 16) + "." +
                           std::to_string((version >> 8) & 0xFF) + ")";
            state.libs = libs;
            state.ok = true;
            Log::verbose("[Video] FFmpeg %s loaded for MP4 playback%s", libs.version.c_str(), libs.swscale ? "" : " (without libswscale)");
            return;
        }
        state.reason = FFmpegBackend::missingLibrariesMessage();
    }

    Loaded& ensureLoaded(){
        Loaded* state = loadedState();
        std::call_once(state->once, [state]{ load(*state); });
        return *state;
    }

    // FFmpeg's codec names for the codecs of the capability table
    bool knownCodec(const std::string& codec){
        static const char* const names[] = {"h264", "hevc", "vp8", "vp9", "av1", "aac", "mp3", "opus", "vorbis", "flac", "ac3", "eac3",
                                            "alac", "mpeg4", "pcm_s16le"};
        for (const char* name : names){
            if (codec == name)
                return true;
        }
        return false;
    }

}

const char* FFmpegBackend::missingLibrariesMessage(){
    return "MP4 playback on Linux uses the system's FFmpeg libraries: install libavcodec, libavformat and libswresample "
           "(Debian/Ubuntu: libavcodec60 libavformat60 libswresample4 or newer; Fedora: ffmpeg-libs from RPM Fusion: Fedora's "
           "ffmpeg-free has no H.264), or convert the video to WebM AV1";
}

bool FFmpegBackend::isAvailable(std::string* reason){
    Loaded& state = ensureLoaded();
    if (!state.ok && reason)
        *reason = state.reason;
    return state.ok;
}

std::string FFmpegBackend::getVersion(){
    Loaded& state = ensureLoaded();
    return state.ok ? state.libs.version : std::string();
}

std::unique_ptr<PlatformDecoderBackend> FFmpegBackend::create(){
    Loaded& state = ensureLoaded();
    if (!state.ok)
        return nullptr;
    std::string error;
    std::unique_ptr<PlatformDecoderBackend> backend;
    switch (state.libs.avcodecMajor){
        case 58: backend = ffmpeg58::create(state.libs, error); break;
        case 59: backend = ffmpeg59::create(state.libs, error); break;
        case 60: backend = ffmpeg60::create(state.libs, error); break;
        case 61: backend = ffmpeg61::create(state.libs, error); break;
        case 62: backend = ffmpeg62::create(state.libs, error); break;
        default: break;
    }
    if (!backend && !error.empty())
        Log::error("[Video] FFmpeg %s: %s", state.libs.version.c_str(), error.c_str());
    return backend;
}

bool FFmpegBackend::hasDecoder(const std::string& codec){
    Loaded& state = ensureLoaded();
    if (!state.ok)
        return false;
    std::lock_guard<std::mutex> lock(state.decodersMutex);
    auto found = state.decoders.find(codec);
    if (found != state.decoders.end())
        return found->second;
    bool has = false;
    switch (state.libs.avcodecMajor){
        case 58: has = ffmpeg58::hasDecoder(state.libs, codec.c_str()); break;
        case 59: has = ffmpeg59::hasDecoder(state.libs, codec.c_str()); break;
        case 60: has = ffmpeg60::hasDecoder(state.libs, codec.c_str()); break;
        case 61: has = ffmpeg61::hasDecoder(state.libs, codec.c_str()); break;
        case 62: has = ffmpeg62::hasDecoder(state.libs, codec.c_str()); break;
        default: break;
    }
    state.decoders[codec] = has;
    return has;
}

void FFmpegBackend::reloadForTesting(){
    loadedSlot() = new Loaded();
}

// ---------------------------------------------------------------- the platform hooks

const char* platformvideo::backendName(){
    return "ffmpeg";
}

bool platformvideo::isAvailable(std::string* reason){
    return FFmpegBackend::isAvailable(reason);
}

std::unique_ptr<PlatformDecoderBackend> platformvideo::createBackend(){
    return FFmpegBackend::create();
}

platformvideo::Support platformvideo::supports(const std::string& codec){
    if (!knownCodec(codec))
        return Support::NO;
    return FFmpegBackend::hasDecoder(codec) ? Support::YES : Support::NO;
}

bool platformvideo::readsContainer(const std::string& container){
    return container == "mp4" || container == "mov" || container == "3gp" || container == "webm" || container == "matroska";
}

bool platformvideo::supportsGrowingSources(){
    return true;
}

std::string platformvideo::unsupportedHint(const std::string& codec){
    std::string reason;
    if (!FFmpegBackend::isAvailable(&reason))
        return reason;
    if (codec == "h264" || codec == "hevc" || codec == "aac")
        return "this system's FFmpeg has no " + codec + " decoder (Fedora's ffmpeg-free has none: install ffmpeg-libs from RPM Fusion), "
               "or convert the video to WebM AV1";
    return "this system's FFmpeg has no " + codec + " decoder; convert the video to WebM AV1";
}
