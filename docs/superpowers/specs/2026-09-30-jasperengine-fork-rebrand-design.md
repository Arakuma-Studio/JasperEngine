# JasperEngine: Fork and Rebrand of Doriax — Design

Date: 2026-09-30
Status: Implemented (see "Implementation notes" at the end)

## 1. Intent

**Stated by the owner**

- JasperEngine is the owner's own branded, general-purpose game engine.
- The base is Doriax (https://github.com/doriaxengine/doriax, MIT).
- First milestone: a clean fork plus a full rebrand that builds and runs. Feature direction (2D, rendering, scripting, editor) is decided later, each in its own spec.
- Full rename. This is a hard fork; painless upstream merges are not a goal.
- All six platforms matter: Linux, Windows, macOS, iOS, Android, Web.
- The GitHub repo stays private. CI uses on-demand full builds, the MinGW job is dropped, and ccache is used.
- Names, as the owner gave them:

  | Kind | Name |
  |---|---|
  | Namespace / lowercase ids | `jasperengine` |
  | Main header, web entry | `JasperEngine.h`, `JasperEngineWeb.cpp` |
  | Macros / CMake vars | `JASPERENGINE_*` |
  | Editor display name | `JasperEngine Editor` |
  | Editor executable | `jasperengine-editor` |
  | App id | `com.jaspergameengine.editor` |
  | Domain | `jaspergameengine.com` (owned, nothing hosted yet) |

**Assumed; the owner did not specify**

- No logo exists yet, so generated placeholder art is used.
- External links point to `github.com/JasperRaccoon/JasperEngine` until the domain hosts something.
- No existing Doriax projects need migrating.

**Success criteria**

- The string "doriax" (any case) appears only in the attribution allowlist.
- The engine and editor build for all six platforms: Linux, Web and Android locally, and all of them in GitHub Actions.
- The Linux editor, an exported Linux game and the Web build run and are shown to the owner as screenshots.

## 2. Starting point

- Doriax upstream `main` at `29348d7` (2026-09-29): 3,008 commits since 2016, about 200 MB of history, and 3,708 files in the tree.
- About 91k lines in `engine/core` and about 284k in `editor/`.
- Stack: Sokol (graphics), Box2D and Jolt (physics), Lua via LuaBridge3, SoLoud (audio), ImGui (editor), yaml-cpp.
- Name footprint: 3,158 occurrences in 714 files, 202 distinct tokens, and 20+ paths containing the name.
- This repo currently has one commit containing a one-line README.
- Host: cmake, gcc and clang are installed, and Docker is available. There is no sudo, several apt build dependencies are missing, and there is no `/dev/kvm`.

## 3. Rename rules (`tools/rebrand.py`)

The script is a committed, idempotent Python 3 tool (standard library only).

- `--dry-run` prints per-rule match counts and planned path renames.
- `--apply-paths` performs only the `git mv` operations.
- `--apply-content` rewrites text files.
- `--check` exits non-zero if any case-insensitive "doriax" remains outside the allowlist. It also lists binary files that contain the name.

### 3.1 Content rules

Rules are applied in this order. Special cases go first so the general rules never see them.

1. **URLs**
   - `github.com/doriaxengine/doriax` → `github.com/JasperRaccoon/JasperEngine`.
   - `https://doriax.org...`, `https://www.doriax.org...` and `https://docs.doriax.org...` → `https://github.com/JasperRaccoon/JasperEngine`.
   - `raw.githubusercontent.com/doriaxengine/...` asset links occur only in the README, which is rewritten by hand (section 4.2). The script excludes the README.
2. **Bundle and package ids**
   - `org.doriax.doriaxengine` → `com.jaspergameengine.jasperengine`
   - `org.doriax.editor` → `com.jaspergameengine.editor`
   - `org.doriaxengine.doriax` → `com.jaspergameengine.jasperengine`
   - Path form (e.g. `org/doriaxengine/doriax` in Java and Gradle paths) → `com/jaspergameengine/jasperengine`
   - Path-segment lists in code, e.g. `"org" / "doriaxengine" / "doriax"` in `editor/Exporter.cpp`, are fixed by hand and caught by `--check` and the Android build.
3. **Display strings**
   - `Doriax Engine` → `JasperEngine`
   - `Doriax Editor` and `Doriax editor` → `JasperEngine Editor`
4. **Glued macro:** `DORIAXEDITOR_` → `JASPERENGINE_EDITOR_`
5. **General rules**, as plain substring replacement so compound identifiers convert too:

   | Match | Replace |
   |---|---|
   | `DORIAX` | `JASPERENGINE` |
   | `Doriax` | `JasperEngine` |
   | `doriax` | `jasperengine` |

   Examples:
   - `DoriaxEditorWindowDelegate` → `JasperEngineEditorWindowDelegate`
   - `libdoriax` → `libjasperengine`
   - `.doriax/` (the per-project hidden folder) → `.jasperengine/`
   - `doriaxengine` (when not already handled by rule 1 or 2) → `jasperengineengine`. `--dry-run` must report zero hits for this; any hit gets a specific rule.

### 3.2 Path rules

Every file or directory whose name contains the name, in any case, is renamed with `git mv` using the general rules. Examples:

- `engine/core/Doriax.h` → `JasperEngine.h`
- `engine/platform/emscripten/DoriaxWeb.cpp` → `JasperEngineWeb.cpp`
- `engine/doriax.py` → `jasperengine.py`
- `Doriax.xcodeproj` → `JasperEngine.xcodeproj`
- `doriax-editor*` icons and `.desktop` files → `jasperengine-editor*`
- `org.doriax.editor.metainfo.xml` → `com.jaspergameengine.editor.metainfo.xml`
- `engine/project/assets/doriax.png` → `jasperengine.png`
- Android Java package directories move from `org/doriaxengine/doriax/` to `com/jaspergameengine/jasperengine/`

Directories are renamed deepest first.

### 3.3 Scope and exclusions

- **Processed:** text files tracked by git, detected by the absence of NUL bytes in the first 8 KB.
- **Excluded:**
  - `.git/`
  - `LICENSE`, `NOTICE.md`, `README.md` (handled by hand)
  - Binary files
  - Third-party trees `libs/`, `engine/libs/` and `shadercompiler/libs/`, **except** the three files that reference Doriax: `engine/libs/sokol/sokol.cpp`, `engine/libs/sokol/sokol_app.h` and `engine/libs/harfbuzz/CMakeLists.txt`
- **Kept as-is:** per-file copyright headers (`// (c) Eduardo Doria and contributors`, SPDX lines). The general rules cannot match "Doria " anyway.
- **Allowlist for `--check`:** `LICENSE`, `NOTICE.md`, `README.md`, `tools/rebrand.py`, `docs/superpowers/`, and any line containing "Based on Doriax" (the About window credit).

### 3.4 Known consequence

Projects made with upstream Doriax will not open until their `.doriax/` folder is renamed to `.jasperengine/`. No migration shim is added.

## 4. Branding and assets

### 4.1 Placeholder art (`tools/make_placeholder_art.py`)

Draws a bold "J" mark on a solid background. Pillow runs in a throwaway venv and is not a project dependency. The script and its outputs are committed; replacing the art means changing one source image and re-running it.

| Output | Used by |
|---|---|
| `editor/resources/icons/jasperengine-editor.png` (2048²) | Master editor icon |
| `editor/resources/icons/linux/jasperengine-editor-{16,32,48,64,128,256,512}.png` | Linux `.desktop` / AppImage |
| `editor/resources/icons/jasperengine-editor.ico`, `jasperengine-editor-cmd.ico` | Windows resources |
| `editor/resources/icons/jasperengine-logo_png.h` | Loading and About windows (C byte array, same format as the original) |
| `engine/project/assets/jasperengine.png` (413×260) | Default project template image |

Any other binary asset that `--check` reports as containing the name is regenerated or edited by hand.

### 4.2 Text

- **`README.md`:** rewritten. What JasperEngine is, the platform list, how to build (Docker scripts), and "Based on Doriax" with a link. Doriax's screenshots, badges, sample games and history section are removed.
- **`LICENSE`:** keeps `Copyright (c) 2026 Eduardo Doria and contributors` and adds `Copyright (c) 2026 JasperDev`.
- **`NOTICE.md`:** new. States that JasperEngine is a fork of Doriax, with the upstream URL and the fork-point commit.
- **About window:** adds a "Based on Doriax" line.

## 5. Build, verification and CI

### 5.1 Local builds (Docker)

| Target | Image | Notes |
|---|---|---|
| Linux | `docker/linux.Dockerfile`, `ubuntu:22.04` with Doriax CI apt packages, ninja, ccache, Xvfb, Mesa | Engine and editor |
| Web | `emscripten/emsdk`-based | Engine project |
| Android | Ubuntu with Android SDK, NDK and JDK | APK; not runnable (no KVM) |

- `tools/build.sh <linux|web|android> [--clean]` builds inside the matching container.
- The repo is mounted into the container, and build output goes to `build/<target>/`, which is git-ignored.
- The container runs as the invoking UID so files stay owned by the user.

### 5.2 Baseline

Before any rename, unmodified Doriax (commit 1) must build for Linux, Web and Android with the same scripts (commit 2). A failure there is fixed in the build tooling, never in engine code.

### 5.3 Local acceptance checks (after rebrand)

1. `tools/rebrand.py --check` exits 0.
2. The Linux engine and `jasperengine-editor` build with no errors.
3. `tools/smoke-test.sh <editor-project>` passes: `jasperengine-editor export` succeeds (`engine/project` is not an editor project, so the fixture is Doriax's public Tappy Plane sample, cloned outside the repo). The exported CMake project builds, and the game runs under Xvfb for at least 5 s without crashing.
4. The Web build is served locally, loads in headless Chromium, and renders a frame. Console errors are reviewed.
5. The Android APK builds.
6. Screenshots of the editor (title and logo visible), the exported Linux game and the Web game are shared with the owner.

### 5.4 GitHub Actions

Workflows are renamed and rebranded.

**Removed:**
- `nix.yml`, `flake.nix`, `flake.lock`, `misc/nix/`
- The Windows MinGW editor job
- All SignPath signing configuration

| Trigger | Jobs |
|---|---|
| Every push, any branch | Linux editor, Linux engine, Android, Web (Linux runners, about 27 min) |
| `workflow_dispatch`, PR into `main`, `v*` tag, release | The above, plus Windows MSVC editor, Windows MSVC engine (GL and Vulkan), and one merged macOS job (editor, Xcode macOS, Xcode iOS) |
| Release published | Attach Linux AppImage, Windows zip and macOS dmg |

**Caching:**
- ccache via `actions/cache` for CMake/Ninja builds on Linux, Android, Web and macOS.
- The Windows MSVC jobs move to the Ninja generator (inside a VS dev shell) so ccache applies. If that proves unreliable, they revert to the VS generator without cache, and the spec is updated.
- Xcode builds are not cached.

**Cost estimate:**
- A full-matrix run is about 460 billed minutes (~$2.70 at 2026 rates).
- A normal push uses about 27 Linux minutes, within the free tier.

### 5.5 Definition of done

All checks in 5.3 pass, and one full-matrix run on the PR into `main` is green on all jobs.

## 6. Git plan

- Work happens on branch `rebrand`.
- Remote `upstream` = `https://github.com/doriaxengine/doriax`, full history.
- `upstream/main` is merged with `--allow-unrelated-histories`. The README conflict is resolved to the upstream version, and the README is rewritten in commit 7.
- Tag `doriax-base` marks the fork point `29348d7`.

| # | Commit |
|---|---|
| 1 | Merge Doriax history; tag `doriax-base` |
| 2 | Docker images, `tools/build.sh`, `tools/smoke-test.sh`; baseline verified |
| 3 | Remove Nix, container recipes and FUNDING.yml (moved ahead of the rename so it does not touch files being deleted); add `tools/rebrand.py` |
| 4 | Rebrand: path renames only |
| 5 | Rebrand: content rewrite |
| 6 | Placeholder art and art script |
| 7 | README, LICENSE, NOTICE, About window |
| 8 | CI workflows |
| 9+ | Verification fixes |

Commits 4 and 5 are separate so git rename detection keeps `git log --follow` working.

The owner is asked before the first push of `rebrand` and before opening the PR.

## 7. Out of scope

- Any engine or editor feature work.
- A real logo, website, docs site or community links.
- Code signing and notarization.
- Migrating existing Doriax projects.
- Running the Android build on a device or emulator. Running Windows and macOS builds beyond CI compilation; CI only proves they compile and package.

## 8. Implementation notes

Differences from the plan above, discovered while implementing:

- **Headless editor:** the editor's file-dialog library needs a D-Bus session. The Linux image generates `/etc/machine-id`, and the smoke test runs the editor under `dbus-run-session` with the host's `/etc/passwd` mounted read-only.
- **Exporter path bug (upstream):** `export --project` with a relative path resolves scene files twice (`project/project/...`). The smoke test passes absolute paths. It is not fixed in engine code in this milestone.
- **Extra rename rule:** a bare `org.doriax` id (the metainfo developer id) maps to `com.jaspergameengine`.
- **Metainfo:** the upstream screenshot entries were removed, and the developer is JasperDev.
- **Windows CI:** the editor and both engine builds share one job (as macOS does), to avoid paying for Windows setup three times.
- **Removed as part of Nix:** `misc/containers/` (its images were built by the Nix workflow).
- **Version string:** `git describe` only matches `v[0-9]*` tags (CMake and CI). Otherwise the `doriax-base` tag named every build, e.g. "doriax-base-11-g8be898d9". Until the first `v*` tag, builds show the commit hash.
- **Web check:** the bare engine sample stops at startup for lack of precompiled shaders, which only the editor's export generates. The browser check therefore uses the web export of the fixture project (`export --backend opengles`, then `emcmake`), which runs.
- **Linux image:** includes `xdotool`, used to open menus for screenshots.
