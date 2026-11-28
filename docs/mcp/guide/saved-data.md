# Saved data (SaveGame slots) for agents

A game keeps progress in SaveGame slots (`docs/saved-data.md`; Lua: `jasper://docs/lua-api`,
"SaveGame"). The editor's Play saves them under `.jasperengine/user/rundata/saves/`, one file per
generation (`slot1.g12.jsave`). They are binary and checksummed: never write or edit them with
`write_file`. Use `user_settings` with `store: "saves"`; it works on the store the running game
uses, so the game sees what you write at once and you see its pending writes.

## Actions

| Call | Does |
|---|---|
| `{action: "list", store: "saves"}` | every slot: version, saved_at, size, generations, namespaces, meta, requires, status (`ok`, `recovered`, `corrupt`, `newer_version`, `newer_format`) |
| `{action: "get", store: "saves", key: "slot1"}` | the `main` namespace as SaveGame JSON, with version, generation, namespaces, meta, `recovered_from` |
| `... "namespace": "inventory"` / `"generation": 7` | another namespace / an older generation's file |
| `{action: "get", store: "saves", file: "bug-report/slot1.g40.jsave"}` | decodes a save file a player sent (read only) |
| `{action: "set", store: "saves", key: "slot1", value: {...}}` | writes a namespace (written and flushed before the reply); `version` stamps a data version |
| `{action: "delete" \| "clear", store: "saves", ...}` | deletes a slot / every slot (undo steps) |
| `{action: "corrupt", store: "saves", key: "slot1", mode: "flip"}` | damages the newest generation: `truncate`, `flip` (a byte), `header` (an undo step) |
| `{action: "restore", store: "saves", key: "slot1", generation: 7}` | writes generation 7's data as the newest generation (an undo step) |

The CLI is the same tool: `jasperengine-editor call user_settings --store saves --action list --project <dir>`.

## SaveGame JSON

The JSON view is exact: what `get` shows, `set` writes back unchanged.

- Integers stay integers; a float keeps its decimal point (`3.0`).
- `{"$vec2": [x, y]}`, `{"$vec3": [x, y, z]}`, `{"$vec4": [x, y, z, w]}`, `{"$quat": [x, y, z, w]}`.
- A table with keys exactly 1..n is an array; any other table an object, integer keys as `"#5"`.
  A string key starting with `#` or `$` is written doubled (`"##tag"`, `"$$price"`).
- `null` is Lua's `Json.null`; `{"$float": "nan" | "inf" | "-inf"}`; bytes that are no UTF-8:
  `{"$bytes": "<base64>"}`.

## Test a migration

1. Write the slot at the old version: `set` with `version: 1` and the old shape.
2. Play the game whose `SaveGame.version` is higher, then read it in the game:
   `eval_lua "local d, i = SaveGame.read('slot1'); return {d, i.migratedFrom}"`.
3. The files keep version 1 until the game's next write; `get` shows what is on disk.

A slot of a newer version than the game is never written over: `set` fails naming the version
to pass.

## Check that a game survives a damaged save

1. Let the game save (or `set`) at least two generations.
2. `corrupt` the slot (`mode`: `flip`, `truncate` or `header`).
3. Read it in the game (`eval_lua "return SaveGame.read('slot1')"`): the data of the previous
   generation, `info.recoveredFrom`, and one warning in the Console (`read_console`).
4. `list` says `recovered`; the damaged file is set aside as `.corrupt`.

## Errors

Every failure names its code: `name` (slot names are `a-z 0-9 _ -`), `encode`, `too_large`,
`not_found`, `corrupt`, `newer_version`, `newer_format`, `migration`, `io`, `quota` (web),
`unavailable` (web storage off), `cancelled`.
