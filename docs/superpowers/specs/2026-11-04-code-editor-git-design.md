# Code editor and version control, scene default shader properties (design)

Date 2026-11-04. Branch `dc/code-editor-git`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#19).
Doriax references: `28531afd4` (change markers, footer), `0fc0ba677` and `22181844b` (highlighting, Lua indent;
already ported), `e3243c29b` (scene default shader uniforms), `17870f97a` (uniform tables). Version control design:
`docs/specs/S29-version-control.md` (its D5 decides the baseline).

## 1. Goals

1. The built-in code editor shows what changed against version control, as VS Code and Rider do: gutter markers
   (added, modified, deleted), the same marks on the vertical scrollbar (overview ruler), a diff popup on hover or
   click with the old lines, **Revert Change** per change (one undo step), **Next/Previous Change**, and a change
   summary in the editor's status row. The markers follow every edit within a frame. They work with Git and
   Subversion through Jasper's `VcsService`, not a second git monitor.
2. The status bar's version-control segment names a detached HEAD by its tag or short commit, and shows a merge,
   rebase, cherry-pick, revert or bisect in progress (with rebase progress) and the conflict count; a click goes
   to the Conflicts window while there are conflicts.
3. Highlighting of comments and strings that span lines, and Lua auto-indent, keep their regression tests: the five
   highlight tests deleted by the test prune `304e64e43` come back through a public seam.
4. A scene's default shader (Scene > Default Shaders: Mesh, Skybox, UI, Points, Lines) gets scene-level property
   values that every component using that default inherits. A component (or a mesh's material) overrides a value;
   **Revert to Scene Default** drops the override. Saved in the scene, generated into exported games, undoable,
   with a Lua API and MCP surfaces.
5. Shader property rows get their own resizable tables under a header naming the shader, so long member names no
   longer widen the Shader column, and post-process pass tables become resizable without freezing a too-narrow
   label column.

## 2. Non-goals

- Staging hunks (VS Code's "Stage Change"). Jasper's Commit window commits whole files and does not stage (S29 D5);
  a hunk-level stage would be the only staging UI in the editor.
- Inline blame, history per line, or side-by-side diff views in the code editor. History and the full diff stay in
  the History window and the text diff viewer (`VcsUi::openTextDiff`), which the new status-row menu opens.
- Change markers in an external script editor (VS Code, Rider): they have their own.
- Scene-level defaults for shader **textures** and **features/keywords**. Features change the compiled variant and
  the export's shader keys (feature `export-pipeline`); textures need their own binding lifetime. Values only
  touch uniform data. Revisit when a user asks.
- Scene-level shader global bindings: a value that changes every frame belongs in a Shader Global (one packed block
  for every program), not in a scene default that rewrites every inheriting component's block.
- A WATER default shader type: Doriax has one; in Jasper water is a material (`library:water`), so the mesh default
  covers it.
- Changing what component getters return (`mesh:getShaderUniform(name)` keeps returning the component's own stored
  value, Unity `Material.GetFloat` style). The effective value is reported by `get_shader_uniforms` and drawn by
  the Inspector.

## 3. Users and stories

- Human editing `Player.lua`: a green bar marks the five lines added since the last commit, an amber striped bar the
  changed line, a red triangle where two lines were removed. Hovering the triangle shows the removed lines,
  highlighted as Lua; the revert arrow puts them back; Ctrl+Z removes them again.
- Human reviewing before a commit: Alt+F5 jumps from change to change; the status row says "3 changes"; its menu
  opens the full diff or the file's history.
- Human after `git checkout v1.2`: the status bar reads "tag v1.2" instead of "(detached)". During
  `git rebase -i` it reads "feature | REBASE 2/5"; with a conflicted merge, "main | MERGING  2 conflicts" in red,
  and a click opens the Conflicts window.
- Human styling a toon world: Scene > Default Shaders > Mesh = `library:toon`; under it, "Toon Properties" sets
  Shade Steps = 3 and Rim Color for every mesh in the scene; one boss mesh overrides Rim Color in its Inspector, the
  row shows a revert arrow "Revert to Scene Default".
- Agent: `code_editor` status lists each open buffer's changes against HEAD; `code_editor` `revert_change` with
  `line` reverts one change in the buffer the human is looking at; `diff_file` with `source: "editor"` reads the
  unsaved buffer's diff.
- Agent: `set_shader_uniform {shader_type: "mesh", name: "rimColor", value: "#FF8800"}` sets the scene default;
  `get_shader_uniforms` on any inheriting GameObject lists `rimColor` with `source: "scene_default"`.
- Script: `self.scene:setDefaultShaderUniform(ShaderType.MESH, "windStrength", 0.4)` during a storm.

## 4. Findings

Code editor (verified by reading; nothing run):

- No change markers at all. `CustomTextEditor::Render` (`editor/window/widget/CustomTextEditor.cpp:4218-4357`)
  draws search highlights, selections, brackets, line numbers (`renderLineNumbers` :3664, numbers only), text and
  cursors; there is no diff layer, no overview and no popup. `editor/window/CodeEditor.cpp` has no reference to
  `vcs`, `getVcs` or a diff (grep). Doriax `28531afd4` adds them (`editor/util/LineDiff.*`,
  `CustomTextEditor.cpp:2824-2857,3698-4068` at Doriax HEAD) fed by a separate `GitMonitor` thread that diffs
  against the git index.
- The data is already there: `IVcsProvider::baseContent(path, rev)` (`editor/vcs/IVcsProvider.h:67-68`, git
  `cat-file blob` at `editor/vcs/GitProvider.cpp:445`, svn `cat -r` at `SvnProvider.cpp:369`), the poll snapshot with
  `headOid` and each change's `baseOid` and `oldPath` (`VcsTypes.h:28-48,96-103`), and the memo pattern of
  `SceneDiffTracker::baselineKeyOf` (`editor/vcs/SceneDiffTracker.cpp:110-118`) that refetches only when HEAD or the
  blob changes. S29 D5 (`docs/specs/S29-version-control.md:437`) fixes the baseline as HEAD (git) / BASE (svn).
- The widget's undo merges records closer than 300 ms (`CustomTextEditor.cpp:1315-1345`), so a revert clicked just
  after typing would merge into the typing step; Doriax's `revertDiffChange` has the same flaw.
- Doriax tokenizes a peeked removed line from a fresh state (`buildDiffPeekRows`, a new `OpenToken` per peek), so a
  removed line inside a block comment is highlighted as code.
- `GitProvider::textDiff` and `baseContent` compare raw blobs; the buffer drops `\r` (`setLinesFromText`
  :460-473). A CRLF working copy of an LF blob (Windows `core.autocrlf`) must not mark every line.

Status bar:

- `VcsUi::drawStatusBarSegment` (`editor/window/vcs/VcsUi.cpp:399-438`) prints `(detached)` for any detached HEAD
  (:409), and nothing about a merge, rebase, cherry-pick or revert in progress; conflicts only show as files in the
  change count. `GitProvider::parsePorcelainV2` (:255-273) reads `branch.oid`/`branch.head` only; porcelain v2
  does not report operations. Doriax shows the tag or short commit and a `!` mark (`App::showFooterGit`).

Highlighting and Lua indent (parity check, by reading):

- Jasper's tokenizer and indent code (`b714a0753`) are Doriax's `0fc0ba677`/`22181844b` plus HTML/CSS
  (`tokenizeText` :583, `multiLineTokenAt` :734, `scanOpenToken` :781, `opensBlock`/`closesBlock`/
  `reindentAfterTyping` :1580-1722; a `diff -w` of the two shows only the HTML/CSS additions and container-reuse
  changes). Parity holds; nothing to port.
- The five `CustomTextEditorHighlight.*` tests of `b714a0753` were deleted by `304e64e43` ("Prune redundant tests",
  `docs/test-cleanup/dispositions.jsonl:135-139`: they read private `lineTokens` through a friend). Only the four
  indent tests remain (`editor/tests/window/widget/CustomTextEditorTest.cpp`). The multi-line highlighting fix has
  no regression test today.

Scene default shader values:

- The scene's Default Shaders table (`editor/window/InspectorWindow.cpp:12936-12955`) only picks the shader
  (`drawSceneShaderRow` :6629-6667). `SceneSettings` holds five `ShaderRef` paths and no values
  (`engine/core/SceneSettings.h:422-426`, `Scene.h:347-366`). An unset member of a component inheriting the default
  reads, in `CustomUniformBlock::writeValues` (`engine/core/shader/ShaderUniforms.cpp:133-153`): its global binding,
  the component's (or material's) value, the mesh's `shaderUniforms` (as fallback of a material), the manifest
  `@property` default, zero. There is no scene level, so a look shared by a whole scene needs the same value on
  every object.
- Who inherits: a submesh whose material has no shader and whose mesh has no `customShader`
  (`RenderSystem::getSubmeshShader`, `RenderSystem.cpp:3137-3143`); a UI, Sky, Points, Lines, Trail or Line Renderer
  component with an empty `customShader` (`RenderSystem.cpp:10944,12199,11181,11841,11473`).
- Values are written into the blocks only when a component's `needUpdateShaderUniforms` is set
  (`writeCustomUniforms` :431-449, `applyComponentUniforms` :465-474); a scene-level change must flag the inheriting
  components.

Tables:

- Component uniform rows share `custom_shader_table` with the Shader row, and its label width grows with every
  member name (`InspectorWindow.cpp:6326-6333`). Post-process tables are not resizable, with a comment saying a saved
  width would freeze at what was known while the shader compiled (:6838-6840). Doriax `17870f97a` splits the tables
  and makes them resizable but keeps the freeze for members that appear later.

Outside this mandate (recorded for the lead, not changed here): the scene diff, `diff_scene` and `diff_file` default
`rev` to `"HEAD"` for Subversion too (`SceneDiffTracker.h:41,70`, `VcsTools.cpp:256,372,682,715`); for svn `HEAD` is
the server's newest revision (a network read, and someone else's commits show as differences), while D5 says BASE.
Inferred from reading; this spec adds the `VcsService::baselineRevision()` helper that a follow-up can use there.

## 5. Product decisions

1. **The baseline is HEAD (git) / BASE (svn), not the index.** S29 D5; the Commit window, Revert and the scene
   diff all use it, so Revert Change puts back exactly what Assets > Version Control > Revert would. Doriax diffs
   against the index; a user who stages from a terminal still sees all uncommitted changes (Rider behaves so).
2. **Fed by `VcsService`, no second monitor.** Doriax's `GitMonitor` duplicates S29's poller and only knows git.
   A small `TextBaselines` cache fetches each open file's baseline on the process channel, keyed like the scene
   diff (path, revision, HEAD, blob), so a fetch happens on open and after a commit or checkout, never per poll.
3. **Diffs run on the main thread, bounded.** Common-prefix/suffix trimming makes a typical edit O(lines); Myers is
   capped at 1,000 edits (past that the middle is one change); a diff that took `t` waits `20t` before the next
   (at most 5% of frame time, Doriax's rule). No thread hop, so markers move with the keystroke.
4. **Port Doriax's line diff and gutter, improve the edges.** Keep its LineDiff (VS Code's post-passes: shift,
   join, better boundaries) and drawing (3 px bars, 4 px triangles, hatched modified bars, ruler in the scrollbar's
   left third). Add: Jasper's VCS palette (amber modified, green added, red deleted, same as the Project window and
   Inspector), click-to-pin, previous/next/revert buttons in the popup, Alt+F5 / Shift+Alt+F5, context-menu
   Revert Change, a non-merging undo step, base lines highlighted with their own multi-line state, CR-insensitive
   comparison, renamed files diffed against their old path.
5. **No markers where there is nothing to revert to**: untracked and newly added files, ignored files, files outside
   the repository, binary or Git LFS pointer baselines, baselines over 4 MiB. The status row says which ("New
   file", "Untracked", "Ignored", "Not compared: binary"), so the absence is explained.
6. **Status bar words follow git's own prompt** (`__git_ps1`): `MERGING`, `REBASE 2/5`, `CHERRY-PICKING`,
   `REVERTING`, `BISECTING`; a detached HEAD shows its tag (tag icon) or 7-character commit (commit icon). Staged
   counts go in the tooltip, not the label (the editor does not stage).
7. **Scene default values sit between the object and the shader's own default**: global binding > material value
   (mesh) > component value > **scene default value** > manifest `@property` default > zero. Only objects that use
   the scene's default shader inherit; an object with its own shader never sees them.
8. **Values persist across shader changes** (as component `shaderUniforms` already do, and Unity material
   properties): switching the scene's default shader keeps the values; members the new shader lacks are listed as
   "Unused values (N)" with a Remove button.
9. **UI names**: the header under a shader is "<ShaderName> Properties" (Unity's word), member rows keep the GLSL
   name or the manifest label; the revert arrow's tooltip is "Revert to Scene Default" on an inheriting object and
   "Reset to the shader's default" elsewhere; the default row reports "Used by 12 objects", "Not used by any object"
   or "Shader failed to build".
10. **A table's saved column width lives as long as its rows.** The uniforms table ID includes a hash of its member
    names, so a resize is kept while the shader's members stay the same and a new member set starts at a width that
    fits. That removes the freeze the post-process comment warns about, so those tables become resizable too.
11. **Highlight tests come back through a public, observable seam**: `GetTokenTypeAt(line, column)` (also used by
    the diff popup's tests), not a friend struct.
12. **Per-frame scene values are discouraged, not forbidden**: Lua can set them every frame (each set flags the
    inheriting objects); the docs point per-frame values to Shader Globals.

## 6. Design

### 6.1 Line diff (`editor/util/LineDiff.{h,cpp}`, new)

Ported from Doriax `editor/util/LineDiff.*` (fork code, header kept: `// (c) Eduardo Doria and contributors`), in
`namespace jasperengine::editor`. The header comment names the origin of the post-passes ("the passes of VS Code's
line diff, `defaultLinesDiffComputer`, MIT, Copyright (c) Microsoft Corporation") and NOTICE.md gets a line for it
under the adapted-code notes (conservative: the heuristics follow VS Code's).

```cpp
struct LineChange {            // exclusive ends; empty original = insertion, empty modified = deletion
    int originalStart, originalEnd, modifiedStart, modifiedEnd;
    bool operator==(const LineChange&) const = default;
    enum class Kind : uint8_t { Added, Modified, Deleted };
    Kind kind() const;
};
class LineDiff {
public:
    static constexpr int kMaxEdits = 1000;
    static std::vector<LineChange> compute(std::span<const std::string> original, std::span<const std::string> modified);
    // unified diff text (3 context lines, "@@ -a,b +c,d @@"), for diff_file source "editor"
    static std::string unified(std::span<const std::string> original, std::span<const std::string> modified,
                               const std::vector<LineChange>& changes, std::string_view oldName, std::string_view newName);
};
```

Algorithm as Doriax: trim common prefix and suffix, intern lines to ints, Myers O(ND) with a trace capped at
`kMaxEdits`, then `joinByShifting` twice, `shiftToBetterBoundaries`, `joinAcrossShortMatches`. Lines are compared
byte for byte; callers strip `\r` before.

### 6.2 Baselines (`editor/vcs/TextBaselines.{h,cpp}` new; `VcsService`)

`VcsService` gains:

```cpp
// D5: "HEAD" for git, "BASE" for svn ("" while not active)
std::string baselineRevision() const;
// the memo key of a file's content at rev: key, rev, HEAD (headOid), the change's baseOid and oldPath
std::string contentMemoKey(const RepoPath& key, std::string_view rev) const;
```

`SceneDiffTracker::baselineKeyOf` becomes a call to `contentMemoKey` (same text, one implementation).

```cpp
namespace jasperengine::editor::vcs {
    enum class BaselineState : uint8_t { None, Pending, Ready, NewFile, Untracked, Ignored, OutsideRepository,
                                         Binary, LfsPointer, TooLarge, Failed };
    struct TextBaseline {
        BaselineState state = BaselineState::None;
        std::shared_ptr<const std::vector<std::string>> lines;   // Ready: split at '\n', '\r' dropped
        std::string revision;                                     // "HEAD" / "BASE"
        std::string error;                                        // Failed: the tool's message
        uint64_t version = 0;                                     // grows when lines or state change
    };
    class TextBaselines {
    public:
        static constexpr size_t kMaxBytes = 4u << 20;
        // Main thread, once per frame (CodeEditor::show): fetches what the open files need, adopts
        // finished fetches, forgets files no longer asked for
        void update(VcsService& service, std::span<const std::filesystem::path> openFiles);
        const TextBaseline& get(const std::filesystem::path& absolute) const;   // None when unknown
        void clear();
    };
}
```

Per open file: `toKey` fails, so `OutsideRepository`; `isIgnored`, so `Ignored`; the change says untracked or
`newFile()`, so `Untracked` / `NewFile`; otherwise the memo key (with `oldPath` when the change is `Renamed`, so the
renamed file is compared with its old content) is looked up and, when new, a fetch is submitted to
`jobs::system().processChannel()` with `service.providerForReads()` (as `SceneDiffTracker::baselineFor`, R24: no
process on the main thread). The job classifies: NUL byte, so `Binary`; starts with
`version https://git-lfs.github.com/spec/v1`, so `LfsPointer`; over `kMaxBytes`, so `TooLarge`; else splits.
A fetch whose bytes equal the current ones keeps the same `lines` pointer and version (the refetch after a save
changes nothing on screen). Fetch results live under a mutex in a shared record (the job may outlive the cache).
Nothing happens while the service is not `Active` or the preference is off: `update` clears and returns.

### 6.3 The widget (`editor/window/widget/CustomTextEditor.{h,cpp}`)

Public API added:

```cpp
struct DiffColors { ImU32 added, modified, deleted; };
void SetDiffColors(const DiffColors& colors);
// lines of the baseline (shared, not copied), and its name for tooltips ("HEAD")
void SetDiffBase(std::shared_ptr<const std::vector<std::string>> lines, const std::string& revision);
void ClearDiffBase();
bool HasDiffBase() const;
// the changes, brought up to date with the buffer first (tests, MCP); empty without a base
const std::vector<LineChange>& GetDiffChanges();
// the change covering a 0-based line, or the deletion marked at that line; -1 for none
int GetDiffChangeAt(int line);
// puts the baseline's lines back as one undo step that never merges; false when out of range
bool RevertDiffChange(int index);
bool GoToNextChange();       // Alt+F5, wraps
bool GoToPreviousChange();   // Shift+Alt+F5, wraps
// the highlight of the character at (line, column): Default between tokens
TokenType GetTokenTypeAt(int line, int column) const;
```

Private state as Doriax (`hasDiffBase`, `diffChanges`, `diffDirty`, `diffNextTime`, `diffVersion`, hover and peek
fields, `DiffPeekRow`), plus `diffBaseTokens` (`std::vector<std::vector<Token>>`, the base tokenized once per
`SetDiffBase` with `tokenizeText` carrying an `OpenToken` across lines, so a removed line inside `--[[ ]]` is a
comment), `diffPeekPinned`, `diffRevision`, `diffColors`.

- `tokenizeAll` sets `diffDirty` when a base is set (every edit, `SetText`, undo and redo pass there).
- `updateDiff()` at the start of `Render` after the metrics: `LineDiff::compute(*base, lines)`, timed; the next
  diff may run `20 * elapsed` later; while waiting it calls `Backend::getApp().requestFrame()` so an idle editor
  still catches up.
- `addUndoRecord(bool mergeable = true)`: `RevertDiffChange` passes `false`.
- `RevertDiffChange` as Doriax `revertDiffChange` (cursors below the change shift, cursors inside move to its first
  line, `ensureValidCursors`, `mergeCursors`, `tokenizeAll`, `finalizeUndoRecord`), refusing when `diffDirty`
  (the diff is brought up to date first by the public entry points, so it is refused only for an index out of
  range).
- Drawing, in `Render` order: after `renderLineNumbers`, `renderDiffGutter` (x = `origin.x + leftMargin +
  lineNumberDigits * charWidth + leftMargin * 0.5`, rounded; first visible change found by `lower_bound` on
  `modifiedEnd`, not a scan); after `renderCursors`, `renderDiffOverview` (scrollbar's left third, 60% alpha,
  min 2 px, through `ImGui::GetWindowScrollbarRect`); after the find dialog, `renderDiffPeek`.
  Shapes keep colour-blind users served: solid bar added, hatched bar modified, triangle deleted.
- Input: `updateDiffPeek(origin)` runs before `handleMouseInput`. Hover on a marker for 150 ms opens the popup,
  leaving marker and popup for 300 ms closes it; a click on a marker opens it at once pinned (Esc, a click
  elsewhere or an edit that moves the change closes it) and does not move the caret. A click on a marker is
  consumed before `handleMouseInput`.
- Popup header: "Added lines 12-14", "Changed line 7", "Removed 2 lines" and buttons Previous Change, Next Change,
  Revert Change (`ICON_FA_ROTATE_LEFT`, tooltip "Revert to HEAD" with `diffRevision`), close when pinned. Body:
  removed rows then added rows, `-`/`+` signs, line tints and the changed byte range of paired lines (Doriax
  `changedBytes`, cut at UTF-8 boundaries), base rows drawn with `diffBaseTokens`, current rows with `lineTokens`;
  only visible rows drawn; scrolls past 45% of the editor height.
- Context menu: after Find, a separator and Revert Change (enabled when the primary caret's line has a change),
  Next Change `Alt+F5`, Previous Change `Shift+Alt+F5` (enabled when there are changes). Keys handled in
  `handleKeyboardInput`.
- `renderLineTokens(drawList, pos, line, tokens)` is extracted from `renderText` so the popup draws the same.

### 6.4 The code editor window (`editor/window/CodeEditor.{h,cpp}`)

- Owns `vcs::TextBaselines baselines`. `show()` collects the open files' absolute paths and calls
  `baselines.update(project->getVcs(), files)` once per frame when `VersionControlSettings::codeEditorChanges` is
  on (else `baselines.clear()` and every editor's `ClearDiffBase`).
- `EditorInstance` gains `uint64_t baselineVersion = 0`. When `baselines.get(file).version` differs: `Ready`, so
  `SetDiffBase(lines, revision)`; any other state, so `ClearDiffBase()`. `SetDiffColors` from `vcs::colorOf`
  (`Badge::Added/Modified/Deleted`, `editor/window/vcs/VcsOverlay.h`) once at creation.
- Status row: after the language and modified mark, a segment `ICON_FA_CODE_BRANCH " 3 changes"` (or "No changes",
  or the reason of decision 5), tooltip "2 modified, 1 added, 1 removed against HEAD"; a click opens a menu: Next
  Change, Previous Change, Show Diff (`VcsUi::openTextDiff(file, baselineRevision)`), Show History
  (`VcsUi::openHistory(file)`), Revert File... (`VcsUi::run(VcsAction::Revert, {file})`, its own confirmation).
  Nothing is drawn when the service is not active or the preference is off.
- MCP support: `BufferState` gains `std::vector<LineChange> changes`, `std::string baselineState`, `std::string
  baselineRevision`; `revertChangeAt(const std::string& file, int line1Based, std::string* error)`;
  `std::optional<std::string> bufferText(const std::string& file)` (for `diff_file` source editor).

### 6.5 Status bar (`VcsTypes.h`, `GitProvider`, `SvnProvider`, `VcsService`, `VcsUi`)

```cpp
enum class VcsOperation : uint8_t { None, Merge, Rebase, CherryPick, Revert, Bisect };
const char* vcsOperationName(VcsOperation);        // "", "merge", "rebase", "cherry_pick", "revert", "bisect"
struct VcsBranchInfo {
    ...
    std::string tag;                 // a detached HEAD at a tag (git), a ^/tags/ URL's tag (svn)
    VcsOperation operation = VcsOperation::None;
    int operationStep = 0, operationTotal = 0;     // rebase progress, 0 when unknown
    std::string operationBranch;     // the branch being rebased (rebase-merge/head-name)
};
// "main", "tag v1.2", "a1b2c3d", "feature | REBASE 2/5", "main | MERGING"
std::string headLabel(const VcsBranchInfo& branch);
```

- `GitProvider` resolves its git directory once (`git rev-parse --absolute-git-dir`, under the provider's
  configuration mutex at first use; linked worktrees answer their own directory). `status()` checks, with
  `std::filesystem::exists` only (no process): `MERGE_HEAD`, `rebase-merge/` (`msgnum`, `end`, `head-name`),
  `rebase-apply/` (`next`, `last`), `CHERRY_PICK_HEAD`, `REVERT_HEAD`, `BISECT_LOG`.
- New `IVcsProvider::headTag(std::string& out)` (default unsupported); git: `git describe --tags --exact-match HEAD`
  (exit 128 means no tag). `VcsService::pollOnce` calls it only when the HEAD is detached and its oid differs from
  the previous snapshot's, else carries the previous tag over (the provider stays free of mutable state, as its
  header promises).
- `SvnProvider::branchInfo`: a name below `^/tags/` sets `tag` to its next component.
- `VcsUi::drawStatusBarSegment`: icon `ICON_FA_TAG` for a tag, `ICON_FA_CODE_COMMIT` for a bare detached commit,
  `ICON_FA_CODE_BRANCH` otherwise; label `headLabel(...)`, ahead/behind, then "N changes" or, with conflicts,
  "N conflicts" in the conflict colour; an operation in the amber modified colour. Tooltip: provider message,
  "Detached HEAD at a1b2c3d4", upstream, "Changes: 2 staged, 3 modified, 1 untracked, 2 conflicted", the operation
  in words ("Rebasing feature: step 2 of 5. Resolve conflicts, then continue the rebase in a terminal"), the
  existing errors. Click: Conflicts window when there are conflicts, else Commit.

### 6.6 Scene default shader values: engine

`engine/core/shader/ShaderUniforms.{h,cpp}`:

```cpp
enum class UniformSource : uint8_t { Global, Own, Fallback, SceneDefault, ShaderDefault, Zero };
const char* uniformSourceName(UniformSource);   // "global", "own", "fallback", "scene_default", "shader_default", "zero"
struct UniformChain {
    const ShaderUniformValues* values = nullptr;        // the material's or the component's own
    const ShaderUniformValues* fallback = nullptr;      // a mesh's shaderUniforms under its material's
    const ShaderUniformValues* sceneDefault = nullptr;  // Scene::getInheritedShaderUniforms; null: none
    const ShaderManifest* manifest = nullptr;
    const ShaderGlobalBindings* bindings = nullptr;
};
// the value a member is written with, and where it came from
static Vector4 ShaderUniforms::resolve(const UniformChain& chain, const std::string& name, UniformSource* source = nullptr);
void CustomUniformBlock::writeValues(const UniformChain& chain);
```

The two existing `writeValues` overloads stay, as thin wrappers building a chain without `sceneDefault` (callers
outside this feature do not change). `resolve` is the only place the order lives; the Inspector and MCP call it.

`engine/core/SceneSettings.h`: five `ShaderUniformValues` fields (`defaultMeshShaderUniforms`,
`defaultUIShaderUniforms`, `defaultSkyShaderUniforms`, `defaultPointsShaderUniforms`,
`defaultLinesShaderUniforms`), empty by default.

`engine/core/Scene.{h,cpp}`:

```cpp
void setDefaultShaderUniforms(ShaderType type, const ShaderUniformValues& values);
const ShaderUniformValues& getDefaultShaderUniforms(ShaderType type) const;   // empty for other types
void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector4& value);   // + Vector3/2/float
Vector4 getDefaultShaderUniform(ShaderType type, const std::string& name) const;
bool hasDefaultShaderUniform(ShaderType type, const std::string& name) const;
bool removeDefaultShaderUniform(ShaderType type, const std::string& name);
// what an object whose own shader is ownShader inherits: null when it has one, when the scene has no
// default shader of the type, or when the scene has no values for it (the zero-cost path)
const ShaderUniformValues* getInheritedShaderUniforms(ShaderType type, const std::string& ownShader) const;
```

A type without a scene default (DEPTH, POSTPROCESS, ...) logs an error once per call site and changes nothing.
Setters compare and return early when unchanged, then call `RenderSystem::needUpdateDefaultShaderUniforms(type)`.

`engine/core/subsystem/RenderSystem.{h,cpp}`:

- `needUpdateDefaultShaderUniforms(ShaderType)`: sets `needUpdateShaderUniforms` on every component of the type
  that inherits: meshes with an empty `customShader` (their submeshes with a material shader then get no
  inherited pointer when written), UI, Sky, Points, Lines, Trail and Line Renderer components with an empty
  `customShader` (every type whose load resolves `getDefaultCustomShader(type)`; the implementer lists them with a
  grep and covers each). Changing the default shader itself already reloads them.
- `writeSubmeshCustomUniforms`: `sceneDefault = submesh.material.shader.empty() ?
  scene->getInheritedShaderUniforms(MESH, mesh.customShader.path) : nullptr`; the extra pass never inherits.
- `writeComponentUniforms` / `applyComponentUniforms` and the load functions that write blocks directly gain the
  component's `sceneDefault` (the same call with its type and `customShader`).
- No component field is added (MeshComponent's size is feature `mesh-memory`'s budget).

Lua (`engine/core/script/binding/CoreClassesLua.cpp`, next to `setPostProcessUniform` :3224):
`setDefaultShaderUniform(type, name, value)` (overloads Vector4, Vector3, Vector2, number), `getDefaultShaderUniform`,
`hasDefaultShaderUniform`, `removeDefaultShaderUniform`; `ShaderType` is already a Lua namespace (:1984). The Lua
API data regenerates from `tools/lua_api`; `docs/mcp/guide/lua-api.md` gets the four functions and the precedence
line.

### 6.7 Scene default shader values: editor

- `editor/Catalog.h/.cpp`: scene properties `default_mesh_shader_uniforms`, `default_ui_shader_uniforms`,
  `default_sky_shader_uniforms`, `default_points_shader_uniforms`, `default_lines_shader_uniforms` of type
  `ShaderUniformValues` in the getter, default and setter switches (next to `default_mesh_shader` :698, :891,
  :1131), and `Catalog::getDefaultShaderUniformsProperty(ShaderType)`. Edits go through
  `ScenePropertyCmd<ShaderUniformValues>` (undo, redo, dirty scene).
- `editor/Stream.cpp`: encode `defaultMeshShaderUniforms` ... with `encodeShaderUniforms` when non-empty (beside
  :4376); decode with `decodeShaderUniforms`, an absent key clearing the values (play restore stays exact).
  `editor/StreamSchema.cpp:62`: the five keys added to `SceneSettings`.
- `editor/Factory.cpp` (beside :3082): `scene->setDefaultShaderUniforms(ShaderType::MESH, {...})` per non-empty set,
  with the existing uniform formatter, so exported games render the same.
- Inspector, scene section (`InspectorWindow.cpp:12933-12955`): one table per default type (ID
  `<property>_table`), its Shader row, then, when a shader is set: a status row ("Used by N objects" counted over
  the inheriting components only while the scene Inspector draws; "Not used by any object"; "Shader failed to
  build" when every user's build failed), then `SeparatorText("<ShaderName> Properties")` and a uniforms table.
  Rows: a library shader (manifest) draws its `@property` rows with labels, ranges and colour pickers (the
  value-row part of `drawShaderManifestRows` refactored to take a values getter and setter so a scene property can
  feed it); a fork draws its members, taken from the inheriting components' resolved blocks (union, block order,
  engine-written first: Doriax `orderShaderUniforms`); a fork no object uses yet shows "Properties show once an
  object uses this shader". Values set but not declared: "Unused values (N)" with Remove (one undo step).
- Inspector, components: `drawCustomShaderRow` (:6315) splits into the shader table (labels Shader / Depth Shader
  only) and, below `SeparatorText("<ShaderName> Properties")`, a uniforms table. Each row's value is
  `ShaderUniforms::resolve` of the component's chain. An inheriting object's own value shows the revert arrow
  "Revert to Scene Default" (removes the member from its `shaderUniforms`, one undo step); an inherited value
  without an override has the tooltip "From the scene's default (Scene > Default Shaders)". The mesh's material
  rows (`drawShaderManifestRows` with `material.shaderParams`) resolve with the material, mesh and scene layers.
- Tables (decision 10): `beginTable` gains an optional `uint64_t widthKey`; uniform tables pass a hash of their
  member names and use `ImGuiTableFlags_Resizable`; the first column's width is `max(label widths)` with room for
  the revert arrow when one can show. Post-process pass tables (:6838) become resizable with the same keyed IDs and
  the comment there is replaced.

### 6.8 MCP

- `code_editor` (`editor/mcp/tools/ScriptTools.cpp:1820`): status buffers gain `vcs: {state, revision, changes:
  [{kind, lines: [first, last], base_lines: [first, last]}]}` (1-based inclusive; an empty range is `[]`; `kind`
  added/modified/deleted; deleted's `lines` is the line it is marked at). New action `revert_change` with `path` and
  `line` (a line of the change, or the line a deletion is marked at): one undo step in the buffer (unsaved until
  `save`), the result lists the buffer's changes after. Refused, with the reason: no change at that line, no
  baseline (its state), buffer not open.
- `diff_file` (`VcsTools.cpp:676`): `source` `editor` (default) or `disk`. Editor: the open buffer (unsaved edits
  included) against `rev` through `LineDiff::unified`, `unsaved: true` in the result; a file not open in the code
  editor, or a buffer without edits, uses git/svn as today. The result says which `source` answered.
- `get_vcs_status`: `branch` gains `tag`, `operation`, `operation_step`, `operation_total`; a top-level `counts:
  {staged, modified, untracked, conflicted}`; `jasper://vcs/status` the same.
- `get_shader_uniforms` / `set_shader_uniform` (`editor/mcp/tools/AssetTools.cpp:4066,4085`): `shader_type`
  (`mesh|ui|sky|points|lines`) without `entity` and `pass_index` targets the scene's default values (`scene`
  picks the scene). get: `default_shader`, `used_by`, `build_failed`, `members` (as the Inspector unions them),
  `parameters` (manifest), `values`, `unused_values`. set: one `ScenePropertyCmd`; with no default shader of that
  type, the value is stored and the result says it is unused until one is set. On an entity, every member gains
  `effective` and `source` (`uniformSourceName`), so an agent sees the scene default at work.
- `get_scene_settings` / `set_scene_settings` (`SceneTools.cpp:842`): settings `default_<type>_shader_uniforms`,
  kind `ShaderUniforms`: `{name: number | [x,y,z,w] | "#RRGGBB[AA]" | null}`; an object merges (null removes a
  member), `null` clears all.
- Budgets: descriptions stay within `tools/mcp/budget.json` (`tool_budget.py --check`); `docs/mcp/TOOLS.md`
  regenerated. Guide text: `docs/mcp/guide/vcs.md` (code editor markers, `revert_change`, `source`, operation
  fields), `docs/mcp/guide/shaders.md` (scene defaults, precedence, `source`); the agent guide digest only if a line
  fits its headroom.

### 6.9 Settings, docs

- `VersionControlSettings::codeEditorChanges = true` (settings.yaml `version_control.code_editor_changes`),
  Preferences > Version Control > "Show Changes in Code Editor", settings key `editor.vcs.code_editor_changes`
  (`editor/settings/SettingsRows.cpp` rows and search table near :2769).
- `docs/editor/version-control.md` "What changed": the code editor bullet; the status bar paragraph.
- `docs/editor/keyboard-shortcuts.md`: Alt+F5 / Shift+Alt+F5 in the Code Editor context.
- `docs/shaders/README.md` and `docs/mcp/guide/shaders.md`: scene default properties, the precedence list.
- `docs/unity-glossary.md`: "none: Unity has no per-scene shader property defaults (closest: `Shader.SetGlobal*`,
  applied when the material leaves the property undefined) | Scene > Default Shaders > <Shader> Properties, Lua
  `scene:setDefaultShaderUniform`".

### 6.10 Platforms

Everything in 6.1-6.5 and 6.7-6.9 is editor code (Windows, Linux, macOS editors); processes go through the existing
`ProcessExec` (UTF-8 arguments, no shell). Windows: the git directory from `rev-parse` is converted with
`pathFromUtf8`; the CRLF rule of 7 keeps `core.autocrlf` checkouts quiet. macOS/Windows editors are not built here;
nothing platform-specific is added, the implementer keeps them compiling by reading. The runtime part (6.6) is CPU
packing of existing uniform blocks: no shader, pipeline or binding change, so every backend (GL 3.3, GLES3/WebGL2,
Metal, D3D11, Vulkan) and export target is covered by the same code path, and generated scenes carry the values.

## 7. Behaviour rules and edge cases

1. Line comparison ignores a trailing `\r` on both sides; the revert inserts the base lines as the buffer stores
   them (the buffer's save keeps the file's line endings).
2. A buffer reloaded from disk keeps its base; the diff reruns. A file renamed in the editor
   (`handleFileRename`) asks for the new key's baseline.
3. A staged rename (`Renamed` with `oldPath`) diffs against `oldPath` at the baseline revision.
4. HEAD moves (commit, checkout, pull): the memo key changes, the baseline is refetched; until it arrives the old
   markers stay (no flicker to "no changes"); an equal result changes nothing.
5. The popup closes when its change moves, the base changes or the buffer loses its base. A pinned popup survives
   scrolling and follows its change.
6. Revert Change is one undo step named like the editor's other steps; Ctrl+Z restores the change, Ctrl+Y reverts
   again. Multiple carets follow (below: shifted; inside: first line of the change).
7. The 1,000-edit cap: past it the whole differing middle is one modified change; its revert restores the whole
   middle.
8. Conflicted files compare against HEAD (ours): the conflict markers show as added lines.
9. Baseline fetch failures (a submodule path, a corrupt object) give `Failed`, the status row shows "Not compared"
   with the message in its tooltip; nothing is logged per frame (one log line per failed key).
10. Version control off, not available, or no repository: no markers, no status-row segment, no process.
11. Scene default values apply only while the scene's default shader of that type is set; clearing it keeps the
    values (unused), setting it again reuses them.
12. An object with its own shader, a submesh whose material names a shader, and an extra pass never inherit.
13. Prefab instances inherit from the scene they are in; a prefab asset stores no scene values. A child scene uses
    its own scene's defaults (each scene has its own RenderSystem).
14. A scene value for a reserved member (`time`, `resolution`) is refused (`ShaderUniforms::set` already refuses).
15. Play: Lua changes the play copy; Stop restores the edited scene (decode clears absent keys).
16. Undo of a scene value edit flags the inheriting objects again (the setter path), so the view updates.
17. Tables: a member added to a shader (new member set) gives its table a fresh width; an unchanged set keeps the
    user's width across sessions.

## 8. Performance and cost

- Disabled or unused: the preference off, or no active VCS, means no baseline, no diff, no drawing (one bool per
  editor per frame) and no process. Scene defaults unused: `getInheritedShaderUniforms` returns null, so writes are
  identical to today; five empty vectors per scene (120 bytes); no component bytes added; no GPU change.
- Typical edit in a 5,000-line file: prefix/suffix trimming, well under 0.2 ms. Worst case (1,000 edits in 10,000
  lines): bounded by the cap, and the 5% rule spreads it. Trace memory up to about 2 MB transiently at the cap.
- Drawing: visible changes only (`lower_bound`), the ruler one rect per change.
- Processes: one `cat-file`/`svn cat` per open file per HEAD move, on the process channel, never the main thread;
  zero per poll. Detached tag: one `describe` per HEAD change while detached. Operation detection: up to eight
  `exists` calls per poll on the poller thread.
- Scene default edits: O(components of that type) flagging per set; each flagged component rewrites its blocks
  once at its next draw. Per-frame Lua sets are allowed but documented against (decision 12).

## 9. Compatibility

- Scene files: five new optional keys, written only when non-empty; absent means empty, so old scenes load
  unchanged and re-save byte-identical. No scene version bump (an additive key with a default, as
  `physics2DSubSteps`); StreamSchema lists the keys so the unknown-data check (S03) does not flag them.
- `settings.yaml`: `version_control.code_editor_changes`, default true when absent.
- Generated code: new `setDefaultShaderUniforms` calls only for scenes that use the values.
- MCP: added parameters and result fields only; `diff_file`'s default `source: "editor"` returns git's output
  unchanged unless the open buffer has unsaved edits (stated in its description and the guide).
- Lua: four new functions; nothing renamed.

## 10. Tests

Suites per `docs/testing.md`. "Red control" names the run that must fail before the change.

| Suite | Test | Asserts |
|---|---|---|
| editor-tests | `LineDiffTest.Basics` (`editor/tests/util/LineDiffTest.cpp`) | identical: none; insertion, deletion, modification ranges; prefix/suffix trimming; two separate changes |
| editor-tests | `LineDiffTest.Heuristics` | an inserted function between two `end`s lands on the less indented boundary; short matches join; shift joins |
| editor-tests | `LineDiffTest.EditCap` | more than `kMaxEdits` edits give one change covering the middle |
| editor-tests | `LineDiffTest.Unified` | `unified` output for an add/modify/delete matches a fixed text (3 context lines, headers) |
| editor-tests | `LineDiffBench.SingleEditTenThousandLines` (Release only, skipped under sanitizers) | under 1 ms |
| editor-tests | `CustomTextEditorDiff.MarkersFollowEdits` (`editor/tests/window/widget/CustomTextEditorTest.cpp`) | `SetDiffBase` then `InsertText`/`Backspace`: `GetDiffChanges` kinds and ranges after each edit |
| editor-tests | `CustomTextEditorDiff.RevertIsOneUndoStep` | revert restores base lines; text typed under 300 ms before stays a separate undo step (red control: with `addUndoRecord()` merging, Undo after revert also removes the typing); undo/redo round trip; carets follow |
| editor-tests | `CustomTextEditorDiff.CrlfBaseMatches` | a CRLF base and LF buffer of the same text give no changes |
| editor-tests | `CustomTextEditorDiff.PeekTokensCarryBlockComments` | a removed line inside `--[[ ]]` of the base reads `MultiLineComment` (red control: a fresh `OpenToken` per row, Doriax's way) |
| editor-tests | `CustomTextEditorDiff.KeysAndMarkerClick` (ImGui context harness already in the file) | Alt+F5/Shift+Alt+F5 move the caret to the next/previous change and wrap; a click on a marker pins the peek and leaves the caret |
| editor-tests | `CustomTextEditorHighlight.*` (the five deleted by `304e64e43`, rewritten on `GetTokenTypeAt`) | multi-line Lua, levelled long brackets, line comments and strings ending at the line, C++ block comments and raw strings, CMake bracket comments (red control: `tokenizeAll` resetting `OpenToken` per line, the pre-`b714a0753` behaviour, fails them) |
| editor-tests | `TextBaselinesTest` (`editor/tests/vcs/`, temp repos via `VcsTestSupport.h`) | git tracked file: Ready with HEAD content; untracked: Untracked; added: NewFile; ignored: Ignored; staged rename: old content; LFS pointer: LfsPointer; NUL: Binary; svn: BASE content after another checkout commits (not the server's HEAD); a refetch with equal bytes keeps the version |
| editor-tests | `GitProviderTest.DetachedAndOperations` | detached at a tag: `tag` set by `headTag`; detached untagged: empty tag and `headLabel` = 7-char oid; conflicted merge: `Merge`; interrupted rebase: `Rebase`, step/total and branch (red control: today's provider reports none of them) |
| editor-tests | `VcsUiLabelTest.HeadLabel` | `headLabel` texts of decision 6 |
| core-tests | `ShaderUniformsChainTest.Precedence` (`engine/tests/core/shader/`) | `resolve` and `writeValues(chain)` bytes: global > own > fallback > scene default > manifest > zero, with sources |
| core-tests | `SceneDefaultUniformsTest.FlagsOnlyInheritors` | `setDefaultShaderUniform(MESH, ...)` flags meshes without `customShader`, not one with its own; same for UI, Points, Lines/Trail; unchanged value flags nothing; DEPTH refused |
| core-tests (GPU) | `SceneDefaultUniformsGpuTest.InheritedTint` (`engine/tests/core/render/`, the `RenderGpuTest` harness) | a fixture fork writing `u_fs_customParams.tint` as the colour, set as scene default; centre pixel = scene tint; with a component override = override; after removing the scene value = zero (red control: RenderSystem passing no `sceneDefault`) |
| editor-tests | `SceneStreamTest.DefaultShaderUniformsRoundTrip` (`editor/tests/stream/`) | encode/decode keeps the five sets; an old scene without the keys decodes to empty and re-encodes byte-identical |
| editor-tests | `GeneratedScenesTest.DefaultShaderUniforms` | generated factory contains `setDefaultShaderUniforms(ShaderType::MESH, ...)` only when set |
| editor-tests | `SceneDefaultUniformsCmdTest` (`editor/tests/command/`) | `ScenePropertyCmd` on `default_mesh_shader_uniforms`: undo/redo restore values and re-flag inheritors |
| MCP | `tools/mcp/tests/test_code_editor_vcs.py` | in a git scratch project: open a committed Lua file, change it on disk (buffer reloads), `code_editor` status lists the change kinds/lines; `revert_change` on a deletion line restores it, buffer unsaved; `diff_file source=editor` shows no diff and `source=disk` shows the disk diff; `undo`-style `reload` brings the buffer back; preference off: no `vcs` field; untracked file: state `untracked` |
| MCP | `tools/mcp/tests/test_vcs.py::test_detached_and_merge_state` | `get_vcs_status` after `git checkout --detach v1`: `tag` "v1"; after a conflicting merge: `operation` "merge", conflicted count |
| MCP | `tools/mcp/tests/test_scene_default_uniforms.py` | `fork_shader target=scene_default`, `set_shader_uniform shader_type=mesh`; `get_shader_uniforms` on an inheriting entity: `source` `scene_default`, `effective` the value; on an entity with its own fork: not inherited; `set_scene_settings default_mesh_shader_uniforms {name: null}` removes; `undo` restores; save and reopen keeps values; the tools are hidden until their group is enabled (existing group check extended) |
| Lua doc test | the `docs/mcp/guide/lua-api.md` Lua blocks (existing doc test) | the new functions exist and run |

Screenshots (AGENTS.md "Evidence"), from an isolated editor through `tools/editor-shot.sh`: the code editor with
the three marker kinds, the ruler and an open popup; the status bar detached at a tag and mid-rebase; the scene
Default Shaders section with a library shader's properties and a component row showing Revert to Scene Default.

## 11. Acceptance criteria

1. A committed Lua file edited in the code editor shows green, hatched amber and red-triangle markers at the right
   lines within the frame of the edit, and matching marks in the scrollbar's left third.
2. Hover (150 ms) or click on a marker shows the removed and added lines, highlighted, with the changed bytes
   tinted; a removed line inside a block comment is highlighted as a comment.
3. Revert Change (popup button, context menu, `code_editor revert_change`) restores the baseline lines as one undo
   step that never merges with typing; Ctrl+Z brings the change back.
4. Alt+F5 / Shift+Alt+F5 and the popup's arrows visit every change in order and wrap.
5. Git baseline is HEAD and svn baseline is BASE; a staged-only change still shows; a staged rename diffs against
   the old path; CRLF working copies of LF blobs show no false changes.
6. Untracked, new, ignored, outside-repository, binary, LFS-pointer and over-4-MiB files show no markers and the
   status row names the reason.
7. With version control off, or the preference off, no baseline process runs and no marker or segment draws
   (`VcsService::pollCount` unchanged by the code editor; a test counts provider calls).
8. The status bar shows "tag v1.2" / a 7-character commit on a detached HEAD, "| MERGING", "| REBASE 2/5",
   "| CHERRY-PICKING", "| REVERTING", "| BISECTING" during those operations, and conflicts in the conflict colour;
   a click opens Conflicts while conflicts exist.
9. `get_vcs_status` reports `tag`, `operation`, step/total and `counts`.
10. The five highlight tests pass on `GetTokenTypeAt` and fail with the per-line `OpenToken` reset.
11. Scene > Default Shaders shows "<ShaderName> Properties" for each set default; edits are undoable, saved,
    reloaded and generated into exported games; "Used by N objects" is correct.
12. A component inheriting the scene default renders the scene value; its own value overrides it with a "Revert to
    Scene Default" arrow; objects with their own shader are unaffected (GPU test and MCP test).
13. Lua `setDefaultShaderUniform` / `getDefaultShaderUniform` / `hasDefaultShaderUniform` /
    `removeDefaultShaderUniform` work for the five types and refuse others.
14. `get_shader_uniforms` reports `effective` and `source` per member, and with `shader_type` the scene's values;
    `set_shader_uniform shader_type=...` and `set_scene_settings default_<type>_shader_uniforms` change them with undo.
15. Shader rows and property rows are separate tables; long member names do not widen the Shader column; uniforms
    and post-process tables are resizable and a new member set starts at a fitting width.
16. Old scenes load and re-save byte-identical; `tools/check-changed` green; `gen_tool_docs.py --check`,
    `tool_budget.py --check tools/mcp/budget.json` and the Lua doc test pass; NOTICE.md names VS Code's diff
    heuristics.

## 12. Files likely touched

New: `editor/util/LineDiff.{h,cpp}`, `editor/vcs/TextBaselines.{h,cpp}`, `editor/tests/util/LineDiffTest.cpp`,
`editor/tests/vcs/TextBaselinesTest.cpp`, `engine/tests/core/shader/ShaderUniformsChainTest.cpp`,
`engine/tests/core/render/SceneDefaultUniformsGpuTest.cpp` (+ fixture shader), `tools/mcp/tests/test_code_editor_vcs.py`,
`tools/mcp/tests/test_scene_default_uniforms.py`.

Changed (hot files marked *):
- Code editor: `editor/window/widget/CustomTextEditor.{h,cpp}`*, `editor/window/CodeEditor.{h,cpp}`*,
  `editor/tests/window/widget/CustomTextEditorTest.cpp`.
- VCS: `editor/vcs/VcsTypes.h`, `editor/vcs/VcsSnapshot.cpp`, `editor/vcs/IVcsProvider.h`,
  `editor/vcs/GitProvider.{h,cpp}`, `editor/vcs/SvnProvider.cpp`, `editor/vcs/VcsService.{h,cpp}`,
  `editor/vcs/SceneDiffTracker.cpp` (memo helper only), `editor/window/vcs/VcsUi.{h,cpp}`,
  `editor/tests/vcs/GitProviderTest.cpp`.
- Settings: `editor/AppSettings.{h,cpp}`, `editor/settings/SettingsRows.cpp`*,
  `editor/window/dialog/EditorSettingsWindow.cpp`.
- Engine: `engine/core/shader/ShaderUniforms.{h,cpp}`, `engine/core/SceneSettings.h`, `engine/core/Scene.{h,cpp}`*,
  `engine/core/subsystem/RenderSystem.{h,cpp}`* (uniform write helpers and load sites only),
  `engine/core/script/binding/CoreClassesLua.cpp`*.
- Editor data: `editor/Catalog.{h,cpp}`*, `editor/Stream.cpp`*, `editor/StreamSchema.cpp`*, `editor/Factory.cpp`*.
- Inspector: `editor/window/InspectorWindow.{h,cpp}`*, `editor/window/InspectorShaderLibrary.cpp`.
- MCP: `editor/mcp/tools/ScriptTools.cpp` (code_editor), `editor/mcp/tools/VcsTools.cpp`,
  `editor/mcp/tools/AssetTools.cpp`*, `editor/mcp/tools/SceneTools.cpp`*, `tools/mcp/tests/test_vcs.py`,
  `tools/mcp/budget.json` (only if a limit needs an exception), `docs/mcp/TOOLS.md` (generated).
- Docs: `docs/editor/version-control.md`, `docs/editor/keyboard-shortcuts.md`, `docs/mcp/guide/vcs.md`,
  `docs/mcp/guide/shaders.md`, `docs/mcp/guide/lua-api.md`, `docs/shaders/README.md`, `docs/unity-glossary.md`,
  `NOTICE.md`.

## 13. Cross-feature dependencies

- `scene-integrity`: also edits `Stream.cpp`, `InspectorWindow.cpp` (scene section) and scene settings plumbing;
  merge the scene keys and the Default Shaders section carefully. No behavioural dependency.
- `export-pipeline`: shader keys of scene defaults. Values do not change variants or keys, so no key work here;
  both touch `Factory.cpp` (scene settings emission).
- `render-correctness`, `draw-perf`, `water-reflections`: `RenderSystem.cpp` hot spot. This feature changes only
  the custom-uniform write helpers and the `writeValues` calls in load functions; `draw-perf`'s skip of unchanged
  applies must keep rewriting a block when `needUpdateShaderUniforms` is set.
- `mesh-memory`: no MeshComponent field is added here; if submeshes move out of line, `writeSubmeshCustomUniforms`
  follows their new access.
- `scripting`: `CoreClassesLua.cpp` and `lua-api.md` hot spots (adjacent additions only).
- `agent-authoring`: `AssetTools.cpp` (shader overrides) and the agent guide's byte budget; this feature adds no
  agent-guide line unless headroom allows.
- `ui-components`: `InspectorWindow.cpp` (Button rows); independent sections.
- `unicode-text`: UTF-8 paths and child-process output; baseline bytes are read raw (no console code page
  involved), repository paths are UTF-8 `RepoPath`s already.
- Follow-up for the lead (not in any feature): Subversion scene diff and `diff_scene`/`diff_file` defaults use
  `HEAD` where D5 says BASE (section 4); `VcsService::baselineRevision()` from this feature is the fix's helper.

## Deviations

Recorded during the implementation (branch `dc/code-editor-git`).

1. **Subversion's HEAD reads BASE everywhere** (the follow-up of section 4, done here at the lead's request):
   `VcsService::effectiveRevision` maps `"HEAD"` to `"BASE"` for svn in `VcsService::baseContent`/`textDiff` (the
   text diff viewer, `diff_file`) and in `SceneDiffTracker`'s baseline reads (the Scene Diff, the Hierarchy and
   Inspector markers, `diff_scene`, `revert_to_baseline`). A revision number still reads that revision. Red control:
   `TextBaselines.SvnBaselineIsBase` (an unmodified file of a working copy behind the server diffs empty).
2. **`SvnProvider::textDiff` never diffed**: it passed the peg-escaped path (`a.lua@`), which `svn diff` takes
   literally for a working copy (1.14: "node 'a.lua@' was not found"), and its fallback then showed the whole file as
   added; it also took svn's empty diff of an unchanged file for an unversioned one. Found by the test above; fixed:
   the plain path, an empty diff of a versioned file is empty, an unversioned file (svn refuses to diff it) is still all
   added. Same test (its red run reverts both fixes).
3. **Prebuilt players**: the spec missed the cooked scene format (`engine/core/serialization/SceneSettingsCodec.cpp`).
   The five value sets end the scene settings record, after S57's (1.1) and S54's (1.2) fields: `FORMAT_MINOR` goes to
   3 (content format 1.3). An older reader skips them (the record is length-prefixed), a 1.3 reader of an older file
   stops at the record's end. Test
   `SceneDefaultShaderUniforms.CookedRoundTrip`.
4. **Keyboard shortcuts doc**: `docs/editor/keyboard-shortcuts.md` is generated from the editor's command catalog,
   which does not list the code editor's own keys (Ctrl+F, Alt+F5 are the widget's). Alt+F5 / Shift+Alt+F5 are
   documented in `docs/editor/version-control.md` and shown in the code editor's context menu instead.
5. **Agent guide**: no line added (lead decision: only agent-authoring restructures the guide; lean MCP rules). The
   detail is in `docs/mcp/guide/vcs.md` and `docs/mcp/guide/shaders.md`; two eval cases
   (`guard_scene_default_shader_values`, `guard_code_editor_unsaved_diff`) cover the new tool text.
6. **Tests grouped**: the head-label checks are in `editor/tests/vcs/GitProviderTest.cpp` (`GitProvider.HeadLabels`,
   `GitProvider.DetachedAndOperations`) rather than a `VcsUiLabelTest`; the stream, factory, cook and undo tests of
   the scene values share `editor/tests/stream/SceneDefaultShaderUniformsTest.cpp`; the scene flagging test is
   `engine/tests/core/subsystem/SceneDefaultUniformsTest.cpp`.
7. **`CodeEditor::isFileModified`** now reads the undo position instead of the flag `show()` refreshes, so an edit
   between frames (an MCP `revert_change`) counts at once for `diff_file` and `close`.
8. **Shared helpers**: `ProjectChecks::shaderTypeForComponent` maps components to shader types (no second mapping);
   `Catalog::getShaderUniformChain` and `Catalog::getDefaultShaderUse` are the one place the Inspector and MCP build
   the value chain and the default shader's users from.
