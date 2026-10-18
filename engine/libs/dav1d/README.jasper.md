# dav1d in JasperEngine

- Upstream: https://code.videolan.org/videolan/dav1d, tag `1.5.4` (BSD-2-Clause, `COPYING`).
- Used by: the HTML UI's `<video>` (`engine/core/htmlui/media/Av1Decoder.cpp`,
  `docs/html-ui/phase-6-video.md`).

## Vendored (byte for byte from the tag)
`COPYING`, `README.md`, `NEWS`, `THANKS.md`, `include/dav1d/*.h`, `include/common/*.h`,
`include/compat/{msvc,gcc}/stdatomic.h`, `src/*.c`, `src/*.h`, `src/win32/thread.c`, and the
two CPU-flag headers `src/x86/cpu.h` and `src/arm/cpu.h` (`src/cpu.h` includes them by architecture).

Not vendored: `tools/`, `tests/`, `examples/`, `doc/`, `gcovr.cfg`, the meson files, `package/`,
and the asm and SIMD code (`src/x86`, `src/arm`, `src/ppc`, `src/riscv`, `src/loongarch` but the
two headers above, `src/ext`).

## Ours
- `CMakeLists.txt`: replaces meson. 25 sources plus the 13 `*_tmpl.c` compiled twice
  (`BITDEPTH=8`, `16`), hidden visibility, `DAV1D_API` empty.
- `config/config.h`: one configuration for every platform, chosen by the preprocessor
  (`HAVE_ASM 0`, `CONFIG_LOG 1`, both bit depths). `config/vcs_version.h`: the version string.
- The Xcode targets `dav1d-ios`/`dav1d-macos` (`tools/htmlui/xcode_add_video_libs.py`). Xcode
  compiles a file once per target, so the script writes `bitdepth/<template>_<8|16>.c`, one
  wrapper per template and depth (`#define BITDEPTH`, `#include "src/<template>.c"`); CMake does
  not use them (its OBJECT libraries compile the templates twice).

## Updating
Copy the same file set from the new tag, compare `src/meson.build`'s `libdav1d_sources`,
`libdav1d_tmpl_sources` (and `win32` sources) with `CMakeLists.txt`, check the `cdata.set*` list
in `meson.build` against `config/config.h`, update `vcs_version.h`, then run the HTML UI video
tests (`HTMLUI_TEST_ARGS=--gtest_filter=Video*`) and the Xcode script with `--check`.

## Adding the asm (phase 6 follow-up)
Copy `src/x86` (NASM: needs `config.asm` with `ARCH_X86_64`, `PIC`, `private_prefix dav1d`,
`STACK_ALIGNMENT`; CMake `enable_language(ASM_NASM)`) and/or `src/arm` (GAS `.S` files, built by
the C compiler; `src/arm/cpu.c`, the `*_init_tmpl.c` headers are already included by the
templates). Add their sources per architecture as `src/meson.build` does, set `HAVE_ASM 1` for
those architectures, and run dav1d's `checkasm` on each target CPU before shipping.
