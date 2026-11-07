// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// glTF material extensions (docs/specs/S11-materials-lighting.md R4, R5): KHR_materials_clearcoat,
// _sheen, _anisotropy, _iridescence, _transmission, _volume, _specular, _ior and
// _emissive_strength into Material::pbr and Material::emissiveStrength, and KHR_materials_unlit
// as the library Unlit shader. Values are clamped to the specifications' ranges; a lobe whose
// values come in two textures gets one map packed from them on a job (TexturePack).

#include "MeshSystem.h"
#include "component/ModelComponent.h"
#include "pool/ModelPool.h"

#include "Log.h"
#include "pool/TextureDataPool.h"
#include "shader/ShaderUniforms.h"
#include "texture/TexturePack.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>

using namespace jasperengine;

namespace {

    // one report per model and message (a model loads again on every scene that has it)
    bool reportOnce(const std::string& filename, const std::string& message){
        static std::mutex mutex;
        static std::set<std::string> reported;
        std::lock_guard<std::mutex> lock(mutex);
        return reported.insert(filename + "\n" + message).second;
    }

    struct GltfTextureRef{
        int index = -1;
        int texCoord = 0;
        float scale = 1.0f;
    };

    // finite as a float: JSON has no NaN or infinity, but 1e39 overflows a float to infinity
    bool finiteFloat(double number){
        return std::isfinite(number) && std::fabs(number) <= (double)std::numeric_limits<float>::max();
    }

    // a number of an extension's object, the default when absent; a non-finite one (past the
    // float range: it would be infinity) takes the default and is reported
    float number(const tinygltf::Value& object, const char* key, float fallback, bool& nonFinite){
        if (!object.Has(key))
            return fallback;
        const tinygltf::Value& value = object.Get(key);
        if (!value.IsNumber())
            return fallback;
        const double number = value.GetNumberAsDouble();
        if (!finiteFloat(number)){
            nonFinite = true;
            return fallback;
        }
        return (float)number;
    }

    Vector3 color(const tinygltf::Value& object, const char* key, const Vector3& fallback, bool& nonFinite){
        if (!object.Has(key))
            return fallback;
        const tinygltf::Value& value = object.Get(key);
        if (!value.IsArray() || value.ArrayLen() < 3)
            return fallback;
        Vector3 result;
        for (int c = 0; c < 3; c++){
            const tinygltf::Value& channel = value.Get(c);
            const double number = channel.IsNumber() ? channel.GetNumberAsDouble() : NAN;
            if (!finiteFloat(number)){
                nonFinite = true;
                return fallback;
            }
            result[c] = (float)number;
        }
        return result;
    }

    GltfTextureRef textureRef(const tinygltf::Value& object, const char* key){
        GltfTextureRef ref;
        if (!object.Has(key))
            return ref;
        const tinygltf::Value& info = object.Get(key);
        if (!info.IsObject() || !info.Has("index") || !info.Get("index").IsInt())
            return ref;
        ref.index = info.Get("index").GetNumberAsInt();
        if (info.Has("texCoord") && info.Get("texCoord").IsInt())
            ref.texCoord = info.Get("texCoord").GetNumberAsInt() >= 1 ? 1 : 0;
        if (info.Has("scale") && info.Get("scale").IsNumber())
            ref.scale = (float)info.Get("scale").GetNumberAsDouble();
        return ref;
    }

    // a texture's image as a packing source (null when it is not one)
    bool packSource(const tinygltf::Model& gltf, int textureIndex, TexturePack::Source& source){
        if (textureIndex < 0 || textureIndex >= (int)gltf.textures.size())
            return false;
        const int image = gltf.textures[textureIndex].source;
        if (image < 0 || image >= (int)gltf.images.size())
            return false;
        const tinygltf::Image& data = gltf.images[image];
        source.pixels = data.image.data();
        source.width = data.width;
        source.height = data.height;
        source.components = data.component;
        source.bytesPerComponent = data.bits == 16 ? 2 : 1;
        const size_t needed = (size_t)source.width * (size_t)source.height * (size_t)source.components * (size_t)source.bytesPerComponent;
        return source.valid() && data.image.size() >= needed;
    }

    // what identifies a texture's pixels: the model file and the image (as the core maps)
    std::string imageKey(const std::string& modelKey, const tinygltf::Model& gltf, int textureIndex){
        if (textureIndex < 0 || textureIndex >= (int)gltf.textures.size())
            return "none";
        return modelKey + "|gltf-image|" + std::to_string(gltf.textures[textureIndex].source);
    }

}

bool MeshSystem::loadGLTFLobeMap(ModelComponent& model, const GltfLobeMap& lobeMap, Texture& map, int& texCoord){
    const int first = lobeMap.first;
    const int second = lobeMap.second;
    map = Texture();
    if (first < 0 && second < 0)
        return true;
    texCoord = lobeMap.firstTexCoord;
    if (first < 0)
        texCoord = lobeMap.secondTexCoord;

    // one texture holding both (glTF's own packing, or a lobe with one map): as it is
    if (first == second || !lobeMap.layout)
        return loadGLTFTexture(first >= 0 ? first : second, model, map, model.filename + "|" + lobeMap.name);

    // two (or one in another channel): packed into the lobe's layout on a job
    const tinygltf::Model& gltf = *model.gltfModel;
    // (a preview's images, decoded smaller, keyed apart: MeshSystem::getImageKeySuffix)
    const std::string imageKeySuffix = getImageKeySuffix();
    const std::string poolKey = getModelFilenameKey(model.filename);
    const std::string modelKey = poolKey + imageKeySuffix;
    const std::string id = TexturePack::makeId(imageKey(modelKey, gltf, first), imageKey(modelKey, gltf, second), *lobeMap.layout);
    map.setGenerated(id);
    // the model's decoded images stay in ModelPool while it is used (a full-resolution map
    // whose parse a reload replaced takes the pooled one); once the model is gone the map
    // fails to load and its factors draw alone
    std::weak_ptr<tinygltf::Model> source = model.gltfModel;
    const std::string fallbackKey = imageKeySuffix.empty() ? poolKey : std::string();
    const TexturePack::Layout* layout = lobeMap.layout;
    TextureDataPool::setGenerator(id, [source, fallbackKey, first, second, layout]() -> TextureData {
        std::shared_ptr<tinygltf::Model> gltfModel = source.lock();
        if (!gltfModel && !fallbackKey.empty())
            gltfModel = ModelPool::getGLTF(fallbackKey);
        if (!gltfModel)
            return TextureData();
        TexturePack::Source a, b;
        const bool hasA = packSource(*gltfModel, first, a);
        const bool hasB = packSource(*gltfModel, second, b);
        if (!hasA && !hasB)
            return TextureData();
        return TexturePack::pack(hasA ? &a : nullptr, hasB ? &b : nullptr, *layout);
    });

    // the sampler of the first texture there is
    const int samplerTexture = first >= 0 ? first : second;
    if (samplerTexture < (int)gltf.textures.size()){
        const int sampler = gltf.textures[samplerTexture].sampler;
        if (sampler >= 0 && sampler < (int)gltf.samplers.size()){
            map.setMinFilter(convertFilter(gltf.samplers[sampler].minFilter));
            map.setMagFilter(convertFilter(gltf.samplers[sampler].magFilter));
            map.setWrapU(convertWrap(gltf.samplers[sampler].wrapS));
            map.setWrapV(convertWrap(gltf.samplers[sampler].wrapT));
        }
    }
    return true;
}

bool MeshSystem::loadGLTFMaterialExtensions(const tinygltf::Material& gltfMaterial, ModelComponent& model, Material& material, bool vertexColor){
    const std::string& filename = model.filename;
    const tinygltf::ExtensionMap& extensions = gltfMaterial.extensions;
    MaterialPbrExtensions pbr;
    bool nonFinite = false;
    bool mixedUVSets = false;

    auto extension = [&](const char* name) -> const tinygltf::Value* {
        auto it = extensions.find(name);
        return (it != extensions.end() && it->second.IsObject()) ? &it->second : nullptr;
    };
    auto lobeMap = [&](const tinygltf::Value& ext, const char* firstKey, const char* secondKey, const TexturePack::Layout* layout,
                       const char* name, Texture& map, int& texCoord) -> bool {
        const GltfTextureRef first = textureRef(ext, firstKey);
        const GltfTextureRef second = secondKey ? textureRef(ext, secondKey) : GltfTextureRef();
        mixedUVSets = mixedUVSets || (first.index >= 0 && second.index >= 0 && first.texCoord != second.texCoord);
        GltfLobeMap source;
        source.first = first.index;
        source.second = second.index;
        source.firstTexCoord = first.texCoord;
        source.secondTexCoord = second.texCoord;
        source.layout = secondKey ? layout : nullptr;
        source.name = name;
        return loadGLTFLobeMap(model, source, map, texCoord);
    };

    if (const tinygltf::Value* ext = extension("KHR_materials_clearcoat")){
        pbr.clearcoat.enabled = true;
        pbr.clearcoat.factor = number(*ext, "clearcoatFactor", 0.0f, nonFinite);
        pbr.clearcoat.roughness = number(*ext, "clearcoatRoughnessFactor", 0.0f, nonFinite);
        if (!lobeMap(*ext, "clearcoatTexture", "clearcoatRoughnessTexture", &TexturePack::CLEARCOAT, "clearcoatTexture",
                     pbr.clearcoat.map, pbr.clearcoat.mapTexCoord))
            return false;
        const GltfTextureRef normal = textureRef(*ext, "clearcoatNormalTexture");
        if (normal.index >= 0){
            if (!loadGLTFTexture(normal.index, model, pbr.clearcoat.normalMap, filename + "|clearcoatNormalTexture"))
                return false;
            pbr.clearcoat.normalTexCoord = normal.texCoord;
            pbr.clearcoat.normalScale = std::isfinite(normal.scale) ? normal.scale : 1.0f;
        }
    }

    if (const tinygltf::Value* ext = extension("KHR_materials_sheen")){
        pbr.sheen.enabled = true;
        pbr.sheen.color = color(*ext, "sheenColorFactor", Vector3(0.0f, 0.0f, 0.0f), nonFinite);
        pbr.sheen.roughness = number(*ext, "sheenRoughnessFactor", 0.0f, nonFinite);
        if (!lobeMap(*ext, "sheenColorTexture", "sheenRoughnessTexture", &TexturePack::SHEEN, "sheenTexture",
                     pbr.sheen.map, pbr.sheen.mapTexCoord))
            return false;
    }

    if (const tinygltf::Value* ext = extension("KHR_materials_anisotropy")){
        pbr.anisotropy.enabled = true;
        pbr.anisotropy.strength = number(*ext, "anisotropyStrength", 0.0f, nonFinite);
        pbr.anisotropy.rotation = number(*ext, "anisotropyRotation", 0.0f, nonFinite);
        if (!lobeMap(*ext, "anisotropyTexture", nullptr, nullptr, "anisotropyTexture", pbr.anisotropy.map, pbr.anisotropy.mapTexCoord))
            return false;
    }

    if (const tinygltf::Value* ext = extension("KHR_materials_iridescence")){
        pbr.iridescence.enabled = true;
        pbr.iridescence.factor = number(*ext, "iridescenceFactor", 0.0f, nonFinite);
        pbr.iridescence.ior = number(*ext, "iridescenceIor", 1.3f, nonFinite);
        pbr.iridescence.thicknessNm = Vector2(number(*ext, "iridescenceThicknessMinimum", 100.0f, nonFinite),
                                              number(*ext, "iridescenceThicknessMaximum", 400.0f, nonFinite));
        if (!lobeMap(*ext, "iridescenceTexture", "iridescenceThicknessTexture", &TexturePack::IRIDESCENCE, "iridescenceTexture",
                     pbr.iridescence.map, pbr.iridescence.mapTexCoord))
            return false;
    }

    // transmission and the volume's thickness share one map (R and G)
    const tinygltf::Value* transmission = extension("KHR_materials_transmission");
    const tinygltf::Value* volume = extension("KHR_materials_volume");
    if (transmission){
        pbr.transmission.enabled = true;
        pbr.transmission.factor = number(*transmission, "transmissionFactor", 0.0f, nonFinite);
        const GltfTextureRef factorMap = textureRef(*transmission, "transmissionTexture");
        const GltfTextureRef thicknessMap = volume ? textureRef(*volume, "thicknessTexture") : GltfTextureRef();
        mixedUVSets = mixedUVSets || (factorMap.index >= 0 && thicknessMap.index >= 0 && factorMap.texCoord != thicknessMap.texCoord);
        GltfLobeMap source;
        source.first = factorMap.index;
        source.second = thicknessMap.index;
        source.firstTexCoord = factorMap.texCoord;
        source.secondTexCoord = thicknessMap.texCoord;
        source.layout = &TexturePack::TRANSMISSION;
        source.name = "transmissionTexture";
        if (!loadGLTFLobeMap(model, source, pbr.transmission.map, pbr.transmission.mapTexCoord))
            return false;
    }
    if (volume){
        // a volume without transmission draws nothing of it (MaterialPbr::getPbrVariant)
        pbr.volume.enabled = true;
        pbr.volume.thickness = number(*volume, "thicknessFactor", 0.0f, nonFinite);
        pbr.volume.attenuationDistance = INFINITY;
        if (volume->Has("attenuationDistance")){
            const float distance = number(*volume, "attenuationDistance", INFINITY, nonFinite);
            pbr.volume.attenuationDistance = distance > 0.0f ? distance : INFINITY;
        }
        pbr.volume.attenuationColor = color(*volume, "attenuationColor", Vector3(1.0f, 1.0f, 1.0f), nonFinite);
    }

    const tinygltf::Value* specular = extension("KHR_materials_specular");
    const tinygltf::Value* ior = extension("KHR_materials_ior");
    if (specular || ior){
        // one lobe for both: KHR_materials_ior alone is the IOR with the default reflectance
        pbr.specular.enabled = true;
        if (specular){
            pbr.specular.factor = number(*specular, "specularFactor", 1.0f, nonFinite);
            pbr.specular.color = color(*specular, "specularColorFactor", Vector3(1.0f, 1.0f, 1.0f), nonFinite);
            if (!lobeMap(*specular, "specularTexture", "specularColorTexture", &TexturePack::SPECULAR, "specularTexture",
                         pbr.specular.map, pbr.specular.mapTexCoord))
                return false;
        }
        if (ior)
            pbr.specular.ior = number(*ior, "ior", 1.5f, nonFinite);
    }

    if (const tinygltf::Value* ext = extension("KHR_materials_emissive_strength")){
        const float strength = number(*ext, "emissiveStrength", 1.0f, nonFinite);
        material.emissiveStrength = strength >= 0.0f ? strength : 1.0f;
    }

    if (MaterialPbr::clamp(pbr) && reportOnce(filename, "range"))
        Log::warn("glTF %s: a material extension value is out of its range; clamped", filename.c_str());
    if (nonFinite && reportOnce(filename, "nan"))
        Log::warn("glTF %s: a material extension value is not a finite number; its default is used", filename.c_str());
    if (mixedUVSets && reportOnce(filename, "uv sets"))
        Log::warn("glTF %s: a lobe's two textures use different UV sets; its packed map takes the first's", filename.c_str());

    if (pbr != MaterialPbrExtensions())
        material.pbr = pbr;

    // KHR_materials_unlit: the library Unlit shader with the base colour (Lighting None)
    if (extension("KHR_materials_unlit")){
        material.shader = "library:unlit";
        material.lighting = MaterialLighting::NONE;
        const Vector4& base = material.baseColorFactor;
        ShaderUniforms::set(material.shaderParams, "baseColor", base);
        if (!material.baseColorTexture.empty())
            material.shaderTextures.push_back({"u_baseTexture", material.baseColorTexture});
        if (vertexColor)
            material.shaderFeatures.push_back("VERTEX_COLOR");
        if (material.alphaMode == MaterialAlphaMode::MASK){
            material.shaderFeatures.push_back("ALPHA_CLIP");
            ShaderUniforms::set(material.shaderParams, "alphaCutoff", Vector4(material.alphaCutoff, 0.0f, 0.0f, 0.0f));
        }else if (material.alphaMode == MaterialAlphaMode::BLEND){
            material.blendMode = MaterialBlendMode::ALPHA;
        }
        std::sort(material.shaderFeatures.begin(), material.shaderFeatures.end());
    }
    return true;
}
