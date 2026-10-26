// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The toolchain fingerprint of C++ script libraries (S23 R14-R16, R34): hard and soft fields,
// the marker read from the file before loading, and the messages naming each field.

#include "script/ToolchainFingerprint.h"
#include "support/AllocationCounter.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    const FingerprintMismatch* find(const std::vector<FingerprintMismatch>& mismatches, const std::string& field) {
        for (const FingerprintMismatch& mismatch : mismatches) {
            if (field == mismatch.field) return &mismatch;
        }
        return nullptr;
    }

    ToolchainFingerprint gccEditor() {
        ToolchainFingerprint fp = currentToolchainFingerprint();
        fp.compilerVendor = toolchain::VendorGCC;
        fp.compilerMajor = 13;
        fp.compilerMinor = 2;
        fp.stdlib = toolchain::StdlibLibstdcxx;
        fp.stdlibVersion = 13;
        fp.stdlibAbi = 1;
        // a Release editor, whatever this test binary is (the sanitizer builds keep assertions:
        // no NDEBUG): the tests below change these fields and expect them to differ
        fp.ndebug = 1;
        fp.iteratorDebugLevel = 0;
        fp.crtDebug = 0;
        return fp;
    }

    ToolchainFingerprint msvcEditor() {
        ToolchainFingerprint fp = currentToolchainFingerprint();
        fp.compilerVendor = toolchain::VendorMSVC;
        fp.compilerMajor = 1939;
        fp.compilerMinor = 33523;
        fp.stdlib = toolchain::StdlibMSVC;
        fp.stdlibVersion = 202402;
        fp.stdlibAbi = 0;
        return fp;
    }

    void writeFile(const std::filesystem::path& path, const std::string& prefix, size_t markerOffset, const std::string& marker, size_t total) {
        std::ofstream out(path, std::ios::binary);
        std::string chunk(1 << 20, '\x7f');
        size_t written = 0;
        out.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        written += prefix.size();
        while (written + chunk.size() <= markerOffset) {
            out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            written += chunk.size();
        }
        out.write(chunk.data(), static_cast<std::streamsize>(markerOffset - written));
        written = markerOffset;
        out.write(marker.data(), static_cast<std::streamsize>(marker.size()));
        written += marker.size();
        while (written + chunk.size() <= total) {
            out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            written += chunk.size();
        }
        if (total > written) out.write(chunk.data(), static_cast<std::streamsize>(total - written));
    }

}

TEST(ToolchainFingerprint, EqualHasNoMismatch) {
    EXPECT_TRUE(compare(gccEditor(), gccEditor()).empty());
    const ScriptLibraryCheck check = checkFingerprint(gccEditor(), gccEditor());
    EXPECT_TRUE(check.ok);
    EXPECT_TRUE(check.message.empty());
}

TEST(ToolchainFingerprint, CompareNamesEachDifferingField) {
    ToolchainFingerprint library = gccEditor();
    library.iteratorDebugLevel = 2;
    library.ndebug = 0;
    library.compilerMinor = 3;
    const std::vector<FingerprintMismatch> mismatches = compare(gccEditor(), library);
    ASSERT_EQ(mismatches.size(), 3u);
    ASSERT_NE(find(mismatches, "iteratorDebugLevel"), nullptr);
    EXPECT_TRUE(find(mismatches, "iteratorDebugLevel")->hard);
    EXPECT_EQ(find(mismatches, "iteratorDebugLevel")->library, 2u);
    EXPECT_EQ(find(mismatches, "iteratorDebugLevel")->editor, 0u);
    EXPECT_TRUE(find(mismatches, "ndebug")->hard);
    EXPECT_FALSE(find(mismatches, "compilerMinor")->hard) << "a GCC minor version is soft";

    const ScriptLibraryCheck check = checkFingerprint(library, gccEditor());
    EXPECT_FALSE(check.ok);
    EXPECT_NE(check.message.find("Refused script library: "), std::string::npos) << check.message;
    EXPECT_NE(check.message.find("iteratorDebugLevel library=2 editor=0 (Debug STL vs Release editor)"), std::string::npos) << check.message;
    EXPECT_NE(check.message.find("ndebug"), std::string::npos);
    EXPECT_NE(check.message.find("Rebuild with the editor's configuration"), std::string::npos);
}

TEST(ToolchainFingerprint, HardFields) {
    const char* hardFields[] = {"format", "pointerBits", "arch", "stdlib", "stdlibAbi", "crtDebug", "crtDll",
                                "iteratorDebugLevel", "ndebug", "exceptions", "rtti", "nativeWchar", "charUnsigned", "engineAbiHash"};
    for (const char* field : hardFields) {
        ToolchainFingerprint library = gccEditor();
        toolchain::field(library, toolchain::fieldIndex(field)) += 1;
        const std::vector<FingerprintMismatch> mismatches = compare(gccEditor(), library);
        ASSERT_EQ(mismatches.size(), 1u) << field;
        EXPECT_TRUE(mismatches[0].hard) << field;
        EXPECT_FALSE(checkFingerprint(library, gccEditor()).ok) << field;
    }
}

TEST(ToolchainFingerprint, SoftFields) {
    for (const char* field : {"compilerMajor", "compilerMinor", "cxxStandard"}) {
        ToolchainFingerprint library = gccEditor();
        toolchain::field(library, toolchain::fieldIndex(field)) += 1;
        const ScriptLibraryCheck check = checkFingerprint(library, gccEditor());
        EXPECT_TRUE(check.ok) << field;
        EXPECT_NE(check.message.find(field), std::string::npos) << check.message;
    }
    // libstdc++ is backward compatible both ways for our use: a newer one is soft too
    ToolchainFingerprint newer = gccEditor();
    newer.stdlibVersion = 14;
    EXPECT_TRUE(checkFingerprint(newer, gccEditor()).ok);
}

TEST(ToolchainFingerprint, VendorFamilies) {
    ToolchainFingerprint clang = gccEditor();
    clang.compilerVendor = toolchain::VendorClang;
    std::vector<FingerprintMismatch> mismatches = compare(gccEditor(), clang);
    ASSERT_EQ(mismatches.size(), 1u);
    EXPECT_FALSE(mismatches[0].hard) << "Clang and GCC share the Itanium ABI";

    ToolchainFingerprint clangCl = msvcEditor();
    clangCl.compilerVendor = toolchain::VendorClangCl;
    mismatches = compare(msvcEditor(), clangCl);
    ASSERT_EQ(mismatches.size(), 1u);
    EXPECT_FALSE(mismatches[0].hard) << "clang-cl shares MSVC's ABI";

    ToolchainFingerprint mingw = msvcEditor();
    mingw.compilerVendor = toolchain::VendorMinGW;
    mismatches = compare(msvcEditor(), mingw);
    ASSERT_NE(find(mismatches, "compilerVendor"), nullptr);
    EXPECT_TRUE(find(mismatches, "compilerVendor")->hard) << "MinGW vs MSVC";
    EXPECT_NE(find(mismatches, "compilerVendor")->hint.find("MinGW"), std::string::npos);
}

TEST(ToolchainFingerprint, MsvcNewerStlIsHard) {
    ToolchainFingerprint newer = msvcEditor();
    newer.stdlibVersion += 1;
    std::vector<FingerprintMismatch> mismatches = compare(msvcEditor(), newer);
    ASSERT_EQ(mismatches.size(), 1u);
    EXPECT_TRUE(mismatches[0].hard);

    ToolchainFingerprint older = msvcEditor();
    older.stdlibVersion -= 1;
    mismatches = compare(msvcEditor(), older);
    ASSERT_EQ(mismatches.size(), 1u);
    EXPECT_FALSE(mismatches[0].hard) << "an older or equal MSVC STL loads";
}

TEST(ToolchainFingerprint, MarkerRoundTrip) {
    const ToolchainFingerprint fp = gccEditor();
    const ToolchainFingerprintMarker marker = makeToolchainMarker(fp);
    const std::string text(marker.text);
    EXPECT_EQ(text.rfind("JE_TOOLCHAIN_FP_V1{format=1;pointerBits=", 0), 0u) << text;
    const std::optional<ToolchainFingerprint> parsed = parseMarker(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(compare(fp, *parsed).empty());
    EXPECT_FALSE(parseMarker("JE_TOOLCHAIN_FP_V1{format=1}").has_value()) << "every field is required";
    EXPECT_FALSE(parseMarker("JE_TOOLCHAIN_FP_V1{format=x").has_value());
}

TEST(ToolchainFingerprint, OverrideChangesOneField) {
    constexpr ToolchainFingerprint overridden = withFingerprintOverride(currentToolchainFingerprint(), "iteratorDebugLevel", 2);
    EXPECT_EQ(overridden.iteratorDebugLevel, 2u);
    const ToolchainFingerprint same = withFingerprintOverride(currentToolchainFingerprint(), "noSuchField", 7);
    EXPECT_TRUE(compare(currentToolchainFingerprint(), same).empty());
}

TEST(ToolchainFingerprint, EngineAgreesWithThisBuild) {
    // the test binary links the engine it was built with (same toolchain, same defines)
    const ToolchainFingerprint* engine = jasperengine_engineFingerprint();
    ASSERT_NE(engine, nullptr);
    ToolchainFingerprint mine = currentToolchainFingerprint();
    // the engine's translation unit names the editor version; the test's may not
    mine.engineAbiHash = engine->engineAbiHash;
    EXPECT_TRUE(compare(mine, *engine).empty()) << formatFingerprint(*engine);
}

TEST(ToolchainFingerprint, ScanMarkerAt50MegabytesWithinBudget) {
    test::TempDir dir;
    const std::filesystem::path library = dir.path() / "libscripts.so";
    const ToolchainFingerprint fp = gccEditor();
    // a decoy prefix of the marker before the real one
    writeFile(library, "JE_TOOLCHAIN_FP_V1{broken", 50u << 20, makeToolchainMarker(fp).text, 64u << 20);

    const auto start = std::chrono::steady_clock::now();
    const std::optional<ToolchainFingerprint> found = scanMarker(library);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(compare(fp, *found).empty());
    // R34: <= 20 ms for 64 MB (the file is in the page cache: written just now)
    std::printf("[bench] fingerprint marker scan of 64 MB: %.2f ms (budget 20)\n", ms);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ms, 20.0 * testsupport::perfSlack());
    }

    // a marker cut by the 8 MB chunk boundary is still found
    const std::filesystem::path straddling = dir.path() / "straddling.so";
    writeFile(straddling, "", (8u << 20) - 20, makeToolchainMarker(fp).text, 9u << 20);
    EXPECT_TRUE(scanMarker(straddling).has_value());
}

TEST(ToolchainFingerprint, MissingMarkerIsRefused) {
    test::TempDir dir;
    const std::filesystem::path library = dir.path() / "old.so";
    writeFile(library, "", 1024, "no marker here", 4096);
    const ScriptLibraryCheck check = checkScriptLibrary(library, currentToolchainFingerprint());
    EXPECT_FALSE(check.ok);
    EXPECT_FALSE(check.markerFound);
    EXPECT_NE(check.message.find("library predates fingerprinting, rebuild"), std::string::npos) << check.message;

    const ScriptLibraryCheck missing = checkScriptLibrary(dir.path() / "absent.so", currentToolchainFingerprint());
    EXPECT_FALSE(missing.ok);
}

TEST(ToolchainFingerprint, WarnModeDowngradesRefusals) {
    ToolchainFingerprint library = gccEditor();
    library.rtti = 0;
#if defined(_WIN32)
    _putenv_s("JASPER_FINGERPRINT", "warn");
#else
    setenv("JASPER_FINGERPRINT", "warn", 1);
#endif
    const ScriptLibraryCheck check = checkFingerprint(library, gccEditor());
#if defined(_WIN32)
    _putenv_s("JASPER_FINGERPRINT", "");
#else
    unsetenv("JASPER_FINGERPRINT");
#endif
    EXPECT_TRUE(check.ok);
    EXPECT_TRUE(check.downgraded);
    EXPECT_NE(check.message.find("JASPER_FINGERPRINT=warn"), std::string::npos);
}
