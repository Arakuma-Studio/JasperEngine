# S35 — Agent audio: deterministic audio driver, Play capture, "agents can hear", sound QA

**Wave / Depends on.** Sound wave A (spec-first: implemented after the integration branch lands in
`main`). Depends on **BF14** merged (the SoLoud, timing and listener fixes, §8) and on **S38 M1**
(`AudioMeter`, a small library that lands first). Milestone M4 (`check`) also needs **S36 M1**
(`AudioAnalysis`). Runs in parallel with S36, S37 and S38; S39 and S40 use its capture in their
tests. Owner area: `engine/core/audio/` (`AudioClock`, `AudioTap`, `AudioCapture`,
`AudioMarkers`), `engine/core/subsystem/AudioSystem.*`, `editor/audio/` (the session reader, QA,
timeline image, baselines), `editor/recorder/RecorderSession.cpp` (audio only),
`editor/window/AudioCapturesWindow.*`, `editor/mcp/tools/AudioSessionTools.cpp`,
`docs/mcp/guide/sound.md` (its Capture and QA sections).

## 1. Goal

An agent cannot hear, so it measures. A Play session (in the editor, or a `--headless` player)
can run on a **deterministic audio driver**: SoLoud's null driver, mixed once per game frame by
the game's own unscaled frame time, so game logic, voice events and every output sample are the
same on every run with the same `fixed_fps` and seed. That driver feeds one tap, which writes a
**capture**: the mixed output, every voice start and stop, mixer transitions and gameplay markers
on one sample clock, plus per-group levels. `audio_session` reads it back as numbers and a
timeline picture, and `check` turns it into PASS/FAIL/WARN/SKIP verdicts with the measured value,
the threshold and where it came from, what limits it and how to fix it. The editor Recorder's
movie audio becomes a client of the same tap. Everything is built on upstream's Audio Mixer
(groups, effects, snapshots, voice budget); nothing of it is replaced.

## 2. Scope

**In scope**
- The deterministic driver (`AudioBackend::Deterministic`): when it is used, its clock, where it
  is pumped, synchronous loads, the constant latency it reports.
- One output tap and its clients: the capture writer and the S27 Recorder (its offline path
  `beginOfflineCapture`/`mixOffline`/`endOfflineCapture` goes away).
- Captures: `master.wav`, `events.jsonl`, `levels.jsonl`, `session.json`, optional group stems;
  the capture stamping points in `AudioSystem`; gameplay markers; editor and player lifecycles.
- `audio_session` (`status`, `arm`, `report`, `check`), the `play stop` audio summary, the timeline
  image, baselines, the anti-gaming rules, the twelve checks re-designed on upstream's mixer.
- Lua `Audio.mark(label)` and `Audio.isCapturing()` (this spec creates the `Audio` table; S37
  adds playback to it).
- Window > Audio > Captures (the human view of a session and its `listen` moments).
- Development players: `--audio-capture <dir>`.
- Later milestones folded in from sound-layer phase 7 (§Milestones): none here; phase 7's review
  page and capture frames are dropped (§4.3).

**Out of scope**
- Bug fixes: B4–B13, S-1–S-7 and X-1 of the analyses are **BF14's** (§8); this spec assumes them.
- Hearing while capturing (sound-layer `AudioMonitor`, `AudioOutput`, P6-2): a capture mutes the
  device, as the Recorder does today (S27 R29).
- SoLoud voice taps, `mix_blocks.bin`, `voice_taps.bin`, `limiter.bin` (sound-layer G-6): the
  checks work from the master output, the groups' meter blocks and logged gains instead.
- The `audio-check` CLI (sound-layer P4-5): S31's `call` runs `audio_session` (R27).
- File analysis (`analyze_audio`, S36), loudness targets and the true-peak limiter (S38), Sound
  Event fields in the log (S37 adds them), music lines (S39), animation cue lines (S40).

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **Backends.** `AudioSystem::init` (`engine/core/subsystem/AudioSystem.cpp:121-175`) tries
  MINIAUDIO, then NOSOUND (a mixing thread on the wall clock, upstream A14), then NULLDRIVER.
  `platformConfig().silentAudio` (set by `--headless` and `--silent-audio`,
  `engine/core/player/Launch.cpp:62-63`) makes it return false: every source goes Stopped at once
  and logic diverges from a desktop run. This contradicts S30 R17 ("Audio uses the null backend").
  BF14 changes it to the NoSound path; S35 replaces that with the deterministic driver.
- **The null pump.** `AudioSystem::mixManually` (`AudioSystem.cpp:186-199`) mixes
  `(unsigned)(seconds × rate + 0.5)` frames per call (drift, analysis S-7) and is called at the end
  of `AudioMixer::update` (`engine/core/audio/AudioMixer.cpp:1126`), which `Engine::runFrame`
  calls **before** the scenes update (`engine/core/Engine.cpp:2183`): a voice started in frame k
  is first mixed in frame k+1.
- **The Recorder's offline audio** (S27 R29). `AudioSystem::beginOfflineCapture`
  (`AudioSystem.cpp:217-230`) de-inits and re-inits SoLoud with NULLDRIVER but never calls
  `AudioMixer::onAudioInit()`, so the mixer is not built (`AudioMixer::getBus` returns null while
  `!built`, `AudioMixer.cpp:1181-1187`) and every voice of a recorded movie bypasses the groups,
  their effects and faders (found while checking). **BF14 fixes it before main** (the user's
  decision of 2026-11-01), as it fixes prebuilt players ignoring the mixer (S38 §3); S35 assumes
  both fixes and still replaces this path with the deterministic driver (R9). `RecorderSession::mixAudio`
  (`editor/recorder/RecorderSession.cpp:230-240`) mixes per game frame with
  `recorder::audioFramesOf` (an exact `llround` accumulator, `RecorderSettings.cpp:323-326`), and
  `AudioMixer::update` mixes the same null driver again (analysis X-1; BF14).
- **What an agent can see today.** `get_play_state.audio` (`editor/mcp/tools/PlayTools.cpp:625`,
  `playAudioJson`): the voice count and the groups' peak meters above -60 dB. No event log, no
  sample clock, no loudness, no capture of a Play. `get_asset_info` probes WAV only
  (`editor/mcp/tools/AssetTools.cpp:497`, `wavInfo`).
- **Hooks that exist.** One voice start (`AudioSystem::startVoice`, `AudioSystem.cpp:657`), one
  stop (`stopVoice`, `:276`), one-shots (`playOneShot`/`startPendingOneShots`, `:697-750`), point
  voices (`playClipAtPoint`/`updatePointVoices`, `:761-830`), the budget steal inside
  `startVoice` (`:661-679`), fades (`:832-916`). Group meters: `MeterFilter` in each group bus's
  ninth slot (`engine/core/audio/AudioFilters.h:31-44`). Physics contacts reach the main thread
  as ordered records (`engine/core/util/JoltPhysicsAux.h:408-445`, `RawContact3D`).
- **No `Audio` Lua table** exists (`engine/core/script/binding/ECSClassesLua.cpp:434-500` binds
  `AudioSystem`, `AudioListener`, `AudioMixer*`), so the name is free.

## 4. Reference: sound-layer (`snd/sound-layer` 20b461161; checkout `je-wt/snd-ro`)

sound-layer is based on upstream adf45351d and conflicts with the mixer that landed since. Its
designs and code are source material; everything is re-hosted on upstream's `AudioSystem` and
`AudioMixer`.

### 4.1 Reused (adapted)

| What | Where (commit, file) | Change and why |
|---|---|---|
| Sample-cursor accumulator | 471dfd6ba `engine/core/audio/AudioClock.{h,cpp}` (87 lines) | As is, plus `reset(rate)`; it drives the deterministic driver instead of a second pump. |
| Deterministic mode, pump, synchronous loads, latency constant | P1 §2.2 (master §2 decision 3) | Pumped after the scenes' update, not from `Engine::systemDraw`; upstream's backends stay, Deterministic is added. |
| Capture writer: `master.wav`, `events.jsonl`, `session.json` | 471dfd6ba…6276defae `AudioCapture.{h,cpp}` (2,492 lines) | Format restarted at version 1 (`jasperengine.audio-capture`); bus fields become upstream group id and name; `voice_taps`, `mix_blocks`, `limiter.bin` dropped; `levels.jsonl` replaces bus band frames; writes through S03 temp + `publishFile`. |
| Stamping and stop reasons | P1 §2.3 `VoiceRequest`, `StopReason` | Added at upstream's existing choke points (`startVoice`, `stopVoice`, one-shot, point, steal); reasons extended with upstream's (`stolen_budget`, `fade_out`). |
| Gameplay markers | 751a3867c `AudioMarkers.{h,cpp}`, P4 §2.3 | Contacts come from physics-contacts' main-thread records, so `AudioCallQueue` (64e9f5e5f) is dropped; HTML UI and animation-event markers added. |
| `audio_session status/report/check` | `editor/mcp/tools/AudioTools.cpp` (0c0313ac9…d8785115e), `editor/audio/AudioQa.{h,cpp}` (fad196a52…b70aac593, 4,857 lines), `AudioTimeline.*`, `AudioTimelineImage.*`, `AudioBaseline.*` | Inputs re-derived from upstream's group meters and logged gains; mixer/bus status left to `audio_mixer get` (no duplication); schema trimmed to S31's 1,500 characters; `arm` replaces `play.audio_capture`. |
| Independent QA oracle | bcaa4b6db…b70aac593 `tools/mcp/audio_qa_ref.py` (1,903 lines), `audio_refs.py` | Ported with the bus model replaced by groups. |
| Captures view | P4 §2.9, `editor/window/CaptureTimelineView.*` (905 lines) | Re-hosted as its own window (the Sound Browser is S37's); commands through S25. |
| Agent rules | P4 §2.7, §2.12 (never loosen thresholds, two fix rounds, report numbers, `listen`) | Text in `jasper://docs/sound`; the timestamped threshold audit becomes a diff against defaults (D8). |
| Tests | `test_audio_capture`, `_session`, `_markers`, `_qa*`, `_play`, `_goal` (category E of the editor analysis) | Rewritten on the new formats and fixtures regenerated on target (`tools/fixtures/audio-qa`, `audio-markers`). |

### 4.2 Changed on purpose

- **No `play.audio_capture` parameter.** `play` is a core tool at its grandfathered limit
  (`tools/mcp/budget.json` `play` 1709); `audio_session {action:"arm"}` (group `audio`) does it
  at zero core cost (D10).
- **Capture follows upstream's groups, not buses.** Per-group levels come from the existing
  `MeterFilter` slot, so no new SoLoud filter slot and no vendored tap patch are needed (D5).
- **One pump, one tap.** The Recorder and the capture share the deterministic driver's output,
  so neither X-1's double mix nor an unbuilt mixer (both fixed by BF14) can come back: there is no
  second pump and only one init path (D3).

### 4.3 Dropped

`AudioMonitor`/`AudioOutput` (no live hearing during capture), the voice/duck taps and their
SoLoud patches (032ebeca0, a571175de, f32ae9449), `limiter.bin`, `AudioCallQueue`, the
`audio-check` CLI (`AudioCheckCli.*`), the Captures tab inside the Sound Browser, sound-layer's
`AudioSettings.qa*` fields and `AudioSettingsCmd` audit log, the static review page (P7-4) and
game frames in captures (P7-5; the Recorder records the picture), the `--headless` NoSound path
of BF14 (replaced, R4).

## 5. Requirements

### Functional — the deterministic driver

- **R1. Backend.** `AudioBackend` (`engine/core/subsystem/AudioSystem.h`) gains `Deterministic`.
  `AudioSystem::init` opens it as `Soloud::init(flags, NULLDRIVER, 48000, 512, 2)` through the
  same path as every backend, including `setGlobalVolume` and `AudioMixer::onAudioInit()`, so the
  mixer's groups, effects and faders are built. `AudioSystem::getOutputMode()` returns `device`,
  `no_output` (upstream's NoSound or Null fallback) or `deterministic`; MCP and logs spell these.
  The log line is `Audio: deterministic driver (48000 Hz, game-time pump)`.
- **R2. Clock.** `engine/core/audio/AudioClock.{h,cpp}`: `reset(rate)`, `advance(seconds)` adds
  to an accumulator and returns `n = llround(accumulated × rate) − cursor` frames, then moves
  `cursor` by `n`; `cursor()`, `seconds()`. The pump mixes `n` frames in chunks of at most 512
  (`Soloud::mix`). With `fixed_fps 60` at 48 kHz every frame mixes exactly 800 frames.
- **R3. Pump point.** In Deterministic mode the mix runs once per engine frame **after** every
  scene's update and before rendering (`Engine::runFrame`), with the frame's unscaled delta as
  the double `runFrame` computes (the value behind `Engine::getUnscaledDeltatime()`, whose float
  would drift), which is 0 while the app is paused, the simulation held or the editor's Play
  paused. A voice started in frame k is in frame k's mix. `AudioMixer::update` does not mix in
  this mode; it still mixes upstream's Null fallback as BF14 leaves it (BF14
  fixes that pump's S-7 drift; it may share `AudioClock`). An editor Step mixes exactly one frame.
- **R4. When it is used.**
  - A player run with `--headless` or `--silent-audio` (S30 R17), in every flavour.
  - An editor Play session that captures (R10): by default when the editor has no output device,
    or when armed (R19).
  - A Recorder movie with Capture Audio (S27 R29).
  - `JASPERENGINE_AUDIO_BACKEND=deterministic` and `AudioSystem::setBackend(Deterministic)`
    (tests).

  Everywhere else upstream's selection (device, then NoSound, then Null) is unchanged.
- **R5. Switching.** Entering or leaving Deterministic re-initialises SoLoud only at Play start
  and stop and at a Recorder start and end: voices of the edit scene stop (the Inspector preview
  is stopped and disabled while a capture runs, tooltip "Audio preview is off while Play captures
  audio"). The mode is never switched during a Play.
- **R6. Synchronous loads.** While Deterministic, `SoundPool::loadClip` loads synchronously even
  when `Engine::isAsyncLoading()` is true, so a start never waits a variable number of frames. A
  start that waited anyway logs `load_delay` (frames) and makes the session not deterministic.
- **R7. Clocked voices.** In Deterministic mode `enableClocked` voices start unclocked at the
  frame's first sample (every start of a frame is queued before that frame's mix); the clock time
  passed to `AudioMixer::play` is `AudioClock::seconds()`. Device mode is BF14's (B9).
- **R8. The guarantee.** The same project, scene, `fixed_fps`, `audio.seed` (R33) and input give
  byte-identical `master.wav`, `events.jsonl` and `levels.jsonl` on every run and on every build
  of the same engine version for one platform. Only `session.json`'s `started_utc`/`ended_utc` and
  the session id differ. `latency_samples` (the frames between a voice's stamp and its first
  sample in the mix) is a constant measured once by a test (sound-layer's probe B5 found 1, plus
  any lookahead of a Master limiter, S38) and reported in every session.
- **R9. One tap.** `engine/core/audio/AudioTap.{h,cpp}`: clients subscribe with
  `subscribe(name, fn(const float* interleaved, uint32_t frames, uint64_t firstFrame))` and are
  called after each chunk the deterministic pump mixes, in subscription order, on the main
  thread. The capture writer and the Recorder are its clients.
  - `RecorderSession` subscribes instead of calling `beginOfflineCapture`/`mixOffline`/
    `endOfflineCapture`, which are deleted. It still writes 16-bit PCM and muxes as S27 R29 says;
    per game frame it receives exactly `audioFramesOf(frame, frameRate)` frames (both are the same
    `llround` accumulator; a unit test holds them equal for 24, 30, 60 and 144 fps).
  - A Recorder movie and an S35 capture may run in the same Play.

### Functional — captures

- **R10. Editor lifecycle.** A capture begins in `Project::start` after the play scenes are
  loaded and before the game's first events (`Engine::pauseGameEvents(false)`,
  `Engine::onViewLoaded`), so voices started by scripts' `init` are captured and no game frame
  runs before sample 0. It ends in `Project::finalizeStop` after every played scene's sounds are
  stopped, so their `voice_stop` lines are written, and before the edit-time audio is restored
  (`Project::resetPlayAudio`, `editor/Project.cpp:3061`).
  - Directory `<project>/.jasperengine/audio/captures/<id>/`, id `YYYYMMDD-HHMMSS-NN` (UTC, `NN`
    a two-digit counter). Before creating one, the oldest sessions are deleted so at most
    `audio.capture_keep` remain (only names matching the pattern).
  - After `audio.capture_max_seconds` of audio, `master.wav` and the levels stop growing; events
    keep being logged; `session.json` gets `truncated: true`.
  - A capture that cannot write fails with an error that reaches `play start`'s result and the
    Console; Play continues uncaptured (still deterministic when no device).
  - A session left `capturing` with no live capture (editor crash) reads as `aborted`.
- **R11. `master.wav`.** RIFF/WAVE, `WAVE_FORMAT_IEEE_FLOAT`, a `fact` chunk, interleaved float32
  stereo at 48 kHz: exactly the buffers the pump mixed from sample 0. Written to a temporary
  sibling (`io::makeTemporarySiblingPath`) whose RIFF, `fact` and `data` sizes are rewritten after
  every second of audio, so a reader (and a crashed session) sees a valid WAV up to the last
  flush; published with `io::publishFile` at the end (S03; `tools/raw_writes_allowlist.txt` gets
  the stream writer's line with its reason).
- **R12. `events.jsonl`.** One JSON object per line, LF, in stamping order (`sample` never
  decreases), flushed every frame, at most 100,000 lines (then counted in `events_dropped`). The
  first line is `{"format":"jasperengine.audio-events","version":1}`.
  - Every line: `n` (1-based), `kind`, `sample` (cursor when queued), `t` (s), `frame` (pump
    frames since begin), `game_time` (s since begin).
  - Voice lines (`voice_start`, `voice_stop`, `voice_pause`, `voice_resume`, `voice_reject`):
    `voice` (SoLoud handle), `clip` (project-relative), `entity` (hierarchy path, the same
    `EntityPath` every MCP tool uses), `entity_id`, `scene`, `group` (name) and `group_id`, and
    `owner` (`main`, `one_shot`, `point`; S37 adds `free`, S39 `music`, S40 `cue`).
  - `voice_start` adds `gain`, `gain_db`, `pitch`, `pan`, `looping`, `spatial`, `position`
    (spatial), `priority`, `cause` (`play`, `play_on_awake`, `resume_restart`, `one_shot`,
    `point`; later specs add theirs), `delay_samples` (> 0 only), `load_delay` (> 0 only).
  - `voice_stop` adds `reason` (`stop`, `ended`, `replaced`, `clip_changed`, `disabled`,
    `removed`, `scene`, `stop_all`, `stolen_budget`, `stolen_source` (upstream's per-source
    `maxVoices`), `fade_out`) and `played_s`. `voice_reject` adds `reason` (`budget`: the new voice
    was the least important, upstream A11).
  - `mixer` lines: `action` `transition` (`snapshot` or `snapshots` + `weights`, `seconds`,
    `unscaled`), `set_float`/`clear_float` (`name`, `value`, `seconds`), `start_snapshot`; from
    `AudioMixer::transitionTo`, `transitionToSnapshots`, `setFloat`, `clearFloat` and the load of
    the start snapshot.
  - `marker` (`label` ≤ 200 bytes, `source` `lua`|`cpp`), `input` (`device` key/mouse/touch/
    gamepad, `action` down/up, `code`, `x`, `y`; from the engine's input handlers, so `send_input`
    and real input are both marked), and the gameplay markers of R14.
  - Unknown kinds and fields are ignored by readers (later specs add kinds within version 1).
- **R13. Stamping points.** `AudioSystem::startVoice`, `stopVoice`, the one-shot and point-voice
  paths, the budget steal, the per-source steal, fades that end in a stop, `stopSceneSounds` and
  `stopAll` each write their line through one function. With no capture active each site costs
  one relaxed atomic load (`AudioCapture::isActive()`); in a release player the sites compile to
  nothing (R30).
- **R14. Gameplay markers** (sound-layer P4 §2.3.1, on target's hooks; label in brackets):
  - `collision_2d`, `sensor_2d` from `PhysicsSystem::beginContact2D`/`hitContact2D` and
    `beginSensorContact2D` dispatch (`speed` from `hitContact2D`'s approach speed when present)
    [`collision_2d Ball#2 ~ Floor`];
  - `collision_3d`, `sensor_3d` from the main-thread dispatch of `RawContact3D` `Added` records
    (`speed` = `max(0, (velocity1 − velocity2) · normal)` of the record), one per body pair per
    physics step;
  - `ui_press` (`ButtonComponent::onPress`), `ui_click` (`UIComponent::onClick`), `ui_hover`
    (`onPointerEnter`), each just before the call, with canvas `x`, `y`;
  - `html_click` for an HTML UI document element click (the document's event dispatch), with the
    element's id or tag path;
  - `animation_event` when an Animation Player or Animator dispatches an `AnimationEvent`
    (`function`, `state`, `time`);
  - `spawn` and `destroy` (`ObjectManager` instantiate and destroy, with the prefab path and GUID
    when there is one), `scene_load` (`mode` `start`|`load`|`add`; the session's first line is the
    start scene's).

  Each site is `if (AudioMarkers::isEnabled())`, enabled only between capture begin and end.
- **R15. `levels.jsonl`.** Every 100 ms of audio (4,800 frames) one line `{sample, t, master:
  {peak_db, rms_db, bands_db[10]}, groups: {<name>: {peak_db, rms_db, bands_db[10]}}, voices:
  [[voice, level_db], …]}`. Group levels are the blocks each group's `MeterFilter` sees (after
  the group's effects), plus the group fader's current dB, so they are post-fader; bands are
  `AudioMeter`'s 10-band octave bank (31.5 Hz–16 kHz, S38); voice levels are SoLoud's
  `getOverallVolume` in dB (3D attenuation included). The `MeterFilter` capture hook is active
  only during a deterministic capture (main-thread mix), so it needs no lock.
- **R16. Stems.** With `audio.capture_stems`, each group's post-fader blocks are also written as
  `groups/<name>.wav` (float32, R11 rules).
- **R17. `session.json`.** Written at begin (`state: "capturing"`) and end, atomically:
  `format: "jasperengine.audio-capture"`, `version: 1`, `session`, `state`
  (`capturing`|`complete`; `aborted` on read), `project`, `scene`, `started_utc`, `ended_utc`,
  `engine_version`, `sample_rate`, `channels`, `buffer_size`, `latency_samples`, `driver`
  (`deterministic`), `fixed_fps` (0 = real time), `seed`, `deterministic` (every frame had a fixed
  delta and no voice had a load delay), `not_deterministic[]` (reasons), `frames`, `samples`,
  `duration_s`, `captured_samples`, `truncated`, `max_seconds`, `files` (`master`, `events`,
  `levels`, `stems[]`), `mixer` (path and group id → name map at begin), `output_stage` (S38: the
  clipper and post-clip scaler), `counts` (per line kind), `events_dropped`, `voices_peak`,
  `warnings[]`. Loudness is not stored: readers measure `master.wav` (R20).
- **R18. Player capture** (development players, `JASPERENGINE_AUDIO_CAPTURE` on, R30).
  `--audio-capture <dir>` (S30 R17's list) runs the deterministic driver and writes one session
  into `<dir>/<id>/` from the start scene's first frame to exit. A release player rejects the
  flag with exit 2 and `--audio-capture needs a development player`. `audio_session` and S36's
  `analyze_audio` accept a session directory path (`session: "/abs/path/<id>"`).

### Functional — reading captures

- **R19. `audio_session`** (group `audio`, activity Read, allowed while playing). Actions:
  - `status`: output mode, backend, whether a capture runs, its id, cursor and duration, live
    voices, whether the next Play is armed, and the sessions newest first (`{session, scene,
    state, started_utc, duration_s, deterministic, events, markers}`). Mixer and group levels are
    not repeated (`audio_mixer get` and `get_play_state.audio` have them).
  - `arm` (`capture` true/false, `max_seconds`, `stems`): applies to the next `play start` or
    `restart` in this editor, by MCP, CLI or toolbar; consumed by it. Without an arm, a Play
    captures when the editor has no output device.
  - `report` (`session`, `start`, `end`, `events_limit` 0–2,000 default 200, `image`,
    `max_width` 256–2,048): `capture{…}`, `range`, `loudness{integrated_lufs, short_term_max,
    momentary_max, lra}` and `peak{sample_dbfs, true_peak_dbtp, clipped}` measured on `master.wav`
    over the range with `AudioMeter` (stream mode), `groups{<name>: {peak_db, rms_db,
    voices_peak}}` from `levels.jsonl`, `events[]` and `markers[]` numbered `E<n>`/`M<n>` as in
    the image, totals and truncation flags, `voices{started, stopped, stolen, rejected, peak,
    by_clip[]}`, `mixer[]`, and `guidance`; plus the timeline image (R22).
  - `check` (R21–R25).

  The text summary is one line per item; images go out as MCP images (S31 R20). `report` and
  `check` read on an S06 Background job and become `McpJobs` jobs when they take more than 1 s.
- **R20. `play stop`.** The stop result's data gains `audio: {session, duration_s,
  integrated_lufs, momentary_max, true_peak_dbtp, voices_started, voices_peak, markers,
  warnings[], next: "audio_session report"}` and one text line; absent without a capture. (Result
  only: the listed schema is unchanged.)
- **R21. `check`.** Inputs: `checks[]` (default all, always output in the order below), `expect[]`
  (≤ 64 `{on, entity, sound, within_ms 0–500, offset_ms −10,000–10,000, align onset|peak|end}`),
  `baseline`, `save_baseline`, `fail_on` (`fail`|`warn`). Every check entry has exactly `id`,
  `kind` (`mixer`: guaranteed by the engine; `content`: the sound or mix), `status`
  (PASS|FAIL|WARN|SKIP), `round`, `measured`, `threshold`, `threshold_source` (an `audio.*` key,
  the session, or `expect[i].within_ms`), `limited_by` (a sentence; null on PASS/SKIP), `at[]`
  (≤ 10 times), `fix_hint`, `method`. Verdict: FAIL if any FAILs, else WARN, else PASS if any
  PASS, else SKIP. The result is also written to `<session>/check.json`.

  | id | kind | rule (thresholds are `audio.qa.*` unless named) |
  |---|---|---|
  | `loudness` | content | integrated LUFS of the range within `audio.loudness_target_lufs` ± `audio.loudness_tolerance_lu` (S38); SKIP below `loudness_min_seconds` of gated audio |
  | `true_peak` | mixer | true peak ≤ `audio.true_peak_ceiling_dbtp` (S38) + 0.1 dB; WARN when the Master limiter's gain reduction (S38 telemetry, `limiter` lines) exceeds `limiter_warn_db` ("mix too hot") |
  | `clipping` | mixer | no output sample with \|x\| ≥ 1 − 2⁻¹⁵ |
  | `coverage` | content | each marker matching an `expect` rule has a matching `voice_start` within `within_ms` (default `sync_tolerance_ms`) of the aligned point; without rules, WARN when a marker class (kind + entity) is sounded on some occurrences and not on others |
  | `sync` | content | audible onset − stamp − `latency_samples` − `delay_samples` ≤ `head_silence_max_ms` for every matched start (onset: the first sample where the master rises `audibility_margin_db` over the loudest 1 ms of the previous 50 ms) |
  | `audibility` | content | per started voice, its predicted band levels (the clip's analysis bands from S36 with its import gain, plus the logged `gain_db`) against the other groups' bands over its first 500 ms: margin ≥ `audibility_margin_db` in the clip's dominant band; `limited_by` names the masking group and band; a suggested gain |
  | `voice_budget` | mixer | WARN when peak voices > `audio.max_voices` × `voice_budget_warn`; FAIL when a voice with `priority` ≤ `important_priority` was stolen or rejected |
  | `ducking` | content | for each group with a `duck` effect: the measured drop while its side-chain group is above −60 dB is at least −20 log10(`level`) − 3 dB; WARN when the side-chain group is not `voice_over_music_db` above the ducked one |
  | `silence` | content | after the first music voice (group `music_group`, or S39's music owner), no gap below −60 dBFS longer than `max_silence_seconds` |
  | `load_delay` | mixer | WARN for every voice that waited for loading |
  | `repetition` | content | WARN when one source or event plays the same clip ≥ `repeat_min_plays` times in a row within `repeat_window_s` |
  | `regression` | content | against a baseline: matched starts within `regression_timing_ms`, group bands within `regression_band_db`; SKIP without `baseline` |

- **R22. Timeline image.** Lanes top to bottom: title, master waveform, spectrogram, loudness
  (momentary and short-term, the target line), one lane per group with level above −60 dB, markers
  (`M<n>`), events (`E<n>` starts and stops), and for `check` the failures in red and the
  `listen` moments; every image exactly `max_width` wide; labels from an embedded public-domain
  8×8 font (sound-layer `AudioImages`, shared with S36).
- **R23. Anti-gaming.** The result carries `thresholds{values, changed[]}`: every `audio.qa.*`,
  `audio.loudness_*` and `audio.true_peak_ceiling_dbtp` that differs from its registry default,
  with both values. `round` counts consecutive FAILs of a check on the same scene in this editor
  session; at `fix_rounds` the guidance says to stop and report. `guidance` always ends with
  "Report these numbers; never claim a sound plays without a capture." `listen` lists 3–5
  `{t, why}` (first FAIL per check, the worst audibility margin, the loudest moment, the first
  start of each new clip). A check never changes a sound or a mix; it suggests.
- **R24. Exit codes.** With `fail_on`, a verdict at or above it returns the result with
  `isError: true`, so `jasperengine-editor call audio_session …` exits 1 (S31 R6); the guide shows
  the `batch` recipe (play start with `fixed_fps`, wait, play stop, check) for CI.
- **R25. Baselines.** `save_baseline: "<name>"` writes `baselines/audio/<name>.json`
  (`jasperengine.audio-baseline` v1: the matched starts, their onsets and the group bands per
  marker) through `WriteFilesCmd` (one undo step, S24); `baseline: "<name>"` or a session reference
  compares against it.

### Platform & backends

- **R26.** The deterministic driver needs no thread and runs on every target, including the web
  without pthreads. Desktop players support `--audio-capture`; mobile and web players do not
  (no writable path the harness reads; the flag is rejected with exit 2).

### Performance budgets (server F, Release, `fixed_fps 60`, 48 kHz)

- **R27.** Deterministic pump without capture: ≤ 0.6 ms per frame for 32 voices through 4 groups
  with one reverb (SoLoud's own mix cost; measured before and after, no more than 5 % above the
  upstream Null pump on the same scene).
- **R28.** Capture on top: ≤ 0.4 ms per frame (lines, levels, WAV writes buffered; disk flushes
  once per second). With capture off: no allocation and one relaxed load per stamping site
  (`AudioCaptureTest.OffPathAllocatesNothing` with the core tests' allocation counter, the
  `--wrap=malloc` one of `engine/tests/core/CMakeLists.txt`).
- **R29.** `report` on a 60 s session ≤ 1.0 s, `check` (all twelve) ≤ 2.5 s, on one Background
  worker.

### Editor UX (human)

- **R30.** Window > Audio > Captures (`window.audioCaptures`, S25 CommandCatalog, group Window):
  the session list (newest first, state, scene, duration, verdict of the last check), and for the
  selected one the same lanes as R22 drawn live (scroll, zoom), check results in
  `Theme::Colors::ErrorText`, and when a `check` produced `listen` moments a banner "Your agent
  asked you to listen to these moments" with one row per moment; clicking a row (or Enter) plays
  `master.wav` from t − 0.5 s to t + 1.5 s through an editor preview voice outside the mixer
  (`audioCaptures.playMoment`, `audioCaptures.stop` Escape, context `AudioCaptures`). Disabled while a
  capture runs.
- **R31.** While a capture runs, the Game view shows the banner "Capturing audio: output muted"
  (the Recorder's banner mechanism); the toolbar Play of an editor with a device does not capture
  unless armed.

### Agent UX (MCP, Lua, CLI)

- **R32. Lua.** A new global table `Audio` (`engine/core/script/binding/AudioClassesLua.cpp`):
  `Audio.mark(label)` writes a `marker` line during a capture and does nothing otherwise;
  `Audio.isCapturing()` → boolean. Both exist in every build (release: no-op and false), so a
  script behaves the same everywhere. Parsed by `tools/lua_api` (S23 R29) into `lua-api.json`,
  the LuaLS stubs and suggestions; documented in `docs/mcp/guide/lua-api.md` (Audio section, with
  a ```` ```lua run ```` block for the doc test, S23 R29a).
- **R33. Settings** (S25 SettingsRegistry, Project Settings > Audio, project.yaml `audio:` node,
  written only when not default): see §6.3.
- **R34. Docs.** `docs/mcp/guide/sound.md` (`jasper://docs/sound`, created by whichever of
  S35/S36 lands first) gets sections "Capture a Play", "Checks", "Rules", "Hand over to a human";
  `docs/mcp/guide/README.md` lists it; the instructions' Sound line becomes the one of §6.1.

### Data & compatibility

- **R35.** No project file changes: old projects, scenes and the `.mixer` load and save
  byte-identically; settings keys are written only when changed. Captures live under
  `.jasperengine/` (already ignored by version control). Release players: no size change (§6.6).

## 6. Design constraints & decisions

- **D1. Deterministic for headless, agents and captures; upstream's selection elsewhere** (the
  user's decision of 2026-11-01, resolving the analysis' D1). A human editor with speakers keeps
  hearing the device; a no-device editor keeps NoSound in edit mode (the Inspector preview and the
  Mixer window's meters still work there), and switches only for a Play that captures (R4, R5).
- **D2. The pump runs after the scenes.** Upstream mixes Null before the scenes (`Engine.cpp:2183`),
  which delays every start by one frame; mixing after them puts a voice, its trigger and its marker
  on the same sample and frame.
- **D3. One tap.** The Recorder and the capture read the same chunks, so a movie's audio and a
  capture of the same Play are the same samples (AC6), and no second pump can exist.
- **D4. No vendored SoLoud changes in S35.** Constant latency relies on BF14's S-5 (last block)
  and S-6 (bus read-ahead) fixes; S35's tests measure it.
- **D5. Group levels from `MeterFilter`.** It already sits after each group's effects in every
  bus (`AudioFilters.h:31-44`); adding a capture hook there gives post-effect group signals with
  no new slot (`FILTERS_PER_STREAM` stays 9).
- **D6. Capture is compiled out of release players.** `JASPERENGINE_AUDIO_CAPTURE` (CMake option)
  is ON for the editor, the test targets and development players, OFF for release players.
  `AudioCapture.cpp`, `AudioMarkers.cpp` and the stamping sites are under it; `Audio.mark` and
  `Audio.isCapturing` stay as stubs. The deterministic driver and `AudioClock` are always built
  (`--headless` needs them).
- **D7. JSON Lines, not binary.** Events and levels are JSONL (diffable, read by Python oracles
  with no format code); 100 ms levels for 120 s are about 1,200 lines.
- **D8. The threshold audit is a diff.** `thresholds.changed` compares with the registry defaults;
  version control has the history. sound-layer's timestamped `AudioSettingsCmd` log is dropped.
- **D9. No hearing during capture** (as S27 R29).
- **D10. `arm` instead of a `play` parameter**: zero growth of the core listing (§6.1).
- **D11. Sessions are local scratch.** `.jasperengine/audio/captures/` with a keep count; a
  baseline the user wants to keep is a project file (`baselines/audio/`).

API sketch:

```cpp
namespace jasperengine {
  class AudioClock { public: void reset(unsigned int rate); uint32_t advance(double seconds);
                     uint64_t cursor() const; double seconds() const; };
  class AudioTap { public: using Fn = std::function<void(const float*, uint32_t frames, uint64_t first)>;
                   static int subscribe(std::string name, Fn fn); static void unsubscribe(int id); };
  struct AudioCaptureOptions { std::filesystem::path root; std::string project, scene;
                               int maxSeconds = 120, keep = 3; bool stems = false; uint32_t seed = 1; double fixedFps = 0; };
  class AudioCapture {   // JASPERENGINE_AUDIO_CAPTURE; stubs otherwise
  public: static bool begin(const AudioCaptureOptions&, std::string* error); static void end();
          static bool isActive(); static std::filesystem::path lastSession();
          static void mark(std::string_view label, bool fromLua); };
}
// AudioSystem additions: enum class AudioOutputMode { Device, NoOutput, Deterministic };
// static AudioOutputMode getOutputMode(); static void pumpDeterministic(double unscaledSeconds);
```

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `audio_session` | `audio` | new: `action` status/arm/report/check, `session`, `start`, `end`, `events_limit`, `image`, `max_width`, `capture`, `max_seconds`, `stems`, `checks[]`, `expect[]`, `baseline`, `save_baseline`, `fail_on`; per-check prose and enum meanings in `jasper://docs/sound` | ≤ 1,500 (sound-layer's was ~2,250; the trim moves the check list and `expect.on` vocabulary to the guide) |
| `play` | core | none (result gains `audio`, R20) | 0 |
| `get_play_state` | core | none (result unchanged) | 0 |

- Group `audio` after all of S35–S40 (budget.json `group_default` 15,000 chars, 12 tools):
  `audio_mixer` (upstream, ~900, `editor/mcp/tools/AudioTools.cpp:181-203`), `audio_session`
  1,500, S36's `analyze_audio` 1,400, `generate_sound` 1,450, `search_sounds` 1,450,
  `set_audio_import_settings` 1,100, S37's `edit_sound_event` 1,400: **7 tools, ≈ 9,200 chars**.
  No `budget.json` exception is needed. The group description becomes "Audio Mixer, sound
  analysis, placeholders, sound search, Sound Events, Play capture and checks." (written by the
  first of S35/S36/S37 to land).
- Core listing: +0 characters from S35. The guide's Sound line becomes "Sound: audio_mixer
  (jasper://docs/audio); analysis, placeholders, search, Sound Events, capture and checks: group
  audio (jasper://docs/sound)." (+~110 bytes once, shared with S36/S37; the guide stays ≤ 16,384
  bytes with its 1,500 bytes of headroom, `budget.json` `guide`).
- Conventions (S31 R20): snake_case, `additionalProperties:false` on `expect` items, `anyOf` not
  `oneOf`, the AC10 hidden-by-default assertion, `test_tool_coverage.py`, `gen_tool_docs.py`.

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `<session>/session.json` | `jasperengine.audio-capture` v1 | own JSON writer/reader (`AudioCapture`, `editor/audio/AudioSessionReader`), unknown keys ignored |
| `<session>/events.jsonl` | header line `jasperengine.audio-events` v1 | JSON Lines; kinds and fields additive within v1 (S37, S39, S40 add) |
| `<session>/levels.jsonl` | `jasperengine.audio-levels` v1 (header line) | JSON Lines |
| `<session>/master.wav`, `groups/*.wav` | RIFF WAVE IEEE float32, `fact` | stream writer (R11) |
| `<session>/check.json` | `jasperengine.audio-check` v1 | `nlohmann::json`, written atomically |
| `baselines/audio/<name>.json` (project) | `jasperengine.audio-baseline` v1 | `nlohmann::json`, `WriteFilesCmd`; not an S04 asset (outside the assets folder) |

None is an S08 component codec: no component changes in S35.

### 6.3 Settings (S25 SettingsRegistry; Project scope, page Audio, subsection Capture and QA)

| Key | Type, range | Default |
|---|---|---|
| `audio.capture_max_seconds` | int 1–3,600 | 120 |
| `audio.capture_keep` | int 1–20 | 3 |
| `audio.capture_stems` | bool | false |
| `audio.seed` | int 0–2³¹−1 (the seed of deterministic runs; S37's Sound Event RNG reads it) | 1 |
| `audio.qa.head_silence_max_ms` | float 0–500 | 15 |
| `audio.qa.sync_tolerance_ms` | float 0–500 | 20 |
| `audio.qa.audibility_margin_db` | float 0–24 | 3 |
| `audio.qa.voice_budget_warn` | float 0.1–1 | 0.8 |
| `audio.qa.important_priority` | int 0–256 | 55 |
| `audio.qa.voice_over_music_db` | float 0–40 | 10 |
| `audio.qa.max_silence_seconds` | float 0.1–60 | 2 |
| `audio.qa.music_group` | string (a group name) | `Music` |
| `audio.qa.loudness_min_seconds` | float 1–600 | 10 |
| `audio.qa.limiter_warn_db` | float 0–24 | 6 |
| `audio.qa.fix_rounds` | int 1–10 | 2 |
| `audio.qa.repeat_min_plays` | int 2–32 | 4 |
| `audio.qa.repeat_window_s` | float 0.5–60 | 4 |
| `audio.qa.regression_timing_ms` | float 0–100 | 5 |
| `audio.qa.regression_band_db` | float 0–12 | 1 |

The loudness and ceiling keys the checks read are S38's. The page's help says "Changing these
does not fix audio; checks report changed thresholds."

### 6.4 Editor commands (S25 CommandCatalog)

| Id | Name | Context, default key |
|---|---|---|
| `window.audioCaptures` | Audio Captures | menu Window > Audio |
| `audioCaptures.playMoment` | Play Moment | `AudioCaptures` (new `ShortcutContext`), Enter |
| `audioCaptures.stop` | Stop | `AudioCaptures`, Escape |
| `audioCaptures.nextMoment` / `previousMoment` | Next / Previous Moment | `AudioCaptures`, Down / Up |

`tools/ci/check-hardcoded-shortcuts.py` must pass (no `IsKeyPressed` in the window).

### 6.5 Lua API (S23 `tools/lua_api`)

`Audio.mark(label: string)`, `Audio.isCapturing(): boolean`. No change to `AudioSystem`,
`AudioListener`, `AudioMixer`, `AudioSource`.

### 6.6 Player size (S34, oe-port ceda68ed3)

- Release players: the deterministic driver and `AudioClock` (always built) and the two Lua stubs,
  estimated ≤ 3 KB of code on Linux, ≤ 2 KB of wasm (gzip); everything else is under
  `JASPERENGINE_AUDIO_CAPTURE`, off in release players. Measured with S34's
  `tools/players/player_sizes.py` and `lib_size.py --modules` before and after (AC14).
- Development players grow by the capture (estimated 60–90 KB); S34 R4 keeps the development
  flavour full, so no variant changes.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R2, R3) | `engine/tests/core/audio/AudioClockTest.cpp`: 144 fps at 44.1 kHz for 60 s mixes exactly `llround(60 × 44100)` frames; 60 fps at 48 kHz gives 800 per frame; zero and negative deltas mix 0. `DeterministicDriverTest.StartIsInItsFrame`: a voice started in frame k has its first non-zero sample in frame k's chunk at `latency_samples`. |
| AC2 (R1, R5) | `DeterministicDriverTest.MixerIsBuilt`: under Deterministic a tone through group Music with a low-pass at 200 Hz reads > 12 dB below the bypassed tone on `getMeterDb("Music")` (the mixer is built through the one init path). After BF14 this also holds for `beginOfflineCapture`, so the test guards against a regression when R9 deletes that path. |
| AC3 (R4, S30 R17) | `tools/ci/test_player_cli.py::test_headless_audio_deterministic`: a fixture player with a looping source and a Lua `onStop` counter, `--headless --fixed-fps 60 --quit-after 300 --audio-capture` twice: identical SHA-1 of `master.wav`, `events.jsonl`, `levels.jsonl`; the Lua log shows `isPlaying` true at frame 60 (red on the base: sources go Stopped under `--headless`). |
| AC4 (R8) | `tools/mcp/tests/test_audio_capture.py::test_deterministic_twice`: the `audio-qa` fixture's "Jump" scene, `play start fixed_fps 60`, `send_input` space at frames 30 and 90, stop at frame 180, twice: identical file hashes; `latency_samples` equals the value measured by `test_latency_constant` (a single-sample click clip at 48 and 44.1 kHz). |
| AC5 (R10–R17) | `test_audio_capture.py`: the begin/end lines (`scene_load start` first; every `voice_start` has its `voice_stop`, reason `scene` for those alive at Stop); a script `init` start is captured at sample ≤ 800; `capture_max_seconds 2` truncates `master.wav` at 96,000 frames and keeps events; `capture_keep 2` leaves two sessions; a read-only capture root fails `play start` with the error and plays on; a killed editor leaves a session that `status` reports `aborted` and whose `master.wav` temp decodes up to its last flush. |
| AC6 (R9) | `tools/mcp/tests/test_recorder.py::test_mp4_audio_matches_capture`: a Recorder movie with audio and an armed capture in one Play: the movie's PCM16 audio equals the capture's `master.wav` quantised the same way, frame for frame; the mixer's effect is audible in the movie (BF14's fix, kept by the tap). `editor/tests/recorder/RecorderSettingsTest.cpp` `FramesMatchAudioClock` for 24/30/60/144 fps. |
| AC7 (R14) | `tools/mcp/tests/test_audio_markers.py`: `ui_click` before the voice its subscriber starts, same sample; Bounce 2D gives `collision_2d` with `speed`; a 3D drop gives one `collision_3d` per pair per step with `speed` within 5 % of √(2gh); `spawn`/`destroy` carry the prefab GUID; `animation_event` carries the function; with no capture `AudioMarkers::isEnabled()` is false and the counters stay 0. |
| AC8 (R15, R16) | `test_audio_capture.py::test_levels`: a 1 kHz tone on Music at −12 dBFS reads −12 ± 0.5 dB peak in the 1k band of `groups.Music`, and fader −6 dB moves it by −6 ± 0.2; stems' WAV equals the group signal (correlation > 0.999 with the master when only Music plays). |
| AC9 (R19, R20) | `tools/mcp/tests/test_audio_session.py`: `status` without sessions; `arm {capture:false}` then a Play has no session; a no-device editor captures by default; `report` loudness equals `tools/mcp/audio_refs.py` (pyloudnorm) on the same `master.wav` within 0.1 LU; `E<n>`/`M<n>` numbering matches the image metadata; `play stop` returns the `audio` object; the image decodes, has the requested width and ≥ 2 % non-background pixels per lane. |
| AC10 (R21, R23) | `test_audio_qa.py` seeded failures on the `audio-qa` fixture, each FAIL or WARN with the right `limited_by` and `at`, each fixed version PASS: a clip with 120 ms head silence → `sync`; music 6 dB too hot → `audibility` naming Music and its band; a click marker with no sound → `coverage`; the Master driven hot → `true_peak` WARN (when S38 M2 is merged; SKIP note otherwise); a gap in music → `silence`; four identical footsteps in a row → `repetition`. A changed `audio.qa.sync_tolerance_ms` appears in `thresholds.changed`; the third consecutive FAIL says to stop. Python oracle `tools/mcp/audio_qa_ref.py` agrees on every measured value within its stated tolerance. |
| AC11 (R21) | `test_audio_qa.py::test_mixer_passes_its_own_checks`: the fixture scenes that only use the mixer's guarantees (limiter, duck, budget) PASS every `mixer` check. |
| AC12 (R24, R25) | `call audio_session '{"action":"check","fail_on":"fail"}'` exits 1 on the seeded failure and 0 on the fixed scene (`tools/mcp/tests/test_call_cli.py` style); `save_baseline` writes the file, undo removes it, `regression` passes on the same build and FAILs after a clip is shifted by 20 ms. |
| AC13 (R27–R29) | `tools/perf/audio_capture_bench` on server F: the R27/R28 budgets, before/after table in this spec; `AudioCaptureTest.OffPathAllocatesNothing`; `test_audio_qa.py::test_check_cost` ≤ 2.5 s for 60 s. |
| AC14 (R35, §6.6) | S34's size tools on the release Linux, Android arm64 and web players before/after: growth within §6.6; old fixtures re-save byte-identically (`test_runtime_regressions.py`); `tools/check_raw_writes.py` passes with the one new allow-list line. |
| AC15 (R30, R31) | `tools/mcp/tests/test_audio_captures_editor.py` (xvfb, isolated editor): the window lists sessions, shows the lanes and the listen banner, Enter plays a moment (an editor preview voice exists; the capture list is unchanged), Escape stops; screenshots in the evidence. `check-hardcoded-shortcuts.py` passes. |
| AC16 (§6.1, R32, R34) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py` (guide ≤ 16,384 bytes), the AC10 group test for `audio_session`; `test_stubs.py` and `tools/lua_api/tests` find `Audio.mark`/`isCapturing`; the doc test runs the lua block. |

## 8. Dependencies & conflicts

- **Depends on.**
  - **BF14** (running): assumed fixed before S35 starts: S-1 (OGG streaming), S-2 (stream seek),
    S-3/S-4 (loader bounds, `AudioFileCheck`), S-5 (last block), S-6 (bus read-ahead), S-7 (null
    drift), X-1 (Recorder double mix), the unbuilt mixer in `beginOfflineCapture` (§3) and the
    prebuilt players' audio settings (S38 §3; both confirmed for BF14 on 2026-11-01), B4
    (listener, Doppler), B5 (`--headless` → NoSound, which R4 then replaces), B6 (pause layers),
    B9, B11, B13, and decision 9 (centred 2D/UI listener). If one is still open when S35 starts,
    S35 M0 lists it as a precondition for BF14, not as S35 work.
  - **S38 M1** (`AudioMeter`: loudness, true peak, octave bands) for `report`, `levels.jsonl` and
    `check`. **S36 M1** (`AudioAnalysis`) for `audibility` and `sync`'s clip features.
  - Existing: S03 (atomic writes), S06 (Background jobs), S09 (`logCategoryAudio()`), S24
    (`WriteFilesCmd`), S25 (registry, catalog), S27 (Recorder), S30 (player flags), S31 (groups,
    budget, `call`), S34 (sizes).
- **Blocks.** S37 adds event fields to the log; S39 (`music` lines) and S40 (`cue` lines) test
  through captures.
- **Merge hotspots.** `AudioSystem.cpp` (S37, S39, S40, BF14), `AudioMixer.cpp` (S38),
  `Engine.cpp` `runFrame`, `editor/Project.cpp` (Play start/stop), `RecorderSession.cpp` (S27
  owners), `AudioFilters.h` (S38 changes `LimiterFilter`, S35 `MeterFilter`),
  `McpToolGroups.cpp`, `McpInstructions.cpp`, `docs/mcp/guide/sound.md`.

## 9. Risks & defaults

- **Disk use.** float32 stereo is 23 MB per minute; defaults 120 s × 3 sessions ≤ 140 MB under
  `.jasperengine/`. Default kept; agents raise `capture_max_seconds` per need.
- **A capture mutes the device.** As the Recorder (S27 R29); the Game view banner says so.
- **Constant latency depends on BF14's S-5/S-6.** Default: AC4 measures it; a mismatch is a BF14
  regression, not an S35 tolerance.
- **Physics ordering.** 3D markers rely on physics-contacts' sorted main-thread dispatch; one
  marker per pair per step bounds the log.
- **Determinism across platforms** is not promised (float SIMD paths differ): R8 is per platform.

**Resolved questions**
- The Recorder's capture skipping the mixer and prebuilt players ignoring Project Settings > Audio:
  **resolved 2026-11-01, fixed by BF14**; S35 assumes them and still replaces the Recorder path
  (R9). The release-player module question is resolved in S37 §9 (Q1).

## Milestones

- **M0** Preconditions: BF14 merged; S38 M1 merged.
- **M1** Deterministic driver, `AudioClock`, pump point, synchronous loads, `--headless`/
  `--silent-audio` (R1–R8); AC1–AC3.
- **M2** `AudioTap`, the Recorder migration (R9); AC6.
- **M3** Capture writer, stamping, markers, levels, stems, player capture, Lua `Audio.mark`
  (R10–R18, R32); AC4, AC5, AC7, AC8.
- **M4** `audio_session` status/arm/report, `play stop` summary, timeline image (R19, R20, R22);
  AC9. Needs S38 M1.
- **M5** `check`, anti-gaming, exit codes, baselines (R21, R23–R25); AC10–AC12. Needs S36 M1.
- **M6** Captures window and banner (R30, R31); AC15.
- **M7** Budgets, docs, size, final gate (R27–R29, R34, R35); AC13, AC14, AC16.

## Deviations

- **What a Recorder movie decodes to changes, on purpose, in its first 1,600 frames only.** S62 pinned `408859a6...`; the pin is now `821d2802...`.
  The cause was not the voice but the Master bus: `playBus` set the bus's unity pan on a running voice, which ramped from the centre's 0.707
  over the first mix call of the audio, so the output depended on that call's length (red: `DeterministicDriverTest.AStartDoesNotDependOnTheChunks`,
  48 kHz and 44.1 kHz clips, frames 3 to 1,599 differ by up to 0.056; one call a frame on the driver reproduced S62's exact hash). The bus now starts
  paused with its pan, so the first sample is at unity and one call and chunks of 512 are the same samples at both rates. A volume a game moves during
  a Play (`setVolume`, a fader) still ramps over one mix call: making that chunk-independent means a per-voice fixed-length ramp in SoLoud's mix loop,
  which is not simple and is left as is.
- **A capture writes S38's `limiter` lines** with `levels.jsonl`'s 100 ms lines, only when a limiter takes something off; `latency_samples` adds the lookahead on the voice's way out (Master's in `session.json`).
- **The `audio-qa` and `audio-markers` fixtures are not used:** each test makes its scenes over MCP, which keeps the seed next to its check.
- **AC13 is a gtest (`AudioCaptureBenchTest`) with `perfSlack()`**, not `tools/perf/audio_capture_bench`: AGENTS.md's way of reporting wall-clock budgets. Server F no longer exists.
- **AC5's unwritable root is a file where a folder goes**, not a read-only folder: the tests run as root in some images.
- **`voice` is the capture's own serial, not SoLoud's handle:** handles count every play since the process began, so two
  runs of one editor differed and R8's bytes could not hold. Found by `test_deterministic_twice`.
- **`levels.jsonl`'s bands are a spectrum, not `AudioOctaveBank`:** the bank on five signals cost 6.7 ms a frame against R28's 0.4 ms
  (`AudioCaptureBench`, the red run). Each 100 ms line takes a Hann FFT of its last 85 ms, summed over the bank's octave edges, so a click in
  a line's first 15 ms can read low in the bands (not in `peak_db` or `rms_db`).
- **R8's latency is not one constant:** measured (`DeterministicDriverTest.LatencyIsOneSampleForEachBusOnTheWay`), a voice
  reaches the output 1 sample late, plus 1 for each bus on its way: 2 through Master, 3 through a group under it. Each
  `voice_start` has its `latency_samples`; `session.json`'s is Master's. The checks subtract the voice's.
- **S37 landed its observer first.** The capture is a client of `AudioSystem::setVoiceObserver`, whose `AudioVoiceLine`
  gained `pause`/`resume` kinds and `pan`, `looping`, `spatial`, `position` and `loadDelay`. `isDeterministic()` stays
  S37's "mixed on a null driver" (a seed is `audio.seed` on the Null fallback too); `getOutputMode()` tells the new driver
  apart. S37's voice lines stay in release players, where R13 wanted nothing: they cost one bool per site, and
  `JASPERENGINE_AUDIO_TRACE` is useful there.
- **`play start` also says a Play is capturing** (`audio_capture`, and a text line). Found by the eval: agents played and read runtime state, and never reached
  `audio_session` until the result told them. It replaces a line in the agent guide, which the eval showed nothing needs.
- **`stopVoice` writes the stop line itself**, with the reason of a `StopReason` scope around the stop, so no stop has a
  second path. `ended` is found after each mix, and is stamped at the end of the frame that ended it.
- **An explicit `JASPERENGINE_AUDIO_BACKEND=null|nosound` does not capture by default** (R4 says "no device"): a test that
  chose its backend keeps it; `arm` still captures.
- **A failed write stops the capture**, keeping what was written: the error goes to the Console, `session.json` says
  `state: "failed"` and `error`, the game goes on.
- **A player's capture has no project settings:** `max_seconds` 3,600, `keep` 20; `--quit-after` ends it.
- **R9's Recorder gets silence for a paused Play** (the pump mixes nothing, the frames go on), so sound and picture stay
  in step; before, sounds paused with the scene while the offline mix went on, the same silence.
- **The mixer's transitions hold while a Play on this driver is paused**, or how long a pause lasted would be in the mix.
- **`ClockedVoicesStartAsFarApart...` plays at clock times itself:** with `mixOffline` gone no pump leaves a mix between two
  frames. The device path's use of the mixer's clock is read, not tested.
- **AC6 is `test_movie_audio_matches_capture` on MOV:** its audio is PCM; an MP4's AAC cannot equal anything sample for sample.
- **`html_click` has no scene:** a context does not know its scene; `scene` is empty on these lines.
- **`voice_start.position` and the group levels' fader** are the values as of the frame's start: a fader moving during a chunk
  ramps in SoLoud but is one gain per chunk in the group's level and stem.
- **AC14's "before" is S37's tip**, the branch this one builds on, not `main`: what S35 adds alone.

## Measured (AC13, AC14)

`AudioCaptureBench.PumpAndCaptureBudgets`, server B under load, 32 voices, 4 groups, a reverb, 60 fps, 48 kHz:

| | ms a frame | budget |
|---|---|---|
| Null pump | 0.141 | |
| deterministic pump | 0.135 (-4.0 %) | 0.6, and at most 5 % above Null |
| capture on top | 0.115 | 0.4 |

Release player, before and after, `full` variant:

| Player | Before | After | Growth | Section 6.6's estimate |
|---|---|---|---|---|
| Linux glcore, stripped | 41,213,008 | 41,217,104 | 4,096 bytes (one file page) | at most 3 KB |
| Web single-thread wasm, gzip | 9,867,146 | 9,867,712 | 566 bytes (3,009 raw) | at most 2 KB |
| Android arm64 library, stripped | 34,314,400 | 34,320,232 | 5,832 bytes | none stated |

The Linux and Android figures pass the 3 KB estimate by what the linker's pages and the tap's `std::function` list take; nothing of the capture itself is in them.
