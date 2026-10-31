// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Streaming SHA-256 (FIPS 180-4): the content manifests of shipped games, player runtime
// archives and releases (docs/specs/S30-shipping.md R21, R28). x86-64 CPUs with the SHA
// extensions take a hardware path (chosen at run time); everything else the portable one.

#include "Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace jasperengine {

    class JASPERENGINE_API Sha256 {
    public:
        using Digest = std::array<uint8_t, 32>;

        Sha256();
        void reset();
        void update(const void* data, size_t size);
        void update(std::string_view data) { update(data.data(), data.size()); }
        // the digest of everything updated since the last reset; resets
        Digest finish();
        std::string finishHex();

        static std::string toHex(const Digest& digest);
        // true when this CPU runs the hardware path (SHA-NI)
        static bool hardwareAccelerated();

    private:
        uint32_t state[8];
        uint8_t buffer[64];
        size_t buffered = 0;
        uint64_t length = 0;
    };

    // 64 lowercase hex digits
    JASPERENGINE_API std::string sha256Hex(std::string_view data);
    // the hex digest of a file, read in chunks; false (with a message) when it cannot be read
    JASPERENGINE_API bool sha256File(const std::filesystem::path& path, std::string& hex, std::string* error = nullptr,
                                     uint64_t* size = nullptr);

}
