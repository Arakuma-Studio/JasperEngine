// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What an executable or library is built for (docs/specs/S30-shipping.md R26, R30), from its
// headers: ELF e_machine, PE Machine, Mach-O cputype (every slice of a fat binary), the wasm
// magic; whether a Mach-O slice is code-signed; the newest GLIBC_x.y an ELF needs; the page
// alignment of its loadable segments. Portions adapted from OpenEngine
// Tools/Scripts/publish_game.py (MIT), binary_target().

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::publish {

    struct BinaryInfo {
        enum Format { Elf, Pe, MachO, Fat, Wasm, Unknown } format = Unknown;
        // x64, x86, arm64, arm (per slice of a fat binary)
        std::vector<std::string> arches;
        // every Mach-O slice has LC_CODE_SIGNATURE (an arm64 one without it does not run)
        bool signedMachO = false;
        // the arches of the Mach-O slices that are not signed
        std::vector<std::string> unsignedSlices;
        // the newest GLIBC_x.y version the ELF needs ("" when none)
        std::string glibcFloor;
        // the smallest PT_LOAD alignment of an ELF (Android: 16 KB pages need 16384)
        uint64_t minLoadAlignment = 0;
    };

    std::optional<BinaryInfo> inspectBinary(const std::filesystem::path& path, std::string* error);
    std::optional<BinaryInfo> inspectBinary(std::span<const uint8_t> bytes, std::string* error);
    const char* formatName(BinaryInfo::Format format);
    // "2.17" > "2.4": the newer of two GLIBC versions
    bool glibcNewer(const std::string& a, const std::string& b);

}
