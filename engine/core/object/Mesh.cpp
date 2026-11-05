// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Mesh.h"
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

#include <algorithm>
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

bool Mesh::load(){
    MeshComponent& mesh = getComponent<MeshComponent>();

    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadMesh(entity, mesh, renderSystem->getScenePipelines(), instmesh, terrain);
}

void Mesh::setTexture(const std::string& path){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.submeshes[0].material.baseColorTexture.setPath(path);

    mesh.submeshes[0].needUpdateTexture = true;
    markChanged<MeshComponent>();
}

void Mesh::setTexture(const std::string& id, TextureData data){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.submeshes[0].material.baseColorTexture.setData(id, data);

    mesh.submeshes[0].needUpdateTexture = true;
    markChanged<MeshComponent>();
}

void Mesh::setTexture(Framebuffer* framebuffer){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.submeshes[0].material.baseColorTexture.setFramebuffer(framebuffer);

    mesh.submeshes[0].needUpdateTexture = true;
    markChanged<MeshComponent>();
}

namespace {
    // a submesh the mesh has: submesh 0 always (the slot exists before a generator builds it)
    bool validNormalSubmesh(const MeshComponent& mesh, unsigned int submesh){
        const unsigned int count = std::max(mesh.numSubmeshes, 1u);
        if (submesh >= count){
            Log::error("Cannot set the normal map of submesh %u: the mesh has %u submesh%s", submesh, mesh.numSubmeshes,
                       mesh.numSubmeshes == 1 ? "" : "es");
            return false;
        }
        return true;
    }

    // as the editor's change of material.normalTexture (needUpdateTexture), and a reload when the
    // submesh gains or loses its normal map: the shader variant samples one or not (HAS_NORMAL_MAP)
    void normalTextureChanged(MeshComponent& mesh, unsigned int submesh, bool hadNormalMap){
        mesh.submeshes[submesh].needUpdateTexture = true;
        if (hadNormalMap != !mesh.submeshes[submesh].material.normalTexture.empty())
            mesh.needReload = true;
    }
}

void Mesh::setNormalTexture(const std::string& path){
    setNormalTexture(0, path);
}

void Mesh::setNormalTexture(unsigned int submesh, const std::string& path){
    MeshComponent& mesh = getComponent<MeshComponent>();
    if (!validNormalSubmesh(mesh, submesh))
        return;

    Texture& texture = mesh.submeshes[submesh].material.normalTexture;
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

void Mesh::setNormalTexture(const std::string& id, TextureData data){
    MeshComponent& mesh = getComponent<MeshComponent>();

    Texture& texture = mesh.submeshes[0].material.normalTexture;
    const bool hadNormalMap = !texture.empty();
    // a fresh texture (no path left from a file), with the sampler the old one had
    Texture fromData;
    fromData.setMinFilter(texture.getMinFilter());
    fromData.setMagFilter(texture.getMagFilter());
    fromData.setWrapU(texture.getWrapU());
    fromData.setWrapV(texture.getWrapV());
    fromData.setData(id, data);
    texture = fromData;
    normalTextureChanged(mesh, 0, hadNormalMap);
}

std::string Mesh::getNormalTexture() const{
    return getNormalTexture(0);
}

std::string Mesh::getNormalTexture(unsigned int submesh) const{
    MeshComponent& mesh = getComponent<MeshComponent>();
    if (submesh >= std::max(mesh.numSubmeshes, 1u))
        return "";

    const Texture& texture = mesh.submeshes[submesh].material.normalTexture;
    if (texture.empty())
        return "";
    const std::string path = texture.getPath();
    return path.empty() ? texture.getId() : path;
}

void Mesh::setColor(Vector4 color){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.submeshes[0].material.baseColorFactor = Color::sRGBToLinear(color);
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
    MeshComponent& mesh = getComponent<MeshComponent>();

    return Color::linearTosRGB(mesh.submeshes[0].material.baseColorFactor);
}

float Mesh::getAlpha() const{
    return getColor().w;
}

void Mesh::setMaterial(const Material& material){
    setMaterial(0, material);
}

Material Mesh::getMaterial() const{
    return getMaterial(0);
}

void Mesh::setMaterial(unsigned int submesh, const Material& material){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.submeshes[submesh].material != material){
        const Material& current = mesh.submeshes[submesh].material;
        // the shader, its features, its render state and which custom textures it has
        // are baked when the mesh loads; parameters only rewrite the uniform blocks
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

        mesh.submeshes[submesh].material = material;
        mesh.submeshes[submesh].needUpdateTexture = true;
        markChanged<MeshComponent>();
        if (reload)
            mesh.needReload = true;
        if (paramsChanged){
            mesh.needUpdateShaderUniforms = true;
            markChanged<MeshComponent>();
        }
    }
}

Material Mesh::getMaterial(unsigned int submesh) const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.submeshes[submesh].material;
}

void Mesh::setPrimitiveType(PrimitiveType primitiveType){
    setPrimitiveType(0, primitiveType);
}

PrimitiveType Mesh::getPrimitiveType() const{
    return getPrimitiveType(0);
}

void Mesh::setPrimitiveType(unsigned int submesh, PrimitiveType primitiveType){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.submeshes[submesh].primitiveType != primitiveType){
        mesh.submeshes[submesh].primitiveType = primitiveType;

        mesh.needReload = true;
    }
}

PrimitiveType Mesh::getPrimitiveType(unsigned int submesh) const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.submeshes[submesh].primitiveType;
}

void Mesh::setFaceCulling(bool faceCulling){
    setFaceCulling(0, faceCulling);
}

bool Mesh::isFaceCulling() const{
    return isFaceCulling(0);
}

void Mesh::setFaceCulling(unsigned int submesh, bool faceCulling){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.submeshes[submesh].faceCulling != faceCulling){
        mesh.submeshes[submesh].faceCulling = faceCulling;

        mesh.needReload = true;
    }
}

bool Mesh::isFaceCulling(unsigned int submesh) const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.submeshes[submesh].faceCulling;
}


void Mesh::setCastShadowsWithTexture(bool castShadowsWithTexture){
    setCastShadowsWithTexture(0, castShadowsWithTexture);
}

bool Mesh::isCastShadowsWithTexture() const{
    return isCastShadowsWithTexture(0);
}

void Mesh::setCastShadowsWithTexture(unsigned int submesh, bool castShadowsWithTexture){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.submeshes[submesh].textureShadow = castShadowsWithTexture;

    mesh.needReload = true;
}

bool Mesh::isCastShadowsWithTexture(unsigned int submesh) const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.submeshes[submesh].textureShadow;
}

void Mesh::setCullingMode(CullingMode cullingMode){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.cullingMode != cullingMode){
        mesh.cullingMode = cullingMode;

        mesh.needReload = true;
    }
}

CullingMode Mesh::getCullingMode() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.cullingMode;
}

void Mesh::setWindingOrder(WindingOrder windingOrder){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.windingOrder != windingOrder){
        mesh.windingOrder = windingOrder;

        mesh.needReload = true;
    }
}

WindingOrder Mesh::getWindingOrder() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.windingOrder;
}


AABB Mesh::getAABB() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.aabb;
}

AABB Mesh::getVerticesAABB() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.verticesAABB;
}

AABB Mesh::getWorldAABB() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.worldAABB;
}

unsigned int Mesh::getNumSubmeshes() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.numSubmeshes;
}

void Mesh::setReceiveLights(bool receiveLights){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.receiveLights != receiveLights){
        mesh.receiveLights = receiveLights;

        mesh.needReload = true;
    }
}

bool Mesh::isReceiveLights() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.receiveLights;
}

void Mesh::setReceiveIBL(bool receiveIBL){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.receiveIBL != receiveIBL){
        mesh.receiveIBL = receiveIBL;

        mesh.needReload = true;
    }
}

bool Mesh::isReceiveIBL() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.receiveIBL;
}

void Mesh::setCastShadows(bool castShadows){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.castShadows = castShadows;
}

bool Mesh::isCastShadows() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.castShadows;
}

void Mesh::setStaticShadowCaster(bool staticShadowCaster){
    getComponent<MeshComponent>().staticShadowCaster = staticShadowCaster;
}

bool Mesh::isStaticShadowCaster() const{
    return getComponent<MeshComponent>().staticShadowCaster;
}

void Mesh::setLodEnabled(bool lodEnabled){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.lodEnabled != lodEnabled){
        mesh.lodEnabled = lodEnabled;

        mesh.needReload = true;
    }
}

bool Mesh::isLodEnabled() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.lodEnabled;
}

void Mesh::setLodBias(float lodBias){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.lodBias = lodBias;
}

float Mesh::getLodBias() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.lodBias;
}

void Mesh::setAllowGpuInstancing(bool allow){
    MeshComponent& mesh = getComponent<MeshComponent>();

    // its batch variants are loaded with it
    if (mesh.allowGpuInstancing != allow){
        mesh.allowGpuInstancing = allow;

        mesh.needReload = true;
    }
}

bool Mesh::isAllowGpuInstancing() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.allowGpuInstancing;
}

void Mesh::setLodFadeMode(LodFadeMode mode){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.lodFadeMode != mode){
        mesh.lodFadeMode = mode;
        // a mesh that may fade loads its fade shaders (RenderSystem::prepareLodFade)
        mesh.needReload = true;
    }
}

LodFadeMode Mesh::getLodFadeMode() const{
    return getComponent<MeshComponent>().lodFadeMode;
}

void Mesh::setLodAnimateCrossFading(bool animate){
    getComponent<MeshComponent>().lodAnimateCrossFading = animate;
}

bool Mesh::isLodAnimateCrossFading() const{
    return getComponent<MeshComponent>().lodAnimateCrossFading;
}

void Mesh::setLodFadeTransitionWidth(float width){
    if (std::isfinite(width))
        getComponent<MeshComponent>().lodFadeTransitionWidth = std::clamp(width, 0.0f, 1.0f);
}

float Mesh::getLodFadeTransitionWidth() const{
    return getComponent<MeshComponent>().lodFadeTransitionWidth;
}

void Mesh::setLodCullBelow(float share){
    if (std::isfinite(share))
        getComponent<MeshComponent>().lodCullBelow = std::clamp(share, 0.0f, 1.0f);
}

float Mesh::getLodCullBelow() const{
    return getComponent<MeshComponent>().lodCullBelow;
}

void Mesh::forceLod(int level){
    getComponent<MeshComponent>().lodForced = std::clamp(level, -1, MAX_MESH_LODS - 1);
}

int Mesh::getForcedLod() const{
    return getComponent<MeshComponent>().lodForced;
}

int Mesh::getCurrentLod() const{
    return getComponent<MeshComponent>().lodFade.plan.dominant();
}

void Mesh::setReceiveShadows(bool receiveShadows){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.receiveShadows != receiveShadows){
        mesh.receiveShadows = receiveShadows;

        mesh.needReload = true;
    }
}

bool Mesh::isReceiveShadows() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.receiveShadows;
}

void Mesh::setRenderInReflectionProbes(bool renderInReflectionProbes){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.renderInReflectionProbes != renderInReflectionProbes){
        mesh.renderInReflectionProbes = renderInReflectionProbes;

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
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.renderInReflectionProbes;
}

void Mesh::setShadowsBillboard(bool shadowsBillboard){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.shadowsBillboard = shadowsBillboard;
}

bool Mesh::isShadowsBillboard() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.shadowsBillboard;
}

void Mesh::setTransparent(bool transparent){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.transparent = transparent;
}

bool Mesh::isTransparent() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.transparent;
}

void Mesh::setAutoTransparency(bool autoTransparency){
    MeshComponent& mesh = getComponent<MeshComponent>();

    mesh.autoTransparency = autoTransparency;
}

bool Mesh::isAutoTransparency() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.autoTransparency;
}

void Mesh::setCustomShader(const std::string& path){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.customShader != path){
        mesh.customShader = path;

        mesh.needReload = true;
    }
}

std::string Mesh::getCustomShader() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.customShader;
}

void Mesh::setCustomDepthShader(const std::string& path){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (mesh.customDepthShader != path){
        mesh.customDepthShader = path;

        mesh.needReload = true;
    }
}

std::string Mesh::getCustomDepthShader() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.customDepthShader;
}

void Mesh::setShaderUniform(const std::string& name, const Vector4& value){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (ShaderUniforms::set(mesh.shaderUniforms, name, value)){
        mesh.needUpdateShaderUniforms = true;
        markChanged<MeshComponent>();
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
    MeshComponent& mesh = getComponent<MeshComponent>();

    return ShaderUniforms::get(mesh.shaderUniforms, name);
}

bool Mesh::removeShaderUniform(const std::string& name){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (!ShaderUniforms::remove(mesh.shaderUniforms, name))
        return false;

    mesh.needUpdateShaderUniforms = true;
    markChanged<MeshComponent>();
    return true;
}

void Mesh::setAsMirror(){
    // default normal +Z matches a Wall surface; flip for differently-oriented meshes
    setAsMirror(Vector3(0, 0, 1));
}

void Mesh::setAsMirror(Vector3 normal){
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
    scene->getSystem<MeshSystem>()->createInstancedMesh(entity);
}

void Mesh::removeInstancedMesh(){
    scene->getSystem<MeshSystem>()->removeInstancedMesh(entity);
}

bool Mesh::hasInstancedMesh() const{
    return scene->getSystem<MeshSystem>()->hasInstancedMesh(entity);
}

void Mesh::setInstancedBillboard(bool billboard, bool cylindrical){
    createInstancedMesh();
    InstancedMeshComponent& instmesh = getComponent<InstancedMeshComponent>();

    instmesh.instancedBillboard = billboard;
    instmesh.instancedCylindricalBillboard = cylindrical;

    instmesh.needUpdateInstances = true;
    markChanged<InstancedMeshComponent>();
}

void Mesh::setInstancedBillboard(bool billboard){
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
