// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontFace.h"

#include "io/Data.h"

#include "hb.h"
#include "hb-ot.h"

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

std::shared_ptr<FontFace> FontFace::loadFile(const std::string& path, int faceIndex, std::string* error, FontFaceRasterizer rasterizer){
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

    if (!fontFace->init(faceIndex, error, rasterizer))
        return nullptr;

    return fontFace;
}

std::shared_ptr<FontFace> FontFace::loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                               bool copy, const std::string& label, std::string* error,
                                               FontFaceRasterizer rasterizer){
    std::shared_ptr<FontFace> fontFace(new FontFace());
    fontFace->label = label;
    if (copy && data){
        fontFace->storage.assign(data, data + length);
        fontFace->bytes = fontFace->storage.data();
    }else{
        fontFace->bytes = data;
    }
    fontFace->length = data ? length : 0;

    if (!fontFace->init(faceIndex, error, rasterizer))
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

bool FontFace::init(int index, std::string* error, FontFaceRasterizer rasterizer){
    faceIndex = index;

    size_t offset = 0;
    const bool found = findFontOffset(bytes, length, index, offset);
    rasterizable = found && stbtt_InitFont(&info, bytes, (int)offset);
    if (!found || (!rasterizable && rasterizer == FontFaceRasterizer::Required)){
        if (error)
            *error = "Failed to initialize font: " + label;
        return false;
    }

    blob = hb_blob_create((const char*)bytes, (unsigned int)length, HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    face = hb_face_create(blob, (unsigned int)index);
    font = hb_font_create(face);

    if (!font || face == hb_face_get_empty() || hb_face_get_upem(face) == 0 || hb_face_get_glyph_count(face) == 0){
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
    if (rasterizable){
        stbtt_GetFontVMetrics(&info, &ascender, &descender, &lineGap);
    }else{
        //the hhea values stb_truetype reads
        hb_blob_t* hhea = hb_face_reference_table(face, HB_TAG('h', 'h', 'e', 'a'));
        unsigned int hheaLength = 0;
        const unsigned char* hheaData = (const unsigned char*)hb_blob_get_data(hhea, &hheaLength);
        if (hheaData && hheaLength >= 10){
            ascender = readS16(hheaData + 4);
            descender = readS16(hheaData + 6);
            lineGap = readS16(hheaData + 8);
        }
        hb_blob_destroy(hhea);
    }

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
        hb_codepoint_t glyph = 0;
        hb_glyph_extents_t extents = {};
        if (rasterizable && stbtt_GetCodepointBox(&info, 'x', &x0, &y0, &x1, &y1) && y1 > 0){
            xHeight = y1;
        }else if (!rasterizable && hb_font_get_nominal_glyph(font, 'x', &glyph) && hb_font_get_glyph_extents(font, glyph, &extents) &&
                  extents.y_bearing > 0){
            xHeight = extents.y_bearing;
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
    if (!rasterizable){
        hb_codepoint_t glyph = 0;
        return hb_font_get_nominal_glyph(font, codepoint, &glyph) ? (uint32_t)glyph : 0;
    }
    return (uint32_t)stbtt_FindGlyphIndex(&info, (int)codepoint);
}


float FontFace::scaleForPixelHeight(float pixels) const{
    if (!rasterizable)
        return (ascender - descender) != 0 ? pixels / (float)(ascender - descender) : 0.0f;
    return stbtt_ScaleForPixelHeight(&info, pixels);
}

float FontFace::scaleForEmSize(float pixels) const{
    if (!rasterizable)
        return pixels / (float)unitsPerEm;
    return stbtt_ScaleForMappingEmToPixels(&info, pixels);
}

bool FontFace::rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                              int& width, int& height, int& xoff, int& yoff) const{
    if (!rasterizable){
        width = height = xoff = yoff = 0;
        pixels.clear();
        return false;
    }
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
    if (!rasterizable)
        return hb_font_get_glyph_h_advance(font, glyphIndex) * scale;
    int advance, leftSideBearing;
    stbtt_GetGlyphHMetrics(&info, (int)glyphIndex, &advance, &leftSideBearing);
    return advance * scale;
}

bool FontFace::canRasterize() const{
    return rasterizable;
}

const stbtt_fontinfo& FontFace::getInfo() const{
    return info;
}

hb_font_t* FontFace::getHbFont() const{
    return font;
}
