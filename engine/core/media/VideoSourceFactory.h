// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOSOURCEFACTORY_H
#define VIDEOSOURCEFACTORY_H

#include "Export.h"
#include "IVideoSource.h"
#include "PlatformVideoSource.h"

#include <functional>
#include <memory>
#include <string>

namespace jasperengine {

    struct VideoProbe;

    // Opens a video for a player, choosing the source by the file's content, not its extension
    // (docs/superpowers/specs/2026-10-31-video-mp4-design.md §5.3): WebM AV1/Opus -> VideoStream
    // (software, deterministic, every platform); anything else -> this platform's decoders
    // (PlatformVideoSource: FFmpeg on Linux, MediaCodec, AVFoundation, Media Foundation; the
    // browser's element on the Web) when they have its codecs, else MEDIA_ERR_SRC_NOT_SUPPORTED
    // with what to do. http(s) URLs download into the media cache (MediaCache) and play from the
    // file, while it grows where the decoders can (fast-start MP4s); the Web streams them itself.
    class JASPERENGINE_API VideoSourceFactory {
    public:
        // CamelCase: Objective-C defines YES and NO as macros
        enum class Support { No, Maybe, Yes };
        enum class Platform { Linux, Windows, MacOS, IOS, Android, Web };
        static constexpr int PLATFORM_COUNT = 6;

        struct Options {
            bool audio = true;          // decode the audio (false: the picture only)
            bool threaded = true;       // a worker thread (where the platform has threads)
        };

        // Never null: a source that cannot be made is FAILED with the reason (getError)
        static std::unique_ptr<IVideoSource> create(const std::string& pathOrUrl, const Options& options);
        static std::unique_ptr<IVideoSource> create(const std::string& pathOrUrl);
        // a source over bytes that may still be downloading (the media cache's progressive start)
        static std::unique_ptr<IVideoSource> createPlatform(std::shared_ptr<MediaByteSource> bytes, const Options& options);

        static bool isUrl(const std::string& path);

        // Whether this platform plays a codec now: any id (Matroska "V_VP9", MP4 "avc1", RFC 6381
        // "avc1.42E01E", FFmpeg "h264") in a container ("webm" or "mp4"); AV1 and Opus in WebM
        // are always YES (software). MAYBE: it depends on the device (Android HEVC, the Web)
        static Support codecSupport(const std::string& codec, const std::string& container);
        // the container is read by this platform's decoders
        static bool readsContainer(const std::string& container);
        // a probed file plays here; NO with the reason (the platform's hint); backend: which
        // source would play it ("webm", "ffmpeg", ...)
        static Support probeSupport(const VideoProbe& probe, std::string* reason, std::string* backend = nullptr);
        // the platform decoders' name ("ffmpeg", "mediacodec", "avfoundation", "mediafoundation",
        // "browser"); "" without
        static const char* platformBackend();
        // false with what to install (Linux without FFmpeg)
        static bool platformAvailable(std::string* reason = nullptr);

        // The per-platform matrix (editor, get_asset_info): whether a probed file plays on an
        // export platform, from what each OS ships: "yes", "needs_ffmpeg" (Linux: the system's
        // FFmpeg), "needs_extension" (Windows: HEVC/VP9 Video Extensions), "device_dependent"
        // (Android HEVC/AV1, the browser's HEVC), "no"
        static const char* playableOn(Platform platform, const VideoProbe& probe);
        static const char* platformId(Platform platform);       // "linux", "windows", "macos", "ios", "android", "web"
        static const char* platformLabel(Platform platform);    // "Linux", "Windows", ...

        // Tests. Platform decoders off: only the WebM path plays (the HTML UI suites' fixed world).
        static void setPlatformDecodersEnabledForTesting(bool enabled);
        static bool arePlatformDecodersEnabled();
        // a fake platform backend (null factory: the real one again)
        static void setPlatformBackendForTesting(PlatformBackendFactory factory, const char* name,
                                                 std::function<Support(const std::string&)> supports);
        // every create() asks this first (a non-null result is used)
        using SourceOverride = std::function<std::unique_ptr<IVideoSource>(const std::string&, const Options&)>;
        static void setSourceOverrideForTesting(SourceOverride override);
    };

}

#endif //VIDEOSOURCEFACTORY_H
