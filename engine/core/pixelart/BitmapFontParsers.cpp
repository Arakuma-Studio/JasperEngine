// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapFontParsers.h"

#include "util/FontFace.h"
#include "stb_image.h"
#include "stb_truetype.h"
#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace jasperengine::pixelart {

    namespace{
        FontError makeError(const char* kind, std::string message, std::string fix, std::string field){
            FontError error;
            error.kind = kind;
            error.message = std::move(message);
            error.fix = std::move(fix);
            error.field = std::move(field);
            return error;
        }

        // the smallest period whose last `gap` phases are empty everywhere, the cells starting at
        // 0 with no gap after the last one; at least two cells
        bool detectAxis(const std::vector<bool>& empty, int& cell, int& gap){
            const int length = (int)empty.size();
            for (int period = 2; period <= length / 2 + 1; period++){
                const int count = (length + period - 1) / period;
                if (count < 2)
                    break;
                const int g = count * period - length;
                if (g < 0 || g >= period)
                    continue;
                std::vector<bool> phaseEmpty((size_t)period, true);
                for (int x = 0; x < length; x++)
                    if (!empty[(size_t)x])
                        phaseEmpty[(size_t)(x % period)] = false;
                bool anyEmpty = false;
                for (bool e : phaseEmpty)
                    anyEmpty = anyEmpty || e;
                if (!anyEmpty)
                    continue;
                bool gapOk = true;
                for (int p = period - g; p < period; p++)
                    gapOk = gapOk && phaseEmpty[(size_t)p];
                if (!gapOk)
                    continue;
                // a cell needs ink somewhere: an axis empty from start to end is no grid
                bool ink = false;
                for (int p = 0; p < period - g; p++)
                    ink = ink || !phaseEmpty[(size_t)p];
                if (!ink)
                    continue;
                cell = period - g;
                gap = g;
                return true;
            }
            return false;
        }
    }

    // ---- grid

    bool cellFromFileName(const std::string& path, int& width, int& height){
        size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        const size_t at = name.find_last_of('@');
        if (at == std::string::npos)
            return false;
        const std::string size = name.substr(at + 1);
        const size_t x = size.find_first_of("xX");
        if (x == std::string::npos || x == 0 || x + 1 >= size.size())
            return false;
        for (size_t i = 0; i < size.size(); i++){
            if (i != x && !std::isdigit((unsigned char)size[i]))
                return false;
        }
        width = std::atoi(size.substr(0, x).c_str());
        height = std::atoi(size.substr(x + 1).c_str());
        return width > 0 && height > 0 && width <= 1024 && height <= 1024;
    }

    bool detectGridCell(const uint8_t* rgba, int width, int height, int& cellWidth, int& cellHeight, int& gapX, int& gapY){
        if (!rgba || width <= 0 || height <= 0)
            return false;
        std::vector<bool> columns((size_t)width, true), rows((size_t)height, true);
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                if (rgba[((size_t)y * (size_t)width + (size_t)x) * 4 + 3] != 0){
                    columns[(size_t)x] = false;
                    rows[(size_t)y] = false;
                }
            }
        }
        int cw = 0, ch = 0, gx = 0, gy = 0;
        if (!detectAxis(columns, cw, gx) || !detectAxis(rows, ch, gy))
            return false;
        cellWidth = cw;
        cellHeight = ch;
        gapX = gx;
        gapY = gy;
        return true;
    }

    bool parseGrid(const uint8_t* rgba, int width, int height, const GridOptions& options, RawFont& out, std::vector<FontError>& errors){
        int cellW = options.cellWidth, cellH = options.cellHeight;
        int gapX = options.gapX, gapY = options.gapY;
        if (cellW <= 0 || cellH <= 0){
            if (cellFromFileName(options.imagePath, cellW, cellH)){
                out.detections.push_back("cell " + std::to_string(cellW) + "x" + std::to_string(cellH) + " from the file name");
            }else if (detectGridCell(rgba, width, height, cellW, cellH, gapX, gapY)){
                out.detections.push_back("cell " + std::to_string(cellW) + "x" + std::to_string(cellH) + " detected from transparent gutters (gap " +
                                         std::to_string(gapX) + "x" + std::to_string(gapY) + ")");
            }else{
                errors.push_back(makeError("font_cell_unknown", "no cell size: the file name has no @WxH and no transparent gutters repeat",
                                           "name the sheet <name>@WxH.png or set source.cell", "source.cell"));
                return false;
            }
        }
        const int strideX = cellW + std::max(gapX, 0);
        const int strideY = cellH + std::max(gapY, 0);
        const int usableW = width - std::max(options.marginX, 0);
        const int usableH = height - std::max(options.marginY, 0);
        const int cols = usableW >= cellW ? (usableW - cellW) / strideX + 1 : 0;
        const int rows = usableH >= cellH ? (usableH - cellH) / strideY + 1 : 0;
        const int cells = cols * rows;
        out.cellWidth = cellW;
        out.cellHeight = cellH;
        out.cellsTotal = cells;
        out.nativeSize = cellH;
        if (cells == 0){
            errors.push_back(makeError("font_no_cells", "the sheet (" + std::to_string(width) + "x" + std::to_string(height) + ") holds no " +
                                       std::to_string(cellW) + "x" + std::to_string(cellH) + " cell",
                                       "check the cell size in the file name or set cell", "source.cell"));
            return false;
        }

        // (cell, codepoint) in sheet order
        std::vector<std::pair<int, uint32_t>> mapping;
        if (!options.rows.empty()){
            for (size_t r = 0; r < options.rows.size(); r++){
                const std::vector<uint32_t>& row = options.rows[r];
                if ((int)r >= rows || (int)row.size() > cols){
                    errors.push_back(makeError("font_chars_overflow",
                                               "row " + std::to_string(r) + " has " + std::to_string(row.size()) + " chars but the sheet has " +
                                               std::to_string(rows) + " rows of " + std::to_string(cols) + " cells (" + std::to_string(cellW) + "x" +
                                               std::to_string(cellH) + ")",
                                               "check the cell size in the file name or set cell", "source.rows[" + std::to_string(r) + "]"));
                    return false;
                }
                for (size_t c = 0; c < row.size(); c++)
                    mapping.push_back({(int)r * cols + (int)c, row[c]});
            }
        }else{
            if ((int)options.chars.size() > cells){
                errors.push_back(makeError("font_chars_overflow",
                                           std::to_string(options.chars.size()) + " chars but " + std::to_string(cells) + " cells (" +
                                           std::to_string(cols) + " x " + std::to_string(rows) + " of " + std::to_string(cellW) + "x" +
                                           std::to_string(cellH) + ")",
                                           "check the cell size in the file name or set cell", "source.chars"));
                return false;
            }
            for (size_t i = 0; i < options.chars.size(); i++)
                mapping.push_back({(int)i, options.chars[i]});
        }

        std::set<uint32_t> seen;
        for (const auto& [cell, codepoint] : mapping){
            if (codepoint == 0xFFFF || seen.count(codepoint))
                continue;
            seen.insert(codepoint);
            const int cx = options.marginX + (cell % cols) * strideX;
            const int cy = options.marginY + (cell / cols) * strideY;
            RawGlyph glyph;
            glyph.codepoint = codepoint;
            glyph.cell = cell;
            glyph.width = cellW;
            glyph.height = cellH;
            glyph.rgba.resize((size_t)cellW * (size_t)cellH * 4);
            for (int y = 0; y < cellH; y++){
                std::memcpy(&glyph.rgba[(size_t)y * (size_t)cellW * 4], &rgba[(((size_t)(cy + y)) * (size_t)width + (size_t)cx) * 4], (size_t)cellW * 4);
            }
            out.glyphs.push_back(std::move(glyph));
        }
        return true;
    }

    // ---- BMFont

    namespace{
        struct Fields {
            std::string tag;
            std::map<std::string, std::string> values;
            int integer(const char* key, int fallback = 0) const{
                auto it = values.find(key);
                if (it == values.end() || it->second.empty())
                    return fallback;
                return std::atoi(it->second.c_str());
            }
            long long wide(const char* key) const{
                auto it = values.find(key);
                return (it == values.end()) ? 0 : std::atoll(it->second.c_str());
            }
            std::string text(const char* key) const{
                auto it = values.find(key);
                return (it == values.end()) ? std::string() : it->second;
            }
        };

        Fields splitLine(const std::string& line){
            Fields fields;
            size_t i = 0;
            while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
            while (i < line.size() && !std::isspace((unsigned char)line[i])) fields.tag += line[i++];
            while (i < line.size()){
                while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
                std::string key;
                while (i < line.size() && line[i] != '=' && !std::isspace((unsigned char)line[i])) key += line[i++];
                if (i >= line.size() || line[i] != '='){
                    continue;
                }
                i++;
                std::string value;
                if (i < line.size() && line[i] == '"'){
                    i++;
                    while (i < line.size() && line[i] != '"') value += line[i++];
                    if (i < line.size()) i++;
                }else{
                    while (i < line.size() && !std::isspace((unsigned char)line[i])) value += line[i++];
                }
                if (!key.empty())
                    fields.values[key] = value;
            }
            return fields;
        }

        // the channel values of `common` (0 glyph, 1 outline, 2 glyph and outline, 3 zero, 4 one)
        bool hasOutlineChannels(int alpha, int red, int green, int blue){
            for (int value : {alpha, red, green, blue})
                if (value == 1 || value == 2)
                    return true;
            return false;
        }

        void addChar(BMFontFile& out, long long id, const Fields& f, std::vector<FontError>& errors){
            if (id < 0 || id > 0x10FFFF){
                errors.push_back(makeError("font_bmfont_codepoint", "char id " + std::to_string(id) + " is beyond U+10FFFF",
                                           "remove that char from the .fnt", "chars"));
                return;
            }
            BMFontChar c;
            c.id = (uint32_t)id;
            c.x = f.integer("x");
            c.y = f.integer("y");
            c.width = f.integer("width");
            c.height = f.integer("height");
            c.xoffset = f.integer("xoffset");
            c.yoffset = f.integer("yoffset");
            c.xadvance = f.integer("xadvance");
            c.page = f.integer("page");
            c.chnl = f.integer("chnl", 15);
            out.chars.push_back(c);
        }

        bool parseText(const std::string& text, BMFontFile& out, std::vector<FontError>& errors){
            int alpha = 0, red = 0, green = 0, blue = 0;
            size_t start = 0;
            while (start < text.size()){
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                start = end + 1;
                const Fields f = splitLine(line);
                if (f.tag == "info"){
                    out.size = std::abs(f.integer("size"));
                }else if (f.tag == "common"){
                    out.lineHeight = f.integer("lineHeight");
                    out.base = f.integer("base");
                    out.scaleW = f.integer("scaleW");
                    out.scaleH = f.integer("scaleH");
                    out.packed = f.integer("packed") != 0;
                    alpha = f.integer("alphaChnl");
                    red = f.integer("redChnl");
                    green = f.integer("greenChnl");
                    blue = f.integer("blueChnl");
                    out.pages.resize((size_t)std::max(0, std::min(f.integer("pages"), 256)));
                }else if (f.tag == "page"){
                    const int id = f.integer("id");
                    if (id >= 0 && id < 256){
                        if ((size_t)id >= out.pages.size()) out.pages.resize((size_t)id + 1);
                        out.pages[(size_t)id] = f.text("file");
                    }
                }else if (f.tag == "char"){
                    addChar(out, f.wide("id"), f, errors);
                }else if (f.tag == "kerning"){
                    BitmapFontSettings::Kern k;
                    k.left = (uint32_t)f.wide("first");
                    k.right = (uint32_t)f.wide("second");
                    k.amount = f.integer("amount");
                    out.kerning.push_back(k);
                }
            }
            if (out.packed && hasOutlineChannels(alpha, red, green, blue)){
                errors.push_back(makeError("font_bmfont_packed", "packed=1 with outline channels: only the glyph mask is read",
                                           "export the BMFont unpacked (32-bit, glyph in alpha) without outline channels", "common.packed"));
            }
            return errors.empty();
        }

        Fields xmlFields(const tinyxml2::XMLElement* element){
            Fields f;
            f.tag = element->Name();
            for (const tinyxml2::XMLAttribute* a = element->FirstAttribute(); a; a = a->Next())
                f.values[a->Name()] = a->Value();
            return f;
        }

        bool parseXml(const std::string& text, BMFontFile& out, std::vector<FontError>& errors){
            tinyxml2::XMLDocument doc;
            if (doc.Parse(text.c_str(), text.size()) != tinyxml2::XML_SUCCESS || !doc.FirstChildElement("font")){
                errors.push_back(makeError("font_bmfont_invalid", "the XML .fnt does not parse", "export the BMFont again", "file"));
                return false;
            }
            const tinyxml2::XMLElement* font = doc.FirstChildElement("font");
            std::string asText;
            // the XML elements carry the text format's fields: one line each
            auto line = [&asText](const Fields& f){
                asText += f.tag;
                for (const auto& [key, value] : f.values) asText += " " + key + "=\"" + value + "\"";
                asText += "\n";
            };
            if (const tinyxml2::XMLElement* e = font->FirstChildElement("info")) line(xmlFields(e));
            if (const tinyxml2::XMLElement* e = font->FirstChildElement("common")) line(xmlFields(e));
            if (const tinyxml2::XMLElement* pages = font->FirstChildElement("pages"))
                for (const tinyxml2::XMLElement* e = pages->FirstChildElement("page"); e; e = e->NextSiblingElement("page")) line(xmlFields(e));
            if (const tinyxml2::XMLElement* chars = font->FirstChildElement("chars"))
                for (const tinyxml2::XMLElement* e = chars->FirstChildElement("char"); e; e = e->NextSiblingElement("char")) line(xmlFields(e));
            if (const tinyxml2::XMLElement* kernings = font->FirstChildElement("kernings"))
                for (const tinyxml2::XMLElement* e = kernings->FirstChildElement("kerning"); e; e = e->NextSiblingElement("kerning")) line(xmlFields(e));
            out.encoding = BMFontFile::Encoding::XML;
            return parseText(asText, out, errors);
        }

        struct Reader {
            const uint8_t* data;
            size_t size;
            size_t at = 0;
            bool ok = true;
            uint32_t u(int bytes){
                if (at + (size_t)bytes > size){ ok = false; at = size; return 0; }
                uint32_t v = 0;
                for (int i = 0; i < bytes; i++) v |= (uint32_t)data[at + (size_t)i] << (8 * i);
                at += (size_t)bytes;
                return v;
            }
            int s16(){ return (int)(int16_t)(uint16_t)u(2); }
        };

        bool parseBinary(const std::vector<uint8_t>& bytes, BMFontFile& out, std::vector<FontError>& errors){
            out.encoding = BMFontFile::Encoding::BINARY;
            auto truncated = [&errors](const std::string& where){
                errors.push_back(makeError("font_bmfont_truncated", "the binary .fnt ends inside its " + where + " block",
                                           "export the BMFont again", "file"));
                return false;
            };
            if (bytes.size() < 4 || bytes[3] != 3){
                errors.push_back(makeError("font_bmfont_version", "binary .fnt version " + std::to_string(bytes.size() >= 4 ? bytes[3] : 0) +
                                           " (only version 3 is read)", "export it with BMFont 1.10 or later", "file"));
                return false;
            }
            int alpha = 0, red = 0, green = 0, blue = 0;
            size_t at = 4;
            bool sawCommon = false;
            while (at < bytes.size()){
                if (at + 5 > bytes.size())
                    return truncated("block header");
                const int type = bytes[at];
                const uint32_t length = (uint32_t)bytes[at + 1] | ((uint32_t)bytes[at + 2] << 8) | ((uint32_t)bytes[at + 3] << 16) | ((uint32_t)bytes[at + 4] << 24);
                at += 5;
                static const char* names[] = {"", "info", "common", "pages", "chars", "kerning"};
                const std::string name = (type >= 1 && type <= 5) ? names[type] : "unknown";
                if ((size_t)length > bytes.size() - at)
                    return truncated(name);
                Reader r{bytes.data() + at, length};
                if (type == 1){
                    out.size = std::abs(r.s16());
                }else if (type == 2){
                    out.lineHeight = (int)r.u(2);
                    out.base = (int)r.u(2);
                    out.scaleW = (int)r.u(2);
                    out.scaleH = (int)r.u(2);
                    const int pages = (int)r.u(2);
                    out.packed = (r.u(1) & 0x80) != 0;
                    alpha = (int)r.u(1);
                    red = (int)r.u(1);
                    green = (int)r.u(1);
                    blue = (int)r.u(1);
                    if (!r.ok)
                        return truncated(name);
                    out.pages.resize((size_t)std::min(pages, 256));
                    sawCommon = true;
                }else if (type == 3){
                    // null-terminated names, all of the same length
                    std::vector<std::string> names;
                    size_t i = 0;
                    while (i < length){
                        std::string page;
                        while (i < length && r.data[i] != 0) page += (char)r.data[i++];
                        if (i >= length)
                            return truncated(name);
                        i++;
                        names.push_back(page);
                    }
                    if (out.pages.size() < names.size()) out.pages.resize(names.size());
                    for (size_t p = 0; p < names.size() && p < out.pages.size(); p++) out.pages[p] = names[p];
                }else if (type == 4){
                    if (length % 20 != 0)
                        return truncated(name);
                    for (uint32_t i = 0; i < length / 20; i++){
                        Fields f;
                        const long long id = (long long)r.u(4);
                        f.values["x"] = std::to_string(r.u(2));
                        f.values["y"] = std::to_string(r.u(2));
                        f.values["width"] = std::to_string(r.u(2));
                        f.values["height"] = std::to_string(r.u(2));
                        f.values["xoffset"] = std::to_string(r.s16());
                        f.values["yoffset"] = std::to_string(r.s16());
                        f.values["xadvance"] = std::to_string(r.s16());
                        f.values["page"] = std::to_string(r.u(1));
                        f.values["chnl"] = std::to_string(r.u(1));
                        addChar(out, id, f, errors);
                    }
                }else if (type == 5){
                    if (length % 10 != 0)
                        return truncated(name);
                    for (uint32_t i = 0; i < length / 10; i++){
                        BitmapFontSettings::Kern k;
                        k.left = r.u(4);
                        k.right = r.u(4);
                        k.amount = r.s16();
                        out.kerning.push_back(k);
                    }
                }
                at += length;
            }
            if (!sawCommon)
                return truncated("common");
            if (out.packed && hasOutlineChannels(alpha, red, green, blue)){
                errors.push_back(makeError("font_bmfont_packed", "packed=1 with outline channels: only the glyph mask is read",
                                           "export the BMFont unpacked (32-bit, glyph in alpha) without outline channels", "common.packed"));
            }
            return errors.empty();
        }
    }

    bool parseBMFont(const std::vector<uint8_t>& bytes, BMFontFile& out, std::vector<FontError>& errors){
        out = BMFontFile();
        if (bytes.size() >= 3 && bytes[0] == 'B' && bytes[1] == 'M' && bytes[2] == 'F')
            return parseBinary(bytes, out, errors) && errors.empty();
        std::string text(bytes.begin(), bytes.end());
        size_t first = 0;
        // a UTF-8 byte order mark
        if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF)
            first = 3;
        while (first < text.size() && std::isspace((unsigned char)text[first])) first++;
        if (first < text.size() && text[first] == '<')
            return parseXml(text.substr(first), out, errors) && errors.empty();
        if (text.compare(first, 4, "info") != 0 && text.compare(first, 6, "common") != 0){
            errors.push_back(makeError("font_bmfont_invalid", "not an AngelCode BMFont file (text, XML or binary)",
                                       "export the font from BMFont, or use a grid sheet", "file"));
            return false;
        }
        out.encoding = BMFontFile::Encoding::TEXT;
        return parseText(text, out, errors) && errors.empty();
    }

    bool bmfontToRaw(const BMFontFile& file, const std::vector<DecodedPage>& pages, RawFont& out, std::vector<FontError>& errors){
        out = RawFont();
        out.fileAdvances = true;
        out.nativeSize = file.size > 0 ? file.size : file.lineHeight;
        out.lineHeight = file.lineHeight;
        out.baseline = file.base;
        out.cellHeight = file.lineHeight;
        out.kerning = file.kerning;
        std::set<uint32_t> seen;
        for (const BMFontChar& c : file.chars){
            if (seen.count(c.id))
                continue;
            seen.insert(c.id);
            if (c.id == 32)
                out.spaceAdvance = c.xadvance;
            int channel = -1;   // the byte of RGBA read as the mask; -1 all channels
            switch (c.chnl){
                case 15: channel = -1; break;
                case 1: channel = 2; break;   // blue
                case 2: channel = 1; break;   // green
                case 4: channel = 0; break;   // red
                case 8: channel = 3; break;   // alpha
                default:
                    errors.push_back(makeError("font_bmfont_channel", "char " + std::to_string(c.id) + " has chnl=" + std::to_string(c.chnl) +
                                               ": only 15 (all channels) or one channel (1, 2, 4, 8) is read",
                                               "export the BMFont with the glyph in all channels (32-bit, not packed)", "chars"));
                    return false;
            }
            RawGlyph glyph;
            glyph.codepoint = c.id;
            glyph.offsetX = c.xoffset;
            glyph.offsetY = c.yoffset;
            glyph.fileAdvance = c.xadvance;
            out.cellWidth = std::max(out.cellWidth, c.width);
            if (c.width > 0 && c.height > 0){
                if (c.page < 0 || (size_t)c.page >= pages.size() || pages[(size_t)c.page].rgba.empty()){
                    errors.push_back(makeError("font_bmfont_page_missing", "char " + std::to_string(c.id) + " is on page " + std::to_string(c.page) +
                                               ", which was not loaded", "copy the page next to the .fnt", "pages"));
                    return false;
                }
                const DecodedPage& page = pages[(size_t)c.page];
                if (c.x < 0 || c.y < 0 || c.x + c.width > page.width || c.y + c.height > page.height){
                    errors.push_back(makeError("font_bmfont_rect", "char " + std::to_string(c.id) + " lies outside its page (" +
                                               std::to_string(page.width) + "x" + std::to_string(page.height) + ")",
                                               "export the BMFont again", "chars"));
                    return false;
                }
                glyph.width = c.width;
                glyph.height = c.height;
                glyph.rgba.resize((size_t)c.width * (size_t)c.height * 4);
                for (int y = 0; y < c.height; y++){
                    for (int x = 0; x < c.width; x++){
                        const uint8_t* src = &page.rgba[(((size_t)(c.y + y)) * (size_t)page.width + (size_t)(c.x + x)) * 4];
                        uint8_t* dst = &glyph.rgba[((size_t)y * (size_t)c.width + (size_t)x) * 4];
                        if (channel < 0){
                            std::memcpy(dst, src, 4);
                        }else{
                            dst[0] = dst[1] = dst[2] = 255;
                            dst[3] = src[channel];
                        }
                    }
                }
            }
            out.glyphs.push_back(std::move(glyph));
        }
        return true;
    }

    // ---- TrueType

    namespace{
        void collectCoordinates(const stbtt_fontinfo& info, int glyph, std::vector<int>& out){
            stbtt_vertex* vertices = nullptr;
            const int count = stbtt_GetGlyphShape(&info, glyph, &vertices);
            for (int i = 0; i < count; i++){
                out.push_back(vertices[i].x);
                out.push_back(vertices[i].y);
                if (vertices[i].type == STBTT_vcurve || vertices[i].type == STBTT_vcubic){
                    out.push_back(vertices[i].cx);
                    out.push_back(vertices[i].cy);
                }
                if (vertices[i].type == STBTT_vcubic){
                    out.push_back(vertices[i].cx1);
                    out.push_back(vertices[i].cy1);
                }
            }
            if (vertices)
                stbtt_FreeShape(&info, vertices);
        }
    }

    int detectPixelSize(const FontFace& face, std::vector<int>* candidates){
        const stbtt_fontinfo& info = face.getInfo();
        std::vector<int> coordinates;
        for (uint32_t c = 33; c < 127; c++){
            const uint32_t glyph = face.getGlyphIndex(c);
            if (glyph != 0)
                collectCoordinates(info, (int)glyph, coordinates);
        }
        const int unitsPerEm = face.getUnitsPerEm();
        std::vector<int> nonZero;
        for (int v : coordinates)
            if (v != 0) nonZero.push_back(v);
        if (nonZero.empty() || unitsPerEm <= 0)
            return 0;
        std::vector<std::pair<double, int>> shares;   // (share, pixel size)
        for (int size = 5; size <= 64; size++){
            if (unitsPerEm % size != 0)
                continue;
            const int unit = unitsPerEm / size;
            size_t multiples = 0;
            for (int v : nonZero)
                if (v % unit == 0) multiples++;
            const double share = (double)multiples / (double)nonZero.size();
            // the largest unit: the smallest pixel size that fits
            if (share >= 0.9)
                return size;
            shares.push_back({share, size});
        }
        if (candidates){
            std::stable_sort(shares.begin(), shares.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
            for (size_t i = 0; i < shares.size() && i < 3; i++)
                candidates->push_back(shares[i].second);
        }
        return 0;
    }

    bool rasterTrueType(const FontFace& face, const TrueTypeOptions& options, RawFont& out, std::vector<FontError>& errors){
        out = RawFont();
        int pixelSize = options.pixelSize;
        if (pixelSize <= 0){
            std::vector<int> candidates;
            pixelSize = detectPixelSize(face, &candidates);
            if (pixelSize <= 0){
                std::string best;
                for (int c : candidates) best += (best.empty() ? "" : ", ") + std::to_string(c) + " px";
                errors.push_back(makeError("font_pixel_size_unknown",
                                           "no pixel grid in the outlines (90 % of coordinates on one unit, 5 to 64 px)" +
                                           (best.empty() ? std::string() : "; closest: " + best),
                                           "set source.pixel_size, or use a pixel font", "source.pixel_size"));
                return false;
            }
            out.detections.push_back("pixel size " + std::to_string(pixelSize) + " detected from the outline grid");
        }
        const stbtt_fontinfo& info = face.getInfo();
        const float scale = (float)pixelSize / (float)face.getUnitsPerEm();
        const int ascent = (int)std::lround((float)face.getAscender() * scale);
        const int descent = (int)std::lround((float)-face.getDescender() * scale);
        const int gap = (int)std::lround((float)face.getLineGap() * scale);
        out.nativeSize = ascent + descent;
        out.baseline = ascent;
        out.lineHeight = gap > 0 ? ascent + descent + gap : ascent + descent + 1;
        out.cellHeight = ascent + descent;
        out.cellWidth = pixelSize;
        out.fileAdvances = true;

        std::map<uint32_t, uint32_t> glyphToCodepoint;
        std::vector<unsigned char> coverage;
        for (uint32_t codepoint : options.chars){
            const uint32_t index = face.getGlyphIndex(codepoint);
            if (index == 0 && codepoint != 0)
                continue;
            glyphToCodepoint.emplace(index, codepoint);
            RawGlyph glyph;
            glyph.codepoint = codepoint;
            glyph.fileAdvance = (int)std::lround(face.getGlyphAdvance(index, scale));
            if (codepoint == 32)
                out.spaceAdvance = glyph.fileAdvance;
            int w = 0, h = 0, xoff = 0, yoff = 0;
            if (face.rasterizeGlyph(index, scale, coverage, w, h, xoff, yoff)){
                glyph.width = w;
                glyph.height = h;
                glyph.offsetX = xoff;
                glyph.offsetY = ascent + yoff;
                glyph.rgba.assign((size_t)w * (size_t)h * 4, 0);
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++){
                    const bool opaque = options.threshold <= 0.0f ? coverage[i] > 0 : (float)coverage[i] / 255.0f >= options.threshold;
                    glyph.rgba[i * 4 + 0] = glyph.rgba[i * 4 + 1] = glyph.rgba[i * 4 + 2] = 255;
                    glyph.rgba[i * 4 + 3] = opaque ? 255 : 0;
                }
            }
            out.glyphs.push_back(std::move(glyph));
        }

        // the kern table only (GPOS kerning is not read for a bitmap font)
        const int pairs = stbtt_GetKerningTableLength(&info);
        if (pairs > 0){
            std::vector<stbtt_kerningentry> table((size_t)pairs);
            stbtt_GetKerningTable(&info, table.data(), pairs);
            for (const stbtt_kerningentry& entry : table){
                auto left = glyphToCodepoint.find((uint32_t)entry.glyph1);
                auto right = glyphToCodepoint.find((uint32_t)entry.glyph2);
                const int amount = (int)std::lround((float)entry.advance * scale);
                if (left != glyphToCodepoint.end() && right != glyphToCodepoint.end() && amount != 0){
                    BitmapFontSettings::Kern k;
                    k.left = left->second;
                    k.right = right->second;
                    k.amount = amount;
                    out.kerning.push_back(k);
                }
            }
        }
        return true;
    }

    bool decodeImage(const std::vector<uint8_t>& bytes, DecodedPage& out){
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
        if (!pixels)
            return false;
        out.width = w;
        out.height = h;
        out.rgba.assign(pixels, pixels + (size_t)w * (size_t)h * 4);
        stbi_image_free(pixels);
        return true;
    }

}
