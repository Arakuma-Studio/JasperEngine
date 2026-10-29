// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine Tools/Scripts/publish_game.py (MIT):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors

#include "BinaryInfo.h"

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace jasperengine::editor;
using namespace jasperengine::editor::publish;

namespace {

    struct Bytes {
        std::span<const uint8_t> data;
        bool little = true;
        bool has(uint64_t at, uint64_t size) const { return at <= data.size() && size <= data.size() - at; }
        uint16_t u16(uint64_t at) const {
            if (!has(at, 2)) return 0;
            const uint8_t* p = data.data() + at;
            return little ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]);
        }
        uint32_t u32(uint64_t at) const {
            if (!has(at, 4)) return 0;
            const uint8_t* p = data.data() + at;
            return little ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
                          : ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        uint64_t u64(uint64_t at) const {
            const uint64_t a = u32(at), b = u32(at + 4);
            return little ? a | (b << 32) : (a << 32) | b;
        }
    };

    std::string elfArch(uint16_t machine) {
        switch (machine) {
            case 0x3E: return "x64";
            case 0x03: return "x86";
            case 0xB7: return "arm64";
            case 0x28: return "arm";
            default: return "elf-machine-" + std::to_string(machine);
        }
    }

    std::string machArch(uint32_t cputype) {
        switch (cputype) {
            case 0x01000007: return "x64";
            case 0x00000007: return "x86";
            case 0x0100000C: return "arm64";
            case 0x0000000C: return "arm";
            default: return "mach-cpu-" + std::to_string(cputype);
        }
    }

    // a GLIBC_x.y name newer than the floor so far
    void noteGlibc(const std::string& name, std::string& floor) {
        if (name.rfind("GLIBC_", 0) != 0) return;
        const std::string version = name.substr(6);
        if (version.empty() || !std::isdigit(static_cast<unsigned char>(version[0]))) return;
        if (floor.empty() || glibcNewer(version, floor)) floor = version;
    }

    void inspectElf(const Bytes& b, BinaryInfo& info) {
        const bool is64 = b.data[4] == 2;
        Bytes e{b.data, b.data[5] == 1};
        info.format = BinaryInfo::Elf;
        info.arches.push_back(elfArch(e.u16(18)));
        const uint64_t phoff = is64 ? e.u64(32) : e.u32(28);
        const uint64_t shoff = is64 ? e.u64(40) : e.u32(32);
        const uint16_t phentsize = e.u16(is64 ? 54 : 42), phnum = e.u16(is64 ? 56 : 44);
        const uint16_t shentsize = e.u16(is64 ? 58 : 46), shnum = e.u16(is64 ? 60 : 48);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint64_t ph = phoff + (uint64_t)i * phentsize;
            if (!e.has(ph, phentsize)) break;
            if (e.u32(ph) != 1) continue;   // PT_LOAD
            const uint64_t align = is64 ? e.u64(ph + 48) : e.u32(ph + 28);
            if (info.minLoadAlignment == 0 || align < info.minLoadAlignment) info.minLoadAlignment = align;
        }
        // .gnu.version_r (SHT_GNU_verneed) names the versions it needs, in its linked string table
        struct Section { uint32_t type; uint64_t offset, size; uint32_t link; };
        std::vector<Section> sections;
        for (uint16_t i = 0; i < shnum; i++) {
            const uint64_t sh = shoff + (uint64_t)i * shentsize;
            if (!e.has(sh, shentsize)) break;
            Section s;
            s.type = e.u32(sh + 4);
            s.offset = is64 ? e.u64(sh + 24) : e.u32(sh + 16);
            s.size = is64 ? e.u64(sh + 32) : e.u32(sh + 20);
            s.link = e.u32(sh + (is64 ? 40 : 24));
            sections.push_back(s);
        }
        for (const Section& s : sections) {
            if (s.type != 0x6ffffffe || s.link >= sections.size()) continue;
            const Section& strings = sections[s.link];
            uint64_t at = s.offset;
            for (int guard = 0; guard < 4096 && e.has(at, 16); guard++) {
                const uint16_t count = e.u16(at + 2);
                const uint32_t aux = e.u32(at + 8), next = e.u32(at + 12);
                uint64_t a = at + aux;
                for (uint16_t k = 0; k < count && e.has(a, 16); k++) {
                    const uint32_t nameOffset = e.u32(a + 8);
                    const uint32_t nextAux = e.u32(a + 12);
                    if (e.has(strings.offset + nameOffset, 1)) {
                        const char* name = reinterpret_cast<const char*>(e.data.data() + strings.offset + nameOffset);
                        const size_t max = static_cast<size_t>(std::min<uint64_t>(strings.size - std::min<uint64_t>(nameOffset, strings.size), 64));
                        noteGlibc(std::string(name, strnlen(name, max)), info.glibcFloor);
                    }
                    if (nextAux == 0) break;
                    a += nextAux;
                }
                if (next == 0) break;
                at += next;
            }
        }
    }

    // one Mach-O image at `base`; its arch, and whether it has LC_CODE_SIGNATURE
    bool inspectMachSlice(const Bytes& whole, uint64_t base, std::string& arch, bool& isSigned) {
        const uint32_t magic = Bytes{whole.data, true}.u32(base);
        bool little = true, is64 = false;
        if (magic == 0xFEEDFACE) is64 = false;
        else if (magic == 0xFEEDFACF) is64 = true;
        else if (magic == 0xCEFAEDFE) { little = false; is64 = false; }
        else if (magic == 0xCFFAEDFE) { little = false; is64 = true; }
        else return false;
        Bytes m{whole.data, little};
        arch = machArch(m.u32(base + 4));
        const uint32_t ncmds = m.u32(base + 16);
        uint64_t at = base + (is64 ? 32 : 28);
        isSigned = false;
        for (uint32_t i = 0; i < ncmds && m.has(at, 8); i++) {
            const uint32_t cmd = m.u32(at), size = m.u32(at + 4);
            if (cmd == 0x1D) isSigned = true;   // LC_CODE_SIGNATURE
            if (size < 8) break;
            at += size;
        }
        return true;
    }

}

const char* publish::formatName(BinaryInfo::Format format) {
    switch (format) {
        case BinaryInfo::Elf: return "ELF";
        case BinaryInfo::Pe: return "PE";
        case BinaryInfo::MachO: return "Mach-O";
        case BinaryInfo::Fat: return "universal Mach-O";
        case BinaryInfo::Wasm: return "WebAssembly";
        case BinaryInfo::Unknown: return "unknown";
    }
    return "unknown";
}

bool publish::glibcNewer(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        unsigned long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + static_cast<unsigned long>(a[i++] - '0');
        while (j < b.size() && b[j] != '.') y = y * 10 + static_cast<unsigned long>(b[j++] - '0');
        if (x != y) return x > y;
        if (i < a.size()) i++;
        if (j < b.size()) j++;
    }
    return false;
}

std::optional<BinaryInfo> publish::inspectBinary(std::span<const uint8_t> data, std::string* error) {
    BinaryInfo info;
    Bytes b{data, true};
    if (data.size() >= 4 && std::memcmp(data.data(), "\0asm", 4) == 0) {
        info.format = BinaryInfo::Wasm;
        info.arches = {"wasm32"};
        return info;
    }
    if (data.size() >= 20 && std::memcmp(data.data(), "\x7f" "ELF", 4) == 0) {
        inspectElf(b, info);
        return info;
    }
    if (data.size() >= 64 && data[0] == 'M' && data[1] == 'Z') {
        const uint32_t pe = b.u32(0x3C);
        if (b.has(pe, 6) && std::memcmp(data.data() + pe, "PE\0\0", 4) == 0) {
            info.format = BinaryInfo::Pe;
            const uint16_t machine = b.u16(pe + 4);
            info.arches.push_back(machine == 0x8664 ? "x64" : machine == 0x14c ? "x86" : machine == 0xAA64 ? "arm64"
                                                            : "pe-machine-" + std::to_string(machine));
            return info;
        }
    }
    const uint32_t big = Bytes{data, false}.u32(0);
    if (big == 0xCAFEBABE || big == 0xCAFEBABF) {
        // a universal binary: big-endian fat header, then each slice (fat_arch or fat_arch_64)
        const bool fat64 = big == 0xCAFEBABF;
        Bytes f{data, false};
        const uint32_t count = f.u32(4);
        if (count == 0 || count > 32) {
            if (error) *error = "corrupt universal binary header";
            return std::nullopt;
        }
        info.format = BinaryInfo::Fat;
        info.signedMachO = true;
        for (uint32_t i = 0; i < count; i++) {
            const uint64_t entry = 8 + (uint64_t)i * (fat64 ? 32 : 20);
            const uint64_t offset = fat64 ? f.u64(entry + 8) : f.u32(entry + 8);
            std::string arch;
            bool isSigned = false;
            if (!inspectMachSlice(b, offset, arch, isSigned)) {
                if (error) *error = "slice " + std::to_string(i) + " is not a Mach-O image";
                return std::nullopt;
            }
            info.arches.push_back(arch);
            if (!isSigned) {
                info.signedMachO = false;
                info.unsignedSlices.push_back(arch);
            }
        }
        return info;
    }
    std::string arch;
    bool isSigned = false;
    if (inspectMachSlice(b, 0, arch, isSigned)) {
        info.format = BinaryInfo::MachO;
        info.arches.push_back(arch);
        info.signedMachO = isSigned;
        if (!isSigned) info.unsignedSlices.push_back(arch);
        return info;
    }
    info.format = BinaryInfo::Unknown;
    return info;
}

std::optional<BinaryInfo> publish::inspectBinary(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read " + path.string();
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return inspectBinary(bytes, error);
}
