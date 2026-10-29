// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RESOURCEPACK_H
#define RESOURCEPACK_H

#include "Export.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    class JPak;

    // The packed content Data::open() reads through; File keeps to the filesystem and the
    // editor never opens a pack. Two formats:
    //  - a .jpak v1 content pack (JPak.h; docs/specs/S30-shipping.md D6): what Build Settings >
    //    Pack Content writes (content.jpak), found next to the executable or mounted by the
    //    player (the web player mounts the one it fetched, in memory);
    //  - the older "resources.pak" (DXPK1, obfuscated), still read.
    // Keys are the pack's paths: asset://x is "assets/x", shader://x "assets/shaders/x", lua://x
    // "lua/x", content://x "x" (game.json, cooked scenes), a plain relative path "assets/<path>".
    class JASPERENGINE_API ResourcePack {
    public:
        // Paths are the ones Data::open() takes: asset://, lua://, shader://, content:// or
        // asset-relative
        static bool contains(const std::string& path);
        static bool read(const std::string& path, std::vector<unsigned char>& outData);
        // Data::open can take ownership directly, without copying the packed bytes.
        static bool readOwned(const std::string& path, std::unique_ptr<unsigned char[]>& outData,
                              unsigned int& outSize);

        // Mounts a content pack before anything reads (replacing one mounted earlier): a file, or
        // memory the caller keeps alive. False with a message when it is not a valid pack.
        static bool mountContentPack(const std::filesystem::path& path, std::string* error = nullptr);
        static bool mountContentPack(std::span<const uint8_t> bytes, std::string* error = nullptr);
        static const JPak* contentPack();
        // the pack key of a path (empty: no pack holds it, e.g. data:// or an absolute path)
        static std::string packKey(const std::string& path);
    };

}

#endif
