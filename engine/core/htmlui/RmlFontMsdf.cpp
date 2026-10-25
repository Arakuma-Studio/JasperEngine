// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontMsdf.h"

#include "util/FontFace.h"

#include "../../libs/msdfgen/msdfgen.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

bool RmlFontMsdf::generate(const FontFace& face, uint32_t glyphIndex, float boldEm, RmlDistanceGlyph& out){
    out = RmlDistanceGlyph();
    const stbtt_fontinfo& info = face.getInfo();
    stbtt_vertex* vertices = nullptr;
    const int count = stbtt_GetGlyphShape(&info, (int)glyphIndex, &vertices);
    if (count <= 0 || !vertices){
        if (vertices)
            stbtt_FreeShape(&info, vertices);
        return false;
    }

    // the outline in font units (y up): stb_truetype's moves, lines, quadratic and cubic curves
    msdfgen::Shape shape;
    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 pen;
    for (int i = 0; i < count; i++){
        const stbtt_vertex& v = vertices[i];
        const msdfgen::Point2 point(v.x, v.y);
        switch (v.type){
        case STBTT_vmove:
            contour = &shape.addContour();
            break;
        case STBTT_vline:
            if (contour && point != pen)
                contour->addEdge(msdfgen::EdgeHolder(pen, point));
            break;
        case STBTT_vcurve:
            if (contour)
                contour->addEdge(msdfgen::EdgeHolder(pen, msdfgen::Point2(v.cx, v.cy), point));
            break;
        case STBTT_vcubic:
            if (contour)
                contour->addEdge(msdfgen::EdgeHolder(pen, msdfgen::Point2(v.cx, v.cy), msdfgen::Point2(v.cx1, v.cy1), point));
            break;
        default:
            break;
        }
        pen = point;
    }
    stbtt_FreeShape(&info, vertices);

    shape.contours.erase(std::remove_if(shape.contours.begin(), shape.contours.end(),
                                        [](const msdfgen::Contour& c){ return c.edges.empty(); }),
                         shape.contours.end());
    if (shape.contours.empty())
        return false;
    shape.normalize();
    // TrueType and CFF outlines wind either way: what msdfgen calls inside is made consistent
    shape.orientContours();
    msdfgen::edgeColoringSimple(shape, 3.0);

    const int unitsPerEm = std::max(face.getUnitsPerEm(), 1);
    const double scale = (double)emTexels / (double)unitsPerEm;          // texels per font unit
    const double rangeUnits = (double)distanceRange / scale;             // distance each side, font units
    const double boldUnits = std::max((double)boldEm, 0.0) * (double)unitsPerEm;
    const msdfgen::Shape::Bounds bounds = shape.getBounds();
    if (!(bounds.r > bounds.l) || !(bounds.t > bounds.b))
        return false;
    // the outline, its dilation and the distance range around it, and a texel to spare
    const double pad = (double)distanceRange + boldUnits * scale + 1.0;
    const int width = (int)std::ceil((bounds.r - bounds.l) * scale + 2.0 * pad);
    const int height = (int)std::ceil((bounds.t - bounds.b) * scale + 2.0 * pad);
    const msdfgen::Vector2 translate(-bounds.l + pad / scale, -bounds.b + pad / scale);

    msdfgen::Bitmap<float, 4> field(width, height);
    // 0.5 where the outline dilated by the bold is, the range each side of it
    const msdfgen::DistanceMapping mapping(msdfgen::Range(-rangeUnits - boldUnits, rangeUnits - boldUnits));
    const msdfgen::SDFTransformation transformation(msdfgen::Projection(msdfgen::Vector2(scale), translate), mapping);
    msdfgen::generateMTSDF(field, shape, transformation, msdfgen::MSDFGeneratorConfig(true));

    // bytes, rows top first (msdfgen's bitmap is bottom first)
    out.width = width;
    out.height = height;
    out.rgba.resize((size_t)width * (size_t)height * 4);
    for (int y = 0; y < height; y++){
        const int row = height - 1 - y;
        for (int x = 0; x < width; x++){
            const float* texel = field(x, row);
            uint8_t* dst = &out.rgba[((size_t)y * (size_t)width + (size_t)x) * 4];
            for (int c = 0; c < 4; c++)
                dst[c] = (uint8_t)std::clamp((int)std::lround(texel[c] * 255.0f), 0, 255);
        }
    }
    out.left = (float)-translate.x;
    out.bottom = (float)-translate.y;
    out.right = out.left + (float)((double)width / scale);
    out.top = out.bottom + (float)((double)height / scale);
    return true;
}
