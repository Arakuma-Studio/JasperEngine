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
