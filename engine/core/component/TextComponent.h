// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXT_COMPONENT_H
#define TEXT_COMPONENT_H

#include "asset/AssetRef.h"
#include "Engine.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "util/CharExtent.h"
#include "util/TextGlyphInfo.h"
#include "math/Vector4.h"

#include <array>

namespace jasperengine{

    class STBText;
    namespace pixelart { class BitmapText; }

    //like a CSS font-family list: the main font first, then the fallbacks tried in order
    //for what it misses. Empty slots are skipped, the built-in fonts close the chain
    typedef std::array<AssetRef, MAX_TEXT_FONTS> FontArray;

    // TextMesh Pro's alignment (S94 F24): which point of the text's box sits on its origin, and how
    // each line sits in the box. The box is the fixed width/height when set, else the content.
    // The two enums take disjoint values, so a value of one given to the other is refused (Lua's
    // TextAlignment table holds both).
    enum class TextHorizontalAlignment{
        LEFT = 0,
        CENTER = 1,
        RIGHT = 2
    };

    // along the text's local +y: Bottom (the default) puts the box above the origin in a y-up
    // world (below it on a y-down UI camera); Baseline the first line's baseline; Capline and
    // Midline the middle of its capitals and of its lowercase letters
    enum class TextVerticalAlignment{
        TOP = 3,
        MIDDLE = 4,
        BOTTOM = 5,
        BASELINE = 6,
        CAPLINE = 7,
        MIDLINE = 8
    };

    // what fontSize measures (S94 F26): ascender to descender (stb_truetype's pixel height), or the
    // em square as CSS and TextMesh Pro do
    enum class TextFontSizeUnit{
        PIXEL_HEIGHT = 0,
        EM = 1
    };

    struct JASPERENGINE_API TextComponent{
        bool loaded = false;

        FontArray font;
        std::string text = "";
        unsigned int fontSize = 20;
        bool multiline = true;
        unsigned int maxTextSize = 100;

        std::vector<Vector2> charPositions;
        //visual span of each codepoint, for a bidi aware caret and selection
        std::vector<CharExtent> charExtents;

        bool fixedWidth = false;
        bool fixedHeight = false;

        TextHorizontalAlignment horizontalAlignment = TextHorizontalAlignment::LEFT;
        TextVerticalAlignment verticalAlignment = TextVerticalAlignment::BOTTOM;
        TextFontSizeUnit fontSizeUnit = TextFontSizeUnit::PIXEL_HEIGHT;

        // Outline (TMP "Outline"): UI units drawn outside the glyph edge, and its colour
        // (linear, as UIComponent::color; Text's setters take sRGB)
        float outlineWidth = 0.0f;
        Vector4 outlineColor = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        // a bitmap font's pixel outline grows through 4 or 8 neighbours (S51 R10); its outline
        // width and shadow offset are whole font pixels
        int outlineNeighbors = 8;
        // Shadow (TMP "Underlay"): colour (linear), offset in UI units (y down: right-down),
        // softness (blur radius) and dilate (the shape grows), UI units
        bool shadowEnabled = false;
        Vector4 shadowColor = Vector4(0.0f, 0.0f, 0.0f, 0.5f);
        Vector2 shadowOffset = Vector2(2.0f, 2.0f);
        float shadowSoftness = 0.0f;
        float shadowDilate = 0.0f;
        // TMP maxVisibleCharacters: characters past it are not drawn (-1 all)
        int visibleCharacters = -1;

        // runtime: each glyph quad's character (cluster) and line, the atlas options it was
        // built with, and whether its buffer carries a vertex colour (a Text Effect)
        std::vector<TextGlyphInfo> glyphInfo;
        float rasterScale = 1.0f;
        int sdfPadding = 0;
        bool vertexColor = false;
        // bumped by every rebuild of the glyphs (a Text Effect takes them as its base again)
        unsigned long buildCount = 0;
        // runtime: the box's lowest corner in local units after the alignment (picking, gizmos)
        Vector2 boxMin = Vector2(0.0f, 0.0f);

        std::shared_ptr<STBText> stbtext = NULL;
        // a bitmap font (S51), set instead of stbtext: its layout, its pixel scale k (0 for a
        // vector font, R7), the units one font pixel takes, and the face colour its vertices carry
        std::shared_ptr<pixelart::BitmapText> bitmapText;
        int pixelScale = 0;
        float pixelUnit = 0.0f;
        Vector3 bitmapFaceColor = Vector3(1.0f, 1.0f, 1.0f);
        bool bitmapSizeNoted = false;

        //atlas state this text was built with, see STBText
        unsigned long atlasVersion = 0;

        bool needReloadAtlas = false;
        bool needUpdateText = true;
    };

}

#endif //TEXT_COMPONENT_H