// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Dynamic Batching (docs/superpowers/specs/2026-11-04-sprite-batching-design.md, user docs
// docs/rendering/batching.md). The passes submit the Sprites, small meshes and UI elements they
// draw; an eligible one is baked (its texture rect in its UVs, its colour factor in its vertex
// colours, its model matrix in its positions, normals and tangents, relative to a 4,096-unit origin
// cell), kept as a record while nothing it was made of changes, and put into a run of the
// DrawBatcher; the runs draw when anything else draws (SokolSystem's deferred flush) through
// drawMesh / drawMeshDepth / drawUI of their first object, with neutral per-object uniforms
// (passBatchGeometry). The image is the one the objects drawn one by one make.

#include "RenderSystem.h"

#include "Scene.h"
#include "Engine.h"
#include "Log.h"
#include "component/ImageComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"
#include "profiler/Profile.h"
#include "render/UIDrawColor.h"
#include "shader/ShaderBuildTypes.h"
#include "sokol/SokolObject.h"
#include "sokol/SokolPassRecorder.h"
#include "subsystem/FluidSystem.h"
#include "subsystem/RenderSystemShared.h"
#include "texture/MaterialPbr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>

using namespace jasperengine;

namespace{
    // the MESH shader properties a batched variant may have: everything it reads per object the
    // batch bakes or compares (an allowlist: a property added later does not batch until it is
    // checked here with a test)
    constexpr int BATCH_MESH_PROPERTIES[] = {
        0,  // Ult
        1,  // Uv1
        2,  // Uv2
        3,  // Puc (object lights compared)
        4,  // Shw (pass-wide blocks)
        6,  // Nor (normals baked)
        7,  // Nmp (with Tan only)
        8,  // Tan (tangents baked)
        9,  // Vc3
        10, // Vc4 (colour factor baked)
        11, // Txr (texture rect baked)
        12, // Fog
        21, // Sao
        22, // L2d
        23, // S2d
        24, // Ams (cutoff compared)
        25, // Aop
        28, // Vtl (object lights compared)
        29, // Lc4
        30, // L16
        31, // Pcl
        33  // Shq
    };

    uint64_t batchMeshPropertyMask(){
        uint64_t mask = 0;
        for (int bit : BATCH_MESH_PROPERTIES)
            mask |= shaderPropertyBit(bit);
        return mask;
    }

    std::array<uint8_t, 64>& attributeRoles(){
        static std::array<uint8_t, 64> roles = {};
        return roles;
    }

    // the float offset of an attribute of `elements` floats in an interleaved buffer, -1 without it
    int batchFloatAttribute(Buffer& buffer, AttributeType type, unsigned int elements){
        Attribute* attribute = buffer.getAttribute(type);
        if (!attribute || attribute->getDataType() != AttributeDataType::FLOAT || attribute->getElements() != elements)
            return -1;
        if (attribute->getOffset() + sizeof(float) * elements > buffer.getStride())
            return -1;
        return (int)attribute->getOffset();
    }

    bool batchAffine(const Matrix4& m){
        return m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f && m[3][3] == 1.0f;
    }

    float batchSign(float value){
        return value < 0.0f ? -1.0f : 1.0f;
    }

    void batchNormalize3(float v[3]){
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 0.0f){
            v[0] /= length;
            v[1] /= length;
            v[2] /= length;
        }
    }

    // the upper 3x3 of a column-major matrix times v
    void batchLinear3(const Matrix4& m, const float v[3], float out[3]){
        for (int r = 0; r < 3; r++)
            out[r] = m[0][r] * v[0] + m[1][r] * v[1] + m[2][r] * v[2];
    }

    float batchDeterminant3(const Matrix4& m){
        return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
               m[1][0] * (m[0][1] * m[2][2] - m[2][1] * m[0][2]) +
               m[2][0] * (m[0][1] * m[1][2] - m[1][1] * m[0][2]);
    }

    // the views and samplers of an object's own texture slots, as a key section (the pass's own
    // textures, shadow maps or SSAO, are bound to the run's draw)
    bool addTextureSection(BatchKey& key, const ObjectRender& render, ShaderRender* shader, std::initializer_list<TextureShaderType> types){
        uint32_t ids[16];
        size_t count = 0;
        if (shader){
            for (TextureShaderType type : types){
                const std::pair<int, int> slot = shader->shaderData.getTextureIndex(type);
                if (slot.first < 0 || count + 2 > 16)
                    continue;
                render.getTextureBinding(slot, ids[count], ids[count + 1]);
                count += 2;
            }
        }
        return key.addSection(BatchBreak::TEXTURE, ids, count * sizeof(uint32_t));
    }

    size_t batchRoundUp4(size_t value){
        return (value + 3) & ~(size_t)3;
    }

    constexpr size_t BATCH_VERTEX_CHUNK = 256 * 1024;
    constexpr size_t BATCH_INDEX_CHUNK = 64 * 1024;
    constexpr size_t BATCH_CHUNK_MAX = 16 * 1024 * 1024;
}

//-----Attribute roles (pixel-art S52's per-object values in vertices)-----

void RenderSystem::setBatchAttributeRole(AttributeType type, BatchAttributeRole role){
    const size_t index = (size_t)type;
    if (index < attributeRoles().size())
        attributeRoles()[index] = (uint8_t)role;
}

RenderSystem::BatchAttributeRole RenderSystem::getBatchAttributeRole(AttributeType type){
    const size_t index = (size_t)type;
    return index < attributeRoles().size() ? (BatchAttributeRole)attributeRoles()[index] : BatchAttributeRole::VALUE;
}

//-----Records-----

RenderSystem::BatchRecord* RenderSystem::findBatchRecord(Entity entity){
    const size_t page = entity / BATCH_PAGE;
    if (page >= batchPages.size() || !batchPages[page])
        return nullptr;
    const uint32_t index = batchPages[page]->index[entity % BATCH_PAGE];
    return index ? &batchRecords[index - 1] : nullptr;
}

bool RenderSystem::hasBatchRecord(Entity entity) const{
    const size_t page = entity / BATCH_PAGE;
    return page < batchPages.size() && batchPages[page] && batchPages[page]->index[entity % BATCH_PAGE] != 0;
}

RenderSystem::BatchRecord& RenderSystem::addBatchRecord(Entity entity){
    const size_t page = entity / BATCH_PAGE;
    if (page >= batchPages.size())
        batchPages.resize(page + 1);
    if (!batchPages[page]){
        batchPages[page] = std::make_unique<BatchPage>();
        std::fill(std::begin(batchPages[page]->index), std::end(batchPages[page]->index), 0u);
    }
    batchRecords.emplace_back();
    batchRecords.back().entity = entity;
    batchPages[page]->index[entity % BATCH_PAGE] = (uint32_t)batchRecords.size();
    batchPages[page]->count++;
    return batchRecords.back();
}

void RenderSystem::dropBatchRecord(Entity entity){
    const size_t page = entity / BATCH_PAGE;
    if (page >= batchPages.size() || !batchPages[page])
        return;
    uint32_t& slot = batchPages[page]->index[entity % BATCH_PAGE];
    if (slot == 0)
        return;
    const size_t index = slot - 1;
    // its draws alone make the MVP update() left (ensureBatchMVP)
    if (batchRecords[index].mvpStale)
        batchStaleMVPs.insert(entity);
    slot = 0;
    if (index + 1 != batchRecords.size()){
        batchRecords[index] = std::move(batchRecords.back());
        const Entity moved = batchRecords[index].entity;
        batchPages[moved / BATCH_PAGE]->index[moved % BATCH_PAGE] = (uint32_t)index + 1;
    }
    batchRecords.pop_back();
    if (--batchPages[page]->count == 0)
        batchPages[page].reset();
}

void RenderSystem::dropBatchRecords(){
    while (!batchRecords.empty())
        dropBatchRecord(batchRecords.back().entity);
    batchPages.clear();
}

void RenderSystem::ensureBatchMVP(Entity entity, Transform& transform){
    BatchRecord* record = findBatchRecord(entity);
    if (record && record->mvpStale)
        record->mvpStale = false;
    else if (batchStaleMVPs.empty() || batchStaleMVPs.erase(entity) == 0)
        return;
    // each camera's pass makes its own MVPs (perCameraMVP): the main camera's would replace them
    if (hasMultipleCameras || hasReflectionProbes)
        return;
    const Entity cameraEntity = scene->getCamera();
    CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity);
    Transform* cameraTransform = scene->findComponent<Transform>(cameraEntity);
    if (camera && cameraTransform)
        updateMVP(0, transform, *camera, *cameraTransform);
}

//-----Eligibility-----

BatchBreak RenderSystem::meshBatchForm(Entity entity, Signature signature, MeshComponent& mesh, Transform& transform, PipelineType pipType, bool depthPass){
    if (!Engine::isDynamicBatching())
        return BatchBreak::DISABLED;
#ifdef JASPERENGINE_EDITOR
    // the Scene view's LOD Level and Overdraw Draw Modes tint each object's material colour, which
    // a batch's vertex colours already carry (each drawn alone takes the tint)
    if ((rv().drawMode == ViewDrawMode::LOD_LEVEL || rv().drawMode == ViewDrawMode::OVERDRAW) && !isEditorOverlay(entity))
        return BatchBreak::DISABLED;
#endif
    if (signature.test(scene->getComponentId<InstancedMeshComponent>()))
        return BatchBreak::INELIGIBLE_INSTANCED;
    if (signature.test(scene->getComponentId<TerrainComponent>()) || signature.test(scene->getComponentId<TilemapComponent>()))
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (!mesh.loaded || mesh.needReload)
        return BatchBreak::INELIGIBLE_NOT_READY;
    if (mesh.numSubmeshes != 1)
        return BatchBreak::INELIGIBLE_SUBMESHES;

    Submesh& submesh = mesh.submeshes[0];
    if (submesh.vertexCount == 0 || !submesh.shader)
        return BatchBreak::INELIGIBLE_NOT_READY;
    // the built-in Lit with only the scene's High Lit Quality (a "builtin:lit" reference of its
    // own, the same per-object inputs) batches as the plain built-in shader does
    static const uint32_t litQualityBit = MaterialPbr::getDefineBit(MaterialPbr::LIT_QUALITY_DEFINE);
    const bool builtinLit = submesh.customShaderId == 0 ||
        (submesh.litDefineBits == litQualityBit && submesh.material.shader.empty() && scene->getDefaultCustomShader(ShaderType::MESH).empty());
    if (submesh.manifest || !builtinLit || submesh.customDepthShaderId != 0 ||
            !mesh.customShader.empty() || !mesh.customDepthShader.empty())
        return BatchBreak::INELIGIBLE_CUSTOM_SHADER;
    if (submesh.extraPass)
        return BatchBreak::INELIGIBLE_EXTRA_PASS;
    if (submesh.hasSkinning || submesh.hasMorphTarget)
        return BatchBreak::INELIGIBLE_SKINNED;
    if (mesh.lattice.count > 0 || (submesh.shaderProperties & shaderPropertyBit(32)))
        return BatchBreak::INELIGIBLE_LATTICE;
    if (mesh.vat.variantActive != VatVariant::NONE || (submesh.shaderProperties & (shaderPropertyBit(34) | shaderPropertyBit(35))))
        return BatchBreak::INELIGIBLE_VAT;
    if (submesh.lodData || mesh.numLods > 1)
        return BatchBreak::INELIGIBLE_LOD;
    if (submesh.hasIBL)
        return BatchBreak::INELIGIBLE_IBL;
    if (submesh.slotFSMirror != -1)
        return BatchBreak::INELIGIBLE_MIRROR;

    const uint64_t properties = submesh.shaderProperties;
    static const uint64_t allowed = batchMeshPropertyMask();
    if ((properties & ~allowed) || submesh.pbrParamsSize != 0 || submesh.pbrMaps[0] != PbrMap::COUNT)
        return BatchBreak::INELIGIBLE_SHADER_PROPERTY;
    // a normal map without tangents builds its frame from the object-space normal (surface_vs.glsl)
    if ((properties & shaderPropertyBit(7)) && !(properties & shaderPropertyBit(8)))
        return BatchBreak::INELIGIBLE_SHADER_PROPERTY;

    // indexed 16-bit triangles of the mesh's own interleaved buffer: the pipeline draws the
    // batch's indices as the object's
    if (submesh.primitiveType != PrimitiveType::TRIANGLES || mesh.numExternalBuffers != 0 || !submesh.attributes.empty() ||
            submesh.smoothNormals >= 0 || !mesh.buffer.isRenderAttributes() || mesh.buffer.getStride() == 0 || !mesh.buffer.getData())
        return BatchBreak::INELIGIBLE_GEOMETRY;
    const Attribute* index = mesh.indices.getAttribute(AttributeType::INDEX);
    if (!index || index->getDataType() != AttributeDataType::UNSIGNED_SHORT || index->getOffset() != 0 ||
            mesh.indices.getCount() < submesh.vertexCount || !mesh.indices.getData())
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (batchFloatAttribute(mesh.buffer, AttributeType::POSITION, 3) < 0)
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (mesh.buffer.getCount() > BATCH_MAX_MESH_VERTICES)
        return BatchBreak::INELIGIBLE_VERTEX_COUNT;

    const Material& material = submesh.material;
    if (material.baseColorTexture.isFramebuffer() || material.emissiveTexture.isFramebuffer() ||
            material.metallicRoughnessTexture.isFramebuffer() || material.occlusionTexture.isFramebuffer() ||
            material.normalTexture.isFramebuffer())
        return BatchBreak::INELIGIBLE_RENDER_TARGET;
    if (frameFluids && frameFluids->hasMaterialFluids() && frameFluids->getMaterialTexture(entity))
        return BatchBreak::INELIGIBLE_FLUID;

    if (transform.billboard && transform.fakeBillboard)
        return BatchBreak::INELIGIBLE_BILLBOARD;
#ifdef JASPERENGINE_EDITOR
    // a billboard turned to a secondary view's camera (its pass matrices)
    if (isObjectOverridden(transform))
        return BatchBreak::INELIGIBLE_BILLBOARD;
#endif
    if (!batchAffine(objectModel(transform)))
        return BatchBreak::INELIGIBLE_TRANSFORM;

    const ObjectRender& render = depthPass ? submesh.depthRender : submesh.render;
    if ((depthPass && !submesh.depthShader) || render.getPipelineId(pipType) == SG_INVALID_ID)
        return BatchBreak::INELIGIBLE_NOT_READY;
    return BatchBreak::NONE;
}

BatchBreak RenderSystem::uiBatchForm(UIComponent& ui, TextureRender* textureOverride){
    if (!Engine::isDynamicBatching())
        return BatchBreak::DISABLED;
    if (!ui.loaded || ui.needReload || !ui.shader || ui.buffer.getSize() == 0)
        return BatchBreak::INELIGIBLE_NOT_READY;
    if (ui.customShaderId != 0 || !ui.libraryBindings.textures.empty())
        return BatchBreak::INELIGIBLE_CUSTOM_SHADER;
    // the UI shader properties a batch reads as drawn alone (an allowlist, as the meshes': a
    // property added later draws alone until it is checked here)
    static const uint64_t uiAllowed = ShaderPool::getUIProperties(true, true, true, true, true);
    if (ui.shaderProperties & ~uiAllowed)
        return BatchBreak::INELIGIBLE_SHADER_PROPERTY;
    if (textureOverride || ui.texture.isFramebuffer())
        return BatchBreak::INELIGIBLE_RENDER_TARGET;
    if (ui.primitiveType != PrimitiveType::TRIANGLES || !ui.buffer.isRenderAttributes() || ui.buffer.getStride() == 0)
        return BatchBreak::INELIGIBLE_GEOMETRY;
    const Attribute* index = ui.indices.getAttribute(AttributeType::INDEX);
    if (ui.indices.getCount() == 0 || !index || index->getDataType() != AttributeDataType::UNSIGNED_SHORT ||
            index->getOffset() != 0 || !ui.indices.getData())
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (batchFloatAttribute(ui.buffer, AttributeType::POSITION, 3) < 0)
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (ui.buffer.getCount() > BATCH_MAX_UI_VERTICES)
        return BatchBreak::INELIGIBLE_VERTEX_COUNT;
    return BatchBreak::NONE;
}

BatchBreak RenderSystem::getBatchEligibility(Entity entity){
    Signature signature = scene->getSignature(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform)
        return BatchBreak::INELIGIBLE_GEOMETRY;
    if (signature.test(scene->getComponentId<MeshComponent>()))
        return meshBatchForm(entity, signature, scene->getComponent<MeshComponent>(entity), *transform, PIP_DEFAULT, false);
    if (signature.test(scene->getComponentId<UIComponent>()))
        return uiBatchForm(scene->getComponent<UIComponent>(entity), nullptr);
    return BatchBreak::INELIGIBLE_GEOMETRY;
}

//-----Baking-----

bool RenderSystem::bakeMeshRecord(BatchRecord& record, MeshComponent& mesh){
    Submesh& submesh = mesh.submeshes[0];
    Buffer& buffer = mesh.buffer;
    const uint64_t properties = submesh.shaderProperties;

    record.ui = false;
    record.stride = buffer.getStride();
    record.vertexCount = buffer.getCount();
    record.indexCount = submesh.vertexCount;
    record.positionOffset = batchFloatAttribute(buffer, AttributeType::POSITION, 3);
    record.normalOffset = batchFloatAttribute(buffer, AttributeType::NORMAL, 3);
    record.tangentOffset = batchFloatAttribute(buffer, AttributeType::TANGENT, 4);
    record.uv1Offset = batchFloatAttribute(buffer, AttributeType::TEXCOORD1, 2);
    record.uv2Offset = batchFloatAttribute(buffer, AttributeType::TEXCOORD2, 2);
    record.colorOffset = batchFloatAttribute(buffer, AttributeType::COLOR, 4);
    record.extraAttributes.clear();
    for (const auto& attribute : buffer.getAttributes()){
        const BatchAttributeRole role = getBatchAttributeRole(attribute.first);
        if (role != BatchAttributeRole::VALUE && attribute.second.getDataType() == AttributeDataType::FLOAT &&
                attribute.second.getElements() >= 3 && attribute.second.getOffset() + sizeof(float) * 3 <= record.stride)
            record.extraAttributes.push_back({(int)attribute.second.getOffset(), role});
    }
    if (record.positionOffset < 0 || (size_t)record.vertexCount * record.stride > buffer.getSize() || record.indexCount == 0)
        return false;

    record.source = buffer.getData();
    record.sourceSize = buffer.getSize();
    record.hasRect = submesh.hasTextureRect;
    record.rect = submesh.textureRect;
    record.color = submesh.material.baseColorFactor;
    // a variant reading vertex colour multiplies them by the colour factor (pbr.glsl getBaseColor)
    record.bakesColor = (properties & shaderPropertyBit(10)) && record.colorOffset >= 0;
    record.bakesNormals = (properties & shaderPropertyBit(6)) != 0;

    const size_t bytes = (size_t)record.vertexCount * record.stride;
    record.local.assign(buffer.getData(), buffer.getData() + bytes);
    unsigned char* vertices = record.local.data();
    for (uint32_t v = 0; v < record.vertexCount; v++, vertices += record.stride){
        if (record.hasRect){
            // (surface_vs.glsl surfaceUV1/2: uv * rect.zw + rect.xy, both sets)
            for (int offset : {record.uv1Offset, record.uv2Offset}){
                if (offset < 0)
                    continue;
                float uv[2];
                std::memcpy(uv, vertices + offset, sizeof(uv));
                uv[0] = uv[0] * record.rect.getWidth() + record.rect.getX();
                uv[1] = uv[1] * record.rect.getHeight() + record.rect.getY();
                std::memcpy(vertices + offset, uv, sizeof(uv));
            }
        }
        if (record.bakesColor){
            float color[4];
            std::memcpy(color, vertices + record.colorOffset, sizeof(color));
            color[0] *= record.color.x;
            color[1] *= record.color.y;
            color[2] *= record.color.z;
            color[3] *= record.color.w;
            std::memcpy(vertices + record.colorOffset, color, sizeof(color));
        }
    }

    record.indices.resize(record.indexCount);
    std::memcpy(record.indices.data(), mesh.indices.getData(), sizeof(uint16_t) * record.indexCount);
    for (uint16_t index : record.indices){
        if (index >= record.vertexCount)
            return false;
    }

    record.sourceWrites = mesh.buffer.getWriteCount();
    record.indexWrites = mesh.indices.getWriteCount();
    record.placed = record.local;
    record.localValid = true;
    record.placedValid = false;
    return true;
}

bool RenderSystem::bakeUIRecord(BatchRecord& record, UIComponent& ui, const Vector4& drawColor, bool bakesColor){
    Buffer& buffer = ui.buffer;
    record.ui = true;
    record.stride = buffer.getStride();
    record.vertexCount = buffer.getCount();
    record.indexCount = ui.indices.getCount();
    record.positionOffset = batchFloatAttribute(buffer, AttributeType::POSITION, 3);
    record.normalOffset = -1;
    record.tangentOffset = -1;
    record.uv1Offset = -1;
    record.uv2Offset = -1;
    record.colorOffset = batchFloatAttribute(buffer, AttributeType::COLOR, 4);
    record.extraAttributes.clear();
    if (record.positionOffset < 0 || (size_t)record.vertexCount * record.stride > buffer.getSize() || record.indexCount == 0)
        return false;

    record.source = buffer.getData();
    record.sourceSize = buffer.getSize();
    record.hasRect = false;
    record.color = drawColor;
    record.bakesColor = bakesColor && record.colorOffset >= 0;
    record.bakesNormals = false;

    const size_t bytes = (size_t)record.vertexCount * record.stride;
    record.local.assign(buffer.getData(), buffer.getData() + bytes);
    if (record.bakesColor){
        unsigned char* vertices = record.local.data();
        for (uint32_t v = 0; v < record.vertexCount; v++, vertices += record.stride){
            float color[4];
            std::memcpy(color, vertices + record.colorOffset, sizeof(color));
            color[0] *= drawColor.x;
            color[1] *= drawColor.y;
            color[2] *= drawColor.z;
            color[3] *= drawColor.w;
            std::memcpy(vertices + record.colorOffset, color, sizeof(color));
        }
    }

    record.indices.resize(record.indexCount);
    std::memcpy(record.indices.data(), ui.indices.getData(), sizeof(uint16_t) * record.indexCount);
    for (uint16_t index : record.indices){
        if (index >= record.vertexCount)
            return false;
    }

    record.sourceWrites = ui.buffer.getWriteCount();
    record.indexWrites = ui.indices.getWriteCount();
    record.placed = record.local;
    record.localValid = true;
    record.placedValid = false;
    return true;
}

void RenderSystem::placeBatchRecord(BatchRecord& record, const Matrix4& model, const Matrix4* normalMatrix){
    // the origin cell of its translation: positions are relative to it, so a draw far from the
    // world origin keeps the precision of its own model matrix. Cells are centred on multiples of
    // the size, the world origin a cell's centre: a scene around it is one cell, not eight
    for (int a = 0; a < 3; a++){
        const double cell = std::floor((double)model[3][a] / BATCH_CELL_SIZE + 0.5);
        record.cell[a] = (int32_t)std::clamp(cell, -2.0e9, 2.0e9);
    }
    const double cellOrigin[3] = {(double)record.cell[0] * BATCH_CELL_SIZE, (double)record.cell[1] * BATCH_CELL_SIZE,
                                  (double)record.cell[2] * BATCH_CELL_SIZE};

    // the tangent frame as the shader mirrors it (includes/tangent_frame.glsl): a negative
    // determinant turns the bitangent, a mirrored texture rect turns u (tangent) or v; the batch
    // draws with an identity model and a whole rect, so both are baked into the tangent
    const float rectSignX = (record.hasRect && record.uv1Offset >= 0) ? batchSign(record.rect.getWidth()) : 1.0f;
    const float rectSignY = (record.hasRect && record.uv1Offset >= 0) ? batchSign(record.rect.getHeight()) : 1.0f;
    const float mirror = batchSign(batchDeterminant3(model));

    float lo[3] = {INFINITY, INFINITY, INFINITY};
    float hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    const unsigned char* source = record.local.data();
    unsigned char* target = record.placed.data();
    auto placePoint = [&](const unsigned char* from, unsigned char* to, bool bounds){
        float p[3];
        std::memcpy(p, from, sizeof(p));
        float relative[3];
        for (int r = 0; r < 3; r++){
            const double world = (double)model[0][r] * p[0] + (double)model[1][r] * p[1] + (double)model[2][r] * p[2] + (double)model[3][r];
            relative[r] = (float)(world - cellOrigin[r]);
            if (bounds){
                const float worldFloat = (float)world;
                lo[r] = std::min(lo[r], worldFloat);
                hi[r] = std::max(hi[r], worldFloat);
            }
        }
        std::memcpy(to, relative, sizeof(relative));
    };
    auto placeDirection = [&](const Matrix4& matrix, const unsigned char* from, unsigned char* to){
        float n[3];
        std::memcpy(n, from, sizeof(n));
        float out[3];
        batchLinear3(matrix, n, out);
        batchNormalize3(out);
        std::memcpy(to, out, sizeof(out));
    };

    for (uint32_t v = 0; v < record.vertexCount; v++, source += record.stride, target += record.stride){
        placePoint(source + record.positionOffset, target + record.positionOffset, true);
        if (record.bakesNormals && normalMatrix){
            if (record.normalOffset >= 0)
                placeDirection(*normalMatrix, source + record.normalOffset, target + record.normalOffset);
            if (record.tangentOffset >= 0){
                float t[4];
                std::memcpy(t, source + record.tangentOffset, sizeof(t));
                float out[3];
                batchLinear3(model, t, out);
                batchNormalize3(out);
                const float placed[4] = {out[0] * rectSignX, out[1] * rectSignX, out[2] * rectSignX, t[3] * mirror * rectSignY * rectSignX};
                std::memcpy(target + record.tangentOffset, placed, sizeof(placed));
            }
        }
        for (const std::pair<int, BatchAttributeRole>& extra : record.extraAttributes){
            if (extra.second == BatchAttributeRole::POINT)
                placePoint(source + extra.first, target + extra.first, false);
            else if (normalMatrix)
                placeDirection(*normalMatrix, source + extra.first, target + extra.first);
        }
    }

    std::memcpy(record.lo, lo, sizeof(lo));
    std::memcpy(record.hi, hi, sizeof(hi));
    record.placedWith = model;
    record.placedValid = true;
    record.cullSerial = 0;
}

const Matrix4& RenderSystem::batchCellMVP(const int32_t cell[3]){
    if (std::memcmp(cell, batchPass.mvpCell, sizeof(batchPass.mvpCell)) != 0){
        std::memcpy(batchPass.mvpCell, cell, sizeof(batchPass.mvpCell));
        const Matrix4 translation = Matrix4::translateMatrix(Vector3((float)((double)cell[0] * BATCH_CELL_SIZE),
            (float)((double)cell[1] * BATCH_CELL_SIZE), (float)((double)cell[2] * BATCH_CELL_SIZE)));
        batchPass.cellMVP = batchPass.viewProjection * translation;
    }
    return batchPass.cellMVP;
}

bool RenderSystem::cullBatchRecord(BatchRecord& record){
    if (record.cullSerial != batchCull.getSerial()){
        record.inside = batchCull.inside(record.lo, record.hi);
        record.cullSerial = batchCull.getSerial();
    }else{
        batcher.getStats().cullCacheHits++;
    }
    if (!record.inside)
        batcher.getStats().culled++;
    return !record.inside;
}

void RenderSystem::appendBatchItem(BatchRun& run, const BatchRecord& record){
    const uint32_t first = run.vertexCount;
    unsigned char* vertices = run.appendVertices(record.vertexCount);
    std::memcpy(vertices, record.placed.data(), (size_t)record.vertexCount * record.stride);
    run.appendIndices(record.indices.data(), record.indexCount, first);
}

//-----Keys-----

bool RenderSystem::fillMeshKey(BatchKey& key, Entity entity, MeshComponent& mesh, const BatchRecord& record, bool mirrored){
    (void)entity;
    const Submesh& submesh = mesh.submeshes[0];
    const Material& material = submesh.material;
    const uint64_t properties = submesh.shaderProperties;
    const bool depth = batchPass.pass != BatchPass::COLOR;
    const ObjectRender& render = depth ? submesh.depthRender : submesh.render;

    key.pass = batchPass.pass;
    key.pipType = (uint8_t)batchPass.pipType;
    key.mirrored = mirrored;
    key.noCull = passNoFaceCulling();
    key.pipeline = render.getPipelineId(batchPass.pipType);
    std::memcpy(key.cell, record.cell, sizeof(key.cell));
    key.scissor = SokolSystem::getRequestedScissor();
    key.clearSections();
    if (key.pipeline == SG_INVALID_ID)
        return false;

    bool fits = depth
        ? addTextureSection(key, render, submesh.depthShader.get(), {TextureShaderType::DEPTHTEXTURE})
        : addTextureSection(key, render, submesh.shader.get(), {TextureShaderType::BASECOLOR, TextureShaderType::METALLICROUGHNESS,
                            TextureShaderType::OCCULSION, TextureShaderType::EMISSIVE, TextureShaderType::NORMAL});
    if (depth){
        if (submesh.slotFSDepthMaterial != -1){
            const float depthMaterial[2] = {material.baseColorFactor.w, material.alphaCutoff};
            fits = key.addSection(BatchBreak::DEPTH_MATERIAL, depthMaterial, sizeof(depthMaterial));
        }
        return fits;
    }

    if (!record.bakesColor)
        fits = fits && key.addSection(BatchBreak::COLOR_FACTOR, &material.baseColorFactor, sizeof(Vector4));
    const bool lit = !(properties & shaderPropertyBit(0));
    if (lit){
        const float params[8] = {material.metallicFactor, material.roughnessFactor, material.alphaCutoff,
                                 material.emissiveFactor.x * material.emissiveStrength, material.emissiveFactor.y * material.emissiveStrength,
                                 material.emissiveFactor.z * material.emissiveStrength, material.foliageTransmission, mesh.receiveLights ? 1.0f : 0.0f};
        fits = fits && key.addSection(BatchBreak::MATERIAL_PARAMS, params, sizeof(params));
    }else{
        const uint8_t receiveLights = mesh.receiveLights ? 1 : 0;
        fits = fits && key.addSection(BatchBreak::MATERIAL_PARAMS, &receiveLights, 1);
        if (properties & shaderPropertyBit(24))
            fits = fits && key.addSection(BatchBreak::ALPHA_CUTOFF, &material.alphaCutoff, sizeof(float));
    }
    if (submesh.slotFSTexCoordSets != -1){
        const int32_t sets[5] = {material.baseColorTexCoord, material.metallicRoughnessTexCoord, material.occlusionTexCoord,
                                 material.emissiveTexCoord, material.normalTexCoord};
        fits = fits && key.addSection(BatchBreak::UV_SETS, sets, sizeof(sets));
    }
    if (properties & (shaderPropertyBit(3) | shaderPropertyBit(28))){
        // the object's lights and SH (MeshLights): equal lists build equal blocks
        const MeshLights& lights = mesh.lights;
        uint8_t block[MAX_LIGHTS_PER_OBJECT * sizeof(uint16_t) + sizeof(lights.sh) + 2];
        size_t size = 0;
        const uint8_t count = (uint8_t)std::min<unsigned int>(lights.count, MAX_LIGHTS_PER_OBJECT);
        block[size++] = count;
        block[size++] = lights.foldedDirectional ? 1 : 0;
        std::memcpy(block + size, lights.lights, sizeof(uint16_t) * count);
        size += sizeof(uint16_t) * count;
        std::memcpy(block + size, lights.sh, sizeof(lights.sh));
        size += sizeof(lights.sh);
        fits = fits && key.addSection(BatchBreak::OBJECT_LIGHTS, block, size);
    }
    return fits;
}

//-----Passes-----

void RenderSystem::beginBatchPass(BatchPass pass, BatchMode mode, CameraComponent* camera, Transform* cameraTransform, PipelineType pipType,
                                  const Matrix4& viewProjection, float farClip, const Plane* frustumPlanes){
    if (batchPassActive)
        endBatchPass();
    if (!Engine::isDynamicBatching())
        return;
    batchPass = BatchPassContext();
    batchPass.pass = pass;
    batchPass.camera = camera;
    batchPass.cameraTransform = cameraTransform;
    batchPass.pipType = pipType;
    batchPass.viewProjection = viewProjection;
    batchPass.farClip = farClip;
    batchPass.frustumPlanes = frustumPlanes;
    if (frustumPlanes)
        (pass == BatchPass::COLOR || pass == BatchPass::DEPTH_PREPASS ? batchCull : batchDepthCull).set(frustumPlanes, farClip);
    batchPass.culls = frustumPlanes != nullptr;
    batcher.setCallbacks(&RenderSystem::drawBatchRunCallback, &SokolSystem::flushDeferred, this);
    batcher.beginPass(mode, mode == BatchMode::STRICT ? 1 : (mode == BatchMode::REORDER ? 8 : 16));
    SokolSystem::setDeferredFlush(&RenderSystem::flushBatches, this);
    batchPassActive = true;
}

void RenderSystem::endBatchPass(){
    if (!batchPassActive)
        return;
    SokolSystem::flushDeferred();
    SokolSystem::setDeferredFlush(nullptr, nullptr);
    batchPassActive = false;
}

void RenderSystem::flushHeldBatchDraws(){
    if (batchPassActive)
        SokolSystem::flushDeferred();
}

void RenderSystem::flushBatches(void* owner){
    static_cast<RenderSystem*>(owner)->batcher.flush();
}

void RenderSystem::drawBatchRunCallback(void* owner, BatchRun& run){
    static_cast<RenderSystem*>(owner)->drawBatchRun(run);
}

bool RenderSystem::submitMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType,
                              InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView, int instanceView, const LodView* lodView){
    // an entry of a visible list without the submesh, the OIT accumulation, a fade-copies entry
    // or a forward queue the OIT pass took this submesh from: drawMesh draws it (or nothing) as
    // S55's queues ask
    if (!batchPassActive || !(passSubmeshMask & 1u) || passSubmeshQueue == SubmeshQueue::OIT || passLodFade == LodFadePass::FADE ||
            (passOit && mesh.numSubmeshes > 0 && drawsOit(mesh.submeshes[0])))
        return drawMesh(entity, mesh, transform, camera, camTransform, pipType, instmesh, terrain, tilemap, terrainView, instanceView, lodView);

    const Signature signature = scene->getSignature(entity);
    const BatchBreak form = meshBatchForm(entity, signature, mesh, transform, pipType, false);
    if (form != BatchBreak::NONE){
        if (hasBatchRecord(entity))
            dropBatchRecord(entity);
        batcher.noteAlone(BatchPass::COLOR, entity, form);
        return drawMesh(entity, mesh, transform, camera, camTransform, pipType, instmesh, terrain, tilemap, terrainView, instanceView, lodView);
    }
    batchPass.terrainView = terrainView;
    batchPass.instanceView = instanceView;
    batchPass.lodView = lodView;

    BatchRecord* record = findBatchRecord(entity);
    if (!record)
        record = &addBatchRecord(entity);

    // geometry written since its last draw (MeshSystem, scripts): uploaded, baked again
    if (mesh.needUpdateBuffer)
        updateMeshBuffers(entity, mesh);
    Submesh& submesh = mesh.submeshes[0];
    const bool bakesNormals = (submesh.shaderProperties & shaderPropertyBit(6)) != 0;
    Rect currentRect = submesh.textureRect;
    // (a write another pass uploaded first: the buffers' write counts)
    if (!record->localValid || record->ui || record->source != mesh.buffer.getData() || record->sourceSize != mesh.buffer.getSize() ||
            record->sourceWrites != mesh.buffer.getWriteCount() || record->indexWrites != mesh.indices.getWriteCount() ||
            record->vertexCount != mesh.buffer.getCount() || record->indexCount != submesh.vertexCount ||
            record->hasRect != submesh.hasTextureRect || (record->hasRect && record->rect != currentRect) ||
            (record->bakesColor && record->color != submesh.material.baseColorFactor) || record->bakesNormals != bakesNormals){
        if (!bakeMeshRecord(*record, mesh)){
            dropBatchRecord(entity);
            batcher.noteAlone(BatchPass::COLOR, entity, BatchBreak::INELIGIBLE_GEOMETRY);
            return drawMesh(entity, mesh, transform, camera, camTransform, pipType, instmesh, terrain, tilemap, terrainView, instanceView, lodView);
        }
        batcher.getStats().rebaked++;
    }

    Matrix4 scratch[3];
    const Matrix4* block = objectMatrices(transform, scratch);
    if (!record->placedValid || std::memcmp(&record->placedWith, &block[0], sizeof(Matrix4)) != 0){
        Matrix4 normal;
        if (record->bakesNormals)
            normal = hasLights ? block[1] : block[0].inverse().transpose();
        placeBatchRecord(*record, block[0], record->bakesNormals ? &normal : nullptr);
        batcher.getStats().replaced++;
    }else{
        batcher.getStats().settled++;
    }

    // (a visible list culled it already: passPreCulled)
    if (!passPreCulled && batchPass.culls && mesh.worldAABB != AABB::ZERO && cullBatchRecord(*record))
        return true;

    refreshSubmeshTextures(submesh, mesh.receiveLights, checkPBRFrabebufferUpdate(submesh.material));

    BatchKey key;
    if (!fillMeshKey(key, entity, mesh, *record, isMirroredDraw(mesh, block[0]))){
        batcher.noteAlone(BatchPass::COLOR, entity, BatchBreak::INELIGIBLE_NOT_READY);
        return drawMesh(entity, mesh, transform, camera, camTransform, pipType, instmesh, terrain, tilemap, terrainView, instanceView, lodView);
    }

    BatchItem item;
    item.entity = entity;
    item.minX = record->lo[0];
    item.minY = record->lo[1];
    item.maxX = record->hi[0];
    item.maxY = record->hi[1];
    item.vertexCount = record->vertexCount;
    item.indexCount = record->indexCount;
    BatchHead head;
    head.entity = entity;
    head.mesh = &mesh;
    head.transform = &transform;
    BatchRun& run = batcher.place(key, head, item, record->stride);
    appendBatchItem(run, *record);
    return true;
}

bool RenderSystem::submitMeshDepth(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const vs_depth_t& params,
                                   InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO, PipelineType pipelineType,
                                   int instanceView, const LodView* lodView){
    // drawn alone: the camera pre-pass's params carry the colour draw's MVP (the same gl_Position),
    // which a batched mesh's lazy MVP may still have to be made for (ensureBatchMVP)
    auto drawAlone = [&]() -> bool{
        if (batchPassActive && batchPass.pass == BatchPass::DEPTH_PREPASS){
            ensureBatchMVP(entity, transform);
            vs_depth_t own = params;
            own.mvpMatrix = objectMVP(transform);
            return drawMeshDepth(entity, mesh, transform, cameraFar, frustumPlanes, own, instmesh, terrain, tilemap, forSSAO, pipelineType, instanceView, lodView);
        }
        return drawMeshDepth(entity, mesh, transform, cameraFar, frustumPlanes, params, instmesh, terrain, tilemap, forSSAO, pipelineType, instanceView, lodView);
    };
    if (!batchPassActive || !(passSubmeshMask & 1u) || passLodFade == LodFadePass::FADE)
        return drawAlone();

    const Signature signature = scene->getSignature(entity);
    // the camera pre-pass draws in batch form exactly what the colour pass does, so both compute
    // their depth from the same vertices and matrices (invariant gl_Position); other depth passes
    // need only their depth program
    BatchBreak form = (batchPass.pass == BatchPass::DEPTH_PREPASS)
        ? meshBatchForm(entity, signature, mesh, transform, batchPass.colorPipType, false)
        : meshBatchForm(entity, signature, mesh, transform, pipelineType, true);
    if (form != BatchBreak::NONE){
        batcher.noteAlone(batchPass.pass, entity, form);
        return drawAlone();
    }
    if (!mesh.castShadows && !forSSAO)
        return true;

    Submesh& submesh = mesh.submeshes[0];
    if (forSSAO && (submesh.skipsScreenEffects || submesh.stencilTested))
        return true;
    if (passSubmeshQueue == SubmeshQueue::OPAQUE && submesh.alphaBlend)
        return true;
    if (!submesh.depthShader || submesh.depthRender.getPipelineId(pipelineType) == SG_INVALID_ID)
        return true; // no depth program for this pass: drawn alone it is left out too

    BatchRecord* record = findBatchRecord(entity);
    if (!record)
        record = &addBatchRecord(entity);
    if (mesh.needUpdateBuffer)
        updateMeshBuffers(entity, mesh);
    const bool bakesNormals = (submesh.shaderProperties & shaderPropertyBit(6)) != 0;
    Rect currentRect = submesh.textureRect;
    // (a write another pass uploaded first: the buffers' write counts)
    if (!record->localValid || record->ui || record->source != mesh.buffer.getData() || record->sourceSize != mesh.buffer.getSize() ||
            record->sourceWrites != mesh.buffer.getWriteCount() || record->indexWrites != mesh.indices.getWriteCount() ||
            record->vertexCount != mesh.buffer.getCount() || record->indexCount != submesh.vertexCount ||
            record->hasRect != submesh.hasTextureRect || (record->hasRect && record->rect != currentRect) ||
            (record->bakesColor && record->color != submesh.material.baseColorFactor) || record->bakesNormals != bakesNormals){
        if (!bakeMeshRecord(*record, mesh)){
            dropBatchRecord(entity);
            return true;
        }
        batcher.getStats().rebaked++;
    }
    Matrix4 scratch[3];
    const Matrix4* block = objectMatrices(transform, scratch);
    if (!record->placedValid || std::memcmp(&record->placedWith, &block[0], sizeof(Matrix4)) != 0){
        Matrix4 normal;
        if (record->bakesNormals)
            normal = hasLights ? block[1] : block[0].inverse().transpose();
        placeBatchRecord(*record, block[0], record->bakesNormals ? &normal : nullptr);
        batcher.getStats().replaced++;
    }else{
        batcher.getStats().settled++;
    }

    if (!passPreCulled && frustumPlanes && mesh.worldAABB != AABB::ZERO){
        // (the pre-pass has the colour pass's planes, cascades and the scene depth their own)
        bool culled;
        if (batchPass.pass == BatchPass::DEPTH_PREPASS){
            culled = cullBatchRecord(*record);
        }else{
            culled = !batchDepthCull.inside(record->lo, record->hi);
            if (culled)
                batcher.getStats().culled++;
        }
        if (culled)
            return true;
    }

    refreshDepthTextures(submesh);

    BatchKey key;
    if (!fillMeshKey(key, entity, mesh, *record, isMirroredDraw(mesh, block[0])))
        return true;

    BatchItem item;
    item.entity = entity;
    item.vertexCount = record->vertexCount;
    item.indexCount = record->indexCount;
    BatchHead head;
    head.entity = entity;
    head.mesh = &mesh;
    head.transform = &transform;
    BatchRun& run = batcher.place(key, head, item, record->stride);
    appendBatchItem(run, *record);
    return true;
}

bool RenderSystem::submitUI(Entity entity, UIComponent& ui, Transform& transform, PipelineType pipType, TextureRender* textureOverride){
    if (!batchPassActive)
        return drawUI(ui, transform, pipType, textureOverride);

    const BatchBreak form = uiBatchForm(ui, textureOverride);
    if (form != BatchBreak::NONE){
        if (hasBatchRecord(entity))
            dropBatchRecord(entity);
        batcher.noteAlone(BatchPass::UI, entity, form);
        return drawUI(ui, transform, pipType, textureOverride);
    }

    // its texture as drawUI makes it current
    refreshUITexture(ui);

    BatchRecord* record = findBatchRecord(entity);
    if (!record)
        record = &addBatchRecord(entity);
    if (ui.needUpdateBuffer)
        uploadUIBuffers(ui);

    const uint64_t vertexColorBit = ShaderPool::getUIProperties(false, false, false, true, false);
    const bool vertexColor = (ui.shaderProperties & vertexColorBit) != 0;
    const Vector4 drawColor = uiElementColor(ui, false);
    // (a write a Canvas Group layer uploaded first: the buffers' write counts)
    if (!record->localValid || !record->ui || record->source != ui.buffer.getData() || record->sourceSize != ui.buffer.getSize() ||
            record->sourceWrites != ui.buffer.getWriteCount() || record->indexWrites != ui.indices.getWriteCount() ||
            record->vertexCount != ui.buffer.getCount() || record->indexCount != ui.indices.getCount() ||
            (record->bakesColor && record->color != drawColor) || record->bakesColor != (vertexColor && batchFloatAttribute(ui.buffer, AttributeType::COLOR, 4) >= 0)){
        if (!bakeUIRecord(*record, ui, drawColor, vertexColor)){
            dropBatchRecord(entity);
            batcher.noteAlone(BatchPass::UI, entity, BatchBreak::INELIGIBLE_GEOMETRY);
            return drawUI(ui, transform, pipType, textureOverride);
        }
        batcher.getStats().rebaked++;
    }
    const Matrix4& model = objectModel(transform);
    if (!record->placedValid || std::memcmp(&record->placedWith, &model, sizeof(Matrix4)) != 0){
        placeBatchRecord(*record, model, nullptr);
        batcher.getStats().replaced++;
    }else{
        batcher.getStats().settled++;
    }

    BatchKey key;
    key.pass = BatchPass::UI;
    key.pipType = (uint8_t)pipType;
    key.pipeline = ui.render.getPipelineId(pipType);
    std::memcpy(key.cell, record->cell, sizeof(key.cell));
    key.scissor = SokolSystem::getRequestedScissor();
    bool fits = key.pipeline != SG_INVALID_ID && addTextureSection(key, ui.render, ui.shader.get(), {TextureShaderType::UI});
    if (!record->bakesColor)
        fits = fits && key.addSection(BatchBreak::UI_COLOR, &drawColor, sizeof(drawColor));
    if (ui.slotFSTextParams >= 0)
        fits = fits && key.addSection(BatchBreak::TEXT_PARAMS, ui.textParams, sizeof(ui.textParams));
    if (!fits){
        batcher.noteAlone(BatchPass::UI, entity, BatchBreak::INELIGIBLE_NOT_READY);
        return drawUI(ui, transform, pipType, textureOverride);
    }

    BatchItem item;
    item.entity = entity;
    item.minX = record->lo[0];
    item.minY = record->lo[1];
    item.maxX = record->hi[0];
    item.maxY = record->hi[1];
    item.vertexCount = record->vertexCount;
    item.indexCount = record->indexCount;
    BatchHead head;
    head.entity = entity;
    head.ui = &ui;
    head.transform = &transform;
    BatchRun& run = batcher.place(key, head, item, record->stride);
    appendBatchItem(run, *record);
    return true;
}

void RenderSystem::drawBatchRun(BatchRun& run){
    BatchGeometry geometry;
    const size_t vertexBytes = run.vertices.size();
    if (!uploadBatch(run.vertices.data(), vertexBytes, false, geometry.vertexBuffer, geometry.vertexOffset)){
        batcher.getStats().runsSkipped++;
        return;
    }
    geometry.indexCount = (uint32_t)run.indices.size();
    if (run.allQuads && run.vertexCount <= BATCH_QUADS * 4){
        if (!batchQuadIndices.isCreated()){
            std::vector<uint16_t> quads((size_t)BATCH_QUADS * 6);
            for (uint32_t q = 0; q < BATCH_QUADS; q++){
                const uint16_t base = (uint16_t)(q * 4);
                const uint16_t pattern[6] = {base, (uint16_t)(base + 1), (uint16_t)(base + 2), base, (uint16_t)(base + 2), (uint16_t)(base + 3)};
                std::memcpy(quads.data() + (size_t)q * 6, pattern, sizeof(pattern));
            }
            JE_GPU_ALLOC_SCOPE(Uniforms, "Dynamic batching");
            batchQuadIndices.createBuffer((unsigned int)(quads.size() * sizeof(uint16_t)), quads.data(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE);
        }
        geometry.indexBuffer = &batchQuadIndices;
        geometry.indexOffset = 0;
    }else if (!uploadBatch(run.indices.data(), run.indices.size() * sizeof(uint16_t), true, geometry.indexBuffer, geometry.indexOffset)){
        batcher.getStats().runsSkipped++;
        return;
    }
    if (!geometry.indexBuffer || !geometry.indexBuffer->isCreated()){
        batcher.getStats().runsSkipped++;
        return;
    }

    geometry.mirrored = run.key.mirrored;
    const Matrix4& mvp = batchCellMVP(run.key.cell);
    geometry.objectBlock[0] = Matrix4::translateMatrix(Vector3((float)((double)run.key.cell[0] * BATCH_CELL_SIZE),
        (float)((double)run.key.cell[1] * BATCH_CELL_SIZE), (float)((double)run.key.cell[2] * BATCH_CELL_SIZE)));
    geometry.objectBlock[1] = Matrix4();
    geometry.objectBlock[2] = mvp;

    // what the instancing batch holds draws first (its draw is older), so the annotation below is
    // this run's draw's
    SokolDrawBatch::flush();
    SokolSystem::applyBatchScissor(run.key.scissor);
    if (SokolPassRecorder::isDrawListEnabled())
        SokolPassRecorder::annotateNextDraw((uint16_t)std::min<uint32_t>(run.items, UINT16_MAX), (uint8_t)run.openedBy);

    // the run draws its whole submesh, whichever queue entry flushed it (S55's pass state)
    const SubmeshQueue queue = passSubmeshQueue;
    const uint32_t submeshMask = passSubmeshMask;
    const bool preCulled = passPreCulled;
    const LodFadePass lodFade = passLodFade;
    passSubmeshQueue = SubmeshQueue::ALL;
    passSubmeshMask = 0xFFFFFFFFu;
    passPreCulled = false;
    passLodFade = LodFadePass::ALL;
    passBatchGeometry = &geometry;
    const BatchHead& head = run.head;
    if (run.key.pass == BatchPass::UI && head.ui){
        const uint64_t vertexColorBit = ShaderPool::getUIProperties(false, false, false, true, false);
        geometry.neutralColor = (head.ui->shaderProperties & vertexColorBit) && batchFloatAttribute(head.ui->buffer, AttributeType::COLOR, 4) >= 0;
        drawUI(*head.ui, *head.transform, (PipelineType)run.key.pipType, nullptr, batchPass.inCanvasLayer);
    }else if (run.key.pass == BatchPass::COLOR && head.mesh){
        geometry.neutralColor = (head.mesh->submeshes[0].shaderProperties & shaderPropertyBit(10)) &&
                                batchFloatAttribute(head.mesh->buffer, AttributeType::COLOR, 4) >= 0;
        drawMesh(head.entity, *head.mesh, *head.transform, *batchPass.camera, *batchPass.cameraTransform, (PipelineType)run.key.pipType,
                 nullptr, nullptr, nullptr, batchPass.terrainView, batchPass.instanceView, batchPass.lodView);
    }else if (head.mesh){
        vs_depth_t params = {geometry.objectBlock[0], batchPass.viewProjection, mvp};
        drawMeshDepth(head.entity, *head.mesh, *head.transform, batchPass.farClip, batchPass.frustumPlanes, params, nullptr, nullptr, nullptr,
                      batchPass.forSSAO, (PipelineType)run.key.pipType, batchPass.instanceView, batchPass.lodView);
    }
    passBatchGeometry = nullptr;
    passSubmeshQueue = queue;
    passSubmeshMask = submeshMask;
    passPreCulled = preCulled;
    passLodFade = lodFade;
}

void RenderSystem::drawBatchGeometry(ObjectRender& render, Buffer& vertices, const BatchGeometry& batch){
    render.replaceVertexBuffer(vertices.getRender(), batch.vertexBuffer, batch.vertexOffset);
    render.setIndexBuffer(batch.indexBuffer, batch.indexOffset);
    render.draw(0, batch.indexCount, 1);
    render.replaceVertexBuffer(vertices.getRender(), vertices.getRender(), 0);
    render.resetIndexBuffer();
}

//-----Stream chunks-----

bool RenderSystem::uploadBatch(const void* data, size_t size, bool index, BufferRender*& buffer, size_t& offset){
    if (size == 0)
        return false;
    std::vector<BatchChunk>& chunks = index ? batchIndexChunks : batchVertexChunks;
    // the chunk this frame appends to (the ones before it are full)
    size_t& cursor = index ? batchIndexCursor : batchVertexCursor;
    (index ? batchIndexDemand : batchVertexDemand) += batchRoundUp4(size);

    while (cursor < chunks.size() && chunks[cursor].used + batchRoundUp4(size) > chunks[cursor].capacity)
        cursor++;
    if (cursor == chunks.size()){
        const size_t last = chunks.empty() ? (index ? BATCH_INDEX_CHUNK : BATCH_VERTEX_CHUNK) / 2 : chunks.back().capacity;
        const size_t capacity = std::min(std::max(last * 2, batchRoundUp4(size)), std::max(BATCH_CHUNK_MAX, batchRoundUp4(size)));
        chunks.emplace_back();
        JE_GPU_ALLOC_SCOPE(Uniforms, "Dynamic batching");
        if (!chunks.back().buffer.createBuffer((unsigned int)capacity, nullptr, index ? BufferType::INDEX_BUFFER : BufferType::VERTEX_BUFFER,
                                               BufferUsage::STREAM)){
            chunks.pop_back();
            return false;
        }
        chunks.back().capacity = capacity;
    }
    BatchChunk& chunk = chunks[cursor];
    const int appended = chunk.buffer.appendBuffer((unsigned int)size, data);
    if (appended < 0)
        return false; // the Vulkan stream budget refused it (SokolSystem logs the frame)
    chunk.used = (size_t)appended + batchRoundUp4(size);
    buffer = &chunk.buffer;
    offset = (size_t)appended;
    batcher.getStats().uploadBytes += size;
    return true;
}

void RenderSystem::prepareBatchChunks(){
    const uint64_t commits = SokolSystem::getCommitCount();
    if (commits == batchChunkCommit)
        return;
    batchChunkCommit = commits;

    // the chunks of last frame as one, sized for what it needed, when one chunk holds it; past
    // the largest chunk they are all needed and kept (made again each frame they would cost an
    // allocation each). Kept while the demand fits a quarter of them no longer than
    // BATCH_SHRINK_FRAMES frames
    auto settle = [&](std::vector<BatchChunk>& chunks, size_t& demand, uint32_t& smallFrames, bool index){
        size_t capacity = 0;
        for (const BatchChunk& chunk : chunks)
            capacity += chunk.capacity;
        const bool merge = chunks.size() > 1 && demand <= BATCH_CHUNK_MAX;
        const bool small = demand * 4 < capacity;
        if (merge || (small && ++smallFrames > BATCH_SHRINK_FRAMES)){
            for (BatchChunk& chunk : chunks)
                chunk.buffer.destroyBuffer();
            chunks.clear();
            if (demand > 0){
                const size_t size = std::min(std::max(demand + demand / 2, index ? BATCH_INDEX_CHUNK : BATCH_VERTEX_CHUNK), BATCH_CHUNK_MAX);
                chunks.emplace_back();
                JE_GPU_ALLOC_SCOPE(Uniforms, "Dynamic batching");
                if (chunks.back().buffer.createBuffer((unsigned int)size, nullptr, index ? BufferType::INDEX_BUFFER : BufferType::VERTEX_BUFFER,
                                                      BufferUsage::STREAM))
                    chunks.back().capacity = size;
                else
                    chunks.pop_back();
            }
            if (small)
                smallFrames = 0;
        }else if (!small){
            smallFrames = 0;
        }
        for (BatchChunk& chunk : chunks)
            chunk.used = 0;
        demand = 0;
    };
    batchVertexCursor = 0;
    batchIndexCursor = 0;
    settle(batchVertexChunks, batchVertexDemand, batchSmallVertexFrames, false);
    settle(batchIndexChunks, batchIndexDemand, batchSmallIndexFrames, true);
}

void RenderSystem::destroyBatchChunks(){
    for (BatchChunk& chunk : batchVertexChunks)
        chunk.buffer.destroyBuffer();
    for (BatchChunk& chunk : batchIndexChunks)
        chunk.buffer.destroyBuffer();
    batchVertexChunks.clear();
    batchIndexChunks.clear();
    batchVertexCursor = 0;
    batchIndexCursor = 0;
    batchVertexDemand = 0;
    batchIndexDemand = 0;
    batchQuadIndices.destroyBuffer();
}

//-----Frames-----

void RenderSystem::beginBatchFrame(){
    batcher.clearFrame();
    batcher.getStats().enabled = Engine::isDynamicBatching();
    if (Engine::isDynamicBatching())
        prepareBatchChunks();
}

void RenderSystem::endBatchFrame(){
    endBatchPass();
    BatchStats& stats = batcher.getStats();
    stats.streamChunks = (uint32_t)(batchVertexChunks.size() + batchIndexChunks.size());
    BatchFrame& frame = batchFrames[SokolSystem::getViewTag()];
    frame.stats = stats;
    frame.breaks = batcher.getBreakLog();
}

const RenderSystem::BatchFrame* RenderSystem::getBatchFrame(uint32_t viewTag) const{
    auto it = batchFrames.find(viewTag);
    return it != batchFrames.end() ? &it->second : nullptr;
}

//-----Shared with the single draws-----

void RenderSystem::refreshSubmeshTextures(Submesh& submesh, bool receiveLights, bool pbrFramebufferOutdated){
    const ShaderLibraryBindings& libraryBindings = submesh.libraryBindings;
    const bool needUpdateFramebuffer = pbrFramebufferOutdated ||
        (!libraryBindings.textures.empty() && shaderTexturesOutdated(libraryBindings, submesh.material.shaderTextures));

    if (submesh.needUpdateTexture || needUpdateFramebuffer){
        ShaderData& shaderData = submesh.shader.get()->shaderData;
        const TextureBindMark bindMark = markTextureBinding();
        if (loadPBRTextures(submesh.material, shaderData, submesh.render, receiveLights) &&
                loadPbrExtTextures(submesh, shaderData, submesh.render) &&
                bindShaderTextures(libraryBindings, submesh.render, submesh.material.shaderTextures)){
            submesh.needUpdateTexture = false;
            submesh.needRebindTexture = false;
            noteTextureBinding(submesh, bindMark);
        }
    }
}

void RenderSystem::refreshDepthTextures(Submesh& submesh){
    if (submesh.needUpdateDepthTexture && (submesh.depthShaderProperties & (1u << 0))){
        ShaderData& depthShaderData = submesh.depthShader.get()->shaderData;
        if (loadDepthTexture(submesh.material, depthShaderData, submesh.depthRender)){
            submesh.needUpdateDepthTexture = false;
        }
    }else if (submesh.needUpdateDepthTexture){
        submesh.needUpdateDepthTexture = false;
    }
}

void RenderSystem::refreshUITexture(UIComponent& ui){
    if (ui.needUpdateTexture || ui.texture.isFramebufferOutdated()){
        ShaderData& shaderData = ui.shader.get()->shaderData;
        ui.texture.setAlphaBorderAuto(true);
        if (TextureRender* textureRender = ui.texture.getRender(&emptyWhite)){
            if (textureRender->isCreated()){
                ui.render.addTexture(shaderData.getTextureIndex(TextureShaderType::UI), ShaderStageType::FRAGMENT, textureRender);
                ui.needUpdateTexture = false;
            }
        }
    }
}

void RenderSystem::uploadUIBuffers(UIComponent& ui){
    if (!ui.needUpdateBuffer)
        return;
    if (ui.buffer.getSize() > ui.gpuBufferBytes || (ui.indices.getCount() > 0 && ui.indices.getSize() > ui.gpuIndicesBytes)){
        // the CPU geometry outgrew the GPU buffers made at load (sokol does not clamp an update):
        // no upload past their end, a reload makes bigger ones; this frame keeps the previous geometry
        if (!ui.needReload)
            Log::warn("A UI element's geometry outgrew its GPU buffer (%zu > %zu bytes): reloading it", ui.buffer.getSize(), ui.gpuBufferBytes);
        ui.needReload = true;
        return;
    }
    // (a batch record baked before sees it, also when a Canvas Group layer uploads it: noteWrite)
    ui.buffer.noteWrite();
    ui.indices.noteWrite();
    ui.buffer.getRender()->updateBuffer(ui.buffer.getSize(), ui.buffer.getData());

    if (ui.indices.getCount() > 0){
        ui.indices.getRender()->updateBuffer(ui.indices.getSize(), ui.indices.getData());
        ui.vertexCount = ui.indices.getCount();
    }else{
        ui.vertexCount = ui.buffer.getCount();
    }

    ui.needUpdateBuffer = false;
}

Vector4 RenderSystem::uiElementColor(const UIComponent& ui, bool inCanvasLayer) const{
    const Vector4 baseColor = ui.vertexColorAbsolute ? Vector4(1.0f, 1.0f, 1.0f, ui.color.w) : ui.color;
    return uiDrawColor(baseColor, inCanvasLayer ? ui.groupLayerAlpha : ui.groupAlpha, ui.drawBlendMode);
}

void RenderSystem::applyMaterialPrefix(ObjectRender& render, int slot, const Material& material, unsigned int size, bool lit, bool neutralColor){
    float prefix[16] = {};
    size = std::min<unsigned int>(size, sizeof(prefix));
    // the shader part is Material's first 16 floats (Material.h)
    std::memcpy(prefix, &material.baseColorFactor, size);
    if (neutralColor){
        prefix[0] = prefix[1] = prefix[2] = prefix[3] = 1.0f;
    }
    if (lit && size > 10 * sizeof(float)){
        for (int c = 8; c < 11; c++)
            prefix[c] *= material.emissiveStrength;
    }
    render.applyUniformBlock(slot, size, prefix);
}
