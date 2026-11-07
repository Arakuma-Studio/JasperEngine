// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A glTF 2.0 file as the model cook reads and writes it (spec S53 R9, R12): the JSON kept as it
// was read (key order, extensions, extras, unknown members), its buffers, and the data of every
// bufferView with EXT_meshopt_compression / KHR_meshopt_compression decoded (each stream checked
// before the decoder sees it: the decoder only asserts). Accessors read as their raw element bytes
// (a lossless cook copies them bit for bit) or as floats. Writes a binary glTF.

#pragma once

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace gltf {
        constexpr int BYTE = 5120;
        constexpr int UNSIGNED_BYTE = 5121;
        constexpr int SHORT = 5122;
        constexpr int UNSIGNED_SHORT = 5123;
        constexpr int UNSIGNED_INT = 5125;
        constexpr int FLOAT = 5126;
        constexpr int ARRAY_BUFFER = 34962;
        constexpr int ELEMENT_ARRAY_BUFFER = 34963;

        int componentSize(int componentType);           // 0 for an invalid type
        int componentCount(const std::string& type);    // "VEC3" -> 3, 0 when invalid
        const char* typeName(int components);           // 3 -> "VEC3" (1-4)
    }

    // An accessor's shape
    struct GltfAccessorLayout {
        int componentType = 0;
        int components = 0;
        bool normalized = false;
        size_t count = 0;
        size_t elementSize = 0;     // componentSize * components
        bool sparse = false;
        bool hasView = false;
    };

    class GltfDocument {
    public:
        using Json = nlohmann::ordered_json;

        // A .gltf or .glb (told apart by its bytes); external buffers relative to its directory.
        // The source bytes read (main file, then external buffers in order) go to `read` when given.
        bool load(const std::filesystem::path& file, std::string& error, std::vector<uint8_t>* read = nullptr);
        bool loadMemory(const std::vector<uint8_t>& bytes, const std::filesystem::path& baseDir, const std::string& label,
                        std::string& error, std::vector<uint8_t>* read = nullptr);

        Json& json() { return root; }
        const Json& json() const { return root; }
        const std::vector<std::vector<uint8_t>>& getBuffers() const { return buffers; }
        // the extensions the file requires
        std::vector<std::string> requiredExtensions() const;
        std::vector<std::string> usedExtensions() const;

        // The bytes of a bufferView (decoded when compressed); false with a reason when invalid
        bool viewData(size_t view, const uint8_t*& data, size_t& size, size_t& stride, std::string& error) const;

        bool accessorLayout(size_t accessor, GltfAccessorLayout& out, std::string& error) const;
        // count * elementSize bytes, dense: the bufferView's stride removed, the sparse
        // substitution applied, zeros without a bufferView
        bool readAccessorRaw(size_t accessor, std::vector<uint8_t>& out, GltfAccessorLayout& layout, std::string& error) const;
        // as floats, `components` per element: normalized integers as the GPU reads them
        bool readAccessorFloats(size_t accessor, std::vector<float>& out, int& components, std::string& error) const;
        // an index list (indices accessor) as 32-bit values
        bool readIndices(size_t accessor, std::vector<uint32_t>& out, std::string& error) const;

        // A buffer of `bytes` and a bufferView over all of it (data the cook decoded: Draco
        // streams, a transcoded image); the view's index
        size_t addView(std::vector<uint8_t> bytes, size_t stride = 0, int target = 0);
        // Makes the buffers and bufferViews arrays when the file has none, so references into the
        // JSON stay valid across addView (ordered_json moves its members when a key is added);
        // pruneViews() removes them again when they stayed empty (glTF forbids empty arrays)
        void prepareViews();
        void pruneViews();
        // An image's encoded bytes: its bufferView, a data URI, or a file next to the model
        bool imageBytes(size_t image, std::vector<uint8_t>& out, std::string& error) const;
        // the directory external buffers and images resolve against
        const std::filesystem::path& getBaseDir() const { return baseDir; }

        // meshopt_decodeFilterOct with its scalar arithmetic: the SIMD paths estimate the length
        // (SSE's rsqrt differs by vendor, NEON's is coarser and fused), so the normals the cook
        // decodes from its own encoding, and the LODs it guides with them, would differ by CPU.
        // A source's views decode as a game's loader does (meshoptimizer's own).
        static void decodeOctahedral(void* data, size_t count, size_t stride);

        // A binary glTF of `json` with `bin` as its BIN chunk (padded as the format wants)
        static std::vector<uint8_t> writeGlb(const Json& json, const std::vector<uint8_t>& bin);

        // the number of a JSON member (a non-negative integer); false when missing or not one
        static bool getIndex(const Json& object, const char* key, size_t& out);
        static const Json* arrayOf(const Json& root, const char* key);

    private:
        Json root;
        std::vector<std::vector<uint8_t>> buffers;
        // the decoded bytes of compressed bufferViews (empty for the others)
        std::vector<std::vector<uint8_t>> decodedViews;
        std::string label;
        std::filesystem::path baseDir;

        bool decodeCompressedViews(std::string& error);
    };

}
