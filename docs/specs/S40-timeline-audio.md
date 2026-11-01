# S40 — Timeline audio: audio cues on animation clips (Animation Player, Animator, montages)

**Wave / Depends on.** Sound wave B (spec-first: implemented after the integration branch lands in
`main`). Depends on **S37 M2 and M4** (`startEvent`, free voices and handles, the `soundlayer`
module), **S36 M1 and M2** (waveform cache, the analysis' `peak_ms` and trims for alignment) and
**S35 M3** (captures prove the sample placement). Runs in parallel with S39. Owner area:
`engine/core/animation/ClipAudio.*`, `AnimationClipIO.*` (the `audio` key), `KeyframeSampling.*`
(cue crossing), `engine/core/subsystem/ActionSystemPlayers.cpp` and
`engine/core/animation/AnimatorInstance.cpp` (firing), `editor/window/AnimationWindowClipAudio.cpp`,
`editor/animation/AnimationClipDocument.*` (cue edits), `editor/audio/TimelineAudioPreview.*`,
`editor/mcp/tools/WorldTools.cpp` (`edit_animation`/`get_animation` cue keys), the Timeline audio
section of `docs/mcp/guide/sound.md` and `docs/animation-clips.md`.

## 1. Goal

Sound lines up with animation without scripts, as Unity's Timeline audio tracks and animation
events do. An animation clip (`.anim`) holds **audio cues** on an Audio lane: at a time in the
clip, play a clip or a Sound Event (2D or following a bound GameObject), start an AudioSource of
the animated hierarchy (so the clip's existing `audiosource` property tracks shape it), or move the
mixer to a snapshot. The Animation Player, the Animator (S21) and montages fire cues with the same
crossing rules as clip events, placed to the sample inside the frame. The Animation window shows
the clip's waveform in the lane, aligns a cue's start, peak or end to the playhead, and plays the
cues in its preview. Agents author cues with `edit_animation`.

## 2. Scope

**In scope**
- The `.anim` `audio` array (cue kinds `sound`, `source`, `snapshot`), its version rule, crossing
  and firing for the Animation Player, the Animator (layers, blend trees, montages), sample
  placement, stop rules.
- The Animation window's Audio lane in clip mode, alignment, drag and drop, preview playback, undo.
- `edit_animation` and `get_animation` cue keys; capture lines.

**Out of scope**
- The legacy action timeline (`ActionComponent` frames of an `AnimationComponent`, sound-layer
  phase 6's "audio actions", G-31): see Q1.
- `AudioSnapshotComponent` (user decision 10's component): snapshot cues need no component, and the
  256-bit signatures made the slot question moot.
- Scrubbing audio (scrubbing stays silent), audio in the Animator window's graph preview, vertex
  animation (`.vat`) and pose animation clips.

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **What exists for audio in clips (upstream anim-properties).** Property tracks can animate an
  AudioSource's `volume`, `speed` (pitch) and `pan` and its enabled flag
  (`engine/core/animation/PropertyBindings.cpp:566-570`, applied to the playing voice by
  `AudioSystem` through `needUpdate`). Nothing starts a sound at a clip time: the only way is a clip
  **event** (`ClipEventData {time, function, args}`, `engine/core/animation/AnimationClip.h:117-121`)
  calling a Lua method that plays it, which is frame-quantized and needs code.
- **Event crossing.** `anim::collectCrossedEvents` (`KeyframeSampling.h:123-136`) returns each
  event crossed in an update with its position on the state's unwrapped time line (forward,
  backward, loop and ping-pong wraps, once per crossing). The Animation Player pends them with a
  sub-frame time `when(position)` (`engine/core/subsystem/ActionSystemPlayers.cpp:522-583`); the
  Animator collects them per clip slot and layer (`AnimatorInstance::fireEvents`,
  `AnimatorInstance.cpp:1159-1173`) with S21 R9's leader rule.
- **The file.** `.anim` is JSON `{"format": "jasperengine.anim", "version": 1|2}`
  (`AnimationClipIO.h:55` `ANIM_VERSION = 2`; version 2 only for humanoid clips,
  `AnimationClipIO.cpp:1994-1996`); unknown top-level keys are kept and written back
  (`knownTopLevelKeys`, `:1754-1755`); a version above `ANIM_VERSION` is refused (`:1815-1817`).
- **The editor.** Upstream's Animation window clip mode has a dopesheet, curves view and an
  events lane (`editor/window/AnimationWindowClipEvents.cpp`); edits go through
  `AnimationClipDocument`. MCP: `edit_animation` (core, `WorldTools.cpp:5673-5729`, at its
  `budget.json` exception of 3,606 characters) and `get_animation` (`:5731`).
- **sound-layer** built timeline audio only on the legacy action timeline (`snd/p6-engine`
  7281b47c7: `ActionSystem` +233, `playFromAction`, sub-frame `actionDelaySamples`), never merged;
  its Animation window Audio track (P6 §2.3) was never built (`snd/p6-timeline` has no ref).

## 4. Reference: sound-layer (`snd/p6-engine` 6d51b7d29; spec P6 §2.2-2.3)

| What | Where | Reuse / change / drop |
|---|---|---|
| Sub-frame start placement | 7281b47c7 (`actionDelaySamples`), P6 §2.2.3 | **Reused**: delay = the crossing's fraction of the frame × the frame's unscaled seconds × the output rate, through `setDelaySamples` (S37 R7). Moved from action frames to clip crossings. |
| Length from the clip | 7281b47c7 `clipLengthSeconds`, `SoundPool::probeLength` | **Reused** for the lane's block length and "Align End". |
| Audio track UI: waveform in the block, Align Start/Peak/End to Playhead, preview voices | P6 §2.3.1, §2.3.3, §2.3.4 (`TimelineAudioPreview`) | **Reused as design**, re-hosted on upstream's clip-mode lanes and `AnimationClipDocument`. |
| Mixer Snapshot actions, `AudioSnapshotComponent` | P6 §2.2.1, decision 10 | **Changed** to `snapshot` cues (`AudioMixer::transitionTo`), no component. |
| Audio actions on `ActionComponent` frames | P6 §2.2.1-2.2.2, `ActionSystem` hooks | **Dropped** here (Q1). |
| Capture lines (`voice_start` cause `action`, `frame_index`) | P6 §2.2.4 | **Changed** to `cue` lines and cause `animation`. |
| Tests | `test_audio_timeline.py` (T-1…T-11), `audio-timeline` fixture "Footsteps" | Scenarios reused (four footsteps on predicted samples, loop wraps, speed 2, pause/stop), rewritten for clips. |

## 5. Requirements

### Functional — the data

- **R1. The `audio` array** of an `.anim` (top-level, after `events`, written only when not
  empty), sorted by `time`:

  ```json
  "audio": [
    {"time": 0.25, "sound": {"guid": "…", "path": "sounds/step.soundevent"}, "path": "Body/LeftFoot",
     "spatial": true, "volume_db": -2, "pitch_semitones": 0, "group": "SFX", "stop": "none", "name": "Left step"},
    {"time": 0.0, "source": "Body/Engine", "stop": "state_exit"},
    {"time": 1.0, "snapshot": "Build", "seconds": 0.5}
  ]
  ```

  Exactly one of:
  - `sound`: an `AssetRef` to an audio clip or a `.soundevent`, played as an S37 free voice; with
    `spatial: true` it follows the GameObject at `path` (relative to the player's root, "" = the
    root; the binding grammar of tracks), else 2D; `volume_db` (−60…12, default 0),
    `pitch_semitones` (−24…24, default 0), `group` (a mixer group name; default the event's, else
    Master);
  - `source`: a path to a GameObject of the hierarchy with an AudioSource: the cue calls that
    source's `play()` (its clip or event, its group and 3D settings), so the clip's
    `audiosource.volume/speed/pan` property tracks on the same source shape the voice;
  - `snapshot`: a snapshot name of the project's mixer and `seconds` (default 0), `unscaled`
    (default: the mixer's update mode): `AudioMixer::transitionTo` at the cue.

  Common keys: `time` (s, ≥ 0), `stop` (`none` default: plays to its end; `state_exit`: stopped
  with a 10 ms fade when the state that fired it stops, is left or finishes its crossfade out; a
  `source` cue stops the source), `follow_speed` (bool, default false: when true the voice's pitch
  also scales with the state's speed), `name` (optional label). A cue's `time` may exceed the
  clip's keys: it extends `effectiveLength()` as an event does.
- **R2. Version.** A clip with cues is written as **version 3** (`ANIM_VERSION` becomes 3);
  clips without cues keep version 1 or 2 byte-identically (the humanoid precedent). An older engine
  refuses a version-3 clip with its own message; S30's version lock keeps exports consistent.
  `AnimationClipIO` parses, validates (one sound/source/snapshot, ranges, a `path` that is a valid
  binding path) and writes the array; invalid cues are dropped with a warning per cue on load, as
  events are.
- **R3. References.** `sound` refs are S04 dependency edges of the clip (`field` `audio[i].sound`)
  and are relinked on rename by `AnimationClipDocument` (its `remapRef` path,
  `AnimationClipDocument.cpp:498`); a missing one shows in S04's Missing panel.

### Functional — firing

- **R4. Crossing.** `anim::collectCrossedCues` (beside `collectCrossedEvents`, same rules: forward
  `from < t ≤ to`, backward `to ≤ t < from`, `includeStart` for a state that just started, loop
  and ping-pong boundaries split the interval, each cue at most once per update, with its position
  on the unwrapped time line). Every site that fires clip events fires cues with the same inputs:
  - the Animation Player (`ActionSystemPlayers.cpp:577-583` and the queued-state path `:1445`):
    every playing state fires its cues, as its events;
  - the Animator (`AnimatorInstance::fireEvents` and the montage path `:1340`): S21 R9's rules
    (per layer, `LeaderOnly` by default: the highest-weight clip), montages as S21 R8 fires events;
  - edit-mode previews fire nothing (they never run game code), except R11's editor preview.
- **R5. Placement.** A crossed cue's start is delayed inside the frame by `round(f × unscaledFrame
  × rate)` output frames, where `f` is the crossing's fraction of the frame (the player's
  `when(position)`, `ActionSystemPlayers.cpp:522-524`; the Animator's equivalent from `from`, `to`
  and `position`) and `unscaledFrame` the frame's unscaled seconds (S35 R3's value), clamped to
  the frame. Under the deterministic driver the voice's first sample is exactly there
  (`latency_samples` aside, S35 R8); with a device within one device period. Time scale 0 crosses
  nothing; a state's speed and the time scale are already in `f`.
- **R6. Starting.** `sound` cues go through S37's `Audio::play` path (its limits, RNG, the
  event's own ranges, then the cue's `volume_db`/`pitch_semitones` on top); `source` cues through
  `AudioSource::play()` with the delay; `snapshot` cues call `AudioMixer::transitionTo` in the
  frame (no sub-frame delay: a fade has no use for one). A cue whose sound does not resolve, whose
  `path` binds nothing or whose snapshot is unknown does nothing, with one warning per cue per
  Play (`Audio: cue 2 of anim/run.anim: Body/LeftFoot binds nothing`).
- **R7. Ownership and stops.** Cue voices belong to the AudioSystem of the player's scene (S37
  `free` voices): they stop with the scene (reason `scene`), with `stop: state_exit` as R1 says
  (reason `cue_exit`), and with `Audio.stopAll`. Pausing the player's scene pauses them; an
  Animation Player's `stop()` applies `state_exit` to its states' cues.

### Functional — capture

- **R8.** With S35: a `cue` line per fired cue (`clip`, `cue` index, `name`, `kind`, `player`
  (entity path), `state`, `layer`, `position`, `delay_samples`), then the `voice_start` with owner
  `cue`, cause `animation`, the same sample plus `delay_samples`; `snapshot` cues write S35's
  `mixer transition` line with `cause: "animation"`; S35's `animation_event` markers and cue lines
  share the frame's sample.

### Editor UX (human)

- **R9. The Audio lane** (`editor/window/AnimationWindowClipAudio.cpp`) in clip mode, under the
  events lane: one row per cue kind in use; a `sound` or `source` cue is a block as long as its
  sound (S36's analysis duration at speed 1, the source's clip or event: longest clip, layer
  delays included) showing S36's cached waveform min/max between its edges (an event: its first
  clip under a stacked outline; a `follow_speed` cue scaled by the state speed preview), a peak
  tick at the analysis' `peak_ms` (trim head taken off); a `snapshot` cue is a flag with its name.
  New theme colours `Theme::Colors::AnimationAudio` and `AnimationSnapshot`.
- **R10. Editing.** Drag to move (snapped to the clip's frame grid; Alt for free), Delete, a
  popover with R1's fields; drop an audio file or `.soundevent` from the Project window or the Sound
  Browser (S37; built-in cards are imported first) at the drop time; Add Property gains Audio Cue,
  Source Cue and Snapshot Cue. Context menu and keys (`Animation` context): **Align Start to
  Playhead** (`time = t`), **Align Peak to Playhead** (`time = t − (peak_ms − head_ms)/1000`; an
  event: its first clip's, plus its `delay_ms` in `layer` mode; disabled with a tooltip when the
  result is below 0), **Align End to Playhead** (`time = t − length`). Every edit is one
  `AnimationClipDocument` command named per S24 ("Move Audio Cue", "Align Peak to Playhead"…) and
  saved with the clip.
- **R11. Preview.** While the clip-mode preview **plays**, `TimelineAudioPreview` starts an editor
  preview voice for each cue the playhead crosses (R4's rule on the preview's time), at the offset
  inside the frame, and stops `state_exit` cues when the preview stops, loops or is sought; snapshot
  cues do nothing in the preview (preview voices are outside the mixer's groups); scrubbing is
  silent; off while S35 captures (the same tooltip as the Inspector preview).

### Agent UX (MCP, Lua, CLI)

- **R12. `edit_animation`** (clip mode) gains `audio` (an array of R1 objects; `sound` by path or
  GUID, `source`/`path` by binding path) applied with the existing `events_mode` (`replace` or
  `merge` by `time` and kind), validated before writing, one undo step as today. `get_animation`
  returns `audio[]` with resolved paths and each cue's length (result only).
- **R13. Lua.** No new API: cues fire by themselves; Lua plays sounds with S37's `Audio.play` and
  reacts to clip events as today. `docs/mcp/guide/lua-api.md` says so in the Animation section.
- **R14. Docs.** `docs/animation-clips.md` (the `audio` array, version 3, firing rules),
  `sound.md` "Timeline audio" (recipes: footsteps with a Sound Event and Align Peak, an engine loop
  shaped by property tracks through a `source` cue, a "build" snapshot), `docs/unity-glossary.md`
  (Timeline Audio Track / animation events playing sounds → audio cues).

### Performance budgets (server F, Release)

- **R15.** Crossing cues costs nothing for clips without cues (an empty-array check per update);
  200 Animation Players each crossing one cue per frame ≤ 0.3 ms per frame above the same scene
  without cues (`AnimBench`-style bench).

### Data & compatibility

- **R16.** Clips without cues, every existing scene and controller: byte-identical (R2). Property
  tracks and events are unchanged and keep working beside cues.

## 6. Design constraints & decisions

- **D1. Cues on clips, not on a new timeline asset.** The `.anim` is the timeline the Animation
  Player, Animator and montages already share; one crossing rule serves all three.
- **D2. Three kinds cover the asks.** `sound` (what Unity's Timeline audio clip does), `source`
  (builds on upstream's `audiosource` property tracks), `snapshot` (sound-layer's "build" and
  "dead stop" without `AudioSnapshotComponent`).
- **D3. Placement reuses the event's sub-frame position** (`when(position)`), so events and cues of
  one crossing agree.
- **D4. Version 3 only with cues**: an older engine cannot silently drop sounds a designer placed.
- **D5. No Lua surface**: cues are data; scripts already have S37's API.

API sketch:

```cpp
namespace jasperengine {
  enum class ClipCueKind : uint8_t { Sound, Source, Snapshot };
  enum class ClipCueStop : uint8_t { None, StateExit };
  struct ClipAudioCue { float time = 0; ClipCueKind kind = ClipCueKind::Sound; AssetRef sound; std::string path, group, snapshot, name;
                        bool spatial = false, followSpeed = false; std::optional<bool> unscaled;
                        float volumeDb = 0, pitchSemitones = 0, seconds = 0; ClipCueStop stop = ClipCueStop::None; };
  // ClipData::audio, AnimationClip::getAudioCues(); anim::collectCrossedCues(...) mirrors collectCrossedEvents
}
```

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `edit_animation` | core | `audio` array (`time`, `sound`, `source`, `snapshot`, `path`, `spatial`, `volume_db`, `pitch_semitones`, `group`, `stop`, `seconds`); field meanings in `jasper://docs/animation` | +190 (exception 3,606 → 3,800 in `budget.json` with the reason "S40 audio cues"), offset by moving `edit_animation`'s humanoid sentence (~150 characters) to `jasper://docs/animation` when the default listing would pass 100,000 |
| `get_animation` | core | result only | 0 |

Group `audio`: unchanged (S35 §6.1). The instructions' "Clip property tracks" line gains "; audio
cues: audio (jasper://docs/sound)" (+~35 bytes).

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `*.anim` | `jasperengine.anim` v3 when it has `audio` cues (v1/v2 otherwise, unchanged) | upstream's `AnimationClipIO` (engine) |

### 6.3 Settings

None.

### 6.4 Editor commands (S25 CommandCatalog, context `Animation`)

| Id | Name | Default key |
|---|---|---|
| `animation.alignCueStart` | Align Start to Playhead | none |
| `animation.alignCuePeak` | Align Peak to Playhead | Shift+A (`A` is Frame All Keys, `DefaultCommands.cpp:353`) |
| `animation.alignCueEnd` | Align End to Playhead | none |
| `animation.addAudioCue` / `addSourceCue` / `addSnapshotCue` | Add Audio / Source / Snapshot Cue | none |

`check-hardcoded-shortcuts.py` passes.

### 6.5 Lua API

None (R13).

### 6.6 Player size (S34)

Cue parsing is part of `AnimationClipIO` (core, a few KB); playback is in S37's `soundlayer`
module (estimated 6–10 KB of it). Detection: a shipped `.anim` with an `audio` array needs the
module (S37 §6.6). A clip with cues in a build without the module is a build error naming the clip
(it cannot happen with detection on; with Strip Engine Code off every module is in).

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1–R3) | `engine/tests/core/animation/ClipAudioTest.cpp`: round trip of R1's example as version 3; a clip without cues stays version 1/2 byte-identical; validation drops invalid cues with warnings; a version-3 file is refused by the base's reader (fixture string). `test_animation_clip.py::test_cue_relink`: renaming the event relinks the cue in the same undo step. |
| AC2 (R4) | `ClipAudioTest.Crossing`: the event-crossing test table (forward, backward, loop, ping-pong, `includeStart`) applied to cues gives identical results to events at the same times. |
| AC3 (R5, R6, R8) | `tools/mcp/tests/test_audio_timeline.py` (S35 deterministic captures, `fixed_fps 60`): a "Footsteps" clip with four `sound` cues at 0.1, 0.35, 0.6, 0.85 s on an Animation Player: four `cue` lines and `voice_start`s on the samples `round(t × 48000) + latency` (± 0); at speed 2 and time scale 0.5 the samples follow; a loop wrap fires the cue at 0.1 s once per pass; an Animator with a 1D blend tree fires only the leader's cues (S21 R9); a montage fires its clip's cues. |
| AC4 (R6, R7) | `test_audio_timeline.py`: a `source` cue starts the hierarchy's AudioSource and its `audiosource.volume` track fades it (S35 levels follow the curve within 0.5 dB); `state_exit` stops on a crossfade to another state (reason `cue_exit`); a `snapshot` cue logs the transition in its frame; an unresolved cue warns once. |
| AC5 (R9–R11) | `tools/mcp/tests/test_animation_audio_editor.py` (xvfb, isolated editor): the lane draws a waveform block; drop an event creates a cue at the drop time in one undo step; Align Peak with the playhead at 1.0 s puts the clip's analysed peak on 1.0 s (`get_animation` reads it back); the playing preview creates preview voices and none in a capture; screenshots in the evidence. |
| AC6 (R12, §6.1) | `test_animation_clip.py::test_edit_audio_cues` (replace/merge, refusal of two kinds in one cue, undo); `test_tool_budget.py` with the raised exception and the default listing ≤ 100,000; `gen_tool_docs.py --check`. |
| AC7 (R15, R16) | The bench within R15; `test_runtime_regressions.py` and the S21 suites unchanged; old clips re-save byte-identical. |

## 8. Dependencies & conflicts

- **Depends on.** S37 (free voices, events, `soundlayer`), S36 (waveforms, `peak_ms`, trims,
  durations), S35 (captures), S21 (Animator, montages, the leader rule), upstream's clip system and
  anim-properties, S04 (edges, Missing panel), S24 (named commands), S25 (catalog), S31 (budget).
- **Coordination.** S21's owners for `AnimationClipIO`, `AnimatorInstance` and the Animation
  window (merge hotspots: `AnimationClipIO.cpp`, `KeyframeSampling.*`, `ActionSystemPlayers.cpp`,
  `AnimatorInstance.cpp`, `AnimationWindowClip*.cpp`, `WorldTools.cpp`, `budget.json`).

## 9. Risks & defaults

- **Core listing growth** (+190): default: §6.1's offset keeps the default listing within 100,000.
- **Many cues in crowds** (200 characters × footsteps): default: S37's instance limit and
  upstream's budget bound voices; R15 bounds the firing cost.
- **Device-mode jitter** (one device period): default: documented; offline exact.

**Open questions for the user**
- **Q1. The legacy action timeline.** sound-layer put timeline audio on `ActionComponent` action
  frames (`AnimationComponent`'s timeline, G-31), which upstream's clips and S21 now supersede for
  new content. Port those audio actions too? **Recommendation: no**: clips are the Unity-like
  timeline that the Animator and montages share; a legacy animation can play a sound through an
  action's Lua or an AnimationEvent, and porting would add a second set of rules and UI.

## Milestones

- **M1** Data: the `audio` array, version 3, validation, edges and relinking (R1–R3); AC1.
- **M2** Firing in the Animation Player, Animator and montages, placement, stops, capture lines
  (R4–R8); AC2–AC4.
- **M3** The Audio lane, editing, alignment, preview (R9–R11); AC5. Needs S36 M1/M2.
- **M4** `edit_animation`/`get_animation`, docs, budgets (R12–R16); AC6, AC7.

## Deviations

None yet (spec-first stream; implementers append here).
