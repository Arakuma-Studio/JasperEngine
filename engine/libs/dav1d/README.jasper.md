# dav1d in JasperEngine

- Upstream: https://code.videolan.org/videolan/dav1d, tag `1.5.4` (BSD-2-Clause, `COPYING`).
- Used by: the HTML UI's `<video>` (`engine/core/htmlui/media/Av1Decoder.cpp`,
  `docs/html-ui/phase-6-video.md`).

## Vendored (byte for byte from the tag)
`COPYING`, `README.md`, `NEWS`, `THANKS.md`, `include/dav1d/*.h`, `include/common/*.h`,
`include/compat/{msvc,gcc}/stdatomic.h`, `src/*.c`, `src/*.h`, `src/win32/thread.c`, and the asm:
`src/x86/*` (the NASM files, their headers and `cpu.c`), `src/arm/*` (`asm.S`, the headers,
`cpu.c`, `64/*.S`, `32/*.S`) and `src/ext/x86/x86inc.asm` (x264's macro layer, ISC licence in its
header).

Not vendored: `tools/` (checkasm included), `tests/`, `examples/`, `doc/`, `gcovr.cfg`, the meson
files, `package/`, and the code of other architectures (`src/ppc`, `src/riscv`, `src/loongarch`).

## Ours
- `CMakeLists.txt`: replaces meson. 25 sources plus the 13 `*_tmpl.c` compiled twice
  (`BITDEPTH=8`, `16`), hidden visibility, `DAV1D_API` empty. The asm, as upstream's
  `src/meson.build` lists it, with dav1d's run-time CPU dispatch (`src/x86/cpu.c`, `src/arm/cpu.c`):
  - x86-64 (Linux, Windows, macOS on Intel, Android x86_64): NASM 2.14 or newer through CMake's
    `ASM_NASM` (elf64, win64, macho64 with `-DDAV1D_NASM_PREFIX`); without NASM the build says so
    and stays C (`dav1d: asm none`). 32-bit x86 stays C.
  - aarch64 (Android arm64-v8a, Linux arm64, iOS, macOS on Apple silicon) and armv7 (Android
    armeabi-v7a, assembled as ARM, not Thumb): the `.S` files through the C compiler (CMake's
    `ASM`). The dotprod, i8mm, SVE and SVE2 functions need clang 17 (Apple clang 16) or newer
    (`config.h`); older toolchains and GNU as keep NEON only. With MSVC on arm, upstream needs
    gas-preprocessor and armasm: C there.
  - Web (Emscripten): C. dav1d has no WebAssembly SIMD code.
  - `JASPERENGINE_DAV1D_ASM=OFF` builds C only.
- `config/config.h`: one configuration for every platform, chosen by the preprocessor (`HAVE_ASM`
  when the build defines `DAV1D_ASM` on x86-64/aarch64/armv7, `CONFIG_LOG 1`, both bit depths); it
  is also read by the `.S` files. `config/config.asm`: NASM's (x86-64). `config/vcs_version.h`:
  the version string.
- The Xcode targets `dav1d-ios`/`dav1d-macos` (`tools/htmlui/xcode_add_video_libs.py`). Xcode
  compiles a file once per target, so the script writes `bitdepth/<template>_<8|16>.c`, one
  wrapper per template and depth (`#define BITDEPTH`, `#include "src/<template>.c"`), and
  `xcode/cpu_x86.c`, `xcode/cpu_arm.c` (the two `cpu.c` of the asm, each for its architecture) and
  `xcode/aarch64_<name>.S` (the arm64 `.S` files under names no C file has: Xcode names objects by
  the file name, and `msac.S`, `refmvs.S` sit next to `msac.c`, `refmvs.c`); CMake does not use
  them. The `.S` files are excluded from x86_64 slices and the NASM files from arm64 ones
  (`EXCLUDED_SOURCE_FILE_NAMES[arch=...]`); a build rule assembles `*.asm` into `<name>.nasm.o` with
  the `nasm` of PATH, Homebrew's or `/usr/local/bin` (x86-64 Macs and the Intel simulator need it).
  The targets have `USE_HEADERMAP = NO`: Xcode's project header map, searched before the search
  paths, resolved `#include "config.h"` to `engine/libs/thorvg/config/config.h` (the asm did not
  assemble, and the C build had no bit depth); `xcode/cpu_*.c` stop the build with an `#error`
  when `config.h` is not ours.

## Updating
Copy the same file set from the new tag, compare `src/meson.build`'s `libdav1d_sources`,
`libdav1d_tmpl_sources`, `libdav1d_sources_asm` (and `win32` sources) with `CMakeLists.txt`, check
the `cdata.set*`/`cdata_asm.set*` lists in `meson.build` against `config/config.h` and
`config/config.asm`, update `vcs_version.h`, then run the HTML UI video tests
(`HTMLUI_TEST_ARGS=--gtest_filter=Video*:Av1*`: `Av1Decoder.AsmDecodesBitExactlyAsC` compares the
asm with the C functions frame by frame) and the Xcode script with `--check`. Upstream's checkasm
(`tools/checkasm`, not vendored) is the deeper check of a new asm release.
