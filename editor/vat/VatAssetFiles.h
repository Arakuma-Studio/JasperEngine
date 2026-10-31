// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2.1, 5.3)
// - Purpose: the two files of a Vertex Animation asset as one thing for the editor's file
//   operations: "<stem>.vat" (JSON, GUID-tracked by AssetDatabase) and its generated
//   "<stem>.vat.glb" (the render mesh + payload, a model file). RenameFileCmd, CopyFileCmd and
//   DeleteFileCmd take the pair along (so the Project window, the MCP asset tools and the CLI all
//   do), a rename rewrites the .vat's "mesh" field, and a Vertex Animator's GameObject whose Model
//   is the old mesh follows its .vat when the reference resolves by GUID (a closed scene opened
//   after a rename, a prefab asset, an undone delete).
//   The .vat.glb names the project's texture files by URIs relative to itself: a move or copy of
//   the pair to another directory rewrites them (rebaseVatMeshUris: the GLB's JSON chunk only,
//   the BIN chunk kept byte for byte) so they still name the same files (or the copies made in
//   the same step); the commands' undo puts the JSON back. A delete never touches the textures.
//   A rename of a pair member to another extension is refused (pairRenameRefusal).
// - Thread-safety: pure helpers (followVatMesh writes one registry: the caller's thread).
// - Invariants: a pair is only ever formed by the exact names "<stem>.vat" + "<stem>.vat.glb" in
//   the same directory; a lone file (the other one missing) is handled alone.

#pragma once

#include "ecs/Entity.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor::vat{

    // "<x>.vat.glb" (the generated mesh of "<x>.vat")
    bool isVatMeshFile(const std::string& path);
    // "<x>.vat" of a "<x>.vat.glb", "<x>.vat.glb" of a "<x>.vat"; empty for any other path
    std::filesystem::path pairPathOf(const std::filesystem::path& path);
    // pairPathOf when that file exists (a regular file), else empty
    std::filesystem::path existingPairOf(const std::filesystem::path& path);
    // `paths` followed by the existing pair of each one not listed already (order kept, no
    // duplicates): what a move, copy or delete of a selection really handles
    std::vector<std::filesystem::path> withPairs(const std::vector<std::filesystem::path>& paths);

    // The pair a rename of `oldPath` to `newPath` takes along: X.vat -> Y.vat moves X.vat.glb to
    // Y.vat.glb, X.vat.glb -> Y.vat.glb moves X.vat to Y.vat (when that file exists). False when
    // there is none (another extension on either side, no pair on disk, a directory)
    bool renamedPair(const std::filesystem::path& oldPath, const std::filesystem::path& newPath,
                     std::filesystem::path& pairOld, std::filesystem::path& pairNew);

    // Why `from` can't be renamed / moved to `to` (`fromName` in the text): it is one file of an
    // existing pair and `to` doesn't keep its extension (".vat" / ".vat.glb"); empty when fine
    std::string pairRenameRefusal(const std::filesystem::path& from, const std::filesystem::path& to, const std::string& fromName);

    // The JSON chunk of a GLB (its first chunk), as stored (trailing space padding included).
    // False when the file can't be read or is not a GLB 2.0
    bool readGlbJson(const std::filesystem::path& glb, std::string& json);
    // Replaces the JSON chunk of a GLB with `json` (padded with spaces to 4 bytes), keeping every
    // other chunk's bytes, and fixes the chunk and file lengths (written through a temporary file)
    bool writeGlbJson(const std::filesystem::path& glb, const std::string& json);

    // A .vat.glb that went from `oldDirectory` to its current place `glb` (a move or a copy): its
    // relative image URIs, which named files from `oldDirectory`, name the same files from the
    // new directory. `along` lists what went in the same step (old -> new absolute paths of files
    // or folders): a texture among them (or inside one of the folders) is named at its new place.
    // data: URIs, URIs with a scheme or an absolute path, and bufferView (embedded) images are
    // left alone. True when the chunk changed (`before` / `after` hold the two texts, for an undo
    // that writes `before` back while the file still holds `after`)
    bool rebaseVatMeshUris(const std::filesystem::path& glb, const std::filesystem::path& oldDirectory,
                           const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& along,
                           std::string& before, std::string& after);
    // Every .vat.glb at `path` (a file) or under it (a folder)
    std::vector<std::filesystem::path> vatMeshesAt(const std::filesystem::path& path);

    // A rebase made by a command, to be undone: the file now at `file` held `after` and goes back
    // to `before`
    struct VatUriEdit{
        std::filesystem::path file;
        std::string before;
        std::string after;
    };
    // After a move or copy of `moved` (old -> new absolute paths): every .vat.glb that changed
    // directory rebased (rebaseVatMeshUris); the edits, keyed by the new place
    std::vector<VatUriEdit> rebaseMovedVatMeshes(const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& moved);
    // The undo of a move: every edit's file (now back at `oldPlace(edit.file)`) gets `before` when
    // it still holds `after`
    void restoreVatMeshUris(const std::vector<VatUriEdit>& edits,
                            const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& moved);

    // What the Project window offers to the baker (vertex animation spec 5.3, 4.6):
    // a source to bake: a model (.gltf, .glb, .fbx; not a generated .vat.glb) or an .anim
    bool isVatBakeSource(const std::filesystem::path& path);
    // an export to import (reads the disk): OpenVAT's "<name>-remap_info.json", Houdini's legacy
    // "<asset>_data.json", a Unity "<asset>_mat.mat" with a "geo" folder next to it (or next to its
    // folder), or a mesh (.fbx, .glb, .gltf, .obj) with VAT textures beside it: OpenVAT's
    // "<name>_vat.(exr|png)", Houdini's "<asset>_pos.(exr|png|tga)" here or in "../tex" (a mesh
    // "<asset>_mesh.fbx")
    bool isVatImportSource(const std::filesystem::path& path);

    // The .vat text with its "mesh" field set (canonical text, vat::writeVat). False when the
    // text is not a Vertex Animation (it is left unchanged) or already names `mesh`
    bool setMeshField(std::string& text, const std::string& mesh);

    // A Vertex Animator's reference moved from `oldVat` to `newVat` (asset-relative .vat paths):
    // the Model of the same GameObject that played the old mesh ("<oldVat>.glb") plays the new
    // one; with `fillEmpty`, an empty Model (its mesh was deleted, then the delete undone) gets
    // the new mesh too. True when the Model changed (it reloads)
    bool followVatMesh(EntityRegistry* registry, Entity entity, const std::string& oldVat, const std::string& newVat,
                       bool fillEmpty);

}
