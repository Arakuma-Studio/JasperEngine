// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The detail level builder (spec S54 R2, R3, AC3): MeshLodPool::buildLevels, the pure function the
// runtime's builds and the model cook share, and the baked levels a cooked glb carries
// (JE_mesh_lods, contract C1).

#include "pool/MeshLodPool.h"

#include "tiny_gltf.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    constexpr float PI = 3.14159265358979f;

    // a UV sphere of radius 1; flat: every triangle has its own vertices and face normal (the
    // hard-edged meshes whose normals made the old error grow far past the real deviation)
    MeshLodSource sphere(unsigned int slices, unsigned int stacks, bool flat, bool normals = true, bool uvs = true){
        std::vector<float> positions;
        std::vector<float> uv;
        for (unsigned int y = 0; y <= stacks; y++){
            const float v = (float)y / stacks;
            const float phi = v * PI;
            for (unsigned int x = 0; x <= slices; x++){
                const float u = (float)x / slices;
                const float theta = u * 2.0f * PI;
                positions.insert(positions.end(), {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)});
                uv.insert(uv.end(), {u, v});
            }
        }
        std::vector<uint32_t> indices;
        for (unsigned int y = 0; y < stacks; y++){
            for (unsigned int x = 0; x < slices; x++){
                const uint32_t a = y * (slices + 1) + x, b = a + slices + 1;
                if (y > 0) indices.insert(indices.end(), {a, b, a + 1});
                if (y + 1 < stacks) indices.insert(indices.end(), {a + 1, b, b + 1});
            }
        }

        MeshLodSource source;
        source.indexType = AttributeDataType::UNSIGNED_INT;
        if (!flat){
            source.positions = positions;
            source.indices = indices;
            if (normals) source.normals = positions;
            if (uvs) source.uvs = uv;
            return source;
        }
        for (size_t t = 0; t < indices.size(); t += 3){
            const float* p[3] = {&positions[indices[t] * 3], &positions[indices[t + 1] * 3], &positions[indices[t + 2] * 3]};
            const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float length = std::max(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-12f);
            for (float& c : n) c /= length;
            for (int k = 0; k < 3; k++){
                source.indices.push_back((uint32_t)(source.positions.size() / 3));
                source.positions.insert(source.positions.end(), p[k], p[k] + 3);
                if (normals) source.normals.insert(source.normals.end(), n, n + 3);
                if (uvs) source.uvs.insert(source.uvs.end(), &uv[indices[t + k] * 2], &uv[indices[t + k] * 2] + 2);
            }
        }
        return source;
    }

    std::vector<uint32_t> levelIndices(const MeshLodData& data, unsigned int level){
        std::vector<uint32_t> out;
        const MeshLodLevel& lod = data.levels[level];
        for (unsigned int i = 0; i < lod.indexCount; i++){
            uint32_t value;
            if (data.indexType == AttributeDataType::UNSIGNED_INT){
                std::memcpy(&value, data.indices.data() + (size_t)(lod.indexBase + i) * 4, 4);
            }else{
                uint16_t v16;
                std::memcpy(&v16, data.indices.data() + (size_t)(lod.indexBase + i) * 2, 2);
                value = v16;
            }
            out.push_back(value);
        }
        return out;
    }

    // exact distance from a point to a triangle soup, by brute force
    double nearest(const float* p, const std::vector<float>& positions, const std::vector<uint32_t>& indices){
        auto sub = [](const double* a, const double* b, double* o){ for (int i = 0; i < 3; i++) o[i] = a[i] - b[i]; };
        auto dot = [](const double* a, const double* b){ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const double q[3] = {p[0], p[1], p[2]};
        double best = 1e30;
        for (size_t t = 0; t + 2 < indices.size(); t += 3){
            double a[3], b[3], c[3];
            for (int i = 0; i < 3; i++){
                a[i] = positions[indices[t] * 3 + i];
                b[i] = positions[indices[t + 1] * 3 + i];
                c[i] = positions[indices[t + 2] * 3 + i];
            }
            // closest point on the triangle by projecting onto its plane, else onto its edges
            double ab[3], ac[3], aq[3], n[3];
            sub(b, a, ab); sub(c, a, ac); sub(q, a, aq);
            n[0] = ab[1] * ac[2] - ab[2] * ac[1]; n[1] = ab[2] * ac[0] - ab[0] * ac[2]; n[2] = ab[0] * ac[1] - ab[1] * ac[0];
            const double nn = dot(n, n);
            double d2 = 1e30;
            if (nn > 0.0){
                const double s = dot(aq, n) / nn;
                const double proj[3] = {q[0] - n[0] * s, q[1] - n[1] * s, q[2] - n[2] * s};
                double ap[3], bp[3], cp[3], bc[3], ca[3], ab2[3];
                sub(proj, a, ap); sub(proj, b, bp); sub(proj, c, cp); sub(c, b, bc); sub(a, c, ca); sub(b, a, ab2);
                auto cross = [](const double* x, const double* y, double* o){ o[0] = x[1] * y[2] - x[2] * y[1]; o[1] = x[2] * y[0] - x[0] * y[2]; o[2] = x[0] * y[1] - x[1] * y[0]; };
                double c1[3], c2[3], c3[3];
                cross(ab2, ap, c1); cross(bc, bp, c2); cross(ca, cp, c3);
                if (dot(c1, n) >= 0.0 && dot(c2, n) >= 0.0 && dot(c3, n) >= 0.0)
                    d2 = s * s * nn;
            }
            const double* corners[3] = {a, b, c};
            for (int e = 0; e < 3; e++){
                const double* u = corners[e];
                const double* v = corners[(e + 1) % 3];
                double uv[3], uq[3];
                sub(v, u, uv); sub(q, u, uq);
                const double len = dot(uv, uv);
                const double tt = len > 0.0 ? std::clamp(dot(uq, uv) / len, 0.0, 1.0) : 0.0;
                const double r[3] = {uq[0] - uv[0] * tt, uq[1] - uv[1] * tt, uq[2] - uv[2] * tt};
                d2 = std::min(d2, dot(r, r));
            }
            best = std::min(best, d2);
        }
        return std::sqrt(best);
    }

    // the largest distance between the two surfaces, densely sampled (barycentric grids of both)
    double sampledDeviation(const std::vector<float>& positions, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b){
        double worst = 0.0;
        auto sample = [&](const std::vector<uint32_t>& from, const std::vector<uint32_t>& to){
            const int n = 4;
            for (size_t t = 0; t + 2 < from.size(); t += 3){
                for (int i = 0; i <= n; i++){
                    for (int j = 0; j <= n - i; j++){
                        const float u = (float)i / n, v = (float)j / n, w = 1.0f - u - v;
                        float p[3];
                        for (int k = 0; k < 3; k++)
                            p[k] = positions[from[t] * 3 + k] * w + positions[from[t + 1] * 3 + k] * u + positions[from[t + 2] * 3 + k] * v;
                        worst = std::max(worst, nearest(p, positions, to));
                    }
                }
                const float centroid[3] = {
                    (positions[from[t] * 3] + positions[from[t + 1] * 3] + positions[from[t + 2] * 3]) / 3.0f,
                    (positions[from[t] * 3 + 1] + positions[from[t + 1] * 3 + 1] + positions[from[t + 2] * 3 + 1]) / 3.0f,
                    (positions[from[t] * 3 + 2] + positions[from[t + 1] * 3 + 2] + positions[from[t + 2] * 3 + 2]) / 3.0f};
                worst = std::max(worst, nearest(centroid, positions, to));
            }
        };
        sample(a, b);
        sample(b, a);
        return worst;
    }

}

// R2 bug 2: a UV-only mesh weighed its UVs with the normals' weights (the weights were taken
// by count from one fixed list)
TEST(MeshLodPool, AttributeWeightsFollowTheAttributesPresent){
    const std::vector<float> uvOnly = MeshLodPool::attributeWeights(false, true, false);
    const std::vector<float> both = MeshLodPool::attributeWeights(true, true, false);
    ASSERT_EQ(uvOnly.size(), 2u);
    ASSERT_EQ(both.size(), 5u);
    EXPECT_EQ(uvOnly[0], both[3]);
    EXPECT_EQ(uvOnly[1], both[4]);
    EXPECT_NE(both[0], both[3]) << "normals and UVs weigh differently, so a mix-up shows";
    EXPECT_EQ(MeshLodPool::attributeWeights(false, false, true).size(), 4u);
    EXPECT_TRUE(MeshLodPool::attributeWeights(false, false, false).empty());
}

// R2 bug 1, AC3: a level's error is its geometric deviation from the source, not the attribute-
// weighted cost: at most the densely sampled deviation (x 1.05), and more than nothing
TEST(MeshLodPool, LevelErrorIsTheGeometricDeviation){
    for (bool flat : {false, true}){
        SCOPED_TRACE(flat ? "hard normals" : "smooth normals");
        const MeshLodSource source = sphere(24, 12, flat);
        const std::shared_ptr<MeshLodData> data = MeshLodPool::buildLevels(source, MeshLodRecipe::runtime());
        ASSERT_GT(data->numLevels, 1u);
        for (unsigned int l = 1; l < data->numLevels; l++){
            const std::vector<uint32_t> level = levelIndices(*data, l);
            const double sampled = sampledDeviation(source.positions, source.indices, level);
            EXPECT_GT(data->levels[l].error, 0.0f) << "level " << l;
            EXPECT_LE(data->levels[l].error, sampled * 1.05 + 1e-6) << "level " << l << " reports more than it deviates";
            EXPECT_GE(data->levels[l].error, data->levels[l - 1].error) << "errors grow with the level";
        }
    }
}

// R2 bug 4: the recipe's error budget bounds the levels (FLT_MAX let the triangle targets rule)
TEST(MeshLodPool, TheRecipesErrorBudgetBoundsTheLevels){
    const MeshLodSource source = sphere(32, 16, false);
    MeshLodRecipe loose = MeshLodRecipe::runtime();
    loose.targetError = 1.0f;
    MeshLodRecipe tight = loose;
    tight.targetError = 0.002f;
    const std::shared_ptr<MeshLodData> a = MeshLodPool::buildLevels(source, loose);
    const std::shared_ptr<MeshLodData> b = MeshLodPool::buildLevels(source, tight);
    ASSERT_GT(a->numLevels, 1u);
    const unsigned int coarsest = a->numLevels - 1;
    const unsigned int tightTriangles = b->numLevels > coarsest ? b->levels[coarsest].indexCount : (unsigned int)source.indices.size();
    EXPECT_GT(tightTriangles, a->levels[coarsest].indexCount) << "a tight budget keeps more of the mesh";
    for (unsigned int l = 1; l < b->numLevels; l++)
        EXPECT_LE(b->levels[l].error, a->levels[l].error) << "level " << l;
}

// A smooth low-poly mesh reaches its distant levels: selection guards their quality, the budget
// only stops degenerate collapses (red with the budgets of 2 %: the chain stopped at 2 levels and
// Shadow LOD Bias had no coarser level to pick)
TEST(MeshLodPool, ASmoothLowPolySphereReachesItsDistantLevels){
    const MeshLodSource source = sphere(16, 7, false);
    ASSERT_EQ(source.indices.size(), 192u * 3u);
    const std::shared_ptr<MeshLodData> data = MeshLodPool::buildLevels(source, MeshLodRecipe::forReduction(MeshLodReduction::BALANCED, 3));
    EXPECT_GE(data->numLevels, 3u);
    const float size = 2.0f;    // the unit sphere's extent, what the budget is a share of
    for (unsigned int l = 1; l < data->numLevels; l++)
        EXPECT_LT(data->levels[l].error, 0.15f * size) << "level " << l;
}

// R2: a faceted mesh (no shared vertices) used to stall at LOD 0; the permissive retry reduces it
TEST(MeshLodPool, AFacetedMeshStillGetsLevels){
    const std::shared_ptr<MeshLodData> data = MeshLodPool::buildLevels(sphere(24, 12, true), MeshLodRecipe::runtime());
    EXPECT_GE(data->numLevels, 2u);
}

// R2: skinned meshes qualify, their bone weights guiding the collapses
TEST(MeshLodPool, ASkinnedMeshQualifies){
    MeshLodSource source = sphere(24, 12, false);
    const size_t vertices = source.positions.size() / 3;
    for (size_t v = 0; v < vertices; v++){
        const float upper = source.positions[v * 3 + 1] > 0.0f ? 1.0f : 0.0f;
        source.skinWeights.insert(source.skinWeights.end(), {upper, 1.0f - upper, 0.0f, 0.0f});
    }
    const std::shared_ptr<MeshLodData> data = MeshLodPool::buildLevels(source, MeshLodRecipe::runtime());
    ASSERT_GT(data->numLevels, 1u);
    // no triangle of a level spans both bones' halves more than the source's band at the equator
    for (unsigned int l = 1; l < data->numLevels; l++){
        const std::vector<uint32_t> level = levelIndices(*data, l);
        for (size_t t = 0; t < level.size(); t += 3){
            float lo = 1e9f, hi = -1e9f;
            for (int k = 0; k < 3; k++){
                lo = std::min(lo, source.positions[level[t + k] * 3 + 1]);
                hi = std::max(hi, source.positions[level[t + k] * 3 + 1]);
            }
            EXPECT_LT(hi - lo, 1.2f) << "level " << l << " triangle " << t / 3 << " collapsed across the joint";
        }
    }
}

TEST(MeshLodPool, TheRuntimeRecipeIsBalanced){
    const MeshLodRecipe recipe = MeshLodRecipe::runtime();
    EXPECT_EQ(recipe.levels, (unsigned int)MAX_MESH_LODS - 1);
    EXPECT_FLOAT_EQ(recipe.ratio, 0.5f);
    EXPECT_FLOAT_EQ(recipe.targetError, 0.15f);
    EXPECT_FALSE(recipe.permissive);
    EXPECT_FLOAT_EQ(MeshLodRecipe::forReduction(MeshLodReduction::PRESERVE_DETAIL, 3).targetError, 0.05f);
    EXPECT_FLOAT_EQ(MeshLodRecipe::forReduction(MeshLodReduction::SMALLEST, 3).targetError, 0.3f);
    EXPECT_TRUE(MeshLodRecipe::forReduction(MeshLodReduction::SMALLEST, 2).permissive);
    EXPECT_EQ(MeshLodRecipe::forReduction(MeshLodReduction::SMALLEST, 2).levels, 2u);
}

TEST(MeshLodPool, BuildsAreDeterministic){
    const MeshLodSource source = sphere(20, 10, false);
    const std::shared_ptr<MeshLodData> a = MeshLodPool::buildLevels(source, MeshLodRecipe::runtime());
    const std::shared_ptr<MeshLodData> b = MeshLodPool::buildLevels(source, MeshLodRecipe::runtime());
    EXPECT_EQ(a->indices, b->indices);
    for (unsigned int l = 0; l < MAX_MESH_LODS; l++)
        EXPECT_EQ(a->levels[l].error, b->levels[l].error);
}

// R3: prefilled levels are there at once, without a build
TEST(MeshLodPool, PrefilledLevelsNeedNoBuild){
    const uint64_t builds = MeshLodPool::getBuildCount();
    auto data = std::make_shared<MeshLodData>();
    data->numLevels = 2;
    data->baked = true;
    const uint64_t key = MeshLodPool::hash(0, "prefilled", 9);
    MeshLodPool::prefill(key, data);
    EXPECT_EQ(MeshLodPool::get(key), data);
    EXPECT_EQ(MeshLodPool::getBuildCount(), builds);
    // a refused source settles to no levels at once
    const uint64_t refused = MeshLodPool::hash(0, "refused", 7);
    MeshLodPool::prefill(refused, nullptr);
    ASSERT_TRUE(MeshLodPool::get(refused));
    EXPECT_EQ(MeshLodPool::get(refused)->numLevels, 1u);
    MeshLodPool::clearUnused();
    EXPECT_FALSE(MeshLodPool::has(refused)) << "nobody holds it";
    data.reset();
    MeshLodPool::clearUnused();
}

namespace {
    // a primitive whose JE_mesh_lods names levels of these indices and errors (uint16 accessors)
    tinygltf::Model bakedModel(const std::vector<std::vector<uint16_t>>& levels, const std::vector<double>& errors, int recipe){
        tinygltf::Model model;
        tinygltf::Buffer buffer;
        tinygltf::Primitive primitive;
        tinygltf::Value::Array list;
        for (size_t l = 0; l < levels.size(); l++){
            tinygltf::BufferView view;
            view.buffer = 0;
            view.byteOffset = buffer.data.size();
            view.byteLength = levels[l].size() * 2;
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(levels[l].data());
            buffer.data.insert(buffer.data.end(), bytes, bytes + view.byteLength);
            model.bufferViews.push_back(view);
            tinygltf::Accessor accessor;
            accessor.bufferView = (int)l;
            accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
            accessor.type = TINYGLTF_TYPE_SCALAR;
            accessor.count = levels[l].size();
            model.accessors.push_back(accessor);
            tinygltf::Value::Object level;
            level["indices"] = tinygltf::Value((int)l);
            level["error"] = tinygltf::Value(errors[l]);
            list.push_back(tinygltf::Value(level));
        }
        model.buffers.push_back(buffer);
        tinygltf::Value::Object ext;
        ext["recipe"] = tinygltf::Value(recipe);
        ext["levels"] = tinygltf::Value(list);
        primitive.extensions[MESH_LODS_GLTF_EXTENSION] = tinygltf::Value(ext);
        model.meshes.emplace_back();
        model.meshes[0].primitives.push_back(primitive);
        return model;
    }
}

// C1: a cooked primitive's levels, coarser first after LOD 0, over the primitive's vertices
TEST(MeshLodPool, ReadsTheLevelsACookedModelBaked){
    const tinygltf::Model model = bakedModel({{0, 1, 2, 2, 1, 3}, {0, 1, 3}}, {0.01, 0.05}, kMeshLodRecipe);
    int recipe = 0;
    const std::shared_ptr<MeshLodData> data = MeshLodPool::readGltfLevels(model, model.meshes[0].primitives[0], AttributeDataType::UNSIGNED_INT, 4, recipe);
    ASSERT_TRUE(data);
    EXPECT_EQ(recipe, kMeshLodRecipe);
    EXPECT_TRUE(data->baked);
    ASSERT_EQ(data->numLevels, 3u);
    EXPECT_EQ(data->levels[1].indexCount, 6u);
    EXPECT_EQ(data->levels[2].indexCount, 3u);
    EXPECT_FLOAT_EQ(data->levels[2].error, 0.05f);
    EXPECT_EQ(levelIndices(*data, 2), (std::vector<uint32_t>{0, 1, 3}));

    // an index past the primitive's vertices, or a negative error, is refused whole
    EXPECT_FALSE(MeshLodPool::readGltfLevels(model, model.meshes[0].primitives[0], AttributeDataType::UNSIGNED_INT, 3, recipe));
    const tinygltf::Model negative = bakedModel({{0, 1, 2}}, {-1.0}, kMeshLodRecipe);
    EXPECT_FALSE(MeshLodPool::readGltfLevels(negative, negative.meshes[0].primitives[0], AttributeDataType::UNSIGNED_SHORT, 4, recipe));
    // a primitive without the extension has none
    tinygltf::Primitive plain;
    EXPECT_FALSE(MeshLodPool::readGltfLevels(model, plain, AttributeDataType::UNSIGNED_SHORT, 4, recipe));
}
