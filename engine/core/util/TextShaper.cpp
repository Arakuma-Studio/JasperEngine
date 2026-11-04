// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextShaper.h"

#include "FontFace.h"
#include "StringUtils.h"

#include <algorithm>

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
        const bool vertical = options.direction == ShapeDirection::Vertical;
        hb_buffer_set_direction(buffer, vertical ? HB_DIRECTION_TTB : (rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR));
        hb_buffer_set_script(buffer, (hb_script_t)script);
        if (!options.language.empty()){
            hb_buffer_set_language(buffer, hb_language_from_string(options.language.c_str(), (int)options.language.size()));
        }
        hb_buffer_guess_segment_properties(buffer);

        //the usual case (no extra features) keeps to the stack
        hb_feature_t fixedFeatures[4];
        std::vector<hb_feature_t> extraFeatures;
        hb_feature_t* features = fixedFeatures;
        if (!options.features.empty()){
            extraFeatures.resize(4 + options.features.size());
            features = extraFeatures.data();
        }
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

        for (const ShapeFeature& feature : options.features){
            features[featureCount++] = {feature.tag, feature.value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
        }

        hb_font_t* font = (face < options.fonts.size() && options.fonts[face]) ? options.fonts[face] : faces[face]->getHbFont();
        hb_shape(font, buffer, featureCount ? features : NULL, featureCount);

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
            glyph.yAdvance = positions[i].y_advance * faceScale;
            glyph.line = line;

            shaped.push_back(glyph);
        }

        if (!options.justifyAdvance.empty()){
            //a justified line: each cluster takes the space of its codepoints (a ligature, the space of all
            //it covers), added after its last glyph in visual order; its glyphs move by the space before it
            std::vector<size_t> clusters;
            for (size_t i = first; i < shaped.size(); i++)
                clusters.push_back(shaped[i].cluster);
            std::sort(clusters.begin(), clusters.end());
            clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
            for (size_t i = first; i < shaped.size();){
                size_t groupEnd = i + 1;
                while (groupEnd < shaped.size() && shaped[groupEnd].cluster == shaped[i].cluster)
                    groupEnd++;
                const size_t clusterBegin = shaped[i].cluster;
                auto next = std::upper_bound(clusters.begin(), clusters.end(), clusterBegin);
                const size_t clusterEnd = (next != clusters.end()) ? *next : offset + length;
                float advance = 0.0f, shift = 0.0f;
                for (size_t c = clusterBegin; c < clusterEnd && c < options.justifyAdvance.size(); c++){
                    advance += options.justifyAdvance[c];
                    shift += (c < options.justifyOffset.size()) ? options.justifyOffset[c] : 0.0f;
                }
                if (advance != 0.0f){
                    for (size_t g = i; g < groupEnd; g++)
                        shaped[g].xOffset += shift;
                    shaped[groupEnd - 1].xAdvance += advance;
                }
                i = groupEnd;
            }
        }

        if (options.letterSpacing != 0.0f || options.wordSpacing != 0.0f){
            //once per cluster, on its last glyph in visual order, so marks stay on their base;
            //letter spacing skips the invisible default-ignorables (as browsers), word spacing goes to spaces
            for (size_t i = first; i < shaped.size(); i++){
                if (i + 1 == shaped.size() || shaped[i + 1].cluster != shaped[i].cluster){
                    const uint32_t codepoint = shaped[i].cluster < codepoints.size() ? codepoints[shaped[i].cluster] : 0;
                    //vertical text advances down: the spacing lengthens -yAdvance
                    float& advance = vertical ? shaped[i].yAdvance : shaped[i].xAdvance;
                    const float sign = vertical ? -1.0f : 1.0f;
                    if (!TextShaper::isDefaultIgnorable(codepoint))
                        advance += sign * options.letterSpacing;
                    if (codepoint == 0x0020 || codepoint == 0x00A0 || codepoint == 0x1361)
                        advance += sign * options.wordSpacing;
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

        //a cluster at a time (TextShaper::clusterEnd): it takes the first face that covers all of it, as
        //Chromium falls back per grapheme cluster, and its script is its first real one
        std::vector<Item> items;
        const size_t end = offset + length;
        size_t start = offset;
        unsigned int face = 0;
        hb_script_t script = HB_SCRIPT_COMMON;

        for (size_t i = offset; i < end;){
            //(most clusters are one codepoint: nothing below the combining marks extends one but CR LF and a ZWJ)
            const uint32_t first = codepoints[i];
            size_t clusterEnd = i + 1;
            if (clusterEnd < end && (codepoints[clusterEnd] >= 0x0300 || first == 0x200D || first == 0x000D))
                clusterEnd = TextShaper::clusterEnd(codepoints, i, end);
            const unsigned int nextFace = (clusterEnd == i + 1) ? TextShaper::selectFace(faces, first, face)
                                                                : TextShaper::selectClusterFace(faces, codepoints, i, clusterEnd, face);

            hb_script_t nextScript = script;
            for (size_t k = i; k < clusterEnd; k++){
                hb_script_t charScript = hb_unicode_script(unicode, codepoints[k]);
                //punctuation, spaces and combining marks take the script around them
                if (charScript != HB_SCRIPT_COMMON && charScript != HB_SCRIPT_INHERITED && charScript != HB_SCRIPT_UNKNOWN){
                    nextScript = charScript;
                    break;
                }
            }

            if (i == offset || (nextFace == face && (nextScript == script || script == HB_SCRIPT_COMMON))){
                //a run that started on neutrals adopts the first real script it meets
                face = nextFace;
                script = nextScript;
                i = clusterEnd;
                continue;
            }

            items.push_back({start, i - start, face, script});

            start = i;
            face = nextFace;
            script = nextScript;
            i = clusterEnd;
        }
        items.push_back({start, end - start, face, script});

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

    //the first face in the chain that has it, as Chromium: after a fallback, the primary again
    for (unsigned int i = 0; i < faces.size(); i++){
        if (faces[i]->hasCodepoint(codepoint)){
            return i;
        }
    }

    //uncovered, the primary font draws its .notdef box
    return 0;
}

namespace {

    bool isRegionalIndicator(uint32_t c){
        return c >= 0x1F1E6 && c <= 0x1F1FF;
    }

    //Hangul_Syllable_Type: L, V, T jamo, LV and LVT syllables
    enum class Hangul { None, L, V, T, LV, LVT };
    Hangul hangulType(uint32_t c){
        if ((c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C))
            return Hangul::L;
        if ((c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6))
            return Hangul::V;
        if ((c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB))
            return Hangul::T;
        if (c >= 0xAC00 && c <= 0xD7A3)
            return ((c - 0xAC00) % 28 == 0) ? Hangul::LV : Hangul::LVT;
        return Hangul::None;
    }

    //Grapheme_Cluster_Break Extend or SpacingMark (UAX #29 GB9, GB9a): marks, the joiners and variation
    //selectors, tags, emoji modifiers
    bool extendsCluster(hb_unicode_funcs_t* unicode, uint32_t c){
        if (c < 0x0300)
            return false;
        if (c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0020 && c <= 0xE007F) ||
            (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x1F3FB && c <= 0x1F3FF))
            return true;
        const hb_unicode_general_category_t category = hb_unicode_general_category(unicode, c);
        return category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
               category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
    }

}

size_t TextShaper::clusterEnd(const std::vector<uint32_t>& codepoints, size_t begin, size_t end){
    hb_unicode_funcs_t* unicode = nullptr;
    size_t j = begin + 1;
    for (; j < end; j++){
        const uint32_t previous = codepoints[j - 1], c = codepoints[j];
        //most text: nothing below the combining marks extends a cluster but a CR LF pair
        if (c < 0x0300 && previous != 0x200D){
            if (previous == 0x000D && c == 0x000A)
                continue;
            break;
        }
        //GB11: a ZWJ joins what follows (emoji sequences)
        if (previous == 0x200D)
            continue;
        if (!unicode)
            unicode = hb_unicode_funcs_get_default();
        if (extendsCluster(unicode, c))
            continue;
        //GB12, GB13: regional indicators pair
        if (isRegionalIndicator(c) && j == begin + 1 && isRegionalIndicator(previous))
            continue;
        //GB6-GB8: Hangul syllable sequences
        const Hangul a = hangulType(previous), b = hangulType(c);
        if ((a == Hangul::L && b != Hangul::None && b != Hangul::T) || ((a == Hangul::V || a == Hangul::LV) && (b == Hangul::V || b == Hangul::T)) ||
            ((a == Hangul::T || a == Hangul::LVT) && b == Hangul::T))
            continue;
        break;
    }
    return j;
}

unsigned int TextShaper::selectClusterFace(const std::vector<const FontFace*>& faces, const std::vector<uint32_t>& codepoints,
                                           size_t begin, size_t end, unsigned int current){
    if (end == begin + 1 || faces.size() <= 1)
        return selectFace(faces, codepoints[begin], current);

    //the first face that has every visible codepoint of the cluster, or the one codepoint they compose into
    size_t first = end;
    for (size_t k = begin; k < end; k++){
        if (!isDefaultIgnorable(codepoints[k])){
            first = k;
            break;
        }
    }
    if (first == end)
        return current < faces.size() ? current : 0;
    //a base and marks that compose into one codepoint (HarfBuzz composes them when the face has it)
    uint32_t composed = codepoints[first];
    for (size_t k = first + 1; k < end && composed; k++){
        if (isDefaultIgnorable(codepoints[k]))
            continue;
        hb_codepoint_t result = 0;
        composed = hb_unicode_compose(hb_unicode_funcs_get_default(), composed, codepoints[k], &result) ? result : 0;
    }
    for (unsigned int i = 0; i < faces.size(); i++){
        if (composed && faces[i]->hasCodepoint(composed))
            return i;
        bool all = true;
        for (size_t k = first; k < end && all; k++)
            all = isDefaultIgnorable(codepoints[k]) || faces[i]->hasCodepoint(codepoints[k]);
        if (all)
            return i;
    }
    //no face has all of it: the one its first visible codepoint takes
    return selectFace(faces, codepoints[first], current);
}

void TextShaper::shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                            const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                            unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out){
    if (length == 0 || faces.empty() || offset > codepoints.size() || length > codepoints.size() - offset)
        return;

    //top to bottom has no bidi reordering: one run in logical order, itemized by face and script
    if (options.direction == ShapeDirection::Vertical){
        shapeRunItems(faces, scales, codepoints, offset, length, false, line, options, out);
        return;
    }

    // Printable ASCII has a single LTR bidi run in an LTR (or auto/LTR-default)
    // paragraph. Keep the usual font/script itemization and HarfBuzz shaping, but
    // avoid allocating and resolving a bidi paragraph for common UI labels.
    // RTL paragraphs, controls and every non-ASCII character take the full path.
    if (options.baseDirection != 2 &&
        std::all_of(codepoints.begin() + offset, codepoints.begin() + offset + length,
                    [](uint32_t c){ return c >= 0x20 && c <= 0x7E; })){
        shapeRunItems(faces, scales, codepoints, offset, length, false, line, options, out);
        return;
    }

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
