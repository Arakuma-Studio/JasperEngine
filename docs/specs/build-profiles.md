# Build Profiles: named build variants per platform with their own identity, scenes, define symbols, content filters and Player Settings overrides, resolved at build time

Builds on S30 (shipping, landed), S25 (asset labels, landed) and S04 (asset identity, landed).
Sibling of build-and-run-on-device.md (Build And Run builds the selected profile). Paths are relative
to the repository root.

## 1. Goal

A project builds one game per platform today: one Product Name with per-platform overrides, one
identifier, one scene list, one set of Player Settings. A demo, a store-specific edition, a
branded white-label copy or a test build with cheats needs a second copy of the project, a script
that rewrites settings before each build, or a person who remembers to flip ten fields and flip
them back. Build Profiles, after Unity 6, make each variant a named, committed file: a platform plus
overrides of the name, identifier, version suffix, icon, scenes, scripting define symbols,
content exclusions, graphics API and any Player Settings key. A build resolves the profile once into
the values it writes; the project itself never changes. No profile means exactly today's build.

## 2. Scope

**In scope**
- `.buildprofile` files under `EditorSettings/BuildProfiles/`, one per profile, versioned YAML.
- Per profile: platform; Product Name, Identifier, Version Suffix, Icon; Store and Store Title with
  store limit checks; Graphics API; Development Build; output folder; a scene list and start scene
  (or the project's Scenes In Build); Scripting Define Symbols; Exclude Labels and Exclude Paths;
  Player Settings overrides (settings keys of the shared and the profile's platform categories).
- The active profile (per user): Play uses its identity, defines and scene check; Build Settings
  opens on it; CLI and MCP name a profile explicitly.
- Build Settings UI (list, Add, Duplicate, Rename, Delete, Switch Profile, every row undoable),
  `edit_build_profile`, `build_project profile`, `get_build_info profiles`, CLI `build --profile`,
  Lua `System.getBuildProfile()`, `System.isDefined()`, `System.getBuildInfo()`,
  `publishing.json` targets naming a profile.

**Out of scope (default decisions)**
- A Lua preprocessor (`--#if DEMO`). Defines are runtime values; code behind `System.isDefined`
  still ships. Code that must not ship goes in a folder the profile excludes (R11).
- Store listing texts (descriptions, keywords, screenshots) and uploads to Google Play or App Store
  Connect: only the title the build itself writes is checked.
- Overriding physics, input, tags and layers, scripting or editor settings per profile: a profile
  changes how the game is packaged and tuned, not what its scenes mean (R13).
- Per-profile shader additions and exclusions: the shader list follows the profile's scenes; the
  project's additions and exclusions apply to every profile.
- Unifying the per-platform identity fallback chains (section 3, the identity finding): kept as
  they are (Risks).
- Executable name and save-data folder: both stay derived from the Project Name, so a demo and its
  full game share saves by default; a separate save folder is savegame.md's to offer.

## 3. Current state

All read from the code (not run):

- **One identity, per-platform overrides.** `player.product_name` is the Project Name (editor title,
  default app and executable name: `editor/settings/SettingsRows.cpp:1143-1157`, label "Project
  Name" at `:4243`); `player.application_name` is the display name labelled "Product Name"
  (`:1159`, `:4244`); `player.identifier` and `player.version` are shared (`:1161-1169`). Each platform
  has its own overrides: `web.application_name`, `linux.application_name`, `windows.product_name`
  and versions, `macos.application_name`/`bundle_identifier`/`version_name`/`build_number`/`icon`,
  `ios.*` likewise (`:1474-1515`), `android.application_name`/`package_name`/`version_code`/
  `version_name`/`launcher_icon` and adaptive icons (`:1633-1641`). `ApplicationSettings` holds the
  shared four (`editor/Project.h:649-657`).
- **The identity a build writes differs by platform (finding).** Play and the source export resolve
  "platform override, else shared value, else default" (`editor/Project.cpp:3514-3527`,
  `:3546-3551`, `:3560-3580`). The prebuilt finalizers do not all do so:
  - Android uses the shared chain (`editor/ExporterPrebuilt.cpp:1180-1183`).
  - macOS and iOS fall back from their own overrides straight to the Project Name and to
    `com.jasperengine.<app>`, skipping `player.application_name`, `player.identifier` and
    `player.version` (`:1020-1023`, `:1239-1241`).
  - Windows falls back to the Project Name (`:993`).
  - `game.json` takes `appId` from the platform overrides only and `version` from
    `android.version_name` on every platform (`:591-597`).
- **Icons.** `game.json`'s window icon is `player.window_icon` (`:697-703`); Windows patches it into
  the executable (`:997-1001`); macOS and iOS use their own icon, else the default icon
  (`:1026`, `:1246`); Android uses `android.launcher_icon` or the adaptive pair, else the engine's
  icon, never the default icon (`editor/platform/AndroidPackager.cpp:225`, `:243`, `:362-367`).
- **Scenes.** A prebuilt build cooks and lists every project scene (`editor/ExporterPrebuilt.cpp:601-611`)
  with the scenes each one involves (`editor/Project.h:2130`); "Scenes In Build" is the project's
  scene list (`project.yaml` `scenes`, `editor/Stream.cpp:3443`; `startSceneId`, `:3250`), edited by
  `edit_build_scenes` (`docs/mcp/TOOLS.md:283-296`).
- **Content.** `copyAssets` walks the whole project tree (`editor/Exporter.cpp:3635-3663`) except the
  non-asset top-level folders: hidden ones, `build`, `Recordings`, `EditorSettings`
  (`editor/Project.cpp:5989-5991`). Nothing drops content per platform or variant. Asset labels exist
  (0-32 per asset, in `.meta` or in-file `labels:`, `editor/organisation/AssetLabels.h:6-12`).
- **No defines, no profiles.** Nothing in `editor/` or `engine/` names define symbols or build
  profiles (grep for `definesymbol`, `scriptingdefine`, `buildprofile`, `build_profile`). Lua has
  `System.getAppName/getAppVersion/getAppIdentifier` (`engine/core/script/binding/CoreClassesLua.cpp:4064-4067`)
  and `Engine.isDevelopmentBuild` (`:2418`).
- **Publishing targets** carry platform, arch, backend, flavour, build folder, executable, depot,
  device profile and required files (`editor/publish/Publishing.h:24-34`); `deviceProfile` is the S56
  tier profile, unrelated to build profiles.
- **Per-project editor files.** `EditorSettings/` holds committed, non-shipped editor data
  (`editor/widget/CurvePresets.cpp:60`, `editor/widget/SwatchLibrary.cpp:43`, written with
  `io::writeFileAtomic` at `:185`). Per-user state lives in `.jasperengine/user`
  (`editor/Workspace.h:11-17`).
- **Formats.** `project.yaml` is `formatVersion` 1 (`editor/StreamMigrations.cpp:711`); the cooked
  content format is minor 5 (`engine/core/serialization/CookedScene.h:42`).
- **Settings setters** take a `SettingsContext` holding a `Project*` and validate with `apply=false`
  (`editor/settings/SettingsRegistry.h:32-43`, `:70-72`).

## 4. Reference

| Source | Pattern studied | Take / improve |
|---|---|---|
| craftsman-end-motor `tools/brand/rebrand.sh` | The visible name comes "from one place": a script text-replaces the current name by the new one across a closed list of files, regenerates translations, baked pages, the logo and the store listing, checks lengths (short name ≤ 24, store title ≤ 30, listing short text ≤ 80, long text ≤ 4,000) and leaves a diff to commit (`:2-21`, `:39-43`, `:96`) | Take: one source of truth for the name and store-length checks. Improve: no text replacement in committed files. The name is one setting, resolved when a build writes it; changing it touches one file. The lengths become a table of store limits, checked at build and at `publish prepare` |
| craftsman-end-motor `tools/ios/bundle_assets.sh` | Per-platform content dropping: the iOS app omits two resource packs and keeps store packs as stubs (`:10-22`); icons resized per Info.plist slot (`:26-30`) | Take: per-variant content exclusion by path. Improve: declared in the profile (Exclude Paths, Exclude Labels), checked against references so an excluded asset a scene still uses fails the build instead of crashing the game |
| craftsman-end-motor `tools/ios/run_sim.sh` | Variants switched by environment flags (`TEST_MODE`, `REMOTE`, `SCALE` through `SIMCTL_CHILD_*`, `:17-26`) | Take: named switches a game can read. Improve: Scripting Define Symbols stored in the profile and shipped with the build, the same in Play and on every platform, instead of host-side environment |
| Unity 6 **Build Profiles** (File > Build Profiles; platform profiles and custom Build Profile assets; Scene List override; Scripting Defines; Player Settings overrides; Switch Profile; `-activeBuildProfile` on the command line) | Names and behaviour | Names only, no code. Unity stores profiles as assets; ours live in `EditorSettings/` so they never ship |

Ideas only: no code, data, assets or Minecraft-specific mechanics may be copied from
craftsman-end-motor (a port reconstructed from a retail binary). Profiles are genre-agnostic.

## 5. Requirements

### Functional: the profile

- **R1 File.** `EditorSettings/BuildProfiles/<Name>.buildprofile`, YAML, written with
  `io::writeFileAtomic`:

  ```yaml
  formatVersion: 1
  platform: android                 # windows linux macos android ios web
  productName: "My Game Demo"       # "" inherits
  identifier: com.studio.mygame.demo
  versionSuffix: "-demo"
  icon: art/icon-demo.png           # project-relative image
  store: google-play                # none google-play app-store steam itch web
  storeTitle: "My Game: Demo"
  backend: gles3                    # "" = the platform's default (platformBackends)
  development: false
  outputDir: ../builds/android-demo # "" = Build Settings' folder for the platform
  scenes:                           # absent = the project's Scenes In Build
    - {guid: 6c1f..., path: scenes/Menu.scene}
    - {guid: 0a9e..., path: scenes/Level1.scene}
  startScene: ""                    # guid; "" = the first of scenes
  defines: [DEMO, NO_ADS]
  excludeLabels: [full-only]
  excludePaths: [music/full/, levels/bonus/]
  overrides:
    player.window_title: "My Game Demo"
    android.package_format: apk
  ```

  - The name is the file stem: 1-64 characters of `[A-Za-z0-9 _.-]`, unique without case.
  - Every key is optional except `formatVersion` and `platform`.
  - Keys this version does not know are kept verbatim on save (S03's rule for unknown data).
  - A file with a newer `formatVersion` is listed, read-only, with "made by a newer editor: update to
    edit or build it".
  - An unreadable file is listed with its parse error and cannot be built.
- **R2 Typed fields map to settings keys.** Each typed field writes, in the build's settings snapshot
  (R14), the key the platform's finalizer already reads, so today's chains pick it up unchanged:

  | Field | Android | iOS | macOS | Windows | Linux | Web |
  |---|---|---|---|---|---|---|
  | Product Name | `android.application_name` | `ios.application_name` | `macos.application_name` | `windows.product_name` | `linux.application_name` | `web.application_name` |
  | Identifier | `android.package_name` | `ios.bundle_identifier` | `macos.bundle_identifier` | `player.identifier` | `player.identifier` | `player.identifier` |
  | Icon | `android.launcher_icon` (adaptive pair cleared) | `ios.icon` | `macos.icon` | `player.window_icon` | `player.window_icon` | `web.favicon` and `player.window_icon` |

  - Product Name also sets `player.window_title` when the project's is empty, so a desktop window
    shows the profile's name.
  - Version Suffix is appended to the resolved version where the platform allows text:
    `android.version_name` (which `game.json` also carries) and the web page's version. It is not
    applied to Apple short versions or Windows numeric versions; the build says so once
    (`W-PROFILE-SUFFIX-NUMERIC`).
  - A typed field and an override of the same key in one profile is a validation error naming both
    ("Product Name and the override android.application_name both set the Android label: keep one").
- **R3 Identifier and name validation.** The same validators as the settings keys they map to
  (dotted identifiers per platform, `editor/settings/SettingsRows.cpp:1162-1166`, `:1495`, `:1508`, `:1634`).
- **R4 Scene list.** `scenes` is an ordered list of project scenes by GUID; the path is a hint
  rewritten on save. The build cooks and lists only these scenes plus the scenes they involve
  (child scenes, `collectInvolvedScenesForExport`), which Build Settings shows greyed as
  "(added: involved by Level1)". The start scene is `startScene`, else the first listed. A GUID no
  longer in the project fails the build naming the path hint; an empty list is a validation error.
- **R5 Runtime scene refusal.** Loading a scene the build left out logs
  "Scene 'Bonus' is not in this build (Build Profile 'Demo')". In the editor's Play with an active
  profile the scene still loads, with the warning
  "Scene 'Bonus' is not in Build Profile 'Demo': a build leaves it out".
- **R6 Scripting Define Symbols.** Up to 64 symbols of `[A-Za-z_][A-Za-z0-9_]*`, at most 64
  characters each, unique. A build ships them (`game.json` `defines`, which a Compile build reads
  too); Play with an active profile has them; without a profile the set is empty.
- **R7 Graphics API and Development Build.** `backend` must be one of
  `platformBackends(platform)` (`editor/ShippingTypes.h:39`); `development` selects the flavour as
  the Development Build checkbox does. A profile's values replace Build Settings' for its builds.
- **R8 Output folder.** `outputDir` (absolute or project-relative) follows S30's rules: never inside
  the project, no `;` (`docs/shipping.md:49-52`); a refusal is a validation error with the same text.
- **R9 Active profile.** Per user, in `.jasperengine/user/build.yaml` (`activeBuildProfile`). It
  selects the profile Build Settings opens on, and what Play applies: identity
  (`System.getAppName/Identifier/Version`), defines and the scene warning (R5). Other overrides
  apply to builds only; the Game view toolbar shows "Profile: Demo" with a tooltip saying so. A
  missing or unreadable active profile falls back to none with one warning.

### Functional: content

- **R10 Exclude Labels.** An asset whose S25 labels include one of `excludeLabels` (compared as
  labels are, without case) is not copied, cooked or packed.
- **R11 Exclude Paths.** Project-relative folder prefixes (ending in `/`) or file paths; everything
  under them is left out, Lua included. Paths escaping the project or naming a non-asset folder are
  validation errors.
- **R12 References are checked.** Before cooking, every shipped scene, prefab, material, UI Document
  and Sound Event is checked against the exclusions through the S04 dependency graph; a reference
  to an excluded asset fails the build: "Scene Level1 uses art/full/boss.png, which Build Profile
  'Demo' excludes (label full-only)". A shipped Lua file whose literal `require` names an excluded
  module warns `W-PROFILE-LUA-REQUIRE` (computed requires cannot be seen; S34's caveat). S34's module
  scan reads the filtered Lua set, so an excluded cheat script no longer keeps a module.

### Functional: Player Settings overrides and resolution

- **R13 Allowed keys.** `overrides` keys are `get_project_settings` keys whose category is `player`,
  `graphics` or `quality`, or the profile's platform category (`android`, `ios`, `macos`, `windows`,
  `linux`, `web`). Others (`physics`, `physics2d`, `input`, `scripting`, `debug`, `editor`,
  `tags_and_layers`, `preferences`, another platform's) are refused naming the allowed categories.
  Each value is validated by the key's own setter with `apply=false` against the project; a value
  that becomes invalid later (a deleted icon) fails the build naming the profile and key.
- **R14 One resolution per build.** At the start of a build the editor makes a settings snapshot: a
  settings-only `Project` decoded from `Stream::encodeProject` of the open project (no scenes opened,
  no asset database, no watchers), then applies the typed fields (R2) and `overrides` through the
  settings setters with a `SettingsContext` bound to the snapshot. The exporter reads every Project
  Settings value from the snapshot and scenes and assets from the open project. The open project and
  `project.yaml` never change; a cancelled or failed build leaves nothing behind.
- **R15 No profile is today.** Building a platform with no profile uses the open project itself as
  the snapshot: every file the build writes is byte-for-byte what it writes today (AC2).
- **R16 Store limits.** `editor/publish/StoreLimits.{h,cpp}` holds one table, checked at build
  (warnings) and by `publish prepare` (errors) for a target whose profile names a store:

  | Store | Checked | Limit | Code |
  |---|---|---|---|
  | `google-play` | Store Title | 30 characters | `W-STORE-TITLE-LENGTH` |
  | `app-store` | Store Title | 30 characters | `W-STORE-TITLE-LENGTH` |
  | `google-play`, `app-store` | resolved home-screen label | warn above 12 characters (launchers truncate) | `W-LABEL-TRUNCATED` |
  | `google-play` | identifier | Android application id rules (R3) | error |
  | `app-store` | identifier, version | Apple bundle id rules; numeric short version | error |
  | `steam`, `itch`, `web`, `none` | nothing beyond R3 | | |

  Characters are counted as Unicode code points. The numbers are the stores' published limits as
  known when this spec was written (assumed, not re-checked against the live consoles); the table
  is the single place to update them, each row citing its source in a comment, and
  `StoreLimits.*` tests pin it.
- **R17 Where the profile is recorded.** `game.json` gains `buildProfile` (string) and `defines`
  (list); `content-manifest.json` gains `buildProfile`. Both are omitted without a profile. Every build,
  Compile builds included, applies them through `System::setBuildInfo` (`player::applySettings`).

### Platform & backends

- **R18** Profiles are editor data: every target and backend builds from one the same way. The
  runtime part (R6, R17, R26) is a string and a set read once at start, on every platform including
  the web, with no per-frame work.

### Performance

- **Budgets:**
  - Listing profiles at project open: one directory listing and ≤ 2 ms per profile parse, ≤ 50 ms
    for 50 profiles.
  - Settings snapshot and resolution: ≤ 50 ms for a 2,000-line `project.yaml` (a build takes
    seconds; reported with `perfSlack()`).
  - Exclusion predicate: one prefix-trie walk per path plus a label lookup in the in-memory label
    store; ≤ 5 % on `copyAssets` for 10,000 files.
  - Runtime: `System.isDefined` is one hash-set lookup; a build without a profile ships nothing new.
  - Editor without profiles: 0 added work per frame.

### Editor UX (human)

- **R19 Build Settings.**
  - Under the Platform list, a **Build Profiles** list: each profile with its platform's icon, the
    active one marked "Active", an unreadable one with a warning icon and its error as tooltip.
    **Add Build Profile** asks for a name and a platform. The context menu has Duplicate, Rename,
    Delete (confirms; the file goes to the OS trash where S29's helpers allow) and Switch Profile.
  - Selecting a profile shows its pane:
    - a header "<name> — <platform>" with **Switch Profile** (or "Active");
    - **Identity**: Product Name, Identifier, Version Suffix, Icon with preview; each row shows the
      inherited value greyed when empty;
    - **Store**: Store, Store Title with a live counter ("18 / 30", red above the limit);
    - **Build**: Graphics API, Development Build, Output Folder;
    - **Scene List**: Override Scenes In Build (off = the project's list), the list with drag order,
      start scene, Add Open Scenes;
    - **Scripting Define Symbols**: a list with + and −;
    - **Content**: Exclude Labels (label chips with their colours), Exclude Paths (folder picker);
    - **Player Settings Overrides**: Add Override... opens a search over the allowed keys (S25's
      settings search); each override row shows the project's value greyed, the key's own widget,
      and Remove.
  - Build and Build And Run build the selected profile; selecting a platform builds it without a
    profile (today).
  - Every profile edit is one undoable command (`EditBuildProfileCmd`, project scope, "Edit Build
    Profile 'Demo'"), merged while typing or dragging like Inspector edits; Add, Duplicate, Rename
    and Delete are undoable too. Switch Profile is per user and not undoable (as switching platform).
- **R20 Project Settings.** A row whose key the active profile overrides shows a marker with the
  tooltip "Overridden by Build Profile 'Demo' in builds", which opens the profile.
- **R21 External edits.** A `.buildprofile` changed on disk (a VCS checkout) reloads through the
  editor's file watcher (S05); an open pane with unsaved typing keeps it and says the file changed.

### Agent UX (MCP, CLI, Lua)

- **R22 `edit_build_profile`** (new, group `shipping`; no existing tool owns a file of this kind):
  `{action: list|get|create|duplicate|rename|delete|set|activate, name?, new_name?, platform?,
  values?: {product_name, identifier, version_suffix, icon, store, store_title, backend, development,
  output_dir, scenes, start_scene, defines, exclude_labels, exclude_paths}, overrides?: {key: value
  or null to remove}, dry_run?}`.
  - Every call that changes a file is one undo step (project scope) and returns `files_written`.
  - `activate` takes `name` (or `null` for none) and is not undoable.
  - `get` returns the stored values, `resolved` (per typed field: the value the build will write and
    where it came from: "profile", "android.application_name", "player.application_name", "Project
    Name") and `store_findings`.
  - Validation errors name the fix ("android.package_format is an Android key; this profile builds
    for iOS: use ios.* or player.* keys"). The description says scenes are paths or GUIDs and points
    to `edit_build_scenes` for the project's own list.
- **R23 `build_project`** gains `profile` (string): platform, backend, development and output folder
  come from it; a `platform`, `graphics_api` or `development` argument that differs fails naming the
  profile's value. The result adds `profile` and the store warnings in `warnings[]`.
  **`get_build_info`** adds `profiles[{name, platform, active, readable, error?}]` and
  `active_profile`.
- **R24 `publishing.json`.** A target may name `"profile": "<name>"` (optional, absent = today).
  `publish prepare` then refuses it when the build's `content-manifest.json` `buildProfile` differs,
  when the target's platform, backend or flavour differ from the profile's, or when a store limit
  (R16) fails. Build Settings > Publish shows a Profile column. `schemaVersion` stays 1.
- **R25 CLI.** `jasperengine-editor build --project P --profile <name> [--out D]`: the profile
  supplies platform, backend, flavour and folder (an explicit `--platform` that differs exits 2);
  prints `profile:` with the other `key: value` lines. Listing and editing go through
  `jasperengine-editor call edit_build_profile`.
- **R26 Lua.**

  ```lua
  System.getBuildProfile()   -- "Demo"; "" without a profile (and in Play with none active)
  System.isDefined("DEMO")   -- true when the build's (or the active profile's in Play) defines list it
  System.getDefines()        -- {"DEMO", "NO_ADS"}
  System.getBuildInfo()      -- BuildInfo: {profile, defines, development, contentPatch, contentPatched}
  ```

  `BuildInfo` is declared in the new hand-written fragment
  `engine/core/script/binding/stubs/BuildInfo.lua` (the generator cannot express a returned table);
  build-and-run-on-device.md adds `contentPatch` and `contentPatched` (whichever lands first creates
  the file). The functions are bound in `CoreClassesLua.cpp` beside `getAppName`, so `tools/lua_api`
  extracts them.
- **R27 Guide.** `docs/mcp/guide/shipping.md` gets a "Build Profiles" section (when to make one, the
  allowed override categories, store limits, the `edit_build_profile` → `build_project profile` loop).
  No line in the always-sent guide unless an eval case needs it (`tools/mcp/eval/cases.py`).

### Data & compatibility

- **R28** A project without `EditorSettings/BuildProfiles/` behaves and builds as today (R15). No
  `project.yaml` key changes meaning: no project version bump and no migration.
- **R29** `.buildprofile` is `formatVersion: 1`; a later change of a key's meaning bumps it with a
  migration in the profile codec (profiles are not `project.yaml`, so `editor/StreamMigrations.cpp`
  gains a `DocumentKind::BuildProfile` table only when that first happens).
- **R30** `game.json` `buildProfile`/`defines` and `content-manifest.json` `buildProfile` are a new
  content minor (`FORMAT_MINOR` 5 → 6 in `engine/core/serialization/CookedScene.h:42`): a player of
  minor 5 would ignore `defines` and run a demo as the full game, so the editor refuses such a runtime
  under S30's existing rule (`docs/shipping.md:129-136`).
- **R31** `EditorSettings/` is already a non-asset folder (`editor/Project.cpp:5989-5991`): profiles
  never ship and are never imported. `.jasperengine/user/build.yaml` is per user and not committed.

## 6. Design constraints & decisions

- **D1 A profile is overrides, not a copy.** Every field defaults to inheriting; only what a variant
  changes is stored, so a profile stays valid as the project's settings evolve.
- **D2 Resolve in a snapshot, never in place.** Applying overrides to the open project and restoring
  them was rejected: an autosave, a crash or a settings window during the build would persist or show
  them. The snapshot (R14) is read-only to the exporter.
- **D3 Typed fields are sugar over settings keys** (R2), so the finalizers keep their chains and a
  profile with no fields is identical to no profile. The identity inconsistency between finalizers is
  not fixed here (Risks).
- **D4 Files in `EditorSettings/BuildProfiles/`, one per profile.** One file per profile merges
  cleanly in version control and maps to Unity's one asset per profile; the folder never ships.
- **D5 API sketch.**

  ```cpp
  // editor/build/BuildProfile.h
  namespace jasperengine::editor {
      struct BuildProfile {
          std::string name;                      // the file stem
          BuildPlatform platform = BuildPlatform::Linux;
          std::string productName, identifier, versionSuffix, store, storeTitle, backend;
          std::filesystem::path icon, outputDir;
          std::optional<bool> development;
          std::optional<std::vector<SceneRef>> scenes;   // absent: Scenes In Build
          std::string startSceneGuid;
          std::vector<std::string> defines, excludeLabels, excludePaths;
          std::map<std::string, mcp::Json> overrides;    // settings key -> value
          YAML::Node unknown;                            // kept verbatim
      };
      struct BuildProfileFile {   // read/write EditorSettings/BuildProfiles/<name>.buildprofile
          static bool read(const std::filesystem::path& file, BuildProfile& out, std::string& error);
          static io::WriteResult write(const std::filesystem::path& file, const BuildProfile& profile);
      };
      // validation against the open project: every finding, each naming its fix
      std::vector<std::string> validateProfile(const Project& project, const BuildProfile& profile);
      // the settings snapshot of a build (R14); the project itself when profile is null (R15)
      std::shared_ptr<const Project> resolveBuildSettings(const Project& project, const BuildProfile* profile,
                                                          std::vector<BuildWarning>& warnings, std::string& error);
      // the content filter of copyAssets, copyLua, the cooks and the pack (R10-R11)
      class BuildContentFilter {
      public:
          BuildContentFilter(const BuildProfile* profile, const AssetLabels& labels);
          bool excludes(std::string_view projectRelativePath, std::string* why = nullptr) const;
      };
  }
  // ExportConfig gains: std::string buildProfile; (empty: none)
  ```

  Runtime: `PlayerConfig` gains `buildProfile` and `defines`; `System::setBuildInfo(name, defines)`
  stores them once; `System::isDefined` looks them up.
- **D6 Validation shared by every surface.** Build Settings rows, `edit_build_profile`, the CLI and the
  build call `validateProfile`; the build fails on any finding, the UI shows them inline.
- **D7 Naming.** Build Profile, Switch Profile, Scene List, Scripting Define Symbols, Player Settings
  Overrides: Unity's. "Build Profile" is distinct from S56's "Device Profile" (`deviceProfile` in
  `publishing.json`); the docs say so once.

## 7. Acceptance criteria

- **AC1 Codec.** `editor/tests/build/BuildProfileTest.cpp`: `BuildProfileFile.RoundTripsEveryField`,
  `BuildProfileFile.KeepsUnknownKeys`, `BuildProfileFile.RefusesANewerFormatAsReadOnly`,
  `BuildProfileFile.NameRules`. Fail without: no codec.
- **AC2 No profile, no change.** `editor/tests/build/BuildProfileTest.cpp`
  `BuildProfileResolve.WithoutAProfileTheSnapshotIsTheProject` (pointer identity), and MCP
  `tools/mcp/tests/test_build_profiles.py::test_build_without_profile_is_unchanged` (`CI = True`,
  `PREBUILT_PLAYERS = ["glcore-release"]`): a Linux prebuilt build of the fixture writes `game.json`
  and `content-manifest.json` equal, key for key (paths and hashes masked), to
  `tools/mcp/tests/fixtures/build_profiles/expected-no-profile.json`, recorded from main before the change.
  Fail: any key added without a profile.
- **AC3 Typed fields land where each platform reads them.**
  `BuildProfileResolve.TypedFieldsWriteThePlatformKeys` (a table over the six platforms: Product Name,
  Identifier, Icon, Version Suffix, window title) and `BuildProfileResolve.SuffixSkipsNumericVersions`;
  MCP `test_build_profiles.py::test_profile_identity_reaches_the_build`: profile "Demo" (Linux,
  Product Name "Box Demo", define `DEMO`) → `game.json` `title` "Box Demo", `buildProfile` "Demo",
  `defines` ["DEMO"]; the headless player run logs `System.getBuildProfile()` and
  `System.isDefined("DEMO")` from a script. Fail without: the old title, no keys.
- **AC4 Conflicts and allowed keys.** `BuildProfileValidate.RefusesATypedFieldAndItsOverride`,
  `BuildProfileValidate.RefusesKeysOfOtherCategories` (`physics.gravity`, `ios.icon` in an Android
  profile), `BuildProfileValidate.UsesTheSettingsOwnValidator` (`player.window_width: "abc"` gives the
  setter's message). Fail without: no validation.
- **AC5 Scenes.** `test_build_profiles.py::test_profile_scene_list`: a profile listing one of three
  scenes builds a `game.json` with that scene and its involved child scene only, starts on it, and a
  script loading a left-out scene logs "is not in this build (Build Profile". In Play with the
  profile active the same load warns and succeeds. Fail without: every scene ships.
- **AC6 Content exclusions.** `editor/tests/build/BuildContentFilterTest.cpp`:
  `BuildContentFilter.ExcludesByLabelAndPath`, `BuildContentFilter.RejectsEscapingPaths`;
  `test_build_profiles.py::test_excluded_asset_referenced_by_a_scene_fails`: a texture labelled
  `full-only` used by a shipped scene fails the build with the scene and the asset named; unused, it
  is absent from `content.jpak`. Fail without: the build succeeds and ships it.
- **AC7 Store limits.** `editor/tests/publish/StoreLimitsTest.cpp`:
  `StoreLimits.GooglePlayAndAppStoreTitlesAre30CodePoints`, `StoreLimits.LabelTruncationWarnsAbove12`,
  `StoreLimits.CountsCodePointsNotBytes`; `editor/tests/publish/PublishingTest.cpp`
  `Publishing.PrepareRefusesATargetWhoseProfileBreaksItsStore` and
  `Publishing.PrepareRefusesABuildOfAnotherProfile`. Fail without: no table, no refusal.
- **AC8 MCP tool.** `test_build_profiles.py::test_edit_build_profile`: the tool is hidden until
  `tool_groups` enables `shipping`; create, set, duplicate, rename, delete each write one file and
  each is undone by one `undo`; `activate` changes `get_build_info active_profile` and is not on the
  undo history; `get` reports `resolved` sources. Fail without: unknown tool.
- **AC9 CLI and build_project.** `test_build_profiles.py::test_build_project_profile_conflicts`:
  `build_project profile "Demo" platform "web"` fails naming the profile's platform;
  `jasperengine-editor build --profile Demo` prints `profile: Demo`.
- **AC10 Snapshot isolation.** `BuildProfileResolve.TheOpenProjectNeverChanges`: resolving with every
  kind of override leaves `encodeProject` of the open project byte-identical; plus the timing of a
  2,000-line project reported with `perfSlack()`.
- **AC11 Play.** `test_build_profiles.py::test_active_profile_in_play`: with "Demo" active,
  `eval_lua` in Play returns `System.getAppName()` "Box Demo" and `System.isDefined("DEMO")` true;
  after `activate null`, the project's name and false.
- **AC12 Content format.** `engine/tests/core/player/ContentManifestTest.cpp` (extended)
  `ContentManifest.BuildProfileRoundTrips` and the existing reader-compatibility test updated for
  minor 6 (a deliberate change, said so in its commit).
- **AC13 Old projects.** `editor/tests/build/BuildProfileTest.cpp`
  `BuildProfileStore.AProjectWithoutTheFolderHasNoProfiles` and
  `BuildProfileStore.AnUnreadableProfileDoesNotStopTheProject` (a broken file is listed with its
  error; the project opens and builds its platforms).
- **AC14 Editor UX.** `test_build_profiles.py::test_build_settings_profile_pane` drives the pane through
  `EditorModal` (`find_ui_item`): Add Build Profile, set Store Title over 30 (the counter turns red and
  the warning appears), Switch Profile. Screenshots from a scratch copy of a fixture project (the
  profile list, the profile pane, the Project Settings marker) ship with the PR.
- **AC15 Docs, Lua and budget.** `docs/shipping.md` gets "Build Profiles"; the guide section of R27;
  `docs/mcp/TOOLS.md` regenerated; `tools/lua_api` builds with the new fragment and its doctest knows
  `System.getBuildInfo`; `tool_budget.py --check tools/mcp/budget.json` passes for `shipping` and core.

## 8. Dependencies & conflicts

- **Depends on** S30 (exporter, `game.json`, publishing), S25 (asset labels, settings search), S04
  (scene GUIDs, dependency graph), S05 (file watcher for R21), S34 (module scan reads the filtered Lua
  set), S03 (atomic writes, unknown-key preservation).
- **build-and-run-on-device.md**: Build And Run builds the selected profile; `System.getBuildInfo` and
  `stubs/BuildInfo.lua` are shared (R26).
- **build-size-tools.md**: should report sizes per profile and show what Exclude Labels/Paths removed;
  it reuses `BuildContentFilter`.
- **game-ui-localization.md**: languages a variant ships can be limited through an override key it
  defines in the `player` category; no special field here.
- **savegame.md**: owns any separate save folder per variant (section 2).
- **project-test-runner.md**: may run tests under a profile's defines through `activate`.
- **Merge hotspots**: `editor/Exporter.cpp` and `editor/ExporterPrebuilt.cpp` (every settings read
  moves to the snapshot; S53, S68 and build-size-tools.md touch the same files),
  `editor/Generator.cpp` (the init's build info), `editor/window/dialog/ExportWindow.cpp`
  (build-and-run-on-device.md edits the same footer and rows), `editor/mcp/tools/BuildTools.cpp`,
  `editor/publish/Publishing.{h,cpp}`, `engine/core/player/PlayerConfig.*`,
  `engine/core/serialization/CookedScene.h` (content minor).

## 9. Risks & defaults

- **The identity finding** (section 3): macOS, iOS, Windows and `game.json` ignore the shared Player
  identity that Play shows. Default: not changed here, because fixing it changes what old projects
  build (a macOS bundle id would move from `com.jasperengine.<app>` to `com.yourcompany.project`).
  `edit_build_profile get` and the profile pane show the resolved value and its source, so the
  difference is visible. A follow-up may unify the chains with a project version bump and a migration
  that writes today's effective values as explicit overrides.
- **A settings-only `Project` may be costly to construct** (inferred: `Project` is large). Default:
  measure in AC10; if over budget, the snapshot becomes a `BuildSettingsView` struct holding the
  settings groups the exporter reads, filled the same way. The requirement (R14) does not change.
- **Defines are not secrets.** Code behind `System.isDefined` ships. Default: the docs say to put
  code that must not ship under an Exclude Path.
- **Store limits change.** Default: one table with sources, warnings at build, errors only at
  `publish prepare`, where a person is about to release.
- **Profile renamed while `publishing.json` names it.** Default: Rename updates `publishing.json`
  targets naming the old name in the same undo step and lists them in `files_written`.
- **Two people activate different profiles.** Intended: the active profile is per user.

## Deviations

- **The snapshot (R14) is a `Project` in a settings-snapshot mode** (`Project::makeSettingsSnapshot`):
  decoding a second `Project` ran setters that apply to engine globals (shadow tiers, audio,
  threading, the window title...) and `resetConfigs` closes documents and clears pools. The snapshot
  constructor sets a flag those setters and `resetConfigs` check, decodes `encodeProject` without
  `scenes`, and takes the source's tags, layers and physics world (no profile overrides them). A
  setter added later with a global effect needs the same guard.
- **A Scene List names scenes of the project's Scenes In Build**: the exporter loads, saves and cooks
  the project's scene list; a scene asset outside it is refused by validation ("add it there
  (edit_build_scenes)"). All project prefabs still ship (the prefab cook is not per scene).
- **Compile builds** read the same `game.json` as a prebuilt player (S98 D1, `player::runGame`): the
  profile's snapshot gives its settings, its Scene List's scenes and their involved scenes are the
  ones cooked and listed, and its start scene is game.json's (`cook::GameConfigTarget::settings` and
  `scenes`). Their main.cpp is the same for every build.
- **Keys a profile does not override**: `player.product_name` (executable and save folder, section 2),
  `player.start_scene` and `player.target_platforms` (the profile's Scene List and platform),
  `player.development_build` (`development`). The refusals name the field to use.
- **publish prepare (R24)** reads the build's profile from `content-manifest.json`, else from
  `build-report.json`, which gains `buildProfile` (a mobile build packs its manifest). The store
  check uses the profile's values and a loose `game.json` (label, identifier, version); what a packed
  build does not show is not checked there. `W-LABEL-TRUNCATED` stays a warning at prepare.
- **edit_build_profile** refuses a write with findings (each naming its fix; `dry_run` reports them);
  the Build Settings pane writes each edit and shows the findings inline (the build refuses them).
  It is registered with the publish tools (module "Shipping: player runtimes and releases").
- **Build Settings pane**: no icon preview; an override row draws a widget by the row's type (bool,
  enum, number, else text or JSON) rather than the key's own widget; Exclude Paths are typed (no
  folder picker); Delete confirms inline and does not use the OS trash (undo restores the file);
  the Project Settings marker's tooltip names the profile but does not open it.
- **External edits (R21)** are seen by the editor windows' view of the profiles, which compares the
  folder's and `build.yaml`'s file stamps at most once a second while a window asks
  (`buildprofiles::view`), not through the S05 watcher (which leaves `EditorSettings/` out).
- **Paths**: the code is in `editor/buildprofile/` and its tests in `editor/tests/buildprofile/`, not
  `editor/build/` and `editor/tests/build/`: `.gitignore` ignores every `build` folder.
- **The snapshot's budget (50 ms for 2,000 lines)**: measured at 186 ms on box A; of it encodeProject
  11 ms, a snapshot of an empty project 0.5 ms, validation 0.1 ms, so the decode of the 2,000 label
  colours took ~175 ms: `AssetLabels::setEntries` scanned the palette per entry, lower-casing both names
  on every comparison (quadratic). It now keeps a set of lower-case names (linear); the project's own
  load gains the same. Re-measured on box A after the fix: 18.0 ms (encodeProject 11.7 ms,
  makeSettingsSnapshot 14.2 ms), within the budget.
- **AC4's physics key** is `physics.max_bodies`: the project has no `physics.gravity` setting.
- **AC2's fixture** (`expected-no-profile.json`) holds the key paths of `game.json` and
  `content-manifest.json` (values masked) recorded from this branch's build without a profile, not
  from main: no main editor was built here. The writers add the new keys only with a profile.
- **R30, the content minor, per build.** The cooked files keep their own layout (`FORMAT_MINOR`, 10
  when this landed): `game.json`'s profile and defines are the only thing content 1.11 added, so the
  minor is stamped per build (`cook::buildContentFormatVersion`). A build without a profile writes the
  layout's minor and every player runtime of that layout still serves it; a profile's build writes the
  newer of the layout's minor and `BUILD_PROFILE_MINOR` (11) and refuses an older runtime even under
  `--allow-player-mismatch`, which would have run it with the defines ignored. A later layout bump
  raises `CONTENT_MINOR` with it (a static assert holds it to the newest). A
  runtime reads up to `CONTENT_MINOR` (its player-info, `tools/players/package_player.py`). Found in
  the integration review: the single minor made the editor refuse every 1.5 runtime for every project.
  Tests: `PlayerInstall.APlayerOfTheContentBeforeBuildProfilesServesABuildWithoutOne` (red before),
  `ContentManifest.ReadsContentFormat`; `CookedScene.ReadsAnOlderMinorsContent`.
- **A build's settings snapshot writes nothing.** Settings rows apply some overrides through their
  undoable commands (Device Tiers, Screen Transitions, Saved Data, Draw Submission, ...), whose
  `execute()` saved `project.yaml` (without its scene list) and `workspace.yaml` through the snapshot,
  which keeps the project's path; Saved Data also reconfigured the editor's own store. The snapshot's
  saves are no-ops and its engine calls are guarded.
  `BuildProfileResolve.EveryOverridableKeyLeavesTheProjectFilesAlone` applies every overridable key and
  checks the project folder byte for byte (red before).
- **Undo of a rename.** Undo runs a step's files in reverse order (a rename to another letter case is
  one file on Windows and macOS), and the active profile's rename is part of the step
  (`EditBuildProfileCmd::setActiveNameChange`): undo makes the old name active again.
