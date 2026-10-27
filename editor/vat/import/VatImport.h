// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.6, 4.6.1, 4.7)
// - Purpose: import SideFX Labs (Houdini) VAT 3 exports (soft-body, rigid-body, dynamic remeshing
//   / fluid; particle sprites are refused) and OpenVAT (Blender) exports into the engine's Vertex
//   Animation layout (spec 2.2/2.3), in memory: VatData + VatMeshData + VatPayload, ready for
//   encodeVatFiles. Callers (the baker window, the MCP tool, the bake-vat CLI) write the files
//   through their commands (WriteFilesCmd / CreateVatCmd), so undo works; encodeVatImport() is
//   the convenience that returns the bytes.
// - detectVatImport(path): cheap look at what a path is. Accepts a Houdini ROP export folder (or
//   its geo/, tex/, unity/, data/ subfolder), the mesh (<asset>_mesh.fbx/.glb/.gltf), a data
//   texture (<asset>_pos.exr ...), the Unity material (<asset>_mat.mat), the legacy
//   <asset>_data.json, an OpenVAT folder, its <name>-remap_info.json, its <name>_vat.* texture or
//   its mesh. Reports the kind, the Houdini mode, the files resolved, the metadata found and every
//   missing piece with its fix. Reads the metadata files; reads the mesh only when the mode can't
//   be told from the metadata and the texture names.
// - importVat(spec): decodes everything on the CPU (double precision where the formats need it)
//   and re-packs. Never writes files. `errors` non-empty = nothing usable (ok() false); warnings
//   are kept in the report. Error and warning kinds (spec 4.7 plus the importers' own):
//     errors: not_found (nothing importable at the path), missing_file (a texture/mesh/metadata
//       file the export needs), unsupported (particle sprites, an unknown OpenVAT layout...),
//       read_failed (a file that doesn't parse), metadata_mismatch (bounds flags, texture sizes,
//       UV rows or frame counts that disagree: the numbers are in the message), unknown_target
//       (the export's target engine can't be told: pass axis), fps_unknown (pass fps),
//       bad_mesh (no triangles, no VAT UVs, a node transform that isn't a similarity),
//       too_large (the payload is over the preset's texels: fixes ordered by effect), bone_limit
//       (mode "bones" with more rigid pieces than vat::VAT_MAX_BONES),
//       vertex_limit, invalid_settings (an option value that isn't valid)
//     warnings: stretch_by_velocity_ignored, fluid_uv_from_color_ignored, color_animation_ignored
//       (rigid Bones: per-piece colours that change over time keep their first frame),
//       topology_changes (fluid: absolute positions per corner, no interpolation), bone_limit
//       (Auto with rigid pieces over vat::VAT_MAX_BONES (65535): imported as Vertices),
//       large_interpolation_error
//       (fast spins needed more upsampling than the preset allows), precision_switched (half ->
//       float), memory_high, animations_list_form (OpenVAT `animations` as a list),
//       clip_out_of_range (an OpenVAT clip past the texture's frames: clamped),
//       embedded_textures_dropped (textures inside a .glb/.fbx are not copied),
//       node_transform_baked (the mesh node's transform was applied to the vertices)
// - Houdini decoding (spec 4.6.1, checked against SideFX's Unity shader graphs and the tutorial
//   exports): metadata from the .mat (m_Floats, keywords, shader GUID -> mode), else the data JSON,
//   else the mesh's raw bounds (the guard vertices sit at the bound corners; mirrored into the
//   target's axes first); bound-digit flags in double precision with snapping; UV1 (UV0 for fluid)
//   addresses texel centres, rows from the top, frame f at ((1 - v) + f/N) * Vr * H; guard
//   vertices (V <= 0.1) and their triangles are dropped. Unity-target data is mirrored back with
//   S = diag(-1, 1, 1) (positions, quaternions (x, -y, -z, w), tangent reference (1, 0, 0)), pivots
//   and compressed normals and smooth-trajectory accelerations are stored unmirrored by the
//   exporter; Unreal-target data (Z-up, cm) goes back through the axis swap and / scale.
//   Rigid pieces become bones (one per piece, weight 1, uint16 joints: Bones-mode bones index the
//   payload texture, not the 128-matrix skinning array) up to VAT_MAX_BONES (65535), else Vertices;
//   frames are upsampled (Houdini's precomputed-angle slerp, smooth trajectories) when a piece
//   turns more than 60 degrees between frames or when the GPU's lerp/nlerp between samples would
//   leave a vertex further than the tolerance from Houdini's motion.
// - OpenVAT decoding (spec 4.6; from the format description and its MIT engine decoders, none of
//   the GPL add-on's code): VAT_UV = the last TEXCOORD (glTF, top-left origin) or the UV set named
//   VAT_UV (FBX, bottom-left origin) addresses frame 0's texel; frame f is f rows lower; values
//   are offsets remapped per channel from os-remap Min/Max; normals packed in the lower half of the
//   texture (2n - 1), in <name>_vnrm.*, or absent; Blender Z-up -> Y-up (x, z, -y) for the offsets
//   and normals of glTF meshes (Blender's glTF export is Y-up); FBX meshes keep Blender's own
//   object space for the vertices and the offsets alike (the axis change is the FBX node's
//   transform, baked like any node transform); animations (dict, or a list with a warning) from
//   1-based scene frames to clips; none: one clip of every frame (fps from the options).
// - Thread-safety: pure functions of the files; safe on a worker thread.
// - Paths: UTF-8. Recipe paths are relative to spec.projectRoot when the file is inside it.

#ifndef VAT_IMPORT_H
#define VAT_IMPORT_H

#include "animation/VertexAnimation.h"
#include "vat/VatAssetWriter.h"
#include "vat/VatReport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    // the largest Houdini rigid upsampling factor (spec.upsample; 0 = automatic)
    constexpr int VAT_IMPORT_MAX_UPSAMPLE = 1024;


    // An error or a warning: the baker's issue (VatReport.h)
    using VatImportIssue = VatIssue;

    enum class VatImportKind{
        None,
        HoudiniVat3,    // recipe kind "houdini_vat3"
        OpenVat         // recipe kind "openvat"
    };
    const char* importKindName(VatImportKind kind);    // "", "houdini_vat3", "openvat"

    enum class HoudiniMode{ Unknown, Soft, Rigid, Fluid, Sprite };
    const char* houdiniModeName(HoudiniMode mode);      // "", "soft", "rigid", "fluid", "sprite"

    // Absolute paths of what an export is made of ("" = not found / not part of this kind)
    struct VatImportFiles{
        std::string mesh;
        // Houdini
        std::string position;           // <asset>_pos
        std::string position2;          // <asset>_pos2 (Two Position Textures)
        std::string rotation;           // <asset>_rot
        std::string color;              // <asset>_col
        std::string lookup;             // <asset>_lookup (fluid)
        std::string material;           // unity/<asset>_mat.mat
        std::string dataJson;           // data/<asset>_data.json
        // OpenVAT
        std::string vat;                // <name>_vat.(exr|png)
        std::string normals;            // <name>_vnrm.(exr|png)
        std::string remapInfo;          // <name>-remap_info.json
    };

    struct VatImportDetection{
        VatImportKind kind = VatImportKind::None;
        HoudiniMode mode = HoudiniMode::Unknown;
        std::string modeSource;         // "material shader", "material keywords", "data json", "textures and UV sets"
        std::string assetName;          // "Cloth_Simulation"; OpenVAT: the <name>
        std::string folder;             // the export's root folder
        VatImportFiles files;
        // what the metadata says (0 / "" when unknown)
        std::string metadataSource;     // "material" | "data json" | "mesh bounds" | "remap_info"
        int frames = 0;
        float fps = 0.0f;
        std::string target;             // Houdini: "unity" | "unreal" | "houdini" | "zup" | ""
        std::vector<std::string> clips; // OpenVAT animation names
        // missing pieces (kind missing_file / unknown_target / fps_unknown / unsupported...), each
        // with a fix; errors among them stop an import
        std::vector<VatImportIssue> issues;
        std::string summary;            // one line for the window's Source row and the MCP preview

        bool found() const { return kind != VatImportKind::None; }
    };

    VatImportDetection detectVatImport(const std::string& path);

    struct VatImportSpec{
        std::string path;               // anything detectVatImport accepts
        std::string projectRoot;        // recipe paths are made relative to it (empty: kept as given)
        std::string clipName;           // Houdini: the clip's name (empty: the asset name)
        float fps = 0.0f;               // > 0 overrides the metadata's (required when it has none)
        std::string axis;               // Houdini target: "" (from the metadata) | "unity" | "unreal" | "houdini" | "zup"
        float axisScale = 0.0f;         // engine units per metre of the target (0: 1, Unreal 100)
        std::string mode = "auto";      // rigid: "auto" (Bones up to VAT_MAX_BONES pieces) | "bones" | "vertices"
        std::string precision = "auto"; // Vertices: "auto" | "half" | "float"
        std::string preset = "mobile_web";  // "mobile_web" | "desktop"
        bool normals = true;            // Vertices: write the tangent-frame texel
        bool colors = true;             // Vertices: write the colour texel when the export has colours
        int upsample = 0;               // rigid: 0 = automatic, >= 1 = this factor exactly
        float toleranceMm = 1.0f;       // > 0, scaled up to 0.05% of the bounds diagonal
        // explicit files, overriding the resolved ones ("" = resolved)
        std::string mesh, position, position2, rotation, color, lookup, vat, normalsTexture;
    };

    using VatImportClipReport = VatClipReport;

    // The baker's report (VatReport.h, spec 6) plus what an import adds
    struct VatImportReport: VatReport{
        VatImportKind kind = VatImportKind::None;
        HoudiniMode houdiniMode = HoudiniMode::Unknown;
        int meshVertices = 0;
        int sourcePoints = 0;           // points (FBX control points / glTF vertices) of the source mesh
        int pieces = 0;                 // Houdini rigid: pieces found
        int sourceFrames = 0;           // frames in the export
        int frames = 0;                 // samples in the payload (sourceFrames x upsample)
        int upsample = 1;
        float fps = 0.0f;               // the export's frames per second
        std::string target;             // Houdini: the axes converted from
        float interpolationErrorMm = 0.0f;  // rigid: worst distance from Houdini's motion between samples
    };

    struct VatImportResult{
        std::vector<VatImportIssue> errors;
        VatImportReport report;
        jasperengine::vat::VatData data;    // recipe filled, guid new; mesh/layoutHash/stats.bytes by encodeVatFiles
        VatMeshData mesh;
        jasperengine::vat::VatPayload payload;
        jasperengine::vat::VatSocketTracks sockets;     // none from imports
        std::vector<std::string> filesRead; // absolute paths, every file whose content was used

        bool ok() const { return errors.empty(); }
    };

    VatImportResult importVat(const VatImportSpec& spec);

    // Rebake: the spec that re-runs an import's recipe (sources[0].kind houdini_vat3 / openvat,
    // its path and the "import" options, relative paths resolved against projectRoot); false
    // with `error` for another kind of recipe
    bool importSpecFromRecipe(const jasperengine::vat::VatRecipe& recipe, const std::string& projectRoot, VatImportSpec& spec,
                              std::string& error);

    // encodeVatFiles on a successful import (fills data.mesh, layoutHash, stats.bytes); false with
    // `error` when the import failed or the encoder refuses
    bool encodeVatImport(VatImportResult& result, const std::string& vatPath, VatEncodedFiles& out, std::string* error);

    // the MCP / CLI report: {kind, houdini_mode, mode, mode_reason, vertices, mesh_vertices, bones,
    // source_points, pieces, source_frames, frames, upsample, fps, target, clips[{name, frames, fps, bytes}],
    // texture{width, height, format}, bytes, max_error_mm, mean_error_mm, quantization_error_mm,
    // tolerance_mm, interpolation_error_mm, fetches_per_vertex, warnings[{kind, message, fix}]}
    std::string vatImportReportJson(const VatImportReport& report);
    // {kind, mode, mode_source, asset, folder, files{...}, metadata, frames, fps, target, clips, issues[], summary}
    std::string vatImportDetectionJson(const VatImportDetection& detection);
    // "kind: message (fix)" lines
    std::string vatImportIssuesText(const std::vector<VatImportIssue>& issues);

}

#endif //VAT_IMPORT_H
