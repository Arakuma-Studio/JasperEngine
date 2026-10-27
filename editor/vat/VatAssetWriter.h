// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2.1-2.3)
// - Purpose: turn an in-memory bake/import result into the bytes of "<stem>.vat" (canonical JSON,
//   vat::writeVat) and "<stem>.vat.glb" (render mesh + payload + socket tracks), shared by the
//   baker and the importers. Pure: it writes no file (callers write through WriteFilesCmd /
//   CreateVatCmd so undo works).
// - The glb: one node (identity), one mesh with one primitive per submesh, attributes POSITION,
//   NORMAL, TANGENT (when given), TEXCOORD_0/1 (when given), COLOR_0 (when given),
//   JOINTS_0/WEIGHTS_0 (Bones mode), _VAT_INDEX (float); 32-bit indices; materials from
//   VatMaterialData (PBR metallic-roughness, textures by URI relative to the glb); the
//   JASPER_vertex_animation extension (+ extensionsUsed) with payload and socket buffer views;
//   no skin, no animations, no morph targets. Bounds of POSITION = the given rest positions.

#ifndef VAT_ASSET_WRITER_H
#define VAT_ASSET_WRITER_H

#include "animation/VertexAnimation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    struct VatMaterialData{
        std::string name;
        float baseColor[4] = {1, 1, 1, 1};
        float metallic = 0.0f;
        float roughness = 1.0f;
        float emissive[3] = {0, 0, 0};
        std::string alphaMode = "OPAQUE";   // "OPAQUE" | "MASK" | "BLEND"
        float alphaCutoff = 0.5f;
        bool doubleSided = false;
        // absolute file paths ("" = none); written as URIs relative to the .vat.glb directory
        std::string baseColorTexture;
        std::string normalTexture;
        std::string metallicRoughnessTexture;
        std::string occlusionTexture;
        std::string emissiveTexture;
    };

    struct VatSubmesh{
        std::vector<uint32_t> indices;  // triangles
        int material = -1;              // index into VatMeshData::materials, -1 = default material
    };

    struct VatMeshData{
        // per vertex; positions are the rest pose (Vertices: the offsets' reference; Bones: bind pose)
        std::vector<float> positions;   // 3 per vertex
        std::vector<float> normals;     // 3 per vertex or empty
        std::vector<float> tangents;    // 4 per vertex (w = sign) or empty
        std::vector<float> uv0;         // 2 per vertex or empty
        std::vector<float> uv1;         // 2 per vertex or empty
        std::vector<float> colors;      // 4 per vertex or empty
        std::vector<uint16_t> joints;   // 4 per vertex (Bones mode) or empty
        std::vector<float> weights;     // 4 per vertex (Bones mode) or empty
        std::vector<float> vatIndex;    // 1 per vertex (always)
        std::vector<VatSubmesh> submeshes;
        std::vector<VatMaterialData> materials;

        size_t vertexCount() const { return positions.size() / 3; }
    };

    struct VatEncodedFiles{
        std::string vatPath;            // as given
        std::string meshPath;           // "<stem>.vat.glb" next to it
        std::string vatText;            // canonical JSON
        std::vector<uint8_t> glbBytes;
    };

    // Fills data.mesh (the glb file name), data.layout.layoutHash (computeLayoutHash),
    // data.stats.bytes, then encodes both files. False with `error` when the inputs disagree
    // (payload size vs layout, vertex attribute sizes, vatIndex range, joints in Vertices mode...).
    bool encodeVatFiles(const std::string& vatPath, jasperengine::vat::VatData& data, const VatMeshData& mesh,
                        const jasperengine::vat::VatPayload& payload, const jasperengine::vat::VatSocketTracks& sockets,
                        VatEncodedFiles& out, std::string* error);

    // "<dir>/<stem>.vat" -> "<dir>/<stem>.vat.glb"
    std::string meshPathFor(const std::string& vatPath);

}

#endif //VAT_ASSET_WRITER_H
