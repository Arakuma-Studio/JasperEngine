// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextShaper.h"

#include "FontFace.h"
#include "StringUtils.h"

#include "hb.h"
#include "SheenBidi/SheenBidi.h"

using namespace jasperengine;

namespace {

    void shapeRun(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                  const std::vector<uint32_t>& codepoints, size_t offset, size_t length, bool rtl,
                  unsigned int face, uint32_t script, unsigned int line, const ShapeOptions& options,
                  std::vector<ShapedGlyph>& shaped){
        if (length == 0 || face >= faces.size() || face >= scales.size())
            return;

        float faceScale = scales[face];

        hb_buffer_t* buffer = hb_buffer_create();

        hb_buffer_add_utf32(buffer, codepoints.data(), (int)codepoints.size(), (unsigned int)offset, (int)length);
        //direction and script are already resolved for this run, only language is guessed
        hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, (hb_script_t)script);
        if (!options.language.empty()){
            hb_buffer_set_language(buffer, hb_language_from_string(options.language.c_str(), (int)options.language.size()));
        }
        hb_buffer_guess_segment_properties(buffer);

        hb_feature_t features[4];
        unsigned int featureCount = 0;
        if (!options.kerning){
            features[featureCount++] = {HB_TAG('k', 'e', 'r', 'n'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
        }
        if (options.letterSpacing != 0.0f){
            //spaced letters must stay separate glyphs (CSS Text 3, letter-spacing)
            features[featureCount++] = {HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
            features[featureCount++] = {HB_TAG('c', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
            features[featureCount++] = {HB_TAG('d', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
        }

        hb_shape(faces[face]->getHbFont(), buffer, featureCount ? features : NULL, featureCount);

        unsigned int count = 0;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);

        size_t first = shaped.size();

        for (unsigned int i = 0; i < count; i++){
            ShapedGlyph glyph;

            glyph.face = (uint16_t)face;
            glyph.rtl = rtl;
            glyph.glyphIndex = infos[i].codepoint;
            glyph.cluster = (size_t)infos[i].cluster;
            glyph.xOffset = positions[i].x_offset * faceScale;
            glyph.yOffset = positions[i].y_offset * faceScale;
            glyph.xAdvance = positions[i].x_advance * faceScale;
            glyph.line = line;

            shaped.push_back(glyph);
        }

        if (options.letterSpacing != 0.0f){
            //once per cluster, on its last glyph in visual order, so marks stay on their base
            for (size_t i = first; i < shaped.size(); i++){
                if (i + 1 == shaped.size() || shaped[i + 1].cluster != shaped[i].cluster){
                    shaped[i].xAdvance += options.letterSpacing;
                }
            }
        }

        hb_buffer_destroy(buffer);
    }

    void shapeRunItems(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                       const std::vector<uint32_t>& codepoints, size_t offset, size_t length, bool rtl,
                       unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& shaped){
        if (length == 0)
            return;

        //shaping must not cross a font or a script, a letter only joins inside its own
        struct Item {
            size_t offset;
            size_t length;
            unsigned int face;
            hb_script_t script;
        };

        hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();

        std::vector<Item> items;
        size_t start = offset;
        unsigned int face = TextShaper::selectFace(faces, codepoints[offset], 0);
        hb_script_t script = HB_SCRIPT_COMMON;

        for (size_t i = offset; i < offset + length; i++){
            unsigned int nextFace = TextShaper::selectFace(faces, codepoints[i], face);

            hb_script_t nextScript = script;
            hb_script_t charScript = hb_unicode_script(unicode, codepoints[i]);
            //punctuation, spaces and combining marks take the script around them
            if (charScript != HB_SCRIPT_COMMON && charScript != HB_SCRIPT_INHERITED && charScript != HB_SCRIPT_UNKNOWN){
                nextScript = charScript;
            }

            if (nextFace == face && (nextScript == script || script == HB_SCRIPT_COMMON)){
                //a run that started on neutrals adopts the first real script it meets
                script = nextScript;
                continue;
            }

            items.push_back({start, i - start, face, script});

            start = i;
            face = nextFace;
            script = nextScript;
        }
        items.push_back({start, offset + length - start, face, script});

        //glyphs are emitted left to right, so an RTL run lays its pieces out backwards
        for (size_t i = 0; i < items.size(); i++){
            const Item& item = rtl ? items[items.size() - 1 - i] : items[i];
            shapeRun(faces, scales, codepoints, item.offset, item.length, rtl, item.face, (uint32_t)item.script, line, options, shaped);
        }
    }

}

bool TextShaper::isDefaultIgnorable(uint32_t c){
    if (c < 0x00AD)
        return false;
    return c == 0x00AD || c == 0x034F || c == 0x061C || c == 0x115F || c == 0x1160 || c == 0x17B4 || c == 0x17B5 ||
           (c >= 0x180B && c <= 0x180F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x206F) || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || c == 0xFFA0 ||
           (c >= 0xFFF0 && c <= 0xFFF8) || (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A) ||
           (c >= 0xE0000 && c <= 0xE0FFF);
}

unsigned int TextShaper::selectFace(const std::vector<const FontFace*>& faces, uint32_t codepoint, unsigned int current){
    //nothing to fall back to, skip the coverage lookups
    if (faces.size() <= 1)
        return 0;

    //invisible and zero-width: no reason to leave the face around it (a run that starts
    //with one takes the primary face)
    if (isDefaultIgnorable(codepoint))
        return current < faces.size() ? current : 0;

    if (current < faces.size() && faces[current]->hasCodepoint(codepoint)){
        return current;
    }

    for (unsigned int i = 0; i < faces.size(); i++){
        if (faces[i]->hasCodepoint(codepoint)){
            return i;
        }
    }

    //uncovered, the primary font draws its .notdef box
    return 0;
}

void TextShaper::shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                            const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                            unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out){
    if (length == 0 || faces.empty() || offset + length > codepoints.size())
        return;

    SBCodepointSequence sequence;
    sequence.stringEncoding = SBStringEncodingUTF32;
    sequence.stringBuffer = (void*)codepoints.data();
    sequence.stringLength = (SBUInteger)codepoints.size();

    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (!algorithm){
        shapeRunItems(faces, scales, codepoints, offset, length, options.baseDirection == 2, line, options, out);
        return;
    }

    //auto: the base direction comes from the first strong character
    SBLevel baseLevel = SBLevelDefaultLTR;
    if (options.baseDirection == 1)
        baseLevel = 0;
    else if (options.baseDirection == 2)
        baseLevel = 1;

    SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, (SBUInteger)offset, (SBUInteger)length, baseLevel);
    SBLineRef bidiLine = paragraph ? SBParagraphCreateLine(paragraph, (SBUInteger)offset, SBParagraphGetLength(paragraph)) : NULL;

    if (bidiLine){
        //runs come back in visual order, left to right
        SBUInteger runCount = SBLineGetRunCount(bidiLine);
        const SBRun* runs = SBLineGetRunsPtr(bidiLine);

        for (SBUInteger i = 0; i < runCount; i++){
            shapeRunItems(faces, scales, codepoints, (size_t)runs[i].offset, (size_t)runs[i].length, (runs[i].level & 1) != 0, line, options, out);
        }

        SBLineRelease(bidiLine);
    }else{
        shapeRunItems(faces, scales, codepoints, offset, length, options.baseDirection == 2, line, options, out);
    }

    if (paragraph)
        SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
}

float TextShaper::measure(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                          const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                          const ShapeOptions& options){
    std::vector<ShapedGlyph> shaped;
    shapeRange(faces, scales, codepoints, offset, length, 0, options, shaped);

    float total = 0.0f;
    for (const ShapedGlyph& glyph : shaped){
        total += glyph.xAdvance;
    }

    return total;
}

void TextShaper::decodeUtf8(const char* text, size_t length, std::vector<uint32_t>& codepoints){
    codepoints.clear();
    if (!text || length == 0)
        return;

    bool hadInvalid = false;
    codepoints = StringUtils::decodeUtf8ToCodepoints(std::string(text, length), hadInvalid);
}
