// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoSourceFactory.h"

#include "GrowingFileReader.h"
#include "Log.h"
#include "MediaCache.h"
#include "Mp4Probe.h"
#include "PlatformVideoBackend.h"
#include "VideoFormats.h"
#include "VideoStream.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;

namespace {

    struct FactoryTesting {
        bool platformEnabled = true;
        PlatformBackendFactory backend;
        const char* backendName = nullptr;
        std::function<VideoSourceFactory::Support(const std::string&)> supports;
        VideoSourceFactory::SourceOverride override;
    };

    FactoryTesting& testing(){
        static FactoryTesting value;
        return value;
    }

    VideoSourceFactory::Support fromPlatform(platformvideo::Support support){
        switch (support){
            case platformvideo::Support::YES: return VideoSourceFactory::Support::YES;
            case platformvideo::Support::MAYBE: return VideoSourceFactory::Support::MAYBE;
            case platformvideo::Support::NO: break;
        }
        return VideoSourceFactory::Support::NO;
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    bool isWebmContainer(const std::string& container){
        return container == "webm" || container == "matroska";
    }

    bool fakeBackend(){
        return (bool)testing().backend;
    }

    PlatformBackendFactory backendFactory(){
        if (fakeBackend())
            return testing().backend;
        return []{ return platformvideo::createBackend(); };
    }

    const char* backendLabel(){
        if (fakeBackend())
            return testing().backendName ? testing().backendName : "fake";
#if defined(__EMSCRIPTEN__)
        return "browser";
#else
        return platformvideo::backendName();
#endif
    }

    const char* currentPlatformName(){
#if defined(__EMSCRIPTEN__)
        return "the Web";
#elif defined(__ANDROID__)
        return "Android";
#elif defined(__APPLE__)
        return "Apple platforms";
#elif defined(_WIN32)
        return "Windows";
#else
        return "Linux";
#endif
    }

    // ---------------------------------------------------------------- a source that failed

    class FailedSource : public IVideoSource {
    public:
        explicit FailedSource(MediaError failure): error(std::move(failure)){}
        State getState() const override{ return State::FAILED; }
        State waitOpen() override{ return State::FAILED; }
        const VideoInfo& getInfo() const override{ return info; }
        MediaError getError() const override{ return error; }
        void setLoop(bool) override{}
        FrameResult acquireFrame(int, double, bool, int64_t, VideoFrame&) override{ return FrameResult::FAILED; }
        int64_t sequenceFor(int, double) const override{ return -1; }
        int frameIndexFor(double) const override{ return -1; }
        double keyframeTimeAtOrBefore(double) const override{ return 0.0; }
        void syncAudio(int64_t) override{}
        bool isAudioSyncPending() const override{ return false; }
        void waitAudioSync() override{}
        std::shared_ptr<VideoAudioRing> getAudioRing() const override{ return nullptr; }
        bool pump(int) override{ return false; }
        VideoStats getStats() const override{ return VideoStats(); }
        double getBufferedSeconds() const override{ return 0.0; }
        bool isFrameExact() const override{ return true; }
        const char* backendName() const override{ return ""; }

    private:
        MediaError error;
        VideoInfo info;
    };

    std::unique_ptr<IVideoSource> failed(MediaErrorCode code, const std::string& message){
        MediaError error;
        error.code = code;
        error.message = message;
        return std::make_unique<FailedSource>(std::move(error));
    }

    std::unique_ptr<IVideoSource> createLocal(const std::string& path, const VideoSourceFactory::Options& options);

    // ---------------------------------------------------------------- a URL through the media cache

    class CachedUrlSource : public IVideoSource {
    public:
        CachedUrlSource(const std::string& source, const VideoSourceFactory::Options& opts): url(source), options(opts){
            download = MediaCache::acquire(url);
        }
        ~CachedUrlSource() override{
            // the decoders first (their reads of the growing file end), then the download
            inner.reset();
            MediaCache::release(download);
        }

        State getState() const override{
            const_cast<CachedUrlSource*>(this)->poll();
            if (inner)
                return inner->getState();
            return error ? State::FAILED : State::BUFFERING;
        }
        State waitOpen() override{
            // the download moves on the main thread's frames: never waited for here
            poll();
            return inner ? inner->waitOpen() : getState();
        }
        const VideoInfo& getInfo() const override{ return inner ? inner->getInfo() : empty; }
        MediaError getError() const override{
            if (inner)
                return inner->getError();
            return error;
        }
        void setLoop(bool value) override{
            loop = value;
            if (inner)
                inner->setLoop(value);
        }
        FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shown, VideoFrame& frame) override{
            poll();
            if (!inner)
                return error ? FrameResult::FAILED : FrameResult::NOT_READY;
            return inner->acquireFrame(iteration, seconds, wait, shown, frame);
        }
        int64_t sequenceFor(int iteration, double seconds) const override{ return inner ? inner->sequenceFor(iteration, seconds) : -1; }
        int frameIndexFor(double seconds) const override{ return inner ? inner->frameIndexFor(seconds) : -1; }
        double keyframeTimeAtOrBefore(double seconds) const override{ return inner ? inner->keyframeTimeAtOrBefore(seconds) : 0.0; }
        void syncAudio(int64_t frames) override{ if (inner) inner->syncAudio(frames); }
        bool isAudioSyncPending() const override{ return inner && inner->isAudioSyncPending(); }
        void waitAudioSync() override{ if (inner) inner->waitAudioSync(); }
        std::shared_ptr<VideoAudioRing> getAudioRing() const override{ return inner ? inner->getAudioRing() : nullptr; }
        bool pump(int steps) override{
            poll();
            return inner ? inner->pump(steps) : !error;
        }
        VideoStats getStats() const override{ return inner ? inner->getStats() : VideoStats(); }
        double getBufferedSeconds() const override{ return inner ? inner->getBufferedSeconds() : 0.0; }
        double getDownloadProgress() const override{ return download ? download->getProgress() : 0.0; }
        bool isFrameExact() const override{ return inner ? inner->isFrameExact() : true; }
        bool rendersToTexture() const override{ return inner && inner->rendersToTexture(); }
        bool uploadTo(Framebuffer& target) override{ return inner && inner->uploadTo(target); }
        bool playsOwnAudio() const override{ return inner && inner->playsOwnAudio(); }
        void setOutputVolume(float volume) override{ if (inner) inner->setOutputVolume(volume); }
        void setClock(bool advancing, double rate) override{ if (inner) inner->setClock(advancing, rate); }
        const char* backendName() const override{ return inner ? inner->backendName() : ""; }

    private:
        void poll(){
            if (inner || error || !download)
                return;
            switch (download->getStatus()){
                case MediaDownload::Status::FAILED:
                    error = download->getError();
                    if (!error){
                        error.code = MediaErrorCode::NETWORK;
                        error.message = "the download of " + url + " failed";
                    }
                    return;
                case MediaDownload::Status::COMPLETE:
                    inner = createLocal(download->getFilePath(), options);
                    break;
                case MediaDownload::Status::DOWNLOADING:
                    tryProgressive();
                    break;
            }
            if (inner)
                inner->setLoop(loop);
        }

        // a fast-start MP4 plays while it downloads (where the decoders read a growing file)
        void tryProgressive(){
            if (progressiveRefused || !platformvideo::supportsGrowingSources() || !VideoSourceFactory::arePlatformDecodersEnabled())
                return;
            std::shared_ptr<GrowingFileState> file = download->getFile();
            const int64_t available = file->getAvailable();
            // probe again once a quarter more arrived (at least 4 KiB): a short clip's moov box
            // is a few KiB, a long one's some MiB, and each probe reads it again from the start
            if (available < 32 || (probedAt >= 0 && available < probedAt + std::max<int64_t>(4096, probedAt / 4)))
                return;
            probedAt = available;
            std::unique_ptr<MediaByteSource> snapshot = GrowingFileReader::snapshot(file, url);
            uint8_t head[12] = {};
            if (snapshot->readAt(0, head, sizeof(head)) < 8)
                return;
            if (!Mp4Probe::sniff(head, sizeof(head))){
                // WebM (indexed whole at open) and the rest wait for the whole file
                progressiveRefused = true;
                return;
            }
            const Mp4ProbeResult probe = Mp4Probe::probe(*snapshot);
            if (probe.needMoreData && !probe.ok){
                if (probe.mdatOffset >= 0 && probe.moovOffset < 0){
                    // the media data comes first: the moov is at the end
                    refuse("its moov box follows the media data");
                }
                return;
            }
            if (!probe.ok){
                refuse(probe.error);
                return;
            }
            if (!probe.fastStart || probe.fragmented){
                refuse(probe.fragmented ? "it is fragmented" : "its moov box follows the media data");
                return;
            }
            std::shared_ptr<MediaByteSource> bytes = std::make_shared<GrowingFileReader>(file, url);
            VideoProbe check;
            check.container = probe.container;
            if (probe.video.kind == Mp4Track::Kind::VIDEO){
                check.videoCodec = probe.video.fourcc;
                check.videoCodecString = probe.video.codecString;
            }
            if (probe.audio.kind == Mp4Track::Kind::AUDIO){
                check.audioCodec = probe.audio.fourcc;
                check.audioCodecString = probe.audio.codecString;
            }
            check.encrypted = probe.video.encrypted || probe.audio.encrypted;
            std::string reason;
            if (VideoSourceFactory::probeSupport(check, &reason) == VideoSourceFactory::Support::NO){
                error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                error.message = "'" + url + "': " + reason;
                return;
            }
            Log::verbose("[Video] '%s': playing while it downloads (fast start)", url.c_str());
            inner = std::make_unique<PlatformVideoSource>(bytes, &probe, backendFactory(), backendLabel(), options.audio);
        }

        void refuse(const std::string& why){
            progressiveRefused = true;
            Log::verbose("[Video] '%s' plays once downloaded: %s (re-encode with -movflags +faststart, or the Inspector's "
                         "Optimize for streaming, to start while it downloads)", url.c_str(), why.c_str());
        }

        std::string url;
        VideoSourceFactory::Options options;
        std::shared_ptr<MediaDownload> download;
        std::unique_ptr<IVideoSource> inner;
        MediaError error;
        VideoInfo empty;
        bool loop = false;
        bool progressiveRefused = false;
        int64_t probedAt = -1;
    };

    // ---------------------------------------------------------------- local files

    std::string unsupportedMessage(const std::string& path, const std::string& reason){
        return "'" + path + "': " + reason;
    }

    std::unique_ptr<IVideoSource> createLocal(const std::string& path, const VideoSourceFactory::Options& options){
        std::string openError;
        std::shared_ptr<MediaByteSource> bytes = MediaByteSource::openFile(path, openError);
        // what cannot be read fails as the WebM path always did (its messages)
        if (!bytes)
            return std::make_unique<VideoStream>(path, options.threaded, options.audio);
        uint8_t head[12] = {};
        const int64_t got = bytes->readAt(0, head, sizeof(head));
        const bool ebml = got >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3;
        const bool mp4 = !ebml && got >= 8 && Mp4Probe::sniff(head, (size_t)got);

        if (!mp4){
            // WebM: AV1/Opus in software; another codec through the platform's decoders when they have it
            if (ebml){
                const VideoProbe probe = VideoFormats::probeFile(path);
                std::string backend;
                std::string reason;
                if (probe.backend != "webm" && !probe.backend.empty() &&
                    VideoSourceFactory::probeSupport(probe, &reason, &backend) != VideoSourceFactory::Support::NO && backend != "webm"){
#if defined(__EMSCRIPTEN__)
                    return platformvideo::createBrowserSource(path, false, options.audio);
#else
                    return std::make_unique<PlatformVideoSource>(bytes, nullptr, backendFactory(), backendLabel(), options.audio);
#endif
                }
            }
            return std::make_unique<VideoStream>(path, options.threaded, options.audio);
        }

        const Mp4ProbeResult probe = Mp4Probe::probe(*bytes);
        if (!probe.ok)
            return failed(MediaErrorCode::SRC_NOT_SUPPORTED, unsupportedMessage(path, probe.error));
        VideoProbe check;
        check.container = probe.container;
        if (probe.video.kind == Mp4Track::Kind::VIDEO){
            check.videoCodec = probe.video.fourcc;
            check.videoCodecString = probe.video.codecString;
        }
        if (probe.audio.kind == Mp4Track::Kind::AUDIO){
            check.audioCodec = probe.audio.fourcc;
            check.audioCodecString = probe.audio.codecString;
        }
        check.encrypted = probe.video.encrypted || probe.audio.encrypted;
        std::string reason;
        if (VideoSourceFactory::probeSupport(check, &reason) == VideoSourceFactory::Support::NO)
            return failed(MediaErrorCode::SRC_NOT_SUPPORTED, unsupportedMessage(path, reason));
#if defined(__EMSCRIPTEN__)
        if (!fakeBackend())
            return platformvideo::createBrowserSource(path, false, options.audio);
#endif
        return std::make_unique<PlatformVideoSource>(bytes, &probe, backendFactory(), backendLabel(), options.audio);
    }

    // ---------------------------------------------------------------- the platform matrix

    enum class Cell { YES, NEEDS_FFMPEG, NEEDS_EXTENSION, DEVICE, NO };

    int rank(Cell cell){
        switch (cell){
            case Cell::YES: return 0;
            case Cell::NEEDS_FFMPEG: return 1;
            case Cell::NEEDS_EXTENSION: return 2;
            case Cell::DEVICE: return 3;
            case Cell::NO: return 4;
        }
        return 4;
    }

    const char* cellText(Cell cell){
        switch (cell){
            case Cell::YES: return "yes";
            case Cell::NEEDS_FFMPEG: return "needs_ffmpeg";
            case Cell::NEEDS_EXTENSION: return "needs_extension";
            case Cell::DEVICE: return "device_dependent";
            case Cell::NO: return "no";
        }
        return "no";
    }

    // what each OS's own decoders have (docs/mcp/guide/video.md's matrix); Linux is FFmpeg's
    Cell cellFor(VideoSourceFactory::Platform platform, const std::string& codec, bool webm){
        using P = VideoSourceFactory::Platform;
        const bool apple = platform == P::MacOS || platform == P::IOS;
        // AVFoundation reads no WebM; Media Foundation reads WebM only for VP9 with the extension
        if (webm && apple)
            return Cell::NO;
        if (platform == P::Linux)
            return codec.empty() ? Cell::NO : Cell::NEEDS_FFMPEG;
        struct Row { const char* codec; Cell windows, apple, android, web; };
        static const Row rows[] = {
            {"h264",   Cell::YES, Cell::YES, Cell::YES, Cell::YES},
            {"hevc",   Cell::NEEDS_EXTENSION, Cell::YES, Cell::DEVICE, Cell::DEVICE},
            {"vp8",    Cell::NO, Cell::NO, Cell::YES, Cell::DEVICE},
            {"vp9",    Cell::NEEDS_EXTENSION, Cell::NO, Cell::YES, Cell::DEVICE},
            {"av1",    Cell::NEEDS_EXTENSION, Cell::DEVICE, Cell::DEVICE, Cell::DEVICE},
            {"mpeg4",  Cell::YES, Cell::YES, Cell::YES, Cell::NO},
            {"aac",    Cell::YES, Cell::YES, Cell::YES, Cell::YES},
            {"mp3",    Cell::YES, Cell::YES, Cell::YES, Cell::YES},
            {"opus",   Cell::NEEDS_EXTENSION, Cell::NO, Cell::YES, Cell::DEVICE},
            {"vorbis", Cell::NEEDS_EXTENSION, Cell::NO, Cell::YES, Cell::DEVICE},
            {"flac",   Cell::YES, Cell::YES, Cell::YES, Cell::YES},
            {"alac",   Cell::YES, Cell::YES, Cell::DEVICE, Cell::DEVICE},
            {"ac3",    Cell::DEVICE, Cell::YES, Cell::DEVICE, Cell::DEVICE},
            {"eac3",   Cell::DEVICE, Cell::YES, Cell::DEVICE, Cell::DEVICE},
        };
        for (const Row& row : rows){
            if (codec != row.codec)
                continue;
            switch (platform){
                case P::Windows: return row.windows;
                case P::MacOS:
                case P::IOS: return row.apple;
                case P::Android: return row.android;
                case P::Web: return row.web;
                case P::Linux: break;
            }
        }
        return Cell::NO;
    }

}

// ---------------------------------------------------------------- public

bool VideoSourceFactory::isUrl(const std::string& path){
    const std::string head = lower(path.substr(0, 8));
    return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
}

std::unique_ptr<IVideoSource> VideoSourceFactory::create(const std::string& pathOrUrl){
    return create(pathOrUrl, Options());
}

std::unique_ptr<IVideoSource> VideoSourceFactory::create(const std::string& pathOrUrl, const Options& options){
    if (testing().override){
        if (std::unique_ptr<IVideoSource> source = testing().override(pathOrUrl, options))
            return source;
    }
    if (pathOrUrl.empty())
        return failed(MediaErrorCode::SRC_NOT_SUPPORTED, "no video (the path is empty)");
    if (isUrl(pathOrUrl)){
#if defined(__EMSCRIPTEN__)
        // the browser streams it (CORS applies)
        if (arePlatformDecodersEnabled() && !fakeBackend())
            return platformvideo::createBrowserSource(pathOrUrl, true, options.audio);
#endif
        return std::make_unique<CachedUrlSource>(pathOrUrl, options);
    }
    return createLocal(pathOrUrl, options);
}

std::unique_ptr<IVideoSource> VideoSourceFactory::createPlatform(std::shared_ptr<MediaByteSource> bytes, const Options& options){
    if (!bytes)
        return failed(MediaErrorCode::SRC_NOT_SUPPORTED, "no video bytes");
    uint8_t head[12] = {};
    const int64_t got = bytes->readAt(0, head, sizeof(head));
    if (got >= 8 && Mp4Probe::sniff(head, (size_t)got)){
        const Mp4ProbeResult probe = Mp4Probe::probe(*bytes);
        if (!probe.ok)
            return failed(MediaErrorCode::SRC_NOT_SUPPORTED, unsupportedMessage(bytes->path(), probe.error));
        return std::make_unique<PlatformVideoSource>(bytes, &probe, backendFactory(), backendLabel(), options.audio);
    }
    return std::make_unique<PlatformVideoSource>(bytes, nullptr, backendFactory(), backendLabel(), options.audio);
}

VideoSourceFactory::Support VideoSourceFactory::codecSupport(const std::string& codec, const std::string& container){
    const std::string name = VideoFormats::decoderName(codec);
    const bool webm = isWebmContainer(container);
    // the engine's own decoders: AV1 and Opus in WebM, everywhere
    if (webm && (name == "av1" || name == "opus"))
        return Support::YES;
    if (!testing().platformEnabled || name.empty())
        return Support::NO;
    if (fakeBackend())
        return testing().supports ? testing().supports(name) : Support::YES;
#if defined(__EMSCRIPTEN__)
    return fromPlatform(platformvideo::browserSupports(name, webm ? "webm" : "mp4"));
#else
    if (!platformvideo::readsContainer(webm ? "webm" : "mp4") || !platformvideo::isAvailable(nullptr))
        return Support::NO;
    return fromPlatform(platformvideo::supports(name));
#endif
}

bool VideoSourceFactory::readsContainer(const std::string& container){
    if (isWebmContainer(container))
        return true;
    if (!testing().platformEnabled)
        return false;
    if (fakeBackend())
        return true;
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return platformvideo::readsContainer(container) && platformvideo::isAvailable(nullptr);
#endif
}

VideoSourceFactory::Support VideoSourceFactory::probeSupport(const VideoProbe& probe, std::string* reason, std::string* backend){
    auto refuse = [&](const std::string& why){
        if (reason)
            *reason = why;
        if (backend)
            backend->clear();
        return Support::NO;
    };
    if (probe.encrypted)
        return refuse("the video is encrypted (DRM): the engine plays no protected content");
    const bool webm = isWebmContainer(probe.container);
    const bool mp4 = probe.container == "mp4" || probe.container == "mov" || probe.container == "3gp";
    if (!webm && !mp4)
        return refuse(probe.error ? probe.error.message : "not a video the engine plays (WebM or MP4)");
    const std::string video = VideoFormats::decoderName(probe.videoCodecString.empty() ? probe.videoCodec : probe.videoCodecString);
    const std::string audio = VideoFormats::decoderName(probe.audioCodecString.empty() ? probe.audioCodec : probe.audioCodecString);
    // the software path: AV1 (and/or Opus) WebM, on every platform, even where the OS decodes AV1
    if (webm && (probe.videoCodec.empty() || video == "av1") && (probe.audioCodec.empty() || audio == "opus" || video == "av1")){
        if (backend)
            *backend = "webm";
        return Support::YES;
    }
    const char* platform = backendLabel();
    std::string missing;
    if (!platformAvailable(&missing) && !fakeBackend())
        return refuse(missing.empty() ? std::string("this platform has no decoder for it; ") + VideoFormats::conversionHint() : missing);
    Support result = Support::YES;
    if (!probe.videoCodec.empty()){
        const Support support = codecSupport(video.empty() ? probe.videoCodec : video, probe.container);
        if (support == Support::NO){
            std::string hint;
#if !defined(__EMSCRIPTEN__)
            if (!fakeBackend())
                hint = platformvideo::unsupportedHint(video.empty() ? probe.videoCodec : video);
#endif
            if (hint.empty())
                hint = VideoFormats::conversionHint();
            return refuse(VideoFormats::codecName(probe.videoCodec) + " video does not play on " + currentPlatformName() + ": " + hint);
        }
        if (support == Support::MAYBE)
            result = Support::MAYBE;
    }else if (!probe.audioCodec.empty()){
        // sound only: the audio codec decides
        const Support support = codecSupport(audio.empty() ? probe.audioCodec : audio, probe.container);
        if (support == Support::NO)
            return refuse(VideoFormats::codecName(probe.audioCodec) + " audio does not play on " + currentPlatformName());
        if (support == Support::MAYBE)
            result = Support::MAYBE;
    }else{
        return refuse("the file has no video or audio track");
    }
    if (backend)
        *backend = platform;
    return result;
}

const char* VideoSourceFactory::platformBackend(){
    if (!testing().platformEnabled)
        return "";
    return backendLabel();
}

bool VideoSourceFactory::platformAvailable(std::string* reason){
    if (!testing().platformEnabled){
        if (reason)
            *reason = "the platform's video decoders are switched off (tests)";
        return false;
    }
    if (fakeBackend())
        return true;
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return platformvideo::isAvailable(reason);
#endif
}

const char* VideoSourceFactory::playableOn(Platform platform, const VideoProbe& probe){
    if (probe.encrypted || !probe.readable)
        return "no";
    const bool webm = isWebmContainer(probe.container);
    const bool mp4 = probe.container == "mp4" || probe.container == "mov" || probe.container == "3gp";
    if (!webm && !mp4)
        return "no";
    const std::string video = VideoFormats::decoderName(probe.videoCodecString.empty() ? probe.videoCodec : probe.videoCodecString);
    const std::string audio = VideoFormats::decoderName(probe.audioCodecString.empty() ? probe.audioCodec : probe.audioCodecString);
    if (webm && (probe.videoCodec.empty() || video == "av1") && (probe.audioCodec.empty() || audio == "opus"))
        return "yes";
    Cell worst = Cell::YES;
    auto take = [&](Cell cell){
        if (rank(cell) > rank(worst))
            worst = cell;
    };
    if (!probe.videoCodec.empty())
        take(webm && video == "av1" ? Cell::YES : cellFor(platform, video, webm));
    if (!probe.audioCodec.empty())
        take(webm && audio == "opus" ? Cell::YES : cellFor(platform, audio, webm));
    return cellText(worst);
}

const char* VideoSourceFactory::platformId(Platform platform){
    switch (platform){
        case Platform::Linux: return "linux";
        case Platform::Windows: return "windows";
        case Platform::MacOS: return "macos";
        case Platform::IOS: return "ios";
        case Platform::Android: return "android";
        case Platform::Web: return "web";
    }
    return "";
}

const char* VideoSourceFactory::platformLabel(Platform platform){
    switch (platform){
        case Platform::Linux: return "Linux";
        case Platform::Windows: return "Windows";
        case Platform::MacOS: return "macOS";
        case Platform::IOS: return "iOS";
        case Platform::Android: return "Android";
        case Platform::Web: return "Web";
    }
    return "";
}

void VideoSourceFactory::setPlatformDecodersEnabledForTesting(bool enabled){
    testing().platformEnabled = enabled;
}

bool VideoSourceFactory::arePlatformDecodersEnabled(){
    return testing().platformEnabled;
}

void VideoSourceFactory::setPlatformBackendForTesting(PlatformBackendFactory factory, const char* name, std::function<Support(const std::string&)> supports){
    testing().backend = std::move(factory);
    testing().backendName = name;
    testing().supports = std::move(supports);
}

void VideoSourceFactory::setSourceOverrideForTesting(SourceOverride override){
    testing().override = std::move(override);
}
