# Text and file names in any language

Projects, folders, assets, scenes and objects can be named in any language, and text fields take
any character, emoji included. Design record: `docs/superpowers/specs/2026-11-04-unicode-text-design.md`.

## Windows

The editor, the CLI (`jasperengine-editor`, `jasperengine-editor-cmd`), games and the test
binaries run in Windows' UTF-8 code page (their manifest, `engine/platform/win/jasperengine.manifest`),
so a folder named `关卡` or `Jogos` works as it does on Linux and macOS.

- The editor and the CLI need **Windows 10 version 1903** or later (Windows 11, Windows Server
  2022). Older Windows ignores the setting; the editor then stops at start with a message (on
  the console, or in a message box when started from Explorer) instead of corrupting projects
  with names outside the system's code page.
- Games run on older Windows too: the engine converts its own paths explicitly.
- The CLI's console shows UTF-8 while it runs; the shell gets its code page back at exit.
- Output of the programs the editor runs (CMake, the compilers, Python, vswhere) reaches the
  Build window and `editor.log` as UTF-8: a line that is not UTF-8 is read in the system's
  console code page (what `cl` and `link` write). Python programs are started with
  `PYTHONUTF8=1`.

## Paths in project files

Every path a project file stores (`project.yaml`, scenes, prefabs, materials) uses `/`, whatever
the platform that saved it, and a `\` in a stored path is read as a separator (Unity's rule): a
project made on Windows opens on Linux and macOS. A file whose own name holds `\` (possible on
Linux and macOS, not on Windows) cannot be referenced from a project file.

## Fonts in the editor

The editor's fonts (Roboto, JetBrains Mono) fall back to the system's Chinese, Japanese and
Korean fonts, found in the background when the editor starts, so CJK names show as glyphs in
the Hierarchy, the Inspector, the Project window, the code editor and the Console:

| System | Fonts looked for |
|---|---|
| Windows | Yu Gothic, Meiryo, MS Gothic (Japanese); Malgun Gothic, Gulim (Korean); Microsoft YaHei, SimSun (Simplified Chinese); Microsoft JhengHei, MingLiU (Traditional Chinese) |
| macOS | Hiragino Sans (Japanese); Apple SD Gothic Neo (Korean); PingFang SC, Hiragino Sans GB, STHeiti (Simplified); PingFang TC, Heiti TC (Traditional) |
| Linux | what fontconfig (`fc-match`) gives for the language, else Noto Sans CJK, WenQuanYi or Droid Sans Fallback where distributions install them |

Han characters are drawn differently in each language. **Preferences > General > Editor Font >
CJK Font Language** picks whose forms come first: Auto (the system's language), Japanese,
Korean, Simplified Chinese or Traditional Chinese. It applies at the next start.

Emoji typed or pasted into the editor's fields are kept. They draw in one colour where the system
has an outline emoji font (Segoe UI Emoji on Windows; on Linux an outline emoji font fontconfig
finds), and as a box elsewhere: the editor's font rasteriser draws no colour glyphs.

Agents read what was found in `editor_status` (`editor_fonts`: `state`, `effective_language`,
`fallbacks`) and set the preference with the settings tools (`preferences.cjk_font_language`).

## In games

A game draws text with its own fonts: the default font holds Latin and Arabic. A game that shows
Chinese, Japanese, Korean or emoji gives its Text components (or its HTML UI) a font with those
glyphs. Input Fields keep any character typed into them, emoji included, on every platform
(`jasper://docs/ui`).
