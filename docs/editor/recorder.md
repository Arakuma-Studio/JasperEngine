# Recorder

**Window > General > Recorder** records the Game view to a PNG sequence or a movie at a fixed
frame rate, like Unity Recorder (docs/specs/S27-editor-tools.md R24-R31).

## Settings

- **Recorder**: Image Sequence or Movie. **Capture**: Game View.
- **Output Resolution**: Match Game View, 480p, 720p, 1080p, 1440p, 2160p, or Custom (16-8192
  and at most the GPU's largest texture; movies round to even sizes).
- **Frame Rate**: 24, 25, 30, 50, 60 or Custom (1-240).
- **Recording Mode**: Manual (until you stop), Single Frame, Frame Interval [first, last] or
  Time Interval [start, end) seconds. Frame 0 is the first game frame after Play starts.
- **Exit Play Mode** when the recording ends; **Record on Play** starts recording whenever Play
  starts.
- **Output Folder** (default `<project>/Recordings`, left out of version control and exports)
  and **File Name** with `<Scene>`, `<Take>`, `<Date>`, `<Time>`, `<Resolution>` and, for
  sequences, `<Frame>` (4 digits from the first recorded frame). Defaults: `<Scene>_<Take>` and
  `<Scene>_<Take>/frame_<Frame>`.
- **Take** goes up after each recording.
- Movies: **Format** MP4 (H.264) or MOV (ProRes 422 HQ, when your FFmpeg has `prores_ks`),
  **Quality** Low/Medium/High (CRF 28/23/18 for libx264; a bitrate for hardware encoders),
  **Capture Audio**.

The settings are the project's (`.jasperengine/user/recorder.yaml`).

## FFmpeg

Movies are encoded by **your** FFmpeg: the editor pipes the frames to it and links no codec.
It uses `JASPER_FFMPEG`, Preferences > External Tools > FFmpeg, or `ffmpeg` on `PATH`. The
H.264 encoder is the first present of libx264, h264_videotoolbox, h264_mf, h264_nvenc, h264_qsv,
h264_amf and libopenh264. Without FFmpeg or an H.264 encoder Movie is disabled with the reason
(**Open Preferences...**, **Look Again**). Image sequences always work.

## Recording

- **Start Recording** (the window's button, the ● button next to Play, or F10) enters Play if
  needed. **Stop Recording**, F10, stopping Play or the end of the Recording Mode finishes it.
- While recording the game runs at exactly 1 / Frame Rate per frame, VSync is off and every
  editor frame renders one game frame, so a recording is the same however fast the computer is.
  Pause stops the capture; Step records one frame. The Game view shows "REC 00:12 · frame 734"
  (not in the recording).
- Frames are never dropped: when the encoder falls behind, the editor waits for it.
- **Capture Audio** takes the game's sound from the deterministic audio driver, in step with the frames (you hear nothing
  while it records; the window says so) and muxes it in at the end (AAC 192 kb/s in MP4, PCM in
  MOV). It needs the recording to start with Play; started mid-play, audio is off with a warning.
- No frame shows a stand-in: a frame drawn while a shader, pipeline, texture or model was still
  loading is drawn again with the game held (in game time: the recording's clock does not move)
  until one is drawn with everything ready. A load that finishes nothing for 120 s fails the
  recording, naming it. Objects that never report loaded are waited for 6 s once, with a warning.
- At the end the Console says "Recorded N frames to <path>", and `<output>.recording.json` holds
  `{frames, fps, width, height, encoder, scene, take, startFrame, warnings}`.

## When a recording fails

A recording that reports success is a good recording, and the output's name only ever holds a
complete one: a movie and its audio are written under hidden temporary names (`.<name>.<pid>-<n>.je-tmp`),
as are the frames of a sequence, and take their real names at the very end (a crash leaves those
hidden files, never a damaged movie under its name).

- **Start.** When the estimated size does not fit the free space the window and the Console warn
  ("This recording needs about 1.2 GB and the disk has 300 MB free"); it still starts. A recording
  with no set end is estimated by its first minute.
- **Every write is checked**: the audio chunks and the WAV header, the frames to the encoder, the
  encoder's and the mux's output, each PNG. The first failure stops the recording. Play goes on and
  your Exit Play Mode does not apply: only the recording ended.
- **The result** says what failed and why, in the window, the Console and `record_game`'s failed
  job: "Recording failed: the audio write failed: disk full, 3 MB free; 412 frames were kept as
  Recordings/Rec_004.partial.mp4 (no audio)". FFmpeg's last lines are in the Console.
- **What was recorded is kept only if it decodes**, as `<name>.partial.<ext>` (checked by decoding
  it: an MP4 FFmpeg could not finish has no index and is removed, which is what a disk that filled
  up under the video gives; an audio failure keeps the video, without sound). A sequence's frames
  before the failure go to `<folder>.partial/`, without gaps; the folder you chose is left as it was.

## From scripts and CI

There is no separate command: record through MCP's `record_game` with `call`:

```sh
jasperengine-editor call --project MyGame --wait-job record_game --frames 90 --format mp4 --output out.mp4
```

`record_game` takes `frames` or `seconds`, `start_frame`, `fps`, `width`/`height`, `format`
(png, mp4, mov), `quality`, `audio`, `output` and `exit_play`, and returns the sidecar with the
files (`jasper://docs/editor-tools`).
