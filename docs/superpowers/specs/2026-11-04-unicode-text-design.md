# Unicode text: text and paths in every language just work (design)

Date 2026-11-04. Branch `dc/unicode-text`. Program: Doriax catch-up, ledger
[`docs/doriax-catchup/LEDGER.md`](../../doriax-catchup/LEDGER.md) (feature 1). Owner decisions that bind
this spec: Windows moves to the UTF-8 code page without a Windows test first; old projects with
ANSI-encoded YAML are not supported (no legacy YAML conversion).

Everything below about Windows, macOS, iOS and Android behaviour was read from the code, not run
(none of those builds here). Line numbers are hints from `main` at `b4f7816ea`.

## 1. Goals

1. **One text encoding everywhere: UTF-8.** Every `std::string` that holds text or a path is
   UTF-8 on every platform, in the editor, the CLI, games and the test executables. On Windows the
   process runs in the UTF-8 active code page (manifest), so `path::string()`, `fs::path(std::string)`,
   `fopen`, `getenv` and the `-A` Win32 functions are UTF-8 as they are on Linux and macOS.
2. **Text from outside arrives as UTF-8.** Child-process output (compilers, CMake, vswhere, Python),
   the console, the log, and Apple bundle paths are converted at the boundary; invalid bytes never
   reach a log file, the Console window or an MCP reply.
3. **Project files are portable.** Every path the editor stores in a project file uses `/`; paths
   saved with `\` (by a Windows editor before this change, or typed by an agent) load on every
   platform.
4. **Typed characters arrive whole.** A character beyond U+FFFF (emoji, rare CJK) typed on Windows
   or Android reaches the component InputField, `Engine.onCharInput` and the editor's own text
   fields as one character, never as two halves or two U+FFFD.
5. **The editor shows every language.** Chinese, Japanese and Korean names and text in the editor
   (Hierarchy, Inspector, Project window, code editor, Console) draw with the system's CJK fonts,
   with the Han glyph forms of the user's language, without slowing the editor's start.
6. **Agents can drive and check it.** `send_input` can type text the way Windows and Android
   deliver it (one UTF-16 unit at a time); `editor_status` reports the editor's fallback fonts; the
   CJK font language is a preference reachable through the settings tools.
7. **The class of bug stays fixed.** A lint (`tools/check_text_encoding.py`) fails on the patterns
   that caused these bugs (`CP_ACP`, ANSI Win32 path APIs, `NSASCIIStringEncoding`, vswhere without
   `-utf8`).

## 2. Non-goals

- **Converting ANSI-encoded YAML from older editors** (Doriax `TextEncoding::loadYamlFile`): owner
  decision, no such projects exist.
- **Guards at each project walk** (Doriax `isNarrowPath`, 62 `.string()` sites): the editor runs
  only in the UTF-8 code page (decision 2), where `string()` cannot throw for a valid name.
- **Runtime CJK or emoji glyphs in games.** A game's TextComponent and HTML UI draw with the game's
  fonts; a game that shows CJK ships a CJK font (a system fallback would need a platform font API
  per target and 10-20 MB fonts on web; Unity also needs a font asset with the glyphs). The editor's
  Game view draws exactly what the export draws.
- **Color emoji in the editor.** ImGui rasterises with stb_truetype, which cannot draw COLR, sbix or
  CBDT glyphs; emoji show in monochrome where the system has an outline emoji font (Windows), as a
  missing-glyph box elsewhere. Their text is never altered (goal 4).
- **Unicode normalization (NFC/NFD) of file names.** Names are compared as bytes; APFS preserves the
  form a name was created with and looks names up normalization-insensitively.
- **Names that are not valid Unicode** (an NTFS name with a lone UTF-16 surrogate, created only by
  broken programs). They cannot be written into a UTF-8 project file at all; an editor walk that
  meets one fails as it does today. Explorer cannot create such a name.
- **MinGW-built editors.** libstdc++'s `filesystem::path` converts narrow strings through the C++
  locale, not the active code page (inferred; not checked on a MinGW build). MSVC is the editor's
  Windows toolchain (CI builds with `cl`); MinGW editor builds are documented as ASCII-paths-only.
  Games built with MinGW are fine: the engine runtime converts paths explicitly
  (`engine/core/io/FileUtils.cpp:53-77`, `SystemWin::platformFopen`).
- **The Android caret in mid-text.** The component InputField diff still assumes edits at the end of
  the text, as today (only the units it counts change).

## 3. Users and stories

- A developer in Shanghai on Windows names a folder `关卡` and a scene `第一关.scene`: the Project
  window shows them, renames work, the scene saves, Play and export work. Today, on a Western code
  page, `path::string()` throws in the first walk that meets the name (Project window, asset root
  change, terrain cleanup, export) (inferred).
- A developer in Tokyo on an English Windows sets Preferences > General > Editor Font >
  **CJK Font Language** to Japanese: kanji in the Hierarchy use Japanese glyph forms.
- A player on Windows types `Hi 😀` into a component InputField: the field holds `Hi 😀`. Today it
  holds `Hi ��` (inferred from `UISystem.cpp:1389`, `StringUtils.cpp:15-17`).
- A player on Android edits a field that already holds `😀`: the emoji survives the first keystroke.
  Deleting it takes one backspace, not two.
- A team shares a project between Windows and Linux: a texture picked in a `.material` on Windows
  loads on Linux (`textures/wood.png`, not `textures\wood.png`), and a window icon in a subfolder
  survives the Android export.
- A Brazilian developer's MSVC build fails with `C:\Users\João\...\Player.cpp(12): error C2065`: the
  Build dialog and `editor.log` show `João`, not `Jo?o` or bytes that break the MCP reply.
- An agent checks a fix: `send_input {type:"text", text:"a😀b", per_unit:true}` types the string as
  Windows `WM_CHAR` does, then `get_component` reads the Text: `a😀b`. `editor_status` lists
  `editor_fonts.fallbacks` to confirm CJK coverage before taking a screenshot of the Hierarchy.
- A Lua script `function M:onCharInput(c) self.typed = self.typed .. c end` collects `😀` as one
  string on every platform.

## 4. Findings

Doriax commits are references (`/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/doriax`).
Every dossier item is listed; "covered" means this spec fixes it through another item.

| # | Doriax | What is wrong in Jasper (path:line) | This spec |
|---|---|---|---|
| F1 | `e0a30fe8a` UTF-8 code page | `engine/platform/win/jasperengine.manifest:9-12` has no `activeCodePage`; `editor/util/CommandRunner.cpp:734,738` widen arguments with `CP_ACP`; `:703` emits child output raw; `editor/cli/CommandLine.cpp:611-622` attaches the console without setting its code page; encodings are mixed across ~1,400 `.string()` lines and ~1,000 `pathToUtf8/pathFromUtf8` lines in the editor. The manifest helper reaches the editor, `-cmd` and games (`CMakeLists.txt:1525,1673`, `engine/CMakeLists.txt:2313`), not the test executables (`engine/cmake/JasperTesting.cmake:131,151`). | §6.1, §6.2 (without `loadYamlFile`) |
| F2 | `5243947a4` ANSI in the log | `editor/Out.cpp:64-67` passes text to `Logger::write` unchanged; `engine/core/log/Logger.cpp:1502` has no UTF-8 check; only MCP repairs (`editor/mcp/McpServer.cpp:630`). | §6.3 (in the engine logger, for every sink) |
| F3 | `baf4a8442` asset roots crash | `editor/Project.cpp` `visitMaterialFiles` (~:2288) calls `entry.path().string()` over the project, reached from `changeAssetRoots`; window path has no catch (`ProjectSettingsWindow.cpp` ~:4380). | covered by F1 (§6.1); Windows test W2 |
| F4 | `fab67c871` names outside ANSI in scans | Unguarded `.string()` in walks: `ProjectWindow.cpp` (~:2428, :2587, :2620), `Project.cpp` (~:9188, :2246, :2288), `CodeEditor.cpp:444,450`, `UIUtils.cpp:17`, `TerrainEditWindow.cpp:468,472,558`, `Generator.cpp:1201`, `Exporter.cpp` (5 sites); the Exporter catches only `fs::filesystem_error`. | covered by F1; decision 2 replaces guards |
| F5 | `be679524e` vswhere `-utf8` | `editor/Generator.cpp:3145,3164` (displayName), `editor/util/CommandRunner.cpp:121` (installationPath), `editor/util/ExternalCodeEditor.cpp:44` (productPath) run vswhere without `-utf8`: it prints in the console code page. | §6.2 (one helper for all four) |
| F6 | `84bb05ab0` CJK editor fonts | `editor/App.cpp:2375-2444` merges only Noto Sans Arabic into the UI, bold and code fonts; no system font lookup exists. | §6.6 (async, CoreText, preference) |
| F7 | `88d9daa04` wchar_t cast in text edits | `engine/core/util/StringUtils.cpp:12,209,224` and `.h:15`: `toUTF8(wchar_t)`; every InputField insert or erase re-encodes the whole text through `static_cast<wchar_t>(cp)`, so on Windows U+1F600 becomes U+F600. `editor/util/BidiText.cpp:301` does the same. Linux cannot show it (32-bit `wchar_t`); `engine/tests/core/util/StringUtilsTest.cpp:4-5` records it as a limit. | §6.4 |
| F8 | `b6f659426` surrogate halves to InputField | `engine/core/Engine.cpp:3558-3589`: documents get combined code points (`Utf16Combiner`), but `onCharInput` and `UISystem::eventOnCharInput` (`UISystem.cpp:3265`) get each raw unit, and `handleInputFieldCharInput` (`:1389`) turns each half into U+FFFD. `sendCharacterToGame` (`Engine.cpp:2951-2972`) splits whole code points back into halves where `wchar_t` is 16 bits; `editor/window/PlayInput.cpp:196-221` does the same. Halves come from `WinInputRouter.cpp:388` and `NativeEngine.cpp:950`. | §6.4 |
| F9 | `6712bbc39` Android outside-BMP input | `engine/platform/android/NativeEngine.cpp:307` keeps `textInputBuffer` as `std::wstring`; `showSoftInput` (`:847-848`) stores code points, the component diff (`:932-952`) stores and compares UTF-16 units and sends one `'\b'` per unit and each surrogate to `systemCharInput`. A field holding an emoji is damaged on the first keystroke; deleting an emoji sends two backspaces. Encoder, decoder and selection counts are already right (`:332`, `:419`, `:851-862`). | §6.5 |
| F10 | `18655c01a` 4-byte UTF-8 from ART | Independently solved: `NativeEngine.cpp:330-352` accepts plain 4-byte UTF-8. | context only |
| F11 | `07659877a` picked paths with `\` | `editor/window/InspectorWindow.cpp:1145` (font), `:4709` (texture), `:4989` (cube map), `:5104` (cube face), `:7777` (model) pass `relative.string()`. Scenes and prefabs are rewritten by the asset resolver at save (`editor/project/ProjectAssets.cpp:107-117`), but a `.material` file is not: `bindMaterialAssetRefs` (`ProjectAssets.cpp:178-199`) sets only the GUID, and opening a scene elsewhere copies the hint verbatim (`Project.cpp` ~:4411). | §6.7 |
| F12 | `210051960` project.yaml with `\` | `editor/Stream.cpp:2557,2561,2564,2841,2844,2847` write `windowIcon`, `assetsDir`, `luaDir`, `launcherIcon`, `adaptiveIconForeground/Background` with `.string()`; the loader reads them verbatim (`Stream.cpp` ~:3196-3204, ~:3554-3556). Settings rows produce `\` on Windows (`lexically_normal`, `SettingsRows.cpp:195,2118`). | §6.7 |
| F13 | `90b816356` script paths with `\` | Refuted by the skeptic (the resolver rewrites script refs at save), but `InspectorWindow.cpp:2343-2348,11781-11782` still produce `\`; a script not yet in the database keeps it. | §6.7 (sweep + encode/decode rule) |
| F14 | `f691fa43c` Apple bundle paths | `engine/platform/apple/JasperEngineApple.mm:290,340,356` return `[path cStringUsingEncoding:NSASCIIStringEncoding]`: NULL for a non-ASCII path, and `std::string(NULL)` crashes at start (Xcode-built macOS/iOS app under a non-ASCII folder). `engine/platform/mac/SystemMac.mm:31` already uses `UTF8String`. | §6.8 |
| F15 | `3b274f1eb` MCP UTF-8 paths | Already ported (`editor/mcp/McpRefs.cpp:437-438`); the residual native-encoding sites (`SceneTools.cpp:518`, `OrganisationTools.cpp:391,736`, `AssetIdentityTools.cpp:44,165`) become UTF-8 with F1. | covered by F1 |
| F16 | `13d0993e9` accented project paths | Covered by F1, except `editor/window/dialog/ProjectSaveDialog.cpp:59,61`, which dereference `getenv("USERPROFILE"/"HOME")` without a null check; `editor/util/FileUtils.h:56` uses `GetModuleFileNameA`. | §6.1, §6.9 |
| F17 | `285ea7bce` Resources window names | Covered by F1 (Jasper's Project window: `ProjectWindow.cpp:1329,1337,2457,2620,2896,2909,2948-2952`). | covered by F1 |
| F18 | `237d54595` fork dialog paths | Covered by F1 (`ScriptCreateDialog.cpp:531,607,608,611`, `ShaderForkDialog.cpp:77`). | covered by F1 |
| F19 | `fbed5b422` Save Scene name | Covered by F1 (`SceneSaveDialog.cpp:99-115`). | covered by F1 |
| F20 | `ea70df4c9` build script output | `engine/jasperengine.py:214-215` prints commands with paths through Python's piped stdout, which uses the ANSI code page on Windows and raises on a character outside it; `open()` at `:55,105` uses the locale encoding. | §6.10 |
| F21 | `29567a924` terrain map scan | Jasper never skipped files, but `TerrainEditWindow.cpp:468,472,524-534,558,577,580` call `.string()` per entry; a throw becomes a failed save (`Project.cpp` ~:7290-7304). | covered by F1 |
| F22 | (ours) test executables | Tests do not get the manifest (`JasperTesting.cmake`), so on Windows they would test another code page than users run. | §6.1 |
| F23 | (ours) engine runtime on old Windows | Games keep running before Windows 10 1903 (decision 2), but `engine/core/diag/CrashHandlerWin.cpp:57,167` (`CreateFileA`, `CreateDirectoryA` on the user-data path), `engine/core/log/Backtrace.cpp:145` (`GetModuleFileNameA`), `engine/core/log/sinks/DebuggerSink.cpp:25` (`OutputDebugStringA`) and `engine/platform/win/WindowWin.cpp:112-130` (`CP_ACP` fallback) depend on the code page. | §6.1 |
| F24 | (ours) `Engine.onCharInput` contract | Lua gets a one-character string (`LuaFunctionBase.cpp:159`), while `docs/mcp/guide/lua-api.md:259` and `editor/util/ScriptEvents.cpp:72` call it `codepoint`; on Windows a handler gets two U+FFFD strings for one emoji. | §6.4 |
| F25 | (ours) editor text fields drop emoji | ImGui is built with 16-bit `ImWchar` (`libs/imgui/imconfig.h:70`), so a typed or pasted character beyond U+FFFF becomes U+FFFD in Inspector fields and the code editor (`imgui.cpp:2706,11216`). | §6.6 |

## 5. Product decisions

1. **UTF-8 code page for every Windows executable we build**: editor, `jasperengine-editor-cmd`,
   games and prebuilt players, and the test executables (so tests see what users run). Reason:
   one line fixes the 60+ walk sites, the dialogs, MCP paths and the CLI at once, and it is
   Microsoft's documented way to make narrow APIs UTF-8. The process helpers that stand for foreign
   programs in tests (`je_log_crash_helper`, `je_vcs_process_helper`) stay without it, on purpose.
2. **The editor and CLI require Windows 10 version 1903 or later; games do not.** At start, on
   Windows, the editor checks `GetACP() == CP_UTF8`; otherwise it refuses to start with a clear
   message (a message box when started from Explorer, stderr otherwise) and exits 1. Reason: on
   older Windows the manifest is ignored and every unguarded `.string()` crashes on a name outside
   the ANSI code page, which corrupts work silently; guarding 62 sites forever costs more than
   following Unity (Unity 6's editor needs Windows 10 21H1). The same check catches a build that
   lost its manifest. Games keep running on older Windows: the runtime converts explicitly, and the
   remaining code-page dependent calls are removed (F23).
3. **Child output is UTF-8 by rule "valid stays, invalid is the console code page".** Each line a
   child writes is kept when it is valid UTF-8; otherwise it is decoded from the system's OEM code
   page on Windows (what `cl`, `link` and `cmd` write to a pipe) and repaired with U+FFFD
   elsewhere. Python children get `PYTHONUTF8=1` (and `PYTHONIOENCODING=utf-8`) unless the caller's
   environment sets them, so Emscripten and `jasperengine.py` print UTF-8. Reason: no per-tool
   tables, and git (already UTF-8 through its own runner) is unaffected.
4. **The log is valid UTF-8 at the engine logger**, not only in the editor's `Out`: every record,
   from any thread, editor or game, is checked in `logSubmit`; invalid text is converted from the
   system ANSI code page on Windows (a pre-1903 game's `e.what()`) and repaired with one U+FFFD per
   invalid byte elsewhere (the rule MCP already uses). Reason: one choke point covers the log file,
   the Console, the platform sinks and MCP.
5. **Project files store `/`, and `\` in a stored path is a separator on every platform** (Unity's
   AssetDatabase rule). The editor writes `/` at the source (pickers) and again at the encoder; the
   decoder turns `\` into `/` in path fields, so files written with `\` load everywhere without a
   format change or version bump. A name containing `\` on Linux/macOS cannot be referenced from a
   project file (Windows cannot hold such a name either).
6. **Character input carries whole code points end to end.** `Engine::systemCharInput` takes
   `char32_t` (one UTF-16 unit or one code point); the engine combines halves once, and documents,
   `Input.inputString`, `Engine.onCharInput` and the component UI each get whole code points.
   `Engine.onCharInput` becomes `FunctionSubscribe<void(char32_t)>`; Lua handlers keep receiving a
   one-character UTF-8 string (now never half a character); C++ handlers written as
   `void onCharInput(wchar_t)` still compile. Reason: the same event must mean the same thing on every
   platform; today Windows and Android scripts see halves, Linux and macOS whole characters.
7. **CJK fonts are found off the main thread and merged before a frame.** The editor starts with its
   bundled fonts; a background job finds the system CJK fonts (Windows: known files in the Fonts
   folder; macOS: CoreText by family; Linux: `fc-match`, then known paths), memory-maps them and the
   main thread merges them into the UI, bold and code fonts before the next `ImGui::NewFrame`.
   Reason: Doriax runs `fc-match` on the main thread at start (up to 3 s); ImGui 1.92.8 supports
   merging into an existing font later (`ImFontConfig::DstFont`, `imgui_draw.cpp:3084`), so start
   time does not change.
8. **Han glyph forms follow a preference, Auto by default.** Preferences > General > Editor Font >
   **CJK Font Language**: Auto (the system language), Japanese, Korean, Simplified Chinese,
   Traditional Chinese; applied at the next start (said in the row). Reason: Han unification draws
   the same code point differently per language; a Japanese developer on an English system would
   otherwise read Chinese forms. Unity has no such row because it bundles per-language fonts.
9. **The editor's ImGui uses 32-bit characters (`IMGUI_USE_WCHAR32`)** and merges a system outline
   emoji/symbol font where one exists (Windows `seguiemj.ttf`, `seguisym.ttf`; Linux fontconfig's
   outline `emoji` match). Reason: an Inspector field holding an emoji must not lose it when edited;
   the cost is a few hundred KB of glyph index per font size, only once such a character is drawn.
10. **vswhere is run through one helper that always adds `-utf8`.** Reason: four call sites today,
    and the next one must not be able to forget it (the lint forbids calling vswhere elsewhere).
11. **A lint instead of a sweep of `.string()`.** With decision 2, `.string()` is UTF-8 everywhere the
    editor runs; the lint forbids only what still breaks (`CP_ACP`, ANSI Win32 calls, non-UTF-8
    `cStringUsingEncoding:`, vswhere outside its helper). Reason: 1,400 mechanical edits across 156
    files would collide with every other branch of the program for no behaviour change.
12. **No new MCP tool.** `send_input` gets `per_unit`, `editor_status` gets `editor_fonts`, and the
    preference is a settings key (`preferences.cjk_font_language`). Reason: conventions rule 1
    (extend before adding).

## 6. Design

### 6.1 Windows: the code page, the startup check, the runtime's own calls

- `engine/platform/win/jasperengine.manifest`: add
  `<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>`
  inside `<windowsSettings>`, and extend the header comment ("activeCodePage makes narrow strings
  UTF-8, so any file name works as on the other platforms. Windows before 10 1903 ignores it; the
  editor then refuses to start, games keep running."). `WindowsManifest.cmake` header comment names
  both settings. MSVC merges the file through `/MANIFESTINPUT`; MinGW embeds it as `RT_MANIFEST`
  (unchanged mechanism).
- `engine/cmake/JasperTesting.cmake`: both branches call `jasperengine_add_windows_manifest(${name})`
  when `WIN32` (include `${JASPERENGINE_ROOT}/platform/win/WindowsManifest.cmake` once at the top, or
  the editor tree's include at `CMakeLists.txt:200` when the root differs). Also
  `jasperengine-crash-test` (`engine/tests/core/CMakeLists.txt:31`). The process helpers
  (`editor/tests/EditorTests.cmake:20,46`) stay without it, with a comment saying why.
- New `editor/util/TextEncoding.{h,cpp}` (namespace `jasperengine::editor`, class `TextEncoding`):
  - `static std::optional<std::string> codePageProblem(unsigned activeCodePage)`: `nullopt` for
    65001, else the message "JasperEngine Editor needs Windows 10 version 1903 or later: this Windows
    ignores the UTF-8 setting of the editor, so names in other languages would corrupt projects.
    Update Windows to use the editor (games made with it still run here)." Pure, tested everywhere.
  - `static bool checkWindowsCodePage()`: on Windows, calls `codePageProblem(GetACP())`; on a problem,
    writes it to stderr when `GetStdHandle(STD_ERROR_HANDLE)` is a valid console or pipe handle,
    else shows `MessageBoxW` (the message widened with `CP_UTF8`); returns false. Elsewhere true.
  - `static void useUtf8Console()`: on Windows, when stdout or stderr is a console
    (`GetConsoleMode` succeeds), remembers `GetConsoleOutputCP()` and sets `SetConsoleOutputCP(CP_UTF8)`,
    restoring the old value with `std::atexit`; idempotent. Elsewhere nothing. Input code page is
    left alone (no interactive console input; `ReadConsoleA` in UTF-8 is unreliable before Windows 11).
  - `static std::string consoleToUtf8(std::string_view line)` and
    `static std::string legacyToUtf8(std::string_view text)`: see §6.2, §6.3.
- `editor/main.cpp` `main()`: first statement (before `Out::startLogging`):
  `if (!editor::TextEncoding::checkWindowsCodePage()) return 1;` then `TextEncoding::useUtf8Console();`.
  `editor/cli/CommandLine.cpp` `attachHostConsoleIfNeeded()` calls `useUtf8Console()` after
  `AttachConsole` (the console did not exist when `main` ran).
- Engine runtime calls that must not depend on the code page (games still run before 1903):
  - `engine/core/diag/CrashHandlerWin.cpp`: the session folder and file paths are widened once when
    the handler is installed (`MultiByteToWideChar(CP_UTF8, ...)` into the handler's static state)
    and the crash path uses `CreateDirectoryW` / `CreateFileW` (no allocation in the handler, as
    today).
  - `engine/core/log/Backtrace.cpp:145`: `GetModuleFileNameW` into a stack buffer, then
    `WideCharToMultiByte(CP_UTF8, ...)` into the existing char buffer.
  - `engine/core/log/sinks/DebuggerSink.cpp:25`: `OutputDebugStringW` of the record widened with
    `CP_UTF8`.
  - `engine/platform/win/WindowWin.cpp` `winUtf8ToWide`: drop the `CP_ACP` fallback; invalid UTF-8
    converts with `MultiByteToWideChar(CP_UTF8, 0, ...)`, which yields U+FFFD per bad sequence.
- Editor calls switched to the wide API (correct whatever the code page, and needed by the lint):
  `editor/util/FileUtils.h:56` `GetModuleFileNameW` + `std::filesystem::path(std::wstring)`;
  `editor/project/ProjectDataSafety.cpp:51` `GetComputerNameW` + UTF-8 conversion;
  `editor/util/CommandRunner.cpp` `CreateJobObjectA(nullptr, nullptr)` -> `CreateJobObjectW`;
  `CommandRunner.cpp:734,738` widen with `CP_UTF8` (the comment at `:727-733` says the arguments are
  UTF-8).

### 6.2 Child processes and vswhere (`editor/util/CommandRunner.{h,cpp}`)

- `run()`'s `emit` lambda (`:703`) converts each line with `TextEncoding::consoleToUtf8` before
  `onLine` / `Out::build`. Both read paths (Windows `:818,863`; POSIX `emitLines` `:961-1025`) go
  through `emit`, so `capture()` and `runArgs()` are covered.
- `TextEncoding::consoleToUtf8(line)`: returns `line` when `StringUtils::isValidUtf8(line)`; on
  Windows otherwise converts from the system OEM code page
  (`GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER, ...)`,
  read once; under the manifest `GetOEMCP()` returns 65001, so it cannot be used); elsewhere returns
  `StringUtils::repairUtf8(line)`. A line never splits a DBCS character (`\n` is never a trail byte
  in 932/936/949/950).
- Python children on Windows: when `run()` gets no `environment`, it passes
  `currentEnvironment()` plus `PYTHONUTF8=1` and `PYTHONIOENCODING=utf-8` for each one not already
  set (`setVariable`), as a UTF-16 block (the existing `environmentBlock` path). When an
  environment is given (MSVC, Emscripten), the same two are added unless present. `launchDetached`
  (external editors) is unchanged: those programs are the user's.
- New `static std::string CommandRunner::vswhere(std::vector<std::string> arguments, int timeoutMs = 0)`:
  resolves vswhere, builds the argv with the pure
  `static std::vector<std::string> vswhereArguments(std::string program, std::vector<std::string> arguments)`
  (appends `-utf8`), runs `capture`, trims. `static bool hasVswhere()`. `findVswherePath()` becomes
  private.
  Callers: `vsInstallationPath()` (`:117-123`), `editor/Generator.cpp:3142-3166` (both displayName
  queries; `vswhere.empty()` checks become `CommandRunner::hasVswhere()`),
  `editor/util/ExternalCodeEditor.cpp:41-47`.

### 6.3 The log (`engine/core/log/Logger.cpp`, `engine/core/util/StringUtils.{h,cpp}`)

- `StringUtils` gains (engine/core, every platform):
  - `static bool isValidUtf8(std::string_view text)`: ASCII fast path eight bytes at a time, then the
    strict rules of `McpServer::utf8SequenceLength` (`McpServer.cpp:587-608`: no overlongs, no
    surrogates, nothing above U+10FFFF), which move here.
  - `static std::string repairUtf8(std::string_view text)`: one U+FFFD per invalid byte.
  - `static std::string legacyToUtf8(std::string_view text)`: Windows: valid UTF-8 unchanged, else
    decoded from the system ANSI code page (`LOCALE_IDEFAULTANSICODEPAGE`, read once), and if that
    fails `repairUtf8`; elsewhere `repairUtf8` unless valid.
- `logSubmit` (the common path of `write` and `vwritef`): after `shouldLog`, if
  `!StringUtils::isValidUtf8(text)`, the text becomes `StringUtils::legacyToUtf8(text)` (into the
  thread's `largeText` buffer, no allocation for valid text). The rate-limit key is computed from the
  repaired text.
- `McpServer::sanitizeUtf8` (`McpServer.cpp:630`) becomes `return StringUtils::repairUtf8(text);`
  (same output). The other validators (`LuaJson`, `NativeBridge`, HTML UI `sanitizeUtf8`,
  texsynth) stay: they report positions or errors, not just repair.
- `editor/Out.cpp` is unchanged (it goes through the logger).

### 6.4 Character input: whole code points (`engine/core`)

- `engine/core/util/StringUtils.{h,cpp}`: `toUTF8(wchar_t)` becomes `static std::string toUTF8(char32_t codepoint)`
  (same replacement rule for surrogates and values above U+10FFFF). `eraseCodepointsUtf8` and
  `insertUtf8AtCodepoint` (`:209,224`) append `toUTF8(static_cast<char32_t>(cp))`. Callers passing
  `wchar_t` still compile (implicit conversion). The comment at the top of `StringUtilsTest.cpp`
  about U+FFFF goes.
- `engine/core/Engine.{h,cpp}`:
  - `static void systemCharInput(char32_t unit);` doc: "One UTF-16 unit (Windows WM_CHAR, Android
    key path) or one whole code point; halves are combined here, so everything after this sees
    whole code points."
  - `static FunctionSubscribe<void(char32_t)> onCharInput;`
  - `systemCharInput`: push into `charInputCombiner`; for the complete code points (0, 1 or 2) call
    a new private `deliverCodepoints(const std::vector<uint32_t>&, scenes)`: `Input::addInputCharacter`
    each, documents get the UTF-8 string as today; then, with the same condition as today
    (`!ignoreEventsHandledByUI || !consumed`), `onCharInput.call(cp)` for each; unless consumed,
    `UISystem::eventOnCharInput(cp)` for each scene that `canSendUIEvents`. A pending high surrogate delivers nothing (the old
    `wantsTextComposition()` half rule goes).
  - `flushCharInput`: the U+FFFD of a lone high goes through `deliverCodepoints` (today documents
    only).
  - `sendCharacterToGame` (`:2951-2972`) loses the 16-bit split: one call per code point.
- `engine/core/subsystem/UISystem.{h,cpp}`: `eventOnCharInput(char32_t)`,
  `handleInputFieldCharInput(..., char32_t)`; `StringUtils::toUTF8(codepoint)` inserts the whole
  character. Lua binding `ECSClassesLua.cpp:613` unchanged (integer argument).
- `engine/core/script/LuaFunctionBase.{h,cpp}`: `push_value(lua_State*, wchar_t)` becomes
  `push_value(lua_State*, char32_t)` (pushes the UTF-8 string, as today). `CoreClassesLua.cpp:2608-2613`
  binds `FunctionSubscribe<void(char32_t)>`; the Lua class keeps the name `FunctionSubscribe_V_W`
  (internal; no script names it). `tools/lua_api/parser.py:3410-3413` `handler_arg_type`: `char32_t`
  (and `wchar_t`) map to `string`; `ENGINE_EVENT_ARGS['onCharInput'] = ['char']` stays.
- Platform callers change only types: `editor/window/PlayInput.cpp:196-221` passes
  `event.Text.Char` whole (the split and its comment go; with `IMGUI_USE_WCHAR32`,
  `InputQueueCharacters` is whole too, but the trail stays the source); `PlayInput.cpp:227`
  `char32_t charInput`; `engine/platform/apple/ios/EngineView.mm` `sendEditingKey:character:` takes
  `char32_t`; `engine/platform/emscripten/JasperEngineWeb.cpp:986` `char32_t cp`;
  `engine/platform/win/WinInputRouter.cpp:380-389` comment ("one UTF-16 unit; the engine combines
  pairs"). `editor/util/BidiText.cpp:301` uses `toUTF8(static_cast<char32_t>(codepoint))`.
- `editor/util/ScriptEvents.cpp:72`: `{{"char32_t", "character"}}`, description "A character is typed
  (one whole character, emoji included; Lua: a one-character string)". The generated C++ stub is
  `void onCharInput(char32_t character)`. `REGISTER_ENGINE_EVENT(onCharInput)` with an old
  `void onCharInput(wchar_t)` still compiles: `engineEventCallback` returns
  `std::function<void(wchar_t)>`, which converts to `std::function<void(char32_t)>` (test C3).
- `engine/tests/htmlui/TestInput.cpp:102` keeps its `std::function<void(wchar_t)>` handler (it is
  the compatibility case) but records `char32_t`-wide values through a new handler in the new tests.

### 6.5 Android text input (`engine/platform/android`)

- New header-only `engine/platform/android/AndroidTextInput.h` (no Android or JNI includes, so core
  tests compile it on Linux), namespace `jasperengine::android`, moving from `NativeEngine.cpp`
  unchanged: `decodeModifiedUtf8` (`:332`), `utf16ToCodepoints` (`:376`), `wideToCodepoints`,
  `codepointsToModifiedUtf8` (`:419`), `codepointsToUtf8`. New:
  `struct TextEdit { size_t backspaces = 0; std::u32string typed; };`
  `TextEdit diffText(const std::u32string& before, const std::u32string& after);` (common prefix by
  code point; one backspace per code point after it in `before`; the rest of `after` typed).
- `NativeEngine.cpp`: `static std::u32string textInputBuffer;` (code points). `showSoftInput`
  stores `wideToCodepoints(text)`; the document branch (`:926-931`) stores the code points of
  `units` (`utf16ToCodepoints`); the component branch (`:932-952`) converts `units` to code points
  once (the same vector feeds `resetDocumentTextInput` below it), applies `diffText`, sends
  `systemCharInput('\b')` per backspace and `systemCharInput(cp)` per typed code point, and stores
  the code points. The out-of-range read in the prefix loop (`textInputBuffer[pos]` before the bound
  check, `:942`) disappears with it.
- Red-before-green: commit 1 moves the functions verbatim and adds `diffText` with today's
  semantics expressed on UTF-16 units (`diffUnits`); test A1 against it fails; commit 2 switches to
  code points.

### 6.6 Editor fonts (`editor/util/SystemFonts.*`, `editor/App.cpp`, preferences)

- New `editor/util/SystemFonts.h`, `SystemFonts.cpp` (Windows, Linux, common code) and
  `SystemFontsMac.mm` (CoreText lookup), derived from Doriax `84bb05ab0` (`editor/util/SystemFonts.cpp`,
  MIT, header kept: same as ours):
  ```cpp
  namespace jasperengine::editor {
  enum class CjkLanguage { Auto, Japanese, Korean, SimplifiedChinese, TraditionalChinese };
  class SystemFonts {
  public:
      struct Font { std::string path; int faceIndex = 0; std::string covers; // "ja", "ko", "zh-Hans", "zh-Hant", "emoji"
                    const void* data = nullptr; size_t size = 0; };
      enum class State { Idle, Searching, Ready, None };
      static void startSearch(CjkLanguage preference);      // starts the background job once
      static bool applyPending(const std::vector<ImFont*>& targets, const std::vector<float>& sizes); // main thread, before NewFrame
      static State state();
      static std::vector<Font> fonts();                       // a copy for editor_status
      // pure parts, tested
      static std::vector<CjkLanguage> searchOrder(CjkLanguage preference, std::string_view userLocale);
      static std::vector<Font> parseFontconfig(std::string_view fcMatchOutput, std::string_view lang);
      static bool isLoadable(const void* data, size_t size, int faceIndex);
      static int faceIndexByPostScriptName(const void* data, size_t size, std::string_view name);
  };
  }
  ```
- Search order: the preferred language (Auto: `GetUserDefaultLocaleName` on Windows,
  `[NSLocale preferredLanguages]` on macOS, `LC_ALL`/`LC_MESSAGES`/`LANG` on Linux; `ja` -> Japanese,
  `ko` -> Korean, `zh` with `TW`/`HK`/`MO`/`Hant` -> Traditional, other `zh` and anything else ->
  Simplified), then Simplified Chinese (Han coverage), Korean (Hangul), Japanese (kana), each kept
  only when its font differs (path and face) from one already chosen; then the emoji/symbol fonts.
- Lookup per platform (on the job, `jobs::` Background lane; no ImGui calls):
  - Windows: the Fonts folder from `SHGetKnownFolderPath(FOLDERID_Fonts)`. Japanese `YuGothM.ttc`,
    `meiryo.ttc`, `msgothic.ttc`; Simplified `msyh.ttc`, `simsun.ttc`; Traditional `msjh.ttc`,
    `mingliub.ttc`; Korean `malgun.ttf`, `gulim.ttc`; emoji/symbols `seguiemj.ttf`, `seguisym.ttf`.
    First existing loadable file per language, face 0. Mapped with `CreateFileW` /
    `CreateFileMappingW` / `MapViewOfFile`.
  - macOS (`SystemFontsMac.mm`): `CTFontCreateWithName` for "Hiragino Sans" (ja), "Apple SD Gothic
    Neo" (ko), "PingFang SC" (zh-Hans), "PingFang TC" (zh-Hant), fallbacks "Hiragino Sans GB",
    "STHeiti"; file from `kCTFontURLAttribute` (`CFURLGetFileSystemRepresentation`), face by matching
    `CTFontCopyPostScriptName` against the TTC's name table (`faceIndexByPostScriptName`, nameID 6).
    No emoji font (Apple Color Emoji is bitmap-only). Mapped with `open`/`mmap`. The editor target
    links `CoreText` (check `CMakeLists.txt`'s Apple frameworks list).
  - Linux: `CommandRunner::capture({"fc-match", "-s", "-f", "%{file}\\t%{index}\\t%{lang}\\n",
    "sans-serif:lang=<lang>"}, 3000)` (no shell), parsed by `parseFontconfig` (keeps `.ttf/.ttc/.otf`,
    face indices without a variable-instance part (`index >> 16 == 0`), and fonts whose language list
    holds the language); emoji: `fc-match -s -f ... "emoji"` keeping only fonts with outlines
    (`isLoadable` plus a `glyf` or `CFF ` table with a non-empty outline for U+1F600, so CBDT-only
    Noto Color Emoji is skipped). Without `fc-match`, known paths: `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`,
    `/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc`, `/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc`,
    `/usr/share/fonts/truetype/wqy/wqy-microhei.ttc`, `/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc`,
    `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf`; in a Noto CJK collection the face is
    chosen by its family name (`JP`, `KR`, `SC`, `TC` suffix), not a hard-coded index.
  - Every candidate passes `isLoadable` (stb_truetype `stbtt_GetFontOffsetForIndex` +
    `stbtt_InitFont`, the checks ImGui's loader asserts on) before it is kept; the rest are unmapped.
    `SystemFonts.cpp` includes `imstb_truetype.h` with `STBTT_STATIC` and
    `STB_TRUETYPE_IMPLEMENTATION` (local symbols).
- Merge: `App::setup()` (`App.cpp:2375-2444`) records the three targets (`font1`, `boldFont`,
  `codeFont`) and their sizes and calls `SystemFonts::startSearch(AppSettings::getCjkFontLanguage())`.
  `editor/EditorFrame.cpp` (before `ImGui::NewFrame()`, `:180`) and `editor/backend/BackendMac.mm`
  (before both `ImGui::NewFrame()` calls, `:634`, `:1067`) call `App::applySystemFonts()`, which is an
  atomic flag check until the job is done, then once:
  `ImFontConfig cfg; cfg.MergeMode = true; cfg.DstFont = target; cfg.FontDataOwnedByAtlas = false;
  cfg.FontNo = face; cfg.OversampleH = cfg.OversampleV = 1; cfg.RasterizerMultiply = <the target's>;`
  `io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), size, targetSize, &cfg)` for each font and
  target. Merging before `NewFrame` keeps baked-font pointers valid (`AddFont` discards the target's
  bakes, `imgui_draw.cpp:3085`). The mappings live until exit (ImGui does not own them).
- `CMakeLists.txt` (editor tree, next to `IMGUI_ENABLE_TEST_ENGINE` at `:357`):
  `add_definitions("-DIMGUI_USE_WCHAR32")`. The editor's ImWchar users (`CustomTextEditor.cpp:3570`,
  `ScriptCreateDialog.cpp:45`, `NativePluginDialog.cpp:23,29`, `ShaderForkDialog.cpp:17`,
  `SelectableTextView.cpp:163`) are read for 16-bit assumptions; none was found (inferred).
- Preference: `AppSettings::getCjkFontLanguage()/setCjkFontLanguage(CjkLanguage)`, stored in
  `settings.yaml` as `cjk_font_language: auto|japanese|korean|simplified_chinese|traditional_chinese`
  (missing or unknown -> `auto`). Settings row in `editor/settings/SettingsRows.cpp`: key
  `preferences.cjk_font_language`, page General, section "Editor Font", label "CJK Font Language",
  search words "cjk chinese japanese korean kanji hanja font language glyphs", description "Which
  language's glyph forms the editor uses for Chinese characters, from the system's fonts. Applies at
  the next start." Preferences are not undoable (like every other preference row; Unity's
  Preferences have no undo either).
- `editor_status` (`editor/mcp/tools/CoreTools.cpp` `editorStatusTool`): structured
  `editor_fonts: {state: "searching"|"ready"|"none", cjk_font_language: "<preference>",
  effective_language: "ja"|..., fallbacks: [{file, face, covers}]}`. No text-summary line unless the
  state is `none` ("Editor fonts: no system CJK font found; CJK text shows as boxes"). The tool
  description is unchanged (no schema change).

### 6.7 Portable paths in project files

- `editor/Stream.h/.cpp`: `static std::string portablePath(std::string path)` (replaces every `\` with
  `/`; no other change) and `static std::string portablePath(const std::filesystem::path& path)`
  (`FileUtils::pathToGenericUtf8` then the same replacement).
  - Encode: `encodeAssetRef` (`:1338`), `encodeShaderRef` (`:1371`), `encodeTexture` (`:1426,1452,1460`
    path fields), project.yaml keys `windowIcon`, `assetsDir`, `luaDir` (`:2557,2561,2564`),
    `scriptDirs` (`:2569`), Android `launcherIcon`, `adaptiveIconForeground`,
    `adaptiveIconBackground` (`:2841,2844,2847`), keystore `path` (`:2875`).
  - Decode: `decodeAssetRef`, `decodeShaderRef`, `decodeTexture` (single, cube single, cube faces),
    and the project.yaml readers of the same keys (`Stream.cpp` ~:3196-3204, ~:3554-3556 and the
    Android block) apply `portablePath` to the string they read.
  - Factory output needs no change: exports resolve references first (§4 F11), and decoded values
    already carry `/`.
- Pickers write `/` at the source: `InspectorWindow.cpp:1145,4709,4989,5104,7777` and script entries
  `:2343-2348,11781-11782` use `FileUtils::pathToGenericUtf8(...)` instead of `.string()`.
- `.material` files: `Project::bindMaterialAssetRefs` (`editor/project/ProjectAssets.cpp:178-199`)
  also refreshes `ref.path` to the database's form (`AssetIndex::fromProjectRelative(roots, site.base,
  found.path)`) when it resolved the reference, as `resolveSceneAssetRefs` does (`:105-117`).
- A path field holding `\` loaded from an old file is converted in memory, which does not mark the
  scene modified; the next save writes `/`.

### 6.8 Apple bundle paths (`engine/platform/apple/JasperEngineApple.mm`)

- `getAssetPath` (`:290`), `getLuaPath` (`:340`), `getShaderPath` (`:356`) return
  `bundlePath(path)`, a file-local helper: `const char* utf8 = [path UTF8String]; return utf8 ? std::string(utf8) : std::string();`
  (the form `SystemMac.mm:31` already uses for the default macOS backend). No other
  `cStringUsingEncoding:` remains in `engine/` or `editor/` (the lint enforces it).
- Built for iOS (`engine/CMakeLists.txt:792-812`) and Xcode-generated macOS Metal (`:648-675`), and
  through the exported Xcode workspace; not buildable here: the implementer keeps it compiling by
  reading (Objective-C++, three one-line changes).

### 6.9 Small items covered by the code page but kept

- `editor/window/dialog/ProjectSaveDialog.cpp:56-62`: `homeDirPath = FileUtils::pathToUtf8(ShellEnv::homeDirectory());`
  (no null dereference when `USERPROFILE`/`HOME` is unset).

### 6.10 `engine/jasperengine.py`

- At `if __name__ == '__main__':` before `build()`: for `sys.stdout` and `sys.stderr`, when the
  stream is not a terminal (`not stream.isatty()`) and has `reconfigure`, call
  `stream.reconfigure(encoding="utf-8", errors="backslashreplace")`. A terminal keeps Python's own
  console handling (PEP 528 writes UTF-16 to the Windows console).
- `open(project)` (`:55`) and `open(project, "w")` (`:105`) pass `encoding="utf-8"` (the Xcode project
  file is UTF-8).

### 6.11 The lint (`tools/check_text_encoding.py`)

Same shape as `tools/check_raw_writes.py` (scans `engine/` and `editor/`, skipping `libs`,
`external`, `tests`; comments stripped; `--self-test` plants one violation per rule and must fail;
allow-list `tools/text_encoding_allowlist.txt` with `<path> :: <fragment> :: <reason>`, entries that
match nothing fail). Rules:

1. `CP_ACP` anywhere (say `CP_UTF8`, or `StringUtils::legacyToUtf8` for legacy text).
2. ANSI Win32 calls that take or return text or paths: `GetModuleFileNameA`, `CreateFileA`,
   `CreateDirectoryA`, `CreateFileMappingA`, `CreateJobObjectA`, `LoadLibraryA`, `LoadLibraryExA`,
   `ShellExecuteA`, `ShellExecuteExA`, `FindFirstFileA`, `FindNextFileA`, `GetTempPathA`,
   `GetFullPathNameA`, `DeleteFileA`, `MoveFileA`, `MoveFileExA`, `CopyFileA`, `GetFileAttributesA`,
   `GetFileAttributesExA`, `SetCurrentDirectoryA`, `GetCurrentDirectoryA`, `CreateProcessA`,
   `MessageBoxA`, `SetWindowTextA`, `GetEnvironmentVariableA`, `SetEnvironmentVariableA`,
   `SHGetFolderPathA`, `OutputDebugStringA`, `GetComputerNameA`, `GetUserNameA`, `RegQueryValueExA`,
   `FormatMessageA`.
3. `cStringUsingEncoding:` with anything but `NSUTF8StringEncoding`, and `NSASCIIStringEncoding`.
4. `findVswherePath` outside `editor/util/CommandRunner.{h,cpp}` (vswhere goes through
   `CommandRunner::vswhere`, which adds `-utf8`).

It runs in CI's unit job next to the data-safety lints (`.github/workflows/ci.yml:98-103`) and is
listed in `docs/testing.md` (the `unit` row at `:809` and the lint list at `:957`).

### 6.12 MCP

- `send_input` text event (`editor/mcp/tools/PlayTools.cpp:2913-2926`): new optional boolean
  `per_unit` ("One character event per UTF-16 unit, as Windows WM_CHAR and Android keys deliver
  typing; default false: the whole string at once, as a paste or IME commit."). With `per_unit`, the
  queued step calls `Engine::systemCharInput(unit)` for each UTF-16 unit of the text (the string
  decoded with `StringUtils::decodeUtf8ToCodepoints`, code points above U+FFFF split into a pair).
  The comment at `:2918-2919` is corrected. Schema built with `McpSchema.h`; `tool_budget.py --check`
  must pass.
- `editor_status`: §6.6.
- `preferences.cjk_font_language`: through the existing settings tools (no tool change; the row
  registers it).
- Agent guide (`editor/mcp/McpInstructions.cpp`): no change (budget); details go to
  `docs/mcp/guide/input.md` and `docs/mcp/guide/ui.md`.
- Regenerate `docs/mcp/TOOLS.md` (`tools/mcp/gen_tool_docs.py`).

### 6.13 Docs

- `docs/mcp/guide/input.md`: `Input.inputString` row unchanged; new lines: `Engine.onCharInput`
  receives one whole character per call on every platform (a one-character string in Lua,
  `char32_t` in C++); `send_input` `per_unit`.
- `docs/mcp/guide/lua-api.md:259`: `onCharInput` | `char` | "a character was typed (a one-character
  UTF-8 string; emoji arrive whole on every platform)".
- `docs/mcp/guide/ui.md`: InputField takes any character, emoji included; whether it shows depends
  on the Text's font.
- `docs/shipping.md` "Platforms": Windows: the editor needs Windows 10 version 1903 or later
  (UTF-8 code page); games run on older Windows too.
- New `docs/editor/text-and-languages.md`: file names in any language, the Windows requirement, the
  CJK font lookup and the preference, emoji in the editor, `/` in project files, game fonts need
  their own glyphs (with a pointer to the font docs).
- `docs/core-utilities.md` "Logging": the log is always valid UTF-8 (repair rule). A short
  "Text encoding" section for contributors: UTF-8 in every `std::string`, `/` in project files,
  `char32_t` for characters, the lint.
- `docs/testing.md`: the lint; the Windows red controls (§10).
- `NOTICE.md`: the test font subset under "Test data" (SIL OFL 1.1); `SystemFonts` names its Doriax
  origin in its header comment (Doriax is this repository's upstream, `NOTICE.md:3-8`).

## 7. Behaviour rules and edge cases

1. A Windows editor or CLI on a system whose `GetACP()` is not 65001 exits 1 before logging, with the
   message of §6.1; `--help` included. A Windows 10 1809 system with the "Beta: Use Unicode UTF-8"
   option on passes (its ACP is 65001).
2. Windows Server 2019 (1809) cannot run the CLI (`jasperengine-editor export`); Server 2022 can.
   Documented in `docs/shipping.md`.
3. A game on Windows before 1903 behaves as today; its crash reports, backtraces and debugger
   output are correct for non-ASCII user folders (F23).
4. Console output of the CLI is UTF-8 while it runs; the shell's code page is restored at normal
   exit. A crash leaves the console in UTF-8 (harmless; documented).
5. A child line that is valid UTF-8 is never reinterpreted, even if the tool meant the OEM code page
   (a 1252/850 line that happens to be valid UTF-8 is rare and short).
6. Log text that is not valid UTF-8 shows U+FFFD (or, on Windows, its ANSI reading) in the file, the
   Console and MCP; valid text is byte-identical to today.
7. `\` in any stored path field is `/` after load and after save. A path of an asset whose real
   name holds `\` (Linux/macOS) becomes a missing reference; the missing-asset list names it.
8. A typed high surrogate followed by a low one is one character for documents, `Input.inputString`,
   `onCharInput` and InputField. A high followed by anything else is U+FFFD then that character; a
   lone low is U+FFFD; a high still pending at the frame's draw is U+FFFD (all three, everywhere).
9. InputField `maxTextSize` counts code points: an emoji counts 1 (as today on Linux).
10. Android: a field holding `a😀`; typing `b` sends one `b`, no backspace; deleting the emoji
    sends one backspace and removes the emoji only.
11. `onCharInput` handlers in Lua see `"😀"` once; old C++ handlers declared with `wchar_t` compile
    and on Windows receive the code point truncated to 16 bits (as close to today as possible;
    the docs say to use `char32_t`).
12. CJK fonts: until the search finishes (typically under 100 ms) CJK text draws as boxes; it
    redraws with glyphs on the next frame after the merge. No system CJK font: boxes, `editor_status`
    says `none`, and the editor works. A font file that fails `isLoadable` is skipped silently (one
    Debug log line).
13. Changing **CJK Font Language** takes effect at the next start; the row says so.
14. With no `fc-match` and none of the known files, Linux shows boxes (the Docker images have no
    CJK fonts; the tests use a fixture font).
15. Emoji in the editor draw in monochrome on Windows (Segoe UI Emoji outlines), from an outline
    emoji font on Linux when installed, as boxes on macOS; their text is preserved everywhere.
16. Apple: an app whose bundle sits under a non-ASCII folder starts and finds its assets.
17. vswhere output with a localized Visual Studio name (e.g. `Visual Studio Community 2022 – 日本語`)
    shows correctly in the kit list.

## 8. Performance and cost

- Manifest: no runtime cost. Startup check: one `GetACP()` call.
- Logger: one validation pass per record that passes the level filter (word-at-a-time ASCII scan;
  under 1 ns/byte), no allocation for valid text. Game builds pay this too; measure with
  `LoggerBenchTest` (budget: the existing benchmark's time grows by less than 5%).
- CommandRunner: one validation pass per output line; Windows builds the environment block per run
  (a few KB).
- Character input: unchanged work per character (the split loop disappears).
- Android: one UTF-16 to code point conversion per text change (already done for documents).
- Fonts: zero main-thread cost at start (background job; `fc-match` spawn on Linux runs off the main
  thread); memory-mapped files (only touched pages resident); glyphs rasterised on demand as today.
  `IMGUI_USE_WCHAR32`: ImGui's per-baked-size glyph index grows to the highest code point drawn
  (about 6 bytes per code point: ~240 KB for CJK, ~770 KB once an emoji is drawn, per font size).
  Editor only; games are untouched.
- Nothing here runs in a game that does not type text, beyond the logger check.

## 9. Compatibility

- Old projects: no format change, no version bump. Files holding `\` in path fields now load on
  every platform (better than today). ANSI-encoded YAML from older Windows editors is not converted
  (owner decision 2026-11-04).
- `settings.yaml`: new optional `cjk_font_language`, default `auto`.
- C++ API: `Engine::systemCharInput(char32_t)`, `Engine::onCharInput` is `FunctionSubscribe<void(char32_t)>`,
  `UISystem::eventOnCharInput(char32_t)`, `StringUtils::toUTF8(char32_t)`. Callers with `wchar_t`
  compile unchanged; a C++ script whose handler takes `wchar_t` compiles (C3). Native plugins compiled
  against the old headers must be rebuilt (the toolchain fingerprint already forces a rebuild on
  engine header changes).
- Lua API: `onCharInput` keeps its string argument; on Windows and Android a character beyond U+FFFF
  now arrives as one call instead of two U+FFFD calls (behaviour change, documented).
- Windows editor support: Windows 10 1903+ (new requirement, documented). Games: unchanged.
- MCP: `send_input` gains an optional parameter; `editor_status` gains a field. No breaking change.

## 10. Tests

Red controls: "Linux red" means the test fails on today's `main` in the Linux build and passes
after; "Windows red" means it can only fail on Windows (the CI Windows job runs both unit suites:
`.github/workflows/ci.yml:662-671`; it runs on demand, so the implementer records those as
unverified unless the lead runs it on the commit before and after the change).

Core tests (`jasperengine-core-tests`):

| Test | Asserts | Red |
|---|---|---|
| C1 `StringUtils.ToUtf8EncodesEveryPlane` (`engine/tests/core/util/StringUtilsTest.cpp`) | `toUTF8(U'\U0001F600')` is `F0 9F 98 80`; surrogates and > U+10FFFF give U+FFFD | Windows red |
| C2 `StringUtils.EditsKeepCharactersBeyondTheBmp` | inserting `x` at 1 in `a😀b` gives `ax😀b`; erasing index 0 keeps `😀` | Windows red |
| C3 `FunctionSubscribeCompat.CharInputHandlerTakingWcharStillRegisters` (`FunctionSubscribeCompatTest.cpp`) | a test class with `void onCharInput(wchar_t)` registers through `REGISTER_ENGINE_EVENT` and receives `'a'` | compile check (not a fix) |
| C4 `StringUtils.ValidatesAndRepairsUtf8` | the overlong, surrogate, truncated and >U+10FFFF cases of `McpServer` tests: `isValidUtf8` false, `repairUtf8` one U+FFFD per bad byte; ASCII and 4-byte input unchanged | new function |
| C5 `Logger.InvalidUtf8ReachesSinksRepaired` (`engine/tests/core/log/LoggerTest.cpp`) | `Logger::write` and `writef("%s")` of `"ok \xff"` reach a capture sink as `"ok \xEF\xBF\xBD"` | Linux red |
| A1 `AndroidTextInput.DiffCountsCodePoints` (`engine/tests/core/input/AndroidTextInputTest.cpp`) | before `a😀`, after `a😀b`: 0 backspaces, typed `b`; before `a😀`, after `a`: 1 backspace | Linux red (against commit 1's unit-based `diffText`) |
| A2 `AndroidTextInput.ShowSoftInputAndDiffAgree` | `wideToCodepoints(L"😀")` and the code points of the decoded modified UTF-8 of the same text are equal, so the first keystroke diffs to "typed only" | Linux red (commit 1) |
| A3 `AndroidTextInput.DecodesModifiedAndPlainUtf8` | the moved decoders keep today's results (surrogate pairs, 4-byte, malformed -> U+FFFD) | no (moved code) |
| W1 `WindowsCodePage.TestProcessIsUtf8` (`#ifdef _WIN32`, core and editor test binaries) | `GetACP() == CP_UTF8` | Windows red (without the test manifest) |

HTML UI tests (`tools/build.sh htmlui-tests`, `engine/tests/htmlui/TestInput.cpp`, fixture `InputTest`):

| Test | Asserts | Red |
|---|---|---|
| H1 `InputTest.OnCharInputGetsWholeCodePoints` | with no focused document field, `systemCharInput(0xD83D)`, `(0xDE00)` give `onCharInput` exactly one call with U+1F600; `Input.inputString` holds `😀` | Linux red (today two calls with the halves) |
| H2 `InputTest.ComponentInputFieldKeepsSurrogatePairs` | a focused component InputField receiving `a`, `0xD83D`, `0xDE00`, `b` holds `a😀b`; `'\b'` then leaves `a😀` | Linux red (today `a\uFFFD\uFFFDb`) |
| H3 `InputTest.LoneHighSurrogateFlushesEverywhere` | a lone high then `systemDraw()` gives U+FFFD to the InputField and `onCharInput` | Linux red |
| H4 the existing `InputTest.SurrogatePairsReachDocumentsCombined` | unchanged (documents) | regression guard |

Editor tests (`jasperengine-editor-tests`):

| Test | Asserts | Red |
|---|---|---|
| E1 `TextEncoding.CodePageProblem` | `codePageProblem(65001)` is empty; `(1252)` names Windows 10 version 1903 | new function |
| E2 `TextEncoding.ConsoleToUtf8KeepsValidAndRepairsInvalid` | valid UTF-8 unchanged; `"\xff"` gives U+FFFD (non-Windows) | new function |
| E3 `CommandRunnerTest.InvalidUtf8OutputIsRepaired` (`editor/tests/CommandRunnerTest.cpp`, POSIX) | `capture({"/usr/bin/printf", "a\\377b"})` returns `a\uFFFDb`; a UTF-8 line comes back byte-identical | Linux red |
| E4 `CommandRunnerTest.VswhereArgumentsEndWithUtf8` | the argv `CommandRunner::vswhereArguments({...})` (the pure part of `vswhere`) ends with `-utf8` | new function (the lint is the red control) |
| E5 `PortablePaths.ProjectYamlStoresForwardSlashes` (new `editor/tests/project/PortablePathsTest.cpp`) | a project with `windowIcon = "icons\\app.png"`, `luaDir = "scripts\\lua"`, Android `launcherIcon = "icons\\launcher.png"` saves `icons/app.png`, `scripts/lua`, `icons/launcher.png` | Linux red |
| E6 `PortablePaths.BackslashPathsLoad` | decoding a scene YAML whose texture `path`, asset refs and a cube map's `paths` hold `\` gives `/`; a project.yaml with `windowIcon: icons\app.png` loads as `icons/app.png` | Linux red |
| E7 `PortablePaths.MaterialFileStoresPicksWithSlashes` | `writeMaterialFile` of a material whose base-color texture path is `textures\\wood.png` (file present) writes `path: textures/wood.png` and its GUID | Linux red |
| E8 `SystemFonts.SearchOrder` | Auto with `ja_JP.UTF-8` gives ja, zh-Hans, ko; `zh_TW` gives zh-Hant first; an explicit preference wins over the locale | new function |
| E9 `SystemFonts.ParsesFontconfigOutput` | a recorded `fc-match -s` output keeps the right file/face, drops variable instances, `.pcf` files and fonts not covering the language | new function |
| E10 `SystemFonts.MergesIntoExistingFonts` | ImGui context with Roboto; merging the fixture font (`editor/tests/fixtures/fonts/NotoSansCJKsc-subset.otf`, OFL, a few KB: 中文日本語한국어) into it via `applyPending` makes `IsGlyphInFont(U'中')`, `(U'한')` true and leaves `A` from Roboto | new feature |
| E11 `SystemFonts.RejectsUnloadableFaces` | random bytes and a face index past the collection fail `isLoadable`; `faceIndexByPostScriptName` finds the fixture's face 0 | new function |
| E12 `PreferencesCjkFont.RowRoundTrips` | the settings row reads `auto`, sets `japanese`, `settings.yaml` holds `cjk_font_language: japanese`, an unknown value reads as `auto` | new row |
| W2 `WindowsUnicodePaths.ProjectWalksTakeNamesOutsideTheAnsiCodePage` (`#ifdef _WIN32`) | a scratch project with `assets/关卡/第一关😀.png` and a material referencing it: asset database open, `Project::changeAssetRoots` to a new assets folder, and the terrain map scan finish without exception and list the file by its UTF-8 name | Windows red (CI runners are code page 1252; without the manifest `path::string()` throws) |

MCP tests (`tools/mcp/run-tests.sh`):

| Test | Asserts | Red |
|---|---|---|
| M1 new `tools/mcp/tests/test_unicode_text.py::test_per_unit_typing_keeps_emoji` | Play a scene with a focused component InputField; `send_input {type:"text", text:"a😀b", per_unit:true}`; `get_component` of the Text gives `a😀b`; a Lua probe's `onCharInput` collected `{"a","😀","b"}` | no (new parameter); H1 and H2 are the red controls of the behaviour |
| M2 `test_unicode_text.py::test_project_yaml_paths_are_portable` | settings tool sets `player.window_icon` to `icons\\app.png` (the file exists); `project.yaml` on disk holds `icons/app.png` | Linux red |
| M3 `test_unicode_text.py::test_editor_fonts_status` | `editor_status.editor_fonts.state` becomes `ready` or `none` within 5 s; `preferences.cjk_font_language` reads `auto` and accepts `korean` | new |
| M4 `tools/mcp/tests/test_utf8_paths.py` (extended) | a scene and a folder named `关卡 😀` are created, saved, listed and moved; the assets root change round-trips with them present | regression guard on Linux |

Lint: `python3 tools/check_text_encoding.py --self-test` (each planted violation fails) and
`python3 tools/check_text_encoding.py` (red today: `CommandRunner.cpp:734,738`, `WindowWin.cpp:120`,
the six ANSI calls of F23/F16, `JasperEngineApple.mm:290,340,356`, `Generator.cpp:3142`,
`ExternalCodeEditor.cpp:41`; green after).

Not tested automatically, verified by reading: the Apple bundle path change (§6.8, no macOS/iOS
build here), `jasperengine.py` (§6.10: a Python unit test would only test `reconfigure`; the
implementer runs `python3 engine/jasperengine.py --help | cat` on Linux to check nothing breaks),
the Windows console code page, Windows/macOS font lookup (E8-E11 cover the shared logic).

Screenshots (visible change): `tools/editor-shot.sh` on a scratch project copy with a scene
`第一关` holding objects `玩家`, `ゲーム`, `한국어` and an InputField text `Hi 😀`, in a container
with `fonts-noto-cjk` installed (installed into a throwaway image layer for the shot, not into
`docker/linux.Dockerfile`), Hierarchy and Inspector before and after. Kept outside the repository.

## 11. Acceptance criteria

1. Every Windows executable the build makes (editor, `-cmd`, game, prebuilt player, core and editor
   test binaries) embeds a manifest with `activeCodePage` UTF-8 (read in the CMake and the manifest;
   W1 on Windows CI).
2. The editor and CLI on Windows exit 1 with the §6.1 message when the code page is not UTF-8 (E1;
   reading of `main.cpp`).
3. `python3 tools/check_text_encoding.py --self-test` and `python3 tools/check_text_encoding.py` pass
   on the branch and the second fails on `main`; CI runs both.
4. C5, E2, E3 pass: no invalid UTF-8 reaches a log sink or a CommandRunner caller.
5. All vswhere runs carry `-utf8` (E4, lint rule 4).
6. C1, C2, H1, H2, H3 pass; H4 still passes: whole code points everywhere after `systemCharInput`.
7. A1, A2, A3 pass, A1 and A2 failing on commit 1 of §6.5.
8. E5, E6, E7, M2 pass: project files carry `/`, and `\` paths load.
9. `JasperEngineApple.mm` has no `NSASCIIStringEncoding` (lint).
10. E8-E12, M3 pass; on a Linux editor with Noto CJK installed, `editor_status.editor_fonts.state`
    is `ready` with a `fallbacks` entry, and the Hierarchy screenshot shows the CJK names as glyphs
    (screenshots attached to the report).
11. The editor's start time (`editor_status` frame times, or `JASPER_MCP_TRACE` timestamps from launch
    to the first frame) does not grow by more than 10 ms with the font search, on the Linux build.
12. `send_input` `per_unit` works (M1); `tools/mcp/tool_budget.py --check tools/mcp/budget.json`
    passes; `docs/mcp/TOOLS.md` regenerated (`gen_tool_docs.py --check` clean).
13. The Lua API data regenerates with `onCharInput(char: string)`; `tools/lua_api` doc test passes.
14. `tools/check-changed` passes on the branch; the full unit suites, `htmlui-tests` and the MCP
    files of §10 pass.
15. Docs of §6.13 updated; `NOTICE.md` lists the fixture font.
16. Windows (W1, W2, C1, C2) and macOS/iOS (§6.8) results are reported as verified only if a CI run
    on the branch shows them; otherwise reported as unverified, with the reason.

## 12. Files likely touched

Build and lint: `engine/platform/win/jasperengine.manifest`, `engine/platform/win/WindowsManifest.cmake`,
`engine/cmake/JasperTesting.cmake`, `engine/tests/core/CMakeLists.txt`, `editor/tests/EditorTests.cmake`,
`CMakeLists.txt` (`IMGUI_USE_WCHAR32`, new sources, CoreText), `.github/workflows/ci.yml`,
`tools/check_text_encoding.py` (new), `tools/text_encoding_allowlist.txt` (new),
`tools/lua_api/parser.py`.

Engine: `engine/core/util/StringUtils.{h,cpp}`, `engine/core/Engine.{h,cpp}` (char input only),
`engine/core/subsystem/UISystem.{h,cpp}` (char input only), `engine/core/script/LuaFunctionBase.{h,cpp}`,
`engine/core/script/binding/CoreClassesLua.cpp` (one class binding), `engine/core/log/Logger.cpp`,
`engine/core/log/Backtrace.cpp`, `engine/core/log/sinks/DebuggerSink.cpp`,
`engine/core/diag/CrashHandlerWin.cpp`, `engine/platform/win/WindowWin.cpp`,
`engine/platform/win/WinInputRouter.cpp` (comment), `engine/platform/android/NativeEngine.cpp`,
`engine/platform/android/AndroidTextInput.h` (new), `engine/platform/apple/JasperEngineApple.mm`,
`engine/platform/apple/ios/EngineView.mm`, `engine/platform/emscripten/JasperEngineWeb.cpp` (one type),
`engine/jasperengine.py`.

Editor: `editor/main.cpp`, `editor/cli/CommandLine.cpp`, `editor/util/TextEncoding.{h,cpp}` (new),
`editor/util/CommandRunner.{h,cpp}`, `editor/Generator.cpp` (vswhere block only),
`editor/util/ExternalCodeEditor.cpp`, `editor/util/FileUtils.h` (`getExecutablePath`),
`editor/util/SystemFonts.{h,cpp}`, `editor/util/SystemFontsMac.mm` (new), `editor/App.{h,cpp}`
(font setup), `editor/EditorFrame.cpp`, `editor/backend/BackendMac.mm` (two calls),
`editor/AppSettings.{h,cpp}`, `editor/settings/SettingsRows.cpp`, `editor/Stream.{h,cpp}`,
`editor/project/ProjectAssets.cpp`, `editor/window/InspectorWindow.cpp` (seven picker lines),
`editor/window/PlayInput.cpp`, `editor/window/dialog/ProjectSaveDialog.cpp`,
`editor/project/ProjectDataSafety.cpp`, `editor/util/BidiText.cpp`, `editor/util/ScriptEvents.cpp`,
`editor/mcp/McpServer.cpp` (`sanitizeUtf8`), `editor/mcp/tools/PlayTools.cpp` (`send_input` text),
`editor/mcp/tools/CoreTools.cpp` (`editor_status`).

Tests: `engine/tests/core/util/StringUtilsTest.cpp`, `engine/tests/core/util/FunctionSubscribeCompatTest.cpp`,
`engine/tests/core/log/LoggerTest.cpp`, `engine/tests/core/input/AndroidTextInputTest.cpp` (new),
`engine/tests/htmlui/TestInput.cpp`, `editor/tests/CommandRunnerTest.cpp`,
`editor/tests/project/PortablePathsTest.cpp` (new), `editor/tests/util/TextEncodingTest.cpp` (new),
`editor/tests/util/SystemFontsTest.cpp` (new), `editor/tests/fixtures/fonts/NotoSansCJKsc-subset.otf` (new),
`editor/tests/CanvasGroupTest.cpp` (`L'a'` literals keep compiling; no edit expected),
`tools/mcp/tests/test_unicode_text.py` (new), `tools/mcp/tests/test_utf8_paths.py`.

Docs: `docs/mcp/guide/input.md`, `docs/mcp/guide/lua-api.md`, `docs/mcp/guide/ui.md`,
`docs/shipping.md`, `docs/editor/text-and-languages.md` (new), `docs/core-utilities.md`,
`docs/testing.md`, `docs/html-ui/phase-1-integration.md:1150-1151` (the char-input rows),
`docs/mcp/TOOLS.md` (generated), `NOTICE.md`.

Hot files shared with other branches: `editor/Stream.cpp`, `editor/window/InspectorWindow.cpp`,
`editor/util/CommandRunner.cpp`, `editor/Generator.cpp`, `engine/core/Engine.cpp`,
`engine/core/subsystem/UISystem.cpp`, `engine/platform/android/NativeEngine.cpp`,
`engine/platform/apple/JasperEngineApple.mm`, `editor/App.cpp`, `editor/mcp/tools/PlayTools.cpp`,
`engine/jasperengine.py`, `CMakeLists.txt`, `.github/workflows/ci.yml`, `docs/testing.md`.

## 13. Cross-feature dependencies

- **ui-components** (strongest overlap): both edit the component InputField and character input
  (`UISystem.cpp` `handleInputFieldCharInput`/`eventOnCharInput`, `Engine.cpp` `systemCharInput`,
  `systemKeyDown` typing guard, `NativeEngine.cpp` DPAD_CENTER next to the text-input block). Land
  `unicode-text`'s §6.4/§6.5 first or rebase on it: ui-components' typing guard and navigation must
  take `char32_t` and keep "whole code points after `systemCharInput`".
- **mobile-shipping**: removes AdMob keys right beside the project.yaml lines of §6.7
  (`Stream.cpp:2862-2863`) and code in `JasperEngineApple.mm` (`:610`); textual conflicts only.
- **build-toolchain**: edits `engine/jasperengine.py` (exec bit, Emscripten 6 on Windows) and
  `editor/Generator.cpp` (`/MP`, defines); §6.10 and the vswhere block are small, separate hunks.
- **export-pipeline**: "build errors visible in the Build dialog and log end" reads CommandRunner
  output: it gets UTF-8 lines from §6.2; "safe paths in web preload/globs" should use the same
  UTF-8/`/` rules (`Stream::portablePath`, `FileUtils::pathToGenericUtf8`).
- **agent-authoring**: "build-tools check on Windows" should call `CommandRunner::vswhere` /
  `hasVSWithCppTools` (the lint refuses `findVswherePath` elsewhere).
- **scripting**: Lua bindings and `tools/lua_api/parser.py`; the `onCharInput` type change touches
  both (one line each).
- **code-editor-git**: the code editor (`CustomTextEditor.cpp`) runs with 32-bit `ImWchar` after this
  branch; its gutter/diff work must not assume 16-bit characters.
- **project-window**: "texture slots show file names" edits `InspectorWindow.cpp` texture rows near the
  picker lines of §6.7; Project window names already work through the code page (no edit here).
- **platform-lifecycle**: `JasperEngineWeb.cpp` key map near the `char32_t` change; editor startup
  (`editor/main.cpp`) if it adds minimize handling there.
- **runtime-core**: `FunctionSubscribe` exception reporting; the `onCharInput` instantiation changes
  type (no logic overlap).
- None with render-correctness, mesh-memory, draw-perf, sprite-batching, ssr-probes,
  water-reflections, water-boats, scene-integrity.

## Deviations

Recorded during implementation (2026-11-04, branch `dc/unicode-text`).

1. **No `SystemFontsMac.mm`.** CoreText and CoreFoundation are C APIs: the macOS lookup lives in
   `editor/util/SystemFonts.cpp` (`#ifdef __APPLE__`), the user's language from
   `CFLocaleCopyPreferredLanguages`. The editor links `CoreText` and `CoreFoundation`.
2. **No font fixture file and no NOTICE entry.** `editor/tests/util/SystemFontsTest.cpp` builds its
   fonts in memory (one square glyph per code point, a TrueType collection for the face tests), so
   nothing third-party ships with the tests. Its tests are `SystemFonts.SearchOrder`,
   `SettingNamesRoundTrip`, `ParsesFontconfigOutput`, `ChecksFontsAndCollections` (E9-E11),
   `MergesIntoExistingFonts` (E10) and `PreferenceRoundTrips` (E12).
3. **The font probe is per language** (kana for Japanese, Hangul for Korean, a Han character for
   Chinese): Malgun Gothic, the Korean default on Windows, has no Han characters.
4. **`editor_status` adds no text line** (lean MCP rules: hints go in `structuredContent`); its
   `editor_fonts.state` is `idle` when no search started (a session without the editor UI), then
   `searching`, `ready` or `none`.
5. **MCP and settings path inputs read `\` as a separator too** (`McpRefs::resolveProjectPath`,
   `readProjectFile` of the settings rows): decision 5 applied to what agents send, so
   `textures\wood.png` from an agent on Windows names the same file on every platform (and M2
   runs on Linux). A `..\` is caught by the existing `..` check.
6. **`send_input` `per_unit`** got an eval case (`tools/mcp/eval/cases.py`,
   `guard_typing_utf16_units`), as the lean MCP rules require of new tool text.
7. **Logger truncation cuts between characters** (`logSubmit`): a 64 KB cut inside a multi-byte
   character produced invalid UTF-8 after the repair step; test `LoggerTest.TruncationKeepsWholeCharacters`.
8. **`Stack<char32_t>` for Lua** (`engine/core/script/LuaBridgeAddon.h`): LuaBridge had no stack
   type for the character (nor for `wchar_t` before); Lua passes a one-character string or a code
   point number to `Engine.onCharInput:call` and `UISystem:eventOnCharInput`.
9. **`Module32FirstW`** in the Windows crash report (the modules list) as well as the session
   paths: module paths are UTF-8 in the report whatever the code page.
10. **W2** is `TextEncoding.ProjectWalksTakeNamesInAnyLanguage` and runs on every platform: on
    Linux and macOS it guards the walks, on Windows it is the red control of the manifest.
    `TextEncoding.ProcessRunsInTheUtf8CodePage` is W1 (Windows only). The tests of §6.1-§6.2 are
    in `editor/tests/util/TextEncodingTest.cpp` (E1, E2, E4).
11. **`InputTest`'s fixture records `char32_t`** (`engine/tests/htmlui/TestInput.cpp`); the
    `wchar_t` handler compatibility is C3 in the core tests.
12. **The font search runs on the jobs' Background lane** (`jobs::enqueueDetached`), started from
    `App::engineInit` once the job system runs (`App::setup` only records the fonts it merges into).
13. **Android's diff is `ComponentTextInput`** (`AndroidTextInput.h`: `reset`, `update` returning a
    `TextEdit`), the state the keyboard callback needs, instead of a free `diffText` function.
14. **Crash report paths are UTF-8 from the start and widened at crash time** (`CrashHandlerWin.cpp`,
    into stack buffers, no allocation): `copyPath` fills them from `path.u8string()` (not
    `string()`, the ANSI code page before Windows 10 1903) and cuts them between characters; the
    engine's report root comes from the platform's UTF-8 path through `std::u8string`.
15. **The log repairs into a local string** (review): the thread's buffer was overwritten when the
    same call emitted a rate-limit summary back through `logSubmit`; the summary's sample is cut
    between characters. Test `LoggerTest.RepairedRecordSurvivesTheSummaryItsCallEmits`.
16. **`legacyToUtf8` reads text as ANSI only when the process is not in the UTF-8 code page**
    (review); in the UTF-8 code page invalid bytes are repaired with U+FFFD.
17. **A sub-asset key keeps its backslashes** (review): `FileUtils::portablePath` (which
    `Stream::portablePath`, `McpRefs` and the settings rows use) changes only the container's part of
    `models/hero.glb#mesh:Body\Skin`. Test `PortablePaths.SubAssetKeysKeepTheirBackslashes`.
18. **`currentEnvironment()` keeps the `=C:` drive entries** (review): a cmd.exe child reads each
    drive's current folder from them, now that every child gets an environment block.
19. **`void onCharInput(wchar_t)` in C++ is deprecated** (`docs/mcp/guide/cpp-scripting.md`): it still
    compiles, and on Windows gets the character cut to 16 bits;
    `FunctionSubscribeCompatTest.CharInputHandlersOfEitherTypeRegister` pins that per platform.
