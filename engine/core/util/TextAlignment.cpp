// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "TextAlignment.h"

#include <algorithm>
#include <limits>

using namespace jasperengine;

namespace {

    struct AlignedLine {
        float left = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float rightWithSpaces = std::numeric_limits<float>::lowest();
        uint32_t lastCluster = 0;
        bool any = false;
        // the distinct pen starts of its spaces before its last other character
        std::vector<float> gaps;
    };

}

std::vector<float> TextAlignment::horizontalOffsets(const std::vector<TextGlyphInfo>& glyphs, const std::vector<uint32_t>& codepoints,
                                                    float boxWidth, TextHorizontalAlignment alignment){
    std::vector<float> offsets(glyphs.size(), 0.0f);
    if (alignment == TextHorizontalAlignment::LEFT || glyphs.empty())
        return offsets;

    uint32_t lineCount = 0;
    for (const TextGlyphInfo& glyph : glyphs)
        lineCount = std::max(lineCount, glyph.line + 1);
    std::vector<AlignedLine> lines(lineCount);
    for (const TextGlyphInfo& glyph : glyphs){
        AlignedLine& line = lines[glyph.line];
        line.any = true;
        line.left = std::min(line.left, glyph.penLeft);
        line.rightWithSpaces = std::max(line.rightWithSpaces, glyph.penRight);
        if (!glyph.space)
            line.right = std::max(line.right, glyph.penRight);
        line.lastCluster = std::max(line.lastCluster, glyph.cluster);
    }
    for (AlignedLine& line : lines){
        // a line of spaces only spans them
        if (line.any && line.right < line.left)
            line.right = line.rightWithSpaces;
    }

    if (alignment == TextHorizontalAlignment::JUSTIFIED){
        for (const TextGlyphInfo& glyph : glyphs){
            AlignedLine& line = lines[glyph.line];
            if (glyph.space && glyph.penRight <= line.right &&
                    std::find(line.gaps.begin(), line.gaps.end(), glyph.penLeft) == line.gaps.end())
                line.gaps.push_back(glyph.penLeft);
        }
    }

    for (size_t i = 0; i < glyphs.size(); i++){
        const TextGlyphInfo& glyph = glyphs[i];
        const AlignedLine& line = lines[glyph.line];
        const float width = line.right - line.left;
        switch (alignment){
            case TextHorizontalAlignment::CENTER:
                offsets[i] = (boxWidth - width) * 0.5f - line.left;
                break;
            case TextHorizontalAlignment::RIGHT:
                offsets[i] = boxWidth - line.right;
                break;
            case TextHorizontalAlignment::JUSTIFIED:{
                const size_t next = (size_t)line.lastCluster + 1;
                const bool paragraphEnd = next >= codepoints.size() || codepoints[next] == 10;
                const float extra = boxWidth - width;
                if (paragraphEnd || line.gaps.empty() || extra <= 0.0f)
                    break;
                size_t before = 0;
                for (float gap : line.gaps){
                    if (gap < glyph.penLeft)
                        before++;
                }
                offsets[i] = extra / (float)line.gaps.size() * (float)before;
                break;
            }
            case TextHorizontalAlignment::LEFT:
                break;
        }
    }
    return offsets;
}

float TextAlignment::verticalOffset(unsigned int lineCount, float lineHeight, float ascent, float boxHeight, TextVerticalAlignment alignment){
    const float block = (float)lineCount * lineHeight;
    switch (alignment){
        case TextVerticalAlignment::MIDDLE: return (boxHeight - block) * 0.5f;
        case TextVerticalAlignment::BOTTOM: return boxHeight - block;
        case TextVerticalAlignment::BASELINE: return boxHeight * 0.5f - ascent;
        case TextVerticalAlignment::TOP: break;
    }
    return 0.0f;
}

unsigned int TextAlignment::fitSize(unsigned int minSize, unsigned int maxSize, const std::function<bool(unsigned int)>& fits){
    minSize = std::max(minSize, 1u);
    maxSize = std::max(maxSize, minSize);
    unsigned int low = minSize;
    unsigned int high = maxSize;
    unsigned int best = minSize;
    while (low <= high){
        const unsigned int mid = low + (high - low) / 2;
        if (fits(mid)){
            best = mid;
            low = mid + 1;
        }else{
            if (mid == 0)
                break;
            high = mid - 1;
        }
    }
    return best;
}
