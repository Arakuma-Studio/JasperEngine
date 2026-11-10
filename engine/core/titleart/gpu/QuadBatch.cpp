// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/common.ts (drawInstancedQuads, QUAD_VERT) by SafEight,
// used with the author's permission.

#include "QuadBatch.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr int DATA_WIDTH = 1024;     // texels a row of the data texture

    TitleLutData packFloats(const std::vector<float>& floats) {
        TitleLutData data;
        data.format = ColorFormat::RGBA32F;
        data.filter = TextureFilter::NEAREST;
        const size_t texels = std::max<size_t>(1, floats.size() / 4);
        data.width = DATA_WIDTH;
        data.height = (int)((texels + DATA_WIDTH - 1) / DATA_WIDTH);
        data.bytes.assign((size_t)data.width * (size_t)data.height * 16, 0);
        if (!floats.empty()) std::memcpy(data.bytes.data(), floats.data(), floats.size() * sizeof(float));
        return data;
    }

}

BlendMode gen::quadBlendMode(QuadBlend blend) {
    switch (blend) {
        case QuadBlend::Normal: return BlendMode::PREMULTIPLIED;
        case QuadBlend::Add: return BlendMode::LIGHT_PREMULTIPLIED;
        case QuadBlend::Screen: return BlendMode::SCREEN_PREMULTIPLIED;
        case QuadBlend::Max: return BlendMode::MAX_PREMULTIPLIED;
    }
    return BlendMode::PREMULTIPLIED;
}

void QuadBatch::add(const QuadInstance& q) {
    if (size() >= (size_t)QUAD_BATCH_MAX) return;
    // texel 0: centre and size; 1: rotation and `more`; 2: colour; 3: extra (titleart/gen/tfx_quads.glsl)
    const float r[16] = {q.cx, q.cy, q.w, q.h, q.rotation, q.more[0], q.more[1], q.more[2], q.color[0], q.color[1], q.color[2], q.color[3],
                         q.extra[0], q.extra[1], q.extra[2], q.extra[3]};
    records.insert(records.end(), r, r + 16);
}

TitleLutData QuadBatch::data() const {
    return packFloats(records);
}

bool QuadBatch::draw(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& target, const TitleAffine& spaceToTarget,
                     QuadBlend blend, float minPx, const std::function<void(TitlePass&)>& setup) const {
    if (records.empty()) return false;
    const TitleLut* lut = ctx.runner.lut(data());
    if (!lut) return false;
    TitlePass pass = genPass(ctx, program, label, target, spaceToTarget);
    pass.params[5][2] = minPx;
    pass.vertexData.lut = lut;
    pass.vertices = (int)size() * 6;
    pass.blend = quadBlendMode(blend);
    if (setup) setup(pass);
    return ctx.runner.run(pass);
}

bool gen::drawMesh(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& target, const TitleAffine& spaceToTarget,
                   const std::vector<MeshVertex>& vertices, QuadBlend blend, const std::function<void(TitlePass&)>& setup) {
    const size_t count = vertices.size() - vertices.size() % 3;
    if (count == 0) return false;
    // two texels a vertex: (x, y, u, v), extra
    std::vector<float> floats;
    floats.reserve(count * 8);
    for (size_t i = 0; i < count; i++) {
        const MeshVertex& v = vertices[i];
        const float r[8] = {v.x, v.y, v.u, v.v, v.extra[0], v.extra[1], v.extra[2], v.extra[3]};
        floats.insert(floats.end(), r, r + 8);
    }
    const TitleLut* lut = ctx.runner.lut(packFloats(floats));
    if (!lut) return false;
    TitlePass pass = genPass(ctx, program, label, target, spaceToTarget);
    pass.vertexData.lut = lut;
    pass.vertices = (int)count;
    pass.blend = quadBlendMode(blend);
    if (setup) setup(pass);
    return ctx.runner.run(pass);
}
