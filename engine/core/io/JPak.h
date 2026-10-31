// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// .jpak v1 content packs (docs/specs/S30-shipping.md D6): what a player reads its content from
// when the build packed it (Build Settings > Pack Content, always on the web).
//
//   header   "JPAK\0\0\0\0", u32 version (1), u32 entry count, u64 index bytes   (24 bytes)
//   index    per entry: u32 path length, path (UTF-8, '/'-separated), u64 offset, u64 size,
//            u64 FNV-1a 64 of the bytes
//   blobs    each at a 16-byte aligned offset (from the start of the file)
//
// Little endian. Paths follow the legal-path rule: relative, no "..", "." or empty parts, no
// '\\', unique when case-folded. The reader works on a file (positional reads) or on a span of
// memory (the web player's fetched pack, read in place); every entry read is checked against its
// FNV-1a 64. The editor writes packs (editor/cook/JPakWriter); tools/jpak.py inspects them.

#include "Export.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API JPak {
    public:
        static constexpr char MAGIC[8] = {'J', 'P', 'A', 'K', 0, 0, 0, 0};
        static constexpr uint32_t VERSION = 1;
        static constexpr size_t HEADER_BYTES = 24;
        static constexpr uint64_t ALIGNMENT = 16;

        struct Entry {
            std::string path;
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t fnv1a64 = 0;
        };

        JPak();
        ~JPak();
        JPak(const JPak&) = delete;
        JPak& operator=(const JPak&) = delete;

        // false with a message when the file is not a readable v1 pack
        bool openFile(const std::filesystem::path& path, std::string* error = nullptr);
        // the pack's bytes stay owned by the caller and must outlive it
        bool openMemory(std::span<const uint8_t> bytes, std::string* error = nullptr);
        void close();
        bool isOpen() const { return open; }

        const std::vector<Entry>& entries() const { return list; }
        const Entry* find(std::string_view path) const;
        // the entry's bytes, checked against its FNV-1a 64; false with a message when missing,
        // unreadable or corrupt (the message names the entry)
        bool read(std::string_view path, std::vector<uint8_t>& out, std::string* error = nullptr) const;
        bool read(const Entry& entry, uint8_t* out, std::string* error = nullptr) const;
        // the entry's bytes in place (memory packs only; checked once, on the first view)
        bool view(std::string_view path, std::span<const uint8_t>& out, std::string* error = nullptr) const;

        // the legal-path rule (no message: empty when legal)
        static std::string checkPath(std::string_view path);
        // the case-folded key two paths collide on
        static std::string foldCase(std::string_view path);

    private:
        bool parse(uint64_t totalSize, std::string* error);
        bool readAt(uint64_t offset, void* out, uint64_t size) const;

        bool open = false;
        std::FILE* file = nullptr;
        std::span<const uint8_t> memory;
        std::vector<Entry> list;
        std::unordered_map<std::string, size_t> byPath;
        mutable std::mutex fileMutex;
        // memory packs: set once an entry's hash matched
        std::unique_ptr<std::atomic<bool>[]> verified;
    };

}
