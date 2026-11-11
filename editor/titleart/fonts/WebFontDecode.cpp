// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WebFontDecode.h"

#include "FontTables.h"

// (miniz's zlib-named macros, compress / uncompress ..., stay out of this file)
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"
#include <woff2/decode.h>
#include <woff2/output.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace jasperengine::editor::titlefonts {

    namespace {

        uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
        uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
        void put16(std::vector<uint8_t>& b, size_t o, uint32_t v) {
            b[o] = (uint8_t)(v >> 8);
            b[o + 1] = (uint8_t)v;
        }
        void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
            b[o] = (uint8_t)(v >> 24);
            b[o + 1] = (uint8_t)(v >> 16);
            b[o + 2] = (uint8_t)(v >> 8);
            b[o + 3] = (uint8_t)v;
        }
        size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

        std::string megabytes(unsigned long long bytes) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
            return text;
        }

        DecodedFont refuse(std::string why) {
            DecodedFont out;
            out.error = std::move(why);
            return out;
        }

        std::string tooLarge(unsigned long long bytes, size_t maxDecoded) {
            return "it decodes to " + megabytes(bytes) + ", more than the " + std::to_string(maxDecoded / (1024 * 1024)) + " MB a font may have";
        }

        std::string truncated(unsigned long long declared, size_t actual) {
            return "the file is truncated: its header says " + std::to_string(declared) + " bytes, it has " + std::to_string(actual);
        }

        std::string hex32(uint32_t v) {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08X", v);
            return text;
        }

        struct WoffTable {
            uint32_t tag = 0;
            uint32_t checksum = 0;
            std::vector<uint8_t> data;
        };

        // the sfnt of `tables` (sorted by tag) with `flavor`, head's checkSumAdjustment recomputed
        std::vector<uint8_t> assembleSfnt(uint32_t flavor, std::vector<WoffTable>& tables) {
            std::sort(tables.begin(), tables.end(), [](const WoffTable& a, const WoffTable& b) { return a.tag < b.tag; });
            const size_t count = tables.size();
            uint32_t power = 1, log2 = 0;
            while (power * 2 <= count) {
                power *= 2;
                log2++;
            }
            size_t total = 12 + 16 * count;
            for (const WoffTable& t : tables) total += pad4(t.data.size());
            std::vector<uint8_t> out(total, 0);
            put32(out, 0, flavor);
            put16(out, 4, (uint32_t)count);
            put16(out, 6, power * 16);
            put16(out, 8, log2);
            put16(out, 10, (uint32_t)count * 16 - power * 16);
            size_t offset = 12 + 16 * count;
            size_t headOffset = 0;
            bool head = false;
            for (size_t i = 0; i < count; i++) {
                const WoffTable& t = tables[i];
                const size_t record = 12 + 16 * i;
                put32(out, record, t.tag);
                put32(out, record + 4, t.checksum);
                put32(out, record + 8, (uint32_t)offset);
                put32(out, record + 12, (uint32_t)t.data.size());
                if (!t.data.empty()) std::memcpy(out.data() + offset, t.data.data(), t.data.size());
                if (t.tag == sfntTag("head") && t.data.size() >= 12) {
                    headOffset = offset;
                    head = true;
                }
                offset += pad4(t.data.size());
            }
            if (head) {
                put32(out, headOffset + 8, 0);
                put32(out, headOffset + 8, 0xB1B0AFBAu - sfntChecksum(out.data(), out.size()));
            }
            return out;
        }

    }

    FontFileFormat sniffFontFormat(const uint8_t* data, size_t size) {
        if (!data || size < 4) return FontFileFormat::Unknown;
        const uint32_t magic = be32(data);
        if (magic == sfntTag("wOFF")) return FontFileFormat::Woff;
        if (magic == sfntTag("wOF2")) return FontFileFormat::Woff2;
        if (magic == sfntTag("ttcf")) return FontFileFormat::Collection;
        if (magic == sfntTag("OTTO")) return FontFileFormat::OpenTypeCff;
        if (magic == 0x00010000u || magic == sfntTag("true")) return FontFileFormat::TrueType;
        return FontFileFormat::Unknown;
    }

    bool isWebFontFileName(const std::string& fileName) {
        std::string lower = fileName;
        for (char& c : lower) c = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        const auto endsWith = [&lower](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        return endsWith(".woff") || endsWith(".woff2");
    }

    std::string sfntExtension(const uint8_t* data, size_t size) {
        if (isFontCollection(data, size)) return ".ttc";
        const OutlineKind kind = outlineKind(data, size, 0);
        if (kind == OutlineKind::Cff || kind == OutlineKind::Cff2) return ".otf";
        if (size >= 4 && be32(data) == sfntTag("OTTO")) return ".otf";
        return ".ttf";
    }

    DecodedFont decodeWebFont(const uint8_t* data, size_t size, size_t maxDecoded) {
        switch (sniffFontFormat(data, size)) {
            case FontFileFormat::Woff: return decodeWoff(data, size, maxDecoded);
            case FontFileFormat::Woff2: return decodeWoff2(data, size, maxDecoded);
            case FontFileFormat::Unknown: return refuse("it is not a font file (no WOFF, WOFF2, TrueType or OpenType signature)");
            default: return refuse("it is not a web font: it is already a TrueType or OpenType file");
        }
    }

    DecodedFont decodeWoff(const uint8_t* data, size_t size, size_t maxDecoded) {
        constexpr size_t HEADER = 44, ENTRY = 20;
        if (!data || size < HEADER) return refuse("the file is truncated: it is shorter than a WOFF header");
        if (be32(data) != sfntTag("wOFF")) return refuse("it is not a WOFF file");
        const uint32_t flavor = be32(data + 4);
        const uint32_t length = be32(data + 8);
        const uint16_t numTables = be16(data + 12);
        const uint32_t totalSfntSize = be32(data + 16);
        if (size < length) return refuse(truncated(length, size));
        if (totalSfntSize > maxDecoded) return refuse(tooLarge(totalSfntSize, maxDecoded));
        if (numTables == 0) return refuse("the WOFF file has no tables");
        if (size < HEADER + ENTRY * (size_t)numTables) return refuse("the file is truncated: its table directory is cut off");

        // the decoded size from the directory, before anything is inflated
        unsigned long long decodedSize = 12 + 16ull * numTables;
        for (uint16_t i = 0; i < numTables; i++) decodedSize += pad4(be32(data + HEADER + ENTRY * i + 12));
        if (decodedSize > maxDecoded) return refuse(tooLarge(decodedSize, maxDecoded));

        std::vector<WoffTable> tables;
        tables.reserve(numTables);
        for (uint16_t i = 0; i < numTables; i++) {
            const uint8_t* entry = data + HEADER + ENTRY * i;
            WoffTable table;
            table.tag = be32(entry);
            const uint32_t offset = be32(entry + 4), compLength = be32(entry + 8), origLength = be32(entry + 12);
            table.checksum = be32(entry + 16);
            const std::string name = "table '" + sfntTagName(table.tag) + "'";
            if ((unsigned long long)offset + compLength > size) return refuse("the file is truncated: " + name + " is cut off");
            if (compLength > origLength) return refuse(name + " is larger compressed than decoded, which WOFF forbids");
            table.data.resize(origLength);
            if (compLength == origLength) {
                if (origLength) std::memcpy(table.data.data(), data + offset, origLength);
            } else {
                mz_ulong written = origLength;
                const int status = mz_uncompress(table.data.data(), &written, data + offset, compLength);
                if (status != MZ_OK || written != origLength) {
                    return refuse(name + " does not inflate (" + std::string(status == MZ_OK ? "wrong length" : mz_error(status)) + ")");
                }
            }
            // head's checksum is taken with checkSumAdjustment zeroed (OpenType); some tools sum it as stored
            uint32_t computed = sfntChecksum(table.data.data(), table.data.size());
            if (computed != table.checksum && table.tag == sfntTag("head") && table.data.size() >= 12) {
                std::vector<uint8_t> zeroed = table.data;
                put32(zeroed, 8, 0);
                computed = sfntChecksum(zeroed.data(), zeroed.size());
            }
            if (computed != table.checksum) {
                return refuse(name + " fails its checksum (the WOFF records " + hex32(table.checksum) + ", the data sums to " + hex32(computed) + ")");
            }
            tables.push_back(std::move(table));
        }

        DecodedFont out;
        out.bytes = assembleSfnt(flavor, tables);
        out.extension = sfntExtension(out.bytes.data(), out.bytes.size());
        out.ok = true;
        return out;
    }

    DecodedFont decodeWoff2(const uint8_t* data, size_t size, size_t maxDecoded) {
        constexpr size_t HEADER = 48;
        if (!data || size < HEADER) return refuse("the file is truncated: it is shorter than a WOFF2 header");
        if (be32(data) != sfntTag("wOF2")) return refuse("it is not a WOFF2 file");
        const uint32_t length = be32(data + 8);
        const uint32_t totalSfntSize = be32(data + 16);
        if (size < length) return refuse(truncated(length, size));
        if (totalSfntSize > maxDecoded) return refuse(tooLarge(totalSfntSize, maxDecoded));
        if (woff2::ComputeWOFF2FinalSize(data, size) == 0) return refuse("the WOFF2 header is not valid");

        std::string decoded;
        woff2::WOFF2StringOut sink(&decoded);
        sink.SetMaxSize(maxDecoded);
        if (!woff2::ConvertWOFF2ToTTF(data, size, &sink)) {
            if (sink.Size() >= maxDecoded) return refuse(tooLarge(sink.Size(), maxDecoded));
            return refuse("the WOFF2 decoder refused it: its Brotli stream or its table data is damaged");
        }
        DecodedFont out;
        out.bytes.assign(decoded.begin(), decoded.begin() + (std::ptrdiff_t)std::min(decoded.size(), sink.Size()));
        if (sfntFaceOffsets(out.bytes.data(), out.bytes.size()).empty()) return refuse("the WOFF2 decoder produced no font");
        out.extension = sfntExtension(out.bytes.data(), out.bytes.size());
        out.ok = true;
        return out;
    }

    std::vector<FontFaceListing> listFontFaces(const uint8_t* data, size_t size) {
        std::vector<FontFaceListing> faces;
        const size_t count = sfntFaceOffsets(data, size).size();
        for (size_t i = 0; i < count; i++) {
            const FontNames names = fontNames(data, size, (int)i);
            FontFaceListing face;
            face.index = (int)i;
            face.family = names.family;
            face.style = names.style;
            face.postScriptName = names.postScriptName;
            faces.push_back(std::move(face));
        }
        return faces;
    }

}
