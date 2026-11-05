# S37 — Sound Events, gameplay triggers, the Lua `Audio` API, the Sound Browser

**Wave / Depends on.** Sound wave A (spec-first: implemented after the integration branch lands in
`main`). Depends on BF14. M1–M5 depend on nothing else of S35–S40 and run in parallel with them;
M6 (the Sound Browser) needs **S36 M2 and M7** (waveforms, import settings, search, built-in
sources); the capture fields of R14 need **S35 M3** (whichever lands second adds them). Blocks S39
(event playlists, the `Audio` table) and S40 (cue playback). Owner area:
`engine/core/audio/SoundEvent*`, `engine/core/object/sound/Audio.*`, `SoundHandle.*`,
`engine/core/subsystem/AudioSystem.*` (events, triggers, free voices),
`engine/core/component/AudioSourceComponent.h` (new fields),
`engine/core/script/binding/AudioClassesLua.cpp`, `editor/audio/SoundEvent*`,
`editor/window/SoundBrowserWindow.*`, `editor/window/InspectorSoundEvent.cpp`,
`editor/reflect/components/CameraAudioReflect.cpp` (AudioSource rows),
`editor/mcp/tools/SoundTools.cpp` (`edit_sound_event`), `docs/mcp/guide/sound.md` (its Sound
Events section).

## 1. Goal

A game's sound varies and reacts without code. A **Sound Event** (`.soundevent`, Unity's Audio
Random Container) holds clips played by weight, shuffle, sequence or as layers, with pitch, volume
and delay ranges, a cooldown, an instance limit with stealing and an output group of upstream's
mixer. Scenes reference it by GUID (decision 3), so it can be renamed and moved freely. An
AudioSource plays a clip or an event, can play itself on UI clicks, presses and hovers and on 2D
and 3D collisions and sensors, scaled by impact speed. Lua gets an `Audio` table for free voices
with handles, consistent with upstream's `AudioSource` and `AudioMixer`. Humans get a Sound
Browser that searches, auditions and drags sounds from the project and the built-in library.
Prebuilt players load `.soundevent` files as data.

## 2. Scope

**In scope**
- The `.soundevent` format, its S04 identity (GUID in its `.meta`), validation, dependency edges
  and relinking.
- The engine runtime: modes, ranges, cooldown, instance limit and steal composed with upstream's
  per-source `maxVoices` and global voice budget, the seeded RNG, missing clips and events.
- New `AudioSourceComponent` fields through S08 reflection (`event`, `playOn`, impact speeds,
  random ranges), triggers, `playOneShot` of events.
- Lua `Audio.play`, `Audio.stopAll`, `SoundHandle`, `AudioPlayOn`, `AudioSource` additions.
- Editor: AudioSource Inspector drawers, the `.soundevent` Inspector view and audition, create
  commands, the Sound Browser window (S25 commands and context).
- MCP `edit_sound_event`; event fields in S35's capture log.
- Exports: `.soundevent` files as assets loaded by the engine (no codegen); the `soundlayer`
  engine module of S34 (§6.6).

**Out of scope**
- Buses, snapshots and their Lua functions (sound-layer `Audio.setBus*`, `setSnapshot`): upstream's
  `AudioMixer`, `AudioMixerGroup` and `transitionTo` cover them.
- Music playlists and stingers (S39; it adds the clip key `loops`), animation cues (S40).
- HTML UI document triggers (Lua handles those), keyboard/gamepad UI activation (the engine has no
  UI navigation for `UIComponent`).
- Bus renames rewriting references (sound-layer `AudioBusRefs`): groups have stable ids (AM A3).

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **AudioSource** (`engine/core/component/AudioSourceComponent.h`, reflected in
  `editor/reflect/components/CameraAudioReflect.cpp:96-134`): `filename` is an `AssetRef`
  (`AssetKind::Audio`), plus upstream's `outputGroup`/`outputGroupName`, `loadType`, `priority`,
  `maxVoices` (1..64, default 8: main + one-shots, oldest one-shot stolen), `ignoreListenerPause`,
  all `OmitIfDefault`. No event, trigger or variation fields. Lua: `AudioSource:playOneShot(path
  [, volumeScale])` (`GameObjectClassesLua.cpp:4202`), `AudioSource.playClipAtPoint(scene, path,
  position [, volume [, group]])` with no handle (`AudioSystem.cpp:761`).
- **No random container** exists (no `soundevent` anywhere in engine or editor).
- **Asset plumbing precedents.** `.controller`/`.montage` are referrers whose `{path, guid}`
  references are indexed as dependency edges (`editor/asset/AssetIndex.cpp:160-170, 309-335`) and
  relinked by their documents (`editor/animation/AnimatorAssetDocument.cpp:272`). Sidecar-identity
  types get a `.meta` (`AssetTypes.cpp:25-53`). `AssetKind` (`editor/reflect/Reflect.h:49-52`) has
  no Sound Event kind.
- **Signals for triggers.** `UIComponent::onClick`, `onPointerEnter` (`UIComponent.h:88-93`),
  `ButtonComponent::onPress` (`ButtonComponent.h:27`), `PhysicsSystem::beginContact2D`,
  `beginSensorContact2D`, `hitContact2D` (with the approach speed) and `onContactAdded3D`
  (`PhysicsSystem.h:698-718`); 3D contacts arrive on the main thread as `RawContact3D` records
  with pre-solve point velocities and the normal (`JoltPhysicsAux.h:410-428`), delivered as
  `Contact3D` snapshots that carry no speed (`Contact3D.h:47-100`).
- **Exports.** `copyAssets` copies every asset file (`Exporter.cpp:2700-2790`); S04's `AssetMap`
  (`assetmap.bin`) maps GUIDs to paths in a game; S30 R4 cooks component fields through
  `RuntimeFields.gen.inc`.
- **Commands.** `ShortcutContext` has `Global, Editor, SceneView, Hierarchy, Project, Console,
  Animation, PoseAnimation` (`editor/shortcuts/CommandCatalog.h:36`).

## 4. Reference: sound-layer (`snd/sound-layer` 20b461161; checkout `je-wt/snd-ro`)

### 4.1 Reused (adapted)

| What | Where (commit, file) | Change and why |
|---|---|---|
| Event model and play modes | 38b4ad0f9…a3f7287b0 `engine/core/audio/SoundEvent.{h,cpp}` (676), P3 §2.2-2.3 | `bus` → upstream output group `{id, name}`; `max_voices` renamed `max_instances` (upstream's AudioSource `maxVoices` means something else); composed with upstream's per-source cap and global budget; no `AudioSettings` dependency. |
| Seeded RNG (exact) | P3 §2.3.6 (fnv1a64, splitmix64, xorshift64*) | As is; the seed is S35's `audio.seed` in deterministic runs. |
| Asset reader/writer, validation, problems | c3b9ff4f9 `editor/audio/SoundEventAsset.{h,cpp}` (1,166), P3 §2.2.1, §2.5 | YAML with an in-file `guid` → JSON with the GUID in `.meta` (decision 3; S30 players load JSON); clips are `AssetRef {guid, path}` with dependency edges; the parser moves to the engine (`SoundEventIO`). |
| Triggers | P3 §2.4.3-2.4.5 (PhysicsSystem +382, JoltPhysicsAux +114, Box2DAux +16) | 3D from physics-contacts' main-thread records, so `AudioCallQueue` and the mutex queue are dropped; lazy tagged subscriptions kept. |
| Free voices and handles | 471dfd6ba…64e9f5e5f `engine/core/object/sound/Audio.{h,cpp}`, 773a38d6a `SoundHandle.{h,cpp}`, P3 §2.7 | `Audio.play` takes an options table in upstream's style; fades named as upstream's `AudioSource` fades; bus and snapshot functions dropped. |
| Inspector rework | `editor/window/InspectorAudio.cpp` (1,286), P3 §2.9 | Re-hosted as S08 named drawers and warning rows; the hand layout dropped. |
| Sound Browser | 1dcc9ed4d…d716d93a1 `editor/window/SoundBrowserWindow.{h,cpp}` (6,655), P3 §2.10, P5 §2.8 | Import settings and the big waveform move to S36's Inspector clip view; the Browser keeps search, cards, audition, drag and drop and the event editor; its 21 `IsKeyPressed` become CommandCatalog entries in a `SoundBrowser` context; the Captures tab goes to S35's window. |
| Offline audition | 801e9fca5 `editor/audio/SoundEventRender.{h,cpp}` (413), P3 §2.10.6 | Renders through S36's `ClipSource` with import settings. |
| `edit_sound_event` | `editor/mcp/tools/AudioTools.cpp` (sound-layer), P3 §2.11.2 | Trimmed to ≤ 1,400 characters; `bus` → `output_group`. |
| Tests | category C of the editor analysis (`test_audio_events`, `_event_assets`, `_edit_event`, `_export_events`, `_triggers`, `_lua`, `_p3_review_fixes`, `_status_events`), `audio_browser_editor` (category D) | Rewritten: groups instead of buses, no codegen, captures through S35; fixture `audio-events` regenerated on target. |
| Glossary | P3 §2.12 | Audio Random Container → Sound Event. |

### 4.2 Changed on purpose

- **The GUID lives in `.meta`** (decision 3), not in the file, so there is one identity system
  (S04 R26).
- **No codegen.** Sound-layer's `Generator` emitted `SoundEventLibrary::add(...)`; S30's prebuilt
  players cannot run generated code, so the engine reads `.soundevent` files like `.mixer` files.
- **Per-source cap semantics stay upstream's** (`maxVoices` 1..64, default 8; sound-layer's
  "0 = unlimited" is dropped, analysis G-29).

### 4.3 Dropped

`AudioBusRefs` and `set_project_settings bus_renames`, `WriteAudioAssetCmd` (S24's
`WriteFilesCmd` and the document pattern replace it), the `bus` field, `Audio.setBusVolume`/
`getBusVolume`/`setBusMuted`/`setSnapshot`/`getSnapshot`/`masterVolumeDb`, the per-event codegen,
`AudioCallQueue`, `create_entity audio_source event/play_on` parameters (set through
`set_properties` reflection instead, zero core growth), `find_assets type: sound_event` (core
growth; `search_sounds` lists events).

## 5. Requirements

### Functional — the asset

- **R1. `.soundevent`** (JSON, UTF-8, LF, 2-space indent, keys in this order; every key but
  `format`, `version`, `mode` and `clips` written only when not default; unknown keys kept):

  ```json
  {
    "format": "jasperengine.soundevent",
    "version": 1,
    "mode": "shuffle",
    "clips": [
      {"clip": {"guid": "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10", "path": "sounds/step_1.wav"},
       "weight": 2, "volume_db": -1.5, "pitch_semitones": 0, "delay_ms": 0},
      {"clip": {"guid": "…", "path": "sounds/step_2.wav"}}
    ],
    "avoid_repeat": true,
    "pitch_semitones": [-1, 1],
    "volume_db": [-3, 0],
    "delay_ms": [0, 20],
    "output": {"id": "5fx00001", "name": "SFX"},
    "max_instances": 4,
    "steal": "oldest",
    "cooldown_ms": 50,
    "priority": 128,
    "loop": false,
    "spatial": {"min_distance": 1, "max_distance": 30, "attenuation": "linear_distance", "rolloff": 1, "doppler": 1},
    "type": "footstep"
  }
  ```

  Modes `random` (weighted, `avoid_repeat` default true), `shuffle`, `sequence`, `layer`
  (sound-layer P3 §2.2.2). Ranges and validation (one function, `SoundEventIO::validate`, used by
  the editor, MCP and the engine): clips 1–64 (duplicates allowed); weight 0–1000 with at least one
  > 0 in `random`; clip `volume_db` −60…12, `pitch_semitones` −24…24, `delay_ms` 0–2,000; ranges
  `[min, max]` within those, `min ≤ max`; `max_instances` 0 (no limit)–255; `steal` `oldest`,
  `quietest`, `newest`, `none`; `cooldown_ms` 0–60,000; `priority` 0–256 (128 = the playing
  source's); `spatial` optional (`min_distance` > 0, `max_distance` ≥ it, `rolloff` 0–100,
  `doppler` 0–10, `attenuation` one of upstream's `AudioSourceAttenuation` names); `type` empty or
  an S36 sound type. Tags are S25 labels in the event's `.meta`. Parse errors list a JSON path and
  a reason (the `AudioMixerIO` style); a file whose `version` is above 1 is refused with the
  versions this engine reads.
- **R2. Identity.** `AssetTypes` gains `{AssetType::SoundEvent, AssetIdentity::Sidecar,
  AssetPathBase::Assets}` for `.soundevent`; `AssetKind::SoundEvent` is added (append-only, S08
  R33). Every reference to an event is an `AssetRef` resolved by GUID first (S04).
- **R3. Edges and relinking.** `AssetIndex` treats `.soundevent` as a referrer: one dependency edge
  per clip (`field` `clips[i].clip`, expected type Audio). When a clip moves or is renamed, the
  editor relinks the event's `path` from the GUID (the `.controller` pattern) as part of the same
  undo step as the move (S24); a missing clip shows in S04's Missing panel and in R20's problems.
- **R4. Output group.** `output {id, name}` resolves as upstream resolves an AudioSource's group
  (AM A3: id, then name, then Master, warning once); absent = the playing source's group (an
  `Audio.play` voice: Master or its `group` option).

### Functional — the runtime

- **R5. `SoundEventIO` and `SoundEventLibrary`** (`engine/core/audio/`): parse/write/validate in
  the engine (nlohmann JSON, no editor code); the library loads a definition by path on first use
  (synchronously: files are small), keeps it per path, maps GUIDs to paths through S04's
  `AssetMap` in games and the asset database in the editor, and reloads it when S05 reports a
  change (playing instances keep their voices; the next play uses the new definition; a clip list
  or mode change resets the event's bag and sequence).
- **R6. Starting an event** (`AudioSystem::startEvent`, the one function every caller uses), in
  this order:
  1. **Cooldown**: rejected (`voice_reject cooldown`) when less than `cooldown_ms` passed since the
     last accepted start; in deterministic runs compared in samples on S35's cursor, with a device
     on the audio clock.
  2. **Instance limit**: with `max_instances` live, a candidate (not protected, priority value ≥
     the new one's; `oldest`/`newest` by start, `quietest` by the loudest voice's
     `getOverallVolume`; ties by instance id) is stopped **before** the new voices start
     (`stolen_instance`), or the play is rejected (`instance_limit`) when there is none or `steal`
     is `none`.
  3. **Pick** with the play's RNG (R8): the bag, last index, sequence position and play index
     advance.
  4. **Voices**: one per instance, or one per clip in `layer`; each through upstream's
     `AudioSystem::startVoice`, so upstream's per-source `maxVoices` (A10) and global budget and
     priority stealing (A11) apply after the instance limit. An instance whose every voice was
     rejected is not live; its pick stays consumed.
  5. **Missing clips**: a clip that does not load is skipped with one warning per clip per Play
     and `voice_reject missing_clip`; the other picks do not change.
- **R7. Per-voice values** (sound-layer P3 §2.2.3): speed = source speed × 2^((p + clip pitch)/12)
  × API pitch; linear gain = source volume × 10^((v + clip volume)/20) × impact gain × API volume;
  delay = round((d + clip delay) × rate / 1000) frames through `Soloud::setDelaySamples` before the
  voice is unpaused. Loop: the event's `loop`, or the source's `looping` for its main voice.
  Spatial: the event's `spatial` when present, else the source's. Priority: the event's when not
  128, else the source's.
- **R8. RNG.** Exactly sound-layer P3 §2.3.6: `state = splitmix64(seed64 ^ fnv1a64(key) ^
  (playIndex × 0x9E3779B97F4A7C15))` (`0x9E3779B97F4A7C15` if 0), xorshift64* draws, `u =
  (next() >> 11) × 2⁻⁵³`; draw order pick, then pitch, volume, delay (always three); `key` the
  event GUID's 32 hex characters, or `"entity:<id>"` for clip-mode variation (R11). `seed64` is
  S35's `audio.seed` while the deterministic driver runs (so captures repeat), otherwise drawn
  from `std::random_device` at each runtime reset (Play start, game start). `tools/mcp/audio_refs.py`
  reproduces it in Python.
- **R9. AudioSource fields** (`AudioSourceComponent.h`; S08 `JE_FIELD`s after `ignoreListenerPause`,
  all `OmitIfDefault`; cooked through `RuntimeFields.gen.inc`, S30 R4):

  | Field | Type | Default | Meaning |
  |---|---|---|---|
  | `event` | `AssetRef` (`AssetKind::SoundEvent`) | empty | when set, the source plays the event (its `filename` is ignored, no fallback) |
  | `playOn` | `uint32_t` flags (`AudioPlayOn`) | 0 | R12 |
  | `minImpactSpeed` | float ≥ 0 | 0 | contact triggers below it play nothing |
  | `fullVolumeImpactSpeed` | float ≥ 0 | 0 | 0 = no scaling |
  | `pitchRandomSemitones` | float 0–24 | 0 | clip mode: each start ±p semitones |
  | `volumeRandomDb` | float 0–60 | 0 | clip mode: each start −v…0 dB (never louder) |

  Old scenes and prefabs re-save byte-identically (S08 R30, R31).
- **R10. Event mode of a source.** `play()` starts an instance whose voices are the source's main
  voice (layers included); stop, pause, seek, `isPlaying`, `onStart`/`onStop` (upstream A15)
  apply to all of them; the natural end is "none of them valid". `playOneShot(path [,
  volumeScale])` accepts a `.soundevent` path or GUID too and counts against `maxVoices` per
  voice. An event that does not resolve plays nothing (one warning per GUID per Play,
  `voice_reject missing_event`).
- **R11. Clip-mode variation**: `pitchRandomSemitones`/`volumeRandomDb` vary every start of a
  clip-mode source (main, one-shots, triggers) with R8's RNG and the source's own start count;
  both 0 draws nothing and plays as today.
- **R12. Triggers** (`AudioPlayOn`: `UIClick` 1, `UIPress` 2, `UIHover` 4, `Collision2D` 8,
  `Collision3D` 16, `Sensor2D` 32, `Sensor3D` 64; scene and MCP names `ui_click`, `ui_press`,
  `ui_hover`, `collision_2d`, `collision_3d`, `sensor_2d`, `sensor_3d`): a flagged source on the
  entity that owns the UI component or the body plays a one-shot of its clip or event when:
  - `UIClick`/`UIPress`/`UIHover`: `UIComponent::onClick`, `ButtonComponent::onPress`,
    `UIComponent::onPointerEnter` of its own entity;
  - `Collision2D`: `hitContact2D` when either collider has hit events on (its approach speed ×
    `pointsToMeterScale2D`), else `beginContact2D` (no speed: gain 1, gate skipped); one signal per
    pair (sound-layer P3 §2.4.3);
  - `Sensor2D`: `beginSensorContact2D` (sensor or visitor);
  - `Collision3D`/`Sensor3D`: `onContactAdded3D` without/with `isSensor()`, speed
    `Contact3D::getApproachSpeed()` (new: `max(0, (v1 − v2) · n)` from the `RawContact3D` record's
    pre-solve velocities and normal, carried into the snapshot), one per body pair and flag per
    frame (the largest speed);
  - impact gain `min(1, s / fullVolumeImpactSpeed)` (1 when 0); `s < minImpactSpeed` rejects
    (`impact_gate`); a trigger while the source's scene audio is paused is dropped.
  Subscriptions are made lazily with the tag `jasperengine:audio:e<entity>;` (UI) or
  `jasperengine:audio:physics;` (per scene, only for the signals some source needs), removed when
  no source needs them and at Stop and scene teardown, so `hasSubscribers()` stays false for
  scenes that use none (sound-layer P3 §2.4.5).

### Functional — the Lua and C++ `Audio` API

- **R13. `Audio`** (`engine/core/object/sound/Audio.{h,cpp}`; the global table S35 created):
  - `Audio.play(eventOrClip [, opts]) -> SoundHandle`: an event (GUID or path) or a clip path;
    `opts` = `{target = GameObject (3D, follows it, stops when it is destroyed), position = Vector3
    (3D, fixed), scene = Scene (owner; default the target's scene, else the main scene), volume =
    1 (linear, as AudioSource.volume), pitch = 1 (as AudioSource.pitch), group = "SFX" (a group
    name, as AudioSource.outputGroup; default the event's, else Master), delay = 0 (s), unscaled =
    false}`. An unknown name returns an invalid handle and warns once per name per Play.
  - `Audio.stopAll([group]) -> integer`: stops the voices started by `Audio.play`, triggers and
    one-shots, all or those routed to `group` or a group below it; `AudioSystem.stopAll()` stays.
  - `SoundHandle` (value type `{id, generation}`): `isValid()`, `isPlaying()`, `stop()`,
    `pause()`, `resume()`, `fadeOut(seconds [, opts])` (`opts.pause`, `opts.unscaled`, as
    upstream's `AudioSource:fadeOut`), `fadeTo(volume, seconds [, opts])`, properties `volume`,
    `pitch`, `pan`, `position` (rw; setting it stops following), `time` (ro), `voiceCount` (ro).
    Getters of an invalid handle return 0 / the zero vector; setters do nothing.
  - `AudioPlayOn` enum namespace (as upstream's `AudioLoadType`).
  - `AudioSource` additions: `event` (rw string: reads the path, assigning a path or GUID
    resolves it), `playOn`, `minImpactSpeed`, `fullVolumeImpactSpeed`, `pitchRandomSemitones`,
    `volumeRandomDb`.
  - Every call made inside a physics callback or off the main thread defers to the frame's audio
    update; non-finite numbers are refused with the engine's warning.
- **R14. Capture fields** (with S35): `voice_start` adds `event` (GUID), `event_path`,
  `variation`, `layer`, `instance`, `trigger`, `impact_speed`; `owner` gains `free` and `cause`
  gains `event`, `trigger`, `lua`, `cpp`; `voice_reject` reasons `cooldown`, `instance_limit`,
  `missing_clip`, `missing_event`, `impact_gate`; `voice_stop` reason `stolen_instance`
  (`by_instance`). All within S35's `jasperengine.audio-events` v1.

### Editor UX (human)

- **R15. AudioSource Inspector** (S08 named drawers in `editor/window/InspectorDefault.cpp`):
  the Event field (a `.soundevent` picker; when set the Audio Clip row reads "(played by the
  event)"), Play On as checkbox chips, the two impact speeds shown when a collision flag is set,
  a Variation group (clip mode), and warning rows: an event with missing clips, a collision flag
  on an entity without a Rigidbody or Rigidbody2D, a UI flag on an entity without a UI component,
  event and clip both set.
- **R16. `.soundevent` Inspector view** (`InspectorSoundEvent.cpp`, `drawSoundEventAsset`): mode,
  the clip list (drop clips from the Project window or the Browser, reorder, weight, per-clip dB,
  semitones, delay), two-handle range sliders, output group combo (the mixer's groups),
  instances/steal/priority/cooldown/loop, the spatial override, type, labels, the problems rows,
  **Match Levels** (each clip's `volume_db` set so all play at their average LUFS-M, none past
  −1 dBTP; not for `layer`), and **Audition ×8** (`SoundEventRender`: the next 8 plays rendered
  offline with the event's RNG from `audio.seed`, through S36's `ClipSource`, played one after
  another through an editor preview voice, with each pick's clip and LUFS shown). Every edit is a
  command on the document (S24 names: "Set Sound Event Mode", …), saved with the project.
- **R17. Create.** Assets > Create > Sound Event (`assets.createSoundEvent`) and, with audio files
  selected, Project context > Create Sound Event from Clips (`assets.createSoundEventFromClips`,
  mode shuffle).
- **R18. Sound Browser** (`window.soundBrowser`, Window > Audio > Sound Browser; needs S36):
  - sources Project, Essentials, Library (S36's built-ins; a missing library shows how to get it);
    a search field, type chips, character filters (duration, dark/neutral/bright, sharp/soft,
    tonal, loop, clip/event), "Similar to Selected", all through S36's `SoundSearch`;
  - cards with S36's cached mini waveform, type badge, duration, LUFS-M and a licence badge;
  - keyboard audition through CommandCatalog commands in a new `SoundBrowser` shortcut context
    (§6.4); optional auto-play on select (`preferences.sound_browser_autoplay`) and loudness-matched
    audition (`preferences.sound_browser_match_loudness`, previews at −20 LUFS-M);
  - a detail pane: for a clip, S36's Inspector clip view; for an event, R16's editor;
  - drag and drop: project files as the existing `resource_files` payload; built-in cards as a
    `builtin_sounds` payload imported first through S36's planner (R18 of S36). Onto the Hierarchy
    or the Scene view: a new GameObject with an AudioSource; onto the Inspector's Audio Clip or
    Event field: sets it; onto a GameObject with a Button: adds an AudioSource with
    `playOn: ui_click`. Each is one undo step;
  - previews are editor preview voices, never captured, disabled while S35 captures.

### Agent UX (MCP, Lua, CLI)

- **R19. Reflection does the wiring.** `set_properties` writes the new fields (`event` by path or
  GUID, `playOn` as a list of names); `get_entity` and `list_component_types` report them (S08
  R24-R26). No core tool gains a parameter.
- **R20. `edit_sound_event`** (group `audio`, activity Edit, idempotent, allowed while playing):
  `path` (`.soundevent` in the assets folder), `create` (refused when it exists; the GUID and
  `.meta` come from S04's asset database when `WriteFilesCmd` creates the file), `values` (R1 keys; `clips` items take paths or GUIDs;
  `output_group` by name; null resets), `match_levels`, `audition` (0–16 offline renders),
  `image` (the renditions' sheet); one undo step; during Play the next plays use it. Returns
  `{event, values, problems[] (missing clips, loudness spread > 3 LU, near-duplicate clips,
  silent layers, unknown group), renditions[] {n, picks[], lufs, peak_dbtp}}`. `get_asset_info`
  of a `.soundevent` returns its values and problems (result only).
- **R21. Docs.** `docs/mcp/guide/sound.md` section "Sound Events and triggers" (format, modes,
  RNG, limits and their order, recipes: footsteps, UI clicks, impacts); `lua-api.md` Audio,
  SoundHandle, AudioPlayOn, the AudioSource additions (lua blocks for S23's doc test);
  `docs/unity-glossary.md` (Audio Random Container → Sound Event, PlayOneShot of an event).

### Performance budgets (server F, Release)

- **R22.** `startEvent` ≤ 15 µs per voice above upstream's `startVoice` (no file access after the
  first load); triggers cost nothing when no source uses a flag (no subscription exists); 64
  trigger plays in one frame ≤ 1 ms.

### Data & compatibility

- **R23.** Old scenes, prefabs and exports unchanged (fields omitted at default); a project
  without events writes nothing new; `.soundevent` v1 is read by this and later engines.
- **R24. Exports.** `.soundevent` files ship as assets (`copyAssets`); clip references are
  checked at export (a missing clip is a build warning naming the event and clip); the engine
  loads them at first use in editor Play, Compile builds and prebuilt players alike. No generated
  code.

## 6. Design constraints & decisions

- **D1. JSON, not YAML.** The engine runtime reads JSON (`AudioMixerIO`, `AnimationClipIO`,
  `AnimatorAssetIO`); S30 players load data. Same `format`/`version` header as `.mixer`.
- **D2. Sidecar identity** (decision 3): the file body holds no GUID; `.meta` does.
- **D3. Limits compose in a fixed order**: cooldown → instance limit (steals before starting) →
  upstream per-source cap → upstream global budget. Sound-layer's bus limit has no upstream
  equivalent and is not added (per-group voice limits, analysis G-17, are not requested).
- **D4. `Audio.play` with an options table**, as upstream's `AudioMixer.setFloat(..., opts)`;
  linear volume and `pitch` as upstream's `AudioSource`; groups by name as `outputGroup`.
- **D5. Triggers are AudioSource fields**, not a component (no ECS slot; decision 10 is moot,
  256-bit signatures).
- **D6. No new core MCP parameters** (S31): everything through reflection and the `audio` group.
- **D7. The `soundlayer` engine module** (§6.6) holds the runtime of S36's import settings, S37,
  S39 and S40.

API sketch:

```cpp
namespace jasperengine {
  enum class SoundEventMode { Random, Shuffle, Sequence, Layer };
  enum class AudioStealPolicy { Oldest, Quietest, Newest, None };   // new (sound-layer's name)
  struct SoundEventClip { AssetRef clip; float weight = 1, volumeDb = 0, pitchSemitones = 0, delayMs = 0; int loops = 1; /* S39 */ };
  struct SoundEventDef { SoundEventMode mode = SoundEventMode::Random; std::vector<SoundEventClip> clips;
      bool avoidRepeat = true; float pitchMin = 0, pitchMax = 0, volumeMin = 0, volumeMax = 0, delayMin = 0, delayMax = 0;
      std::string outputGroupId, outputGroupName; int maxInstances = 0; AudioStealPolicy steal = AudioStealPolicy::Oldest;
      float cooldownMs = 0; int priority = 128; bool loop = false; std::optional<SoundEventSpatial> spatial; std::string type; };
  namespace SoundEventIO { bool parse(const std::string& json, SoundEventDef&, std::string* error);
                           std::string write(const SoundEventDef&); std::vector<std::string> validate(const SoundEventDef&); }
  struct PlayOptions { EntityHandle target; std::optional<Vector3> position; Scene* scene = nullptr;
                       float volume = 1, pitch = 1, delay = 0; std::string group; bool unscaled = false; };
  class SoundHandle { public: bool isValid() const; bool isPlaying() const; void stop(); void pause(); void resume();
      void fadeOut(float seconds, bool pause = false, bool unscaled = false); void fadeTo(float volume, float seconds, bool unscaled = false);
      float getVolume() const; void setVolume(float); float getPitch() const; void setPitch(float);
      float getPan() const; void setPan(float); Vector3 getPosition() const; void setPosition(const Vector3&); double getTime() const; };
  class Audio { public: static SoundHandle play(const std::string& eventOrClip, const PlayOptions& = {});
                static int stopAll(const std::string& group = ""); static void mark(std::string_view); static bool isCapturing(); };
}
```

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Estimated listing chars |
|---|---|---|
| `edit_sound_event` | `audio` | ≤ 1,400 (sound-layer ~1,450 with `bus`; the keys list moves to the guide); S36 M9 may add `slice` within 1,500 |
| `set_properties`, `get_entity`, `list_component_types` | core | 0 (reflection) |
| `get_asset_info` | core | 0 (result only) |

Group `audio` totals: S35 §6.1 (7 tools, ≈ 9,200 / 15,000). Core: 0.

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `*.soundevent` | `jasperengine.soundevent` v1 | `SoundEventIO` (engine, nlohmann JSON), written by the editor's `SoundEventDocument` in R1 key order |
| `*.soundevent.meta` | S04 `metaVersion: 1` (GUID, `labels`) | `AssetMeta` |
| AudioSource fields in `.scene`/`.prefab` | S03 scene `formatVersion` unchanged (additive `OmitIfDefault`) | S08 generic YAML codec (`kAudioSourceFields`), `RuntimeFields.gen.inc` for cooked scenes (S30 R4), `cppAssignments` for Compile builds |

### 6.3 Settings (S25)

| Key | Scope | Type | Default |
|---|---|---|---|
| `preferences.sound_browser_autoplay` | Preference | bool | false |
| `preferences.sound_browser_match_loudness` | Preference | bool | true |

The RNG seed is S35's `audio.seed`.

### 6.4 Editor commands (S25 CommandCatalog)

| Id | Name | Context, default key |
|---|---|---|
| `window.soundBrowser` | Sound Browser | menu Window > Audio |
| `assets.createSoundEvent` | Create Sound Event | menu Assets > Create |
| `assets.createSoundEventFromClips` | Create Sound Event from Clips | Project context menu |
| `soundBrowser.focusSearch` | Search | `SoundBrowser` (new context), Ctrl+F |
| `soundBrowser.next` / `soundBrowser.previous` | Next / Previous | `SoundBrowser`, Down / Up (Right / Left in the grid) |
| `soundBrowser.play` | Play or Stop | `SoundBrowser`, Space |
| `soundBrowser.playMatched` | Play Loudness-Matched | `SoundBrowser`, Shift+Space |
| `soundBrowser.use` | Use (select, or import a built-in) | `SoundBrowser`, Enter |
| `soundBrowser.selectAll` | Select All | `SoundBrowser`, Ctrl+A |
| `soundBrowser.stop` | Stop / Clear Search | `SoundBrowser`, Escape |
| `soundEvent.audition` | Audition ×8 | `SoundBrowser` and Editor (the Inspector view), no key |
| `soundEvent.matchLevels` | Match Clip Levels | same, no key |

`check-hardcoded-shortcuts.py` passes (no `IsKeyPressed` in the window).

### 6.5 Lua API (S23 `tools/lua_api`)

`Audio.play`, `Audio.stopAll`, `SoundHandle` (class), `AudioPlayOn` (enum), `AudioSource.event`,
`.playOn`, `.minImpactSpeed`, `.fullVolumeImpactSpeed`, `.pitchRandomSemitones`,
`.volumeRandomDb`, `AudioSource:playOneShot` (events accepted), `Contact3D:getApproachSpeed()`.
Bound in `AudioClassesLua.cpp` and `GameObjectClassesLua.cpp` under `#if JASPERENGINE_SOUND_LAYER`
(S34 reads the condition, §6.6); `Audio.mark`/`isCapturing` (S35) are outside it.

### 6.6 Player size (S34) and the `soundlayer` module

- **The module.** S34 R1 gains `soundlayer` (`JASPERENGINE_SOUND_LAYER`, default ON): S36's
  `ClipSource`/`AudioImport`, this spec's `SoundEvent*`, `Audio::play`, `SoundHandle`, triggers,
  S39's music system and S40's cue playback. Off, it compiles to nothing; the engine plays clips
  exactly as today.
- **Detection** (S34 R5, `editor/players/ModuleUsage`): needed when a scene or prefab of the build
  has an AudioSource with `event`, a non-zero `playOn` or random range; when the export writes
  `je-audio-imports.json`; when a shipped `.anim` has audio cues (S40); when a Lua script names
  `Audio.play`, `Audio.stopAll`, `SoundHandle`, `AudioPlayOn`, an AudioSource member of R13 or a
  music function (S39) (read from the bindings' `#if`, so no list to maintain); and for any dynamic
  access (S34 D4).
- **Estimated size** (to be measured with `lib_size.py --modules`): 90–140 KB Linux, 50–80 KB
  Android arm64, 45–70 KB wasm (gzip about a third), music and cues included.
- **Variants** (resolved Q1, 2026-11-01): `soundlayer` is in **every** prebuilt variant of
  `engine/player/variants.json` (`full`, `ui`, `core`, `lite3d`, `lite2d`), as S34 keeps
  navigation in every variant; each release player grows by the measured module size, recorded in
  S34's M table. A Compile build leaves the module out when the detection says it is unused (S34
  R6). The detection still runs for prebuilt builds: its evidence is shown, and `content-manifest.json`
  `modules` lists `soundlayer` when needed (S34 R3).

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R2) | `engine/tests/core/audio/SoundEventIOTest.cpp`: round trip of R1's example in key order, defaults omitted, unknown keys kept, every validation error with its JSON path, version 2 refused. `editor/tests/SoundEventAssetTest.cpp`: the file gets a `.meta` with a GUID on creation; renaming the event keeps the GUID and every scene reference. |
| AC2 (R3) | `tools/mcp/tests/test_audio_event_assets.py`: moving a clip relinks the event's `path` in the same undo step; deleting it shows the event in the Missing panel and in `edit_sound_event` problems; S04's dependency query lists the event's clips. |
| AC3 (R6–R8) | `test_audio_events.py` (S35 captures, deterministic driver): shuffle over 4 clips × 8 plays has each clip twice and none back to back (also across bags); `random` weights converge within 3 % over 4,000 offline picks (unit test); the logged pitches lie in range; cooldown rejections at the sample boundary; `max_instances 2` with `oldest` logs `stolen_instance` before the third instance's start; `audio_refs.py`'s Python RNG reproduces the picks of a capture exactly. |
| AC4 (R6 order) | `test_audio_events.py::test_limits_compose`: an event with `max_instances 4` on a source with `maxVoices 3` and `audio.max_voices 2`: the stop/reject lines follow D3's order. |
| AC5 (R9–R11) | `test_audio_events.py`: old fixture scenes re-save byte-identical; `set_properties event` by path and by GUID; an unresolved event plays nothing with one warning and `missing_event`; clip-mode variation draws within range and repeats under the same seed. |
| AC6 (R12) | `test_audio_triggers.py`: `ui_click` gives exactly one `voice_start` per `send_input` click, same sample as its marker; Bounce 2D: gain monotonic in approach speed and `impact_gate` below `minImpactSpeed`; a 3D drop from h: `Contact3D:getApproachSpeed()` within 5 % of √(2gh) (Lua) and one voice per pair; a scene with no flagged source has no audio subscription (`hasSubscribers()` false). |
| AC7 (R13) | `test_audio_lua.py`: `Audio.play` with each option; a handle goes invalid after the end; `fadeOut` with `pause`; `stopAll("SFX")` counts; calls inside a contact callback are deferred; `tools/lua_api/tests` and `test_stubs.py` list every R13 name; the doc test runs the lua blocks. |
| AC8 (R14) | With S35: the capture lines carry the event fields; `audio_session report` groups starts by event. |
| AC9 (R15–R18) | `test_audio_browser_editor.py` (xvfb, isolated editor): Browser search, arrow and Space audition (a preview voice plays, no capture line), Enter imports a library clip with its `.meta`, drag onto a Button adds a `ui_click` source in one undo step; the event view's Audition ×8 shows 8 renditions; Match Levels brings the clips within 1 LU; screenshots in the evidence; `check-hardcoded-shortcuts.py` passes. |
| AC10 (R20) | `test_audio_edit_event.py`: create, values, null reset, `match_levels`, `audition` with picks equal to the next 8 Play picks under the same seed, problems for a missing clip and an unknown group, one undo step per call; the AC10 group assertion. |
| AC11 (R24, §6.6) | S34's variant test: every entry of `engine/player/variants.json` includes `soundlayer` and every release player's `features` lists it. `test_build.py::test_sound_events_prebuilt` (`JASPER_SLOW_BUILD=1`): a prebuilt Linux player plays an event and a trigger (S35 player capture shows the picks of the editor's capture of the same scene, same seed); `content-manifest.json` `modules` lists `soundlayer`; a project without the features builds without it on a Compile build and its player's `features` match S34. |
| AC12 (R22) | `engine/tests/core/bench/SoundEventBench.cpp` on server F within R22. |
| AC13 (§6.1, §6.5) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_reflection.py` (the new AudioSource fields' schema), S34's module tests (`ModuleUsage` evidence lines for each detection rule). |

## 8. Dependencies & conflicts

- **Depends on.** BF14; S03, S04 (identity, `AssetMap`, edges, Missing panel), S05 (reload), S08
  (reflection, drawers, `RuntimeFields.gen.inc` via S30 R4), S23 (`tools/lua_api`), S24, S25
  (catalog, contexts, preferences), S30 (players load data), S31, S34 (module mechanism); S35 M3
  for R14; S36 M2/M7 for R18 (and S36 M3's `ClipSource` for audition with import settings).
- **BF14 makes prebuilt players load the project's mixer** (S38 §3; resolved 2026-11-01), so an
  event's `output` group resolves in prebuilt games as in Play; S37 assumes it.
- **Blocks.** S39 (playlists use events; `Audio` table), S40 (cues use `startEvent` and free
  voices), S36 M9 (slicing writes events).
- **Merge hotspots.** `AudioSystem.cpp` (S35, S39, S40, BF14), `AudioSourceComponent.h`,
  `CameraAudioReflect.cpp`, `PhysicsSystem.*` and `JoltPhysicsAux.h` (speed in the snapshot),
  `GameObjectClassesLua.cpp`, `AssetTypes.cpp`, `AssetIndex.cpp`, `Reflect.h` (`AssetKind`),
  `CommandCatalog.h` (contexts), `DefaultCommands.cpp`, `engine/player/variants.json`.

## 9. Risks & defaults

- **Event and clip both set on a source.** Default: the event wins, a warning row says so (no
  silent fallback when the event is missing, Unity's "Missing").
- **Hidden cost of triggers.** Default: lazy subscriptions; AC6 proves zero subscriptions without
  flags.
- **A large Browser.** sound-layer's was 6,655 lines; default: the clip detail is S36's Inspector
  view reused, not a copy.

**Resolved questions**
- **Q1. Release-player size of the sound runtime** (S36 M3, S37, S39, S40) — **resolved
  2026-11-01 by the user**: the `soundlayer` module goes into **every** prebuilt player variant,
  like navigation; Compile builds still strip it when unused (§6.6). The rejected options were
  leaving it out of the lite variants (a small game with one Sound Event would move to a variant
  4.7–8.9 MB larger) and doubling the variants.

## Milestones

- **M1** Format, identity, edges, relinking, `SoundEventIO`, `SoundEventLibrary` (R1–R5); AC1, AC2.
- **M2** Runtime, RNG, AudioSource fields, event mode, clip variation (R6–R11); AC3–AC5.
- **M3** Triggers and `Contact3D:getApproachSpeed` (R12); AC6.
- **M4** Lua/C++ `Audio` API, `SoundHandle`, capture fields (R13, R14); AC7, AC8.
- **M5** Inspector drawers, `.soundevent` view, audition, create commands, `edit_sound_event`
  (R15–R17, R20); AC10.
- **M6** Sound Browser (R18); AC9. Needs S36 M2/M7.
- **M7** Exports, the `soundlayer` module, budgets, docs (R21–R24, §6.6); AC11–AC13.

## Deviations

- **S35 lands after M1-M4.** There is no capture, deterministic driver or `audio.seed` yet, so:
  - `AudioSystem::setVoiceObserver` and `AudioVoiceLine` hold R14's fields and are the seam S35's capture plugs
    into; `JASPERENGINE_AUDIO_TRACE=1` logs each line as JSON, which is how the MCP tests read them. AC3-AC7 are
    verified on these lines rather than on capture files; AC8 waits for S35.
  - A "deterministic run" is, until S35, a run mixed on the null driver (`AudioSystem::isDeterministic`), whose cursor
    is the frames it mixed. This includes `--headless` players, which have no output to vary.
  - The `audio.seed` setting (Project Settings > Audio, `AudioConfig::seed`, `game.json`) is added here. S35 keeps it.
- **`hasSubscribers()` is not bound to Lua.** AC6's "no subscription" is `SoundEventTriggerTest.NoFlagNoSubscription`,
  not an MCP test.
- **The `type` vocabulary is listed in `SoundEventIO`** as well as in S36's `editor/audio/SoundTypes.h`: the engine
  cannot include an editor header. M6 adds a test that the two lists match, once S36 is on `main`.
- **`Audio.play`'s `delay`** runs on frames of scaled time (`unscaled`: of unscaled time), so it is exact to a frame;
  the event's own `delay_ms` stays exact to the sample (R7).
- **`AudioStealPolicy` and the source cap:** a play's voices never steal from each other through `maxVoices`. A voice
  with no one-shot to steal is rejected with reason `max_voices` (a reason R14 did not list).
- **`AudioSource.event` and S73.** S73 landed first: `event` is `AssetKind::SoundEvent` in the generated table, with one added row in `asset_ref_rows_frozen.txt` and the six new AudioSource fields in `parity_golden`.
- **`SoundSearch::eventProblems`** is added as S36 left it (the analysis cache, near-duplicates, inaudible layers). Match Levels
  lives in `SoundEventRender::matchLevels`, not `SoundSearch`: it measures clips as they play, with their import settings.
- **The Browser's detail pane is the Inspector** for project sounds: selecting a card inspects the asset, which is S36's clip view
  or R16's editor. A built-in clip has no file yet, so the Browser shows its description and licence.
- **Previews stop for a capture through `AudioSystem::isOfflineCapture()`** (the Recorder). S35 adds its own capture to that test.
- **AudioSource's rows are in the hand drawer `drawAudioComponent`** (`drawAudioEventRows`), not named drawers: the component has no
  reflection-drawn Inspector to host them.
- **R16's labels row is read-only:** labels are edited in the Project window's bar, S25's one place for them.
- **AC12's bench is in `editor/tests/audio/SoundEventBenchTest.cpp`** (suite `SoundEventBench`), not `engine/tests/core/bench`: it plays through the same
  `AudioSystem` as the other runtime tests, with the fixtures the editor's test target already has. Server F no longer exists; it ran on the server named in the report.
- **AC11's tests are `test_audio_events_prebuilt.py` and `test_audio_module_compile.py`**, not `test_build.py::test_sound_events_prebuilt`: each needs its own project,
  and the second a project that never held an event. Picks are read from the voice lines until S35's player capture lands.
- **`editor/audio/SoundDrop` adds the drops** of M6 (a sound onto a Button, an entity or empty scene space); a built-in sound is imported first, on one undo step with the assignment.
- **The module's size is measured on Linux only** (S34's last M entry): the Android and wasm players were not built for it.
- **AC12's budgets are printed, not enforced, without `JASPER_TEST_PERF_SLACK`**, as for every wall-clock test. Server F no longer exists, and the bench run was not on the benchmark server.
