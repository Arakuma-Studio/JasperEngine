// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Modules/FileWatcher/Include/FileWatcher/FileIdentity.h (NOTICE.md),
// in this engine's namespace and naming.

#ifndef JASPERENGINE_FILEIDENTITY_H
#define JASPERENGINE_FILEIDENTITY_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine {

    // Filesystem identity of a file: the volume it lives on and its index within that volume
    // (NTFS file index, POSIX device + inode).
    //
    // The identity survives renames and in-place rewrites of the same file and changes when a
    // path is replaced by a different file (a safe-save renames a temp file over the target),
    // so it tells a renamed file from a deleted-then-created one where neither the path nor
    // the timestamp can.
    struct JASPERENGINE_API FileIdentity {
        uint64_t volume = 0;
        uint64_t index = 0;
        bool valid = false;

        bool operator==(const FileIdentity& other) const {
            return volume == other.volume && index == other.index && valid == other.valid;
        }
        bool operator!=(const FileIdentity& other) const { return !(*this == other); }

        // Persistent text form ("" when invalid)
        std::string toString() const;
    };

    // Identity of the file at `path`; invalid when the file cannot be opened.
    JASPERENGINE_API FileIdentity readFileIdentity(const std::filesystem::path& path);

}

template <>
struct std::hash<jasperengine::FileIdentity> {
    std::size_t operator()(const jasperengine::FileIdentity& identity) const noexcept {
        // Volume counts are tiny and indices dense; the odd multiplier spreads the volume
        // across the word so two volumes' index runs do not collide.
        constexpr std::uint64_t volumeSpread = 0x9E3779B97F4A7C15ull;
        return std::hash<std::uint64_t>{}(identity.index ^ (identity.volume * volumeSpread));
    }
};

#endif // JASPERENGINE_FILEIDENTITY_H
