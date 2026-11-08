// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapFont.h"

#include "pixelart/BitmapFontParsers.h"
#include "pixelart/BuiltinPixelFont.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "util/FontFace.h"
#include "util/GlyphAtlas.h"
#include "util/StringUtils.h"
#include "json.hpp"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

// stb_image_write.c (C) defines it without declaring it in its header part
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace jasperengine::pixelart {

    namespace{
        using Json = nlohmann::json;

        constexpr uint32_t kFallbackBoxKey = 0xFFFFFFFFu;
        constexpr uint32_t kCookedVersion = 1;

        FontError fontError(const char* kind, std::string message, std::string fix, std::string field){
            FontError error;
            error.kind = kind;
            error.message = std::move(message);
            error.fix = std::move(fix);
            error.field = std::move(field);
            return error;
        }

        std::string lowerExtension(const std::string& path){
            const size_t slash = path.find_last_of("/\\");
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                return "";
            std::string ext = path.substr(dot);
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            return ext;
        }

        bool readBytes(const std::string& path, std::vector<uint8_t>& out){
            Data data;
            if (path.empty() || data.open(path.c_str()) != FileErrors::FILEDATA_OK)
                return false;
            out.assign(data.getMemPtr(), data.getMemPtr() + data.length());
            return true;
        }

        uint64_t kernKey(uint32_t left, uint32_t right){
            return ((uint64_t)left << 32) | (uint64_t)right;
        }

        // a glyph image ready to pack: cropped to its ink
        struct PackGlyph {
            uint32_t codepoint = 0;
            int width = 0, height = 0;
            std::vector<uint8_t> rgba;
            int offsetX = 0, offsetY = 0;
            int advance = 0;
        };

        void cropToInk(PackGlyph& glyph){
            int left = glyph.width, right = -1, top = glyph.height, bottom = -1;
            for (int y = 0; y < glyph.height; y++){
                for (int x = 0; x < glyph.width; x++){
                    if (glyph.rgba[((size_t)y * (size_t)glyph.width + (size_t)x) * 4 + 3] != 0){
                        left = std::min(left, x);
                        right = std::max(right, x);
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
                }
            }
            if (right < 0){
                glyph.width = glyph.height = 0;
                glyph.rgba.clear();
                return;
            }
            const int w = right - left + 1, h = bottom - top + 1;
            std::vector<uint8_t> cropped((size_t)w * (size_t)h * 4);
            for (int y = 0; y < h; y++)
                std::memcpy(&cropped[(size_t)y * (size_t)w * 4], &glyph.rgba[(((size_t)(top + y)) * (size_t)glyph.width + (size_t)left) * 4], (size_t)w * 4);
            glyph.rgba.swap(cropped);
            glyph.offsetX += left;
            glyph.offsetY += top;
            glyph.width = w;
            glyph.height = h;
        }

        // ink columns of an image: false when it has none
        bool inkColumns(const RawGlyph& glyph, int& left, int& right){
            left = glyph.width;
            right = -1;
            for (int y = 0; y < glyph.height; y++)
                for (int x = 0; x < glyph.width; x++)
                    if (glyph.rgba[((size_t)y * (size_t)glyph.width + (size_t)x) * 4 + 3] != 0){
                        left = std::min(left, x);
                        right = std::max(right, x);
                    }
            return right >= 0;
        }

        int inkBottom(const RawGlyph& glyph){
            for (int y = glyph.height - 1; y >= 0; y--)
                for (int x = 0; x < glyph.width; x++)
                    if (glyph.rgba[((size_t)y * (size_t)glyph.width + (size_t)x) * 4 + 3] != 0)
                        return y;
            return -1;
        }

        void appendUtf8(std::string& out, uint32_t c){
            if (c < 0x80){
                out += (char)c;
            }else if (c < 0x800){
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }else if (c < 0x10000){
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }else{
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
        }

        // one codepoint from a one-character string; false otherwise
        bool singleCodepoint(const std::string& text, uint32_t& out){
            bool invalid = false;
            const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, invalid);
            if (invalid || codepoints.size() != 1)
                return false;
            out = codepoints[0];
            return true;
        }

        // the font's whole-pixel loads guard: fallback fonts naming each other
        int& fallbackDepth(){
            static thread_local int depth = 0;
            return depth;
        }
    }

    // ---- names and paths

    FontKind fontKindOf(const std::string& path){
        return isBitmapFontPath(path) ? FontKind::BITMAP : FontKind::VECTOR;
    }

    bool isBitmapFontPath(const std::string& path){
        if (path.rfind("builtin:", 0) == 0)
            return path == kBuiltinPixelFont;
        const std::string ext = lowerExtension(path);
        return ext == ".bitmapfont" || ext == ".fnt" || ext == ".jfont";
    }

    std::string codepointToUtf8(uint32_t codepoint){
        std::string out;
        appendUtf8(out, codepoint);
        return out;
    }

    std::vector<uint32_t> expandFontChars(const std::string& chars, const char* defaultSet){
        const std::string set = chars.empty() ? std::string(defaultSet ? defaultSet : "ascii") : chars;
        std::vector<uint32_t> out;
        auto range = [&out](uint32_t first, uint32_t last){ for (uint32_t c = first; c <= last; c++) out.push_back(c); };
        if (set == "ascii"){
            range(0x20, 0x7E);
        }else if (set == "latin1"){
            range(0x20, 0x7E);
            range(0xA0, 0xFF);
        }else if (set == "latin_ext"){
            range(0x20, 0x7E);
            range(0xA0, 0xFF);
            range(0x100, 0x17F);
        }else{
            bool invalid = false;
            out = StringUtils::decodeUtf8ToCodepoints(set, invalid);
        }
        return out;
    }

    std::string resolveFontRelative(const std::string& fontPath, const std::string& relative){
        if (relative.empty())
            return relative;
        // already rooted (asset://, data://, an absolute path)
        if (relative.find("://") != std::string::npos || relative[0] == '/' || (relative.size() > 1 && relative[1] == ':'))
            return relative;
        const size_t slash = fontPath.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? std::string() : fontPath.substr(0, slash + 1);
        std::string joined = base + relative;
        // fold "dir/../"
        std::vector<std::string> parts;
        std::string prefix;
        const size_t scheme = joined.find("://");
        if (scheme != std::string::npos){
            prefix = joined.substr(0, scheme + 3);
            joined = joined.substr(scheme + 3);
        }
        size_t start = 0;
        while (start <= joined.size()){
            size_t end = joined.find_first_of("/\\", start);
            if (end == std::string::npos) end = joined.size();
            const std::string part = joined.substr(start, end - start);
            if (part == ".." && !parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            start = end + 1;
        }
        std::string out = prefix;
        for (size_t i = 0; i < parts.size(); i++)
            out += (i ? "/" : "") + parts[i];
        return out;
    }

    // ---- settings

    bool parseBitmapFontSettings(const std::string& text, BitmapFontSettings& s, std::vector<FontError>& errors){
        s = BitmapFontSettings();
        Json doc = Json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()){
            errors.push_back(fontError("font_invalid_json", "the .bitmapfont is not a JSON object", "write it as JSON (see jasper://docs/pixel-fonts)", ""));
            return false;
        }
        auto bad = [&errors](const std::string& field, const std::string& what, const std::string& fix){
            errors.push_back(fontError("font_bad_value", field + " " + what, fix, field));
        };
        auto intAt = [&](const Json& object, const char* key, const std::string& field, int& out, int minimum, int maximum) -> bool{
            auto it = object.find(key);
            if (it == object.end())
                return false;
            if (!it->is_number_integer() || it->get<long long>() < minimum || it->get<long long>() > maximum){
                bad(field, "must be a whole number from " + std::to_string(minimum) + " to " + std::to_string(maximum), "set " + field + " to a whole number");
                return false;
            }
            out = it->get<int>();
            return true;
        };
        auto pairAt = [&](const Json& object, const char* key, const std::string& field, int& x, int& y, int minimum) -> bool{
            auto it = object.find(key);
            if (it == object.end())
                return false;
            if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number_integer() || !(*it)[1].is_number_integer() ||
                    (*it)[0].get<int>() < minimum || (*it)[1].get<int>() < minimum){
                bad(field, "must be [x, y] whole numbers from " + std::to_string(minimum), "write " + field + " as [x, y]");
                return false;
            }
            x = (*it)[0].get<int>();
            y = (*it)[1].get<int>();
            return true;
        };

        if (doc.value("format", std::string()) != "jasperengine.bitmapfont")
            bad("format", "must be \"jasperengine.bitmapfont\"", "add \"format\": \"jasperengine.bitmapfont\"");
        if (doc.contains("version")){
            if (!doc["version"].is_number_integer() || doc["version"].get<int>() < 1)
                bad("version", "must be a whole number from 1", "set \"version\": 1");
            else
                s.version = doc["version"].get<int>();
        }

        auto source = doc.find("source");
        if (source == doc.end() || !source->is_object()){
            errors.push_back(fontError("font_no_source", "the .bitmapfont has no source object",
                                       "add \"source\": {\"type\": \"grid\", \"image\": \"<sheet>@6x9.png\"}", "source"));
            return false;
        }
        const std::string type = source->value("type", std::string());
        if (type == "grid"){
            s.source = BitmapFontSettings::Source::GRID;
            s.file = source->value("image", std::string());
            if (s.file.empty())
                errors.push_back(fontError("font_no_source", "a grid source needs an image", "set source.image to the sheet PNG", "source.image"));
        }else if (type == "bmfont" || type == "truetype"){
            s.source = type == "bmfont" ? BitmapFontSettings::Source::BMFONT : BitmapFontSettings::Source::TRUETYPE;
            s.file = source->value("file", std::string());
            if (s.file.empty())
                errors.push_back(fontError("font_no_source", "a " + type + " source needs a file", "set source.file", "source.file"));
        }else{
            errors.push_back(fontError("font_bad_value", "source.type must be grid, bmfont or truetype", "set source.type", "source.type"));
            return false;
        }
        pairAt(*source, "cell", "source.cell", s.cellWidth, s.cellHeight, 1);
        pairAt(*source, "margin", "source.margin", s.marginX, s.marginY, 0);
        pairAt(*source, "gap", "source.gap", s.gapX, s.gapY, 0);
        if (source->contains("chars")){
            if (!(*source)["chars"].is_string()) bad("source.chars", "must be a string", "write the characters as one string");
            else s.chars = (*source)["chars"].get<std::string>();
        }
        if (source->contains("rows")){
            const Json& rows = (*source)["rows"];
            bool ok = rows.is_array();
            for (const Json& row : rows) ok = ok && row.is_string();
            if (!ok) bad("source.rows", "must be a list of strings, one per sheet row", "write one string per row");
            else for (const Json& row : rows) s.rows.push_back(row.get<std::string>());
        }
        intAt(*source, "pixel_size", "source.pixel_size", s.pixelSize, 0, 64);
        if (source->contains("threshold")){
            const Json& t = (*source)["threshold"];
            if (!t.is_number() || t.get<double>() < 0.0 || t.get<double>() > 1.0) bad("source.threshold", "must be from 0 to 1", "set source.threshold to 0.5");
            else s.threshold = t.get<float>();
        }

        if (doc.contains("advance")){
            const Json& a = doc["advance"];
            if (a.is_string() && a.get<std::string>() == "auto") s.advance = BitmapFontSettings::Advance::AUTO;
            else if (a.is_string() && a.get<std::string>() == "monospace") s.advance = BitmapFontSettings::Advance::MONOSPACE;
            else if (a.is_string() && a.get<std::string>() == "file") s.advance = BitmapFontSettings::Advance::FILE;
            else if (a.is_number_integer() && a.get<int>() >= 0 && a.get<int>() <= 1024){ s.advance = BitmapFontSettings::Advance::FIXED; s.advanceValue = a.get<int>(); }
            else bad("advance", "must be \"auto\", \"monospace\", \"file\" or a whole number", "set advance to \"auto\"");
        }
        if (doc.contains("trim")){
            const std::string trim = doc["trim"].is_string() ? doc["trim"].get<std::string>() : std::string();
            if (trim == "both") s.trimBoth = true;
            else if (trim != "right") bad("trim", "must be \"right\" or \"both\"", "set trim to \"right\"");
        }
        intAt(doc, "spacing", "spacing", s.spacing, -64, 64);
        int value = 0;
        if (intAt(doc, "space_width", "space_width", value, 0, 1024)) s.spaceWidth = value;
        if (intAt(doc, "line_height", "line_height", value, 1, 1024)) s.lineHeight = value;
        if (doc.contains("baseline") && !(doc["baseline"].is_string() && doc["baseline"].get<std::string>() == "auto")){
            if (intAt(doc, "baseline", "baseline", value, 0, 1024)) s.baseline = value;
        }
        if (doc.contains("fallback")){
            uint32_t c = 0;
            if (!doc["fallback"].is_string() || !singleCodepoint(doc["fallback"].get<std::string>(), c))
                bad("fallback", "must be one character", "set fallback to \"?\"");
            else
                s.fallback = doc["fallback"].get<std::string>();
        }
        if (doc.contains("colored")){
            const Json& c = doc["colored"];
            if (c.is_boolean()) s.colored = c.get<bool>() ? 1 : 0;
            else if (!(c.is_string() && c.get<std::string>() == "auto")) bad("colored", "must be \"auto\", true or false", "set colored to \"auto\"");
        }
        intAt(doc, "alpha_cutoff", "alpha_cutoff", s.alphaCutoff, 0, 255);
        if (doc.contains("glyphs")){
            const Json& glyphs = doc["glyphs"];
            if (!glyphs.is_object()){
                bad("glyphs", "must be an object of characters", "write glyphs as {\"i\": {\"advance\": 2}}");
            }else{
                for (auto it = glyphs.begin(); it != glyphs.end(); ++it){
                    const std::string field = "glyphs." + it.key();
                    uint32_t c = 0;
                    if (!singleCodepoint(it.key(), c) || !it.value().is_object()){
                        bad(field, "must be one character with an object", "key each override by its character");
                        continue;
                    }
                    BitmapFontSettings::GlyphOverride o;
                    if (intAt(it.value(), "advance", field + ".advance", value, 0, 1024)) o.advance = value;
                    if (it.value().contains("offset"))
                        pairAt(it.value(), "offset", field + ".offset", o.offsetX, o.offsetY, -1024);
                    s.glyphs[c] = o;
                }
            }
        }
        if (doc.contains("kerning")){
            const Json& kerning = doc["kerning"];
            if (!kerning.is_array()){
                bad("kerning", "must be a list of [left, right, pixels]", "write kerning as [[\"T\", \"o\", -1]]");
            }else{
                for (size_t i = 0; i < kerning.size(); i++){
                    const Json& k = kerning[i];
                    BitmapFontSettings::Kern pair;
                    const std::string field = "kerning[" + std::to_string(i) + "]";
                    if (!k.is_array() || k.size() != 3 || !k[0].is_string() || !k[1].is_string() || !k[2].is_number_integer() ||
                            !singleCodepoint(k[0].get<std::string>(), pair.left) || !singleCodepoint(k[1].get<std::string>(), pair.right)){
                        bad(field, "must be [left, right, pixels]", "write each pair as [\"T\", \"o\", -1]");
                        continue;
                    }
                    pair.amount = k[2].get<int>();
                    s.kerning.push_back(pair);
                }
            }
        }
        if (doc.contains("fallback_fonts")){
            const Json& fonts = doc["fallback_fonts"];
            bool ok = fonts.is_array();
            for (const Json& f : fonts) ok = ok && f.is_string();
            if (!ok) bad("fallback_fonts", "must be a list of bitmap font paths", "list .bitmapfont or .fnt paths");
            else for (const Json& f : fonts) s.fallbackFonts.push_back(f.get<std::string>());
        }
        return errors.empty();
    }

    namespace{
        // compact JSON with ", " and ": " (one line)
        std::string compact(const Json& value){
            if (value.is_object()){
                std::string out = "{";
                bool first = true;
                for (auto it = value.begin(); it != value.end(); ++it){
                    out += (first ? "" : ", ") + Json(it.key()).dump() + ": " + compact(it.value());
                    first = false;
                }
                return out + "}";
            }
            if (value.is_array()){
                std::string out = "[";
                for (size_t i = 0; i < value.size(); i++)
                    out += (i ? ", " : "") + compact(value[i]);
                return out + "]";
            }
            return value.dump();
        }

        uint32_t firstCodepoint(const std::string& text){
            bool invalid = false;
            const std::vector<uint32_t> c = StringUtils::decodeUtf8ToCodepoints(text, invalid);
            return c.empty() ? 0 : c[0];
        }

        void writeValue(std::string& out, const Json& value, int indent);

        void writeObject(std::string& out, const std::vector<std::pair<std::string, const Json*>>& entries, int indent){
            if (entries.empty()){
                out += "{}";
                return;
            }
            const std::string pad((size_t)(indent + 2), ' ');
            out += "{\n";
            for (size_t i = 0; i < entries.size(); i++){
                out += pad + Json(entries[i].first).dump() + ": ";
                writeValue(out, *entries[i].second, indent + 2);
                out += (i + 1 < entries.size()) ? ",\n" : "\n";
            }
            out += std::string((size_t)indent, ' ') + "}";
        }

        void writeValue(std::string& out, const Json& value, int indent){
            const std::string line = compact(value);
            if (line.size() <= 72 || (!value.is_object() && !value.is_array())){
                out += line;
                return;
            }
            if (value.is_array()){
                const std::string pad((size_t)(indent + 2), ' ');
                out += "[\n";
                for (size_t i = 0; i < value.size(); i++){
                    out += pad;
                    writeValue(out, value[i], indent + 2);
                    out += (i + 1 < value.size()) ? ",\n" : "\n";
                }
                out += std::string((size_t)indent, ' ') + "]";
                return;
            }
            std::vector<std::pair<std::string, const Json*>> entries;
            for (auto it = value.begin(); it != value.end(); ++it)
                entries.push_back({it.key(), &it.value()});
            writeObject(out, entries, indent);
        }

        // known keys in their documented order, then the others sorted
        std::vector<std::pair<std::string, const Json*>> ordered(const Json& object, const std::vector<std::string>& known){
            std::vector<std::pair<std::string, const Json*>> entries;
            for (const std::string& key : known){
                auto it = object.find(key);
                if (it != object.end())
                    entries.push_back({key, &*it});
            }
            for (auto it = object.begin(); it != object.end(); ++it){   // nlohmann::json objects iterate sorted
                if (std::find(known.begin(), known.end(), it.key()) == known.end())
                    entries.push_back({it.key(), &it.value()});
            }
            return entries;
        }
    }

    std::string canonicalBitmapFontJson(const std::string& text){
        Json doc = Json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
            return text;
        // glyphs by codepoint, kerning by pair (R33)
        Json glyphsSorted;
        std::vector<std::pair<uint32_t, std::string>> glyphKeys;
        if (doc.contains("glyphs") && doc["glyphs"].is_object()){
            for (auto it = doc["glyphs"].begin(); it != doc["glyphs"].end(); ++it)
                glyphKeys.push_back({firstCodepoint(it.key()), it.key()});
            std::sort(glyphKeys.begin(), glyphKeys.end());
        }
        if (doc.contains("kerning") && doc["kerning"].is_array()){
            std::vector<Json> pairs(doc["kerning"].begin(), doc["kerning"].end());
            std::stable_sort(pairs.begin(), pairs.end(), [](const Json& a, const Json& b){
                auto key = [](const Json& k){
                    if (!k.is_array() || k.size() < 2 || !k[0].is_string() || !k[1].is_string())
                        return std::make_pair(0xFFFFFFFFu, 0xFFFFFFFFu);
                    return std::make_pair(firstCodepoint(k[0].get<std::string>()), firstCodepoint(k[1].get<std::string>()));
                };
                return key(a) < key(b);
            });
            doc["kerning"] = Json(pairs);
        }

        static const std::vector<std::string> topKeys = {"format", "version", "source", "advance", "trim", "spacing", "space_width",
                                                         "line_height", "baseline", "fallback", "colored", "alpha_cutoff", "glyphs",
                                                         "kerning", "fallback_fonts"};
        static const std::vector<std::string> sourceKeys = {"type", "image", "file", "cell", "margin", "gap", "chars", "rows",
                                                            "pixel_size", "threshold"};
        std::string out;
        const std::vector<std::pair<std::string, const Json*>> top = ordered(doc, topKeys);
        out += "{\n";
        for (size_t i = 0; i < top.size(); i++){
            const std::string& key = top[i].first;
            out += "  " + Json(key).dump() + ": ";
            if (key == "source" && top[i].second->is_object()){
                writeObject(out, ordered(*top[i].second, sourceKeys), 2);
            }else if (key == "glyphs" && top[i].second->is_object()){
                std::vector<std::pair<std::string, const Json*>> entries;
                for (const auto& [codepoint, name] : glyphKeys){
                    (void)codepoint;
                    entries.push_back({name, &(*top[i].second)[name]});
                }
                writeObject(out, entries, 2);
            }else if (key == "kerning" && top[i].second->is_array() && !top[i].second->empty()){
                out += "[\n";
                const Json& pairs = *top[i].second;
                for (size_t p = 0; p < pairs.size(); p++)
                    out += "    " + compact(pairs[p]) + ((p + 1 < pairs.size()) ? ",\n" : "\n");
                out += "  ]";
            }else{
                writeValue(out, *top[i].second, 2);
            }
            out += (i + 1 < top.size()) ? ",\n" : "\n";
        }
        out += "}\n";
        return out;
    }

    // ---- building

    class BitmapFontBuilder {
    public:
        // packs the glyphs into R8 or RGBA8 pages of at most 2048, 1 px gutter all around (R11)
        static void pack(BitmapFont& font, std::vector<PackGlyph>& glyphs){
            const int channels = font.colored ? 4 : 1;
            std::vector<size_t> order(glyphs.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&glyphs](size_t a, size_t b){
                if (glyphs[a].height != glyphs[b].height) return glyphs[a].height > glyphs[b].height;
                if (glyphs[a].width != glyphs[b].width) return glyphs[a].width > glyphs[b].width;
                return glyphs[a].codepoint < glyphs[b].codepoint;
            });

            // the smallest square (then 2:1) size holding everything, else full pages
            int size = 32;
            std::vector<std::unique_ptr<GlyphAtlas>> atlases;
            std::vector<std::array<int, 3>> places(glyphs.size());   // page, x, y
            auto tryPack = [&](int width, int height, bool multi) -> bool{
                atlases.clear();
                auto open = [&](){
                    atlases.push_back(std::make_unique<GlyphAtlas>((unsigned int)width, (unsigned int)height, 1, channels));
                    atlases.back()->reserveLeading(1);
                };
                open();
                for (size_t i : order){
                    const PackGlyph& g = glyphs[i];
                    if (g.width == 0 || g.height == 0){
                        places[i] = {0, 0, 0};
                        continue;
                    }
                    int x = 0, y = 0;
                    if (!atlases.back()->pack(g.width, g.height, x, y)){
                        if (!multi)
                            return false;
                        open();
                        if (!atlases.back()->pack(g.width, g.height, x, y))
                            return false;
                    }
                    places[i] = {(int)atlases.size() - 1, x, y};
                }
                return true;
            };
            bool packed = false;
            while (size <= kMaxAtlasPage && !packed){
                packed = tryPack(size, size, false);
                if (!packed && size * 2 <= kMaxAtlasPage)
                    packed = tryPack(size * 2, size, false);
                if (!packed)
                    size *= 2;
            }
            if (!packed)
                tryPack(kMaxAtlasPage, kMaxAtlasPage, true);

            std::vector<uint8_t> pixels;
            for (size_t i = 0; i < glyphs.size(); i++){
                const PackGlyph& g = glyphs[i];
                BitmapGlyph glyph;
                glyph.offsetX = (int16_t)g.offsetX;
                glyph.offsetY = (int16_t)g.offsetY;
                glyph.advance = (int16_t)g.advance;
                if (g.width > 0 && g.height > 0){
                    glyph.page = (uint16_t)places[i][0];
                    glyph.x = (uint16_t)places[i][1];
                    glyph.y = (uint16_t)places[i][2];
                    glyph.w = (uint16_t)g.width;
                    glyph.h = (uint16_t)g.height;
                    if (channels == 4){
                        atlases[(size_t)glyph.page]->blit(g.rgba.data(), g.width, g.height, glyph.x, glyph.y);
                    }else{
                        pixels.resize((size_t)g.width * (size_t)g.height);
                        for (size_t p = 0; p < pixels.size(); p++) pixels[p] = g.rgba[p * 4 + 3];
                        atlases[(size_t)glyph.page]->blit(pixels.data(), g.width, g.height, glyph.x, glyph.y);
                    }
                }
                if (g.codepoint == kFallbackBoxKey){
                    font.fallbackGlyph = glyph;
                }else{
                    font.glyphs[g.codepoint] = glyph;
                }
            }
            font.pages.clear();
            for (const std::unique_ptr<GlyphAtlas>& atlas : atlases){
                BitmapFontPage page;
                page.width = (int)atlas->getWidth();
                page.height = (int)atlas->getHeight();
                page.channels = channels;
                page.pixels = atlas->getPixels();
                font.pages.push_back(std::move(page));
            }
            font.codepoints.clear();
            for (const auto& entry : font.glyphs) font.codepoints.push_back(entry.first);
            std::sort(font.codepoints.begin(), font.codepoints.end());
            font.makeTextures();
        }

        // every glyph of `font` back as an image to pack again
        static std::vector<PackGlyph> unpack(const BitmapFont& font){
            std::vector<PackGlyph> out;
            auto add = [&](uint32_t codepoint, const BitmapGlyph& g){
                PackGlyph p;
                p.codepoint = codepoint;
                p.width = g.w;
                p.height = g.h;
                p.offsetX = g.offsetX;
                p.offsetY = g.offsetY;
                p.advance = g.advance;
                font.copyGlyphRgba(g, p.rgba);
                out.push_back(std::move(p));
            };
            for (uint32_t c : font.codepoints) add(c, font.glyphs.at(c));
            add(kFallbackBoxKey, font.fallbackGlyph);
            return out;
        }

        static std::shared_ptr<BitmapFont> build(RawFont raw, const BitmapFontSettings& s, BitmapFontReport* reportOut){
            std::shared_ptr<BitmapFont> font = std::make_shared<BitmapFont>();
            BitmapFontReport& report = font->report;
            report.detections = raw.detections;
            report.cellWidth = raw.cellWidth;
            report.cellHeight = raw.cellHeight;

            // alpha_cutoff (the S42 fix for sloppy sheets), then what the texels say
            size_t opaque = 0, partial = 0;
            bool sameRgb = true, haveRgb = false;
            uint8_t rgb[3] = {0, 0, 0};
            for (RawGlyph& g : raw.glyphs){
                for (size_t i = 0; i + 3 < g.rgba.size(); i += 4){
                    uint8_t& a = g.rgba[i + 3];
                    if (s.alphaCutoff > 0)
                        a = (a >= s.alphaCutoff) ? 255 : 0;
                    if (a == 0)
                        continue;
                    opaque++;
                    if (a < 255) partial++;
                    if (!haveRgb){
                        rgb[0] = g.rgba[i]; rgb[1] = g.rgba[i + 1]; rgb[2] = g.rgba[i + 2];
                        haveRgb = true;
                    }else if (g.rgba[i] != rgb[0] || g.rgba[i + 1] != rgb[1] || g.rgba[i + 2] != rgb[2]){
                        sameRgb = false;
                    }
                }
            }
            report.partialAlphaShare = opaque ? (float)((double)partial / (double)opaque) : 0.0f;
            font->colored = s.colored < 0 ? !sameRgb : s.colored == 1;

            const int cellH = raw.cellHeight > 0 ? raw.cellHeight : std::max(raw.nativeSize, 1);
            font->nativeSize = std::max(1, raw.nativeSize > 0 ? raw.nativeSize : cellH);
            font->spacing = s.spacing;
            if (s.spaceWidth) font->spaceWidth = *s.spaceWidth;
            else if (raw.spaceAdvance >= 0 && raw.fileAdvances) font->spaceWidth = raw.spaceAdvance;
            else font->spaceWidth = std::max(1, (int)std::lround((double)cellH / 3.0));
            font->lineHeight = s.lineHeight ? *s.lineHeight : (raw.lineHeight > 0 ? raw.lineHeight : cellH + 1);

            // baseline: the most common bottom ink row of A-Z and 0-9, plus 1
            if (s.baseline){
                font->baseline = *s.baseline;
            }else if (raw.baseline >= 0){
                font->baseline = raw.baseline;
            }else{
                std::map<int, int> votes;
                for (const RawGlyph& g : raw.glyphs){
                    if (!((g.codepoint >= 'A' && g.codepoint <= 'Z') || (g.codepoint >= '0' && g.codepoint <= '9')))
                        continue;
                    const int bottom = inkBottom(g);
                    if (bottom >= 0) votes[g.offsetY + bottom + 1]++;
                }
                int best = cellH, bestVotes = 0;
                for (const auto& [row, count] : votes){
                    if (count > bestVotes){ best = row; bestVotes = count; }
                }
                font->baseline = best;
                if (bestVotes > 0)
                    report.detections.push_back("baseline " + std::to_string(best) + " from the bottom of A-Z and 0-9");
            }

            const BitmapFontSettings::Advance mode = s.advance ? *s.advance
                                                               : (raw.fileAdvances ? BitmapFontSettings::Advance::FILE : BitmapFontSettings::Advance::AUTO);
            std::vector<PackGlyph> glyphs;
            bool hasSpace = false;
            int cellsUsed = 0;
            for (const RawGlyph& g : raw.glyphs){
                PackGlyph p;
                p.codepoint = g.codepoint;
                p.width = g.width;
                p.height = g.height;
                p.rgba = g.rgba;
                p.offsetX = g.offsetX;
                p.offsetY = g.offsetY;
                if (g.cell >= 0) cellsUsed++;
                int left = 0, right = -1;
                const bool ink = inkColumns(g, left, right);
                if (ink && s.trimBoth)
                    p.offsetX -= g.offsetX + left;
                if (g.codepoint == 32){
                    hasSpace = true;
                    p.advance = font->spaceWidth;
                    p.width = p.height = 0;
                    p.rgba.clear();
                }else if (!ink){
                    report.emptyGlyphs.push_back(g.codepoint);
                    p.advance = (mode == BitmapFontSettings::Advance::FILE && g.fileAdvance >= 0) ? g.fileAdvance : font->spaceWidth;
                }else{
                    const int autoAdvance = p.offsetX + right + 1;
                    switch (mode){
                        case BitmapFontSettings::Advance::AUTO: p.advance = autoAdvance; break;
                        case BitmapFontSettings::Advance::MONOSPACE: p.advance = raw.cellWidth > 0 ? raw.cellWidth : autoAdvance; break;
                        case BitmapFontSettings::Advance::FILE: p.advance = g.fileAdvance >= 0 ? g.fileAdvance : autoAdvance; break;
                        case BitmapFontSettings::Advance::FIXED: p.advance = s.advanceValue; break;
                    }
                }
                glyphs.push_back(std::move(p));
            }
            if (!hasSpace){
                PackGlyph space;
                space.codepoint = 32;
                space.advance = font->spaceWidth;
                glyphs.push_back(std::move(space));
            }

            // per-glyph overrides
            for (const auto& [codepoint, o] : s.glyphs){
                auto it = std::find_if(glyphs.begin(), glyphs.end(), [cp = codepoint](const PackGlyph& p){ return p.codepoint == cp; });
                if (it == glyphs.end()){
                    report.warnings.push_back(fontError("font_override_unknown_glyph", "glyphs." + codepointToUtf8(codepoint) + " names a glyph the font lacks",
                                                        "remove the override or add the character to the source", "glyphs." + codepointToUtf8(codepoint)));
                    continue;
                }
                if (o.advance) it->advance = *o.advance;
                it->offsetX += o.offsetX;
                it->offsetY += o.offsetY;
                report.overridesApplied++;
            }

            for (PackGlyph& p : glyphs) cropToInk(p);

            // the fallback glyph: "?" (or the file's) when present, else a hollow box
            uint32_t fallback = '?';
            if (!s.fallback.empty()) singleCodepoint(s.fallback, fallback);
            auto fallbackIt = std::find_if(glyphs.begin(), glyphs.end(), [fallback](const PackGlyph& p){ return p.codepoint == fallback; });
            PackGlyph box;
            box.codepoint = kFallbackBoxKey;
            if (fallbackIt != glyphs.end()){
                box.width = fallbackIt->width;
                box.height = fallbackIt->height;
                box.rgba = fallbackIt->rgba;
                box.offsetX = fallbackIt->offsetX;
                box.offsetY = fallbackIt->offsetY;
                box.advance = fallbackIt->advance;
            }else{
                if (!s.fallback.empty())
                    report.warnings.push_back(fontError("font_fallback_missing", "the fallback " + s.fallback + " is not in the font: a box is drawn",
                                                        "set fallback to a character of the font", "fallback"));
                const int cw = raw.cellWidth > 0 ? raw.cellWidth : (font->nativeSize + 1) / 2 + 1;
                box.width = std::max(3, cw - 1);
                box.height = std::max(3, font->baseline);
                box.offsetY = font->baseline - box.height;
                box.advance = box.width;
                box.rgba.assign((size_t)box.width * (size_t)box.height * 4, 0);
                for (int y = 0; y < box.height; y++){
                    for (int x = 0; x < box.width; x++){
                        if (x == 0 || y == 0 || x == box.width - 1 || y == box.height - 1){
                            uint8_t* px = &box.rgba[((size_t)y * (size_t)box.width + (size_t)x) * 4];
                            px[0] = px[1] = px[2] = px[3] = 255;
                        }
                    }
                }
            }
            glyphs.push_back(std::move(box));

            // kerning: the source's, then the file's over it
            for (const BitmapFontSettings::Kern& k : raw.kerning)
                if (k.amount != 0) font->kerningTable[kernKey(k.left, k.right)] = k.amount;
            for (const BitmapFontSettings::Kern& k : s.kerning){
                font->kerningTable[kernKey(k.left, k.right)] = k.amount;
                report.overridesApplied++;
            }

            pack(*font, glyphs);

            report.glyphs = (int)font->glyphs.size();
            report.cellsUsed = cellsUsed;
            report.cellsEmpty = std::max(0, raw.cellsTotal - cellsUsed);
            report.nativeSize = font->nativeSize;
            report.lineHeight = font->lineHeight;
            report.baseline = font->baseline;
            report.colored = font->colored;
            report.kerningPairs = (int)font->kerningTable.size();
            for (uint32_t c = 0x20; c <= 0x7E; c++)
                if (!font->glyphs.count(c)) report.missingAscii.push_back(c);
            std::sort(report.emptyGlyphs.begin(), report.emptyGlyphs.end());
            if (s.version > kBitmapFontVersion){
                report.warnings.push_back(fontError("font_newer_version", "version " + std::to_string(s.version) + " is newer than this editor's " +
                                                    std::to_string(kBitmapFontVersion) + ": loaded read-only", "update the editor", "version"));
            }
            if (reportOut) *reportOut = report;
            return font;
        }
    };

    std::shared_ptr<BitmapFont> BitmapFont::build(RawFont raw, const BitmapFontSettings& settings, BitmapFontReport* report){
        return BitmapFontBuilder::build(std::move(raw), settings, report);
    }

    uint64_t BitmapFont::nextSerial(){
        static std::atomic<uint64_t> next{1};
        return next++;
    }

    void BitmapFont::makeTextures(){
        for (BitmapFontPage& page : pages){
            page.texture = std::make_unique<TextureData>(page.width, page.height, (unsigned int)page.pixels.size(),
                                                         page.channels == 4 ? ColorFormat::RGBA : ColorFormat::RED, page.channels,
                                                         (void*)page.pixels.data());
        }
    }

    TextureData* BitmapFont::getPageTexture(size_t page) const{
        return page < pages.size() ? pages[page].texture.get() : nullptr;
    }

    void BitmapFont::copyGlyphRgba(const BitmapGlyph& g, std::vector<uint8_t>& out) const{
        out.assign((size_t)g.w * (size_t)g.h * 4, 0);
        if (g.w == 0 || g.h == 0 || g.page >= pages.size())
            return;
        const BitmapFontPage& page = pages[g.page];
        for (int y = 0; y < g.h; y++){
            for (int x = 0; x < g.w; x++){
                const size_t src = ((size_t)(g.y + y) * (size_t)page.width + (size_t)(g.x + x)) * (size_t)page.channels;
                uint8_t* dst = &out[((size_t)y * (size_t)g.w + (size_t)x) * 4];
                if (page.channels == 4){
                    std::memcpy(dst, &page.pixels[src], 4);
                }else{
                    dst[0] = dst[1] = dst[2] = 255;
                    dst[3] = page.pixels[src];
                }
            }
        }
    }

    // ---- queries

    const BitmapGlyph* BitmapFont::findGlyph(uint32_t codepoint) const{
        auto it = glyphs.find(codepoint);
        return it == glyphs.end() ? nullptr : &it->second;
    }

    const BitmapGlyph* BitmapFont::glyph(uint32_t codepoint) const{
        const BitmapGlyph* g = findGlyph(codepoint);
        return g ? g : &fallbackGlyph;
    }

    bool BitmapFont::hasGlyph(uint32_t codepoint) const{
        return glyphs.count(codepoint) != 0;
    }

    int BitmapFont::kerning(uint32_t left, uint32_t right) const{
        if (kerningTable.empty())
            return 0;
        auto it = kerningTable.find(kernKey(left, right));
        return it == kerningTable.end() ? 0 : it->second;
    }

    int BitmapFont::advanceOf(uint32_t codepoint) const{
        return glyph(codepoint)->advance;
    }

    int BitmapFont::measure(std::u32string_view line) const{
        int width = 0;
        uint32_t previous = 0;
        for (size_t i = 0; i < line.size(); i++){
            const uint32_t c = (uint32_t)line[i];
            if (i > 0) width += spacing + kerning(previous, c);
            width += advanceOf(c);
            previous = c;
        }
        return width;
    }

    int BitmapFont::getXHeight() const{
        const BitmapGlyph* x = findGlyph('x');
        if (x && x->h > 0)
            return std::max(1, baseline - x->offsetY);
        return std::max(1, (int)std::lround((double)baseline * 0.6));
    }

    // ---- merging (R14)

    std::shared_ptr<BitmapFont> BitmapFont::merged(const std::shared_ptr<BitmapFont>& primary, const std::vector<std::shared_ptr<BitmapFont>>& others){
        if (!primary)
            return nullptr;
        std::shared_ptr<BitmapFont> font = std::make_shared<BitmapFont>();
        font->path = primary->path;
        font->dependencies = primary->dependencies;
        font->report = primary->report;
        font->nativeSize = primary->nativeSize;
        font->lineHeight = primary->lineHeight;
        font->baseline = primary->baseline;
        font->spacing = primary->spacing;
        font->spaceWidth = primary->spaceWidth;
        font->colored = primary->colored;
        font->kerningTable = primary->kerningTable;
        std::vector<PackGlyph> glyphs = BitmapFontBuilder::unpack(*primary);
        std::set<uint32_t> have(primary->codepoints.begin(), primary->codepoints.end());
        for (const std::shared_ptr<BitmapFont>& other : others){
            if (!other || other.get() == primary.get())
                continue;
            font->dependencies.insert(font->dependencies.end(), other->dependencies.begin(), other->dependencies.end());
            for (uint32_t c : other->codepoints){
                if (have.count(c))
                    continue;
                have.insert(c);
                font->colored = font->colored || other->colored;
                PackGlyph p;
                const BitmapGlyph& g = other->glyphs.at(c);
                p.codepoint = c;
                p.width = g.w;
                p.height = g.h;
                // the other font's baseline lands on this one's
                p.offsetX = g.offsetX;
                p.offsetY = g.offsetY + (primary->baseline - other->baseline);
                p.advance = g.advance;
                other->copyGlyphRgba(g, p.rgba);
                glyphs.push_back(std::move(p));
            }
            for (const auto& [key, amount] : other->kerningTable)
                font->kerningTable.emplace(key, amount);
        }
        BitmapFontBuilder::pack(*font, glyphs);
        font->report.glyphs = (int)font->glyphs.size();
        font->report.colored = font->colored;
        return font;
    }

    // ---- loading

    std::shared_ptr<BitmapFont> BitmapFont::builtinPixel(){
        static std::weak_ptr<BitmapFont> cached;
        if (std::shared_ptr<BitmapFont> font = cached.lock())
            return font;
        RawFont raw = builtinPixelRaw();
        BitmapFontSettings settings;
        std::shared_ptr<BitmapFont> font = build(std::move(raw), settings, nullptr);
        font->path = kBuiltinPixelFont;
        cached = font;
        return font;
    }

    std::shared_ptr<BitmapFont> BitmapFont::loadBMFont(const std::string& path, const std::vector<uint8_t>& bytes,
                                                       const BitmapFontSettings* settings, BitmapFontReport* reportOut){
        BitmapFontReport failed;
        BMFontFile file;
        std::vector<FontError> errors;
        if (!parseBMFont(bytes, file, errors)){
            failed.errors = errors;
            if (reportOut) *reportOut = failed;
            return nullptr;
        }
        std::vector<DecodedPage> pages(file.pages.size());
        std::vector<std::string> dependencies;
        for (size_t i = 0; i < file.pages.size(); i++){
            if (file.pages[i].empty())
                continue;
            const std::string pagePath = resolveFontRelative(path, file.pages[i]);
            std::vector<uint8_t> pageBytes;
            if (!readBytes(pagePath, pageBytes) || !decodeImage(pageBytes, pages[i])){
                failed.errors.push_back(fontError("font_bmfont_page_missing", "page " + std::to_string(i) + " (" + file.pages[i] + ") cannot be read",
                                                  "copy the page next to the .fnt", "pages"));
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            dependencies.push_back(pagePath);
        }
        RawFont raw;
        if (!bmfontToRaw(file, pages, raw, errors)){
            failed.errors = errors;
            if (reportOut) *reportOut = failed;
            return nullptr;
        }
        static const char* encodings[] = {"text", "XML", "binary"};
        raw.detections.push_back(std::string("BMFont ") + encodings[(int)file.encoding] + " file, " + std::to_string(file.chars.size()) + " chars");
        BitmapFontSettings defaults;
        std::shared_ptr<BitmapFont> font = build(std::move(raw), settings ? *settings : defaults, reportOut);
        font->path = path;
        font->dependencies = dependencies;
        return font;
    }

    std::shared_ptr<BitmapFont> BitmapFont::loadSettings(const std::string& path, const std::string& json, BitmapFontReport* reportOut){
        BitmapFontReport failed;
        BitmapFontSettings settings;
        if (!parseBitmapFontSettings(json, settings, failed.errors)){
            if (reportOut) *reportOut = failed;
            return nullptr;
        }
        const std::string source = resolveFontRelative(path, settings.file);
        std::shared_ptr<BitmapFont> font;
        std::vector<std::string> dependencies = {path, source};
        if (settings.source == BitmapFontSettings::Source::GRID){
            std::vector<uint8_t> bytes;
            DecodedPage image;
            if (!readBytes(source, bytes) || !decodeImage(bytes, image)){
                failed.errors.push_back(fontError("font_source_missing", "the sheet " + settings.file + " cannot be read",
                                                  "put the PNG next to the .bitmapfont or fix source.image", "source.image"));
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            GridOptions options;
            options.cellWidth = settings.cellWidth;
            options.cellHeight = settings.cellHeight;
            options.marginX = settings.marginX;
            options.marginY = settings.marginY;
            options.gapX = settings.gapX;
            options.gapY = settings.gapY;
            options.imagePath = settings.file;
            if (!settings.rows.empty()){
                for (const std::string& row : settings.rows){
                    bool invalid = false;
                    options.rows.push_back(StringUtils::decodeUtf8ToCodepoints(row, invalid));
                }
            }else{
                options.chars = expandFontChars(settings.chars, "ascii");
            }
            RawFont raw;
            if (!parseGrid(image.rgba.data(), image.width, image.height, options, raw, failed.errors)){
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            font = build(std::move(raw), settings, reportOut);
        }else if (settings.source == BitmapFontSettings::Source::BMFONT){
            std::vector<uint8_t> bytes;
            if (!readBytes(source, bytes)){
                failed.errors.push_back(fontError("font_source_missing", "the BMFont file " + settings.file + " cannot be read",
                                                  "fix source.file", "source.file"));
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            font = loadBMFont(source, bytes, &settings, reportOut);
            if (!font)
                return nullptr;
            dependencies.insert(dependencies.end(), font->dependencies.begin(), font->dependencies.end());
        }else{
            std::string error;
            std::shared_ptr<FontFace> face = FontFace::loadFile(source, 0, &error);
            if (!face){
                failed.errors.push_back(fontError("font_source_missing", "the font file " + settings.file + " cannot be read",
                                                  "fix source.file", "source.file"));
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            TrueTypeOptions options;
            options.pixelSize = settings.pixelSize;
            options.threshold = settings.threshold;
            options.chars = expandFontChars(settings.chars, "latin1");
            RawFont raw;
            if (!rasterTrueType(*face, options, raw, failed.errors)){
                if (reportOut) *reportOut = failed;
                return nullptr;
            }
            font = build(std::move(raw), settings, reportOut);
        }
        font->path = path;
        font->dependencies = dependencies;

        // fallback_fonts: their glyphs join this font's atlas (R14)
        if (!settings.fallbackFonts.empty()){
            if (fallbackDepth() >= 4){
                font->report.warnings.push_back(fontError("font_fallback_cycle", "fallback_fonts nest more than 4 deep: the rest is skipped",
                                                          "remove the cycle from fallback_fonts", "fallback_fonts"));
            }else{
                fallbackDepth()++;
                std::vector<std::shared_ptr<BitmapFont>> others;
                for (size_t i = 0; i < settings.fallbackFonts.size(); i++){
                    const std::string otherPath = resolveFontRelative(path, settings.fallbackFonts[i]);
                    BitmapFontReport otherReport;
                    std::shared_ptr<BitmapFont> other = (otherPath == path) ? nullptr : load(otherPath, &otherReport);
                    const std::string field = "fallback_fonts[" + std::to_string(i) + "]";
                    if (!other){
                        font->report.warnings.push_back(fontError("font_fallback_missing", settings.fallbackFonts[i] + " does not load",
                                                                  "fix or remove it", field));
                    }else if (other->getNativeSize() != font->getNativeSize()){
                        font->report.warnings.push_back(fontError("font_fallback_size", settings.fallbackFonts[i] + " is " +
                                                                  std::to_string(other->getNativeSize()) + " px, not " +
                                                                  std::to_string(font->getNativeSize()) + ": skipped",
                                                                  "use a fallback of the same native size", field));
                    }else{
                        others.push_back(other);
                    }
                }
                fallbackDepth()--;
                if (!others.empty()){
                    BitmapFontReport kept = font->report;
                    font = merged(font, others);
                    font->report = kept;
                    font->report.glyphs = (int)font->getGlyphCount();
                }
            }
        }
        if (reportOut) *reportOut = font->report;
        return font;
    }

    std::shared_ptr<BitmapFont> BitmapFont::load(const std::string& path, BitmapFontReport* reportOut){
        if (path == kBuiltinPixelFont){
            std::shared_ptr<BitmapFont> font = builtinPixel();
            if (reportOut) *reportOut = font->report;
            return font;
        }
        const std::string ext = lowerExtension(path);
        std::vector<uint8_t> bytes;
#ifndef JASPERENGINE_EDITOR
        // a shipped game reads the cooked font next to its source (R34); old exports keep the source
        if (ext != ".jfont" && readBytes(path + ".jfont", bytes)){
            std::shared_ptr<BitmapFont> cooked = loadCooked(bytes, reportOut);
            if (cooked){
                cooked->path = path;
                return cooked;
            }
        }
#endif
        if (!readBytes(path, bytes)){
            BitmapFontReport failed;
            failed.errors.push_back(fontError("font_file_missing", "the font " + path + " cannot be read", "check the path", "path"));
            if (reportOut) *reportOut = failed;
            return nullptr;
        }
        if (ext == ".jfont"){
            std::shared_ptr<BitmapFont> font = loadCooked(bytes, reportOut);
            if (font) font->path = path;
            return font;
        }
        if (ext == ".fnt")
            return loadBMFont(path, bytes, nullptr, reportOut);
        return loadSettings(path, std::string(bytes.begin(), bytes.end()), reportOut);
    }

    // ---- cooked .jfont (R34)

    namespace{
        void put(std::vector<uint8_t>& out, uint32_t value, int bytes){
            for (int i = 0; i < bytes; i++) out.push_back((uint8_t)(value >> (8 * i)));
        }
        void putGlyph(std::vector<uint8_t>& out, const BitmapGlyph& g){
            put(out, g.page, 2); put(out, g.x, 2); put(out, g.y, 2); put(out, g.w, 2); put(out, g.h, 2);
            put(out, (uint16_t)g.offsetX, 2); put(out, (uint16_t)g.offsetY, 2); put(out, (uint16_t)g.advance, 2);
        }
        struct CookedReader {
            const std::vector<uint8_t>& bytes;
            size_t at = 0;
            bool ok = true;
            uint32_t u(int n){
                if (at + (size_t)n > bytes.size()){ ok = false; return 0; }
                uint32_t v = 0;
                for (int i = 0; i < n; i++) v |= (uint32_t)bytes[at + (size_t)i] << (8 * i);
                at += (size_t)n;
                return v;
            }
            int s(int n){ return n == 2 ? (int)(int16_t)(uint16_t)u(2) : (int)u(4); }
            BitmapGlyph glyph(){
                BitmapGlyph g;
                g.page = (uint16_t)u(2); g.x = (uint16_t)u(2); g.y = (uint16_t)u(2); g.w = (uint16_t)u(2); g.h = (uint16_t)u(2);
                g.offsetX = (int16_t)s(2); g.offsetY = (int16_t)s(2); g.advance = (int16_t)s(2);
                return g;
            }
        };
    }

    std::vector<uint8_t> BitmapFont::toCooked() const{
        std::vector<uint8_t> out = {'J', 'F', 'N', 'T'};
        put(out, kCookedVersion, 4);
        put(out, (uint32_t)nativeSize, 4);
        put(out, (uint32_t)lineHeight, 4);
        put(out, (uint32_t)baseline, 4);
        put(out, (uint32_t)spacing, 4);
        put(out, (uint32_t)spaceWidth, 4);
        put(out, colored ? 1u : 0u, 1);
        put(out, (uint32_t)codepoints.size(), 4);
        for (uint32_t c : codepoints){
            put(out, c, 4);
            putGlyph(out, glyphs.at(c));
        }
        putGlyph(out, fallbackGlyph);
        std::vector<std::pair<uint64_t, int>> kerns(kerningTable.begin(), kerningTable.end());
        std::sort(kerns.begin(), kerns.end());
        put(out, (uint32_t)kerns.size(), 4);
        for (const auto& [key, amount] : kerns){
            put(out, (uint32_t)(key >> 32), 4);
            put(out, (uint32_t)(key & 0xFFFFFFFFu), 4);
            put(out, (uint16_t)(int16_t)amount, 2);
        }
        put(out, (uint32_t)pages.size(), 4);
        for (const BitmapFontPage& page : pages){
            put(out, (uint32_t)page.width, 2);
            put(out, (uint32_t)page.height, 2);
            put(out, (uint32_t)page.channels, 1);
            int length = 0;
            unsigned char* deflated = stbi_zlib_compress((unsigned char*)page.pixels.data(), (int)page.pixels.size(), &length, 8);
            put(out, (uint32_t)length, 4);
            if (deflated){
                out.insert(out.end(), deflated, deflated + length);
                std::free(deflated);
            }
        }
        // the report's measured values ride along for get_asset_info on an exported font
        put(out, (uint32_t)report.cellWidth, 4);
        put(out, (uint32_t)report.cellHeight, 4);
        put(out, (uint32_t)std::lround(report.partialAlphaShare * 1000000.0f), 4);
        return out;
    }

    std::shared_ptr<BitmapFont> BitmapFont::loadCooked(const std::vector<uint8_t>& bytes, BitmapFontReport* reportOut){
        auto fail = [reportOut](const std::string& why){
            if (reportOut){
                *reportOut = BitmapFontReport();
                reportOut->errors.push_back(fontError("font_cooked_invalid", "the .jfont " + why, "export the game again", "file"));
            }
            return std::shared_ptr<BitmapFont>();
        };
        if (bytes.size() < 8 || std::memcmp(bytes.data(), "JFNT", 4) != 0)
            return fail("has no JFNT header");
        CookedReader r{bytes, 4};
        if (r.u(4) != kCookedVersion)
            return fail("is of another version");
        std::shared_ptr<BitmapFont> font = std::make_shared<BitmapFont>();
        font->nativeSize = r.s(4);
        font->lineHeight = r.s(4);
        font->baseline = r.s(4);
        font->spacing = r.s(4);
        font->spaceWidth = r.s(4);
        font->colored = r.u(1) != 0;
        const uint32_t count = r.u(4);
        if (!r.ok || count > 0x110000)
            return fail("is truncated");
        for (uint32_t i = 0; i < count && r.ok; i++){
            const uint32_t c = r.u(4);
            font->glyphs[c] = r.glyph();
            font->codepoints.push_back(c);
        }
        font->fallbackGlyph = r.glyph();
        const uint32_t kerns = r.u(4);
        for (uint32_t i = 0; i < kerns && r.ok; i++){
            const uint32_t left = r.u(4), right = r.u(4);
            font->kerningTable[kernKey(left, right)] = r.s(2);
        }
        const uint32_t pageCount = r.u(4);
        if (!r.ok || pageCount > 64)
            return fail("is truncated");
        for (uint32_t i = 0; i < pageCount; i++){
            BitmapFontPage page;
            page.width = (int)r.u(2);
            page.height = (int)r.u(2);
            page.channels = (int)r.u(1);
            const uint32_t length = r.u(4);
            if (!r.ok || r.at + length > bytes.size() || (page.channels != 1 && page.channels != 4) ||
                    page.width <= 0 || page.height <= 0 || page.width > kMaxAtlasPage || page.height > kMaxAtlasPage)
                return fail("is truncated");
            page.pixels.resize((size_t)page.width * (size_t)page.height * (size_t)page.channels);
            const int decoded = stbi_zlib_decode_buffer((char*)page.pixels.data(), (int)page.pixels.size(),
                                                        (const char*)bytes.data() + r.at, (int)length);
            if (decoded != (int)page.pixels.size())
                return fail("has a damaged page");
            r.at += length;
            font->pages.push_back(std::move(page));
        }
        font->report.cellWidth = r.s(4);
        font->report.cellHeight = r.s(4);
        font->report.partialAlphaShare = (float)r.u(4) / 1000000.0f;
        if (!r.ok)
            return fail("is truncated");
        std::sort(font->codepoints.begin(), font->codepoints.end());
        font->makeTextures();
        BitmapFontReport& report = font->report;
        report.glyphs = (int)font->glyphs.size();
        report.nativeSize = font->nativeSize;
        report.lineHeight = font->lineHeight;
        report.baseline = font->baseline;
        report.colored = font->colored;
        report.kerningPairs = (int)font->kerningTable.size();
        for (uint32_t c = 0x20; c <= 0x7E; c++)
            if (!font->glyphs.count(c)) report.missingAscii.push_back(c);
        if (reportOut) *reportOut = report;
        return font;
    }

}
