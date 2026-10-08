// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_HASH_H
#define COMPAT_HASH_H

#include "Export.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // Content hashes of the compat layer: SHA-1 names data: URIs and generated keyframes
    // (@htmlui/data/<sha1-12>.png, kf-<sha1-8>), SHA-256 is the import manifest's file
    // checksum, FNV-1a 64 the compile cache keys.
    JASPERENGINE_API std::string sha1Hex(std::string_view data);       // 40 lowercase hex digits
    JASPERENGINE_API std::string sha256Hex(std::string_view data);     // 64 lowercase hex digits
    JASPERENGINE_API uint64_t fnv1a64(std::string_view data, uint64_t seed = 0xcbf29ce484222325ull);
    JASPERENGINE_API std::string toHex(uint64_t value, int digits = 16);

}

#endif //COMPAT_HASH_H
