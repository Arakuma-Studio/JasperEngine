# Lucide icons (built-in pack)

`lucide.pack` holds every icon of a [Lucide](https://lucide.dev) release (`VERSION`), compressed,
for the HTML UI's `data-lucide` icons (phase-2 spec §3.11.8). The engine embeds it at build time
(`embed_lucide.cmake` → `htmlui_lucide.h`, wired by `lucide.cmake`; the editor's Apple export
writes the same header into `engine/shaders/`, where an empty stand-in sits for builds without
the step) and reads it with `htmlui::compat::LucideIcons`.

License: `LICENSE` (ISC, plus MIT for the icons derived from Feather), recorded in `NOTICE.md`.

## Updating

```
git clone --depth 1 --branch <tag> https://github.com/lucide-icons/lucide /tmp/lucide-<tag>
python3 tools/htmlui/build_lucide_pack.py /tmp/lucide-<tag> engine/libs/lucide/lucide.pack
cp /tmp/lucide-<tag>/LICENSE engine/libs/lucide/LICENSE
```

then update `VERSION` (tag, commit, icon and alias counts: `compat_lucide_test.cpp` checks
them) and the `NOTICE.md` row. The builder is deterministic (same checkout, same bytes).

## Format

Little-endian. `"LUCP"`, `u32` format version (1), `u32` inflated payload size, then a zlib
stream (RFC 1950) of the payload:

| Field | |
|---|---|
| `u32` icon count, `u32` alias count | |
| `u16` length + bytes | the Lucide release tag |
| icon count × `{u16 name length, name, u32 offset, u32 length}` | sorted by name (bytes); names are the kebab-case file names |
| alias count × `{u16 name length, name, u32 icon index}` | sorted by name |
| `u32` size + bytes | every icon's inner shapes, concatenated |

An icon's shapes are its SVG children as compact markup (`<path d="..."/>`), attributes in
source order, without Lucide's common root attributes (`width`/`height` 24, `viewBox 0 0 24 24`,
`fill none`, `stroke currentColor`, `stroke-width 2`, round caps and joins), which every icon
has (the builder checks it). Names resolve as `lucide.createIcons()` resolves them: the
PascalCase form of the name against the icons and the aliases.
