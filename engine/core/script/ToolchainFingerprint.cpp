// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ToolchainFingerprint.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>

using namespace jasperengine;

namespace {

    // The ABI families a compiler belongs to: what decides whether two of them can share C++
    enum class AbiFamily { Unknown, Msvc, MinGW, Itanium };

    AbiFamily abiFamily(uint32_t vendor){
        switch (vendor) {
            case toolchain::VendorMSVC:
            case toolchain::VendorClangCl:
                return AbiFamily::Msvc;
            case toolchain::VendorMinGW:
                return AbiFamily::MinGW;
            case toolchain::VendorClang:
            case toolchain::VendorAppleClang:
            case toolchain::VendorGCC:
                return AbiFamily::Itanium;
            default:
                return AbiFamily::Unknown;
        }
    }

    const char* abiFamilyName(AbiFamily family){
        switch (family) {
            case AbiFamily::Msvc: return "MSVC ABI";
            case AbiFamily::MinGW: return "MinGW";
            case AbiFamily::Itanium: return "Itanium C++ ABI";
            default: return "unknown ABI";
        }
    }

    const char* archName(uint32_t arch){
        switch (arch) {
            case toolchain::ArchX86_64: return "x86_64";
            case toolchain::ArchArm64: return "arm64";
            case toolchain::ArchX86: return "x86";
            case toolchain::ArchArm: return "arm";
            case toolchain::ArchWasm: return "wasm";
            case toolchain::ArchRiscV64: return "riscv64";
            default: return "unknown";
        }
    }

    const char* stdlibName(uint32_t stdlib){
        switch (stdlib) {
            case toolchain::StdlibMSVC: return "MSVC STL";
            case toolchain::StdlibLibstdcxx: return "libstdc++";
            case toolchain::StdlibLibcxx: return "libc++";
            default: return "unknown standard library";
        }
    }

    std::string onOff(uint32_t value, const char* what){
        return std::string(what) + (value ? " on" : " off");
    }

    std::string versus(const std::string& library, const std::string& editor){
        return library + " vs " + editor + " editor";
    }

    void add(std::vector<FingerprintMismatch>& out, size_t index, const ToolchainFingerprint& editor,
             const ToolchainFingerprint& library, bool hard, std::string hint){
        out.push_back({toolchain::FIELD_NAMES[index], toolchain::field(editor, index), toolchain::field(library, index), hard, std::move(hint)});
    }

    std::string hintFor(const std::string& field, const ToolchainFingerprint& editor, const ToolchainFingerprint& library){
        if (field == "format") return "fingerprint format";
        if (field == "pointerBits") return versus(std::to_string(library.pointerBits) + "-bit", std::to_string(editor.pointerBits) + "-bit");
        if (field == "arch") return versus(archName(library.arch), archName(editor.arch));
        if (field == "compilerVendor") return versus(compilerVendorName(library.compilerVendor), compilerVendorName(editor.compilerVendor));
        if (field == "compilerMajor" || field == "compilerMinor") return "another compiler version";
        if (field == "stdlib") return versus(stdlibName(library.stdlib), stdlibName(editor.stdlib));
        if (field == "stdlibVersion") {
            return library.stdlibVersion > editor.stdlibVersion ? "the library's standard library is newer than the editor's"
                                                                : "the library's standard library is older than the editor's";
        }
        if (field == "stdlibAbi") {
            if (library.stdlib == toolchain::StdlibLibstdcxx) return "_GLIBCXX_USE_CXX11_ABI differs (old vs new std::string)";
            return "another standard library ABI version";
        }
        if (field == "crtDebug") return versus(library.crtDebug ? "Debug CRT" : "Release CRT", editor.crtDebug ? "Debug" : "Release");
        if (field == "crtDll") return versus(library.crtDll ? "DLL CRT" : "static CRT", editor.crtDll ? "DLL CRT" : "static CRT");
        if (field == "iteratorDebugLevel") return versus(library.iteratorDebugLevel ? "Debug STL" : "Release STL", editor.iteratorDebugLevel ? "Debug STL" : "Release");
        if (field == "ndebug") return versus(library.ndebug ? "NDEBUG build" : "assertions build", editor.ndebug ? "NDEBUG" : "assertions");
        if (field == "exceptions") return versus(onOff(library.exceptions, "exceptions"), onOff(editor.exceptions, "exceptions"));
        if (field == "rtti") return versus(onOff(library.rtti, "RTTI"), onOff(editor.rtti, "RTTI"));
        if (field == "nativeWchar") return "/Zc:wchar_t differs";
        if (field == "charUnsigned") return versus(library.charUnsigned ? "unsigned char" : "signed char", editor.charUnsigned ? "unsigned char" : "signed char");
        if (field == "cxxStandard") return "another C++ standard";
        if (field == "engineAbiHash") return "built for another engine build (editor version or engine defines)";
        return std::string();
    }

    std::string describe(const std::vector<FingerprintMismatch>& mismatches, bool hard){
        std::string text;
        for (const FingerprintMismatch& mismatch : mismatches) {
            if (mismatch.hard != hard) continue;
            if (!text.empty()) text += "; ";
            text += std::string(mismatch.field) + " library=" + std::to_string(mismatch.library) +
                    " editor=" + std::to_string(mismatch.editor);
            if (!mismatch.hint.empty()) text += " (" + mismatch.hint + ")";
        }
        return text;
    }

    // JASPER_FINGERPRINT=warn: refusals become warnings (support cases only)
    bool refusalsDowngraded(){
        const char* mode = std::getenv("JASPER_FINGERPRINT");
        return mode && std::strcmp(mode, "warn") == 0;
    }

}

const char* jasperengine::compilerVendorName(uint32_t vendor){
    switch (vendor) {
        case toolchain::VendorMSVC: return "MSVC";
        case toolchain::VendorClangCl: return "clang-cl";
        case toolchain::VendorClang: return "Clang";
        case toolchain::VendorAppleClang: return "AppleClang";
        case toolchain::VendorGCC: return "GCC";
        case toolchain::VendorMinGW: return "MinGW";
        default: return "unknown compiler";
    }
}

std::vector<FingerprintMismatch> jasperengine::compare(const ToolchainFingerprint& editor, const ToolchainFingerprint& library){
    std::vector<FingerprintMismatch> out;
    for (size_t i = 0; i < toolchain::FIELD_COUNT; i++) {
        const uint32_t editorValue = toolchain::field(editor, i);
        const uint32_t libraryValue = toolchain::field(library, i);
        if (editorValue == libraryValue) continue;

        const std::string name = toolchain::FIELD_NAMES[i];
        bool hard = true;
        if (name == "compilerVendor") {
            // within one ABI family (GCC and Clang on libstdc++, MSVC and clang-cl) only a warning
            const AbiFamily editorFamily = abiFamily(editorValue);
            const AbiFamily libraryFamily = abiFamily(libraryValue);
            hard = editorFamily != libraryFamily || editorFamily == AbiFamily::Unknown;
            if (hard) {
                out.push_back({toolchain::FIELD_NAMES[i], editorValue, libraryValue, true,
                               versus(abiFamilyName(libraryFamily), abiFamilyName(editorFamily)) + std::string(" (") +
                               compilerVendorName(libraryValue) + " vs " + compilerVendorName(editorValue) + ")"});
                continue;
            }
        } else if (name == "compilerMajor" || name == "compilerMinor" || name == "cxxStandard") {
            hard = false;
        } else if (name == "stdlibVersion") {
            // the MSVC STL is forward compatible only: a newer library can call into an
            // older msvcp140 what it does not have
            hard = editor.stdlib == toolchain::StdlibMSVC && library.stdlib == toolchain::StdlibMSVC && libraryValue > editorValue;
        }
        add(out, i, editor, library, hard, hintFor(name, editor, library));
    }
    return out;
}

std::optional<ToolchainFingerprint> jasperengine::parseMarker(std::string_view text){
    const std::string_view prefix(toolchain::MARKER_PREFIX);
    if (text.substr(0, prefix.size()) != prefix) return std::nullopt;
    const size_t end = text.find('}', prefix.size());
    if (end == std::string_view::npos) return std::nullopt;
    std::string_view body = text.substr(prefix.size(), end - prefix.size());

    ToolchainFingerprint fp{};
    bool seen[toolchain::FIELD_COUNT] = {};
    while (!body.empty()) {
        const size_t separator = body.find(';');
        const std::string_view item = body.substr(0, separator);
        body = separator == std::string_view::npos ? std::string_view() : body.substr(separator + 1);

        const size_t equals = item.find('=');
        if (equals == std::string_view::npos || equals + 1 >= item.size()) return std::nullopt;
        const std::string name(item.substr(0, equals));
        const std::string_view digits = item.substr(equals + 1);
        uint64_t value = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(c - '0');
            if (value > 0xffffffffull) return std::nullopt;
        }
        const size_t index = toolchain::fieldIndex(name.c_str());
        // a field of a newer format is not ours to judge: the format field decides
        if (index == toolchain::FIELD_COUNT) continue;
        toolchain::field(fp, index) = static_cast<uint32_t>(value);
        seen[index] = true;
    }
    for (bool present : seen) {
        if (!present) return std::nullopt;
    }
    return fp;
}

std::optional<ToolchainFingerprint> jasperengine::scanMarker(const std::filesystem::path& library){
    std::unique_ptr<FILE, int (*)(FILE*)> file(
#if defined(_WIN32)
        _wfopen(library.c_str(), L"rb"),
#else
        std::fopen(library.c_str(), "rb"),
#endif
        &std::fclose);
    if (!file) return std::nullopt;

    // Chunks overlap by a whole marker, so one cut by a chunk end is seen whole in the next
    constexpr size_t CHUNK = 8u << 20;
    constexpr size_t OVERLAP = toolchain::MARKER_CAPACITY;
    const std::string_view prefix(toolchain::MARKER_PREFIX);
    const std::boyer_moore_horspool_searcher searcher(prefix.begin(), prefix.end());

    std::vector<char> buffer(CHUNK + OVERLAP);
    size_t kept = 0;
    for (;;) {
        const size_t read = std::fread(buffer.data() + kept, 1, CHUNK, file.get());
        const size_t size = kept + read;
        const bool last = read < CHUNK;

        auto it = buffer.begin();
        const auto end = buffer.begin() + static_cast<std::ptrdiff_t>(size);
        while (true) {
            it = std::search(it, end, searcher);
            if (it == end) break;
            const size_t at = static_cast<size_t>(it - buffer.begin());
            const size_t available = std::min(size - at, OVERLAP);
            // a marker cut at the chunk end is parsed in the next chunk
            if (!last && size - at < OVERLAP) break;
            if (auto fp = parseMarker(std::string_view(buffer.data() + at, available))) return fp;
            ++it;
        }
        if (last) break;

        kept = std::min(size, OVERLAP);
        std::memmove(buffer.data(), buffer.data() + size - kept, kept);
    }
    return std::nullopt;
}

std::string jasperengine::formatFingerprint(const ToolchainFingerprint& fp){
    std::string text;
    for (size_t i = 0; i < toolchain::FIELD_COUNT; i++) {
        if (i > 0) text += ' ';
        text += std::string(toolchain::FIELD_NAMES[i]) + "=" + std::to_string(toolchain::field(fp, i));
    }
    return text;
}

ScriptLibraryCheck jasperengine::checkFingerprint(const ToolchainFingerprint& library, const ToolchainFingerprint& host){
    ScriptLibraryCheck check;
    check.markerFound = true;
    check.mismatches = compare(host, library);

    const bool anyHard = std::any_of(check.mismatches.begin(), check.mismatches.end(),
                                     [](const FingerprintMismatch& m) { return m.hard; });
    const std::string soft = describe(check.mismatches, false);
    if (anyHard) {
        check.downgraded = refusalsDowngraded();
        check.ok = check.downgraded;
        check.message = std::string(check.downgraded ? "Loading an incompatible script library (JASPER_FINGERPRINT=warn): "
                                                     : "Refused script library: ") +
                        describe(check.mismatches, true) +
                        ". Rebuild with the editor's configuration (Preferences \xE2\x96\xB8 External Tools)";
        if (!soft.empty()) check.message += "; also " + soft;
    } else {
        check.ok = true;
        if (!soft.empty()) check.message = "Script library built with another toolchain version: " + soft;
    }
    return check;
}

ScriptLibraryCheck jasperengine::checkScriptLibrary(const std::filesystem::path& library, const ToolchainFingerprint& host){
    std::error_code ec;
    if (!std::filesystem::is_regular_file(library, ec)) {
        ScriptLibraryCheck check;
        check.message = "Refused script library: " + library.generic_string() + " not found";
        return check;
    }
    const std::optional<ToolchainFingerprint> marker = scanMarker(library);
    if (!marker) {
        ScriptLibraryCheck check;
        check.downgraded = refusalsDowngraded();
        check.ok = check.downgraded;
        check.message = std::string(check.downgraded ? "Loading a script library without a toolchain fingerprint (JASPER_FINGERPRINT=warn): "
                                                     : "Refused script library: ") +
                        "the library predates fingerprinting, rebuild it (" + library.filename().generic_string() + ")";
        return check;
    }
    return checkFingerprint(*marker, host);
}

extern "C" const jasperengine::ToolchainFingerprint* jasperengine_engineFingerprint(){
    static constexpr ToolchainFingerprint fingerprint = currentToolchainFingerprint();
    return &fingerprint;
}
