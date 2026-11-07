# S36 — Sound library and generator: file analysis, placeholders, essentials, the CC0 starter library, import settings and provenance, credits, search

**Wave / Depends on.** Sound wave A (spec-first: implemented after the integration branch lands in
`main`). Depends on **S38 M1** (`AudioMeter`) and on BF14 (OGG streaming and loader fixes S-1–S-4,
which the library's OGGs exercise). Runs in parallel with S35, S37 and S38. Blocks S35 M5
(`audibility` reads clip analysis), S37 M6 (the Sound Browser uses search, waveforms and the
built-in sources), S39 (tempo keys in the audio importer, loop regions), S40 (waveforms, peak
alignment). Owner area: `editor/audio/` (analysis, images, synth, essentials, library, search,
credits, import settings, provenance), `engine/core/audio/ClipSource.*`, `AudioImport.*`,
`engine/core/pool/SoundPool.*` (import settings only), `editor/window/InspectorAudioClip.cpp`,
`editor/mcp/tools/SoundTools.cpp`, `editor/resources/sound-library/`, `tools/sound-library/`,
`docs/mcp/guide/sound.md` (its Files, Placeholders and Library sections).

## 1. Goal

Agents and humans get sound content and its facts without a network: any audio file of a project
is measured (loudness, peaks, bands, onsets, silences, loop seam, pitch, a kind and plain-language
notes) and pictured; placeholder sounds are generated deterministically by seed; 40 curated CC0
clips are always available in the editor and 927 more ship with the editor install, each with its
SHA-256 and licence; every clip's import settings (gain, trims, click-safe fades, loop region,
type) and its licence and provenance live in its S04 `.meta`; the engine plays the import
settings; exports write the audio credits into `licenses/`; and `search_sounds` finds sounds by
words, type, character or similarity. None of it adds a byte to a player that does not use it.

## 2. Scope

**In scope**
- `AudioAnalysis`, `AudioImages`, `ClipDefects`, `AudioCompare`, `SoundTypes` (editor) and
  `analyze_audio` (files, built-in clips, Recorder WAVs, S35 captures).
- `get_asset_info` for every audio format.
- `.meta` sections `import.audioImporter` (version 1) and `provenance` (version 1), their editor
  (Inspector clip view), their MCP tool `set_audio_import_settings`, S25 labels for tags.
- The runtime of import settings: `ClipSource` (gain, trims, fades, loop region) in Play and in
  exports, through an exported table (`je-audio-imports.json`).
- `generate_sound` (sfxr compiled into the editor only, plus DSP recipes).
- The 40-clip essentials pack, embedded in the editor; Assets > Import Sound Essentials.
- The 927-clip CC0 starter library, committed as one squashed commit, staged with the editor
  install, never in players; its reader, its CLI, `library:` references.
- The maintainer pipeline `tools/sound-library/` writing S04 `.meta` sections (manifest v4).
- Export credits: `licenses/THIRD-PARTY-AUDIO.txt`.
- `search_sounds`, its feature index, an S25 Search palette provider.
- Later milestones folded in from sound-layer phase 7 (the analyses rate them worth it):
  `generate_sound` layered presets (P7-1), slicing a multi-take file into a Sound Event (P7-3,
  needs S37 M1), and "like this" fits against a reference of any licence (P7-2, user decision 6).

**Out of scope**
- Capture-based reading (S35), Sound Events and the Sound Browser (S37), loudness targets and the
  limiter (S38), tempo detection and the `music` keys' runtime (S39).
- Per-clip load type: upstream keeps Load Type per AudioSource (upstream A8); the importer has
  none.
- Import normalisation to a LUFS target (sound-layer `importNormalizeLufs`): not requested; an
  explicit `gain_db` covers it.
- A runtime download of sounds; any network access from the editor's sound code (decisions 4, 5).
- Phase 7's review page and capture frames (dropped, S35 §4.3).

## 3. Current state (sync/origin-main-4 45f4ba1f0, read)

- **Asset identity.** Audio files `.wav .ogg .mp3 .flac` are `AssetIdentity::Sidecar`
  (`editor/asset/AssetTypes.cpp:31`): each has a `<file>.meta` with a GUID and opaque `import:`
  and `labels:` sections that S04 keeps byte for byte (`editor/asset/AssetMeta.h:4-17`). S14 put
  texture settings at `import.textureImporter` (`version: 1`), written through `SetAssetMetaCmd`
  (S14 decision 1). No audio importer exists; upstream's mixer spec lists per-clip settings as a
  non-goal (`docs/superpowers/specs/2026-10-31-audio-mixer-design.md` §2).
- **Facts about audio files.** `get_asset_info` reads WAV headers only (`wavInfo`,
  `editor/mcp/tools/AssetTools.cpp:497`, used at `:965`); OGG/MP3/FLAC get nothing. No loudness,
  true peak or band code exists outside the vendored decoders.
- **Synthesis.** SoLoud's sfxr is vendored (`engine/libs/soloud/src/audiosource/sfxr/soloud_sfxr.cpp`)
  but not compiled (`engine/libs/soloud/CMakeLists.txt` lists `soloud_wav.cpp`,
  `soloud_wavstream.cpp`, no sfxr).
- **Content.** No built-in sounds. Embedding precedent: the pose templates
  (`CMakeLists.txt:403-420`). Editor install rules: `CMakeLists.txt:1696-1765` (notices go to
  `bin/`, macOS `Contents/Resources`).
- **Export.** `Exporter::copyAssets` copies every asset and skips `.meta` files
  (`editor/Exporter.cpp:2746`), so import settings in metas never reach a game unless exported in
  another form. `licenses/` holds the players' notices and the shader notices
  (`editor/ExporterPrebuilt.cpp:619-645` `stageNotices`; `Exporter.cpp:5887-5930`); no audio
  credits.
- **Playback.** `SoundPool` caches `Wav`/`WavStream` samples keyed `"<file>#<loadType>"`
  (`engine/core/pool/SoundPool.h:47-93`); every voice plays the whole file at its own level.

## 4. Reference: sound-layer (`snd/sound-layer` 20b461161; checkout `je-wt/snd-ro`)

### 4.1 Reused (adapted)

| What | Where (commit, file) | Change and why |
|---|---|---|
| Analysis | 0c0313ac9…d8785115e `editor/audio/AudioAnalysis.{h,cpp}` (2,924 lines), P1 §2.7 | Links to sound-layer's `AudioImportSettings`/`SoundLibrary` re-cut onto this spec's types; cache written through S03 `writeFileAtomic` (Cache durability); `logCategoryAudio()` (S09). |
| Images | 0c0313ac9…fad196a52 `AudioImages.{h,cpp}` (921), P1 §2.8 | As is (shared with S35's timeline image); colours move to `Theme`. |
| Defect notes | dfc97b89a…0ea965478 `ClipDefects.{h,cpp}` (517), P2.5 §2.2-2.4 | As is, notes only. |
| Differences in words | fad196a52…7cf986fd0 `AudioCompare.{h,cpp}` (192) | Into `analyze_audio` (`differences_vs_1`). |
| Sound types | `editor/audio/SoundTypes.h` (386), P1 §2.9 | As is: 31 types, kinds and loudness targets. |
| Synth | 583149e69…8eb6b7a7a `SoundSynth.{h,cpp}` (707), `WavWriter.h`, P1 §2.10 | Writes through `WriteFilesCmd`, whose created files get their `.meta` and GUID from S04's asset database (`WriteFilesCmd.cpp:103-105`); provenance in `.meta`, not `.import`. |
| Essentials | 69c34e90d…e25a50f56 `tools/sound-library/build_essentials.py`, 583149e69 `embed_essentials.py`, 583149e69…478566a5d `SoundEssentials.{h,cpp}`, `editor/resources/sound-library/essentials/**` (40 OGG, 0.44 MiB) | Manifest v1 → v2 with a per-clip `meta`; the import goes through `AssetOps`. |
| Library data | `editor/resources/sound-library/library/**` (928 OGG, `manifest.json` v3, `analysis.json`, 11 licence files) | One squashed commit, `fs_608433` dropped (927 clips), manifest v4. |
| Library reader, resolver, CLI | 1f2798e18…2c9f6a117 `SoundManifest.*`, `SoundLibrary.*`, `SoundLibraryCli.*`, 478566a5d…2c9f6a117 `BuiltinSoundImport.*` | Install staging re-done on target's rules; the import planner writes `.meta` through `AssetOps`. |
| Pipeline | 8973c0b82…9b1ad67b8 `tools/sound-library/build_library.py` (3,493), `soundlib.py`, `library.json`, `library-audit.jsonl`, `library-semantic.jsonl`, `semantic_stage.py` | Writes manifest v4 (`meta` per clip) instead of `.import` defaults; an exclusion list and denied terms. |
| Import settings and runtime | 583149e69…1f2798e18 `AudioImportSettings.*` (1,251), 9d032a80e…594f2bc87 `engine/core/audio/ClipSource.{h,cpp}` (767), `AudioImport.{h,cpp}`, P2 §2.x, P2.5 §2.5-2.8 | Stored in `.meta` `import.audioImporter`; load type dropped (upstream's per source); exported as a table, not generated C++. |
| Credits | c962b2651 `AudioCredits.{h,cpp}` (210), P5 §2.10 | Into `licenses/THIRD-PARTY-AUDIO.txt` on both export methods. |
| Search | 801e9fca5…b28012c84 `SoundSearch.{h,cpp}` (1,421), P3 §2.11.1 | Bus filters dropped; S25 Search palette provider added. |
| Waveform cache | 801e9fca5…0ea965478 `AudioWaveform.{h,cpp}` (476) | Used by the Inspector clip view and Project thumbnails (and S37, S40). |
| Oracles and tests | `tools/mcp/audio_refs.py`, `audio_defects_ref.py`; categories A and B of the editor analysis (`test_audio_analysis`, `_generate`, `_ogg`, `_defects*`, `_validate`, `_essentials`, `_import`, `test_sound_library_data`, `_pipeline`, `_semantic`, `test_audio_library_core`, `_library_mcp`, `test_audio_search`, `test_audio_credits`) | `.import` assertions become `.meta` assertions; capture sub-cases move to S35; fixtures (`audio`, `audio-defects`, `audio-ogg`, `audio-credits`) regenerated on target. |
| Phase 7 | P7 §2.2 (layered presets), §2.3 (fit), §2.4 (slicing) | Later milestones M8–M10. |

### 4.2 Changed on purpose

- **No `.import` sidecars.** A second sidecar per clip would fight S04 R26 (one `.meta` per asset)
  and every move/rename path; the data goes into `.meta` sections.
- **No per-clip load type**: upstream's Load Type per AudioSource stays (decision of the brief).
- **Exports read a table, not code.** S30's prebuilt players load data; generated
  `SoundPool::setImportSettings` lines (sound-layer master §8) cannot reach them.
- **No new core MCP parameters.** `import_files` already takes strings in `sources`; built-in ids
  are documented in `jasper://docs/sound` and returned by `search_sounds` (§6.1).

### 4.3 Dropped

`<file>.<ext>.import` sidecars and their move/rename code (`CopyFileCmd`/`DeleteFileCmd`/
`RenameFileCmd` changes, editor analysis C-4), `WriteAudioAssetCmd`, sound-layer's
`AudioPreview` "Editor" bus (upstream previews through the selected group), the per-clip load
type, `importNormalizeLufs`, the clip `fs_608433` ("Nintendo Coin Pickup").

## 5. Requirements

### Functional — analysis

- **R1. `AudioAnalysis`** (`editor/audio/AudioAnalysis.{h,cpp}`) decodes WAV (PCM 8/16/24/32,
  float), OGG Vorbis, MP3 and FLAC with SoLoud's decoders after BF14's `AudioFileCheck`, needs no
  device, and measures (sound-layer P1 §2.7.2, unchanged definitions): format (codec, encoding,
  channels, rate, frames, duration, bytes), loudness (integrated, momentary max, short-term max,
  LRA), sample and true peak, clipped samples, RMS, DC, 10 octave bands and band share, sub share,
  centroid, flatness, rolloff, onsets, `peak_ms`, `attack_ms`, head and tail silence (−60 dBFS),
  stereo correlation, loop seam (`seam_jump`, `seam_step_db`, `seam_level_db`), pitch and
  confidence, `measured_kind`, and the resolved `kind` (built-in manifest, then the importer's
  `type`, then measured). Loudness, peaks and bands come from S38's `AudioMeter`.
- **R2. Notes.** Plain-language `notes[]` `{id, text}` (P1 §2.7.6: silent, clipping, true_peak,
  dc_offset, head_silence, low_end, soft_attack, riser_early, loop_seam, loudness_target, phase)
  plus the P2.5 defect notes (`ClipDefects`: edge clicks, inner clicks, truncated tail,
  digital-silence gaps, narrow resonance, aliasing). Thresholds are `SoundTypes.h` constants.
- **R3. As it plays.** With import settings (R8), the analysis measures the clip as the engine
  plays it (gain, trims, fades applied; loop seam measured at the loop region's wrap) and reports
  the raw file under `source`. A note says which.
- **R4. Cache.** Key: the file's SHA-1 and `AudioAnalysis::VERSION` (plus the import settings'
  hash for R3); file `<project>/.jasperengine/cache/audio/<sha1>[-i<hash>]-v<N>.json`, written
  with `io::writeFileAtomic` (Cache durability); an in-memory `(path, size, mtime) → sha1` map; no
  eviction (entries are 2–20 KB).
- **R5. Images** (`AudioImages`): waveform, log spectrogram, loudness lanes and the numbered sheet
  of sound-layer P1 §2.8, PNG through `stbi_write_png_to_func` (S03 lint: memory form), exactly
  `max_width` wide, labels from an embedded public-domain 8×8 font; images always go out with the
  numbered JSON they show.
- **R6. `get_asset_info`** gives, for every audio format, `data.audio {codec, encoding, channels,
  sample_rate, frames, duration_seconds, bytes, analysis{duration_s, momentary_max,
  integrated_lufs, true_peak_dbtp, sample_peak_dbfs, peak_ms, head_ms, kind, notes[]},
  import?, provenance?}` (the existing `channels`, `sample_rate`, `duration_seconds` names stay);
  an undecodable file gives `data.audio_error`. Its description changes "WAV format/duration" to
  "audio format/duration" (+2 characters).

### Functional — import settings and provenance

- **R7. `import.audioImporter`** (S04 `.meta`, under S14's `import:` container, `version: 1`):

  ```yaml
  import:
    audioImporter:
      version: 1
      type: footstep          # SoundTypes.h vocabulary; omitted = none
      gain_db: -3             # -60..24, default 0
      trim_head_ms: auto      # number >= 0 or "auto" (to the first sample above -60 dBFS); default 0
      trim_tail_ms: 0         # number >= 0 or "auto"; default 0
      fade_in_ms: 2           # 0..1000, raised-cosine, default 0
      fade_out_ms: 20         # 0..1000, default 0
      loop_start_ms: 1250.5   # >= 0 or -1 (the trimmed start), default -1
      loop_end_ms: -1         # > loop_start or -1 (the trimmed end), default -1
      # music: {bpm, beats_per_bar, first_beat_ms}  (S39, additive within version 1)
  ```

  Keys are written only when not default, in this order; unknown keys are kept (a later version's
  keys survive a rewrite). Validation is one function (`AudioImportSettings::validate`), used by
  the Inspector, MCP and the export.
- **R8. `provenance`** (a new top-level `.meta` section, `version: 1`; S04's `AssetMeta::isKnown`
  lists it, opaque to S04, owned by this spec):

  ```yaml
  provenance:
    version: 1
    licence: CC0-1.0                  # SPDX id, or generated | unknown | proprietary
    licence_url: http://creativecommons.org/publicdomain/zero/1.0/
    author: Kenney (www.kenney.nl)
    attribution: Kenney (www.kenney.nl)
    source: kenney                    # kenney | freesound | generate_sound | user | <free text>
    url: https://kenney.nl/assets/impact-sounds
    builtin: {set: essentials, id: footstep/concrete_1, sha256: d7267e18…}   # imported built-ins
    generator: {preset: jump, seed: 1234, params: {}, loudness_lufs: -18, version: 1}   # generate_sound
    reference: {path: …, licence: …, author: …, url: …}   # fits (M10, user decision 6)
  ```

  Tags go into S25's `labels:` section (lowercase `[a-z0-9_]`, ≤ 32), not here.
- **R9. Writing.** Every write of either section goes through `SetAssetMetaCmd` (it rewrites only
  its own section, S14 decision 1), one undo step per user action or MCP call; `AssetOps` moves,
  copies and deletes the sections with their asset (no new code: they are `.meta` content).
- **R10. Runtime: `ClipSource`** (`engine/core/audio/ClipSource.{h,cpp}`, a `SoLoud::AudioSource`
  wrapping upstream's `Wav`, `WavStream` or `EngineWavStream` sample): plays the trimmed region
  at `gain_db`, applies the raised-cosine fades as it reads (the fade-in only before the first
  loop wrap, the fade-out only on a pass that ends the region: never at a wrap), and loops the loop
  region; a looping streamed clip keeps a spare decoder so a wrap never decodes inside the mix
  (sound-layer G-20, afc8445ad). A clip with default settings is not wrapped: its samples are
  played bit for bit as today. `SoundPool` keys a wrapped sample `"<file>#<loadType>#i<hash>"`.
- **R11. Editor Play** registers the project's non-default settings (`AudioImportTable`) at Play
  start and on every `.meta` change of an audio file (S05 reload, kind `audio`): playing voices
  keep their sample, the next start uses the new one.
- **R12. Exports.** When any shipped clip has non-default settings, the export writes
  `assets/je-audio-imports.json` (`jasperengine.audio-imports` v1: clip path → settings resolved
  to frames at the file's rate) and lists it in S30's `content-manifest.json`; the engine loads it
  at start (both export methods, every platform, from the pack too). A project without settings
  writes no file, so its build is byte-identical to today's.

### Functional — placeholders

- **R13. `SoundSynth`** (`editor/audio/SoundSynth.{h,cpp}`): the sfxr presets `coin`, `laser`,
  `explosion`, `powerup`, `hurt`, `jump`, `blip` (SoLoud `Sfxr::loadPreset(preset, seed)` with
  `SfxrParams` overrides) and the DSP recipes `click`, `pluck`, `bell`, `whoosh`, `riser`,
  `thump`, plus `random` (P1 §2.10: parameters, ranges and the xorshift32 seeding as specified
  there). Output is brought to its type's loudness target (or `loudness_lufs`, momentary max)
  without passing −1 dBTP, quantised to PCM16 without dither, and is byte-identical for the same
  preset, seed, params, target and editor version. Generated files have no click at their edges
  (P2.5 §2.9: a 2 ms fade-out where the last sample is not at zero).
- **R14. sfxr in the editor only.** `soloud_sfxr.cpp` is compiled into the editor target (its
  source list), never into `engine/libs/soloud` (players).
- **R15. `generate_sound`** writes `<path>.wav` (or `<stem>_1..n.wav` for `variations`, seeds
  `seed..seed+n−1`) and its `.meta` (GUID and meta from S04's asset database when `WriteFilesCmd` creates the file, then `provenance` with
  `licence: generated` and `generator{…}`, `audioImporter.type`) through one `WriteFilesCmd` and
  one `SetAssetMetaCmd` group: one undo step; refused while an export runs; allowed while playing.

### Functional — built-in sounds

- **R16. Essentials** (`editor/resources/sound-library/essentials/`): the 40 Kenney CC0 clips of
  sound-layer P1 §2.12.3 (7 packs, 423,628 bytes), copied unmodified from the pinned zips, with
  `manifest.json` (`jasperengine-sound-essentials` **v2**: v1's fields plus per clip `meta`, the
  `.meta` sections an import writes: `import.audioImporter` with the click-safe fades sound-layer
  measured (13 clips), `provenance`, `labels`) and `licenses/<pack>.txt` verbatim.
  `tools/sound-library/embed_essentials.py` (run by CMake with `DEPENDS` on every file, as the
  pose templates are embedded) generates `sound_essentials.h` and fails the build on any SHA-256
  or size mismatch.
- **R17. Starter library** (`editor/resources/sound-library/library/`): the CC0 clips of
  sound-layer phase 5 minus `pickup/fs_608433_handheld_coin` (user decision 2026-11-01): **927
  clips** (531 Freesound previews, 396 Kenney; 10 Kenney packs' licence texts plus
  `voiceover-pack.credits.txt`), `manifest.json` (`jasperengine-sound-library` **v4**: v3's fields
  plus per clip `meta` as R16), `analysis.json`, `.gitattributes` (`-text`, `*.ogg binary`).
  - It enters the repository as **one squashed commit** (about 22 MiB; the branch history of the
    folder is 50.5 MiB and is not merged), with the pipeline and its provenance records
    (`tools/sound-library/library.json`, `library-audit.jsonl`, `library-semantic.jsonl`, and the
    independent licence re-check of 2026-10-21, `je-snd/wip/x/snd-p5-review/.p5r/licences.json`,
    committed as `tools/sound-library/licence-recheck-20261021.json`).
  - The editor install stages it next to the binary (`bin/sound-library/`, macOS
    `Contents/Resources/sound-library/`); the build tree stages it beside the editor target;
    `FileUtils::getSoundLibraryDir()` finds it, `JASPERENGINE_SOUND_LIBRARY_DIR` overrides. A
    missing library is a state, not an error (`search_sounds` says how to get it).
  - **Never in players**: nothing under `editor/resources/` is a player input; an exported game
    holds only the clips a project imported, as ordinary assets with their `.meta` (which exports
    skip, R12).
- **R18. References.** `essentials:<id>`, `essentials:<group>/*`, `essentials:*`, `library:<id>`,
  `library:<type>/*` are accepted by `analyze_audio`, `search_sounds similar_to` and
  `import_files sources` (≤ 48 after expansion). An import copies the bytes (SHA-256 checked) to
  `<to_dir>/<id>.ogg` and writes the manifest's `meta` plus the minted GUID, all files of one call
  in one undo step through `AssetOps` (`BuiltinSoundImport` is the one planner). An identical file
  already there is skipped; a different one is refused unless `overwrite`.
- **R19. Assets > Import Sound Essentials** (`assets.importSoundEssentials`): imports all 40 into
  `<assets>/sounds/essentials/` in one undo step; existing identical files are skipped, different
  ones are left and listed in an alert.
- **R20. CLI** (`editor/cli/`): `jasperengine-editor audio-analyze <file>… [--json]` (what the
  pipeline's `judge` step runs), `sound-library verify [--dir D]` and `sound-library list
  [--type T] [--json]`. Exit codes 0 / 1 (verification failed) / 2 (usage).

### Functional — the pipeline (maintainers only)

- **R21.** `tools/sound-library/build_library.py` and `build_essentials.py` keep their commands
  (`fetch`, `prepare`, `judge`, `dedupe`, `write`, `verify`, `review`, `seed`; P5 §2.11) and their
  rules (HTTPS only, SHA-256 pins, 1 request/s, an honest User-Agent, robots.txt, downloaded data
  never executed, `--require-hashes` for the optional CLAP stage on `sndcompute`), and change:
  - `write` emits manifest v4 / essentials v2 with per-clip `meta` = exactly the S04 `.meta`
    sections an import writes; a new `migrate-manifest` command converts v3 → v4 offline and
    deterministically (how the squashed commit is produced);
  - `library.json` gains `excluded[] {id, reason}` (first entry: `pickup/fs_608433_handheld_coin`,
    "imitates a console start-up sound mark: trademark risk CC0 does not cover"); `verify` fails
    when an excluded id or a `denied_terms` word (franchise and brand names, starting with the
    ones the analysis found in source titles) appears in any id, path, tag or description;
  - `verify` stays standard-library only and needs no editor.

### Functional — credits and search

- **R22. Credits.** Every export (Prebuilt `stageNotices`, Compile `copyLicenses`) writes
  `licenses/THIRD-PARTY-AUDIO.txt` when any shipped audio file has a `provenance`, unless
  `audio.credits_file` is false: one section per licence, a line per clip (path, author, source
  URL, licence and its URL, attribution text), Kenney's packs grouped by pack. Then:
  - `licence: unknown` → warning `W-AUDIO-LICENCE` with the paths (Build Settings, CLI, MCP);
  - `licence: generated` → listed under "Placeholder sounds (generate_sound)" and warning
    `W-AUDIO-PLACEHOLDER` (earshot addition 16), so placeholders are not shipped by mistake;
  - a clip without `provenance` (the user's own) is not listed and warns nothing.
- **R23. `SoundSearch`** (`editor/audio/SoundSearch.{h,cpp}`): an index over the analysis cache,
  the importer `type`, S25 labels and the built-in manifests; `query` (all words must match name,
  path, type, labels, description), `type`, `scope` (`project`, `essentials`, `library`, `all`),
  `filters{min_ms, max_ms, brightness dark|neutral|bright, attack sharp|soft, tonal, loop, kind
  clip|event}`, `similar_to` (distance over duration, attack, centroid, flatness, pitch and bands),
  `limit`; files the cache lacks are analysed on the call (`analysed: n`). It never uses the
  network. `kind event` lists S37's Sound Events when present.
- **R24. Search palette.** An S25 Search provider "Sounds" lists project clips and built-in clips
  by name, type and labels; Enter selects (project) or imports (built-in, R18).

### Editor UX (human)

- **R25. Inspector clip view** (`editor/window/InspectorAudioClip.cpp`, `drawAudioClipAsset`,
  beside `drawAudioMixerAsset`, `InspectorWindow.h:420`): waveform/spectrogram toggle with onset
  and peak markers, the measurements table, notes in `Theme::Colors::WarningText`, Import Settings
  (type combo, gain, trim, fade and loop fields plus draggable handles on the waveform, Apply and
  Revert as S14's importer view), Provenance (editable fields; built-in fields read-only with
  "Edit"), labels, and Audition (plays as the engine plays it, optionally loudness-matched, through
  an editor preview voice; disabled while S35 captures).
- **R26. Project window** waveform thumbnails for audio files (`AudioWaveform` cache on a
  Background job).
- **R27. Menus** (S25 CommandCatalog): `assets.importSoundEssentials` (Assets > Import Sound
  Essentials, also in the Project window's context menu), `assets.createPlaceholderSound` (Assets >
  Create > Placeholder Sound…: preset, seed, variations, target; then R15), `assets.reanalyzeAudio`
  (context menu: drop the cache entries of the selection).

### Agent UX (MCP, Lua, CLI)

- **R28. Tools** (group `audio`, §6.1): `analyze_audio`, `generate_sound`, `search_sounds`,
  `set_audio_import_settings`. All results have a text summary and `structuredContent`; images go
  out as MCP images; calls longer than 1 s are `McpJobs` jobs.
  - `analyze_audio` (Read, idempotent, allowed while playing): `paths` (1–24; built-in refs),
    or `capture` (`"last"`, `"current"`, an id or a session path: needs S35), `start`, `end`,
    `group` (capture: `master` or a group with stems), `image` (`none`, `waveform`,
    `spectrogram`, `both`, `sheet`), `detail` (`summary`, `full`), `max_width`, `offset` (pages of
    48). Two or more items add `differences_vs_1` in words (`AudioCompare`). A Recorder
    `.audio.wav` is an ordinary file path.
  - `generate_sound` (Edit): `path`, `preset`, `seed`, `variations` (1–8), `params`,
    `loudness_lufs` (−40…−6), `image`, `overwrite`.
  - `search_sounds` (Read): R23's inputs plus `sheet`; results `{n, path or id, type, kind,
    duration_ms, lufs, licence, labels, heard_tags (library: a model's guess, said so),
    in_project, import: "import_files sources:[<id>]"}`.
  - `set_audio_import_settings` (Edit, idempotent): `paths` (1–64), `settings` (R7 keys; null
    resets one), `provenance` (R8 keys), `labels` (S25 labels to set); one undo step; returns the
    resolved settings and a fresh analysis summary per path.
- **R29. No Lua API.** Import settings are applied by the engine; nothing new is scriptable.
- **R30. Docs.** `docs/mcp/guide/sound.md` sections "Measure a file", "Placeholders", "Built-in
  sounds and licences", "Import settings"; the Sound line of the instructions (S35 §6.1);
  `docs/unity-glossary.md` rows (AudioImporter → `import.audioImporter`, AudioClip import settings).

### Performance budgets (server F, Release)

- **R31.** `analyze_audio` of a 1 s clip ≤ 40 ms uncached, ≤ 2 ms cached; of a 3-minute OGG
  ≤ 1.2 s; `search_sounds` over the whole library with a warm cache ≤ 80 ms; `ClipSource` adds
  ≤ 3 % to the mix cost of a voice (bench against a plain `Wav` voice).
- **R32.** Editor start: the essentials and the library manifest load lazily (first use); the
  editor's start time is unchanged within noise.

### Data & compatibility

- **R33.** Old projects: no `.meta` changes until a user edits a setting; audio plays bit for bit
  as before (R10); a clip with a sound-layer `.import` sidecar is not read (that branch never
  merged; there are none on target).
- **R34.** Repository: the squashed commit's size is recorded in Progress; the editor install
  grows by about 20.9 MiB (the library) and the editor binary by 0.44 MiB (the essentials).

## 6. Design constraints & decisions

- **D1. Sections, not sidecars.** `import.audioImporter` follows S14's container rule;
  `provenance` is a top-level section because a licence is not an import setting and other asset
  types (fonts, textures) can use it later.
- **D2. Built-in clips have no GUID.** A GUID belongs to a project asset; `AssetOps` mints it at
  import, so two imports of one clip into one project never collide.
- **D3. Import settings are data in exports** (R12), loaded by the engine at start, so prebuilt
  players need nothing generated.
- **D4. The engine plays the settings, the analysis hears them** (R3), so an agent's numbers match
  what a player outputs.
- **D5. Plain git for the library**, one squashed commit, as decided; no LFS (the editor repository
  uses none) and no download.
- **D6. Editor-only code.** Analysis, images, synth, search, library, credits and the Inspector
  view are editor code; the engine gains only `ClipSource`, `AudioImport` and the table loader.

API sketch:

```cpp
namespace jasperengine {
  struct ClipImportSettings { float gainDb = 0; uint32_t trimHeadFrames = 0, trimTailFrames = 0;
      uint32_t fadeInFrames = 0, fadeOutFrames = 0; int64_t loopStartFrame = -1, loopEndFrame = -1;
      float bpm = 0; int beatsPerBar = 4; int64_t firstBeatFrame = 0;   // S39 fields
      bool isDefault() const; uint64_t hash() const; };
  class AudioImportTable { public: static bool load(const std::string& assetPath);   // je-audio-imports.json
      static void set(const std::string& clip, const ClipImportSettings&); static void clear();
      static const ClipImportSettings* find(const std::string& clip); };
}
namespace jasperengine::editor {
  struct AudioAnalysisItem;   // the analyze_audio item (sound-layer P1 §2.7.3 shape)
  class AudioAnalysis { public: static constexpr int VERSION = 1;
      static bool analyse(const AudioSource& in, const AnalysisOptions&, AudioAnalysisItem& out, std::string* error); };
}
```

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Estimated listing chars | Note |
|---|---|---|---|
| `analyze_audio` | `audio` | ≤ 1,400 | sound-layer's ~1,700; measurement list moved to the guide |
| `generate_sound` | `audio` | ≤ 1,450 | preset enum (14 names) stays; params prose to the guide |
| `search_sounds` | `audio` | ≤ 1,450 | the 31-type enum stays (closed set, S31 R20) |
| `set_audio_import_settings` | `audio` | ≤ 1,100 | `settings`/`provenance` as `anyObject` with the keys in the guide |
| `get_asset_info` | core | +2 | description word only; richer result |
| `import_files` | core | 0 | built-in ids pass through `sources` as strings |

Group `audio` totals with S35/S37: 7 tools, ≈ 9,200 of 15,000 characters (S35 §6.1). Core: +2.

### 6.2 File formats

| File | Format id, version | Codec |
|---|---|---|
| `.meta` `import.audioImporter` | section `version: 1` | YAML through `AssetMeta` sections and `SetAssetMetaCmd`; `AudioImportSettings` parse/serialize (keys in R7 order) |
| `.meta` `provenance` | section `version: 1` | as above (`AudioProvenance`) |
| `assets/je-audio-imports.json` (exports) | `jasperengine.audio-imports` v1 | `nlohmann::json` (engine), written by the exporter |
| `.jasperengine/cache/audio/*.json` | `analyzer_version` N | `nlohmann::json`, regenerable |
| essentials `manifest.json` | `jasperengine-sound-essentials` v2 | JSON (indent 1, sorted), `SoundManifest` |
| library `manifest.json`, `analysis.json` | `jasperengine-sound-library` v4 | JSON, `SoundManifest` |
| `licenses/THIRD-PARTY-AUDIO.txt` (exports) | plain text | `AudioCredits` |
| generated `.wav` | RIFF PCM16, `fmt ` + `data` only | `WavWriter.h` |

No S08 component codec changes (no component fields in S36).

### 6.3 Settings (S25)

| Key | Scope | Type | Default |
|---|---|---|---|
| `audio.credits_file` | Project (Audio) | bool: write `licenses/THIRD-PARTY-AUDIO.txt` | true |

The library location is the install's (R17) or `JASPERENGINE_SOUND_LIBRARY_DIR`; no setting.

### 6.4 Editor commands (S25 CommandCatalog)

`assets.importSoundEssentials`, `assets.createPlaceholderSound`, `assets.reanalyzeAudio` (group
Assets, menu and Project context menu; no default keys).

### 6.5 Lua API

None (R29).

### 6.6 Player size (S34)

- `ClipSource`, `AudioImport` and the table loader are the only engine code (estimated 18–25 KB
  Linux, 10–14 KB wasm gzip). They are needed only when a project ships import settings: they
  belong to the optional engine module `soundlayer` that S37 defines (S37 §6.6), detected by "the
  export writes `je-audio-imports.json`". The module is in every prebuilt variant (S37 Q1,
  resolved 2026-11-01); a Compile build without import settings leaves it out: zero growth.
- sfxr, analysis, images, search, the essentials (0.44 MiB) and the library (20.9 MiB): editor
  only, zero in players.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R5) | `tools/mcp/tests/test_audio_analysis.py` with `audio_refs.py` signals: EBU Tech 3341 stereo 1 kHz at −23/−33 dBFS → −23.0/−33.0 ± 0.1 LUFS; fs/4 sine at 45° → true peak ≈ sample peak + 3 dB ± 0.2; pyloudnorm agrees within 0.1 LU on the `audio` fixture; a 40 Hz sine lands in 31.5/63 Hz; a click train's onsets within ±1 ms; images decode at the requested width and are not blank. |
| AC2 (R1, R6) | `test_audio_ogg.py`: OGG, MP3, FLAC and WAV fixtures give the same measurements within 0.2 dB as their WAV decodes; `get_asset_info` returns `data.audio` for each, `audio_error` for a truncated file (no crash under ASan, thanks to BF14's `AudioFileCheck`). |
| AC3 (R2) | `test_audio_defects.py` with `audio_defects_ref.py`: each seeded defect yields its note, its clean twin none. |
| AC4 (R7–R11) | `editor/tests/AudioImportSettingsTest.cpp`: parse/serialize round trip in key order, unknown keys kept, validation errors. `test_audio_import.py`: `set_audio_import_settings` writes only `import.audioImporter`, other `.meta` sections byte-identical; undo restores; a move keeps it. `engine/tests/core/audio/ClipSourceTest.cpp`: default settings play bit-identical to `Wav`; trims, gain, fades (frame 0 of a fade-in is exactly 0), loop region wraps sample-exact (a looping 1 kHz sine region has no seam step above its own), streamed and decoded agree. |
| AC5 (R12) | `test_build.py::test_audio_import_table` (prebuilt Linux player, `JASPER_SLOW_BUILD=1`): the table is in the build and the content manifest; the player plays the trimmed clip (an S35 player capture shows the trimmed length to the sample); a project without settings builds byte-identical to the base. |
| AC6 (R13–R15) | `test_audio_generate.py`: every preset non-silent, within ±1 LU of its target, ≤ −1 dBTP, byte-identical for the same seed, different for another; variations write `_1.._n`; one undo removes files and metas; the `.meta` has `provenance.licence: generated`; `nm` of a release player shows no `Sfxr` symbol. |
| AC7 (R16, R19) | `test_audio_essentials.py`: the 40 clips' SHA-256 equal the manifest's; `analyze_audio essentials:*` measurements match the manifest's `measured` (independent decoder) within tolerance; Import Sound Essentials imports 40 with GUIDs and `provenance`, skips identical files; a corrupted embedded byte fails the build (`embed_essentials.py` unit test). |
| AC8 (R17, R21) | `tools/mcp/tests/test_sound_library_data.py` (the licence and SHA-256 gate): 927 clips, every SHA-256 matches, no unlisted OGG, every Freesound clip has author, URL, licence URL and an audit line, every Kenney clip a pinned zip and member hash; `fs_608433` absent everywhere; no denied term in ids, tags or descriptions; `build_library.py verify` exits 0 with the standard library only; `migrate-manifest` of the v3 manifest reproduces the committed v4 byte for byte. |
| AC9 (R17, R18, R20) | `test_audio_library_core.py`: the install staging (`tools/build.sh linux` install tree has `bin/sound-library/manifest.json`), `JASPERENGINE_SOUND_LIBRARY_DIR` override, `sound-library verify` exit codes, `library:<id>` import with `meta` and GUID, `library:<type>/*` expansion cap 48. |
| AC10 (R22) | `test_audio_credits.py`: a project with a CC0 essential, a CC-BY clip, a generated placeholder and an `unknown` clip exports (Prebuilt and Compile) a `licenses/THIRD-PARTY-AUDIO.txt` with the right sections, `W-AUDIO-LICENCE` and `W-AUDIO-PLACEHOLDER` warnings in the build result; `audio.credits_file false` writes none. |
| AC11 (R23, R24) | `test_audio_search.py`: query words, type, filters, `similar_to` ranking (a clip's nearest neighbour is its own variation), `scope`, no network (the harness's network guard); the Search palette lists a library clip and Enter imports it. |
| AC12 (R25–R27) | `test_audio_clip_inspector.py` (xvfb, isolated editor): the clip view shows the waveform, notes and settings; dragging the trim handle and Apply writes one undo step; thumbnails appear; screenshots in the evidence. |
| AC13 (R31, R32) | `tools/perf` bench for R31 on server F (before/after table here); editor start time within 2 % of the base. |
| AC14 (§6.1, §6.6) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, the AC10 group assertion for the four tools; S34 size tools: a Compile build without import settings is unchanged; prebuilt players carry the measured `soundlayer` size. |

## 8. Dependencies & conflicts

- **Depends on.** S38 M1 (`AudioMeter`); BF14 (S-1–S-4: OGG streaming abort, stream seek,
  loader bounds, `AudioFileCheck`); S03, S04 (`AssetMeta` sections, `AssetOps`,
  `AssetFileWrite`), S05 (reload of `.meta` changes), S06 (Background jobs), S09, S14 (the
  `import:` container), S24, S25 (registry, catalog, labels, Search providers), S30
  (`stageNotices`, content manifest), S31, S34.
- **Blocks.** S35 M5, S37 M6, S39 M2/M4, S40 M3.
- **Coordination.** S04's `AssetMeta::isKnown` gains `provenance` (one line). S37 defines the
  `soundlayer` module that holds `ClipSource`.
- **Merge hotspots.** `editor/mcp/tools/AssetTools.cpp` (`get_asset_info`, `import_files`),
  `Exporter.cpp`/`ExporterPrebuilt.cpp` (credits, table), root `CMakeLists.txt` (embedding,
  install, sfxr), `SoundPool.*` (BF14), `InspectorWindow.*`, `ProjectWindow.cpp` (thumbnails),
  `DefaultCommands.cpp`.

## 9. Risks & defaults

- **Freesound licences are uploaders' assertions.** Default: the audit and the re-check are
  committed with the data; `verify` gates them; a licence change upstream is handled by excluding
  the clip in the next library version.
- **Repository growth (~22 MiB).** Decided by the user; one commit, no history.
- **CLAP "heard" tags are a model's guess.** `search_sounds` and the Inspector label them so.
- **Two decoders disagree slightly on OGG.** Default: the essentials' `measured` values come from
  libsndfile, and AC7 states its tolerance (0.3 dB, 1 ms).
- **A user edits an essential clip in place.** It is a project file like any other; `provenance`
  keeps the source; the credits still list it.

**Resolved questions**
- Release-player size: `ClipSource` is in the `soundlayer` module, which is in every prebuilt
  variant; Compile builds strip it when unused (S37 §9 Q1, resolved 2026-11-01).

## Milestones

- **M1** `AudioAnalysis`, `AudioImages`, `ClipDefects`, `AudioCompare`, `SoundTypes`,
  `analyze_audio` (files and built-in refs; captures when S35 M4 is in), `get_asset_info` (R1–R6,
  R28 part); AC1–AC3. Needs S38 M1.
- **M2** Import settings and provenance sections, `set_audio_import_settings`, Inspector clip view,
  thumbnails (R7–R9, R25, R26); AC4 (editor half), AC12.
- **M3** `ClipSource`, Play registration, export table (R10–R12); AC4 (engine half), AC5.
- **M4** `SoundSynth`, `generate_sound`, essentials, Import Sound Essentials (R13–R16, R19); AC6,
  AC7.
- **M5** Starter library squashed commit, reader, staging, CLI, pipeline v4 (R17, R18, R20, R21);
  AC8, AC9.
- **M6** Credits (R22); AC10.
- **M7** `search_sounds`, Search palette (R23, R24); AC11. Then budgets, docs, size; AC13, AC14.
- **M8** (later, sound-layer P7-1) `generate_sound` layered presets in families (`ui_click`,
  `swoosh`, `impact`, `pop`: layers, a chain, what a seed varies, a variety report; P7 §2.2), within
  `generate_sound`'s 1,450 characters.
- **M9** (later, P7-3; needs S37 M1) slice a multi-take file into a shuffle Sound Event: onset
  detection, one clip file per take with its `.meta`, the event written by S37's writer; an
  `edit_sound_event` `slice` input (S37 §6.1 reserves ≤ 150 characters for it).
- **M10** (later, P7-2, user decision 6) "Make a placeholder like this": fit a preset to a reference
  clip of any licence (64 bounded renders, no model, no audio copied), the reference recorded in
  `provenance.reference`.

## Progress

- The squashed library commit is 21.5 MiB of files (`du -sb editor/resources/sound-library/library`: 22,538,857 bytes): 927 clips, manifest v4, analysis v1, 11 licence files (R34).

AC13, measured with `time` on the same files (`je-evidence/s36/mac-bench-before.log`, `it17.measure.log`, `it18.*.log`). The
63 ms and 6.3 s that started this came from a loaded H and say nothing either way. Server F no longer exists: A is a quiet
16-vCPU VPS (load 1), the Mac an idle M-series machine.

| R31, R32 | Budget | Mac, CPU | A, CPU | A, wall |
|---|---|---|---|---|
| `analyze_audio` 1 s clip, uncached, over startup | 40 ms | 10 ms | 30 ms | 30 ms |
| 3-minute stereo OGG, before | 1.2 s | 0.95 s | 7.0 s | 7.0 s |
| the same, the meter beside the spectral stages | 1.2 s | | 7.1 s | 5.8 s (load 9) |
| of which `AudioMeter` on `main` | | 0.51 s | 4.8 s | |
| of which `AudioMeter` on S38's branch (70e471bf2) | | 0.24 s | 1.3 s | |
| `AudioClipSource` over a `Wav` voice, the mix, Release, gain and trim | 3 % | | +0.2 % | |
| the same, trim only | 3 % | | +0.6 % | |
| Editor start, new project to the first MCP reply, 15 alternating pairs, against `main` | 2 % | | +1.8 % | +0.9 % |

The 1 s clip is inside its budget. The 3-minute file is real on A: the meter's 44 biquads a sample were 4.8 s of its 7.0, and S38's
branch replaces them. Inferred, not measured together: with S38 merged A takes about 2.1 s of wall time, decode 0.45 s then the
longer of the meter and the spectral stages, and the Mac well under 1.2 s. So A stays over R31 until the decode and the meter
get faster; both are below this spec.

The clip voice was +7.5 % at the unit tests' default -O2, where GCC leaves the gain loop scalar. Players ship at -O3, which is
what the bench now asks for.

`soundlayer` on Linux: the Release engine sample is 159,776 bytes larger with the module than without (40,020,944 against
39,861,168, with S37 M1-M7 in it), and `lib_size.py --modules` names 34,295 bytes of S36's own. A build with the option off
compiles under WERROR (`it17.off.log`).

## Deviations

- **The class is `AudioClipSource` (R10)**, in `engine/core/audio/ClipSource.{h,cpp}`: `jasperengine::ClipSource` is already an
  enum of the animation clips (`animation/AnimationClip.h`). S37, S39 and S40 read "ClipSource" as this class.
- **`AssetMeta::isKnown` is unchanged (R8).** The spec asked it to list `provenance`, but that list holds the sections
  `AssetMeta` writes itself, and `setSection` refuses them. Being unknown is what keeps `provenance` byte for byte.
- **Files and their `.meta` go through `WriteFilesCmd` (R15, R18).** `generate_sound` and built-in imports write the
  clip and a complete `.meta` (minted GUID, or the existing one and its other sections) as items of one command: one
  undo step that needs no second command, and no moment with a clip that has a GUID but no sections. `AssetOps` has no
  create operation. `set_audio_import_settings` and the Inspector use `SetAssetMetaCmd` as R9 says.
- **R4 cache of a clip as it plays.** The file `<sha1>-i<hash>-v<N>.json` holds the whole played analysis (R3), so
  sound-layer's per-file list of four played regions is gone, and with it its eviction.
- **`AudioImportTable::find` returns a copy (`std::optional`), not a pointer.** The pool's load jobs read the table while
  the main thread sets it.
- **The table loads on the first `SoundPool::loadClip` (R12),** once per process, not at engine start: that is the one
  place every platform and export method passes before a clip plays, and a game with no table pays one failed open.
- **`analyze_audio` and `search_sounds` wait 60 s, then hand over the job (R28)**, through `replyWhenJobDone`, with no
  `wait` parameter: the budget has no room for it, and images only travel in the reply. A job an agent polls has the items.
- **A `kind event` filter finds nothing until S37 M6 lands** (its branch lists events in `SoundSearch`), and `analyze_audio capture` reads sessions from disk with no
  running capture known until S35 lands (tested on session directories written by the test).
- **`test_audio_ogg` cuts ffmpeg's decode to the last page's granule.** The Vorbis spec makes the last page's granule the
  stream's length, and an encoder ends a clip that way. ffmpeg 4.4.2 (the image's) does not trim: it hands over whole
  decoded packets. Verified on 61 of the 967 OGGs, sampled evenly (`je-evidence/s36/ogg-overrun.log`): libsndfile, which
  decodes through libvorbisfile, returned exactly the granule on 61 of 61, while ffmpeg returned 41 to 978 frames more on
  61 of 61 (221,056 against 220,500 on `spaceEngineLow_000.ogg`). ffmpeg 6.1.1 on this host instead returns 128 frames fewer on
  `wood.ogg` and `error.ogg`, so ffmpeg's end depends on its version. Where both decode, every channel matches libsndfile
  within 3e-7. The editor loads each at the granule, as libsndfile does, so the difference is ffmpeg's and not ours.
- **This branch merges `feat/s37-sound-events` (M9).** `edit_sound_event` and S37's writer are S37 M1 and M5, which were on
  its branch and not on `main`, so the branch holds S37 M1-M5 and whichever of the two lands second has nothing to merge
  but its own commits.
- **`soundlayer` is wired in both streams.** S37 §6.6 defines the module; S36 added the option, the module name, the variants and
  `engine/core/audio/SoundLayerSources.cmake`, S37 M7 its own detections and sources. The merge keeps one of each.
- **`like` refuses `params`,** which the search owns: a fit's result can be adjusted afterwards with `params` and its seed.
  `compare` is the default image of one fitted file and not a value of `image`, which keeps the schema inside its budget.
- **`SoundFit` measures a file's head and tail at -60 dB re its peak,** not dBFS: phase 7 §2.3.2 calls every measure
  level-independent, which the raw render and the written file need.
- **`force` applies to `noise` layers, `brightness` to `sweep` too:** phase 7 §2.2.3 names noise layers for force, and §2.2.4
  every filter for brightness, and a sweep's band-pass is a filter.
- **The variety report has its own near-duplicate limit, `VARIETY_NEAR_DUPLICATE_DISTANCE` 0.020** (phase 7 §2.2.5 uses A4's
  `NEAR_DUPLICATE_DISTANCE`, and its example says "none under 0.020"). A4's is 0.075 here, from an event's distinct clips.
  At 0.075, seeds 1 to 8 called 15 of the 16 families near-duplicates (`test_audio_design`, red run `je-evidence/s36/it11.mcp.log`),
  whose advice is empty: a preset's seeds are close by design.
- **A new game's AGENTS.md names the sound tools and their group.** `generate_sound` is in group `audio`, and with only core
  listed agents wrote a WAV by script or gave up: `guard_placeholder_coin_finds_the_group` 0/2 without the line, 4/4 with it
  (`je-evidence/eval/runs/s36-coin-without2`, `s36-coin-line2`, `s36-coin-line3`; one more sample only hit an account's limit). A
  Sound section in the guide was tried first and cut: it left the placeholder, layered, fit, search and "too quiet" prompts at
  0 of 10. The other six sound cases set `groups_enabled` and measure the tools' own text. A project written before this line
  has none until its AGENTS.md is regenerated.
- **`edit_sound_event`'s `slice` is an open object in the schema, its keys checked in the handler.** A closed one costs about
  300 characters, and S37 §6.1 reserves 150.
- **`preview` reports `source.licensed`, not the licence's fields.** The event's reply says what is unknown in `problems`.
- **`test_d3_licence_files_match_the_pinned_zips` and the tests that need the zips skip, accepted.** kenney.nl now answers the pinned
  URLs with its asset pages, which hold no zip link (`it17.zips.log`), so no pinned zip can be fetched. The committed licence
  files are byte for byte the reference's (`diff -r`). They skip by name with this reason. The real-model semantic tests also need
  a 776 MB model.
- **R31's 3-minute OGG is open until S38 lands.** A is not the reference. Measure again by CPU time on H or the Mac after S38; the
  Mac's 0.95 s, before S38, is the number to beat.
- **Importing mixed sources.** `import_files` refuses built-in references mixed with other sources: a call has one plan
  and one undo step.
- **The pipeline's pins run on Python 3.12, fetched by uv.** `numpy==2.5.4` needs Python 3.12 and the build image has 3.10, so
  the tests' `uv run` and the scripts' documented commands pass `--python 3.12`. Older pins that install on 3.10 move values
  by up to 0.01, so the committed data would no longer be reproduced byte for byte.
