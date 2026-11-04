// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GltfNormalize.h"

#include "Log.h"
#include "subsystem/MeshSystem.h"
#include "texture/BasisTranscode.h"
#include "texture/Ktx2.h"

#include "meshoptimizer.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <tuple>

using namespace jasperengine;

// file-local helpers, named for unity builds (engine/cmake/JasperUnityBuild.cmake)
namespace je_gltf_normalize {

    const char* const MESHOPT_EXT = "EXT_meshopt_compression";
    const char* const MESHOPT_KHR = "KHR_meshopt_compression";
    const char* const QUANTIZATION = "KHR_mesh_quantization";
    const char* const TEXTURE_TRANSFORM = "KHR_texture_transform";
    const char* const BASISU = "KHR_texture_basisu";
    const char* const WEBP = "EXT_texture_webp";
    // the name some exporters wrote before EXT_texture_webp, read the same
    const char* const WEBP_KHR = "KHR_texture_webp";
    const char* const DRACO = "KHR_draco_mesh_compression";
    const char* const INSTANCING = "EXT_mesh_gpu_instancing";
    const char* const LOD = "MSFT_lod";
    // the cook's baked LOD levels (contract C1): read by the LOD pool, passed through here
    const char* const JE_MESH_LODS = "JE_mesh_lods";

    const std::vector<std::string>& supportedList(){
        static const std::vector<std::string> list = {
            "KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_anisotropy", "KHR_materials_iridescence",
            "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_specular", "KHR_materials_ior",
            "KHR_materials_emissive_strength", "KHR_materials_unlit",
            // metadata only: nothing to draw differently
            "KHR_xmp", "KHR_xmp_json_ld",
            // converted to core glTF by GltfNormalize::run
            MESHOPT_EXT, MESHOPT_KHR, QUANTIZATION, TEXTURE_TRANSFORM, BASISU, INSTANCING, LOD,
            DRACO, WEBP, WEBP_KHR,
            JE_MESH_LODS,
        };
        return list;
    }

    struct Decoders {
        std::mutex mutex;
        GltfNormalize::DracoDecoder draco = nullptr;
        GltfNormalize::WebpDecoder webp = nullptr;
    };

    Decoders& decoders(){
        static Decoders instance;
        return instance;
    }

    GltfNormalize::DracoDecoder dracoDecoder(){
        std::lock_guard<std::mutex> lock(decoders().mutex);
        return decoders().draco;
    }

    GltfNormalize::WebpDecoder webpDecoder(){
        std::lock_guard<std::mutex> lock(decoders().mutex);
        return decoders().webp;
    }

    bool listed(const std::vector<std::string>& names, const std::string& name){
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // the file name a message shows: the path as the caller has it
    struct Context {
        tinygltf::Model& model;
        const std::string& filename;
        GltfNormalizeResult& result;
        size_t decodedBytes = 0;
        // one buffer, appended to the model, for every stream this step writes
        int outBuffer = -1;

        bool required(const char* extension) const{
            return listed(model.extensionsRequired, extension);
        }
        bool used(const char* extension) const{
            return listed(model.extensionsUsed, extension) || required(extension);
        }

        void add(GltfDiagnosticSeverity severity, const std::string& extension, const std::string& part,
                 const std::string& message, const std::string& fix, bool cookFixes = false){
            GltfDiagnostic diagnostic;
            diagnostic.severity = severity;
            diagnostic.extension = extension;
            diagnostic.part = part;
            diagnostic.message = message;
            diagnostic.fix = fix;
            diagnostic.cookFixes = cookFixes;
            if (std::find(result.diagnostics.begin(), result.diagnostics.end(), diagnostic) == result.diagnostics.end())
                result.diagnostics.push_back(std::move(diagnostic));
        }
        // an error that stops the load: what in the file is wrong, and that the file is damaged
        bool fail(const std::string& extension, const std::string& part, const std::string& what){
            add(GltfDiagnosticSeverity::Error, extension, part,
                "glTF " + filename + ": " + part + (extension.empty() ? std::string() : " (" + extension + ")") + " " + what,
                "the file is damaged or was written wrong: export it again");
            result.ok = false;
            return false;
        }
        void handled(const char* extension){
            if (!listed(result.handledExtensions, extension))
                result.handledExtensions.push_back(extension);
        }

        // room for `bytes` more decoded bytes (S53 R2: a corrupt count is an error, never an allocation)
        bool reserve(size_t bytes, const std::string& extension, const std::string& part){
            if (bytes > GltfNormalize::kMaxDecodedBytes || decodedBytes > GltfNormalize::kMaxDecodedBytes - bytes)
                return fail(extension, part, "decodes to more than " + std::to_string(GltfNormalize::kMaxDecodedBytes >> 20) + " MB");
            decodedBytes += bytes;
            return true;
        }

        // a new bufferView over `bytes` in the step's own buffer
        int appendView(const std::vector<uint8_t>& bytes, int byteStride, int target, const std::string& name){
            if (outBuffer < 0){
                tinygltf::Buffer buffer;
                buffer.name = "GltfNormalize";
                model.buffers.push_back(std::move(buffer));
                outBuffer = (int)model.buffers.size() - 1;
            }
            std::vector<unsigned char>& data = model.buffers[outBuffer].data;
            while (data.size() % 4)
                data.push_back(0);
            tinygltf::BufferView view;
            view.buffer = outBuffer;
            view.byteOffset = data.size();
            view.byteLength = bytes.size();
            view.byteStride = (size_t)byteStride;
            view.target = target;
            view.name = name;
            data.insert(data.end(), bytes.begin(), bytes.end());
            model.bufferViews.push_back(std::move(view));
            return (int)model.bufferViews.size() - 1;
        }
    };

    // --- JSON values

    bool intMember(const tinygltf::Value& object, const char* key, int& out){
        if (!object.IsObject() || !object.Has(key))
            return false;
        const tinygltf::Value& value = object.Get(key);
        if (!value.IsNumber())
            return false;
        const double number = value.GetNumberAsDouble();
        if (!std::isfinite(number) || number < 0.0 || number > (double)std::numeric_limits<int>::max() || number != std::floor(number))
            return false;
        out = (int)number;
        return true;
    }

    bool sizeMember(const tinygltf::Value& object, const char* key, size_t& out){
        if (!object.IsObject() || !object.Has(key))
            return false;
        const tinygltf::Value& value = object.Get(key);
        if (!value.IsNumber())
            return false;
        const double number = value.GetNumberAsDouble();
        // 2^53: exact in a double, and past any file a load reads
        if (!std::isfinite(number) || number < 0.0 || number > 9007199254740992.0 || number != std::floor(number))
            return false;
        out = (size_t)number;
        return true;
    }

    std::string stringMember(const tinygltf::Value& object, const char* key, const std::string& fallback){
        if (!object.IsObject() || !object.Has(key) || !object.Get(key).IsString())
            return fallback;
        return object.Get(key).Get<std::string>();
    }

    bool numberMember(const tinygltf::Value& object, const char* key, double& out){
        if (!object.IsObject() || !object.Has(key) || !object.Get(key).IsNumber())
            return false;
        out = object.Get(key).GetNumberAsDouble();
        return std::isfinite(out);
    }

    const tinygltf::Value* extension(const tinygltf::ExtensionMap& extensions, const char* name){
        auto found = extensions.find(name);
        if (found == extensions.end() || !found->second.IsObject())
            return nullptr;
        return &found->second;
    }

    // --- accessors

    size_t componentSize(int componentType){
        const int32_t size = tinygltf::GetComponentSizeInBytes((uint32_t)componentType);
        return size > 0 ? (size_t)size : 0;
    }

    int componentCount(int type){
        const int32_t count = tinygltf::GetNumComponentsInType((uint32_t)type);
        return count > 0 ? count : 0;
    }

    // a component as glTF reads it: normalized integers to [0, 1] or [-1, 1] (glTF 2.0, 3.11)
    float component(const uint8_t* p, int componentType, bool normalized){
        switch (componentType){
            case TINYGLTF_COMPONENT_TYPE_BYTE: {
                const int8_t v = (int8_t)p[0];
                return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return normalized ? (float)p[0] / 255.0f : (float)p[0];
            case TINYGLTF_COMPONENT_TYPE_SHORT: {
                int16_t v;
                std::memcpy(&v, p, 2);
                return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t v;
                std::memcpy(&v, p, 2);
                return normalized ? (float)v / 65535.0f : (float)v;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                uint32_t v;
                std::memcpy(&v, p, 4);
                return (float)v;
            }
            case TINYGLTF_COMPONENT_TYPE_FLOAT: {
                float v;
                std::memcpy(&v, p, 4);
                return v;
            }
            default:
                return 0.0f;
        }
    }

    // the bytes of `count` elements of `elementSize` at `offset` with `stride` in a bufferView,
    // checked against the view and its buffer
    const uint8_t* viewElements(const tinygltf::Model& model, int viewIndex, size_t offset, size_t count, size_t elementSize,
                                size_t stride, std::string& error){
        if (viewIndex < 0 || viewIndex >= (int)model.bufferViews.size()){
            error = "names bufferView " + std::to_string(viewIndex) + ", which the file does not have";
            return nullptr;
        }
        const tinygltf::BufferView& view = model.bufferViews[viewIndex];
        if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()){
            error = "bufferView " + std::to_string(viewIndex) + " names a buffer the file does not have";
            return nullptr;
        }
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        if (view.byteOffset > buffer.data.size() || view.byteLength > buffer.data.size() - view.byteOffset){
            error = "bufferView " + std::to_string(viewIndex) + " ends past its buffer";
            return nullptr;
        }
        if (count == 0)
            return buffer.data.data() + view.byteOffset;
        // the last element's end, without overflow
        if (stride != 0 && count - 1 > (std::numeric_limits<size_t>::max() - elementSize - offset) / stride){
            error = "has a count past any buffer";
            return nullptr;
        }
        const size_t end = offset + (count - 1) * stride + elementSize;
        if (end > view.byteLength){
            error = "reads past the end of bufferView " + std::to_string(viewIndex);
            return nullptr;
        }
        return buffer.data.data() + view.byteOffset + offset;
    }

    // An accessor's values as floats (sparse substitution applied), checked against its
    // bufferView; `components` per element
    bool readFloats(const tinygltf::Model& model, int accessorIndex, std::vector<float>& out, int& components, std::string& error){
        if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size()){
            error = "names accessor " + std::to_string(accessorIndex) + ", which the file does not have";
            return false;
        }
        const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
        components = componentCount(accessor.type);
        const size_t size = componentSize(accessor.componentType);
        if (components == 0 || size == 0){
            error = "accessor " + std::to_string(accessorIndex) + " has no valid type";
            return false;
        }
        if (accessor.count > GltfNormalize::kMaxDecodedBytes / ((size_t)components * sizeof(float))){
            error = "accessor " + std::to_string(accessorIndex) + " has a count past any buffer";
            return false;
        }
        out.assign(accessor.count * (size_t)components, 0.0f);
        const size_t elementSize = size * (size_t)components;
        if (accessor.bufferView >= 0){
            if (accessor.bufferView >= (int)model.bufferViews.size()){
                error = "accessor " + std::to_string(accessorIndex) + " names a bufferView the file does not have";
                return false;
            }
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            const size_t stride = view.byteStride ? view.byteStride : elementSize;
            std::string where;
            const uint8_t* base = viewElements(model, accessor.bufferView, accessor.byteOffset, accessor.count, elementSize, stride, where);
            if (!base){
                error = "accessor " + std::to_string(accessorIndex) + " " + where;
                return false;
            }
            for (size_t e = 0; e < accessor.count; e++){
                const uint8_t* element = base + e * stride;
                for (int c = 0; c < components; c++)
                    out[e * components + c] = component(element + c * size, accessor.componentType, accessor.normalized);
            }
        }
        if (accessor.sparse.isSparse && accessor.sparse.count > 0){
            const size_t sparseCount = (size_t)accessor.sparse.count;
            const size_t indexSize = componentSize(accessor.sparse.indices.componentType);
            std::string where;
            const uint8_t* indices = indexSize ? viewElements(model, accessor.sparse.indices.bufferView, accessor.sparse.indices.byteOffset,
                                                              sparseCount, indexSize, indexSize, where) : nullptr;
            const uint8_t* values = indices ? viewElements(model, accessor.sparse.values.bufferView, accessor.sparse.values.byteOffset,
                                                           sparseCount, elementSize, elementSize, where) : nullptr;
            if (!indices || !values){
                error = "accessor " + std::to_string(accessorIndex) + "'s sparse values " + (where.empty() ? "have no valid index type" : where);
                return false;
            }
            for (size_t s = 0; s < sparseCount; s++){
                const size_t target = (size_t)component(indices + s * indexSize, accessor.sparse.indices.componentType, false);
                if (target >= accessor.count){
                    error = "accessor " + std::to_string(accessorIndex) + "'s sparse values point past its count";
                    return false;
                }
                for (int c = 0; c < components; c++)
                    out[target * components + c] = component(values + s * elementSize + c * size, accessor.componentType, accessor.normalized);
            }
        }
        return true;
    }

    std::vector<uint8_t> floatBytes(const std::vector<float>& values){
        std::vector<uint8_t> bytes(values.size() * sizeof(float));
        if (!values.empty())
            std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }

    // Points `accessorIndex` at a float copy of its values (glTF's normalization applied), with
    // bounds when `bounds`: what a KHR_mesh_quantization stream becomes when no vertex format
    // binds it, or a CPU reader needs floats
    bool widenAccessor(Context& context, int accessorIndex, int target, bool bounds, const std::string& what){
        std::vector<float> values;
        int components = 0;
        std::string error;
        if (!readFloats(context.model, accessorIndex, values, components, error))
            return context.fail(QUANTIZATION, what, error);
        if (!context.reserve(values.size() * sizeof(float), QUANTIZATION, what))
            return false;
        tinygltf::Accessor& accessor = context.model.accessors[accessorIndex];
        const int stride = components * (int)sizeof(float);
        accessor.bufferView = context.appendView(floatBytes(values), target == TINYGLTF_TARGET_ARRAY_BUFFER ? stride : 0, target,
                                                 "GltfNormalize|float|" + std::to_string(accessorIndex));
        accessor.byteOffset = 0;
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.normalized = false;
        accessor.sparse = tinygltf::Accessor::Sparse();
        accessor.sparse.isSparse = false;
        accessor.sparse.count = 0;
        if (bounds && accessor.count > 0){
            accessor.minValues.assign((size_t)components, std::numeric_limits<double>::max());
            accessor.maxValues.assign((size_t)components, -std::numeric_limits<double>::max());
            for (size_t e = 0; e < accessor.count; e++){
                for (int c = 0; c < components; c++){
                    accessor.minValues[c] = std::min(accessor.minValues[c], (double)values[e * components + c]);
                    accessor.maxValues[c] = std::max(accessor.maxValues[c], (double)values[e * components + c]);
                }
            }
        }else{
            accessor.minValues.clear();
            accessor.maxValues.clear();
        }
        return true;
    }

    // --- EXT_meshopt_compression, KHR_meshopt_compression (R1, R2)

    struct MeshoptStream {
        int view = -1;
        const char* extension = nullptr;
        int source = -1;
        size_t sourceOffset = 0;
        size_t sourceLength = 0;
        size_t stride = 0;
        size_t count = 0;
        std::string mode;
        std::string filter;
    };

    std::string viewPart(int view){
        return "bufferView " + std::to_string(view);
    }

    bool readStream(Context& context, int viewIndex, const char* name, const tinygltf::Value& object, MeshoptStream& stream){
        const std::string part = viewPart(viewIndex);
        stream.view = viewIndex;
        stream.extension = name;
        if (!intMember(object, "buffer", stream.source))
            return context.fail(name, part, "has no valid compressed buffer");
        if (object.Has("byteOffset") && !sizeMember(object, "byteOffset", stream.sourceOffset))
            return context.fail(name, part, "has an invalid byteOffset");
        if (!sizeMember(object, "byteLength", stream.sourceLength))
            return context.fail(name, part, "has no valid byteLength");
        if (!sizeMember(object, "byteStride", stream.stride))
            return context.fail(name, part, "has no valid byteStride");
        if (!sizeMember(object, "count", stream.count))
            return context.fail(name, part, "has no valid count");
        stream.mode = stringMember(object, "mode", "");
        stream.filter = stringMember(object, "filter", "NONE");

        // the extension's rules (its "Validation" section): what the decoder asserts, checked
        const bool attributes = stream.mode == "ATTRIBUTES";
        const bool indices = stream.mode == "TRIANGLES" || stream.mode == "INDICES";
        if (!attributes && !indices)
            return context.fail(name, part, "has an unknown mode \"" + stream.mode + "\"");
        if (attributes && (stream.stride < 4 || stream.stride > 256 || stream.stride % 4 != 0))
            return context.fail(name, part, "has a byteStride of " + std::to_string(stream.stride) + ", not a multiple of 4 from 4 to 256");
        if (indices && stream.stride != 2 && stream.stride != 4)
            return context.fail(name, part, "has an index byteStride of " + std::to_string(stream.stride) + ", not 2 or 4");
        if (stream.mode == "TRIANGLES" && stream.count % 3 != 0)
            return context.fail(name, part, "has a triangle count of " + std::to_string(stream.count) + " indices, not a multiple of 3");
        if (indices && stream.filter != "NONE")
            return context.fail(name, part, "filters indices");
        const bool khr = std::strcmp(name, MESHOPT_KHR) == 0;
        if (stream.filter == "OCTAHEDRAL" || stream.filter == "COLOR"){
            if (stream.filter == "COLOR" && !khr)
                return context.fail(name, part, "uses the COLOR filter, which only KHR_meshopt_compression has");
            if (stream.stride != 4 && stream.stride != 8)
                return context.fail(name, part, "has a " + stream.filter + " filter over a byteStride of " + std::to_string(stream.stride) + ", not 4 or 8");
        }else if (stream.filter == "QUATERNION"){
            if (stream.stride != 8)
                return context.fail(name, part, "has a QUATERNION filter over a byteStride of " + std::to_string(stream.stride) + ", not 8");
        }else if (stream.filter != "NONE" && stream.filter != "EXPONENTIAL"){
            return context.fail(name, part, "has an unknown filter \"" + stream.filter + "\"");
        }

        const tinygltf::BufferView& view = context.model.bufferViews[viewIndex];
        if (stream.count != 0 && stream.stride > std::numeric_limits<size_t>::max() / stream.count)
            return context.fail(name, part, "has a count past any buffer");
        if (stream.count * stream.stride != view.byteLength)
            return context.fail(name, part, "decodes " + std::to_string(stream.count) + " x " + std::to_string(stream.stride) +
                                " bytes into a bufferView of " + std::to_string(view.byteLength));
        if (stream.source < 0 || stream.source >= (int)context.model.buffers.size())
            return context.fail(name, part, "names compressed buffer " + std::to_string(stream.source) + ", which the file does not have");
        const std::vector<unsigned char>& source = context.model.buffers[stream.source].data;
        if (stream.sourceOffset > source.size() || stream.sourceLength > source.size() - stream.sourceOffset)
            return context.fail(name, part, "reads " + std::to_string(stream.sourceLength) + " compressed bytes past the end of buffer " +
                                std::to_string(stream.source));
        if (view.buffer < 0 || view.buffer >= (int)context.model.buffers.size() || view.buffer == stream.source)
            return context.fail(name, part, "decodes into buffer " + std::to_string(view.buffer) + ", which cannot take it");
        return true;
    }

    bool decodeMeshopt(Context& context){
        tinygltf::Model& model = context.model;
        std::vector<MeshoptStream> streams;
        for (size_t v = 0; v < model.bufferViews.size(); v++){
            for (const char* name : {MESHOPT_EXT, MESHOPT_KHR}){
                const tinygltf::Value* object = extension(model.bufferViews[v].extensions, name);
                if (!object)
                    continue;
                MeshoptStream stream;
                if (!readStream(context, (int)v, name, *object, stream))
                    return false;
                streams.push_back(stream);
                break;
            }
        }
        if (streams.empty())
            return true;

        // the placeholders (the patched tinygltf loads no data for them): sized to the views
        // that land in them, which must all be compressed
        std::vector<size_t> placeholderSize(model.buffers.size(), 0);
        std::vector<bool> placeholder(model.buffers.size(), false);
        for (const MeshoptStream& stream : streams){
            const tinygltf::BufferView& view = model.bufferViews[stream.view];
            if (model.buffers[view.buffer].data.empty())
                placeholder[view.buffer] = true;
        }
        std::vector<bool> compressedView(model.bufferViews.size(), false);
        for (const MeshoptStream& stream : streams)
            compressedView[stream.view] = true;
        for (size_t v = 0; v < model.bufferViews.size(); v++){
            const tinygltf::BufferView& view = model.bufferViews[v];
            if (view.buffer < 0 || view.buffer >= (int)model.buffers.size() || !placeholder[view.buffer])
                continue;
            if (!compressedView[v])
                return context.fail(MESHOPT_EXT, viewPart((int)v), "points into fallback buffer " + std::to_string(view.buffer) +
                                    ", which has no data, without being compressed");
            if (view.byteLength > std::numeric_limits<size_t>::max() - view.byteOffset)
                return context.fail(MESHOPT_EXT, viewPart((int)v), "ends past any buffer");
            placeholderSize[view.buffer] = std::max(placeholderSize[view.buffer], view.byteOffset + view.byteLength);
        }
        for (size_t b = 0; b < model.buffers.size(); b++){
            if (!placeholder[b])
                continue;
            if (!context.reserve(placeholderSize[b], MESHOPT_EXT, "buffer " + std::to_string(b)))
                return false;
            model.buffers[b].data.assign(placeholderSize[b], 0);
        }

        for (const MeshoptStream& stream : streams){
            const tinygltf::BufferView& view = model.bufferViews[stream.view];
            context.handled(stream.extension);
            // an authored fallback (gltfpack -cf) is already the data: nothing to decode
            if (!placeholder[view.buffer])
                continue;
            unsigned char* target = model.buffers[view.buffer].data.data() + view.byteOffset;
            const unsigned char* source = model.buffers[stream.source].data.data() + stream.sourceOffset;
            // the filters store whole elements (the quaternion one 8 bytes at a time) and a view
            // is only 4-byte aligned in its buffer: such a view decodes in an aligned copy first
            std::vector<unsigned char> aligned;
            unsigned char* destination = target;
            if (reinterpret_cast<uintptr_t>(target) % 16 != 0 && view.byteLength > 0){
                aligned.resize(view.byteLength);
                destination = aligned.data();
            }
            int code = -1;
            if (stream.mode == "ATTRIBUTES")
                code = meshopt_decodeVertexBuffer(destination, stream.count, stream.stride, source, stream.sourceLength);
            else if (stream.mode == "TRIANGLES")
                code = meshopt_decodeIndexBuffer(destination, stream.count, stream.stride, source, stream.sourceLength);
            else
                code = meshopt_decodeIndexSequence(destination, stream.count, stream.stride, source, stream.sourceLength);
            if (code != 0)
                return context.fail(stream.extension, viewPart(stream.view),
                                    std::string("has a compressed stream that does not decode (") + (code == -1 ? "an unknown header or version" : "truncated or corrupt") + ")");
            if (stream.filter == "OCTAHEDRAL")
                meshopt_decodeFilterOct(destination, stream.count, stream.stride);
            else if (stream.filter == "QUATERNION")
                meshopt_decodeFilterQuat(destination, stream.count, stream.stride);
            else if (stream.filter == "EXPONENTIAL")
                meshopt_decodeFilterExp(destination, stream.count, stream.stride);
            else if (stream.filter == "COLOR")
                meshopt_decodeFilterColor(destination, stream.count, stream.stride);
            if (destination != target && view.byteLength > 0)
                std::memcpy(target, destination, view.byteLength);
        }

        // decoded indices name vertices the primitive has (corrupt deltas could point anywhere,
        // and the simplifier and the GPU would read past the vertex buffers)
        std::vector<bool> decodedView(model.bufferViews.size(), false);
        for (const MeshoptStream& stream : streams)
            decodedView[stream.view] = placeholder[model.bufferViews[stream.view].buffer];
        for (size_t m = 0; m < model.meshes.size(); m++){
            for (size_t p = 0; p < model.meshes[m].primitives.size(); p++){
                const tinygltf::Primitive& primitive = model.meshes[m].primitives[p];
                if (primitive.indices < 0 || primitive.indices >= (int)model.accessors.size())
                    continue;
                const tinygltf::Accessor& indices = model.accessors[primitive.indices];
                if (indices.bufferView < 0 || indices.bufferView >= (int)decodedView.size() || !decodedView[indices.bufferView])
                    continue;
                auto position = primitive.attributes.find("POSITION");
                if (position == primitive.attributes.end() || position->second < 0 || position->second >= (int)model.accessors.size())
                    continue;
                const size_t vertices = model.accessors[position->second].count;
                const size_t size = componentSize(indices.componentType);
                if (size != 1 && size != 2 && size != 4)
                    continue;
                const tinygltf::BufferView& view = model.bufferViews[indices.bufferView];
                std::string error;
                const uint8_t* data = viewElements(model, indices.bufferView, indices.byteOffset, indices.count, size,
                                                   view.byteStride ? view.byteStride : size, error);
                const std::string part = "mesh " + std::to_string(m) + " primitive " + std::to_string(p);
                if (!data)
                    return context.fail(MESHOPT_EXT, part, "has indices that " + error);
                for (size_t i = 0; i < indices.count; i++){
                    if ((size_t)component(data + i * size, indices.componentType, false) >= vertices)
                        return context.fail("", part, "has decoded indices past its " + std::to_string(vertices) + " vertices (" +
                                            viewPart(indices.bufferView) + ")");
                }
            }
        }
        return true;
    }

    // --- KHR_draco_mesh_compression (R6)

    bool decodeDraco(Context& context){
        tinygltf::Model& model = context.model;
        bool any = false;
        for (const tinygltf::Mesh& mesh : model.meshes){
            for (const tinygltf::Primitive& primitive : mesh.primitives)
                any = any || extension(primitive.extensions, DRACO);
        }
        if (!any)
            return true;
        const GltfNormalize::DracoDecoder decoder = dracoDecoder();
        const bool required = context.required(DRACO);
        if (!decoder){
            if (required){
                context.add(GltfDiagnosticSeverity::Error, DRACO, "",
                            "glTF " + context.filename + " requires " + DRACO + ", which games do not decode",
                            "the cook transcodes it: reimport the model in the editor and export again", true);
                context.result.ok = false;
                return false;
            }
            context.add(GltfDiagnosticSeverity::Warning, DRACO, "",
                        "glTF " + context.filename + " uses " + DRACO + ", which games do not decode; its uncompressed data is loaded",
                        "the cook transcodes it: reimport the model in the editor and export again", true);
            return true;
        }

        // one decode per stream (primitives may share one)
        std::map<int, int> decodedViews;
        for (size_t m = 0; m < model.meshes.size(); m++){
            for (size_t p = 0; p < model.meshes[m].primitives.size(); p++){
                tinygltf::Primitive& primitive = model.meshes[m].primitives[p];
                const tinygltf::Value* object = extension(primitive.extensions, DRACO);
                if (!object)
                    continue;
                const std::string part = "mesh " + std::to_string(m) + " primitive " + std::to_string(p);
                int viewIndex = -1;
                if (!intMember(*object, "bufferView", viewIndex))
                    return context.fail(DRACO, part, "has no Draco bufferView");
                if (decodedViews.count(viewIndex))
                    continue;
                decodedViews[viewIndex] = 1;
                std::string error;
                const tinygltf::BufferView* view = viewIndex < (int)model.bufferViews.size() ? &model.bufferViews[viewIndex] : nullptr;
                const uint8_t* data = view ? viewElements(model, viewIndex, 0, 1, view->byteLength, view->byteLength, error) : nullptr;
                if (!data)
                    return context.fail(DRACO, part, error.empty() ? "names a Draco bufferView the file does not have" : error);

                std::vector<GltfDracoRequest> requests;
                std::vector<int> targets;
                if (object->Has("attributes") && object->Get("attributes").IsObject()){
                    for (const std::string& name : object->Get("attributes").Keys()){
                        int uniqueId = -1;
                        auto attribute = primitive.attributes.find(name);
                        if (!intMember(object->Get("attributes"), name.c_str(), uniqueId) || attribute == primitive.attributes.end() ||
                                attribute->second < 0 || attribute->second >= (int)model.accessors.size())
                            return context.fail(DRACO, part, "maps attribute " + name + " to nothing the primitive has");
                        const tinygltf::Accessor& accessor = model.accessors[attribute->second];
                        GltfDracoRequest request;
                        request.uniqueId = uniqueId;
                        request.componentType = accessor.componentType;
                        request.components = componentCount(accessor.type);
                        request.normalized = accessor.normalized;
                        if (request.components == 0 || componentSize(request.componentType) == 0)
                            return context.fail(DRACO, part, "decodes " + name + " into an accessor of no valid type");
                        requests.push_back(request);
                        targets.push_back(attribute->second);
                    }
                }
                GltfDracoMesh decoded;
                if (!decoder(data, view->byteLength, requests, decoded, error))
                    return context.fail(DRACO, part, "does not decode: " + error);

                size_t bytes = decoded.indices.size() * sizeof(uint32_t);
                for (const std::vector<uint8_t>& values : decoded.attributes)
                    bytes += values.size();
                if (!context.reserve(bytes, DRACO, part))
                    return false;
                for (size_t r = 0; r < requests.size(); r++){
                    tinygltf::Accessor& accessor = model.accessors[targets[r]];
                    const size_t expected = (size_t)decoded.vertexCount * (size_t)requests[r].components * componentSize(requests[r].componentType);
                    if (accessor.count != decoded.vertexCount || r >= decoded.attributes.size() || decoded.attributes[r].size() != expected)
                        return context.fail(DRACO, part, "decodes " + std::to_string(decoded.vertexCount) + " vertices for an accessor of " +
                                            std::to_string(accessor.count));
                    const int stride = requests[r].components * (int)componentSize(requests[r].componentType);
                    accessor.bufferView = context.appendView(decoded.attributes[r], stride % 4 == 0 ? stride : 0, TINYGLTF_TARGET_ARRAY_BUFFER,
                                                             "GltfNormalize|draco|" + std::to_string(targets[r]));
                    accessor.byteOffset = 0;
                    accessor.sparse.isSparse = false;
                }
                if (primitive.indices >= 0 && primitive.indices < (int)model.accessors.size()){
                    tinygltf::Accessor& indices = model.accessors[primitive.indices];
                    if (indices.count != decoded.indices.size())
                        return context.fail(DRACO, part, "decodes " + std::to_string(decoded.indices.size()) + " indices for an accessor of " +
                                            std::to_string(indices.count));
                    std::vector<uint8_t> bytesOut;
                    const size_t size = componentSize(indices.componentType);
                    if (size != 1 && size != 2 && size != 4)
                        return context.fail(DRACO, part, "has indices of no valid type");
                    bytesOut.resize(decoded.indices.size() * size);
                    for (size_t i = 0; i < decoded.indices.size(); i++){
                        const uint32_t index = decoded.indices[i];
                        if (index >= decoded.vertexCount || (size < 4 && index >= (1u << (8 * size))))
                            return context.fail(DRACO, part, "decodes an index past its vertices");
                        std::memcpy(bytesOut.data() + i * size, &index, size); // little-endian, as glTF
                    }
                    indices.bufferView = context.appendView(bytesOut, 0, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER,
                                                            "GltfNormalize|draco|" + std::to_string(primitive.indices));
                    indices.byteOffset = 0;
                }
            }
        }
        context.handled(DRACO);
        context.result.needsEditorDecoders = true;
        context.add(GltfDiagnosticSeverity::Info, DRACO, "",
                    "glTF " + context.filename + " uses " + DRACO + ", which only the editor decodes",
                    "the cook transcodes it, so games never carry the decoder", true);
        return true;
    }

    // --- KHR_mesh_quantization (R1)

    // a vertex format the renderer binds, as MeshSystem maps accessors (SokolObject::getVertexFormat)
    // and the readers on the CPU take
    bool bindable(const std::string& semantic, const tinygltf::Accessor& accessor, const tinygltf::Model& model){
        const int components = componentCount(accessor.type);
        const int type = accessor.componentType;
        if (type == TINYGLTF_COMPONENT_TYPE_FLOAT)
            return true;
        // positions are read as floats on the CPU (bounds, picking, LODs, physics)
        if (semantic == "POSITION")
            return false;
        if (accessor.sparse.isSparse)
            return false;
        const bool byteOrShort = type == TINYGLTF_COMPONENT_TYPE_BYTE || type == TINYGLTF_COMPONENT_TYPE_SHORT ||
                                 type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE || type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
        if (!byteOrShort || !accessor.normalized)
            return false;
        const bool isShort = type == TINYGLTF_COMPONENT_TYPE_SHORT || type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
        if (components == 4)
            return true;  // BYTE4N, UBYTE4N, SHORT4N, USHORT4N
        if (components == 2)
            return isShort; // SHORT2N, USHORT2N (no two-byte format)
        if (components == 3 && semantic == "NORMAL" && (type == TINYGLTF_COMPONENT_TYPE_BYTE || type == TINYGLTF_COMPONENT_TYPE_SHORT)){
            // an octahedral or quantized normal padded to four components (gltfpack, the cook): bound
            // as BYTE4N or SHORT4N, the shader reads xyz. Every element's padding must be in the view.
            if (accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size())
                return false;
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            const size_t padded = 4 * componentSize(type);
            return view.byteStride != 0 && view.byteStride >= padded && accessor.count > 0 &&
                   accessor.byteOffset % view.byteStride + padded <= view.byteStride &&
                   accessor.byteOffset + (accessor.count - 1) * view.byteStride + padded <= view.byteLength;
        }
        return false;
    }

    bool widenAttributes(Context& context){
        tinygltf::Model& model = context.model;
        std::set<int> widened;
        auto widen = [&](int accessorIndex, int target, bool bounds, const std::string& part) -> bool {
            if (widened.count(accessorIndex))
                return true;
            widened.insert(accessorIndex);
            if (!widenAccessor(context, accessorIndex, target, bounds, part))
                return false;
            context.handled(QUANTIZATION);
            return true;
        };
        for (size_t m = 0; m < model.meshes.size(); m++){
            for (size_t p = 0; p < model.meshes[m].primitives.size(); p++){
                tinygltf::Primitive& primitive = model.meshes[m].primitives[p];
                const std::string part = "mesh " + std::to_string(m) + " primitive " + std::to_string(p);
                for (const auto& [semantic, accessorIndex] : primitive.attributes){
                    if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size())
                        continue;
                    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
                    // the core types the loader already takes (colours, joints, weights), and the
                    // engine's own (a Vertex Animation index)
                    const bool quantizable = semantic == "POSITION" || semantic == "NORMAL" || semantic == "TANGENT" ||
                                             semantic.rfind("TEXCOORD_", 0) == 0;
                    if (!quantizable || bindable(semantic, accessor, model))
                        continue;
                    if (!widen(accessorIndex, TINYGLTF_TARGET_ARRAY_BUFFER, semantic == "POSITION", part + " " + semantic))
                        return false;
                }
                // morph target deltas: the morph attributes are floats
                for (const std::map<std::string, int>& target : primitive.targets){
                    for (const auto& [semantic, accessorIndex] : target){
                        if (accessorIndex < 0 || accessorIndex >= (int)model.accessors.size())
                            continue;
                        if (model.accessors[accessorIndex].componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                            continue;
                        if (!widen(accessorIndex, TINYGLTF_TARGET_ARRAY_BUFFER, false, part + " morph " + semantic))
                            return false;
                    }
                }
            }
        }
        return true;
    }

    // --- animation outputs and instance transforms (R1, R3)

    bool widenAnimations(Context& context){
        tinygltf::Model& model = context.model;
        std::set<int> done;
        for (size_t a = 0; a < model.animations.size(); a++){
            for (size_t s = 0; s < model.animations[a].samplers.size(); s++){
                const int output = model.animations[a].samplers[s].output;
                if (output < 0 || output >= (int)model.accessors.size() || done.count(output))
                    continue;
                done.insert(output);
                if (model.accessors[output].componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                    continue;
                if (!widenAccessor(context, output, 0, false, "animation " + std::to_string(a) + " sampler " + std::to_string(s)))
                    return false;
            }
        }
        return true;
    }

    bool widenInstances(Context& context){
        tinygltf::Model& model = context.model;
        for (size_t n = 0; n < model.nodes.size(); n++){
            const tinygltf::Value* object = extension(model.nodes[n].extensions, INSTANCING);
            if (!object)
                continue;
            context.handled(INSTANCING);
            if (!object->Has("attributes") || !object->Get("attributes").IsObject())
                continue;
            const tinygltf::Value& attributes = object->Get("attributes");
            size_t count = std::numeric_limits<size_t>::max();
            for (const char* name : {"TRANSLATION", "ROTATION", "SCALE"}){
                int accessor = -1;
                if (!attributes.Has(name))
                    continue;
                const std::string part = "node " + std::to_string(n) + " " + name;
                if (!intMember(attributes, name, accessor) || accessor >= (int)model.accessors.size())
                    return context.fail(INSTANCING, part, "names an accessor the file does not have");
                if (count != std::numeric_limits<size_t>::max() && model.accessors[accessor].count != count)
                    return context.fail(INSTANCING, part, "has " + std::to_string(model.accessors[accessor].count) +
                                        " instances, not the " + std::to_string(count) + " of the others");
                count = model.accessors[accessor].count;
                if (model.accessors[accessor].componentType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
                        !widenAccessor(context, accessor, 0, false, part))
                    return false;
            }
        }
        return true;
    }

    // --- KHR_texture_transform (R1)

    struct UvTransform {
        double offset[2] = {0.0, 0.0};
        double rotation = 0.0;
        double scale[2] = {1.0, 1.0};
        int texCoord = -1; // the transform's own set, -1 for the texture's

        bool identity() const{
            return offset[0] == 0.0 && offset[1] == 0.0 && rotation == 0.0 && scale[0] == 1.0 && scale[1] == 1.0;
        }
        bool operator==(const UvTransform& o) const{
            return offset[0] == o.offset[0] && offset[1] == o.offset[1] && rotation == o.rotation && scale[0] == o.scale[0] && scale[1] == o.scale[1];
        }
        bool operator<(const UvTransform& o) const{
            return std::tie(offset[0], offset[1], rotation, scale[0], scale[1]) < std::tie(o.offset[0], o.offset[1], o.rotation, o.scale[0], o.scale[1]);
        }
        // the extension's matrix: translation * rotation * scale (its README, "glTF Schema Updates")
        void apply(float& u, float& v) const{
            const double c = std::cos(rotation), s = std::sin(rotation);
            const double su = scale[0] * u, sv = scale[1] * v;
            u = (float)(c * su - s * sv + offset[0]);
            v = (float)(s * su + c * sv + offset[1]);
        }
    };

    bool readTransform(const tinygltf::ExtensionMap& extensions, UvTransform& transform){
        const tinygltf::Value* object = extension(extensions, TEXTURE_TRANSFORM);
        if (!object)
            return false;
        auto pair = [&](const char* key, double* out){
            if (!object->Has(key) || !object->Get(key).IsArray() || object->Get(key).ArrayLen() < 2)
                return;
            for (int i = 0; i < 2; i++){
                const tinygltf::Value& value = object->Get(key).Get(i);
                if (value.IsNumber() && std::isfinite(value.GetNumberAsDouble()))
                    out[i] = value.GetNumberAsDouble();
            }
        };
        pair("offset", transform.offset);
        pair("scale", transform.scale);
        numberMember(*object, "rotation", transform.rotation);
        int texCoord = -1;
        if (intMember(*object, "texCoord", texCoord))
            transform.texCoord = texCoord;
        return true;
    }

    // One texture slot of a material: a core slot (a tinygltf struct) or one inside a
    // KHR_materials_* extension (a JSON object), read and written the same way
    struct TextureSlot {
        std::string name;
        int* texCoord = nullptr;                    // core slot
        tinygltf::ExtensionMap* extensions = nullptr;
        tinygltf::Value* object = nullptr;          // extension slot
        UvTransform transform;
        bool hasTransform = false;

        int set() const{
            if (transform.texCoord >= 0)
                return transform.texCoord;
            if (texCoord)
                return *texCoord;
            int value = 0;
            intMember(*object, "texCoord", value);
            return value;
        }
        void setSet(int set){
            if (texCoord){
                *texCoord = set;
            }else{
                object->Get<tinygltf::Value::Object>()["texCoord"] = tinygltf::Value(set);
            }
        }
        void dropTransform(){
            if (extensions){
                extensions->erase(TEXTURE_TRANSFORM);
            }else if (object->Has("extensions") && object->Get("extensions").IsObject()){
                tinygltf::Value::Object& list = object->Get<tinygltf::Value::Object>()["extensions"].Get<tinygltf::Value::Object>();
                list.erase(TEXTURE_TRANSFORM);
            }
        }
    };

    std::vector<TextureSlot> textureSlots(tinygltf::Material& material){
        std::vector<TextureSlot> slots;
        auto core = [&](const char* name, int index, int* texCoord, tinygltf::ExtensionMap* extensions){
            if (index < 0)
                return;
            TextureSlot slot;
            slot.name = name;
            slot.texCoord = texCoord;
            slot.extensions = extensions;
            slot.hasTransform = readTransform(*extensions, slot.transform);
            slots.push_back(slot);
        };
        tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;
        core("baseColorTexture", pbr.baseColorTexture.index, &pbr.baseColorTexture.texCoord, &pbr.baseColorTexture.extensions);
        core("metallicRoughnessTexture", pbr.metallicRoughnessTexture.index, &pbr.metallicRoughnessTexture.texCoord,
             &pbr.metallicRoughnessTexture.extensions);
        core("normalTexture", material.normalTexture.index, &material.normalTexture.texCoord, &material.normalTexture.extensions);
        core("occlusionTexture", material.occlusionTexture.index, &material.occlusionTexture.texCoord, &material.occlusionTexture.extensions);
        core("emissiveTexture", material.emissiveTexture.index, &material.emissiveTexture.texCoord, &material.emissiveTexture.extensions);
        for (auto& [name, value] : material.extensions){
            if (!value.IsObject())
                continue;
            for (auto& [key, member] : value.Get<tinygltf::Value::Object>()){
                if (!member.IsObject() || !member.Has("index") || !member.Get("index").IsInt())
                    continue;
                TextureSlot slot;
                slot.name = name + "." + key;
                slot.object = &member;
                if (member.Has("extensions") && member.Get("extensions").IsObject()){
                    const tinygltf::Value& list = member.Get("extensions");
                    if (list.Has(TEXTURE_TRANSFORM) && list.Get(TEXTURE_TRANSFORM).IsObject()){
                        tinygltf::ExtensionMap one;
                        one[TEXTURE_TRANSFORM] = list.Get(TEXTURE_TRANSFORM);
                        slot.hasTransform = readTransform(one, slot.transform);
                    }
                }
                slots.push_back(slot);
            }
        }
        return slots;
    }

    // the engine draws two UV sets (TEXCOORD_0, TEXCOORD_1; a later set reads as the second)
    constexpr int UV_SETS = 2;

    bool bakeTextureTransforms(Context& context){
        tinygltf::Model& model = context.model;
        if (!context.used(TEXTURE_TRANSFORM))
            return true;
        context.handled(TEXTURE_TRANSFORM);

        // the primitives of each material
        std::vector<std::vector<tinygltf::Primitive*>> users(model.materials.size());
        for (tinygltf::Mesh& mesh : model.meshes){
            for (tinygltf::Primitive& primitive : mesh.primitives){
                if (primitive.material >= 0 && primitive.material < (int)model.materials.size())
                    users[primitive.material].push_back(&primitive);
            }
        }
        // a baked copy per (UV accessor, transform), shared by the primitives that ask for it
        std::map<std::pair<int, UvTransform>, int> baked;
        auto bake = [&](int source, const UvTransform& transform, int& out, const std::string& part) -> bool {
            auto found = baked.find({source, transform});
            if (found != baked.end()){
                out = found->second;
                return true;
            }
            std::vector<float> values;
            int components = 0;
            std::string error;
            if (!readFloats(model, source, values, components, error))
                return context.fail(TEXTURE_TRANSFORM, part, error);
            if (components != 2)
                return context.fail(TEXTURE_TRANSFORM, part, "has texture coordinates that are not two-component");
            if (!context.reserve(values.size() * sizeof(float), TEXTURE_TRANSFORM, part))
                return false;
            for (size_t i = 0; i + 1 < values.size(); i += 2)
                transform.apply(values[i], values[i + 1]);
            tinygltf::Accessor accessor;
            accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            accessor.type = TINYGLTF_TYPE_VEC2;
            accessor.count = model.accessors[source].count;
            accessor.bufferView = context.appendView(floatBytes(values), 8, TINYGLTF_TARGET_ARRAY_BUFFER,
                                                     "GltfNormalize|uv|" + std::to_string(source));
            accessor.sparse.isSparse = false;
            accessor.sparse.count = 0;
            model.accessors.push_back(std::move(accessor));
            out = (int)model.accessors.size() - 1;
            baked[{source, transform}] = out;
            return true;
        };

        for (size_t m = 0; m < model.materials.size(); m++){
            std::vector<TextureSlot> slots = textureSlots(model.materials[m]);
            bool anyTransform = false;
            for (const TextureSlot& slot : slots)
                anyTransform = anyTransform || (slot.hasTransform && !slot.transform.identity());
            if (!anyTransform){
                for (TextureSlot& slot : slots){
                    if (slot.hasTransform){
                        if (slot.transform.texCoord >= 0)
                            slot.setSet(slot.transform.texCoord);
                        slot.dropTransform();
                    }
                }
                continue;
            }
            const std::string materialPart = "material " + std::to_string(m);

            // per UV set, the transforms its textures ask for (the first slot's first: the base colour)
            std::map<int, std::vector<UvTransform>> perSet;
            for (const TextureSlot& slot : slots){
                const int set = slot.set();
                if (set >= UV_SETS)
                    continue;
                std::vector<UvTransform>& list = perSet[set];
                if (std::find(list.begin(), list.end(), slot.transform) == list.end())
                    list.push_back(slot.transform);
            }
            // a second transform of set 0 can move to set 1 when no texture uses set 1 and no
            // primitive of the material has its own TEXCOORD_1
            bool secondSetFree = perSet.count(1) == 0;
            for (tinygltf::Primitive* primitive : users[m])
                secondSetFree = secondSetFree && primitive->attributes.count("TEXCOORD_1") == 0;
            UvTransform moved;
            bool move = false;
            std::string dropped;
            for (auto& [set, list] : perSet){
                if (list.size() == 1)
                    continue;
                if (set == 0 && list.size() == 2 && secondSetFree){
                    moved = list[1];
                    move = true;
                    list.resize(1);
                    continue;
                }
                dropped += dropped.empty() ? "" : ", ";
                dropped += "set " + std::to_string(set);
                list.resize(1);
            }
            if (!dropped.empty())
                context.add(GltfDiagnosticSeverity::Warning, TEXTURE_TRANSFORM, materialPart,
                            "glTF " + context.filename + ": material " + std::to_string(m) + " (" + model.materials[m].name +
                            ") moves the textures of one UV set (" + dropped + ") with more " + TEXTURE_TRANSFORM +
                            " transforms than the engine draws; the first texture's transform is used for all",
                            "give the textures of a UV set one transform, or put them on separate UV sets, and export again");

            for (tinygltf::Primitive* primitive : users[m]){
                for (const auto& [set, list] : perSet){
                    auto uv = primitive->attributes.find("TEXCOORD_" + std::to_string(set));
                    if (uv == primitive->attributes.end() || list.front().identity())
                        continue;
                    int out = -1;
                    if (!bake(uv->second, list.front(), out, materialPart))
                        return false;
                    uv->second = out;
                }
                if (move){
                    auto uv = primitive->attributes.find("TEXCOORD_0");
                    if (uv == primitive->attributes.end())
                        continue;
                    int out = -1;
                    // from the authored set 0, before the line above replaced it
                    int source = uv->second;
                    for (const auto& [key, value] : baked){
                        if (value == uv->second)
                            source = key.first;
                    }
                    if (!bake(source, moved, out, materialPart))
                        return false;
                    primitive->attributes["TEXCOORD_1"] = out;
                }
            }
            for (TextureSlot& slot : slots){
                const int set = slot.set();
                if (move && set == 0 && slot.transform == moved && !(perSet[0].front() == moved))
                    slot.setSet(1);
                else if (slot.transform.texCoord >= 0)
                    slot.setSet(slot.transform.texCoord);
                slot.dropTransform();
            }
        }
        return true;
    }

    // --- texture sources: KHR_texture_basisu, EXT_texture_webp (R1, R6)

    void textureSources(Context& context){
        tinygltf::Model& model = context.model;
        for (size_t t = 0; t < model.textures.size(); t++){
            tinygltf::Texture& texture = model.textures[t];
            const std::string part = "texture " + std::to_string(t);
            int image = -1;
            if (const tinygltf::Value* basisu = extension(texture.extensions, BASISU)){
                context.handled(BASISU);
                if (!intMember(*basisu, "source", image) || image >= (int)model.images.size()){
                    context.add(GltfDiagnosticSeverity::Warning, BASISU, part, "glTF " + context.filename + ": " + part +
                                " names a KTX2 image the file does not have", "export the model again");
                }else if (BasisTranscode::isAvailable()){
                    texture.source = image;
                }else if (texture.source < 0){
                    context.add(GltfDiagnosticSeverity::Error, BASISU, part,
                                "glTF " + context.filename + " requires " + BASISU + ", which this build does not transcode; " + part + " draws untextured",
                                "the cook transcodes it: reimport the model in the editor and export again", true);
                }
                continue;
            }
            const char* webpName = extension(texture.extensions, WEBP) ? WEBP : extension(texture.extensions, WEBP_KHR) ? WEBP_KHR : nullptr;
            if (!webpName)
                continue;
            context.handled(webpName);
            const tinygltf::Value* webp = extension(texture.extensions, webpName);
            if (!intMember(*webp, "source", image) || image >= (int)model.images.size()){
                context.add(GltfDiagnosticSeverity::Warning, webpName, part, "glTF " + context.filename + ": " + part +
                            " names a WebP image the file does not have", "export the model again");
            }else if (webpDecoder()){
                texture.source = image;
                context.result.needsEditorDecoders = true;
                context.add(GltfDiagnosticSeverity::Info, webpName, "",
                            "glTF " + context.filename + " uses " + webpName + ", which only the editor decodes",
                            "the cook transcodes it, so games never carry the decoder", true);
            }else if (texture.source < 0){
                context.add(GltfDiagnosticSeverity::Error, webpName, part,
                            "glTF " + context.filename + " requires " + webpName + ", which games do not decode; " + part + " draws untextured",
                            "the cook transcodes it: reimport the model in the editor and export again", true);
            }
        }
    }

    // --- MSFT_lod (R4)

    void takeLodLevels(Context& context){
        tinygltf::Model& model = context.model;
        context.result.lodChains = GltfNormalize::readLodChains(model);
        if (context.result.lodChains.empty())
            return;
        context.handled(LOD);
        // the lower levels (and what is under them) draw nothing: level 0 is the mesh
        std::vector<int> stack;
        for (const GltfLodChain& chain : context.result.lodChains){
            for (const GltfLodLevel& level : chain.levels)
                stack.push_back(level.node);
        }
        std::vector<bool> visited(model.nodes.size(), false);
        while (!stack.empty()){
            const int node = stack.back();
            stack.pop_back();
            if (node < 0 || node >= (int)model.nodes.size() || visited[node])
                continue;
            visited[node] = true;
            model.nodes[node].mesh = -1;
            for (int child : model.nodes[node].children)
                stack.push_back(child);
        }
    }

    // --- what the loader does not read (R5)

    void reportUnread(Context& context){
        const tinygltf::Model& model = context.model;
        for (const std::string& name : model.extensionsRequired){
            if (GltfNormalize::isSupportedExtension(name))
                continue;
            context.add(GltfDiagnosticSeverity::Error, name, "",
                        "glTF " + context.filename + " requires " + name + ", which the engine does not read; it is loaded without it",
                        "export the model again without " + name);
        }
        for (const std::string& name : model.extensionsUsed){
            if (GltfNormalize::isSupportedExtension(name) || listed(model.extensionsRequired, name))
                continue;
            context.add(GltfDiagnosticSeverity::Warning, name, "",
                        "glTF " + context.filename + " uses " + name + ", which the engine does not read; it is ignored",
                        "nothing to do, or export the model again without " + name);
        }
    }

    // one Console line per model and message (a model parses again on a reload)
    bool reportOnce(const std::string& filename, const std::string& text){
        static std::mutex mutex;
        static std::set<std::string> reported;
        std::lock_guard<std::mutex> lock(mutex);
        return reported.insert(filename + "\n" + text).second;
    }

}

bool GltfDiagnostic::operator==(const GltfDiagnostic& other) const{
    return severity == other.severity && extension == other.extension && part == other.part && message == other.message &&
           fix == other.fix && cookFixes == other.cookFixes;
}

std::string GltfDiagnostic::text() const{
    return fix.empty() ? message : message + " (" + fix + ")";
}

bool GltfNormalizeResult::hasErrors() const{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const GltfDiagnostic& d){ return d.severity == GltfDiagnosticSeverity::Error; });
}

std::string GltfNormalizeResult::firstError() const{
    for (const GltfDiagnostic& diagnostic : diagnostics){
        if (diagnostic.severity == GltfDiagnosticSeverity::Error)
            return diagnostic.text();
    }
    return std::string();
}

bool GltfNormalize::normalize(tinygltf::Model& model, const std::string& filename, GltfNormalizeResult& result){
    result = GltfNormalizeResult();
    je_gltf_normalize::Context context{model, filename, result};
    // each step only touches what its extension marks; in this order since the later steps read
    // what the earlier ones decoded
    if (!je_gltf_normalize::decodeMeshopt(context) || !je_gltf_normalize::decodeDraco(context))
        return false;
    if (!je_gltf_normalize::widenAttributes(context) || !je_gltf_normalize::widenAnimations(context) || !je_gltf_normalize::widenInstances(context))
        return false;
    if (!je_gltf_normalize::bakeTextureTransforms(context))
        return false;
    je_gltf_normalize::textureSources(context);
    je_gltf_normalize::takeLodLevels(context);
    // read as they are where the vertex layout binds them, or passed on (the LOD pool's)
    for (const char* name : {je_gltf_normalize::QUANTIZATION, je_gltf_normalize::JE_MESH_LODS}){
        if (context.used(name))
            context.handled(name);
    }
    je_gltf_normalize::reportUnread(context);
    return result.ok;
}

bool GltfNormalize::run(tinygltf::Model& model, const std::string& filename, GltfNormalizeResult& result){
    const bool ok = normalize(model, filename, result);
    // the error that stops the load is the caller's to log, as any parse error ("Can't load ...")
    const std::string failure = ok ? std::string() : result.firstError();
    for (const GltfDiagnostic& diagnostic : result.diagnostics){
        const std::string text = diagnostic.text();
        if (text == failure || !je_gltf_normalize::reportOnce(filename, text))
            continue;
        if (diagnostic.severity == GltfDiagnosticSeverity::Error)
            Log::error("%s", text.c_str());
        else if (diagnostic.severity == GltfDiagnosticSeverity::Warning)
            Log::warn("%s", text.c_str());
        else
            Log::verbose("%s", text.c_str());
    }
    ModelLoadDiagnostics::set(filename, result.diagnostics);
    return ok;
}

std::vector<GltfLodChain> GltfNormalize::readLodChains(const tinygltf::Model& model){
    std::vector<GltfLodChain> chains;
    for (size_t n = 0; n < model.nodes.size(); n++){
        const tinygltf::Node& node = model.nodes[n];
        const tinygltf::Value* object = je_gltf_normalize::extension(node.extensions, je_gltf_normalize::LOD);
        if (!object || !object->Has("ids") || !object->Get("ids").IsArray())
            continue;
        std::vector<double> coverage;
        if (node.extras.IsObject() && node.extras.Has("MSFT_screencoverage") && node.extras.Get("MSFT_screencoverage").IsArray()){
            const tinygltf::Value& list = node.extras.Get("MSFT_screencoverage");
            for (size_t i = 0; i < list.ArrayLen(); i++)
                coverage.push_back(list.Get(i).IsNumber() ? list.Get(i).GetNumberAsDouble() : -1.0);
        }
        auto coverageAt = [&](size_t i){
            return i < coverage.size() && std::isfinite(coverage[i]) ? (float)coverage[i] : -1.0f;
        };
        GltfLodChain chain;
        chain.base.node = (int)n;
        chain.base.mesh = node.mesh;
        chain.base.screenCoverage = coverageAt(0);
        const tinygltf::Value& ids = object->Get("ids");
        for (size_t i = 0; i < ids.ArrayLen(); i++){
            if (!ids.Get(i).IsInt())
                continue;
            const int id = ids.Get(i).GetNumberAsInt();
            if (id < 0 || id >= (int)model.nodes.size() || id == (int)n)
                continue;
            GltfLodLevel level;
            level.node = id;
            level.mesh = model.nodes[id].mesh;
            level.screenCoverage = coverageAt(i + 1);
            chain.levels.push_back(level);
        }
        if (!chain.levels.empty())
            chains.push_back(std::move(chain));
    }
    return chains;
}

bool GltfNormalize::readInstances(const tinygltf::Model& model, int node, std::vector<GltfInstance>& instances){
    instances.clear();
    if (node < 0 || node >= (int)model.nodes.size())
        return false;
    const tinygltf::Value* object = je_gltf_normalize::extension(model.nodes[node].extensions, je_gltf_normalize::INSTANCING);
    if (!object || !object->Has("attributes") || !object->Get("attributes").IsObject())
        return false;
    const tinygltf::Value& attributes = object->Get("attributes");
    struct Stream {
        const char* name;
        int components;
        std::vector<float> values;
    };
    Stream streams[] = {{"TRANSLATION", 3, {}}, {"ROTATION", 4, {}}, {"SCALE", 3, {}}};
    size_t count = 0;
    bool any = false;
    for (Stream& stream : streams){
        int accessor = -1;
        if (!je_gltf_normalize::intMember(attributes, stream.name, accessor))
            continue;
        int components = 0;
        std::string error;
        if (!je_gltf_normalize::readFloats(model, accessor, stream.values, components, error) || components != stream.components)
            return false;
        const size_t streamCount = stream.values.size() / (size_t)components;
        if (any && streamCount != count)
            return false;
        count = streamCount;
        any = true;
    }
    if (!any)
        return false;
    instances.resize(count);
    for (size_t i = 0; i < count; i++){
        if (!streams[0].values.empty())
            std::copy_n(streams[0].values.begin() + i * 3, 3, instances[i].translation);
        if (!streams[1].values.empty())
            std::copy_n(streams[1].values.begin() + i * 4, 4, instances[i].rotation);
        if (!streams[2].values.empty())
            std::copy_n(streams[2].values.begin() + i * 3, 3, instances[i].scale);
    }
    return true;
}

bool GltfNormalize::isSupportedExtension(const std::string& name){
    return je_gltf_normalize::listed(je_gltf_normalize::supportedList(), name);
}

const std::vector<std::string>& GltfNormalize::supportedExtensions(){
    return je_gltf_normalize::supportedList();
}

void GltfNormalize::setDracoDecoder(DracoDecoder decoder){
    std::lock_guard<std::mutex> lock(je_gltf_normalize::decoders().mutex);
    je_gltf_normalize::decoders().draco = decoder;
}

void GltfNormalize::setWebpDecoder(WebpDecoder decoder){
    std::lock_guard<std::mutex> lock(je_gltf_normalize::decoders().mutex);
    je_gltf_normalize::decoders().webp = decoder;
}

bool GltfNormalize::hasDracoDecoder(){
    return je_gltf_normalize::dracoDecoder() != nullptr;
}

bool GltfNormalize::hasWebpDecoder(){
    return je_gltf_normalize::webpDecoder() != nullptr;
}

bool GltfNormalize::isWebp(const uint8_t* data, size_t size){
    return size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0;
}

bool GltfNormalize::decodeImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error){
    error.clear();
    if (isWebp(data, size)){
        const WebpDecoder decoder = je_gltf_normalize::webpDecoder();
        if (!decoder){
            error = "a WebP image, which games do not decode";
            return false;
        }
        return decoder(data, size, rgba, width, height, error);
    }
    const std::span<const uint8_t> bytes(data, size);
    if (!Ktx2::isKtx2(bytes))
        return false;
    Ktx2MemorySource source(bytes);
    Ktx2Image image;
    if (!Ktx2::read(source, 0, image, error, 0))
        return false;
    width = (int)image.info.width;
    height = (int)image.info.height;
    if (image.info.codec == Ktx2Codec::Raw){
        if (image.info.format != GpuTextureFormat::RGBA8 || image.info.faces != 1 || image.levels.empty()){
            error = "a KTX2 image of a format a model texture does not take";
            return false;
        }
        rgba = std::move(image.levels[0]);
        return true;
    }
    if (!BasisTranscode::isAvailable()){
        error = "a Basis Universal image, which this build does not transcode";
        return false;
    }
    BasisTranscode::Session session;
    if (!session.open(std::move(image), error))
        return false;
    if (session.faces() != 1){
        error = "a KTX2 image with " + std::to_string(session.faces()) + " faces";
        return false;
    }
    return session.transcodeLevel(0, GpuTextureFormat::RGBA8, TextureContent::ColorAlpha, rgba, error);
}

namespace je_gltf_normalize {

    struct DiagnosticsRegistry {
        std::mutex mutex;
        std::map<std::string, std::vector<GltfDiagnostic>> models;
        uint64_t revision = 0;
    };

    DiagnosticsRegistry& registry(){
        static DiagnosticsRegistry instance;
        return instance;
    }

    std::string diagnosticsKey(const std::string& filename){
        return MeshSystem::getModelFilenameKey(filename);
    }

}

void ModelLoadDiagnostics::set(const std::string& filename, const std::vector<GltfDiagnostic>& diagnostics){
    const std::string key = je_gltf_normalize::diagnosticsKey(filename);
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    auto found = je_gltf_normalize::registry().models.find(key);
    if (diagnostics.empty()){
        if (found == je_gltf_normalize::registry().models.end())
            return;
        je_gltf_normalize::registry().models.erase(found);
    }else{
        if (found != je_gltf_normalize::registry().models.end() && found->second == diagnostics)
            return;
        je_gltf_normalize::registry().models[key] = diagnostics;
    }
    je_gltf_normalize::registry().revision++;
}

std::vector<GltfDiagnostic> ModelLoadDiagnostics::get(const std::string& filename){
    const std::string key = je_gltf_normalize::diagnosticsKey(filename);
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    auto found = je_gltf_normalize::registry().models.find(key);
    return found == je_gltf_normalize::registry().models.end() ? std::vector<GltfDiagnostic>() : found->second;
}

bool ModelLoadDiagnostics::hasErrors(const std::string& filename){
    for (const GltfDiagnostic& diagnostic : get(filename)){
        if (diagnostic.severity == GltfDiagnosticSeverity::Error)
            return true;
    }
    return false;
}

std::vector<std::string> ModelLoadDiagnostics::models(){
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    std::vector<std::string> keys;
    for (const auto& entry : je_gltf_normalize::registry().models)
        keys.push_back(entry.first);
    return keys;
}

uint64_t ModelLoadDiagnostics::revision(){
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    return je_gltf_normalize::registry().revision;
}

void ModelLoadDiagnostics::clear(const std::string& filename){
    const std::string key = je_gltf_normalize::diagnosticsKey(filename);
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    if (je_gltf_normalize::registry().models.erase(key))
        je_gltf_normalize::registry().revision++;
}

void ModelLoadDiagnostics::clearAll(){
    std::lock_guard<std::mutex> lock(je_gltf_normalize::registry().mutex);
    if (!je_gltf_normalize::registry().models.empty())
        je_gltf_normalize::registry().revision++;
    je_gltf_normalize::registry().models.clear();
}
