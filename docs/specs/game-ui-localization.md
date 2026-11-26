# Game UI Localization: String Tables, ICU messages, keyed HTML UI and Text, one game language, missing-text capture and pseudo-locales

Sibling of `dialogue-graphs.md` (its lines are entries of the tables specified here) and builds on
S87 (Title Art languages, the CSV/XLIFF code and the project settings it fixed for this system,
S87 D3). Every `file:line` of section 3 was read at `97d450661`; nothing was built or run for this
spec, so budgets are targets the implementer measures, and behaviour no test covers yet is marked
*(inferred)*.

## 1. Goal

A game made with JasperEngine can ship in many languages without hand-written per-screen tables. A
writer keeps every player-facing string in **String Tables** (one source text per key, one
translation file per language), translators get CSV or XLIFF and send it back, the HTML UI, the Text
component, dialogue lines and Lua all read the same keys with arguments and plurals, the game picks
the player's language from the device or from a menu and switches it live, and a shipped build never
shows a raw key. Developers and agents can see what is not translated yet (missing keys, literal
text reaching the screen, text that overflows in German) before a player does.

## 2. Scope

**In scope**
- String Table assets: a `.stringtable` holding keys, source text, context and length limits, and a
  `.translation` per language; ICU MessageFormat subset (arguments, `number`, `plural`, `select`)
  with CLDR plural rules and number symbols for every CLDR language.
- One game language: resolution with S87's rules from a script choice, a remembered player choice,
  the device's languages, the project default; fallback chain per key; live switching; OS locale
  change events on every platform.
- Consumers: HTML UI (`data-l10n-id`, `data-l10n-args`, `data-l10n-attr-*`, `translate="no"`, root
  `lang`/`dir`, per-language fallback fonts, a data-model transform), the Text component
  (Localization Key), Lua and C++ (`Localization`), dialogue lines (`dialogue-graphs.md`).
- Development aids: missing-translation capture (missing keys, literal text, stale translations),
  pseudo-locales `en-XA` (accented, expanded) and `ar-XB` (right-to-left) in the Game view.
- Editor: Window > Localization Tables (grid editor, undo), Project window Create > Localization >
  String Table, Project Settings > Localization rows; CSV/XLIFF import and export (S87's code,
  shared); cooking for export; export-check warnings.
- MCP: `edit_string_table` (new group `localization`), `inspect_ui {l10n}`, `assert_ui {languages}`,
  `configure_game_view {language: "en-XA"}`; docs and the agent guide.

**Out of scope (decided)**
- Localised assets (per-language textures, audio, voice clips, prefabs). Rationale: GUID-addressed
  asset swapping touches the asset database, cooking and every renderer; no consumer needs it for
  text. Title Art keeps its own per-language bakes (S87 D1). A later spec may add an Asset Table
  that reuses this spec's language resolution (§6 D2) and table file layout.
- Title Art reading its text from a String Table (S87 D3 leaves the door open; no member added here).
- Machine translation in the engine (agents translate with their own model and write through
  `edit_string_table`, as S87 R60 decided for titles).
- Date, time, currency and unit formatting; ICU `selectordinal`, `spellout`, `duration`; gender
  agreement beyond `select`; bidirectional isolation inside one string beyond what HarfBuzz and
  RmlUi's `-rmlui-direction` already do.
- Translating the editor's own ImGui UI.
- Network protocol changes. Multiplayer games send `{key, args}` and format on the receiving
  client (§6 D9); nothing translates already-formatted text.

## 3. Current state

**Settings exist, used only by Title Art.**
- `editor::LocalizationSettings {languages, defaultLanguage}` with `effectiveDefault()` (""→ the
  first language, else `"en"`) is at `editor/Project.h:681-692`; the comment says the names are for
  "a later project-wide localisation". Saved under `localization` in `project.yaml`, non-defaults
  only (`editor/Stream.cpp:3071-3082`), read with canonical tags and duplicates dropped
  (`Stream.cpp:4063-4080`). MCP/CLI rows `localization.languages`, `localization.default_language`
  (`editor/settings/SettingsRows.cpp:2475-2527`); Project Settings > Localization draws them
  (`editor/window/dialog/ProjectSettingsWindow.cpp:3524-3570`).
- Every reader is Title Art: `TitleArtBaker.cpp:742`, `TitleArtDocument.cpp:462`,
  `TitleLanguageExport.cpp:34`, `TitleDesignerLanguages.cpp:283`, `TitleArtToolsRender.cpp:998`.
- The default reaches a compiled game as `TitleArtLanguage::setProjectDefault(...)` in the generated
  main (`editor/Generator.cpp:2745-2746`, from `PlayerSettings::defaultLanguage`,
  `editor/Project.cpp:3513`); the editor sets it at `Project.cpp:3539`. The prebuilt player's
  package carries no language value (no match for "language" in `editor/ExporterPrebuilt.cpp` or
  `engine/player/`) *(inferred: a prebuilt game always starts with the default "en")*.
- `localization.languages` is not shipped to games at all; it only filters Title Art bakes.

**Language resolution exists in core, under a Title Art name.** `engine/core/util/TitleArtLanguage.h`
(contract at lines 7-19) holds `canonicalize`, `isValid`, RFC 4647 `match` with the Chinese script
rule, `resolve`, `candidates(component, preferred, source)`, the global language (`setGlobal`,
`global`, listeners, `generation`) and `localesChanged()`. It lives in core so baked titles switch
without the `titleart` module. Lua exposes it only as `TitleArt.setLanguage` and
`TitleArt.onLanguageChanged` (`engine/core/script/binding/TitleArtClassesLua.cpp:410-418`), which
exist only when `JASPERENGINE_TITLE_ART` is compiled.

**Device languages exist on every platform.** `System::getPreferredLocales()` (base `{"en-US"}`,
`engine/core/System.h:290-299`, `System.cpp:178-188`) is overridden on Windows
(`engine/platform/win/SystemWin.cpp:262`), macOS (`mac/SystemMac.mm:243`), Linux
(`linux/SystemLinux.cpp:184`), Android (`android/JasperEngineAndroid.cpp:439`), iOS
(`apple/JasperEngineApple.mm:619`) and the web (`emscripten/JasperEngineWeb.cpp:1613`); Lua has
`System.getLocale`, `getLanguageCode`, `getPreferredLanguages` (`CoreClassesLua.cpp:4059-4063`).
Nothing reacts when the user changes the OS language while the game runs: the only caller of
`TitleArtLanguage::localesChanged()` is the editor's simulated language (`editor/Platform.cpp:177`).

**The Game view already simulates a language.** Device > Language with 14 presets plus a custom tag
(`editor/window/GameWindow.cpp:338-343`, `:1171-1190`), `configure_game_view {language}`
(`editor/mcp/tools/ViewTools.cpp:2036-2044`, schema `:2374`), returned by
`editor::Platform::getPreferredLocales` (`editor/Platform.cpp:295-299`). It drives Title Art and
`System.getLocale`; no UI text changes with it, so the analyst's "nothing to drive" is half true.

**No string tables, no message format, no keyed UI.**
- No runtime string lookup, plural rule or message formatter exists in `engine/` (searched
  `plural`, `MessageFormat`, `l10n`, `i18n`: no hits outside Title Art and vendored libraries).
- S17 lists localisation out of scope (`docs/specs/S17-htmlui-widgets.md:40`); S87 too
  (`S87-title-localisation-interchange.md:90-91`), and its D3 (`:899-906`) fixes three things this
  spec must reuse: keys `<string_namespace>.<text_key>` (`[a-z0-9_]{1,48}` and `[a-z0-9_.-]{1,64}`,
  `:253-259`), its CSV/XLIFF files (§6.6, `:1110-1135`), and the two settings with R12-R13's
  resolution.
- The CSV/XLIFF reader and writer exist for titles: `editor/titleart/localisation/TitleStrings.h`
  (contract `:4-21`: RFC 4180 CSV with delimiter detection and Unity Localization headers, XLIFF 1.2
  export, 1.2 and 2.0 import, inline markup refused per unit, bounded parsers) and
  `TitleStrings.cpp` (550 lines). Its row type (`TitleStringRow`) is generic; routing to documents is
  title-specific.

**HTML UI.** `lang` becomes the inherited `-rmlui-language` property
(`engine/core/htmlui/compat/html/transforms/AttributeTransform.cpp:376-385`), `dir` becomes
`-rmlui-direction` (`:370-375`), `:lang()` is rewritten to `[lang|=x]` without inheritance
(`compat/css/SelectorRewriter.cpp:1019-1026`). The language reaches HarfBuzz
(`RmlFontEngine.cpp:407`, `engine/core/util/TextShaper.cpp:34-35`) but not font choice: a face chain
is keyed by families, italic, weight and size, and the fallback faces follow in registration order
whatever the language (`RmlFontEngine.cpp:262-310`, `FontRegistry.h:23-39`), so a Japanese page and
a Chinese page get the same Han glyphs. `data-*` attributes are kept in the RML
(`compat/tables/ElementTable.cpp:190`); `translate` is dropped with an Info (`:245`). Data models are
created in `engine/core/object/ui/UIDataModel.cpp:148`; RmlUi offers
`DataModelConstructor::RegisterTransformFunc` and `DataModelHandle::DirtyAllVariables`
(`engine/libs/rmlui/Include/RmlUi/Core/DataModelHandle.h:20, :96`). Documents load through
`HtmlUIContext::loadDocument` (`engine/core/htmlui/HtmlUIContext.cpp:280`).

**MCP.** `inspect_ui` and `assert_ui` are core tools (`editor/mcp/tools/HtmlUiTools.cpp:1969`,
`:2004`); `assert_ui` already re-lays out per `viewports` entry (`:1302-1308`) and has `text_fits`
(`engine/core/htmlui/HtmlUIAssertions.cpp:720`). There is no tool for string data.

**Text component.** `TextComponent::text` is one literal string (`engine/core/component/TextComponent.h:30`).

**Build flavours.** `Engine::isDevelopmentBuild()` (`engine/core/Engine.h:825-828`) tells a
Development Build from a release one.

## 4. Reference

| Source | Pattern studied | Take / improve |
|---|---|---|
| Craftsman `tools/l10n_app/build.py`, `include/mcpe015/app_l10n.h` | One generated catalog of 11 languages keyed by the **English source text** as written in code, `fromEn(en)` at each module's English branch, English fallback; the game language comes from the in-game menu, not the phone; `code(lang)` goes to the HTML window's `<html lang>` so CJK fonts resolve | Take: one catalog, a guaranteed fallback, the language pushed into `<html lang>`, the in-game choice beating the device. Improve: stable keys instead of source text as key (an edited English sentence silently orphans 10 translations there), a source hash marks stale translations (S87's `source_hash`) |
| Craftsman `include/mcpe015/srv_l10n.h` | The server translates per recipient by recognising already-formatted text against templates (`%s`/`%d` holes) and re-formatting in the recipient's language; off means zero cost | Take: per-recipient language and zero cost when off. Do not copy: regex/template recognition of formatted strings (ambiguous, slow, breaks on nested arguments). We send key + arguments (D9) |
| Craftsman `tools/l10n_srv/audit.sh` | An audit build logs every text that reached a player without a catalog row | Take: missing-text capture in Development Builds and the editor, per key and per literal text node (R20-R22) |
| Craftsman per-module tables (`{"key", es, en, pt}`, `l == "es" ? ...`) | Hard-coded per-screen language tables | Anti-pattern: every table grows a column per language and every screen its own `langIndex()`; String Tables replace them |
| Unity Localization package (docs only) | Locales, String Table Collections with one table per Locale, Smart Strings, Locale Selectors (PlayerPrefs, System Locale, Specific), Pseudo-Locales, Localize String Event, CSV and XLIFF extensions | Names: **Localization Tables** window, **String Table**, **Locale** → we keep S87's "language", **Pseudo-Locale**. Improve: one key namespace shared with Title Art, ICU instead of Smart Strings (translation tools understand ICU), no per-component event plumbing |
| Project Fluent DOM bindings (docs only) | `data-l10n-id` / `data-l10n-args` on elements | Attribute names only |
| ICU MessageFormat, CLDR plural rules and number symbols (Unicode License v3) | Message syntax, plural categories per language | A subset (D4); CLDR data compiled by a generator into a table with its licence notice |

**IP rule.** The Craftsman tree is a port reconstructed from a retail binary. Ideas only: no code,
data, catalog rows, translations, assets or game-specific mechanics are copied from it, and nothing
here is specific to its genre. Unity, Fluent and ICU are cited for names and syntax only.

## 5. Requirements

### Functional: tables and messages

- **R1 String Table.** `Name.stringtable` (JSON, §6.1) holds `namespace` (S87's rule, made from the
  file stem at creation and kept on rename), `source_language` (default: the project default),
  and ordered entries `{key, text, context, max_chars}`; `key` follows S87's `text_key` rule. The
  global key is `<namespace>.<key>`. A key whose first segment names no namespace of the project is
  looked up in the **Default Table** (`localization.default_table`, a namespace; empty: the
  project's only table when it has one, else none), so a small game can write `get("play")`. A namespace is unique in the project, and distinct from every
  Title Art `string_namespace` (editor error `l10n-namespace-taken` naming the owner).
- **R2 Translations.** `Name.<lang>.translation` beside its owner holds `owner {guid, path}`,
  `language` and per key `{text, status, source_hash}`; `status` is `translated` or `needs_review`
  (S87's values). Writing a source text whose hash differs from a translation's `source_hash` turns
  that translation `needs_review` (it is still shown, R8). An owner is a `.stringtable` or a
  `.dialogue` (`dialogue-graphs.md` supplies its entries).
- **R3 Messages.** Texts are ICU MessageFormat restricted to D4: `{name}`, `{n, number}` (also
  `integer`, `percent`), `{n, plural, =0 {...} one {# item} other {# items}}` with `offset:`, `{g,
  select, a {...} other {...}}`, nesting up to 4 levels, apostrophe quoting. A pattern that does
  not parse is an editor error at the cell (with column) and at runtime prints its source text
  verbatim, once-logged; formatting never throws and never reads past the pattern.
- **R4 Plurals and numbers.** CLDR cardinal plural rules and the decimal and group symbols of every
  CLDR language (pinned release, D4) are compiled into `PluralRules.gen.inc`; a tag resolves by
  dropping subtags (`pt-PT` → `pt-PT` rules if CLDR has them, else `pt`), unknown → `root` (only
  `other`). `#` formats like `{n, number}`.
- **R5 Arguments.** Lua tables, C++ `LocalizationArgs` (name → string, int64 or double) and HTML
  `data-l10n-args` (a JSON object) supply arguments; a string argument that is itself `@key` is
  replaced by that key's text first (one level), so `{item}` can be a translated noun. A Lua list part (C++: arguments named `"0"`, `"1"`, ...) fills
  `{0}`, `{1}`, ... (positional, Unity Smart Strings' habit). Missing
  arguments print `{name}` in editor/Development builds and an empty string in release.

### Functional: the game language

- **R6 Available languages.** `localization.languages` when not empty, else the union of every
  table's source language and translation languages, in project-default-first order.
- **R7 Resolution** (once at start and on every change): the first candidate that
  `TitleArtLanguage::match`es an available language: (1) `Localization.setLanguage` value
  (= `TitleArt.setLanguage`, one global, D2), (2) the remembered choice when
  `localization.remember_language` (UserSettings key `jasper.language`), (3) the device's
  `getPreferredLocales()` in order (the Game view's simulated language in the editor), (4)
  `localization.default_language` effective value. `Localization.language` is the result.
- **R8 Fallback per key**: the language → its truncations present in the available set
  (`zh-Hant-TW` → `zh-Hant`; `pt-BR` → `pt`) → the project default → the owner's source text. A
  `needs_review` translation counts as present. A key no table has: HTML keeps its authored content,
  the Text component keeps its `text`, `Localization.get` returns its `fallback` argument, else `""`
  in release and `⟦key⟧` in the editor and Development Builds; each such key logs one warning per
  language per run.
- **R9 Switching.** `Localization.setLanguage(code)` (`""` = back to device/remembered) validates
  with `TitleArtLanguage::isValid` (invalid: false and nothing changes), re-resolves, and when the
  result changes: loads the tables lazily (R13), re-translates every keyed HTML element, Text
  component and data model, notifies `onLanguageChanged(code)` once, and Title Art follows through
  its existing listener. With `remember_language`, a non-empty code is written to UserSettings.
- **R10 OS locale changes.** Each platform calls `TitleArtLanguage::localesChanged()` when the OS
  language list changes: Android `onConfigurationChanged` (locale), iOS and macOS
  `NSCurrentLocaleDidChangeNotification`, Windows `WM_SETTINGCHANGE` with `"intl"`, the web
  `languagechange`; Linux reads the environment once (no event). Only a game that has not set a
  language explicitly changes.

### Functional: consumers

- **R11 HTML UI keys.** In a document whose source contains `data-l10n` (flag computed at compile,
  so other documents pay nothing):
  - `data-l10n-id="ns.key"` replaces the element's text content with the message;
    `data-l10n-args='{"count": 3}'` supplies arguments; `data-l10n-attr-<name>="ns.key"` sets
    attribute `<name>` (`placeholder`, `title`, `alt`, `aria-label`, `value`);
  - an element with `data-l10n-id` must contain only text; child elements → diagnostic
    `HTMLUI-L10N-0001` (Warning, at compile and in `check --ui`) and the element is left as written;
  - keyed elements are collected once per load and re-translated in place on a language change
    (no recompile, no reload); rows a `list-view`/`grid-view`/`tree-view`/`table-view` instantiates
    from a `<template>`, and fragments inserted by `insertAdjacentHTML`/`innerHTML`/`outerHTML`,
    are translated as they are made;
  - Lua `element:setLocalized(key, args?)` writes both attributes and translates at once;
    `doc:translate()` re-collects after scripts edited attributes directly.
- **R12 Root language and direction.** In such documents the root gets `lang` = the game language
  and, unless the author wrote `dir` on `<html>`, `dir` = `rtl` for right-to-left scripts (Arab,
  Hebr, Thaa, Syrc, Nkoo, Adlm, and `ar-XB`) else `ltr`, on load and on every change.
  `translate="no"` is kept (ElementTable row changes from dropped to kept) and excludes a subtree
  from capture (R20).
- **R13 Loading.** A table's files for the language chain load on the first lookup of its namespace
  after a change (cooked, R27; JSON in the editor). Tables of other namespaces stay unloaded.
  In the editor a table edit (window, MCP, file change) applies at once, in Edit mode as in Play:
  keyed HTML and Text components show their messages in the Scene and Game views without Play.
- **R14 Language fonts.** `localization.fonts` (`{lang: [font asset paths]}`) registers those faces
  as fallbacks for that language: an HTML text run whose inherited `-rmlui-language` matches (S87
  match) gets them before the generic fallback faces; the face-chain cache key gains the matched
  language. A Text component whose `font` is empty uses the first font of the game language.
- **R15 Data models.** Every data model the engine creates gets the transform `l10n`:
  `{{ 'shop.coins' | l10n('count', coins) }}` (name/value pairs); a language change calls
  `DirtyAllVariables` on models that used it.
- **R16 Text component.** New field `localizationKey` (Inspector **Localization Key**, with a key picker):
  when set and the key exists, `text` shows the message at start and after every change; Lua
  `text:setLocalized(key, args?)` sets key and runtime arguments (arguments are not saved).
- **R17 Lua.** `Localization.get(key, args?, fallback?)`, `Localization.format(pattern, args?,
  language?)`, `Localization.has(key)`, `Localization.language` (rw: assigning it is `setLanguage`), `Localization.languages` (r),
  `Localization.setLanguage(code)`, `Localization.getLanguageName(code)` (the endonym, shipped in
  the manifest), `Localization.isRightToLeft(code?)`, `Localization.onLanguageChanged`. Stubs in
  `engine/core/script/binding/stubs/Localization.lua`. Available whatever modules a game compiles.
- **R18 C++.** `jasperengine::Localization` (§6 sketch), exported, same semantics; usable from C++
  scripts and from editor code.

### Functional: development aids

- **R19 Pseudo-locales.** `en-XA`: the project default language's text with ASCII letters accented
  (`a`→`á`, fixed map), lengthened by 40 % (rounded up, filler `·`), wrapped in `⟦` `⟧`; `ar-XB`:
  the same text in right-to-left order with `dir=rtl`. ICU syntax and `{arguments}` are preserved
  (only literal text is transformed). Both are always "available" in the editor and Development Builds, never in release.
  Literal (unkeyed) text stays plain, so it stands out.
- **R20 Capture** (editor Play and Development Builds; compiled out of release):
  - *missing*: a key absent from the language's chain before the source (`kind: untranslated`), a
    key no table has (`unknown_key`), a `needs_review` translation shown (`stale`), each with the
    first HTML element ref or GameObject and a count;
  - *literal*: `Localization.scan()` walks the live HTML text nodes and Text components and reports
    text with at least one letter that is not under a keyed element, not under `translate="no"`, and
    not equal to one of the last 1024 strings `Localization` produced (a ring of 64-bit hashes, so
    text a script set from `get()` counts as localised).
  Scanning in the default language is allowed (it finds hard-coded text before translation starts).
- **R21 Export checks.** Exports warn `W-L10N-UNKNOWN-KEY` (a `data-l10n-id` or Text
  `localizationKey` no table has), `W-L10N-UNTRANSLATED` (one line per shipped language with the count),
  `W-L10N-STALE`, `W-L10N-LANGUAGE-DROPPED` (a translation of a language outside a non-empty
  `localization.languages`, left out), `W-L10N-BAD-MESSAGE`; none blocks the export.
- **R21a Project check.** `jasperengine-editor check` (and `--files`, the pre-commit hook) runs tool
  `strings` without the engine: a key twice in one table, a namespace two owners claim, a message
  that does not parse, a Default Table that names no table, and unknown keys referenced by HTML
  (`data-l10n-id`, `data-l10n-attr-*`), Text `localizationKey` and Lua string literals passed to
  `Localization.get`, `Localization.has` and `setLocalized`. Errors for the first four, warnings for
  unknown keys (the same codes as R21).

### Platform & backends

- **R22** CPU only, no GPU path; every export target; single-threaded web unchanged. Lookups and
  formatting are main-thread or any-thread pure (tables are immutable after load and swapped
  atomically on a change).
- **R23** RTL, CJK and complex scripts reach HarfBuzz as today; the spec only adds the language and
  direction and the per-language fallback faces.

### Performance (release, cooked tables; desktop = build server A, mobile = S13 AC23's Mali-G57/Adreno 610 class device, web = Chrome on that desktop)

- **R24 Lookups**: `get` of a 40-byte message without arguments ≤ 0.4 / 1.5 / 2 µs and no heap
  allocation besides the returned string (C++ `getView` returns a `std::string_view`: 0
  allocations); with two arguments and a plural ≤ 2 / 8 / 10 µs.
- **R25 Switch**: changing language with 6 documents holding 300 keyed elements and a 5,000-entry
  table ≤ 6 / 20 / 25 ms on the main thread, excluding RmlUi's relayout; first lookup of a namespace
  (cooked load of 5,000 entries, ~250 KB) ≤ 2 / 8 / 10 ms.
- **R26 Memory**: a loaded table = its UTF-8 text + 16 bytes per entry; only the current chain's
  languages are resident (at most 3 files per namespace). Plural rule and symbol data ≤ 24 KB code
  and data in the player.
- **R27 Zero cost.** A project without String Tables ships no manifest; `Localization` is inactive:
  no file read at start, documents without `data-l10n` skip the pass (one flag test), the `l10n`
  transform is not registered, no capture ring exists. Measured: `LocalizationTest.InactiveCostsNothing`
  counts 0 allocations and 0 file opens across start, a document load and 600 frames.

### Editor UX (human)

- **R28 Window > Localization Tables** (`window.localizationTables`, Unity's name):
  - left: the project's tables and dialogues (namespace, owner path, per-language coverage %);
  - grid: Key | Context | Max | source language | each language, cells coloured missing (red),
    needs review (amber), over `max_chars` (count shown red); search (key, any text), filters
    "Missing in…", "Needs review", "Over length"; the language columns follow
    `localization.languages` order, hidden per column;
  - cell editing inline (multi-line), with ICU validation under the cell and **Insert Plural**
    (writes the skeleton with the language's CLDR categories: `ru` gets one, few, many, other);
    **Mark Reviewed** on a `needs_review` cell;
  - toolbar: **Add Entry**, **Remove**, **Rename Key** (table and translations; lists the
    references it did not rewrite: HTML, scenes, scripts), **Find References**, **Add Language**,
    **Import…**, **Export…** (CSV, XLIFF; S87's dialogs and options), **Pseudo Preview** (the grid
    shows `en-XA` beside the source);
  - a **Play Misses** tab lists R20's captures; **Extract to Table** on a literal text row adds an
    entry with that text and writes `data-l10n-id` on its element in the `.html` (located through
    the compiled source map), one `CommandGroup` over both files;
  - every edit is one named undo step (`StringTableEditCmd`, file snapshots like `GraphEditCmd`);
    a cell drag-typing session folds into one step; a dialogue's rows edit its `.dialogue` and
    translation files in one `CommandGroup`.
- **R29 Create menu.** Project window Create > Localization > String Table (namespace from the name,
  source language from the project default); double-click opens the window on it.
- **R30 Project Settings > Localization** gains **Remember Language Choice** (`localization.remember_language`, default on), **Default
  Table** (`localization.default_table`, R1) and
  **Language Fonts** (per language, a font list), each one undo step; the existing rows keep their
  text but drop "Title Art" from Languages' help (it now governs every table).
- **R31 Game view.** Device > Language gains **Pseudo (en-XA)** and **Pseudo RTL (ar-XB)**; the
  toolbar label shows the resolved game language when it differs from the simulated one
  ("ja-JP → ja"). A **Localization** item under the Game view's Stats overlay shows misses counted
  this Play (click opens the window filtered to them).
- **R32 Inspector.** Text: **Localization Key** row with picker (search over all keys, shows the
  source text) and a "missing" badge; UI Document: a read-only **Localization** foldout in Play
  (keyed elements, misses).

### Agent UX (MCP, Lua, CLI)

- **R33 `edit_string_table`** (group `localization`, ≤ 1,500 characters): no `path` lists tables
  and dialogues with coverage per language and this Play's miss count; `path` + `get` returns
  entries (filtered by `keys`, `language`, `missing`, `limit` 200) with status and over-length
  flags; `ops` (`set {key, text, language?, context?, max_chars?}`, `remove`, `rename`,
  `add_language`, `remove_language`, `mark_reviewed`) applied as one undo step, refused whole when
  any op fails (error names the op index and the fix); `create {namespace?, source_language?}`;
  `import {file, add_languages?}` (S87 statuses and notes); `export {format, languages?, out}`.
  Results carry `next` hints (e.g. keys referenced by HTML but absent, with the `set` op to add them).
- **R34 `inspect_ui {l10n: true}`** adds per document `l10n: {language, keyed, literal: [{ref,
  text}], missing: [{ref, key, kind}]}` (≤ 50 each, counts beyond); in edit mode the static
  document is scanned, in Play the live one.
- **R35 `assert_ui {languages: [...]}`** (≤ 8 tags, pseudo allowed) runs every check once per
  language (× `viewports`, ≤ 24 layouts), switching the language for the layout and restoring it;
  failures name the language. Main use: `text_fits` in `de`, `en-XA`.
- **R36 `configure_game_view {language}`** accepts `en-XA`/`ar-XB` (no schema change; the
  parameter's description gains "en-XA pseudo").
- **R37 Docs.** New `docs/localization.md` (tables, messages, fallback, HTML, Text, Lua, fonts,
  pseudo, capture, export, multiplayer pattern) and `docs/mcp/guide/localization.md` (recipes:
  localise a menu; find literal text; check German fits); `docs/mcp/guide/ui.md` (language section
  points there), `html-ui.md` (attributes), `lua-api.md` lines, `TOOLS.md` and
  `html-ui-support.md` regenerated; one "Other areas" index entry in the guide.
- **R38 Eval.** Cases in `tools/mcp/eval/cases.py`: `guard_l10n_keyed_html` (agent localises a
  button with `data-l10n-id` + `edit_string_table` instead of writing a Lua table) and
  `guard_l10n_assert_languages` (agent checks overflow with `assert_ui {languages}` instead of
  switching the Game view and screenshotting).

### Data & compatibility

- **R39 New files** carry `format` and `version: 1`; readers keep unknown members (written back
  verbatim) and refuse a newer `version` with a message naming the editor version needed.
- **R40 New fields with defaults, no version bump**: `TextComponent.localizationKey = ""`; project
  settings `localization.remember_language = true`, `localization.default_table = ""`,
  `localization.fonts = {}`. Reflection tables,
  codecs, `ReflectParity` golden and `catalog_snapshot.json` gain `localizationKey`.
- **R41 Asset types.** `AssetType::StringTable` (37 at the time of writing, after #149's IES Profile 36; covers `.stringtable`
  and `.translation`, GUID in-file) appended after `TitleArt = 34` with an explicit value; Lua
  name `stringtable`. Moving or renaming an owner moves its `<stem>.<lang>.translation` siblings
  with it (one undo step); translations also resolve their owner by GUID.
- **R42** No meaning of an existing key changes, so no migration: S87's settings keep their keys
  and values; `TitleArt.setLanguage` keeps working and now also drives `Localization` (D2).

## 6. Design constraints & decisions

- **D1 Layer.** Runtime in `engine/core/localization/` (`jasperengine::l10n` internals,
  `jasperengine::Localization` facade): `MessageFormat`, `PluralRules` (+ `.gen.inc`),
  `StringTableRuntime` (cooked and JSON readers), `Localization` (resolution, chain, capture,
  pseudo), `HtmlL10n` in `engine/core/htmlui/` (the DOM pass, guarded by `JASPERENGINE_HTMLUI`).
  Editor side in `editor/localization/` (`StringTableDocument`, `StringTableEditCmd`,
  `StringFiles` lifted from `TitleStrings.cpp`, cooker, window). Core, not a player module: the
  facade is small and dialogue, HTML UI and Text all need it; R27 makes it free when unused.
- **D2 One game language.** The global stays `TitleArtLanguage::setGlobal` (no second global, no
  rename of a shipped class); `Localization::setLanguage` calls it, listens to it and to
  `localesChanged`, and resolves against the tables' languages. `TitleArt.setLanguage` and
  `Localization.setLanguage` are documented as the same value; the remembered choice (R7 step 2)
  becomes a candidate for titles too.
- **D3 Source in the owner, translations beside it.** Mirrors S87 D1/D2 (source in the document,
  sparse overrides) and lets `dialogue-graphs.md` own its source text. Translators' merge conflicts
  stay per language file. Keys never derive from source text (the Craftsman catalog's weakness).
- **D4 ICU subset, CLDR data.** ICU because XLIFF tools (and agents) know it and S87's files carry
  text verbatim; the subset is what games need for counts, names and grammatical choices. The
  generator `tools/l10n/gen_plural_rules.py` reads a pinned CLDR JSON release (`cldr-core`
  `supplemental/plurals.json`, `cldr-numbers-full` symbols), compiles each rule to a `switch` over
  `n, i, v, w, f, t, e` operands, and writes `engine/core/localization/PluralRules.gen.inc` and
  `LICENSE.cldr.txt`; its row goes into AGENTS.md's Generated files table.
- **D5 Cooking.** Export writes per (namespace, shipped language) `l10n/<ns>.<lang>.strbin`
  through `AtomicFile`: header, key hashes (FNV-1a 64, sorted, binary search; collisions in one table
  are an export error naming both keys), offsets, one UTF-8 blob, each message pre-tokenised (a flag
  for "plain text": no parse at lookup). `l10n/manifest.json`: namespaces, languages with endonyms
  (from `TitleLanguageNames`), source languages, default, `remember_language`, `default_table`, fonts. Dialogue owners
  are cooked the same way (`dialogue-graphs.md`). Editor Play reads the JSON files directly and
  rebuilds a table on hot reload.
- **D6 Shared interchange code.** `TitleStrings.cpp`'s CSV/XLIFF parsers and writers move to
  `editor/localization/StringFiles.{h,cpp}` with neutral types (`StringRow`, `StringsFile`,
  `StringsNote`); `TitleStrings.h` keeps its functions as wrappers, so `TitleStringsTest` passes
  unchanged. String Table import routes rows by `file`, else by namespace; a title's row is reported
  `unknown_key` with "import it with edit_title import_strings".
- **D7 HTML pass after load, not in the compiler.** The compiled RML stays language-free (one
  compile cache entry, patch hot reload untouched); the pass runs in `HtmlUIContext::loadDocument`
  after `applyAutofocus`, keeping per document a vector `{Rml::Element*, key, args, attrs}` that a
  change walks. Values are inserted as text nodes, never parsed as markup.
- **D8 Capture is a development feature.** `#if` on `JASPERENGINE_L10N_CAPTURE` (on in the editor
  and Development Builds); release players compile the hooks to nothing.
- **D9 Multiplayer.** A server never formats player-facing text: it sends `{key, args}` and each
  client formats in its own language; documented with a Lua example. This replaces the Craftsman
  server's recognise-and-reformat.
- **D10 Pseudo-locales are virtual languages** computed from the default language's text on
  lookup (no files), cached per key while active.

```cpp
// engine/core/localization/Localization.h
namespace jasperengine {
    using LocalizationValue = std::variant<std::string, int64_t, double>;
    using LocalizationArgs = std::vector<std::pair<std::string, LocalizationValue>>;

    class JASPERENGINE_API Localization {
    public:
        static bool isActive();                                   // a manifest or a table exists
        static std::string get(std::string_view key, const LocalizationArgs& args = {},
                               std::string_view fallback = {});
        static std::string_view getView(std::string_view key);    // plain messages; "" when missing
        static bool has(std::string_view key);
        static std::string format(std::string_view pattern, const LocalizationArgs& args,
                                  std::string_view language = {});
        static const std::string& getLanguage();                  // resolved
        static const std::vector<std::string>& getLanguages();    // available (R6)
        static bool setLanguage(const std::string& code, std::string* error = nullptr);
        static std::string getLanguageName(const std::string& code);
        static bool isRightToLeft(std::string_view code);
        static FunctionSubscribe<void(const std::string&)>& getOnLanguageChanged();
        // development builds and the editor (D8)
        struct Miss { std::string key, language, kind, where; uint32_t count = 0; };
        static std::vector<Miss> getMisses();
        static void clearMisses();
        // tables (the editor's Play, hot reload, tests)
        static void loadManifest(const std::string& path);         // "" = none: inactive
        static void reloadNamespace(const std::string& ns);
        static void resetForTesting();
    };
}
```

```lua
-- stubs/Localization.lua (hand-written shapes)
---@class Localization
---@field language string        # resolved game language, e.g. "pt-BR"
---@field languages string[]
---@field onLanguageChanged fun(code: string)
---@field get fun(key: string, args?: table<string, string|number>, fallback?: string): string
---@field format fun(pattern: string, args?: table, language?: string): string
---@field has fun(key: string): boolean
---@field setLanguage fun(code: string): boolean   # "" = device / remembered choice
---@field getLanguageName fun(code: string): string
---@field isRightToLeft fun(code?: string): boolean
---@field scan fun(): table          # development builds: {literal = {...}, missing = {...}}
```

```html
<button data-l10n-id="ui.menu.play">Play</button>
<p data-l10n-id="ui.shop.coins" data-l10n-args='{"count": 3}'>3 coins</p>
<input data-l10n-attr-placeholder="ui.search.hint" placeholder="Search">
<span translate="no">JasperQuest</span>
```

MCP sketch (`edit_string_table`, group `localization`; schema via `McpSchema` builders):
`{path?, create?: {namespace, source_language}, get?: {keys, language, missing, limit},
ops?: [{op, ...}], import?: {file, add_languages}, export?: {format: csv|xliff, languages, out}}`.
`inspect_ui` gains `l10n` (boolean); `assert_ui` gains `languages` (array of strings, ≤ 8).

### 6.1 File formats

```json
{"format": "jasperengine.stringtable", "version": 1, "guid": "6f0c…", "namespace": "ui",
 "source_language": "en",
 "entries": [
  {"key": "menu.play", "text": "Play", "context": "Main menu button", "max_chars": 12},
  {"key": "shop.coins", "text": "{count, plural, one {# coin} other {# coins}}"}]}
```

```json
{"format": "jasperengine.translation", "version": 1, "guid": "a91e…",
 "owner": {"guid": "6f0c…", "path": "assets/l10n/ui.stringtable"}, "language": "de",
 "entries": {
  "menu.play": {"text": "Spielen", "status": "translated", "source_hash": "3f2a9c01"},
  "shop.coins": {"text": "{count, plural, one {# Münze} other {# Münzen}}", "status": "needs_review",
                 "source_hash": "77b0e412"}}}
```

- Entries of a translation are sorted by key; two-space indent, LF, final newline (diff-friendly,
  deterministic like `graph::save`). `source_hash` is S87's (FNV-1a 32 of the NFC source, hex).
- CSV and XLIFF are S87 §6.6 unchanged: `key` is the global key, `file` the owner path in
  multi-owner exports, `max_chars` → XLIFF `maxwidth`.

## 7. Acceptance criteria

Unit tests follow docs/testing.md naming. "Red" says what fails without the change.

- **AC1 Messages.** `engine/tests/core/localization/MessageFormatTest.cpp`:
  `MessageFormat.SubstitutesNamedArguments`, `PluralPicksTheCldrCategory` (ru 1/2/5/21/1.5, ar
  0/1/2/3/11/100, pl, ja, `=0` before `one`, `offset:`), `SelectAndNestedPlural`,
  `NumberUsesLanguageSymbols` (de `1.234,5`, fr narrow no-break space), `BadPatternsPrintSourceText`
  (500 mutated patterns from a seed: no throw, output bounded, ASan clean). Red: no formatter exists.
- **AC2 CLDR data.** `PluralRulesTest.GeneratedTableMatchesCldrSamples`: every `@integer`/`@decimal`
  sample of the pinned CLDR file (the generator writes them into a test `.inc`) maps to its category
  for every language. Red: no table.
- **AC3 Resolution and fallback.** `engine/tests/core/localization/LocalizationTest.cpp`:
  `ResolvesWithTitleArtRules` (device `zh-HK` → `zh-Hant`, `pt-PT` → `pt-BR` when only that exists,
  remembered choice beats device, `setLanguage("")` returns to device), `FallbackChain`
  (`pt-BR`→`pt`→default→source per key, `needs_review` shown), `UnknownKeyNeverShowsTheKeyInRelease`
  (`isDevelopmentBuild` false: fallback or ""; true: `⟦key⟧`; one warning per key and language),
  `TitleArtSetLanguageDrivesLocalization`, `InactiveCostsNothing` (R27 counts). Red: no
  `Localization`.
- **AC4 Files.** `editor/tests/localization/StringTableDocumentTest.cpp`:
  `RoundTripIsByteIdentical` (unknown members kept), `SourceEditMarksTranslationsForReview`,
  `NewerVersionIsRefusedWithTheVersionNeeded`, `NamespaceClashWithTitleArtIsAnError`.
- **AC5 Interchange.** `editor/tests/localization/StringFilesTest.cpp`: `ImportsS87CsvAndXliff`
  (the fixtures of `TitleStringsTest`), `RoutesTitleRowsAsUnknownKey`, `PluralsSurviveXliffRoundTrip`;
  `TitleStringsTest.*` unchanged and green after the move (D6).
- **AC6 Undo.** `editor/tests/localization/StringTableEditCmdTest.cpp`:
  `EachWindowEditIsOneUndoStep`, `RenameMovesTranslationsAndListsReferences`,
  `MovingTheOwnerMovesItsTranslations`.
- **AC7 HTML.** `engine/tests/htmlui/TestL10nDocument.cpp` (htmlui suite):
  `HtmlL10n.TranslatesKeyedElementsAndAttributes`, `SwitchesLanguageWithoutReload` (same
  `Rml::Element*` before and after), `SetsLangAndDirOnTheRoot` (author `dir` kept),
  `TemplateRowsAndInsertedHtmlAreTranslated` (a `list-view` of 1,000 rows scrolled; red: rows show
  the template text), `ChildElementsReportL10n0001`, `DocumentsWithoutL10nAreUntouched` (0
  allocations by the pass), `DataModelTransformFollowsLanguage`.
- **AC8 Fonts.** `TestFontStack.cpp`: `FontRegistryTest.LanguageFontsLeadTheFallbackChain` (a face
  registered for `ja` precedes the generic fallbacks for `lang=ja-JP`, not for `zh`); red: the chain
  ignores language.
- **AC9 Pseudo golden.** `TestGoldenText.cpp`: `GoldenText.PseudoLocaleEnXa` and `PseudoLocaleArXb`
  render `examples/settings` with `fixtures/fonts/Roboto-Bold.ttf`; goldens
  `engine/tests/htmlui/golden/l10n/pseudo_en_xa.png`, `pseudo_ar_xb.png`, created on glcore, matched
  on Vulkan.
- **AC10 Cooked tables.** `editor/tests/localization/StringTableCookTest.cpp`:
  `CookedLookupMatchesJson` (every key, 12 languages), `HashCollisionIsAnExportError` (forced with a
  test seed); `engine/tests/core/localization/LocalizationBench.cpp` `LocalizationBench.Lookup` and
  `Switch` print `[bench]` lines and enforce R24/R25 through `perfSlack()`.
- **AC11 Text and Lua.** `editor/tests/engine/localization/TextLocalizationTest.cpp`:
  `KeyedTextFollowsLanguage`, `EmptyFontUsesLanguageFont`; `engine/tests/core/script/LocalizationLuaTest.cpp`:
  `LocalizationLua.ApiMatchesStubs` (every stub member callable), `OnLanguageChangedFiresOnce`,
  `EventIsClearedAtStop` (also `tools/check_event_clears.py` passes).
- **AC12 Capture.** `LocalizationTest.CaptureReportsUntranslatedUnknownAndStale`,
  `HtmlL10n.ScanReportsLiteralTextOnly` (keyed, `translate="no"`, and a `get()`-produced string are
  not reported; a hard-coded "Options" is); `LocalizationTest.CaptureCompiledOutOfRelease` (symbol
  absent in a release core build: `nm` check in the test's CMake step).
- **AC13 MCP.** `tools/mcp/tests/test_localization.py` (`CI = True`, own project):
  `test_group_hidden_until_enabled`, `test_edit_string_table_ops_undo_and_refusal`,
  `test_import_export_csv_xliff`, `test_inspect_ui_l10n_reports_literals_and_misses`,
  `test_assert_ui_languages_text_fits` (a German string overflows a fixed button: fails naming `de`;
  red: `languages` unknown), `test_pseudo_language_in_game_view` (screenshot text differs; Title Art
  untouched), `test_play_switch_language_live` (`eval_lua Localization.setLanguage('ja')`, then
  `inspect_ui` shows the Japanese text in the same element ref).
- **AC14 Shipped game.** `test_localization.py::test_export_ships_cooked_tables` (`JASPER_SLOW_BUILD=1`
  lane): prebuilt and compiled Linux exports run headless with `LANG=de_DE.UTF-8`, the game prints
  `Localization.get('ui.menu.play')` = `Spielen`; with `localization.languages: [en]` the de file is
  absent and `W-L10N-LANGUAGE-DROPPED` is reported. Web gate: the same project's web export prints it
  in the browser log with `navigator.languages = ['de']`.
- **AC15 OS locale change.** `engine/tests/core/localization/LocalizationTest.cpp`
  `LocaleEventReresolvesUnlessSetExplicitly` drives `TitleArtLanguage::localesChanged()` with a fake
  `System` (red: nothing listens). Platform call sites: `tools/android-emulator-test.sh` gains a step
  that changes the system locale with `adb shell cmd locale` while the test game runs and waits for
  its `[Localization] language de` log line; the web gate dispatches `languagechange` with a new
  `navigator.languages`. Windows, macOS and iOS events are a manual check recorded in the PR (change
  the OS language with the game open; the log shows the re-resolution).
- **AC16 Compatibility.** `ReflectParity.*` with `localizationKey` added; the upgrade-from-main project loads
  with no new load-report entry; `TitleLanguageShippingTest.*` unchanged.
- **AC17 Docs and budget.** `gen_tool_docs.py --check`, `gen_html_ui_support.py` output committed,
  `tool_budget.py --check tools/mcp/budget.json` (core growth from `inspect_ui`/`assert_ui`
  parameters fits, `localization` group within defaults), eval cases of R38 fail with the text
  removed (`tools/mcp/eval/README.md`).

## 8. Dependencies & conflicts

- **Uses** S87 (TitleArtLanguage, settings, CSV/XLIFF, `TitleLanguageNames`), S17 (items views'
  row instantiation hook), S16 (font engine), S34 (module manifest: localisation is core, no module),
  S31 (MCP conventions, budget), S03 (AtomicFile writes of tables, translations and cooked files).
- **Blocks / used by** `dialogue-graphs.md` (lines and options are table entries; its owner type
  and cooking use D3/D5). `text-entry-gamepad-cursor-back-button.md` (on-screen keyboard labels
  should be keys), `input-device-awareness.md` (glyph prompts with `{button}` arguments),
  `dev-console-for-agents.md` (a `l10n` console command can call `Localization.scan`),
  `shipped-game-diagnostics.md` (may upload capture data from Development Builds),
  `build-profiles.md` (a profile may narrow shipped languages; it sets `localization.languages`),
  `build-size-tools.md` (reports cooked `l10n/` bytes per language), `savegame.md` (the remembered
  language is UserSettings, not a save slot), `project-test-runner.md` (tests can loop
  `assert_ui {languages}`).
- **Merge hotspots**: `editor/Project.h` (LocalizationSettings), `editor/Stream.cpp`,
  `SettingsRows.cpp`, `ProjectSettingsWindow.cpp`, `editor/titleart/localisation/TitleStrings.cpp`
  (D6 move), `RmlFontEngine.cpp` (chain key), `HtmlUIContext.cpp` (post-load hook),
  `compat/tables/ElementTable.cpp` (`translate`, `data-l10n-*` rows), `HtmlUiTools.cpp` (two
  parameters), `GameWindow.cpp` (presets), `AssetType.h`/`AssetTypes.cpp` (values claimed by siblings:
  take the next free explicit value at merge), `TextComponent.h` + reflection, `Exporter.cpp`
  (cooking), `McpToolGroups.cpp`, `docs/mcp/conventions.md` group table, AGENTS.md Generated files
  row (D4).

## 9. Risks & defaults

- *Keys versus source-text keys*: authors may prefer writing English in HTML. Default: the authored
  content stays the fallback, and the Play Misses tab's **Extract to Table** (R28) makes the key
  path one click.
- *ICU in translators' hands*: broken plurals ship. Default: editor validation, import notes
  `bad_message` per row, `W-L10N-BAD-MESSAGE`, and runtime prints the source text.
- *Han unification*: a project without `localization.fonts` for ja/zh shows whichever CJK fallback
  font it registered first. Default: the window warns when a shipped CJK language has no language
  font.
- *Large tables on web*: lazy per namespace (R13) and cooked binary keep start cost bounded;
  default advice in docs: one namespace per screen group.
- *Plural data drift*: CLDR pinned; updating is a generator run plus AC2.
- *Two setters for one language* (`TitleArt.setLanguage`, `Localization.setLanguage`): kept for
  compatibility (R42); docs name `Localization.setLanguage` as the one to use.
- *Stale translations shown*: default show (better than source) and count; the export warns.

## Deviations

- **D-1 Pre-tokenised messages (D5).** The cooked `.strbin` stores per entry the key hash, offset,
  length and two flags (plain text, needs review): 16 bytes per entry as R26 asks. A message with
  ICU syntax is parsed at lookup instead of being stored pre-tokenised; plain messages (the common
  case) skip the parser through the flag.
- **D-2 Hash collisions (AC10).** `HashCollisionIsAnExportError` forces the collision with a key
  written twice (both hash alike) instead of a test seed; the error path and message naming both keys
  are the same.
- **D-3 HTML pass (R11, D7).** The pass runs on the elements as RmlUi makes them (an RmlUi plugin)
  rather than from a compile-time flag: a document without `data-l10n-*` pays one attribute scan per
  created element and nothing else, and none while the project has no String Tables. This also covers
  template rows and inserted markup without a hook per widget.
- **D-4 Root direction (R12).** A left-to-right language removes the direction the pass set instead of
  writing `ltr`, so an author's `dir` on `<html>` stays in left-to-right languages; a right-to-left
  language sets `rtl` over it.
- **D-5 Language fonts (R14).** The face chain is keyed by the game language (the language every keyed
  document's root gets), not by each text run's inherited `-rmlui-language`, which RmlUi does not
  pass to the font engine. Documents loaded after a language change get its fonts; already-loaded
  documents keep their face handles until reloaded.
- **D-6 `setLanguage("")` (R9, AC3).** `""` also forgets the remembered choice, so the game follows the
  device again (AC3 asks `setLanguage("")` to return to the device).
- **D-7 Remembered choice for titles (D2).** The remembered choice is a candidate of
  `Localization` only; Title Art follows `Localization.setLanguage` (the same global) but not the
  value remembered from an earlier run.
- **D-8 ar-XB (R19).** The literal text is reversed code point by code point (plus `rtl` on the root)
  rather than wrapped in bidi controls, so it reads backwards whatever the text shaper does with
  RLO/PDF.
- **D-9 Capture in release (AC12).** Capture compiles out with `JASPERENGINE_L10N_CAPTURE=0` (release
  players). The check is `tools/l10n/check_capture_symbols.py` over the release and editor engines
  (D-11), not the spec's `CaptureCompiledOutOfRelease` gtest.
- **D-10 Inactive cost (R27, AC3).** `InactiveCostsNothing` counts 0 allocations across 600 rounds of
  every lookup call without tables; the start, document load and frame loop of the spec's version
  need a running engine the core test binary does not have.
- **D-11 How the rest was built.** The OS locale hooks go through `System::notifyLocalesMayHaveChanged`,
  which re-resolves only when the language list changed (an Android configuration change is also a
  rotation); the prebuilt Android activity now keeps running on `locale|layoutDirection` changes.
  The web hook is verified by the `localization` web gate; Android (`--locale-change` in
  `tools/android-emulator-test.sh`), iOS, macOS and Windows are built but not run. Extract to Table
  finds the element by its text in the `.html` (the whole text of one element), not through the
  compiled source map. The pseudo goldens render a keyed settings panel
  (`fixtures/golden/l10n_pseudo.rml`) rather than `examples/settings`, whose labels carry no keys.
  `LocalizationBench.Switch` measures the re-resolve and 300 keyed lookups without RmlUi documents.
  AC12's check is `tools/l10n/check_capture_symbols.py` over the release and editor engines (the
  capture code defines `jasperengine_l10n_capture`), not a gtest.
- **D-11a macOS and iOS hooks (Mac pass).** Both ran on an arm64 Mac with `tools/test-projects/localization`
  (macOS: `cmake -S engine -G Ninja -DPROJECT_ROOT=...`; iOS: the same project for the simulator with
  `-G Xcode -DCMAKE_SYSTEM_NAME=iOS`, on a simulator device of its own). The game starts in `en`; the
  app's own `AppleLanguages` is set to `de` (`defaults write` of its domain, inside the simulator for
  iOS, so the user's languages stay), which alone logs nothing; then the notification System
  Settings posts, `AppleLanguagePreferencesChangedNotification` (distributed on macOS, Darwin through
  `notifyutil -p` in the simulator), and both log `[Localization] language de` and the game's
  `onLanguageChanged` line (`Spielen`). Windows is still not run.
- **D-12 Guide and tool text (R36, R37).** The agent guide's "Other areas" index names the area alone
  (`localization`, read as `jasper://docs/localization`) without its tools in parentheses: with
  them the guide left 1,458 of the 1,500 bytes of headroom `tool_budget.py` requires. The tools are
  named in the `localization` group's description and in `docs/mcp/guide/localization.md`.
  `configure_game_view`'s `language` description gained the example `en-XA` but not the word
  "pseudo": the tool would pass its 1,500-character limit; `docs/mcp/guide/ui.md` explains the two
  pseudo-locales.
- **D-13 The diagnostic's code (R11).** The child-elements warning is `HTMLUI-LOC-0001`, not
  `HTMLUI-L10N-0001`: an area is capital letters only, as the code checks and tools read a code
  (`HTMLUI-[A-Z]+-NNNN`).
- **D-14 assert_ui's description (R35).** "languages" left `assert_ui`'s description ("viewports:
  more layouts."); the `languages` parameter stays. `guard_l10n_assert_languages`, sharpened with
  `configure_game_view language` and `inspect_ui l10n` in reach, passed 4/4 without the word.
- **D-15 One design with S95's String Tables (#11, `feature/s95-lua-runtime-gaps` WP-6).** S95 built
  the same feature independently; the user chose one merged design, landing here, which S95 then
  adopts. Kept from this spec: the files (a source `.stringtable` plus one `.translation` per
  language with S87's `status` and `source_hash`, GUID in the file), ICU messages and every CLDR
  language, namespaces and lazy per-namespace loading, `data-l10n-id` / `-args` / `-attr-*`,
  pseudo-locales, capture, language fonts, OS locale hooks, the Localization Tables window,
  `edit_string_table`'s schema and the zero-cost rule. Taken from S95:
  - positional arguments `{0}`, `{1}` from a Lua list (R5);
  - `Localization.language` writable (R17), as Unity's `SelectedLocale`;
  - keys without a namespace through a **Default Table** (R1, R30), in place of S95's bare-key search
    over every table, which would load every table and make a key's meaning depend on the others;
  - the stored names `localizationKey` (Text) and `localization.remember_language` / **Remember
    Language Choice** (R16, R30, R40), which match their Inspector labels, and one Lua method
    `setLocalized(key, args?)` on Text and on HTML elements (R11, R16);
  - table edits apply at once, and keyed text shows in Edit mode (R13);
  - the `check` tool `strings` (R21a).
  Dropped from S95, with the reason:
  - its one-file format with every language's values inline: per-language files give review status,
    stale detection, small diffs and lazy loading;
  - plural values stored as `{form: text}` objects: ICU plurals cover them, Insert Plural writes the
    skeleton, and CSV/XLIFF already exchanged plurals as ICU;
  - `data-loc`: one HTML route (`data-l10n-id`), which also carries arguments and attributes;
  - the `"Table:key"` address: `<namespace>.<key>` already names the table;
  - a cross-table duplicate-key check: namespaces make keys unique by construction (a namespace
    collision is still an error);
  - its GUID in a `.meta` file: the GUID lives in the file, as Title Art's and dialogues' do.
  Neither design had landed, so the renames need no migration.
