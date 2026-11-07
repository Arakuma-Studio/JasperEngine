# The Project window

The Project window shows the project's folders and files, as Unity's does: the folder tree on the
left (in the split layouts), the listing of one folder on the right, a search over all of them.
This page is about what it does with files. Agents do the same with MCP: `move_asset`,
`delete_asset`, `create_folder`, `create_script` (`jasper://docs/assets`).

## Copy, Cut, Paste and Duplicate

- **Copy** (Ctrl+C) and **Cut** (Ctrl+X) put the selected files and folders on the Project
  window's clipboard; cut items are dimmed until they are pasted.
- **Paste** (Ctrl+V) goes where you aim, as in Explorer and Unity:
  - right-click a **folder** > Paste: into that folder;
  - right-click a **file** > Paste: into the file's folder, beside it;
  - right-click empty space > Paste, Ctrl+V, Edit > Paste: into the folder the listing shows (during
    a search: the folder the search started from).
- **Duplicate** (Ctrl+D, the right-click menu, Assets > Duplicate, Edit > Duplicate with the Project
  window focused) copies each selected item beside itself.
- A name that is taken is never overwritten: the copy, or the moved file, gets Unity's free name,
  `hero 1.png`, `hero 2.png` (a folder: `Sprites 1`). A Vertex Animation's `.vat` and `.vat.glb`
  take one free name together.
- What was pasted or duplicated is selected, and the Inspector shows the last of it.
- A folder is never pasted, dropped or duplicated into itself or below itself: the editor says so
  and nothing changes. A refused or failed paste keeps a cut on the clipboard, so you can pick
  another folder; a successful one empties it. Items moved or deleted since the copy are skipped
  (the Console says how many); a cut pasted into the folder it is in changes nothing.
- Every paste, duplicate, drag and drop is one undo step ("Duplicate 3 Files", "Move File hero.png
  as hero 1.png"). Copies get new GUIDs; moves keep theirs, so references follow.
- Holding Ctrl+D or Ctrl+V makes one copy per press. While you type in the search field or a name
  field, the keys stay with the text.

Dragging: a file you press on is the one you drag (with the rest of the selection when it is
selected). An empty-space press starts the selection rectangle; with Ctrl or Shift held it adds to
the selection.

## Create

The toolbar's **+** button, Assets > Create and the **Create** submenu at the top of every
right-click menu (empty space, a file, a folder, a folder of the tree) list the same items, in the
same order:

Folder; Lua Script, C++ Script, C++ Source File, C++ Header File; HTML Document; Animation Clip,
Animator Controller, Avatar Mask, Animation Montage, Pose Animation; Audio Mixer; Physics Material,
Physics Material 2D; Shake Preset; Sprite Atlas. Assets > Create also has 2D (Sprite Atlas, Sprite Atlas from
Selection), Scene and Native Plugin... A selection of textures or Aseprite files also has **Create Sprite Atlas from
Selection** in its context menu ([sprites](../sprites.md)).

- From a folder's menu, the item is made **inside that folder**; from a file's menu, beside the
  file; elsewhere in the folder the listing shows. The editor then shows and selects it.
- Folder, the scripts, the C++ files and HTML Document ask for their name first (the name is the
  class, the module or the document); Esc creates nothing. The other assets are made at once and
  renamed in place.
- A Lua script is created only inside the Lua directory (Project Settings > Scripting > Lua
  Directory); elsewhere the item is disabled and its tooltip says where that is. A C++ file outside
  the Script Directories is compiled only when it is attached as a script; the name dialog says so.
- Every create is one undo step (Ctrl+Z removes the file; an open, unchanged Code Editor tab of it
  closes). The templates are the ones the Inspector's script dialog and `create_script` write.

## The assets and Lua directories

The project's assets directory and Lua directory (and a folder holding one) can't be cut, dragged,
renamed or deleted from the Project window: the editor says to change them in Project Settings >
Editor > Assets Directory or Project Settings > Scripting > Lua Directory. That change moves the
referenced files into the new directory, keeping their layout and their `.meta`, `.humanoid` and
Vertex Animation companions, rewrites every scene, prefab and material that refers to them, and
removes the folders the moves emptied. OK first shows what would move; when something would go
wrong (a different file already at the destination, a file that can't be written, a link leading
out of the new directory) it says what and changes nothing. It can't be undone; changing the
directory back moves the files back. Agents: `set_project_settings` with `dry_run` first.

## Shortcuts

| Command | Keys |
|---|---|
| Copy, Cut, Paste | Ctrl+C, Ctrl+X, Ctrl+V |
| Duplicate | Ctrl+D |
| Select All | Ctrl+A |
| Delete | Delete, Backspace |
| Rename | F2 |
| Resize items | Ctrl+mouse wheel |

All of them are rebindable in Edit > Shortcuts... ([keyboard-shortcuts.md](keyboard-shortcuts.md)).
