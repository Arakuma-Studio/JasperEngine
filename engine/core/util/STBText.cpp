// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "STBText.h"

#include <cmath>
#include <string>
#include "Log.h"
#include "FontFace.h"
#include "FontRegistry.h"
#include "GlyphAtlas.h"
#include "StringUtils.h"

using namespace jasperengine;

STBText::STBText() {
    fontLoaded = false;

    atlasWidth = 0;
    atlasHeight = 0;
    atlasVersion = 0;

    ascent = 0;
    descent = 0;
    lineGap = 0;
    lineHeight = 0;

    textureData = NULL;
}

STBText::~STBText() {
    faces.clear();
    facePointers.clear();

    delete textureData;
}

uint64_t STBText::glyphKey(unsigned int face, uint32_t glyphIndex){
    return ((uint64_t)face << 32) | (uint64_t)glyphIndex;
}

bool STBText::addFace(const std::string& fontpath, unsigned int fontSize){
    std::string error;
    std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(fontpath, 0, &error);
    if (!face){
        Log::error("%s", error.c_str());
        return false;
    }

    addFace(face, fontSize);

    return true;
}

void STBText::addFace(const std::shared_ptr<FontFace>& face, unsigned int fontSize){
    if (!face)
        return;

    faces.push_back(face);
    facePointers.push_back(face.get());
    faceScales.push_back(face->scaleForPixelHeight((float)fontSize * options.rasterScale));
}

void STBText::addBuiltInFaces(unsigned int fontSize){
    //Roboto first, the metrics and the .notdef box come from it
    addFace(FontRegistry::getBuiltInLatin(), fontSize);
    addFace(FontRegistry::getBuiltInArabic(), fontSize);
}

void STBText::shapeRange(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, unsigned int line, std::vector<ShapedGlyph>& shaped){
    TextShaper::shapeRange(facePointers, faceScales, codepoints, offset, length, line, ShapeOptions(), shaped);
}

void STBText::measureAdvances(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, std::vector<float>& advances){
    std::vector<ShapedGlyph> shaped;
    shapeRange(codepoints, offset, length, 0, shaped);

    //widths do not depend on visual order, charge each advance to its codepoint
    for (const ShapedGlyph& glyph : shaped){
        if (glyph.cluster < advances.size()){
            advances[glyph.cluster] += glyph.xAdvance;
        }
    }
}

float STBText::shapeLineInto(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, unsigned int line, std::vector<ShapedGlyph>& lineGlyphs){
    lineGlyphs.clear();
    shapeRange(codepoints, offset, length, line, lineGlyphs);

    float total = 0.0f;
    for (const ShapedGlyph& glyph : lineGlyphs){
        total += glyph.xAdvance;
    }

    return total;
}

unsigned int STBText::shapeLines(const std::vector<uint32_t>& codepoints, unsigned int width, bool fixedWidth, bool multiline, std::vector<ShapedGlyph>& shaped){
    unsigned int line = 0;

    std::vector<float> advances;
    if (multiline && fixedWidth){
        advances.assign(codepoints.size(), 0.0f);
    }

    std::vector<ShapedGlyph> lineGlyphs;

    size_t start = 0;
    while (start <= codepoints.size()){
        size_t end = start;
        while (end < codepoints.size() && codepoints[end] != 10){
            end++;
        }

        if (multiline && fixedWidth && end > start){
            //a first guess at where the line ends
            measureAdvances(codepoints, start, end - start, advances);

            size_t lineStart = start;
            while (lineStart < end){
                size_t breakAt = end;
                size_t lastSpace = end;
                float estimate = 0.0f;

                for (size_t i = lineStart; i < end; i++){
                    if (codepoints[i] == 32){
                        lastSpace = i;
                    }

                    estimate += advances[i];

                    if (estimate > (float)width && i > lineStart){
                        breakAt = (lastSpace > lineStart && lastSpace < i) ? lastSpace : i;
                        break;
                    }
                }

                //the guess is not binding, a letter that was medial inside the paragraph
                //becomes a wider final form at the break, so give ground until it fits
                while (true){
                    if (shapeLineInto(codepoints, lineStart, breakAt - lineStart, line, lineGlyphs) <= (float)width)
                        break;
                    if (breakAt <= lineStart + 1)
                        break; //a single cluster wider than the budget, nothing to gain

                    size_t retreat = breakAt - 1;
                    for (size_t j = breakAt - 1; j > lineStart; j--){
                        if (codepoints[j] == 32){
                            retreat = j;
                            break;
                        }
                    }
                    breakAt = retreat;
                }

                shaped.insert(shaped.end(), lineGlyphs.begin(), lineGlyphs.end());
                line++;

                //a break on a space swallows it, like a newline would
                lineStart = (breakAt < end && codepoints[breakAt] == 32) ? breakAt + 1 : breakAt;
            }

        }else{
            shapeRange(codepoints, start, end - start, line, shaped);
            line++;
        }

        if (end >= codepoints.size())
            break;

        start = end + 1;
    }

    return line;
}

void STBText::resetAtlas(unsigned int width, unsigned int height){
    if (atlas){
        retiredAtlases.push_back(std::move(atlas));
    }

    // an SDF atlas keeps its padding empty between glyphs too: a shadow samples up to that far
    // outside its own glyph and must find nothing there
    atlas.reset(new GlyphAtlas(width, height, options.sdfPadding > 0 ? options.sdfPadding : atlasPadding));
    atlasWidth = width;
    atlasHeight = height;

    atlasVersion++;
}

bool STBText::growAtlas(){
    if (atlasWidth * 2 > atlasLimit || atlasHeight * 2 > atlasLimit){
        return false;
    }

    std::vector<uint64_t> cached;
    cached.reserve(glyphMap.size());
    for (const auto& [key, _] : glyphMap){
        cached.push_back(key);
    }

    resetAtlas(atlasWidth * 2, atlasHeight * 2);
    glyphMap.clear();

    for (uint64_t key : cached){
        getGlyph((unsigned int)(key >> 32), (uint32_t)key);
    }

    return true;
}

void STBText::rasterizeGlyph(unsigned int face, uint32_t glyphIndex, FontGlyph& glyph, int x, int y){
    float scale = faceScales[face];

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&faces[face]->getInfo(), glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    int gw = x1 - x0;
    int gh = y1 - y0;

    std::vector<unsigned char> bitmap;
    int bw, bh, bx, by;
    if (faces[face]->rasterizeGlyph(glyphIndex, scale, bitmap, bw, bh, bx, by)){
        atlas->blit(bitmap.data(), bw, bh, x, y);
    }

    glyph.xoff = x0;
    glyph.yoff = y0;
    glyph.xoff2 = x1;
    glyph.yoff2 = y1;

    glyph.s0 = (float)x / atlasWidth;
    glyph.t0 = (float)y / atlasHeight;
    glyph.s1 = (float)(x + gw) / atlasWidth;
    glyph.t1 = (float)(y + gh) / atlasHeight;

    glyph.xadvance = faces[face]->getGlyphAdvance(glyphIndex, scale);

    atlasVersion++;
}

const STBText::FontGlyph* STBText::getGlyph(unsigned int face, uint32_t glyphIndex){
    if (!fontLoaded || face >= faces.size())
        return NULL;

    uint64_t key = glyphKey(face, glyphIndex);

    auto it = glyphMap.find(key);
    if (it != glyphMap.end()){
        return &it->second;
    }

    if (options.sdfPadding > 0){
        return getSdfGlyph(face, glyphIndex, key);
    }

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&faces[face]->getInfo(), glyphIndex, faceScales[face], faceScales[face], &x0, &y0, &x1, &y1);

    int x, y;
    if (!atlas->pack(x1 - x0, y1 - y0, x, y)){
        //growAtlas() repacks the cached glyphs, this one is still missing
        if (!growAtlas() || !atlas->pack(x1 - x0, y1 - y0, x, y)){
            Log::error("Failed to pack glyph in font atlas");
            return NULL;
        }
    }

    FontGlyph& glyph = glyphMap[key];
    rasterizeGlyph(face, glyphIndex, glyph, x, y);

    return &glyph;
}

// a signed distance field of the glyph with sdfPadding pixels around it (128 on the edge, a
// pixel away 128 / sdfPadding less outside, more inside); the quad covers the padding too
const STBText::FontGlyph* STBText::getSdfGlyph(unsigned int face, uint32_t glyphIndex, uint64_t key){
    const float scale = faceScales[face];
    const int padding = options.sdfPadding;

    int width = 0, height = 0, xoff = 0, yoff = 0;
    unsigned char* sdf = stbtt_GetGlyphSDF(&faces[face]->getInfo(), scale, (int)glyphIndex, padding, 128, 128.0f / (float)padding,
                                           &width, &height, &xoff, &yoff);
    if (!sdf){
        // no ink (a space): an empty quad that still advances
        width = 0;
        height = 0;
    }

    int x = 0, y = 0;
    if (!atlas->pack(width, height, x, y)){
        // growAtlas() repacks the cached glyphs, this one is still missing
        if (!growAtlas() || !atlas->pack(width, height, x, y)){
            if (sdf)
                stbtt_FreeSDF(sdf, nullptr);
            Log::error("Failed to pack glyph in font atlas");
            return NULL;
        }
    }

    if (sdf){
        atlas->blit(sdf, width, height, x, y);
        stbtt_FreeSDF(sdf, nullptr);
    }

    FontGlyph& glyph = glyphMap[key];
    glyph.xoff = (float)xoff;
    glyph.yoff = (float)yoff;
    glyph.xoff2 = (float)(xoff + width);
    glyph.yoff2 = (float)(yoff + height);

    glyph.s0 = (float)x / atlasWidth;
    glyph.t0 = (float)y / atlasHeight;
    glyph.s1 = (float)(x + width) / atlasWidth;
    glyph.t1 = (float)(y + height) / atlasHeight;

    glyph.xadvance = faces[face]->getGlyphAdvance(glyphIndex, scale);

    atlasVersion++;

    return &glyph;
}

const STBText::FontGlyph* STBText::getGlyphForCodepoint(uint32_t codepoint){
    if (!fontLoaded)
        return NULL;

    auto it = codepointMap.find(codepoint);
    if (it == codepointMap.end()){
        unsigned int face = TextShaper::selectFace(facePointers, codepoint, 0);
        //not covered by any face resolves to glyph 0, the primary .notdef box
        uint32_t glyphIndex = faces[face]->getGlyphIndex(codepoint);

        it = codepointMap.emplace(codepoint, glyphKey(face, glyphIndex)).first;
    }

    return getGlyph((unsigned int)(it->second >> 32), (uint32_t)(it->second & 0xFFFFFFFF));
}

void STBText::refreshTextureData(){
    unsigned int textureSize = atlasWidth * atlasHeight * sizeof(unsigned char);

    delete textureData;
    textureData = new TextureData(atlasWidth, atlasHeight, textureSize, ColorFormat::RED, 1, (void*)atlas->getPixels().data());
}

// the metrics are kept in raster pixels and reported in text pixels
float STBText::getAscent(){
    return (options.rasterScale == 1.0f) ? ascent : ascent / options.rasterScale;
}

float STBText::getDescent(){
    return (options.rasterScale == 1.0f) ? descent : descent / options.rasterScale;
}

float STBText::getLineGap(){
    return (options.rasterScale == 1.0f) ? lineGap : lineGap / options.rasterScale;
}

int STBText::getLineHeight(){
    return (options.rasterScale == 1.0f) ? lineHeight : (int)std::lround((float)lineHeight / options.rasterScale);
}

float STBText::getCharWidth(uint32_t codepoint){
    const FontGlyph* glyph = getGlyphForCodepoint(codepoint);
    if (!glyph)
        return 0;

    return (options.rasterScale == 1.0f) ? glyph->xadvance : glyph->xadvance / options.rasterScale;
}

void STBText::setOptions(const STBTextOptions& options){
    this->options = options;
    if (!std::isfinite(this->options.rasterScale) || this->options.rasterScale <= 0.0f)
        this->options.rasterScale = 1.0f;
    if (this->options.sdfPadding < 0)
        this->options.sdfPadding = 0;
}

const STBTextOptions& STBText::getOptions() const{
    return options;
}

float STBText::getSdfDistanceScale() const{
    // stb: value = 128 + distance * (128 / padding)
    return (options.sdfPadding > 0) ? 255.0f * (float)options.sdfPadding / 128.0f : 0.0f;
}

unsigned long STBText::getAtlasVersion() const{
    return atlasVersion;
}

TextureData* STBText::load(const std::string& fontpath, const std::vector<std::string>& fallbackPaths, unsigned int fontSize){

    fontLoaded = false;
    faces.clear();
    facePointers.clear();
    faceScales.clear();

    if (!fontpath.empty() && !addFace(fontpath, fontSize)){
        faces.clear();
        facePointers.clear();
        faceScales.clear();
        return NULL;
    }

    for (const std::string& fallbackPath : fallbackPaths){
        //a missing fallback is not fatal, the rest of the chain still covers the text
        addFace(fallbackPath, fontSize);
    }

    //the built-in fonts close the chain, a font made for one script still renders
    //what it does not carry
    addBuiltInFaces(fontSize);

    if (faces.empty()){
        return NULL;
    }

    //line metrics come from the primary font, fallbacks follow its baseline
    int ascent = faces[0]->getAscender();
    int descent = faces[0]->getDescender();
    int lineGap = faces[0]->getLineGap();

    float scale = faceScales[0];

    this->ascent = ascent * scale;
    this->descent = descent * scale;
    this->lineGap = lineGap * scale;
    this->lineHeight = (ascent - descent + lineGap) * scale;

    glyphMap.clear();
    codepointMap.clear();

    resetAtlas(512, 512);

    fontLoaded = true;

    refreshTextureData();

    return textureData;
}

void STBText::createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                         std::vector<CharExtent>& charExtents,
                         unsigned int& width, unsigned int& height, bool fixedWidth, bool fixedHeight, bool multiline, bool invert,
                         std::vector<TextGlyphInfo>* glyphInfo){

    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, hadInvalid);
    if (hadInvalid) {
        Log::warn("Invalid character");
    }

    unsigned long startVersion = atlasVersion;

    // the layout runs in raster pixels; everything it outputs goes back to text pixels at the end
    const float rasterScale = options.rasterScale;
    const bool scaled = rasterScale != 1.0f;
    const unsigned int layoutWidth = (scaled && fixedWidth) ? (unsigned int)std::lround((float)width * rasterScale) : width;
    const int firstVertex = (int)buffer->getCount();

    if (glyphInfo)
        glyphInfo->clear();

    std::vector<ShapedGlyph> shaped;
    int lineCount = (int)shapeLines(codepoints, layoutWidth, fixedWidth, multiline, shaped);

    //rasterize every glyph first, a grow in the middle of the layout would repack
    //the ones already written in the buffer
    for (const ShapedGlyph& glyph : shaped){
        getGlyph(glyph.face, glyph.glyphIndex);
    }

    float offsetX = 0;
    float offsetY = 0;

    Attribute* atrVertice = buffer->getAttribute(AttributeType::POSITION);
    Attribute* atrTexcoord = buffer->getAttribute(AttributeType::TEXCOORD1);

    int minX0 = 0, maxX1 = 0, minY0 = 0, maxY1 = 0;
    int ind = 0;
    unsigned int currentLine = 0;

    //trailing pen position of each codepoint, so charPositions can stay indexed by
    //codepoint even when a cluster produced several glyphs or none
    std::vector<Vector2> clusterEnd(codepoints.size(), Vector2(0, 0));
    std::vector<bool> clusterSet(codepoints.size(), false);

    //visual span of each cluster, which is what a caret and a selection need
    std::vector<CharExtent> clusterExtent(codepoints.size());

    for (size_t s = 0; s < shaped.size(); s++){

        const ShapedGlyph& shapedGlyph = shaped[s];

        if (shapedGlyph.line != currentLine){
            currentLine = shapedGlyph.line;
            offsetY = currentLine * lineHeight;
            offsetX = 0;
        }

        const FontGlyph* glyph = getGlyph(shapedGlyph.face, shapedGlyph.glyphIndex);
        if (!glyph)
            continue;

        //aligned to integer, like stbtt_GetPackedQuad does
        float quadX = std::floor(offsetX + shapedGlyph.xOffset + glyph->xoff + 0.5f);
        float quadY = std::floor(offsetY - shapedGlyph.yOffset + glyph->yoff + 0.5f);

        stbtt_aligned_quad quad;
        quad.x0 = quadX;
        quad.y0 = quadY;
        quad.x1 = quadX + (glyph->xoff2 - glyph->xoff);
        quad.y1 = quadY + (glyph->yoff2 - glyph->yoff);
        quad.s0 = glyph->s0;
        quad.t0 = glyph->t0;
        quad.s1 = glyph->s1;
        quad.t1 = glyph->t1;

        float glyphStart = offsetX;
        offsetX += shapedGlyph.xAdvance;

        if (shapedGlyph.cluster < clusterEnd.size()){
            size_t cluster = shapedGlyph.cluster;

            clusterEnd[cluster] = Vector2(offsetX, offsetY);

            if (!clusterSet[cluster]){
                clusterExtent[cluster].left = glyphStart;
                clusterExtent[cluster].right = offsetX;
            }else{
                //several glyphs of the same cluster, take the whole span they cover
                clusterExtent[cluster].left = std::min(clusterExtent[cluster].left, glyphStart);
                clusterExtent[cluster].right = std::max(clusterExtent[cluster].right, offsetX);
            }
            clusterExtent[cluster].y = offsetY;
            clusterExtent[cluster].rtl = shapedGlyph.rtl;

            clusterSet[cluster] = true;
        }

        if (invert) {
            float auxt0 = quad.t0;
            quad.t0 = quad.t1;
            quad.t1 = auxt0;

            float auxy0 = quad.y0;
            quad.y0 = -quad.y1;
            quad.y1 = -auxy0;
        }
            
        // the ink box measures the text: an SDF quad's padding (outline, shadow) does not, as TMP
        const float inkPadding = (float)options.sdfPadding;
        if (quad.x0 + inkPadding < minX0)
            minX0 = quad.x0 + inkPadding;
        if (quad.y0 + inkPadding < minY0)
            minY0 = quad.y0 + inkPadding;
        if (quad.x1 - inkPadding > maxX1)
            maxX1 = quad.x1 - inkPadding;
        if (quad.y1 - inkPadding > maxY1)
            maxY1 = quad.y1 - inkPadding;
        if (offsetX > maxX1)
            maxX1 = offsetX;
            
        //every glyph is added, UISystem clips the quads to the layout rect
        buffer->addVector3(atrVertice, Vector3(quad.x0, quad.y0, 0));
        buffer->addVector3(atrVertice, Vector3(quad.x1, quad.y0, 0));
        buffer->addVector3(atrVertice, Vector3(quad.x1, quad.y1, 0));
        buffer->addVector3(atrVertice, Vector3(quad.x0, quad.y1, 0));

        buffer->addVector2(atrTexcoord, Vector2(quad.s0, quad.t0));
        buffer->addVector2(atrTexcoord, Vector2(quad.s1, quad.t0));
        buffer->addVector2(atrTexcoord, Vector2(quad.s1, quad.t1));
        buffer->addVector2(atrTexcoord, Vector2(quad.s0, quad.t1));

        indices.push_back(ind);
        indices.push_back(ind+1);
        indices.push_back(ind+2);
        indices.push_back(ind);
        indices.push_back(ind+2);
        indices.push_back(ind+3);
        ind = ind + 4;

        if (glyphInfo){
            TextGlyphInfo info;
            info.cluster = shapedGlyph.cluster;
            info.line = shapedGlyph.line;
            glyphInfo->push_back(info);
        }
    }

    //a cluster covers every codepoint up to the next one, and they share its glyphs,
    //so they split its width. Inside an RTL run the first owns the rightmost slice
    Vector2 lastPosition = Vector2(0, 0);
    for (size_t i = 0; i < codepoints.size(); i++){
        if (!clusterSet[i]){
            clusterEnd[i] = lastPosition;
            continue;
        }

        size_t covered = 1;
        while (i + covered < codepoints.size() && !clusterSet[i + covered] && codepoints[i + covered] != 10){
            covered++;
        }

        CharExtent cluster = clusterExtent[i];
        float slice = (cluster.right - cluster.left) / (float)covered;
        lastPosition = clusterEnd[i];

        for (size_t j = 0; j < covered; j++){
            CharExtent& extent = clusterExtent[i + j];

            extent = cluster;
            if (cluster.rtl){
                extent.right = cluster.right - slice * j;
                extent.left = extent.right - slice;
            }else{
                extent.left = cluster.left + slice * j;
                extent.right = extent.left + slice;
            }

            clusterEnd[i + j] = lastPosition;
        }

        i += covered - 1;
    }

    //both arrays stay indexed by codepoint, in logical order
    charPositions.clear();
    charExtents.clear();

    for (size_t i = 0; i < codepoints.size(); i++){
        if (codepoints[i] == 10) //\n
            continue;

        charPositions.push_back(clusterEnd[i]);
        charExtents.push_back(clusterExtent[i]);
    }

    //Empty text
    if (codepoints.size() == 0){
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));

        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        
        indices.push_back(0);
        indices.push_back(1);
        indices.push_back(2);
    }
    if (scaled){
        const float inverse = 1.0f / rasterScale;

        Attribute* positions = buffer->getAttribute(AttributeType::POSITION);
        for (int i = firstVertex; i < (int)buffer->getCount(); i++){
            Vector3 vertex = buffer->getVector3(positions, i);
            buffer->setVector3(i, positions, Vector3(vertex.x * inverse, vertex.y * inverse, vertex.z));
        }
        for (Vector2& position : charPositions){
            position = position * inverse;
        }
        for (CharExtent& extent : charExtents){
            extent.left *= inverse;
            extent.right *= inverse;
            extent.y *= inverse;
        }

        if (!fixedWidth)
            width = (unsigned int)std::ceil((float)(maxX1 - minX0) * inverse);
        if (!fixedHeight)
            height = (unsigned int)std::lround((float)(lineCount * lineHeight) * inverse);
    }else{
        if (!fixedWidth)
            width = maxX1 - minX0;
        if (!fixedHeight)
            height = lineCount * lineHeight;
    }

    if (atlasVersion != startVersion){
        refreshTextureData();
    }
}

TextureData* STBText::getTextureData(){
    return textureData;
}
