// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The model cook (spec S53): the node invariant for every setting combination (AC5), the
// lossless levels decoding bit for bit and Optimize only reordering (AC6), the lossy levels
// within their documented error (AC7), Index Format, Normals, the blended triangle order, the
// baked levels of contract C1 (built by MeshLodPool::buildLevels, C2), the KHR_meshopt_compression
// layout read back with meshoptimizer's own decoders, the cache key and the files the cook keeps
// as they are. Decoding goes through meshoptimizer here: the engine's loader learns the format
// in its own stream.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/ModelFixtures.h"
#include "support/TextureCorpus.h"

#include "model/GltfDocument.h"
#include "model/ModelCooker.h"
#include "model/ModelSourceDecode.h"
#include "pool/MeshLodPool.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;
using GJson = GltfDocument::Json;

namespace {

    struct Cooked {
        std::vector<uint8_t> glb;
        ModelCookRecord record;
        GltfDocument doc;
    };

    fs::path writeSource(const ScratchDir& scratch, const std::vector<uint8_t>& bytes, const std::string& name = "model.glb"){
        const fs::path file = scratch.path / name;
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        return file;
    }

    ResolvedModelSettings settingsOf(OptimizeMesh optimize, MeshCompression compression, GenerateLods lods = GenerateLods::Off){
        ResolvedModelSettings settings;
        settings.optimizeMesh = optimize;
        settings.meshCompression = compression;
        settings.generateLods = lods;
        return settings;
    }

    ::testing::AssertionResult cookModel(const fs::path& source, const ResolvedModelSettings& settings, Cooked& out,
                                    ModelFamily family = ModelFamily::Desktop){
        ModelCookRequest request;
        request.source = source;
        request.assetPath = source.filename().generic_string();
        request.settings = settings;
        request.family = family;
        std::string error;
        if (!ModelCooker::cookToMemory(request, out.glb, out.record, error))
            return ::testing::AssertionFailure() << "cook: " << error;
        if (!out.doc.loadMemory(out.glb, source.parent_path(), "cooked.glb", error))
            return ::testing::AssertionFailure() << "the cooked file does not read back: " << error;
        return ::testing::AssertionSuccess();
    }

    // what the order invariant compares, as text: node names and their mesh, skin and children,
    // each mesh's name and its primitives' materials, materials, skins, animations
    // each triangle turned to start at its smallest index, its winding kept
    std::vector<uint32_t> cornersTurned(std::vector<uint32_t> list){
        for (size_t t = 0; t + 2 < list.size(); t += 3){
            const uint32_t* c = list.data() + t;
            const size_t first = (size_t)(std::min_element(c, c + 3) - c);
            const uint32_t turned[3] = {c[first], c[(first + 1) % 3], c[(first + 2) % 3]};
            std::copy(turned, turned + 3, list.begin() + (std::ptrdiff_t)t);
        }
        return list;
    }

    std::string structure(const GJson& root){
        std::string out;
        for (const GJson& node : root.value("nodes", GJson::array()))
            out += node.value("name", "") + ":" + node.value("mesh", GJson(-1)).dump() + ":" + node.value("skin", GJson(-1)).dump() + ":" +
                   node.value("children", GJson::array()).dump() + "|";
        for (const GJson& mesh : root.value("meshes", GJson::array())){
            out += mesh.value("name", "") + "[";
            for (const GJson& primitive : mesh["primitives"])
                out += primitive.value("material", GJson(-1)).dump() + ";" + std::to_string(primitive.value("targets", GJson::array()).size()) + ",";
            out += "]";
        }
        for (const GJson& material : root.value("materials", GJson::array()))
            out += material.value("name", "") + "/" + material.value("alphaMode", "OPAQUE") + "|";
        for (const GJson& animation : root.value("animations", GJson::array()))
            out += animation.value("name", "") + ":" + std::to_string(animation["channels"].size()) + "|";
        out += std::to_string(root.value("skins", GJson::array()).size()) + " skins";
        return out;
    }

    // the accessor of a primitive's attribute
    size_t attribute(const GltfDocument& doc, size_t mesh, size_t primitive, const char* semantic){
        return doc.json()["meshes"][mesh]["primitives"][primitive]["attributes"][semantic].get<size_t>();
    }

    std::vector<uint32_t> indicesOf(const GltfDocument& doc, size_t mesh, size_t primitive){
        std::vector<uint32_t> out;
        std::string error;
        const GJson& json = doc.json()["meshes"][mesh]["primitives"][primitive];
        EXPECT_TRUE(doc.readIndices(json["indices"].get<size_t>(), out, error)) << error;
        return out;
    }

    // a primitive's triangles as the bytes of their corners over every attribute (the corner
    // order rotated to start at the smallest), in draw order: two vertex orders of one mesh compare equal
    std::vector<std::string> triangles(const GltfDocument& doc, size_t mesh, size_t primitive){
        const GJson& json = doc.json()["meshes"][mesh]["primitives"][primitive];
        std::vector<std::vector<uint8_t>> streams;
        std::vector<size_t> sizes;
        for (auto it = json["attributes"].begin(); it != json["attributes"].end(); ++it){
            std::vector<uint8_t> data;
            GltfAccessorLayout layout;
            std::string error;
            EXPECT_TRUE(doc.readAccessorRaw(it->get<size_t>(), data, layout, error)) << error;
            streams.push_back(std::move(data));
            sizes.push_back(layout.elementSize);
        }
        auto corner = [&](uint32_t v){
            std::string bytes;
            for (size_t s = 0; s < streams.size(); s++)
                bytes.append(reinterpret_cast<const char*>(streams[s].data() + v * sizes[s]), sizes[s]);
            return bytes;
        };
        const std::vector<uint32_t> list = indicesOf(doc, mesh, primitive);
        std::vector<std::string> out;
        for (size_t t = 0; t + 2 < list.size(); t += 3){
            std::string c[3] = {corner(list[t]), corner(list[t + 1]), corner(list[t + 2])};
            const int first = (int)(std::min_element(c, c + 3) - c);
            out.push_back(c[first] + c[(first + 1) % 3] + c[(first + 2) % 3]);
        }
        return out;
    }

    std::multiset<std::string> asSet(const std::vector<std::string>& list){ return {list.begin(), list.end()}; }

    std::vector<float> floats(const GltfDocument& doc, size_t accessor, int& components){
        std::vector<float> out;
        std::string error;
        EXPECT_TRUE(doc.readAccessorFloats(accessor, out, components, error)) << error;
        return out;
    }

}

TEST(ModelCook, KeepsNodesMeshesPrimitivesMaterialsAndAnimationsForEverySetting){
    ScratchDir scratch("model-cook-structure");
    const fs::path source = writeSource(scratch, makeTestModel(12, false));
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    const std::string expected = structure(original.json());
    int combinations = 0;
    for (OptimizeMesh optimize : {OptimizeMesh::Everything, OptimizeMesh::PolygonOrder, OptimizeMesh::VertexOrder, OptimizeMesh::Nothing})
    for (MeshCompression compression : {MeshCompression::Off, MeshCompression::Lossless, MeshCompression::Low, MeshCompression::Medium, MeshCompression::High})
    for (bool weld : {true, false})
    for (IndexFormat index : {IndexFormat::Auto, IndexFormat::Bits32})
    for (ModelNormals normals : {ModelNormals::Import, ModelNormals::Calculate})
    for (GenerateLods lods : {GenerateLods::Automatic, GenerateLods::On, GenerateLods::Off}){
        ResolvedModelSettings settings = settingsOf(optimize, compression, lods);
        settings.weldVertices = weld;
        settings.indexFormat = index;
        settings.normals = normals;
        settings.lodReduction = LodReduction::Smallest;
        Cooked cooked;
        ASSERT_TRUE(cookModel(source, settings, cooked)) << settings.canonical();
        const std::string why = settings.canonical();
        EXPECT_EQ(structure(cooked.doc.json()), expected) << why;
        // the same triangles in every primitive (LOD 0 is never reduced), the extras kept
        for (size_t m = 0; m < 2; m++){
            for (size_t p = 0; p < original.json()["meshes"][m]["primitives"].size(); p++)
                EXPECT_EQ(indicesOf(cooked.doc, m, p).size(), indicesOf(original, m, p).size()) << why;
        }
        EXPECT_EQ(cooked.doc.json()["extras"]["kept"], "by the cook") << why;
        EXPECT_EQ(cooked.doc.json()["accessors"].size() >= original.json()["accessors"].size(), true) << "accessors keep their indices";
        combinations++;
    }
    EXPECT_EQ(combinations, 4 * 5 * 2 * 2 * 2 * 3);
}

TEST(ModelCook, LosslessWithoutOptimizeDecodesToTheSourceBytes){
    ScratchDir scratch("model-cook-lossless");
    const fs::path source = writeSource(scratch, makeTestModel(10, false));
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    Cooked cooked;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Nothing, MeshCompression::Lossless), cooked));
    // every accessor of the source: the same element bytes
    for (size_t a = 0; a < original.json()["accessors"].size(); a++){
        std::vector<uint8_t> before, after;
        GltfAccessorLayout layoutBefore, layoutAfter;
        ASSERT_TRUE(original.readAccessorRaw(a, before, layoutBefore, error)) << error;
        ASSERT_TRUE(cooked.doc.readAccessorRaw(a, after, layoutAfter, error)) << error;
        EXPECT_EQ(layoutBefore.componentType, layoutAfter.componentType) << "accessor " << a;
        if (layoutBefore.componentType == gltf::UNSIGNED_INT && layoutAfter.componentType == gltf::UNSIGNED_SHORT)
            continue;
        EXPECT_EQ(before, after) << "accessor " << a;
    }
    // and it is smaller: the codec stored the same values in less space
    EXPECT_LT(cooked.record.diskBytes, cooked.record.sourceBytes);
    EXPECT_EQ(cooked.record.visualChange(), "None");
}

TEST(ModelCook, OptimizeOnlyReordersAndNeverWorsensTheVertexCache){
    ScratchDir scratch("model-cook-optimize");
    const fs::path source = writeSource(scratch, makeTestModel(16, false));
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    for (OptimizeMesh optimize : {OptimizeMesh::Everything, OptimizeMesh::PolygonOrder, OptimizeMesh::VertexOrder}){
        Cooked cooked;
        ASSERT_TRUE(cookModel(source, settingsOf(optimize, MeshCompression::Lossless), cooked));
        for (size_t m = 0; m < 2; m++){
            for (size_t p = 0; p < original.json()["meshes"][m]["primitives"].size(); p++){
                // the same triangles of the same vertex values, bit for bit
                EXPECT_EQ(asSet(triangles(cooked.doc, m, p)), asSet(triangles(original, m, p))) << optimizeMeshName(optimize) << " " << m << "/" << p;
            }
        }
        EXPECT_LE(cooked.record.after.acmr, cooked.record.before.acmr + 1e-6) << optimizeMeshName(optimize);
        if (optimize == OptimizeMesh::Everything){
            // the unwelded grid: every vertex but the welded set was an exact duplicate
            EXPECT_LT(cooked.record.after.vertices, cooked.record.before.vertices);
            EXPECT_GT(cooked.record.duplicatePercent, 50.0);
            EXPECT_LT(cooked.record.after.acmr, cooked.record.before.acmr);
        }
    }
}

TEST(ModelCook, BlendedPrimitivesKeepTheirTriangleOrder){
    ScratchDir scratch("model-cook-blended");
    const fs::path source = writeSource(scratch, makeTestModel(10, false));
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    Cooked cooked;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless), cooked));
    // Body/1 is BLEND: welded and fetch-ordered, drawn in the source's order
    EXPECT_EQ(triangles(cooked.doc, 0, 1), triangles(original, 0, 1));
    EXPECT_NE(triangles(cooked.doc, 0, 0), triangles(original, 0, 0)) << "the opaque one was reordered";
    ASSERT_EQ(cooked.record.keptOrder.size(), 1u);
    EXPECT_EQ(cooked.record.keptOrder[0], "Body/1 (blended)");

    // a scene's blend override names a submesh of every mesh
    ModelCookRequest request;
    request.source = source;
    request.assetPath = "model.glb";
    request.settings = settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless);
    request.blended = {{-1, 0}};
    std::vector<uint8_t> glb;
    ModelCookRecord record;
    ASSERT_TRUE(ModelCooker::cookToMemory(request, glb, record, error)) << error;
    GltfDocument overridden;
    ASSERT_TRUE(overridden.loadMemory(glb, scratch.path, "cooked.glb", error)) << error;
    EXPECT_EQ(triangles(overridden, 0, 0), triangles(original, 0, 0));
    EXPECT_EQ(triangles(overridden, 1, 0), triangles(original, 1, 0));
}

TEST(ModelCook, LossyLevelsStayWithinTheirDocumentedError){
    ScratchDir scratch("model-cook-lossy");
    const fs::path source = writeSource(scratch, makeTestModel(16, true));
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;
    struct Level { MeshCompression compression; double normalDegrees; };
    // the documented bounds (docs/meshes.md): positions within max|component| * 2^(1 - bits)
    // per component, normals within these angles, uv within half a step of their bits
    for (const Level& level : {Level{MeshCompression::Low, 0.3}, Level{MeshCompression::Medium, 1.0}, Level{MeshCompression::High, 1.0}}){
        const ModelCooker::QuantizationBits bits = ModelCooker::quantizationBits(level.compression);
        Cooked cooked;
        ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Nothing, level.compression), cooked));
        const char* name = meshCompressionName(level.compression);
        int c0 = 0, c1 = 0;
        const std::vector<float> p0 = floats(original, attribute(original, 0, 0, "POSITION"), c0);
        const std::vector<float> p1 = floats(cooked.doc, attribute(cooked.doc, 0, 0, "POSITION"), c1);
        ASSERT_EQ(p0.size(), p1.size());
        for (int c = 0; c < 3; c++){
            float largest = 0.0f;
            for (size_t v = (size_t)c; v < p0.size(); v += 3) largest = std::max(largest, std::fabs(p0[v]));
            const double bound = largest * std::ldexp(1.0, 1 - bits.position);
            for (size_t v = (size_t)c; v < p0.size(); v += 3)
                ASSERT_LE(std::fabs(p0[v] - p1[v]), bound) << name << " component " << c;
        }
        // positions stay floats: CPU readers see FLOAT VEC3
        EXPECT_EQ(cooked.doc.json()["accessors"][attribute(cooked.doc, 0, 0, "POSITION")]["componentType"], gltf::FLOAT);
        const std::vector<float> n0 = floats(original, attribute(original, 0, 0, "NORMAL"), c0);
        const std::vector<float> n1 = floats(cooked.doc, attribute(cooked.doc, 0, 0, "NORMAL"), c1);
        double worstAngle = 0.0;
        for (size_t v = 0; v + 2 < n0.size(); v += 3){
            const double length = std::sqrt((double)n1[v] * n1[v] + (double)n1[v + 1] * n1[v + 1] + (double)n1[v + 2] * n1[v + 2]);
            const double cosine = (n0[v] * n1[v] + n0[v + 1] * n1[v + 1] + n0[v + 2] * n1[v + 2]) / length;
            worstAngle = std::max(worstAngle, std::acos(std::min(1.0, cosine)) * 180.0 / 3.14159265358979);
        }
        EXPECT_LE(worstAngle, level.normalDegrees) << name;
        const std::vector<float> t0 = floats(original, attribute(original, 0, 0, "TEXCOORD_0"), c0);
        const std::vector<float> t1 = floats(cooked.doc, attribute(cooked.doc, 0, 0, "TEXCOORD_0"), c1);
        const double uvBound = 0.5 / ((1 << bits.uv) - 1) + 1.0 / 65535.0;
        for (size_t i = 0; i < t0.size(); i++)
            ASSERT_LE(std::fabs(t0[i] - t1[i]), uvBound) << name;
        EXPECT_GT(cooked.record.positionError, 0.0);
        EXPECT_NE(cooked.record.visualChange(), "None");
        // normals in normalized integers need KHR_mesh_quantization
        const std::vector<std::string> required = cooked.doc.requiredExtensions();
        EXPECT_NE(std::find(required.begin(), required.end(), "KHR_mesh_quantization"), required.end()) << name;
        EXPECT_LT(cooked.record.diskBytes, cooked.record.sourceBytes) << name;
    }
}

TEST(ModelCook, WritesKhrMeshoptCompressionAsTheExtensionDefinesIt){
    ScratchDir scratch("model-cook-layout");
    const fs::path source = writeSource(scratch, makeTestModel(8, true));
    Cooked cooked;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::High), cooked));
    const GJson& root = cooked.doc.json();
    // a fallback buffer without data, the extension required
    ASSERT_EQ(root["buffers"].size(), 2u);
    EXPECT_FALSE(root["buffers"][1].contains("uri"));
    EXPECT_TRUE(root["buffers"][1]["extensions"]["KHR_meshopt_compression"]["fallback"].get<bool>());
    const std::vector<std::string> required = cooked.doc.requiredExtensions();
    EXPECT_NE(std::find(required.begin(), required.end(), "KHR_meshopt_compression"), required.end());
    // every compressed view decodes with meshoptimizer's own decoders to what the document reads
    const std::vector<uint8_t> bin = cooked.doc.getBuffers()[0];
    int compressed = 0;
    for (size_t v = 0; v < root["bufferViews"].size(); v++){
        const GJson& view = root["bufferViews"][v];
        if (!view.contains("extensions"))
            continue;
        const GJson& ext = view["extensions"]["KHR_meshopt_compression"];
        EXPECT_EQ(view["buffer"], 1);
        const size_t count = ext["count"], stride = ext["byteStride"], offset = ext.value("byteOffset", 0), length = ext["byteLength"];
        ASSERT_LE(offset + length, bin.size());
        std::vector<uint8_t> out(count * stride);
        const std::string mode = ext["mode"];
        int result = 0;
        if (mode == "ATTRIBUTES") result = meshopt_decodeVertexBuffer(out.data(), count, stride, bin.data() + offset, length);
        else if (mode == "TRIANGLES") result = meshopt_decodeIndexBuffer(out.data(), count, stride, bin.data() + offset, length);
        else result = meshopt_decodeIndexSequence(out.data(), count, stride, bin.data() + offset, length);
        ASSERT_EQ(result, 0) << "view " << v;
        const std::string filter = ext.value("filter", "NONE");
        if (filter == "OCTAHEDRAL") meshopt_decodeFilterOct(out.data(), count, stride);
        if (filter == "EXPONENTIAL") meshopt_decodeFilterExp(out.data(), count, stride);
        const uint8_t* data = nullptr;
        size_t size = 0, viewStride = 0;
        std::string error;
        ASSERT_TRUE(cooked.doc.viewData(v, data, size, viewStride, error)) << error;
        ASSERT_EQ(size, out.size());
        EXPECT_EQ(std::memcmp(data, out.data(), size), 0) << "view " << v;
        EXPECT_EQ(view["byteLength"].get<size_t>(), count * stride);
        if (view.contains("byteStride"))
            EXPECT_EQ(view["byteStride"].get<size_t>(), stride);
        compressed++;
    }
    EXPECT_GT(compressed, 4);

    // Off: no codec, nothing required of it
    Cooked plain;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Off), plain));
    for (const GJson& view : plain.doc.json()["bufferViews"])
        EXPECT_FALSE(view.contains("extensions"));
    EXPECT_EQ(plain.doc.json()["buffers"].size(), 1u);
    EXPECT_FALSE(plain.doc.json().contains("extensionsRequired"));
}

TEST(ModelCook, BakedLevelsAreTheRuntimesLevelsOverTheCookedData){
    ScratchDir scratch("model-cook-lods");
    const fs::path source = writeSource(scratch, makeTestModel(24, true));
    Cooked cooked;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless, GenerateLods::Automatic), cooked));
    const GJson& root = cooked.doc.json();
    const GJson& primitive = root["meshes"][0]["primitives"][0];
    ASSERT_TRUE(primitive.contains("extensions"));
    const GJson& lods = primitive["extensions"]["JE_mesh_lods"];
    EXPECT_EQ(lods["recipe"].get<int>(), kMeshLodRecipe);
    ASSERT_GE(lods["levels"].size(), 1u);
    // what the runtime would build from the cooked data (RenderSystem::readLodSource)
    MeshLodSource lodSource;
    int components = 0;
    lodSource.positions = floats(cooked.doc, attribute(cooked.doc, 0, 0, "POSITION"), components);
    const std::vector<float> normals = floats(cooked.doc, attribute(cooked.doc, 0, 0, "NORMAL"), components);
    const std::vector<float> uvs = floats(cooked.doc, attribute(cooked.doc, 0, 0, "TEXCOORD_0"), components);
    lodSource.indices = indicesOf(cooked.doc, 0, 0);
    lodSource.indexType = root["accessors"][primitive["indices"].get<size_t>()]["componentType"] == gltf::UNSIGNED_INT
                              ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT;
    lodSource.attributeCount = 5;
    for (size_t v = 0; v < lodSource.positions.size() / 3; v++)
        lodSource.attributes.insert(lodSource.attributes.end(), {normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2], uvs[v * 2], uvs[v * 2 + 1]});
    const std::shared_ptr<MeshLodData> runtime = MeshLodPool::buildLevels(lodSource, MeshLodRecipe::runtime());
    ASSERT_EQ((size_t)runtime->numLevels, lods["levels"].size() + 1);
    size_t previous = lodSource.indices.size();
    const size_t vertices = lodSource.positions.size() / 3;
    const size_t indexSize = lodSource.indexType == AttributeDataType::UNSIGNED_INT ? 4 : 2;
    for (size_t l = 0; l < lods["levels"].size(); l++){
        std::vector<uint32_t> baked;
        std::string error;
        ASSERT_TRUE(cooked.doc.readIndices(lods["levels"][l]["indices"].get<size_t>(), baked, error)) << error;
        const MeshLodLevel& level = runtime->levels[l + 1];
        ASSERT_EQ(baked.size(), (size_t)level.indexCount);
        std::vector<uint32_t> built(baked.size());
        for (size_t i = 0; i < baked.size(); i++){
            std::memcpy(&built[i], runtime->indices.data() + ((size_t)level.indexBase + i) * indexSize, indexSize);
            ASSERT_LT(baked[i], vertices);
        }
        // the same triangles in the same order; the index codec may turn a triangle's corners
        ASSERT_EQ(cornersTurned(baked), cornersTurned(built)) << "level " << l + 1;
        EXPECT_FLOAT_EQ(lods["levels"][l]["error"].get<float>(), level.error);
        // coarser after LOD 0, each level
        EXPECT_LT(baked.size(), previous);
        previous = baked.size();
    }
    ASSERT_EQ(cooked.record.lods.size(), lods["levels"].size());
    for (const ModelLodLevelStats& level : cooked.record.lods){
        EXPECT_GT(level.deviation, 0.0);
        EXPECT_LT(level.deviation, 0.5);
        EXPECT_LT(level.share, 1.0);
    }
    const std::vector<std::string> used = cooked.doc.usedExtensions();
    EXPECT_NE(std::find(used.begin(), used.end(), "JE_mesh_lods"), used.end());
    const std::vector<std::string> required = cooked.doc.requiredExtensions();
    EXPECT_EQ(std::find(required.begin(), required.end(), "JE_mesh_lods"), required.end()) << "a reader without it draws LOD 0";

    // On with Smallest: fewer triangles per level; Off: no levels
    ResolvedModelSettings smallest = settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless, GenerateLods::On);
    smallest.lodReduction = LodReduction::Smallest;
    smallest.lodCount = 2;
    Cooked small;
    ASSERT_TRUE(cookModel(source, smallest, small));
    ASSERT_EQ(small.record.lods.size(), 2u);
    EXPECT_LT(small.record.lods[0].triangles, cooked.record.lods[0].triangles);
    Cooked none;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless, GenerateLods::Off), none));
    EXPECT_FALSE(none.doc.json()["meshes"][0]["primitives"][0].contains("extensions"));
    EXPECT_TRUE(none.record.lods.empty());
}

TEST(ModelCook, IndexFormatAutoPicks16BitsAndRefuses16PastItsRange){
    ScratchDir scratch("model-cook-index");
    // 182 quads a side: 33,489 welded vertices; unwelded, 198,744
    GltfBuild build;
    GridOptions grid;
    grid.quads = 182;
    grid.welded = false;
    grid.shuffled = false;
    // the primitive first: adding its accessors may move the JSON that holds "meshes"
    const GJson primitive = addGridPrimitive(build, grid);
    build.json["meshes"].push_back({{"name", "Big"}, {"primitives", GJson::array({primitive})}});
    build.json["nodes"] = GJson::array({{{"mesh", 0}}});
    const fs::path source = writeSource(scratch, build.glb());
    ResolvedModelSettings settings = settingsOf(OptimizeMesh::Nothing, MeshCompression::Off);
    settings.indexFormat = IndexFormat::Bits16;
    Cooked cooked;
    const ::testing::AssertionResult refused = cookModel(source, settings, cooked);
    ASSERT_FALSE(refused);
    EXPECT_NE(std::string(refused.message()).find("16-bit"), std::string::npos) << refused.message();
    // welded, the mesh fits 16 bits: Auto takes them
    settings = settingsOf(OptimizeMesh::Everything, MeshCompression::Off);
    ASSERT_TRUE(cookModel(source, settings, cooked));
    const GJson& root = cooked.doc.json();
    EXPECT_EQ(root["accessors"][root["meshes"][0]["primitives"][0]["indices"].get<size_t>()]["componentType"], gltf::UNSIGNED_SHORT);
    settings.indexFormat = IndexFormat::Bits32;
    ASSERT_TRUE(cookModel(source, settings, cooked));
    const GJson& wide = cooked.doc.json();
    EXPECT_EQ(wide["accessors"][wide["meshes"][0]["primitives"][0]["indices"].get<size_t>()]["componentType"], gltf::UNSIGNED_INT);
}

TEST(ModelCook, CalculatedNormalsCreaseAtTheSmoothingAngle){
    ScratchDir scratch("model-cook-normals");
    // a cube of 8 shared corners without normals
    GltfBuild build;
    const std::vector<float> corners = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    const std::vector<uint32_t> faces = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    GJson primitive = {{"attributes", {{"POSITION", build.addFloats(corners, 3, gltf::ARRAY_BUFFER, true)}}}, {"indices", build.addIndices(faces, gltf::UNSIGNED_SHORT)}};
    build.json["meshes"].push_back({{"name", "Cube"}, {"primitives", {primitive}}});
    build.json["nodes"] = GJson::array({{{"mesh", 0}}});
    const fs::path source = writeSource(scratch, build.glb());
    ResolvedModelSettings settings = settingsOf(OptimizeMesh::Everything, MeshCompression::Lossless);
    settings.normals = ModelNormals::Calculate;
    settings.smoothingAngle = 60.0f;
    Cooked cooked;
    ASSERT_TRUE(cookModel(source, settings, cooked));
    int components = 0;
    const std::vector<float> normals = floats(cooked.doc, attribute(cooked.doc, 0, 0, "NORMAL"), components);
    ASSERT_EQ(components, 3);
    // hard edges: four corners per face, every normal along an axis
    EXPECT_EQ(normals.size() / 3, 24u);
    for (size_t v = 0; v < normals.size(); v += 3){
        const float largest = std::max({std::fabs(normals[v]), std::fabs(normals[v + 1]), std::fabs(normals[v + 2])});
        EXPECT_NEAR(largest, 1.0f, 1e-4f);
    }
    // past the cube's 90 degrees, one smooth normal per corner
    settings.smoothingAngle = 180.0f;
    ASSERT_TRUE(cookModel(source, settings, cooked));
    EXPECT_EQ(floats(cooked.doc, attribute(cooked.doc, 0, 0, "NORMAL"), components).size() / 3, 8u);
}

TEST(ModelCook, CooksAreDeterministicAndCachedByContent){
    ScratchDir scratch("model-cook-cache");
    const fs::path source = writeSource(scratch, makeTestModel(10, false));
    Cooked a, b;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Medium, GenerateLods::Automatic), a));
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Medium, GenerateLods::Automatic), b));
    EXPECT_EQ(a.glb, b.glb);

    ModelCookCache cache;
    cache.open(scratch.path / "cache", 1ull << 30);
    ModelCookRequest request;
    request.source = source;
    request.assetPath = "model.glb";
    const ModelCookResult first = ModelCooker::cook(cache, request, nullptr);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.cacheHit);
    const ModelCookResult second = ModelCooker::cook(cache, request, nullptr);
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_TRUE(second.cacheHit);
    EXPECT_EQ(first.key, second.key);
    EXPECT_EQ(second.record.diskBytes, first.record.diskBytes);
    // a renamed copy: the same artifact
    const fs::path copy = scratch.path / "renamed.glb";
    fs::copy_file(source, copy);
    ModelCookRequest renamed = request;
    renamed.source = copy;
    std::string key, error;
    ASSERT_TRUE(ModelCooker::planKey(cache, renamed, key, error)) << error;
    EXPECT_EQ(key, first.key);
    // another family, other settings, other blended submeshes: other keys
    ModelCookRequest mobile = request;
    mobile.family = ModelFamily::Mobile;
    ASSERT_TRUE(ModelCooker::planKey(cache, mobile, key, error));
    EXPECT_NE(key, first.key);
    ModelCookRequest high = request;
    high.settings.meshCompression = MeshCompression::High;
    ASSERT_TRUE(ModelCooker::planKey(cache, high, key, error));
    EXPECT_NE(key, first.key);
    ModelCookRequest blended = request;
    blended.blended = {{0, 0}};
    ASSERT_TRUE(ModelCooker::planKey(cache, blended, key, error));
    EXPECT_NE(key, first.key);
    cache.close();
}

TEST(ModelCook, AFileItCannotRewriteShipsAsItIsAndAnUnreadExtensionFails){
    ScratchDir scratch("model-cook-foreign");
    std::vector<uint8_t> bytes = makeTestModel(6, true);
    GltfDocument doc;
    std::string error;
    ASSERT_TRUE(doc.loadMemory(bytes, scratch.path, "model.glb", error)) << error;
    GJson root = doc.json();
    root["meshes"][0]["primitives"][0]["extensions"] = {{"VENDOR_blob", {{"bufferView", 0}}}};
    const fs::path foreign = writeSource(scratch, GltfDocument::writeGlb(root, doc.getBuffers()[0]), "foreign.glb");
    Cooked cooked;
    ASSERT_TRUE(cookModel(foreign, ResolvedModelSettings(), cooked));
    EXPECT_TRUE(cooked.record.passThrough);
    std::ifstream in(foreign, std::ios::binary);
    const std::vector<uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(cooked.glb, original);
    ASSERT_FALSE(cooked.record.warnings.empty());
    EXPECT_NE(cooked.record.warnings[0].find("VENDOR_blob"), std::string::npos) << cooked.record.warnings[0];

    // a required extension the engine does not read: the cook fails with the loader's Error,
    // and its record keeps the finding (R5)
    root = doc.json();
    root["extensionsRequired"] = GJson::array({"VENDOR_required_test"});
    root["extensionsUsed"] = GJson::array({"VENDOR_required_test"});
    const fs::path unread = writeSource(scratch, GltfDocument::writeGlb(root, doc.getBuffers()[0]), "unread.glb");
    const ::testing::AssertionResult refused = cookModel(unread, ResolvedModelSettings(), cooked);
    ASSERT_FALSE(refused);
    EXPECT_NE(std::string(refused.message()).find("VENDOR_required_test"), std::string::npos) << refused.message();
    const std::string blocking = ModelSourceDecode::blockingError(cooked.record.diagnostics);
    EXPECT_NE(blocking.find("VENDOR_required_test"), std::string::npos) << blocking;

    // Draco listed but used by no primitive: dropped, a player has nothing to decode
    root = doc.json();
    root["extensionsRequired"] = GJson::array({"KHR_draco_mesh_compression"});
    root["extensionsUsed"] = GJson::array({"KHR_draco_mesh_compression"});
    const fs::path listed = writeSource(scratch, GltfDocument::writeGlb(root, doc.getBuffers()[0]), "listed.glb");
    ASSERT_TRUE(cookModel(listed, ResolvedModelSettings(), cooked));
    const std::vector<std::string> left = cooked.doc.requiredExtensions();
    EXPECT_EQ(std::find(left.begin(), left.end(), "KHR_draco_mesh_compression"), left.end());
    EXPECT_TRUE(ModelSourceDecode::blockingError(cooked.record.diagnostics).empty());
}

TEST(ModelCook, ReadsMeshoptCompressedSourcesAndRejectsCorruptStreams){
    ScratchDir scratch("model-cook-meshopt-source");
    const fs::path source = writeSource(scratch, makeTestModel(8, true));
    Cooked first;
    ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, MeshCompression::Medium), first));
    // the cook of a cook (gltfpack's files take the same path): the same triangles again
    const fs::path cookedFile = writeSource(scratch, first.glb, "cooked.glb");
    Cooked second;
    ASSERT_TRUE(cookModel(cookedFile, settingsOf(OptimizeMesh::Nothing, MeshCompression::Lossless), second));
    EXPECT_EQ(asSet(triangles(second.doc, 0, 0)), asSet(triangles(first.doc, 0, 0)));
    // a truncated stream: an error naming the bufferView, never the decoder's assert
    GJson root = first.doc.json();
    for (GJson& view : root["bufferViews"]){
        if (!view.contains("extensions")) continue;
        GJson& ext = view["extensions"]["KHR_meshopt_compression"];
        ext["byteLength"] = std::max<size_t>(1, ext["byteLength"].get<size_t>() / 3);
        break;
    }
    const fs::path broken = writeSource(scratch, GltfDocument::writeGlb(root, first.doc.getBuffers()[0]), "broken.glb");
    GltfDocument doc;
    std::string error;
    EXPECT_FALSE(doc.load(broken, error));
    EXPECT_NE(error.find("bufferView"), std::string::npos) << error;
    root = first.doc.json();
    for (GJson& view : root["bufferViews"]){
        if (!view.contains("extensions")) continue;
        view["extensions"]["KHR_meshopt_compression"]["byteStride"] = 300;
        break;
    }
    const fs::path stride = writeSource(scratch, GltfDocument::writeGlb(root, first.doc.getBuffers()[0]), "stride.glb");
    EXPECT_FALSE(doc.load(stride, error));
    EXPECT_NE(error.find("byteStride 300"), std::string::npos) << error;
}

TEST(ModelCookBench, HundredThousandTrianglesUnderTwoSeconds){
    // spec S53 R28: a 100k-triangle model cooks in under 2 s on one build-server core (the
    // budget holds on the benchmark run, JASPER_TEST_PERF_SLACK; elsewhere it is printed)
    ScratchDir scratch("model-cook-bench");
    GltfBuild build;
    GridOptions grid;
    grid.quads = 224;      // 100,352 triangles
    grid.welded = false;
    // the primitive first: adding its accessors may move the JSON that holds "meshes"
    const GJson primitive = addGridPrimitive(build, grid);
    build.json["meshes"].push_back({{"name", "Terrain"}, {"primitives", GJson::array({primitive})}});
    build.json["nodes"] = GJson::array({{{"mesh", 0}}});
    const fs::path source = writeSource(scratch, build.glb());
    for (MeshCompression compression : {MeshCompression::Lossless, MeshCompression::Medium}){
        const auto start = std::chrono::steady_clock::now();
        Cooked cooked;
        ASSERT_TRUE(cookModel(source, settingsOf(OptimizeMesh::Everything, compression, GenerateLods::Automatic), cooked));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_EQ(cooked.record.after.triangles, 100352);
        std::printf("[bench] cook of 100,352 triangles (%s, LODs) %.3f s (cook %.0f ms; budget 2 s); %.2f MB -> %.2f MB\n",
                    meshCompressionName(compression), seconds, cooked.record.cookMs, cooked.record.sourceBytes / 1048576.0,
                    cooked.record.diskBytes / 1048576.0);
        if (perfSlack() > 0.0)
            EXPECT_LE(cooked.record.cookMs / 1000.0, 2.0 * perfSlack());
    }
}
