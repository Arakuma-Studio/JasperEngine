// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Mesh.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"
#include "render/ObjectRender.h"
#include "util/Color.h"
#include "Log.h"
#include "component/MirrorComponent.h"
#include "render/LodFade.h"
#include "component/ReflectionProbeComponent.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/MeshSystem.h"
#include "component/VertexAnimatorComponent.h"
#include "animation/VertexAnimatorRuntime.h"

#include "component/ModelComponent.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace jasperengine;

Mesh::Mesh(Scene* scene): GameObject(scene){
    addComponent<MeshComponent>();
}

Mesh::Mesh(Scene* scene, Entity entity): GameObject(scene, entity){
}

Mesh::~Mesh(){
}

Mesh::Mesh(const Mesh& rhs) = default;
Mesh& Mesh::operator=(const Mesh& rhs) = default;
Mesh::Mesh(Mesh&& rhs) noexcept = default;
Mesh& Mesh::operator=(Mesh&& rhs) noexcept = default;

namespace {
    // the submesh slots a mesh has: submesh 0 always (the slot exists before a generator builds it)
    unsigned int submeshSlots(const MeshComponent& mesh){
        return std::max(mesh.numSubmeshes, 1u);
    }

    // every submesh access of this file: the one place the submesh storage is reached
    Submesh& submeshAt(MeshComponent& mesh, unsigned int submesh){
        return mesh.submeshes[submesh];
    }

    // what a getter of a submesh that does not exist answers
    const Submesh& defaultSubmesh(){
        static const Submesh submesh{};
        return submesh;
    }

    // a part's placement in its model's space: its local matrices up to the model, or, for a
    // part moved out of the model, through world space (none when the model's matrix is singular)
    bool partToModel(Scene* scene, Entity part, Entity model, Matrix4& out){
        if (scene->isParentOf(model, part)){
            Matrix4 matrix;
            for (Entity e = part; e != model && e != NULL_ENTITY; ){
                const Transform& transform = scene->getComponent<Transform>(e);
                matrix = transform.localMatrix * matrix;
                e = transform.parent;
            }
            out = matrix;
            return true;
        }
        const Transform* modelTransform = scene->findComponent<Transform>(model);
        const Transform* partTransform = scene->findComponent<Transform>(part);
        if (!modelTransform || !partTransform || std::fabs(modelTransform->modelMatrix.determinant()) <= FLT_MIN)
            return false;
        out = modelTransform->modelMatrix.inverse() * partTransform->modelMatrix;
        return true;
    }
}

std::vector<Entity> Mesh::getModelParts() const{
    const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    if (!model || model->meshNodesMapping.empty())
        return {};
    return scene->getSystem<MeshSystem>()->getModelParts(entity);
}

std::vector<Mesh::MeshTarget> Mesh::getTargetMeshes() const{
    std::vector<MeshTarget> targets;
    for (Entity part : getModelParts())
        targets.push_back({part, &scene->getComponent<MeshComponent>(part)});
    if (targets.empty())
        targets.push_back({entity, &getComponent<MeshComponent>()});
    return targets;
}

Mesh::MeshTarget Mesh::getFirstMesh() const{
    const std::vector<Entity> parts = getModelParts();
    if (!parts.empty())
        return {parts[0], &scene->getComponent<MeshComponent>(parts[0])};
    return {entity, &getComponent<MeshComponent>()};
}

Mesh::MeshTarget Mesh::findSubmesh(unsigned int& submesh, const char* what) const{
    const std::vector<Entity> parts = getModelParts();
    unsigned int count = 0;
    if (parts.empty()){
        MeshComponent& mesh = getComponent<MeshComponent>();
        if (submesh < submeshSlots(mesh))
            return {entity, &mesh};
        count = mesh.numSubmeshes;
    }else{
        for (Entity part : parts){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(part);
            if (submesh < count + mesh.numSubmeshes){
                submesh -= count;
                return {part, &mesh};
            }
            count += mesh.numSubmeshes;
        }
    }
    if (what){
        Log::error("%s: \"%s\" has no submesh %u (%u submesh%s)", what, scene->getEntityName(entity).c_str(), submesh,
                   count, count == 1 ? "" : "es");
    }
    return {NULL_ENTITY, nullptr};
}

bool Mesh::refuseOnParts(const char* what) const{
    if (getModelParts().empty())
        return false;
    // once per model: a script calling it every frame would flood the Console
    ModelComponent& model = getComponent<ModelComponent>();
    if (!model.refusedPartsCallLogged){
        model.refusedPartsCallLogged = true;
        Log::error("%s: \"%s\" is a model made of parts (one per mesh node): call it on a part",
                   what, scene->getEntityName(entity).c_str());
    }
    return true;
}

bool Mesh::load(){
    const std::vector<Entity> parts = getModelParts();
    if (!parts.empty()){
        bool loaded = true;
        for (Entity part : parts){
            loaded = Mesh(scene, part).load() && loaded;
        }
        return loaded;
    }

    MeshComponent& mesh = getComponent<MeshComponent>();

    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadMesh(entity, mesh, renderSystem->getScenePipelines(), instmesh, terrain);
}

void Mesh::setTexture(const std::string& path){
    for (const MeshTarget& target : getTargetMeshes()){
        Submesh& submesh = submeshAt(*target.mesh, 0);
        submesh.material.baseColorTexture.setPath(path);
        submesh.needUpdateTexture = true;
        scene->markChanged<MeshComponent>(target.entity);
    }
}

void Mesh::setTexture(const std::string& id, TextureData data){
    for (const MeshTarget& target : getTargetMeshes()){
        Submesh& submesh = submeshAt(*target.mesh, 0);
        submesh.material.baseColorTexture.setData(id, data);
        submesh.needUpdateTexture = true;
        scene->markChanged<MeshComponent>(target.entity);
    }
}

void Mesh::setTexture(Framebuffer* framebuffer){
    for (const MeshTarget& target : getTargetMeshes()){
        Submesh& submesh = submeshAt(*target.mesh, 0);
        submesh.material.baseColorTexture.setFramebuffer(framebuffer);
        submesh.needUpdateTexture = true;
        scene->markChanged<MeshComponent>(target.entity);
    }
}

namespace {
    // as the editor's change of material.normalTexture (needUpdateTexture), and a reload when the
    // submesh gains or loses its normal map: the shader variant samples one or not (HAS_NORMAL_MAP)
    void normalTextureChanged(MeshComponent& mesh, unsigned int submesh, bool hadNormalMap){
        submeshAt(mesh, submesh).needUpdateTexture = true;
        if (hadNormalMap != !submeshAt(mesh, submesh).material.normalTexture.empty())
            mesh.needReload = true;
    }

    void setNormalTexturePath(MeshComponent& mesh, unsigned int submesh, const std::string& path){
        Texture& texture = submeshAt(mesh, submesh).material.normalTexture;
        const bool hadNormalMap = !texture.empty();
        if (path.empty()){
            if (!hadNormalMap)
                return;
            texture = Texture();
        }else{
            if (hadNormalMap && texture.getPath() == path)
                return;
            texture.setPath(path);
        }
        normalTextureChanged(mesh, submesh, hadNormalMap);
    }

    // the editor's material comparison: the shader, its features, its render state and which
    // custom textures it has are baked when the mesh loads; parameters only rewrite the uniform blocks
    void applySubmeshMaterial(Scene* scene, Entity entity, MeshComponent& mesh, unsigned int submesh, const Material& material){
        if (submeshAt(mesh, submesh).material == material)
            return;
        const Material& current = submeshAt(mesh, submesh).material;
        bool reload = current.shader != material.shader || current.shaderFeatures != material.shaderFeatures ||
            current.blendMode != material.blendMode || current.cullMode != material.cullMode ||
            current.depthWrite != material.depthWrite || current.depthTest != material.depthTest ||
            current.depthOffsetFactor != material.depthOffsetFactor || current.depthOffsetUnits != material.depthOffsetUnits ||
            current.sortPriority != material.sortPriority || current.lighting != material.lighting ||
            current.stencil != material.stencil || current.colorWrite != material.colorWrite ||
            current.stochasticTiling != material.stochasticTiling ||
            current.shaderTextures.size() != material.shaderTextures.size();
        for (size_t t = 0; !reload && t < material.shaderTextures.size(); t++){
            reload = current.shaderTextures[t].first != material.shaderTextures[t].first;
        }
        // so is all of the extra pass but its parameters and texture files
        const MaterialPass& currentPass = *current.extraPass;
        const MaterialPass& pass = *material.extraPass;
        reload = reload || currentPass.shader != pass.shader || currentPass.shaderFeatures != pass.shaderFeatures ||
            currentPass.blendMode != pass.blendMode || currentPass.cullMode != pass.cullMode ||
            currentPass.depthWrite != pass.depthWrite || currentPass.depthTest != pass.depthTest ||
            currentPass.depthOffsetFactor != pass.depthOffsetFactor || currentPass.depthOffsetUnits != pass.depthOffsetUnits ||
            currentPass.stencil != pass.stencil || currentPass.colorWrite != pass.colorWrite || currentPass.order != pass.order ||
            currentPass.shaderTextures.size() != pass.shaderTextures.size();
        for (size_t t = 0; !reload && t < pass.shaderTextures.size(); t++){
            reload = currentPass.shaderTextures[t].first != pass.shaderTextures[t].first;
        }
        const bool paramsChanged = current.shaderParams != material.shaderParams || currentPass.shaderParams != pass.shaderParams ||
            current.shaderGlobalBindings != material.shaderGlobalBindings;

        submeshAt(mesh, submesh).material = material;
        submeshAt(mesh, submesh).needUpdateTexture = true;
        scene->markChanged<MeshComponent>(entity);
        if (reload)
            mesh.needReload = true;
        if (paramsChanged){
            mesh.needUpdateShaderUniforms = true;
        }
    }
}

void Mesh::setNormalTexture(const std::string& path){
    for (const MeshTarget& target : getTargetMeshes()){
        setNormalTexturePath(*target.mesh, 0, path);
    }
}

void Mesh::setNormalTexture(unsigned int submesh, const std::string& path){
    MeshTarget target = findSubmesh(submesh, "Cannot set the normal map");
    if (!target.mesh)
        return;
    setNormalTexturePath(*target.mesh, submesh, path);
}

void Mesh::setNormalTexture(const std::string& id, TextureData data){
    for (const MeshTarget& target : getTargetMeshes()){
        Texture& texture = submeshAt(*target.mesh, 0).material.normalTexture;
        const bool hadNormalMap = !texture.empty();
        // a fresh texture (no path left from a file), with the sampler the old one had
        Texture fromData;
        fromData.setMinFilter(texture.getMinFilter());
        fromData.setMagFilter(texture.getMagFilter());
        fromData.setWrapU(texture.getWrapU());
        fromData.setWrapV(texture.getWrapV());
        fromData.setData(id, data);
        texture = fromData;
        normalTextureChanged(*target.mesh, 0, hadNormalMap);
    }
}

std::string Mesh::getNormalTexture() const{
    MeshTarget target = getFirstMesh();
    const Texture& texture = submeshAt(*target.mesh, 0).material.normalTexture;
    if (texture.empty())
        return "";
    const std::string path = texture.getPath();
    return path.empty() ? texture.getId() : path;
}

std::string Mesh::getNormalTexture(unsigned int submesh) const{
    MeshTarget target = findSubmesh(submesh, nullptr);
    if (!target.mesh)
        return "";

    const Texture& texture = submeshAt(*target.mesh, submesh).material.normalTexture;
    if (texture.empty())
        return "";
    const std::string path = texture.getPath();
    return path.empty() ? texture.getId() : path;
}

void Mesh::setColor(Vector4 color){
    const Vector4 linear = Color::sRGBToLinear(color);
    for (const MeshTarget& target : getTargetMeshes()){
        submeshAt(*target.mesh, 0).material.baseColorFactor = linear;
    }
}

void Mesh::setColor(const float red, const float green, const float blue, const float alpha){
    setColor(Vector4(red, green, blue, alpha));
}

void Mesh::setColor(const float red, const float green, const float blue){
    setColor(Vector4(red, green, blue, getColor().w));
}

void Mesh::setAlpha(const float alpha){
    Vector4 color = getColor();
    setColor(Vector4(color.x, color.y, color.z, alpha));
}

Vector4 Mesh::getColor() const{
    return Color::linearTosRGB(submeshAt(*getFirstMesh().mesh, 0).material.baseColorFactor);
}

float Mesh::getAlpha() const{
    return getColor().w;
}

void Mesh::setMaterial(const Material& material){
    for (const MeshTarget& target : getTargetMeshes()){
        applySubmeshMaterial(scene, target.entity, *target.mesh, 0, material);
    }
}

Material Mesh::getMaterial() const{
    return submeshAt(*getFirstMesh().mesh, 0).material;
}

void Mesh::setMaterial(unsigned int submesh, const Material& material){
    MeshTarget target = findSubmesh(submesh, "Cannot set the material");
    if (!target.mesh)
        return;
    applySubmeshMaterial(scene, target.entity, *target.mesh, submesh, material);
}

Material Mesh::getMaterial(unsigned int submesh) const{
    MeshTarget target = findSubmesh(submesh, "Cannot get the material");
    if (!target.mesh)
        return defaultSubmesh().material;
    return submeshAt(*target.mesh, submesh).material;
}

namespace {
    void applyPrimitiveType(MeshComponent& mesh, unsigned int submesh, PrimitiveType primitiveType){
        if (submeshAt(mesh, submesh).primitiveType != primitiveType){
            submeshAt(mesh, submesh).primitiveType = primitiveType;
            mesh.needReload = true;
        }
    }

    void applyFaceCulling(MeshComponent& mesh, unsigned int submesh, bool faceCulling){
        if (submeshAt(mesh, submesh).faceCulling != faceCulling){
            submeshAt(mesh, submesh).faceCulling = faceCulling;
            mesh.needReload = true;
        }
    }

    void applyTextureShadow(MeshComponent& mesh, unsigned int submesh, bool castShadowsWithTexture){
        submeshAt(mesh, submesh).textureShadow = castShadowsWithTexture;
        mesh.needReload = true;
    }
}

void Mesh::setPrimitiveType(PrimitiveType primitiveType){
    for (const MeshTarget& target : getTargetMeshes()){
        applyPrimitiveType(*target.mesh, 0, primitiveType);
    }
}

PrimitiveType Mesh::getPrimitiveType() const{
    return submeshAt(*getFirstMesh().mesh, 0).primitiveType;
}

void Mesh::setPrimitiveType(unsigned int submesh, PrimitiveType primitiveType){
    MeshTarget target = findSubmesh(submesh, "Cannot set the primitive type");
    if (target.mesh)
        applyPrimitiveType(*target.mesh, submesh, primitiveType);
}

PrimitiveType Mesh::getPrimitiveType(unsigned int submesh) const{
    MeshTarget target = findSubmesh(submesh, "Cannot get the primitive type");
    return target.mesh ? submeshAt(*target.mesh, submesh).primitiveType : defaultSubmesh().primitiveType;
}

void Mesh::setFaceCulling(bool faceCulling){
    for (const MeshTarget& target : getTargetMeshes()){
        applyFaceCulling(*target.mesh, 0, faceCulling);
    }
}

bool Mesh::isFaceCulling() const{
    return submeshAt(*getFirstMesh().mesh, 0).faceCulling;
}

void Mesh::setFaceCulling(unsigned int submesh, bool faceCulling){
    MeshTarget target = findSubmesh(submesh, "Cannot set face culling");
    if (target.mesh)
        applyFaceCulling(*target.mesh, submesh, faceCulling);
}

bool Mesh::isFaceCulling(unsigned int submesh) const{
    MeshTarget target = findSubmesh(submesh, "Cannot get face culling");
    return target.mesh ? submeshAt(*target.mesh, submesh).faceCulling : defaultSubmesh().faceCulling;
}

void Mesh::setCastShadowsWithTexture(bool castShadowsWithTexture){
    for (const MeshTarget& target : getTargetMeshes()){
        applyTextureShadow(*target.mesh, 0, castShadowsWithTexture);
    }
}

bool Mesh::isCastShadowsWithTexture() const{
    return submeshAt(*getFirstMesh().mesh, 0).textureShadow;
}

void Mesh::setCastShadowsWithTexture(unsigned int submesh, bool castShadowsWithTexture){
    MeshTarget target = findSubmesh(submesh, "Cannot set castShadowsWithTexture");
    if (target.mesh)
        applyTextureShadow(*target.mesh, submesh, castShadowsWithTexture);
}

bool Mesh::isCastShadowsWithTexture(unsigned int submesh) const{
    MeshTarget target = findSubmesh(submesh, "Cannot get castShadowsWithTexture");
    return target.mesh ? submeshAt(*target.mesh, submesh).textureShadow : defaultSubmesh().textureShadow;
}

namespace {
    // a mesh-wide field of every target mesh, and the reload it needs when it changes
    template <typename Targets, typename T, typename V>
    void setOnAll(const Targets& targets, T MeshComponent::*field, const V& value, bool reloads){
        for (const auto& target : targets){
            if (target.mesh->*field != value){
                target.mesh->*field = value;
                if (reloads)
                    target.mesh->needReload = true;
            }
        }
    }
}

void Mesh::setCullingMode(CullingMode cullingMode){
    setOnAll(getTargetMeshes(), &MeshComponent::cullingMode, cullingMode, true);
}

CullingMode Mesh::getCullingMode() const{
    return getFirstMesh().mesh->cullingMode;
}

void Mesh::setWindingOrder(WindingOrder windingOrder){
    setOnAll(getTargetMeshes(), &MeshComponent::windingOrder, windingOrder, true);
}

WindingOrder Mesh::getWindingOrder() const{
    return getFirstMesh().mesh->windingOrder;
}

AABB Mesh::getAABB() const{
    const std::vector<Entity> parts = getModelParts();
    if (parts.empty())
        return getComponent<MeshComponent>().aabb;
    AABB aabb;
    for (Entity part : parts){
        Matrix4 matrix;
        if (partToModel(scene, part, entity, matrix))
            aabb.merge(matrix * scene->getComponent<MeshComponent>(part).aabb);
    }
    return aabb;
}

AABB Mesh::getVerticesAABB() const{
    const std::vector<Entity> parts = getModelParts();
    if (parts.empty())
        return getComponent<MeshComponent>().verticesAABB;
    AABB aabb;
    for (Entity part : parts){
        Matrix4 matrix;
        if (partToModel(scene, part, entity, matrix))
            aabb.merge(matrix * scene->getComponent<MeshComponent>(part).verticesAABB);
    }
    return aabb;
}

AABB Mesh::getWorldAABB() const{
    const std::vector<Entity> parts = getModelParts();
    if (parts.empty())
        return getComponent<MeshComponent>().worldAABB;
    AABB aabb;
    for (Entity part : parts){
        aabb.merge(scene->getComponent<MeshComponent>(part).worldAABB);
    }
    return aabb;
}

unsigned int Mesh::getNumSubmeshes() const{
    const std::vector<Entity> parts = getModelParts();
    if (parts.empty())
        return getComponent<MeshComponent>().numSubmeshes;
    unsigned int count = 0;
    for (Entity part : parts)
        count += scene->getComponent<MeshComponent>(part).numSubmeshes;
    return count;
}

unsigned int Mesh::getVertexCount() const{
    unsigned int count = 0;
    for (const MeshTarget& target : getTargetMeshes())
        count += target.mesh->vertexCount;
    return count;
}

void Mesh::setReceiveLights(bool receiveLights){
    setOnAll(getTargetMeshes(), &MeshComponent::receiveLights, receiveLights, true);
}

bool Mesh::isReceiveLights() const{
    return getFirstMesh().mesh->receiveLights;
}

void Mesh::setReceiveIBL(bool receiveIBL){
    setOnAll(getTargetMeshes(), &MeshComponent::receiveIBL, receiveIBL, true);
}

bool Mesh::isReceiveIBL() const{
    return getFirstMesh().mesh->receiveIBL;
}

void Mesh::setCastShadows(bool castShadows){
    setOnAll(getTargetMeshes(), &MeshComponent::castShadows, castShadows, false);
}

bool Mesh::isCastShadows() const{
    return getFirstMesh().mesh->castShadows;
}

void Mesh::setStaticShadowCaster(bool staticShadowCaster){
    setOnAll(getTargetMeshes(), &MeshComponent::staticShadowCaster, staticShadowCaster, false);
}

bool Mesh::isStaticShadowCaster() const{
    return getFirstMesh().mesh->staticShadowCaster;
}

void Mesh::setLodEnabled(bool lodEnabled){
    setOnAll(getTargetMeshes(), &MeshComponent::lodEnabled, lodEnabled, true);
}

bool Mesh::isLodEnabled() const{
    return getFirstMesh().mesh->lodEnabled;
}

void Mesh::setLodBias(float lodBias){
    setOnAll(getTargetMeshes(), &MeshComponent::lodBias, lodBias, false);
}

float Mesh::getLodBias() const{
    return getFirstMesh().mesh->lodBias;
}

void Mesh::setAllowGpuInstancing(bool allow){
    // its batch variants are loaded with it
    setOnAll(getTargetMeshes(), &MeshComponent::allowGpuInstancing, allow, true);
}

bool Mesh::isAllowGpuInstancing() const{
    return getFirstMesh().mesh->allowGpuInstancing;
}

void Mesh::setLodFadeMode(LodFadeMode mode){
    // a mesh that may fade loads its fade shaders (RenderSystem::prepareLodFade)
    setOnAll(getTargetMeshes(), &MeshComponent::lodFadeMode, mode, true);
}

LodFadeMode Mesh::getLodFadeMode() const{
    return getFirstMesh().mesh->lodFadeMode;
}

void Mesh::setLodAnimateCrossFading(bool animate){
    setOnAll(getTargetMeshes(), &MeshComponent::lodAnimateCrossFading, animate, false);
}

bool Mesh::isLodAnimateCrossFading() const{
    return getFirstMesh().mesh->lodAnimateCrossFading;
}

void Mesh::setLodFadeTransitionWidth(float width){
    if (std::isfinite(width))
        setOnAll(getTargetMeshes(), &MeshComponent::lodFadeTransitionWidth, std::clamp(width, 0.0f, 1.0f), false);
}

float Mesh::getLodFadeTransitionWidth() const{
    return getFirstMesh().mesh->lodFadeTransitionWidth;
}

void Mesh::setLodCullBelow(float share){
    if (std::isfinite(share))
        setOnAll(getTargetMeshes(), &MeshComponent::lodCullBelow, std::clamp(share, 0.0f, 1.0f), false);
}

float Mesh::getLodCullBelow() const{
    return getFirstMesh().mesh->lodCullBelow;
}

void Mesh::forceLod(int level){
    setOnAll(getTargetMeshes(), &MeshComponent::lodForced, std::clamp(level, -1, MAX_MESH_LODS - 1), false);
}

int Mesh::getForcedLod() const{
    return getFirstMesh().mesh->lodForced;
}

int Mesh::getCurrentLod() const{
    return getFirstMesh().mesh->lodFade.plan.dominant();
}

void Mesh::setReceiveShadows(bool receiveShadows){
    setOnAll(getTargetMeshes(), &MeshComponent::receiveShadows, receiveShadows, true);
}

bool Mesh::isReceiveShadows() const{
    return getFirstMesh().mesh->receiveShadows;
}

void Mesh::setRenderInReflectionProbes(bool renderInReflectionProbes){
    bool changed = false;
    for (const MeshTarget& target : getTargetMeshes()){
        if (target.mesh->renderInReflectionProbes != renderInReflectionProbes){
            target.mesh->renderInReflectionProbes = renderInReflectionProbes;
            changed = true;
        }
    }
    if (changed){
        // baked cubemaps keep the old content until the probes capture again
        auto probes = scene->getComponentArray<ReflectionProbeComponent>();
        for (int i = 0; i < probes->size(); i++){
            ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
            probe.needUpdate = true;
            probe.captureRevision++;
        }
    }
}

bool Mesh::isRenderInReflectionProbes() const{
    return getFirstMesh().mesh->renderInReflectionProbes;
}

void Mesh::setShadowsBillboard(bool shadowsBillboard){
    setOnAll(getTargetMeshes(), &MeshComponent::shadowsBillboard, shadowsBillboard, false);
}

bool Mesh::isShadowsBillboard() const{
    return getFirstMesh().mesh->shadowsBillboard;
}

void Mesh::setTransparent(bool transparent){
    setOnAll(getTargetMeshes(), &MeshComponent::transparent, transparent, false);
}

bool Mesh::isTransparent() const{
    return getFirstMesh().mesh->transparent;
}

void Mesh::setAutoTransparency(bool autoTransparency){
    setOnAll(getTargetMeshes(), &MeshComponent::autoTransparency, autoTransparency, false);
}

bool Mesh::isAutoTransparency() const{
    return getFirstMesh().mesh->autoTransparency;
}

void Mesh::setCustomShader(const std::string& path){
    setOnAll(getTargetMeshes(), &MeshComponent::customShader, path, true);
}

std::string Mesh::getCustomShader() const{
    return getFirstMesh().mesh->customShader;
}

void Mesh::setCustomDepthShader(const std::string& path){
    setOnAll(getTargetMeshes(), &MeshComponent::customDepthShader, path, true);
}

std::string Mesh::getCustomDepthShader() const{
    return getFirstMesh().mesh->customDepthShader;
}

void Mesh::setShaderUniform(const std::string& name, const Vector4& value){
    for (const MeshTarget& target : getTargetMeshes()){
        if (ShaderUniforms::set(target.mesh->shaderUniforms, name, value)){
            target.mesh->needUpdateShaderUniforms = true;
            scene->markChanged<MeshComponent>(target.entity);
        }
    }
}

void Mesh::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void Mesh::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void Mesh::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 Mesh::getShaderUniform(const std::string& name) const{
    return ShaderUniforms::get(getFirstMesh().mesh->shaderUniforms, name);
}

bool Mesh::removeShaderUniform(const std::string& name){
    bool removed = false;
    for (const MeshTarget& target : getTargetMeshes()){
        if (ShaderUniforms::remove(target.mesh->shaderUniforms, name)){
            target.mesh->needUpdateShaderUniforms = true;
            scene->markChanged<MeshComponent>(target.entity);
            removed = true;
        }
    }
    return removed;
}

void Mesh::setAsMirror(){
    // default normal +Z matches a Wall surface; flip for differently-oriented meshes
    setAsMirror(Vector3(0, 0, 1));
}

void Mesh::setAsMirror(Vector3 normal){
    if (refuseOnParts("setAsMirror"))
        return;
    // turn this (flat) mesh into a planar reflection surface. The engine creates and
    // drives the reflection camera internally and feeds it to the base texture.
    MirrorComponent* mirror = scene->findComponent<MirrorComponent>(entity);
    if (mirror){
        mirror->normal = normal;
    }else{
        MirrorComponent mirrorComp;
        mirrorComp.normal = normal;
        addComponent<MirrorComponent>(mirrorComp);
    }
}

void Mesh::removeMirror(){
    if (scene->findComponent<MirrorComponent>(entity)){
        removeComponent<MirrorComponent>();
    }
}

bool Mesh::isMirror() const{
    return scene->findComponent<MirrorComponent>(entity) != NULL;
}


void Mesh::createInstancedMesh(){
    if (refuseOnParts("createInstancedMesh"))
        return;
    scene->getSystem<MeshSystem>()->createInstancedMesh(entity);
}

void Mesh::removeInstancedMesh(){
    scene->getSystem<MeshSystem>()->removeInstancedMesh(entity);
}

bool Mesh::hasInstancedMesh() const{
    return scene->getSystem<MeshSystem>()->hasInstancedMesh(entity);
}

void Mesh::setInstancedBillboard(bool billboard, bool cylindrical){
    if (refuseOnParts("setInstancedBillboard"))
        return;
    createInstancedMesh();
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    instmesh.instancedBillboard = billboard;
    instmesh.instancedCylindricalBillboard = cylindrical;

    instmesh.needUpdateInstances = true;
    markChanged<InstancedMeshComponent>();
}

void Mesh::setInstancedBillboard(bool billboard){
    if (refuseOnParts("setInstancedBillboard"))
        return;
    createInstancedMesh();
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    if (instmesh.instancedBillboard != billboard){
        instmesh.instancedBillboard = billboard;

        instmesh.needUpdateInstances = true;
        markChanged<InstancedMeshComponent>();
    }
}

bool Mesh::isInstancedBillboard() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.instancedBillboard;
    }

    return false;
}

void Mesh::setInstancedCylindricalBillboard(bool cylindricalBillboard){
    if (refuseOnParts("setInstancedCylindricalBillboard"))
        return;
    createInstancedMesh();
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    if (instmesh.instancedCylindricalBillboard != cylindricalBillboard){
        instmesh.instancedCylindricalBillboard = cylindricalBillboard;

        instmesh.needUpdateInstances = true;
        markChanged<InstancedMeshComponent>();
    }
}

bool Mesh::isInstancedCylindricalBillboard() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.instancedCylindricalBillboard;
    }

    return false;
}

void Mesh::setMaxInstances(unsigned int maxInstances){
    if (hasInstancedMesh()){
        MeshComponent& mesh = getComponent<MeshComponent>();
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        if (instmesh.maxInstances != maxInstances){
            instmesh.maxInstances = maxInstances;

            mesh.needReload = true;
        }
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

unsigned int Mesh::getMaxInstances() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.maxInstances;
    }

    Log::error("There is no instanced mesh component in this mesh");
    return 0;
}

void Mesh::setDistanceFade(bool distanceFade){
    if (hasInstancedMesh()){
        MeshComponent& mesh = getComponent<MeshComponent>();
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        if (instmesh.distanceFade != distanceFade){
            instmesh.distanceFade = distanceFade;

            mesh.needReload = true;
        }
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

bool Mesh::isDistanceFade() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.distanceFade;
    }

    return false;
}

void Mesh::setFadeRange(float fadeStart, float fadeEnd){
    setFadeStart(fadeStart);
    setFadeEnd(fadeEnd);
}

void Mesh::setCullDistance(float distance){
    if (hasInstancedMesh()){
        getComponent<InstancedMeshComponent>().cullDistance = distance;
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

float Mesh::getCullDistance() const{
    return hasInstancedMesh() ? getComponent<InstancedMeshComponent>().cullDistance : 0.0f;
}

void Mesh::setFadeStart(float fadeStart){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        instmesh.fadeStart = fadeStart;
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

float Mesh::getFadeStart() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.fadeStart;
    }

    return 0;
}

void Mesh::setFadeEnd(float fadeEnd){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        instmesh.fadeEnd = fadeEnd;
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

float Mesh::getFadeEnd() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.fadeEnd;
    }

    return 0;
}

void Mesh::setCullInstances(bool cullInstances){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        instmesh.cullInstances = cullInstances;
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

bool Mesh::isCullInstances() const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.cullInstances;
    }

    return false;
}

void Mesh::addInstance(InstanceData instance){
    if (refuseOnParts("addInstance"))
        return;
    createInstancedMesh();

    MeshComponent& mesh = getComponent<MeshComponent>();
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    instmesh.instances.push_back(instance);

    if (instmesh.maxInstances < instmesh.instances.size()){
        instmesh.maxInstances = instmesh.maxInstances * 2;
        mesh.needReload = true;
    }

    instmesh.needUpdateInstances = true;
    markChanged<InstancedMeshComponent>();
}

void Mesh::addInstance(Vector3 position){
    InstanceData instance = {};

    instance.position = position;

    addInstance(instance);
}

void Mesh::addInstance(float x, float y, float z){
    addInstance(Vector3(x, y, z));
}

void Mesh::addInstance(Vector3 position, Quaternion rotation, Vector3 scale){
    InstanceData instance = {};

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;

    addInstance(instance);
}

void Mesh::addInstance(Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color){
    InstanceData instance = {};

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.color = color;

    addInstance(instance);
}

void Mesh::addInstance(Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color, Rect textureRect){
    InstanceData instance = {};

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.color = color;
    instance.textureRect = textureRect;

    addInstance(instance);
}

InstanceData& Mesh::getInstance(size_t index){
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    return instmesh.instances.at(index);
}

void Mesh::updateInstance(size_t index, InstanceData instance){
    if (refuseOnParts("updateInstance"))
        return;
    createInstancedMesh();

    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    InstanceData& current = instmesh.instances.at(index);
    // the Vertex Animator's state of the instance is runtime state, not user data: it stays (a
    // new InstanceData() from a script would draw the instance at rest)
    instance.vat0 = current.vat0;
    instance.vat1 = current.vat1;
    current = instance;

    instmesh.needUpdateInstances = true;
    markChanged<InstancedMeshComponent>();
}

void Mesh::updateInstance(size_t index, Vector3 position){
    InstanceData& instance = getInstance(index);

    instance.position = position;

    updateInstance(index, instance);
}

void Mesh::updateInstance(size_t index, float x, float y, float z){
    updateInstance(index, Vector3(x, y, z));
}

void Mesh::updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale){
    InstanceData& instance = getInstance(index);

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;

    updateInstance(index, instance);
}

void Mesh::updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color){
    InstanceData& instance = getInstance(index);

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.color = color;

    updateInstance(index, instance);
}

void Mesh::updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color, Rect textureRect){
    InstanceData& instance = getInstance(index);

    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.color = color;
    instance.textureRect = textureRect;

    updateInstance(index, instance);
}

void Mesh::removeInstance(size_t index){
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    instmesh.instances.erase(instmesh.instances.begin() + index);
    // a Vertex Animator's per-instance states stay with their instances
    if (VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(entity)){
        vat::eraseInstanceState(*animator, index);
    }

    instmesh.needUpdateInstances = true;
    markChanged<InstancedMeshComponent>();
}

bool Mesh::isInstanceVisible(size_t index){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.instances.at(index).visible;
    }

    return false;
}

void Mesh::setInstanceVisible(size_t index, bool visible) const{
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        if (instmesh.instances.at(index).visible != visible){
            instmesh.instances.at(index).visible = visible;

            instmesh.needUpdateInstances = true;
            markChanged<InstancedMeshComponent>();
        }
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

void Mesh::updateInstances(){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        instmesh.needUpdateInstances = true;
        markChanged<InstancedMeshComponent>();
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

size_t Mesh::getNumInstances(){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        return instmesh.instances.size();
    }

    return 0;
}

void Mesh::clearInstances(){
    if (hasInstancedMesh()){
        InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

        instmesh.instances.clear();
        if (VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(entity)){
            vat::clearInstanceStates(*animator);
        }

        instmesh.needUpdateInstances = true;
        markChanged<InstancedMeshComponent>();
    }else{
        Log::error("There is no instanced mesh component in this mesh");
    }
}

void Mesh::setEnabled(bool enabled){
    getComponent<MeshComponent>().enabled = enabled;
}

bool Mesh::isEnabled() const{
    return getComponent<MeshComponent>().enabled;
}
