// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The step between a glTF parse and its use (docs/specs/S53-model-import-cook.md R1-R6): it
// decodes and widens what the extensions of optimised files (gltfpack, gltf-transform, Blender's
// Draco) store, so the rest of the loader reads core glTF only:
// - EXT_meshopt_compression and KHR_meshopt_compression (every mode and filter, each stream
//   validated first) into their fallback buffers;
// - KHR_draco_mesh_compression and EXT_texture_webp through decoders the editor registers
//   (games never carry them: the cook transcodes those files);
// - KHR_mesh_quantization: integer attributes the vertex layout cannot bind widened to floats
//   (positions always, the readers on the CPU take floats), the normalized ones it binds kept;
// - KHR_texture_transform baked into the UVs of the texture sets it covers;
// - normalized animation outputs and instance transforms (EXT_mesh_gpu_instancing) to floats;
// - KHR_texture_basisu's image as the texture's source (decoded by the KTX2/Basis path);
// - MSFT_lod: the lower levels taken off the scene, their chain kept for the cook.
// A file without these extensions costs a walk over its JSON objects and nothing else.
//
// What the step finds goes to the Console once and to ModelLoadDiagnostics, which the
// Inspector, the Project window badges and MCP read.

#ifndef GLTFNORMALIZE_H
#define GLTFNORMALIZE_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tinygltf {
    class Model;
}

namespace jasperengine {

    enum class GltfDiagnosticSeverity {
        Info,     // nothing is wrong: what the load did that the cook acts on (an editor-only decode)
        Warning,  // the model loads; something in it is ignored or approximated
        Error     // a required part cannot be read: the model is wrong or does not load, export fails
    };

    // One finding of a model's load (S53 R5): the Console line is `message`, then `fix`
    struct JASPERENGINE_API GltfDiagnostic {
        GltfDiagnosticSeverity severity = GltfDiagnosticSeverity::Warning;
        std::string extension;   // the glTF extension it is about, empty when none
        std::string part;        // where in the file ("bufferView 3", "texture 1"), empty for the file
        std::string message;     // what is wrong, naming the file
        std::string fix;         // what to do about it
        bool cookFixes = false;  // the model's cook removes it (Draco, WebP, Basis on native targets)

        bool operator==(const GltfDiagnostic& other) const;
        // message and fix as one Console line
        std::string text() const;
    };

    // MSFT_lod on a node: the node is level 0 (the one that loads), `levels` the lower ones in
    // the file's order (coarser each time), with the MSFT_screencoverage switch points when
    // the file has them (-1 when absent). The cook folds them into the mesh's LOD levels (R4).
    struct JASPERENGINE_API GltfLodLevel {
        int node = -1;
        int mesh = -1;
        float screenCoverage = -1.0f;
    };

    struct JASPERENGINE_API GltfLodChain {
        GltfLodLevel base;
        std::vector<GltfLodLevel> levels;
    };

    // one copy of an EXT_mesh_gpu_instancing node, in the node's space (rotation x, y, z, w as glTF)
    struct JASPERENGINE_API GltfInstance {
        float translation[3] = {0.0f, 0.0f, 0.0f};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
    };

    struct JASPERENGINE_API GltfNormalizeResult {
        // false: the model cannot be used (an Error diagnostic says why)
        bool ok = true;
        std::vector<GltfDiagnostic> diagnostics;
        std::vector<GltfLodChain> lodChains;
        // the extensions this load decoded or converted (for the Inspector and inspect_models)
        std::vector<std::string> handledExtensions;
        // decoded with a decoder only the editor has (Draco, WebP): the file must always cook
        bool needsEditorDecoders = false;

        bool hasErrors() const;
        // the first error as a Console line, empty without one
        std::string firstError() const;
    };

    // What the editor's Draco decoder is asked for: a Draco attribute (its unique id in the
    // stream) as the glTF accessor that receives it is typed
    struct JASPERENGINE_API GltfDracoRequest {
        int uniqueId = -1;
        int componentType = 0;   // TINYGLTF_COMPONENT_TYPE_*
        int components = 0;
        bool normalized = false;
    };

    struct JASPERENGINE_API GltfDracoMesh {
        uint32_t vertexCount = 0;
        std::vector<uint32_t> indices;
        // one per request, vertexCount * components values of its component type, packed
        std::vector<std::vector<uint8_t>> attributes;
    };

    class JASPERENGINE_API GltfNormalize {
    public:
        // a Draco stream (a KHR_draco_mesh_compression bufferView) into the requested attributes
        using DracoDecoder = bool (*)(const uint8_t* data, size_t size, const std::vector<GltfDracoRequest>& requests,
                                      GltfDracoMesh& mesh, std::string& error);
        // a WebP image into RGBA8, rows top first
        using WebpDecoder = bool (*)(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height,
                                     std::string& error);

        // The largest model a load decodes (meshopt fallback buffers, Draco, widened streams): a
        // corrupt count asks for more, and is an error instead of an allocation
        static constexpr size_t kMaxDecodedBytes = size_t(512) << 20;

        // Normalises `model` in place (once per parse, before anything reads it) and reports what
        // it found for `filename` (Console, ModelLoadDiagnostics). False when the model cannot be
        // used; result.firstError() says why.
        static bool run(tinygltf::Model& model, const std::string& filename, GltfNormalizeResult& result);
        // the same without the Console and the registry (the cook, tests)
        static bool normalize(tinygltf::Model& model, const std::string& filename, GltfNormalizeResult& result);

        // the MSFT_lod chains of a model as the file has them (before run, which takes the lower
        // levels off the scene)
        static std::vector<GltfLodChain> readLodChains(const tinygltf::Model& model);

        // the instances of an EXT_mesh_gpu_instancing node of a normalised model (its accessors are
        // floats then); false when the node has none
        static bool readInstances(const tinygltf::Model& model, int node, std::vector<GltfInstance>& instances);

        // an extension the loader reads (including the ones run converts away)
        static bool isSupportedExtension(const std::string& name);
        static const std::vector<std::string>& supportedExtensions();

        // the editor's decoders (editor/util/GltfEditorDecoders.cpp); games register none
        static void setDracoDecoder(DracoDecoder decoder);
        static void setWebpDecoder(WebpDecoder decoder);
        static bool hasDracoDecoder();
        static bool hasWebpDecoder();

        // an image's encoded bytes that the glTF image decoder (stb) does not read: WebP through
        // the registered decoder, KTX2 (KHR_texture_basisu) through the Basis transcoder, both to
        // RGBA8. False with an empty `error` when the bytes are neither.
        static bool isWebp(const uint8_t* data, size_t size);
        static bool decodeImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height,
                                std::string& error);
    };

    // The findings of each model's last load (S53 R5), keyed by MeshSystem::getModelFilenameKey:
    // the Console has them once, the Inspector, the Project window badges and inspect_models ask
    // here. A load replaces its model's list; revision() changes on every change.
    class JASPERENGINE_API ModelLoadDiagnostics {
    public:
        static void set(const std::string& filename, const std::vector<GltfDiagnostic>& diagnostics);
        static std::vector<GltfDiagnostic> get(const std::string& filename);
        static bool hasErrors(const std::string& filename);
        // the models that have a finding, in key order
        static std::vector<std::string> models();
        static uint64_t revision();
        static void clear(const std::string& filename);
        static void clearAll();
    };

}

#endif // GLTFNORMALIZE_H
