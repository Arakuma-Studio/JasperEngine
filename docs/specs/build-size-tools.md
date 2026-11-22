# Build Size Tools: report history and comparison, why each file ships, size budgets, and Force To Mono

Shipping follow-up. Builds on S53 R21 (the Build Report, landed), S04 (the typed dependency index,
landed), S36 (audio import settings, landed) and S30 (prebuilt players, landed). Paths are relative to
the repository root. Every part is genre-agnostic: any game that ships to phones or the web pays for
its bytes.

## 1. Goal

An export writes a Build Report with category totals and the 25 largest files, but a developer cannot
tell what grew since the last build, why a file ships at all, or stop a build that went over its size
target; and a stereo clip played as a positional sound costs twice the bytes and, in our mixer, loses
one channel when panned. This spec keeps every export's report in a project history, compares any two
reports per category and per file, names the chain of references that put each file in the build (and
the files nothing references), checks per-platform size budgets that warn or fail the build, and adds
Force To Mono (Off, On, Auto) to audio import settings, with Auto deciding from a measured stereo width.
Humans get it in the Build Report window, the Inspector and Project Settings; agents get it through
`get_build_info` and `set_audio_import_settings`.

## 2. Scope

**In scope**
- Report content: every measured file (not only the 25 largest), what the export left out with its
  reason and bytes, the reference reachability of each shipped asset, budget results. Same file,
  same version, optional keys.
- Report history per platform in the project; comparison of two reports; Referenced By chains; the
  Unreferenced list.
- Size budgets in Project Settings > Player (warn or fail), checked at export.
- Force To Mono (Off, On, Auto): a runtime downmix for every format, a mono WAV cook at export for
  PCM WAV sources, a side/mid stereo-width measure in the audio analysis, `AudioSource.channels` in Lua.
- Build Report window (Compare With…, tabs, Referenced By pane), audio clip Inspector row, Project
  Settings rows, export completion page line; `get_build_info`, `set_audio_import_settings` and
  `analyze_audio` additions; docs.

**Out of scope (decisions)**
- **Per-platform audio Compression Format, Quality and Sample Rate** (Unity's
  `AudioImporterSampleSettings`). The engine vendors no lossy encoder: libopus is decoder-only
  (`engine/libs/opus/README.jasper.md`: "Not vendored: the encoder") and SoLoud carries decoders only
  (`engine/libs/soloud/src/audiosource/wav/`: dr_wav, dr_mp3, dr_flac, stb_vorbis). Adding an encoder is
  a heavy new dependency on every editor platform; a later spec may. A sample-rate override (a WAV-only
  saving that needs an offline resampler) is a follow-up, not decided here.
- **Leaving unreferenced files out automatically.** The report lists them; nothing is excluded, because
  the index cannot see a path a script builds at run time (D4). `build-profiles.md` may add exclusion
  rules.
- **Store and transfer sizes** (APK/AAB zip compression, the web server's gzip or brotli). Report bytes
  stay S53's: the files on disk before packing.
- **Visual before/after contact sheets** of changed textures (Craftsman's `eemodels_sheet.py`) and
  **atlas-fit simulation** (S45 already packs atlases and reports its pages).
- Engine library size by module (S34 and `tools/players/lib_size.py` own it).

## 3. Current state

All read from the code at the worktree's base commit (not run):

- **Report model.** `BuildReport` (`editor/cook/BuildReport.h:25-71`): `BuildReportFile {path,
  category, bytes, source, sourceBytes}` (`:25-31`), `platform`, `method`, `totalBytes`, `categories`,
  `largest` (`:38-42`), `kLargest = 25` (`:36`). `addFile` sorts and truncates to 25 on every add, so
  the full list is never kept (`BuildReport.cpp:72-81`). Categories are models, textures, audio,
  scripts, scenes, engine, other (`:42-45`), by path and extension (`:47-70`).
- **Report file.** `toJson` writes `{"version": 1, platform, method, total_bytes, categories, largest}`
  (`:132-146`); `fromJson` refuses any other version and reads known keys only (`:148-170`); `write`
  goes through `io::writeFileAtomic` (`:172-181`). The last report's path is a process static
  (`setLast`/`last`, `:219-227`): it is lost when the editor restarts.
- **When it is written.** `Exporter::measureBuildContent` measures `assets/`, `lua/`, `scenes/`,
  `prefabs/` after `copyAssets` and the audio import table (`editor/Exporter.cpp:1286-1290`,
  `:3513-3520`), the engine files after the compile (`:3522-3524`); `writeBuildReport` writes
  `<output>/build-report.json`, copies totals into the export result and calls `setLast`
  (`:3526-3543`). Call sites: compile (`:1347`), source export (`:1358`), prebuilt
  (`editor/ExporterPrebuilt.cpp:346`).
- **Cooked names change with content.** A cooked model ships as `<sha256 of its bytes>.glb`
  (`editor/model/ModelExport.cpp:106`); cooked textures are read from the cooks' `index.json`
  (`BuildReport.cpp:106-130`). A build path is therefore not a stable key between two builds; the
  `source` asset is.
- **What ships.** `copyAssets` copies every file under the assets root (`Exporter.cpp:3663-3760`)
  except top-level hidden folders, `build`, `Recordings`, `EditorSettings`
  (`editor/Project.cpp:5989-5991`) and sources it replaces: `.aseprite` files (`:3693`), Title Art not
  needed (`:3712`), dropped languages' files (`:3714`), Procedural Materials (`:3716`), atlas-only
  textures (`:3719`), cooked texture sources (`:3723`), bitmap font sources (`:3727`), cooked model
  sources (`:3731`). None of these is reported, with or without bytes. `copyLua` ships the whole Lua
  tree (`:4030-4060`). Scenes ship cooked as `scenes/<id>.jscn`, prefabs as `prefabs/<id>.jpfb` with
  their GUID (`editor/cook/SceneCooker.cpp:320,334`).
- **Window.** `BuildReportWindow` (`editor/window/BuildReportWindow.cpp:44-136`): a path field and Open,
  category bars, the 25-row table (File, Category, Size, Source, Source Size), a click on Source
  inspects the asset (`:125-129`); with no export in the session it says so (`:72-75`). No history, no
  comparison, no referrers. The export completion page shows the totals and an Open button
  (`editor/window/dialog/ExportWindow.cpp:1749-1761`).
- **Agents.** `get_build_info` (`editor/mcp/tools/BuildTools.cpp:1287-1444`, registered `:2337-2352`,
  `core` group) returns toolchains, scenes, shaders and `last_build`, nothing of the report.
  `build_project`'s result has no report fields either; only the CLI's `build --json` prints
  `build_report`, `build_bytes`, `build_categories` (`editor/cli/ShippingCommands.cpp:314-319`).
- **Dependency index.** `AssetIndex` (`editor/asset/AssetIndex.h:1-9`, `:59-71`): `referencesTo`,
  `dependenciesOf`, `forEachEdge`, `projectReferrer()` (project.yaml, whose edges include the scene
  list and standalone prefabs: `DepEdge.h:29,32`). Edge kinds (`editor/asset/DepEdge.h:21-41`) cover
  scene and prefab component fields, materials, script properties, HTML/CSS, shader includes, glTF URIs
  and Lua `require` only (`:38`): a path string handed to `AudioSource:loadSound` or `PrefabAsset.load`
  is not an edge (inferred from the kinds; no scanner for it was found). The index builds on a
  background thread and answers `isReady()` false until then (`AssetIndex.h:6-9`). `find_references`
  and `get_asset_dependencies` expose it (`editor/mcp/tools/AssetIdentityTools.cpp:158-210`,
  `assets_pipeline` group).
- **Budgets.** No size budget exists anywhere in the editor or engine (grep for `size_budget`,
  `sizeBudget`: none). Project Settings rows live in `editor/settings/SettingsRows.cpp` with their
  window placement table (`:4242-4300`, Player > Publishing Settings holds `player.build_method`,
  `player.strip_engine_code`); `ProjectSettingsValuesCmd` makes several keys one undo step
  (`editor/command/type/ProjectSettingsValuesCmd.h:13-16`); `set_project_settings` itself is not
  undoable (`docs/mcp/TOOLS.md:422`).
- **Audio import.** `AudioImportSettings` (`editor/audio/AudioImportSettings.h:79-89`): type, gain,
  trims, fades, loop points, music, unknown keys kept; meta key `import.audioImporter`, version 1
  (`:41-42`); keys written only when not default (`:6-8`). The engine plays them through
  `ClipImportSettings` (`engine/core/audio/AudioImport.h:18-50`) from `je-audio-imports.json`
  (`:66-83`, version 1; `AudioImport.cpp:102-135` reads known keys, refuses a newer version). There is
  no channel or encoding option. Exports ship sound files as they are (`Exporter.cpp:3401-3414` writes
  the table; no audio cook exists).
- **Playback.** `AudioClipSource` wraps the pool's sample and takes its channel count from the file
  (`engine/core/audio/ClipSource.cpp:100-123`). SoLoud mixes a 2-channel voice into stereo output by
  scaling channel 0 by the left pan gain and channel 1 by the right (`engine/libs/soloud/src/core/soloud.cpp:1140-1203`;
  mono takes the 1→2 path that feeds the same sample to both, `:1205`). A positional stereo voice
  panned hard to one side therefore drops the other channel's content (inferred from the code).
  Positional means: the scene has an AudioListener and the source's GameObject has a Transform
  (`engine/core/subsystem/AudioSystem.cpp:2114`, `engine/core/object/sound/AudioSource.cpp:164-166`).
- **Analysis.** `AudioAnalysis` measures stereo as the Pearson correlation of channels 0 and 1 only
  (`editor/audio/AudioAnalysis.cpp:809-830`), with a "cancels in mono" note below
  `PHASE_CORRELATION_MIN = -0.3` (`:2433-2436`, `editor/audio/SoundTypes.h:115`). Its cache is
  versioned (`AudioAnalysis.h:29`, file names `-v<VERSION>`, `:241`).
- **Inspector.** `InspectorAudioClip.cpp` draws Type, Gain, trims, fades and loop rows (`:508-534`) and
  applies them with `SetAssetMetaCmd` (undoable, `:580-583`). `set_audio_import_settings`
  (`editor/mcp/tools/SoundTools.cpp:1443-1466`, `audio` group) takes the same keys.
- **Tests.** `test_model_importer_ui.py:86-175` checks that `build-report.json` exists and the window
  shows it; there is no unit test of `BuildReport` in `editor/tests/`.

## 4. Reference

| Source | Pattern studied | Take | Change |
|---|---|---|---|
| Craftsman `tools/ee_optimize_assets.py` | Builds the shippable copy of an add-on into a sibling folder, never touching the originals; prices every folder it leaves out (file count, raw bytes, zipped bytes, the reason in words); a before/after table per group; downmixes positional sounds to mono after measuring each file's stereo width (RMS of L−R over RMS of L and R: near-mono recordings saved as stereo measured about −22 to −24 dB, real stereo beds about −7 to −14 dB) and keeps beds stereo; verifies its pass-through copies decode to identical PCM; found that its mixer pans a stereo positional voice per channel, so half the recording vanishes when panned | "Left out, with reason and bytes"; before/after per group as a report comparison; width-measured downmix with beds kept wide; deterministic cooked output | Our export already writes a separate tree; the report is part of every export, not a separate tool. The decision uses the clip's own setting (Off/On/Auto) because positional use is decided per AudioSource at run time, not per file. Auto's threshold is −18 dB (D7) |
| Craftsman `tools/eemodels_sheet.py` | Before/after contact sheets of rendered models from two engine builds | The idea of comparing two builds side by side | Numbers only (out of scope: images) |
| Unity `BuildReport` API, Build Report Inspector package (docs) | `BuildReport.summary.totalSize`, `PackedAssets`, `PackedAssetInfo.sourceAssetPath` and `packedSize` | Source asset per shipped file (S53 has it), names | Comparison, history and reachability are ours |
| Unity `AssetDatabase.GetDependencies` (docs) | The asset graph | S04's index plays this role | Reachability is computed at export and stored, so it describes that build |
| Unity `AudioImporter.forceToMono` (docs) | "Force To Mono" checkbox | The Inspector name | A three-way Off/On/Auto; Auto measures width |

**Origin and IP.** The Craftsman tree is a reconstruction of a retail game; it was read for ideas only.
No code, data, assets, thresholds tuned to its content, or game-specific mechanics are copied (no
add-on formats, sound categories or atlas rules). Unity is cited for names from its public
documentation only.

## 5. Requirements

### Functional: report content (package P1)

- **R1 Full report.** Every export's `build-report.json` keeps today's keys and adds: `id` (R6),
  `created` (UTC, ISO 8601), `editor_version`, `files` (every measured file: `path`, `category`,
  `bytes`, and `source`, `source_bytes` when cooked or replaced), `left_out` (R2), `reachability` (R3),
  `budgets` (R13). `largest` stays the 25 largest, for readers that know only it.
- **R2 Left out.** Each source the export did not ship as itself is listed `{source, bytes, reason}`
  with reason one of `cooked_texture`, `cooked_model`, `atlas_only`, `font_only`, `aseprite_import`,
  `title_art_unused`, `language_dropped`, `procedural_material_baked` (the skips of §3), in source path
  order. Their total is the report's `left_out_bytes`.
- **R3 Reachability.** At export the exporter copies the index's edges (main thread) and walks them
  breadth first from the roots, project.yaml (`AssetIndex::projectReferrer`) then every shipped script
  (each `.lua` of the Lua tree and assets, each C++ script source), roots and edges in path then
  field-locator order. Each reached asset records its first referrer and the edge's field:
  `reachability.assets[asset] = {via, field, kind}`. Each shipped file maps to an asset: `source` when
  cooked, `scenes/<id>.jscn` to the scene of that id, `prefabs/<id>.jpfb` to the prefab of its GUID,
  `assets/<rel>` to `<rel>`. Files with no asset get `engine` or `generated` (the asset map, the audio
  import table, cook indexes, Title Art runtime data); shipped assets no root reaches get `unreferenced`.
- **R4 Index readiness.** The exporter waits up to 30 s for `isReady()`; past that the report has
  `reachability: null`, warning `W-BUILD-REPORT-INDEX` ("the asset index was still building: the
  report has no Referenced By data; export again once the status bar's Indexing ends").
- **R5 Determinism.** The same project built twice gives the same report except `id`, `created` and
  `editor_version` (byte-compared in AC2).

### Functional: history and comparison (P1)

- **R6 History.** After writing `build-report.json`, the exporter writes the same bytes (AtomicFile) to
  `<project>/.jasperengine/build-reports/<platform>/<id>.json`, `id` = `<yyyyMMdd-HHmmss>-<method>`
  in UTC (`-2`, `-3`… on a collision), and deletes all but the newest 10 of that platform. A history
  write failure is warning `W-BUILD-REPORT-HISTORY`, never a failed export. `BuildReport::last()`
  falls back to the newest history entry, so the window has a report after a restart.
- **R7 Comparison.** `BuildReport::compare(base, head)` gives per category `{before, after, delta}`, the
  total, and per file one row keyed by `source` when present, else the build path:
  `added`, `removed`, `grown`, `shrunk` (unchanged rows are counted, not listed), with `before`,
  `after`, `delta`, sorted by |delta| descending then key. A base without `files` (a report from before
  this spec) compares its 25 largest and the categories and is marked `partial`.
- **R8 Across platforms.** Comparing reports of two platforms is allowed and marked
  `platform_differs` (texture families differ by design).

### Functional: Referenced By and Unreferenced (P2)

- **R9 Chain.** For a shipped file, the chain from its asset back to a root follows
  `reachability.assets[...].via` (at most 32 steps, a repeat ends it): each step `{asset, field,
  kind}`, the last a root ("project.yaml" with its field, such as `project/scenes[3]`, or a script
  path). An `unreferenced` file has an empty chain and the note "no scene, prefab, material, project
  setting or script reaches it in the index (a script may still load it by a path string: the index
  sees `require` only)". `engine` and `generated` files say so.
- **R10 Older reports.** A report without `reachability` answers from the live index, marked `live`
  ("as the project is now, not as it was built").
- **R11 Unreferenced list.** The report's unreferenced files with their bytes and total, largest first.

### Functional: size budgets (P2)

- **R12 Settings.** Project Settings keys `player.size_budgets`: a list of `{platform, category,
  limit_mb}` (`platform`: `all`, `windows`, `linux`, `macos`, `android`, `ios`, `web`, `desktop`;
  `category`: `total` or a report category; `limit_mb` > 0; 1 MB = 1,048,576 bytes as the report shows
  it), and `player.size_budget_action`: `warn` (default) or `fail`. A row for the build's platform
  replaces the `all` row of the same category. Source exports are never checked.
- **R13 Check.** Content categories (models, textures, audio, scripts, scenes, other) are checked right
  after `measureBuildContent`, before any compile or packaging; `engine` and `total` after the engine
  is measured. Every row's result goes to the report's `budgets` (`{platform, category, limit_bytes,
  bytes, over_bytes}`). Over a limit: warning `W-SIZE-BUDGET` per row, naming the platform, category,
  size, limit, excess and the category's three largest files, and the fix ("shrink them, or change the
  budget in Project Settings > Player > Size Budgets"). With `fail` the export writes the report and
  its history entry, then fails with that text; the output folder is kept for inspection.
- **R14** With no budget rows nothing is evaluated.

### Functional: Force To Mono (P3)

- **R15 Setting.** Import key `force_to_mono`: `off` (default, not written), `on`, `auto`. Any other
  value fails with "force_to_mono must be off, on or auto". A mono file ignores it.
- **R16 Width measure.** The analysis' `stereo` gains `side_mid_db` = 10·log10(Σ(L−R)² / Σ(L+R)²) over
  channels 0 and 1, clamped to [−90, 60]; `null` for a mono or silent file; 60 when the mid is silent
  and the side is not. A stereo clip at or below −18 dB gets the note "near-mono stereo: Force To Mono
  plays it the same, pans it correctly as a positional sound and, for a WAV, halves its bytes".
  `AudioAnalysis::VERSION` becomes 2.
- **R17 Auto.** Resolves to mono when `side_mid_db <= SoundTypes::MONO_SIDE_MID_DB` (−18) and the
  correlation is at least `PHASE_CORRELATION_MIN`; otherwise stereo. Resolved by
  `AudioImportRegistry::resolve` through the analysis cache, as `auto` trims are.
- **R18 Runtime.** `ClipImportSettings::forceMono`: `AudioClipSource` reports one channel and gives the
  mean of the file's channels per frame, for held and streamed samples; trims, fades, loop points,
  lengths and seeks are unchanged. `je-audio-imports.json` writes `"force_mono": true` only when set;
  `ClipImportSettings::hash()` mixes the flag in only when true (existing hashes, wrapped samples and
  analysis cache names stay as they are).
- **R19 Export cook.** A clip resolved to mono whose file is RIFF/WAVE PCM (8, 16, 24 bit) or float32
  ships as a mono WAV at the same path: same rate and frame count, the same sample format (8-bit
  becomes PCM16), the mean of the channels, deterministic bytes. Other formats (Ogg Vorbis, MP3, FLAC)
  ship unchanged and downmix at run time (no encoder, §2). The report gives the file `source` (the
  asset) and `source_bytes` (the original's size).
- **R20 Everywhere a clip plays.** Play in the editor (the registry), every export, Sound Event layers
  that play the clip by path (the same pool entry; inferred from S37 sharing the pool), and
  `analyze_audio`'s "as it plays" measurement.

### Platform & backends

- **R21** The report, history, comparison, budgets, window and cook are editor code on the Linux,
  Windows and macOS editors. The runtime downmix is scalar CPU code inside `AudioClipSource`: every
  export target, no SIMD requirement, no graphics involvement.

### Performance

Release build on the build server (x86-64), printed by the tests and gated through `perfSlack()`.

- **R22 Export.** Reachability for 10,000 assets and 60,000 edges ≤ 50 ms; a report of 20,000 files
  ≤ 4 MB written in ≤ 150 ms; the history copy and prune ≤ 50 ms; budget evaluation ≤ 1 ms.
- **R23 Compare.** Two 20,000-file reports parsed and compared ≤ 80 ms (main thread; `get_build_info`
  answers without a job).
- **R24 Window.** A 20,000-row table draws in ≤ 1 ms of CPU per frame (`ImGuiListClipper`); a column
  sort ≤ 20 ms; a Referenced By chain ≤ 1 ms.
- **R25 Runtime.** A clip without Force To Mono pays one branch per mixed buffer, none per sample. A
  Force To Mono voice of a stereo file costs no more `AudioClipSource` plus mixer CPU than the same
  voice in stereo (it takes SoLoud's cheaper 1→2 path), and allocates nothing per buffer.
- **R26 Memory.** A mono WAV cook halves the clip's decoded memory in the game (a held `Wav` is float
  per channel); the runtime downmix of a compressed or streamed clip saves no memory (documented).

### Editor UX (human)

- **R27 Build Report window toolbar.** The path field and Open stay; a **Report** dropdown lists the
  project's history ("Web · 2026-11-19 14:22 · prebuilt · 142.0 MB", same platform first);
  **Compare With…** lists the history (other platforms marked) and **Browse…**; **Clear Comparison**.
  Comparing adds Before, After and Delta (green when smaller, orange when larger) to the category rows.
- **R28 Tabs.** **Files** (all files, a search box, a category filter, sortable File, Category, Size,
  Source, Source Size, and Delta when comparing); **Changes** (comparing only: Change, File, Category,
  Before, After, Delta, with Added, Removed, Grown and Shrunk toggles); **Unreferenced** (files and
  their total); **Left Out** (source, reason in words, bytes, total); **Budgets** (each row with a bar,
  Under or Over, and **Edit Budgets…**, which opens Project Settings at Size Budgets).
- **R29 Detail pane.** Selecting a row shows **Referenced By**: the chain as clickable steps (an asset
  is shown in the Project window and Inspector; a GameObject step names its path and component field).
  For a stereo audio file it also shows "Channels: Stereo · width −24.3 dB · Force To Mono: Off" and,
  when Auto would pick mono, **Set Force To Mono: Auto**, an undoable `SetAssetMetaCmd`.
- **R30 Project Settings > Player > Publishing Settings > Size Budgets.** A table (Platform, Category,
  Limit (MB), remove), **Add Budget**, and **When Over Budget: Warn | Fail Build**; each edit is one
  undo step (`ProjectSettingsValuesCmd`); searchable as "size budget download".
- **R31 Audio clip Inspector.** A **Force To Mono** row (Off, On, Auto) after Loop End; with Auto a
  greyed hint "→ Mono (width −24.3 dB)" or "→ Stereo (width −9.1 dB: a wide image)"; disabled with
  "(the file is mono)" for mono files. Apply and Revert as today (undoable).
- **R32 Export completion page.** With an earlier report of the platform in the history: "Build
  Report: 142.0 MB (+3.2 MB since 2026-11-18 10:15)", and one line per budget over its limit.

### Agent UX (MCP, Lua, CLI)

- **R33 `get_build_info` parameters** (extending the core tool, conventions checklist 1):
  `report` (string: `last`, the default when any of the three is given, a history id, or a
  `build-report.json` or output folder path), `compare_to` (string: `previous`, the history entry
  before `report`'s of the same platform, a history id, or a path), `referenced_by` (array of build or
  asset paths, at most 50). Results add `report` (`id`, `platform`, `method`, `created`, `total_bytes`,
  `categories`, `largest` (25), `unreferenced` (count, bytes, top 10), `left_out` (count, bytes, top
  10), `budgets`, `history` ids of the platform), `comparison` (categories, total, at most 50
  `changes`, counts, `partial`, `platform_differs`, `truncated`) and `referenced_by` (`{file, asset,
  reason, chain, live}` per path). An unknown id or path fails naming the history ids it has.
  `structuredContent.next`: over a budget, which files to look at; an unreferenced file asked about,
  R9's note and "delete it, or keep it if a script loads it by a computed path". Text: "Build Report
  web 20261119-142233-prebuilt: 142.0 MB (+3.2 MB vs 20261118-101500-prebuilt); textures +2.9 MB
  (3 grown, 1 added) …".
- **R34 `set_audio_import_settings`.** `settings` accepts `force_to_mono`; the schema's key list gains
  it; the reply's resolved settings include `force_mono` (true/false after Auto) and `channels` (as it
  plays). `analyze_audio` items carry `stereo.side_mid_db` and R16's note (result shape only).
- **R35 CLI.** `jasperengine-editor call get_build_info` takes the same arguments (S31); `build --json`
  adds `build_report_id` and `size_budgets` (the report's rows), and a `fail` budget exits non-zero
  through the existing failed-build path.
- **R36 Lua.** `AudioSource.channels` (read-only): the channels of the loaded clip as it plays (1 under
  Force To Mono, 0 with no clip). Documented in the binding comment the generator reads and in
  `docs/mcp/guide/lua-api.md`. The report, history and budgets are editor-only (no run-time part, so
  no Lua API).
- **R37 Docs.** `docs/shipping.md` and `docs/mcp/guide/shipping.md` get a Build Report section
  (history, Compare With, Referenced By, Unreferenced, Left Out, budgets); `docs/meshes.md:118-127`
  keeps a one-line pointer to it; `docs/mcp/guide/sound.md`'s key table gains `force_to_mono`;
  `docs/unity-glossary.md` gains rows (Force To Mono, Build Report comparison).
- **R38 Eval and budget.** `tools/mcp/eval/cases.py` gets two cases that fail without the new text:
  "the web build grew, find out why" (graded on `get_build_info compare_to: previous`) and "this
  texture should not be in the build" (graded on `referenced_by`). The three parameters add at most
  380 characters to `get_build_info`; `force_to_mono` adds at most 20 to `set_audio_import_settings`;
  `tool_budget.py --check` passes and `TOOLS.md` is regenerated.

### Data & compatibility

- **R39 Report file.** `build-report.json` stays version 1; R1's keys are optional. Old editors read new
  reports (unknown keys ignored, `BuildReport.cpp:148-170`); this editor reads old ones (no `files`:
  R7's partial comparison; no `reachability`: R10).
- **R40 Project settings.** `sizeBudgets` and `sizeBudgetAction` in project.yaml are optional with
  defaults (none, warn): no version bump, no migration.
- **R41 Audio meta.** `force_to_mono` is optional in `import.audioImporter`; `currentVersion` stays 1;
  an older editor keeps the key through a round trip (unknown keys survive, `AudioImportSettings.h:7-8,88-89`)
  and plays the clip in stereo.
- **R42 Game data.** `je-audio-imports.json` stays version 1 with an optional `force_mono`; prebuilt
  players are matched to the editor version (S30), so no older runtime reads it.
- **R43 Caches and committed analysis.** The audio analysis cache moves to version 2 and rebuilds on
  demand; no project data changes meaning. The committed starter library analysis
  (`editor/resources/sound-library/library/analysis.json`, pinned by
  `AudioAnalysisLibrary.ClipsReadAsTheCommittedAnalysis`, `editor/tests/audio/AudioAnalysisLibraryTest.cpp:31-79`,
  and `test_sound_library_data.py::test_d5_analysis_is_current`) is regenerated in the same commit with
  `tools/sound-library/build_library.py analyse --out editor/resources/sound-library/library --editor <built editor>`
  (its items gain `stereo.side_mid_db` and `analyzer_version` 2); the commit says why.

## 6. Design constraints & decisions

**D1 One report file, version 1, full list inside.** A separate file list would need its own name,
history and pairing. Adding `files` to the same JSON keeps "every export writes one file next to its
output", keeps old editors reading it, and costs about 150 bytes per file (4 MB for 20,000 files, R22).
`largest` is kept (25 rows) rather than derived, so S53's readers and tests stay as they are.

**D2 Compare by source asset.** Cooked model names are content hashes (`ModelExport.cpp:106`): a
changed model is a new path. Keying by `source` turns "removed + added" into "grown" for every cooked
or replaced file; files shipped as they are key by build path.

**D3 Reachability at export, stored.** A forward walk from the roots is linear in the edges and
describes that build, so a comparison months later still explains it. A live query would describe the
project as it is now (R10 keeps it only for old reports).

**D4 Roots, and what the index cannot see.** project.yaml covers the scene list and standalone prefabs
(`SceneList`, `ScenePrefabList` edges); every shipped script is a root because the export ships the
whole Lua tree and scripts reach assets through `require` and script properties. A path string built or
passed at run time (`loadSound("sfx/" .. name)`) is invisible to the index, so `unreferenced` is a
lead, never a verdict, and nothing is excluded automatically.

**D5 History in `.jasperengine/`.** Beside the other per-project editor data (cache, assetdb,
builds), out of the shipped tree, ten per platform: enough for "since yesterday" and "since the last
release" without growing without bound. A report worth keeping longer is a file: Compare With… takes
any path.

**D6 Budgets.** Rows instead of a key per platform and category (64 keys). Content rows fail before a
compile, so a 20-minute compile is not wasted on a build that will fail; the report is written first so
the failure can be read in the window. `warn` is the default so adding a budget never breaks a build
unasked.

**D7 Force To Mono semantics.**
- *Runtime downmix plus a WAV cook.* The downmix fixes the panning for every format; only PCM WAV can be
  rewritten smaller without an encoder. A mono WAV from FLAC would be about as large as the FLAC, so
  FLAC stays as it is.
- *Default Off.* Today's behaviour; Auto is opt-in (old projects sound the same).
- *Auto at −18 dB.* Side energy at most 1.6 % of mid energy: the downmix loses under 0.07 dB of energy
  and no audible image on a near-mono recording. It sits between the near-mono stereo files and the
  real stereo beds measured in the Craftsman study (about −22 to −24 dB against −7 to −14 dB), so beds
  stay stereo with margin. A constant in `SoundTypes.h`, not a setting.
- *Mean of the channels.* Identical channels keep their level exactly; uncorrelated ones drop 3 to 6
  dB, which only happens with On (Auto never picks such clips).
- *Per clip, not per use.* Whether a voice is positional is decided per AudioSource at run time
  (`AudioSystem.cpp:2114`); the Build Report and Inspector hints point at stereo clips Auto would fold.

**D8 Paths.** `get_build_info` grows instead of a new tool (conventions checklist 1): the report is the
build's last result, which it already returns (`last_build`). If the default listing no longer fits,
the parameters move to a `shipping`-group `get_build_report` (a Deviation), never a raised limit.

**API sketch (editor C++):**

```cpp
// editor/cook/BuildReport.h (additions)
struct BuildReportLeftOut { std::string source; uint64_t bytes = 0; std::string reason; };
struct BuildReportReach { std::string via, field, kind; };       // per asset
struct BuildReportBudget { std::string platform, category; uint64_t limitBytes = 0, bytes = 0; };
class BuildReport {
    // ... existing members ...
    std::string id, created, editorVersion;
    std::vector<BuildReportFile> files;                           // every file, path order
    std::vector<BuildReportLeftOut> leftOut;
    std::optional<std::map<std::string, BuildReportReach>> reachability;
    std::vector<BuildReportBudget> budgets;
    std::vector<std::string> chainOf(const std::string& fileOrAsset) const;   // R9
};
struct BuildReportChange { std::string key, path, category, change; uint64_t before = 0, after = 0; int64_t delta = 0; };
struct BuildReportDiff {
    std::map<std::string, std::pair<uint64_t, uint64_t>> categories;
    uint64_t totalBefore = 0, totalAfter = 0;
    std::vector<BuildReportChange> changes;                       // |delta| desc, key
    size_t unchanged = 0; bool partial = false, platformDiffers = false;
};
BuildReportDiff compare(const BuildReport& base, const BuildReport& head);
// editor/cook/BuildReportHistory.h
namespace BuildReportHistory {
    constexpr size_t kKeep = 10;
    bool record(const Project&, const BuildReport&, const std::string& bytes, std::string& warning);
    std::vector<std::string> list(const Project&, const std::string& platform);   // newest first
    std::filesystem::path resolve(const Project&, const std::string& idOrPath, std::string& error);
}
// editor/cook/Reachability.h
std::map<std::string, BuildReportReach> computeReachability(const std::vector<DepEdge>& edges,
    const std::vector<std::string>& roots);                       // deterministic BFS (R3)
```

```cpp
// engine/core/audio/AudioImport.h
struct ClipImportSettings { /* ... */ bool forceMono = false; };   // hash() mixes it in only when true
// editor/audio/AudioImportSettings.h
enum class ForceToMono { Off, On, Auto };
ForceToMono forceToMono = ForceToMono::Off;                       // key force_to_mono, after loop_end_ms
// editor/audio/SoundTypes.h
inline constexpr double MONO_SIDE_MID_DB = -18.0;
```

**MCP sketch:**

```cpp
{"report", string("last (default), a history id or a build-report.json path.")},
{"compare_to", string("previous, a history id or a path: per category and file changes.")},
{"referenced_by", array(stringItem, "Build or asset paths: why each ships.")}
```

## 7. Acceptance criteria

| AC | Req | Verification |
|---|---|---|
| AC1 | R1, R39 | `BuildReportTest.FullListAndOptionalKeys` (`editor/tests/cook/BuildReportTest.cpp`, new): 40 files in a `TempDir`, `files` has 40 in path order, `largest` 25, `version` still 1; `fromJson` of a base-tree report (fixture `editor/tests/fixtures/build-report/v1-largest-only.json`, written by the base tree) loads with no `files`; the fixture `v1-with-new-keys.json` loads with today's key set unchanged (`largest`, `categories`, totals equal). Red: on the base tree `files` is absent |
| AC2 | R5 | `BuildReportTest.SameProjectSameReport`: two `addTree` runs and two reachability walks over the same fixture give byte-identical JSON after removing `id`, `created`, `editor_version` |
| AC3 | R2 | `test_export_pipeline.py::test_build_report_left_out` (MCP, source export of a fixture with an `.aseprite`, an atlas-only texture, a cooked texture and a Procedural Material): `left_out` lists each with the right reason and its source bytes (`os.path.getsize`) |
| AC4 | R3, R4, R9 | `ReachabilityTest.ChainsAndRoots` (`editor/tests/cook/ReachabilityTest.cpp`): a graph project.yaml → scene → material → texture, a script → `require`d module, an orphan texture: chains as expected, orphan `unreferenced`, ties broken by path then field; a 10,000-asset/60,000-edge synthetic graph within R22's 50 ms. `test_export_pipeline.py::test_build_report_referenced_by`: `get_build_info referenced_by: ["textures/wall.png", "textures/orphan.png"]` returns the scene → material → texture chain and `unreferenced` with R9's note in `next` |
| AC5 | R6 | `BuildReportHistoryTest.RecordsAndPrunes`: 12 records of `web` keep the newest 10, ids sort by time, a collision gets `-2`; a read-only history folder gives the warning and `record` returns true for the export. `test_export_pipeline.py::test_build_report_history_survives_restart`: export, restart the editor, the window's report and `get_build_info report: last` are that export's |
| AC6 | R7, R8 | `BuildReportTest.CompareBySource`: a model whose cooked hash name changed is one `grown` row keyed by its source; an added texture, a removed sound, a shrunk script; sort by |delta|; a base without `files` gives `partial`; two platforms give `platform_differs`. Red: keying by build path yields removed + added |
| AC7 | R33 | `test_export_pipeline.py::test_build_report_compare`: two source exports around replacing a texture with a larger one; `get_build_info compare_to: previous` reports the texture `grown` with the byte delta, the category delta, and the text summary; an unknown id fails listing the history ids |
| AC8 | R12-R14 | `SizeBudgetTest.RowsAndSpecificity` (`editor/tests/export/SizeBudgetTest.cpp`): a `web` row overrides `all` for the same category; MB is 1,048,576 bytes; source exports skip; no rows means no evaluation (a counter stays 0). `test_export_pipeline.py::test_size_budget_warns_and_fails`: a 1 MB textures budget over a 3 MB texture gives `W-SIZE-BUDGET` with the texture named; with `fail` the export fails before any compile step, `build-report.json` and the history entry exist and carry `budgets`. Red: on the base tree the keys are unknown to `set_project_settings` |
| AC9 | R16, R43 | `AudioAnalysisTest.SideMidWidth` (`editor/tests/audio/AudioAnalysisTest.cpp`, new, for `AudioAnalysis.cpp`): identical channels give −90 (clamped), independent noise gives about 0 dB (±0.5), a −24 dB side mix gives −24 ± 0.2, a mono file `null`, silence `null`; the note appears at −18 and not at −17; the cache file name carries `-v2`. `AudioAnalysisLibrary.ClipsReadAsTheCommittedAnalysis` and `test_d5_analysis_is_current` pass against the regenerated `analysis.json` (red: they fail with the new analyzer and the old file) |
| AC10 | R15, R17, R41 | `AudioImportSettingsTest.ForceToMonoKey`: parse, write (absent when off, after `loop_end_ms` otherwise), bad value error text, an unknown later key still round-trips; `AudioImportSettingsTest.AutoResolvesFromWidth`: a −24 dB clip resolves mono, a −9 dB clip stereo, a phase-inverted clip stereo |
| AC11 | R18, R25 | `ClipSourceTest.ForceMonoMeanOfChannels` (`engine/tests/core/audio/ClipSourceTest.cpp`): a held and a streamed stereo WAV with `forceMono` report 1 channel and give (L+R)/2 per frame, trims, loop wrap and fades at the same frames as the stereo clip; `ClipImportSettings::hash()` of every existing test vector unchanged. `ClipSourceBudgetTest` gains `ForceMonoCostsNoMore`: mixing 32 forced-mono voices costs ≤ the same 32 stereo voices (`perfSlack()`), 0 allocations per buffer (`ThreadAllocationCount`) |
| AC12 | R19 | `test_audio_import.py::test_force_to_mono_export` (MCP): a 16-bit and a 24-bit stereo WAV and a stereo Ogg set to On, a source export: the WAVs ship mono with half the data bytes (± header), same rate and frames, decoded samples equal the mean of the originals (±1 LSB); the Ogg ships byte-identical; `je-audio-imports.json` has `force_mono: true` for all three; two exports give identical WAV bytes; the report's audio rows carry `source_bytes` |
| AC13 | R20, R34, R36 | `test_audio_import.py::test_force_to_mono_plays_mono` (MCP): `set_audio_import_settings {force_to_mono: "auto"}` on a near-mono stereo clip replies `force_mono: true`, `channels: 1`; in Play `eval_lua` reads `AudioSource.channels == 1` on a source playing it; `analyze_audio` of the clip reports one channel as it plays and `stereo.side_mid_db` for the file |
| AC14 | R27-R29, R31, R32 | `test_build_report_ui.py` (MCP UI test in the style of `test_model_importer_ui.py`, `find_ui_item` and screenshots on an isolated editor): two exports, Compare With… the earlier one, the Changes tab lists the grown texture, the detail pane shows its Referenced By chain, the Unreferenced and Left Out tabs list the fixture's files; the audio clip Inspector shows the Force To Mono row with the Auto hint; screenshots attached to the PR |
| AC15 | R30 | `SizeBudgetTest.SettingsRowsUndo`: adding a row and switching to Fail Build through `ProjectSettingsValuesCmd`, then undo, restores project.yaml byte for byte; `get_project_settings search: "size budget"` finds both keys |
| AC16 | R22-R24 | `BuildReportBench.LargeReport` (`editor/tests/cook/BuildReportBenchTest.cpp`): 20,000 synthetic files: write ≤ 150 ms and ≤ 4 MB, parse + compare ≤ 80 ms, chain lookup ≤ 1 ms, printed and gated through `perfSlack()` |
| AC17 | R33, R38 | `tool_budget.py --check tools/mcp/budget.json` passes; `gen_tool_docs.py --check` passes after regenerating `TOOLS.md` (`test_docs.py`); the two eval cases of R38 pass with the new text and fail on the base tree's (`tools/mcp/eval/README.md` procedure) |
| AC18 | R35 | `test_export_pipeline.py::test_cli_build_json_budget`: `jasperengine-editor build --json` on the fixture with a `fail` budget exits non-zero and prints `build_report_id` and `size_budgets` |
| AC19 | R21 | Web gate (`tools/build.sh web`) and the Android build pass with the `AudioClipSource` change; `ClipSourceTest` runs in the macOS and Windows unit lanes |
| AC20 | R40, R42, R43 | `ProjectSaveTest.SizeBudgetKeysOptional`: a project.yaml without the keys loads with no budgets and saves without them; with them, they round-trip. `ClipSourceTest.ImportTableForceMono` (`engine/tests/core/audio/ClipSourceTest.cpp`, where `AudioImportTable` is tested today): a table without `force_mono` parses as before; with it, sets the flag |

## 8. Dependencies & conflicts

- **Depends on:** S53 R21 (BuildReport and its window, landed), S04 (AssetIndex, DepEdge,
  `projectReferrer`, landed), S36 (audio import settings and analysis, landed), S30 (prebuilt
  exports, `ExporterPrebuilt.cpp`), S37 (Sound Events share the clip pool; R20 relies on it).
- **Sibling specs:** `build-profiles.md` adds Exclude Paths and Exclude Labels per profile: files it
  leaves out appear in R2's Left Out with the reason `profile_excluded`, and a profile may carry its own
  size budget rows (replacing the project's for that profile); whichever spec lands second adds the
  reason and the hook in R13's evaluation. Its exclusions are the place to act on R11's Unreferenced
  list. `build-and-run-on-device.md` and `release-performance-and-gpu-quirks.md`: no coupling
  beyond sharing `Exporter.cpp`. `project-test-runner.md`: a CI job may call `get_build_info
  compare_to` or rely on a `fail` budget; no coupling.
- **Merge hotspots:** `editor/Exporter.cpp` (`copyAssets`, `measureBuildContent`, `writeBuildReport`),
  `editor/ExporterPrebuilt.cpp`, `editor/cook/BuildReport.{h,cpp}`, `editor/window/BuildReportWindow.{h,cpp}`,
  `editor/window/dialog/ExportWindow.cpp`, `editor/mcp/tools/BuildTools.cpp` (`get_build_info`, also
  touched by S64 and S30 follow-ups), `editor/settings/SettingsRows.cpp` (placement table),
  `editor/Stream.cpp` (project.yaml keys), `editor/audio/AudioImportSettings.{h,cpp}`,
  `editor/audio/AudioAnalysis.{h,cpp}` and the regenerated
  `editor/resources/sound-library/library/analysis.json` (any other analyzer change in flight must
  regenerate it after this one lands), `editor/audio/AudioImportRegistry.cpp`,
  `editor/window/InspectorAudioClip.cpp`, `engine/core/audio/AudioImport.{h,cpp}`,
  `engine/core/audio/ClipSource.{h,cpp}`, `editor/mcp/tools/SoundTools.cpp`,
  `editor/cli/ShippingCommands.cpp`, `docs/shipping.md`, `docs/mcp/guide/shipping.md`,
  `docs/mcp/guide/sound.md`, `docs/mcp/TOOLS.md` (generated), `tools/mcp/budget.json`,
  `tools/mcp/tests/test_export_pipeline.py`, `tools/mcp/tests/test_audio_import.py`.

## 9. Risks & defaults

| Risk / open question | Default taken |
|---|---|
| `unreferenced` flags assets a script loads by a computed path | Reported as a lead with R9's note (D4); never excluded automatically |
| The report grows to several MB for very large projects | 4 MB for 20,000 files (R22); it sits beside the output, which for a web folder may be uploaded with the game: documented ("delete build-report.json before uploading, or export to a folder you upload from") |
| A content budget fails before compile while the engine budget would also fail later | Both are reported when reached; a content failure stops the export before the engine is measured, which the message says |
| A forced-mono stereo clip with uncorrelated channels loses 3-6 dB | Only with On; Auto never picks it (D7); the Inspector shows the width beside the choice |
| Hard-panned stereo positional voices lose a channel today | Inferred from SoLoud's mix code (§3); AC13 confirms the mono path; a fix inside SoLoud (downmixing positional stereo voices) is out of scope because it would change how every existing stereo positional clip sounds |
| The analysis cache rebuild (version 2) costs a decode per clip on first use | Same as any analyzer change; it happens lazily, per clip, as today |
| `get_build_info` is a core tool and every session pays for its schema | R38 caps the growth at 380 characters; over the default listing, D8's fallback applies (a Deviation) |
| Two editors exporting the same project at the same second | The id gets `-2`; AtomicFile keeps each history file whole |
| History lives in `.jasperengine/`, which a team may not share | A report is a file: Compare With… and `compare_to` take any path; CI keeps its own |

## Deviations

Report, history, reachability and budgets (P1, P2):

- **Names.** The comparison is the free function `compareBuildReports(base, head)` and the walk takes
  `ReachEdge`s (both ends as report asset keys; `reachEdgesOf` converts the index's `DepEdge`s), so
  the exporter and `get_build_info`'s live answer (R10) share one conversion. `BuildReport::last()`
  stays the session's report; `BuildReportHistory::last(project)` adds the history fallback (R6).
- **When the edges are copied.** In a main-thread task when the report is written (after the scripts
  and C++ sources are copied), not right after `measureBuildContent`: one copy serves the file to
  asset mapping (scene ids, prefab GUIDs) and the walk.
- **C++ script sources are not roots.** They hold no edges of the index (the scenes' `CppScript`
  edges reach them), and `cppExportedPaths` also lists the generated sources, which are no assets.
- **Reachability entries carry `object`** (the GameObject holding the field, `DepEdge::entityPath`)
  when there is one, so R29's step names the GameObject's path rather than its file id.
- **File layout.** `build-report.json` keeps one line per file, left-out source and reached asset
  (containers of scalars on one line); a file's `asset` is written only when its path does not
  imply it (`assets/<rel>` ships `<rel>`, a cooked file its `source`), and files with no asset get
  `origin` (`engine`, `generated`); a file's `category` is written only when its path does not give it
  (`largest` keeps every key for older readers); a reachability entry is `[via, field, kind]` (and the
  GameObject). Keys per entry put the 20,000-file bench at 4.67 MB; without them it fits R22's 4 MB.
- **Budget specificity.** A `desktop` row sits between a platform's row and `all` (windows, linux and
  macos builds take it over `all`; their own row over it).
- **AC8 and AC18** use a prebuilt Linux build with the stand-in runtime `test_build_method.py`
  installs (source exports are never checked, R12; a compile would cost minutes for the warn case).
- **`get_build_report` (D8's fallback).** The three parameters on `get_build_info` (354 characters)
  put the default listing at 99,254 of its 99,000 (`tool_budget.py --check`), and trimming them and
  the description still left it over. They are a `shipping`-group tool, `get_build_report {report,
  compare_to, referenced_by}`, which returns only the report part of R33's result; `get_build_info`'s
  description points to it ("sizes: get_build_report", 28 characters). The eval cases (R38) and the
  MCP tests call it; the CLI is `jasperengine-editor call get_build_report` (R35).
- **`string_table_cooked`**: game-ui-localization (merged meanwhile) ships `.stringtable` and
  `.translation` files cooked into `assets/l10n`; Left Out lists them with this reason.
- **`profile_excluded`** (build-profiles.md landed first): Left Out lists each file a Build Profile's
  exclusions leave out of the assets walk; a profile's own budget rows come through its
  `player.size_budgets` override, which the build's settings snapshot applies.

Force To Mono (P3):

- Test names follow the suites their files already use: AC11's `ClipSourceTest.ForceMonoMeanOfChannels` is
  `AudioClipSource.ForceMonoMeanOfChannels`, AC20's `ClipSourceTest.ImportTableForceMono` is
  `AudioImportTable.ImportTableForceMono`, `ClipSourceBudgetTest` is the suite `ClipSourceBudget`, AC9's test is
  `AudioAnalysis.SideMidWidth`. AC11's pinned hashes are `ClipImportSettings.ExistingHashesStayAndForceMonoChangesIt`.
- `analyze_audio` reports the channels as a clip plays in `played.channels` (every played item), and the file's
  `stereo` joins the keys copied under `source`, so a forced clip still shows the file's `side_mid_db`.
- Auto treats a null correlation (one channel constant) as not cancelling: only the width decides then.
- The mono WAV cook writes only the `fmt ` and `data` chunks: a clip's loop and trims come from its import
  settings, not from `smpl` or `cue ` chunks, which the engine does not read.
- **R23 on box A.** `BuildReportBench.LargeReport` parsed and compared two 20,000-file reports in
  80.6-89.2 ms on box A (a 2.4 GHz Haswell; RelWithDebInfo, `JASPER_TEST_PERF_SLACK=1`), the compare
  24-31 ms of it: an `unordered_map` node per file. `compareBuildReports` keys the files in one
  open-addressed array sized up front: 62-69 ms (the compare 11-14 ms) over three runs, the same rows.
