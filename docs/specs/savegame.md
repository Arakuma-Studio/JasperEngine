# Save Games: slots, namespaces, versioned migrations, backup generations and corruption recovery

Spec-first. Builds on S03 (atomic writes), S06 (jobs), S24 (one undo timeline) and S30 (player
configuration). Paths are relative to the repository root unless they start with
`craftsman-end-motor/`, which means `/var/lib/orquester/workspaces/jaspersito/craftsman-end-motor/`.
Every `file:line` of section 3 was read for this spec; nothing was built or run, so budgets are
targets the implementer measures, and behaviour no test covers yet is marked *(inferred)*.

## 1. Goal

A game needs to keep a player's progress safely: several save slots, data written by different
systems that do not trample each other, old saves that still load after the game changes its data
layout, and a crash, a full disk or a torn write that costs at most the last save, never the whole
file. Today the engine offers a flat key/value store (UserSettings/PlayerPrefs) and raw file
helpers, so every game rebuilds this badly. **SaveGame** gives Lua and C++ one store of named
slots holding Lua tables, written off the main thread with write coalescing, stamped with a
data version and upgraded by game-supplied migrations, kept in several checksummed generations
and recovered from the newest valid one; humans get a **Saved Data** window and agents get the
same operations through the existing `user_settings` tool.

## 2. Scope

**In scope**
- Lua module `SaveGame` and C++ `jasperengine::save` (store, file format, value codec).
- Slots (`slot1`, `autosave`...), namespaces inside a slot (`main`, `inventory`, `quests`...),
  per-slot metadata for save menus (`meta`) and an optional content manifest (`requires`).
- A binary file format with magic, format version, length caps and an XXH3-64 checksum footer; a
  value codec that round-trips Lua tables with integer and string keys, integers versus floats,
  binary strings, `Vector2/3/4`, `Quaternion` and `Json.null`.
- Data versions and migrations (`SaveGame.version`, `SaveGame.addMigration`); a save of a newer
  version or format is reported and never overwritten by accident.
- Backup generations (current + 2 by default), recovery from the newest valid one, a corrupt
  generation set aside, Restore Backup.
- Asynchronous writes on a serial job queue with a main-thread completion callback, coalescing,
  synchronous flush on app pause and shutdown, size caps.
- Platforms: desktop, Android, iOS, the web (IndexedDB through the existing `syncFileSystem`,
  quota errors reported to the game).
- Editor: Saved Data window (slots and PlayerPrefs), Project Settings rows, undoable Delete,
  Restore Backup and Corrupt (for testing).
- MCP: a `store` parameter and three actions on `user_settings`; CLI through `jasperengine-editor call`.
- Docs: `docs/saved-data.md`, `docs/mcp/guide/saved-data.md`, Lua stub, `lua-api.md` lines.

**Out of scope (decisions)**
- Automatic scene or GameObject snapshots ("save the world"). Games choose what to save; the
  runtime even drops components it does not know from cooked content
  (`engine/core/serialization/RuntimeFields.cpp:97-104`), so an engine-defined world snapshot would
  need its own versioning story. A later spec may build one on top of this store.
- Cloud saves (Steam Cloud, iCloud Drive, Google Play Saved Games, Unity Cloud Save-like services).
  The folder layout (D9) is chosen so a platform's file sync can be pointed at it later.
- Encryption, signing or anti-tamper. The checksum detects damage, not edits (documented).
- Compression: players link only `zstd_decompress` (`engine/CMakeLists.txt:1306`, T6 R79 made the
  same call), so saves are uncompressed; the size cap bounds them.
- Thumbnails/screenshots in slots: a game stores a PNG through `File.writeText` and names it in
  `meta`. A `Texture`-in-save API is a later addition.
- Changing UserSettings/PlayerPrefs (stays the settings store) and T6's `saveChanges`
  (`docs/specs/T6-voxel-volumes.md:713`, its own delta format).
- A save folder per build variant (`build-profiles.md` leaves it here): a demo and its full game
  share `data://saves/` on purpose, so progress carries over; a game that wants them apart prefixes
  its slot names (`demo-slot1`).
- Multi-process access to one save folder (two copies of a desktop game at once): last writer wins
  per generation; the newest valid generation is read. No file lock.

## 3. Current state

**PlayerPrefs store.** `UserSettingsStore` is the desktop/editor PlayerPrefs store: one file
`data://UserSettings.xml` read once and kept in memory, written one second after the oldest
unsaved change (`WRITE_DELAY`), or at once by `save()` on `UserSettings.save`, app pause, shutdown
and the editor's Play stop (`engine/core/io/UserSettingsStore.h:19-35,38`). The write is
`FileUtils::writeFileAtomic` (`engine/core/io/UserSettingsStore.cpp:159`) followed by
`System::syncFileSystem` (`:170`); the header's "UserSettings.xml.tmp renamed over it" is stale.
Android (SharedPreferences), Apple (NSUserDefaults) and the web (localStorage) have their own
stores (`UserSettingsStore.h:19-21`). Values are flat strings keyed by name: no slots, no grouping,
no version, no checksum, no backup.

**Engine hooks.** `Engine::runFrame` ticks the write-behind (`engine/core/Engine.cpp:2823`); the
pause path saves after `onPause` (`:3055`, "a paused app may be killed without another call"),
shutdown saves twice (`:2953` after `onShutdown`, `:2999` after scenes are gone). The editor saves at
Play stop (`editor/Project.cpp:11723`).

**Raw file helpers.** `FileUtils` gives Lua `File.readText/writeText/appendText/exists/remove/
makeDirs/list` and `Json.readFile/writeFile` (`engine/core/script/binding/IOClassesLua.cpp:471-478,
579-580`). Paths: `data://` is the user data folder, a bare relative path means `data://`, `..` and
unknown schemes are refused (`engine/core/io/FileUtils.h:13-24`). `writeText` is atomic and then
calls `syncFileSystem` (`FileUtils.cpp:308-323`). A game that saves today hand-writes JSON with
these: no versioning, no generations, a synchronous write on the main thread.

**JSON is not a save format.** `LuaJson` writes `Vector2/3/4` and `Quaternion` as plain objects
`{x, y[, z[, w]]}` and reads objects back as plain tables (`engine/core/script/LuaJson.h:19-29`), so
a vector does not round-trip; a Lua table `{[1]="a", [5]="b"}` becomes an object with keys `"1"`
and `"5"`, so integer keys come back as strings.

**User data folders.** Desktop exports: `%APPDATA%\<appId>` (`engine/platform/win/SystemWin.cpp:205-219`)
and `$XDG_DATA_HOME/<appId>` or `~/.local/share/<appId>` (`engine/platform/linux/SystemLinux.cpp:134-150`);
Android: internal storage (`engine/platform/android/JasperEngineAndroid.cpp:218-220`); Apple:
`Documents` (`engine/platform/apple/JasperEngineApple.mm:277-282`, backed up by iOS); the base
`System` falls back to the asset path (`engine/core/System.cpp:327-329`). The editor's Play uses
`.jasperengine/user/rundata` so playtests do not dirty the working tree (`editor/Project.cpp:9831-9843`).

**Web persistence.** `/datafs` is an IDBFS mount restored before the project starts
(`engine/platform/emscripten/JasperEngineWeb.cpp:526-535`); `syncFileSystem` schedules an IndexedDB
sync 500 ms later and `flushFileSystem` syncs now (`:753-773`); a hidden tab syncs at once
(`:625-628`). A failed sync is only logged: `syncfs_callback` prints "Failed to save in iDB file
system" and nothing reaches the game (`:124-128`), so a quota error is invisible to scripts. The
IDBFS database is per origin and mount path *(inferred from Emscripten's IDBFS)*, so games on one
origin share `/datafs`; localStorage keys are prefixed with `storageId` for that reason
(`:1344-1353`), `data://` files are not.

**Threads.** `jobs::SerialQueue` runs FIFO jobs on pool workers, one at a time
(`engine/core/jobs/SerialQueue.h:28-40`); `jobs::postToMain` queues main-thread work run at the start
of each frame (`engine/core/jobs/JobSystem.h:179-180`). The web build is single-threaded unless
`EMSCRIPTEN_THREAD_SUPPORT` is on (`engine/CMakeLists.txt:398`), so there jobs run on the main thread
*(inferred)*. `AsyncOwner` ends asynchronous work silently when its script, GameObject or scene goes
(`engine/core/script/AsyncOwner.h:22-28`).

**Hashing.** `hashContent64` (XXH3, `engine/core/util/ContentHash.h:27-30`) is in every game.

**Editor and MCP.** The `user_settings` tool lists, gets, sets, deletes and clears PlayerPrefs keys
in the store the running game shares (`editor/mcp/tools/DataTools.cpp:4-7,170-186`), module "Saved
data (PlayerPrefs)", group `scripting` (`editor/mcp/tools/McpTools.cpp:40`). Edit > Clear User Settings
(PlayerPrefs)... deletes them (`editor/shortcuts/DefaultCommands.cpp:133-134`). There is no window
that shows saved data. `grep -rn SaveGame engine editor` finds nothing (verified).

## 4. Reference

**Craftsman engine (studied, ideas only).** `craftsman-end-motor` is a port of Minecraft PE 0.15
reconstructed from Mojang's retail binary. Ideas only: no code, data, assets or Minecraft-specific
mechanics may be copied, and nothing here is about worlds, chunks or players of that game.

| Pattern | Where | What we take |
|---|---|---|
| Separate per-feature data store: id -> namespace -> key -> value, durable before return, opened lazily, an empty value deletes | `include/mcpe015/player_data.h:3-60` | Namespaces so systems cannot collide (R3); zero cost until first use (R30); a store that is not the world data |
| One file rewritten per change measured 484, 559 and 846 ms hitches (2.7 MB rebuilt and fsynced on the main thread); fixed by writing only the changed part | `include/mcpe015/minigame_stash.h:9-30` | Namespace sections re-encoded alone, the file written on a worker, coalescing (R10-R13, R27) |
| Binary file with magic, per-section checksum, length caps checked before use, "newer version" reported and left alone | `src/clips_format.cpp:4-9,34-41,598,682` | Header caps (R19), checksum footer (R18), `newer_version`/`newer_format` never overwritten (R8) |
| Torn tail trimmed to whole records on open | `src/blocklog_store.h:9-14` | A torn generation is skipped, the previous one read (R15) |
| Backup journal written before a destructive repair | `include/mcpe015/limpiadupe.h:37-40` | Undo keeps the bytes before Delete/Restore/Corrupt (R36); a corrupt generation is set aside, not deleted (R16) |

**Unity (names and behaviour only).** `PlayerPrefs` and `Application.persistentDataPath`; Unity has
no built-in slot system (games use `JsonUtility` or assets). Unreal Engine's `USaveGame` with
`UGameplayStatics::SaveGameToSlot`, `LoadGameFromSlot`, `DoesSaveGameExist`, `DeleteGameInSlot` and
`AsyncSaveGameToSlot` is the closest named equivalent: our module name `SaveGame` and the slot
vocabulary follow it. No code from either.

## 5. Requirements

### Functional: slots and data

- **R1 Slots.** A slot name is 1-48 characters of `a-z`, `0-9`, `_`, `-` (lower case only, so a slot
  is one file on case-insensitive volumes). Another name fails every call with code `name`:
  *Slot names use a-z, 0-9, '_' and '-' (got "Slot 1"); try "slot-1".* (the suggestion lower-cases
  and replaces other characters with `-`).
- **R2 Values.** A namespace holds one Lua value, normally a table. Saved: booleans, integers
  (int64), floats (IEEE double, bit-exact; NaN and infinities too), strings (binary-safe), tables
  with integer or string keys, `Vector2`, `Vector3`, `Vector4`, `Quaternion` (float32 components, as
  the engine stores them) and `Json.null`. Refused with code `encode` and the path
  (*cannot save a function at $.player.onHit*, *cannot save a boolean key at $.flags*): functions,
  threads, other userdata, keys that are not integers or strings, cycles (*cycle at $.a.parent*),
  nesting deeper than 64. A table reached twice is saved twice (documented). Metatables are not
  saved; class instances come back as plain tables.
- **R3 Namespaces.** A slot holds 1-64 namespaces named like slots (1-32 characters). `write` without
  a namespace writes `main`. Writing a namespace replaces only that namespace; `replaceAll = true`
  drops every other one. `read` returns one namespace (default `main`) and lists the others in
  `info.namespaces`.
- **R4 Snapshot on call.** `write` encodes the value before it returns: later changes to the table do
  not reach the save. A `read` after a `write` returns what was written, even before it reached the
  disk (read-your-writes), and a `read` never blocks on a write in flight.
- **R5 Metadata.** `write(..., {meta = t})` stores `t` (a table, at most 4 KiB encoded) as the slot's
  metadata (title, play time, level name...), returned by `list()`/`info()` without decoding any
  namespace; `meta = nil` keeps the previous metadata. `requires = {name = integer, ...}` records what
  content the save needs (pack names to minimum versions); the engine stores and returns it and
  never interprets it (content packs are a later spec).
- **R6 List, info, exists, delete.** `list()` returns every slot with a readable header, newest first:
  `slot, version, savedAt` (UTC seconds, float), `size` (bytes of the current generation),
  `generations` (valid ones), `namespaces`, `meta`, `requires`, `status` (`ok`, `recovered`,
  `corrupt`, `newer_version`, `newer_format`). It reads headers only (R26). `exists(slot)` is true when
  any generation file is there. `delete(slot)` removes every generation and set-aside file of the slot.

### Functional: versions and migrations

- **R7 Data version.** `SaveGame.version` (integer >= 1, default 1) is stamped into every write.
  `SaveGame.addMigration(fromVersion, fn)` registers `fn(namespaces, info) -> namespaces`, which
  upgrades a slot from `fromVersion` to `fromVersion + 1`; `namespaces` is a table of every namespace
  of the slot (name -> value). Reading a slot of version `v < SaveGame.version` runs the migrations
  `v, v+1, ...` in order on the decoded data and returns the result with `info.migratedFrom = v`.
  A missing step fails with code `migration`: *slot "slot1" is version 2 and SaveGame.version is 4,
  but no migration from 2 is registered (SaveGame.addMigration(2, fn))*. A migration that raises or
  returns no table fails with code `migration` and its message. Either failure leaves the files
  untouched.
- **R8 Newer data is never lost.** A slot whose newest valid generation has a version above
  `SaveGame.version` reads as code `newer_version`; a file whose format version is above this
  engine's reads as `newer_format`. Neither is changed: `write` to such a slot fails with the same
  code unless `force = true`, and recovery never skips past them to an older generation.
- **R9 Migrated data is written lazily.** A migration's result stays in memory; the files keep the old
  version until the next `write` of any namespace of that slot, which writes every namespace at the
  current version (the S03 rule: a migrated document is written in the current shape by its next save).

### Functional: writing

- **R10 Asynchronous write.** `write` returns `true` once the value is encoded and queued, or
  `false, err` at once for `name`, `encode`, `too_large` and `newer_version`. The file is written on a
  per-store `jobs::SerialQueue` (Background lane) and `onComplete(ok, err)` runs on the main thread
  in a later frame. With `owner` (as `Http`: a script `self`, a GameObject or a scene), a gone owner's
  callback is dropped silently; the write still happens.
- **R11 Coalescing.** Per slot at most one write is in flight and at most one waits. A write while one
  is in flight replaces the waiting snapshot; the waiting snapshot is written when the in-flight one
  lands. Every `onComplete` of the writes a landed snapshot contains fires with that snapshot's
  result, in call order. So 60 writes in a second to one slot produce at most 2 file writes in
  flight-or-queued at any time and no unbounded queue.
- **R12 Namespace cost.** A write re-encodes only its namespace; the slot's other sections are kept
  encoded in memory and shared with the writer by reference (copy-on-write), so the main-thread cost
  of a write is proportional to the namespace written, not to the slot (R27).
- **R13 Flush.** `SaveGame.flush([slot])` blocks until every queued write (of that slot) has landed or
  failed and returns `ok, err`. The engine flushes every slot on app pause (after `onPause`, before
  `UserSettings::save`), at shutdown (after `onShutdown`) and at the editor's Play stop, with a 2 s
  limit; past it the remaining writes are logged as abandoned (*SaveGame: "slot1" was not written
  before the app paused (2 s)*).
- **R14 Size cap.** A slot's encoded file may not exceed Project Settings > Player > Saved Data > Max
  Slot Size (MiB) (default 8, range 1-64). A write that would exceed it fails at once with code
  `too_large`, naming the size, the cap and the three largest namespaces.

### Functional: generations and recovery

- **R15 Generations.** Every landed write is a new file `<slot>.g<N>.jsave` with `N` one above the
  highest generation of the slot; then generations beyond Backups Kept + 1 (Backups Kept default 2,
  range 0-9) are deleted, oldest first. Each file is written through `io::writeFileAtomic` with
  `Durability::Document`, so a crash at any point leaves every earlier generation intact and the new
  one complete or absent. A read tries generations newest first and uses the first one that parses,
  checks and decodes.
- **R16 Recovery.** When the newest generation is invalid (short file, bad magic, a length beyond the
  file, a checksum mismatch, a decode error), the read uses the next valid one and sets
  `info.recoveredFrom = N` (the generation used) and `info.problems` (one string per skipped file:
  *slot1.g41.jsave: checksum mismatch*); one warning per slot per session is logged. The invalid file
  is renamed to `<name>.corrupt` (only the newest set-aside file per slot is kept). With no valid
  generation the read returns `nil` with code `corrupt` and leaves every file in place.
- **R17 Restore Backup.** `SaveGame.getBackups(slot)` lists every generation (`generation, savedAt,
  version, size, valid`). `SaveGame.restoreBackup(slot, generation [, onComplete])` writes that
  generation's sections, metadata and version as a new newest generation (the chosen file stays), so a
  restore can itself be undone by restoring the previous newest.

### Functional: file format and codec

- **R18 File.** Little-endian. Header: magic `JSAV\r\n\x1A\n` (8 bytes; a text-mode copy or a
  truncated download breaks it), format version `u16` (1), flags `u16` (0), header size `u32`,
  generation `u64`, saved-at `i64` (Unix milliseconds, UTC), data version `u32`, section count `u32`,
  metadata size `u32`, metadata (a JLV1 table `{meta, requires, engine = "<version>", platform}`).
  Sections: name length `u16`, name, payload length `u32`, payload (one JLV1 value). Footer: XXH3-64
  (seed 0) of every byte before it, then `JSAVEND\n`. Readers check the footer before parsing.
- **R19 Caps before allocation.** A reader refuses, before allocating: files above 64 MiB, header size
  outside 44..4140, more than 64 sections, names above 32 bytes, metadata above 4 KiB, any length
  beyond the bytes left, a varint longer than 10 bytes, a count above the bytes left, nesting above 64.
  A failure names the offset (*bad section length 4294967295 at offset 812*).
- **R20 JLV1 values.** One tag byte: `0x00` false, `0x01` true, `0x02` integer (zigzag LEB128),
  `0x03` float (8 bytes), `0x04` string (varint length, bytes), `0x05` table (varint `n`, the values of
  keys `1..n`; varint `m`, then `m` key/value pairs with integer keys ascending, then string keys
  in byte order), `0x06` Vector2, `0x07` Vector3, `0x08` Vector4, `0x09` Quaternion (float32 each),
  `0x0A` `Json.null`. The array part is the longest run `1..n` of non-nil values. An unknown tag is a
  decode error; a new tag needs format version 2. Equal values encode to equal bytes.
- **R21 JSON view (tools only).** `save::toJson` / `fromJson` are an exact bijection for the editor and
  MCP: scalars as JSON (floats keep a decimal point; NaN and infinities as `{"$float": "nan"|"inf"|"-inf"}`;
  non-UTF-8 strings as `{"$bytes": "<base64>"}`); a table whose keys are exactly `1..n` as an array, any
  other table as an object (an empty table is `{}`), integer keys as `"#<n>"`, string keys starting
  with `#` or `$` escaped by doubling that character; vectors as `{"$vec2"|"$vec3"|"$vec4"|"$quat": [..]}`;
  `Json.null` as `null`. An unknown `$` key fails, naming the four valid ones.

### Platform & backends

- **R22 Folders.** Saves live in `data://saves/` (desktop, Android, iOS, the editor's Play:
  `.jasperengine/user/rundata/saves/`), and on the web in `data://<storageId>/saves/` so two games of
  one origin cannot see each other's slots (D9). Nothing else in that folder is touched.
- **R23 Web.** A write lands in MEMFS, then the store calls `flushFileSystem`; `onComplete` fires after
  the IndexedDB sync that follows the write finishes. `syncfs_callback` forwards its result to the
  store: success completes the writes, `QuotaExceededError` completes them with code `quota` and the
  browser's message plus *free space in the browser's site data or save less*, any other error with
  code `io`. With IndexedDB unavailable (`syncfs_enable_callback` failed: private mode, storage
  disabled) writes complete with code `unavailable` (*saves last until the page closes*) and the data
  stays readable for the session. The first write calls `navigator.storage.persist()` once (best effort,
  result logged at debug level). `SaveGame.storage()` returns `{used, quota}` from
  `navigator.storage.estimate()` refreshed after each sync (`-1` when unknown).
- **R24 Mobile.** Android and iOS flush on pause synchronously (R13); a write queued while paused waits
  for resume except when issued from `onPause`, which is flushed by the pause path. `storage()` returns
  the volume's free space and `-1` quota.
- **R25 No graphics.** SaveGame touches no GPU API; it is identical on the five backends. It compiles in
  every player (no module flag): R30 makes it free when unused.

### Performance (targets; gated by `*Bench` suites through `perfSlack()`)

- **R26 Header-only listing.** `list()` reads at most the header and metadata (<= 4,140 bytes) of the
  newest generation per slot: 50 slots list in <= 5 ms on desktop, <= 15 ms on a mid-range phone.
- **R27 Main-thread write cost.** Encoding 10,000 entries (256 KiB) takes <= 3 ms on the desktop
  reference (`SaveGameBench.Encode10kEntries`); writing a 2 KiB namespace into a slot of 2.7 MiB costs
  <= 0.3 ms on the main thread, file I/O excluded (`SaveGameBench.SmallNamespaceInLargeSlot`, the
  Craftsman scenario). On the single-threaded web build the whole write of a 256 KiB slot (encode,
  MEMFS write, checksum) is <= 4 ms on the main thread.
- **R28 Read cost.** Decoding a 256 KiB namespace into Lua takes <= 4 ms on the desktop reference
  (`SaveGameBench.Decode10kEntries`); the checksum of an 8 MiB file <= 2 ms (XXH3).
- **R29 Memory.** The store keeps encoded images of slots read or written this session, at most 32 MiB
  in total: past it the least recently used images without pending writes are dropped (they reload on
  the next read). `SaveGame.unload([slot])` drops them at once.
- **R30 Zero cost when unused.** Until the first `SaveGame` call: no store object, no queue, no
  directory, no allocation; the pause, shutdown and Play-stop hooks test one pointer
  (`SaveGameStore.NothingExistsUntilFirstUse` counts allocations).

### Editor UX (human)

- **R31 Saved Data window.** Window > General > Saved Data. Two tabs: **Save Slots** and
  **PlayerPrefs** (the existing store's keys and values, with Delete Key and the existing Clear User
  Settings action). Toolbar: Refresh, Open Folder (the Play data folder), Clear All Saves... (asks).
  The slot list has columns Slot, Version, Saved, Size, Backups, Status (OK, Recovered, Corrupt,
  Newer Version, Newer Format) and Title (`meta.title` when present).
- **R32 Inspecting a slot.** Selecting a slot shows its namespaces as a read-only tree (key, type,
  value; vectors as `(x, y, z)`; long strings cut with their size), **Copy as JSON** (R21's JSON), the
  metadata and `requires`, and a **Backups** list (generation, saved, version, size, Valid/Corrupt)
  with **Restore Backup** per row. Buttons: **Delete Slot**, and under a **Testing** dropdown
  **Corrupt Newest Generation** with Truncate, Flip a Byte and Overwrite Header.
- **R33 Live in Play.** While the window is visible it refreshes at most 4 times a second from the
  store's in-memory state (no disk read while a slot's image is loaded); closed, it costs nothing.
  Slots written in this Play session show a dot until Play stops.
- **R34 Project Settings.** Project Settings > Player > Saved Data: **Backups Kept** (2, 0-9) and **Max
  Slot Size (MiB)** (8, 1-64), reflected (S08) like `DebugSettings`, with tooltips; the editor's Play
  applies changes at the next write.
- **R35 Messages.** Every failed action shows the store's message in the Console and a notification:
  *Couldn't restore "slot1" generation 39: the file is corrupt (checksum mismatch).*
- **R36 Undo.** Delete Slot, Restore Backup, Corrupt Newest Generation and Clear All Saves are commands
  in the editor's undo timeline (S24), named "Delete Save Slot 'slot1'" etc. Each keeps the bytes of
  every file it changes (bounded by the slot cap); undo writes them back through `writeFileAtomic`.
  Undo refuses, with a Console message, when the slot changed since (its newest generation differs):
  *Can't undo "Delete Save Slot 'slot1'": the game saved that slot since.* Saved data is not project
  content: these commands never mark a scene modified.

### Agent UX (MCP, Lua, CLI)

- **R37 `user_settings` gains `store`.** `store: "prefs"` (default, today's behaviour) or `"saves"`.
  With `saves`, `key` is the slot, and the actions are: `list` (R6's fields per slot), `get` (`key`,
  optional `namespace`, optional `generation`: the decoded namespace as R21 JSON plus `version`,
  `generation`, `recovered_from`, `namespaces`, `meta`), `set` (`key`, `value` as R21 JSON, optional
  `namespace`, optional `version` (default: the game's `SaveGame.version` in Play, else the slot's or
  1); written and flushed before the reply), `delete`, `clear` (every slot), `corrupt` (`key`,
  `mode: truncate|flip|header`), `restore` (`key`, `generation`). `get`
  also takes `file` (a project-relative or absolute path of a `.jsave`, read only) to decode a save a
  player sent. The tool keeps one schema (new keys `store`, `namespace`, `generation`, `mode`, `file`);
  it stays within 1,500 characters.
- **R38 One store.** The tool and the window use the store the running game uses, so an agent sees the
  game's pending writes and the game sees the agent's writes at once (as PlayerPrefs today). Without
  Play the tool opens the store on the Play data folder.
- **R39 Errors name the fix.** For example: *No slot "slot2". Slots: slot1, autosave.*;
  *"value" must be an object or array in SaveGame JSON; vectors are {"$vec3": [x, y, z]}*;
  *slot "slot1" is version 5 and this game writes 4: pass version 5 to overwrite it, or read it with get.*
  After `corrupt`, `next` says: *Read it in the game (eval_lua "return SaveGame.read('slot1')") to
  see the recovery, or user_settings {store: "saves", action: "get", key: "slot1"}.*
- **R40 Lua.** The `SaveGame` module of §6 with a hand-written stub
  `engine/core/script/binding/stubs/SaveGame.lua` (`SaveWriteOptions`, `SaveReadOptions`, `SaveInfo`,
  `SaveSlotInfo`, `SaveBackupInfo`, `SaveError`).
- **R41 CLI.** `jasperengine-editor call user_settings --store saves --action list --project <dir>`
  works through the existing CLI (S31) with nothing added.
- **R42 Docs.** `docs/saved-data.md` (slots, namespaces, versions and migrations, generations and
  recovery, platforms and folders, size and speed advice: split large data into namespaces or slots,
  the Saved Data window), `docs/mcp/guide/saved-data.md` (test a migration with `set` at an old
  version; corrupt and verify recovery), the "Other areas" index line of the agent guide,
  `docs/mcp/guide/lua-api.md` (a `SaveGame` section next to "Json"), `docs/unity-glossary.md`
  (PlayerPrefs -> UserSettings; "save slots" -> SaveGame), `TOOLS.md` regenerated.
- **R43 Eval.** A case in `tools/mcp/eval/cases.py` (`guard_saved_data_corrupt`) in which an agent asked
  to check that a game survives a corrupt save calls `user_settings` with `store: "saves"`,
  `action: "corrupt"` and does not hand-edit files with `write_file`; it fails without R37's text.

### Data & compatibility

- **R44 New optional fields only.** `project.yaml` gains `saveGame: {backups, maxSlotMiB}` (omitted at
  the defaults), `PlayerConfig` (`game.json`) gains `saveGame` with the same defaults
  (`engine/core/player/PlayerConfig.h`, written only when not the defaults). No version bump, no
  migration; old projects load unchanged.
- **R45 The save format is versioned on its own.** Format version 1 is frozen by
  `SaveGameFormat.Version1Golden` (a committed `.jsave` fixture). A reader of version `k` reads every
  version `<= k`; a new tag or header field bumps the format version.
- **R46 PlayerPrefs untouched.** `UserSettings`, `PlayerPrefs` and their files do not change; nothing
  migrates PlayerPrefs into slots.

## 6. Design constraints & decisions

**D1 Binary, not JSON.** JSON loses vectors and integer keys (§3), prints floats in text, and cannot be
read with a streaming cap check. JLV1 is compact, bit-exact and deterministic; the JSON view of R21
covers humans and tools. *Rejected:* JSON with type tags as the stored form (twice the size,
parse cost, and still needs the bijection).

**D2 Generation files, not rotation.** Writing `<slot>.g<N+1>.jsave` and deleting old ones needs no
rename chain: each step is atomic on its own (S03), and a crash between write and prune leaves one
extra file, pruned by the next write. Rotation (`slot.1 -> slot.2`) has crash windows where a
generation exists under two names or none.

**D3 One checksum per file.** XXH3-64 is already linked and runs at memory speed; per-section CRCs
(Craftsman's clips) would allow partial recovery, but mixing sections of two generations could join
inconsistent data. A damaged generation is skipped whole.

**D4 Sections are the namespaces.** The store holds per slot a vector of `shared_ptr<const std::string>`
sections. A write swaps one pointer and pushes an immutable snapshot (the vector copy) to the queue; the
job concatenates header, sections and footer. No Lua on the worker.

**D5 Coalescing state machine per slot:** `idle -> writing(snapshot k) [-> waiting(snapshot m)]`.
`write` in `idle` starts a job; in `writing` it sets/replaces `waiting`. A landed job posts its result to
the main thread (`jobs::postToMain`), which fires callbacks and starts `waiting` if set.

**D6 Reads are synchronous by default.** A save menu or a level start needs the data now; files are
capped and decode fast (R28). `readAsync` exists for large slots and does the file read and checksum
on the queue, the Lua decode on the main thread.

**D7 Store per process, shared by the editor.** `save::SaveGameStore::instance()` is created on first
use with the folder of `System::getUserDataPath()` (`data://saves`, R22). In the editor the Play data
folder is the root, so the window, MCP and the game share one store (as `UserSettingsStore`). Switching
projects flushes and resets it (as `UserSettingsStore::unload`, `editor/Project.cpp:7325`).

**D8 Errors are values.** Every API returns `SaveError {code, message, path}`; nothing throws into Lua
except argument type errors (`luaL_argerror`, as the `Json` module). Codes: `name`, `encode`,
`too_large`, `not_found`, `corrupt`, `newer_version`, `newer_format`, `migration`, `io`, `quota`,
`unavailable`, `cancelled`.

**D9 Folder layout.** `saves/<slot>.g<N>.jsave`, plus at most one `<slot>.g<M>.jsave.corrupt`. Nothing
else. The web prefix `<storageId>/` matches what localStorage keys already do.

**D10 No world snapshot, no compression, no encryption** (§2).

**D11 Settings flow.** `SaveGameSettings {int backups = 2; int maxSlotMiB = 8;}` in `editor/` reflected
like `DebugSettings` (`editor/DebugSettings.h:19-47`); game.json carries them and every build's
`player::applySettings` (S98 D1) calls `savegame::configure(settings)` before the first scene.

### C++ sketch (`engine/core/save/`, namespace `jasperengine::save`)

```cpp
// SaveValue.h: JLV1 and the JSON view
enum class SaveErrorCode { None, Name, Encode, TooLarge, NotFound, Corrupt, NewerVersion, NewerFormat,
                           Migration, Io, Quota, Unavailable, Cancelled };
struct SaveError { SaveErrorCode code = SaveErrorCode::None; std::string message; std::string path;
                   bool ok() const { return code == SaveErrorCode::None; } };
JASPERENGINE_API bool encodeLua(lua_State* L, int index, std::string& out, SaveError& error);
JASPERENGINE_API bool decodeLua(lua_State* L, std::string_view payload, SaveError& error); // pushes one value
JASPERENGINE_API bool toJson(std::string_view payload, nlohmann::json& out, SaveError& error);
JASPERENGINE_API bool fromJson(const nlohmann::json& value, std::string& payload, SaveError& error);
JASPERENGINE_API const char* errorCodeName(SaveErrorCode code);               // "too_large"

// SaveFormat.h: one file
struct SaveSection { std::string name; std::shared_ptr<const std::string> payload; };
struct SaveFile { uint16_t format = 1; uint64_t generation = 0; int64_t savedAtMs = 0;
                  uint32_t dataVersion = 1; std::string metadata; std::vector<SaveSection> sections; };
JASPERENGINE_API std::string buildSaveFile(const SaveFile& file);
JASPERENGINE_API bool parseSaveFile(std::string_view bytes, SaveFile& out, SaveError& error);
JASPERENGINE_API bool parseSaveHeader(std::string_view bytes, SaveFile& out, SaveError& error); // no sections

// SaveGameStore.h
struct SaveSettings { int backups = 2; int maxSlotMiB = 8; };
JASPERENGINE_API void configure(const SaveSettings& settings);
struct SlotInfo { std::string slot; uint32_t version; double savedAt; uint64_t size; int generations;
                  std::vector<std::string> namespaces; std::string metadata; std::string status; };
struct ReadInfo { uint64_t generation = 0; uint64_t recoveredFrom = 0; uint32_t version = 0;
                  std::vector<std::string> problems; std::vector<std::string> namespaces; };
using WriteCallback = std::function<void(const SaveError&)>;
class JASPERENGINE_API SaveGameStore {
public:
    static SaveGameStore& instance();          // created on first use
    static SaveGameStore* existing();          // null until then: the engine hooks
    SaveError write(std::string_view slot, std::string_view nameSpace, std::string payload,
                    const WriteOptions& options, WriteCallback onComplete);
    SaveError read(std::string_view slot, std::string_view nameSpace, std::string& payload, ReadInfo& info);
    std::vector<SlotInfo> list();
    bool exists(std::string_view slot);
    SaveError remove(std::string_view slot);
    SaveError restore(std::string_view slot, uint64_t generation, WriteCallback onComplete);
    SaveError flush(std::string_view slot = {}, std::chrono::milliseconds limit = std::chrono::seconds(2));
    void unload(std::string_view slot = {});
    void reset();                              // editor project switch, tests
    // the web layer: the result of the IndexedDB sync that followed queued writes
    void fileSystemSynced(bool ok, std::string_view errorName, std::string_view message);
};
```

Migrations live in the Lua binding (`SaveGameLua.cpp`): the store returns the raw sections and the
version; the binding decodes all sections, runs the registered functions, re-encodes them into the
store's image with `markMigrated(slot, version)` so R9 holds. C++ games use the store at the payload
level; a C++ migration hook is out of scope.

### Lua sketch (`engine/core/script/binding/SaveGameLua.cpp`)

```lua
SaveGame.version = 3
SaveGame.addMigration(1, function(ns) ns.main.gold = ns.main.coins or 0; ns.main.coins = nil; return ns end)
SaveGame.addMigration(2, function(ns) ns.quests = ns.quests or {}; return ns end)

local ok, err = SaveGame.write("slot1", {level = "forest", pos = Vector3(4, 0, 2), gold = 120},
    {meta = {title = "Forest - 2h 13m"}, onComplete = function(ok, err)
        if not ok then self.toast:show("Couldn't save: " .. err.message) end
    end, owner = self})
SaveGame.write("slot1", inventory, {namespace = "inventory"})      -- only this section re-encoded

local data, info = SaveGame.read("slot1")                          -- main; info.migratedFrom, info.recoveredFrom
if not data then print(info.code, info.message) end
for _, s in ipairs(SaveGame.list()) do print(s.slot, s.meta.title, s.status) end
SaveGame.readAsync("slot1", function(data, info) end, {namespace = "inventory", owner = self})
SaveGame.exists("slot1"); SaveGame.delete("slot1"); SaveGame.flush()
SaveGame.getBackups("slot1"); SaveGame.restoreBackup("slot1", 39); SaveGame.unload()
local s = SaveGame.storage()                                       -- {used = .., quota = ..}
```

`err` and failed `info` are `SaveError` tables `{code, message, path?}`. Unknown option keys log one
warning (as `HttpOptions`).

### MCP sketch

```json
{"action": "list", "store": "saves"}
{"action": "get", "store": "saves", "key": "slot1", "namespace": "inventory"}
{"action": "set", "store": "saves", "key": "slot1", "value": {"pos": {"$vec3": [4, 0, 2]}}, "version": 1}
{"action": "corrupt", "store": "saves", "key": "slot1", "mode": "flip"}
{"action": "restore", "store": "saves", "key": "slot1", "generation": 39}
```

Description addition (counted in the 1,500): *store "saves": SaveGame slots (key = slot): list, get
(namespace, generation, or a .jsave file), set (JSON; vectors {"$vec3":[..]}), delete, clear, corrupt,
restore (generation).*

## 7. Acceptance criteria

Unit tests go in `engine/tests/core/save/` (`jasperengine-core-tests`) unless stated; each fails
without the change because the code it calls does not exist, plus the specific reason given.

- **AC1** `SaveGameValue.RoundTripsEveryType`: booleans, `math.mininteger`, `math.maxinteger`, `0.1`,
  `-0.0`, NaN payload bits, a string with `\0` and invalid UTF-8, nested tables with integer and string
  keys, sparse integer keys, `Vector2/3/4`, `Quaternion`, `Json.null` encode and decode to equal values
  (types included: `math.type`). Fails with LuaJson (vectors and integer keys change).
- **AC2** `SaveGameValue.ErrorsNameThePath`: a function, a boolean key, a cycle and depth 65 each fail
  with code `encode` and the exact path text of R2.
- **AC3** `SaveGameValue.EqualTablesGiveEqualBytes`: two tables built in different insertion orders
  encode to identical bytes (R20 ordering).
- **AC4** `SaveGameValue.JsonViewIsABijection`: R21's table of cases (including `"#3"`, `"##x"`,
  `{"$vec3":[..]}`, `$float`, `$bytes`) round-trips JSON -> payload -> JSON and payload -> JSON ->
  payload; an unknown `$` key fails naming the valid ones.
- **AC5** `SaveGameFormat.Version1Golden`: `engine/tests/core/fixtures/save/v1.jsave` (committed) parses
  to the expected sections; `buildSaveFile` of the same `SaveFile` reproduces it byte for byte.
- **AC6** `SaveGameFormat.RejectsDamage`: every truncation length of the golden, every single-byte flip
  (checked over all offsets), a header claiming 4 GiB sections and a varint of 11 bytes are rejected
  without crashing and without an allocation above 64 KiB (`AllocationCountScope`), each naming its
  reason. Runs under ASan in `tools/run-sanitizer-tests.sh`.
- **AC7** `SaveGameFormat.NewerFormatIsReportedNotRead`: format version 2 -> `newer_format`.
- **AC8** `SaveGameStore.KeepsBackupsKeptPlusOneGenerations`: 10 writes with Backups Kept 2 leave
  `g8, g9, g10`; with 0, only the newest.
- **AC9** `SaveGameStore.RecoversFromNewestValidGeneration`: corrupt `g10` (each mode of R32) -> the read
  returns `g9`'s data, `recoveredFrom = 9`, one warning, `g10` renamed `.corrupt`; all corrupt ->
  `corrupt` with every file in place.
- **AC10** `SaveGameStore.CrashAtEveryWriteStepLeavesAReadableSlot`: with `io::setAtomicFileFaultInjector`
  failing at `open`, `write`, `sync` and `rename` in turn (and a simulated crash between publish and
  prune), the slot always reads as either the old or the new data; a failed write reports `io` and the
  previous generation stays current.
- **AC11** `SaveGameKillSlow.KillDuringWritesLeavesAReadableSlot`: `jasperengine-crash-test` gains a mode
  that writes a 4 MiB slot in a loop; the test SIGKILLs it at 20 random moments (Linux, macOS) and each
  time the slot reads valid. Without generations (one file rewritten in place) it fails.
- **AC12** `SaveGameStore.CoalescesWrites`: 100 writes to one slot in one frame with a held queue
  (a test gate) produce exactly 2 file writes (the first and the last snapshot) and 100 callbacks in
  call order, all with the last write's result.
- **AC13** `SaveGameStore.NamespaceWriteKeepsOtherNamespaces` and `ReadSeesPendingWrite`: R3 and R4.
- **AC14** `SaveGameStore.NewerVersionIsNeverOverwritten`: a slot at version 5 read by version 4 gives
  `newer_version`; `write` fails the same; `force = true` writes; recovery does not skip it.
- **AC15** `SaveGameStore.FlushOnPauseWritesPending`: `Engine::systemPause` with a held queue writes the
  waiting snapshot before it returns; a queue held past 2 s logs R13's line.
- **AC16** `SaveGameStore.TooLargeNamesTheNamespaces`: R14's message with sizes and the three largest.
- **AC17** `SaveGameStore.NothingExistsUntilFirstUse`: engine start, 100 frames, pause, shutdown with no
  `SaveGame` call: no `saves` folder, `existing() == nullptr`, zero allocations in the hooks.
- **AC18** `engine/tests/core/script/SaveGameLuaTest.cpp`: `MigrationsRunInOrder` (1 -> 3 through two
  functions; `migratedFrom = 1`; files unchanged until the next write, then all at version 3),
  `MissingMigrationLeavesTheFiles` (R7's message), `CallbackDroppedWhenOwnerGone`,
  `CallbackRunsOnTheMainThread`, `SlotNameErrorSuggests` (R1).
- **AC19** Benches `SaveGameBench.Encode10kEntries`, `Decode10kEntries`, `SmallNamespaceInLargeSlot`,
  `List50Slots` print `[bench]` lines and enforce R26-R28 under `JASPER_TEST_PERF_SLACK`.
- **AC20** Editor `editor/tests/savedata/SavedDataWindowTest.cpp`: `DeleteRestoreAndCorruptAreUndoable`
  (each undo restores the files byte for byte), `UndoRefusedWhenTheSlotChangedSince`,
  `SettingsRoundTripAndOmitDefaults` (R34, R44; plus the `ReflectParity` golden key).
- **AC21** MCP `tools/mcp/tests/test_saved_data.py` (`CI = True`, own project from `mcp --create`):
  `test_saves_in_play` (a script writes `slot1` with a vector and an integer-keyed table; `list` and
  `get` show them in R21 JSON; `set` from the tool is read by the game through `eval_lua`),
  `test_corrupt_and_recover` (`corrupt` then the game's `read` reports `recoveredFrom`, the window state
  via `list` says `recovered`), `test_restore_and_newer_version` (R17, R8 messages), `test_file_read`
  (`get` with `file` on a copied `.jsave`), `test_group_listing` (the tool stays in `scripting`).
  `tool_budget.py --check` passes with `user_settings` <= 1,500 characters.
- **AC22** Web gate `GATE=savegame` (after `WEB_PROJECT=savegame tools/build.sh web`;
  `tools/test-projects/savegame/`): the page writes `slot1`, waits for `onComplete`, then the gate
  reloads the page (a new `reload` baseline step in `tools/web/gates/`) and checks the console line
  `savegame: read ok gen 1`; a second run with CDP `Storage.overrideQuotaForOrigin` at 64 KiB expects
  `savegame: quota` from `onComplete`. Fails today: no `SaveGame`, and quota errors never reach Lua.
- **AC23** Android: `tools/android-emulator-test.sh` with the same project writes a slot, sends the app
  to the background (`adb shell input keyevent HOME`), kills it (`am kill`), relaunches and expects
  `savegame: read ok`. Manual on iOS per `docs/specs/checklists/savegame.md` (written with the code).
- **AC24** Eval `guard_saved_data_corrupt` (R43) passes on the Orquester-managed accounts and fails
  with the R37 description text removed.
- **AC25** Lints: `python3 tools/check_raw_writes.py` passes (every write through `writeFileAtomic`);
  `gen_tool_docs.py --check` and `check_agent_docs.py` pass.

## 8. Dependencies & conflicts

- **Depends on** S03 (`AtomicFile`, fault injector), S06 (`SerialQueue`, `postToMain`), S08 (reflected
  settings), S24 (undo timeline for R36), S30 (`PlayerConfig`, prebuilt `init()`), S31 (CLI).
- **Siblings.** `dev-console-for-agents.md`: none required; a game may add a `save` command.
  `project-test-runner.md`: slots live under `data://saves/`, so its sandbox and Play Data profiles
  cover them; its `Tests/Data` snapshots are the slot fixtures (`.jsave` files copied in), and tests
  can also seed slots with `user_settings {store: "saves", action: "set"}`.
  `input-recorder-and-input-scripts.md`: replays that load saves need deterministic slot contents; the
  JSON view gives them fixtures. `shipped-game-diagnostics.md`: a game may attach its `.jsave` files to a
  user report (that spec leaves attaching to the game); `user_settings {action: "get", file}` decodes
  one, and a recovery's warning line reaches its journal like any log line. `build-profiles.md`: the save folder is not per profile (§2);
  Backups Kept and Max Slot Size could become per-profile values; not required here. `cancellable-prioritised-events.md`:
  `onComplete` is a plain callback, no event priority needed. `game-ui-localization.md`: error messages
  are developer-facing English; games show their own text.
- **T6 R79** keeps its own voxel save files; a later change may move them into a SaveGame namespace.
- **Merge hotspots:** `engine/core/Engine.cpp` (pause and shutdown hooks next to `UserSettings::save`),
  `editor/Project.cpp` (Play stop, project switch), `editor/mcp/tools/DataTools.cpp`,
  `engine/platform/emscripten/JasperEngineWeb.cpp` (`syncfs_callback`), `engine/core/player/PlayerConfig.*`,
  `editor/Generator.cpp`, `tools/mcp/budget.json`, `docs/mcp/guide/lua-api.md`, the CMake source lists
  (`engine/core` sources and the Xcode sync).

## 9. Risks & defaults

- **Main-thread encode of a huge namespace** still hitches. Default: R14's cap plus docs telling games to
  split data; `write` logs a warning once when one encode exceeds 8 ms (*SaveGame: encoding "world" took
  23 ms; split it into namespaces*).
- **Flash wear and battery** from frequent writes. Default: coalescing plus generations is at most one
  file per landed snapshot; the docs recommend saving on checkpoints, not per frame. No minimum interval
  is enforced.
- **IndexedDB eviction** on the web (storage pressure, Safari's 7-day rule for unvisited sites). Default:
  `navigator.storage.persist()` on first write and the documented limitation; no workaround.
- **Generations triple disk use.** Default 2 backups (3 files); 0 is allowed for games with very large
  slots.
- **A game writing during `onPause` on Android** may be killed before the flush ends. Default: the 2 s
  limit and the log line; games keep slots small.
- **Clock skew** makes `savedAt` unreliable for ordering. Default: ordering uses generation numbers;
  `savedAt` is display-only.
- **Tampering** (players editing saves): out of scope, documented; the checksum is not a signature.
- **The web gate's reload step** is new gate-runner code; if it cannot land with this spec, AC22 keeps
  the quota half and the reload check moves to the Android test (recorded in Deviations).

## Deviations

- **D-a List counts generation files.** R6 says `generations` counts the valid ones, but R26 reads
  only the newest header per slot: `list()` counts the generation files on disk (`getBackups` and
  the Saved Data window's Backups list check each one). Its `status` is header-only too: `recovered`
  when an earlier read set a newer generation aside (or the newest header is unreadable), `corrupt`
  only when no header reads; a checksum failure of the newest file shows at its first read.
- **D-b `SaveGame.storage()` has a third field.** Desktop and mobile have no quota: the table is
  `{used, quota, free}`, `used` the bytes of the slot files (desktop, mobile) or the site's usage
  (web), `free` the volume's free space (desktop, mobile) or `quota - used` (web).
- **D-c `$quat` order.** The JSON view and the file store a Quaternion as `[x, y, z, w]` (the MCP
  convention), not the Lua constructor's `(w, x, y, z)`.
- **D-d JSON keys that are not UTF-8.** R21 has no spelling for a string key that is not UTF-8; to
  keep the view a bijection it is `"$bytes:<base64>"` (a key starting with `$` is written `$$`, so
  nothing collides). NaN payloads other than the canonical quiet NaN are `{"$float": "nan:<16 hex>"}`.
- **D-e The 2 s flush test is a `Slow` suite.** AC15's "a queue held past 2 s logs R13's line" waits
  2 s, above the 1 s rule for ordinary tests: it is `SaveGameStoreSlow.FlushPastTheLimitLogsAbandoned`
  (`engine/tests/core/save/SaveGameSlowTest.cpp`); `SaveGameStore.FlushOnPauseWritesPending` keeps the
  fast half. AC11's kill test lives in the same file.
- **D-f AC17 runs in a child process.** The store is process-wide and other tests make it, so
  `SaveGameStore.NothingExistsUntilFirstUse` runs the test binary again on the opt-in
  `SaveGameStore.DISABLED_NothingExistsUntilFirstUseChild`, which checks no store, no allocation in
  100 lifecycle hook calls, a pause and resume, and no `saves` folder. Engine shutdown is not part of
  it: the test binary shuts the engine down once, after every test.
- **D-g AC6 under ASan.** `tools/run-sanitizer-tests.sh` builds the editor tests only; the format
  tests are core tests, run under ASan with `SANITIZE=address,undefined UNIT_SUITE=core
  UNIT_TEST_ARGS=--gtest_filter='SaveGame*' tools/build.sh unit-tests`.
- **D-h No ReflectParity key.** `ReflectParity` covers component tables only; the settings struct is
  checked by `SavedDataWindow.SettingsRoundTripAndOmitDefaults` (project.yaml, game.json, the generated
  init, undo).
- **D-i Undo of the window's steps.** Delete, Restore Backup, Corrupt and Clear All Saves go on the
  project history from MCP too (`user_settings` `delete`, `clear`, `corrupt`, `restore`); `set` is
  not an undo step (the spec lists it with none).
- **D-j Settings rows.** The MCP keys are `player.saved_data.backups_kept` and
  `player.saved_data.max_slot_size_mib` (set_project_settings), under Player > Saved Data.
- **D-k The web quota gate is its own baseline.** `GATE=savegame` reloads the page and reads the
  slot back; `GATE=savegame-quota` (baseline key `project` points it at the savegame build) sets the
  site quota to 64 KiB with `Storage.overrideQuotaForOrigin` and expects `quota` from the 256 KiB
  slot's `onComplete`.
- **D-m Namespace `jasperengine::savegame`.** The C++ API is in `jasperengine::savegame`, not `::save`: with
  `using namespace jasperengine`, a namespace called `save` breaks every unqualified call of a function
  named `save` in a file that sees it (the editor's Recorder window was the first).
- **D-l Android kill and relaunch.** `tools/android-emulator-test.sh` gains `--kill-relaunch-after
  TEXT` (HOME, `am kill`, relaunch; the expectations then apply to the new process).
- **Mac pass.** AC11's `SaveGameKillSlow.KillDuringWritesLeavesAReadableSlot` passed on an arm64 Mac
  (`tools/ci/macos.sh tests`). AC23's iOS checklist (`checklists/savegame.md`) ran on the simulator with
  `tools/test-projects/savegame` built for it: steps 1-4 passed (`savegame: written`, `savegame: big
  ok`; `Documents/saves` held `big.g1.jsave` and `slot1.g1.jsave`; a SIGKILL, then the relaunch read
  `savegame: read ok gen 1`). Step 5 needs a game that saves in `onApplicationPause` (the test project
  does not), step 6 a nearly full disk, step 7 the dev console: not run.
- **D-m R27/R28 on box A.** Encoding 10,000 entries took 3.4-3.7 ms and decoding them 3.8-4.0 ms on
  box A (a 2.4 GHz Haswell). The encoder now keeps each pair on the Lua stack from `lua_next` (a table
  too big for the stack looks its values up as before), sorts keys by an 8-byte prefix first and
  reuses its key lists; the decoder holds a table's array values on the stack until it has read the
  pair count, so the table is made with both parts sized. Six runs: encode 2.43-2.65 ms and 2.98
  in one, decode 3.12-3.53 ms. The bytes are the same; the order `pairs` visits a decoded table in may differ from
  before (Lua never specified it; the same bytes give the same order).
