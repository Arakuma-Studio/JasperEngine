// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontTables.h"

#include <algorithm>
#include <cstring>

namespace jasperengine::editor::titlefonts {

    namespace {

        // ---------- bounds-checked big-endian reads ----------

        struct Reader {
            const uint8_t* data = nullptr;
            size_t size = 0;

            bool has(size_t offset, size_t length) const { return offset <= size && length <= size - offset; }
            uint8_t u8(size_t o) const { return has(o, 1) ? data[o] : 0; }
            uint16_t u16(size_t o) const { return has(o, 2) ? (uint16_t)(data[o] << 8 | data[o + 1]) : 0; }
            int16_t s16(size_t o) const { return (int16_t)u16(o); }
            uint32_t u32(size_t o) const {
                return has(o, 4) ? (uint32_t)data[o] << 24 | (uint32_t)data[o + 1] << 16 | (uint32_t)data[o + 2] << 8 | (uint32_t)data[o + 3] : 0;
            }
            float fixed(size_t o) const { return (float)((double)(int32_t)u32(o) / 65536.0); }
        };

        // a table's bytes as a reader of their own (empty when it is out of the file)
        Reader tableReader(const uint8_t* data, size_t size, int faceIndex, uint32_t tag) {
            SfntTable table;
            Reader whole{data, size};
            if (!findSfntTable(data, size, faceIndex, tag, table) || !whole.has(table.offset, table.length)) return {};
            return Reader{data + table.offset, table.length};
        }

        bool isSfntVersion(uint32_t version) {
            return version == 0x00010000u || version == sfntTag("OTTO") || version == sfntTag("true");
        }

        // Mac Roman 0x80..0xFF
        constexpr uint16_t MAC_ROMAN_HIGH[128] = {
            0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
            0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
            0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
            0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
            0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
            0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
            0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
            0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};

        std::string decodeUtf16Be(const uint8_t* p, size_t length) {
            std::string out;
            for (size_t i = 0; i + 1 < length; i += 2) {
                uint32_t c = (uint32_t)(p[i] << 8 | p[i + 1]);
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < length) {
                    const uint32_t low = (uint32_t)(p[i + 2] << 8 | p[i + 3]);
                    if (low >= 0xDC00 && low < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                appendUtf8(out, c);
            }
            return out;
        }

        std::string decodeMacRoman(const uint8_t* p, size_t length) {
            std::string out;
            for (size_t i = 0; i < length; i++) appendUtf8(out, p[i] < 0x80 ? p[i] : MAC_ROMAN_HIGH[p[i] - 0x80]);
            return out;
        }

    }

    std::string sfntTagName(uint32_t tag) {
        std::string out(4, ' ');
        for (int i = 0; i < 4; i++) out[i] = (char)((tag >> (24 - 8 * i)) & 0xFF);
        return out;
    }

    std::vector<uint32_t> sfntFaceOffsets(const uint8_t* data, size_t size) {
        const Reader r{data, size};
        const uint32_t version = r.u32(0);
        if (version == sfntTag("ttcf")) {
            const uint32_t count = r.u32(8);
            if (count == 0 || count > 0xFFFF || !r.has(12, (size_t)count * 4)) return {};
            std::vector<uint32_t> offsets;
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t offset = r.u32(12 + (size_t)i * 4);
                if (!r.has(offset, 12) || !isSfntVersion(r.u32(offset))) return {};
                offsets.push_back(offset);
            }
            return offsets;
        }
        if (r.has(0, 12) && isSfntVersion(version)) return {0};
        return {};
    }

    bool isFontCollection(const uint8_t* data, size_t size) {
        return Reader{data, size}.u32(0) == sfntTag("ttcf") && !sfntFaceOffsets(data, size).empty();
    }

    bool sfntTables(const uint8_t* data, size_t size, int faceIndex, std::vector<SfntTable>& out) {
        out.clear();
        const std::vector<uint32_t> faces = sfntFaceOffsets(data, size);
        if (faceIndex < 0 || (size_t)faceIndex >= faces.size()) return false;
        const Reader r{data, size};
        const size_t base = faces[(size_t)faceIndex];
        const uint16_t count = r.u16(base + 4);
        if (!r.has(base + 12, (size_t)count * 16)) return false;
        for (uint16_t i = 0; i < count; i++) {
            const size_t record = base + 12 + (size_t)i * 16;
            out.push_back({r.u32(record), r.u32(record + 4), r.u32(record + 8), r.u32(record + 12)});
        }
        return true;
    }

    bool findSfntTable(const uint8_t* data, size_t size, int faceIndex, uint32_t tag, SfntTable& out) {
        std::vector<SfntTable> tables;
        if (!sfntTables(data, size, faceIndex, tables)) return false;
        for (const SfntTable& table : tables) {
            if (table.tag == tag) {
                out = table;
                return true;
            }
        }
        return false;
    }

    OutlineKind outlineKind(const uint8_t* data, size_t size, int faceIndex) {
        std::vector<SfntTable> tables;
        if (!sfntTables(data, size, faceIndex, tables)) return OutlineKind::None;
        bool glyf = false, loca = false, cff = false, cff2 = false;
        for (const SfntTable& t : tables) {
            glyf = glyf || (t.tag == sfntTag("glyf") && t.length > 0);
            loca = loca || t.tag == sfntTag("loca");
            cff = cff || (t.tag == sfntTag("CFF ") && t.length > 0);
            cff2 = cff2 || (t.tag == sfntTag("CFF2") && t.length > 0);
        }
        if (cff2) return OutlineKind::Cff2;
        if (cff) return OutlineKind::Cff;
        if (glyf && loca) return OutlineKind::TrueType;
        return OutlineKind::None;
    }

    uint32_t sfntChecksum(const uint8_t* data, size_t size) {
        uint32_t sum = 0;
        size_t i = 0;
        for (; i + 4 <= size; i += 4) sum += (uint32_t)data[i] << 24 | (uint32_t)data[i + 1] << 16 | (uint32_t)data[i + 2] << 8 | data[i + 3];
        if (i < size) {
            uint8_t tail[4] = {0, 0, 0, 0};
            std::memcpy(tail, data + i, size - i);
            sum += (uint32_t)tail[0] << 24 | (uint32_t)tail[1] << 16 | (uint32_t)tail[2] << 8 | tail[3];
        }
        return sum;
    }

    std::string fontNameString(const uint8_t* data, size_t size, int faceIndex, uint16_t nameId) {
        const Reader r = tableReader(data, size, faceIndex, sfntTag("name"));
        if (!r.data) return "";
        return nameTableString(r.data, r.size, nameId);
    }

    std::string nameTableString(const uint8_t* table, size_t tableSize, uint16_t nameId) {
        const Reader r{table, tableSize};
        if (!r.data || !r.has(0, 6)) return "";
        const uint16_t count = r.u16(2);
        const size_t storage = r.u16(4);
        // the best record: Windows Unicode English, any Windows Unicode, Unicode, Mac Roman English
        int bestScore = 0;
        size_t bestOffset = 0, bestLength = 0;
        bool bestMac = false;
        for (uint16_t i = 0; i < count; i++) {
            const size_t record = 6 + (size_t)i * 12;
            if (!r.has(record, 12) || r.u16(record + 6) != nameId) continue;
            const uint16_t platform = r.u16(record), encoding = r.u16(record + 2), language = r.u16(record + 4);
            int score = 0;
            if (platform == 3 && (encoding == 1 || encoding == 10)) score = language == 0x0409 ? 6 : 5;
            else if (platform == 0) score = 4;
            else if (platform == 3 && encoding == 0) score = 3;
            else if (platform == 1 && encoding == 0) score = language == 0 ? 2 : 1;
            if (score <= bestScore) continue;
            const size_t length = r.u16(record + 8), offset = storage + r.u16(record + 10);
            if (!r.has(offset, length)) continue;
            bestScore = score;
            bestOffset = offset;
            bestLength = length;
            bestMac = platform == 1;
        }
        if (bestScore == 0) return "";
        return bestMac ? decodeMacRoman(r.data + bestOffset, bestLength) : decodeUtf16Be(r.data + bestOffset, bestLength);
    }

    FontNames fontNames(const uint8_t* data, size_t size, int faceIndex) {
        FontNames names;
        names.family = fontNameString(data, size, faceIndex, 16);
        if (names.family.empty()) names.family = fontNameString(data, size, faceIndex, 1);
        names.style = fontNameString(data, size, faceIndex, 17);
        if (names.style.empty()) names.style = fontNameString(data, size, faceIndex, 2);
        names.fullName = fontNameString(data, size, faceIndex, 4);
        names.postScriptName = fontNameString(data, size, faceIndex, 6);
        return names;
    }

    Os2Info fontOs2(const uint8_t* data, size_t size, int faceIndex) {
        Os2Info info;
        const Reader r = tableReader(data, size, faceIndex, sfntTag("OS/2"));
        if (r.data && r.has(0, 64)) {
            info.present = true;
            info.weightClass = r.u16(4);
            info.fsType = r.u16(8);
            info.familyClass = r.s16(30);
            for (int i = 0; i < 10; i++) info.panose[i] = r.u8(32 + (size_t)i);
            const uint16_t selection = r.u16(62);
            info.italic = (selection & 0x0001) != 0 || (selection & 0x0200) != 0;
        } else if (r.data && r.has(0, 10)) {
            info.present = true;
            info.weightClass = r.u16(4);
            info.fsType = r.u16(8);
        }
        const Reader post = tableReader(data, size, faceIndex, sfntTag("post"));
        if (post.data && post.has(12, 4)) info.monospacedHint = post.u32(12) != 0;
        return info;
    }

    std::vector<FontAxisInfo> fontAxes(const uint8_t* data, size_t size, int faceIndex) {
        std::vector<FontAxisInfo> axes;
        const Reader r = tableReader(data, size, faceIndex, sfntTag("fvar"));
        if (!r.data || !r.has(0, 16) || r.u16(0) != 1) return axes;
        const size_t axesOffset = r.u16(4);
        const uint16_t count = r.u16(8), axisSize = r.u16(10);
        if (axisSize < 20) return axes;
        for (uint16_t i = 0; i < count; i++) {
            const size_t a = axesOffset + (size_t)i * axisSize;
            if (!r.has(a, 20)) break;
            FontAxisInfo axis;
            axis.tag = sfntTagName(r.u32(a));
            axis.minValue = r.fixed(a + 4);
            axis.defaultValue = r.fixed(a + 8);
            axis.maxValue = r.fixed(a + 12);
            axis.hidden = (r.u16(a + 16) & 0x0001) != 0;
            axis.name = fontNameString(data, size, faceIndex, r.u16(a + 18));
            if (axis.name.empty()) axis.name = axis.tag;
            axes.push_back(std::move(axis));
        }
        return axes;
    }

    std::vector<FontInstanceInfo> fontNamedInstances(const uint8_t* data, size_t size, int faceIndex) {
        std::vector<FontInstanceInfo> instances;
        const Reader r = tableReader(data, size, faceIndex, sfntTag("fvar"));
        if (!r.data || !r.has(0, 16) || r.u16(0) != 1) return instances;
        const size_t axesOffset = r.u16(4);
        const uint16_t axisCount = r.u16(8), axisSize = r.u16(10);
        const uint16_t count = r.u16(12), instanceSize = r.u16(14);
        if (instanceSize < 4 + 4 * (size_t)axisCount) return instances;
        const size_t first = axesOffset + (size_t)axisCount * axisSize;
        for (uint16_t i = 0; i < count; i++) {
            const size_t at = first + (size_t)i * instanceSize;
            if (!r.has(at, instanceSize)) break;
            FontInstanceInfo instance;
            instance.name = fontNameString(data, size, faceIndex, r.u16(at));
            for (uint16_t a = 0; a < axisCount; a++) instance.coordinates.push_back(r.fixed(at + 4 + (size_t)a * 4));
            if (instance.name.empty()) instance.name = "Instance " + std::to_string(i + 1);
            instances.push_back(std::move(instance));
        }
        return instances;
    }

    // ---------- CharMap ----------

    void CharMap::add(uint32_t first, uint32_t last) {
        if (first <= last) ranges.push_back({first, last});
    }

    void CharMap::finish() {
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& range : ranges) {
            if (!merged.empty() && range.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, range.second);
            else merged.push_back(range);
        }
        ranges = std::move(merged);
    }

    bool CharMap::load(const uint8_t* data, size_t size, int faceIndex) {
        ranges.clear();
        symbol = false;
        const Reader r = tableReader(data, size, faceIndex, sfntTag("cmap"));
        if (!r.data) return false;
        const uint16_t count = r.u16(2);
        // the preferred subtable: full Unicode first, then BMP Unicode, then symbol, then Mac Roman
        int bestScore = 0;
        size_t best = 0;
        for (uint16_t i = 0; i < count; i++) {
            const size_t record = 4 + (size_t)i * 8;
            if (!r.has(record, 8)) break;
            const uint16_t platform = r.u16(record), encoding = r.u16(record + 2);
            const size_t offset = r.u32(record + 4);
            if (!r.has(offset, 2)) continue;
            const uint16_t format = r.u16(offset);
            if (format == 14) continue;   // variation sequences: not a code point map
            int score = 0;
            if (platform == 3 && encoding == 10) score = 10;
            else if (platform == 0 && (encoding == 4 || encoding == 6)) score = 9;
            else if (platform == 3 && encoding == 1) score = 8;
            else if (platform == 0) score = 7;
            else if (platform == 3 && encoding == 0) score = 3;
            else if (platform == 1 && encoding == 0) score = 1;
            if (score > bestScore) {
                bestScore = score;
                best = offset;
            }
        }
        if (bestScore == 0) return false;
        symbol = bestScore == 3;
        const bool macRoman = bestScore == 1;
        const uint16_t format = r.u16(best);
        if (format == 0) {
            for (uint32_t c = 0; c < 256; c++) {
                if (r.u8(best + 6 + c) != 0 && (!macRoman || c < 0x80)) add(c, c);
            }
        } else if (format == 4) {
            const size_t segments = r.u16(best + 6) / 2;
            const size_t ends = best + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, rangeOffsets = deltas + segments * 2;
            if (!r.has(rangeOffsets, segments * 2)) return false;
            for (size_t s = 0; s < segments; s++) {
                const uint32_t end = r.u16(ends + s * 2), start = r.u16(starts + s * 2);
                const uint16_t delta = r.u16(deltas + s * 2), rangeOffset = r.u16(rangeOffsets + s * 2);
                if (start > end) continue;
                uint32_t runStart = 0;
                bool inRun = false;
                for (uint32_t c = start; c <= end; c++) {
                    uint16_t glyph;
                    if (rangeOffset == 0) {
                        glyph = (uint16_t)(c + delta);
                    } else {
                        const size_t at = rangeOffsets + s * 2 + rangeOffset + (size_t)(c - start) * 2;
                        glyph = r.has(at, 2) ? r.u16(at) : 0;
                        if (glyph != 0) glyph = (uint16_t)(glyph + delta);
                    }
                    const bool mapped = glyph != 0 && c != 0xFFFF;
                    if (mapped && !inRun) {
                        runStart = c;
                        inRun = true;
                    } else if (!mapped && inRun) {
                        add(runStart, c - 1);
                        inRun = false;
                    }
                }
                if (inRun) add(runStart, end == 0xFFFF ? 0xFFFE : end);
            }
        } else if (format == 6) {
            const uint32_t first = r.u16(best + 6), entries = r.u16(best + 8);
            for (uint32_t i = 0; i < entries; i++) {
                const uint32_t c = first + i;
                if (r.u16(best + 10 + (size_t)i * 2) != 0 && (!macRoman || c < 0x80)) add(c, c);
            }
        } else if (format == 10) {
            const uint32_t first = r.u32(best + 12), entries = r.u32(best + 16);
            for (uint32_t i = 0; i < entries && r.has(best + 20 + (size_t)i * 2, 2); i++) {
                if (r.u16(best + 20 + (size_t)i * 2) != 0) add(first + i, first + i);
            }
        } else if (format == 12 || format == 13) {
            const uint32_t groups = r.u32(best + 12);
            for (uint32_t g = 0; g < groups; g++) {
                const size_t at = best + 16 + (size_t)g * 12;
                if (!r.has(at, 12)) break;
                uint32_t first = r.u32(at);
                const uint32_t last = std::min<uint32_t>(r.u32(at + 4), 0x10FFFF);
                const uint32_t glyph = r.u32(at + 8);
                if (glyph == 0) {
                    // format 13 maps the whole group to .notdef; format 12 only its first code
                    if (format == 13) continue;
                    first++;
                }
                add(first, last);
            }
        } else {
            return false;
        }
        finish();
        return !ranges.empty();
    }

    bool CharMap::covers(uint32_t codepoint) const {
        const auto inRanges = [this](uint32_t c) {
            auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(c, UINT32_MAX));
            if (it == ranges.begin()) return false;
            --it;
            return c >= it->first && c <= it->second;
        };
        if (inRanges(codepoint)) return true;
        // a symbol font maps U+0020..U+00FF at U+F020..U+F0FF (as Windows does)
        return symbol && codepoint >= 0x20 && codepoint <= 0xFF && inRanges(0xF000 + codepoint);
    }

    // ---------- UTF-8 ----------

    std::vector<uint32_t> utf8CodePoints(const std::string& text) {
        std::vector<uint32_t> out;
        const auto* p = reinterpret_cast<const uint8_t*>(text.data());
        const size_t n = text.size();
        for (size_t i = 0; i < n;) {
            const uint8_t c = p[i];
            uint32_t cp = 0xFFFD;
            size_t length = 1;
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < n && (p[i + 1] & 0xC0) == 0x80) {
                cp = (uint32_t)(c & 0x1F) << 6 | (p[i + 1] & 0x3F);
                length = cp >= 0x80 ? 2 : 1;
                if (cp < 0x80) cp = 0xFFFD;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
                cp = (uint32_t)(c & 0x0F) << 12 | (uint32_t)(p[i + 1] & 0x3F) << 6 | (p[i + 2] & 0x3F);
                length = 3;
                if (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < n && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80 &&
                       (p[i + 3] & 0xC0) == 0x80) {
                cp = (uint32_t)(c & 0x07) << 18 | (uint32_t)(p[i + 1] & 0x3F) << 12 | (uint32_t)(p[i + 2] & 0x3F) << 6 | (p[i + 3] & 0x3F);
                length = 4;
                if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
            }
            out.push_back(cp);
            i += length;
        }
        return out;
    }

    void appendUtf8(std::string& out, uint32_t c) {
        if (c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) c = 0xFFFD;
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }

    bool isIgnorableForCoverage(uint32_t c) {
        return c < 0x21 || (c >= 0x7F && c <= 0xA0) || c == 0x00AD || c == 0x034F || c == 0x1680 || (c >= 0x2000 && c <= 0x200F) ||
               (c >= 0x2028 && c <= 0x202F) || (c >= 0x205F && c <= 0x206F) || c == 0x3000 || (c >= 0xFE00 && c <= 0xFE0F) ||
               c == 0xFEFF || (c >= 0xE0000 && c <= 0xE0FFF);
    }

}
