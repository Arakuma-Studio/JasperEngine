/*
 * dav1d configuration of the engine's build (engine/libs/dav1d/CMakeLists.txt and the Xcode
 * targets). Upstream generates this file with meson per target; here one header serves every
 * platform the engine builds for (Linux, Windows, macOS, iOS, Android, Emscripten), deciding with
 * the preprocessor. The asm (x86-64 NASM, aarch64 and armv7 NEON) is used when the build defines
 * DAV1D_ASM (CMakeLists.txt does where it assembles it; the Xcode targets do); the CPU features
 * are detected at run time by dav1d's own code (src/x86/cpu.c, src/arm/cpu.c). This header is also
 * read by the .S files (src/arm/asm.S); config.asm is NASM's.
 */
#ifndef DAV1D_ENGINE_CONFIG_H
#define DAV1D_ENGINE_CONFIG_H

/* both bit depths: 8-bit and 10/12-bit streams */
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
/* errors reach the engine's log through Dav1dSettings.logger (Av1Decoder) */
#define CONFIG_LOG 1
#define TRIM_DSP_FUNCTIONS 1
#define ENDIANNESS_BIG 0

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86 1
#define ARCH_X86_64 1
#define ARCH_X86_32 0
#elif defined(__i386__) || defined(_M_IX86)
#define ARCH_X86 1
#define ARCH_X86_64 0
#define ARCH_X86_32 1
#else
#define ARCH_X86 0
#define ARCH_X86_64 0
#define ARCH_X86_32 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_AARCH64 1
#define ARCH_ARM 0
#elif defined(__arm__) || defined(_M_ARM)
#define ARCH_AARCH64 0
#define ARCH_ARM 1
#else
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#endif

/* the asm: x86-64 (NASM), aarch64, armv7 (NEON); 32-bit x86 and wasm stay C */
#if defined(DAV1D_ASM) && (ARCH_X86_64 || ARCH_AARCH64 || ARCH_ARM)
#define HAVE_ASM 1
#else
#define HAVE_ASM 0
#endif

/* Mach-O symbols carry a leading underscore (asm.S's EXTERN; config.asm for NASM) */
#if defined(__APPLE__)
#define PREFIX 1
#endif

/* aarch64 assembler: .arch, and the .arch_extension directives and instructions of the
 * optional extensions (dotprod, i8mm, SVE, SVE2: their functions are dispatched at run time).
 * Clang 17 (Apple clang 16) and later have them all; with older toolchains and GNU as (whose
 * version the preprocessor cannot see) those functions are left out and NEON serves. .func is
 * debug information only: off. */
#define HAVE_AS_FUNC 0
#if ARCH_AARCH64
#define HAVE_AS_ARCH_DIRECTIVE 1
#if defined(__clang__) && ((defined(__apple_build_version__) && __clang_major__ >= 16) || \
                           (!defined(__apple_build_version__) && __clang_major__ >= 17))
#define AS_ARCH_LEVEL armv8.2-a
#define DAV1D_AARCH64_EXTENSIONS 1
#else
#define AS_ARCH_LEVEL armv8-a
#define DAV1D_AARCH64_EXTENSIONS 0
#endif
#else
#define HAVE_AS_ARCH_DIRECTIVE 0
#define DAV1D_AARCH64_EXTENSIONS 0
#endif
#define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE DAV1D_AARCH64_EXTENSIONS
#define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE DAV1D_AARCH64_EXTENSIONS
#define HAVE_AS_ARCHEXT_SVE_DIRECTIVE DAV1D_AARCH64_EXTENSIONS
#define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE DAV1D_AARCH64_EXTENSIONS
#define HAVE_DOTPROD DAV1D_AARCH64_EXTENSIONS
#define HAVE_I8MM DAV1D_AARCH64_EXTENSIONS
#define HAVE_SVE DAV1D_AARCH64_EXTENSIONS
#define HAVE_SVE2 DAV1D_AARCH64_EXTENSIONS

/* Emscripten (wasm32) and every other architecture: no arch-specific code */
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0

#if defined(_WIN32)
#define HAVE_UNISTD_H 0
#define HAVE_IO_H 1
#define HAVE_POSIX_MEMALIGN 0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#ifndef _CRT_DECLARE_NONSTDC_NAMES
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
#else
#define HAVE_UNISTD_H 1
#define HAVE_IO_H 0
#define HAVE_POSIX_MEMALIGN 1
#endif

#define HAVE_SYS_TYPES_H 1
#define HAVE_MEMALIGN 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_CLOCK_GETTIME 0
#define HAVE_SIGACTION 0
#define HAVE_DLSYM 0
/* thread names, affinity and CPU features are optional: off keeps one header for every libc */
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
/* the arm CPU features of Linux and Android (bionic has getauxval from API 18) */
#if defined(__linux__)
#define HAVE_GETAUXVAL 1
#else
#define HAVE_GETAUXVAL 0
#endif
#define HAVE_ELF_AUX_INFO 0

#if defined(_MSC_VER) && !defined(__clang__)
#define HAVE_C11_GENERIC 0
#else
#define HAVE_C11_GENERIC 1
#endif

#endif /* DAV1D_ENGINE_CONFIG_H */
