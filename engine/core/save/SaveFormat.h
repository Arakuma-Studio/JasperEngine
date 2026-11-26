// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SAVE_SAVEFORMAT_H
#define SAVE_SAVEFORMAT_H

// One save file, `<slot>.g<N>.jsave` (docs/specs/savegame.md R18, R19). Little-endian:
//   header   magic "JSAV\r\n\x1A\n", format u16 (1), flags u16 (0), header size u32 (44 +
//            metadata), generation u64, saved-at i64 (Unix ms, UTC), data version u32, section
//            count u32, metadata size u32, metadata (a JLV1 table {engine, meta, platform, requires})
//   sections name length u16, name, payload length u32, payload (one JLV1 value)
//   footer   XXH3-64 (seed 0) of every byte before it, then "JSAVEND\n"
// Readers check every length against the bytes left before they allocate, and the footer before
// they parse. A format above FORMAT_VERSION whose footer checks reads as NewerFormat and is never
// rewritten (every format keeps the footer: with a damaged one it is Corrupt).

#include "Export.h"
#include "save/SaveValue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::savegame {

    constexpr uint16_t FORMAT_VERSION = 1;
    constexpr size_t HEADER_FIXED_BYTES = 44;
    constexpr size_t FOOTER_BYTES = 16;
    constexpr size_t MAX_METADATA_BYTES = 4096;
    constexpr size_t MAX_HEADER_BYTES = HEADER_FIXED_BYTES + MAX_METADATA_BYTES;
    constexpr size_t MAX_FILE_BYTES = 64u * 1024u * 1024u;
    constexpr size_t MAX_SECTIONS = 64;
    constexpr size_t MAX_SECTION_NAME = 32;
    constexpr size_t MAX_SLOT_NAME = 48;

    struct SaveSection {
        std::string name;
        std::shared_ptr<const std::string> payload;
    };

    struct SaveFile {
        uint16_t format = FORMAT_VERSION;
        uint64_t generation = 0;
        int64_t savedAtMs = 0;
        uint32_t dataVersion = 1;
        // JLV1 table {engine, meta, platform, requires}
        std::string metadata;
        std::vector<SaveSection> sections;
    };

    // The parts of the metadata table (each an encoded payload, empty when absent)
    struct SaveMetadata {
        std::string meta;
        std::string requirements;
        std::string engine;
        std::string platform;
    };

    JASPERENGINE_API std::string buildMetadata(const SaveMetadata& metadata);
    // False when the bytes are no metadata table
    JASPERENGINE_API bool splitMetadata(std::string_view bytes, SaveMetadata& out);

    JASPERENGINE_API std::string buildSaveFile(const SaveFile& file);
    // The whole file: footer, checksum, header, sections (each payload validated). False with
    // code Corrupt naming the reason and offset, or NewerFormat
    JASPERENGINE_API bool parseSaveFile(std::string_view bytes, SaveFile& out, SaveError& error);
    // The first bytes of a file (at least the header, at most MAX_HEADER_BYTES are read): no
    // sections, no checksum
    JASPERENGINE_API bool parseSaveHeader(std::string_view bytes, SaveFile& out, SaveError& error);

    // Slot and namespace names: 1..max characters of a-z, 0-9, '_', '-'
    JASPERENGINE_API bool isValidName(std::string_view name, size_t max);
    // The suggestion of R1: lower case, other characters as '-', cut to `max`
    JASPERENGINE_API std::string suggestName(std::string_view name, size_t max);

}

#endif // SAVE_SAVEFORMAT_H
