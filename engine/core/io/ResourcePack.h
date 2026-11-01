// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RESOURCEPACK_H
#define RESOURCEPACK_H

#include "Export.h"

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace jasperengine {

    // Native exports can pack assets and Lua sources into a single "resources.pak"
    // (Project Settings > Build). Entries are obfuscated, not encrypted. Data::open()
    // reads through it; File keeps to the filesystem and the editor never opens a pack.
    class JASPERENGINE_API ResourcePack {
    public:
        // Paths are the ones Data::open() takes: asset://, lua:// or asset-relative
        static bool contains(const std::string& path);
        // the unpacked size of an entry, false when the pack does not hold it
        static bool size(const std::string& path, uint64_t& outSize);
        static bool read(const std::string& path, std::vector<unsigned char>& outData);
        // Data::open can take ownership directly, without copying the packed bytes.
        static bool readOwned(const std::string& path, std::unique_ptr<unsigned char[]>& outData,
                              unsigned int& outSize);

        // A packed entry read in parts (videos: their decoders read positions, never the whole
        // file into memory): open() gives the entry's size and a handle of the pack file;
        // readRange() the bytes [from, from + count) of the entry, deobfuscated
        class JASPERENGINE_API RangeReader {
        public:
            ~RangeReader();
            bool open(const std::string& path);
            uint64_t size() const { return entrySize; }
            bool readRange(uint64_t from, unsigned char* dst, size_t count);
        private:
            void* file = nullptr;
            uint64_t entryOffset = 0;
            uint64_t entrySize = 0;
            uint8_t key = 0;
            uint32_t shift = 0;
        };
    };

}

#endif
