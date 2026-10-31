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
