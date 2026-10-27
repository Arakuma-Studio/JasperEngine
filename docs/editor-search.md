# Search, shortcuts and organisation in the editor

How to find things and keep a project organised (S25, `docs/specs/S25-editor-search-organisation.md`).
Agents reach the same features through the `editor_ux` MCP tools
(`docs/mcp/guide/editor-integration.md`).

## Search All (Ctrl+K)

**Edit > Search All...** (Ctrl+K, Cmd+K on macOS) opens one search over everything the editor
has: menu commands (with their shortcuts), windows, every setting of Preferences and Project
Settings, the GameObjects of the open scenes, the components Add Component offers, and the
project's assets. Type part of a name; typos and abbreviations are forgiven ("brnach",
"gmo" for GameObject). With nothing typed it lists what you ran recently, then the commands and
windows.

A prefix searches one kind only:

| Prefix | Kind |
|---|---|
| `>` | commands |
| `/` | windows |
| `:` | settings |
| `@` | GameObjects |
| `+` | components (added to the selected GameObjects) |
| `asset:` | assets, with the Project window's search grammar below |

The chips under the field do the same, and Tab moves between them. Up and Down pick a result,
Enter runs it (a command runs as its menu item, a window comes to the front, a setting opens its
page at the row and flashes it, a GameObject is selected, an asset is pinged in the Project
window), Ctrl+Enter frames the GameObject or opens the asset, Esc closes.

## Shortcuts

Every key binding of the editor is in one catalog: the menus, the toolbar tooltips and
**Help > Keyboard Shortcuts** show what it says, and `docs/editor/keyboard-shortcuts.md` lists
the defaults. **Edit > Shortcuts...** rebinds them: pick a command, click its key and press the
new chord. A chord another command already uses in the same place asks first (Reassign takes it
from that command, Cancel changes nothing); a key that only applies in one window (the Scene
View's Home, the Project window's F2) may share a chord with a general command, which the window
then shadows. Reset puts one command or all of them back.

Your bindings are kept in `shortcuts.yaml` in the editor's settings folder and apply to every
project. A binding the file has that this editor does not know is kept and ignored; two commands
the file gives the same key resolve to the default holder, with one warning in the Console.

Where a key works: *Global* keys (Save, Search All, Play) work everywhere, the Code Editor and
text fields included; *Editor* keys work in every window but the Code Editor and never while you
type in a field; a window's own keys only while it has the focus (the Scene View also while the
mouse is over it). On macOS, Ctrl is Cmd and Alt is Option. Letters follow your keyboard layout.

## Window layouts

**Window > Layouts** lists the layout presets: the built-in Default and the ones you saved with
**Save Layout...** (the docking, the window sizes and which windows are open). Presets are yours,
for every project; **Save Layout to File...** and **Load Layout from File...** share them. Picking
another layout while the current one has unsaved changes asks whether to save it first. **Reset
Layout** is Default. Each project also remembers its own working layout
(`.jasperengine/user/layout.yaml`), so it reopens as you left it.

## Searching the Project window

The Project window's search box takes Unity's search syntax:

| Term | Finds |
|---|---|
| `hero` | names with the word (several words: all of them; typos forgiven) |
| `"boss room"` | names containing exactly that text |
| `t:texture`, `t:Texture2D`, `t:Prefab`, `t:AudioClip`, `t:Folder`... | assets of a type |
| `l:Hero` | assets with that label |
| `ext:png` | an extension |
| `path:levels/` | a part of the path |
| `re:^tile_\d+$` | a regular expression on the name |
| `ref:textures/hero.png` | assets that reference it |
| `missing:` | assets with missing references |

The same key twice means either (`t:texture t:model`), different keys mean both
(`t:texture l:Hero`), and `OR` between terms joins whole searches. The buttons left of the
field build these for you (**Search by Type**, **Search by Label**); **Save Search** keeps the
current one.

## Labels

Select assets and use the label button at the bottom of the Project window to add or remove
labels (up to 32 per asset). Labels are stored with the asset (its `.meta` file, or a `labels:`
line in scenes, prefabs and materials), so they travel with it through version control. Their
colours, and renaming or deleting a label everywhere, are in **Project Settings > Editor > Asset
Labels**. Every label change is one undo step.

## Saved searches and Favorites

**Favorites** in the Project window's tree lists the built-in searches (All Materials, All
Models, All Prefabs, All Scenes, All Scripts) and the ones you saved. A saved search is a query
plus an optional folder; edit it as text or as rows (Type, Name contains, Label, Extension, Path,
Regex, with All or Any). Your own are kept per project in `.jasperengine/user/organisation.yaml`;
shared ones (Share with Project) in `project.yaml`.

## Bookmarks

**Window > Bookmarks** keeps assets, folders, scenes and GameObjects you go back to. Drag them in
(or use Add to Bookmarks in the Project window and the Hierarchy), drag to reorder, click to go
there. A bookmark follows its asset when it is moved or renamed; a GameObject bookmark finds the
object again by its id, then by its path in the scene, and shows it as missing otherwise.

## Searching settings

The search field of **Preferences** and **Project Settings** filters both the page list and the
rows of each page ("vsync" leaves Quality and General, with only their VSync rows), and
highlights what matched. Clearing it shows every row again.
