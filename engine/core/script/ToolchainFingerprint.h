// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The fill below (currentToolchainFingerprint) is adapted from OpenEngine's
// GE_FillToolchainFingerprint (Engine/Modules/NativeScripting/Include/NativeScripting/
// NativeScriptingABI.h): Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT.

#ifndef TOOLCHAIN_FINGERPRINT_H
#define TOOLCHAIN_FINGERPRINT_H

// What a C++ script library and the editor that loads it must agree on to share STL and
// engine types: compiler ABI family, standard library and its ABI, CRT flavour, the defines
// that change layouts. Everything here is plain integers, compiled into both sides
// (static inline / constexpr), so each binary records the toolchain it was really built
// with. A library carries it twice: as a text marker the editor reads from the file BEFORE
// loading it (no static initializer of a mismatched library ever runs), and as an export
// compared after loading.

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
// the standard library's identity macros (_GLIBCXX_RELEASE, _LIBCPP_VERSION,
// _MSVC_STL_UPDATE) come from its headers, <version> is the cheapest one defining them
#if __has_include(<version>)
#include <version>
#endif

#include "Export.h"

// The engine build this library is made for: the editor's version string (git describe:
// version tag, commit, dirty flag). The engine's TU gets it from CMake, the editor from
// EditorVersion.h, the generated script sources from the editor that writes them.
#ifndef JASPERENGINE_ABI_VERSION
#  ifdef JASPERENGINE_EDITOR_VERSION
#    define JASPERENGINE_ABI_VERSION JASPERENGINE_EDITOR_VERSION
#  else
#    define JASPERENGINE_ABI_VERSION "unknown"
#  endif
#endif

namespace jasperengine {

    struct ToolchainFingerprint {
        uint32_t format;
        uint32_t pointerBits;
        uint32_t arch;
        uint32_t compilerVendor;
        uint32_t compilerMajor;
        uint32_t compilerMinor;
        uint32_t stdlib;
        uint32_t stdlibVersion;
        uint32_t stdlibAbi;
        uint32_t crtDebug;
        uint32_t crtDll;
        uint32_t iteratorDebugLevel;
        uint32_t ndebug;
        uint32_t exceptions;
        uint32_t rtti;
        uint32_t nativeWchar;
        uint32_t charUnsigned;
        uint32_t cxxStandard;
        uint32_t engineAbiHash;
    };

    namespace toolchain {

        inline constexpr uint32_t FORMAT = 1;

        enum Arch : uint32_t { ArchUnknown = 0, ArchX86_64 = 1, ArchArm64 = 2, ArchX86 = 3, ArchArm = 4, ArchWasm = 5, ArchRiscV64 = 6 };
        enum Vendor : uint32_t { VendorUnknown = 0, VendorMSVC = 1, VendorClangCl = 2, VendorClang = 3, VendorAppleClang = 4, VendorGCC = 5, VendorMinGW = 6 };
        enum Stdlib : uint32_t { StdlibUnknown = 0, StdlibMSVC = 1, StdlibLibstdcxx = 2, StdlibLibcxx = 3 };

        // The fields in declaration order, as the marker names them
        inline constexpr const char* FIELD_NAMES[] = {
            "format", "pointerBits", "arch", "compilerVendor", "compilerMajor", "compilerMinor",
            "stdlib", "stdlibVersion", "stdlibAbi", "crtDebug", "crtDll", "iteratorDebugLevel",
            "ndebug", "exceptions", "rtti", "nativeWchar", "charUnsigned", "cxxStandard", "engineAbiHash"
        };
        inline constexpr size_t FIELD_COUNT = sizeof(FIELD_NAMES) / sizeof(FIELD_NAMES[0]);
        static_assert(sizeof(ToolchainFingerprint) == FIELD_COUNT * sizeof(uint32_t), "one name per field");

        constexpr uint32_t& field(ToolchainFingerprint& fp, size_t index){
            uint32_t* fields[] = {&fp.format, &fp.pointerBits, &fp.arch, &fp.compilerVendor, &fp.compilerMajor, &fp.compilerMinor,
                                  &fp.stdlib, &fp.stdlibVersion, &fp.stdlibAbi, &fp.crtDebug, &fp.crtDll, &fp.iteratorDebugLevel,
                                  &fp.ndebug, &fp.exceptions, &fp.rtti, &fp.nativeWchar, &fp.charUnsigned, &fp.cxxStandard, &fp.engineAbiHash};
            return *fields[index];
        }
        constexpr uint32_t field(const ToolchainFingerprint& fp, size_t index){
            ToolchainFingerprint copy = fp;
            return field(copy, index);
        }

        constexpr bool equalNames(const char* a, const char* b){
            for (; *a && *a == *b; a++, b++) {}
            return *a == *b;
        }

        // FIELD_COUNT when there is no such field
        constexpr size_t fieldIndex(const char* name){
            for (size_t i = 0; i < FIELD_COUNT; i++) {
                if (equalNames(FIELD_NAMES[i], name)) return i;
            }
            return FIELD_COUNT;
        }

        constexpr uint32_t fnv1a(const char* text, uint32_t hash = 2166136261u){
            for (; *text; text++) {
                hash ^= static_cast<uint8_t>(*text);
                hash *= 16777619u;
            }
            return hash;
        }

        // The engine build and the defines that change its layouts
        constexpr uint32_t engineAbiHash(){
            uint32_t hash = fnv1a(JASPERENGINE_ABI_VERSION);
#if defined(JASPERENGINE_PHYSICS_2D)
            hash = fnv1a("|PHYSICS_2D", hash);
#endif
#if defined(JASPERENGINE_PHYSICS_3D)
            hash = fnv1a("|PHYSICS_3D", hash);
#endif
#if defined(NO_THREAD_SUPPORT)
            hash = fnv1a("|NO_THREAD_SUPPORT", hash);
#endif
#if defined(DISABLE_LUA_BINDINGS)
            hash = fnv1a("|DISABLE_LUA_BINDINGS", hash);
#endif
#if defined(JASPERENGINE_SHARED)
            hash = fnv1a("|SHARED", hash);
#endif
            return hash;
        }

    }

    // The toolchain of the calling translation unit
    constexpr ToolchainFingerprint currentToolchainFingerprint(){
        using namespace toolchain;
        ToolchainFingerprint fp{};
        fp.format = FORMAT;
        fp.pointerBits = static_cast<uint32_t>(sizeof(void*) * 8);

#if defined(__x86_64__) || defined(_M_X64)
        fp.arch = ArchX86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
        fp.arch = ArchArm64;
#elif defined(__i386__) || defined(_M_IX86)
        fp.arch = ArchX86;
#elif defined(__arm__) || defined(_M_ARM)
        fp.arch = ArchArm;
#elif defined(__wasm__)
        fp.arch = ArchWasm;
#elif defined(__riscv) && __riscv_xlen == 64
        fp.arch = ArchRiscV64;
#endif

#if defined(__MINGW32__)
        fp.compilerVendor = VendorMinGW;
#elif defined(_MSC_VER) && defined(__clang__)
        fp.compilerVendor = VendorClangCl;
#elif defined(_MSC_VER)
        fp.compilerVendor = VendorMSVC;
#elif defined(__clang__) && defined(__apple_build_version__)
        fp.compilerVendor = VendorAppleClang;
#elif defined(__clang__)
        fp.compilerVendor = VendorClang;
#elif defined(__GNUC__)
        fp.compilerVendor = VendorGCC;
#endif

#if defined(__clang__)
        fp.compilerMajor = static_cast<uint32_t>(__clang_major__);
        fp.compilerMinor = static_cast<uint32_t>(__clang_minor__);
#elif defined(_MSC_VER)
        fp.compilerMajor = static_cast<uint32_t>(_MSC_VER);
        fp.compilerMinor = static_cast<uint32_t>(_MSC_FULL_VER % 100000);
#elif defined(__GNUC__)
        fp.compilerMajor = static_cast<uint32_t>(__GNUC__);
        fp.compilerMinor = static_cast<uint32_t>(__GNUC_MINOR__);
#endif

#if defined(_MSVC_STL_UPDATE)
        fp.stdlib = StdlibMSVC;
        fp.stdlibVersion = static_cast<uint32_t>(_MSVC_STL_UPDATE);
#elif defined(_LIBCPP_VERSION)
        fp.stdlib = StdlibLibcxx;
        fp.stdlibVersion = static_cast<uint32_t>(_LIBCPP_VERSION);
#  if defined(_LIBCPP_ABI_VERSION)
        fp.stdlibAbi = static_cast<uint32_t>(_LIBCPP_ABI_VERSION);
#  endif
#elif defined(_GLIBCXX_RELEASE)
        fp.stdlib = StdlibLibstdcxx;
        fp.stdlibVersion = static_cast<uint32_t>(_GLIBCXX_RELEASE);
#  if defined(_GLIBCXX_USE_CXX11_ABI)
        fp.stdlibAbi = static_cast<uint32_t>(_GLIBCXX_USE_CXX11_ABI);
#  endif
#endif

#if defined(_DEBUG)
        fp.crtDebug = 1;
#endif
#if defined(_DLL)
        fp.crtDll = 1;
#endif

        // _ITERATOR_DEBUG_LEVEL lives in MSVC's <yvals.h>, which <version> does not include:
        // without it (and no /D) MSVC's own default applies, 2 for the debug CRT, else 0
#if defined(_ITERATOR_DEBUG_LEVEL)
        fp.iteratorDebugLevel = static_cast<uint32_t>(_ITERATOR_DEBUG_LEVEL);
#elif defined(_MSC_VER) && defined(_DEBUG)
        fp.iteratorDebugLevel = 2;
#elif defined(_GLIBCXX_DEBUG)
        fp.iteratorDebugLevel = 1;
#endif

#if defined(NDEBUG)
        fp.ndebug = 1;
#endif
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
        fp.exceptions = 1;
#endif
#if defined(__cpp_rtti) || defined(_CPPRTTI) || defined(__GXX_RTTI)
        fp.rtti = 1;
#endif
        // wchar_t is a keyword everywhere but under MSVC's /Zc:wchar_t-
#if !defined(_MSC_VER) || defined(_NATIVE_WCHAR_T_DEFINED)
        fp.nativeWchar = 1;
#endif
#if defined(_CHAR_UNSIGNED) || defined(__CHAR_UNSIGNED__)
        fp.charUnsigned = 1;
#endif
#if defined(_MSVC_LANG)
        fp.cxxStandard = static_cast<uint32_t>(_MSVC_LANG);
#else
        fp.cxxStandard = static_cast<uint32_t>(__cplusplus);
#endif
        fp.engineAbiHash = toolchain::engineAbiHash();
        return fp;
    }

    static inline void fill(ToolchainFingerprint& out){
        out = currentToolchainFingerprint();
    }

    // ---- the marker: "JE_TOOLCHAIN_FP_V1{format=1;pointerBits=64;...;engineAbiHash=123}"

    namespace toolchain {
        inline constexpr char MARKER_PREFIX[] = "JE_TOOLCHAIN_FP_V1{";
        inline constexpr size_t MARKER_CAPACITY = 768;
    }

    struct ToolchainFingerprintMarker {
        char text[toolchain::MARKER_CAPACITY];
    };

    constexpr ToolchainFingerprintMarker makeToolchainMarker(const ToolchainFingerprint& fp){
        ToolchainFingerprintMarker marker{};
        size_t at = 0;
        auto put = [&](const char* text) {
            for (; *text && at + 1 < toolchain::MARKER_CAPACITY; text++) marker.text[at++] = *text;
        };
        auto putNumber = [&](uint32_t value) {
            char digits[11] = {};
            int count = 0;
            do {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            while (count > 0 && at + 1 < toolchain::MARKER_CAPACITY) marker.text[at++] = digits[--count];
        };
        put(toolchain::MARKER_PREFIX);
        for (size_t i = 0; i < toolchain::FIELD_COUNT; i++) {
            if (i > 0) put(";");
            put(toolchain::FIELD_NAMES[i]);
            put("=");
            putNumber(toolchain::field(fp, i));
        }
        put("}");
        return marker;
    }

    // A test hook (AC10 of the scripting spec): the generated script sources apply the
    // editor's JASPER_TEST_FINGERPRINT_OVERRIDE (field=value) to their own fingerprint
    constexpr ToolchainFingerprint withFingerprintOverride(ToolchainFingerprint fp, const char* name, uint32_t value){
        const size_t index = toolchain::fieldIndex(name);
        if (index < toolchain::FIELD_COUNT) toolchain::field(fp, index) = value;
        return fp;
    }

    // ---- checking a library against the editor

    struct FingerprintMismatch {
        const char* field;
        uint32_t editor;
        uint32_t library;
        bool hard;           // refused (else a warning, the library loads)
        std::string hint;    // what the difference means, e.g. "Debug STL vs Release editor"
    };

    // Every differing field. Hard: format, pointer size, arch, the compiler's ABI family
    // (MSVC and clang-cl vs Itanium vs MinGW), stdlib and its ABI, the CRT (debug, DLL),
    // the iterator debug level, NDEBUG, exceptions, RTTI, wchar_t, char signedness, the engine
    // ABI hash, and (MSVC STL only) a library STL newer than the editor's. Soft: the compiler
    // within its family and its version, an older or equal STL version, the C++ standard.
    JASPERENGINE_API std::vector<FingerprintMismatch> compare(const ToolchainFingerprint& editor, const ToolchainFingerprint& library);

    // Parses a marker text (MARKER_PREFIX ... "}"); nullopt unless every field is there
    JASPERENGINE_API std::optional<ToolchainFingerprint> parseMarker(std::string_view text);
    // The marker inside a binary, read in chunks without loading it: nullopt when there is
    // none (a library from before fingerprinting) or the file cannot be read
    JASPERENGINE_API std::optional<ToolchainFingerprint> scanMarker(const std::filesystem::path& library);

    JASPERENGINE_API std::string formatFingerprint(const ToolchainFingerprint& fp);
    JASPERENGINE_API const char* compilerVendorName(uint32_t vendor);

    struct ScriptLibraryCheck {
        bool ok = false;                      // may be loaded (no hard mismatch, or downgraded)
        bool markerFound = false;
        bool downgraded = false;              // JASPER_FINGERPRINT=warn turned refusals into warnings
        std::vector<FingerprintMismatch> mismatches;
        std::string message;                  // "Refused script library: ..." / the soft warning, empty when equal
    };
    // The check before loading a script library (Play, reload, the prebuilt player): its
    // marker against `host` (the loading binary's own fingerprint)
    JASPERENGINE_API ScriptLibraryCheck checkScriptLibrary(const std::filesystem::path& library, const ToolchainFingerprint& host);
    // The same rules for a fingerprint already read (the export of a loaded library)
    JASPERENGINE_API ScriptLibraryCheck checkFingerprint(const ToolchainFingerprint& library, const ToolchainFingerprint& host);

}

// The engine library's own fingerprint, filled in an engine translation unit
extern "C" JASPERENGINE_API const jasperengine::ToolchainFingerprint* jasperengine_engineFingerprint();

#if defined(_WIN32)
#  define JASPERENGINE_FINGERPRINT_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define JASPERENGINE_FINGERPRINT_EXPORT __attribute__((visibility("default"), used))
#else
#  define JASPERENGINE_FINGERPRINT_EXPORT
#endif

// Placed once in a script library's generated sources: the marker the editor scans before
// loading the file, and the export it compares after. FP is a constexpr ToolchainFingerprint.
// Both are exported, so no linker drops them.
#define JASPERENGINE_SCRIPT_TOOLCHAIN_FINGERPRINT(FP) \
    extern "C" JASPERENGINE_FINGERPRINT_EXPORT const jasperengine::ToolchainFingerprintMarker jasper_scriptToolchainMarker = \
        jasperengine::makeToolchainMarker(FP); \
    extern "C" JASPERENGINE_FINGERPRINT_EXPORT const jasperengine::ToolchainFingerprint* jasper_scriptToolchainFingerprint() { \
        static constexpr jasperengine::ToolchainFingerprint fingerprint = FP; \
        return &fingerprint; \
    }

#endif // TOOLCHAIN_FINGERPRINT_H
