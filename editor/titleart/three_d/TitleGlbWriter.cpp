// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGlbWriter.h"

#include "titleart/render/TitleContent.h"
#include "titleart/render/TitleRenderServices.h"
#include "titleart/three_d/Title3DMath.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/mesher/TitleMesher.h"
#include "texture/TexturePrepare.h"

#include "stb_image_write.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    constexpr double PI = 3.14159265358979323846;

    struct Writer {
        tinygltf::Model model;
        std::vector<unsigned char>& data() { return model.buffers[0].data; }

        Writer() {
            model.asset.version = "2.0";
            model.asset.generator = "JasperEngine Title Art";
            model.buffers.emplace_back();
            model.scenes.emplace_back();
            model.defaultScene = 0;
        }

        int view(const void* bytes, size_t size, int target) {
            while (data().size() % 4) data().push_back(0);
            tinygltf::BufferView v;
            v.buffer = 0;
            v.byteOffset = data().size();
            v.byteLength = size;
            v.target = target;
            const unsigned char* p = static_cast<const unsigned char*>(bytes);
            data().insert(data().end(), p, p + size);
            model.bufferViews.push_back(v);
            return (int)model.bufferViews.size() - 1;
        }

        int accessor(const std::vector<float>& values, int components, int type, bool bounds) {
            tinygltf::Accessor a;
            a.bufferView = view(values.data(), values.size() * sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
            a.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            a.count = values.size() / (size_t)components;
            a.type = type;
            if (bounds && a.count) {
                a.minValues.assign((size_t)components, DBL_MAX);
                a.maxValues.assign((size_t)components, -DBL_MAX);
                for (size_t i = 0; i < a.count; i++) {
                    for (int c = 0; c < components; c++) {
                        a.minValues[(size_t)c] = std::min(a.minValues[(size_t)c], (double)values[i * components + c]);
                        a.maxValues[(size_t)c] = std::max(a.maxValues[(size_t)c], (double)values[i * components + c]);
                    }
                }
            }
            model.accessors.push_back(a);
            return (int)model.accessors.size() - 1;
        }

        int indexAccessor(const std::vector<uint32_t>& indices) {
            tinygltf::Accessor a;
            a.bufferView = view(indices.data(), indices.size() * 4, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            a.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
            a.count = indices.size();
            a.type = TINYGLTF_TYPE_SCALAR;
            model.accessors.push_back(a);
            return (int)model.accessors.size() - 1;
        }

        // a 256 x 1 sRGB ramp as an embedded PNG texture
        int rampTexture(const titleart::TitleGradient& gradient) {
            uint8_t texels[256 * 4];
            titleart::bakeGradient(gradient, texels);
            std::vector<unsigned char> png;
            stbi_write_png_to_func([](void* context, void* d, int size) {
                auto* out = static_cast<std::vector<unsigned char>*>(context);
                const unsigned char* p = static_cast<const unsigned char*>(d);
                out->insert(out->end(), p, p + size);
            }, &png, 256, 1, 4, texels, 256 * 4);
            tinygltf::Image image;
            image.mimeType = "image/png";
            image.bufferView = view(png.data(), png.size(), 0);
            model.images.push_back(image);
            if (model.samplers.empty()) {
                tinygltf::Sampler s;
                s.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
                s.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
                s.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
                s.wrapT = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
                model.samplers.push_back(s);
            }
            tinygltf::Texture t;
            t.source = (int)model.images.size() - 1;
            t.sampler = 0;
            model.textures.push_back(t);
            return (int)model.textures.size() - 1;
        }
    };

    std::array<float, 3> linear(const titleart::TitleColor& c) {
        return {titleart::srgbToLinear(c.r), titleart::srgbToLinear(c.g), titleart::srgbToLinear(c.b)};
    }

    tinygltf::Value number(double v) { return tinygltf::Value(v); }

    // the material of one title material, its extensions and extras
    int writeMaterial(Writer& w, const titleart::TitleMaterial3D& m, double size, double units, uint64_t sourceHash, std::set<std::string>& used,
                      std::vector<std::string>& warnings) {
        tinygltf::Material mat;
        mat.name = m.preset;
        // a ramp overrides the colour (the renderer's rule): the factor is white under its texture
        const auto base = m.gradient ? std::array<float, 3>{1, 1, 1} : linear(m.color);
        mat.pbrMetallicRoughness.baseColorFactor = {base[0], base[1], base[2], m.opacity};
        mat.pbrMetallicRoughness.metallicFactor = m.metallic;
        mat.pbrMetallicRoughness.roughnessFactor = m.roughness;
        if (m.gradient) mat.pbrMetallicRoughness.baseColorTexture.index = w.rampTexture(*m.gradient);
        const auto em = linear(m.emission);
        const double strength = m.emissionIntensity;
        if (strength > 0) {
            mat.emissiveFactor = {em[0], em[1], em[2]};
            if (strength > 1) {
                tinygltf::Value::Object ext;
                ext["emissiveStrength"] = number(strength);
                mat.extensions["KHR_materials_emissive_strength"] = tinygltf::Value(ext);
                used.insert("KHR_materials_emissive_strength");
            } else {
                mat.emissiveFactor = {em[0] * strength, em[1] * strength, em[2] * strength};
            }
        }
        if (m.opacity < 1) mat.alphaMode = "BLEND";
        if (m.clearcoat > 0) {
            tinygltf::Value::Object ext;
            ext["clearcoatFactor"] = number(m.clearcoat);
            ext["clearcoatRoughnessFactor"] = number(m.clearcoatRoughness);
            mat.extensions["KHR_materials_clearcoat"] = tinygltf::Value(ext);
            used.insert("KHR_materials_clearcoat");
        }
        if (m.iridescence > 0) {
            tinygltf::Value::Object ext;
            ext["iridescenceFactor"] = number(m.iridescence);
            ext["iridescenceIor"] = number(m.iridescenceIor);
            ext["iridescenceThicknessMaximum"] = number(m.iridescenceThicknessNm[1]);
            mat.extensions["KHR_materials_iridescence"] = tinygltf::Value(ext);
            used.insert("KHR_materials_iridescence");
        }
        if (m.transmission > 0) {
            tinygltf::Value::Object t;
            t["transmissionFactor"] = number(m.transmission);
            mat.extensions["KHR_materials_transmission"] = tinygltf::Value(t);
            tinygltf::Value::Object v;
            v["thicknessFactor"] = number(0.3 * size * units);
            v["attenuationColor"] = tinygltf::Value(tinygltf::Value::Array{number(base[0]), number(base[1]), number(base[2])});
            mat.extensions["KHR_materials_volume"] = tinygltf::Value(v);
            used.insert("KHR_materials_transmission");
            used.insert("KHR_materials_volume");
            if (m.dispersion > 0) {
                tinygltf::Value::Object d;
                d["dispersion"] = number(m.dispersion);
                mat.extensions["KHR_materials_dispersion"] = tinygltf::Value(d);
                used.insert("KHR_materials_dispersion");
            }
        }
        if (std::abs(m.ior - 1.5) > 1e-6) {
            tinygltf::Value::Object ext;
            ext["ior"] = number(m.ior);
            mat.extensions["KHR_materials_ior"] = tinygltf::Value(ext);
            used.insert("KHR_materials_ior");
        }
        tinygltf::Value::Object title;
        title["env_multiplier"] = number(m.envMultiplier);
        title["specular_multiplier"] = number(m.specularMultiplier);
        char hash[17];
        std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)sourceHash);
        title["source_hash"] = tinygltf::Value(std::string(hash));
        tinygltf::Value::Object extras;
        extras["jasperengine_title"] = tinygltf::Value(title);
        mat.extras = tinygltf::Value(extras);
        if (m.hasPattern()) warnings.push_back("material '" + m.preset + "': the pattern is not exported");
        if (!m.baseTexture.empty()) warnings.push_back("material '" + m.preset + "': the base texture is not exported");
        if (m.surface != titleart::SurfaceKind3D::None) warnings.push_back("material '" + m.preset + "': the surface relief is not exported");
        if (m.flakes > 0) warnings.push_back("material '" + m.preset + "': flakes are dropped (a static normal map does not read as sparkle)");
        w.model.materials.push_back(mat);
        return (int)w.model.materials.size() - 1;
    }

    // the bent ramp t of a vertex, title_ramp.glsl's formula
    float rampT(const titleart::TitleMaterial3D& m, const titleart::TitleMesh& mesh, const titleart::TitleMeshVertex& v) {
        const auto n = titleart::decodeTitleNormal(v.normal[0], v.normal[1]);
        const float px = v.position[0], py = v.position[1], pz = v.position[2];
        const double a = m.gradientAngle * PI / 180.0;
        const double dx = std::cos(a), dy = -std::sin(a);
        double t;
        if (m.gradientDepth) {
            const titleart::TitleMeshPart& part = mesh.parts[std::min<size_t>(v.ids[2], mesh.parts.size() - 1)];
            t = (part.zFront - pz) / std::max(1e-4f, part.zFront - part.zBack);
        } else {
            const titleart::TitleMeshSpan& entry = m.gradientSpan == titleart::GradientSpan3D::Layer ? mesh.spans[0]
                                                    : m.gradientSpan == titleart::GradientSpan3D::Line ? mesh.spans[std::min<size_t>(v.ids[0], mesh.spans.size() - 1)]
                                                                                                       : mesh.spans[std::min<size_t>(v.ids[1], mesh.spans.size() - 1)];
            const float* box = m.gradientSpan == titleart::GradientSpan3D::Line || m.gradientSpan == titleart::GradientSpan3D::Layer ? entry.line : entry.chr;
            const double cx = (box[0] + box[2]) * 0.5, cy = (box[1] + box[3]) * 0.5;
            const double hx = std::max(1e-4, (box[2] - box[0]) * 0.5), hy = std::max(1e-4, (box[3] - box[1]) * 0.5);
            if (m.gradientKind == titleart::GradientKind3D::Radial) {
                t = std::hypot((px - cx) / hx, (py - cy) / hy) * 0.7071068;
            } else {
                const double ext = std::abs(hx * dx) + std::abs(hy * dy);
                t = ((px - cx) * dx + (py - cy) * dy) / (2 * ext) + 0.5;
                if (m.gradientKind == titleart::GradientKind3D::Reflected) t = std::abs(t * 2 - 1);
            }
            t = (t - 0.5) / std::max(1e-3, m.gradientScale) + 0.5 - m.gradientOffset;
            t += m.gradientBend * 0.25 * (n[0] * dx + n[1] * dy - std::hypot(n[0], n[1]));
        }
        return (float)std::clamp(t, 0.0, 1.0);
    }

    // a group's 2D placement (S80's: flips, clockwise rotation, uniform scale, then position)
    titleart::geometry::Affine localPlacement(const Json& layer) {
        const titleart::TitleTransform t = titleart::decodeTransform(layer.value("transform", Json::object()));
        titleart::geometry::Affine local = titleart::geometry::Affine::scale((t.flipX ? -1.0 : 1.0) * t.scale, (t.flipY ? -1.0 : 1.0) * t.scale);
        local = titleart::geometry::Affine::rotateDegrees(-t.rotation).after(local);
        return titleart::geometry::Affine::translate(t.position.x, t.position.y).after(local);
    }

    std::vector<double> matrixOf(const titleart::Mat4& m, double units) {
        std::vector<double> out(16);
        for (int i = 0; i < 16; i++) out[(size_t)i] = m.m[(size_t)i];
        // translations in metres (the positions are scaled by `units` too)
        out[12] *= units;
        out[13] *= units;
        out[14] *= units;
        return out;
    }

}

TitleGlbResult editor::writeTitleGlb(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, const TitleGlbOptions& options) {
    TitleGlbResult result;
    Writer w;
    std::set<std::string> used;
    auto content = titleart::makeGeometryTitleContentProvider();
    const double units = options.unitsPerPx;
    // every layer's parent group, for the placement of layers inside groups
    std::map<const Json*, const Json*> parents;
    titleart::forEachLayer(doc.root, [&](const Json& layer, const Json* parent, int) {
        parents[&layer] = parent;
        return true;
    });
    titleart::forEachLayer(doc.root, [&](const Json& layer, const Json* parent, int) {
        if (!titleart::layerTypeHasStyle(titleart::layerTypeOf(layer)) || layer.value("dimension", std::string("2d")) != "3d") return true;
        titleart::three_d::TitleMeshRequest request;
        request.source = titleart::three_d::collectTitleSource(layer, doc, *content, assets, nullptr);
        request.style = titleart::decodeStyle3D(layer);
        request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
        request.wrap = true;
        if (request.source.glyphs.empty()) return true;
        titleart::three_d::TitleMesher mesher;
        const std::shared_ptr<const titleart::TitleMesh> mesh = mesher.build(request);
        const titleart::three_d::TitleSlotMaterials materials = titleart::three_d::resolveSlotMaterials(layer, request.style, *mesh);
        const double size = titleart::layerSourceSize(layer);
        std::vector<int> materialIndex(materials.materials.size(), -1);
        // the layer root inside its groups' 2D placement
        titleart::geometry::Affine parentPlace;
        for (const Json* at = parent; at; at = parents[at]) parentPlace = localPlacement(*at).after(parentPlace);
        tinygltf::Node node;
        node.name = layer.value("name", std::string("Title"));
        node.matrix = matrixOf(titleart::title3DLayerMatrix(layer.value("transform", Json::object()), parentPlace), units);
        tinygltf::Mesh gmesh;
        gmesh.name = node.name;
        // per rig (0: the unrigged parts), per slot: the primitive's own vertices
        std::map<uint16_t, tinygltf::Mesh> riggedMeshes;
        for (size_t s = 0; s < mesh->slots.size(); s++) {
            const titleart::TitleMeshSlot& slot = mesh->slots[s];
            if (!slot.indexCount) continue;
            if (!options.includeBands && (slot.role == titleart::TitleSlotRole::BandFront || slot.role == titleart::TitleSlotRole::BandSide)) continue;
            const uint8_t mi = materials.slotMaterial[s];
            const titleart::TitleMaterial3D& m = materials.materials[mi];
            if (materialIndex[mi] < 0) materialIndex[mi] = writeMaterial(w, m, size, units, mesh->sourceHash, used, result.warnings);
            std::map<uint16_t, std::pair<std::map<uint32_t, uint32_t>, std::vector<uint32_t>>> perRig;
            for (uint32_t i = 0; i < slot.indexCount; i++) {
                const uint32_t vi = mesh->index(slot.firstIndex + i);
                const uint16_t rig = mesh->parts[std::min<size_t>(mesh->vertices[vi].ids[2], mesh->parts.size() - 1)].rig;
                auto& [remap, indices] = perRig[rig];
                auto it = remap.find(vi);
                if (it == remap.end()) it = remap.emplace(vi, (uint32_t)remap.size()).first;
                indices.push_back(it->second);
            }
            for (auto& [rig, data] : perRig) {
                auto& [remap, indices] = data;
                std::vector<float> positions(remap.size() * 3), normals(remap.size() * 3), uv0(remap.size() * 2), uv1(remap.size() * 2);
                const titleart::TitleMeshRig* rigInfo = rig ? &mesh->rigs[rig - 1] : nullptr;
                for (const auto& [vi, local] : remap) {
                    const titleart::TitleMeshVertex& v = mesh->vertices[vi];
                    const auto n = titleart::decodeTitleNormal(v.normal[0], v.normal[1]);
                    // a rigged glyph's vertices around its pivot (its node carries the rig)
                    const float ox = rigInfo ? rigInfo->pivot[0] : 0.0f, oy = rigInfo ? rigInfo->pivot[1] : 0.0f, oz = rigInfo ? rigInfo->pivot[2] : 0.0f;
                    for (int c = 0; c < 3; c++) {
                        positions[local * 3 + c] = (float)((v.position[c] - (c == 0 ? ox : c == 1 ? oy : oz)) * units);
                        normals[local * 3 + c] = n[(size_t)c];
                    }
                    uv0[local * 2] = m.gradient ? rampT(m, *mesh, v) : 0.5f;
                    uv0[local * 2 + 1] = 0.5f;
                    if (!mesh->wrap.empty()) {
                        uv1[local * 2] = halfToFloat(mesh->wrap[vi * 2]);
                        uv1[local * 2 + 1] = halfToFloat(mesh->wrap[vi * 2 + 1]);
                    }
                }
                tinygltf::Primitive p;
                p.mode = TINYGLTF_MODE_TRIANGLES;
                p.material = materialIndex[mi];
                p.attributes["POSITION"] = w.accessor(positions, 3, TINYGLTF_TYPE_VEC3, true);
                p.attributes["NORMAL"] = w.accessor(normals, 3, TINYGLTF_TYPE_VEC3, false);
                p.attributes["TEXCOORD_0"] = w.accessor(uv0, 2, TINYGLTF_TYPE_VEC2, false);
                if (!mesh->wrap.empty()) p.attributes["TEXCOORD_1"] = w.accessor(uv1, 2, TINYGLTF_TYPE_VEC2, false);
                p.indices = w.indexAccessor(indices);
                result.triangles += (uint32_t)(indices.size() / 3);
                (rig ? riggedMeshes[rig] : gmesh).primitives.push_back(p);
            }
        }
        if (!gmesh.primitives.empty()) {
            w.model.meshes.push_back(gmesh);
            node.mesh = (int)w.model.meshes.size() - 1;
        }
        const int nodeIndex = (int)w.model.nodes.size();
        w.model.nodes.push_back(node);
        w.model.scenes[0].nodes.push_back(nodeIndex);
        // rigged glyphs: children whose matrix is the rig about its pivot
        const Json& chars = layer.contains("chars") ? layer["chars"] : Json::object();
        for (auto& [rig, rmesh] : riggedMeshes) {
            const titleart::TitleMeshRig& info = mesh->rigs[rig - 1];
            const std::string key = std::to_string(info.charIndex);
            const titleart::CharRig3D adjust = titleart::decodeCharRig(info.charIndex, chars.contains(key) ? chars[key] : Json::object());
            const titleart::Mat4 m = titleart::title3DRigMatrix(adjust, {info.pivot[0], info.pivot[1], info.pivot[2]}, size) *
                                     titleart::Mat4::translate(info.pivot[0], info.pivot[1], info.pivot[2]);
            rmesh.name = node.name + " char " + key;
            w.model.meshes.push_back(rmesh);
            tinygltf::Node child;
            child.name = rmesh.name;
            child.mesh = (int)w.model.meshes.size() - 1;
            child.matrix = matrixOf(m, units);
            w.model.nodes.push_back(child);
            w.model.nodes[(size_t)nodeIndex].children.push_back((int)w.model.nodes.size() - 1);
        }
        result.layers++;
        return true;
    });
    if (!result.layers) {
        result.error = "no_3d_layer: the title has no Dimension 3D layer to export";
        return result;
    }
    w.model.extensionsUsed.assign(used.begin(), used.end());
    w.model.buffers[0].name = "title";
    tinygltf::TinyGLTF writer;
    std::ostringstream stream;
    if (!writer.WriteGltfSceneToStream(&w.model, stream, false, true)) {
        result.error = "the GLB could not be written";
        return result;
    }
    const std::string bytes = stream.str();
    result.bytes.assign(bytes.begin(), bytes.end());
    result.ok = true;
    return result;
}
