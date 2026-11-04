# S62 — Recorder integrity: a recording that reports success is a good recording

**Depends on.** S27 (Recorder), BF14 (recorder audio), S32 (async pipelines). Lands before S35, which edits
`editor/recorder/RecorderSession.cpp` too. Touches `editor/recorder/`.

## 1. Goal
The Movie Recorder never reports success for a file with damaged audio or video, and never records frames drawn with
stand-ins for things still loading. A full disk today gives a movie with bad audio reported as a success
(`RecorderSession.cpp:239` ignores `wav.write`), and readback may catch frames before pipelines are ready, the bug
`PoseRender` had.

## 2. Requirements
- R1. Every write of the session (wav chunks, wav header rewrite at `:272`, video encoder input, mux output) is checked.
  The first failure stops the recording cleanly: Play continues, the encoder is closed, temp files are removed.
- R2. The result says what failed and why ("disk full, N MB free"), in the Recorder window, the Console and the MCP job
  result (`failed`, not `done`). The chosen output path gets no damaged file. What was recorded before the failure is kept
  as `<name>.partial.<ext>` only if it is decodable, and the result says so.
- R3. Final output is written with S03's atomic rule: it appears under its real name only once complete.
- R4. A free-space check at start warns when the estimated size does not fit (estimate shown); it does not refuse.
- R5. Check `RecorderSession` readback against the `PoseRender` bug with a red test: force pending pipelines or textures
  (the S32 test hooks), record, compare frames. Fix: a captured frame waits until the frame's GPU work is really ready, in
  game time, so a fixed-rate recording has no stand-in frames. If not reproduced, the test stays and Deviations says so.
- R6. Failure-injection hook for tests (environment variable, as other editor test hooks do); no cost when unset.

## 3. Acceptance
- AC1. Red-then-green test for R1/R2: injected write failure at audio and at video; job `failed`, no file at the output path.
- AC2. A real small-tmpfs run on a build server, or the injection plus a statement that tmpfs is unavailable.
- AC3. A normal recording's output is unchanged (frame count, audio length, hash of decoded audio against a reference).
- AC4. R5's test. `tools/check-changed` green, recorder MCP tests green. Docs updated.

## Deviations

- **R5 reproduced.** Readiness was read only when the frame was read back, after `Engine::updateGpuCreation` had already
  collected what finished, so the frame drawn before it counted as ready, and the check did not look at runtime shader
  builds or model loads. Red on the build before this change (`test_recorder_integrity.py`): a texture set at game frame 10
  recorded frame 10 with the old texture, and a material variant recorded frames 10 to 12 with the old look, with or without
  `JASPER_ASYNC_SHADERS_DELAY_MS`, and with `JASPER_ASYNC_SHADERS=sync`. A frame is now recorded only if everything was
  ready before it drew and after, nothing waited or stood in while it drew and nothing finished meanwhile (`FrameMarks`).
- **R5, a load past its limit.** The old wait recorded the stand-in after 30 s with a warning. A load that finishes nothing
  for 120 s (`PoseRender`'s limit) now fails the recording naming it, as a screenshot does. Only what never reports loaded
  (objects, not assets) is given up on, after 6 s, once, with a warning.
- **A frame that cannot be read back fails the recording** (it was a warning and a gap in the file). `failed_step` is
  `readback`; the wait's is `loading`.
- **R2, a sequence.** Its partial is the folder `<folder>.partial/`, with the frames before the failure and no gap. Sequence
  frames are written as hidden temporaries next to their names and renamed at the end, so a failed recording leaves a folder
  that already held a recording as it was.
- **R2, what a partial movie holds.** The video only ("no audio" in the result): the WAV is not trustworthy after a failed
  write, and muxing needs the disk that is full. On a really full disk the video has no index and nothing is kept.
- **R1, an audio file that cannot be opened** now refuses the start (it was a warning and a movie without sound).
- **R3, temporaries** are `io::makeTemporarySiblingPath` names, so FFmpeg is told the container (`-f`). Those a crash leaves in
  `Recordings/` are not swept: the asset database's walk that feeds the orphan sweep skips that folder.
- **R4, a recording with no set end** is estimated by its first minute; the estimate is only in the warning and the result
  (`estimated_bytes`, `free_bytes`), not a row of the window.
- **Outside `editor/recorder/`:** `Host::gameScene` (`App.cpp`), `Texture::getLoadingAnswerCount`, and `record_game`'s result
  (`failed_step`, `partial`, `partial_frames`; a failed write is `failed` even when the job was being cancelled).
- **Agent text:** what to do after a failed write is in the failed result's text, not the guide (of an error result, Claude
  Code shows the model only the text). Case: `guard_record_failed_write` in `tools/mcp/eval/cases.py`.
- **Not swept:** S66's `StagingFolders` owns folders through a lock file beside them; the Recorder's temporaries are single
  files named by pid, so it is no fit, and they stay until someone removes them.
