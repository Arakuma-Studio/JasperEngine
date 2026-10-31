# Script data: Json module, UserSettings/PlayerPrefs, persistent files (design)

Feature `script-data` (ledger #18, branch `gaps/script-data`). Binding conventions:
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) "Shared conventions" (**`Json` is the single JSON<->Lua
conversion**; `http` and `native-bridge` use it and merge after this feature).

## 1. Goals

1. A `Json` Lua module: `Json.encode(value [, opts])`, `Json.decode(text [, opts])`, non-raising
   `tryEncode/tryDecode`, `Json.null`, `Json.array/Json.object` markers, file helpers; exact, documented
   rules for arrays vs objects, integers vs floats, null, UTF-8, errors with a path. Built on nlohmann 3.11.3
   (`engine/libs/json/json.hpp`) and exposed to C++ (`LuaJson`) for other features.
2. UserSettings (PlayerPrefs) completed on every platform: `hasKey`, `deleteKey` (alias of `removeKey`),
   `deleteAll`, `save` with defined flush semantics; typed getters that never throw; locale-independent
   numbers; a Unity-familiar `PlayerPrefs` module over the same store.
3. Persistent file helpers for saves: `File.readText/writeText/appendText/exists/remove/makeDirs/list` with
   atomic writes, `data://` (user data) and read-only `asset://`/`lua://` paths, `Json.readFile/writeFile`,
   Web persistence (`System.syncFileSystem()`, waiting for the IndexedDB restore), and a proper per-user data
   directory on desktop exports.
4. A data-asset story: JSON files and Lua data modules, documented as the ScriptableObject replacement.

## 2. Non-goals

- ScriptableObject-like typed assets with Inspector editing (a typed asset system with schemas, references
  and an Inspector is a separate feature; the game's 20 Garden SOs port to a Lua data module or JSON file).
- Binary serialization (BinaryFormatter-like), encryption/obfuscation of saves (Unity's PlayerPrefs are not
  encrypted either), cloud saves.
- JSON schema validation, JSON5, streaming parsers, ordered objects (keys are always sorted, 6.1).
- A Lua `io` replacement: `io` keeps working with `FileData.getSystemPath(...)` paths (documented limits on Web).

## 3. Users and stories

- Agent porting `JsonConvert.DeserializeObject<DateResponse>(text)`: `local t = Json.decode(text);
  local ts = t.utc_timestamp`.
- Agent porting analytics `JsonConvert.SerializeObject(new { playerID })`: `Json.encode({playerID = id})`.
- Human: `PlayerPrefs.setInt("Minigame_Money", 120)`, `if PlayerPrefs.hasKey("OnboardingDone")`,
  `PlayerPrefs.save()` (same as Unity).
- Human porting a save system: `Json.writeFile("data://garden.json", state)`, later
  `local state = Json.readFile("data://garden.json")` (nil + message when absent).
- Agent testing saves: MCP `user_settings` (list/get/set/delete/clear) inspects and resets the editor's store.
- C++ feature code (`http`): `LuaJson::push(L, json)` / `LuaJson::toJson(L, idx, out, opts, err)`.

## 4. Research summary

- Unity PlayerPrefs (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/PlayerPrefs.html):
  int/float/string, HasKey, DeleteKey (no-op if absent), DeleteAll, Save; stored in the registry (Windows),
  plist (macOS), `~/.config/unity3d` (Linux), SharedPreferences (Android), NSUserDefaults (iOS), IndexedDB
  (WebGL); saved automatically on quit; `Save()` writes now (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/PlayerPrefs.Save.html).
  Taken: API, semantics, "flush on pause and quit". Left: registry/plist backends (the engine's existing
  per-platform stores stay; changing them would lose existing saves).
- Unity `Application.persistentDataPath`: per-user app data directories on desktop. Taken for desktop exports.
- JSON libraries for Lua: lua-cjson (`cjson.null`, `cjson.empty_array`, errors on sparse arrays), dkjson
  (`__jsontype` metatable field, `__tojson`), Roblox `HttpService:JSONEncode` (arrays = sequential integer keys).
  Taken: a null sentinel, array/object markers on metatables, `__tojson`, "contiguous 1..n = array".
- nlohmann json 3.11.3: `std::map`-based objects (sorted keys), shortest round-trip float output, `dump`
  error handler strict/replace/ignore for invalid UTF-8, `parse(..., ignore_comments)`, parse errors with byte
  positions. All used as is.

## 5. Product decisions

1. **One module `Json`** (PascalCase class, static functions) in Lua, implemented in C++ over nlohmann; no
   pure-Lua JSON.
2. **Object keys are always written sorted** (byte order): deterministic output for diffs and tests; no option.
3. **Array rule**: a table whose keys are exactly the integers 1..n (n >= 1) is an array; an **empty table is an
   array `[]`** unless marked `Json.object`; everything else is an object (integer keys written as decimal
   strings). Marked tables (`Json.array(t)`, `Json.object(t)`) always encode as marked. Decoding marks empty
   arrays/objects so `encode(decode(s))` keeps `{}` vs `[]`.
4. **null**: `Json.null` (a unique userdata) by default, so arrays keep their length and objects their keys;
   option `null = "nil"` drops them.
5. **Numbers**: Lua integers <-> JSON integers (int64); JSON numbers with a fraction or exponent, or outside
   int64, decode as floats; floats encode with the shortest round-trip form and keep a decimal point (`3.0`);
   NaN and infinities are an error (JSON cannot hold them).
6. **Errors raise with a path** (`Json.encode: cannot encode a function at $.player.onHit`); `tryEncode` /
   `tryDecode` return `nil, message` instead. Paths use Lua indices for arrays (`$.items[1]`).
7. **Engine math values encode as objects**: `Vector2/3/4` -> `{"x","y"[,"z"[,"w"]]}`, `Quaternion` ->
   `{"x","y","z","w"}`; other userdata error unless their metatable has `__tojson`.
8. **UserSettings keeps its per-platform stores** (XML on desktop/editor, SharedPreferences, NSUserDefaults,
   localStorage); this feature fills the missing operations and fixes their bugs. **`PlayerPrefs` is an alias
   module over the same keys** (Unity names, lowerCamel per engine style).
9. **Flush semantics**: every platform persists on `save()`, on app pause (background) and on shutdown/Play
   stop; desktop/editor additionally write at most once per second after a change (write-behind) instead of
   rewriting the XML file on every set (today, `XMLUtils.cpp:98,124`). Android/iOS/Web stores persist by
   themselves; `save()` forces a synchronous write where the platform has one.
10. **Desktop exports store user data per user** (`data://`): Linux `$XDG_DATA_HOME/<identifier>` (default
    `~/.local/share/<identifier>`), Windows `%APPDATA%\<identifier>`, macOS
    `~/Library/Application Support/<identifier>`, `<identifier>` = Project Settings `application.identifier`.
    Today it is the working directory (`SystemLinux.cpp:120`, `SystemWin.cpp:191`, `SystemMac.mm:177`), which is
    the install directory (not writable under Program Files). A one-time migration copies `./UserSettings.xml`
    when the new directory has none. Editor Play keeps `<project>/.jasperengine/user/rundata`.
11. **File helper paths**: `data://...` (writable, user data), `asset://...` and `lua://...` (read-only), a bare
    relative path means `data://`; `..` segments and absolute paths are rejected for writes (portable saves);
    absolute paths are allowed for reads on desktop only.
12. **Web waits for the IndexedDB restore** (`/datafs`, `JasperEngineWeb.cpp:357-362`) before the first scene
    starts (bounded at 3 s, then a warning), so saves are readable in `init`.
13. **Data assets** = JSON files (`Json.readFile("lua://data/items.json")`, exported with the Lua tree,
    `Exporter.cpp:1000`, or `asset://`) and Lua data modules (`require("data.items")`, shared tables).

## 6. Design

### 6.1 `Json` (C++ `engine/core/script/LuaJson.{h,cpp}`, binding in `IOClassesLua.cpp`)

C++ API (used by bindings, `http`, `native-bridge`):

```cpp
namespace jasperengine::LuaJson {
    enum class InvalidUtf8 { Error, Replace, Ignore };
    struct EncodeOptions { int indent = -1; /* -1 compact */ InvalidUtf8 utf8 = InvalidUtf8::Error; bool emptyAsObject = false; };
    struct DecodeOptions { bool nullAsNil = false; bool allowComments = false; };
    constexpr int MaxDepth = 512;

    // Lua value at `index` -> json; false + error ("cannot encode a function at $.a[2]")
    JASPERENGINE_API bool toJson(lua_State*, int index, nlohmann::json& out, const EncodeOptions&, std::string& error);
    JASPERENGINE_API void push(lua_State*, const nlohmann::json&, const DecodeOptions&);     // json -> Lua value
    JASPERENGINE_API bool encode(lua_State*, int index, std::string& out, const EncodeOptions&, std::string& error);
    JASPERENGINE_API bool decode(lua_State*, std::string_view text, const DecodeOptions&, std::string& error); // pushes 1 value on success
    JASPERENGINE_API void pushNull(lua_State*);  // Json.null
    JASPERENGINE_API bool isNull(lua_State*, int index);
}
```

Encode mapping (`toJson`), depth-first with a visited set (cycle -> error "cycle at <path>"), depth > 512 ->
error:

| Lua value | JSON |
|---|---|
| `nil` (top level only; inside tables nil cannot exist) | `null` |
| `Json.null` | `null` |
| boolean | `true`/`false` |
| integer (`lua_isinteger`) | integer |
| float, finite | number (`number_float_t`, prints `3.0`, `0.1`, `1e+300`) |
| float NaN/inf | error "NaN or infinity at <path>" |
| string | string; invalid UTF-8 per `utf8` option (default error "invalid UTF-8 at <path>") |
| table with metatable `__tojson` | encode the value `__tojson(self)` returns (must not return the same table) |
| table marked `Json.array` | array of `t[1..#t]` (`#` of the table; holes are an error "hole at <path>[i]") |
| table marked `Json.object` | object |
| unmarked table, keys exactly 1..n, n >= 1 | array |
| unmarked empty table | `[]` (or `{}` with `emptyAsObject`) |
| other unmarked table | object: string keys as is; integer keys as decimal strings; any other key type (float, boolean, table, ...) -> error "invalid key type <t> at <path>" |
| `Vector2`/`Vector3`/`Vector4`/`Quaternion` userdata | object `x,y[,z[,w]]` |
| other userdata, function, thread | error "cannot encode a <type> at <path>" (userdata with `__tojson` allowed) |

Decode mapping (`push`): `null` -> `Json.null` (or nil with `nullAsNil`: object members omitted, array
elements leave holes); booleans; `number_integer` -> Lua integer; `number_unsigned` <= INT64_MAX -> integer,
larger -> float; `number_float` -> float (so `1.0`, `1e2` are floats); strings; arrays -> tables 1..n; objects
-> tables with string keys (duplicate keys: last wins). Empty arrays/objects get the `Json.array` /
`Json.object` marker metatable. Parse errors map nlohmann's byte position to line/column:
`syntax error at line 3, column 14: unexpected '}'; expected string literal`. Depth > 512 -> error.

Markers: two registry tables `jasperengine.json.arrayMeta` / `objectMeta` (each `{__jsontype = "array"}` /
`"object"`; `__name` for `tostring`). `Json.array([t])`/`Json.object([t])` set it on `t` (or a new table) and
return it; a table that already has another metatable keeps it and gets `__jsontype` set in it (dkjson rule)
-- `toJson` reads `getmetatable(t).__jsontype`.

Lua API (`beginClass<LuaJsonModule>("Json")`, static functions):

| Function | Semantics |
|---|---|
| `Json.encode(value [, opts]) -> string` | opts: `pretty` (bool; indent 2), `indent` (integer >= 0, implies pretty), `invalidUtf8` (`"error"`/`"replace"`/`"ignore"`), `emptyTable` (`"array"`/`"object"`); unknown option keys error (catches typos) |
| `Json.decode(text [, opts]) -> value` | opts: `null` (`"sentinel"`/`"nil"`), `allowComments` (bool); `text` must be a string |
| `Json.tryEncode(value [, opts]) -> string \| nil, err` | never raises (except for invalid opts) |
| `Json.tryDecode(text [, opts]) -> value \| nil, err` | a valid JSON `null` returns `Json.null, nil` (so `nil` always means failure) |
| `Json.null` | static property, the sentinel; `tostring` is `"Json.null"` |
| `Json.isNull(v) -> bool` | |
| `Json.array([t]) -> t`, `Json.object([t]) -> t` | markers |
| `Json.readFile(path [, opts]) -> value \| nil, err` | `File.readText` + decode; a missing file returns `nil, "not found: <path>"`; a syntax error returns `nil, "<path>: syntax error at ..."` |
| `Json.writeFile(path, value [, opts]) -> true \| nil, err` | encode (opts as `encode`) + `File.writeText` (atomic) |

C++ scripts use nlohmann directly (`#include "json.hpp"`; the project CMake already has the include path,
root `CMakeLists.txt:446`); `cpp-scripting.md` says so.

### 6.2 UserSettings and PlayerPrefs

`System` (`engine/core/System.h:161-178`) gains virtuals `bool hasKey(const char*)`, `void removeAllKeys()`,
`void saveUserSettings()`, and `UserSettings` (`engine/core/io/UserSettings.h`) gains the static
`hasKey`, `deleteKey` (= `removeKey`), `deleteAll`, `save`.

Base implementation (desktop and editor, `System.cpp:205-270`, `XMLUtils.cpp:64-129`) becomes a
`UserSettingsXml` store:
- Loaded lazily once (tinyxml2 document of `data://UserSettings.xml`), kept in memory; getters read memory.
- Setters modify memory and mark dirty with a timestamp; `Engine::runFrame` end calls
  `UserSettings::tick()` which writes when dirty for >= 1 s; `save()`, `Engine::systemPause`,
  `Engine::systemShutdown`, the editor's Play stop and `System` destruction write immediately.
- Writes are atomic: `UserSettings.xml.tmp` then rename (an existing file is replaced; Windows `MoveFileExW`
  with `MOVEFILE_REPLACE_EXISTING`); on failure an error is logged once per session and memory stays dirty.
- `hasKey`: element present (an empty value is present). `getString` of a present empty value returns `""`
  (today it returns the default, `System.cpp:238`: fixed).
- Numbers are written locale-independently: integers decimal, float/double shortest round-trip
  (`std::to_chars`; fallback `snprintf("%.9g"/"%.17g")` under the C locale); read with `std::from_chars`
  (fallback `strtod` under the C locale). Today `std::to_string` writes 6 decimals in the current locale and
  `std::stoi` throws on bad data (`System.cpp:208-222`).
- Typed getters never throw: a value that does not parse returns the default and warns once per key.
- `deleteAll`: removes every child of `<userSettings>`.
- Bool stays `"true"`/`"false"`; data stays Base64 (format unchanged; existing files read as before).

Android (`UserSettings.java`, `JasperEngineAndroid.cpp:236-361`, `NativeEngine.{h,cpp}` method ids):
`hasKey` -> `sharedPref.contains(key)`; `removeAllKeys` -> `edit().clear().apply()`; `save` ->
`edit().commit()` (blocks until pending `apply()`s are on disk). Typed reads of another type keep returning
the default (`:16-22`).

iOS/macOS (`JasperEngineApple.mm:327-420`): `hasKey` -> `objectForKey:` != nil; `removeAllKeys` ->
`removePersistentDomainForName:[[NSBundle mainBundle] bundleIdentifier]`; `save` -> `synchronize`.

Web (`JasperEngineWeb.cpp:1173-1214`): keys `<storageId>/<key>` as today, with an explicit tombstone value
`"\u0001jasperengine:deleted"` for a hidden legacy bare key (an old build's empty-string tombstone, a prefixed
`""` whose bare key exists, still counts as deleted). `getStringForKey` returns a present `""`.
`hasKey`: prefixed present and not a tombstone, or bare legacy present and prefixed absent. `removeAllKeys`:
removes every key starting with `<storageId>/` and writes tombstones for bare legacy keys it has read this
session (it cannot know the other bare keys of the origin; documented). `save`: no-op (localStorage is
synchronous). Web also overrides the typed getters/setters so they round-trip through the same string format
as desktop (no `stoi` throw path).

Lua (`IOClassesLua.cpp:63-95`): `UserSettings.hasKey(k)`, `deleteKey(k)`, `deleteAll()`, `save()` added;
`removeKey` kept. New `PlayerPrefs` static class:

| PlayerPrefs | Same as |
|---|---|
| `getInt(k [, default=0])` / `setInt(k, v)` | `getIntegerForKey`/`setIntegerForKey` (`v` must be an integer: `3.5` errors "setInt: integer expected") |
| `getFloat(k [, default=0.0])` / `setFloat(k, v)` | `getFloatForKey`/`setFloatForKey` |
| `getString(k [, default=""])` / `setString(k, v)` | `getStringForKey`/`setStringForKey` |
| `hasKey(k)`, `deleteKey(k)`, `deleteAll()`, `save()` | UserSettings |

Mixed types (writing `setInt("a")` then `getString("a")`) are not specified across platforms (Android keeps
types, XML/localStorage do not); the docs say "read a key with the type you wrote it", like Unity.

### 6.3 File helpers (static functions on the existing `File` class, `IOClassesLua.cpp:29-61`; C++ `engine/core/io/FileUtils.{h,cpp}` `jasperengine::FileUtils`)

| Lua | Returns | Notes |
|---|---|---|
| `File.readText(path)` | `string` or `nil, err` | whole file, bytes as is; `asset://` on Android reads from the APK (`platformFopen`/`Data`) |
| `File.writeText(path, text)` | `true` or `nil, err` | atomic (temp + rename), creates missing parent directories, then `System::syncFileSystem()` |
| `File.appendText(path, text)` | `true` or `nil, err` | not atomic; creates the file |
| `File.exists(path)` | bool | files and directories |
| `File.remove(path)` | `true` or `nil, err` | files only; then `syncFileSystem` |
| `File.makeDirs(path)` | `true` or `nil, err` | |
| `File.list(dir)` | table of names (sorted, files and dirs, no `.`/`..`) or `nil, err` | `data://` only |

Path resolution (`FileUtils::resolve(path, forWrite, out, err)`): `data://x` -> user data path + `x`;
`asset://x`, `lua://x` -> read-only (writes: error "read-only path"); bare relative -> `data://`;
absolute -> allowed for reads on desktop/editor, error elsewhere and for writes; any `..` segment -> error.
Errors are messages like `"not found: data://save.json"`, `"permission denied: ..."`.

`System` Lua binding gains `System.syncFileSystem()` (Web: flushes `/datafs` to IndexedDB now; elsewhere
returns true) and `System.isUserDataLoaded()`. lua-api.md notes that Lua `io` writes on Web need
`System.syncFileSystem()` afterwards.

Desktop user data path (decision 10): `SystemLinux/SystemWin/SystemMac::getUserDataPath()` return the per-user
directory (created on first call) when `JASPERENGINE_APP_IDENTIFIER` is defined (the export's CMake defines
it from `application.identifier`, `Exporter.cpp` desktop configure step), else `"."` (engine tests, editor
builds of the project keep the current behaviour). Migration: if `<dir>/UserSettings.xml` is missing and
`./UserSettings.xml` exists, copy it once and log it.

Web restore wait (decision 12): `JasperEngineWeb` does not call the project's `init()` (the generated entry
that loads the first scene) until `isUserDataLoaded()` (`:304`) or 3 s have passed (warning
"user data not restored after 3 s; saves may be missing"). The canvas shows the loading state meanwhile.

### 6.4 Editor UX, MCP, exported builds

- Editor menu Edit > "Clear User Settings (PlayerPrefs)" with a confirm dialog: deletes the editor store
  (`.jasperengine/user/rundata/UserSettings.xml`); during Play it calls `UserSettings::deleteAll()` so the game
  sees it at once.
- MCP tool `user_settings` (ProjectTools or a new `DataTools.cpp`): `{action: "list"|"get"|"set"|"delete"|"clear",
  key, value, type: "string"|"int"|"float"|"bool"}`; list returns `{key: stringValue}` of the editor store;
  allowed while playing (goes through `UserSettings` so the running game sees changes); description <= 300 chars.
- `eval_lua` already lets agents call `Json.*`/`File.*`.
- Exported builds: no generated code except the `JASPERENGINE_APP_IDENTIFIER` define for desktop; all
  platforms compile the same `LuaJson`/`FileUtils`; Android/iOS/Web implementations as 6.2.
- Docs: lua-api.md 2.4 rewritten (UserSettings + PlayerPrefs + flush rules + platform storage table), new
  "2.x Json" and "2.x Files and saves" sections (rules tables, examples: save/load a table, remote config
  decode, data module), pitfalls (empty table = array; nil cannot be stored, use `Json.null`; integers vs
  floats; Web `io`), `cpp-scripting.md` (nlohmann), `unity-glossary.md` (PlayerPrefs, JsonUtility/Newtonsoft ->
  `Json`, `Application.persistentDataPath` -> `data://`, `File.ReadAllText` -> `File.readText`,
  ScriptableObject -> JSON/Lua data module, `Resources.Load<TextAsset>` -> `File.readText("asset://...")`).
- Stubs: generated from the bindings; `Json.null` typed as `JsonNull`; `opts` documented as `---@class
  JsonEncodeOptions` / `JsonDecodeOptions` in a fragment `stubs/Json.lua`.

## 7. Behaviour rules and edge cases

- `Json.encode(Json.decode(s))` equals `s` re-serialized with sorted keys, for any valid `s` whose numbers are
  representable (int64 or double) and whose strings are valid UTF-8.
- A table used both as array and object (`{1, 2, x = 3}`) is an object `{"1":1,"2":2,"x":3}`.
- Negative or zero integer keys make an object. `{[1]=a, [3]=c}` is an object `{"1":..,"3":..}` (not an array
  with a hole); marked `Json.array` with holes is an error.
- Strings with embedded zeros are fine (`lua_tolstring` length).
- Large integers: `9007199254740993` decodes as an exact integer; `1e400` is a syntax/range error from nlohmann
  (reported); `18446744073709551615` decodes as float.
- Re-entrancy: `__tojson` may call `Json.encode` (separate visited set).
- UserSettings during Play in the editor and in exports: the same API; Play stop flushes; a crash may lose at
  most 1 s of desktop changes (documented; call `save()` after important writes).
- Threads: `UserSettings`, `Json`, `File` helpers are main-thread APIs (Lua); `FileUtils` C++ functions are
  thread-safe for distinct paths (no shared state besides the XML store, which is guarded by a mutex).
- Web: `File.writeText` triggers the existing debounced sync (`File.cpp:54`, 500 ms) and `System.syncFileSystem()`
  flushes immediately; `Engine::systemPause` (tab hidden) also flushes.
- Scene loads do not affect any of this (process-wide stores).

## 8. Performance

- `Json.encode/decode` cost is nlohmann's plus one Lua table walk; target: 1 MB JSON decodes in < 30 ms on
  desktop. No per-call global state.
- UserSettings: getters O(log n) on the in-memory document (map index by key built at load); setters no
  disk I/O on desktop (write-behind); a frame without changes costs one branch in `tick()`.

## 9. Tests

gtest `editor/tests/LuaJsonTest.cpp` (Lua state from `Engine::systemInit` as other editor tests):
1. Encode scalars, nested tables, sorted keys, pretty/indent output (exact strings).
2. Array/object rules incl. empty table, markers, `emptyTable` option, mixed keys, integer keys -> strings,
   holes error, float key error with path `$.a`.
3. Numbers: `1` vs `1.0` both ways; `0.1` round-trip; int64 bounds; uint64; NaN error.
4. null sentinel both ways; `null = "nil"`; `tryDecode("null")` returns `Json.null`.
5. UTF-8: invalid byte -> error/replace/ignore; `é` and surrogate pairs decode.
6. Errors: syntax error line/column; depth 513 error; cycle error path; unknown option error.
7. Vector3/Quaternion encode; `__tojson`.
8. Round trip property over a corpus of 30 JSON files (nlohmann test-style samples written in the test).
gtest `editor/tests/UserSettingsTest.cpp` (temp user data dir):
9. `hasKey` after set (incl. empty string), `deleteKey`, `deleteAll`, defaults for missing keys.
10. Locale: set `LC_NUMERIC=de_DE` (when available) -> float written with a dot and read back exactly.
11. Bad stored value (`<k>abc</k>`) -> `getIntegerForKey` default, no throw.
12. Write-behind: set, no file write until `tick()` after 1 s (fake clock) or `save()`; atomic file present.
13. PlayerPrefs alias reads values written by UserSettings and vice versa; `setInt(3.5)` errors.
gtest `editor/tests/FileUtilsTest.cpp`: path rules (`..`, absolute, `asset://` write error), atomic write
(temp file not left behind), `list`, `Json.writeFile/readFile` round trip, missing file message.
Platform compile checks: Android Gradle build of the engine module (UserSettings.java + JNI), emsdk build of
`JasperEngineWeb.cpp`, macOS/iOS code compiled in CI where available (else reviewed).
MCP `tools/mcp/tests/test_script_data.py`: `user_settings set/list/clear`; a played script writes PlayerPrefs and a
JSON save; `eval_lua` reads them; after `play stop` the XML file contains the keys; `Json` error text via `eval_lua`.

## 10. Acceptance criteria

- [ ] `Json` API and rules exactly as 6.1; `LuaJson` C++ API exported for other features.
- [ ] `UserSettings.hasKey/deleteKey/deleteAll/save` and `PlayerPrefs` on desktop, editor, Android, iOS, Web.
- [ ] No typed getter throws on any platform; numbers locale-independent; present empty strings kept.
- [ ] Desktop write-behind + flush on save/pause/shutdown/Play stop; atomic writes.
- [ ] `File.*` helpers with path rules; `Json.readFile/writeFile`; `System.syncFileSystem/isUserDataLoaded`.
- [ ] Desktop exports store data per user (with migration); Web waits for the IndexedDB restore.
- [ ] MCP `user_settings`; editor menu item; docs, glossary, stubs (`test_stubs.py` green).

## 11. Integration points

`engine/libs/json/json.hpp`; `engine/core/script/binding/IOClassesLua.cpp:29-95`; `engine/core/io/UserSettings.{h,cpp}`;
`engine/core/System.{h,cpp}:154-178, 200-270`; `engine/core/util/XMLUtils.cpp:64-129`; `engine/core/io/File.cpp:54-90`;
`engine/core/io/FileData.cpp:119-143`; `engine/platform/android/java/.../UserSettings.java`,
`JasperEngineAndroid.cpp:198-361`, `NativeEngine.{h,cpp}`; `engine/platform/apple/JasperEngineApple.mm:277, 327-420`;
`engine/platform/emscripten/JasperEngineWeb.cpp:304, 357-362, 511-530, 1173-1214`; `engine/platform/linux/SystemLinux.cpp:120`,
`win/SystemWin.cpp:191`, `mac/SystemMac.mm:177`; `Engine.cpp:1336-1490, 1568-1592`; `editor/Platform.cpp:55`,
`editor/Project.cpp:7797-7808, 9152`; `editor/Exporter.cpp` (desktop define); `editor/mcp/tools/ProjectTools.cpp`.

## 12. Cross-feature dependencies

- **http** and **native-bridge** use `LuaJson::push/toJson` for bodies and bridge payloads (merge after this).
- **coroutines**: none required; `Json.readFile` is synchronous.
- **debug-overlay**: "Save log" uses `FileUtils` (`data://logs/...`).
- **platform-services**: none (app pause flush uses existing `Engine::systemPause`).
- Risk: decision 10 moves desktop saves; the migration copy covers UserSettings, not arbitrary files games
  wrote to the working directory (documented in the release note).

## Implementation notes

What was built differs from the text above in these points (product decisions taken while
implementing; everything else is as specified).

### C++ API (`engine/core/script/LuaJson.h`) for http / native-bridge / debug-overlay

- The names and semantics http and native-bridge rely on are exactly as 6.1:
  `LuaJson::toJson(L, idx, out, opts, err)`, `LuaJson::push(L, json, opts)` (opts defaults to
  `DecodeOptions{}`), `encode`, `decode`, `pushNull`, `isNull`, `EncodeOptions`,
  `DecodeOptions`, `InvalidUtf8`, `MaxDepth`. Added: `LuaJson::parse(text, json&, opts, err)`
  (the depth check and the formatted syntax errors of `decode`, without pushing) and
  `markArray` / `markObject`. **http and native-bridge should parse text with `LuaJson::parse`
  or `LuaJson::decode`** (not `nlohmann::json::parse` directly) to get the 512-level limit and
  the "syntax error at line L, column C: ..." messages; `push` of a C++-built json nested deeper
  than 512 pushes `Json.null` there (warned once) instead of failing, since `push` returns void.
- `toJson` with `InvalidUtf8::Replace` / `Ignore` sanitizes strings while converting (maximal
  ill-formed subpart -> one U+FFFD), so the json it returns always dumps without an error
  handler; with `Error` it fails with the path. The stack is left as it was in every case.
- Debug-overlay: `jasperengine::FileUtils` (`engine/core/io/FileUtils.h`) has the
  static functions of 6.3 plus `resolve(path, forWrite, normalized, systemPath, error)` (the
  absolute path to log), `writeFileAtomic`, `renameFile` and `prepareUserDataFolder`. It is a
  class in `jasperengine`; editor code inside `jasperengine::editor` still sees its own
  `editor::FileUtils` first, so nothing clashes.

### Json rules and messages

- Error texts: `cannot encode a <type> at <path>`, `invalid key type float at $.a`,
  `hole at $.list[2]`, `cycle at $.self`, `NaN or infinity at $.x`, `invalid UTF-8 at $.name[1]`,
  `invalid UTF-8 in a key at $`, `nesting deeper than 512 at $[1]...` (encode) / `... at line 1,
  column 513` (decode), `__tojson failed (<message>) at $`, `__tojson returned the value itself
  at $`, `__tojson is not a function at $`. The raising functions prefix `Json.encode: ` /
  `Json.decode: `; `tryEncode` / `tryDecode` return the bare message.
- New rule: a table with both the integer key `1` and the string key `"1"` (they would become
  the same JSON key, and Lua's iteration order would pick the winner) is an error
  `duplicate key "1" at $ (an integer and a string key)`.
- A table marked `Json.array` encodes `t[1..rawlen(t)]` with raw access; its non-integer keys
  are ignored. Marking a table that carries one of the two marker metatables switches the
  marker (the shared marker metatables are never modified); a table with a metatable of its
  own gets `__jsontype` set in it, as specified. `Json.array(5)` raises.
- `Json.null` is a full userdata whose metatable is protected (`getmetatable(Json.null)` is the
  string `"Json.null"`), so scripts cannot alter it.
- Decoding: a table that ends up empty is marked by what it was in JSON (so `{"a":null}` with
  `null = "nil"` re-encodes as `{}`). A UTF-8 BOM at the start of a file is skipped (nlohmann).
- `Vector2/3/4` and `Quaternion` floats are written as the shortest text that reads back as the
  same float (`0.1`, not `0.10000000149011612`).
- `indent` accepts 0..64 (a larger value is surely a mistake). `Json.writeFile` returns
  `nil, message` for an encode error too (it never raises for the value; only for wrong
  argument types, a missing value argument or bad options).

### UserSettings / PlayerPrefs

- The desktop/editor store is `engine/core/io/UserSettingsStore.{h,cpp}` (the spec's
  "UserSettingsXml"). The file format is extended compatibly: keys that are not plain XML names
  (spaces, `/`, `:`, a leading digit, non-ASCII letters) are written as
  `<entry key="...">value</entry>`; values XML text cannot hold exactly (NUL, CR, other control
  characters, or nothing but whitespace, which XML parsers drop) get `encoding="base64"`. Keys
  are written sorted. Before, such keys produced a file that no longer parsed and every key was
  lost. Files of older builds read as before; a file that is not valid XML is moved aside to
  `UserSettings.xml.corrupt` (logged) instead of being overwritten; a file that is there but
  cannot be read (permissions, a folder in its place) is never replaced: the values work in
  memory for the session and an error says they are not saved.
- Locale independence uses a small helper `engine/core/util/NumberText.{h,cpp}` (shortest
  round-trip text through `%.*g` with the locale's decimal point replaced, and the reverse for
  reading): `std::to_chars`/`from_chars` for floating point are unavailable on the Apple
  deployment targets (macOS 12, iOS 15). Readable forms: `100`, not `1e+02`. Old values written
  with a decimal comma (`0,500000`, an older build in a German locale) are read too.
  `getBoolForKey` also accepts `1`/`0`.
- Empty keys are refused (logged): they could not be stored on any platform before either.
- The store follows `System::instance()`: when another System is installed (tests, tools) it
  writes what the old one had unsaved and reads the new one's file; the editor calls
  `UserSettingsStore::instance().unload()` when it switches projects, so Play always uses the
  open project's `.jasperengine/user/rundata/UserSettings.xml`. `~System` writes what is
  unsaved. A failed write is retried a second later (not every frame).
- Android: besides `hasKey` / `removeAllKeys` / `save` (`commit()`), strings now cross JNI as
  UTF-16 (`newJavaString` / `javaStringToUtf8`), so characters outside the BMP (emoji) survive;
  the old code used modified UTF-8 and leaked `GetStringUTFChars`. Java exceptions are cleared.
- Apple: typed getters accept a stored `NSNumber` or a numeric `NSString` (older builds'
  strings) and return the default for other types instead of raising an Objective-C exception;
  a key or value that is not valid UTF-8 is refused (logged) instead of crashing in
  `setObject:forKey:nil`; strings keep embedded NULs. `removeAllKeys` on a bare executable
  (no bundle identifier) removes the keys of the process's domain one by one.
- Web: instead of tombstones for "bare legacy keys read this session" (which left the other
  legacy keys of the origin visible after `deleteAll`), `deleteAll` sets a marker
  `<storageId>/\u0001jasperengine:cleared` after which no bare legacy key is read any more. Old
  builds' empty-string tombstones are converted to the new tombstone once per storage id
  (marker `<storageId>/\u0001jasperengine:v2`), so a stored `""` is an ordinary value from then
  on. localStorage exceptions (storage disabled, quota) are caught: reads give the default, a
  failed write is logged once. The web build needs no typed getter overrides: the base class's
  typed getters go through the virtual `hasKey` / `getStringForKey` and never throw.
- Web flush on tab hide: the web platform never calls `Engine::systemPause` for a hidden tab, so
  a `visibilitychange` listener writes pending files to IndexedDB at once (no pause behaviour
  was added, to stay out of the way of platform-services' focus events).
- New virtual `System::flushFileSystem()`: Lua `System.syncFileSystem()` calls it (the web build
  syncs to IndexedDB now); `System::syncFileSystem()` stays the debounced request that
  `File.writeText` and `File::write` make.
- Web restore wait: `Engine::systemInit(argc, argv, system, startProject = true)` and a new
  `Engine::systemStartProject()`; the web build passes `false` and starts the project from its
  render loop once IndexedDB is restored (or after 3 s with the warning), then lays the view out
  again (`systemViewChanged`) because `init()` sets the canvas size after the view exists.
  Nothing is drawn meanwhile (the canvas stays blank; the restore takes milliseconds).
- `PlayerPrefs.setInt` accepts a float with an integer value (`6 / 2`), refuses `3.5` ("integer
  expected, got 3.5") and values outside 32 bits (pointing at `UserSettings.setLongForKey`);
  `setString` takes only strings, `setFloat` only numbers; keys must be non-empty strings.
  Getters' defaults are type-checked the same way.
- MCP `user_settings` saves to disk after each set / delete / clear, infers `type` from the JSON
  value (integer -> int, number -> float, boolean -> bool, string -> string), lists the stored
  text of every key and reports the store's project-relative file. Its busy activity is
  `Control` (the store is no project content, so an export running does not refuse it).
- The Edit menu item reads "Clear User Settings (PlayerPrefs)..." (it opens a confirm dialog);
  it clears the shared store and saves, so a running game sees it at once.

### Files and desktop paths

- `File.remove` of a missing file returns `true` (as .NET's `File.Delete`); a folder is refused.
  `File.list` refuses non-`data://` folders. A `:` inside a path segment is refused (portable
  saves; `data://C:/x`), and `\` separates like `/`. On Android `asset://`/`lua://` existence is
  probed through `platformFopen` (the APK), never the device's file system.
- Desktop identifier: the same rule as the web storage id (Project Settings
  `application.identifier`, or the project name when it is the shared default), made safe for a
  folder name by the exporter's `toStorageId` (bytes other than letters, digits, `.` and `-`
  become `_XX`). The define is written for every export mode (only the desktop System classes
  read it). `SystemWin::platformFopen` now opens UTF-8 paths (`_wfsopen`), so a per-user folder
  under a non-ASCII user name works; it was ANSI before.
- The export's per-user folder also moves the runtime shader cache of Linux/Windows desktop
  exports there (it follows `getUserDataPath`), which fixes the same not-writable install folder
  problem for it.

### Docs, agent guide, tools

- The agent guide (`McpInstructions.cpp`) is at its 16 KB budget: the new "Saves" line was paid
  for by shortening three lines (the editor windows list, the LuaLS sentence, "(seconds to
  minutes)").
- `docs/mcp/TOOLS.md` gets a "Saved data (PlayerPrefs)" section (`DataTools.cpp`).
- The Xcode workspace (`engine/workspaces/xcode/JasperEngine.xcodeproj`) lists the four new
  engine sources.

### Verification

- Not verifiable on this host: the Apple code (`JasperEngineApple.mm`, `SystemMac.mm`) and the
  Windows code (`SystemWin.cpp`, the `_WIN32` parts of `FileUtils.cpp`) were written carefully
  but not compiled. Android (Java through Gradle, the JNI and core files with the NDK clang for
  arm64-v8a) and Web (em++) were compiled; the web localStorage store was exercised in node.
