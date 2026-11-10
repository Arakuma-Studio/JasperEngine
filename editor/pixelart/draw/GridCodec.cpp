// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GridCodec.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        const char kHexDigits[] = "0123456789abcdef";

        int hexValue(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        std::string rgbaHex(const PixelRgba& c) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
            return buffer;
        }

        bool parseRgbaHex(std::string_view text, PixelRgba& out) {
            if (!text.empty() && text[0] == '#') text.remove_prefix(1);
            if (text.size() != 6 && text.size() != 8) return false;
            uint8_t v[4] = {0, 0, 0, 255};
            for (size_t i = 0; i < text.size() / 2; i++) {
                const int hi = hexValue(text[i * 2]), lo = hexValue(text[i * 2 + 1]);
                if (hi < 0 || lo < 0) return false;
                v[i] = static_cast<uint8_t>(hi * 16 + lo);
            }
            out = {v[0], v[1], v[2], v[3]};
            return true;
        }

        uint32_t packRgba(const PixelRgba& c) {
            return (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | uint32_t(c[3]);
        }

        PixelProblem badChar(size_t row, size_t column, char c, GridEncoding encoding) {
            std::string shown = c == '"' ? std::string("\\\"") : std::string(1, c);
            return {"bad_grid_char", "rows[" + std::to_string(row) + "]",
                    "rows[" + std::to_string(row) + "] column " + std::to_string(column) + " holds '" + shown + "', not a " +
                        gridEncodingName(encoding) + " character",
                    "valid " + std::string(gridEncodingName(encoding)) + " characters: " + alphabetHint(encoding)};
        }

    }

    const char* gridEncodingName(GridEncoding encoding) {
        switch (encoding) {
            case GridEncoding::Auto: return "auto";
            case GridEncoding::Chars: return "chars";
            case GridEncoding::Hex2: return "hex2";
            case GridEncoding::Legend: return "legend";
            case GridEncoding::Hex8: return "hex8";
        }
        return "auto";
    }

    bool parseGridEncoding(std::string_view text, GridEncoding& out) {
        for (GridEncoding e : {GridEncoding::Auto, GridEncoding::Chars, GridEncoding::Hex2, GridEncoding::Legend, GridEncoding::Hex8}) {
            if (text == gridEncodingName(e)) {
                out = e;
                return true;
            }
        }
        return false;
    }

    char alphabetChar(int index) {
        if (index == 0) return '.';
        if (index >= 1 && index <= 9) return static_cast<char>('0' + index);
        if (index >= 10 && index <= 35) return static_cast<char>('a' + index - 10);
        if (index >= 36 && index <= 61) return static_cast<char>('A' + index - 36);
        return '?';
    }

    int alphabetIndex(char c) {
        if (c == '.' || c == '0') return 0;
        if (c >= '1' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'z') return c - 'a' + 10;
        if (c >= 'A' && c <= 'Z') return c - 'A' + 36;
        return -1;
    }

    std::string alphabetHint(GridEncoding encoding) {
        switch (encoding) {
            case GridEncoding::Hex2: return "two hex digits 00-ff per pixel, -- keeps";
            case GridEncoding::Hex8: return "eight hex digits rrggbbaa per pixel, -------- keeps";
            case GridEncoding::Legend: return "the legend's characters, '.' transparent, '-' or space keeps";
            default: return "'.' or '0' = 0, 1-9, a-z = 10-35, A-Z = 36-61, '-' or space keeps";
        }
    }

    EncodedGrid encodeRegion(const PixelImage& image, const IntRect& rect, GridEncoding want, std::span<const uint8_t> palette, int clearIndex) {
        EncodedGrid grid;
        const bool indexed = image.hasIndices();
        GridEncoding encoding = want;
        if (!indexed && (encoding == GridEncoding::Chars || encoding == GridEncoding::Hex2)) encoding = encoding == GridEncoding::Chars ? GridEncoding::Legend : GridEncoding::Hex8;
        if (encoding == GridEncoding::Auto) encoding = indexed ? GridEncoding::Chars : GridEncoding::Legend;

        if (encoding == GridEncoding::Chars || encoding == GridEncoding::Hex2) {
            bool seen[256] = {};
            int highest = 0;
            for (int y = rect.y; y < rect.y + rect.h; y++) {
                for (int x = rect.x; x < rect.x + rect.w; x++) {
                    const uint8_t i = image.indexAt(x, y);
                    seen[i] = true;
                    highest = std::max<int>(highest, i);
                }
            }
            if (encoding == GridEncoding::Chars && highest > kMaxAlphabetIndex) encoding = GridEncoding::Hex2;
            for (int y = rect.y; y < rect.y + rect.h; y++) {
                std::string row;
                row.reserve(static_cast<size_t>(rect.w) * (encoding == GridEncoding::Hex2 ? 2 : 1));
                for (int x = rect.x; x < rect.x + rect.w; x++) {
                    const uint8_t i = image.indexAt(x, y);
                    if (encoding == GridEncoding::Chars) {
                        row.push_back(alphabetChar(i));
                    } else {
                        row.push_back(kHexDigits[i >> 4]);
                        row.push_back(kHexDigits[i & 15]);
                    }
                }
                grid.rows.push_back(std::move(row));
            }
            for (int i = 0; i < 256; i++) {
                if (!seen[i] || i == clearIndex) continue;
                PixelRgba c{0, 0, 0, 0};
                if (static_cast<size_t>(i) * 4 + 3 < palette.size()) c = {palette[i * 4], palette[i * 4 + 1], palette[i * 4 + 2], palette[i * 4 + 3]};
                std::string hex = rgbaHex(c);
                if (c[3] == 255) hex.resize(7);
                grid.colors[std::to_string(i)] = hex;
            }
            grid.encoding = encoding;
            return grid;
        }

        // RGBA: legend in first-appearance order, row by row, until it runs out of characters
        std::map<uint32_t, int> slots;
        std::vector<PixelRgba> order;
        bool overflow = false;
        for (int y = rect.y; y < rect.y + rect.h && !overflow; y++) {
            for (int x = rect.x; x < rect.x + rect.w; x++) {
                const PixelRgba c = image.at(x, y);
                if (c[3] == 0) continue;
                const uint32_t key = packRgba(c);
                if (slots.count(key)) continue;
                if (static_cast<int>(order.size()) >= kMaxAlphabetIndex) {
                    overflow = true;
                    break;
                }
                slots[key] = static_cast<int>(order.size()) + 1;
                order.push_back(c);
            }
        }
        if (encoding == GridEncoding::Legend && overflow) encoding = GridEncoding::Hex8;
        for (int y = rect.y; y < rect.y + rect.h; y++) {
            std::string row;
            row.reserve(static_cast<size_t>(rect.w) * (encoding == GridEncoding::Hex8 ? 8 : 1));
            for (int x = rect.x; x < rect.x + rect.w; x++) {
                const PixelRgba c = image.at(x, y);
                if (encoding == GridEncoding::Legend) {
                    row.push_back(c[3] == 0 ? '.' : alphabetChar(slots[packRgba(c)]));
                } else {
                    for (int k = 0; k < 4; k++) {
                        row.push_back(kHexDigits[c[k] >> 4]);
                        row.push_back(kHexDigits[c[k] & 15]);
                    }
                }
            }
            grid.rows.push_back(std::move(row));
        }
        if (encoding == GridEncoding::Legend) {
            for (size_t i = 0; i < order.size(); i++) grid.colors[std::string(1, alphabetChar(static_cast<int>(i) + 1))] = rgbaHex(order[i]);
        }
        grid.encoding = encoding;
        return grid;
    }

    bool decodeRows(std::span<const std::string> rows, GridEncoding encoding, const nlohmann::json& legend, bool spriteIndexed,
                    PixelPatch& out, PixelProblem& problem) {
        if (encoding == GridEncoding::Auto) {
            if (legend.is_object() && !legend.empty()) encoding = GridEncoding::Legend;
            else encoding = spriteIndexed ? GridEncoding::Chars : GridEncoding::Hex8;
        }
        if (rows.size() > static_cast<size_t>(kMaxGridRows)) {
            problem = {"region_too_large", "rows", "the grid has " + std::to_string(rows.size()) + " rows, above 4096", "paste at most 4096 rows per op"};
            return false;
        }
        const int perPixel = encoding == GridEncoding::Hex2 ? 2 : encoding == GridEncoding::Hex8 ? 8 : 1;
        // the legend's colours, by character
        PixelRgba legendColor[128] = {};
        bool legendHas[128] = {};
        if (encoding == GridEncoding::Legend) {
            if (!legend.is_object()) {
                problem = {"bad_op_field", "legend", "legend encoding needs a legend object {char: \"#rrggbbaa\"}", "pass legend, or encoding chars"};
                return false;
            }
            for (auto it = legend.begin(); it != legend.end(); ++it) {
                const std::string& key = it.key();
                PixelRgba c;
                if (key.size() != 1 || alphabetIndex(key[0]) <= 0 || !it->is_string() || !parseRgbaHex(it->get<std::string>(), c)) {
                    problem = {"bad_op_field", "legend", "legend entry \"" + key + "\" must map one of 1-9, a-z, A-Z to \"#rrggbb\" or \"#rrggbbaa\"",
                               "legend characters are the alphabet without '.', which is transparent"};
                    return false;
                }
                legendColor[static_cast<unsigned char>(key[0])] = c;
                legendHas[static_cast<unsigned char>(key[0])] = true;
            }
        }
        int width = 0;
        for (size_t r = 0; r < rows.size(); r++) {
            const size_t pixels = (rows[r].size() + perPixel - 1) / perPixel;
            if (pixels > static_cast<size_t>(kMaxGridRowPixels)) {
                problem = {"grid_too_wide", "rows[" + std::to_string(r) + "]", "rows[" + std::to_string(r) + "] holds " + std::to_string(pixels) + " pixels, above 4096",
                           "rows hold at most 4096 pixels; paste wide areas in parts"};
                return false;
            }
            width = std::max(width, static_cast<int>(pixels));
        }
        out = PixelPatch();
        out.w = width;
        out.h = static_cast<int>(rows.size());
        out.indexed = encoding == GridEncoding::Chars || encoding == GridEncoding::Hex2;
        const size_t count = static_cast<size_t>(out.w) * out.h;
        out.keep.assign(count, 1);
        if (out.indexed) out.index.assign(count, 0);
        else out.rgba.assign(count, PixelRgba{0, 0, 0, 0});
        for (size_t r = 0; r < rows.size(); r++) {
            const std::string& row = rows[r];
            if (perPixel > 1 && row.size() % perPixel != 0) {
                problem = {"bad_grid_char", "rows[" + std::to_string(r) + "]",
                           "rows[" + std::to_string(r) + "] has " + std::to_string(row.size()) + " characters, not a multiple of " + std::to_string(perPixel),
                           "valid " + std::string(gridEncodingName(encoding)) + " characters: " + alphabetHint(encoding)};
                return false;
            }
            for (size_t p = 0; p * perPixel < row.size(); p++) {
                const size_t at = r * out.w + p;
                const std::string_view cell(row.data() + p * perPixel, static_cast<size_t>(perPixel));
                if (cell.find_first_not_of(perPixel == 1 ? "- " : "-") == std::string_view::npos) continue;   // keep
                out.keep[at] = 0;
                switch (encoding) {
                    case GridEncoding::Chars: {
                        const int index = alphabetIndex(cell[0]);
                        if (index < 0) {
                            problem = badChar(r, p, cell[0], encoding);
                            return false;
                        }
                        out.index[at] = static_cast<uint8_t>(index);
                        break;
                    }
                    case GridEncoding::Hex2: {
                        const int hi = hexValue(cell[0]), lo = hexValue(cell[1]);
                        if (hi < 0 || lo < 0) {
                            problem = badChar(r, p * 2 + (hi < 0 ? 0 : 1), hi < 0 ? cell[0] : cell[1], encoding);
                            return false;
                        }
                        out.index[at] = static_cast<uint8_t>(hi * 16 + lo);
                        break;
                    }
                    case GridEncoding::Legend: {
                        const unsigned char c = static_cast<unsigned char>(cell[0]);
                        if (c == '.') {
                            out.rgba[at] = {0, 0, 0, 0};
                        } else if (c < 128 && legendHas[c]) {
                            out.rgba[at] = legendColor[c];
                        } else {
                            problem = badChar(r, p, cell[0], encoding);
                            std::string known;
                            for (int k = 0; k < 128; k++) {
                                if (legendHas[k]) known.push_back(static_cast<char>(k));
                            }
                            problem.fix = "valid legend characters here: '.' (transparent), '" + known + "' (the legend), '-' or space keeps";
                            return false;
                        }
                        break;
                    }
                    case GridEncoding::Hex8:
                    default: {
                        uint8_t v[4];
                        for (int k = 0; k < 4; k++) {
                            const int hi = hexValue(cell[k * 2]), lo = hexValue(cell[k * 2 + 1]);
                            if (hi < 0 || lo < 0) {
                                problem = badChar(r, p * 8 + k * 2 + (hi < 0 ? 0 : 1), hi < 0 ? cell[k * 2] : cell[k * 2 + 1], GridEncoding::Hex8);
                                return false;
                            }
                            v[k] = static_cast<uint8_t>(hi * 16 + lo);
                        }
                        out.rgba[at] = {v[0], v[1], v[2], v[3]};
                        break;
                    }
                }
            }
        }
        return true;
    }

}
