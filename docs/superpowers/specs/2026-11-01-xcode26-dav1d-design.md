# dav1d's arm64 asm in the Xcode builds (`xcode26-dav1d`, ledger #26)

Status: fixed 2026-11-01 on `gaps/xcode26-dav1d`, verified on the owner's Mac (Apple M4 Max, macOS 26.2,
Xcode 26.6, Apple clang 21.0.0).

## Problem

`JasperEngine macOS` and `JasperEngine iOS` did not build: the `dav1d-macos` / `dav1d-ios` targets
stopped on dav1d's aarch64 asm, already on the original `main`. The first failing file was
`src/arm/64/refmvs.S` (via `xcode/aarch64_refmvs.S`):

```
src/arm/64/refmvs.S:57:9: error: unrecognized instruction mnemonic        (AARCH64_VALID_JUMP_TARGET, x12)
src/arm/64/refmvs.S:320:23: error: index must be an integer in range [-256, 255].   (RMVSF_IW8 ...)
src/arm/64/refmvs.S:382:22: error: expected compatible register, symbol or integer in range [0, 4095]
... 19 errors, BUILD FAILED (exit 65)
```

The local workaround (renaming the `DAV1D_ASM` define, `EXCLUDED_SOURCE_FILE_NAMES=*.S *.asm`)
built, but the library it produced decoded nothing (see Cause).

## Cause

The cause is not the assembler, and Xcode 26 did not introduce it. The project's header map does.
Xcode passes every target `-iquote <target>-project-headers.hmap`, which maps the file name of
**every header in the project** to its path, and searches it before the target's
`HEADER_SEARCH_PATHS`. thorvg's `engine/libs/thorvg/config/config.h` is in the project, and dav1d's
`engine/libs/dav1d/config/config.h` is not. So every `#include "config.h"` of dav1d (C and `.S`)
read thorvg's file (`clang -H` shows it). That file defines none of dav1d's macros:

- In the asm, `ARCH_AARCH64` is 0, so `src/arm/asm.S` never defines `AARCH64_VALID_JUMP_TARGET`
  (it reaches the assembler as a mnemonic), and `src/arm/asm-offsets.h` defines no `RMVSF_*`
  offsets. Those are exactly the errors above, and every `.S` file has them (the build stops at
  the first failing file).
- In the C, `CONFIG_8BPC` and `CONFIG_16BPC` are undefined: the decoder compiled with no bit depth
  and fails every stream ("Compiled without support for 8-bit decoding"). `HAVE_ASM` is 0 and
  `HAVE_POSIX_MEMALIGN` is 0. With the asm disabled this builds without an error, which is why the
  workaround looked fine.

dav1d's `config.h` was correct all along. On Xcode 26.6, with the right `config.h`, the aarch64 asm
assembles unchanged, including the dotprod, i8mm, SVE and SVE2 files (`config.h` enables them for
Apple clang 16 or newer). So nothing needs an upstream backport or a newer dav1d.

## Fix

`tools/htmlui/xcode_add_video_libs.py` (it owns the dav1d, opus and webm targets):

- It sets `USE_HEADERMAP = NO` in the targets' configurations, for all three libraries and both
  platforms. Their quoted includes now resolve only through their own search paths, as in CMake.
  The pass is idempotent, and `--check` fails while it is missing.
- The generated `engine/libs/dav1d/xcode/cpu_{arm,x86}.c` (compiled in every slice) stop the build
  with an `#error` when the `config.h` they read is not dav1d's (`DAV1D_ENGINE_CONFIG_H`). Another
  header map, or any other wrong `config.h`, now fails the build at once instead of producing a
  decoder that decodes nothing.

The project was regenerated with the script (12 settings lines). `engine/libs/dav1d/README.jasper.md`
documents it.

The verification found that `tools/htmlui/dav1d-asm-check` did not compare asm with C on arm.
`config.h` sets `TRIM_DSP_FUNCTIONS 1`, so `dav1d_get_cpu_flags()` always ORs in the target's
baseline flags (aarch64: NEON, plus dotprod where the compiler targets it; x86-64: SSE2) and the C
versions of those functions are not compiled. The "mask 0 = C" run was really NEON, which gives
x1.00 "speedups". `config.h` now lets a build override `TRIM_DSP_FUNCTIONS` (`#ifndef`; upstream
meson's `trim_dsp` option), and the check's `CMakeLists.txt` builds dav1d with
`TRIM_DSP_FUNCTIONS=0`. Shipped builds keep 1. `Av1Decoder.AsmDecodesBitExactlyAsC` (the engine
test, x86-64) keeps the trimmed build, so its "C" run includes the SSE2 asm. It still compares
SSSE3/AVX2/AVX-512 against that baseline.

## Verification (owner's Mac, asm on, no local workaround)

- Before the fix (base `4b0c4aa4c`): `dav1d-macos`, exit 65, 19 errors (above).
- `dav1d-macos`, `opus-macos`, `webm-macos` (macosx SDK), and `dav1d-ios`, `opus-ios`, `webm-ios`
  for both the iphonesimulator and iphoneos SDKs, arm64, Release: exit 0 and 0 errors for each
  SDK, 139 compiles each.
- App schemes, `ARCHS=arm64 CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO`, derived data in
  `~/dav1d-fix`: `JasperEngine macOS` (generic/platform=macOS), `JasperEngine iOS`
  (generic/platform=iOS Simulator) and `JasperEngine iOS` (generic/platform=iOS) each give exit 0,
  0 errors and `** BUILD SUCCEEDED **`.
- The libraries contain the asm. Each of the three `libdav1d-*.a` defines 591 `_neon` / `_dotprod`
  / `_i8mm` / `_sve` functions, including `_dav1d_splat_mv_neon`, `_dav1d_load_tmvs_neon` and
  `_dav1d_get_cpu_flags_arm`. Every DSP template object (`mc_tmpl_8.o`, `itx_tmpl_16.o`, ...),
  `refmvs.o` and `msac.o` reference `_neon` symbols, so the `*_dsp_init_arm` dispatch is compiled
  in.
- The asm runs and is bit-exact. `check.c` was linked against the Xcode-built `libdav1d-macos.a`
  (target settings, `TRIM_DSP_FUNCTIONS=0`) and run on 8 clips: the test fixtures (8-bit, 10-bit,
  4:4:4, odd size), the video_menu loop, and 720p 8-bit, 720p 10-bit and film-grain encodes. All
  8 were SAME. dav1d's CPU flags were 0x7 (NEON, dotprod, i8mm). The asm was x1.5 to x1.9 faster
  on the test cards and 720p clips and x10.9 on the menu loop. The same check built through the
  CMake path on macOS (Homebrew's CMake 4.2, `dav1d: asm aarch64 NEON`) also gave 8/8 SAME.
- Header audit of the macOS build: no library target's dependency files list a header of another
  library except the real dependencies (rmlui uses lunasvg; tinygltf uses json and stb).
- The guard: compiling the new `xcode/cpu_arm.c` with the old (header-map) arguments stops at the
  `#error`.

Other platforms: the change to the shared code is `config.h`'s `#ifndef` around an unchanged
default. Linux (build box, Release, glcore): `jasperengine-htmlui-tests --gtest_filter=Video*:Av1*`
passed 68 of 68, including `Av1Decoder.AsmDecodesBitExactlyAsC`. The untrimmed `dav1d-asm-check`,
built natively for x86-64 (gcc, NASM 2.16), was SAME on 5 of 5 clips, and C was now really C
(x1.6 to x7.7). Android: dav1d compiled
with NDK 28 for arm64-v8a and armeabi-v7a, with no warnings, and the asm check compiled for both.
The Gradle path's own CMake tree (`configureCMakeDebug[x86_64]`, CMake 3.22.1) built `dav1d`,
including the NASM files. All six Xcode sync scripts pass `--check`.

## Not covered

- The asm check did not run on an iPhone or in the simulator. It ran natively on macOS arm64,
  which uses the same `.S` sources, assembler and settings. The iOS builds only compile and link.
- A pre-existing issue outside the shipped paths: configuring dav1d for Android x86_64 with
  CMake 3.28 (the host's, not Gradle's 3.22.1) makes FindThreads fall back to `-pthread` (bionic
  has no `pthread_cancel`), and `Threads::Threads` then passes `-pthread` to NASM ("unable to open
  include file `thread'"). It is not fixed here.
