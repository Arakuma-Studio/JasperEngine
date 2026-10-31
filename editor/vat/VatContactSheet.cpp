// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatContactSheet.h"

#include "vat/VatBakeCompute.h"

#include "render/CpuSkin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace jasperengine::editor::vat{

namespace{

    // the view: from the front-right (yaw) and above (pitch), looking at the object
    constexpr float VIEW_YAW = 0.6108652f;     // 35 degrees
    constexpr float VIEW_PITCH = 0.3490659f;   // 20 degrees
    constexpr float MARGIN = 0.08f;
    constexpr int SUPERSAMPLE = 2;
    const PoseColor BACKGROUND = PoseColor(41, 41, 43);
    const PoseColor DEFAULT_COLOR = PoseColor(200, 200, 205);

    struct ViewBasis{
        Vector3 right, up, forward;     // forward: from the viewer into the scene
    };

    ViewBasis viewBasis(){
        // the viewer sits at +Z turned by yaw about Y towards +X, raised by pitch
        const Vector3 toViewer(std::sin(VIEW_YAW) * std::cos(VIEW_PITCH), std::sin(VIEW_PITCH), std::cos(VIEW_YAW) * std::cos(VIEW_PITCH));
        ViewBasis b;
        b.forward = -toViewer;
        b.right = Vector3(0, 1, 0).crossProduct(toViewer).normalized();
        b.up = toViewer.crossProduct(b.right).normalized();
        return b;
    }

    PoseColor colorOf(const VatMaterialData* material){
        if (!material) return DEFAULT_COLOR;
        // linear factors shown as sRGB-ish (a gamma), never black so shapes stay visible
        auto channel = [](float linear){
            const float c = std::pow(std::min(std::max(linear, 0.0f), 1.0f), 1.0f / 2.2f);
            return 0.15f + 0.85f * c;
        };
        return PoseColor::fromFloat(channel(material->baseColor[0]), channel(material->baseColor[1]), channel(material->baseColor[2]));
    }

    void readTexel(const VatPayload& payload, uint64_t texel, float out[4]){
        if (payload.precision == VatPrecision::Float){
            std::memcpy(out, payload.bytes.data() + texel * 16, 16);
        }else{
            uint16_t half[4];
            std::memcpy(half, payload.bytes.data() + texel * 8, 8);
            for (int k = 0; k < 4; k++) out[k] = halfToFloat(half[k]);
        }
    }

    std::string fixed2(float value){
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", value);
        return text;
    }

}

std::vector<float> vatSheetTimes(float length, ClipWrapMode wrap){
    std::vector<float> times;
    const bool loop = wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::Default;
    for (int k = 0; k < VAT_SHEET_TIMES; k++){
        times.push_back(length * (float)k / (float)(loop ? VAT_SHEET_TIMES : VAT_SHEET_TIMES - 1));
    }
    return times;
}

bool vatMeshAt(const VatData& data, const VatPayload& payload, const VatMeshData& mesh, int clip, float time, VatSheetMesh& out,
               std::string* error){
    out = VatSheetMesh();
    if (clip < 0 || clip >= (int)data.clips.size()){
        if (error) *error = "no clip " + std::to_string(clip);
        return false;
    }
    const VatClip& vatClip = data.clips[(size_t)clip];
    const VatFrameSample sample = vatSample(vatClip, time);
    const uint64_t texels = (uint64_t)payload.width * (uint64_t)payload.height;
    const uint64_t texelBytes = payload.precision == VatPrecision::Float ? 16 : 8;
    if (payload.bytes.size() < texels * texelBytes){
        if (error) *error = "the payload is smaller than its size";
        return false;
    }
    const size_t vertices = mesh.vertexCount();
    out.positions.resize(vertices);
    const float a = sample.alpha;
    for (size_t v = 0; v < vertices; v++){
        const Vector3 rest(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]);
        if (data.mode == VatMode::Vertices){
            const int element = v < mesh.vatIndex.size() ? (int)mesh.vatIndex[v] : (int)v;
            if (element < 0 || element >= data.layout.vertices){
                if (error) *error = "vertex " + std::to_string(v) + " has no payload row";
                return false;
            }
            const uint64_t t0 = vatTexelIndex(data.mode, data.layout, sample.frame0, element, 0);
            const uint64_t t1 = vatTexelIndex(data.mode, data.layout, sample.frame1, element, 0);
            if (t0 >= texels || t1 >= texels){
                if (error) *error = "frame past the payload";
                return false;
            }
            float o0[4], o1[4];
            readTexel(payload, t0, o0);
            readTexel(payload, t1, o1);
            const Vector3 offset(o0[0] + (o1[0] - o0[0]) * a, o0[1] + (o1[1] - o0[1]) * a, o0[2] + (o1[2] - o0[2]) * a);
            out.positions[v] = vatClip.restNone ? offset : rest + offset;
        }else{
            Vector3 sum(0, 0, 0);
            float total = 0.0f;
            for (int k = 0; k < 4; k++){
                const size_t slot = v * 4 + (size_t)k;
                const float w = slot < mesh.weights.size() ? mesh.weights[slot] : (k == 0 ? 1.0f : 0.0f);
                if (w <= 0.0f) continue;
                const int bone = slot < mesh.joints.size() ? (int)mesh.joints[slot] : 0;
                if (bone >= data.layout.bones){
                    if (error) *error = "vertex " + std::to_string(v) + " uses bone " + std::to_string(bone) + " past the payload's";
                    return false;
                }
                float q0[4], q1[4], p0[4], p1[4];
                const uint64_t r0 = vatTexelIndex(data.mode, data.layout, sample.frame0, bone, 0);
                const uint64_t r1 = vatTexelIndex(data.mode, data.layout, sample.frame1, bone, 0);
                if (r0 + 1 >= texels || r1 + 1 >= texels){
                    if (error) *error = "frame past the payload";
                    return false;
                }
                readTexel(payload, r0, q0);
                readTexel(payload, r0 + 1, p0);
                readTexel(payload, r1, q1);
                readTexel(payload, r1 + 1, p1);
                float q[4], tr[3];
                float length = 0.0f;
                for (int c = 0; c < 4; c++){
                    q[c] = q0[c] + (q1[c] - q0[c]) * a;
                    length += q[c] * q[c];
                }
                length = std::sqrt(length);
                if (length > 0.0f){
                    for (float& c : q) c /= length;
                }else{
                    q[0] = q[1] = q[2] = 0.0f;
                    q[3] = 1.0f;
                }
                for (int c = 0; c < 3; c++) tr[c] = p0[c] + (p1[c] - p0[c]) * a;
                const float scale = p0[3] + (p1[3] - p0[3]) * a;
                sum = sum + applyBone(q, tr, scale, rest) * w;
                total += w;
            }
            out.positions[v] = total > 0.0f ? sum / total : rest;
        }
    }
    for (const VatSubmesh& submesh : mesh.submeshes){
        const VatMaterialData* material = submesh.material >= 0 && (size_t)submesh.material < mesh.materials.size()
                                              ? &mesh.materials[(size_t)submesh.material] : nullptr;
        const PoseColor color = colorOf(material);
        for (size_t i = 0; i + 2 < submesh.indices.size(); i += 3){
            if (submesh.indices[i] >= vertices || submesh.indices[i + 1] >= vertices || submesh.indices[i + 2] >= vertices) continue;
            out.indices.insert(out.indices.end(), {submesh.indices[i], submesh.indices[i + 1], submesh.indices[i + 2]});
            out.colors.push_back(color);
        }
    }
    return true;
}

bool sourceMeshAt(VatPoser& poser, const VatSourceClip& clip, float time, VatSheetMesh& out, std::string* error){
    out = VatSheetMesh();
    VatSampleState state;
    std::string why;
    if (!poser.capture(clip, time, state, why)){
        if (error) *error = why;
        return false;
    }
    for (size_t r = 0; r < poser.renderers.size() && r < state.renderers.size(); r++){
        const VatBakeRenderer& renderer = poser.renderers[r];
        const VatRendererState& now = state.renderers[r];
        cpuskin::CpuSkinResult result;
        if (!cpuskin::cpuDeform(renderer.source, now.skin, now.morph.data(), &now.lattice, result, &why)){
            if (error) *error = renderer.name + ": " + why;
            return false;
        }
        const uint32_t base = (uint32_t)out.positions.size();
        for (const Vector3& p : result.positions) out.positions.push_back(now.toRoot * p);
        for (size_t s = 0; s < renderer.source.submeshes.size(); s++){
            const cpuskin::CpuSkinSubmesh& submesh = renderer.source.submeshes[s];
            if (submesh.primitive != PrimitiveType::TRIANGLES) continue;
            const int materialIndex = s < renderer.submeshMaterial.size() ? renderer.submeshMaterial[s] : -1;
            const VatMaterialData* material = materialIndex >= 0 && (size_t)materialIndex < poser.materials.size()
                                                  ? &poser.materials[(size_t)materialIndex] : nullptr;
            const PoseColor color = colorOf(material);
            for (size_t i = 0; i + 2 < submesh.indices.size(); i += 3){
                out.indices.insert(out.indices.end(), {base + submesh.indices[i], base + submesh.indices[i + 1], base + submesh.indices[i + 2]});
                out.colors.push_back(color);
            }
        }
    }
    return true;
}

PoseImage renderVatCell(const VatSheetMesh& mesh, const Vector3& min, const Vector3& max, int width, int height){
    PoseImage image(width, height, BACKGROUND);
    if (width <= 0 || height <= 0) return image;
    const int w = width * SUPERSAMPLE, h = height * SUPERSAMPLE;
    const ViewBasis view = viewBasis();
    const Vector3 centre = (min + max) * 0.5f;
    // the framed box's extent along the screen axes (its 8 corners)
    float halfX = 0.0f, halfY = 0.0f;
    for (int c = 0; c < 8; c++){
        const Vector3 corner((c & 1) ? max.x : min.x, (c & 2) ? max.y : min.y, (c & 4) ? max.z : min.z);
        const Vector3 d = corner - centre;
        halfX = std::max(halfX, std::fabs(d.dotProduct(view.right)));
        halfY = std::max(halfY, std::fabs(d.dotProduct(view.up)));
    }
    halfX = std::max(halfX, 1e-4f);
    halfY = std::max(halfY, 1e-4f);
    const float scale = (1.0f - MARGIN) * std::min((float)w * 0.5f / halfX, (float)h * 0.5f / halfY);
    // key light from the viewer's upper left
    const Vector3 light = (view.up * 0.6f - view.right * 0.5f - view.forward * 0.8f).normalized();

    std::vector<float> depth((size_t)w * (size_t)h, std::numeric_limits<float>::infinity());
    std::vector<PoseColor> color((size_t)w * (size_t)h, BACKGROUND);
    struct Projected{ float x, y, z; };
    std::vector<Projected> projected(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); i++){
        const Vector3 d = mesh.positions[i] - centre;
        projected[i] = {(float)w * 0.5f + d.dotProduct(view.right) * scale, (float)h * 0.5f - d.dotProduct(view.up) * scale, d.dotProduct(view.forward)};
    }
    for (size_t t = 0; t * 3 + 2 < mesh.indices.size(); t++){
        const uint32_t ia = mesh.indices[t * 3], ib = mesh.indices[t * 3 + 1], ic = mesh.indices[t * 3 + 2];
        if (ia >= projected.size() || ib >= projected.size() || ic >= projected.size()) continue;
        const Vector3 n = (mesh.positions[ib] - mesh.positions[ia]).crossProduct(mesh.positions[ic] - mesh.positions[ia]);
        const float nl = n.length();
        if (!(nl > 0.0f)) continue;
        const float lambert = std::fabs((n / nl).dotProduct(light));
        const float shade = 0.3f + 0.7f * lambert;
        const PoseColor base = t < mesh.colors.size() ? mesh.colors[t] : DEFAULT_COLOR;
        const PoseColor shaded((uint8_t)std::lround(base.r * shade), (uint8_t)std::lround(base.g * shade), (uint8_t)std::lround(base.b * shade));
        const Projected& a = projected[ia];
        const Projected& b = projected[ib];
        const Projected& c = projected[ic];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-12f || !std::isfinite(area)) continue;
        const int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
        const int x1 = std::min(w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        const int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
        const int y1 = std::min(h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        for (int y = y0; y <= y1; y++){
            const float py = (float)y + 0.5f;
            for (int x = x0; x <= x1; x++){
                const float px = (float)x + 0.5f;
                const float wa = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                const float wb = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                const float wc = 1.0f - wa - wb;
                if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
                const float z = wa * a.z + wb * b.z + wc * c.z;
                const size_t at = (size_t)y * (size_t)w + (size_t)x;
                if (z < depth[at]){
                    depth[at] = z;
                    color[at] = shaded;
                }
            }
        }
    }
    // box filter down to the cell
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            int r = 0, g = 0, b = 0;
            for (int sy = 0; sy < SUPERSAMPLE; sy++){
                for (int sx = 0; sx < SUPERSAMPLE; sx++){
                    const PoseColor& c = color[(size_t)(y * SUPERSAMPLE + sy) * (size_t)w + (size_t)(x * SUPERSAMPLE + sx)];
                    r += c.r;
                    g += c.g;
                    b += c.b;
                }
            }
            const int n = SUPERSAMPLE * SUPERSAMPLE;
            image.setPixel(x, y, PoseColor((uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n)));
        }
    }
    return image;
}

PoseImage composeVatSheet(const std::vector<VatSheetRow>& rows, const std::vector<std::vector<float>>& times, const std::string& title,
                          int cellSize){
    PoseSheetLayout layout;
    layout.columns = VAT_SHEET_TIMES;
    layout.rows = std::max(1, (int)rows.size());
    layout.cellWidth = cellSize;
    layout.cellHeight = cellSize;
    layout.titleHeight = title.empty() ? 0 : PoseSheetLayout::LABEL_HEIGHT;
    PoseContactSheet sheet(layout, title);
    // one framing per group: the union of every mesh of its rows
    std::vector<std::pair<Vector3, Vector3>> frames;
    std::vector<bool> framed;
    for (const VatSheetRow& row : rows){
        const size_t group = (size_t)std::max(row.frameGroup, 0);
        if (frames.size() <= group){
            frames.resize(group + 1, {Vector3(0, 0, 0), Vector3(0, 0, 0)});
            framed.resize(group + 1, false);
        }
        for (const VatSheetMesh& mesh : row.meshes){
            for (const Vector3& p : mesh.positions){
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
                if (!framed[group]){
                    frames[group] = {p, p};
                    framed[group] = true;
                }
                Vector3& lo = frames[group].first;
                Vector3& hi = frames[group].second;
                lo = Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
        }
    }
    for (size_t r = 0; r < rows.size(); r++){
        const VatSheetRow& row = rows[r];
        const size_t group = (size_t)std::max(row.frameGroup, 0);
        for (int c = 0; c < VAT_SHEET_TIMES && c < (int)row.meshes.size(); c++){
            sheet.setCell(c, (int)r, renderVatCell(row.meshes[(size_t)c], frames[group].first, frames[group].second, cellSize, cellSize));
            const float t = group < times.size() && (size_t)c < times[group].size() ? times[group][(size_t)c] : 0.0f;
            sheet.setLabel(c, (int)r, row.label + " t=" + fixed2(t));
        }
    }
    return sheet.getImage();
}

}
