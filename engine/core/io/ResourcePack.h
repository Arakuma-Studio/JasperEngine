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
    };

}

#endif
