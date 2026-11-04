// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LatticeEditorOps::bake and canBake (spec §9.4, §12 WP-BAKE). The bake writes the deformation
// the GPU draws now into a copy of the target's glTF model, with the CPU solver
// (lattice::deformVertex) and the bindings resolved exactly as LatticeRender::prepare packs them:
// own bindings, then those inherited from ancestors with Include Children (nearest first), at
// most LATTICE_MAX_PER_MESH, skipping invalid lattices; per submesh the Submesh filter, the
// mask channel (only with vertex colours) and the weight; Before Skinning bindings deform the
// bind pose of skinned submeshes, After Skinning ones cannot be baked there; without skinning
// every binding deforms after the instance matrix, so an instanced mesh cannot be baked.

#include "LatticeEditorOps.h"
#include "util/FileUtils.h"
#include "LatticeBakeGltf.h"

#include "Backend.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/type/LatticeBakeCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "io/FileData.h"
#include "lattice/LatticeSolver.h"
#include "lattice/LatticeTypes.h"
#include "pose/PoseEditSession.h"
#include "subsystem/MeshSystem.h"
#include "window/ProjectWindow.h"

#include "tiny_gltf.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace{

    const int MAX_HIERARCHY_DEPTH = 1024;

    std::string labelOf(Scene* scene, Entity entity){
        const std::string name = scene->getEntityName(entity);
        return "'" + (name.empty() ? "GameObject " + std::to_string(entity) : name) + "'";
    }

    bool isDescendantOf(Scene* scene, Entity entity, Entity ancestor){
        const Transform* transform = scene->findComponent<Transform>(entity);
        for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < MAX_HIERARCHY_DEPTH; depth++){
            if (transform->parent == ancestor) return true;
            transform = scene->findComponent<Transform>(transform->parent);
        }
        return false;
    }

    bool isMeshTarget(Scene* scene, Entity entity){
        // a terrain is never deformed (LatticeRender)
        return scene->isEntityCreated(entity) && scene->findComponent<Transform>(entity) &&
               scene->findComponent<MeshComponent>(entity) && !scene->findComponent<TerrainComponent>(entity);
    }

    // a mesh with geometry: the empty container a glTF Model with mesh parts keeps deforms nothing
    bool hasGeometry(Scene* scene, Entity entity){
        return isMeshTarget(scene, entity) && scene->getComponent<MeshComponent>(entity).numSubmeshes > 0;
    }

    // ---- bindings, as LatticeRender::prepare reads them -----------------------------------------

    struct ResolvedLattice{
        Entity entity = NULL_ENTITY;
        const LatticeComponent* component = nullptr;
        Matrix4 model;
        std::vector<Vector3> padded;
        float maxOffset = 0.0f;
        bool valid = false;
    };

    struct ResolvedBinding{
        Entity modifier = NULL_ENTITY;
        size_t index = 0;
        LatticeBinding binding;
        size_t lattice = 0;
        bool valid = false;  // deforms when reached (a valid lattice, a finite weight)
        bool moving = false; // and moves something: not at rest, weight not 0
        bool relevant = false; // reached one of the meshes being baked
    };

    struct BindingUse{
        size_t binding = 0;
        Matrix4 toNormalized;
        Matrix4 toObject;
    };

    struct MeshStack{
        std::vector<BindingUse> accepted;  // in deformation order
        std::vector<size_t> dropped;       // valid bindings past LATTICE_MAX_PER_MESH
    };

    class Resolver{
    public:
        explicit Resolver(Scene* scene): scene(scene){}

        std::vector<ResolvedLattice> lattices;
        std::vector<ResolvedBinding> bindings;

        // the bindings that deform `mesh`: its own, then the inherited ones nearest ancestor first
        const MeshStack& stackOf(Entity mesh){
            auto found = stacks.find(mesh);
            if (found != stacks.end()) return found->second;
            MeshStack& stack = stacks[mesh];
            if (!isMeshTarget(scene, mesh)) return stack;
            const Transform& transform = scene->getComponent<Transform>(mesh);
            if (const LatticeModifierComponent* own = scene->findComponent<LatticeModifierComponent>(mesh)){
                if (own->enabled) consider(*own, mesh, transform, stack);
            }
            Entity ancestor = transform.parent;
            for (int depth = 0; ancestor != NULL_ENTITY && depth < MAX_HIERARCHY_DEPTH; depth++){
                if (const LatticeModifierComponent* inherited = scene->findComponent<LatticeModifierComponent>(ancestor)){
                    if (inherited->enabled && inherited->includeChildren) consider(*inherited, ancestor, transform, stack);
                }
                const Transform* ancestorTransform = scene->findComponent<Transform>(ancestor);
                ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
            }
            return stack;
        }

        // the bindings resolved so far get `relevant` (the meshes being baked were resolved first)
        void markRelevant(){
            for (ResolvedBinding& binding : bindings) binding.relevant = true;
        }

    private:
        Scene* scene;
        std::map<Entity, MeshStack> stacks;
        std::map<std::pair<Entity, size_t>, size_t> bindingIndex;
        std::map<Entity, size_t> latticeIndex;

        size_t latticeOf(Entity entity){
            auto found = latticeIndex.find(entity);
            if (found != latticeIndex.end()) return found->second;
            const size_t index = lattices.size();
            latticeIndex[entity] = index;
            lattices.emplace_back();
            ResolvedLattice& lattice = lattices.back();
            lattice.entity = entity;

            // skipped as LatticeRender skips them: deleted, no lattice, disabled, inactive,
            // a singular transform, a padded grid that does not fit, non-finite offsets
            if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return index;
            const LatticeComponent* component = scene->findComponent<LatticeComponent>(entity);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!component || !transform || !component->enabled || !scene->isEntityActiveInHierarchy(entity)) return index;
            const float determinant = transform->modelMatrix.linear().determinant();
            if (!transform->modelMatrix.isValid() || !std::isfinite(determinant) || determinant == 0.0f ||
                    !transform->modelMatrix.inverse().isValid()) return index;
            lattice::buildPaddedGrid(*component, lattice.padded);
            const size_t rx = (size_t)std::min(std::max(component->resolutionX, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
            const size_t ry = (size_t)std::min(std::max(component->resolutionY, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
            const size_t rz = (size_t)std::min(std::max(component->resolutionZ, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
            if (lattice.padded.size() != (rx + 2) * (ry + 2) * (rz + 2)) return index;
            lattice.maxOffset = lattice::maxPaddedOffset(lattice.padded);
            if (!std::isfinite(lattice.maxOffset)) return index;
            lattice.component = component;
            lattice.model = transform->modelMatrix;
            lattice.valid = true;
            return index;
        }

        size_t bindingOf(Entity modifier, size_t index, const LatticeBinding& binding){
            auto found = bindingIndex.find({modifier, index});
            if (found != bindingIndex.end()) return found->second;
            const size_t lattice = latticeOf(binding.lattice);
            const size_t resolved = bindings.size();
            bindingIndex[{modifier, index}] = resolved;
            bindings.emplace_back();
            ResolvedBinding& entry = bindings.back();
            entry.modifier = modifier;
            entry.index = index;
            entry.binding = binding;
            entry.lattice = lattice;
            entry.valid = lattices[lattice].valid && std::isfinite(binding.weight);
            entry.moving = entry.valid && binding.weight != 0.0f && lattices[lattice].maxOffset > 0.0f;
            return resolved;
        }

        void consider(const LatticeModifierComponent& modifier, Entity modifierEntity, const Transform& meshTransform, MeshStack& stack){
            // as LatticeRender: a lattice taken from a nearer modifier is not applied again
            const size_t nearer = stack.accepted.size();
            for (size_t b = 0; b < modifier.bindings.size(); b++){
                const size_t resolved = bindingOf(modifierEntity, b, modifier.bindings[b]);
                if (!bindings[resolved].valid) continue;
                bool overridden = false;
                for (size_t a = 0; a < nearer; a++){
                    overridden = overridden || bindings[stack.accepted[a].binding].lattice == bindings[resolved].lattice;
                }
                if (overridden) continue;
                const ResolvedLattice& lattice = lattices[bindings[resolved].lattice];
                const Matrix4 toNormalized = lattice::objectToNormalized(lattice.model, meshTransform.modelMatrix);
                const Matrix4 toObject = toNormalized.inverse();
                // a mesh with a non-invertible transform has no object space to go back to
                if (!toNormalized.isValid() || !toObject.isValid()) continue;
                if (stack.accepted.size() == LATTICE_MAX_PER_MESH){
                    stack.dropped.push_back(resolved);
                    continue;
                }
                BindingUse use;
                use.binding = resolved;
                use.toNormalized = toNormalized;
                use.toObject = toObject;
                stack.accepted.push_back(use);
            }
        }
    };

    // ---- the glTF model behind a mesh --------------------------------------------------------------

    struct BakeSubmesh{
        int node = -1;
        int mesh = -1;
        int primitive = -1;
        bool skinned = false; // HAS_SKINNING (JOINTS_0 or WEIGHTS_0 loaded)
        bool color = false;   // the vertex colour a mask reads
        std::vector<size_t> chosen; // indices into the mesh's accepted bindings, baked in order
    };

    struct BakeMesh{
        Entity entity = NULL_ENTITY;
        Entity model = NULL_ENTITY;
        bool merged = false;    // Merge Static Meshes: node transforms baked into the vertices
        bool instanced = false;
        std::vector<BakeSubmesh> submeshes; // engine submesh order
    };

    // a glTF attribute the loader binds (MeshSystem::loadGLTF): a valid accessor and bufferView
    // and a component type a vertex format has
    const tinygltf::Accessor* loadedAttribute(const tinygltf::Model& gltf, const tinygltf::Primitive& primitive, const char* name){
        auto found = primitive.attributes.find(name);
        if (found == primitive.attributes.end() || found->second < 0 || (size_t)found->second >= gltf.accessors.size()) return nullptr;
        const tinygltf::Accessor& accessor = gltf.accessors[(size_t)found->second];
        if (accessor.bufferView < 0 || (size_t)accessor.bufferView >= gltf.bufferViews.size()) return nullptr;
        switch (accessor.componentType){
            case TINYGLTF_COMPONENT_TYPE_BYTE:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            case TINYGLTF_COMPONENT_TYPE_SHORT:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return &accessor;
            default:
                return nullptr;
        }
    }

    bool isFloatAttribute(const tinygltf::Model& gltf, const tinygltf::Primitive& primitive, const char* name, int type){
        auto found = primitive.attributes.find(name);
        if (found == primitive.attributes.end()) return true; // absent: nothing to bake
        const tinygltf::Accessor* accessor = loadedAttribute(gltf, primitive, name);
        return accessor && accessor->componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && accessor->type == type && !accessor->sparse.isSparse;
    }

    // What a mesh that cannot be baked is, for the reasons shown
    std::string meshKind(Scene* scene, Entity entity){
        if (scene->findComponent<SpriteComponent>(entity)) return "a sprite";
        if (scene->findComponent<MeshPolygonComponent>(entity)) return "a polygon";
        if (scene->findComponent<TilemapComponent>(entity)) return "a tilemap";
        if (scene->findComponent<TerrainComponent>(entity)) return "a terrain";
        if (const ModelComponent* model = scene->findComponent<ModelComponent>(entity)){
            if (FileData::getFilePathExtension(model->filename) == "obj") return "an OBJ model";
            if (model->filename.empty()) return "a Model without a file";
            return "a model that cannot be baked now";
        }
        return "a generated mesh";
    }

    // the Model entity whose glTF load made `mesh` (itself on the flat path, its root for a part)
    Entity modelOfMesh(Scene* scene, Entity mesh){
        if (scene->findComponent<ModelComponent>(mesh)) return mesh;
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t m = 0; m < models->size(); m++){
            const ModelComponent& model = models->getComponentFromIndex(m);
            for (const auto& node : model.meshNodesMapping){
                if (node.second == mesh) return models->getEntity(m);
            }
        }
        return NULL_ENTITY;
    }

    // the meshes a glTF Model's load made, with their submeshes' primitives (MeshSystem::loadGLTF:
    // one entity per mesh node, or every node's primitives on the Model; submeshes reversed)
    bool modelMeshes(Scene* scene, Entity modelEntity, std::vector<BakeMesh>& meshes, std::string& reason){
        ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
        const std::string label = labelOf(scene, modelEntity);
        const std::string ext = FileData::getFilePathExtension(model.filename);
        if (model.filename.empty()){
            reason = label + " is a Model without a file: there is nothing to bake";
            return false;
        }
        if (ext == "obj"){
            reason = label + " is an OBJ model: only glTF models (.gltf, .glb) can be baked (export it as glTF first)";
            return false;
        }
        if (ext != "gltf" && ext != "glb"){
            reason = label + " is not a glTF model (.gltf, .glb): only those can be baked";
            return false;
        }
        std::shared_ptr<MeshSystem> meshSystem = scene->getSystem<MeshSystem>();
        if (!model.gltfModel || model.loadedFilename != MeshSystem::getModelFilenameKey(model.filename) ||
                meshSystem->isAsyncModelLoadPending(modelEntity, model.filename)){
            reason = label + "'s model has not finished loading";
            return false;
        }
        const tinygltf::Model& gltf = *model.gltfModel;

        std::vector<int> meshNodes;
        std::map<int, int> parents;
        for (size_t n = 0; n < gltf.nodes.size(); n++){
            parents[(int)n] = -1;
        }
        for (size_t n = 0; n < gltf.nodes.size(); n++){
            if (gltf.nodes[n].mesh >= 0) meshNodes.push_back((int)n);
            for (int child : gltf.nodes[n].children) parents[child] = (int)n;
        }

        const bool children = !model.meshNodesMapping.empty();
        const bool merged = !children && model.loadedMergeStaticMeshes && meshNodes.size() > 1;
        const std::string stale = label + "'s meshes do not match its file anymore: load the model again (Model File) and bake";

        auto submeshOf = [&](int node, int primitive) -> BakeSubmesh {
            BakeSubmesh submesh;
            submesh.node = node;
            submesh.mesh = gltf.nodes[(size_t)node].mesh;
            submesh.primitive = primitive;
            const tinygltf::Primitive& data = gltf.meshes[(size_t)submesh.mesh].primitives[(size_t)primitive];
            // the merged path drops the skinning attributes (static geometry)
            submesh.skinned = !merged && (loadedAttribute(gltf, data, "JOINTS_0") || loadedAttribute(gltf, data, "WEIGHTS_0"));
            if (const tinygltf::Accessor* color = loadedAttribute(gltf, data, "COLOR_0")){
                submesh.color = color->type == TINYGLTF_TYPE_VEC4 ||
                                (color->type == TINYGLTF_TYPE_VEC3 && (color->componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
                                                                       color->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                                                                       color->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
            }
            return submesh;
        };

        if (children){
            for (int node : meshNodes){
                const int meshIndex = gltf.nodes[(size_t)node].mesh;
                if ((size_t)meshIndex >= gltf.meshes.size()) continue;
                auto mapped = model.meshNodesMapping.find(node);
                if (mapped == model.meshNodesMapping.end() || !isMeshTarget(scene, mapped->second)){
                    reason = stale;
                    return false;
                }
                const MeshComponent& mesh = scene->getComponent<MeshComponent>(mapped->second);
                const size_t slots = gltf.meshes[(size_t)meshIndex].primitives.size();
                if (mesh.numSubmeshes != slots){
                    reason = stale;
                    return false;
                }
                BakeMesh bakeMesh;
                bakeMesh.entity = mapped->second;
                bakeMesh.model = modelEntity;
                bakeMesh.instanced = scene->findComponent<InstancedMeshComponent>(mapped->second) != nullptr;
                for (size_t s = 0; s < slots; s++){
                    bakeMesh.submeshes.push_back(submeshOf(node, (int)(slots - 1 - s)));
                }
                meshes.push_back(std::move(bakeMesh));
            }
            return true;
        }

        if (!isMeshTarget(scene, modelEntity)){
            reason = label + " has no mesh to bake";
            return false;
        }
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(modelEntity);
        std::vector<std::pair<int, int>> slots;
        for (int node : meshNodes){
            const int meshIndex = gltf.nodes[(size_t)node].mesh;
            if ((size_t)meshIndex >= gltf.meshes.size()) continue;
            for (size_t p = 0; p < gltf.meshes[(size_t)meshIndex].primitives.size(); p++){
                slots.push_back({node, (int)p});
            }
        }
        if (mesh.numSubmeshes != slots.size()){
            reason = stale;
            return false;
        }
        BakeMesh bakeMesh;
        bakeMesh.entity = modelEntity;
        bakeMesh.model = modelEntity;
        bakeMesh.merged = merged;
        bakeMesh.instanced = scene->findComponent<InstancedMeshComponent>(modelEntity) != nullptr;
        for (size_t s = 0; s < slots.size(); s++){
            const std::pair<int, int>& slot = slots[slots.size() - 1 - s];
            if (merged){
                const tinygltf::Primitive& data = gltf.meshes[(size_t)gltf.nodes[(size_t)slot.first].mesh].primitives[(size_t)slot.second];
                // the merge bakes only FLOAT vertex data with the node transforms (any other
                // stays untransformed), and the baked file is FLOAT: they would not match
                if (!isFloatAttribute(gltf, data, "POSITION", TINYGLTF_TYPE_VEC3) || !isFloatAttribute(gltf, data, "NORMAL", TINYGLTF_TYPE_VEC3) ||
                        !isFloatAttribute(gltf, data, "TANGENT", TINYGLTF_TYPE_VEC4)){
                    reason = label + " merges static meshes whose vertex data is quantized or sparse: turn Merge Static Meshes off to bake it";
                    return false;
                }
            }
            bakeMesh.submeshes.push_back(submeshOf(slot.first, slot.second));
        }
        meshes.push_back(std::move(bakeMesh));
        return true;
    }

    // ---- the plan: what bakes into which submesh, what stays and why ---------------------------------

    struct BakePlan{
        explicit BakePlan(Scene* scene): scene(scene), resolver(scene){}

        Scene* scene;
        Resolver resolver;
        std::vector<BakeMesh> meshes;          // the meshes being baked
        std::vector<bool> baked;               // per resolver binding
        std::vector<std::string> kept;         // per resolver binding: why it stays
        std::vector<std::string> notes;        // what the user should know (kept bindings, morph targets)
        std::string reason;                    // why nothing can be baked

        bool any() const{
            return std::find(baked.begin(), baked.end(), true) != baked.end();
        }

        std::string bindingLabel(size_t binding) const{
            const ResolvedBinding& entry = resolver.bindings[binding];
            std::string label = "binding " + std::to_string(entry.index + 1) + " of " + labelOf(scene, entry.modifier);
            const Entity lattice = entry.binding.lattice;
            if (lattice != NULL_ENTITY && scene->isEntityCreated(lattice)){
                label += " (lattice " + labelOf(scene, lattice) + ")";
            }
            return label;
        }
    };

    void keep(BakePlan& plan, size_t binding, const std::string& why){
        plan.baked[binding] = false;
        if (plan.kept[binding].empty()) plan.kept[binding] = why;
    }

    bool buildPlan(Scene* scene, Entity target, BakePlan& plan){
        if (!scene || target == NULL_ENTITY || !scene->isEntityCreated(target)){
            plan.reason = "the target GameObject does not exist in this scene";
            return false;
        }
        const std::string targetLabel = labelOf(scene, target);
        const LatticeModifierComponent* targetModifier = scene->findComponent<LatticeModifierComponent>(target);

        // the meshes the target names: its model's (or the model part it is), and with Include
        // Children every mesh below it
        std::vector<Entity> candidates;
        if (const ModelComponent* model = scene->findComponent<ModelComponent>(target)){
            if (model->meshNodesMapping.empty()){
                candidates.push_back(target);
            }else{
                for (const auto& node : model->meshNodesMapping) candidates.push_back(node.second);
            }
        }else if (isMeshTarget(scene, target)){
            candidates.push_back(target);
        }
        if (targetModifier && targetModifier->includeChildren){
            auto meshArray = scene->getComponentArray<MeshComponent>();
            for (size_t m = 0; m < meshArray->size(); m++){
                const Entity mesh = meshArray->getEntity(m);
                if (isDescendantOf(scene, mesh, target)) candidates.push_back(mesh);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        // which of them come from a glTF model that can be baked
        std::map<Entity, std::vector<BakeMesh>> models;
        std::map<Entity, std::string> modelReasons;
        std::map<Entity, std::string> unbakeable; // mesh -> what it is
        for (Entity candidate : candidates){
            if (!isMeshTarget(scene, candidate)) continue;
            const Entity modelEntity = modelOfMesh(scene, candidate);
            if (modelEntity == NULL_ENTITY){
                if (hasGeometry(scene, candidate)) unbakeable[candidate] = meshKind(scene, candidate);
                continue;
            }
            if (!models.count(modelEntity) && !modelReasons.count(modelEntity)){
                std::vector<BakeMesh> meshes;
                std::string why;
                if (modelMeshes(scene, modelEntity, meshes, why)){
                    models[modelEntity] = std::move(meshes);
                }else{
                    modelReasons[modelEntity] = why;
                }
            }
            if (!hasGeometry(scene, candidate)) continue;
            auto found = models.find(modelEntity);
            bool added = false;
            if (found != models.end()){
                for (const BakeMesh& mesh : found->second){
                    if (mesh.entity == candidate){
                        plan.meshes.push_back(mesh);
                        added = true;
                    }
                }
            }
            if (!added){
                auto why = modelReasons.find(modelEntity);
                unbakeable[candidate] = meshKind(scene, (why != modelReasons.end()) ? modelEntity : candidate);
            }
        }
        if (plan.meshes.empty()){
            if (!modelReasons.empty()){
                plan.reason = modelReasons.begin()->second;
            }else if (unbakeable.count(target)){
                plan.reason = targetLabel + " is " + unbakeable[target] + ": only glTF models (.gltf, .glb) can be baked";
            }else if (!unbakeable.empty()){
                plan.reason = "the meshes " + targetLabel + " deforms are not glTF models (" + labelOf(scene, unbakeable.begin()->first) +
                              " is " + unbakeable.begin()->second + "): only glTF models (.gltf, .glb) can be baked";
            }else{
                plan.reason = targetLabel + " has no glTF model to bake" +
                              std::string(targetModifier && !targetModifier->includeChildren ? " (turn Include Children on for the models below it)" : "");
            }
            return false;
        }

        // the bindings of the meshes being baked
        for (const BakeMesh& mesh : plan.meshes){
            plan.resolver.stackOf(mesh.entity);
        }
        plan.resolver.markRelevant();
        std::set<size_t> candidatesBound;
        for (const BakeMesh& mesh : plan.meshes){
            for (const BindingUse& use : plan.resolver.stackOf(mesh.entity).accepted){
                if (plan.resolver.bindings[use.binding].moving) candidatesBound.insert(use.binding);
            }
        }
        const size_t relevantCount = plan.resolver.bindings.size();
        plan.baked.assign(relevantCount, false);
        plan.kept.assign(relevantCount, std::string());
        for (size_t binding : candidatesBound) plan.baked[binding] = true;

        // a binding that also deforms a mesh outside the bake stays (removing it would undeform that one)
        std::set<Entity> baking;
        for (const BakeMesh& mesh : plan.meshes) baking.insert(mesh.entity);
        std::map<Entity, std::vector<Entity>> reachedBy; // modifier entity -> meshes it reaches
        for (size_t binding : candidatesBound){
            const Entity modifierEntity = plan.resolver.bindings[binding].modifier;
            auto reached = reachedBy.find(modifierEntity);
            if (reached == reachedBy.end()){
                std::vector<Entity> list;
                if (hasGeometry(scene, modifierEntity)) list.push_back(modifierEntity);
                const LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(modifierEntity);
                if (modifier && modifier->includeChildren){
                    auto meshArray = scene->getComponentArray<MeshComponent>();
                    for (size_t m = 0; m < meshArray->size(); m++){
                        const Entity mesh = meshArray->getEntity(m);
                        if (hasGeometry(scene, mesh) && isDescendantOf(scene, mesh, modifierEntity)) list.push_back(mesh);
                    }
                }
                reached = reachedBy.emplace(modifierEntity, std::move(list)).first;
            }
            for (Entity mesh : reached->second){
                if (baking.count(mesh)) continue;
                const MeshStack& stack = plan.resolver.stackOf(mesh);
                const bool deforms = std::any_of(stack.accepted.begin(), stack.accepted.end(),
                                                 [binding](const BindingUse& use){ return use.binding == binding; });
                if (!deforms) continue;
                auto known = unbakeable.find(mesh);
                const std::string what = (known != unbakeable.end()) ? known->second
                                       : (modelOfMesh(scene, mesh) != NULL_ENTITY ? "which is not part of this bake" : meshKind(scene, mesh));
                keep(plan, binding, "it also deforms " + labelOf(scene, mesh) + ", " +
                     (what.rfind("which", 0) == 0 ? what : what + ", which cannot be baked") + ": removing it would undo that");
                break;
            }
        }

        // per submesh: what can bake there (stage, instancing), and whether every binding has a use
        std::vector<bool> used(relevantCount, false);
        for (BakeMesh& mesh : plan.meshes){
            const MeshStack& stack = plan.resolver.stackOf(mesh.entity);
            for (size_t s = 0; s < mesh.submeshes.size(); s++){
                const BakeSubmesh& submesh = mesh.submeshes[s];
                for (const BindingUse& use : stack.accepted){
                    const ResolvedBinding& entry = plan.resolver.bindings[use.binding];
                    if (!entry.moving || (entry.binding.submesh >= 0 && entry.binding.submesh != (int)s)) continue;
                    if (submesh.skinned){
                        if (entry.binding.stage == LatticeStage::AFTER_SKINNING){
                            keep(plan, use.binding, "it deforms the skinned " + labelOf(scene, mesh.entity) +
                                 " after skinning (the posed mesh), which a model file cannot hold: set its Stage to Before Skinning to bake it into the bind pose");
                            continue;
                        }
                    }else if (mesh.instanced){
                        keep(plan, use.binding, labelOf(scene, mesh.entity) + " is instanced: each instance is deformed where it stands, which one model file cannot hold");
                        continue;
                    }
                    used[use.binding] = true;
                }
            }
        }
        for (size_t binding : candidatesBound){
            if (!used[binding] && plan.kept[binding].empty()){
                keep(plan, binding, "its Submesh " + std::to_string(plan.resolver.bindings[binding].binding.submesh) + " matches no submesh it reaches, so it deforms nothing");
            }
        }

        // lattices do not commute: a submesh bakes the bindings before the first one that stays
        // (in its order; skinned submeshes: the Before Skinning ones, which deform before the rest)
        for (bool changed = true; changed;){
            changed = false;
            for (const BakeMesh& mesh : plan.meshes){
                const MeshStack& stack = plan.resolver.stackOf(mesh.entity);
                for (size_t s = 0; s < mesh.submeshes.size(); s++){
                    const BakeSubmesh& submesh = mesh.submeshes[s];
                    if (mesh.instanced && !submesh.skinned) continue;
                    long blocker = -1;
                    for (const BindingUse& use : stack.accepted){
                        const ResolvedBinding& entry = plan.resolver.bindings[use.binding];
                        if (!entry.moving || (entry.binding.submesh >= 0 && entry.binding.submesh != (int)s)) continue;
                        if (submesh.skinned && entry.binding.stage == LatticeStage::AFTER_SKINNING) continue;
                        if (!plan.baked[use.binding]){
                            if (blocker < 0) blocker = (long)use.binding;
                        }else if (blocker >= 0){
                            keep(plan, use.binding, "it deforms " + labelOf(scene, mesh.entity) + " after " +
                                 plan.bindingLabel((size_t)blocker) + ", which stays, and lattices applied in another order give another shape");
                            changed = true;
                        }
                    }
                }
            }
        }

        // the bindings each submesh bakes, in order
        for (BakeMesh& mesh : plan.meshes){
            const MeshStack& stack = plan.resolver.stackOf(mesh.entity);
            for (size_t s = 0; s < mesh.submeshes.size(); s++){
                BakeSubmesh& submesh = mesh.submeshes[s];
                for (size_t u = 0; u < stack.accepted.size(); u++){
                    const ResolvedBinding& entry = plan.resolver.bindings[stack.accepted[u].binding];
                    if (plan.baked[stack.accepted[u].binding] && (entry.binding.submesh < 0 || entry.binding.submesh == (int)s)){
                        submesh.chosen.push_back(u);
                    }
                }
            }
        }

        // what stays, and why
        for (size_t binding = 0; binding < relevantCount; binding++){
            if (plan.baked[binding]) continue;
            const ResolvedBinding& entry = plan.resolver.bindings[binding];
            std::string why = plan.kept[binding];
            if (why.empty()){
                if (!entry.valid){
                    why = (entry.binding.lattice == NULL_ENTITY) ? "it names no lattice"
                        : "its lattice is missing, disabled, inactive or scaled to zero, so it deforms nothing now";
                }else if (!entry.moving){
                    why = "its lattice is at rest (or its weight is 0): there is nothing to bake";
                }else{
                    why = "it is past the " + std::to_string(LATTICE_MAX_PER_MESH) + " lattices a mesh takes (or its mesh is scaled to zero), so it deforms nothing now";
                }
                plan.kept[binding] = why;
            }
            plan.notes.push_back("Kept " + plan.bindingLabel(binding) + ": " + why + ".");
        }
        for (const BakeMesh& mesh : plan.meshes){
            const MeshStack& stack = plan.resolver.stackOf(mesh.entity);
            const bool bakesSomething = std::any_of(stack.accepted.begin(), stack.accepted.end(),
                                                    [&plan](const BindingUse& use){ return plan.baked[use.binding]; });
            if (bakesSomething && !stack.dropped.empty()){
                plan.notes.push_back(labelOf(scene, mesh.entity) + " was reached by more than " + std::to_string(LATTICE_MAX_PER_MESH) +
                                     " lattices: with the baked ones gone, the ones ignored until now deform it.");
            }
        }

        if (!plan.any()){
            for (size_t binding = 0; binding < relevantCount; binding++){
                if (plan.resolver.bindings[binding].moving && !plan.kept[binding].empty()){
                    plan.reason = "nothing to bake: " + plan.bindingLabel(binding) + " stays because " + plan.kept[binding];
                    return false;
                }
            }
            bool anyValid = false;
            for (const ResolvedBinding& entry : plan.resolver.bindings) anyValid = anyValid || (entry.valid && entry.relevant);
            plan.reason = anyValid ? "the lattices deforming " + targetLabel + " are at rest: there is nothing to bake"
                                   : "no lattice deforms " + targetLabel + ": bind one first (Lattice Modifier)";
            return false;
        }
        return true;
    }

    // ---- the vertices ---------------------------------------------------------------------------------

    bool sameBits(const Vector3& a, const Vector3& b){
        return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 && std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
               std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
    }

    struct VertexSpace{
        bool merged = false;
        Matrix4 matrix;      // file -> object (merged: the node transform the load bakes in)
        Matrix4 inverse;
        Matrix3 normalMatrix; // inverse transpose of the linear part
        Matrix3 linear;
        Matrix3 linearInverse;
        Matrix3 linearTranspose;
    };

    int accessorOf(const nlohmann::ordered_json& object, const char* semantic){
        auto found = object.find(semantic);
        if (found == object.end() || !found->is_number_integer()) return -1;
        const int64_t value = found->get<int64_t>();
        return value >= 0 && value <= INT32_MAX ? (int)value : -1;
    }

    // deforms one primitive as `node` places it and gives the document the attributes that changed
    bool bakePrimitive(LatticeBakeGltf& document, const BakeSubmesh& submesh, const std::vector<lattice::ObjectSpaceLattice>& chain,
                       const VertexSpace& space, std::string& error){
        const nlohmann::ordered_json& primitive = document.getJson()["meshes"][(size_t)submesh.mesh]["primitives"][(size_t)submesh.primitive];
        const std::string where = "mesh " + std::to_string(submesh.mesh) + " primitive " + std::to_string(submesh.primitive);
        if (!primitive.contains("attributes") || !primitive["attributes"].is_object()){
            error = where + " has no attributes";
            return false;
        }
        const nlohmann::ordered_json& attributes = primitive["attributes"];
        const int positionAccessor = accessorOf(attributes, "POSITION");
        if (positionAccessor < 0){
            return true; // nothing the load draws
        }

        auto read = [&](int accessor, int wanted, std::vector<float>& values, const char* what) -> bool {
            int components = 0;
            if (!document.readAccessor(accessor, values, components, error)){
                error = where + " " + what + ": " + error;
                return false;
            }
            if (components != wanted){
                error = where + " " + what + " is not a " + (wanted == 3 ? "VEC3" : "VEC4");
                return false;
            }
            return true;
        };

        std::vector<float> positions, normals, tangents, colors;
        if (!read(positionAccessor, 3, positions, "POSITION")) return false;
        const size_t count = positions.size() / 3;
        const int normalAccessor = accessorOf(attributes, "NORMAL");
        const int tangentAccessor = accessorOf(attributes, "TANGENT");
        if (normalAccessor >= 0 && !read(normalAccessor, 3, normals, "NORMAL")) return false;
        if (tangentAccessor >= 0 && !read(tangentAccessor, 4, tangents, "TANGENT")) return false;

        bool masked = false;
        for (const lattice::ObjectSpaceLattice& binding : chain) masked = masked || binding.mask != LatticeMaskChannel::NONE;
        int colorComponents = 0;
        if (masked && submesh.color){
            const int colorAccessor = accessorOf(attributes, "COLOR_0");
            if (colorAccessor < 0 || !document.readAccessor(colorAccessor, colors, colorComponents, error) ||
                    (colorComponents != 3 && colorComponents != 4)){
                error = where + " COLOR_0: " + (error.empty() ? "not a VEC3 or VEC4" : error);
                return false;
            }
        }
        if ((!normals.empty() && normals.size() != count * 3) || (!tangents.empty() && tangents.size() != count * 4) ||
                (!colors.empty() && colors.size() != count * (size_t)colorComponents)){
            error = where + " has attributes of different lengths";
            return false;
        }

        // one vertex from the file into object space, through the chain, and the result
        struct Vertex{
            Vector3 position;
            Vector3 normal;
            Vector3 tangent;
        };
        auto toObject = [&space](const Vector3& position, const Vector3& normal, const Vector3& tangent) -> Vertex {
            Vertex vertex{position, normal, tangent};
            if (space.merged){
                vertex.position = space.matrix * position;
                vertex.normal = (space.normalMatrix * normal).normalized();
                vertex.tangent = (space.linear * tangent).normalized();
            }
            return vertex;
        };
        auto deform = [&](size_t i, Vertex vertex) -> Vertex {
            Vector4 color(1.0f, 1.0f, 1.0f, 1.0f);
            if (!colors.empty()){
                const float* c = &colors[i * (size_t)colorComponents];
                color = Vector4(c[0], c[1], c[2], colorComponents == 4 ? c[3] : 1.0f);
            }
            for (const lattice::ObjectSpaceLattice& binding : chain){
                lattice::deformVertex(binding, vertex.position, normals.empty() ? nullptr : &vertex.normal,
                                      tangents.empty() ? nullptr : &vertex.tangent, nullptr, colors.empty() ? nullptr : &color);
            }
            return vertex;
        };

        std::vector<Vertex> rest(count), moved(count);
        std::vector<float> outPositions = positions, outNormals = normals, outTangents = tangents;
        bool positionsChanged = false, normalsChanged = false, tangentsChanged = false;
        for (size_t i = 0; i < count; i++){
            const Vector3 position(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
            const Vector3 normal = normals.empty() ? Vector3(0, 0, 0) : Vector3(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
            const Vector3 tangent = tangents.empty() ? Vector3(0, 0, 0) : Vector3(tangents[i * 4], tangents[i * 4 + 1], tangents[i * 4 + 2]);
            rest[i] = toObject(position, normal, tangent);
            moved[i] = deform(i, rest[i]);

            // a vertex the chain does not move keeps its file values bit for bit
            if (!sameBits(moved[i].position, rest[i].position)){
                const Vector3 p = space.merged ? space.inverse * moved[i].position : moved[i].position;
                outPositions[i * 3] = p.x; outPositions[i * 3 + 1] = p.y; outPositions[i * 3 + 2] = p.z;
                positionsChanged = true;
            }
            if (!normals.empty() && !sameBits(moved[i].normal, rest[i].normal)){
                const Vector3 n = space.merged ? (space.linearTranspose * moved[i].normal).normalized() : moved[i].normal;
                outNormals[i * 3] = n.x; outNormals[i * 3 + 1] = n.y; outNormals[i * 3 + 2] = n.z;
                normalsChanged = true;
            }
            if (!tangents.empty() && !sameBits(moved[i].tangent, rest[i].tangent)){
                // the handedness (w) stays
                const Vector3 t = space.merged ? (space.linearInverse * moved[i].tangent).normalized() : moved[i].tangent;
                outTangents[i * 4] = t.x; outTangents[i * 4 + 1] = t.y; outTangents[i * 4 + 2] = t.z;
                tangentsChanged = true;
            }
        }
        if (positionsChanged) document.setAttribute(submesh.node, {submesh.primitive, -1, "POSITION"}, std::move(outPositions), 3);
        if (normalsChanged) document.setAttribute(submesh.node, {submesh.primitive, -1, "NORMAL"}, std::move(outNormals), 3);
        if (tangentsChanged) document.setAttribute(submesh.node, {submesh.primitive, -1, "TANGENT"}, std::move(outTangents), 4);

        // morph targets (applied before the lattices) become displacements of the baked shape:
        // deform(base + delta) - deform(base), exact at weight 0 and 1; displacements in object
        // space as the load adds them (a merge leaves them untransformed)
        if (!primitive.contains("targets") || !primitive["targets"].is_array()){
            return true;
        }
        const nlohmann::ordered_json& targets = primitive["targets"];
        for (size_t t = 0; t < targets.size(); t++){
            if (!targets[t].is_object()) continue;
            const int deltaPositionAccessor = accessorOf(targets[t], "POSITION");
            const int deltaNormalAccessor = normals.empty() ? -1 : accessorOf(targets[t], "NORMAL");
            const int deltaTangentAccessor = tangents.empty() ? -1 : accessorOf(targets[t], "TANGENT");
            if (deltaPositionAccessor < 0 && deltaNormalAccessor < 0 && deltaTangentAccessor < 0) continue;
            std::vector<float> deltaPositions, deltaNormals, deltaTangents;
            const std::string label = "target " + std::to_string(t);
            if (deltaPositionAccessor >= 0 && !read(deltaPositionAccessor, 3, deltaPositions, ("morph " + label + " POSITION").c_str())) return false;
            if (deltaNormalAccessor >= 0 && !read(deltaNormalAccessor, 3, deltaNormals, ("morph " + label + " NORMAL").c_str())) return false;
            if (deltaTangentAccessor >= 0 && !read(deltaTangentAccessor, 3, deltaTangents, ("morph " + label + " TANGENT").c_str())) return false;
            if ((!deltaPositions.empty() && deltaPositions.size() != count * 3) || (!deltaNormals.empty() && deltaNormals.size() != count * 3) ||
                    (!deltaTangents.empty() && deltaTangents.size() != count * 3)){
                error = where + " morph " + label + " does not have the primitive's vertex count";
                return false;
            }
            std::vector<float> outDeltaPositions = deltaPositions, outDeltaNormals = deltaNormals, outDeltaTangents = deltaTangents;
            bool deltaPositionsChanged = false, deltaNormalsChanged = false, deltaTangentsChanged = false;
            for (size_t i = 0; i < count; i++){
                Vertex morphed = rest[i];
                if (!deltaPositions.empty()) morphed.position = morphed.position + Vector3(deltaPositions[i * 3], deltaPositions[i * 3 + 1], deltaPositions[i * 3 + 2]);
                if (!deltaNormals.empty()) morphed.normal = morphed.normal + Vector3(deltaNormals[i * 3], deltaNormals[i * 3 + 1], deltaNormals[i * 3 + 2]);
                if (!deltaTangents.empty()) morphed.tangent = morphed.tangent + Vector3(deltaTangents[i * 3], deltaTangents[i * 3 + 1], deltaTangents[i * 3 + 2]);
                const Vertex movedMorph = deform(i, morphed);
                if (!deltaPositions.empty() && !(sameBits(movedMorph.position, morphed.position) && sameBits(moved[i].position, rest[i].position))){
                    const Vector3 d = movedMorph.position - moved[i].position;
                    outDeltaPositions[i * 3] = d.x; outDeltaPositions[i * 3 + 1] = d.y; outDeltaPositions[i * 3 + 2] = d.z;
                    deltaPositionsChanged = true;
                }
                if (!deltaNormals.empty() && !(sameBits(movedMorph.normal, morphed.normal) && sameBits(moved[i].normal, rest[i].normal))){
                    const Vector3 d = movedMorph.normal - moved[i].normal;
                    outDeltaNormals[i * 3] = d.x; outDeltaNormals[i * 3 + 1] = d.y; outDeltaNormals[i * 3 + 2] = d.z;
                    deltaNormalsChanged = true;
                }
                if (!deltaTangents.empty() && !(sameBits(movedMorph.tangent, morphed.tangent) && sameBits(moved[i].tangent, rest[i].tangent))){
                    const Vector3 d = movedMorph.tangent - moved[i].tangent;
                    outDeltaTangents[i * 3] = d.x; outDeltaTangents[i * 3 + 1] = d.y; outDeltaTangents[i * 3 + 2] = d.z;
                    deltaTangentsChanged = true;
                }
            }
            if (deltaPositionsChanged) document.setAttribute(submesh.node, {submesh.primitive, (int)t, "POSITION"}, std::move(outDeltaPositions), 3);
            if (deltaNormalsChanged) document.setAttribute(submesh.node, {submesh.primitive, (int)t, "NORMAL"}, std::move(outDeltaNormals), 3);
            if (deltaTangentsChanged) document.setAttribute(submesh.node, {submesh.primitive, (int)t, "TANGENT"}, std::move(outDeltaTangents), 3);
        }
        return true;
    }

    // a node's local matrix, its global one and a skin's inverse bind matrix, computed as the
    // load computes the transforms Merge Static Meshes bakes in (MeshSystem::getGLTFNodeMatrix,
    // getGLTFMeshGlobalMatrix, getGLTFInverseBindMatrix)
    Matrix4 nodeMatrix(const tinygltf::Model& gltf, int nodeIndex){
        if (nodeIndex < 0 || (size_t)nodeIndex >= gltf.nodes.size()) return Matrix4();
        const tinygltf::Node& node = gltf.nodes[(size_t)nodeIndex];
        if (node.matrix.size() == 16){
            const std::vector<double>& m = node.matrix;
            return Matrix4(m[0], m[4], m[8], m[12],
                           m[1], m[5], m[9], m[13],
                           m[2], m[6], m[10], m[14],
                           m[3], m[7], m[11], m[15]);
        }
        const Vector3 translation = node.translation.size() == 3 ? Vector3(node.translation[0], node.translation[1], node.translation[2]) : Vector3(0.0, 0.0, 0.0);
        const Quaternion rotation = node.rotation.size() == 4 ? Quaternion(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2])
                                                              : Quaternion(1.0, 0.0, 0.0, 0.0);
        const Vector3 scale = node.scale.size() == 3 ? Vector3(node.scale[0], node.scale[1], node.scale[2]) : Vector3(1.0, 1.0, 1.0);
        return Matrix4::translateMatrix(translation) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    }

    Matrix4 nodeGlobalMatrix(const tinygltf::Model& gltf, int nodeIndex, const std::map<int, int>& parents){
        Matrix4 matrix = nodeMatrix(gltf, nodeIndex);
        auto parent = parents.find(nodeIndex);
        for (int depth = 0; parent != parents.end() && parent->second >= 0 && depth < MAX_HIERARCHY_DEPTH; depth++){
            matrix = nodeMatrix(gltf, parent->second) * matrix;
            parent = parents.find(parent->second);
        }
        return matrix;
    }

    Matrix4 inverseBindMatrix(const tinygltf::Model& gltf, int skinIndex, size_t jointIndex){
        if (skinIndex < 0 || (size_t)skinIndex >= gltf.skins.size()) return Matrix4();
        const tinygltf::Skin& skin = gltf.skins[(size_t)skinIndex];
        if (skin.inverseBindMatrices < 0 || (size_t)skin.inverseBindMatrices >= gltf.accessors.size()) return Matrix4();
        const tinygltf::Accessor& accessor = gltf.accessors[(size_t)skin.inverseBindMatrices];
        if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != TINYGLTF_TYPE_MAT4 || jointIndex >= accessor.count ||
                accessor.bufferView < 0 || (size_t)accessor.bufferView >= gltf.bufferViews.size()) return Matrix4();
        const tinygltf::BufferView& view = gltf.bufferViews[(size_t)accessor.bufferView];
        if (view.buffer < 0 || (size_t)view.buffer >= gltf.buffers.size()) return Matrix4();
        const std::vector<unsigned char>& data = gltf.buffers[(size_t)view.buffer].data;
        const int stride = accessor.ByteStride(view);
        if (stride < (int)(sizeof(float) * 16)) return Matrix4();
        const size_t offset = view.byteOffset + accessor.byteOffset + jointIndex * (size_t)stride;
        if (offset > data.size() || sizeof(float) * 16 > data.size() - offset) return Matrix4();
        float v[16];
        std::memcpy(v, data.data() + offset, sizeof(v));
        return Matrix4(v[0], v[4], v[8], v[12],
                       v[1], v[5], v[9], v[13],
                       v[2], v[6], v[10], v[14],
                       v[3], v[7], v[11], v[15]);
    }

    // the loaded file still has the nodes, meshes and attributes the scene's meshes were built from
    bool sameLayout(const nlohmann::ordered_json& json, const tinygltf::Model& gltf){
        auto arrayOf = [&json](const char* key) -> const nlohmann::ordered_json* {
            auto found = json.find(key);
            return (found != json.end() && found->is_array()) ? &(*found) : nullptr;
        };
        const nlohmann::ordered_json* nodes = arrayOf("nodes");
        const nlohmann::ordered_json* meshes = arrayOf("meshes");
        const nlohmann::ordered_json* accessors = arrayOf("accessors");
        if ((nodes ? nodes->size() : 0) != gltf.nodes.size() || (meshes ? meshes->size() : 0) != gltf.meshes.size() ||
                (accessors ? accessors->size() : 0) != gltf.accessors.size()){
            return false;
        }
        for (size_t n = 0; n < gltf.nodes.size(); n++){
            if (accessorOf((*nodes)[n], "mesh") != gltf.nodes[n].mesh && !(gltf.nodes[n].mesh < 0 && !(*nodes)[n].contains("mesh"))) return false;
        }
        for (size_t m = 0; m < gltf.meshes.size(); m++){
            const nlohmann::ordered_json& mesh = (*meshes)[m];
            if (!mesh.contains("primitives") || !mesh["primitives"].is_array() || mesh["primitives"].size() != gltf.meshes[m].primitives.size()) return false;
            for (size_t p = 0; p < gltf.meshes[m].primitives.size(); p++){
                const tinygltf::Primitive& primitive = gltf.meshes[m].primitives[p];
                const nlohmann::ordered_json& data = mesh["primitives"][p];
                if (!data.contains("attributes") || !data["attributes"].is_object() || data["attributes"].size() != primitive.attributes.size()) return false;
                for (const auto& [semantic, accessor] : primitive.attributes){
                    if (accessorOf(data["attributes"], semantic.c_str()) != accessor) return false;
                }
                const size_t targets = data.contains("targets") && data["targets"].is_array() ? data["targets"].size() : 0;
                if (targets != primitive.targets.size()) return false;
                for (size_t t = 0; t < targets; t++){
                    for (const auto& [semantic, accessor] : primitive.targets[t]){
                        if (accessorOf(data["targets"][t], semantic.c_str()) != accessor) return false;
                    }
                }
            }
        }
        return true;
    }

    // `<name>_lattice.glb` next to the original, never an existing file: `_2`, `_3`... (a model
    // baked before, `<name>_lattice[_n]`, counts on from its base name)
    fs::path bakedPath(const fs::path& source){
        std::string base = source.stem().string();
        const std::string suffix = "_lattice";
        size_t cut = base.rfind(suffix);
        if (cut != std::string::npos && cut > 0){
            const std::string rest = base.substr(cut + suffix.size());
            const bool counter = rest.empty() || (rest.size() > 1 && rest[0] == '_' &&
                                 std::all_of(rest.begin() + 1, rest.end(), [](char c){ return std::isdigit((unsigned char)c) != 0; }));
            if (counter) base = base.substr(0, cut);
        }
        std::error_code ec;
        for (int n = 1;; n++){
            const fs::path candidate = source.parent_path() / editor::FileUtils::pathFromUtf8(base + suffix + (n == 1 ? std::string() : "_" + std::to_string(n)) + ".glb");
            if (!fs::exists(candidate, ec)) return candidate;
        }
    }

}

bool LatticeEditorOps::canBake(Scene* scene, Entity target, std::string* reason){
    BakePlan plan(scene);
    if (!buildPlan(scene, target, plan)){
        if (reason) *reason = plan.reason;
        return false;
    }
    if (reason) reason->clear();
    return true;
}

void LatticeEditorOps::restoreForBake(uint32_t sceneId, Scene* scene, Entity target){
    if (!scene || target == NULL_ENTITY || !scene->isEntityCreated(target)) return;
    // the entities a bake reads, from a first plan: the target, its meshes and models, the
    // modifiers and the bound lattices (their cages' world matrices too: a preview of a root above
    // them restores as well)
    BakePlan probe(scene);
    buildPlan(scene, target, probe);
    std::vector<Entity> involved = {target};
    for (const ResolvedBinding& entry : probe.resolver.bindings){
        involved.push_back(entry.modifier);
        involved.push_back(entry.binding.lattice);
    }
    for (const BakeMesh& mesh : probe.meshes){
        involved.push_back(mesh.entity);
        involved.push_back(mesh.model);
    }
    involved.erase(std::remove_if(involved.begin(), involved.end(), [scene](Entity entity){
        return entity == NULL_ENTITY || !scene->isEntityCreated(entity);
    }), involved.end());
    PoseEditSession::suspendActiveFor(sceneId, involved);
}

bool LatticeEditorOps::bake(Project* project, uint32_t sceneId, Entity target, std::string* path, std::string* note, std::string* error){
    auto fail = [error](const std::string& message){
        if (error) *error = message;
        return false;
    };
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        return fail("scene " + std::to_string(sceneId) + " is not open");
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        return fail("stop the scene first: baking writes a model file and edits the scene");
    }
    Scene* scene = sceneProject->scene;

    // what is baked is the rest, not a preview (final review M2)
    restoreForBake(sceneId, scene, target);

    BakePlan plan(scene);
    if (!buildPlan(scene, target, plan)){
        return fail(plan.reason);
    }

    // the models to write, in the order their meshes came
    std::vector<Entity> modelOrder;
    for (const BakeMesh& mesh : plan.meshes){
        bool changes = false;
        for (const BakeSubmesh& submesh : mesh.submeshes) changes = changes || !submesh.chosen.empty();
        if (changes && std::find(modelOrder.begin(), modelOrder.end(), mesh.model) == modelOrder.end()){
            modelOrder.push_back(mesh.model);
        }
    }

    std::vector<fs::path> written;
    auto rollback = [&written](){
        std::error_code ec;
        for (const fs::path& file : written) FileUtils::removePath(file, ec);
    };

    std::vector<LatticeBakeCmd::ModelChange> modelChanges;
    std::vector<std::string> storedPaths;
    for (Entity modelEntity : modelOrder){
        ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
        const fs::path source = project->resolveAssetPath(model.filename);
        LatticeBakeGltf document;
        std::string message;
        if (!document.load(source, message)){
            rollback();
            return fail(message);
        }
        if (!sameLayout(document.getJson(), *model.gltfModel)){
            rollback();
            return fail("'" + model.filename + "' changed on disk since it was loaded: load the model again (Model File) and bake");
        }

        // the node transforms a merge bakes in (MeshSystem::loadGLTF)
        std::map<int, int> parents;
        const tinygltf::Model& gltf = *model.gltfModel;
        for (size_t n = 0; n < gltf.nodes.size(); n++) parents[(int)n] = -1;
        for (size_t n = 0; n < gltf.nodes.size(); n++){
            for (int child : gltf.nodes[n].children) parents[child] = (int)n;
        }

        for (const BakeMesh& mesh : plan.meshes){
            if (mesh.model != modelEntity) continue;
            const MeshStack& stack = plan.resolver.stackOf(mesh.entity);
            const MeshComponent& meshComponent = scene->getComponent<MeshComponent>(mesh.entity);
            bool morphWeights = false;
            for (int w = 0; w < MAX_MORPHTARGETS; w++) morphWeights = morphWeights || meshComponent.morphWeights[w] != 0.0f;
            bool morphNoted = false;

            for (const BakeSubmesh& submesh : mesh.submeshes){
                if (submesh.chosen.empty()) continue;
                std::vector<lattice::ObjectSpaceLattice> chain;
                for (size_t u : submesh.chosen){
                    const BindingUse& use = stack.accepted[u];
                    const ResolvedBinding& entry = plan.resolver.bindings[use.binding];
                    const ResolvedLattice& lattice = plan.resolver.lattices[entry.lattice];
                    lattice::ObjectSpaceLattice binding;
                    binding.lattice = lattice.component;
                    binding.padded = &lattice.padded;
                    binding.objectToNormalized = use.toNormalized;
                    binding.normalizedToObject = use.toObject;
                    binding.weight = entry.binding.weight;
                    // without vertex colours the mask is 1, as on the GPU
                    binding.mask = submesh.color ? entry.binding.mask : LatticeMaskChannel::NONE;
                    chain.push_back(binding);
                }

                VertexSpace space;
                if (mesh.merged){
                    const tinygltf::Node& node = gltf.nodes[(size_t)submesh.node];
                    if (node.skin >= 0 && (size_t)node.skin < gltf.skins.size() && !gltf.skins[(size_t)node.skin].joints.empty()){
                        space.matrix = nodeGlobalMatrix(gltf, gltf.skins[(size_t)node.skin].joints[0], parents) *
                                       inverseBindMatrix(gltf, node.skin, 0);
                    }else{
                        space.matrix = nodeGlobalMatrix(gltf, submesh.node, parents);
                    }
                    space.linear = space.matrix.linear();
                    const float determinant = space.linear.determinant();
                    space.inverse = space.matrix.inverse();
                    if (!std::isfinite(determinant) || determinant == 0.0f || !space.inverse.isValid()){
                        rollback();
                        return fail("node " + std::to_string(submesh.node) + " of '" + model.filename +
                                    "' is scaled to zero, so its merged vertices cannot be taken back to the file");
                    }
                    space.merged = true;
                    // the normal matrix exactly as the load computes it
                    space.normalMatrix = space.linear.inverse(1e-6f).transpose();
                    space.linearInverse = space.inverse.linear();
                    space.linearTranspose = space.linear.transpose();
                }

                if (!bakePrimitive(document, submesh, chain, space, message)){
                    rollback();
                    return fail("'" + model.filename + "': " + message);
                }
                if (morphWeights && !morphNoted && !gltf.meshes[(size_t)submesh.mesh].primitives[(size_t)submesh.primitive].targets.empty()){
                    plan.notes.push_back(labelOf(scene, mesh.entity) + " has morph target weights other than 0: its morph targets now displace the baked shape "
                                         "(the same at weights 0 and 1, close in between).");
                    morphNoted = true;
                }
            }
        }

        const fs::path destination = bakedPath(source);
        if (!document.saveBinary(destination, message)){
            rollback();
            return fail(message);
        }
        written.push_back(destination);

        // stored as the Model stores its file: relative to the assets, next to the original
        const fs::path stored = fs::path(model.filename).is_absolute() ? destination
                              : fs::path(model.filename).parent_path() / destination.filename();
        LatticeBakeCmd::ModelChange change;
        change.entity = modelEntity;
        change.filename = stored.generic_string();
        modelChanges.push_back(change);
        storedPaths.push_back(change.filename);
    }

    // the baked bindings leave their modifiers
    std::map<Entity, std::vector<size_t>> removed;
    for (size_t binding = 0; binding < plan.baked.size(); binding++){
        if (plan.baked[binding]) removed[plan.resolver.bindings[binding].modifier].push_back(plan.resolver.bindings[binding].index);
    }
    std::vector<LatticeBakeCmd::BindingsChange> bindingChanges;
    for (const auto& [modifierEntity, indices] : removed){
        const LatticeModifierComponent& modifier = scene->getComponent<LatticeModifierComponent>(modifierEntity);
        LatticeBakeCmd::BindingsChange change;
        change.entity = modifierEntity;
        change.includeChildren = modifier.includeChildren;
        for (size_t b = 0; b < modifier.bindings.size(); b++){
            if (std::find(indices.begin(), indices.end(), b) == indices.end()) change.bindings.push_back(modifier.bindings[b]);
        }
        bindingChanges.push_back(std::move(change));
    }

    Command* command = new LatticeBakeCmd(project, sceneId, modelChanges, bindingChanges);
    command->setNoMerge();
    if (!CommandHandle::get(sceneId)->addCommand(command)){
        rollback();
        return fail("the baked model could not be loaded (see the Console); nothing was changed");
    }

    // the Project window lists the new file
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()){
        projectWindow->refreshCurrentDirectory();
    }

    if (path){
        std::string joined;
        for (const std::string& stored : storedPaths) joined += (joined.empty() ? "" : ", ") + stored;
        *path = joined;
    }
    if (note){
        std::string joined;
        for (const std::string& line : plan.notes) joined += (joined.empty() ? "" : "\n") + line;
        *note = joined;
    }
    return true;
}
