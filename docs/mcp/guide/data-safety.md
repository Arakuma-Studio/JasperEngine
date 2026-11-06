# Data safety

Served as `jasper://docs/data-safety`. What the editor does so a save, a crash or a file it
cannot fully read never costs work, and what that means for the tools. The user-facing
description is `docs/data-safety.md`.

## Saves

- Every save is atomic: a crash or a full disk leaves the old file or the new one, never half a
  file. A read-only file (a Git LFS or Perforce lock) is not written over: the save fails and
  says so.
- Unsaved scenes are lost when the editor closes. Save after each working step (`save_scene`,
  `save_project`); Auto Save only makes crash backups and never writes the scene file.

## Scene files and identity

- A copied folder's scenes are new scenes, each with its own id and name; references between the
  copies still point at the originals.
- A scene file that claims another scene's id when it loads gets a fresh id written into it.

## Files the editor cannot fully read

A scene, prefab, material or `project.yaml` written by a newer editor, a plugin or a hand edit
can hold things this editor does not know. The load goes on and records each in a load report.
An issue has `kind` (`unknown_component`, `unknown_field`, `unknown_enum_value`,
`invalid_value`, `unknown_entity_type`, `unknown_root_key`, `newer_format`, `migrated`),
`fate`, `file`/`line`/`column`, the entity path, component and field, and a did-you-mean
`suggestion` when a known name is close.

- **preserved**: kept aside and written back unchanged on save (unknown components, unknown
  fields of known components, unknown enum names, unknown top-level keys). Setting the field
  yourself replaces the kept value.
- **dropped**: would be lost by the next save (unknown GameObject types, unknown keys nested
  inside a known value such as `submeshes[0]`, a file whose format version is newer than this
  editor's).

`open_scene` returns `load_report` {skipped, preserved, dropped, newer_format}; scene results
carry `degraded` {preserved, dropped}. `get_load_report scene=...` lists every issue, with
`outstanding` false for one already fixed in the editor.

**A save that would drop data fails** with code `degraded_document` and the dropped issues:
`save_scene`, `save_project`, the material tools (`create_material`, `update_material`,
`apply_material_preset`) and the prefab tools that write the asset (`add_to_prefab`,
`remove_from_prefab`, `prefab_overrides`, `prefab_mode`). Then:

1. Read the issues (`get_load_report`). A typo with a `suggestion` is usually fixed by editing
   the file and reopening the scene.
2. Save somewhere else (`save_scene path=<new file>`) to keep the original intact, or
3. pass `allow_data_loss=true` only when losing those values is intended.

`save_scene all=true` saves every clean modified scene (preserved values included), lists the
refused ones and fails only if one was refused. Play and export stop on a degraded scene that
has unsaved changes. The file is never touched by a refused save.

## Auto Save and crash recovery

Auto Save copies each modified scene to `.jasperengine/backup/` (every minute by default; not
while playing; not for scenes with dropped data). Saving or closing the scene deletes its backup,
and so does a normal quit. After a crash, `open_scene` reports `recovery`
{available, status, autosaved_at, applied} and takes `recovery`:

- `keep` (default): the saved file opens; the backup stays and is offered again.
- `restore`: the scene takes the backup's content and stays modified; save to keep it.
- `discard`: deletes the backup; the saved file opens.

A backup only counts for the exact file it was made from: `status` `stale` means the file
changed since (a `git pull`, another editor) and the backup is never applied. `list_scene_backups`
lists them all; `discard_scene_backup scene=...` deletes one.

## One editor per project

A project open in one editor is locked (`.jasperengine/user/editor.lock`). `jasperengine-editor
mcp --project <dir>` on a locked project exits with code 3 and `project_locked` on stderr naming
the other editor's PID. Two editors on one project overwrite each other's saves: close the other
one, or pass `--ignore-lock` only when it is known to be idle.
