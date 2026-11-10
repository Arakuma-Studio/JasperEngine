// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontFace.h"

#include "io/Data.h"

#include "hb.h"
#include "hb-ot.h"
#include "stb_image.h"

#include <woff2/decode.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    uint16_t fontReadU16(const unsigned char* p){
        return (uint16_t)((p[0] << 8) | p[1]);
    }

    int16_t readS16(const unsigned char* p){
        return (int16_t)fontReadU16(p);
    }

    uint32_t fontReadU32(const unsigned char* p){
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }

    constexpr size_t maxWebFontBytes = 64 * 1024 * 1024;

    void fontWriteU16(unsigned char* p, uint16_t value){
        p[0] = (unsigned char)(value >> 8);
        p[1] = (unsigned char)value;
    }

    void fontWriteU32(unsigned char* p, uint32_t value){
        p[0] = (unsigned char)(value >> 24);
        p[1] = (unsigned char)(value >> 16);
        p[2] = (unsigned char)(value >> 8);
        p[3] = (unsigned char)value;
    }

    size_t fontPaddedSize(size_t size){
        return (size + 3) & ~size_t(3);
    }

    uint32_t fontChecksum(const unsigned char* data, size_t length, bool head = false){
        uint32_t sum = 0;
        for (size_t i = 0; i < length; i += 4){
            // The head table's checksum excludes checkSumAdjustment.
            if (head && i == 8)
                continue;
            uint32_t word = 0;
            for (size_t j = 0; j < 4; j++)
                word = (word << 8) | (i + j < length ? data[i + j] : 0);
            sum += word;
        }
        return sum;
    }

    uint32_t fontAdler32(const unsigned char* data, size_t length){
        uint32_t a = 1, b = 0;
        while (length){
            const size_t block = std::min(length, size_t(5552));
            for (size_t i = 0; i < block; i++){
                a += data[i];
                b += a;
            }
            a %= 65521;
            b %= 65521;
            data += block;
            length -= block;
        }
        return (b << 16) | a;
    }

    bool decodeWoff(const unsigned char* data, size_t length, std::vector<unsigned char>& decoded){
        if (length < 44 || fontReadU32(data + 8) != length || fontReadU16(data + 14) != 0)
            return false;
        const size_t count = fontReadU16(data + 12);
        const size_t directoryEnd = 44 + count * 20;
        const size_t sfntSize = fontReadU32(data + 16);
        if (count == 0 || count > 4095 || directoryEnd > length || sfntSize > maxWebFontBytes)
            return false;

        size_t computedSize = 12 + count * 16;
        std::vector<std::pair<size_t, size_t>> ranges{{0, directoryEnd}};
        std::vector<size_t> order;
        uint32_t previousTag = 0;
        size_t lastTableEnd = directoryEnd;
        for (size_t i = 0; i < count; i++){
            const unsigned char* entry = data + 44 + i * 20;
            const uint32_t tag = fontReadU32(entry);
            const size_t at = fontReadU32(entry + 4);
            const size_t compressed = fontReadU32(entry + 8);
            const size_t original = fontReadU32(entry + 12);
            if ((i && tag <= previousTag) || at % 4 || at < directoryEnd || at > length ||
                compressed > length - at || compressed > original ||
                fontPaddedSize(original) > maxWebFontBytes - computedSize)
                return false;
            previousTag = tag;
            computedSize += fontPaddedSize(original);
            ranges.emplace_back(at, at + compressed);
            lastTableEnd = std::max(lastTableEnd, fontPaddedSize(at + compressed));
            order.push_back(i);
        }
        if (computedSize != sfntSize)
            return false;

        const size_t metaAt = fontReadU32(data + 24), metaSize = fontReadU32(data + 28);
        const size_t privateAt = fontReadU32(data + 36), privateSize = fontReadU32(data + 40);
        if ((!metaAt && (metaSize || fontReadU32(data + 32))) || (!privateAt && privateSize))
            return false;
        for (const auto& [at, size] : {std::pair{metaAt, metaSize}, std::pair{privateAt, privateSize}}){
            if (!at)
                continue;
            if (!size || at % 4 || at < lastTableEnd || at > length || size > length - at)
                return false;
            ranges.emplace_back(at, at + size);
        }
        if (metaAt && privateAt && metaAt >= privateAt)
            return false;
        std::sort(ranges.begin(), ranges.end());
        size_t end = 0;
        for (const auto& [at, nextEnd] : ranges){
            if (at != fontPaddedSize(end))
                return false;
            for (size_t i = end; i < at; i++)
                if (data[i] != 0)
                    return false;
            end = nextEnd;
        }
        if (length < end || length > fontPaddedSize(end))
            return false;
        for (size_t i = end; i < length; i++)
            if (data[i] != 0)
                return false;

        decoded.assign(sfntSize, 0);
        std::memcpy(decoded.data(), data + 4, 4);
        fontWriteU16(decoded.data() + 4, (uint16_t)count);
        uint16_t selector = 0;
        while ((size_t(1) << (selector + 1)) <= count)
            selector++;
        const uint16_t searchRange = (uint16_t)((size_t(1) << selector) * 16);
        fontWriteU16(decoded.data() + 6, searchRange);
        fontWriteU16(decoded.data() + 8, selector);
        fontWriteU16(decoded.data() + 10, (uint16_t)(count * 16 - searchRange));
        // Keep the original physical table order, including a font's digital signature.
        std::sort(order.begin(), order.end(), [data](size_t a, size_t b){
            return fontReadU32(data + 44 + a * 20 + 4) < fontReadU32(data + 44 + b * 20 + 4);
        });
        size_t outAt = 12 + count * 16;
        size_t headAt = 0;
        for (size_t i : order){
            const unsigned char* entry = data + 44 + i * 20;
            const size_t at = fontReadU32(entry + 4);
            const size_t compressed = fontReadU32(entry + 8);
            const size_t original = fontReadU32(entry + 12);
            unsigned char* table = decoded.data() + outAt;
            if (compressed == original){
                std::memcpy(table, data + at, original);
            }else{
                if (compressed < 6 || stbi_zlib_decode_buffer((char*)table, (int)original,
                        (const char*)data + at, (int)compressed) != (int)original ||
                    fontAdler32(table, original) != fontReadU32(data + at + compressed - 4))
                    return false;
            }
            const bool head = std::memcmp(entry, "head", 4) == 0;
            if (fontChecksum(table, original, head) != fontReadU32(entry + 16))
                return false;
            if (head && original >= 12)
                headAt = outAt;
            unsigned char* sfntEntry = decoded.data() + 12 + i * 16;
            std::memcpy(sfntEntry, entry, 4);
            std::memcpy(sfntEntry + 4, entry + 16, 4);
            fontWriteU32(sfntEntry + 8, (uint32_t)outAt);
            fontWriteU32(sfntEntry + 12, (uint32_t)original);
            outAt += fontPaddedSize(original);
        }
        if (headAt){
            fontWriteU32(decoded.data() + headAt + 8, 0);
            fontWriteU32(decoded.data() + headAt + 8, 0xB1B0AFBAu - fontChecksum(decoded.data(), decoded.size()));
        }
        return true;
    }

    class WebFontOutput final : public woff2::WOFF2Out {
    public:
        explicit WebFontOutput(std::vector<unsigned char>& bytes): bytes(bytes){}

        bool Write(const void* data, size_t count) override{ return Write(data, bytes.size(), count); }

        bool Write(const void* data, size_t offset, size_t count) override{
            if (offset > maxWebFontBytes || count > maxWebFontBytes - offset)
                return false;
            if (offset + count > bytes.size())
                bytes.resize(offset + count);
            if (count)
                std::memcpy(bytes.data() + offset, data, count);
            return true;
        }

        size_t Size() override{ return bytes.size(); }

    private:
        std::vector<unsigned char>& bytes;
    };

    bool isSfntVersion(const unsigned char* p){
        static const unsigned char trueType[4] = {0, 1, 0, 0};
        return std::memcmp(p, trueType, 4) == 0 || std::memcmp(p, "true", 4) == 0 ||
               std::memcmp(p, "typ1", 4) == 0 || std::memcmp(p, "OTTO", 4) == 0 ||
               (p[0] == '1' && p[1] == 0 && p[2] == 0 && p[3] == 0);
    }

    // stb_truetype trusts the file: check the collection header, the table directory and
    // the tables it reads fixed offsets from before handing the bytes to it
    bool findFontOffset(const unsigned char* data, size_t length, int faceIndex, size_t& offset){
        if (!data || length < 12 || faceIndex < 0)
            return false;

        offset = 0;
        if (std::memcmp(data, "ttcf", 4) == 0){
            uint32_t version = fontReadU32(data + 4);
            if (version != 0x00010000 && version != 0x00020000)
                return false;
            uint32_t numFonts = fontReadU32(data + 8);
            if ((uint32_t)faceIndex >= numFonts || 12 + 4 * (size_t)numFonts > length)
                return false;
            offset = fontReadU32(data + 12 + 4 * (size_t)faceIndex);
        }else if (faceIndex != 0){
            return false;
        }

        if (offset + 12 > length || !isSfntVersion(data + offset))
            return false;

        size_t numTables = fontReadU16(data + offset + 4);
        if (numTables == 0 || offset + 12 + 16 * numTables > length)
            return false;

        for (size_t i = 0; i < numTables; i++){
            const unsigned char* record = data + offset + 12 + 16 * i;
            uint64_t tableOffset = fontReadU32(record + 8);
            uint64_t tableLength = fontReadU32(record + 12);
            if (tableOffset + tableLength > length)
                return false;

            size_t minimum = 0;
            if (std::memcmp(record, "head", 4) == 0) minimum = 54;
            else if (std::memcmp(record, "hhea", 4) == 0) minimum = 36;
            else if (std::memcmp(record, "maxp", 4) == 0) minimum = 6;
            else if (std::memcmp(record, "cmap", 4) == 0) minimum = 4;
            if (tableLength < minimum)
                return false;
        }

        return true;
    }

    std::string nameString(hb_face_t* face, hb_ot_name_id_t nameId){
        // English first, then whatever language the font carries
        unsigned int size = 0;
        hb_language_t language = HB_LANGUAGE_INVALID;
        if (hb_ot_name_get_utf8(face, nameId, language, &size, nullptr) == 0){
            unsigned int count = 0;
            const hb_ot_name_entry_t* entries = hb_ot_name_list_names(face, &count);
            for (unsigned int i = 0; i < count; i++){
                if (entries[i].name_id == nameId){
                    language = entries[i].language;
                    break;
                }
            }
        }

        unsigned int length = hb_ot_name_get_utf8(face, nameId, language, nullptr, nullptr);
        if (length == 0)
            return "";

        std::string text(length + 1, '\0');
        unsigned int capacity = length + 1;
        hb_ot_name_get_utf8(face, nameId, language, &capacity, &text[0]);
        text.resize(capacity);
        return text;
    }

}

std::shared_ptr<FontFace> FontFace::loadFile(const std::string& path, int faceIndex, std::string* error){
    Data data;
    if (path.empty() || data.open(path.c_str()) != FileErrors::FILEDATA_OK){
        if (error)
            *error = "Font file not found: " + path;
        return nullptr;
    }

    std::shared_ptr<FontFace> fontFace(new FontFace());
    fontFace->label = path;
    fontFace->storage.assign(data.getMemPtr(), data.getMemPtr() + data.length());
    fontFace->bytes = fontFace->storage.data();
    fontFace->length = fontFace->storage.size();

    if (!fontFace->init(faceIndex, error))
        return nullptr;

    return fontFace;
}

std::shared_ptr<FontFace> FontFace::loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                               bool copy, const std::string& label, std::string* error){
    std::shared_ptr<FontFace> fontFace(new FontFace());
    fontFace->label = label;
    if (copy && data){
        fontFace->storage.assign(data, data + length);
        fontFace->bytes = fontFace->storage.data();
    }else{
        fontFace->bytes = data;
    }
    fontFace->length = data ? length : 0;

    if (!fontFace->init(faceIndex, error))
        return nullptr;

    return fontFace;
}

FontFace::~FontFace(){
    if (font)
        hb_font_destroy(font);
    if (face)
        hb_face_destroy(face);
    if (blob)
        hb_blob_destroy(blob);
}

bool FontFace::init(int index, std::string* error){
    faceIndex = index;

    if (bytes && length >= 4 && (std::memcmp(bytes, "wOFF", 4) == 0 || std::memcmp(bytes, "wOF2", 4) == 0)){
        const bool woff = std::memcmp(bytes, "wOFF", 4) == 0;
        std::vector<unsigned char> decoded;
        WebFontOutput out(decoded);
        const bool valid = length <= maxWebFontBytes && (woff ? decodeWoff(bytes, length, decoded) :
            (length >= 48 && fontReadU32(bytes + 16) <= maxWebFontBytes &&
             woff2::ConvertWOFF2ToTTF(bytes, length, &out)));
        if (!valid){
            if (error)
                *error = std::string("Failed to decode ") + (woff ? "WOFF" : "WOFF2") + " font: " + label +
                         " (invalid data or decoded font exceeds 64 MiB)";
            return false;
        }
        storage = std::move(decoded);
        bytes = storage.data();
        length = storage.size();
    }

    size_t offset = 0;
    if (!findFontOffset(bytes, length, index, offset) || !stbtt_InitFont(&info, bytes, (int)offset)){
        if (error)
            *error = "Failed to initialize font: " + label;
        return false;
    }

    blob = hb_blob_create((const char*)bytes, (unsigned int)length, HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    face = hb_face_create(blob, (unsigned int)index);
    font = hb_font_create(face);

    if (!font || face == hb_face_get_empty() || hb_face_get_upem(face) == 0){
        if (error)
            *error = "Failed to create shaper for font: " + label;
        return false;
    }

    //shaper stays in font units, positions are multiplied by a scale when read
    unitsPerEm = (int)hb_face_get_upem(face);
    hb_font_set_scale(font, unitsPerEm, unitsPerEm);

    readTables();

    for (uint32_t codepoint = 0; codepoint < 128; codepoint++){
        if (getGlyphIndex(codepoint) != 0)
            asciiCoverage[codepoint >> 6] |= uint64_t(1) << (codepoint & 63);
    }

    return true;
}

void FontFace::readTables(){
    stbtt_GetFontVMetrics(&info, &ascender, &descender, &lineGap);

    bool hasOS2 = false;
    int fsSelection = 0;
    int os2Weight = 0;
    int os2XHeight = 0;

    hb_blob_t* os2 = hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2'));
    unsigned int os2Length = 0;
    const unsigned char* os2Data = (const unsigned char*)hb_blob_get_data(os2, &os2Length);
    if (os2Data && os2Length >= 64){
        hasOS2 = true;
        os2Weight = fontReadU16(os2Data + 4);
        fsSelection = fontReadU16(os2Data + 62);
        if (fontReadU16(os2Data) >= 2 && os2Length >= 88){
            os2XHeight = readS16(os2Data + 86);
        }
    }
    hb_blob_destroy(os2);

    int macStyle = 0;
    hb_blob_t* head = hb_face_reference_table(face, HB_TAG('h', 'e', 'a', 'd'));
    unsigned int headLength = 0;
    const unsigned char* headData = (const unsigned char*)hb_blob_get_data(head, &headLength);
    if (headData && headLength >= 46){
        macStyle = fontReadU16(headData + 44);
    }
    hb_blob_destroy(head);

    hb_blob_t* post = hb_face_reference_table(face, HB_TAG('p', 'o', 's', 't'));
    unsigned int postLength = 0;
    const unsigned char* postData = (const unsigned char*)hb_blob_get_data(post, &postLength);
    if (postData && postLength >= 12){
        postTable = true;
        underlinePosition = readS16(postData + 8);
        underlineThickness = readS16(postData + 10);
    }
    hb_blob_destroy(post);

    weight = (os2Weight >= 1 && os2Weight <= 1000) ? os2Weight : 400;
    // bit 0 italic, bit 9 oblique
    italic = hasOS2 ? ((fsSelection & 0x0201) != 0) : ((macStyle & 0x02) != 0);

    if (os2XHeight > 0){
        xHeight = os2XHeight;
    }else{
        int x0, y0, x1, y1;
        if (stbtt_GetCodepointBox(&info, 'x', &x0, &y0, &x1, &y1) && y1 > 0){
            xHeight = y1;
        }else{
            xHeight = ascender / 2;
        }
    }

    familyName = nameString(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
    if (familyName.empty())
        familyName = nameString(face, HB_OT_NAME_ID_FONT_FAMILY);
}

const std::string& FontFace::getLabel() const{
    return label;
}

const std::string& FontFace::getFamilyName() const{
    return familyName;
}

int FontFace::getWeight() const{
    return weight;
}

bool FontFace::isItalic() const{
    return italic;
}

int FontFace::getFaceIndex() const{
    return faceIndex;
}

int FontFace::getUnitsPerEm() const{
    return unitsPerEm;
}

int FontFace::getAscender() const{
    return ascender;
}

int FontFace::getDescender() const{
    return descender;
}

int FontFace::getLineGap() const{
    return lineGap;
}

int FontFace::getXHeight() const{
    return xHeight;
}

int FontFace::getUnderlinePosition() const{
    return underlinePosition;
}

int FontFace::getUnderlineThickness() const{
    return underlineThickness;
}

bool FontFace::hasPostTable() const{
    return postTable;
}

uint32_t FontFace::getGlyphIndex(uint32_t codepoint) const{
    return (uint32_t)stbtt_FindGlyphIndex(&info, (int)codepoint);
}


float FontFace::scaleForPixelHeight(float pixels) const{
    return stbtt_ScaleForPixelHeight(&info, pixels);
}

float FontFace::scaleForEmSize(float pixels) const{
    return stbtt_ScaleForMappingEmToPixels(&info, pixels);
}

bool FontFace::rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                              int& width, int& height, int& xoff, int& yoff) const{
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&info, (int)glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    xoff = x0;
    yoff = y0;
    width = x1 - x0;
    height = y1 - y0;

    if (width <= 0 || height <= 0){
        width = 0;
        height = 0;
        pixels.clear();
        return false;
    }

    pixels.assign((size_t)width * (size_t)height, 0);
    stbtt_MakeGlyphBitmap(&info, pixels.data(), width, height, width, scale, scale, (int)glyphIndex);

    return true;
}

float FontFace::getGlyphAdvance(uint32_t glyphIndex, float scale) const{
    int advance, leftSideBearing;
    stbtt_GetGlyphHMetrics(&info, (int)glyphIndex, &advance, &leftSideBearing);
    return advance * scale;
}

const stbtt_fontinfo& FontFace::getInfo() const{
    return info;
}

hb_font_t* FontFace::getHbFont() const{
    return font;
}
