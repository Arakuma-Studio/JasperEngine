// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GIMP palette reader follows the "living spec" of GIMP's app/core/gimppalette-load.c as
// Pixelorama reads it (src/Autoload/Palettes.gd, MIT, Copyright (c) 2019-present Orama
// Interactive and contributors), including its empty-slot tag; the other layouts are the
// published file formats.

#include "PaletteFormats.h"

#include "pixelart/aseprite/AsepriteDocument.h"

#include "widget/SwatchLibrary.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>

// stb_image_write's DEFLATE (engine/libs/stb/stb_image_write.c), as editor/util/PngWriter.cpp uses
// it: the PNG strip is encoded here with a fixed compression level, so the bytes do not depend on
// stbi_write_png_compression_level, which other editor code changes
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

// file-local, in a namespace of their own: a unity build merges this file with the other pixel-art
// sources, whose helpers live in jasperengine::editor::pixelart
namespace jasperengine::editor::pixelart::formatsio {
namespace {

    namespace px = jasperengine::pixelart;

    constexpr const char* kSupportedFormats =
        "GIMP .gpl, Lospec .hex, JASC and Microsoft RIFF .pal, Paint.NET .txt, Adobe Swatch Exchange .ase, images "
        "(.png, .gif, .jpg, .bmp, .tga) and .colors swatch libraries";
    // Pixelorama's name for a slot it leaves empty; read as #00000000 so later indices stay put
    constexpr const char* kEmptySlotTag = "PixeloramaEmptySlot";
    constexpr int kMaxImageSide = 8192;
    constexpr int kMaxSwatchSize = 64;
    constexpr int kPaintNetColors = 96;

    PixelProblem makeProblem(const std::string& kind, const std::string& message, const std::string& fix, const std::string& field = "") {
        PixelProblem problem;
        problem.kind = kind;
        problem.field = field;
        problem.message = message;
        problem.fix = fix;
        return problem;
    }

    std::string utf8(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::string lowerAscii(std::string_view text) {
        std::string out(text);
        for (char& c : out) c = (char)std::tolower((unsigned char)c);
        return out;
    }

    bool isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }

    std::string_view trim(std::string_view text) {
        while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
        while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
        return text;
    }

    bool startsWith(std::string_view text, std::string_view prefix) {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool isHexDigits(std::string_view text) {
        for (char c : text) {
            if (!std::isxdigit((unsigned char)c)) return false;
        }
        return !text.empty();
    }

    // the lines of a text file, without their CR of a CRLF
    std::vector<std::string_view> splitLines(std::string_view text) {
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            start = end + 1;
        }
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        return lines;
    }

    std::string_view textOf(std::span<const uint8_t> bytes) {
        std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (startsWith(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
        return text;
    }

    bool looksBinary(std::span<const uint8_t> bytes) {
        const size_t probe = std::min<size_t>(bytes.size(), 4096);
        for (size_t i = 0; i < probe; i++) {
            if (bytes[i] == 0) return true;
        }
        return false;
    }

    // the next whitespace-separated token from `pos` on
    std::string_view nextToken(std::string_view line, size_t& pos) {
        while (pos < line.size() && isSpace(line[pos])) pos++;
        const size_t start = pos;
        while (pos < line.size() && !isSpace(line[pos])) pos++;
        return line.substr(start, pos - start);
    }

    bool parseByte(std::string_view token, int& out) {
        if (token.empty() || token.size() > 3) return false;
        int value = 0;
        for (char c : token) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (value > 255) return false;
        out = value;
        return true;
    }

    bool parseCount(std::string_view token, int& out) {
        token = trim(token);
        if (token.empty() || token.size() > 6) return false;
        int value = 0;
        for (char c : token) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        out = value;
        return true;
    }

    uint8_t unitToByte(float value) {
        const double v = std::isfinite(value) ? std::clamp((double)value, 0.0, 1.0) : 0.0;
        return (uint8_t)std::floor(v * 255.0 + 0.5);
    }

    std::string hex6(px::Rgba8 color) {
        char text[8];
        std::snprintf(text, sizeof(text), "%02x%02x%02x", color.r, color.g, color.b);
        return text;
    }

    // a colour name worth keeping: not a placeholder, and not just the colour's own hex (Lospec
    // and Pixelorama name every colour that way)
    bool meaningfulName(std::string_view name, px::Rgba8 color) {
        name = trim(name);
        if (name.empty() || name == "Untitled") return false;
        if (!name.empty() && name.front() == '#') name.remove_prefix(1);
        return !(name.size() == 6 && lowerAscii(name) == hex6(color));
    }

    std::string singleLine(const std::string& text) {
        std::string out = text;
        for (char& c : out) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        }
        return std::string(trim(out));
    }

    // UTF-16BE code units (Adobe Swatch Exchange names) to UTF-8, up to the terminating 0
    std::string utf16ToUtf8(const std::vector<uint16_t>& units) {
        std::string out;
        auto append = [&out](uint32_t cp) {
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        };
        for (size_t i = 0; i < units.size(); i++) {
            const uint32_t unit = units[i];
            if (unit == 0) break;
            if (unit >= 0xD800 && unit < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
                append(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
                i++;
            } else if (unit >= 0xD800 && unit < 0xE000) {
                append(0xFFFD);
            } else {
                append(unit);
            }
        }
        return out;
    }

    std::vector<uint16_t> utf8ToUtf16(const std::string& text) {
        std::vector<uint16_t> units;
        size_t i = 0;
        while (i < text.size()) {
            const uint8_t lead = (uint8_t)text[i];
            uint32_t cp = 0xFFFD;
            size_t length = 1;
            if (lead < 0x80) {
                cp = lead;
            } else if ((lead & 0xE0) == 0xC0) {
                length = 2;
            } else if ((lead & 0xF0) == 0xE0) {
                length = 3;
            } else if ((lead & 0xF8) == 0xF0) {
                length = 4;
            }
            if (length > 1) {
                if (i + length > text.size()) {
                    length = 1;
                } else {
                    uint32_t value = lead & (0xFF >> (length + 1));
                    bool valid = true;
                    for (size_t k = 1; k < length; k++) {
                        const uint8_t next = (uint8_t)text[i + k];
                        if ((next & 0xC0) != 0x80) valid = false;
                        value = (value << 6) | (next & 0x3F);
                    }
                    if (valid) cp = value;
                    else length = 1;
                }
            }
            i += length;
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                cp -= 0x10000;
                units.push_back((uint16_t)(0xD800 + (cp >> 10)));
                units.push_back((uint16_t)(0xDC00 + (cp & 0x3FF)));
            } else {
                units.push_back((uint16_t)(cp > 0xFFFF ? 0xFFFD : cp));
            }
        }
        return units;
    }

    // ---- reading

    struct Reading {
        px::Palette palette;
        std::vector<PixelProblem> warnings;
        PixelProblem error;
        std::string fileName;
        // an image with more colours than a palette holds: the count stopped at kMaxColors + 1
        bool countStopped = false;

        bool parseError(const std::string& where, const std::string& message) {
            error = makeProblem("parse_error", fileName + ": " + where + ": " + message,
                                "Export the palette again from the program that made it, or fix the file at that place");
            return false;
        }

        void warn(const std::string& kind, const std::string& message, const std::string& fix = "") {
            warnings.push_back(makeProblem(kind, message, fix));
        }

        void addColor(px::Rgba8 color, std::string_view name = {}) {
            if (meaningfulName(name, color)) palette.names[palette.count()] = std::string(trim(name));
            palette.colors.push_back(color);
        }
    };

    std::string lineAt(size_t index) {
        return "line " + std::to_string(index + 1);
    }

    std::string byteAt(size_t offset) {
        return "byte " + std::to_string(offset);
    }

    void appendDescription(std::string& description, std::string_view line) {
        line = trim(line);
        if (line.empty()) return;
        if (!description.empty()) description += '\n';
        description += line;
    }

    bool readGpl(std::string_view text, Reading& r) {
        const std::vector<std::string_view> lines = splitLines(text);
        if (lines.empty() || !startsWith(trim(lines[0]), "GIMP Palette")) return r.parseError(lineAt(0), "a GIMP palette starts with 'GIMP Palette'");
        bool alpha = false;
        std::string description;
        for (size_t i = 1; i < lines.size(); i++) {
            const std::string_view line = trim(lines[i]);
            if (line.empty()) continue;
            if (line.front() == '#') {
                // Pixelorama and Lospec keep the name and description in comments
                const std::string_view comment = trim(line.substr(1));
                if (startsWith(comment, "Palette Name:")) {
                    r.palette.name = std::string(trim(comment.substr(13)));
                } else if (startsWith(comment, "Description:")) {
                    appendDescription(description, comment.substr(12));
                } else if (!startsWith(comment, "Colors:")) {
                    appendDescription(description, comment);
                }
                continue;
            }
            if (!std::isdigit((unsigned char)line.front())) {
                const size_t colon = line.find(':');
                if (colon == std::string_view::npos) return r.parseError(lineAt(i), "expected 'red green blue name', 'Name:' or 'Columns:'");
                const std::string_view key = trim(line.substr(0, colon));
                const std::string_view value = trim(line.substr(colon + 1));
                if (key == "Name") {
                    r.palette.name = std::string(value);
                } else if (key == "Columns") {
                    int columns = 0;
                    if (!parseCount(value, columns)) {
                        r.warn("columns_ignored", lineAt(i) + ": 'Columns: " + std::string(value) + "' is not a number; the palette shows 8 to a row");
                    } else if (columns > px::Palette::kMaxColumns) {
                        r.palette.columns = px::Palette::kMaxColumns;
                        r.warn("columns_clamped", "the file asks for " + std::to_string(columns) + " columns; the palette shows " +
                               std::to_string(px::Palette::kMaxColumns));
                    } else if (columns > 0) {
                        r.palette.columns = columns;
                    }
                } else if (key == "Channels") {
                    // Aseprite's extension: a fourth number per colour, its alpha
                    alpha = value == "RGBA";
                }
                continue;
            }
            size_t pos = 0;
            int channels[4] = {0, 0, 0, 255};
            const int wanted = alpha ? 4 : 3;
            for (int c = 0; c < wanted; c++) {
                if (!parseByte(nextToken(line, pos), channels[c])) {
                    return r.parseError(lineAt(i), std::string("expected ") + (alpha ? "'red green blue alpha name'" : "'red green blue name'") +
                                        " with numbers from 0 to 255: '" + std::string(line) + "'");
                }
            }
            const std::string_view name = trim(line.substr(pos));
            if (name == kEmptySlotTag) {
                r.palette.colors.push_back(px::Rgba8{0, 0, 0, 0});
                continue;
            }
            r.addColor(px::Rgba8{(uint8_t)channels[0], (uint8_t)channels[1], (uint8_t)channels[2], (uint8_t)channels[3]}, name);
        }
        r.palette.description = description;
        return true;
    }

    bool readHex(std::string_view text, Reading& r) {
        const std::vector<std::string_view> lines = splitLines(text);
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string_view line = trim(lines[i]);
            if (line.empty()) continue;
            px::Rgba8 color;
            if (!px::parseHex(line, color)) return r.parseError(lineAt(i), "not a 6- or 8-digit hex colour: '" + std::string(line) + "'");
            r.palette.colors.push_back(color);
        }
        return true;
    }

    bool readJasc(std::string_view text, Reading& r) {
        const std::vector<std::string_view> lines = splitLines(text);
        if (lines.empty() || trim(lines[0]) != "JASC-PAL") return r.parseError(lineAt(0), "a JASC palette starts with 'JASC-PAL'");
        if (lines.size() < 2 || trim(lines[1]) != "0100") {
            return r.parseError(lineAt(1), "expected the version '0100'" + (lines.size() < 2 ? std::string() : ", found '" + std::string(trim(lines[1])) + "'"));
        }
        int count = 0;
        if (lines.size() < 3 || !parseCount(lines[2], count)) return r.parseError(lineAt(2), "expected the number of colours");
        size_t i = 3;
        for (int n = 0; n < count; n++) {
            while (i < lines.size() && trim(lines[i]).empty()) i++;
            if (i >= lines.size()) {
                return r.parseError(lineAt(lines.size()), "the file ends after " + std::to_string(n) + " of its " + std::to_string(count) + " colours");
            }
            const std::string_view line = trim(lines[i]);
            size_t pos = 0;
            int channels[3] = {0, 0, 0};
            for (int c = 0; c < 3; c++) {
                if (!parseByte(nextToken(line, pos), channels[c])) {
                    return r.parseError(lineAt(i), "expected 'red green blue' with numbers from 0 to 255: '" + std::string(line) + "'");
                }
            }
            r.palette.colors.push_back(px::Rgba8{(uint8_t)channels[0], (uint8_t)channels[1], (uint8_t)channels[2], 255});
            i++;
        }
        return true;
    }

    bool readPaintNet(std::string_view text, Reading& r) {
        const std::vector<std::string_view> lines = splitLines(text);
        std::string description;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string_view line = trim(lines[i]);
            if (line.empty()) continue;
            if (line.front() == ';') {
                // Lospec's .txt names the palette in its comments
                const std::string_view comment = trim(line.substr(1));
                if (startsWith(comment, "Palette Name:")) r.palette.name = std::string(trim(comment.substr(13)));
                else if (startsWith(comment, "Description:")) appendDescription(description, comment.substr(12));
                continue;
            }
            if (!isHexDigits(line) || (line.size() != 8 && line.size() != 6)) {
                return r.parseError(lineAt(i), "expected an AARRGGBB hex colour: '" + std::string(line) + "'");
            }
            px::Rgba8 color;
            // AARRGGBB: the alpha comes first
            const std::string reordered = line.size() == 8 ? std::string(line.substr(2)) + std::string(line.substr(0, 2)) : std::string(line);
            px::parseHex(reordered, color);
            r.palette.colors.push_back(color);
        }
        r.palette.description = description;
        return true;
    }

    uint32_t readLe(std::span<const uint8_t> bytes, size_t offset, int size) {
        uint32_t value = 0;
        for (int i = size - 1; i >= 0; i--) value = (value << 8) | bytes[offset + i];
        return value;
    }

    uint32_t readBe(std::span<const uint8_t> bytes, size_t offset, int size) {
        uint32_t value = 0;
        for (int i = 0; i < size; i++) value = (value << 8) | bytes[offset + i];
        return value;
    }

    bool readRiff(std::span<const uint8_t> bytes, Reading& r) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "PAL ", 4) != 0) {
            return r.parseError(byteAt(0), "a RIFF palette starts with 'RIFF', its size and 'PAL '");
        }
        size_t pos = 12;
        while (pos + 8 <= bytes.size()) {
            const uint32_t size = readLe(bytes, pos + 4, 4);
            const size_t data = pos + 8;
            if (std::memcmp(bytes.data() + pos, "data", 4) != 0) {
                // chunks are padded to an even size
                pos = data + (size_t)size + (size & 1u);
                continue;
            }
            if (size < 4 || data + 4 > bytes.size()) return r.parseError(byteAt(data), "the 'data' chunk is too short for its LOGPALETTE header");
            const uint32_t version = readLe(bytes, data, 2);
            if (version != 0x0300) {
                char text[16];
                std::snprintf(text, sizeof(text), "0x%04x", version);
                return r.parseError(byteAt(data), std::string("palette version ") + text + ", expected 0x0300");
            }
            const uint32_t count = readLe(bytes, data + 2, 2);
            const size_t end = data + 4 + (size_t)count * 4;
            if (end > bytes.size()) {
                return r.parseError(byteAt(bytes.size()), "the file is truncated: " + std::to_string(count) + " colours need " +
                                    std::to_string(end) + " bytes, it has " + std::to_string(bytes.size()));
            }
            for (uint32_t i = 0; i < count; i++) {
                // PALETTEENTRY: red, green, blue, flags
                const size_t entry = data + 4 + (size_t)i * 4;
                r.palette.colors.push_back(px::Rgba8{bytes[entry], bytes[entry + 1], bytes[entry + 2], 255});
            }
            return true;
        }
        return r.parseError(byteAt(bytes.size()), "no 'data' chunk");
    }

    // CIELAB under D50 (what Adobe stores) to sRGB, through the Bradford-adapted matrix
    px::Rgba8 labD50ToRgb(double l, double a, double b) {
        const double fy = (l + 16.0) / 116.0;
        const double fx = fy + a / 500.0;
        const double fz = fy - b / 200.0;
        auto inverse = [](double t) {
            const double delta = 6.0 / 29.0;
            return t > delta ? t * t * t : 3.0 * delta * delta * (t - 4.0 / 29.0);
        };
        const double x = 0.96422 * inverse(fx), y = inverse(fy), z = 0.82521 * inverse(fz);
        const double lr = 3.1338561 * x - 1.6168667 * y - 0.4906146 * z;
        const double lg = -0.9787684 * x + 1.9161415 * y + 0.0334540 * z;
        const double lb = 0.0719453 * x - 0.2289914 * y + 1.4052427 * z;
        return px::Rgba8{px::linearToSrgb((float)lr), px::linearToSrgb((float)lg), px::linearToSrgb((float)lb), 255};
    }

    bool readAse(std::span<const uint8_t> bytes, Reading& r) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "ASEF", 4) != 0) {
            return r.parseError(byteAt(0), "neither Adobe Swatch Exchange (it starts with 'ASEF') nor an Aseprite file");
        }
        const uint32_t major = readBe(bytes, 4, 2), minor = readBe(bytes, 6, 2);
        if (major != 1) {
            return r.parseError(byteAt(4), "Adobe Swatch Exchange version " + std::to_string(major) + "." + std::to_string(minor) + "; version 1 is read");
        }
        const uint32_t blocks = readBe(bytes, 8, 4);
        struct Group { std::string name; std::vector<uint16_t> colors; };
        std::vector<Group> groups;
        bool inGroup = false;
        int cmyk = 0, lab = 0;
        size_t pos = 12;
        uint32_t blocksRead = 0;
        for (; blocksRead < blocks && pos < bytes.size(); blocksRead++) {
            if (pos + 6 > bytes.size()) return r.parseError(byteAt(pos), "the file is truncated inside a block header");
            const uint32_t type = readBe(bytes, pos, 2);
            const uint32_t length = readBe(bytes, pos + 2, 4);
            const size_t start = pos + 6;
            const size_t end = start + (size_t)length;
            if (end > bytes.size()) {
                return r.parseError(byteAt(pos), "the file is truncated: the block needs " + std::to_string(length) + " bytes, " +
                                    std::to_string(bytes.size() - start) + " are left");
            }
            size_t at = start;
            auto readName = [&](std::string& name) {
                if (at + 2 > end) return false;
                const uint32_t units = readBe(bytes, at, 2);
                at += 2;
                if (at + (size_t)units * 2 > end) return false;
                std::vector<uint16_t> text(units);
                for (uint32_t i = 0; i < units; i++) text[i] = (uint16_t)readBe(bytes, at + (size_t)i * 2, 2);
                at += (size_t)units * 2;
                name = utf16ToUtf8(text);
                return true;
            };
            if (type == 0xC001) {
                Group group;
                if (!readName(group.name)) return r.parseError(byteAt(start), "a group name runs past its block");
                groups.push_back(std::move(group));
                inGroup = true;
            } else if (type == 0xC002) {
                inGroup = false;
            } else if (type == 0x0001) {
                std::string name;
                if (!readName(name)) return r.parseError(byteAt(start), "a colour name runs past its block");
                if (at + 4 > end) return r.parseError(byteAt(at), "the colour has no colour model");
                const std::string model(reinterpret_cast<const char*>(bytes.data() + at), 4);
                at += 4;
                const int values = model == "RGB " || model == "LAB " ? 3 : model == "CMYK" ? 4 : model == "Gray" ? 1 : 0;
                if (values == 0) return r.parseError(byteAt(at - 4), "unknown colour model '" + model + "'");
                if (at + (size_t)values * 4 > end) return r.parseError(byteAt(at), "the colour's values run past its block");
                float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int i = 0; i < values; i++) {
                    const uint32_t raw = readBe(bytes, at + (size_t)i * 4, 4);
                    std::memcpy(&v[i], &raw, 4);
                    if (!std::isfinite(v[i])) v[i] = 0.0f;
                }
                px::Rgba8 color;
                if (model == "RGB ") {
                    color = px::Rgba8{unitToByte(v[0]), unitToByte(v[1]), unitToByte(v[2]), 255};
                } else if (model == "Gray") {
                    const uint8_t grey = unitToByte(v[0]);
                    color = px::Rgba8{grey, grey, grey, 255};
                } else if (model == "CMYK") {
                    const float k = 1.0f - v[3];
                    color = px::Rgba8{unitToByte((1.0f - v[0]) * k), unitToByte((1.0f - v[1]) * k), unitToByte((1.0f - v[2]) * k), 255};
                    cmyk++;
                } else {
                    // L is stored as 0..1
                    color = labD50ToRgb((double)v[0] * 100.0, v[1], v[2]);
                    lab++;
                }
                if (inGroup && !groups.empty()) groups.back().colors.push_back((uint16_t)std::min(r.palette.count(), 65535));
                r.addColor(color, name);
            }
            pos = end;
        }
        if (blocksRead < blocks) {
            r.warn("truncated", "the header announces " + std::to_string(blocks) + " blocks; the file holds " + std::to_string(blocksRead));
        }
        if (cmyk > 0) {
            r.warn("cmyk_converted", std::to_string(cmyk) + " CMYK colour" + (cmyk == 1 ? " was" : "s were") +
                   " converted to RGB naively, without a colour profile (R = 255 (1 - C)(1 - K)); they can differ from what Photoshop shows",
                   "export the swatches as RGB for exact colours");
        }
        if (lab > 0) {
            r.warn("lab_converted", std::to_string(lab) + " Lab colour" + (lab == 1 ? " was" : "s were") +
                   " converted to sRGB (D50, Bradford), without a colour profile", "export the swatches as RGB for exact colours");
        }
        // the groups become ramps, with unique names
        std::map<std::string, int> used;
        for (size_t g = 0; g < groups.size(); g++) {
            if (groups[g].colors.empty()) continue;
            if ((int)r.palette.ramps.size() >= px::Palette::kMaxRamps) {
                r.warn("ramps_dropped", "the file has more than 64 colour groups; the palette keeps the first 64 as ramps");
                break;
            }
            std::string name = std::string(trim(groups[g].name));
            if (name.empty()) name = "group " + std::to_string(g + 1);
            const std::string base = name;
            for (int n = 2; used.count(name); n++) name = base + " " + std::to_string(n);
            used[name] = 1;
            px::PaletteRamp ramp;
            ramp.name = name;
            ramp.colors = groups[g].colors;
            r.palette.ramps.push_back(std::move(ramp));
        }
        return true;
    }

    bool readImage(std::span<const uint8_t> bytes, Reading& r) {
        const int length = (int)std::min<size_t>(bytes.size(), (size_t)0x7fffffff);
        int width = 0, height = 0, components = 0;
        if (!stbi_info_from_memory(bytes.data(), length, &width, &height, &components)) {
            const char* reason = stbi_failure_reason();
            return r.parseError(byteAt(0), std::string("not a readable image") + (reason ? std::string(" (") + reason + ")" : std::string()));
        }
        if (width > kMaxImageSide || height > kMaxImageSide) {
            return r.parseError(byteAt(0), "the image is " + std::to_string(width) + "x" + std::to_string(height) + "; a palette image is at most " +
                                std::to_string(kMaxImageSide) + " pixels per side");
        }
        unsigned char* pixels = stbi_load_from_memory(bytes.data(), length, &width, &height, &components, 4);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            return r.parseError(byteAt(0), std::string("the image does not decode") + (reason ? std::string(" (") + reason + ")" : std::string()));
        }
        auto at = [&](int x, int y) {
            const unsigned char* p = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            // a transparent pixel has no colour: every one is the same empty slot
            return p[3] == 0 ? px::Rgba8{0, 0, 0, 0} : px::Rgba8{p[0], p[1], p[2], p[3]};
        };

        // a swatch strip: square cells side by side, each one colour (duplicates kept, so the
        // indices are the cells')
        bool strip = height > 0 && width % height == 0;
        if (strip) {
            const int cells = width / height;
            for (int cell = 0; cell < cells && strip; cell++) {
                const px::Rgba8 first = at(cell * height, 0);
                for (int y = 0; y < height && strip; y++) {
                    for (int x = cell * height; x < (cell + 1) * height; x++) {
                        if (at(x, y) != first) {
                            strip = false;
                            break;
                        }
                    }
                }
            }
        }
        if (strip) {
            for (int cell = 0; cell < width / height; cell++) r.palette.colors.push_back(at(cell * height, 0));
        } else {
            // the unique visible colours in scan order, up to one more than a palette holds (a
            // photo has millions: counting them all would take a set of that size)
            std::unordered_set<uint32_t> seen;
            for (int y = 0; y < height && !r.countStopped; y++) {
                for (int x = 0; x < width; x++) {
                    const px::Rgba8 color = at(x, y);
                    if (color.a == 0) continue;
                    const uint32_t key = px::packRgb(color) | ((uint32_t)color.a << 24);
                    if (seen.insert(key).second) {
                        r.palette.colors.push_back(color);
                        if (r.palette.count() > px::Palette::kMaxColors) {
                            r.countStopped = true;
                            break;
                        }
                    }
                }
            }
        }
        stbi_image_free(pixels);
        if (r.palette.colors.empty()) return r.parseError(byteAt(0), "the image has no visible colour");
        return true;
    }

    // An Aseprite file's frame 0 palette (chunk 0x2019 with names and alpha, or the old 0x0004 /
    // 0x0011), through S44's codec; an indexed sprite's transparent index stays see-through
    bool readAseprite(std::span<const uint8_t> bytes, Reading& r) {
        jasperengine::editor::pixelart::aseprite::ReadOptions options;
        options.pixels = false;
        options.provenance = false;
        const jasperengine::editor::pixelart::aseprite::ReadResult read = jasperengine::editor::pixelart::aseprite::read(bytes, options);
        if (!read.ok()) {
            return r.parseError("byte " + std::to_string(read.error.offset), read.error.message);
        }
        const jasperengine::editor::pixelart::aseprite::Document& document = *read.document;
        for (const jasperengine::editor::pixelart::aseprite::PaletteEntry& entry : document.palette) {
            r.addColor(px::Rgba8{entry.color[0], entry.color[1], entry.color[2], entry.color[3]}, entry.name ? *entry.name : std::string());
        }
        if (document.mode == jasperengine::editor::pixelart::aseprite::ColorMode::Indexed && document.transparentIndex < r.palette.colors.size()) {
            r.palette.transparent = document.transparentIndex;
        }
        if (!document.paletteChanges.empty()) {
            r.warn("palette_changes", "the sprite changes its palette on a later frame; the first frame's palette is imported");
        }
        return true;
    }

    bool readSwatches(std::string_view text, Reading& r) {
        SwatchLibrary library;
        std::string error;
        if (!swatches::fromJson(std::string(text), library, error)) return r.parseError(byteAt(0), "not a swatch library: " + error);
        if (library.readOnly) r.warn("newer_version", "the swatch library is " + library.warning);
        r.palette.name = library.name;
        int clamped = 0;
        for (const Swatch& swatch : library.swatches) {
            const Vector4& linear = swatch.linear;
            if (swatch.hdr || linear.x > 1.0f || linear.y > 1.0f || linear.z > 1.0f) clamped++;
            r.addColor(jasperengine::pixelart::fromEngineLinear(linear), swatch.name);
        }
        if (clamped > 0) {
            r.warn("hdr_clamped", std::to_string(clamped) + " HDR swatch" + (clamped == 1 ? " was" : "es were") +
                   " clamped to 8-bit sRGB", "a palette holds display colours; keep HDR colours in the swatch library");
        }
        return true;
    }

    // ---- telling the formats apart

    bool isImageExtension(const std::string& extension) {
        static const char* const kImages[] = {".png", ".gif", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".pnm", ".ppm", ".pgm", ".hdr", ".pic"};
        for (const char* image : kImages) {
            if (extension == image) return true;
        }
        return false;
    }

    bool hasImageMagic(std::span<const uint8_t> bytes) {
        auto starts = [&](const char* magic, size_t size) { return bytes.size() >= size && std::memcmp(bytes.data(), magic, size) == 0; };
        return starts("\x89PNG\r\n\x1a\n", 8) || starts("GIF87a", 6) || starts("GIF89a", 6) || starts("\xFF\xD8\xFF", 3) ||
               starts("8BPS", 4) || (starts("BM", 2) && bytes.size() >= 26);
    }

    bool isAseprite(std::span<const uint8_t> bytes) {
        // the header's magic, a little-endian 0xA5E0 after the file size
        return bytes.size() >= 6 && bytes[4] == 0xE0 && bytes[5] == 0xA5;
    }

    // what the text's lines look like: only colours (AARRGGBB/RRGGBB, '#' optional) and ';' comments
    std::optional<PaletteFormat> sniffColorLines(std::string_view text, const std::string& extension) {
        int semicolons = 0, colors = 0, hashes = 0, sixes = 0;
        for (std::string_view line : splitLines(text)) {
            line = trim(line);
            if (line.empty()) continue;
            if (line.front() == ';') {
                semicolons++;
                continue;
            }
            const bool hash = line.front() == '#';
            if (hash) line.remove_prefix(1);
            if (!isHexDigits(line) || (line.size() != 6 && line.size() != 8)) return std::nullopt;
            colors++;
            if (hash) hashes++;
            if (line.size() == 6) sixes++;
        }
        if (colors == 0) return std::nullopt;
        // AARRGGBB and RRGGBBAA look the same: Paint.NET's comments or its extension decide
        if (semicolons > 0) return hashes == 0 ? std::optional<PaletteFormat>(PaletteFormat::PaintNetTxt) : std::nullopt;
        if (extension == ".txt" && hashes == 0 && sixes == 0) return PaletteFormat::PaintNetTxt;
        return PaletteFormat::Hex;
    }

    std::optional<PaletteFormat> sniffContent(std::span<const uint8_t> bytes, const std::string& extension) {
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "ASEF", 4) == 0) return PaletteFormat::AdobeAse;
        if (isAseprite(bytes) && (bytes.size() >= 128 || extension == ".ase" || extension == ".aseprite")) return PaletteFormat::Aseprite;
        if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "PAL ", 4) == 0) {
            return PaletteFormat::RiffPal;
        }
        if (hasImageMagic(bytes)) return PaletteFormat::Image;
        if (looksBinary(bytes)) return std::nullopt;
        const std::string_view text = textOf(bytes);
        const std::vector<std::string_view> lines = splitLines(text);
        std::string_view first;
        for (std::string_view line : lines) {
            if (!trim(line).empty()) {
                first = trim(line);
                break;
            }
        }
        if (startsWith(first, "GIMP Palette")) return PaletteFormat::Gpl;
        if (first == "JASC-PAL") return PaletteFormat::JascPal;
        if (startsWith(first, "{") && text.find("\"swatches\"") != std::string_view::npos) return PaletteFormat::SwatchLibrary;
        return sniffColorLines(text, extension);
    }

    std::optional<PaletteFormat> formatOfExtension(const std::string& extension, std::span<const uint8_t> bytes) {
        if (extension == ".gpl") return PaletteFormat::Gpl;
        if (extension == ".hex") return PaletteFormat::Hex;
        if (extension == ".pal") return looksBinary(bytes) ? PaletteFormat::RiffPal : PaletteFormat::JascPal;
        if (extension == ".txt") return PaletteFormat::PaintNetTxt;
        if (extension == ".ase") return PaletteFormat::AdobeAse;
        if (extension == ".aseprite") return PaletteFormat::Aseprite;
        if (extension == ".colors") return PaletteFormat::SwatchLibrary;
        if (isImageExtension(extension)) return PaletteFormat::Image;
        return std::nullopt;
    }

    // ---- writing

    void appendText(std::vector<uint8_t>& out, std::string_view text) {
        out.insert(out.end(), text.begin(), text.end());
    }

    void appendBe(std::vector<uint8_t>& out, uint32_t value, int size) {
        for (int i = size - 1; i >= 0; i--) out.push_back((uint8_t)(value >> (8 * i)));
    }

    void appendLe(std::vector<uint8_t>& out, uint32_t value, int size) {
        for (int i = 0; i < size; i++) out.push_back((uint8_t)(value >> (8 * i)));
    }

    // the warning of a format without alpha
    void warnAlpha(const px::Palette& palette, const char* format, std::vector<std::string>& warnings) {
        int dropped = 0;
        for (const px::Rgba8& color : palette.colors) {
            if (color.a != 255) dropped++;
        }
        if (dropped == 0) return;
        warnings.push_back(std::to_string(dropped) + (dropped == 1 ? " colour is" : " colours are") + " not opaque; " + format +
                           " has no alpha, so " + (dropped == 1 ? "it is" : "they are") + " written opaque");
    }

    std::vector<uint8_t> writeGpl(const px::Palette& palette, std::vector<std::string>& warnings) {
        std::string text = "GIMP Palette\nName: " + singleLine(palette.name) + "\nColumns: " + std::to_string(palette.columns) + "\n";
        for (std::string_view line : splitLines(palette.description)) {
            line = trim(line);
            if (!line.empty()) text += "# " + std::string(line) + "\n";
        }
        text += "#\n";
        int emptySlots = 0;
        for (int i = 0; i < palette.count(); i++) {
            const px::Rgba8 color = palette.colors[i];
            char line[32];
            if (color.a == 0) {
                // Pixelorama's empty slot: the index stays, the colour reads back as #00000000
                std::snprintf(line, sizeof(line), "%3d %3d %3d\t", 0, 0, 0);
                text += std::string(line) + kEmptySlotTag + "\n";
                if (color != px::Rgba8{0, 0, 0, 0}) emptySlots++;
                continue;
            }
            std::snprintf(line, sizeof(line), "%3d %3d %3d\t", color.r, color.g, color.b);
            const std::string name = singleLine(palette.colorName(i));
            text += std::string(line) + (name.empty() ? std::string("Untitled") : name) + "\n";
        }
        int partial = 0;
        for (const px::Rgba8& color : palette.colors) {
            if (color.a != 0 && color.a != 255) partial++;
        }
        if (partial > 0) {
            warnings.push_back(std::to_string(partial) + (partial == 1 ? " colour is" : " colours are") +
                               " partly transparent; GIMP palettes have no alpha, so " + (partial == 1 ? "it is" : "they are") + " written opaque");
        }
        if (emptySlots > 0) {
            warnings.push_back(std::to_string(emptySlots) + (emptySlots == 1 ? " transparent colour is" : " transparent colours are") +
                               " written as an empty slot and read back as #00000000");
        }
        std::vector<uint8_t> out;
        appendText(out, text);
        return out;
    }

    std::vector<uint8_t> writeHex(const px::Palette& palette) {
        std::string text;
        for (const px::Rgba8& color : palette.colors) text += px::toHex(color).substr(1) + "\n";
        std::vector<uint8_t> out;
        appendText(out, text);
        return out;
    }

    std::vector<uint8_t> writeJasc(const px::Palette& palette, std::vector<std::string>& warnings) {
        warnAlpha(palette, "JASC-PAL", warnings);
        // Paint Shop Pro writes CRLF
        std::string text = "JASC-PAL\r\n0100\r\n" + std::to_string(palette.count()) + "\r\n";
        for (const px::Rgba8& color : palette.colors) {
            text += std::to_string(color.r) + " " + std::to_string(color.g) + " " + std::to_string(color.b) + "\r\n";
        }
        std::vector<uint8_t> out;
        appendText(out, text);
        return out;
    }

    std::vector<uint8_t> writeRiff(const px::Palette& palette, std::vector<std::string>& warnings) {
        warnAlpha(palette, "RIFF PAL", warnings);
        const uint32_t count = (uint32_t)palette.count();
        std::vector<uint8_t> out;
        appendText(out, "RIFF");
        appendLe(out, 16 + 4 * count, 4);
        appendText(out, "PAL data");
        appendLe(out, 4 + 4 * count, 4);
        appendLe(out, 0x0300, 2);
        appendLe(out, count, 2);
        for (const px::Rgba8& color : palette.colors) {
            out.push_back(color.r);
            out.push_back(color.g);
            out.push_back(color.b);
            out.push_back(0);
        }
        return out;
    }

    std::vector<uint8_t> writePaintNet(const px::Palette& palette, std::vector<std::string>& warnings) {
        if (palette.count() > kPaintNetColors) {
            warnings.push_back("the palette has " + std::to_string(palette.count()) + " colours; Paint.NET reads the first 96");
        }
        std::string text =
            "; paint.net Palette File\r\n"
            "; Lines that start with a semicolon are comments\r\n"
            "; Colors are written as 8-digit hexadecimal numbers: aarrggbb\r\n"
            "; For example, this would specify green: FF00FF00\r\n"
            "; The palette must contain exactly 96 colors\r\n"
            "; If not enough colors are specified, the remainder will be filled with white\r\n";
        for (const px::Rgba8& color : palette.colors) {
            char line[16];
            std::snprintf(line, sizeof(line), "%02X%02X%02X%02X\r\n", color.a, color.r, color.g, color.b);
            text += line;
        }
        std::vector<uint8_t> out;
        appendText(out, text);
        return out;
    }

    void appendAseName(std::vector<uint8_t>& out, const std::string& name) {
        std::vector<uint16_t> units = utf8ToUtf16(name);
        units.push_back(0);
        appendBe(out, (uint32_t)units.size(), 2);
        for (uint16_t unit : units) appendBe(out, unit, 2);
    }

    std::vector<uint8_t> writeAse(const px::Palette& palette, std::vector<std::string>& warnings) {
        warnAlpha(palette, "Adobe Swatch Exchange", warnings);
        // groups hold runs of consecutive colours, so only the ramps that are such a run (and
        // share no colour with an earlier one) become groups; the colour list stays as it is
        std::vector<int> groupAt(palette.colors.size(), -1);
        std::vector<int> groupEnd(palette.ramps.size(), -1);
        std::vector<bool> taken(palette.colors.size(), false);
        int skipped = 0;
        for (size_t r = 0; r < palette.ramps.size(); r++) {
            const std::vector<uint16_t>& indices = palette.ramps[r].colors;
            bool run = !indices.empty() && indices.back() < palette.colors.size();
            for (size_t i = 1; run && i < indices.size(); i++) run = indices[i] == indices[i - 1] + 1;
            for (size_t i = 0; run && i < indices.size(); i++) run = !taken[indices[i]];
            if (!run) {
                skipped++;
                continue;
            }
            for (uint16_t index : indices) taken[index] = true;
            groupAt[indices.front()] = (int)r;
            groupEnd[r] = indices.back();
        }
        if (skipped > 0) {
            warnings.push_back(std::to_string(skipped) + (skipped == 1 ? " ramp is" : " ramps are") +
                               " not written: Adobe Swatch Exchange groups hold runs of consecutive colours");
        }
        int groups = 0;
        for (int end : groupEnd) groups += end >= 0 ? 1 : 0;

        std::vector<uint8_t> out;
        appendText(out, "ASEF");
        appendBe(out, 1, 2);
        appendBe(out, 0, 2);
        appendBe(out, (uint32_t)(palette.count() + 2 * groups), 4);
        int openGroup = -1;
        for (int i = 0; i < palette.count(); i++) {
            if (groupAt[i] >= 0) {
                openGroup = groupAt[i];
                std::vector<uint8_t> block;
                appendAseName(block, palette.ramps[openGroup].name);
                appendBe(out, 0xC001, 2);
                appendBe(out, (uint32_t)block.size(), 4);
                out.insert(out.end(), block.begin(), block.end());
            }
            const px::Rgba8 color = palette.colors[i];
            std::vector<uint8_t> block;
            appendAseName(block, palette.colorName(i));
            appendText(block, "RGB ");
            for (uint8_t channel : {color.r, color.g, color.b}) {
                const float value = (float)channel / 255.0f;
                uint32_t raw = 0;
                std::memcpy(&raw, &value, 4);
                appendBe(block, raw, 4);
            }
            appendBe(block, 2, 2);   // a normal (process) colour
            appendBe(out, 0x0001, 2);
            appendBe(out, (uint32_t)block.size(), 4);
            out.insert(out.end(), block.begin(), block.end());
            if (openGroup >= 0 && groupEnd[openGroup] == i) {
                appendBe(out, 0xC002, 2);
                appendBe(out, 0, 4);
                openGroup = -1;
            }
        }
        return out;
    }

    std::vector<uint8_t> writeSwatches(const px::Palette& palette, std::vector<std::string>& warnings) {
        SwatchLibrary library;
        library.name = palette.name.empty() ? std::string("Palette") : palette.name;
        const int count = std::min(palette.count(), (int)SwatchLibrary::kMaxSwatches);
        if (palette.count() > count) {
            warnings.push_back("a swatch library holds " + std::to_string(SwatchLibrary::kMaxSwatches) + " colours; the last " +
                               std::to_string(palette.count() - count) + " are not written");
        }
        for (int i = 0; i < count; i++) {
            Swatch swatch;
            swatch.name = palette.colorName(i);
            swatch.linear = jasperengine::pixelart::toEngineLinear(palette.colors[i]);
            swatch.hdr = false;
            library.swatches.push_back(std::move(swatch));
        }
        std::vector<uint8_t> out;
        appendText(out, swatches::toJson(library));
        return out;
    }

    uint32_t pngCrc(const uint8_t* data, size_t size, uint32_t crc) {
        crc = ~crc;
        for (size_t i = 0; i < size; i++) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    void appendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
        appendBe(out, (uint32_t)size, 4);
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        if (size) out.insert(out.end(), data, data + size);
        appendBe(out, pngCrc(out.data() + start, size + 4, 0), 4);
    }

    // RGBA8, filter 0 on every row, DEFLATE at a fixed level: the same pixels give the same bytes
    bool encodeStripPng(int width, int height, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& out) {
        const size_t row = (size_t)width * 4;
        std::vector<uint8_t> filtered;
        filtered.reserve((row + 1) * (size_t)height);
        for (int y = 0; y < height; y++) {
            filtered.push_back(0);
            filtered.insert(filtered.end(), rgba.begin() + (std::ptrdiff_t)(row * (size_t)y), rgba.begin() + (std::ptrdiff_t)(row * (size_t)(y + 1)));
        }
        int compressedSize = 0;
        unsigned char* compressed = stbi_zlib_compress(filtered.data(), (int)filtered.size(), &compressedSize, 8);
        if (!compressed) return false;
        out.clear();
        appendText(out, std::string_view("\x89PNG\r\n\x1a\n", 8));
        std::vector<uint8_t> header;
        appendBe(header, (uint32_t)width, 4);
        appendBe(header, (uint32_t)height, 4);
        header.push_back(8);   // bit depth
        header.push_back(6);   // RGBA
        header.push_back(0);
        header.push_back(0);
        header.push_back(0);
        appendPngChunk(out, "IHDR", header.data(), header.size());
        appendPngChunk(out, "IDAT", compressed, (size_t)compressedSize);
        appendPngChunk(out, "IEND", nullptr, 0);
        // stb's default allocator
        std::free(compressed);
        return true;
    }

    ImportResult importImpl(const std::filesystem::path& path, std::span<const uint8_t> bytes, bool firstColorsOnly) {
        ImportResult result;
        Reading r;
        r.fileName = utf8(path.filename());
        const std::string extension = lowerAscii(utf8(path.extension()));

        std::optional<PaletteFormat> format = sniffContent(bytes, extension);
        if (!format) format = formatOfExtension(extension, bytes);
        if (!format) {
            result.error = makeProblem("format_unsupported", "'" + r.fileName + "' is not a palette file this editor reads. It reads " + kSupportedFormats + ".",
                                       "Export the palette as .gpl or .hex from the program that made it, then import that");
            return result;
        }
        result.format = *format;
        if (bytes.empty()) {
            r.parseError(byteAt(0), "the file is empty");
            result.error = r.error;
            return result;
        }

        bool ok = false;
        switch (*format) {
            case PaletteFormat::Gpl: ok = readGpl(textOf(bytes), r); break;
            case PaletteFormat::Hex: ok = readHex(textOf(bytes), r); break;
            case PaletteFormat::JascPal: ok = readJasc(textOf(bytes), r); break;
            case PaletteFormat::RiffPal: ok = readRiff(bytes, r); break;
            case PaletteFormat::PaintNetTxt: ok = readPaintNet(textOf(bytes), r); break;
            case PaletteFormat::AdobeAse: ok = readAse(bytes, r); break;
            case PaletteFormat::Image: case PaletteFormat::PngStrip: ok = readImage(bytes, r); break;
            case PaletteFormat::SwatchLibrary: ok = readSwatches(textOf(bytes), r); break;
            case PaletteFormat::Aseprite:
                ok = isAseprite(bytes) ? readAseprite(bytes, r) : r.parseError(byteAt(4), "not an Aseprite file (no 0xA5E0 magic)");
                break;
        }
        result.warnings = std::move(r.warnings);
        if (!ok) {
            result.error = r.error;
            return result;
        }

        px::Palette& palette = r.palette;
        if (palette.colors.empty()) {
            result.error = makeProblem("parse_error", r.fileName + ": the file holds no colours", "Export the palette again from the program that made it");
            return result;
        }
        if (palette.count() > px::Palette::kMaxColors) {
            const int count = palette.count();
            const std::string counted = r.countStopped ? "more than 256" : std::to_string(count);
            if (!firstColorsOnly) {
                result.error = makeProblem("too_many_colors", "'" + r.fileName + "' has " + counted + " colours; a palette holds at most 256",
                                           "Reduce Colours… or Import the first 256", "colors");
                return result;
            }
            palette.colors.resize(px::Palette::kMaxColors);
            palette.names.erase(palette.names.lower_bound(px::Palette::kMaxColors), palette.names.end());
            for (px::PaletteRamp& ramp : palette.ramps) {
                ramp.colors.erase(std::remove_if(ramp.colors.begin(), ramp.colors.end(), [](uint16_t index) { return index >= px::Palette::kMaxColors; }),
                                  ramp.colors.end());
            }
            palette.ramps.erase(std::remove_if(palette.ramps.begin(), palette.ramps.end(), [](const px::PaletteRamp& ramp) { return ramp.colors.empty(); }),
                                palette.ramps.end());
            result.warnings.push_back(makeProblem("truncated", "'" + r.fileName + "' has " + counted + " colours; the first 256 are imported",
                                                  "", "colors"));
        }
        if (palette.name.empty()) palette.name = utf8(path.stem());
        palette.source.kind = *format == PaletteFormat::Aseprite ? "aseprite" : "file";
        palette.source.file = r.fileName;

        std::vector<px::Diagnostic> diagnostics;
        px::PaletteCodec::validate(palette, diagnostics);
        for (const px::Diagnostic& diagnostic : diagnostics) {
            PixelProblem problem = makeProblem(diagnostic.kind, diagnostic.message, diagnostic.fix, diagnostic.field);
            if (diagnostic.severity == px::Diagnostic::Severity::Error) {
                result.error = problem;
                return result;
            }
            result.warnings.push_back(std::move(problem));
        }
        result.palette = std::move(palette);
        return result;
    }

    std::variant<std::vector<uint8_t>, PaletteError> exportImpl(const px::Palette& palette, PaletteFormat format, std::vector<std::string>& warnings,
                                                                int swatchSize) {
        if (format == PaletteFormat::Aseprite) {
            return makeProblem("format_unsupported", "palettes are not written into Aseprite files (S47 writes .aseprite sprites)",
                               "Export as gpl or hex; Aseprite imports both", "format");
        }
        if (format == PaletteFormat::Image) {
            return makeProblem("format_unsupported", "'image' is an import format; a palette is written as an image with the format 'png'",
                               "Export with format png (a swatch strip)", "format");
        }
        if (palette.colors.empty()) return makeProblem("invalid_value", "the palette has no colours", "add a colour first", "colors");
        if (palette.count() > px::Palette::kMaxColors) {
            return makeProblem("too_many_colors", "the palette has " + std::to_string(palette.count()) + " colours; a palette holds at most 256",
                               "Reduce Colours… or remove colours", "colors");
        }
        switch (format) {
            case PaletteFormat::Gpl: return writeGpl(palette, warnings);
            case PaletteFormat::Hex: return writeHex(palette);
            case PaletteFormat::JascPal: return writeJasc(palette, warnings);
            case PaletteFormat::RiffPal: return writeRiff(palette, warnings);
            case PaletteFormat::PaintNetTxt: return writePaintNet(palette, warnings);
            case PaletteFormat::AdobeAse: return writeAse(palette, warnings);
            case PaletteFormat::SwatchLibrary: return writeSwatches(palette, warnings);
            case PaletteFormat::PngStrip: {
                if (swatchSize < 1 || swatchSize > kMaxSwatchSize) {
                    return makeProblem("invalid_value", "the swatch size is " + std::to_string(swatchSize) + "; it is 1 to 64 pixels",
                                       "use a swatch size from 1 to 64", "swatch_size");
                }
                const int width = palette.count() * swatchSize;
                const int height = swatchSize;
                std::vector<uint8_t> rgba((size_t)width * (size_t)height * 4);
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        const px::Rgba8 color = palette.colors[x / swatchSize];
                        uint8_t* pixel = rgba.data() + ((size_t)y * (size_t)width + (size_t)x) * 4;
                        pixel[0] = color.r;
                        pixel[1] = color.g;
                        pixel[2] = color.b;
                        pixel[3] = color.a;
                    }
                }
                std::vector<uint8_t> png;
                if (!encodeStripPng(width, height, rgba, png)) {
                    return makeProblem("write_error", "the PNG strip could not be encoded", "try again; if it persists, export another format");
                }
                return png;
            }
            case PaletteFormat::Aseprite: case PaletteFormat::Image: break;
        }
        return makeProblem("format_unsupported", "this format is not written", "Export as gpl or hex", "format");
    }
}
}

using namespace jasperengine;

const char* editor::pixelart::paletteFormatName(PaletteFormat format) {
    switch (format) {
        case PaletteFormat::Gpl: return "gpl";
        case PaletteFormat::Hex: return "hex";
        case PaletteFormat::JascPal: return "pal";
        case PaletteFormat::RiffPal: return "pal_riff";
        case PaletteFormat::PaintNetTxt: return "txt";
        case PaletteFormat::AdobeAse: return "ase";
        case PaletteFormat::Aseprite: return "aseprite";
        case PaletteFormat::Image: return "image";
        case PaletteFormat::SwatchLibrary: return "colors";
        case PaletteFormat::PngStrip: return "png";
    }
    return "gpl";
}

bool editor::pixelart::parsePaletteFormat(std::string_view name, PaletteFormat& out) {
    static const PaletteFormat kAll[] = {PaletteFormat::Gpl, PaletteFormat::Hex, PaletteFormat::JascPal, PaletteFormat::RiffPal,
                                         PaletteFormat::PaintNetTxt, PaletteFormat::AdobeAse, PaletteFormat::Aseprite, PaletteFormat::Image,
                                         PaletteFormat::SwatchLibrary, PaletteFormat::PngStrip};
    for (PaletteFormat format : kAll) {
        if (name == paletteFormatName(format)) {
            out = format;
            return true;
        }
    }
    return false;
}

const char* editor::pixelart::paletteFormatExtension(PaletteFormat format) {
    switch (format) {
        case PaletteFormat::Gpl: return ".gpl";
        case PaletteFormat::Hex: return ".hex";
        case PaletteFormat::JascPal: return ".pal";
        case PaletteFormat::RiffPal: return ".pal";
        case PaletteFormat::PaintNetTxt: return ".txt";
        case PaletteFormat::AdobeAse: return ".ase";
        case PaletteFormat::Aseprite: return ".aseprite";
        case PaletteFormat::Image: return ".png";
        case PaletteFormat::SwatchLibrary: return ".colors";
        case PaletteFormat::PngStrip: return ".png";
    }
    return ".gpl";
}

bool editor::pixelart::canExportPalette(PaletteFormat format) {
    return format != PaletteFormat::Aseprite && format != PaletteFormat::Image;
}

editor::pixelart::ImportResult editor::pixelart::importPalette(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                                                              bool firstColorsOnly) {
    return formatsio::importImpl(path, bytes, firstColorsOnly);
}

std::variant<std::vector<uint8_t>, editor::pixelart::PaletteError> editor::pixelart::exportPalette(const jasperengine::pixelart::Palette& palette,
                                                                                                    PaletteFormat format,
                                                                                                    std::vector<std::string>& warnings,
                                                                                                    int swatchSize) {
    return formatsio::exportImpl(palette, format, warnings, swatchSize);
}
