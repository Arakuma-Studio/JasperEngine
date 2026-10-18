# Phase 6: HTML `<video>`

Binding design: `docs/html-ui/DESIGN.md`. Agent rules: `docs/html-ui/BRIEF.md`. This spec
details phase 6 (one work package, `p6-video`); where it extends DESIGN.md it says so (3.17).

## 1 Outcome

A game UI written in standard HTML can play video the way a web page does:

```html
<video id="bg" src="ui/menu-loop.webm" autoplay loop muted playsinline
       style="position: fixed; inset: 0; width: 100%; height: 100%; object-fit: cover"></video>

<video id="trailer" controls preload="metadata" poster="ui/trailer-poster.png" width="640" height="360">
  <source src="ui/trailer.webm" type='video/webm; codecs="av01.0.05M.08, opus"'>
</video>
```

```lua
local trailer = document:getElementById("trailer")
trailer:addEventListener("ended", function() trailer.currentTime = 0 end)
trailer.volume = 0.8
trailer:play()
```

One file format plays the same on Linux, Windows, macOS, iOS, Android and Web: **WebM with AV1
video and Opus audio**, decoded in software by vendored BSD-licensed libraries (dav1d, libopus,
libwebm's parser). Frames are converted from YUV to RGB on the GPU. Playback follows the UI clock
(`HtmlUIClock`), so pinned time in tests, MCP `auto_pause` and `fixed_fps` behave as they do for the
rest of the UI. Seeks are frame-exact, and the video's audio plays through the engine's audio
system (SoLoud) in sync with the picture. `object-fit`, `object-position`, `border-radius`,
filters and phase-5 effects apply to video as to any replaced element. A default control bar,
drawn in HTML/CSS by the user-agent stylesheet, works with the mouse, the keyboard and gamepads.
The Lua element API mirrors `HTMLMediaElement`, with the same events in the same order as Chromium.

The editor converts other formats (MP4/H.264, MOV, MKV, VP9 WebM, ...) to the shipped format when
a user-installed FFmpeg is available (`import_files`, the Project window). Exports ship the
`.webm` files as ordinary assets. Every unsupported format, codec and attribute is diagnosed.

## 2 Scope

### In
- The `<video>` element (`UIVideoElement`, an RmlUi element registered as `video`) with `src`,
  `<source>` children (`src`, `type`), `autoplay`, `loop`, `muted`, `playsinline`, `poster`,
  `controls`, `preload`, `width`/`height`. Also the replaced-element rendering of phases 2-5
  (`object-fit`/`object-position`, rounded content, `image-color`, `opacity`, filters, effects).
- Software decoding on all six platforms: WebM demuxing (libwebm `mkvparser`), AV1 (dav1d 1.5.4,
  8 and 10 bit, 4:2:0, 4:2:2, 4:4:4, monochrome), Opus (libopus 1.6.1, mono/stereo; multichannel
  down-mixed). GPU YUV→RGB conversion (BT.601/BT.709/BT.2020 matrices, limited/full range) as an
  `HTMLUI_POST` variant.
- Playback on the UI clock: frame-exact seeking, decode-ahead on a worker thread, a frame-exact
  (blocking) mode for pinned or fixed-step time, and real-time frame dropping otherwise. Seamless
  loops, `playbackRate`, and the documented effect of game pause and `timeScale`.
- Audio through SoLoud with A/V sync (audio slaved to the media clock, drift measured and
  corrected), volume, muted, and app pause/resume.
- The `HTMLMediaElement` subset in Lua and C++: methods, properties, `MediaError`, ready and
  network states, and every event Chromium fires for local files.
- The default controls: a play/pause button, the time display, a seek bar and a mute button.
  They are non-DOM children styled by the UA sheet, with keyboard, gamepad and pointer input.
- Compat: the element passes through (no longer a placeholder), `<source>` and fallback content
  are handled, attributes are diagnosed, and the support tables, support matrix and guide are
  updated.
- Assets: a WebM probe (container, codecs, size, duration) shared by compat, runtime and editor;
  an importer that converts other formats with a user-installed FFmpeg (never linked, never
  shipped); hot reload; export; and a memory and decode budget.
- Tooling: `inspect_ui` media state and control rectangles, and MCP play-mode tests.
- Tests: decode and seek, events, Lua, A/V sync, leaks, controls, goldens on glcore and Vulkan,
  Chromium parity, MCP, export (Desktop player and web build), web and Android compiles. Also a
  committed test clip with its generator.
- Example `video_menu` (a main menu with a looping video background), docs, CONTRACTS, FORK and
  DESIGN deviations.

### Out (with reason)
- **Platform decoders for MP4 (H.264/HEVC/AAC)**: AVFoundation, MediaCodec, Media Foundation,
  and the browser's own `<video>` on Web. Section 3.1 has the comparison and reasons: the same
  file would not play everywhere, decoding would not be frame-exact or deterministic, audio would
  bypass SoLoud on Web, the patent licences would be the game author's problem, and three of the
  four backends cannot be built or run here. `video/mp4` sources are skipped like any type the
  engine cannot play, and they are diagnosed with the conversion as the fix. The decoder sits
  behind an interface (`VideoDecoder`, 3.6.1), so a platform backend can be added later without
  touching the element.
- **Other codecs**: VP8, VP9, Theora, MPEG-1/2, H.264 and HEVC are diagnosed and converted by the
  importer (3.12). Reasons are in 3.1.
- **dav1d assembly** (x86 AVX2/SSSE3 through NASM, ARM NEON): the measured speed-up is 3.3× on
  x86-64 (3.1.4). It is left out because it cannot be validated here: the build images have no
  NASM, and there is no ARM device or emulator to run dav1d's `checkasm` on. Adding it is a CMake
  switch plus the asm file lists (3.2.1). It is the first follow-up.
- **Alpha video** (VP9 alpha side data, AV1 has no WebM alpha mapping): no royalty-free alpha
  format is decodable by the chosen stack. Use Lottie or a sprite sheet for alpha-blended motion.
- **Streaming and network sources** (HLS, DASH, MSE, `http(s):` at run time): the HTML UI has no
  network at run time (DESIGN goal). Remote URLs are imported by the editor (3.12.2).
- **`<track>`/WebVTT captions, `requestVideoFrameCallback`, `captureStream`, Picture-in-Picture,
  fullscreen, `controlslist`, `disablepictureinpicture`, `disableremoteplayback`, `crossorigin`,
  remote playback**: no engine equivalent. Each is diagnosed (3.13). Show captions as text
  elements driven by `timeupdate`.
- **`<audio>`**: it stays a placeholder (`HTMLUI-HTML-0002`, "play the sound from Lua"). Game
  sounds belong to `AudioSource` components. The video pipeline would support it, but no
  requirement asks for it.
- **Video in CSS images** (`background-image: url(clip.webm)`, `mask-image`, `content`,
  `border-image`) and **video as an effect's texture parameter**: browsers do not play video in CSS
  images, and phase-5 effects have no texture parameters (phase 5 §3.3, "Media effects" in §2).
  It is diagnosed with the fix (a `<video>` behind the content). Effects that *process* a video
  (`filter`, and `backdrop-filter` over one) work unchanged (3.11).
- **Pitch-preserving rate changes**: audio at `playbackRate != 1` is resampled, so the pitch
  follows the rate (`preservesPitch` reads `false`). Time stretching is a DSP project of its own.

## 3 Detailed design

### 3.1 Decoding approach: research and decision

#### 3.1.1 Requirements
1. **Platforms**: Linux, Windows, macOS, iOS, Android and Web (Emscripten, also without pthreads).
2. **Licence**: compatible with the engine's MIT licence and with selling games. No GPL or LGPL in
   what ships. A patent-encumbered codec is acceptable only through a platform decoder that carries
   its own licence.
3. **Determinism**: frame-exact seeking, decoding stepped by the UI/game clock, and identical
   frames on every platform and backend, so goldens hold.
4. **Performance** for game UI: a menu background loop, cutscene-like panels, item previews and
   several small videos at once.
5. **Audio** through the engine's audio system with A/V sync.
6. **Browser parity**: the same file plays in Chromium 153 (the reference browser), so parity
   fixtures compare like with like.

#### 3.1.2 Candidates
| Candidate | Decoder (licence) | Container | Audio | Notes |
|---|---|---|---|---|
| AV1 | dav1d 1.5.4 (BSD-2), 37 k lines of C | WebM (libwebm `mkvparser`, BSD-3 + patent grant, 9 k lines) | Opus (libopus 1.6.1, BSD-3) | AOMedia royalty-free patent licence; decoding is bit-exact by specification; Chromium decodes it with dav1d |
| VP9 | libvpx 1.17 (BSD-3) | WebM | Opus/Vorbis | Google royalty-free grant; bit-exact; libvpx's build generates per-target `rtcd` headers with perl and configure scripts, and needs per-arch asm or a slow C path |
| VP8 | libvpx | WebM | Vorbis | superseded by VP9/AV1 |
| MPEG-1 | pl_mpeg (MIT, one 4.4 k-line header) | MPEG-PS | MP2 (built in) | patents expired; trivial to vendor; IDCT not bit-exact (IEEE 1180 tolerance); no browser plays it |
| Theora | libtheora + libogg (BSD-3) | Ogg | Vorbis | royalty-free; Chromium dropped Theora playback in 2024, so no parity |
| H.264/HEVC (MP4) | platform: AVFoundation, MediaCodec, Media Foundation, browser | MP4 | AAC | royalty pools (Via LA AVC; HEVC: Access Advance, Via LA, independents); on Linux only VA-API on some GPUs or nothing; Chromium's open-source build (the reference) has no H.264 |

#### 3.1.3 Compression (measured)
A hard case: a 10 s Mandelbrot zoom at 1280×720 and 30 fps (`ffmpeg -f lavfi mandelbrot`: fine
detail, smooth gradients, constant motion). Bitrate and Y-PSNR against the source were measured on
the test host (FFmpeg 6.1.1 encoders, frames compared one to one from raw decodes):

| Codec (encoder, settings) | kbps | Y-PSNR |
|---|---|---|
| AV1 (SVT-AV1 1.7 preset 8, CRF 55 / 45 / 35) | 756 / 2,224 / 5,547 | 33.09 / 36.39 / 40.97 dB |
| VP9 (libvpx, good, cpu-used 4, CRF 45 / 33 / 20) | 3,015 / 7,577 / 13,402 | 35.91 / 41.60 / 46.63 dB |
| H.264 (x264 medium, CRF 33 / 28 / 23) | 979 / 2,705 / 5,659 | 31.82 / 35.43 / 39.87 dB |
| MPEG-1 (FFmpeg, q 16 / 8 / 4) | 4,077 / 8,750 / 16,678 | 34.75 / 39.80 / 45.05 dB |
| Theora (libtheora q 6) | 12,859 | 40.61 dB |

Interpolated at 40 dB, the bitrates needed are AV1 ≈ 4.8 Mbit/s, H.264 ≈ 5.8, VP9 ≈ 6.3, MPEG-1
≈ 9 and Theora ≈ 11.5. At 36 dB they are AV1 ≈ 2.1, H.264 ≈ 3.0, VP9 ≈ 3.1 and MPEG-1 ≈ 5.0.
AV1 needs the fewest bytes for the same quality: about half of MPEG-1 and two fifths of Theora,
and about 20 % less than H.264 and VP9 at fast encoder presets. Slower AV1 presets widen the gap.
The importer's default (3.12.1) is SVT-AV1 preset 6 at CRF 32.

#### 3.1.4 Decode cost (measured)
Same clips, one core of the test host (AMD EPYC, 2.0 GHz VM, AVX2), 300 frames, milliseconds per
frame. "dav1d C" is the build this phase vendors: the engine's CMake, no asm, `max_frame_delay` 1.
The "asm" columns are FFmpeg's decoders with their SIMD, for reference.

| Resolution (AV1 kbps) | dav1d C, 1 thread | dav1d C, 4 threads | dav1d asm (libdav1d) | VP9 asm / C | H.264 asm | MPEG-1 pl_mpeg / FFmpeg asm |
|---|---|---|---|---|---|---|
| 256×256 (829) | 2.09 | 1.68 | — | — | — | 0.77 / — |
| 640×360 (1,807) | 5.63 | 3.72 | 1.89 | 1.76 / 4.61 | 1.37 | 2.18 / 0.50 |
| 1280×720 (5,547) | 19.4 | 11.7 | 5.88 | 5.85 / 15.7 | 5.05 | 7.29 / 1.76 |
| 1920×1080 (10,687) | 42.0 | 24.5 | 12.3 | 12.1 / 34.2 | 10.6 | 14.7 / 3.95 |

Opus decoding costs 1.8 ms per second of stereo 48 kHz audio (0.2 % of a core). This content is a
worst case at a high bitrate: typical menu loops at 1-2 Mbit/s decode about twice as fast. The C
build decodes 720p30 on one worker thread with headroom, 1080p30 with 2-4 threads, and a dozen
256×256 previews in under one core. dav1d's asm is 3.3× faster, which makes it the first follow-up
(2).

#### 3.1.5 Decision
**The shipped format is WebM with AV1 video (8 or 10 bit) and optional Opus audio. It is decoded in
software by dav1d, libopus and libwebm's `mkvparser`, vendored as C/C++ source and built by the
engine's CMake and Xcode projects on all six platforms. Platform decoders are not used.** The
reasons, per requirement:

1. **One file, six platforms.** Every library is plain C/C++ with a pthread or Win32 threading
   layer that builds unchanged for Emscripten (single-threaded when the web build has no pthreads,
   3.15) and Android. The platform-decoder route gives no codec that plays everywhere: Linux has
   none in general, and Chromium's open-source build has no H.264.
2. **Licences.** dav1d is BSD-2, libopus BSD-3 and libwebm BSD-3 with Google's WebM patent grant,
   so MIT and commercial games are fine. AV1 and Opus are royalty-free: the AOMedia Patent License
   1.0, and the royalty-free Opus declarations to the IETF (Xiph, Broadcom, Microsoft, ...). A
   third-party pool (Sisvel) claims AV1 patents and licenses device makers. As of this research no
   royalty is claimed for software decoders in applications or for distributed content, but a game
   maker that also manufactures hardware should check. H.264/HEVC would bring per-title and
   per-unit royalty pools into the game author's business; MPEG-1 is free but weak (3.1.3). FFmpeg
   (LGPL/GPL) is never linked or shipped. It is an optional tool the user installs, run as a
   separate process by the editor's importer (3.12.1).
3. **Determinism.** AV1 decoding is bit-exact by specification. dav1d's output does not depend on
   its thread count, and Chromium's dav1d gives the same YUV planes, so parity measures only the
   colour conversion (5.3). The demuxer indexes every frame, so a seek decodes from the previous
   keyframe to the exact frame. Platform decoders are asynchronous, have their own clocks, and on
   Web would decode on the browser's schedule.
4. **Performance.** Measured above. The element decodes ahead on a worker thread, converts on the
   GPU only when the shown frame changes, and stops decoding when paused, hidden, or after the
   queue is full (3.6).
5. **Audio.** Opus is decoded by the engine and played through a SoLoud audio source, slaved to the
   media clock (3.7). The browser's own `<video>` on Web would play its own audio, outside the
   engine mixer and the game's volume.
6. **Parity.** Chromium 153 plays AV1/Opus WebM. VP9 would also qualify, but it costs more bytes
   for the same quality (3.1.3) and its decoder is far harder to vendor (libvpx's generated
   `rtcd`/configure build). AV1 encoders are slower, which only matters for the editor's one-time
   conversion.

Rejected alternatives: **pl_mpeg/MPEG-1** is the simplest to vendor, but it needs about twice the
bytes, is not bit-exact, and no browser plays it (no parity). **Theora** costs about 2.4× the bytes
of AV1 and is no longer played by Chromium. **VP9 via libvpx** is a heavier build with no gain over AV1.
**Platform MP4 decoders** fail requirements 1, 3, 5 and 6, and partly 2 (2.1, "Out").

### 3.2 Libraries, build and licences

#### 3.2.1 Vendored libraries
| Library | Version | Location | Licence files | Build |
|---|---|---|---|---|
| dav1d | 1.5.4 (tag) | `engine/libs/dav1d/` | `COPYING` (BSD-2) | our `CMakeLists.txt`: the 25 sources + 13 bitdepth templates compiled twice (`BITDEPTH=8`, `16`) as object libraries, `src/win32/thread.c` on Windows; `config/config.h` decides `ARCH_*`, `HAVE_*` with the preprocessor (one header for every platform), `HAVE_ASM 0`, `CONFIG_LOG 0`, `TRIM_DSP_FUNCTIONS 1`; `DAV1D_API=` (hidden); `include/compat/msvc` on MSVC |
| libopus | 1.6.1 (tag) | `engine/libs/opus/` | `COPYING` (BSD-3), `AUTHORS` | our `CMakeLists.txt`: the decoder's CELT and SILK sources, float, `OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF`, `OPUS_EXPORT=` (hidden); no DNN (`dnn/` not vendored), no encoder |
| libwebm (`mkvparser` only) | 1.0.0.32 (tag) | `engine/libs/libwebm/` | `LICENSE.TXT` (BSD-3), `PATENTS.TXT`, `AUTHORS.TXT` | our `CMakeLists.txt`: `mkvparser/mkvparser.cc`, hidden visibility |

Each copy is byte for byte from the tag, minus what is not built (dav1d: `tools/`, `tests/`,
`examples/`, `doc/`, and the `src/` asm directories `x86 arm ppc riscv loongarch`; libopus: `dnn/`,
`tests/`, `training/`, `doc/`, the encoder-only sources, and the x86/ARM intrinsics directories).
Each library directory has a `README.jasper.md` that lists what was left out and how to update it.
Adding dav1d's asm later means copying `src/x86` (NASM, `config.asm`) and `src/arm` (GAS `.S`,
built by the C toolchain), adding their file lists to the CMake and Xcode targets, setting
`HAVE_ASM 1`, and running `checkasm` on each architecture.

Hidden visibility as for the other vendored libraries: `-fvisibility=hidden`, the API macros set
empty, `--exclude-libs` for `libdav1d.a libopus.a libwebm.a` where the shared engine links them,
and `hidden-symbols.map` hides `dav1d_*`, `opus_*` and `mkvparser::`. `tools/build.sh linux` fails
if `nm -D` shows any of them, as it does for `Rml::`.

#### 3.2.2 Wiring
- `engine/CMakeLists.txt`: `add_subdirectory` for the three libraries, include paths, warnings off
  (as for the other vendored libraries), and links. Every platform section links them.
- The Xcode workspace gets `dav1d-ios/-macos`, `opus-ios/-macos` and `webm-ios/-macos` targets
  from `tools/htmlui/xcode_add_video_libs.py [--check]`, modelled on `xcode_add_lexbor.py`
  (deterministic ids, idempotent). The engine's new sources are added by the existing
  `xcode_sync_compat.py`, whose scope gains `engine/core/htmlui/media/`.
- Android and Web compile through the engine's CMake: `tools/build.sh android`, `tools/build.sh web`.
- `NOTICE.md` gets a "Video of the HTML/CSS UI" table with the three libraries (version, location,
  licence, use). Exports already carry the third-party notices of `engine/libs` (the exporter copies
  the engine with its licence files).

### 3.3 Architecture

```
 <video> (UIVideoElement, engine/core/htmlui/media/)                 main thread
   │ attributes, <source> choice, poster, controls (non-DOM children), events, Lua API
   │ VideoPlayback: media time from HtmlUIClock, play/pause/seek/loop/rate/ended -> events
   │ picks the frame for the media time, uploads its planes, queues the YUV pass
   ▼
 VideoStream ─────────────── worker thread (one per playing video; inline on Web without pthreads)
   │ VideoDemuxer (WebM: mkvparser over FileData; frame index of both tracks)
   │ VideoDecoder (interface) <- Av1Decoder (dav1d)   -> VideoFrame queue (packed 8-bit planes, pts)
   │ OpusDecoder (libopus) -> VideoAudioRing (float PCM, media time of the next sample)
   ▼                                       ▼
 RmlRenderer: before the context's frame,   SoLoud mixer thread: VideoAudioSource reads the ring;
 HTMLUI_POST "Yuv" pass: Y,U,V (R8) ->      drift correction requested by the main thread (3.7)
 the element's RGBA8 render target, drawn
 like an <img> (object-fit, radius, filters)
```

Files (all new unless marked):
- `engine/core/htmlui/media/VideoFormats.{h,cpp}`: MIME and `codecs` parsing, `canPlayType`, the
  extension table, and `probeVideoFile(path)` → `VideoProbe` (container, codec ids, size, duration,
  bit depth, audio channels, error). Used by compat, the element, the importer and `check --ui`.
- `VideoDemuxer.{h,cpp}`: the WebM reader and the frame index.
- `VideoDecoder.h`, `Av1Decoder.{h,cpp}`, `OpusAudioDecoder.{h,cpp}`.
- `VideoPlayback.{h,cpp}`: the pure media-time state machine. No RmlUi, no threads; unit-tested
  like `LottiePlayback`.
- `VideoStream.{h,cpp}`: the decode pipeline (worker, queue, seeks, audio ring), with blocking and
  non-blocking frame requests.
- `VideoAudio.{h,cpp}`: `VideoAudioRing`, `VideoAudioSource`/`VideoAudioInstance` (SoLoud), and the
  sync controller.
- `UIVideoElement.{h,cpp}`: the element, rendering, events and controls.
- `UIElementVideo.cpp`: the `UIElement` API.
- Changed: `RmlRenderer` (the Yuv pass queue), `HtmlUIContextRender.cpp` (run the queue before
  `beginFrame`), `htmlui_post.frag` and `ShaderPool` (the variant), `HtmlUI.cpp` (instancer
  registration), and the compat tables, transforms, UA sheet and diagnostic codes.

### 3.4 HTML semantics

#### 3.4.1 Mapping (compat)
`<video>` stays `<video>` in the RML; the engine registers the `video` instancer
(`UIVideoElement::registerInstancer` in `HtmlUI::acquire`, like `<lottie>`). The compat transform
(`ElementTransform.cpp`) no longer makes a placeholder of it:
- **Attributes**: `src`, `poster`, `autoplay`, `loop`, `muted`, `controls`, `preload`, `width`,
  `height`, `id`, `class`, `style`, `data-*`, `aria-*`, `role`, `tabindex` and `title` are kept.
  `playsinline`/`webkit-playsinline` are dropped with an Info (always inline). `crossorigin`,
  `controlslist`, `disablepictureinpicture`, `disableremoteplayback`, `x-webkit-airplay` and
  `mediagroup` are dropped with an Info `media-attribute-ignored` (HTMLUI-HTML-0028). Event-handler
  attributes (`onended=...`) keep their phase-2 handling (HTMLUI-HTML-0005).
- **Children**: `<source>` children stay (UA: `display: none`) and are chosen at run time
  (3.4.3). `<track>` is removed (the existing HTMLUI-HTML-0002 "show captions as text elements").
  Any other child is fallback content, which browsers with video support never render. It is
  removed with an Info (HTMLUI-HTML-0028 "fallback content of `<video>` is not rendered").
- **Sources**: `src`, `poster` and `<source src>` go through the phase-2 URL resolver like `<img
  src>`: relative to the document, asset paths, `htmlui-imports` for remote URLs (otherwise
  HTMLUI-IMP-0001), and FileNotFound (HTMLUI-GEN-0001) for a missing file. A local video is
  probed (`probeVideoFile`, header only): a container or codec the engine cannot play gives
  `unsupported-media-format` (HTMLUI-HTML-0027, Warning) with the conversion as the fix
  (`import_files` converts it; 3.12), also for a `type` attribute `canPlayType` rejects.
- The phase-2 size hints of `width`/`height` stay (below author CSS).
- `@htmlui/` virtual paths are accepted for tests.

#### 3.4.2 Attributes at run time
| Attribute | Behaviour |
|---|---|
| `src` | The media resource. Setting it, or calling `load()`, runs the load algorithm: `emptied` if there was media, then a fresh load. Absent: the `<source>` children (3.4.3). |
| `autoplay` | Starts playback once `canplay` is reached (no autoplay policy: sound is allowed, unlike browsers without a user gesture). |
| `loop` | At the end, playback seeks to 0 and continues (`seeking`/`seeked`, no `ended`, seamless: 3.6.4). |
| `muted` | Sets `defaultMuted`, and `muted` at load. The property `muted` does not change the attribute (HTML). |
| `controls` | Shows the default controls (3.10); the element becomes focusable (`tabindex` 0 unless set). |
| `poster` | An image shown until playback starts or a seek happens (the HTML "show poster flag"), and while no frame is decoded. It is fitted with `object-fit`/`object-position` like the video. |
| `preload` | `none`: nothing is opened until `play()`, `load()` or a `currentTime` set (only the poster shows; intrinsic size from the poster or 300×150). `metadata` (also invalid values) and `auto` (also `""`): the file is opened, indexed and the first frame decoded; `auto` also fills the decode-ahead queue. Absent: `auto` (local files cost no network). |
| `width`, `height` | Size hints (phase 2), as for `<img>`. |
| `playsinline` | Dropped by compat (always inline). |

#### 3.4.3 Source selection
As the HTML resource selection algorithm, synchronously in the element when it loads:
1. A `src` attribute wins and `<source>` children are ignored. An empty `src` gives
   `MEDIA_ERR_SRC_NOT_SUPPORTED`.
2. Otherwise, each `<source>` child in order: skip it if it has no `src`, or if its `type` gives
   `canPlayType(type) == ""`, or if `media` does not match (phase-2 `evaluateMedia` against the
   document). Try to open the first that remains. On failure (missing file, unsupported
   container/codec, corrupt header), an `error` event is fired at that `<source>` and the next one
   is tried. If none works, `networkState` becomes `NETWORK_NO_SOURCE` with no `error` on the video
   (as HTML and Chromium), and the Console names every source tried and why it failed.
3. `currentSrc` is the resolved asset path that was opened.

`canPlayType(type)`: `video/webm` or `audio/webm` with codecs from {`av01[.*]`, `opus`} gives
`"probably"`. `video/webm` without `codecs` gives `"maybe"`. Any other codec (`vp8`, `vp9`,
`vp09.*`, `vorbis`, `avc1.*`, `hev1.*`), any other MIME type (`video/mp4`, `video/ogg`,
`video/quicktime`, ...) or a malformed type gives `""`. Extensions accepted for `src` without a
type: `.webm`. A `.mkv` file with AV1/Opus content plays too, since WebM is a Matroska subset and
the probe decides. `.mp4 .m4v .mov .ogv .ogg .avi .wmv .flv .mpg .mpeg .ts .3gp` are rejected
before opening.

#### 3.4.4 Size and rendering
- Intrinsic size: the video's display size (track `DisplayWidth/Height` with the pixel aspect
  ratio, else the coded size) once the metadata is known. Before that, the poster's size once it is
  loaded, else 300×150 (CSS 2, as in Chromium). Changing it relayouts the element.
- The content is drawn like the fork's `<img>`. `ComputeObjectFitRect` places the frame in the
  content box (default `object-fit: fill`, as in CSS; unlike `<lottie>`'s contain), and
  `ReplacedContentGeometry` trims it to the content edge curve of a rounded box (fork P4-G1). It is
  tinted by `image-color` × opacity. Filters, `backdrop-filter`, masks, transforms and phase-5
  effects need nothing: they act on the element's layer.
- `image-rendering: pixelated` samples the converted frame with nearest filtering.
- With nothing to show (no frame and no poster), only the box's own background is painted
  (transparent content, as in Chromium for a video without data).

### 3.5 Timing and the UI clock

#### 3.5.1 Media time
`VideoPlayback` holds the current position `t` (seconds). While it is playing (not paused, not
ended, not stalled on data in real-time mode), each update advances it by `playbackRate ×
(HtmlUIClock::now() − last)`. Like `LottiePlayback` it is lazy: every getter first catches up to
the clock. The frame shown is the one with the greatest presentation time ≤ `t` (pts from the
container, nanoseconds), which is frame-exact: `currentTime = 1.0` on a 30 fps clip shows frame 30.

The duration is the segment's `Duration` if present, else the end of the last video or audio frame
(its pts plus the frame duration, from `DefaultDuration` or the frame spacing). It is `NaN` until
the metadata is known. Playback reaches the end when `t ≥ duration`. Without `loop`, `t` is set to
`duration`, `paused` becomes true, and the events are `timeupdate`, `pause`, `ended` (HTML
"reaches the end"). With `loop`, `t` wraps (3.6.4). `play()` after the end seeks to 0 first.

#### 3.5.2 Effect of the engine's clocks (decision)
| Situation | UI clock | Video |
|---|---|---|
| `Engine.timeScale` 0 (a pause menu) or another scale | unscaled, runs | plays at its own rate: a menu background keeps moving under a pause menu (as `anime`, Lottie, CSS animations) |
| Engine paused (`systemPause`: app in background, window minimised on mobile) | frozen | frozen; SoLoud's `pauseAll` pauses its audio, and resuming continues in sync |
| Simulation held (MCP `auto_pause`, the editor not playing) | frozen | frozen, audio paused (the sync controller pauses the voice when media time stops) |
| `HtmlUIClock::setFixedTime` (tests, tools) | pinned | frozen at the pinned time; seeks still apply and show the exact frame at once |
| `Engine.setFixedDeltatime` (MCP `fixed_fps`) | advances by the fixed delta | advances by it, frame-exact |

A video that should follow game time (for example an in-game screen that freezes with the game)
sets `playbackRate = Engine.timeScale` from its script, or pauses with the game. The guide shows
this. A per-element clock attribute is not added because no standard name exists.

#### 3.5.3 Frame-exact mode and real-time mode
- **Frame-exact mode** is on while `HtmlUIClock::getFixedTime()` is set or
  `Engine::getFixedDeltatime() > 0`. The frame for `t` is waited for: the main thread blocks until
  the worker has decoded it, or decodes inline without a worker. A seek completes inside the same
  update, and the metadata and first frame are ready by the first layout after load. Goldens, the
  MCP `fixed_fps` runs and the browser-parity fixtures are therefore deterministic.
- **Real-time mode** (everything else) never blocks the main thread. The newest decoded frame with
  pts ≤ `t` is shown. If the frame for `t` is not decoded yet, the previous frame stays: `waiting`
  fires, `readyState` drops to `HAVE_CURRENT_DATA` and media time stops until the frame arrives,
  then `playing` fires. This only happens when decoding cannot keep up, and it matches a browser
  buffering. Decoded frames already behind `t` are dropped (`droppedFrames` counts them). If the
  decoder falls more than 0.5 s behind, it skips to the keyframe at or after `t` (as browsers drop
  frames). Seeks are asynchronous: `seeking` fires at once and `seeked` when the frame is decoded.

### 3.6 Decoding pipeline

#### 3.6.1 Interfaces
```cpp
struct VideoFrame {                         // packed 8-bit planes (10-bit is shifted with rounding)
    int64_t ptsNs; int64_t durationNs; int index;      // index in the video track's frame index
    int width, height, chromaShiftX, chromaShiftY;      // 4:2:0 -> 1,1; 4:2:2 -> 1,0; 4:4:4 -> 0,0; mono: no U/V
    VideoColor color;                                    // matrix (601/709/2020/identity), full range
    std::vector<uint8_t> y, u, v;                        // tightly packed rows
};
class VideoDecoder {                        // one compressed stream -> frames; worker thread only
public:
    virtual ~VideoDecoder() = default;
    virtual bool open(const VideoTrackInfo& track, std::string& error) = 0;
    virtual bool send(const uint8_t* data, size_t size, int64_t ptsNs, std::string& error) = 0;
    virtual bool receive(VideoFrame& frame, bool& gotFrame, std::string& error) = 0;   // one per send (max_frame_delay 1)
    virtual void flush() = 0;                                                           // before a seek
};
```
`Av1Decoder` implements it on dav1d with `n_threads` chosen by the frame size (1 up to 640×360, 2
up to 1280×720, else 4), `max_frame_delay = 1` (one frame out per frame in, no frame-threading
latency) and a picture allocator that counts its bytes (leak tests, `inspect_ui` statistics). A
future platform decoder implements the same interface.

#### 3.6.2 Demuxing and the index
`VideoDemuxer` reads the file through `FileData::newFile(path, true)` (a seekable handle: files on
desktop, `AAssetManager` on Android, the preloaded file system on Web, the resource pack) behind
`mkvparser::IMkvReader`, with a 64 KiB read-block cache. At open it parses the EBML header,
`Segment`, `Info`, `Tracks` and every cluster's block headers, and builds an index per track: file
offset, size, pts and keyframe flag of each frame (laced audio blocks are split). The whole
compressed file is not kept in memory: about 24 bytes per frame (40 KB for a minute of 30 fps
video). Accepted: EBML doctype `webm` or `matroska`. The first video track with `V_AV1`, the
first audio track with `A_OPUS`, 8/10-bit, any size up to 4096×4096. A video track with another
codec gives `MEDIA_ERR_SRC_NOT_SUPPORTED`, naming the codec. An audio track with another codec is
ignored, with a warning and the conversion as the fix: the video plays silent. Encrypted tracks
(`ContentEncryption`) are rejected. Unknown-size clusters (live muxers) are read as far as the
parser can.

#### 3.6.3 Decode-ahead and frame choice
The worker decodes into a queue of at most 4 frames past the shown one, and the audio ring up to
0.5 s ahead of `t`. It sleeps when both are full, when the element is paused with its frame shown,
or when the document is hidden or detached. It wakes on consumption, seek, play, rate change or
unload. On Web without pthreads there is no worker: the element decodes inline in its update, at
most 2 frames per update in real-time mode (else as many as frame-exact needs).

The element's `OnUpdate` asks the stream for the frame of `t`. When the shown frame changes, it
uploads the planes into three DYNAMIC R8 `HtmlUITexture`s (Y full size, U/V chroma size; one
`sg_update_image` each per frame, as the dynamic texture rule allows) and queues one conversion
for its RGBA8 target. Frames that are not shown are never uploaded.

#### 3.6.4 Seeking and loops
A seek to `t` clamps it to [0, duration] and finds the target frame (last pts ≤ `t`) and the
keyframe at or before it in the index. If the target is already in the queue, or ahead within
the next 8 frames of the decoder's position, decoding continues forward. Otherwise the decoder is
flushed, the demuxer moves to the keyframe, and the frames before the target are decoded and
discarded. The audio decoder is reset to the packet containing `t − 80 ms` (Opus pre-roll; the
first `CodecDelay`/pre-skip samples of the stream are dropped as RFC 7845 says). `fastSeek(t)` goes
to the keyframe at or before `t` (HTML's approximate seek).

Loops are seamless. With `loop` set, the worker queues the frames from 0 after the last one,
tagged with the next iteration, and the audio ring continues with the start of the audio. At the
wrap the element sees `seeking`/`seeked` (as Chromium fires them) without waiting.

#### 3.6.5 Colour conversion
`htmlui_post.frag` gains an exclusive variant `HAS_YUV` (`HtmlUIPostVariant::YUV`, property bit 6,
short name `Yuv`). It samples Y, U and V (R8, linear filtering, U/V at chroma size) and applies
the stream's matrix and range from a `mat4` and an offset in the existing uniform block. The output
is premultiplied, alpha 1. BT.709 is used when the stream says nothing, or BT.601 for frames under
720 lines, as FFmpeg and browsers do. The transfer function is not converted: the UI composites in
sRGB space and BT.709/sRGB video is shown as is (as Chromium does for SDR video). PQ/HLG (HDR)
streams are shown tone-unmapped with a warning, and the importer tone-maps them.

`RmlRenderer::queueVideoConversion(HtmlUITexture* target, HtmlUITexture* y, u, v, const
VideoColorParams&)` records one job per target (a newer frame replaces an older job).
`HtmlUIContextRender` runs the queue before `beginFrame`, outside any pass: one HTMLUI_POST pass
per job into the target. The target is an `HtmlUITexture` of kind RENDER_TARGET (RGBA8, one
sample, the frame's size) that the element hands to RmlUi through a `CallbackTexture`, so it is
released with RmlUi's textures and its job is dropped when it is. A view loss (`releaseGpuResources`)
kills targets and planes. The element makes them again and re-uploads the shown frame. This adds
no pass inside the context frame: a frame without new video frames costs nothing extra.

The ShaderPool table, the variant collector and export get the new variant through the existing
HTMLUI_POST loops (property count 7). The editor's shader cache version is bumped.

#### 3.6.6 Memory
Per video (C-build measurements are in the phase record, 7):
- compressed data: streamed (read cache 64 KiB) except on Web, where the preloaded file system
  holds the whole file;
- index: about 24 bytes per frame;
- dav1d: its reference pool (up to 8 reference frames plus the frames in flight at the coded
  size: about 12 MiB at 720p 8-bit, measured by the counting allocator);
- the queue: 4 packed frames (1.5 bytes per pixel each);
- GPU: Y/U/V planes (1.5 bytes per pixel) + the RGBA8 target (4 bytes per pixel);
- audio: 0.5 s of float PCM (192 KiB stereo).
Unloading the document, or removing the element, frees all of it (leak tests, 5.1).

### 3.7 Audio and A/V sync
- **Source.** `VideoAudioSource` is a `SoLoud::AudioSource` (48 kHz, the track's channels, mono or
  stereo; more channels are down-mixed by the decoder). Its instance reads `VideoAudioRing`, a
  single-producer single-consumer float ring that records the media time of the next sample. The
  ring is shared (`shared_ptr`) between the instance and the stream, so the mixer thread never sees
  a dead element. `AudioSystem::init()` is called when a video with audio first plays. Without an
  output device (`AudioSystem::isOutputAvailable()` false) no voice is made and audio is not
  decoded, except for the test consumer (5.1).
- **Master clock.** The picture follows the UI clock (3.5), and the audio is slaved to it: the
  only way pinned time, `fixed_fps` and the UI clock's rules can hold for both. Every update the
  sync controller compares the audible audio position (the ring's read position minus SoLoud's
  output latency, `getBackendBufferSize()/getBackendSamplerate()`) with `t`. Inside ±15 ms it does
  nothing. Beyond that it asks the mixer thread to drop samples (audio behind) or to insert silence
  (audio ahead), in steps of at most 10 ms per mixer callback. Beyond ±200 ms (a stall or a host
  hold) it resynchronises at once by flushing and refilling the ring from `t`. Seeks, loops and
  `play()` start the ring at `t`.
- **States.** Paused, ended, waiting or frozen media time pauses the voice (`setPause`). Resuming
  resynchronises. `volume` and `muted` set the voice volume (the global volume applies as for
  every sound). `playbackRate` sets `setRelativePlaySpeed` (pitch follows; rates above 4 or below
  0.25 mute the audio, as Chromium's range for audible rates). The voice is stopped at unload and
  when the element leaves the document. If SoLoud is torn down (`AudioSystem` deinit), the next
  update makes a new voice.
- **Measured** (5.1): with the test consumer pulling 10 ms blocks while the fixture clock steps
  1/60 s, the drift stays within 10 ms. A consumer running 3 % fast or slow stays within 25 ms (the
  controller's band plus one block). After a seek or loop, the drift is within 10 ms by the first
  callback.

### 3.8 States and events

#### 3.8.1 States
`readyState`: `HAVE_NOTHING` 0 before metadata, `HAVE_METADATA` 1 after it, `HAVE_CURRENT_DATA` 2
when the frame for `t` is shown, and `HAVE_ENOUGH_DATA` 4 when the decoder is ahead as well (local
files, so `HAVE_FUTURE_DATA` 3 is passed through on the same update, as Chromium does for local
files). It drops to 1 during an asynchronous seek and to 2 while waiting (3.5.3). `networkState`:
`NETWORK_EMPTY` 0, `NETWORK_IDLE` 1 (opened, or `preload="none"`), `NETWORK_LOADING` 2 (indexing,
or the first frame not yet decoded) and `NETWORK_NO_SOURCE` 3. `error`: `nil`, or a `MediaError`
with `code` 1 `MEDIA_ERR_ABORTED` (`load()` during a load), 2 `MEDIA_ERR_NETWORK` (the file became
unreadable after open), 3 `MEDIA_ERR_DECODE` (corrupt data) or 4 `MEDIA_ERR_SRC_NOT_SUPPORTED`
(missing, unsupported or unreadable at open), and a `message`.

#### 3.8.2 Events
Events are queued and dispatched in order in the element's `OnUpdate`. The events of a load wait
for a later engine frame (Lottie's `holdPending`), so the document's scripts, which run after load,
see them all. They do not bubble (HTML media events), so capture listeners see them. Names and
order, as Chromium 153 fires them for a local file:

| Action | Events |
|---|---|
| Load (`src` set, `load()`, element created) | `loadstart`, `durationchange`, `loadedmetadata`, `loadeddata`, `canplay`, `canplaythrough`; `autoplay`: then `play`, `playing` |
| A new load over a loaded one | `emptied` first (and `abort` if the old load had not finished) |
| `play()` while paused | `play`, then `playing` once the frame is there (same update in frame-exact mode) |
| `pause()` while playing | `timeupdate`, `pause` |
| Normal playback | `timeupdate` at most every 250 ms of UI time (HTML: 15-250 ms) |
| Seek (`currentTime =`) | `seeking`, then `timeupdate`, `seeked` when the frame is shown |
| Loop wrap | `seeking`, `timeupdate`, `seeked` |
| End without loop | `timeupdate`, `pause`, `ended` |
| `waiting` / resume (real-time only) | `waiting`, then `playing` |
| `volume`/`muted` change | `volumechange` |
| `playbackRate`/`defaultPlaybackRate` change | `ratechange` |
| Error | `error` at the `<video>` (`src` attribute) or at the failing `<source>` (3.4.3); `event.detail.message` |

A Lua listener gets the usual `UIEvent`; `error` events carry `detail.message`, and `el.error`
gives the code.

### 3.9 Lua and C++ API
`UIElement` gains the media API, implemented in `UIElementVideo.cpp`. It forwards to the
`UIVideoElement` behind the handle. On any other element it logs an error once per method and
tag, and returns the neutral value (Lottie's rule). The existing `play()`, `pause()`,
`isPlaying()`, `getDuration()` and `setLoop()` serve both `<lottie>` and `<video>`.

| Lua | C++ (`UIElement`) | Notes |
|---|---|---|
| `play()`, `pause()` | `play()`, `pause()` | as HTML; Lua's `play()` returns nothing (no promises) |
| `load()` | `load()` | the load algorithm |
| `canPlayType(type)` | `canPlayType(const std::string&)` | `"probably"`, `"maybe"`, `""` |
| `fastSeek(t)` | `fastSeek(double)` | to the keyframe at or before `t` |
| `currentTime` (get/set) | `getCurrentTime()`, `setCurrentTime(double)` | setting seeks (frame-exact), clamped |
| `duration` | `getDuration()` | `NaN` before metadata |
| `paused`, `ended`, `seeking` | `isPaused()`, `isEnded()`, `isSeeking()` | |
| `loop`, `autoplay`, `controls`, `defaultMuted` (get/set) | `getLoop()`/`setLoop(bool)`, `getAutoplay()/setAutoplay`, `getControls()/setControls`, `getDefaultMuted()/setDefaultMuted` | reflect the attributes |
| `muted`, `volume` (get/set) | `isMuted()/setMuted`, `getVolume()/setVolume(float)` | volume outside [0, 1]: warning, unchanged (HTML throws) |
| `playbackRate`, `defaultPlaybackRate` (get/set) | `getPlaybackRate()/setPlaybackRate`, `getDefaultPlaybackRate()/...` | 0 or [0.0625, 16]; others: warning, unchanged (HTML throws NotSupportedError) |
| `preservesPitch` | `getPreservesPitch()` | `false`; setting `true` warns |
| `readyState`, `networkState` | `getReadyState()`, `getNetworkState()` | 3.8.1 |
| `videoWidth`, `videoHeight` | `getVideoWidth()`, `getVideoHeight()` | display size; 0 before metadata |
| `currentSrc`, `poster`, `preload` (`poster`/`preload` get/set) | `getCurrentSrc()`, `getPoster()/setPoster`, `getPreload()/setPreload` | |
| `error` | `getMediaError()` → `std::optional<UIMediaError>{code, message}` | Lua: `nil` or `{ code, message }` |

The constants `MEDIA_ERR_*`, `HAVE_*` and `NETWORK_*` are exposed on the `UIElement` class table
(`UIElement.HAVE_ENOUGH_DATA`), as HTML exposes them on `HTMLMediaElement`. The Lua stubs
(`generate_lua_stubs.py`) and the API suggestions get the members.

### 3.10 Controls (decision: built in)
The controls are worth building: cutscene panels and trailers need them, they are made of the HTML
UI's own boxes, and they are small. Without them, `controls` would only be a diagnostic, and
authors would build a bar per game.

- **Structure.** When `controls` is present the element makes non-DOM children (RmlUi's
  `AppendChild(…, false)`, like scrollbars). They are not in `children`, `querySelector` or
  `inspect_ui`'s tree, and `inspect_ui` reports their rectangles in the media block:
  ```
  videocontrols                         bottom bar, absolute, full width
    videobutton.htmlui-play             play/pause (class "paused" swaps the icon)
    videotime                           "0:03 / 0:10" (m:ss, h:mm:ss from one hour)
    videotimeline                       seek bar: videotrack > videoprogress, videothumb
    videobutton.htmlui-mute             mute (class "muted")
  ```
- **Look** (UA sheet, Chromium's in spirit): a 40 px bar over a gradient from transparent to
  `rgb(0 0 0 / 60%)`. White 24 px Lucide icons (`play`, `pause`, `volume-2`, `volume-x`; ISC, as
  `/@htmlui/ua/video-*.svg` masks coloured by `background-color: currentColor`) in 32 px buttons.
  Time in 13 px `system-ui` with tabular figures. A 4 px timeline (white 30 % track, white
  progress, a 12 px white thumb). The bar shows while the video is paused, hovered or
  focus-visible, and fades out (`opacity` 250 ms) 2.5 s after the last pointer move while it
  plays (UI clock). Authors restyle it with ordinary selectors on these tag names (`video
  videocontrols { … }`), and the guide lists them. `::-webkit-media-controls-*` selectors stay
  unsupported (diagnosed by phase 2's selector code).
- **Pointer.** A click on the play button toggles playback. A click on the picture toggles it too
  (Chromium). A press on the timeline seeks to that point, and dragging scrubs (seeks follow the
  pointer, once per update). The mute button toggles `muted`. Wheel events are left to the page.
- **Keyboard and gamepad** (the video is the single focus stop, `tabindex` 0): Space, Enter, `K`
  and GamepadA (Enter) toggle playback. ArrowLeft/ArrowRight (and the gamepad's left/right
  directions, which arrive as those keys) seek −/+5 s. `J`/`L` seek −/+10 s. Home/End go to the
  start/end, and `M` toggles mute. These default actions consume the key, so spatial navigation
  does not move on left/right. Up/down are not consumed, so navigation leaves the video. A
  listener's `preventDefault` disables the default action, as for other elements.
- Without `controls` the element is not focusable unless `tabindex` says so, and it has no default
  key actions (Chromium).

### 3.11 Video and effects (decision)
- `filter: effect(...)`, CSS filters, `mask-image` and `opacity` on a `<video>`, and
  `backdrop-filter`/effects of elements over a video, work unchanged. They read the rendered
  layer, which holds the converted frame (phase 5 §3.1 "filter form", "backdrop"). Goldens cover
  `filter: effect(chromatic-aberration)` on a video and `backdrop-filter: effect(frosted-glass)`
  over one (5.2).
- `url(*.webm)` (any video extension) in `background-image`, `background`, `mask-image`,
  `content`, `list-style-image`, `border-image` or `cursor` is reported as `HTMLUI-CSS-0004
  unsupported-value` ("a video cannot be a CSS image") with the fix "place a `<video>` with
  `object-fit: cover` behind the content". The layer is dropped.
- A video as a paint effect's texture input is out of scope: phase 5 has no texture parameter
  type. When one is added, the element's RGBA8 target is the texture to bind.

### 3.12 Assets

#### 3.12.1 Import and conversion (editor)
`editor/htmlui/VideoImporter.{h,cpp}`, used by `import_files` (local files and remote URLs) and by
drops on the Project window:
- A file that `probeVideoFile` accepts (AV1/Opus WebM) is copied unchanged.
- Any other video (by probe or extension: `.mp4 .m4v .mov .mkv .webm`(VP8/VP9) `.ogv .avi .wmv
  .flv .mpg .mpeg .ts .3gp .gif`) is converted when an FFmpeg is found. Lookup order: the
  `JASPER_FFMPEG` environment variable, the editor setting `htmlui.ffmpegPath`, then `ffmpeg` on
  `PATH`. Its encoders are listed once (`ffmpeg -hide_banner -encoders`), and the first AV1 encoder
  of `libsvtav1`, `libaom-av1` and `librav1e` is used:
  `ffmpeg -nostdin -y -i <in> -map 0:v:0 -map 0:a:0? -c:v libsvtav1 -preset 6 -crf 32 -g <2 s of
  frames> -pix_fmt yuv420p[10le] -c:a libopus -b:a 128k -ac 2 -f webm <out>.webm`. `libaom-av1`
  uses `-cpu-used 6 -row-mt 1 -crf 30 -b:v 0`, and `librav1e` uses `-speed 6 -qp 100`. HDR input
  is tone-mapped to BT.709 SDR (`zscale`+`tonemap` when FFmpeg has them, else a warning). Sizes
  above 4096 px are scaled down. The output `<name>.webm` sits where the original would have gone,
  and the original is not copied. The conversion runs as an MCP job (progress, cancel), and its
  command line and result are returned and logged.
- No FFmpeg, or no AV1 encoder: the original is copied, and the result carries
  `HTMLUI-IMP-0010 video-not-converted` (Warning) with the exact FFmpeg command to run, or "install
  FFmpeg with an AV1 encoder (SVT-AV1)". The licence note: FFmpeg is the user's own tool, run as a
  separate program, never linked or shipped.
- A conversion failure (exit code, unreadable input) gives `HTMLUI-IMP-0011 video-conversion-failed`
  (Error) with FFmpeg's last stderr lines.
- Remote URLs in `import_files` `sources` (and the HTML UI auto-import of a document's remote
  `<video src>`/`<source src>`/`poster`) download into `assets/htmlui-imports/` (phase 2), then
  convert as above. The manifest maps the URL to the converted file.

#### 3.12.2 Export and per-platform shipping
`.webm` files are ordinary assets: `copyAssets` copies them, and the resource pack (when enabled)
holds them. Only the files the game has are shipped; the importer already dropped the originals.
Per platform: desktop and iOS read them from the asset directory or the pack. Android stores them
in the APK uncompressed (`.webm` is on AAPT's no-compress list), read through `AAssetManager`.
Web preloads them into the Emscripten file system, so they count fully in the page's memory, and
the guide says so. `HtmlUIDependencies` lists a document's video `src`, `<source src>` and
`poster` files, so the export check (`ProjectChecks`) reports missing ones and hot reload watches
them. A changed video reloads the elements that use it, keeping their `currentTime` and paused
state.

#### 3.12.3 Budget (guide)
| Platform | Guidance (C build, 3.1.4) |
|---|---|
| Desktop | one 1080p30, or two 720p30, plus small previews; bitrate 2-6 Mbit/s |
| Mobile | 720p30 at most for one video, 540p preferred; previews ≤ 256 px |
| Web (no pthreads: decoding on the main thread) | 480p or smaller, one at a time; build with `EMSCRIPTEN_THREAD_SUPPORT` for more |
| Memory per video | 720p: ≈ 12 MiB decoder + 5 MiB queue/GPU (3.6.6); file streamed (Web: resident) |

### 3.13 Diagnostics and the support matrix
New codes (registry rows in `DiagnosticCodes.cpp`, Appendix F of phase 2 extended):

| Code | Name | Severity | When |
|---|---|---|---|
| `HTMLUI-HTML-0027` | `unsupported-media-format` | Warning (by case: Error when no source of a `<video>` is playable) | a `src`/`<source>` whose extension, `type`, container or codec the engine cannot play (also at run time, in the Console) |
| `HTMLUI-HTML-0028` | `media-attribute-ignored` | Info | `playsinline`, `crossorigin`, `controlslist`, `disablepictureinpicture`, `disableremoteplayback`, `x-webkit-airplay`, `mediagroup`; fallback content of `<video>` |
| `HTMLUI-IMP-0010` | `video-not-converted` | Warning | the importer kept a video the engine cannot play (no FFmpeg or no AV1 encoder) |
| `HTMLUI-IMP-0011` | `video-conversion-failed` | Error | FFmpeg failed |

The existing codes cover a missing file (`HTMLUI-GEN-0001`), a remote URL not imported
(`HTMLUI-IMP-0001`), `<track>` (`HTMLUI-HTML-0002`), event attributes (`HTMLUI-HTML-0005`), video
in CSS images (`HTMLUI-CSS-0004`) and `::-webkit-media-controls*` (`HTMLUI-SEL-0001`). Run-time
errors are logged as `[HTML UI] <video src="...">: <message> [HTMLUI-HTML-0027]`.

Support tables (`ElementTable.cpp`; the matrix is regenerated by `gen_html_ui_support.py`):
`video` becomes Emulated ("UIVideoElement: AV1 + Opus WebM; controls, poster, preload"). `source`
says "img: chosen at load; video: chosen at run time". New attribute rows: `video`
`autoplay controls loop muted playsinline poster preload src crossorigin controlslist
disablepictureinpicture disableremoteplayback`, and `source` `src type media` for video.

### 3.14 Tooling
- `inspect_ui`: a video element's entry gains `media`: `{currentSrc, currentTime, duration, paused,
  ended, seeking, loop, muted, volume, playbackRate, readyState, networkState, videoWidth,
  videoHeight, error, frame (index shown), droppedFrames, decodeMs (average per frame), memory
  (bytes), controls: {visible, play, timeline, mute}}` (rectangles in document px).
- `check --ui` and the write feedback of MCP tools report the new diagnostics through compat
  unchanged.
- `import_files` documents the conversion in its description (within the tool-description cap of
  phase 4/5; the other descriptions are not lengthened).
- The guide (`docs/mcp/guide/html-ui.md`) gets a "Video" section: the format, the conversion, the
  attributes, the Lua API, timing, controls, budget, and the effects rule. The UI tools' shared rules
  (`jasper://docs/html-ui`) mention `<video>` in one line.

### 3.15 Platform notes
- **Threads**: `std::thread` workers and dav1d's pthread (POSIX) or Win32 layers on desktop and
  mobile. On Web without `EMSCRIPTEN_THREAD_SUPPORT`, dav1d runs with one thread (it creates
  none) and the stream decodes inline (3.6.3). With it, it uses workers as on desktop.
- **Files**: only `FileData`, so assets, packs, Android assets and the Web file system all work.
- **Audio**: SoLoud's miniaudio backend on every platform. The Web's audio context starts on the
  first user gesture (browser policy, as for all sounds), and until then a video's audio is
  silent and stays in sync.
- **GPU**: R8 textures and RGBA8 targets exist on every sokol backend (GL core, GLES3/WebGL2,
  D3D11, Metal, Vulkan).
- Windows, macOS and iOS cannot be built here. The code uses only the standard library, `FileData`,
  SoLoud and sokol through the render abstraction. The Xcode targets come from the sync scripts
  (`--check` passes), and the CMake platform sections are wired by reading the existing ones.

### 3.16 Contracts
New (CONTRACTS.md, provider phase 6): `UIVideoElement` (the `video` instancer), `VideoPlayback`,
`VideoStream`, `VideoDecoder`, `probeVideoFile`/`canPlayType` (`VideoFormats.h`), the
`HtmlUIPostVariant::YUV` variant and `RmlRenderer::queueVideoConversion`, the `UIElement` media API,
`inspect_ui` `media`, `VideoImporter`, the diagnostic codes of 3.13, and the three vendored
libraries. Changed: the phase-2 element mapping of `video` (no placeholder), and the `source` row.

### 3.17 Deviations from DESIGN.md
To be folded into DESIGN.md "Recorded deviations" at acceptance:
1. `<video>` is supported (the guide and phase 2 said "no `<video>`"): AV1/Opus WebM decoded by
   vendored software decoders (DESIGN "Names and locations" gains the libraries; "Goal" is
   unchanged: still no browser, no webview, no runtime network).
2. The UI clock drives video too (deviation 11 extended), with frame-exact mode under pinned or
   fixed-step time.
3. Phase 2's placeholder for `<video>` (§3.7.3) is replaced by the element. `<audio>`, `<canvas>`
   and `<iframe>` keep it.
4. No RmlUi fork patch is needed: non-DOM children, `ComputeObjectFitRect`,
   `ReplacedContentGeometry` and `CallbackTexture` already exist. FORK.md records that phase 6
   added none.

## 4 Work package `p6-video`
Owner of everything in 3.3's file list, `engine/libs/{dav1d,opus,libwebm}`,
`engine/tests/htmlui/video/`, `engine/tests/htmlui/fixtures/video/`,
`engine/tests/htmlui/fixtures/p3/video/`, `engine/tests/htmlui/golden/video/`,
`tools/htmlui/gen_video_fixtures.py`, `tools/htmlui/xcode_add_video_libs.py`,
`editor/htmlui/VideoImporter.*`, `tools/mcp/tests/test_html_ui_video.py`, the example
`docs/mcp/guide/html-ui-examples/video_menu/`, and this spec. Small additive edits elsewhere,
listed in the phase record: compat (`ElementTransform`, `AttributeTransform`/URL resolution,
`CssRewriter`'s url check, tables, UA sheet and SVGs, `DiagnosticCodes`), `RmlRenderer`,
`HtmlUIContextRender`, `htmlui_post.frag`, `ShaderPool`/`ShaderVariantCompiler` (one variant),
`HtmlUI.cpp` (instancer), `UIElement.h`, `HtmlUIClassesLua.cpp`, `HtmlUIInspector`,
`HtmlUIDependencies`, `AssetTools.cpp` (`import_files`), CMake lists, `hidden-symbols.map`,
`tools/build.sh` (the `nm -D` check), `NOTICE.md`, the guide and the generated docs.

## 5 Test plan

### 5.1 C++ (`engine/tests/htmlui/video/`, target `jasperengine-htmlui-tests`)
The test clips are made by `tools/htmlui/gen_video_fixtures.py` (5.4) and committed in
`engine/tests/htmlui/fixtures/video/`. Every frame of the test card encodes its own index as 8
black-or-white 16×16 blocks (readable after decoding and conversion), with flat colour bars, a
square moving one block per frame, and a gradient strip. The audio has a 1 kHz tone burst at the
start of every second, aligned with a white flash frame.

- **Formats/probe**: `canPlayType` table; probe of every fixture (codec, size, duration, frame
  count, bit depth, channels); VP9 WebM, MP4, Ogg, truncated header, random bytes, missing file,
  encrypted track: the right error and code.
- **Demux/decode**: every frame decodes to its index (pattern) and its pts. Frame-exact seeks to
  every frame time forwards, backwards and across keyframes (`t` = pts, pts + ½ frame, pts − 1 ns)
  each show the right index. `fastSeek` lands on keyframes. A 10-bit clip gives the same patterns
  as the 8-bit one. Thread counts 1/2/4 give identical planes.
- **Playback state machine** (`VideoPlayback`): play/pause/seek/rate (0, 0.5, 2, negative
  rejected)/loop/ended sequences, `timeupdate` cadence, clamping, `play()` after the end.
- **Element on the fixture clock**: load events in Chromium's order (with and without autoplay,
  `preload="none"`); the frame at fixed times; pause/resume (time stands while paused, continues
  exactly after); loop (wrap events, frame 0 after the last frame); end events; source
  selection order and per-source errors; intrinsic size (metadata, poster, 300×150) and relayout;
  poster shown until play/seek; `muted`/`volume`/`playbackRate` events; `load()` mid-load
  (`abort`, `emptied`); attribute changes (`src` swap, `loop` toggle); four videos at once (each at
  its own time and state, the right frames); real-time mode (an artificial decode delay gives
  `waiting`/`playing` and dropped frames, never a block).
- **Lua**: every property and method of 3.9 from a document script, event listeners in order,
  `error` table, constants, the error log on a non-video element (once).
- **Pause/resume and clocks**: fixed time, fixed delta, `Engine.timeScale` 0 (video still plays),
  simulation held (frozen, audio paused).
- **Audio and A/V sync**: Opus decode against the generated tone (bursts at the right samples).
  Sync measured with the test consumer as in 3.7 (exact, 3 % fast, 3 % slow, after a seek, after
  a loop, after pause/resume). Muted gives zeros but keeps the position, and volume scales.
- **Leaks**: 100 load/unload cycles of a document with two videos (and 100 `src` swaps). After
  them the renderer's live textures, GPU handles, framebuffers, dav1d bytes, worker threads,
  SoLoud voices and the stream count are back at their baseline.
- **Errors**: missing file, unsupported codec, corrupt data mid-stream (`MEDIA_ERR_DECODE` at the
  frame, the last good frame stays), a file removed after open, a zero-frame file, unknown track
  layout.
- **Controls**: made with `controls` and removed without it; pointer (play button, picture,
  timeline seek and drag, mute); keyboard (Space, Enter, arrows, J/L, Home/End, M; up/down left
  to navigation); gamepad A and directions; auto-hide on the UI clock; their boxes.

### 5.2 Goldens (`engine/tests/htmlui/golden/video/`, glcore and Vulkan)
At fixed UI times: frames at t = 0, 0.5 and 1.0 s; `object-fit` fill/contain/cover/none/
scale-down with `object-position`; `border-radius` (rounded content); `filter: grayscale()
blur()`; `filter: effect(chromatic-aberration)`; `backdrop-filter: effect(frosted-glass)` over a
video; poster; the controls paused, hovered and focused; four small videos in a grid; a 10-bit
clip. The same images on both backends within the harness's default tolerance.

### 5.3 Browser parity (`engine/tests/htmlui/fixtures/p3/video/`, Chromium 153)
`render-reference.mjs` gains the step `{"seekVideo": ["#id", seconds]}` (the reference sets
`currentTime`, waits for `seeked` and one `requestVideoFrameCallback`; the engine's `FixtureRunner`
sets `currentTime` in frame-exact mode). Fixtures:
- `seeked-cover-radius`: a paused video seeked to 1.0 s, `object-fit: cover`, `object-position:
  30% 70%`, `border-radius: 24px`;
- `seeked-contain`: seeked to 0.5 s, `object-fit: contain`, letterboxed on a coloured background;
- `poster`: `preload="none"` with a PNG poster, `object-fit: cover`, rounded.
Boxes must match exactly (`"tolerance": {"box": 0}`). Images use the strict comparator. The colour
difference of the YUV conversion (Chromium's software path against the `Yuv` shader) is measured
on these fixtures and recorded in the sidecar's comment. If the default metric does not hold, the
override is the measured maximum per channel, with the justification (the BRIEF policy: a
measured, explained difference, no slack).

### 5.4 Test clips and generator
`tools/htmlui/gen_video_fixtures.py` writes the raw frames and the tone with Python (no other
dependencies) and encodes them with FFmpeg. libaom-av1 runs with `-cpu-used 8 -threads 1` and fixed
GOP and keyframes, and `libopus` encodes the audio. Clips:
- `testcard.webm`: 320×180, 30 fps, 2 s, keyframes at 0 and 1 s, Opus stereo;
- `testcard-noaudio.webm`;
- `testcard-10bit.webm`;
- `testcard-vp9.webm` (to test the unsupported-codec path);
- `poster.png`;
- `corrupt.webm` (a valid header with a damaged frame);
- the example's `menu-loop.webm`: 640×360, 4 s, no audio, seamless loop.
Each is about 10-150 KB. `--check` decodes the committed clips with FFmpeg (frame count, size,
codecs, per-frame index pattern, tone positions) without re-encoding, because AV1 bitstreams
differ between encoder versions. `README.md` in the fixture directory documents the generator,
the FFmpeg version and the licence (ours, MIT).

### 5.5 MCP (`tools/mcp/tests/test_html_ui_video.py`)
Play mode with `fixed_fps: 60, auto_pause: true`, on a document with a `<video controls>` and a
looping muted background video:
- `inspect_ui` shows `media` (`readyState` 4, `duration`, `videoWidth`, `paused`) and the control
  rectangles;
- `send_input` click on the play button, then `wait 30 frames`: `currentTime` = 0.5 s exactly, and
  the screenshot shows the frame-0.5 pattern;
- the Space key pauses; ArrowRight seeks +5 s, clamped to the end (ended);
- the background loop's `currentTime` wraps;
- `import_files` with an MP4 made by FFmpeg converts it (or, with `JASPER_FFMPEG` pointing nowhere,
  returns `HTMLUI-IMP-0010`);
- `check --ui` on a document with an `.mp4` source reports `HTMLUI-HTML-0027`.
The whole MCP suite runs too, `test_protocol.py` and the UI tests included.

### 5.6 Export (slow tests)
A project with the `video_menu` example document is exported for Linux (the Desktop player) and
run: the screenshot at a fixed frame shows the video's pattern, and the Console has no error. The
web build is exported and loaded in Chromium headless (the slow web-export test's path): the canvas
shows the video frame. Android is compiled (`tools/build.sh android`), not run.

### 5.7 Builds
Linux with zero new warnings; Vulkan configuration; web; Android; the generator, support and
Xcode sync `--check`s; `run-rmlui-tests.sh` (unchanged fork); `htmlui-editor-tests`.

## 6 Acceptance criteria
1. `<video>` with every attribute of 3.4.2 and `<source>` selection behaves as specified, with
   diagnostics of 3.13.
2. AV1/Opus WebM decodes on Linux (run), Web and Android (compiled and the web build exported and
   played), and Windows/macOS/iOS targets are wired (`--check`s).
3. Frame-exact seeking and deterministic playback under the fixture clock; the 5.1 tests pass
   on glcore and Vulkan, the goldens of 5.2 hold on both.
4. A/V sync within the measured bounds of 3.7.
5. Browser parity fixtures pass with exact boxes and the documented colour tolerance.
6. 100 load/unload cycles leak nothing.
7. Controls work with pointer, keyboard and gamepad.
8. The importer converts with FFmpeg, and diagnoses without it.
9. MCP, export and slow tests pass; docs, matrix, CONTRACTS, FORK, DESIGN updated.

## 7 Phase record
*(filled at the end of `p6-video`: what was built, deviations from this spec, measurements, runs, and
what could not be verified.)*
