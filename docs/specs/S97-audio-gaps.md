# S97 — Audio gaps from the JasperArcade port: source effects, clip lifetime, Spatial Blend, several Audio Sources per GameObject

**Status: implemented** on `feature/s97-audio-gaps` (WP-A..WP-D and the integration). What the code
does where it differs from this text is in "Deviations" at the end.

Arcade engine-gaps program (S92-S98), category "Audio" of the port's ledger
(`JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md` AU1-AU7, and the audio part of S5). The port
was made against engine commit `7f48d7e8d` (2026-10-30). Since then the audio mixer
(`bb7c49aa1`), DontDestroyOnLoad (`7be98af28`), Sound Events and free voices (S37, `17f1685d7`) and
the music system (S39, `02fccb80d`) landed, so most of the category is already done. This spec
triages every item against the current code and specs what is left. Paths are relative to the
repository root. "Inferred" marks a claim read from code, not run.

## 1. Goal

A Unity user porting a game should find every audio tool they used: filters on one Audio Source
(Unity's Audio Low Pass Filter and its siblings), Spatial Blend, two Audio Sources on one
GameObject, clip load types set on the clip, and an Audio Clip whose load state a script can read.
Clips must also stay cheap to switch: a source that changes clip and changes back must not decode
the file again, and the memory that unused clips hold must be bounded and visible. Agents get the
same features through the existing MCP tools, plus a view of loaded clips and memory in
`get_play_state`.

## 2. Scope

**In scope**
- Per-source effects: an ordered effect list on the Audio Source that uses the mixer's effect
  catalogue, with Unity's filter names in Add Component (REQ-1 to REQ-7).
- Clip lifetime: one release rule, an unused-clip cache with a budget, `AudioSystem.unload`, load
  state and a load event, fades that wait for the clip, and a per-clip Load Type import setting
  (REQ-8 to REQ-17).
- Spatial Blend, a GameObject for every new 2D source, and looping free voices (REQ-18 to REQ-23).
- Several Audio Sources on one GameObject (REQ-24 to REQ-30).
- Small fixes found during triage: the inverted MinigameSongs recipe in the guide, the unrecorded
  SoLoud patch, and a stale comment (REQ-31 to REQ-33).

**Out of scope**
- What already exists (§Triage): the mixer, its groups, effects, snapshots and exposed parameters,
  streaming, PlayOneShot, free voices, the music system, DontDestroyOnLoad.
- Unity Audio Reverb Zones, Audio Chorus Filter (SoLoud has no chorus; `flanger` is the nearest),
  spatializer plugins and HRTF. These follow the audio-mixer design's non-goals.
- Mixer sends and receives, and nested mixers (also audio-mixer non-goals).
- A general ECS feature for several components of one type on one entity. REQ-24 solves this for
  Audio Sources only (D6).
- The game's converter (`tools/unity2je`) and its `scripts/shared/Audio.lua`. These are game-side;
  §Triage lists what the game can switch to.
- Nothing from the README do-not-copy list is involved. No OpenEngine code is taken.

## 3. Current state

### Triage

| Item | Verdict | Evidence (current main) | What this spec does |
|---|---|---|---|
| AU1 DSP filters on AudioSource / bus | PARTIAL | Buses are done by `bb7c49aa1`. Mixer groups are SoLoud buses with up to 8 effects each: `lowpass`, `highpass`, `bandpass`, `echo`, `reverb`, `equalizer`, `distortion`, `flanger`, `lofi`, `bassboost`, `duck`, `limiter` (`engine/core/audio/AudioEffects.h:111-125`, `AudioMixer.h:67-245`). Exposed parameters move over time (`AudioMixer::setFloat`, `:125`) and snapshots exist (`:136-142`). The Lua API is `ECSClassesLua.cpp:1188-1240`, and the guide's MinigameSongs recipe covers the "muffled radio" (`docs/mcp/guide/audio.md:256-273`). **Per source there is nothing**: `AudioSourceComponent` has no effect field (`component/AudioSourceComponent.h:106-218`) | REQ-1 to REQ-7: per-source effects |
| AU2 Whole files decoded (streaming) | FIXED | Done by `bb7c49aa1`. Load Type Auto streams files of 1 MiB or more (`pool/SoundPool.cpp:449-459`, `SoundPool.h:75`) through `WavStream`. A packed file is held compressed instead. Test `AudioSourceTest.AutoStreamsBigFilesAndDecodesSmallOnes` (`editor/tests/AudioMixerTest.cpp:809`). The port's songs are several MB each, so they stream | Unity parity on top: a Load Type on the clip's import settings (REQ-16, REQ-17) |
| AU3 No PlayOneShot, no mixer groups | FIXED | Done by `bb7c49aa1`. `AudioSource::playOneShot` (`object/sound/AudioSource.h:134`; test `OneShotsOverlapUpToMaxVoicesTheOldestStolen`, `AudioMixerTest.cpp:657`), `outputGroup` (`:91-93`) and the mixer. Done by `17f1685d7`: free voices `Audio.play(clip, {volume, pitch, group, target, position, scene})` → `SoundHandle` (`object/sound/Audio.h:72-79`, `script/binding/AudioClassesLua.cpp:45-80`) | Only the loop option for free voices is missing (REQ-22) |
| AU4 One AudioSource per GameObject | MISSING | The ECS has one component of a type per entity, and the Inspector refuses a second one (`editor/window/InspectorWindow.cpp:1932`) | REQ-24 to REQ-30 |
| AU5 2D sources have no GameObject; clip switching reloads | PARTIAL | **2D**: a source is 3D exactly when its entity has a Transform (`AudioSource.cpp:151-166`, `subsystem/AudioSystem.cpp:2114`). `AudioSource(scene)` makes a bare entity (`AudioSource.cpp:23-25`), and `getGameObject()` on it throws (`:47-54`). There is no Spatial Blend, so a source on a GameObject always plays 3D, panned by its position (inferred: a Unity 2D source converted onto a GameObject pans). **Switching**: `loadSound(other)` calls `destroySound` (`AudioSource.cpp:60-62`) → `releaseSample` → `SoundPool::remove` (`AudioSystem.cpp:997-1005`), which drops the clip when the switching source was its last holder. Switching back decodes it again (inferred) | REQ-8, REQ-9 (switching), REQ-18 to REQ-21 (Spatial Blend, GameObject) |
| AU6 No release rule; no loaded state or callback for `loadSound` | PARTIAL | `loaded` and `loadFailed` exist on the component only (`ECSClassesLua.cpp:1190-1191`); `AudioSource` has no load state (`GameObjectClassesLua.cpp:4756-4811`) and `getLength()` is 0 while loading. The release rule is uneven and documented nowhere: (a) a source's clip is dropped when its last source lets go (above); (b) clips of one-shots, free voices and Sound Events stay in the pool for the whole process, because `Engine::clearUnusedPools` runs only on low memory (`device/Device.cpp:263`) and on editor project close (`editor/Project.cpp:6481`), never on a scene load (the ledger's `Engine.cpp:905` is the function's definition, not a call, at `7f48d7e8d` too); (c) `AudioSystem.preload` pins until shutdown and nothing unpins (`AudioSystem.cpp:989-995`, `SoundPool.cpp:617-620`). A fade advances while the clip loads (`AudioSystem.cpp:1941-1947`, called at `:2363` with or without a voice), so a `fadeIn` on a loading clip starts part-way up (inferred) | REQ-8 to REQ-15 |
| AU7 Loading another file drops a shared pool entry other sources use | NOT-A-BUG (as stated) | `SoundPool::remove` erases only when `use_count() <= 1` (`SoundPool.cpp:602`), and `releaseSample` resets the component's own pointer first (`AudioSystem.cpp:1001-1003`). Another source that holds the clip keeps it, and so did `7f48d7e8d` (`SoundPool.cpp:231` there). The real cost the ledger saw is the AU5 switching case: the voice that switched was the clip's last holder | Covered by REQ-8 and REQ-9. A regression test pins the shared case (AC9) |
| S5 (audio) Audio stops on scene load | FIXED | `7be98af28` (DontDestroyOnLoad) with `AudioSystem::moveSourceVoices` (`AudioSystem.h:447-453`, `registry/EntityCloner.cpp:1486-1492`; test `MovedVoicesFollowTheirSourceToAnotherScene`, `AudioMixerTest.cpp:859`). Music survives every load through `Audio.playMusic` (`02fccb80d`) | S95 owns persistence in general. For audio, a 2D source needs a GameObject before it can be made persistent (REQ-19), and a free voice owned by the persistent scene must survive a load (AC22) |

Counts: FIXED 3 (AU2, AU3, S5 audio), PARTIAL 3 (AU1, AU5, AU6), MISSING 1 (AU4), NOT-A-BUG 1
(AU7), GAME-SIDE 0.

**What the port can use today** (game-side, for the owner, not engine work):

| `scripts/shared/Audio.lua` / `LMusic.lua` / `CCarSound.lua` | Engine feature now |
|---|---|
| pre-filtered `*.menufx.ogg` copies cross-faded in sync | A Music group with `lowpass` and `highpass`, exposed and swept with `AudioMixer.setFloat(name, value, seconds)` or two snapshots |
| the 16-voice pool, `playOneShot`, `playRandomPitch`, the bank | `Audio.play(path, {volume, pitch = 0.9 + math.random() * 0.2, group = "SFX"})`, or a `.soundevent` with pitch randomisation |
| bus mutes in Lua, `Audio.register` | exposed group volumes with Remember (`AudioMixer.setFloat("SFXVolume", -80)`) |
| `LMusic` crossfades with `destroySound` and at most 2 songs held | `Audio.playMusic(path, {crossfade = 1})`, streamed |
| music re-created per scene and resumed with `seek` | `Audio.playMusic` survives loads |
| `Audio.loop` (engine hum) | an Audio Source with `looping`, or `Audio.play(path, {loop = true})` after REQ-22 |
| `CCarSound` with 4 sources | one source switching clips after REQ-8; or several sources on the kart after REQ-24 |

### Other findings during triage

- The guide's MinigameSongs recipe is inverted (`docs/mcp/guide/audio.md:263-271`). `toGameplay`
  sets 525 Hz / 430 Hz, but the game's own values (`Audio.lua` `menuEffects`/`gameEffects`) are
  525 Hz low-pass and 430 Hz high-pass for the **menu**, and 22000 Hz and 10 Hz for gameplay.
- `FILTERS_PER_STREAM` is 9 in the vendored SoLoud (`engine/libs/soloud/include/soloud.h:84`,
  upstream 8; `bb7c49aa1`), but the patch ledger in `engine/libs/soloud/CMakeLists.txt:2-24` does
  not list it.
- `SoundPool.h:36` says `get_play_state` reads the pool entries; it does not
  (`editor/mcp/tools/PlayTools.cpp:651-667`).
- How SoLoud instantiates filters: `Soloud::play` creates a voice's filter instances from the
  source's `mFilter[]` at play time (`engine/libs/soloud/src/core/soloud_core_basicops.cpp:96-101`).
  `Soloud::setFilterParameter` and `fadeFilterParameter` then address a voice handle. Samples are
  shared by every source of a file (`SoundPool`), so filters must never be set on the shared sample
  (D2).

## 4. Reference

- Unity: Audio Low Pass Filter, Audio High Pass Filter, Audio Echo Filter, Audio Distortion Filter
  and Audio Reverb Filter (components that filter the GameObject's Audio Source). `AudioSource.spatialBlend`
  (0 = 2D, 1 = 3D). `AudioClip.loadState` and `AudioDataLoadState` (Unloaded, Loading, Loaded,
  Failed). `AudioClip.LoadAudioData` / `UnloadAudioData`. AudioImporter Load Type per clip. Several
  Audio Sources per GameObject (`GetComponents<AudioSource>()`).
- In this repository: the audio-mixer design (`docs/superpowers/specs/2026-10-31-audio-mixer-design.md`,
  history only, not rules). S36 §4.2 decided "no per-clip load type", and REQ-16 supersedes that
  decision. `engine/core/audio/ClipSource.{h,cpp}` is the pattern for an engine `SoLoud::AudioSource`
  that wraps a shared sample. The mixer's effect catalogue and parameter curves are in
  `AudioEffects.{h,cpp}` and `AudioMixer.cpp`.
- The port: `scripts/shared/Audio.lua`, `scripts/l/LMusic.lua`, `scripts/c/CCarSound.lua` (API
  shapes only).

## 5. Requirements

### Functional — per-source effects (AU1)

- **REQ-1 Effect list.** `AudioSourceComponent` gains `effects`, an ordered list of up to 4 entries
  `{type, bypass, wet, params}`. The types come from `AudioEffects::all()`, except `duck` and
  `limiter`, which stay bus-only (D3). Parameter names, ranges, defaults and curves are the mixer's
  (`AudioEffectParamInfo`). An unknown type or parameter in a file is kept and inactive, as the mixer
  does.
- **REQ-2 What it filters.** Every voice of the source goes through its effects: the main voice,
  one-shots, a layered Sound Event's voices, and timeline starts. This happens before the source's
  output group. A voice that already plays keeps the chain it started with when the list
  **structure** changes (add, remove, reorder, type). The next start takes the new chain. Changes to
  **values** (parameters, wet, bypass) reach every live voice in the same frame.
- **REQ-3 Timed changes.** A parameter change can take `seconds`, with the mixer's curves (dB and
  plain values linear, Hz along log2) on scaled time unless `unscaled`. The component's stored value
  is the value heard now, so `get_runtime_entity` and the Inspector show it as it moves.
- **REQ-4 Off is free.** A source with an empty list, or with every effect bypassed when it starts,
  plays its sample directly, exactly as today: the same voice, no wrapper, no filter instance. A
  low-pass at or above 0.45 × the sample rate and a high-pass at or below 10 Hz count as bypassed
  (the mixer's rule, `AudioEffects.h:166-169`).
- **REQ-5 Unity names.** Add Component lists Audio Low Pass Filter, Audio High Pass Filter, Audio
  Echo Filter, Audio Distortion Filter and Audio Reverb Filter. Each one adds that effect to the
  GameObject's Audio Source, and adds an Audio Source first if the GameObject has none. The
  glossary maps them, and maps Unity's `cutoffFrequency` / `lowpassResonanceQ` to `cutoff` /
  `resonance`.

### Functional — clip lifetime and load state (AU5 switching, AU6, AU7)

- **REQ-8 One release rule.** A clip (a `SoundPool` entry) is **held** while a source, a voice
  (main, one-shot, free, music, cue) or a preload pin holds it. When the last holder lets go it
  becomes **unused** and stays cached. It is dropped only by:
  - the unused-clip budget (REQ-10);
  - `destroySound()` or `AudioSystem.unload(path)` while it is unused;
  - a low-memory warning (existing);
  - engine shutdown and project close (existing).
  A scene load does not drop unused clips (D5).
- **REQ-9 Switching keeps the old clip.** `loadSound(other)` stops the source's voices and releases
  its clip as unused. It no longer drops the clip (`AudioSource.cpp:60-62`). Switching A → B → A
  decodes A once while it fits the budget. `destroySound()` still drops an unused clip at once and
  is the documented way to free memory now.
- **REQ-10 Unused-clip budget.** Project Settings > Audio > **Unused Clip Cache** (MiB, default 16,
  0 = drop at release as today) caps the bytes of unused entries. Past the cap, the least recently
  released entries go first. Streamed entries count 0 bytes, and at most 256 unused entries are
  kept. The setting reaches exports (`game.json` / generated init, like Max Real Voices).
- **REQ-11 Unload.** `AudioSystem.unload(path [, loadType])` removes the preload pin of the path
  and drops its entries that are unused. It returns how many it dropped. A clip still held stays,
  and is dropped when released.
- **REQ-12 Load state.** `AudioSource.loadState` (read-only) returns `AudioLoadState.Unloaded`,
  `Loading`, `Loaded` or `Failed` (Unity's names). `AudioSystem.getLoadState(path [, loadType])`
  returns the same for a clip in the pool. `getLength()` stays 0 until `Loaded`; the docs say so.
- **REQ-13 Load event.** `AudioSourceComponent.onLoad(ok)` fires once for each `loadSound` (and for
  the scene's own preload of the source's clip), on the main thread, in the audio update of the
  frame the clip is ready or has failed. It never fires synchronously inside `loadSound`. It goes in
  `forEachEvent`, so it is cleared at Stop (`tools/check_event_clears.py`).
- **REQ-14 Fades wait for the clip.** A fade on a source whose start is waiting for its clip begins
  when the main voice starts. A `fadeIn(1)` of a clip that takes 0.5 s to load is at volume 0 when
  its first sample plays.
- **REQ-15 Visible memory.** `get_play_state` `audio` gains `clips`:
  `{count, bytes, unused_bytes, loading, largest: [{path, load_type, bytes, holders}]}`, with up to
  5 entries in `largest`. It reads `SoundPool::getEntries`, which gains `holders` and `unused`.
- **REQ-16 Load Type on the clip.** The clip's import settings (`.meta` `import.audioImporter`) gain
  `load_type`: `auto` (default), `decompress_on_load`, `compressed_in_memory` or `streaming`.
  Resolution order:
  1. a source's own `loadType` when it is not Auto;
  2. else the clip's `load_type` when it is not `auto`;
  3. else Auto by size.
  Free voices, one-shots, events, cues and music use the clip's setting. `load_type` does not change
  `ClipImportSettings::hash()`, because the samples are the same.
- **REQ-17 Load Type reaches exports.** The setting travels in `je-audio-imports.json` (an optional
  key, no version bump), so prebuilt and compiled players resolve it the same way.

### Functional — Spatial Blend, GameObjects, free loops (AU5, S5)

- **REQ-18 Spatial Blend.** `AudioSourceComponent.spatialBlend` (0..1, default 1) works as in Unity:
  - 0 plays 2D (pan from `pan`, no attenuation, no Doppler);
  - 1 plays 3D through SoLoud as today;
  - between them the engine plays a 2D voice with `volume × lerp(1, gain3D, b)` and
    `pan = lerp(pan, pan3D, b)`, computed from the attenuation model on the main thread, with no
    Doppler (D7).
  A source without a Transform is 2D whatever its blend (old scenes).
- **REQ-19 Every new source has a GameObject.** `AudioSource(scene)` creates an entity with a
  Transform and `spatialBlend = 0`, and `getGameObject()` works on it, so it can be parented and
  made persistent. On an old bare entity, `getGameObject()` adds a Transform and sets
  `spatialBlend = 0` instead of throwing; it stays 2D.
- **REQ-20 `sound3D` follows Spatial Blend.** `isSound3D()` means a Transform exists and
  `spatialBlend > 0`. `setSound3D(true)` adds a missing Transform and sets the blend to 1.
  `setSound3D(false)` sets the blend to 0 and keeps the Transform.
- **REQ-21 Editor creation.** GameObject > Audio > Audio Source (MCP `create_entity type
  audio_source`) creates a GameObject with a Transform and Spatial Blend 0. Audio Source 3D creates
  one with Spatial Blend 1. Dropping a clip in the scene (`editor/audio/SoundDrop.cpp:87`) does the
  same.
- **REQ-22 Looping free voices.** `PlayOptions.loop` (Lua `Audio.play(path, {loop = true})`) loops a
  clip's free voice until `SoundHandle:stop()` or its owner scene stops it. A Sound Event's own
  `loop` setting is unchanged.
- **REQ-23 Persistent free voices.** `Audio.play(path, {scene = SceneManager.persistentScene})`
  survives a scene load (AC22 pins this; the code may already do it).

### Functional — several Audio Sources on one GameObject (AU4)

- **REQ-24 Model.** The first Audio Source of a GameObject is the GameObject's own
  `AudioSourceComponent`, as today. Each further one is a **slot**: a child entity with a Transform
  at the local origin and an `AudioSourceComponent` whose new field `slot` is 1, 2, … (default 0 =
  not a slot). It follows the parent, is copied, deleted, prefabbed and made persistent with it, and
  plays through the unchanged `AudioSystem` (D6).
- **REQ-25 Hidden as an entity.** The Hierarchy, `get_scene_tree`, `find_entities` and the Scene
  view picking do not show slot entities. Neither do the Lua `GameObject` child APIs (`childCount`,
  `getChild`, `getChildren`, `findChild`) nor `GameObject.find`: in Unity a further Audio Source is a
  component, not a child.
- **REQ-26 Shown as components.** The Inspector draws the slots of the selected GameObject as
  further "Audio Source" sections, with the same rows, context menu (Remove, Reset, Copy, Paste,
  Move Up/Down to reorder slots) and undo. Add Component > Audio Source on a GameObject that has one
  adds a slot.
- **REQ-27 Lua.** `AudioSource.getAll(gameObject)` returns the GameObject's source first, then its
  slots in slot order. `AudioSource.add(gameObject)` returns a new source: the GameObject's own if
  it has none, else a new slot. `AudioSource.remove(source)` removes a slot, or the GameObject's own
  component. `findObjectOfType("AudioSource")` finds slots as sources.
- **REQ-28 MCP.** `get_entity` on a GameObject lists `audio_sources: [{entity, slot}]`.
  `add_component AudioSource` on a GameObject that has one creates a slot and returns its entity in
  `structuredContent`, with a `next` hint saying to address it by that id. `set_properties`,
  `remove_component` and `get_entity` work on the slot's id. `delete_entities` on a slot removes it.
- **REQ-29 Slot integrity.** On load, a slot entity that is not a child of a GameObject becomes an
  ordinary entity with a warning naming it, with its slot cleared. Two slots with the same number
  under one parent are renumbered in entity order. Neither case fails the load.
- **REQ-30 Effects per slot.** Each slot has its own effect list (REQ-1); a filter never applies
  across sources (D3).

### Functional — fixes found during triage

- **REQ-31** The MinigameSongs recipe in `docs/mcp/guide/audio.md` uses the game's values: menu at
  525 Hz / 430 Hz, gameplay at 22000 Hz / 10 Hz.
- **REQ-32** The SoLoud patch ledger in `engine/libs/soloud/CMakeLists.txt` lists
  `FILTERS_PER_STREAM 9` (why: 8 group effects plus the meter). If a source wrapper (D2) needs more
  slots, it records that too.
- **REQ-33** The comment at `SoundPool.h:36` names what reads the entries after REQ-15.

### Platform & backends

- **REQ-34** Everything works on Windows, Linux, macOS, Android, iOS and HTML5 with the existing
  SoLoud backends. Nothing needs a thread that the Web build lacks. On the Null and deterministic
  drivers, source effects mix on the main thread like the buses do.

### Performance

- **REQ-35** Disabled costs nothing (REQ-4). No new per-frame work happens for sources without
  effects, at Spatial Blend 0 or 1, or on GameObjects without slots.
- **REQ-36 (revised, coordinator's decision; see Deviations)** Source effects cost what the filter
  costs, nothing more: 32 voices each with a `lowpass` through the per-source wrapper mix within
  1.15x SoLoud applying the same filter natively per voice; a source without effects plays the sample
  itself (no wrapper, no filter); 32 low-passed voices mix one 1024-frame block in under 600 us. Null
  driver, 48 kHz, median of 5, enforced through `perfSlack()`.
- **REQ-37** The unused-clip cache costs O(unused entries) only when an entry is released or
  inserted, never per frame. With an Unused Clip Cache of 0, memory behaves as today.

### Editor UX (human)

- **REQ-38** The Audio Source Inspector gains:
  - a **Spatial Blend** slider, labelled 2D … 3D. The "3D Audio Source Settings" section shows when
    the blend is above 0 and a Transform exists;
  - an **Effects** section listing the effects, each with a bypass checkbox, Wet, its parameters
    (the mixer's labels and units) and a cost badge for `reverb` and `equalizer`;
  - an **Add Effect** menu, plus reorder and remove.
  Every edit is one undo step through the existing property commands.
- **REQ-39** The clip's Import Settings show **Load Type** (Auto, Decompress On Load, Compressed In
  Memory, Streaming) with undo.
- **REQ-40** Project Settings > Audio shows **Unused Clip Cache (MiB)** with a reset, searchable in
  settings search.

### Agent UX (MCP, Lua, CLI)

- **REQ-41** Lua (stubs in `engine/core/script/binding/stubs/Audio.lua`, documented in
  `docs/mcp/guide/lua-api.md` and `audio.md`):
  - `AudioSource`: `spatialBlend`, `loadState`, `addEffect(type) -> AudioSourceEffect`,
    `getEffect(indexOrType)` (1-based), `removeEffect(indexOrType)` and `getEffectCount()`;
  - static: `AudioSource.getAll`, `add` and `remove`;
  - `AudioSourceEffect`: `type`, `bypass`, `wet`, `setParameter(name, value [, seconds [, {unscaled}]])`,
    `getParameter(name)`, `hasParameter(name)` and `isValid()`, mirroring `AudioMixerEffect`;
  - `AudioSystem.unload` and `AudioSystem.getLoadState`;
  - the `AudioLoadState` enum and `PlayOptions.loop`.
- **REQ-42** MCP adds no new tool:
  - the effects, `spatialBlend` and `slot` are component properties reached through
    `get_entity`, `set_properties` and `set_runtime_property`;
  - `set_properties` refuses an unknown effect type or parameter with an error that lists the valid
    ones;
  - `set_audio_import_settings` accepts `load_type`;
  - `get_play_state` gains `audio.clips` (REQ-15);
  - `add_component` and `get_entity` handle slots (REQ-28).
- **REQ-43** Each new piece of MCP text gets a case in `tools/mcp/eval/cases.py` that fails without
  it:
  - (a) muffle one source: the agent uses the source's effects, not a new mixer group;
  - (b) make a sub-1-MiB loop stream: the agent uses `set_audio_import_settings load_type`;
  - (c) add a horn as a second source on a car: the agent uses `add_component` twice and sets
    properties on the returned slot.
  `tools/mcp/tool_budget.py --check` passes; lower `budget.json` where text is cut.

### Data & compatibility

- **REQ-44** All new fields are optional with defaults and need no version bump:
  - `effects` (empty, omitted when empty);
  - `spatialBlend` (1, omitted when 1; written as 0 by new 2D sources);
  - `slot` (0, omitted);
  - the import key `load_type` (auto, omitted);
  - the project key `audio.unusedClipCacheMb` (16).
  Old scenes load and sound the same: a bare 2D entity stays 2D, and a source with a Transform keeps
  blend 1.
- **REQ-45** Reflection drives serialization, the Inspector, MCP and generated code:
  - fields go in `editor/reflect/components/CameraAudioReflect.cpp`;
  - `engine/core/serialization/RuntimeFields.gen.inc` is regenerated;
  - `ReflectParity` goldens and `editor/tests/reflect/catalog_snapshot.json` are updated;
  - compiled exports set the new fields (`Factory.cpp` codegen) and cooked scenes carry them.
- **REQ-46** A slot (REQ-24) is stored as an ordinary child entity, so an engine without S97 loads
  it as a child GameObject with an Audio Source. Downgrades lose nothing.

## 6. Design constraints & decisions

- **D1 One effect catalogue.** Source effects reuse `AudioEffects` (types, parameter tables,
  curves, bypass ratios) and the mixer's filter construction. The mixer gains a `perSource` flag per
  effect type (false for `duck`, `limiter`). There are no separate Unity-style filter components:
  with several sources per GameObject (REQ-24) a filter component could not say which source it
  filters, and one list keeps Lua, MCP and the Inspector to one shape. Add Component keeps Unity's
  names (REQ-5).
- **D2 A wrapper per source, never filters on the shared sample.** A source with active effects
  plays through an engine `SoLoud::AudioSource` that wraps the shared sample. The wrapper follows
  `AudioClipSource`: it holds the sample's `shared_ptr`, its `createInstance` delegates, and its
  `mFilter[]` holds the source's filters. `Soloud::play` then gives each voice its own filter
  instances. Value changes go per voice handle (`setFilterParameter` / `fadeFilterParameter`, or
  per-frame values for timed changes, like the mixer). The wrapper is rebuilt only when the
  structure changes. Setting `mFilter` on the pool's sample is forbidden, because every source of
  that file shares it. Up to 4 effects per source fit in `FILTERS_PER_STREAM`.
- **D3 Scope of an effect.** It filters one source's voices (REQ-2, REQ-30). Group-wide processing
  stays on mixer groups, and `duck` and `limiter` stay there.
- **D4 Lua sketch.**
  ```lua
  local src = AudioSource(self.scene, self.entity)
  local lp = src:getEffect("lowpass") or src:addEffect("lowpass")
  lp:setParameter("cutoff", 525, 0.9)          -- Hz, over 0.9 s of scaled time
  src.spatialBlend = 0
  src:getAudioSourceComponent().onLoad = function(ok) if ok then src:fadeIn(1) end end
  src:loadSound("music/boss.ogg")
  if src.loadState == AudioLoadState.Loaded then print(src:getLength()) end
  local horn = AudioSource.add(self.gameObject)    -- a second source on this GameObject
  AudioSystem.unload("music/world.ogg")
  ```
- **D5 The unused-clip cache survives scene loads.** Unity unloads unused assets on a single-mode
  scene load. We keep them within the budget instead, so SFX shared by levels are not decoded again
  at every load. The budget bounds memory, and low memory still drops them. Pool bookkeeping is
  main-thread only (as `SoundPool::replace` is): load jobs only produce the data.
- **D6 Slots are hidden child entities.** A general "several components of one type" ECS change
  would touch every system, the serializer, prefabs and MCP. A child entity reuses `AudioSystem`,
  the Transform hierarchy (it follows the parent), `EntityCloner` (copy, prefab, persistence moves)
  and the existing commands (create and delete child = add and remove slot). The cost is filtering
  a few listings (REQ-25). `slot` lives on `AudioSourceComponent`, so a slot is recognisable
  without a new component type.
- **D7 Spatial Blend between 0 and 1 is computed by the engine.** SoLoud has no partial 3D. The
  engine computes the attenuation-model gain and the pan for a 2D voice. At exactly 1, SoLoud's 3D
  path, Doppler included, is unchanged.
- **D8 New 2D sources get a Transform.** A GameObject is what a Unity user expects, and it is what
  parenting and persistence (S5) need. This is a deliberate behaviour change for
  `AudioSource(scene)` and the editor's 2D Audio Source; the docs' "Changes" note it. Sound is
  unchanged (blend 0).
- **D9 Load Type precedence.** The source's explicit Load Type wins over the clip's, because old
  scenes set it per source; the clip's setting wins over Auto. This supersedes S36 §4.2 "No per-clip
  load type"; the implementer adds a line to S36's Deviations.

## 7. Acceptance criteria

Unit tests go in `editor/tests/engine/<area>/` (engine code driven through editor scenes, as
`AudioMixerTest.cpp` does), on the Null or deterministic driver. MCP tests go in
`tools/mcp/tests/` with `CI = True`. "Red" names the run that fails without the change. Run
narrowly: `UNIT_SUITE=editor UNIT_TEST_ARGS=--gtest_filter=<Suite>.*`,
`tools/build.sh mcp-tests -k <file> -j 2`, `tools/check-changed`.

| AC | Verifies | Check (red control) |
|---|---|---|
| AC1 | REQ-1, REQ-2 | `AudioSourceEffects.LowPassFiltersEveryVoiceOfTheSource` (`editor/tests/engine/subsystem/AudioSourceEffectsTest.cpp`): a 5 kHz sine source with a `lowpass` at 500 Hz; its group's meter reads ≥ 20 dB below an unfiltered twin's, for the main voice and a one-shot. Red: no `effects` field |
| AC2 | REQ-2, D2 | `AudioSourceEffects.TwoSourcesOfOneClipFilterApart`: two sources of one file, one filtered; the unfiltered one's level equals a lone play's (±0.1 dB), and the pool's sample has no filter (`mFilter` all null) |
| AC3 | REQ-2 | `AudioSourceEffects.StructureChangesTakeTheNextStart`: adding a second effect during play leaves the voice's level unchanged until `play()` again; a cutoff change is heard within the same frame's mix |
| AC4 | REQ-3 | `AudioSourceEffects.TimedCutoffMovesAlongLogFrequency`: `setParameter("cutoff", 500, 1)` from 20000; at 0.5 s of scaled time the component reads 3162 Hz ±1 %; with `{unscaled = true}` and time scale 0 it still moves |
| AC5 | REQ-4, REQ-35 | `AudioSourceEffects.EmptyOrBypassedPlaysTheSampleDirectly`: a source with no effects, and one with a 22000 Hz low-pass at 48 kHz, start voices whose sound is the pool sample (no wrapper) and `getDebugCounters` filter builds stay 0. Red: a wrapper always made |
| AC6 | REQ-36 | `AudioSourceEffectsBench.LowPassCostOn32Voices` (suite ends in `Bench` per its file; timings through `perfSlack()`): wrapper over native ratio ≤ 1.15, a block under 600 us (REQ-36 revised) |
| AC7 | REQ-5, REQ-38 | `tools/mcp/tests/test_audio_source_effects.py::test_add_component_filter_aliases`: `add_component "Audio Low Pass Filter"` on an empty GameObject gives it an Audio Source with one `lowpass`; `undo` removes both. Manual: screenshot of the Inspector's Effects section and Spatial Blend slider from `tools/editor-shot.sh` on a scratch project |
| AC8 | REQ-8, REQ-9 | `SoundPoolLifetime.SwitchingBackDoesNotDecodeAgain` (`editor/tests/engine/pool/SoundPoolLifetimeTest.cpp`): A → B → A on one source decodes A once (a decode counter in `SoundPool`). Red: two decodes |
| AC9 | REQ-8, AU7 | `SoundPoolLifetime.ASharedClipStaysWhileAnotherSourceHoldsIt`: source 1 switches away from A while source 2 plays A; the entry stays and source 2's next `play()` decodes nothing (this passes today; it pins the AU7 verdict) |
| AC10 | REQ-10, REQ-37 | `SoundPoolLifetime.UnusedClipsStayWithinTheBudget`: budget 1 MiB, release 4 decoded clips of 400 KB; unused bytes ≤ 1 MiB, the oldest released ones go first, a held clip is never evicted, streamed entries stay. With budget 0 every release drops at once |
| AC11 | REQ-11, REQ-8 | `SoundPoolLifetime.UnloadDropsPinsAndUnusedEntries`: `preload` then `unload` returns 1 and `getLoadState` is Unloaded; `unload` of a held clip returns 0 and the clip drops at its release. Red: no `unload` (the pin stays) |
| AC12 | REQ-8, D5 | `SoundPoolLifetime.FreeVoiceClipsAreBounded`: 50 different 100 KB clips through `Audio.play`, all ended: unused bytes ≤ the budget. Red: all 50 stay (5 MB) |
| AC13 | REQ-12, REQ-13 | `SoundPoolLifetime.LoadStateAndOnLoadUnderAsyncLoading` (async loading on, real decode job): right after `loadSound`, `loadState` is Loading and `onLoad` has not fired; it fires once with true in a later update, and `loadState` is Loaded with `getLength() > 0`; a missing file fires once with false and Failed. Red: no `loadState` / `onLoad` |
| AC14 | REQ-14 | `SoundPoolLifetime.FadeInStartsWithTheVoice`: async load held 10 frames (test hook), `fadeIn(1)`; the first mixed block of the voice has a peak ≤ the fade's first 20 ms gain. Red (inferred): it starts at the elapsed fraction |
| AC15 | REQ-15, REQ-33 | `tools/mcp/tests/test_audio_clip_memory.py::test_play_state_clips`: a Play with a 2 MB stream and two decoded clips; `get_play_state` `audio.clips` has `count` 3, the stream at 0 bytes, `largest` sorted by bytes; after `destroySound` via `eval_lua` the count drops. Red: no `clips` key |
| AC16 | REQ-16, REQ-17, REQ-39 | `AudioImportSettings.LoadTypeResolvesAfterTheSource` (`editor/tests/audio/AudioImportSettingsTest.cpp`): source Auto + clip `streaming` → Streaming for a 100 KB file; source DecompressOnLoad wins; `hash()` unchanged by `load_type`. `test_audio_import.py::test_load_type_key`: `set_audio_import_settings load_type` round-trips through the `.meta`, undo restores it, and a prebuilt export's `je-audio-imports.json` carries it (`test_prebuilt_audio.py` extended) |
| AC17 | REQ-18, D7 | `AudioSpatialBlend.BlendScalesGainAndPan` (`editor/tests/engine/subsystem/AudioSpatialBlendTest.cpp`): a source 10 units to the right, linear rolloff to 20. Blend 0 gives pan 0 and gain 1. Blend 1 gives today's 3D values. Blend 0.5 gives gain 0.75 ±0.01 and pan halfway ±0.02 |
| AC18 | REQ-18, REQ-44 | `AudioSpatialBlend.OldScenesSoundTheSame`: a scene file from before S97 (fixture: a bare 2D entity and a Transform source) loads with blends absent; their voices' pan and gain equal the pre-change values recorded in the fixture's comment |
| AC19 | REQ-19, REQ-20, D8 | `AudioSpatialBlend.NewTwoDSourceHasAGameObject`: `AudioSource(scene)` → `getGameObject()` succeeds, `sound3D` false, parenting it under a moving object leaves its pan 0; `sound3D = true` → blend 1. Red: `getGameObject` throws |
| AC20 | REQ-21 | `test_audio_mixer.py::test_create_audio_source_spatial_blend` (extend): `create_entity audio_source` gives a Transform and `spatialBlend` 0; `audio_source_3d` gives 1 |
| AC21 | REQ-22 | `AudioSourceTest.FreeVoiceLoopsUntilStopped` (in `AudioMixerTest.cpp`): `Audio.play(clip, {loop = true})` of a 0.1 s clip is still playing after 1 s and stops on `stop()` |
| AC22 | REQ-23, S5 | `AudioSourceTest.PersistentFreeVoiceSurvivesALoad`: a free voice owned by the persistent scene plays across `SceneManager.loadScene`; one owned by the main scene stops |
| AC23 | REQ-24, REQ-26, REQ-29 | `AudioSourceSlots.SecondSourceIsAHiddenChildThatFollows` (`editor/tests/engine/object/AudioSourceSlotsTest.cpp`): add, play both, move the parent (the slot's 3D position follows), duplicate (2 + 2 sources), delete (both gone), undo of each. `AudioSourceSlots.BrokenSlotsLoadWithAWarning` covers REQ-29 |
| AC24 | REQ-25, REQ-28 | `tools/mcp/tests/test_audio_source_slots.py`: `add_component AudioSource` twice; `get_scene_tree` and `find_entities` list one entity; `get_entity` lists 2 `audio_sources`; `set_properties` on the slot id sets `filename`; in Play both play (`get_play_state` voices 2); save, reopen, same. Red: the second `add_component` refused |
| AC25 | REQ-27 | `test_audio_lua.py::test_audio_source_slots_lua` (extend): `AudioSource.add`, `getAll` (order), `remove` through `eval_lua` in Play |
| AC26 | REQ-24, S5 | `AudioSourceSlots.SlotsMoveWithDontDestroyOnLoad`: a persistent GameObject with 2 playing sources keeps both voices across a load |
| AC27 | REQ-34 | `tools/build.sh web` and `tools/build.sh android` build. The web gate (`tools/build.sh web-gate`) passes. `test_prebuilt_audio.py` passes with a source that has a low-pass and blend 0 |
| AC28 | REQ-41, REQ-43, REQ-31, REQ-32 | `tools/lua_api/tests/test_lua_api.py` (it also runs the guide's Lua blocks, the D4 sketch among them); `tools/mcp/gen_tool_docs.py --check`; `tool_budget.py --check tools/mcp/budget.json`; the three eval cases pass on an Orquester account, each with its fail-without run recorded; `python3 tools/check_event_clears.py` |
| AC29 | REQ-44, REQ-45 | `ReflectParity.*` with the new keys; `ReflectRegistry.CatalogSnapshot`; `AudioSourceSerializationTest.AbsentS97KeysAreTheirDefaults` (extend `AudioMixerTest.cpp:1054`); a compiled-export smoke (`SMOKE_METHOD=compile tools/smoke-test.sh`) of a scratch project with a filtered slot |

## 8. Dependencies & conflicts

- **Depends on (landed):** the audio mixer (`bb7c49aa1`), S36 (import settings, `AudioClipSource`),
  S37 (free voices), S39 (music), persistence (`7be98af28`), S08 (reflection), S24 (undo).
- **Program overlaps:**
  - **S95 (scripting):** owns persistence and the S5 ledger item. S97 only makes 2D sources
    persistable (REQ-19) and pins two audio cases (AC22, AC26). If S95 adds a generic Lua
    `getComponents`, it should return slots for `"AudioSource"` (REQ-27 is the audio-side API).
  - **S98 (editor/MCP):** touches `get_entity`, `add_component`, the Hierarchy and
    `tools/mcp/budget.json`. Coordinate on `ComponentTools.cpp` and `HierarchyWindow.cpp`.
  - **S94 (rendering):** no file overlap expected.
- **Merge hotspots:**
  - `engine/core/component/AudioSourceComponent.h`;
  - `editor/reflect/components/CameraAudioReflect.cpp`;
  - `engine/core/serialization/RuntimeFields.gen.inc` (generated);
  - `editor/tests/reflect/parity_golden/` and `catalog_snapshot.json`;
  - `engine/core/subsystem/AudioSystem.cpp` (3,300 lines; every package);
  - `engine/core/script/binding/GameObjectClassesLua.cpp` (the AudioSource class) and
    `ECSClassesLua.cpp`;
  - `engine/core/script/binding/stubs/Audio.lua`;
  - `editor/window/InspectorWindow.cpp` (`drawAudioComponent`, `:9870`);
  - `docs/mcp/guide/audio.md`, `docs/unity-glossary.md`, `docs/mcp/TOOLS.md` and
    `tools/mcp/budget.json`.
  §10 says how each is split.

## 9. Risks & defaults

| Risk / question | Default taken |
|---|---|
| A wrapper source per filtered source may break voice bookkeeping that compares samples (hot reload generation, `stopAudioSource`, the voice budget) | The wrapper is the voice's sample everywhere, as `AudioClipSource` already is. AC2, AC3 and the existing `AudioMixerTest` suites guard this. If a path compares the pool sample, it compares the wrapper's inner sample |
| Echo and reverb instances allocate on the audio thread (SoLoud allocates lazily) | Accepted, as for group effects today. The Inspector shows a cost badge; the guide recommends a group for reverb shared by many sources |
| Keeping unused clips raises steady memory compared with today's drop at release | 16 MiB default, 0 restores today's behaviour, and low memory clears it. AC10 and AC12 bound it |
| Hidden child slots leak into scripts that count children | The Lua child APIs and `GameObject.find` skip them (REQ-25). If a blocker shows up in prefab overrides, the fallback is visible children named "Audio Source 2", recorded in Deviations. The slot data stays the same, so no migration is needed |
| `AudioSource(scene)` gaining a Transform changes what a script sees (D8) | It is deliberate. The sound is unchanged (blend 0, AC19), and "Changes" in `docs/mcp/guide/audio.md` and `docs/scripting.md` notes it |
| Spatial Blend between 0 and 1 loses Doppler | Unity's Doppler also scales with the blend; ours is off below 1. Recorded in the guide |
| Streaming from an Android APK or iOS bundle is not exercised by any test here | Unverified. AC27 builds Android. A device run belongs to `tools/android-emulator-test.sh`, recorded as a follow-up if not run |
| The load event of a clip that is already cached fires a frame later than `loadSound` | Intended: it never fires inside the call, so scripts see one ordering (REQ-13) |

## 10. Work packages

Four packages, each in its own worktree and branch, merged into `feature/s97-audio-gaps`. **WP-A
lands an interface commit first**, named `audio: S97 interfaces`. It contains:
- every new field with its default in `AudioSourceComponent.h` (`effects`, `spatialBlend`, `slot`,
  `onLoad` added to `forEachEvent`), plus `AudioLoadState`, `AudioSourceEffectDef` and
  `PlayOptions::loop`;
- their reflection entries in `CameraAudioReflect.cpp` (serialization only, no Inspector rows);
- the regenerated `RuntimeFields.gen.inc`, parity goldens and catalog snapshot;
- the Lua stub names in `stubs/Audio.lua` (bodies may be stubs that log "not yet").
WP-B, WP-C and WP-D merge it before their first change to those files. After that, each package
adds code only inside its own marked blocks in the shared files:
- `AudioSystem.cpp`: WP-A owns the samples section (`:931-1110`), WP-B the voice start and its
  wrapper, WP-C the 3D pass (`:2108-2200` and the free-voice 3D), WP-D nothing;
- `InspectorWindow.cpp` `drawAudioComponent`: WP-B adds the Effects block, WP-C the Spatial Blend
  row and the 3D-section condition, WP-D the slot sections around the call;
- `audio.md`: each package edits only its own subsection, and the guide's §7 table gets one row per
  package.

**WP-A: clip lifetime, load state, Load Type on the clip (M).** Items AU5 (switching), AU6, AU7,
AU2 parity. REQ-8 to REQ-17, REQ-33, eval case (b).
- Owns: `engine/core/pool/SoundPool.*`; `engine/core/audio/AudioImport.*`; the samples section of
  `AudioSystem.cpp`; `AudioSource::loadSound/destroySound`.
- Also owns: the `AudioSystem` and `loadState` Lua bindings; Project Settings > Audio's new row;
  `editor/mcp/tools/PlayTools.cpp` (`playAudioJson`); `SoundTools.cpp` (the import key); the
  Import Settings row in `InspectorWindowClips.cpp`.
- Tests: `SoundPoolLifetimeTest.cpp`, AC8-AC16.
- Order: interfaces → REQ-8/9/10/11 → REQ-12/13/14 → REQ-16/17 → REQ-15 → docs.

**WP-B: per-source effects (L).** Items AU1 (source part), REQ-1 to REQ-7, REQ-30 (effects per slot
come for free), REQ-32, eval case (a).
- Owns: a new `engine/core/audio/SourceEffects.{h,cpp}` (the wrapper and the per-voice parameter
  updates); the `perSource` flag in `AudioEffects.*`; the voice start path in `AudioSystem.cpp`.
- Also owns: the `AudioSourceEffect` Lua class; the Inspector's Effects block; the Add Component
  aliases (`editor/Catalog.cpp` search entries); `set_properties` validation messages for `effects`;
  `engine/libs/soloud/CMakeLists.txt` ledger.
- Tests: `AudioSourceEffectsTest.cpp`, `test_audio_source_effects.py`, AC1-AC7.
- Depends on: the interface commit only.

**WP-C: Spatial Blend, 2D GameObjects, free loops, guide fix (M).** Items AU5 (2D) and S5 (audio),
REQ-18 to REQ-23, REQ-31.
- Owns: the 3D pass in `AudioSystem.cpp`; `AudioSource.cpp` constructors and `sound3D`;
  `editor/command/type/CreateEntityCmd.cpp:918-925`; `editor/audio/SoundDrop.cpp`.
- Also owns: the free-voice loop (`playFree` and `AudioClassesLua.cpp`); the Spatial Blend row; the
  glossary rows.
- Tests: `AudioSpatialBlendTest.cpp`, AC17-AC22.
- Depends on: the interface commit only.

**WP-D: several Audio Sources per GameObject (L).** Item AU4, REQ-24 to REQ-29, REQ-46, eval case
(c).
- Owns: slot creation and removal commands (`editor/command/`); the Hierarchy filter
  (`HierarchyWindow.cpp`) and picking; the Inspector slot sections and Add Component path
  (`InspectorWindow.cpp:1932`); `ComponentTools.cpp` (`get_entity`, `add_component`) and the
  `get_scene_tree` / `find_entities` filters in `EntityTools.cpp`.
- Also owns: the `AudioSource.getAll/add/remove` bindings; the load-time integrity pass (REQ-29).
- Tests: `AudioSourceSlotsTest.cpp`, `test_audio_source_slots.py`, AC23-AC26.
- Depends on: WP-C (2D slots need Spatial Blend 0, so merge WP-C first). It starts on the interface
  commit, and its tests that need blend 0 land after WP-C merges.

**Integration (whoever merges last):** AC27-AC29, the generated files (`docs/mcp/TOOLS.md` with
`gen_tool_docs.py`, `RuntimeFields.gen.inc` if a later field changed), `tool_budget.py --check`,
`tools/check-changed`, and the lints for the areas touched (`check_event_clears.py`,
`check_raw_writes.py` for the settings write, `check_stream_keys.py`).

## Deviations

- **WP-A, REQ-37: the cache's cost is O(pool entries), not O(unused entries),** paid when an entry is cached, a
  holder releases one, or a free voice ends; never per frame. The pool cannot see which entries became unused
  without reading their holders' counts.
- **WP-A, REQ-10: "least recently released" is exact for releases that name the clip** (a source switching or
  removed, `release`). Clips that ended voices let go are ordered by when they were last handed out.
- **WP-A, REQ-11: `unload` of a held clip marks it**, so its last release drops it instead of caching it (AC11);
  handing it out again clears the mark.
- **WP-A, REQ-16/39: the Load Type row is in the clip Inspector (`editor/window/InspectorAudioClip.cpp`)**, which
  draws Import Settings; `InspectorWindowClips.cpp` holds the animation clips. `load_type` is the last key of the
  section, after `music`, so existing files keep their key order.
- **WP-A, AC14 measures the voice's gain, not a mixed block's peak:** the source's volume and SoLoud's volume of the
  voice in the frame it starts are at most the fade's gain after 20 ms (one 20 ms frame).
- **WP-A, AC16's engine half is `AudioImportSettingsLoadType.ResolvesAfterTheSource`** (a fixture with an asset root
  for the 100 KB file), with `AudioImportSettings.LoadTypeKeyRoundTripsAndApplies` for the key, the hash and the
  export table. The export check is in `test_build_method.py::test_prebuilt_build_hears_the_projects_mixer_and_audio_settings`
  (fast: a stand-in runtime) as well as `test_prebuilt_audio.py` (a real player, `JASPER_SLOW_BUILD=1`).
- **WP-B, REQ-36 revised (coordinator's decision, delegated by the product owner).** The first budget (a low-pass
  on each of 32 voices adds at most 15 % to the null driver's mix of them) measured the biquad, not the design: a
  low-pass costs about 1.4 times what SoLoud's mixing of a plain voice does (x3.1 before the biquad patch below,
  x2.2 after). REQ-36 is now:
  - the per-source wrapper costs at most 15 % over SoLoud filtering the same voices natively (the same
    `BiquadResonantFilter` in the sound's own filter slot, which the engine cannot use on a shared sample, D2);
  - a source with no effects plays the sample itself: no wrapper, no filter (AC5);
  - 32 low-passed voices mix one 1024-frame block in under 600 us (about 3.3x the median measured on the build box).
  `AudioSourceEffectsBench.LowPassCostOn32Voices` checks all three. It takes the median of five interleaved rounds of
  31 blocks each, and `perfSlack()` enforces it (docs/testing.md). Measured on the build box (null driver, 48 kHz):
  73-75 us a block without effects, 170-182 us with SoLoud's native low-pass on each voice, and 170-180 us through
  the sources' effects (x0.98-1.00 of native). The wrapper adds nothing to the mix: a voice's instances are the
  sample's own and the filters' own, and values are written only when they change. No optimisation was needed.
  The SoLoud biquad now keeps its coefficients and state in locals (ledger entry in `engine/libs/soloud/CMakeLists.txt`).
  This cut the filtered mix by about a third for the mixer's groups too.
- **WP-B, REQ-5: the Unity filter aliases are in `editor/audio/SourceEffectEdit.{h,cpp}`**, not `editor/Catalog.cpp`.
  That one list feeds the Add Component dialog (the Audio category and its search), the Search palette and MCP
  `add_component`.
- **WP-B, REQ-2: a longer echo delay is cut on a live voice.** An echo's delay longer than the one its voice started
  with is cut to that delay (SoLoud sizes a voice's echo buffer at its first block). The next start takes the new
  delay. The guide says so.
- **WP-D, AC23/AC26 suite names: `AudioSourceSlotsTest` (runtime: AC23's play-and-follow half, AC25's engine half, AC26) and
  `AudioSourceSlotsEditorTest` (AC23's commands, undo and listings, and `BrokenSlotsLoadWithAWarning`).** A fixture named
  `AudioSourceSlots` would clash with the engine class of that name; the runtime and editor halves need different fixtures.
- **WP-D and integration, REQ-29 runs where the editor decodes a scene (`Stream::decodeSceneProjectEntities`) and where a
  prebuilt player loads a cooked one (`CookedSceneLoader::loadScene`).** The cook writes the editor's scene as it is, so an
  edit after the editor's load (a slot moved to the root, a slot duplicated: inferred) can reach a cooked scene broken
  (`CookedScene.BrokenAudioSourceSlotsLoadRepaired`, red without the loader's pass). A cooked scene on this branch carries
  no parent links (a main bug that S96's PR fixes with a hierarchy record, `c633f787f`), so until that lands a prebuilt
  player loads every slot as a root GameObject (its local position taken as a world one, as every child there), and the
  pass says so in a warning; with the hierarchy record a slot stays under its GameObject. The test therefore covers a slot
  number on a root, which does not depend on that record; the renumbering is covered by the editor test. A compiled
  export's generated scene code does not run the pass: every runtime reader of `slot` (`AudioSourceSlots::ownerOf`,
  `getSlots`, `nextSlot`) already treats a slot off any GameObject as no slot and orders shared numbers by entity, as the
  pass would number them (inferred). The pass itself is engine code (`AudioSourceSlots::repair`).
- **WP-D, REQ-26: a new slot is at Spatial Blend 1, as Add Component's Audio Source on the GameObject; Move Up/Down reorder
  the slots among themselves** (the GameObject's own source stays first). The slot sections are not recorded in
  `get_inspector_layout`, which names one component of a type.
- **WP-D, REQ-25: scripts do not see slots as children.** The first draft let the Lua child APIs count them, as Unity's
  HideInHierarchy objects. Unity's model has no child for a further Audio Source, so `childCount`, `getChild`,
  `getChildren`, `findChild` and `GameObject.find` skip them (`AudioSourceSlotsTest.ScriptsWalkingChildrenSeeNoSlots`). A
  slot is still reachable as a source (`AudioSource.getAll`, `findObjectOfType("AudioSource")`), and `source:getGameObject()`
  on a slot returns the slot's own entity, whose `parent` is the GameObject.
- **WP-D, REQ-28: `get_entity` lists `audio_sources` only on a GameObject that has a slot**; `add_component`'s added entry of
  a slot carries `audio_source` (its entity) and `slot`. `add_component AudioSource` values go to the new slot.

## Progress

- WP-A (clip lifetime, load state, Load Type), WP-B (per-source effects), WP-C (Spatial Blend, 2D GameObjects, free
  loops) and WP-D (several Audio Sources per GameObject) are merged into `feature/s97-audio-gaps`.
- Integration: AC27 (web, web gate, prebuilt audio with a low-pass at blend 0), AC28 (Lua API tests but main's
  Grid/TitleArt bindings, `gen_tool_docs.py --check`, `tool_budget.py --check`, `check_event_clears.py`) and AC29
  (`ReflectParity`, `CatalogSnapshot`, `AbsentS97KeysAreTheirDefaults`, a compiled-export smoke with a filtered
  slot). The cooked loader repairs slots too. The three eval cases have their static checks only: the paid eval
  has not run.
