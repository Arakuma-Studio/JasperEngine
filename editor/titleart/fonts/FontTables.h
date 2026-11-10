// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R54, R55, R58-R60)
// - Purpose: the font facts the Title Designer's picker, licence check, web font decoder and
//   preview strips read from a font file's bytes: the faces of a collection, a face's tables, its
//   names, its OS/2 fields (fsType, sFamilyClass, PANOSE, weight), its variation axes and named
//   instances (fvar + name) and its character coverage (cmap formats 0, 4, 6, 10, 12, 13).
// - Design: a reader of the sfnt structures, not HarfBuzz: the editor's shared engine keeps every
//   hb_* symbol hidden (engine/libs/harfbuzz/CMakeLists.txt), so the editor cannot call
//   hb_face_count or hb_ot_var_*; these readers give the same answers from the same tables.
//   Every read is bounds-checked: a truncated or hostile file gives empty answers, never a crash.
// - Thread-safety: pure functions of the bytes; CharMap is a value.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::titlefonts {

    // a table tag from four characters ("glyf")
    constexpr uint32_t sfntTag(const char (&t)[5]) {
        return (uint32_t)(uint8_t)t[0] << 24 | (uint32_t)(uint8_t)t[1] << 16 | (uint32_t)(uint8_t)t[2] << 8 | (uint32_t)(uint8_t)t[3];
    }
    std::string sfntTagName(uint32_t tag);

    struct SfntTable {
        uint32_t tag = 0;
        uint32_t checksum = 0;
        uint32_t offset = 0;        // from the start of the file (collections included)
        uint32_t length = 0;
    };

    enum class OutlineKind : uint8_t { None, TrueType, Cff, Cff2 };

    // the offsets of the faces' table directories: one per member of a .ttc / .otc, one (0) for a
    // single TrueType or OpenType font, none when the bytes are not an sfnt font
    std::vector<uint32_t> sfntFaceOffsets(const uint8_t* data, size_t size);
    bool isFontCollection(const uint8_t* data, size_t size);
    // the face's table directory (false when the face or the directory is out of the bytes)
    bool sfntTables(const uint8_t* data, size_t size, int faceIndex, std::vector<SfntTable>& out);
    bool findSfntTable(const uint8_t* data, size_t size, int faceIndex, uint32_t tag, SfntTable& out);
    // glyf (TrueType), CFF or CFF2 outlines; None for a bitmap-only face
    OutlineKind outlineKind(const uint8_t* data, size_t size, int faceIndex);
    // the OpenType table checksum (sum of big-endian 32-bit words, the tail zero-padded)
    uint32_t sfntChecksum(const uint8_t* data, size_t size);

    // a name table string (Windows Unicode or Mac Roman, English preferred), "" when absent
    std::string fontNameString(const uint8_t* data, size_t size, int faceIndex, uint16_t nameId);
    // the same from the bytes of a `name` table alone (a lister that read only that table)
    std::string nameTableString(const uint8_t* table, size_t size, uint16_t nameId);

    struct FontNames {
        std::string family;         // typographic family (16), else family (1)
        std::string style;          // typographic subfamily (17), else subfamily (2)
        std::string fullName;       // 4
        std::string postScriptName; // 6
    };
    FontNames fontNames(const uint8_t* data, size_t size, int faceIndex);

    struct Os2Info {
        bool present = false;
        uint16_t fsType = 0;
        uint16_t weightClass = 400;
        int16_t familyClass = 0;    // sFamilyClass: class in the high byte, subclass in the low
        uint8_t panose[10] = {};
        bool italic = false;        // fsSelection bit 0 (or 9, oblique)
        bool monospacedHint = false; // post.isFixedPitch
    };
    Os2Info fontOs2(const uint8_t* data, size_t size, int faceIndex);

    struct FontAxisInfo {
        std::string tag;            // "wght"
        std::string name;           // from the name table ("Weight"), else the tag
        float minValue = 0.0f, defaultValue = 0.0f, maxValue = 0.0f;
        bool hidden = false;        // HIDDEN_AXIS flag
    };
    struct FontInstanceInfo {
        std::string name;           // "Bold"
        std::vector<float> coordinates;   // one per axis, in axis order
    };
    // fvar's axes and named instances (empty for a static font)
    std::vector<FontAxisInfo> fontAxes(const uint8_t* data, size_t size, int faceIndex);
    std::vector<FontInstanceInfo> fontNamedInstances(const uint8_t* data, size_t size, int faceIndex);

    // The code points a face maps to a glyph, as sorted ranges (the best Unicode cmap subtable:
    // full repertoire 3/10, 0/4, 0/6 first, then BMP 3/1, 0/3 ... 0/0, then a symbol 3/0 face's
    // F0xx range)
    class CharMap {
    public:
        bool load(const uint8_t* data, size_t size, int faceIndex);
        bool covers(uint32_t codepoint) const;
        bool empty() const { return ranges.empty(); }
        size_t rangeCount() const { return ranges.size(); }

    private:
        void add(uint32_t first, uint32_t last);
        void finish();
        std::vector<std::pair<uint32_t, uint32_t>> ranges;   // inclusive, sorted, merged
        bool symbol = false;
    };

    // UTF-8 -> code points (invalid sequences become U+FFFD)
    std::vector<uint32_t> utf8CodePoints(const std::string& text);
    void appendUtf8(std::string& out, uint32_t codepoint);
    // a code point that never needs a glyph to read (controls, spaces, joiners, variation selectors)
    bool isIgnorableForCoverage(uint32_t codepoint);

}
