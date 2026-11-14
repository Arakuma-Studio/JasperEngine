# Localization: String Tables

Unity's Localization package for a game's texts. Titles keep their translations inside their
`.titleart` (jasper://docs/title-art); every other text lives in String Tables. Enable the tools
with `tool_groups {enable: ["localization"]}`.

## The languages

Project Settings > Localization: `localization.languages` (the languages the game ships, in
order), `localization.default_language` (shown when the device's languages match none; "" is the
first language) and `localization.remember_language` (default true: a language the game chooses
is stored in UserSettings and applied at its next start). Set them with `set_project_settings`.
`edit_string_table set` refuses a language outside `localization.languages` when the list is not
empty; add it there first.

There is one game language. `Localization.language` and `TitleArt.setLanguage` set the same value,
so titles and strings switch together, and `Localization.onLanguageChanged` is
`TitleArt.onLanguageChanged`. At start it is the device's language (in Play the Game view's
Device > Language, `configure_game_view {language}`), matched against the project's languages
(`es-MX` picks `es`), else the default language.

## The file

`assets/.../<Name>.stringtable`, JSON version 1, its GUID in its `.meta`:

```json
{
  "version": 1,
  "entries": [
    {"key": "menu.play", "comment": "Main menu button", "values": {"en": "Play", "es": "Jugar"}},
    {"key": "hud.coins", "values": {"en": {"one": "{count} coin", "other": "{count} coins"},
                                    "es": {"one": "{count} moneda", "other": "{count} monedas"}}}
  ]
}
```

- A key is not empty, has no `:` and no surrounding spaces; it is unique in its table.
- A value is a string, or plural forms: `zero`, `one`, `two`, `few`, `many`, `other`. Which forms
  a language uses are CLDR's cardinal rules (`en`: one, other; `ru`: one, few, many, other; `ar`:
  all six; `ja`: other). A plural value lacking a form its language picks for counts up to 1000
  or a fraction counts as missing (Spanish, French and Italian `many`, for whole millions, is
  optional: `other` stands in).
- `{0}`, `{1}` are positional arguments, `{name}` named ones, `{{` and `}}` braces.
- The table's name (for `"Table:key"`) is its file name without the extension.

## edit_string_table

| Action | What |
|---|---|
| `create` | makes `path` (refused when it exists), with optional `entries` |
| `get` | the entries (only `keys` when given), `missing` per language, `duplicates` |
| `set` | `entries: {key: {language: text, plural forms or null, comment: text}}`: new keys are added, `null` removes a language's value; one undo step; nothing is written when any entry is refused |
| `remove` | `keys` go (with `language`: only that language's values); one undo step |
| `import` | `file`: a CSV or XLIFF (1.2 or 2.0) a translator returned; new keys are added, non-empty cells that differ are set; one undo step |
| `export` | `format: csv` (one file, a column per language) or `xliff` (one `<name>.<language>.xlf` per target language); without `file` the text comes back |

Every result has `missing` (the keys each language lacks) and, when another table defines one of
this table's keys, `duplicates`: `Localization.get("key")` takes the first table in path order, so
rename the key or look it up as `"Table:key"`. `jasperengine-editor check` reports duplicates as
errors. In CSV and XLIFF a plural value is an ICU plural, `{count, plural, one {{count} coin}
other {{count} coins}}`, which translation tools read; `#` in a form imports as `{count}`.

## At runtime

- Lua: `Localization.get(key [, args])`, `Localization.has(key)`, `Localization.language` (rw),
  `Localization.languages`, `Localization.onLanguageChanged` (jasper://docs/lua-api 2.4.4).
- Lookup: the current language, its base language (`pt-BR` -> `pt`), the default language, then
  the key itself (warned once per key in the Console).
- A plural value picks its form by the `count` argument, else the first positional one.
- A Text's `localizationKey` (Inspector: Localization Key; `set_properties {text:
  {localizationKey: "menu.play"}}`) shows the key's value and follows the language, in the editor
  too.
- HTML UI: `<span data-loc="hud.coins" data-loc-args='{"count": 3}'></span>` gets its text when the
  document loads (before `onLoad`) and at every language change; `el:setLocalized(key [, args])`
  sets both attributes from Lua. A value is text, never markup.
- Exports ship every `.stringtable` with the project's languages only; nothing is read until the
  first lookup.

## Checking a translation

1. `edit_string_table get` and read `missing`.
2. Play with `configure_game_view {language: "es-ES"}` and read the texts (`eval_lua "return
   Localization.get('menu.play')"`, a screenshot), then set `language` back to "".
