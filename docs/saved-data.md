# Saved data: SaveGame slots

A game keeps a player's progress in **save slots**: `SaveGame` gives Lua (and C++,
`engine/core/save/SaveGameStore.h`) named slots holding Lua tables, written off the main thread,
stamped with a data version that the game's migrations upgrade, kept in several checksummed
generations and recovered from the newest valid one. PlayerPrefs (`UserSettings`) stays the
place for settings: volume, key bindings, the last menu tab. (Specification:
[specs/savegame.md](specs/savegame.md).)

Unity has no built-in slot system; the closest named equivalent is Unreal's `USaveGame`
(`SaveGameToSlot`, `LoadGameFromSlot`), whose vocabulary `SaveGame` follows.

## Slots and namespaces

- A **slot** is a name of 1-48 characters `a-z`, `0-9`, `_`, `-` (lower case, so a slot is one
  file on any volume): `slot1`, `autosave`, `demo-slot1`. Another name fails every call with
  code `name` and a suggestion (*Slot names use a-z, 0-9, '_' and '-' (got "Slot 1"); try
  "slot-1".*).
- A slot holds up to 64 **namespaces** (named like slots, 1-32 characters), each one Lua value,
  usually a table. `write` without a namespace writes `main`. Writing a namespace replaces only
  that one, so systems that save separately (inventory, quests, the world) never trample each
  other; `replaceAll = true` drops the others. `Dialogue.variables.save(slot)` keeps the dialogue
  variables and visit counts in a namespace `dialogue` ([dialogue.md](dialogue.md)).
- Values: booleans, integers (64-bit) and floats (bit-exact, NaN and infinities too), strings
  (any bytes), tables with integer or string keys, `Vector2`, `Vector3`, `Vector4`, `Quaternion`
  and `Json.null`. Functions, threads, other userdata, boolean or float keys, cycles and nesting
  deeper than 64 fail with code `encode` and the place (*cannot save a function at
  $.player.onHit*, *cycle at $.a.parent*). A table reached twice is saved twice. Metatables are
  not saved: class instances come back as plain tables.
- `meta` is the slot's data for a save menu (title, play time, level), at most 4 KiB, which
  `list()` returns without decoding any namespace. `requires` records the content a save needs
  (`{dlc1 = 2}`); the engine stores and returns it and never interprets it.

```lua
local ok, err = SaveGame.write("slot1", {level = "forest", pos = Vector3(4, 0, 2), gold = 120},
    {meta = {title = "Forest - 2h 13m"}, owner = self, onComplete = function(done, e)
        if not done then Log.warn("Couldn't save: " .. e.message) end
    end})
SaveGame.write("slot1", inventory, {namespace = "inventory"})

local data, info = SaveGame.read("slot1")
if data then
    self.gold = data.gold
else
    Log.warn(info.code .. ": " .. info.message)
end
for _, s in ipairs(SaveGame.list()) do
    Log.print(s.slot .. " " .. tostring(s.meta and s.meta.title) .. " " .. s.status)
end
```

`write` encodes the value before it returns: changing the table afterwards does not change the
save, and a `read` right after returns what was written, before it reaches the disk. It returns
`true`, or `false, err` at once for `name`, `encode`, `too_large`, `newer_version` and
`newer_format`; the file is written by a worker, and `onComplete(ok, err)` runs on the main thread
in a later frame. With `owner` (a script's `self`, a GameObject or a Scene), the callback of an
owner that is gone is dropped; the write still happens. `read` is synchronous (files are capped
and decode fast); `readAsync(slot, fn)` reads the file on a worker for large slots. Every error is
a value `{code, message, path}`: nothing raises but wrong argument types.

## Versions and migrations

`SaveGame.version` (default 1) is stamped into every write. When a game changes its data layout
it raises the version and registers one migration per step:

```lua
SaveGame.version = 3
SaveGame.addMigration(1, function(ns) ns.main.gold = ns.main.coins or 0; ns.main.coins = nil; return ns end)
SaveGame.addMigration(2, function(ns) ns.quests = ns.quests or {}; return ns end)
```

Reading a slot of version 1 runs both on every namespace of the slot (`ns` maps names to values)
and returns the result with `info.migratedFrom = 1`. The files keep version 1 until the next write
of that slot, which writes every namespace at version 3. A missing step or a migration that raises
or returns no table fails with code `migration` and leaves the files alone. A write at a newer
version from elsewhere (a dialogue's save hooks, `user_settings` `set` in Play) runs the same
migrations on the slot's other namespaces first; with no game running to run them it fails with
code `migration` rather than label old data with the new version.

A slot of a **newer** version than the game (a player who went back to an older build) reads as
`newer_version`, and a file of a newer engine format as `newer_format`: neither is ever written
over (`write` fails the same way unless `force = true`), and recovery never skips past them.

## Generations and recovery

Every landed write is a new file `<slot>.g<N>.jsave`; then the generations beyond **Backups Kept**
+ 1 are deleted, oldest first. Each file is written atomically (a temporary sibling, flushed,
renamed: `docs/data-safety.md`), so a crash, a power cut or a full disk leaves every earlier
generation whole and the new one complete or absent.

A read uses the newest generation that is complete, checks (an XXH3 checksum) and decodes. The
checksum comes first: a damaged format field reads as damage, and only a file whose checksum holds
reads as `newer_format`. When
the newest one is damaged, the read returns the previous one with `info.recoveredFrom` (the
generation used) and `info.problems` (*slot1.g41.jsave: checksum mismatch*), logs one warning per
slot per session, and sets the damaged file aside as `<name>.corrupt` (only the newest set-aside
file is kept). With no valid generation the read fails with code `corrupt` and every file stays.
A file that can't be read right now (another program holds it, a permission) is not damage: the
read fails with code `io`, nothing is set aside or deleted, and a write waits until it can be read.
A temporary file a killed write left behind is deleted the next time the game opens the saves
folder, and deleting a slot takes its temporary files too.

`SaveGame.getBackups(slot)` lists every generation; `SaveGame.restoreBackup(slot, generation)`
writes that generation's data as a new newest generation (the chosen file stays, so a restore can
be undone by restoring the previous newest).

The checksum detects damage, not edits: saves are not encrypted or signed, and a player can
change them.

## Writing often

Per slot at most one write is on its way to the disk and one waits; a write while one is in flight
replaces the waiting snapshot, and every `onComplete` of the writes a snapshot contains fires with
its result, in call order. Saving every frame never builds a queue, but save at checkpoints: each
landed snapshot is a file write, which costs flash wear and battery on phones.

`SaveGame.flush([slot])` blocks until what is queued has landed. The engine flushes every slot when
the app is paused (after `onPause`), when it quits (after `onShutdown`) and when the editor's Play
stops, for at most 2 s; what is still not written then is logged (*SaveGame: "slot1" was not
written before the app paused (2 s)*).

## Size and speed

- **Max Slot Size (MiB)** caps a slot's file (default 8, 1-64): a write past it fails with
  `too_large`, naming the size and the three largest namespaces.
- A write re-encodes only its namespace; the others stay encoded in memory. Split large data into
  namespaces (or slots): a 2 KiB namespace written into a 2.7 MiB slot costs a fraction of a
  millisecond on the main thread, whereas re-encoding the whole slot would not.
- One encode longer than 8 ms logs once: *SaveGame: encoding "world" took 23 ms; split it into
  namespaces*.
- `list()` reads only each slot's header and metadata.
- The store keeps the slots read or written this session in memory, at most 32 MiB; past it the
  least recently used ones without pending writes are dropped and reload on the next read.
  `SaveGame.unload([slot])` drops them now.
- Nothing exists until the first `SaveGame` call: a game that never saves pays nothing.

## Platforms and folders

| Where | Folder |
|---|---|
| Editor (Play) | `<project>/.jasperengine/user/rundata/saves/` |
| Desktop exports | `saves/` in the user data folder (`data://`: `%APPDATA%\<id>`, `~/.local/share/<id>`, `~/Library/Application Support/<id>`) |
| Android | the app's internal files, `saves/` |
| iOS | Documents/`saves/` (backed up by iOS) |
| Web | `/datafs/<storageId>/saves/` in IndexedDB, so two games of one site never see each other's slots |

Nothing else in that folder is touched; a demo and its full game share it on purpose (progress
carries over); prefix slot names to keep them apart. Two copies of a desktop game writing one
folder at once: the last writer wins per generation.

- **Web**: a write lands in memory, then the store asks the browser to store it; `onComplete`
  fires once IndexedDB has it. A full site storage completes it with code `quota` (*free space in
  the browser's site data or save less*); with IndexedDB unavailable (private mode, storage off)
  writes complete with `unavailable` (*saves last until the page closes*) and stay readable for
  the session. The first write asks the browser to keep the site's data
  (`navigator.storage.persist()`); browsers may still evict a site nobody visits (Safari after 7
  days). `SaveGame.storage()` returns `{used, quota}` from `navigator.storage.estimate()`.
- **Android, iOS**: the pause flush is synchronous; a write from `onPause` is flushed by the pause
  path. `SaveGame.storage()` returns the volume's free space (`free`) and `quota = -1`.

## The Saved Data window

**Window > General > Saved Data** shows the project's saved data, live while the game plays (at
most 4 refreshes a second; nothing while closed):

- **Save Slots**: Slot (a dot: written in this Play session), Version, Saved, Size, Backups,
  Status (OK, Recovered, Corrupt, Newer Version, Newer Format) and Title (`meta.title`). The
  toolbar has **Refresh**, **Open Folder** and **Clear All Saves...**.
- Selecting a slot shows its namespaces as a read-only tree (vectors as `(x, y, z)`, long strings
  cut with their size), **Copy as JSON**, the metadata and `requires`, and its **Backups**
  (generation, saved, version, size, Valid or Corrupt) with **Restore Backup** per row.
  **Delete Slot**, and under **Testing**, **Corrupt Newest Generation** (Truncate, Flip a Byte,
  Overwrite Header) to check that the game survives a damaged save.
- **PlayerPrefs**: every key and value, **Delete Key** and **Clear User Settings...**.

Delete Slot, Restore Backup, Corrupt Newest Generation and Clear All Saves are undo steps (Edit >
Undo puts the files back byte for byte); undo refuses when the game saved that slot since. A step
that fails partway (a slot that can't be deleted, a restore still writing after two seconds) says
so and stays one undo step for what it changed. Deleting a slot while one of its writes is still
running is refused; try again once it lands. Viewing a slot reads it without repairing it. Saved
data is not project content: these never mark a scene modified.

**Project Settings > Player > Saved Data** holds **Backups Kept** (2, 0-9) and **Max Slot Size
(MiB)** (8, 1-64); Play applies a change at its next write, and builds take them along.

Agents use the same operations through `user_settings` with `store: "saves"`
(`docs/mcp/guide/saved-data.md`).

## File format

A save file is little-endian: the magic `JSAV\r\n\x1A\n` (a text-mode copy or a cut download
breaks it), the format version (1), the generation, the time saved, the data version, the
metadata, then one section per namespace, and a footer with the XXH3-64 of everything before it
and `JSAVEND\n`. Readers check every length against the bytes left before they allocate. A value
is one JLV1 value (`engine/core/save/SaveValue.h`): equal values give equal bytes. Format 1 is
frozen by a committed golden file; a reader of format k reads every format up to k.
