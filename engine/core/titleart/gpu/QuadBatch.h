// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R11, D6)
// - Purpose: one draw of up to 65,536 oriented quads (Pachislop's drawInstancedQuads, from
//   src/generators/common.ts): per instance a centre, a full size along its own axes, a rotation,
//   a premultiplied linear colour and a free vec4. The records are uploaded as an RGBA32F data
//   texture (4 texels each, read with texelFetch in the vertex stage: titleart/gen/tfx_quads.glsl)
//   and drawn as 6 vertices per quad pulled by gl_VertexIndex, so no instanced vertex buffers,
//   no compute and no storage buffers are needed on any backend. A program's fragment body gets
//   `v_quad` (-1..1 along the quad's axes), `v_color` (already scaled for sub-min_px quads),
//   `v_extra`, `v_more` and `v_sizePx`.
// - Sub-pixel energy: a quad below min_px (default 1.5 target px) on a side is drawn at min_px
//   with its colour scaled by the area ratio, so tiny particles keep their light and the look
//   holds across px_scale.
// - Meshes: drawMesh draws triangles whose vertices are records of their own (a position in the
//   drawing space, two attributes and an extra vec4: Brush Swirl's ribbons).
// - Records are uploaded once per content (TitlePassRunner::lut caches by hash), so a live
//   frame that only changes uniforms re-uploads nothing.
// - Thread-safety: main thread inside a render call.

#pragma once

#include "Export.h"
#include "titleart/generators/TitleGen.h"

#include <cstdint>
#include <vector>

namespace jasperengine::titleart::gen {

    constexpr int QUAD_BATCH_MAX = 65536;

    // how quads combine with what the target holds (premultiplied; alpha unions in every mode)
    enum class QuadBlend : uint8_t {
        Normal,     // over
        Add,        // colour sum, alpha union (the compositor's Add): LIGHT_PREMULTIPLIED
        Screen,     // s + d - s d
        Max         // MAX_PREMULTIPLIED (lightning cores, R21)
    };
    JASPERENGINE_API BlendMode quadBlendMode(QuadBlend blend);

    struct QuadInstance {
        float cx = 0, cy = 0;           // centre, drawing-space px
        float w = 0, h = 0;             // full size along its own axes, drawing-space px
        float rotation = 0;             // its +x axis, radians counter-clockwise (+y up)
        float color[4] = {0, 0, 0, 0};  // premultiplied linear
        float extra[4] = {0, 0, 0, 0};  // the fragment body's own
        float more[3] = {0, 0, 0};      // the program's own: its vertex stage's (tfx_quadMotion), then v_more
    };

    struct MeshVertex {
        float x = 0, y = 0;             // drawing-space px
        float u = 0, v = 0;             // the body's v_quad
        float extra[4] = {0, 0, 0, 0};  // v_extra
    };

    class JASPERENGINE_API QuadBatch {
    public:
        // drops records past QUAD_BATCH_MAX
        void add(const QuadInstance& q);
        void clear() { records.clear(); }
        size_t size() const { return records.size() / 16; }
        bool empty() const { return records.empty(); }

        // the records as an RGBA32F data texture: 1024 texels a row, 4 per quad
        TitleLutData data() const;

        // one draw of every quad with `program` (a QuadsXxx GenProgram) into `target`, in the
        // drawing space `spaceToTarget` maps from; `setup` fills the program's own block values
        // (p[6..]) and textures before it runs. False when nothing could be drawn
        bool draw(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& target, const TitleAffine& spaceToTarget,
                  QuadBlend blend, float minPx = 1.5f, const std::function<void(TitlePass&)>& setup = {}) const;

    private:
        std::vector<float> records;
    };

    // triangles (3 vertices each) drawn with a MeshXxx program, as QuadBatch::draw
    JASPERENGINE_API bool drawMesh(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& target,
                                   const TitleAffine& spaceToTarget, const std::vector<MeshVertex>& vertices, QuadBlend blend,
                                   const std::function<void(TitlePass&)>& setup = {});

}
