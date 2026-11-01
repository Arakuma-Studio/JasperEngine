// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Web's video (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6 "Web"): the
// browser's own <video> decodes everything but WebM AV1/Opus (which keeps the engine's software
// path for identical frames). One hidden element per source: a Blob URL of the file's bytes
// (assets and user files, from the engine's file system) or the http(s) URL itself, which the
// browser streams (cross-origin URLs need CORS: crossOrigin = "anonymous", else the texture
// upload throws). Its current frame goes straight into the player's RGBA target with
// texSubImage2D (no YUV pass). The element is its own clock: it follows the player's (play,
// pause, rate, a seek when they drift apart by more than 0.3 s). Its sound plays through a
// WebAudio GainNode at the player's volume (muted until the page's first user gesture, the
// browsers' autoplay rule). The JavaScript is inline (EM_JS), as the engine's other Web code.

#include "media/PlatformVideoBackend.h"

#include "Log.h"
#include "media/MediaByteSource.h"
#include "media/Mp4Probe.h"
#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "texture/Framebuffer.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

using namespace jasperengine;

EM_JS_DEPS(jasper_video, "$UTF8ToString,$stringToNewUTF8,$GL");

// the registry of elements, the first-gesture unlock and the hidden tab
EM_JS(void, jasper_video_install, (), {
    if (Module.jasperVideo) return;
    var registry = { next: 1, videos: {}, unlocked: false, context: null };
    Module.jasperVideo = registry;
    var gestures = ['pointerdown', 'mousedown', 'touchstart', 'touchend', 'keydown', 'click'];
    var unlock = function(){
        registry.unlocked = true;
        // the listeners stay until the page's audio really runs (a gesture the browser does not
        // count leaves the context suspended)
        var done = function(){
            if (registry.context && registry.context.state !== 'running') return;
            for (var i = 0; i < gestures.length; i++) window.removeEventListener(gestures[i], unlock, true);
        };
        if (registry.context && registry.context.state === 'suspended') registry.context.resume().then(done, function(){});
        else done();
        for (var id in registry.videos) {
            var v = registry.videos[id];
            // only elements with sound: one made without audio stays muted
            if (v.withAudio) v.element.muted = false;
            if (v.wantPlay && v.element.paused && !document.hidden) v.element.play().catch(function(){});
        }
    };
    for (var i = 0; i < gestures.length; i++) window.addEventListener(gestures[i], unlock, true);
    // a hidden tab pauses the elements (the engine's frames stop too); they go on when it shows
    document.addEventListener('visibilitychange', function(){
        for (var id in registry.videos) {
            var v = registry.videos[id];
            if (document.hidden) {
                if (!v.element.paused) { v.element.pause(); v.hiddenPaused = true; }
            } else if (v.hiddenPaused) {
                v.hiddenPaused = false;
                if (v.wantPlay) v.element.play().catch(function(){});
            }
        }
    });
});

// a new element; src a URL (http(s) or blob:); returns its id
EM_JS(int, jasper_video_create, (const char* srcPtr, int crossOrigin, int withAudio), {
    var registry = Module.jasperVideo;
    var id = registry.next++;
    var element = document.createElement('video');
    element.playsInline = true;
    element.setAttribute('playsinline', "");
    element.preload = 'auto';
    element.muted = !registry.unlocked || !withAudio;
    if (crossOrigin) element.crossOrigin = 'anonymous';
    var v = { element: element, state: 0, error: "", frames: 0, lastTime: -1, wantPlay: false, ended: false, seeking: false,
              waiting: false, gain: null, blob: "", withAudio: !!withAudio, cors: !!crossOrigin, fps: 0, lastMediaTime: -1 };
    registry.videos[id] = v;
    element.addEventListener('loadedmetadata', function(){ if (v.state === 0) v.state = 1; });
    element.addEventListener('canplay', function(){ if (v.state < 2) v.state = 2; });
    element.addEventListener('waiting', function(){ v.waiting = true; });
    element.addEventListener('playing', function(){ v.waiting = false; v.ended = false; });
    element.addEventListener('canplaythrough', function(){ v.waiting = false; });
    element.addEventListener('ended', function(){ v.ended = true; });
    element.addEventListener('seeking', function(){ v.seeking = true; });
    element.addEventListener('seeked', function(){ v.seeking = false; v.frames++; });
    element.addEventListener('error', function(){
        var e = element.error;
        var codes = { 1: 'aborted', 2: 'a network error', 3: 'a decoding error', 4: 'a format the browser does not play' };
        v.error = 'the browser could not play it: ' + ((e && codes[e.code]) || 'unknown error') + ((e && e.message) ? ' (' + e.message + ')' : "");
        if (v.cors) v.error += '; a cross-origin URL needs the server to send Access-Control-Allow-Origin';
        v.state = 3;
    });
    // new frames: requestVideoFrameCallback where the browser has it, else a changed currentTime
    if (element.requestVideoFrameCallback) {
        var onFrame = function(now, metadata){
            v.frames++;
            // the frame rate: the smallest step between consecutive frames' media times
            if (metadata && metadata.mediaTime !== undefined) {
                var step = metadata.mediaTime - v.lastMediaTime;
                if (v.lastMediaTime >= 0 && step > 0.001 && step < 1 && (v.fps === 0 || 1 / step > v.fps + 0.01)) v.fps = 1 / step;
                v.lastMediaTime = metadata.mediaTime;
            }
            if (registry.videos[id]) element.requestVideoFrameCallback(onFrame);
        };
        element.requestVideoFrameCallback(onFrame);
        v.rvfc = true;
    }
    element.src = UTF8ToString(srcPtr);
    element.load();
    return id;
});

// a Blob URL of bytes in the wasm memory (copied)
EM_JS(char*, jasper_video_blob, (const unsigned char* data, int size, const char* typePtr), {
    var bytes = HEAPU8.slice(data, data + size);
    var url = URL.createObjectURL(new Blob([bytes], { type: UTF8ToString(typePtr) }));
    return stringToNewUTF8(url);
});

EM_JS(void, jasper_video_destroy, (int id, const char* blobPtr), {
    var registry = Module.jasperVideo;
    var v = registry && registry.videos[id];
    if (!v) return;
    delete registry.videos[id];
    try { v.element.pause(); } catch (e) {}
    v.element.removeAttribute('src');
    try { v.element.load(); } catch (e) {}
    if (v.gain) { try { v.gain.disconnect(); } catch (e) {} }
    if (blobPtr) URL.revokeObjectURL(UTF8ToString(blobPtr));
});

// 0 opening, 1 metadata, 2 can play, 3 failed
EM_JS(int, jasper_video_state, (int id), {
    var v = Module.jasperVideo.videos[id];
    return v ? v.state : 3;
});

EM_JS(char*, jasper_video_error, (int id), {
    var v = Module.jasperVideo.videos[id];
    return stringToNewUTF8(v ? v.error : 'no such video');
});

// duration, width, height
EM_JS(double, jasper_video_metric, (int id, int which), {
    var v = Module.jasperVideo.videos[id];
    if (!v) return 0;
    var e = v.element;
    if (which === 0) return isFinite(e.duration) ? e.duration : 0;
    if (which === 1) return e.videoWidth;
    if (which === 2) return e.videoHeight;
    if (which === 3) return e.currentTime;
    if (which === 4) return v.frames;
    if (which === 5) {
        // seconds buffered ahead of the start
        var b = e.buffered;
        return b.length ? b.end(b.length - 1) : 0;
    }
    if (which === 6) return v.waiting || e.readyState < 2 ? 1 : 0;
    if (which === 7) return v.rvfc ? 1 : 0;
    if (which === 8) return v.fps;
    if (which === 9) return e.ended ? 1 : 0;
    return 0;
});

// follows the player's clock: its rate, play/pause, and its time when they drift apart (by more
// than 0.3 s while playing: a seek, a stall of either side; by half a frame while paused: a frame
// step or a seek). Not while the element buffers (the player waits for it then) or has ended
EM_JS(void, jasper_video_clock, (int id, int advancing, double rate, double seconds, int loop, double frameRate), {
    var v = Module.jasperVideo.videos[id];
    if (!v || v.state === 3) return;
    var e = v.element;
    e.loop = !!loop;
    if (rate > 0 && Math.abs(e.playbackRate - rate) > 1e-6) e.playbackRate = rate;
    var apart = Math.abs(e.currentTime - seconds);
    var tolerance = advancing ? 0.3 : 0.5 / Math.max(frameRate, 1);
    var atEnd = e.ended && isFinite(e.duration) && seconds >= e.duration - 0.3;
    if (!v.seeking && !v.waiting && !atEnd && e.readyState >= 1 && apart > tolerance && (!e.loop || apart < e.duration - 0.3))
        e.currentTime = seconds;
    v.wantPlay = !!advancing;
    if (advancing && e.paused && !e.ended && !document.hidden) {
        var p = e.play();
        if (p && p.catch) p.catch(function(err){
            if (!err || err.name !== 'NotAllowedError') return;
            // the autoplay rule: muted, then again (the first gesture unmutes it); blocked even
            // muted: reported (onErrorReceived)
            if (!e.muted) { e.muted = true; e.play().catch(function(again){ if (again && again.name === 'NotAllowedError') v.autoplayBlocked = true; }); }
            else v.autoplayBlocked = true;
        });
    } else if (!advancing && !e.paused) {
        e.pause();
    }
});

EM_JS(int, jasper_video_autoplay_blocked, (int id), {
    var v = Module.jasperVideo.videos[id];
    return v && v.autoplayBlocked ? 1 : 0;
});

// the volume: a GainNode (iOS ignores element.volume), else the element's own
EM_JS(void, jasper_video_volume, (int id, double volume), {
    var registry = Module.jasperVideo;
    var v = registry.videos[id];
    if (!v || !v.withAudio) return;
    if (!v.gain && !v.gainFailed) {
        try {
            var AudioContextClass = window.AudioContext || window.webkitAudioContext;
            if (!registry.context && AudioContextClass) registry.context = new AudioContextClass();
            if (registry.context) {
                var source = registry.context.createMediaElementSource(v.element);
                v.gain = registry.context.createGain();
                source.connect(v.gain);
                v.gain.connect(registry.context.destination);
            }
        } catch (err) { v.gainFailed = true; }
    }
    if (v.gain) v.gain.gain.value = volume;
    else v.element.volume = Math.max(0, Math.min(1, volume));
});

// the current frame into a texture (sokol's GL texture name) of the video's size; 0 on failure,
// -1 for a cross-origin frame the browser refuses (CORS)
EM_JS(int, jasper_video_upload, (int id, int textureName, int width, int height), {
    var v = Module.jasperVideo.videos[id];
    if (!v || v.element.readyState < 2) return 0;
    var gl = GLctx;
    var texture = GL.textures[textureName];
    if (!texture) return 0;
    var unit = gl.getParameter(gl.ACTIVE_TEXTURE);
    gl.activeTexture(gl.TEXTURE0);
    var previous = gl.getParameter(gl.TEXTURE_BINDING_2D);
    var flip = gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL);
    var premultiply = gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL);
    var alignment = gl.getParameter(gl.UNPACK_ALIGNMENT);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    // the picture's first row at texel row 0, as the engine's textures
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    var result = 1;
    try {
        if (v.element.videoWidth === width && v.element.videoHeight === height)
            gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, v.element);
        else
            result = 0;
    } catch (err) {
        result = (err && err.name === 'SecurityError') ? -1 : 0;
    }
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, flip);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, premultiply);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, alignment);
    gl.bindTexture(gl.TEXTURE_2D, previous);
    gl.activeTexture(unit);
    return result;
});

EM_JS(int, jasper_video_can_play, (const char* typePtr), {
    var answer = document.createElement('video').canPlayType(UTF8ToString(typePtr));
    return answer === 'probably' ? 2 : (answer === 'maybe' ? 1 : 0);
});

namespace {

    std::string fromJs(char* text){
        std::string value = text ? text : "";
        free(text);
        return value;
    }

    const char* mimeOf(const std::string& path){
        if (VideoFormats::isMp4Extension(path))
            return path.size() > 4 && (path.compare(path.size() - 4, 4, ".mov") == 0 || path.compare(path.size() - 4, 4, ".MOV") == 0) ? "video/quicktime" : "video/mp4";
        return "video/webm";
    }

    // an RFC 6381 string for a codec (the browser's canPlayType wants one)
    std::string mimeFor(const std::string& codec, const std::string& container){
        static const std::map<std::string, std::string> codecs = {
            {"h264", "avc1.42E01E"}, {"hevc", "hvc1.1.6.L93.B0"}, {"vp8", "vp8"}, {"vp9", container == "webm" ? "vp9" : "vp09.00.10.08"},
            {"av1", "av01.0.05M.08"}, {"aac", "mp4a.40.2"}, {"mp3", "mp3"}, {"opus", "opus"}, {"vorbis", "vorbis"}, {"flac", "flac"},
            {"ac3", "ac-3"}, {"eac3", "ec-3"}, {"alac", "alac"}, {"mpeg4", "mp4v.20.8"},
        };
        auto it = codecs.find(codec);
        const std::string type = container == "webm" ? "video/webm" : "video/mp4";
        return it == codecs.end() ? std::string() : type + "; codecs=\"" + it->second + "\"";
    }

    class WebVideoSource : public IVideoSource {
    public:
        WebVideoSource(const std::string& pathOrUrl, bool url, bool audio): name(pathOrUrl), withAudio(audio){
            jasper_video_install();
            std::string src = pathOrUrl;
            if (!url){
                // the file's bytes from the engine's file system (assets are preloaded there)
                std::string message;
                std::unique_ptr<MediaByteSource> bytes = MediaByteSource::openFile(pathOrUrl, message);
                std::vector<uint8_t> data;
                if (!bytes || !MediaByteSource::readAll(*bytes, data, (size_t)1 << 30)){
                    fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + pathOrUrl + "' could not be read" + (message.empty() ? "" : ": " + message));
                    return;
                }
                // the frame rate and index size from the file itself where it says
                std::unique_ptr<MediaByteSource> memory = MediaByteSource::openMemory(data, pathOrUrl);
                const Mp4ProbeResult probe = Mp4Probe::probe(*memory);
                if (probe.ok && probe.video.frameRate > 0.0){
                    frameRate = probe.video.frameRate;
                    rateFromFile = true;
                }
                if (probe.ok){
                    info.container = probe.container;
                    info.videoCodec = probe.video.fourcc;
                    info.audioCodec = probe.audio.fourcc;
                    info.rotation = probe.video.rotation;
                }
                blob = fromJs(jasper_video_blob(data.data(), (int)data.size(), mimeOf(pathOrUrl)));
                src = blob;
            }
            id = jasper_video_create(src.c_str(), url ? 1 : 0, audio ? 1 : 0);
        }

        ~WebVideoSource() override{
            if (id > 0)
                jasper_video_destroy(id, blob.empty() ? nullptr : blob.c_str());
        }

        State getState() const override{
            const_cast<WebVideoSource*>(this)->poll();
            return state;
        }
        State waitOpen() override{ return getState(); }
        const VideoInfo& getInfo() const override{ return info; }
        MediaError getError() const override{ return error; }
        void setLoop(bool value) override{ loop = value; }

        FrameResult acquireFrame(int iteration, double seconds, bool, int64_t shownSequence, VideoFrame& frame) override{
            poll();
            if (state == State::FAILED)
                return FrameResult::FAILED;
            if (state != State::READY)
                return FrameResult::NOT_READY;
            lastSeconds = seconds;
            followRate();
            jasper_video_clock(id, advancing ? 1 : 0, rate, seconds, loop ? 1 : 0, info.frameRate);
            // buffering: no frame of this time (the player's clock waits)
            if (jasper_video_metric(id, 6) > 0.0 && shownSequence >= 0)
                return FrameResult::NOT_READY;
            // a new frame: requestVideoFrameCallback's count where the browser has it (the
            // current time moves between frames), else a changed current time
            const double frames = jasper_video_metric(id, 4);
            const double current = jasper_video_metric(id, 3);
            const bool counted = jasper_video_metric(id, 7) > 0.0;
            const bool fresh = frames != lastFrames || (!counted && current != lastCurrent && !std::isnan(current));
            lastFrames = frames;
            lastCurrent = current;
            const int64_t sequence = sequenceFor(iteration, seconds);
            if (!fresh && shownSequence >= 0)
                return FrameResult::SAME;
            frame = VideoFrame();
            frame.width = info.displayWidth;
            frame.height = info.displayHeight;
            frame.sequence = sequence;
            frame.index = frameIndexFor(seconds);
            return FrameResult::NEW;
        }

        int64_t sequenceFor(int iteration, double seconds) const override{
            const int index = frameIndexFor(seconds);
            return index < 0 ? -1 : (int64_t)std::max(iteration, 0) * std::max(info.frameCount, 1) + index;
        }
        int frameIndexFor(double seconds) const override{
            if (state != State::READY)
                return -1;
            const int index = (int)std::floor(std::max(seconds, 0.0) * info.frameRate + 1e-6);
            return std::clamp(index, 0, std::max(info.frameCount - 1, 0));
        }
        double keyframeTimeAtOrBefore(double seconds) const override{ return seconds; }
        void syncAudio(int64_t) override{}
        bool isAudioSyncPending() const override{ return false; }
        void waitAudioSync() override{}
        std::shared_ptr<VideoAudioRing> getAudioRing() const override{ return nullptr; }
        bool pump(int) override{
            poll();
            return state == State::OPENING;
        }
        VideoStats getStats() const override{ return VideoStats(); }
        double getBufferedSeconds() const override{ return state == State::READY ? jasper_video_metric(id, 5) : 0.0; }
        bool isFrameExact() const override{ return false; }
        bool rendersToTexture() const override{ return true; }
        bool uploadTo(Framebuffer& target) override{
            if (state != State::READY || !target.isCreated())
                return false;
            const int texture = (int)target.getRender().getColorTexture().backend.getGLHandler();
            const int result = jasper_video_upload(id, texture, (int)target.getWidth(), (int)target.getHeight());
            if (result < 0){
                // reported to the player (onErrorReceived) as the source's failure
                fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + name + "': the browser refuses the frames of this cross-origin URL: "
                     "the server must send Access-Control-Allow-Origin (CORS)");
            }
            return result > 0;
        }
        bool playsOwnAudio() const override{ return true; }
        void setOutputVolume(float volume) override{
            if (state == State::READY && withAudio)
                jasper_video_volume(id, volume);
        }
        void setClock(bool clockAdvancing, double clockRate, double seconds) override{
            advancing = clockAdvancing;
            rate = clockRate;
            // the time comes with the clock: an audio-only source has no frames asked for
            lastSeconds = seconds;
            poll();
            if (state == State::READY)
                jasper_video_clock(id, advancing ? 1 : 0, rate, lastSeconds, loop ? 1 : 0, info.frameRate);
        }
        const char* backendName() const override{ return "browser"; }

    private:
        void fail(MediaErrorCode code, const std::string& message){
            state = State::FAILED;
            error.code = code;
            error.message = message;
        }

        void poll(){
            if (state == State::FAILED || id <= 0)
                return;
            const int jsState = jasper_video_state(id);
            if (jsState == 3){
                // also once playing (a network error mid-way): the player gets it (onErrorReceived)
                fail(state == State::READY ? MediaErrorCode::NETWORK : MediaErrorCode::SRC_NOT_SUPPORTED, "'" + name + "': " + fromJs(jasper_video_error(id)));
                return;
            }
            if (state == State::READY){
                if (jasper_video_autoplay_blocked(id))
                    fail(MediaErrorCode::ABORTED, "'" + name + "': the browser does not let it play before the page gets a click or tap (autoplay rule)");
                return;
            }
            if (jsState < 1)
                return;
            const double duration = jasper_video_metric(id, 0);
            info.hasVideo = jasper_video_metric(id, 1) > 0.0;
            info.hasAudio = withAudio;
            info.width = info.displayWidth = (int)jasper_video_metric(id, 1);
            info.height = info.displayHeight = (int)jasper_video_metric(id, 2);
            info.duration = duration;
            info.durationNs = (int64_t)std::llround(duration * 1e9);
            info.frameRate = frameRate;
            info.frameCount = std::max(1, (int)std::lround(duration * frameRate));
            state = State::READY;
        }

        // a URL (no probe of its file): the frame rate requestVideoFrameCallback measures, once known
        void followRate(){
            if (rateFromFile || rateMeasured)
                return;
            const double measured = jasper_video_metric(id, 8);
            if (measured <= 1.0)
                return;
            // a common rate near the measure (23.976, 25, 29.97, 30, 50, 59.94, 60)
            static const double rates[] = {23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 59.94, 60.0, 120.0};
            double best = measured;
            double closest = 1.0;
            for (double r : rates){
                const double off = std::fabs(r - measured) / r;
                if (off < 0.02 && off < closest){
                    closest = off;
                    best = r;
                }
            }
            rateMeasured = true;
            frameRate = best;
            info.frameRate = best;
            info.frameCount = std::max(1, (int)std::lround(info.duration * best));
        }

        std::string name;
        std::string blob;
        bool withAudio;
        int id = 0;
        State state = State::OPENING;
        VideoInfo info;
        MediaError error;
        bool loop = false;
        bool advancing = false;
        double rate = 1.0;
        double frameRate = 30.0;
        double lastSeconds = 0.0;
        double lastFrames = -1.0;
        double lastCurrent = -1.0;
        bool rateFromFile = false;
        bool rateMeasured = false;
    };

}

std::unique_ptr<IVideoSource> platformvideo::createBrowserSource(const std::string& pathOrUrl, bool url, bool audio){
    return std::make_unique<WebVideoSource>(pathOrUrl, url, audio);
}

platformvideo::Support platformvideo::browserSupports(const std::string& codec, const std::string& container){
    const std::string type = mimeFor(codec, container);
    if (type.empty())
        return Support::No;
    switch (jasper_video_can_play(type.c_str())){
        case 2: return Support::Yes;
        case 1: return Support::Maybe;
        default: return Support::No;
    }
}

// the platform hooks: the browser is the decoder (no PlatformDecoderBackend on the Web)

const char* platformvideo::backendName(){
    return "browser";
}

bool platformvideo::isAvailable(std::string*){
    return true;
}

std::unique_ptr<PlatformDecoderBackend> platformvideo::createBackend(){
    return nullptr;
}

platformvideo::Support platformvideo::supports(const std::string& codec){
    return browserSupports(codec, "mp4");
}

bool platformvideo::readsContainer(const std::string&){
    return true;
}

bool platformvideo::supportsGrowingSources(){
    return false;
}

std::string platformvideo::unsupportedHint(const std::string& codec){
    return "this browser has no " + codec + " decoder; convert the video to WebM AV1, which plays everywhere";
}
