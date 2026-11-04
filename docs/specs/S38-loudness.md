# S38 — Loudness: the LUFS meter, a true-peak lookahead limiter, new-project audio defaults

**Wave / Depends on.** Sound wave A (spec-first: implemented after the integration branch lands in
`main`). **M1 (`AudioMeter`) has no dependency and lands first**: S35 and S36 need it. The rest
depends on BF14 and runs in parallel with S35–S37. Owner area: `engine/core/audio/AudioMeter.*`,
`TruePeak.*`, `AudioEffects.cpp` and `AudioFilters.h` (the `limiter` effect),
`engine/core/subsystem/AudioSystem.*` (output stage, iOS session),
`engine/libs/soloud/src/backend/miniaudio/soloud_miniaudio.cpp` (session category hook),
`editor/Project.cpp` (new-project defaults), `editor/window/AudioMixerWindow.cpp` (Master
readouts), `editor/settings/SettingsRows.cpp` (Audio keys), `docs/mcp/guide/audio.md` and the
Loudness section of `docs/mcp/guide/sound.md`.

## 1. Goal

Loudness becomes measurable and new games ship at a sane level. An engine library measures EBU
R128 / BS.1770-4 loudness and true peak; the editor shows it on the Master strip and agents read
it. Upstream's `limiter` effect gains a true-peak detector and a lookahead, so a Master limiter
can hold a −1 dBTP ceiling. User decision 2 becomes concrete: **every new project gets
`audio/Main.mixer` with a Master true-peak limiter at −1 dBTP, a −18 LUFS target and a linear
output stage**, and existing projects sound exactly as before. User decision 8 lands here too,
through the same new-project mechanism: new projects use the iOS `ambient` audio session.

## 2. Scope

**In scope**
- `AudioMeter` and `TruePeak` (engine library, no SoLoud dependency), calibrated.
- The `limiter` effect's `lookahead` and `truePeak` parameters, its gain-reduction readout.
- The output stage setting (`roundoff` as today, `linear`).
- New-project defaults: the mixer file, `audio.mixer`, the output stage, the iOS session category.
- Loudness settings (target, tolerance, ceiling) in S25's registry, read by S35's checks and S36's
  notes.
- Readouts: the Audio Mixer window's Master strip, `audio_mixer get` and `get_play_state` in Play.

**Out of scope**
- Real-time loudness normalisation of a game's output (a game cannot know its future; the target
  is a measurement and a check, not a gain stage).
- Import normalisation, loudness presets per platform (not requested; S36's `gain_db` covers
  manual levelling).
- Changing any existing project's `.mixer`, output stage or session (decision 2).
- The iOS interruption restart and device suspend of sound-layer's `AudioOutput` (P6 §2.7; not
  requested).
- A Lua loudness API (the meter is editor and capture code; §6.6).

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **No loudness code.** No LUFS, K-weighting or true-peak code outside the vendored decoders.
  Group meters are peak dB: `MeterFilter` keeps the peaks of the last 8 blocks
  (`engine/core/audio/AudioFilters.h:31-44`); `AudioMixer::getMeterDb` returns peak × fader.
- **The limiter.** `LimiterFilter` (`AudioFilters.h:15-27`, instance `AudioEffects.cpp:155-246`):
  "lookahead-free peak limiter, per-sample gain smoothing, channels linked", parameters
  `threshold` (dB, −30…0, default −1) and `release` (s, 0.001–2, default 0.1)
  (`AudioEffects.cpp:80-82`). It detects sample peaks only, so inter-sample peaks pass.
- **The output stage.** `AudioSystem::init` always uses `CLIP_ROUNDOFF`
  (`engine/core/subsystem/AudioSystem.cpp:143`): SoLoud maps every output sample through
  `y = 0.87x − 0.1x³` (clipped beyond ±1.65) and multiplies by the post-clip scaler 0.95
  (`engine/libs/soloud/src/core/soloud.cpp:618, 786-906`). The small-signal gain is
  20 log10(0.87 × 0.95) = **−1.66 dB**, and a −1 dBFS peak leaves as about −3.5 dBFS: a mixer-level
  ceiling never equals the output's, and measured loudness is shifted.
- **New projects.** `Project::createProjectInDirectory` (`editor/Project.cpp:6305-6390`) creates the
  project with the defaults "this version" picks (precedent: `vcsProjectSettings.lfs = true`,
  `:6381`); audio gets none: `audio.mixer` is empty (an implicit Master, AM A2) until
  `audio_mixer edit` creates `audio/Main.mixer` without effects.
- **Prebuilt players ignore the project's audio settings** (found while checking).
  `PlayerConfig` (`engine/core/player/PlayerConfig.h:44-110`, written by
  `Exporter::writeGameConfig`, `editor/ExporterPrebuilt.cpp:496`) has no audio fields, and
  `AudioSystem::configure` / `AudioMixer::load` are emitted only into a Compile build's generated
  `main.cpp` (`editor/Generator.cpp:2869-2873`). A prebuilt game (S30 R5's default method) therefore
  runs with no mixer and default voices. **BF14 fixes this before main** (resolved Q1, 2026-11-01):
  `game.json` gets an audio block `{mixer, maxRealVoices, maxVoices, sampleRate}` that prebuilt
  players apply as the generated `init()` does. S38 assumes that fix.
- **iOS session.** The engine sets none: SoLoud's miniaudio backend calls `ma_device_init(NULL, …)`
  (`soloud_miniaudio.cpp:66-74`), so miniaudio's default applies: PlayAndRecord with
  DefaultToSpeaker, else Playback (`miniaudio.h:36495-36510`): the silent switch is ignored and the
  player's own music is interrupted.

## 4. Reference: sound-layer (`snd/sound-layer` 20b461161; checkout `je-wt/snd-ro`)

| What | Where (commit, file) | Reuse / change / drop |
|---|---|---|
| `AudioMeter` | a19660f8c…fad196a52 `engine/core/audio/AudioMeter.{h,cpp}` (859), P1 §2.6, calibrated against EBU Tech 3341 and pyloudnorm (P1 §3.1) | **Reused as is**; the true-peak oversampler split out as `TruePeak` so the limiter links it without the rest. |
| True-peak lookahead limiter | 4235c1450…751a3867c `engine/core/audio/LimiterFilter.{h,cpp}` (759; fix 053b1c958), P2 | **Reused as the implementation of upstream's `limiter` effect** behind two new parameters whose defaults keep today's algorithm; sound-layer's fixed Master stage (slot 6) and `limiterEnabled`/`limiterCeilingDb` project keys are **dropped** (the limiter is an effect in the `.mixer`). |
| Linear output stage when the limiter is on | P2 (analysis G-14) | **Changed**: an explicit project setting, `audio.output_stage`, that new projects set (D3). |
| Loudness target and tolerance | `AudioSettings::loudnessTargetLufs` −18, `loudnessToleranceLu` 2 (`engine/core/audio/AudioSettings.h:182-183`) | **Reused** as S25 keys; `newProjectDefaults()` (48 voices, limiter on) becomes R8's mixer file; upstream's voice settings stay. |
| Platform loudness presets, `importNormalizeLufs` | P2 | **Dropped** (not requested). |
| iOS session category | `snd/p6-platform` c19ea6d4d, 23bd60955 (`AudioIosSessionCategory`, `soloud_jasperengine.h`) | **Reused** as an S25 key and the backend hook; interruption restart dropped. |
| Tests | `test_audio_mixer` limiter cases, `test_audio_settings` (category F, on sound-layer's mixer) | Scenarios harvested; tests rewritten on upstream's `.mixer` (`editor/tests/AudioMixerTest.cpp` style). |

## 5. Requirements

### Functional — the meter

- **R1. `AudioMeter`** (`engine/core/audio/AudioMeter.{h,cpp}`, plain C++): interleaved or planar
  float input, 1–8 channels, 8–192 kHz, file mode (one call) and stream mode (many `process`
  calls, `result()` at any time). It measures (sound-layer P1 §2.6, unchanged definitions):
  - BS.1770-4 K-weighting (two biquads per channel, coefficients derived per sample rate from
    libebur128's constants; at 48 kHz within 1e−6 of the BS.1770-4 table), channel weights 1, 1, 1,
    0, 1.41, 1.41 for 5.1;
  - momentary (400 ms, every 10 ms) and short-term (3 s, every 100 ms) maxima; integrated loudness
    (400 ms blocks, hop 100 ms, absolute gate −70 LUFS, relative −10 LU; null when nothing passes);
    LRA (EBU Tech 3342); optional 100 ms curves;
  - true peak (R2), sample peak, clipped samples (|x| ≥ 1 − 2⁻¹⁵), RMS (AES17), DC offset;
  - a 10-band octave bank (31.5 Hz–16 kHz, 4th-order Butterworth band edges at fc/√2 and fc·√2)
    and the sub share (20–60 Hz).
- **R2. `TruePeak`** (`engine/core/audio/TruePeak.{h,cpp}`): 4× oversampling with BS.1770-4 Annex
  2's 48-tap FIR (4 phases of 12), per channel, streaming, reporting the maximum absolute
  oversampled value including the original samples; used by `AudioMeter` and the limiter.

### Functional — the limiter

- **R3. Parameters.** The `limiter` effect (`AudioEffects.cpp` table) gains, after `release`:
  `lookahead` ("Lookahead", ms, 0–5, default 0) and `truePeak` ("True Peak", 0/1, default 0).
  Omitted parameters take their defaults (AM §7.4), so every existing `.mixer` file is unchanged
  on disk and in sound.
  - **Format version.** Today's reader refuses a parameter it does not know
    (`engine/core/audio/AudioMixerIO.cpp:499-502`) and any `version` above 1 (`:416`). A mixer
    that sets `lookahead` or `truePeak` away from its default is therefore written as
    **`version: 2`**; every other mixer stays `version: 1`, byte-identical (the `.anim` precedent:
    version 2 only for humanoid clips, `AnimationClipIO.cpp:1995-1996`). `AudioMixerIO` reads 1
    and 2; an older engine refuses a version-2 file with its own "newer than this engine reads"
    message instead of a parameter error. Exports are version-locked to their editor (S30 R10).
- **R4. Algorithm.** With both at their defaults the filter runs today's code path and its output
  is **bit-identical** to the base. Otherwise (sound-layer's algorithm):
  - the signal is delayed by `lookahead` (rounded to frames at the bus rate);
  - the detector reads the undelayed signal, through `TruePeak` when `truePeak` is 1;
  - the gain is the running minimum of the required gain over the lookahead window, smoothed by a
    moving average of the same length (so it reaches its target at the delayed peak), then
    released with `release`; channels linked;
  - non-finite input is replaced by 0 and resets the state (NaN guard);
  - switching `bypass` crossfades over 5 ms (no click);
  - `LimiterFilter::getGainReductionDb()` returns the largest gain reduction of the last 100 ms
    (an atomic the audio thread writes).
- **R5. The guarantee.** With `truePeak` 1 and `lookahead` ≥ 1.5 ms, the bus output's true peak
  is ≤ `threshold` + 0.1 dB for inputs up to 24 dB over the threshold, at 44.1 and 48 kHz.
- **R6. Latency.** A limiter with lookahead delays its bus by `lookahead` frames; S35 adds a
  Master limiter's lookahead to `latency_samples`, and `audio_mixer get` reports
  `latency_ms` per group.
- **R7. Output stage.** `audio.output_stage`: `roundoff` (today: `CLIP_ROUNDOFF`, post-clip
  scaler 0.95) or `linear` (no roundoff flag, post-clip scaler 1.0: samples pass unchanged and are
  hard-clipped at ±1 only as a last guard). `AudioConfig` gains `outputStage`; it applies at the
  next audio init, like the sample rate; S35's `session.json` reports it.

### Functional — new-project defaults

- **R8. Every project the editor creates** (File > New Project, MCP `create_project`, the S27
  Hub's templates unless a template has its own `audio:` node) gets, in
  `Project::createProjectInDirectory`:
  - `<assets>/audio/Main.mixer`:

    ```json
    {
      "format": "jasperengine.mixer", "version": 2, "updateMode": "scaled",
      "groups": [
        {"id": "00000001", "name": "Master", "parent": null, "volume": 0,
         "effects": [{"id": "<8 hex>", "type": "limiter",
                      "params": {"threshold": -1, "release": 0.1, "lookahead": 1.5, "truePeak": 1}}]}
      ]
    }
    ```

    written by `AudioMixerIO::write` (with its `.meta`, if `.mixer` gains an S04 row, see §8);
  - `audio.mixer: audio/Main.mixer`, `audio.output_stage: linear`,
    `audio.ios_session_category: ambient` written to `project.yaml`;
  - nothing else: the loudness target is the registry default (R9), voices stay upstream's
    defaults (32 / 256), and no other groups are invented.
- **R9. Existing projects** are never changed: opening, saving and exporting a project without
  these keys keeps no mixer (or its own), `roundoff`, and the miniaudio default session; no
  migration writes anything. `audio_mixer edit` on a project without a mixer still creates
  `audio/Main.mixer` without effects (upstream's behaviour).

### Functional — settings and readouts

- **R10. Settings** (S25, page Audio, written only when not default):

  | Key | Type, range | Default (all projects) | New projects |
  |---|---|---|---|
  | `audio.loudness_target_lufs` | float −40…−6 | −18 | (default) |
  | `audio.loudness_tolerance_lu` | float 0.5–10 | 2 | (default) |
  | `audio.true_peak_ceiling_dbtp` | float −12…0 | −1 | (default) |
  | `audio.output_stage` | `roundoff` \| `linear` | `roundoff` | `linear` |
  | `audio.ios_session_category` | `default` \| `ambient` \| `solo_ambient` \| `playback` | `default` (miniaudio's) | `ambient` |

  The target, tolerance and ceiling only measure: they change no sample, so their default may be
  the same for every project. The page shows the Master's live LUFS beside the target while
  playing.
- **R11. iOS session.** SoLoud's miniaudio backend gets a hook (vendored patch, marked
  `// JasperEngine patch`) that creates its context with `coreaudio.sessionCategory` from
  `AudioConfig` before `ma_device_init`; `default` leaves miniaudio's own choice. Compile builds
  pass it through `AudioSystem::configure`; prebuilt builds through BF14's `game.json` audio block (R14).
- **R12. Live loudness in the editor.** `AudioMixer::setLoudnessTap(bool)` (editor only calls it)
  makes the Master group's `MeterFilter` also push its blocks into a lock-free single-producer
  ring (2 s); the editor drains it into an `AudioMeter` stream. The Audio Mixer window's Master
  strip shows LUFS-M, LUFS-S and integrated (since Play start or a click on Reset) against the
  target line, a true-peak hold, and, when a limiter is on the Master, a gain-reduction meter
  (`getGainReductionDb`). The tap is off unless the window is visible or an agent reads it (R13).
- **R13. Agents.** In Play, `audio_mixer get` adds `loudness {momentary_lufs, short_term_lufs,
  integrated_lufs, true_peak_dbtp, gain_reduction_db, target_lufs}` for Master, and
  `get_play_state.audio` adds `loudness {momentary_lufs, true_peak_dbtp}` (results only; the
  first read turns the tap on until Stop). S35's capture logs `limiter` lines (`gain_reduction_db`
  every 100 ms) when a limiter exists on any group.
- **R14. Players.** The output stage and the session category reach every export: Compile builds
  through `AudioConfig` in the generated `AudioSystem::configure`, prebuilt builds through the
  audio block of `game.json` that BF14 adds (S38 adds `outputStage`, `iosSessionCategory` beside `mixer`,
  `maxRealVoices`, `maxVoices`, `sampleRate`).

### Platform & backends

- **R15.** `AudioMeter`, `TruePeak` and the limiter are portable C++ (no SIMD requirement; a
  scalar path is the reference). The session category is iOS only (ignored elsewhere); it is
  reviewed and checked on a device by a human (no Xcode on the build servers), as S30's iOS items.

### Performance budgets (server F, Release; Android arm64 on the emulator host for R17)

- **R16.** `AudioMeter` stream mode ≤ 1.5 % of one core for 48 kHz stereo; file mode of a 3-minute
  stereo file ≤ 300 ms.
- **R17.** The limiter with `truePeak` 1 and 1.5 ms lookahead ≤ 2.5 % of one core per stereo bus
  at 48 kHz on Android arm64 (sound-layer measured about 1 %); with defaults, unchanged from the
  base.

### Editor UX (human)

- **R18.** The limiter's effect panel shows the two new parameters (True Peak as a checkbox) and
  its gain-reduction meter; Project Settings > Audio shows R10's keys with help ("Measurement only:
  changing the target changes no sound"); the Mixer window shows R12's readouts.

### Agent UX (MCP, Lua, CLI)

- **R19.** `audio_mixer` ops accept the new parameters (`set_effect params {lookahead, truePeak}`)
  with no schema change; `set_project_settings` accepts R10's keys. Lua reaches them through the
  existing `AudioMixerEffect:setParameter("lookahead", v)` (the effect table drives it): no binding
  changes.
- **R20. Docs.** `docs/mcp/guide/audio.md`: the effects table rows of `limiter`, the output stage,
  the new-project mixer; `sound.md` "Loudness": targets by platform as guidance (not settings),
  how to read LUFS-M for one-shots and integrated for music (S36's types), the −1 dBTP rule.

### Data & compatibility

- **R21.** Old `.mixer` files: byte-identical on load and save; their limiter sounds bit-identical
  (R4). Old projects: R9. A mixer using the new parameters is version 2 (R3): an older engine
  refuses it with a clear message (a new project opened in an editor older than S38 has no mixer
  until it is upgraded).

## 6. Design constraints & decisions

- **D1. Upgrade the existing effect, do not add a type.** A `limiter_tp` type would split
  snapshots, exposed parameters and docs; parameters with old-behaviour defaults keep every file
  and every sound the same (the analysis' D5 option).
- **D2. The new-project mixer is a file**, not a hidden default, so it is visible, editable,
  diffable and versioned like any `.mixer` (AM A1), and a user can remove the limiter.
- **D3. The linear output stage comes with the ceiling.** Under `roundoff` the limiter's −1 dBTP
  leaves the device near −3.5 dBFS and loudness reads 1.66 dB low, so decision 2's ceiling and
  target can only hold at the output with `linear`. Existing projects keep `roundoff` (their sound
  must not change).
- **D4. The target is the same default everywhere.** It changes no sample; only new projects get
  a mixer and a stage, as decided.
- **D5. The meter is not shipped by default.** Release players link only `TruePeak` (through the
  limiter) and the tap hook; `AudioMeter` is referenced from editor code and S35's capture
  (development players).

API sketch:

```cpp
namespace jasperengine {
  struct LoudnessResult { std::optional<double> integrated, momentaryMax, shortTermMax, lra;
      double samplePeakDb, truePeakDb, rmsDb, dcOffset; uint64_t clipped; std::array<double, 10> bandsDb; double subShare; };
  class AudioMeter { public: AudioMeter(unsigned int rate, unsigned int channels, const AudioMeterOptions& = {});
      void process(const float* interleaved, size_t frames); LoudnessResult result() const;
      std::vector<float> momentaryCurve() const, shortTermCurve() const; };
  class TruePeak { public: explicit TruePeak(unsigned int channels); float process(const float* interleaved, size_t frames); void reset(); };
  enum class AudioOutputStage { Roundoff, Linear };   // AudioConfig::outputStage
  enum class IosAudioSession { Default, Ambient, SoloAmbient, Playback };   // AudioConfig::iosSession
}
```

### 6.1 MCP tools and the budget

No new tool and no schema change: `audio_mixer` (+0; parameters are in `jasper://docs/audio`),
`get_play_state` (+0, result only), `set_project_settings` (+0, keys are data). Group `audio`
totals: S35 §6.1. Core: +0.

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `*.mixer` | `jasperengine.mixer` v1, and **v2** only when a limiter sets `lookahead` or `truePeak` (R3) | upstream's `AudioMixerIO`, reading 1–2 |
| `project.yaml` `audio:` | S03 `formatVersion` unchanged (additive keys, written only when not default) | `editor/Stream.cpp` audio node (`Stream.cpp:2918` pattern) through the registry's getters/setters |
| `game.json` audio block | `PlayerConfig::SCHEMA_VERSION` 1, additive keys | `PlayerConfig::fromJson` (BF14's block; S38's two keys) |

### 6.3 Settings

R10's five keys (S25 SettingsRegistry, Project scope, page Audio, subsection Loudness and Output).

### 6.4 Editor commands

`audioMixer.resetLoudness` (Reset Integrated Loudness; menu of the Master strip, no key).

### 6.5 Lua API

None new (R19: the effect table already exposes parameters).

### 6.6 Player size (S34)

- Always linked: `TruePeak` (48 taps, about 1 KB of tables), the limiter's lookahead path and the
  tap hook: estimated ≤ 6 KB Linux, ≤ 4 KB wasm (gzip ≤ 2 KB). The limiter is part of upstream's
  core mixer (every `.mixer` may name it), so it cannot be a module without splitting an effect;
  accepted by the user (Q2, resolved 2026-11-01).
- `AudioMeter`: zero in release players (D5); in development players under
  `JASPERENGINE_AUDIO_CAPTURE` (S35).

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R2) | `engine/tests/core/audio/AudioMeterTest.cpp`: EBU Tech 3341 cases 1–5 (stereo 1 kHz at −23/−33 dBFS → −23.0/−33.0 ± 0.1 LUFS; the gating cases), EBU 3342 LRA cases within ±1 LU, a fs/4 sine at 45° phase → true peak = sample peak + 3.01 ± 0.2 dB, the K-weighting coefficients at 48 kHz within 1e−6, a 40 Hz sine in the 31.5/63 Hz bands, stream equals file mode within 0.01 LU. `tools/mcp/tests/test_audio_meter.py`: pyloudnorm agrees within 0.1 LU on the `audio` fixture files. |
| AC2 (R3, R4, R21) | `editor/tests/AudioMixerTest.cpp` `LimiterDefaultsBitIdentical`: a 10 s noise-plus-tone signal through the base limiter and the new one at defaults → identical samples; old fixture `.mixer` files re-save byte-identical as version 1; a mixer with `truePeak 1` is written as version 2, read back equal, and refused by the base's reader with its version message (a fixture string checked into the test). |
| AC3 (R5) | `LimiterTruePeakGuarantee`: inputs 6, 12 and 24 dB over a −1 dB threshold (inter-sample-peak test signals, fs/4 at 45°, transients), `truePeak 1`, lookahead 1.5 ms, 44.1/48 kHz: output true peak ≤ −0.9 dBTP (`TruePeak` and an independent numpy oracle). NaN input recovers within one block; toggling bypass makes no step larger than the signal's own. |
| AC4 (R6, R7) | `AudioOutputStageTest`: `roundoff` small-signal gain −1.66 ± 0.01 dB (today), `linear` 0.00 dB; S35's `latency_samples` grows by 72 at 48 kHz with a 1.5 ms Master limiter (`test_audio_capture.py::test_latency_with_limiter`). |
| AC5 (R8, R9) | `tools/mcp/tests/test_project_audio_defaults.py`: `create_project` → `assets/audio/Main.mixer` equals R8 (ids aside), `project.yaml` has the three keys; a Play of a hot test scene measured by S35's capture has true peak ≤ −0.9 dBTP and integrated loudness equal to the mixer-level measurement (linear stage); every existing fixture project opened and saved is byte-identical and its capture is bit-identical to the base's. |
| AC6 (R10–R13) | `test_audio_mixer.py::test_loudness_readouts`: in Play `audio_mixer get` returns `loudness` within 0.2 LU of S35's report over the same range; `get_play_state.audio.loudness` present; `set_project_settings audio.output_stage linear` applies at the next Play (log line); `audio.ios_session_category` reaches the generated `main.cpp` (`GeneratorAudioTest`). Mixer window screenshot with the readouts (`test_audio_mixer_editor.py`, evidence). |
| AC7 (R11, R15) | Review of the miniaudio patch (it compiles for iOS in the Xcode project, `tools/xcode_sync_engine.py --check`); a human check on an iOS device: a new project's game respects the silent switch and mixes with Music app playback; an existing project's does not change. Recorded in Progress. |
| AC8 (R16, R17) | `engine/tests/core/bench/AudioMeterBench.cpp`, `LimiterBench` on server F and the Android emulator host: within R16/R17; before/after table in Progress. |
| AC9 (§6.6) | S34's `player_sizes.py` and `lib_size.py --modules`: release players grow by no more than §6.6's estimate; `AudioMeter` symbols absent from release players (`nm`/wasm name section). |
| AC10 (R19, R20) | `test_tool_budget.py` (no growth), `test_docs.py`, the lua doc test for `setParameter("truePeak", 1)`. |

## 8. Dependencies & conflicts

- **Depends on.** BF14 (S-5/S-6, which the limiter's latency test relies on, and the prebuilt
  players' audio block of resolved Q1); upstream's mixer;
  S03, S25, S27 (Hub templates), S30 (game.json), S34.
- **Blocks.** S35 (M4: `AudioMeter`; `true_peak` check reads R3–R5), S36 M1 (`AudioMeter`).
- **Coordination.** `.mixer` files have no row in S04's `AssetTypes` (`editor/asset/AssetTypes.cpp:23-54`
  lists none) and `audio.mixer` is a path (analysis §1 decision 3 note): the new-project file is
  written like any `.mixer` today; if S04's owner adds a `.mixer` row, its `.meta` comes with it.
- **Merge hotspots.** `AudioEffects.cpp`/`AudioFilters.h` (S35 `MeterFilter`), `AudioSystem.cpp`
  (init flags; BF14), `Project.cpp` (`createProjectInDirectory`), `AudioMixerWindow.cpp`,
  `SettingsRows.cpp`, `Generator.cpp`, `PlayerConfig.*`, the vendored miniaudio backend (BF14's
  SoLoud patches).

## 9. Risks & defaults

- **Lookahead latency** (1.5 ms) on every voice of a new project: inaudible, constant, reported
  (R6).
- **Hard clip under `linear`** if a user removes the limiter and drives Master hot: default: the
  Mixer window warns ("Master clips: no limiter with the linear output stage") when the Master
  peak meter reaches 0 dBFS.
- **CPU on low-end Android** (R17): default: the measurement decides; if over budget the
  oversampler gets a NEON path, never a weaker detector.

**Resolved questions**
- **Q1. Prebuilt players do not load the project's mixer or audio settings** (§3) — **resolved
  2026-11-01: BF14 fixes it** (`fix/bf14-audio-bugs`, before main), together with the Recorder's
  capture that skipped the mixer (S35 §3). There is no S38 M0; S38 assumes both fixes and only adds
  `outputStage` and `iosSessionCategory` to BF14's `game.json` audio block (R14).
- **Q2. Always-linked limiter growth** — **resolved 2026-11-01: accepted.** The true-peak and
  lookahead path stays inside upstream's core `limiter` effect, ≤ 6 KB in every release player
  (§6.6); new projects get the `linear` output stage (R7, R8); the mixer is written as format
  version 2 only when the new limiter parameters are used (R3).

## Milestones

- **M1** `AudioMeter`, `TruePeak`, their tests (R1, R2); AC1. **Lands first** (S35, S36 wait for it).
- **M2** Limiter parameters and algorithm, gain-reduction readout, output stage (R3–R7); AC2–AC4.
- **M3** New-project defaults and settings (R8–R10); AC5.
- **M4** Readouts: tap, Mixer window, `audio_mixer get`, `get_play_state` (R12, R13, R18); AC6.
- **M5** iOS session hook and the export paths (R11, R14, R15); AC7 (prebuilt through BF14's `game.json` audio block).
- **M6** Budgets, docs, size (R16, R17, R19–R21); AC8–AC10.

## Deviations

### M1 (`AudioMeter`, `TruePeak`)

1. **API.** §6's sketch holds (`AudioMeter(rate, channels, options)`, `process`, `result`, the curves,
   `TruePeak(channels)`, `LoudnessResult`), with what R1 and sound-layer's consumers also need:
   `processPlanar`, `finish` and one-call `measure`/`measurePlanar` (file mode), `AudioMeterOptions`
   (`mode`, `bands`); in the result `frames`, `channels`, `sampleRate`, `gatedBlocks`, `relativeGate`
   (S35's `gated_seconds`), `nonFinite`, `meanSquare`, `bandsMeanSquare`, `subMeanSquare`. Values
   sound-layer gave as NaN are `std::optional`. The curves are methods, always available, so
   sound-layer's `curves` option is gone. A short-term curve point before 3 s is NaN (the curve is a
   `std::vector<float>`).
2. **`TruePeak::processSample(channel, sample)`** joins the sketch: the limiter (M2) needs the detector
   one step at a time, as sound-layer's did. The filter runs in float, with its table by tap and then
   phase, so its inner loop vectorizes without reordering a sum (R17's budget); each coefficient is
   exact in a float.
3. **Channel weights go beyond the 5.1 of R1:** 5.0 (L R C Ls Rs) weighs 1, 1, 1, 1.41, 1.41 (Tech 3341
   case 6 and pyloudnorm need it) and 7.1 (L R C LFE Lb Rb Ls Rs) 1, 1, 1, 0, 1, 1, 1.41, 1.41 (BS.1770-4's
   positions, and the LFE must not count). Other layouts weigh 1, as in sound-layer.
4. **Two changes to sound-layer's code,** behind the same results. Every 10 ms the K-weighting and band
   filters let go of a state under 1e-30, because a stream's digital silence otherwise leaves them in
   denormals (a 35 % cost in silence measured on the Ryzen of server H; Intel's assists cost more, not
   measured; `AudioMeter.FiltersSettleOnZeroInSilence`). And the windows are subtractions of a running
   sum, so `result()` costs in proportion to the signal and not 40 or 300 times that (R12 reads it live).
5. **`test_audio_meter.py` compiles its own probe** (`engine/tests/core/audio/AudioMeterProbe.cpp`, with
   `AudioMeter.cpp` and `TruePeak.cpp` and nothing else), since nothing in the editor exposes the meter
   before S36's `analyze_audio`. It needs a C++ compiler and FFmpeg, both in the Linux image. The link
   is also the check that the library needs no SoLoud. The "`audio` fixture files" are
   `engine/tests/core/fixtures/audio/` and the two other WAV files the repository has
   (`pickup.wav`, `beep.wav`), plus signals built by the test.
