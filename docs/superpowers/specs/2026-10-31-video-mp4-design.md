# Video: MP4 through platform decoders, URL sources and a Video Player component (engine gaps #15, `gaps/video-mp4`)

Status: binding design. Read `docs/engine-gaps/AGENT-BRIEF.md` first: its shared conventions are binding.
It extends `docs/html-ui/phase-6-video.md` ("phase 6"), which shipped WebM AV1+Opus decoded in software
(dav1d, libopus, libwebm) for the HTML UI `<video>` and listed platform decoders, MP4 and network sources as
out of scope (phase 6 §2 "Out", §3.1.5). This spec adds them **next to** that path: the WebM/AV1 path stays the
deterministic, everywhere-format; MP4 uses each platform's own decoder.

## 1 Goals

1. Play **MP4 (H.264 + AAC)** on Android, iOS, macOS, Windows, Web and Linux (Linux: with the system's FFmpeg
   libraries), **H.265/HEVC** where the platform decodes it, plus VP8/VP9 WebM where a platform decoder has it.
2. Play from **local assets**, from **files downloaded at run time** into the user data dir
   (`System.getUserDataPath()`, `data://`), and from **`http(s)://` URLs** (download into a media cache, with
   progressive start where the backend supports it; the browser streams natively on Web).
3. A Unity-like **`VideoPlayer` component**: play into a render texture (usable by UI Images, materials, Lua),
   override a renderer's material texture, the camera far/near plane, or API only; audio into the engine's
   audio (SoLoud), directly or through an AudioSource (and its mixer bus when `audio-mixer` lands); events
   (prepared, started, loop point reached, error, seek completed); Lua API; Inspector; MCP.
4. The HTML UI `<video>` plays the same sources (`video/mp4`, URLs) through the same backends.
5. The game's use: `S/Ads/FakeAdsManager.cs:44-48,72-83` downloads an mp4 house ad to `persistentDataPath`, sets
   `videoPlayer.url`, renders into a 1920x1080 RenderTexture shown by a RawImage, `Prepare()`, waits
   `isPrepared`, `Play()`, polls `frame`/`frameCount`/`isPlaying`; the component
   (`Scripts/Ads/AdmobManager.prefab`) uses `m_RenderMode: 2` (RenderTexture), `m_TimeUpdateMode: 2` (unscaled),
   `m_AudioOutputMode: 2` (Direct), no loop, `playOnAwake: 0`.

## 2 Non-goals

- Shipping any patent-encumbered decoder (H.264/HEVC/AAC) inside the engine: never (§5.1).
- Adaptive streaming (HLS/DASH), DRM (FairPlay/Widevine), live streams, alpha video, 360/3D layouts, multiple
  audio tracks (the first audio track plays), captions. HLS works incidentally on Web (Safari) only; not tested.
- Video recording/encoding.
- Frame-exact determinism of platform decoders (they are not bit-exact; goldens use the WebM path).
- `VideoClip` import settings (transcoding on import): the existing optional FFmpeg conversion to WebM stays.

## 3 Users and stories

- **Porter**: "My house ad is an mp4 I download to the user data dir. I add a Video Player (Render Texture mode,
  unscaled time), point a UI Image at its texture, call `prepare()`, and play it when `onPrepareCompleted` fires,
  on my Android and iOS builds."
- **Menu designer**: a looping intro.mp4 behind the main menu (Camera Far Plane mode).
- **3D artist**: a TV screen mesh plays a clip (Material Override, `baseColorTexture`).
- **Web developer**: an HTML `<video src="https://cdn.example.com/trailer.mp4">` in the HTML UI.
- **AI agent**: `create_entity type=video_player`, `set_properties {"url": "data://ads/ad1.mp4",
  "renderMode": "render_texture"}`, `set_properties` on an Image `{"texture": "video|<id>"}`, `play`, then
  `get_entity` shows `{prepared, time, frame, frameCount, backend: "ffmpeg", videoCodec: "H.264"}`, `screenshot`.
- **Tester**: on Linux without FFmpeg libraries, `onErrorReceived` says exactly what to install.

## 4 Research summary

- **Unity VideoPlayer** (<https://docs.unity3d.com/ScriptReference/Video.VideoPlayer.html>; codec pages
  <https://docs.unity.com/en-us/engine/current/manual/video/sources/file-compatibility>): platform decoders
  (Media Foundation on Windows, AVFoundation on Apple, MediaCodec on Android, the browser on WebGL); H.264 is the
  recommended codec for most platforms, **VP8/Vorbis WebM is the only recommended format on Linux** (Unity has
  no H.264 on Linux). Render modes CameraFarPlane/CameraNearPlane/RenderTexture/MaterialOverride/APIOnly,
  `aspectRatio`, `timeUpdateMode` (DSPTime/GameTime/UnscaledGameTime), `audioOutputMode`
  (None/AudioSource/Direct/APIOnly), `waitForFirstFrame`, `skipOnDrop`, events `prepareCompleted`, `started`,
  `loopPointReached`, `errorReceived`, `seekCompleted`, `frameReady`, `frameDropped`. **Take**: the component
  shape and names, platform decoders. **Improve**: Linux plays H.264 when the system has FFmpeg (Firefox's
  approach), and WebM AV1 plays everywhere in software (phase 6).
- **Godot 4**: `VideoStreamPlayer` with Theora only (software); MP4 via GDExtensions (FFmpeg). **Leave**:
  Theora (Chromium dropped it, phase 6 §3.1.2).
- **Unreal Media Framework**: per-platform "players" (WmfMedia, AvfMedia, AndroidMedia, Electra), a
  `MediaTexture` sink. **Take**: the per-platform player behind one interface and a texture sink.
- **Firefox on Linux** dlopens the system `libavcodec` (majors 53..61) with vendored headers per major
  (`dom/media/platforms/ffmpeg/ffmpegNN`), so it decodes H.264 without shipping a decoder. **Take**.
- **OpenH264** (Cisco binary, royalties paid by Cisco): its binary licence requires the binary to be downloaded
  separately to the user's device and be user-controllable, and it is video-only (no AAC)
  (<http://www.openh264.org/BINARY_LICENSE.txt>; <https://lwn.net/Articles/572205/>). **Leave**: a runtime download
  flow for games is poor UX and AAC would still be missing.
- **GStreamer via dlopen**: present on most desktops (core lib present on this host), but H.264 needs
  gst-libav/openh264 plugins and the pipeline API is far from the pull model below. **Leave** (keep as a later
  alternative backend behind the same interface).
- **Android**: NDK `AMediaExtractor`/`AMediaCodec` (API 21, `libmediandk`), `AMediaDataSource` (API 28) for
  custom byte sources; mandated decoders: H.264 AVC Baseline/Main/High, AAC; VP8/VP9; HEVC on most devices.
- **Apple**: `AVAssetReader` + `AVAssetReaderTrackOutput` (local files; pull model; NV12 `CVPixelBuffer`s, Linear
  PCM), H.264 + HEVC (iOS 11 / macOS 10.13+), AAC.
- **Windows**: Media Foundation `IMFSourceReader` (H.264 + AAC built in since Windows 7; "N" editions need the
  Media Feature Pack; HEVC needs Microsoft's "HEVC Video Extensions"; VP9 "VP9 Video Extensions"), NV12/float
  PCM output with `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING`, custom `IMFByteStream` for non-file sources.
- **Web**: `HTMLVideoElement` + `texSubImage2D(video)` into a WebGL2 texture; H.264/AAC MP4 in every current
  browser, HEVC in Safari and in Chromium with hardware support, VP9/AV1 WebM in Chromium/Firefox; cross-origin
  URLs need CORS (`crossOrigin = "anonymous"`) or the texture upload throws; autoplay with sound needs a user
  gesture.

## 5 Product decisions

1. **Licensing**: the engine ships no H.264/HEVC/AAC decoder. Platform decoders carry their vendors' licences;
   on Linux the engine dlopens the system FFmpeg (LGPL dynamic loading, nothing linked or shipped; FFmpeg's
   public headers of each supported major are vendored under `engine/libs/ffmpeg-headers/<major>/` with
   `COPYING.LGPLv2.1`, listed in `NOTICE.md`). The guide tells game authors that the MPEG LA / Via LA AVC pool
   licenses *content* separately and to check it for paid titles; WebM AV1 remains the royalty-free option.
2. **Two kinds of source behind one interface**: `IVideoSource` (pull: open, info, acquire the frame for a media
   time, audio ring, seek, loop) implemented by phase 6's `VideoStream` (WebM AV1/Opus, software, deterministic,
   every platform) and by one **platform source** per OS. The player (component or `<video>`) does not know
   which one it has: the same clock, events, A/V sync and colour conversion (phase 6 §3.5-3.7).
3. **Selection by content, not extension**: the probe (`VideoFormats::probeFile`, extended with an ISO-BMFF
   reader) decides. WebM AV1/Opus -> software source everywhere (even where a platform could decode AV1, for
   identical frames). Anything else -> the platform source if it reports support for the codecs, else
   `MEDIA_ERR_SRC_NOT_SUPPORTED` with a platform-specific hint (§7).
4. **CPU frames, one GPU path**: platform sources output NV12/I420 CPU planes; NV12 is de-interleaved to I420 on
   the decode thread (about 0.3 ms per 720p frame), so the existing Y/U/V R8 upload and `HTMLUI_POST` `Yuv`
   conversion are reused unchanged. **Web is the exception**: the browser's element is uploaded straight into the
   RGBA target with `texSubImage2D` (no YUV pass).
5. **The engine owns the clock** on every CPU backend: decoders are pulled ahead into the 4-frame queue, frames
   are chosen by the player's media time (game time scaled or unscaled), audio is slaved to it (phase 6 §3.7). On
   Web the browser element is the clock (playbackRate follows the engine's speed; `timeScale` 0 pauses it).
6. **URLs**: `http(s)://` sources are downloaded by the engine's HTTP client (feature `http`, #1) into the
   **media cache** `<userdata>/.cache/media/<sha256(url)>.<ext>` (ETag/Last-Modified revalidation, Range resume,
   LRU cap 512 MB, `VideoPlayer.clearCache()`), then played from the file. **Progressive start** (play while
   downloading) where the backend can read a growing file through the engine's reader: Linux (FFmpeg custom
   AVIO), Windows (custom `IMFByteStream`), Android API 28+ (`AMediaDataSource`), for MP4s whose `moov` box
   precedes `mdat` ("fast start", detected by the probe); otherwise prepared when the download completes. Web:
   the browser streams the URL itself (no cache).
7. **Video Player component** with Unity's names (§6.4). The render texture is the player's own RGBA8 target,
   linked as `video|<entity>` exactly like camera (`camera|<e>`) and fluid (`fluid|<e>`) texture links
   (`editor/util/CameraTextureLink.h`), so UI Images, materials and Lua (`player.texture`) use it.
8. **Default time update mode is GameTime** (Unity's default; `timeScale` slows/pauses video); `UnscaledGameTime`
   for ads and menus. Phase 6's `<video>` keeps the UI clock.
9. **Audio**: `Direct` (default) plays a SoLoud voice at `directVolume` x global volume; `AudioSource` plays it
   through the target AudioSource's voice settings (3D position/attenuation, volume, and its mixer bus when
   `audio-mixer` #6 lands); `None` decodes no audio. Web: the browser plays the audio; the engine applies its
   volume/mute through a WebAudio gain node (no 3D, no buses there; documented).
10. **H.265**: supported wherever the platform reports a decoder (Apple, Android devices, Windows with the
    extension, Linux FFmpeg, Safari/Chromium with hardware). `VideoPlayer.canPlay(path)` / `VideoFormats::
    canPlayType` answer per platform at run time; the editor shows a per-platform matrix for each asset.

## 6 Design

### 6.1 Files and refactor

Move the platform-neutral media files out of the HTML UI into `engine/core/media/` (git mv, includes updated,
`tools/htmlui/xcode_sync_compat.py` scope and `engine/CMakeLists.txt` updated): `VideoTypes`, `VideoFormats`,
`VideoDemuxer`, `VideoDecoder`, `Av1Decoder`, `OpusAudioDecoder`, `VideoAudio`, `VideoPlayback`, `VideoStream`,
`VideoColorConvert`. The RmlUi-bound files stay (`UIVideoElement`, `UIVideoControls`, `UIElementVideo`). New:

| File | Purpose |
|---|---|
| `media/IVideoSource.h` | the interface (VideoStream's public API, §6.2) |
| `media/VideoSourceFactory.{h,cpp}` | probe, choose and create a source; per-platform capability table |
| `media/Mp4Probe.{h,cpp}` | ISO-BMFF box reader: brands, tracks, `stsd` fourccs (`avc1/avc3`, `hvc1/hev1`, `mp4a`, `Opus`, `vp09`, `av01`), size, duration, frame rate, fast start, rotation matrix |
| `media/MediaCache.{h,cpp}` | URL -> cache file, download state, progress, revalidation, LRU |
| `media/GrowingFileReader.{h,cpp}` | blocking positional reads over a file being downloaded (waits on the cache's condition variable, fails on download error or `abort()`) |
| `media/PlatformVideoSource.{h,cpp}` | shared worker/queue/ring logic for platform sources (template-method over a `PlatformDecoderBackend`) |
| `media/VideoTextureConverter.{h,cpp}` | Y/U/V plane textures + RGBA8 `Framebuffer`, conversion pass on the render system side (same `HTMLUI_POST` `Yuv` shader variant) |
| `platform/linux/FFmpegBackend.{h,cpp}` | dlopen FFmpeg (§6.6) |
| `platform/android/MediaCodecBackend.{h,cpp}` | NDK media |
| `platform/apple/AVFoundationBackend.{h,mm}` | iOS + macOS |
| `platform/windows/MediaFoundationBackend.{h,cpp}` | Media Foundation |
| `platform/web/WebVideoSource.{h,cpp}` + `engine/platform/web/jasper_video.js` (`--js-library`) | browser element |
| `component/VideoPlayerComponent.h`, `object/VideoPlayer.{h,cpp}`, `subsystem/VideoPlayerSystem.{h,cpp}` | the component |

### 6.2 IVideoSource

```cpp
class JASPERENGINE_API IVideoSource {
public:
    using Info = VideoStream::Info;  using Stats = VideoStream::Stats;            // moved to VideoTypes.h as VideoInfo/VideoStats
    enum class State { OPENING, BUFFERING, READY, FAILED };                     // BUFFERING: waiting for download bytes
    virtual ~IVideoSource() = default;
    virtual State getState() const = 0;
    virtual State waitOpen() = 0;
    virtual const VideoInfo& getInfo() const = 0;
    virtual MediaError getError() const = 0;
    virtual void setLoop(bool loop) = 0;
    virtual VideoStream::FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame) = 0;
    virtual int64_t sequenceFor(int iteration, double seconds) const = 0;
    virtual int frameIndexFor(double seconds) const = 0;
    virtual double keyframeTimeAtOrBefore(double seconds) const = 0;
    virtual void syncAudio(int64_t frames) = 0;
    virtual bool isAudioSyncPending() const = 0;
    virtual void waitAudioSync() = 0;
    virtual std::shared_ptr<VideoAudioRing> getAudioRing() const = 0;
    virtual bool pump(int steps) = 0;
    virtual VideoStats getStats() const = 0;
    virtual double getBufferedSeconds() const = 0;     // downloaded/decodable ahead (progressive)
    virtual bool isFrameExact() const = 0;             // false on Web
    virtual bool rendersToTexture() const { return false; }           // Web: true
    virtual bool uploadTo(Framebuffer& target) { return false; }      // Web: texSubImage2D the current frame
    virtual const char* backendName() const = 0;       // "webm", "ffmpeg", "mediacodec", "avfoundation", "mediafoundation", "browser"
};
```

`VideoStream` implements it unchanged in behaviour (`backendName` "webm", `isFrameExact` true). `VideoInfo` gains
`frameRate`, `rotation` (0/90/180/270 from the track matrix; applied by the converter), `container`.
`VideoFrame` unchanged (I420 8-bit planes).

### 6.3 PlatformVideoSource (shared logic)

`PlatformVideoSource` runs one worker thread (as `VideoStream`) and delegates to a backend:

```cpp
class PlatformDecoderBackend {      // worker thread only
public:
    virtual ~PlatformDecoderBackend() = default;
    virtual bool open(MediaByteSource& source, bool wantAudio, VideoInfo& info, std::string& error) = 0;
    virtual bool seek(double seconds) = 0;                 // to the keyframe at or before (the source decodes forward)
    virtual DecodeStep decodeNext(VideoFrame& frame, std::vector<float>& pcm, int64_t& pcmStartFrame) = 0; // one video frame and/or an audio chunk; END, ERROR
    virtual void close() = 0;
};
```

- `MediaByteSource` wraps `FileData::newFile(path, true)` (assets, resource pack, Android `AAsset`) or a
  `GrowingFileReader`; backends that need an OS path or fd (Android API < 28, Apple) get one from it
  (`openFd()` via `AAsset_openFileDescriptor` for uncompressed APK assets — `.mp4/.m4v/.mov/.webm` are added to
  the Gradle `noCompress` list by the Exporter; `osPath()` for bundle/user files).
- Frame index: platform decoders give timestamps, not a full index. The source keeps the index of frames
  decoded so far plus `frameRate` from the probe; `frameIndexFor(t) = floor(t x frameRate + 1e-6)` until the
  frame is known; `frameCount = round(duration x frameRate)` (Unity reports the same estimate).
- Exact seeks: `seek(keyframe <= t)` then decode and discard to the last frame with pts <= t (as phase 6 §3.6.4).
  Loops: at end, `seek(0)` and continue with iteration+1 (gapless as far as the decoder allows: the next
  iteration's first frames are decoded ahead while the last ones play).
- Audio: PCM float, resampled by the backend to 48 kHz stereo (FFmpeg `swr`, MediaCodec output converted from
  PCM16 at its rate with a linear resampler in `PlatformVideoSource`, AVFoundation asks for 48 kHz float, MF asks
  for float at the stream's rate then resampled), written to the `VideoAudioRing` with its media time, so phase
  6's sync controller works unchanged.
- Hardware decoder limits: a backend open failure with "no decoder instance" retries once after 100 ms, then
  fails with `MEDIA_ERR_DECODE` "too many videos playing at once" (Android devices cap instances).

### 6.4 VideoPlayerComponent (engine/core/component/VideoPlayerComponent.h)

| Field | Type | Default | Unity |
|---|---|---|---|
| `enabled` | bool | true | |
| `source` | `VideoPlayerSource {Clip, Url}` | Clip | `source` |
| `clip` | string (asset path) | "" | `clip` |
| `url` | string (`data://...`, absolute, `http(s)://`, asset path) | "" | `url` |
| `playOnAwake` | bool | true | |
| `waitForFirstFrame` | bool | true | |
| `isLooping` | bool | false | |
| `skipOnDrop` | bool | true | |
| `playbackSpeed` | float 0.0625..16 | 1 | |
| `timeUpdateMode` | `VideoTimeUpdateMode {GameTime, UnscaledGameTime}` | GameTime | (DSPTime not offered) |
| `renderMode` | `VideoRenderMode {CameraFarPlane, CameraNearPlane, RenderTexture, MaterialOverride, APIOnly}` | RenderTexture | |
| `targetCamera` | Entity | NULL (main camera) | |
| `targetCameraAlpha` | float 0..1 | 1 | |
| `aspectRatio` | `VideoAspectRatio {NoScaling, FitVertically, FitHorizontally, FitInside, FitOutside, Stretch}` | FitInside | |
| `targetMaterialRenderer` | Entity | NULL (self) | |
| `targetMaterialProperty` | string | "baseColorTexture" (or a shader `@texture` uniform) | |
| `targetSubmesh` | int | -1 (all) | |
| `renderTextureWidth`, `renderTextureHeight` | uint | 0, 0 (the video's size) | (Unity: the RenderTexture's) |
| `audioOutputMode` | `VideoAudioOutputMode {None, AudioSource, Direct}` | Direct | |
| `directVolume` | float 0..1 | 1 | `SetDirectAudioVolume(0)` |
| `directMute` | bool | false | |
| `targetAudioSource` | Entity | NULL (self) | |

Runtime (not serialized): `IVideoSource` pointer, `VideoPlayback` clock, `VideoTextureConverter`, audio voice,
`prepared`, `playing`, `prepareProgress`, error, the saved material texture for MaterialOverride, event
subscriptions (`FunctionSubscribe` like `AnimationPlayer`).

`VideoPlayerSystem` (a SubSystem; update in the frame's update phase, events delivered there on the main thread):
`playOnAwake` -> prepare + play at scene start; each update: advance the clock by scaled or unscaled delta x
`playbackSpeed`, acquire the frame (`wait = !skipOnDrop`), hand it to the converter; fire events; apply
MaterialOverride. The render system runs pending conversions before the frame's first pass
(`RenderSystem::processVideoConversions`, outside any pass, like `HtmlUIContextRender`), and draws
CameraFarPlane (a fullscreen quad after the clear, before the opaque queue, depth test off, `aspectRatio` fit,
alpha `targetCameraAlpha`) / CameraNearPlane (after the transparent queue, before the HTML/native UI) for the
target camera only.

### 6.5 Lua API (`GameObjectClassesLua.cpp`, new `VideoPlayer` deriving EntityHandle; `gameObject:getVideoPlayer()`)

Properties (rw unless noted): the fields of §6.4 with the same names (enums `VideoPlayerSource`,
`VideoTimeUpdateMode`, `VideoRenderMode`, `VideoAspectRatio`, `VideoAudioOutputMode`; entity fields take a
GameObject or nil), `time` (seconds; writing seeks; `canSetTime` ro), `frame` (int; writing seeks to it),
`frameCount` (ro), `frameRate` (ro), `length` (ro, seconds), `width`/`height` (ro), `isPrepared` (ro),
`isPlaying` (ro), `isPaused` (ro), `prepareProgress` (ro 0..1: download then open), `texture` (ro, a `Texture` on
the render target; nil before prepared), `backend` (ro string), `videoCodec`, `audioCodec` (ro).
Methods: `prepare()`, `play()` (prepares first when needed: plays when prepared), `pause()`, `stop()` (to time 0,
releases the decoder; the texture keeps its last frame until the next prepare), `stepForward()` (paused: next
frame), `getFramebuffer()` (for `image:setTexture(fb)`), `VideoPlayer.canPlay(pathOrUrl)` -> "probably" |
"maybe" | "" (static, by probe for files and by extension/MIME for URLs), `VideoPlayer.clearCache([url])`.
Events (function properties called with the player): `onPrepareCompleted`, `onStarted`, `onLoopPointReached`
(each loop wrap and at the end of a non-looping play), `onErrorReceived(player, message)`, `onSeekCompleted`,
`onFrameReady(player, frameIndex)` (only when set: costs a call per frame). The game's flow:

```lua
local vp = self.gameObject:getVideoPlayer()
vp.url = System.getUserDataPath() .. "/ads/ad1.mp4"
vp.onPrepareCompleted = function(p) adImage:setTexture(p:getFramebuffer()); p:play() end
vp.onLoopPointReached = function(p) closeAd() end
vp.onErrorReceived = function(p, msg) Log.error(msg); closeAd() end
vp:prepare()
```

HTML UI: `<video>` and its `UIElement` API (`HtmlUIMediaLua.cpp`) are unchanged; new values work
(`src="ads/ad.mp4"`, `src="https://..."`), `canPlayType("video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\"")` answers per
platform, `element.videoBackend` (ro, new) reports the backend.

### 6.6 Per-platform implementation

**Linux (FFmpegBackend)**: dlopen `libavformat.so.N`, `libavcodec.so.N`, `libavutil.so.M`, `libswresample.so.K`
for the newest matching set among FFmpeg 4.x-7.x (avcodec 58-61; the set is tried newest first and must be
consistent: avutil 56-59, swresample 3-5), compiled against the vendored headers of that major (one translation
unit per major: `FFmpegBackend58.cpp` ... `61.cpp`, sharing a template `FFmpegBackendImpl.h`). Symbols used:
`avformat_alloc_context, avio_alloc_context, avformat_open_input, avformat_find_stream_info, av_find_best_stream,
avcodec_find_decoder, avcodec_alloc_context3, avcodec_parameters_to_context, avcodec_open2, av_read_frame,
avcodec_send_packet, avcodec_receive_frame, av_seek_frame, avcodec_flush_buffers, av_packet_alloc/free/unref,
av_frame_alloc/free/unref, swr_alloc_set_opts(2), swr_init, swr_convert, avformat_close_input, avcodec_free_context,
av_strerror, av_log_set_level`. I/O through a custom `AVIOContext` over `MediaByteSource` (assets in packs and
growing files work; `http` is never opened by FFmpeg itself). Pixel formats: YUV420P/YUVJ420P/NV12 direct, others
through `libswscale` if present, else `MEDIA_ERR_SRC_NOT_SUPPORTED` naming the format. `thread_count = 0` (auto,
<= 4). Missing libraries: `MEDIA_ERR_SRC_NOT_SUPPORTED` "MP4 playback on Linux uses the system's FFmpeg
libraries: install libavcodec, libavformat and libswresample (Debian/Ubuntu: libavcodec60 libavformat60
libswresample4 or newer; Fedora: ffmpeg-libs from RPM Fusion: Fedora's ffmpeg-free has no H.264), or convert the
video to WebM AV1". The capability table asks `avcodec_find_decoder` per codec id.

**Android (MediaCodecBackend)**: link `mediandk` (API 21). Extractor: `AMediaExtractor_setDataSourceFd` (fd from
`AAsset_openFileDescriptor64` or `open()` of a user file) or, API 28+, `AMediaExtractor_setDataSourceCustom`
with an `AMediaDataSource` over `MediaByteSource` (packs, growing files); runtime check
`android_get_device_api_level()`, functions resolved with `dlsym` from `libmediandk.so` so the minSdk 21 build links.
Video codec configured with `COLOR_FormatYUV420Flexible` (0x7F420888) and no surface (ByteBuffer output); the
output format's `color-format` (19 planar / 21 semi-planar), `stride`, `slice-height`, `crop-*` keys give the
plane layout; vendor tiled formats (e.g. 0x7FA30C03) -> retry with the software decoder
`AMediaCodec_createCodecByName("c2.android.avc.decoder" / "OMX.google.h264.decoder")`. Audio: a second
`AMediaCodec` for AAC (PCM16 out). Input/output buffers dequeued with 0 timeouts in the worker loop; EOS handled;
`AMediaCodec_flush` on seeks. App pause: playback paused by the player; codecs kept; `onTrimMemory`/surface loss
does not affect ByteBuffer mode. Capability: `AMediaCodec_createDecoderByType` probing per MIME at first use,
cached.

**iOS/macOS (AVFoundationBackend, Objective-C++, ARC)**: `AVURLAsset` from a file URL (bundle or user file; pack
assets are extracted once to the cache dir); `loadValuesAsynchronouslyForKeys:@[@"tracks", @"duration"]` waited on
the worker; `AVAssetReader` with `AVAssetReaderTrackOutput` video settings
`kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` (or FullRange from the track's
format description), audio `AVFormatIDKey: kAudioFormatLinearPCM, 32-bit float, interleaved, 48000 Hz, 2 ch`;
`copyNextSampleBuffer` pulls; planes copied from `CVPixelBufferLockBaseAddress` (read-only); seeks recreate the
reader with `timeRange` starting at the target (AVFoundation decodes from the prior keyframe internally and returns
from the target). Frameworks AVFoundation, CoreMedia, CoreVideo added by `tools/xcode_sync_engine.py`. Colour:
`CVImageBufferGetAttachment(kCVImageBufferYCbCrMatrixKey)` -> `VideoColor`. This code cannot be built on the
Linux host: it is written against the documented APIs, reviewed line by line, and compiled by the macOS CI job
when available; the spec's fixture runs there.

**Windows (MediaFoundationBackend)**: `MFStartup` once (refcounted), `MFCreateSourceReaderFromByteStream` with a
custom `IMFByteStream` over `MediaByteSource` (sync methods implemented; async ones via `MFPutWorkItem`),
attributes `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` = TRUE (to NV12), `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`
= TRUE; video media type NV12, audio `MFAudioFormat_Float`; `ReadSample` synchronous on the worker;
`SetCurrentPosition` for seeks; stride from `MF_MT_DEFAULT_STRIDE`, frame size and aperture. Link `mfplat mfreadwrite
mfuuid ole32`. Errors map `MF_E_TOPO_CODEC_NOT_FOUND`/`MF_E_INVALIDMEDIATYPE` to "this Windows has no decoder for
<codec>: install <HEVC/VP9 Video Extensions> or the Media Feature Pack (N editions)".

**Web (WebVideoSource)**: `jasper_video.js` creates a hidden `<video playsinline muted?>` per source (`src` = a
Blob URL of the preloaded file's bytes for assets/user files, or the URL itself), `crossOrigin="anonymous"` for
cross-origin URLs, `preload="auto"`; events forwarded to C++ (`loadedmetadata`, `canplay`, `waiting`, `playing`,
`ended`, `error`, `seeked`) and queued to the main thread. `uploadTo(target)`: when
`requestVideoFrameCallback` reported a new frame (or `currentTime` changed where it is unsupported),
`gl.texSubImage2D` the element into the target's GL texture (`sg_gl_query_image_info`), with
`UNPACK_FLIP_Y` matching the engine's texture orientation. Audio: `createMediaElementSource` -> `GainNode`
(volume x engine global volume, mute) -> destination; until the first user gesture the element is muted (the
engine's existing audio unlock hook unmutes it); `onErrorReceived` reports a CORS failure ("the server must send
Access-Control-Allow-Origin") and the autoplay restriction. Frame index/count from `currentTime x frameRate`
(frame rate from the probe of the preloaded file; for URLs from `requestVideoFrameCallback` metadata, else 30).

### 6.7 Colour conversion and output

- `VideoTextureConverter` (engine side): three DYNAMIC R8 textures (one update each per new frame), one RGBA8
  `Framebuffer` (size: `renderTextureWidth/Height` or the display size, rotation applied), conversion with the
  `HTMLUI_POST` `Yuv` variant (phase 6 §3.6.5: matrix/range from `VideoColor`, BT.709 default, BT.601 under 720
  lines) drawn as a fullscreen triangle into the framebuffer; letterboxing for a fixed-size texture follows
  `aspectRatio` (the bars are transparent black). Web: `uploadTo` instead (and a blit when the requested size
  differs).
- The framebuffer's `Texture` is what `player.texture`, `getFramebuffer()` and `video|<entity>` links resolve to
  (`VideoTextureLink` in `editor/util/`, modelled on `FluidTextureLink`: `CameraTextureLink::parseLink/
  linkFramebuffer/isLinkSource/resolve` learn the `video|` kind; exported builds bind it in the Factory code as
  camera links are).

### 6.8 Serialization, exports and assets

- `Stream::encode/decodeVideoPlayerComponent`, `Factory::createVideoPlayerComponent`, `Catalog` properties
  (`ComponentType::VideoPlayerComponent` appended at the end of the enum, `editor/Catalog.h:83`), `EntityRegistry`
  registration, `EntityCloner` (runtime not copied; entity refs remapped), prefab overrides via the catalog.
- Assets: `.mp4 .m4v .mov` become **playable assets** (no longer `isRejectedExtension`); the phase-6 importer no
  longer converts them automatically. The Project window's context menu keeps "Convert to WebM (AV1)..." (FFmpeg
  CLI as before). Exports copy video assets as ordinary assets (the scene/asset collector follows `clip` paths and
  `<video src>`); Android Gradle `aaptOptions.noCompress` gets `mp4 m4v mov webm` (`editor/Exporter.cpp` Android
  section) so `AAsset_openFileDescriptor` works.
- Android export: the INTERNET permission is added when any scene has a VideoPlayer with an `http(s)` url (the
  `http` feature's rule covers scripted URLs).
- Web export: `jasper_video.js` added to the link flags (`--js-library`), like the engine's other JS libraries.
- Windows/macOS/iOS exports: link libraries/frameworks above (engine CMake and Xcode project).
- Linux export: nothing bundled; the README of a Linux export mentions the optional FFmpeg runtime.

### 6.9 Editor UX

- Add Component > Video > **Video Player**; GameObject > Video > **Video Player** (creates one with
  RenderTexture mode) and **Video Player (Camera Background)** (CameraFarPlane, main camera).
- Inspector (Unity order): Source (Clip/URL), Clip (asset picker filtered to video) or URL (text, with "Browse
  user data..." helper and a validity hint), Play On Awake, Wait For First Frame, Loop, Skip On Drop, Playback
  Speed, Time Update Mode, Render Mode and its fields (Target Camera + Alpha + Aspect Ratio; Renderer + Material
  Property dropdown of the renderer's texture slots; Render Texture size with "Native"), Audio Output Mode and
  its fields. A **Codec support** row: the probe's codecs and a per-platform check row (Linux*, Windows, macOS,
  iOS, Android, Web; `*` = needs FFmpeg), with a warning when a target platform of the project lacks it and the
  "Convert to WebM (AV1)" fix. In Play: time/frame scrubber, play/pause/stop, prepared/buffered state, backend.
- Video asset Inspector (`.mp4/.webm`): probe info, the platform matrix, fast start yes/no ("Optimize for
  streaming" button rewrites the moov to the front with a pure-C++ box rewriter, no FFmpeg needed), and a preview
  player using the same source.
- Image/material texture pickers list "Video Player: <name>" link entries (as they list cameras).
- `jasperengine-editor check`: `video-codec-unsupported-on-target` (warning per export platform),
  `video-not-faststart-url` (info).

### 6.10 MCP / agent surfaces

- `create_entity type=video_player` (+ `video_background`); `set_properties`/`get_entity` for every field; runtime
  `get_entity` in Play adds `video: {prepared, playing, time, frame, frameCount, length, backend, videoCodec,
  audioCodec, error, prepareProgress}`; `set_runtime_property` drives `time`/play state, or `eval_lua`.
- `get_asset_info` on a video returns the probe plus `playable_on: {linux, windows, macos, ios, android, web}`
  (`"yes" | "needs_ffmpeg" | "needs_extension" | "device_dependent" | "no"`) and `fast_start`.
- `inspect_ui` media state adds `backend`.
- No new tool. `docs/mcp/TOOLS.md` regenerated; budget within limits (enum additions only).
- Agent guide line: "Video: Video Player component (render texture `video|<id>`), MP4 via platform decoders,
  WebM AV1 everywhere; jasper://docs/video".

### 6.11 Docs

New `docs/mcp/guide/video.md` (formats and the per-platform matrix, Linux FFmpeg note, licensing note, URL cache
and progressive start, the component and recipes: UI ad, TV mesh, menu background, HTML `<video>`), update
`docs/html-ui/phase-6-video.md` (a "Superseded in part by 2026-10-31-video-mp4-design.md" note at §2 Out and
§3.1.5), `docs/mcp/guide/html-ui.md` video section, `docs/mcp/guide/lua-api.md` (VideoPlayer, enums),
`docs/unity-glossary.md` (VideoPlayer, VideoClip -> clip path, RenderTexture -> `video|<id>` link), `NOTICE.md`
(FFmpeg headers, LGPL, not linked), README.

## 7 Behaviour rules and edge cases

- Unsupported codec: `onErrorReceived` with the codec and the platform hint; `isPrepared` stays false. The
  HTML `<video>` tries the next `<source>` (phase 6 source selection) as browsers do.
- URL download failure: error after the `http` feature's retries; a partially downloaded file is kept for resume.
  Offline with a complete cached copy: plays from the cache (revalidation failure is not an error).
- Changing `url`/`clip` while playing: stops, releases, `isPrepared` false; `playOnAwake` does not re-trigger.
- `play()` before prepared: prepares, `onPrepareCompleted`, then `onStarted` when the first frame is shown
  (`waitForFirstFrame` true: the clock starts only then).
- `timeScale = 0` with GameTime: paused picture and audio; Unscaled: unaffected. App pause: everything pauses,
  resumes in sync (phase 6 §3.7).
- Scene unload / entity destroy: source closed, voice stopped, worker joined, cache download continues only if
  another player wants the same URL (else cancelled with the owner, shared convention).
- Two players on the same file: two sources (independent clocks). Same URL: one download shared.
- Rotation metadata (phone videos): applied; `width/height` report the displayed size.
- Variable frame rate MP4: frames are chosen by pts; `frameCount` is an estimate (documented as in Unity).
- MaterialOverride target without that texture slot: error once, nothing overridden.
- Odd sizes (e.g. 1918x1078): chroma sizes rounded up (existing `chromaWidth`).
- 10-bit HEVC: platform outputs P010 on some backends -> converted to 8-bit (shift with rounding) on the worker.

## 8 Performance (mobile budgets)

- Decode is hardware-accelerated on Android/Apple/Windows; the CPU copy of planes plus NV12 de-interleave costs
  ~1.5 ms per 1080p frame on a mid phone on the worker thread (not the main thread); main thread: 3 texture updates
  and one conversion pass per new frame (~0.3 ms GPU at 1080p on a mid phone).
- Budget: one 1080p30 or two 720p30 videos with audio on a 2019 mid-range Android at < 10 % main-thread frame time
  added; measured with `JASPERENGINE_FRAME_STATS=1` on a fixture. The Inspector warns above 2 concurrent players on
  mobile targets and above 1080p.
- Memory per 1080p player: queue 4 x 3 MB, planes 3 MB GPU, target 8 MB GPU, decoder pools (platform).

## 9 Tests and acceptance

Clips: `engine/tests/media/clips/` committed: `h264_aac_160x90.mp4` (2 s, 30 fps, Baseline, faststart, frame number
encoded as coloured quadrants + 1 kHz tone), `h264_aac_160x90_moov_end.mp4`, `hevc_aac_160x90.mp4`,
`vp9_opus_160x90.webm`, each < 60 KB, with `tools/media/make_test_clips.sh` (any FFmpeg build with libopenh264 or
x264 and an AAC encoder; the clips are data). **Note: this host has no FFmpeg CLI or libraries installed**
(`ldconfig -p` shows none): the implementer generates the clips with a locally built FFmpeg (LGPL build +
libopenh264, in `/tmp`, not committed) and installs or builds the FFmpeg shared libraries to run the Linux backend
tests (`JASPERENGINE_FFMPEG_DIR` env var points the loader at a non-system directory for tests).

Unit (`engine/tests/media/`, target `jasperengine-media-tests`, and `editor/tests`):
- `Mp4ProbeTest`: codecs, size, duration, frame rate, fast start, rotation, truncated/garbage files.
- `MediaCacheTest`: cache path, revalidation (ETag 304), resume with Range, LRU eviction, cancel with owner (with a
  local test HTTP server from the `http` feature's tests).
- `GrowingFileReaderTest`: blocking reads, wake on append, failure propagation, abort.
- `FFmpegBackendTest` (skipped with a message when the libraries are absent): decode every frame of the clip,
  frame N's quadrant colours identify N; exact seek to frame 37; loop seamless (no frame skipped); audio PCM
  length = duration x 48 kHz ± 1 %; `moov_end` file plays after full download and refuses progressive start.
- `VideoSourceFactoryTest`: WebM AV1 -> "webm" source on every platform; MP4 -> platform source; missing backend ->
  the documented error text.
- `VideoPlayerSystemTest` (fake source): event order prepare -> started -> loopPointReached; GameTime vs Unscaled;
  timeScale 0; stop/prepare cycle; MaterialOverride restore; destroy mid-prepare cancels.
- Stream/Factory round trip of the component; `video|<id>` link resolve/remap on duplicate and prefab.
- `test_stubs.py` for the new Lua classes.

End to end (MCP, `tools/mcp/tests/test_video_player.py`, Linux with FFmpeg present else skip):
- RenderTexture -> UI Image: Play, wait `prepared`, screenshot: the image's quadrant colours match the frame at the
  reported `time` (± 1 frame); after `stop()` the last frame stays; `onLoopPointReached` fires once for a
  non-looping clip.
- CameraFarPlane: background shows the video behind a cube.
- HTML `<video src=mp4>`: `inspect_ui` shows backend ffmpeg and playing.
- URL: a local HTTP server serving the faststart clip: progressive start before the download completes (logged
  buffered < 100 %); the moov-at-end clip prepares only after completion.

Builds (must compile): Linux editor/engine; Android Gradle (arm64, minSdk 21; MediaCodec backend + dlsym path);
Web (emsdk; `jasper_video.js`); Windows MSVC/MinGW if the host toolchain allows, else a compile check through
the existing cross build; Apple: not buildable here (stated), reviewed; the macOS/iOS fixture runs on Apple CI.
Android device/emulator run of the fixture when an emulator is available (`/opt/android-sdk`), else compile only.

Acceptance:
1. The game's ad flow (download mp4 to the user data dir, prepare, play into a texture on a UI Image, end event)
   works in editor Play on Linux (with FFmpeg) and in the Android export (H.264/AAC).
2. MP4 H.264/AAC plays on every platform's backend that this host can run; the others compile (Android, Web) or are
   reviewed (Apple, Windows if not compilable); HEVC plays where the platform reports a decoder.
3. WebM AV1 behaviour and goldens of phase 6 are unchanged (`jasperengine-htmlui-tests` video suite passes).
4. Missing-decoder cases give the documented messages; the editor shows the per-platform matrix.
5. All component fields: Inspector + undo, serialization, export, MCP, Lua + stubs, docs.

## 10 Integration points

- `engine/core/htmlui/media/*` (moved), `UIVideoElement.{h,cpp}` (uses `IVideoSource` via the factory; `stream`
  member at `UIVideoElement.h:202` becomes `std::unique_ptr<IVideoSource>`), `VideoFormats.cpp` (`canPlayType`,
  `isRejectedExtension`, probe dispatch to `Mp4Probe`), compat transforms/diagnostics for `video/mp4` and URLs
  (`engine/core/htmlui/compat/...`), `HtmlUIMediaLua.cpp` (`videoBackend`).
- `engine/core/subsystem/RenderSystem.cpp` (conversion queue before passes; far/near plane draws in the camera pass),
  `engine/core/Scene` system registration, `engine/core/registry/EntityRegistry.cpp`.
- `engine/CMakeLists.txt` (new sources per platform, `mediandk`, MF libs, `dl` on Linux, JS library on Web),
  Xcode project via `tools/xcode_sync_engine.py`.
- `editor/util/CameraTextureLink.{h,cpp}` + new `VideoTextureLink`, `editor/Catalog.{h,cpp}`, `editor/Stream.cpp`,
  `editor/Factory.cpp`, `editor/Exporter.cpp` (noCompress, INTERNET permission, assets), `editor/window/InspectorWindow.cpp`,
  `editor/window/HierarchyWindow.cpp:493` (menu), `editor/mcp/tools/EntityTools.cpp`, `AssetTools.cpp` (`get_asset_info`).

## 11 Cross-feature dependencies

- **`http` (#1)**: required for URL sources. `MediaCache` uses its C++ API as specified there
  (`engine/core/net/Http.h`: `Http::send(HttpRequestDesc{responseType = File, filePath, reportProgress = true,
  headers}, onComplete, onProgress, owner)`, main-thread callbacks, `HttpRequestHandle::cancel`). Revalidation sends
  `If-None-Match`/`If-Modified-Since` through `HttpRequestDesc.headers` (a 304 keeps the cached file). Two additions
  are requested from `http` (small, compatible): `HttpRequestDesc::resume` (append to an existing `<path>.part` and
  send `Range: bytes=<size>-`, accept 206) and `HttpRequestDesc::keepPartial` (do not delete the `.part` on failure);
  until they exist MediaCache re-downloads from the start. Progressive start reads the `.part` file while it grows,
  bounded by the bytes reported to `onProgress`. Without the `http` feature URL sources fail with "URL video needs
  the HTTP client (Http.isAvailable() is false)". Local files and the game's own downloaded file
  (`Http.download(url, "data://ads/x.mp4", ...)` in the game's script, the http spec's story) do not depend on it.
- **`audio-mixer` (#6)**: `audioOutputMode = AudioSource` follows the AudioSource's bus once buses exist.
- **`platform-services` (#3)**: app focus/pause events (pause video on focus loss is the player's rule already).
- **`coroutines` (#16)**: Unity's `WaitUntil(() => isPrepared)` maps to `waitUntil(function() return vp.isPrepared end)`.
- **`persistence` (#20)**: a persistent VideoPlayer keeps playing across scene loads (its target texture link stays
  valid; camera targets re-resolve to the new main camera).

## Implementation notes

What the implementation does differently from the text above, and why.

**Files and structure**
- Platform directories are the engine's existing ones: `engine/platform/win/MediaFoundationBackend.cpp`,
  `engine/platform/emscripten/WebVideoSource.cpp`, `engine/platform/apple/AVFoundationBackend.mm` (no `.h`:
  every backend is reached through the `platformvideo::` hooks of `media/PlatformVideoBackend.h`, defined once per
  platform: backendName, isAvailable, createBackend, supports, readsContainer, supportsGrowingSources,
  unsupportedHint; on the Web also createBrowserSource and browserSupports).
- The Web's JavaScript is `EM_JS` inside `WebVideoSource.cpp`, not a separate `jasper_video.js` passed with
  `--js-library`: nothing to add to the link flags or the Web export.
- One backend instance per stream (one for the video track, one for the audio track, both reading the same
  `MediaByteSource`), so the backend interface is `open(bytes, stream, found, error)` / `seek` / `decodeNext`
  of one stream instead of §6.3's combined video+audio `decodeNext`. The two decode independently on the
  source's one worker thread; seeking and looping each keep their own position.
- FFmpeg majors supported: avcodec 58 to 62 (FFmpeg 4.x to 8.x; 8.0 was released after the spec was written),
  one translation unit each (`FFmpegBackend58.cpp` ... `62.cpp`) compiled with `-isystem` on its vendored headers
  (`engine/libs/ffmpeg-headers/<major>/`, the closure of the headers used, about 4 MB).
  `JASPERENGINE_FFMPEG_DIR` points the loader at a directory for tests.
- The unit tests are in `editor/tests` (`VideoMp4Test.cpp`: probe, growing files, FFmpeg backend, factory,
  media cache; `VideoPlayerTest.cpp`: the system with a fake source, Catalog, Stream, links, cloning), run by
  `jasperengine-editor-tests`, not in a new `jasperengine-media-tests` target: the editor test binary already
  has the test HTTP server, the engine and the editor's serialization. They need FFmpeg and are excluded on
  Windows/Apple builds of the tests (the backends there are not FFmpeg).
- The clips are 160x90 as specified; `h264_rotated_160x90.mp4` (a 90 degree display matrix) was added for
  the rotation tests. They are each under 16 KB.

**Behaviour**
- Rotation is applied to the planes on the worker thread (`VideoFramePlanes::rotate`), not by the converter:
  every consumer (UI `<video>`, the component, the CPU preview) gets upright frames and `width/height`.
- Material Override sets the renderer's texture slot to the player's texture while it has a picture and
  restores the slot's own texture when the player stops, is disabled or removed (the saved texture is kept by
  the system, not serialized).
- The **Web follows the engine's clock**, contrary to decision 5: the browser element plays at the
  engine's playback rate and is paused/seeked when the engine's clock is (`IVideoSource::setClock`), so
  `timeUpdateMode`, `timeScale`, `pause()` and the UI clock behave as on the other platforms; frames are
  whatever the element shows (`isFrameExact` false). `renderTextureWidth/Height` are not applied on the Web
  (the element's frames are uploaded at the video's size). Volume: a WebAudio GainNode where the context allows
  it, else the element's `volume`; no 3D panning there.
- `audioOutputMode = AudioSource` takes the AudioSource's volume and 3D position/attenuation; there is no mixer
  bus yet (`audio-mixer` #6 has not landed on this branch's base; the bus follows when it does).
- Importing (import_files, the Project window): an MP4/MOV some platform plays is copied as it is; a WebM of
  another codec than AV1 (VP9) is still converted to AV1 + Opus as in phase 6, since Apple decodes no VP9.
- A VP9 + Opus WebM on a platform without a VP9 decoder plays its sound only (as before this feature, through
  the software path), with the video error reported; the spec did not cover this mixed case.
- `get_entity` / `get_runtime_entity` add `video` once the player opened something (prepared, preparing or
  failed): an idle player in edit mode has no block. It also has `preparing`, `paused`, `frameRate`, `width`,
  `height` and `buffered` beyond §6.10's list.
- MCP aliases: `create_entity type video` / `videoplayer` -> `video_player`, `video_camera_background` ->
  `video_background`; component alias `video` -> `videoplayer`.
- The guide is `docs/mcp/guide/video.md`, served as `jasper://docs/video`.
- The editor reuses the edit-time Scene for Play, so `VideoPlayerSystem` is paused with the other systems in
  edit mode (nothing plays, `playOnAwake` waits) and `resetPlayers()` at Play stop closes every source and
  arms `playOnAwake` for the next Play.
- `prepare()` / `play()` right after a new `clip`/`url` (`vp.url = x; vp:prepare()`, the ad flow) open the new
  source at once; a new value alone still stops the player without `playOnAwake` starting it again.
- Progressive start probes the growing download first at 32 bytes, then each time a quarter more (at least
  4 KiB) arrived, until the `moov` box is complete or known to follow the media data.
- The agent guide's line is "Video Player (texture video|<id>), MP4, WebM: jasper://docs/video." (shorter
  than §6.10's: the guide is at its 16 KB limit after the merges).

**Editor, exports, tools**
- Apple frameworks (AVFoundation, CoreMedia, CoreVideo): in the Xcode project the `.mm`'s `#import`s of the
  framework headers are module imports (`CLANG_ENABLE_MODULES = YES`), which autolink them;
  `tools/xcode_sync_engine.py` adds the source file only. CMake Apple builds link them explicitly.
- Android's `noCompress 'mp4', 'm4v', 'mov', 'webm'` is in the Gradle template
  (`engine/workspaces/androidstudio/app/build.gradle`, `androidResources`), which every Android export copies,
  rather than written by the Exporter. The INTERNET permission comes from `Exporter::videoUrlScene()` (a scene
  or prefab with a Video Player whose `url` is http(s)).
- `jasperengine-editor check --videos` (in the default set) reports `video-codec-unsupported-on-target` for
  each of the six platforms where the clip plays `no` (the project has no list of target platforms to narrow
  it), `video-not-faststart-url` (info) and `video-missing` (a clip or local url that is not in the project).
- The Inspector's preview of a video asset is CPU-converted (`VideoColorConvert::toRgba` into an ImGui
  texture) rather than going through the engine's `VideoTextureConverter` (the editor's ImGui renderer is not
  a scene render target).
- Linux exports have no README in this engine (the exporter writes none), so the FFmpeg runtime note is in the
  guide (`jasper://docs/video`, Linux) and in the error message a player gets without FFmpeg.
- The HTML UI test binary (`jasperengine-htmlui-tests`) runs with the platform decoders off
  (`VideoSourceFactory::setPlatformDecodersEnabledForTesting(false)`), so its WebM goldens and its "MP4 is not
  supported" expectations keep testing the software path; MP4 in `<video>` is tested by
  `tools/mcp/tests/test_video_player.py` and `VideoMp4Test`. `test_html_ui_video.py` uses an `.avi` for its
  unsupported-source case (an MP4 now plays).
- Windows: the `IMFByteStream` exposes its `IMFAttributes` (content type, for the source resolver) through a
  separate attributes object returned by `QueryInterface`, not by deriving from both interfaces.
- The factory's platform enum is `VideoSourceFactory::Platform { Linux, Windows, MacOS, IOS, Android, Web }`
  (CamelCase: the Android toolchain defines `ANDROID`).

**Not verified here**
- The Apple (AVFoundation) and Windows (Media Foundation) backends cannot be built on this host: they are
  written against the documented APIs and re-read, not compiled.
- No Android device or emulator run (the Android backend is compiled by the Gradle build only); no mobile
  performance measurement (§8's budget is unmeasured).
