# Game UI localization

String Tables, ICU messages, keyed HTML UI and Text, one game language, missing-text capture and
pseudo-locales (docs/specs/game-ui-localization.md). Agents: `jasper://docs/localization`.

## String Tables

A **String Table** is `Name.stringtable` in the assets folder: a namespace (from the file name, kept
on rename), a source language (the project default when made) and ordered entries `{key, text,
context, max_chars}`. Each translation is `Name.<lang>.translation` beside it, with per key `{text,
status, source_hash}`; `status` is `translated` or `needs_review`. A key is global as
`<namespace>.<key>` (`ui.menu.play`). A namespace is unique in the project and never a Title Art
document's `string_namespace` (error `l10n-namespace-taken`). A key whose first part names no
namespace reads the **Default Table** (Project Settings > Localization; empty: the project's only
String Table), so a small game can write `Localization.get("menu.play")`.

Make one with Project window **Create > Localization > String Table** or
`edit_string_table {path, create: {}}`. Editing a source text turns its translations
`needs_review`: they are still shown and the export warns (`W-L10N-STALE`). Moving or renaming a
table moves its translations with it.

Both files are JSON with `format` and `version: 1`, written deterministically (two spaces, LF,
translations sorted by key). Members this editor does not know are kept.

## Messages

Texts are ICU MessageFormat, the subset games need:

| Syntax | Meaning |
|---|---|
| `{name}` | an argument (a number argument is formatted with the language's symbols) |
| `{n, number}`, `{n, number, integer}`, `{n, number, percent}` | numbers: `1.234,5` in German |
| `{n, plural, =0 {none} one {# coin} other {# coins}}` | CLDR plural categories; `#` is the number; `offset:1` |
| `{g, select, female {...} other {...}}` | a choice by a string argument |
| `''`, `'{literal}'` | apostrophe quoting |

`plural` and `select` need `other` and nest up to 4 levels. Plural rules and number symbols come
from CLDR 49 (`engine/core/localization/PluralRules.gen.inc`, `tools/l10n/gen_plural_rules.py`). A
pattern that does not parse is an error in the editor and shows as written in a game. A string
argument `"@ns.key"` is replaced by that key's text (a translated noun). From Lua, a list fills
`{0}`, `{1}`, ... (`Localization.get("ui.score", {12, "Ana"})` for `"{1} scored {0}"`).

## The game language

Resolved at start and on every change: the first of `Localization.setLanguage`'s value (the same
global as `TitleArt.setLanguage`), the remembered choice (Project Settings > Localization >
**Remember Language Choice**), the device's languages (the Game view's Device > Language in the
editor), the default language, that matches a shipped language (`zh-HK` picks `zh-Hant`, `pt-PT`
picks `pt-BR` when only that ships). The shipped languages are Project Settings > Localization >
Languages, or every table's languages when that list is empty. The OS language changing while the
game runs re-resolves (Android, iOS, macOS, Windows, the web) unless the game set a language.

Per key the text comes from the language, its shorter tags that ship (`pt-BR` -> `pt`), the
default language, then the source text. A key no table has: HTML keeps its authored text, a Text
its `text`, `Localization.get` its `fallback`, else `""` in a release build and `⟦key⟧` in the
editor and Development Builds (one warning per key and language).

## HTML UI

```html
<button data-l10n-id="ui.menu.play">Play</button>
<p data-l10n-id="ui.shop.coins" data-l10n-args='{"count": 3}'>3 coins</p>
<input data-l10n-attr-placeholder="ui.search.hint" placeholder="Search">
<span translate="no">JasperQuest</span>
```

The message replaces the element's text (as text, never markup), so a keyed element holds only text;
one with child elements is left as written (`HTMLUI-LOC-0001`). Rows of list, grid, tree and table
views and markup scripts insert are translated as they are made; a language change translates the
same elements again in place. The document's root gets the language and `rtl` for right-to-left
languages. Lua: `element:setLocalized(key, args)`, `doc:translate()` after editing `data-l10n-*`
directly. Data models get the transform `{{ 'shop.coins' | l10n('count', coins) }}`.

## Text

The Text component's **Localization Key** shows that key's message in Play and games, and in the
Scene and Game views in Edit mode; `text` stays the fallback and what the scene saves. A table edit
shows at once. Lua: `text.localizationKey`, `text:setLocalized(key,
{count = 3})` (arguments are not saved).

## Fonts

Project Settings > Localization > **Language Fonts** gives fonts per language: HTML text gets them
before the other fallback faces while that language is the game language (Japanese and Chinese
pages get their own Han glyphs), and a Text whose Font is empty uses the first. The fonts apply to
documents loaded after the language is set.

## Lua

```lua
local label = Localization.get("ui.shop.coins", {count = 3})
local custom = Localization.format("{n, plural, one {# life} other {# lives}}", {n = lives})
if not Localization.has("ui.menu.extra") then return end
Localization.setLanguage("de")             -- remembered with Remember Language Choice; "" = the device
Localization.language = "de"                -- the same; a bad tag raises
local score = Localization.get("score", {12, "Ana"})   -- the Default Table's "{1} scored {0}"
print(Localization.language, #Localization.languages, Localization.getLanguageName("de"))
Localization.onLanguageChanged = function(code) print("now " .. code) end
```

C++: `jasperengine::Localization` (`localization/Localization.h`) with the same calls.

## Pseudo-locales

`en-XA` shows the default language's text accented, 40 % longer and in `⟦ ⟧`; `ar-XB` shows it
right to left. Both exist in the editor and Development Builds only: pick them in the Game view's
Device > Language, `configure_game_view {language: "en-XA"}`, or `Localization.setLanguage`.
Text that stays plain under them is not localised.

## Capture

In the editor and Development Builds every lookup that fell back is recorded: `untranslated`,
`unknown_key`, `stale`, with the first element or GameObject. `Localization.scan()` also lists text
on screen that no key produced. Agents: `inspect_ui {l10n: true}`, `edit_string_table` (Play misses).

## Translators

`edit_string_table {path, export: {format: "csv" | "xliff", out}}` and `import: {file}` use the
same CSV and XLIFF as Title Art (Unity Localization headers, XLIFF 1.2 out, 1.2 and 2.0 in); a row
whose source changed imports as `needs_review`, a title's row is reported.

## Export

An export writes `assets/l10n/<ns>.<lang>.strbin` per namespace and shipped language and
`l10n/manifest.json`; a project without tables writes nothing and pays nothing. Warnings (none
blocks): `W-L10N-UNKNOWN-KEY`, `W-L10N-UNTRANSLATED`, `W-L10N-STALE`, `W-L10N-LANGUAGE-DROPPED`,
`W-L10N-BAD-MESSAGE`.

`jasperengine-editor check` (with the Lua or UI checks, and in the pre-commit hook when a table,
translation, script, document or scene changes) runs the tool **strings**: errors for a table that
does not read, a key twice in a table, a namespace two owners claim, a message that does not parse
and a Default Table that names no table; warnings for keys HTML, scenes and Lua literals
(`Localization.get/has`, `setLocalized`, `localizationKey =`) name that no table has.

## Multiplayer

A server never formats player-facing text: it sends the key and the arguments, and each client
formats them in its own language.

```lua
-- the message the server sends: a key and its arguments, never the formatted text
local message = {key = "ui.match.won", args = {player = "Ana"}}
-- each client, on receiving it
local shown = Localization.get(message.key, message.args)
```
