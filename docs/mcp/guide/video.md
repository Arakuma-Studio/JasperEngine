# Video: the Video Player, MP4 and URL video

The engine plays video in two places: the **Video Player component** (Unity's VideoPlayer: a render
texture, a material, a camera plane) and the HTML UI's **`<video>`** element. Both open their files
the same way (the design: `docs/superpowers/specs/2026-10-31-video-mp4-design.md`).

## Formats and where they play

| Format | How it decodes | Where |
|---|---|---|
| **WebM AV1 + Opus** | the engine's own software decoders (dav1d, libopus) | everywhere, identical frames (goldens) |
| **MP4 H.264 + AAC** | each platform's decoders | Android, iOS, macOS, Windows, Web; Linux with FFmpeg |
| **MP4/MOV HEVC** | the platform's | Apple; Android devices that have it; Windows with Microsoft's *HEVC Video Extensions*; Linux FFmpeg; Safari and hardware Chromium |
| **WebM VP8/VP9** | the platform's | Android; Windows (VP9 Video Extensions); Linux FFmpeg; Chromium/Firefox; not Apple |

The file's **content** decides, not its extension: AV1/Opus WebM always takes the software path,
anything else the platform's decoders when they have its codecs. When they don't, the player gets an
error (`onErrorReceived`, `<video>` `error`) that names the codec and the fix.

- **Linux**: MP4 plays with the **system's FFmpeg libraries** (libavcodec, libavformat,
  libswresample; FFmpeg 4.x to 8.x), loaded at run time and never shipped. Debian/Ubuntu:
  `libavcodec60 libavformat60 libswresample4` or newer. Fedora: `ffmpeg-libs` from RPM Fusion
  (Fedora's own `ffmpeg-free` has no H.264). Without them: the error says what to install. Unity
  itself plays no H.264 on Linux.
- **Windows N editions** need the Media Feature Pack; HEVC/VP9/AV1 need the Store extensions.
- **Web**: the browser plays it (`<video>` behind the scenes, its frames uploaded with
  `texSubImage2D`); cross-origin URLs need the server to send `Access-Control-Allow-Origin`; sound
  starts after the page's first click or tap (the browsers' autoplay rule).
- **Licensing**: the engine ships no H.264/HEVC/AAC decoder (the platforms' carry their licences;
  Linux loads the system's LGPL FFmpeg dynamically). For paid titles, check the MPEG LA / Via LA AVC
  pool's terms for *content*; WebM AV1 is the royalty-free choice.

The Inspector of a video asset (select it in the Project window) shows the probe, the per-platform
matrix (`*` = Linux needs FFmpeg) and **Convert to WebM (AV1)** (FFmpeg CLI). `get_asset_info` on a
video returns `video` (container, codecs, size, duration, frame rate, rotation), `fast_start` and
`playable_on: {linux, windows, macos, ios, android, web}` with `"yes" | "needs_ffmpeg" |
"needs_extension" | "device_dependent" | "no"`. `jasperengine-editor check --videos` warns per
platform (`video-codec-unsupported-on-target`) and about URL MP4s that are not fast start
(`video-not-faststart-url`). MP4s are imported as they are (no automatic conversion).

## The Video Player component

Add Component > Video > Video Player, or GameObject > Video > **Video Player** (render texture) /
**Video Player (Camera Background)** (far plane of the main camera, looping, filling it). MCP:
`create_entity type video_player` (or `video_background`).

| Field | Default | |
|---|---|---|
| `source` | `clip` | `clip` (a video asset) or `url` |
| `clip` / `url` | "" | `url`: `https://...`, `data://ads/ad1.mp4` (`System.getUserDataPath()`), an absolute or asset path |
| `playOnAwake`, `waitForFirstFrame`, `isLooping`, `skipOnDrop` | true, true, false, true | as Unity |
| `playbackSpeed` | 1 | 0.0625 .. 16 (sound muted outside 0.25 .. 4) |
| `timeUpdateMode` | `game_time` | `game_time` (timeScale slows/pauses it) or `unscaled_game_time` (ads, pause menus) |
| `renderMode` | `render_texture` | `camera_far_plane`, `camera_near_plane`, `render_texture`, `material_override`, `api_only` |
| `targetCamera`, `targetCameraAlpha`, `aspectRatio` | main camera, 1, `fit_inside` | camera planes (and the letterbox of a fixed-size texture) |
| `targetMaterialRenderer`, `targetMaterialProperty`, `targetSubmesh` | this, `baseColorTexture`, -1 | Material Override (a Mesh's material texture or a shader `@texture` uniform; a Sprite/Image's `texture`) |
| `renderTextureWidth/Height` | 0 x 0 (the video's) | a fixed size: the picture fits by `aspectRatio`, bars transparent |
| `audioOutputMode` | `direct` | `none`, `audio_source` (that AudioSource's volume and 3D position/attenuation), `direct` |
| `directVolume`, `directMute`, `targetAudioSource` | 1, false, this | |

The render texture is the player's own RGBA target: show it with a texture link **`video|<entity
id>`** (any texture property: UI Image `texture`, a material slot; Inspector texture pickers list
"Video Player: <name>"), `image:setTexture(player:getFramebuffer())`, or `player.texture`.
Camera Far Plane replaces the background (after the sky, under the scene); Near Plane goes over
the scene, under the UI. Material Override puts the texture into the renderer's slot while the
player has a picture and gives the slot its own texture back when it stops.

The editor plays nothing in edit mode: Play runs `playOnAwake`. In Play the Inspector has the
playback bar (play/pause/stop, step, scrubber, prepared/buffered, the backend) and the texture.

### Lua

```lua
local vp = self.gameObject:getVideoPlayer()       -- nil without one (hasVideoPlayer())
vp.source = VideoPlayerSource.URL
vp.url = System.getUserDataPath() .. "/ads/ad1.mp4"
vp.timeUpdateMode = VideoTimeUpdateMode.UNSCALED_GAME_TIME
vp.onPrepareCompleted = function(p) adImage:setTexture(p:getFramebuffer()); p:play() end
vp.onLoopPointReached = function(p) closeAd() end           -- each loop, and the end of a play
vp.onErrorReceived = function(p, message) Log.error(message); closeAd() end
vp:prepare()
```

- Methods: `prepare()`, `play()` (prepares first when needed), `pause()`, `stop()` (time 0, the
  decoder released; the texture keeps its last frame), `stepForward()`, `getFramebuffer()`;
  static `VideoPlayer.canPlay(pathOrUrl)` (`"probably"`, `"maybe"`, `""`) and
  `VideoPlayer.clearCache([url])`.
- State (read only): `isPrepared`, `isPlaying`, `isPaused`, `frameCount`, `frameRate`, `length`,
  `width`, `height` (displayed: rotation applied), `prepareProgress` (0..1: the download, then the
  opening), `canSetTime`, `texture` (nil before prepared), `backend` (`"webm"`, `"ffmpeg"`,
  `"mediacodec"`, `"avfoundation"`, `"mediafoundation"`, `"browser"`), `videoCodec`, `audioCodec`,
  `error`. `time` and `frame` are read/write (writing seeks; `onSeekCompleted` when the frame shows).
- Events (function properties, called with the player on the main thread): `onPrepareCompleted`,
  `onStarted` (the first frame of a play), `onLoopPointReached`, `onErrorReceived(player,
  message)`, `onSeekCompleted`, `onFrameReady(player, frameIndex)` (a call per frame: set it only
  when needed).
- Enums: `VideoPlayerSource.CLIP/URL`, `VideoTimeUpdateMode.GAME_TIME/UNSCALED_GAME_TIME`,
  `VideoRenderMode.CAMERA_FAR_PLANE/CAMERA_NEAR_PLANE/RENDER_TEXTURE/MATERIAL_OVERRIDE/API_ONLY`,
  `VideoAspectRatio.NO_SCALING/FIT_VERTICALLY/FIT_HORIZONTALLY/FIT_INSIDE/FIT_OUTSIDE/STRETCH`,
  `VideoAudioOutputMode.NONE/AUDIO_SOURCE/DIRECT`.
- Coroutines: Unity's `WaitUntil(() => vp.isPrepared)` is `waitUntil(function() return vp.isPrepared end)`.

Rules: a new `clip`/`url` stops the player (`isPrepared` false; `playOnAwake` does not start it
again). `play()` before prepared prepares, fires `onPrepareCompleted`, then `onStarted` with the
first frame (with `waitForFirstFrame` the clock starts then). Destroying the GameObject (or
unloading its scene) closes the source and stops its sound. Two players of one file decode
separately; of one URL, they share the download. `frameCount` of a file without a frame index
(variable rate, WebM through the platform) is an estimate, `round(length x frameRate)`, as Unity's.
Phone videos are shown turned as their rotation says.

### MCP

`set_properties {"url": "data://ads/ad1.mp4", "source": "url", "renderMode": "render_texture"}`;
an Image's texture `"video|<id>"` (or `{"video": <entity>}`); in Play `get_runtime_entity` (and
`get_entity`) add `video: {prepared, playing, paused, time, frame, frameCount, frameRate, length,
backend, videoCodec, audioCodec, prepareProgress, buffered, error}`; drive it with `eval_lua`
(`go:getVideoPlayer():play()`), then `screenshot`.

## URLs, the media cache, progressive start

`http(s)://` sources are downloaded by the engine's HTTP client (jasper://docs/networking) into
**`<userdata>/.cache/media/<sha256(url)>.<ext>`** and played from the file:

- A cached copy plays at once and is revalidated in the background (ETag / Last-Modified: a 304
  keeps it, a newer file replaces it once nobody plays it). Offline, the cached copy plays (a failed
  check is no error).
- A download that fails keeps its `.part` and resumes with a Range request next time.
- **Progressive start** (play while downloading) for MP4s whose `moov` box precedes the media data
  ("fast start"), where the decoders read a growing file: Linux (FFmpeg), Windows, Android 9+.
  Others (and moov-at-end files) play once downloaded. Make a file fast start with
  `ffmpeg -i in.mp4 -c copy -movflags +faststart out.mp4`, or the video asset Inspector's
  **Optimize for streaming** (no re-encoding).
- The least recently used files go past 512 MB; `VideoPlayer.clearCache([url])` deletes them.
- When the last player wanting an unfinished download goes, the download is cancelled.
- On the Web the browser streams the URL itself (no cache).
- Plain `http://` to hosts other than localhost needs Project Settings > Player > allow insecure HTTP.
- Android exports get the INTERNET permission when a scene's Video Player plays a URL.

## Recipes

- **Ad in the UI** (the porter's house ad): a Video Player (Render Texture, `unscaled_game_time`,
  `playOnAwake` off, Direct audio, no loop), a UI Image with texture `video|<id>`; script:
  `Http.download(url, "data://ads/ad1.mp4", ...)`, then `vp.url = "data://ads/ad1.mp4"`,
  `vp:prepare()`, play in `onPrepareCompleted`, close in `onLoopPointReached`.
- **TV screen mesh**: Material Override on the TV mesh, property `baseColorTexture` (or
  `emissiveTexture` for a glowing screen), loop on.
- **Menu background**: GameObject > Video > Video Player (Camera Background): far plane of the main
  camera, `fit_outside`, looping.
- **HTML UI**: `<video src="ui/intro.mp4" autoplay loop muted>` or `src="https://..."`;
  `element.videoBackend` and `inspect_ui`'s media `backend` say what decodes it;
  `canPlayType('video/mp4; codecs="avc1.42E01E, mp4a.40.2"')` answers for this platform
  (jasper://docs/html-ui).

## Performance

Hardware decode on Android, Apple and Windows; the planes are copied (and NV12 de-interleaved) on a
worker thread; the main thread uploads three planes and runs one conversion pass per new frame.
Budget: one 1080p30 or two 720p30 videos on a mid-range phone. The Inspector warns above two players
on a scene and above 1080p.
