# S39 — Music system: tracks and playlists, tempo, bars and beats, loops, transitions, stingers

**Wave / Depends on.** Sound wave B (spec-first: implemented after the integration branch lands in
`main`). Depends on **S37 M2 and M4** (Sound Events as playlists, free voices, the `Audio` table,
the `soundlayer` module), **S36 M2 and M3** (the audio importer section and `ClipSource`: loop
regions, trims, gains) and **S35 M3** (deterministic captures prove sample placement in the tests).
Runs in parallel with S40. Owner area: `engine/core/audio/MusicPlayer.*`,
`MusicSequenceSource.*`, `engine/core/subsystem/AudioSystem.*` (the music system),
`engine/core/object/sound/Audio.*` (music functions), `engine/core/script/binding/AudioClassesLua.cpp`,
`editor/audio/AudioMusicGrid.*`, the music parts of `editor/window/InspectorAudioClip.cpp` and
`InspectorSoundEvent.cpp`, the Music section of `docs/mcp/guide/sound.md`.

## 1. Goal

A game plays music the way a Unity game does with a music manager, without writing one: tracks
(a clip, or a Sound Event used as a playlist of sections) play gaplessly through their loop
regions, switch with an equal-power crossfade immediately or on the next beat, bar or section
end, optionally moving the mixer to a snapshot at the same moment; stingers land on the next beat
or bar; scripts read the bar and beat and get a callback on each. Music plays as a voice of
upstream's mixer, so groups, ducking, exposed parameters and snapshots apply to it unchanged, and
it survives scene loads. A clip's tempo is set in its `.meta` or detected by the editor.

## 2. Scope

**In scope**
- The music system (one process-wide player), tracks, sections, loop passes, playlists, crossfades,
  quantized switches, snapshot transitions with a switch, stingers, beat and section callbacks.
- Tempo keys in S36's `import.audioImporter` (`music`), the `.soundevent` clip key `loops`.
- Lua and C++ API in the `Audio` table; capture lines for S35.
- Editor: tempo fields and a beat-grid overlay in the clip view, `loops` in the event view, tempo
  and downbeat detection (`AudioMusicGrid`), which sound-layer specified (P6 §2.4.8) but never built.

**Out of scope**
- Vertical layering of stems (a `layer` event cannot be a track: refused); adaptive music graphs
  beyond playlists; time-stretching to a tempo; MIDI.
- Positional (3D) music; per-scene music settings (the `.mixer` start snapshot and Lua cover it).
- Music in edit mode (no edit-time music preview beyond the clip view's audition).

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **No music system.** Music is an AudioSource like any other: upstream's port of the game's
  `MinigameSongs` uses snapshots and `setFloat` sweeps (AM §3); a track change is a script stopping
  one source and starting another, with no gapless hand-over, beat grid or quantization.
- **What it can build on.** Upstream's mixer (groups, `duck`, snapshots with `transitionTo`,
  weighted blends, exposed parameters: `engine/core/audio/AudioMixer.h:99-121`), the voice budget
  with protected voices (AM A11), streaming of large files (AM A8, `EngineWavStream`), persistent
  scenes moving their voices (`AudioSystem::moveSourceVoices`, `AudioSystem.cpp:296`). With S36,
  `ClipSource` plays loop regions; with S37, events pick clips with a seeded RNG.
- **sound-layer's state.** Only the music API skeleton reached the branch tip (M1, d9104244c,
  `MusicPlayer.cpp` 64 lines); the body is on `snd/p6-music` (6de26f04d, 17 commits ahead), never
  merged; tempo detection (`snd/p6-tempo`) was never started.

## 4. Reference: sound-layer (`snd/p6-music` 6de26f04d; tip 20b461161)

### 4.1 Reused (adapted)

| What | Where (commit, file) | Change and why |
|---|---|---|
| Music player and sequence source | d0b19be4e `engine/core/audio/MusicPlayer.{h,cpp}` (677), `MusicSequenceSource.{h,cpp}` (211); M1 d9104244c; 41b3cbcf9 (the music system's line writer, one instance per event track voice) | Bus → upstream group; the "scene-less `AudioSystem`" design kept; sections read S36's `ClipSource`; quantized switches and snapshot options added. |
| Tempo keys and `loops` | 879acc2e7 (`.import` `music:` block, event key `loops`), P6 §2.4.1 | `.import` → `.meta` `import.audioImporter.music`; `loops` added to `.soundevent` v1 (JSON). |
| Beat grid, stingers, crossfade | P6 §2.4.3-2.4.4 | As specified there (equal power, grid formula, boundary, delay); the latency difference term uses upstream groups (they all mix at the output rate). |
| Music API and Lua | d0b19be4e (`Audio.playMusic/stopMusic/advanceMusic/playStinger/getMusic/getMusicTime`) | An options table for `playMusic` (S37's style); `getMusicPosition`, `onMusicBeat`, `onMusicSection` added. |
| Tempo fields in the Browser | 06727c163, 9128cd18a | In S36's clip view, not the Browser. |
| Tempo and downbeat detection | P6 §2.4.8 (Ellis 2007 beat tracker; earshot idea, reimplemented) | Built here for the first time. |
| Tests and fixture | `test_audio_music.py` (M-0…M-9, 3d9601079…8d28dabcb), `tools/fixtures/audio-timeline` Music scenes, `MusicDriver.lua`, `playlist.soundevent` (68824c202) | Rewritten on S35 captures and S37 events; fixture regenerated on target. |

### 4.2 Changed on purpose

- **Quantized switches.** sound-layer made a switch immediate and only stingers quantized (P6 §1
  out of scope); "transitions on bars and beats" are asked for, so a switch takes the same `sync`.
- **Snapshots with a switch.** `playMusic(..., {snapshot})` calls upstream's
  `AudioMixer::transitionTo` on the switch's boundary, so a "combat" mix and its music arrive
  together without a script timer.

### 4.3 Dropped

The protected "Music bus" of sound-layer's mixer (a track routes to an upstream group, R6), the
`AudioCallQueue` deferral (S37's main-thread rule covers physics callbacks), the Browser tempo
fields (S36's clip view has them).

## 5. Requirements

### Functional — model and keys

- **R1. Tracks and sections.** A track is what `Audio.playMusic(x)` names: a clip path, or a Sound
  Event (GUID or path) whose clips are its sections, picked by its mode (`sequence` in order,
  `shuffle`/`random` with S37's picker and RNG, one accepted play per section); `layer` is refused
  (false and one warning). A clip track has one section with `loops = 0`.
- **R2. A section plays** from its trimmed start to its loop end, repeats its loop region (S36
  `loop_start_ms`/`loop_end_ms`; without one, the whole trimmed clip) until it has played the
  region `loops` times (counting the first; `0` = until `Audio.advanceMusic()`), then its tail
  (loop end to trimmed end) and hands over to the next section **at the next sample**. A playlist
  with the event's `loop: true` continues with the next pick (sequence: the first clip again);
  otherwise the track ends after its last section.
- **R3. Keys.**
  - `.soundevent` clip key `loops` (0–1,000, default 1, written only when not 1; ignored unless the
    event plays as music), additive within `jasperengine.soundevent` v1 (S37's reader keeps
    unknown keys, so older engines ignore it). S37's validation and problems gain "music sections
    differ in sample rate or channels" and "a music section is shorter than 100 ms" for events of
    type `music`.
  - S36's `import.audioImporter` gains `music: {bpm, beats_per_bar, first_beat_ms}` (written only
    when `bpm` > 0; `bpm` 20–400, `beats_per_bar` 1–16 default 4, `first_beat_ms` ≥ 0 from the
    file's start, before trims), additive within version 1. `ClipImportSettings` carries them as
    `bpm`, `beatsPerBar`, `firstBeatFrame` (S36's struct reserves them) and S36's export table
    writes them when `bpm` > 0. They never change samples.

### Functional — the music system

- **R4. One player.** `MusicPlayer` (`engine/core/audio/MusicPlayer.{h,cpp}`, main thread) owned
  by a process-wide `AudioSystem` with no scene (sound-layer decision 6: created on first use,
  never updated by a scene, seen by `stopAll`, pause rules and the voice tables like any system).
  It survives every scene load. In the editor it is reset (its voices stopped, reason `scene`) at
  Play start and at Play stop, so edit-time `eval_lua` music never leaks into a capture; in a game
  at exit.
- **R5. `MusicSequenceSource`** (a `SoLoud::AudioSource`, one per playing track): its instance (the
  audio thread) reads the current section's `ClipSource` instance, counts passes, plays tails and
  switches to the next section's instance at the next frame. The main thread prepares the next
  section (pick, `ClipSource`, instance, gain) when the current one starts and hands it over
  through a single-slot atomic; the instance reports `{section, firstFrame}` through a lock-free
  single-producer queue that `MusicPlayer::update` drains. Channels and rate are the first
  section's; a mismatching section refuses the track with a warning naming the clip.
- **R6. Its voice.** One voice per track: owner `music`, cause `music`, protected
  (`protectVoice`), priority 0, routed to `opts.group`, else the event's `output`, else
  `audio.music_group` (§6.3; empty = Master), with upstream's group fallback rule (AM A3). Music
  needs no rule of its own in the mixer: ducking, faders, effects, exposed parameters and snapshots
  apply to its group as to any voice. Pause: the app pause (Master bus) always; `AudioListener.pause`
  unless `opts.ignoreListenerPause` (default false, as upstream's AudioSource); the editor's Play
  pause holds it.
- **R7. Gains.** Each section's S36 gain and its event clip `volume_db`; the event's `volume_db`
  and `pitch_semitones` ranges are drawn once per `playMusic` for the whole track; the track
  ignores the event's `cooldown_ms`, `max_instances`, delay range and `spatial` (music is 2D).

### Functional — time, transitions, stingers

- **R8. Beat grid** of the current section (sound-layer P6 §2.4.4): `framesPerBeat = rate × 60 /
  bpm`; beat k at section frame `f_k = firstBeatFrame − trimHeadFrames + k × framesPerBeat`,
  linear across passes (one warning per clip per Play when the loop region is not a whole number of
  beats: "loop region is 7.93 beats at 120 bpm: stingers will drift"); a bar is a beat with
  `k mod beatsPerBar == 0`. The music position P (section frames the next mix plays first) is the
  voice's stream position × rate minus the frame the section began at.
- **R9. Boundaries and delay.** For `sync` `beat` or `bar`: B = the first grid frame of that kind
  ≥ P; for `section_end`: the current section's next loop end (or its end); delay = round((B − P) ×
  outputRate / (rate × musicSpeed)), clamped at 0. No music, or no `bpm` for `beat`/`bar`: `now`
  with one warning per name per Play. In deterministic runs (S35) the new sound's first sample is
  exactly at B's output sample; with a device, within one device period.
- **R10. Switches.** `playMusic(x, opts)` while a track plays:
  - `crossfade` T ≥ 0 s (default 0): from the switch point the outgoing voice's volume follows
    `g0 · cos(π/2 · t/T)` and the incoming one's `g1 · sin(π/2 · t/T)` over unscaled time (set
    every frame by `MusicPlayer::update`, segment by segment with SoLoud's `fadeVolume`); then the
    outgoing voice stops (reason `music`). T = 0 cuts at the switch point.
  - `sync` `now` (default), `beat`, `bar`, `section_end`: the switch point is R9's boundary of the
    playing track; the incoming voice is started with that delay and the crossfade starts there.
  - `snapshot` (a snapshot name) and `snapshotSeconds` (default T): `AudioMixer::transitionTo`
    (`unscaled` as the mixer's update mode says) is called in the frame whose mix contains the
    switch point, so the mix and the music change together within one frame.
  - Naming the playing track (and not fading out) does nothing and returns true; a third voice is
    never kept: a new switch during a crossfade makes the incoming track the outgoing one from its
    current volume and stops the oldest at once.
- **R11. Stop and advance.** `stopMusic(fade [, opts])`: linear fade then stop (0: at once), with
  `opts.sync` as R9 for the moment the fade starts. `advanceMusic()`: the current section ends its
  loop at the next loop end (an atomic flag the instance reads); false without music.
- **R12. Stingers.** `playStinger(x [, sync])` (`now`, `beat` default, `bar`) plays a clip or event
  as an S37 free voice **owned by the music system** (a stinger asked for just before a scene load
  is not cut), routed like R6 unless its event has its own `output`, delayed per R9, and returns its
  `SoundHandle`. The current track for the grid is the incoming one during a crossfade.
- **R13. Callbacks.** `Audio.onMusicBeat` (`bar`, `beat`; beat 1-based, bar 1-based from the
  section start) and `Audio.onMusicSection` (`section`, `clip`) fire on the main thread in the
  frame whose mix contains that beat or section start, in order, each once (a frame that mixes
  several beats fires them all). They are frame-accurate, not sample-accurate; the guide says so.
- **R14. Queries.** `getMusic()` (the playing track, the incoming one during a crossfade, "" none),
  `getMusicTime()` (seconds of the track played so far, all sections), `getMusicPosition()` →
  `{section, bar, beat, beatFraction, bpm, beatsPerBar}` or nil without music or tempo.
- **R15. Capture lines** (S35's `jasperengine.audio-events` v1): kind `music` with `action` `play`
  (`music`, `from`, `fade_s`, `sync`, `boundary_sample`), `section` (`section`, `clip`,
  `starts_at_sample`), `crossfade_end`, `stop` (`fade_s`), `end`, `stinger` (`clip`, `sync`,
  `boundary_sample`), `advance`; the switch's `mixer transition` line carries `cause: "music"`.
  `voice_start` with owner `music`. S35's `silence` check counts music voices.

### Functional — tempo detection (editor)

- **R16. `AudioMusicGrid`** (`editor/audio/AudioMusicGrid.{h,cpp}`): sound-layer P6 §2.4.8 exactly:
  onset strength from S36's STFT (2048/512) with log compression over 30 Hz–16 kHz (and 30–200 Hz
  for downbeats); tempo by autocorrelation with a log-normal prior around 120 BPM (40–240) and the
  alternatives ×2, ×½, ×3/2, ×2/3; Ellis' dynamic-programming beat tracker; beats moved onto the
  largest envelope rise within ±1/8 beat and a least-squares line for `bpm` (0.01); downbeat phase
  for 3 and 4 beats per bar from the low band; `loop_beats` and a `beat_drift` note; "too short"
  under 4 s, no pulse under clarity 0.1, `confident: false` under 0.3; cached as
  `<cache>/audio/<sha1>-grid-v1.json`; a pure function of the file's bytes.
- **R17. `analyze_audio`** adds `music {bpm, bpm_alt, first_beat_ms, beats_per_bar_guess,
  downbeat_confidence, pulse_clarity, confident, loop_beats, candidates[], current, agrees, fix,
  notes}` to items whose kind is `loop` or type is `music` (P6 §2.4.8's output; `fix` is the
  `settings.music` object to pass to `set_audio_import_settings`).

### Editor UX (human)

- **R18.** S36's Inspector clip view shows BPM, Beats per Bar and First Beat in the Import
  Settings, a **Detect** button (R16 on a Background job, then the beat grid drawn over the
  waveform: beats as ticks, bars tall, the second candidate in another colour when not confident)
  and **Use** (writes `fix` as one `SetAssetMetaCmd`, "Set Tempo from Detection"). Nothing is
  written before a click. S37's event view shows a `Loops` column when the event's type is
  `music`, and a "Play as Music" audition (the playlist through an editor preview of the sequence
  source).

### Agent UX (MCP, Lua, CLI)

- **R19. Lua and C++** (`Audio`, under `JASPERENGINE_SOUND_LAYER`):

  | Lua | C++ | |
  |---|---|---|
  | `Audio.playMusic(x [, opts])` → boolean | `static bool playMusic(const std::string&, const MusicOptions&)` | opts `{crossfade, sync, snapshot, snapshotSeconds, group, volume, ignoreListenerPause}` |
  | `Audio.stopMusic([fade [, opts]])` | `static void stopMusic(float fade = 0, const std::string& sync = "now")` | |
  | `Audio.advanceMusic()` → boolean | `static bool advanceMusic()` | |
  | `Audio.playStinger(x [, sync])` → SoundHandle | `static SoundHandle playStinger(const std::string&, const std::string& sync = "beat")` | |
  | `Audio.getMusic()`, `getMusicTime()`, `getMusicPosition()` | getters | |
  | `Audio.onMusicBeat`, `Audio.onMusicSection` | `FunctionSubscribe<void(int, int)>`, `<void(int, std::string)>` | static properties, bound as `Engine.onUpdate` is (`CoreClassesLua.cpp:2195`) |

  Non-finite or negative times and unknown `sync` names are refused with one warning; calls inside
  a physics callback defer to the frame's audio update (S37 R13).
- **R20. MCP.** No new tool: tempo through `set_audio_import_settings` (`settings.music`), `loops`
  through `edit_sound_event`, detection through `analyze_audio`, control through `eval_lua`;
  `get_play_state.audio` adds `music {track, section, time_s, bar, beat, bpm}` (result only);
  `audio_session report` lists the `music` lines.
- **R21. Docs.** `sound.md` "Music": tracks and sections, loop regions and `loops`, tempo and
  detection (how to read `confident` and the candidates), switches and their `sync`, snapshots with
  a switch, stingers, callbacks and their frame accuracy, a recipe for menu ↔ gameplay music;
  `lua-api.md` (lua blocks run by S23's doc test); `docs/unity-glossary.md` (a music manager →
  the music system).

### Performance budgets (server F, Release)

- **R22.** `MusicSequenceSource` adds ≤ 2 % to the mix cost of a plain streamed voice; a section
  hand-over allocates nothing on the audio thread; `MusicPlayer::update` ≤ 20 µs per frame;
  `AudioMusicGrid` of a 3-minute track ≤ 1.5 s.

### Data & compatibility

- **R23.** Additive keys only (R3); no new file type; projects that use no music are unchanged.
  An older engine reading a `.soundevent` with `loops` ignores it; an older editor keeps an
  importer's `music` block (S36 R7's unknown-key rule).

## 6. Design constraints & decisions

- **D1. A scene-less system owns music** (sound-layer decision 6): it survives loads without
  persistence tricks and sits in the same voice tables as every voice.
- **D2. Sections are switched on the audio thread**, prepared on the main thread: gapless at any
  frame rate, no decoding or allocation in the mix.
- **D3. Quantization is a delay on a voice start** (`setDelaySamples`), so it is sample-exact
  offline and within one device period with a device, with no scheduler of its own.
- **D4. Callbacks are per frame.** Gameplay that must be sample-exact uses stingers and switches,
  which are.
- **D5. Music is a voice of a group**, never a mixer feature: upstream's mixer stays the one model.

API sketch:

```cpp
namespace jasperengine {
  struct MusicOptions { float crossfade = 0; std::string sync = "now"; std::string snapshot; float snapshotSeconds = -1;
                        std::string group; float volume = 1; bool ignoreListenerPause = false; };
  struct MusicPosition { int section = 0, bar = 0, beat = 0, beatsPerBar = 4; double beatFraction = 0, bpm = 0; };
  class MusicPlayer { public: static bool play(const std::string& track, const MusicOptions&); static void stop(float fade, const std::string& sync);
      static bool advance(); static SoundHandle stinger(const std::string&, const std::string& sync); static void update(double unscaledDt);
      static void reset(); static std::string current(); static double time(); static std::optional<MusicPosition> position(); };
}
```

### 6.1 MCP tools and the budget

No tool and no schema change (R20). Group `audio`: S35 §6.1. Core: +0.

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `.soundevent` clip `loops` | `jasperengine.soundevent` v1, additive | S37's `SoundEventIO` |
| `.meta` `import.audioImporter.music` | section `version: 1`, additive | S36's `AudioImportSettings` |
| `je-audio-imports.json` tempo fields | `jasperengine.audio-imports` v1, additive | S36's exporter and loader |
| `.jasperengine/cache/audio/<sha1>-grid-v1.json` | `GRID_VERSION` 1 | `nlohmann::json`, regenerable |

### 6.3 Settings (S25)

| Key | Scope | Type | Default |
|---|---|---|---|
| `audio.music_group` | Project (Audio) | string, a group name ("" = Master) | "" |

### 6.4 Editor commands

`audioClip.detectTempo` (Detect Tempo; the clip view and Project context menu for audio files),
`soundEvent.playAsMusic` (Play as Music; the event view). No default keys.

### 6.5 Lua API (S23 `tools/lua_api`)

R19's names, bound in `AudioClassesLua.cpp` under `#if JASPERENGINE_SOUND_LAYER` (S34 detection
reads them).

### 6.6 Player size (S34)

All runtime code is in S37's `soundlayer` module (estimated 25–35 KB of its total). A project
whose scripts never name a music function and whose content has no music needs nothing more.
`AudioMusicGrid` is editor-only.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1–R3) | `engine/tests/core/audio/MusicSequenceTest.cpp` (deterministic driver): a 3-section sequence event with `loops` 2/1/0 and loop regions: the output equals the concatenation computed by a Python oracle (`audio_refs.py music_sequence`) sample for sample, with no gap or click at hand-overs (seam step below the signal's own); `advanceMusic` ends the 0-loop section at its next loop end; a `layer` event is refused; mismatched rates refuse the track with the clip's name. S36/S37 round trips keep `music` and `loops`. |
| AC2 (R4, R6) | `tools/mcp/tests/test_audio_music.py`: music survives a scene load (no `voice_stop` at the load); stops at Play stop (reason `scene`); an edit-mode `eval_lua` `playMusic` leaves no voice in the next capture; routed to `audio.music_group` "Music" and ducked by a `duck` effect on that group (S35 levels show the drop); app pause holds it; `AudioListener.pause` pauses it unless `ignoreListenerPause`. |
| AC3 (R8–R10) | `test_audio_music.py` on S35 captures: an immediate switch with T = 2 s gives outgoing/incoming gains matching cos/sin within 0.3 dB on the Music group's levels; `sync: bar` starts the incoming voice exactly on the bar's output sample (`music play` line's `boundary_sample` = the incoming `voice_start` sample + delay); `snapshot` produces the `mixer transition` line in the frame containing the boundary; a third switch during a crossfade keeps two voices. |
| AC4 (R12) | `test_audio_music.py::test_stinger`: `beat` and `bar` stingers start on their boundary samples at 120 bpm with `first_beat_ms` 12.4; without tempo a stinger plays `now` with one warning; a stinger survives a scene load. |
| AC5 (R13, R14) | `test_audio_music.py::test_callbacks`: over 8 s at 120 bpm, 4/4: `onMusicBeat` fires 16 times in order with bars 1–4, each in the frame whose mix holds the beat; `getMusicPosition` agrees with the capture's grid. |
| AC6 (R15) | The `music` lines of AC2–AC5 parse with S35's reader; `audio_session report` lists them; `check silence` counts music voices. |
| AC7 (R16, R17) | `editor/tests/AudioMusicGridTest.cpp`: synthetic click tracks at 90, 120, 140 BPM (3/4 and 4/4) → bpm within 0.1, first beat within 5 ms, the right meter; a pulse-free noise → no bpm; a 2 s file → "too short". `test_audio_tempo.py`: `analyze_audio` returns `music` for a loop item; passing `fix` to `set_audio_import_settings` writes the block and `agrees` becomes true. |
| AC8 (R18) | `test_audio_music_editor.py` (xvfb): Detect draws the grid, Use writes one undo step; the event view's Loops column and Play as Music; screenshots in the evidence. |
| AC9 (R19, R21) | `tools/lua_api/tests` and `test_stubs.py` list every R19 name under the module condition; refused arguments warn once; the lua doc blocks run in Play (S23 R29a). |
| AC10 (R22, §6.6) | Benches on server F within R22; S34's module detection marks `soundlayer` needed for a script that calls `Audio.playMusic` (evidence line) and not for one that does not. |

## 8. Dependencies & conflicts

- **Depends on.** S37 (events, picker, RNG, free voices, `Audio`, module), S36 (importer section,
  `ClipSource`, the analysis STFT helpers, the clip view), S35 (captures, the `silence` check's
  music owner), upstream's mixer; S05 (reload of tempo changes), S23, S24, S25, S34.
- **Merge hotspots.** `AudioSystem.cpp` (S35, S37, S40), `Audio.{h,cpp}` and
  `AudioClassesLua.cpp` (S37, S40), `InspectorAudioClip.cpp` (S36), `InspectorSoundEvent.cpp`
  (S37), `AudioAnalysis.cpp` (S36: shared STFT helpers made public, read-only here).

## 9. Risks & defaults

- **A loop region that is not whole beats** drifts the grid: default: the runtime warning and the
  detector's `beat_drift` note; no automatic correction.
- **Detection errors on syncopated music**: default: `confident` and the candidates are shown; the
  user or agent chooses; nothing is written automatically.
- **Device-mode quantization** is within one device period (5–20 ms on mobile): default: documented;
  offline it is exact.

**Resolved questions**
- Release-player size: the `soundlayer` module (which holds the music system) is in every
  prebuilt variant; Compile builds strip it when unused (S37 §9 Q1, resolved 2026-11-01).

## Milestones

- **M1** Keys, `MusicPlayer`, `MusicSequenceSource`, sections, playlists, the music system
  (R1–R7); AC1, AC2.
- **M2** Grid, switches with sync, snapshots, crossfades, stop/advance, stingers (R8–R12); AC3, AC4.
- **M3** Callbacks, queries, Lua, capture lines, `get_play_state` (R13–R15, R19, R20); AC5, AC6,
  AC9.
- **M4** `AudioMusicGrid`, `analyze_audio` music block, the editor's Detect/Use and event view
  (R16–R18); AC7, AC8.
- **M5** Docs, budgets, size (R21–R23); AC10.

## Deviations

- **The scene-less system.** The music system is an `AudioSystem` built with no scene (a private tag
  constructor, created on first use). It is never in a scene's system list: `MusicPlayer::update` runs once a
  frame from `Engine::runFrame`, after the scenes and before the deterministic mix, so a beat callback, a
  snapshot and a switch point land in the frame whose mix holds them. Its voices are registered with no
  owner scene, so `stopSceneSounds` never reaches them and `AudioSystem.stopAll` does.
- **Stingers and `Audio.stopAll`.** A stinger's voice lines say `owner: "music"` (as the track's); `Audio.stopAll`
  (free voices, triggers, one-shots) leaves the music and its stingers alone, `AudioSystem.stopAll` stops them.
- **`playStinger`'s sync** is `now`, `beat` or `bar` (`section_end` is refused like an unknown name).
- **A switch replaced before its switch point.** When a second `playMusic` (or a `stopMusic`) comes while the
  incoming track still waits for its boundary, that track was never heard: it stops at once and the playing one
  stays the track the new switch starts from. (Once the switch point passed, R10's rule applies.)
- **Sequence playlists start at their first clip** and keep their own order, whatever the event's shared
  sequence position is from plays as sound effects; `shuffle` and `random` use S37's picker.
- **`onMusicSection`** is `FunctionSubscribe<void(int, const std::string&)>`, the signature the Lua bindings
  already register (`FunctionSubscribe_V_IS`).
- **Positions.** P is the voice's stream position less its start delay (SoLoud counts the delay in it). Before
  a section's first beat `getMusicPosition` gives bar 0 and beat 0. `getMusicTime` is the track's own seconds
  (not scaled by the event's pitch). The `stop` line also carries `sync` and `boundary_sample`.
- **The music problems** (sections differing in rate or channels, a section under 100 ms) are in the editor's
  `SoundEventDocument::problems`, which can read the clips; `SoundEventIO::validate` checks `loops`' range.
- **Play as Music** plays the inspected event through the runtime `MusicPlayer` in a preview mode (edit mode
  holds the game's events, the preview is not held); Play start resets it like any edit-mode music.
- **R16 tempo autocorrelation (feat/s39-music-grid).** `A[τ]` is the autocorrelation of the onset
  envelope *less its mean*: of the clamped envelope as P6 §2.4.8 writes it, white noise reads about
  0.32 and would pass the 0.1 no-pulse floor. Measured: 12 s of white noise 0.05-0.06, 8 s of pink
  noise 0.08-0.11 (close to the floor: the unit test uses 12 s of white noise).
- **R16 the line through the beats.** A tracked beat more than 1/16 beat off the first line is left out
  and the line refitted (twice): the tracker puts a spurious beat at the file's first frame, which
  moved `bpm` by 0.08 and `first_beat_ms` by 6 ms. Beats are numbered by their gaps (a skipped beat
  keeps the count). The attack envelope's rise is the 1 ms after a sample against the 1 ms before it,
  the file zero-padded, so a beat on sample 0 is found.
- **R16 downbeat accent.** The low band's value at a beat is its largest within ±1/8 beat of the line's
  beat (the onset frame of an attack is not the beat's frame); `B = 3` wins only with a positive accent
  20 % above four's.
- **R17 `agrees`.** bpm within 0.1 (P6: 0.5, which drifts 66 ms over 8 bars at 120), the same beats per
  bar, and beat 1 within 5 ms of a bar line of the detection (P6: 10 ms of its first beat; a block
  whose beat 1 is a later bar is the same grid). `current` is null without a block (P6 showed zeros);
  `agrees` is null for built-in items. `fix` is `{"music": {...}}`, passed as `settings` as the notes'
  `fix` is. The text line reads "pulse 0.71, downbeat 0.62" (P6: "confidence").
- **R18.** The second candidate's button reads "Use <bpm> BPM" (P6: "Use This Grid"), shown when not
  confident; `SetAssetMetaCmd::setAction` names the step "Set Tempo from Detection (<file>)". The grid
  over the waveform is the detection's; the typed tempo is not drawn.

