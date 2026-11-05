// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the loader makes of a model's source, for its cook (docs/specs/S53-model-import-cook.md
// R4-R6): the source parsed and normalised as a game would load it (GltfNormalize::normalize,
// images left encoded), which gives the load's diagnostics, the authored MSFT_lod chains and the
// streams of the extensions only the editor decodes. The cook then moves those streams into its
// own document as core glTF and writes fresh buffers of them: the normalised model's views and
// buffers keep their compression extensions, so they are never written as they are.

#pragma once

#include "model/GltfDocument.h"

#include "subsystem/GltfNormalize.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tinygltf {
    class Model;
}

namespace jasperengine::editor {

    struct ModelSourceLoad {
        std::shared_ptr<tinygltf::Model> model;
        GltfNormalizeResult result;
    };

    class ModelSourceDecode {
    public:
        // Parses `file` with tinygltf (external images not read, none decoded) and normalises it.
        // False when the file does not parse or normalise; `error` (the loader's message) says why.
        static bool inspect(const std::filesystem::path& file, const std::string& label, ModelSourceLoad& out, std::string& error);

        // KHR_draco_mesh_compression: each Draco primitive's accessors take the decoded streams of
        // `load` in new views of `doc`, and the extension leaves the primitives and the file's
        // lists. False with a reason when a stream is missing.
        static bool takeDraco(GltfDocument& doc, const ModelSourceLoad& load, std::string& error);

        // EXT_texture_webp: every WebP image becomes a PNG in a view of its own and each texture
        // with the extension takes it as its source (a fallback source wins), the extension removed.
        // With `basisFallback` (the native families), a KHR_texture_basisu texture without a
        // `source` gets its KTX2 image as a PNG `source` too (the extension stays for web).
        // `transcoded` lists the extensions this removed or completed.
        static bool transcodeImages(GltfDocument& doc, bool basisFallback, std::vector<std::string>& transcoded, std::string& error);

        // the first Error of the load that the cook does not remove (an extension the engine
        // cannot read): what fails the cook, and so the export (R5); empty when none
        static std::string blockingError(const std::vector<GltfDiagnostic>& diagnostics);
    };

}
