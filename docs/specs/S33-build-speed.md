# S33 — Build speed

Wave 3. Depends on S02 (presets, `JASPERENGINE_UNITY_BUILD`, WERROR, the unit-test targets,
`tools/build.sh`) and S30 (release link flags, prebuilt player runtimes and their archives).

## 1. Goal

Engine, editor and game builds become much faster, both clean builds and the edit-and-rebuild
loop, with zero loss of correctness: every test stays green, every configuration stays
warning-free under WERROR, and shipped binaries are unchanged except where they are deliberately
better. Every lever in this spec is justified by a measurement of section 3 and is measured again
after it lands (section 7, AC1). A game with C++ scripts compiles only its own code and links a
prebuilt engine, as S30's prebuilt players already avoid compiling the engine at all.

## 2. Scope

**In scope**
- Measurement tooling that anyone can re-run: `tools/perf/build-bench.sh` (clean, warm-cache,
  no-op and edit rebuilds of one build tree inside its build image) and `tools/perf/build_stats.py`
  (`.ninja_log` breakdowns, header fan-out from `ninja -t deps`, header parse costs, GCC
  `-ftime-report` of the slowest units).
- ccache configuration of `tools/build.sh`, `tools/smoke-test.sh` and CI so a new build directory
  or worktree hits the cache (base directory, debug-info directory hashing, PCH sloppiness).
- Vendored libraries: compile commands free of tree-specific definitions and paths, so one object
  serves every tree; unity builds of the vendored libraries.
- Unity builds of our own code where they pay off (CI and full builds), not in edit loops.
- Precompiled headers for our own targets.
- A faster linker for Linux development and test builds, with a fallback, never for shipped games.
- Header fan-out cuts where a one-line header change rebuilds hundreds of units.
- C++-script games (S30's Compile method) linking a prebuilt engine library.
- `docs/testing.md` (Build speed section) and this spec's measurements.

**Out of scope**
- vcpkg or any package manager (README do-not-copy list).
- C++20 modules (GCC 11 cannot build them reliably; MSVC/Clang/NDK/Emscripten parity missing).
- Distributed compilation (distcc, icecream) and a shared ccache across the build servers: see
  section 9 (follow-up), decided against for now.
- Changing optimisation levels, LTO or any code-generation flag of a shipped binary for speed.

## 3. Current state (measured)

See "Measurements" below; the summary of what is wrong is filled in from them.

## 4. Reference (OpenEngine, MIT; read-only, never executed)

- `CMakePresets.json:679-709`: their editor development preset is ccache with unity builds OFF,
  their clean full build ccache with unity ON. We take the same split (D2), not the file.
- `cmake/AllocationHook.cmake:37`: `SKIP_UNITY_BUILD_INCLUSION` on a source that cannot share a
  unit, the pattern `jasperengine_skip_unity_build` already follows.
- They have no precompiled headers, no linker selection and no prebuilt engine for native game
  code; nothing else is taken.

## Measurements

(being filled in)

## Progress

- 2026-10-30: measurement tools committed (`tools/perf/build-bench.sh`, `tools/perf/build_stats.py`);
  baseline run `base` on server F (b620283cd).
