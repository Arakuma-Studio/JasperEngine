# Localization (agents)

String Tables (`.stringtable`, keys `ns.key`) hold every player-facing string; translations sit
beside them. Tools: `edit_string_table` (group `localization`), `inspect_ui {l10n}`,
`assert_ui {languages}`, `configure_game_view {language}`. Reference: `docs/localization.md`.

## Localise a menu

1. `tool_groups {enable: ["localization"]}`.
2. `edit_string_table {path: "assets/l10n/ui.stringtable", create: {}, ops: [{op: "set", key: "menu.play", text: "Play"}, {op: "add_language", language: "de"}, {op: "set", key: "menu.play", language: "de", text: "Spielen"}]}`: one undo step; one bad op refuses the call.
3. In the HTML: `<button data-l10n-id="ui.menu.play">Play</button>`; arguments with `data-l10n-args='{"count": 3}'`, attributes with `data-l10n-attr-placeholder`. Never a Lua table of strings per language.
4. Texts are ICU: `{count, plural, one {# coin} other {# coins}}`; `plural` and `select` need `other`.
5. Check in Play: `eval_lua Localization.setLanguage('de')`, then `inspect_ui`.

## Find literal text

`inspect_ui {entity, l10n: true}` lists keyed elements, `literal` text no key produced and `missing`
keys with their element ref. `edit_string_table` with no arguments shows coverage per language and
this Play's misses; its `missing_keys` lists keys the HTML names that the table lacks.

## Check German fits

`assert_ui {entity, checks: [{type: "text_fits", selector: "button"}], languages: ["de", "en-XA"]}`
runs every check per language (no Game view switch, no screenshot needed); failures name the
language. `en-XA` is 40 % longer than English: a layout that fits it fits most languages.
