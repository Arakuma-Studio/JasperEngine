// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GUID_H
#define GUID_H

#include "Export.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace jasperengine {

    // The 128-bit identity of an asset (or of a sub-asset inside a model). Its text form is 32
    // lowercase hex digits without dashes, the form prefab files have always used; byte i of
    // the GUID is hex pair i of the text, hi holds bytes 0-7 and lo bytes 8-15 (big-endian).
    struct JASPERENGINE_API Guid {
        uint64_t hi = 0;
        uint64_t lo = 0;

        Guid() = default;
        Guid(uint64_t hi, uint64_t lo): hi(hi), lo(lo) {}

        // 32 hex digits (either case, surrounding spaces ignored); a null Guid for anything else
        static Guid parse(std::string_view hex32);
        static bool isValidText(std::string_view text);

        // 32 lowercase hex digits; empty for the null Guid
        std::string str() const;

        // A sub-asset's GUID from its container's and its key (SubAssetKeys.h). The output is a
        // persistence contract: references store derived GUIDs, so the derivation is frozen
        // (GuidDerive.GoldenVectors), byte-identical on every platform and word size.
        static Guid derive(const Guid& parent, std::string_view key);

        bool isNull() const { return hi == 0 && lo == 0; }
        explicit operator bool() const { return !isNull(); }

        void toBytes(uint8_t out[16]) const;
        static Guid fromBytes(const uint8_t bytes[16]);

        bool operator==(const Guid& other) const { return hi == other.hi && lo == other.lo; }
        bool operator!=(const Guid& other) const { return !(*this == other); }
        bool operator<(const Guid& other) const { return hi < other.hi || (hi == other.hi && lo < other.lo); }
    };

}

namespace std {
    template<> struct hash<jasperengine::Guid> {
        size_t operator()(const jasperengine::Guid& guid) const noexcept {
            const uint64_t mixed = guid.hi ^ (guid.lo * 0x9E3779B97F4A7C15ULL);
            return static_cast<size_t>(mixed ^ (mixed >> 32));
        }
    };
}

#endif // GUID_H
