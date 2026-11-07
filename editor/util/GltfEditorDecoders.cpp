// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The decoders of the glTF extensions only the editor reads (docs/specs/S53-model-import-cook.md
// R6): KHR_draco_mesh_compression (editor/libs/draco) and EXT_texture_webp (editor/libs/libwebp).
// They register with GltfNormalize as the editor's objects load, so every editor executable and
// the editor tests decode them; the engine and the games it exports never link these libraries,
// and the model cook transcodes such files for them.

#include "subsystem/GltfNormalize.h"

#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"
#include "src/webp/decode.h"
#include "tiny_gltf.h"

#include <cstring>
#include <limits>
#include <memory>

using namespace jasperengine;

namespace {

    // the largest WebP image a model texture takes (as the texture importer's largest size)
    constexpr int kMaxWebpSide = 16384;

    template <typename T>
    bool convertAttribute(const draco::PointAttribute& attribute, const draco::Mesh& mesh, int components, std::vector<uint8_t>& out){
        out.resize((size_t)mesh.num_points() * (size_t)components * sizeof(T));
        T* values = reinterpret_cast<T*>(out.data());
        for (draco::PointIndex point(0); point < mesh.num_points(); ++point){
            if (!attribute.ConvertValue<T>(attribute.mapped_index(point), (int8_t)components, values + (size_t)point.value() * components))
                return false;
        }
        return true;
    }

    bool decodeDraco(const uint8_t* data, size_t size, const std::vector<GltfDracoRequest>& requests, GltfDracoMesh& out,
                     std::string& error){
        draco::DecoderBuffer buffer;
        buffer.Init(reinterpret_cast<const char*>(data), size);
        draco::Decoder decoder;
        auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
        if (!decoded.ok()){
            error = decoded.status().error_msg_string();
            return false;
        }
        const std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();
        if (!mesh){
            error = "the stream holds no mesh";
            return false;
        }
        out.vertexCount = mesh->num_points();
        out.indices.resize((size_t)mesh->num_faces() * 3);
        for (draco::FaceIndex face(0); face < mesh->num_faces(); ++face){
            for (int corner = 0; corner < 3; corner++)
                out.indices[(size_t)face.value() * 3 + corner] = mesh->face(face)[corner].value();
        }
        out.attributes.assign(requests.size(), {});
        for (size_t r = 0; r < requests.size(); r++){
            const GltfDracoRequest& request = requests[r];
            const draco::PointAttribute* attribute = mesh->GetAttributeByUniqueId((uint32_t)request.uniqueId);
            if (!attribute){
                error = "the stream has no attribute " + std::to_string(request.uniqueId);
                return false;
            }
            bool converted = false;
            switch (request.componentType){
                case TINYGLTF_COMPONENT_TYPE_FLOAT: converted = convertAttribute<float>(*attribute, *mesh, request.components, out.attributes[r]); break;
                case TINYGLTF_COMPONENT_TYPE_BYTE: converted = convertAttribute<int8_t>(*attribute, *mesh, request.components, out.attributes[r]); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: converted = convertAttribute<uint8_t>(*attribute, *mesh, request.components, out.attributes[r]); break;
                case TINYGLTF_COMPONENT_TYPE_SHORT: converted = convertAttribute<int16_t>(*attribute, *mesh, request.components, out.attributes[r]); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: converted = convertAttribute<uint16_t>(*attribute, *mesh, request.components, out.attributes[r]); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: converted = convertAttribute<uint32_t>(*attribute, *mesh, request.components, out.attributes[r]); break;
                default: break;
            }
            if (!converted){
                error = "attribute " + std::to_string(request.uniqueId) + " does not convert to its accessor's type";
                return false;
            }
        }
        return true;
    }

    bool decodeWebp(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error){
        if (!WebPGetInfo(data, size, &width, &height)){
            error = "not a WebP image libwebp reads";
            return false;
        }
        if (width <= 0 || height <= 0 || width > kMaxWebpSide || height > kMaxWebpSide){
            error = "a WebP image of " + std::to_string(width) + " x " + std::to_string(height) + " pixels, past " +
                    std::to_string(kMaxWebpSide);
            return false;
        }
        const size_t stride = (size_t)width * 4;
        rgba.resize(stride * (size_t)height);
        if (!WebPDecodeRGBAInto(data, size, rgba.data(), rgba.size(), (int)stride)){
            error = "the WebP image does not decode";
            return false;
        }
        return true;
    }

    struct Registration {
        Registration(){
            GltfNormalize::setDracoDecoder(&decodeDraco);
            GltfNormalize::setWebpDecoder(&decodeWebp);
        }
    };

    const Registration registration;

}
