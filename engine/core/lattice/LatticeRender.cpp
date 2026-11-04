// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeRender.h"

#include "Log.h"
#include "Scene.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeSolver.h"
#include "subsystem/RenderSystem.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    static_assert(sizeof(Vector4) == sizeof(float) * 4, "lattice texels are uploaded as RGBA32F");

    // How far the linear part of a matrix can stretch a unit vector (its spectral norm): the
    // longest column when the columns are orthogonal (rotation and scale), else the root sum
    // of their squares, a bound of it (a skew: a rotated child of a non-uniformly scaled parent)
    float linearScale(const Matrix4& m){
        const Vector3 c0(m[0][0], m[0][1], m[0][2]);
        const Vector3 c1(m[1][0], m[1][1], m[1][2]);
        const Vector3 c2(m[2][0], m[2][1], m[2][2]);
        const float l0 = c0.length();
        const float l1 = c1.length();
        const float l2 = c2.length();
        const float tolerance = 1e-4f;
        const bool orthogonal = std::fabs(c0.dotProduct(c1)) <= tolerance * l0 * l1 &&
                                std::fabs(c1.dotProduct(c2)) <= tolerance * l1 * l2 &&
                                std::fabs(c2.dotProduct(c0)) <= tolerance * l2 * l0;
        if (orthogonal)
            return std::max(l0, std::max(l1, l2));
        return std::sqrt(l0 * l0 + l1 * l1 + l2 * l2);
    }

    // the shader's mask channel code (includes/lattice.glsl): -1 none, 0-3 rgba, 4-7 inverted
    int maskChannel(LatticeMaskChannel mask){
        return static_cast<int>(mask) - 1;
    }

    // a lattice a binding names, validated and its padded grid built once a frame: an index
    // into state.lattices (whose entries keep their grids' storage between frames)
    size_t findLattice(Scene* scene, LatticeRenderState& state, Entity entity){
        for (size_t i = 0; i < state.latticeCount; i++){
            if (state.lattices[i].entity == entity)
                return i;
        }
        if (state.latticeCount == state.lattices.size())
            state.lattices.emplace_back();
        const size_t index = state.latticeCount++;
        LatticeRenderState::Lattice& lattice = state.lattices[index];
        lattice.entity = entity;
        lattice.component = nullptr;
        lattice.maxOffset = 0.0f;
        lattice.worldScale = 0.0f;
        lattice.valid = false;

        // a deleted entity, or one without a lattice: skipped silently (the inspector shows it)
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity))
            return index;
        const LatticeComponent* component = scene->findComponent<LatticeComponent>(entity);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!component || !transform || !component->enabled || !scene->isEntityActiveInHierarchy(entity))
            return index;

        // a lattice with a non-invertible transform (zero scale) deforms nothing
        const float determinant = transform->modelMatrix.linear().determinant();
        if (!transform->modelMatrix.isValid() || !std::isfinite(determinant) || determinant == 0.0f ||
                !transform->modelMatrix.inverse().isValid())
            return index;

        // as the solver reads it: the resolution clamped, offsets missing from a short list zero
        lattice.resolution[0] = std::min(std::max(component->resolutionX, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
        lattice.resolution[1] = std::min(std::max(component->resolutionY, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
        lattice.resolution[2] = std::min(std::max(component->resolutionZ, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
        lattice::buildPaddedGrid(*component, lattice.padded);
        if (lattice.padded.size() != (size_t)(lattice.resolution[0] + 2) * (size_t)(lattice.resolution[1] + 2) * (size_t)(lattice.resolution[2] + 2))
            return index;
        lattice.maxOffset = lattice::maxPaddedOffset(lattice.padded);
        if (!std::isfinite(lattice.maxOffset))
            return index;

        lattice.component = component;
        lattice.model = transform->modelMatrix;
        lattice.worldScale = linearScale(transform->modelMatrix);
        lattice.valid = true;
        return index;
    }

    // what a mesh that lost its bindings goes back to
    void resetMesh(MeshComponent& mesh){
        LatticeMeshState& state = mesh.lattice;
        if (state.worldPadding != 0.0f)
            mesh.needUpdateAABB = true;
        state.count = 0;
        state.texels.clear();
        state.texels.shrink_to_fit();
        for (int b = 0; b < LATTICE_MAX_PER_MESH; b++){
            state.recordStart[b] = 0.0f;
            state.submesh[b] = -1;
        }
        state.movingBindings = 0;
        state.worldPadding = 0.0f;
        state.needUpload = false;
        state.wantsTessellation = false;
        state.tessellation = 1;
        // the variant without the lattice: the loaded one draws (undeformed: count 0) until
        // its shaders are built, then the mesh reloads (and frees the texture)
        if (state.variantActive && mesh.loadCalled)
            mesh.needVariantReload = true;
    }

}

void LatticeRender::prepare(Scene* scene){
    if (!scene)
        return;
    prepare(scene, scene->getSystem<RenderSystem>()->getLatticeState());
}

void LatticeRender::prepare(Scene* scene, LatticeRenderState& state){
    auto modifiers = scene->getComponentArray<LatticeModifierComponent>();
    // nothing bound now or last frame: nothing to do
    if (modifiers->size() == 0 && state.meshes.empty())
        return;

    auto transforms = scene->getComponentArray<Transform>();
    const ComponentId transformId = scene->getComponentId<Transform>();
    const ComponentId modifierId = scene->getComponentId<LatticeModifierComponent>();
    const ComponentId meshId = scene->getComponentId<MeshComponent>();
    const ComponentId terrainId = scene->getComponentId<TerrainComponent>();

    state.filled.clear();
    state.latticeCount = 0;
    state.path.clear();
    state.overflow.assign(modifiers->size(), NULL_ENTITY);

    // where the modifiers are in the hierarchy (the Transform array is in hierarchy order,
    // every branch contiguous after its root)
    state.modifierStarts.clear();
    for (size_t m = 0; m < modifiers->size(); m++){
        const Entity entity = modifiers->getEntity(m);
        if (scene->getSignature(entity).test(transformId))
            state.modifierStarts.push_back(transforms->getIndex(entity));
    }
    std::sort(state.modifierStarts.begin(), state.modifierStarts.end());

    LatticeRenderState::Accepted accepted[LATTICE_MAX_PER_MESH];
    int count = 0;

    // the bindings of one modifier, in order, that deform the mesh being filled. A lattice
    // already taken from a nearer modifier (the mesh's own, then nearer ancestors) is skipped:
    // each lattice deforms a mesh once, and a child's own binding (weight 0: excluded)
    // overrides the one it inherits (spec §12 Agent UX pass)
    auto consider = [&](const LatticeModifierComponent& modifier, size_t modifierIndex, Entity meshEntity, const Transform& meshTransform){
        const int nearer = count;
        for (const LatticeBinding& binding : modifier.bindings){
            const size_t latticeIndex = findLattice(scene, state, binding.lattice);
            const LatticeRenderState::Lattice& lattice = state.lattices[latticeIndex];
            if (!lattice.valid || !std::isfinite(binding.weight))
                continue;
            bool overridden = false;
            for (int b = 0; b < nearer; b++)
                overridden = overridden || accepted[b].lattice == latticeIndex;
            if (overridden)
                continue;
            const Matrix4 toNormalized = lattice::objectToNormalized(lattice.model, meshTransform.modelMatrix);
            const Matrix4 toObject = toNormalized.inverse();
            // a mesh with a non-invertible transform has no object space to go back to
            if (!toNormalized.isValid() || !toObject.isValid())
                continue;
            if (count == LATTICE_MAX_PER_MESH){
                if (state.overflow[modifierIndex] == NULL_ENTITY)
                    state.overflow[modifierIndex] = meshEntity;
                continue;
            }
            LatticeRenderState::Accepted& slot = accepted[count++];
            slot.lattice = latticeIndex;
            slot.toNormalized = toNormalized;
            slot.toObject = toObject;
            slot.weight = binding.weight;
            slot.stage = (binding.stage == LatticeStage::BEFORE_SKINNING) ? 0 : 1;
            slot.mask = maskChannel(binding.mask);
            slot.submesh = binding.submesh;
            slot.modifier = &modifier;
        }
    };

    // a mesh's bindings (own first, then inherited, nearest ancestor first) into its state
    auto fill = [&](Entity entity, MeshComponent& mesh, const Transform& transform, LatticeModifierComponent* own, size_t ownIndex){
        count = 0;
        if (own && own->enabled)
            consider(*own, ownIndex, entity, transform);
        for (size_t p = state.path.size(); p-- > 0;){
            if (state.path[p].inherited)
                consider(*state.path[p].inherited, state.path[p].modifierIndex, entity, transform);
        }
        if (count == 0)
            return; // reset below if it had bindings

        // A before-skinning offset is added in the bind pose's object space and then carried
        // by a bone (object space) and the model matrix: model * bone * toObject * delta. Its
        // bound is |model * bone| * |toObject| (not the lattice's scale times the bone's: with
        // a non-uniform model scale, model * bone * model^-1 stretches what a turned bone
        // points along the long axis). Without skinning every binding is an after one.
        bool skinned = false;
        for (unsigned int s = 0; s < mesh.numSubmeshes && !skinned; s++)
            skinned = mesh.submeshes[s].hasSkinning;
        float boneScale = 0.0f;
        if (skinned){
            for (size_t b = 0; b < mesh.bonesMatrix.stored(); b++)
                boneScale = std::max(boneScale, linearScale(transform.modelMatrix * mesh.bonesMatrix[b]));
        }

        LatticeMeshState& lattice = mesh.lattice;
        std::vector<Vector4>& texels = state.texels;
        texels.clear();
        float padding = 0.0f;
        uint32_t moving = 0;
        for (int b = 0; b < count; b++){
            const LatticeRenderState::Accepted& binding = accepted[b];
            const LatticeRenderState::Lattice& source = state.lattices[binding.lattice];
            const LatticeComponent& component = *source.component;

            lattice.recordStart[b] = static_cast<float>(texels.size());
            lattice.submesh[b] = binding.submesh;
            // at rest (or weight 0) it moves nothing: left out of the draw, so the mesh
            // draws exactly as without it
            if (binding.weight != 0.0f && source.maxOffset > 0.0f)
                moving |= (1u << b);

            for (int c = 0; c < 4; c++)
                texels.push_back(binding.toNormalized.column(c));
            for (int c = 0; c < 4; c++)
                texels.push_back(binding.toObject.column(c));
            texels.push_back(Vector4(static_cast<float>(source.resolution[0]), static_cast<float>(source.resolution[1]),
                                     static_cast<float>(source.resolution[2]), static_cast<float>(component.interpolation)));
            // a NaN fade distance fades as 0 does (the solver's !(fade > 0))
            const float fadeDistance = std::isnan(component.fadeDistance) ? 0.0f : component.fadeDistance;
            texels.push_back(Vector4(static_cast<float>(binding.stage), binding.weight,
                                     static_cast<float>(component.outside), fadeDistance));
            texels.push_back(Vector4(static_cast<float>(binding.mask), 0.0f, 0.0f, 0.0f));
            texels.push_back(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            for (const Vector3& offset : source.padded)
                texels.push_back(Vector4(offset.x, offset.y, offset.z, 0.0f));

            // spec 5.5: |delta| <= weight * mask * max |P| in local units (the interpolation
            // weights are a partition of unity, the fade at most 1), local to world at most
            // the lattice's scale (model * toObject is the lattice's linear part)
            float growth = std::fabs(binding.weight) * source.maxOffset * source.worldScale;
            if (binding.stage == 0 && skinned)
                growth = std::fabs(binding.weight) * source.maxOffset * linearScale(binding.toObject) * boneScale;
            padding += growth;
        }
        for (int b = count; b < LATTICE_MAX_PER_MESH; b++){
            lattice.recordStart[b] = 0.0f;
            lattice.submesh[b] = -1;
        }
        // whole rows: the texture is LATTICE_TEXTURE_WIDTH wide
        const size_t rows = (texels.size() + LATTICE_TEXTURE_WIDTH - 1) / LATTICE_TEXTURE_WIDTH;
        texels.resize(rows * LATTICE_TEXTURE_WIDTH, Vector4(0.0f, 0.0f, 0.0f, 0.0f));

        if (texels.size() != lattice.texels.size() ||
                std::memcmp(texels.data(), lattice.texels.data(), texels.size() * sizeof(Vector4)) != 0){
            lattice.texels.swap(texels);
            lattice.needUpload = true;
        }
        lattice.count = count;
        lattice.movingBindings = moving;
        if (!std::isfinite(padding))
            padding = 0.0f;
        if (padding != lattice.worldPadding){
            lattice.worldPadding = padding;
            mesh.needUpdateAABB = true;
        }
        // a sprite's quad becomes a grid (MeshSystem), as the first binding's modifier asks
        lattice.wantsTessellation = true;
        lattice.tessellation = std::min(std::max(accepted[0].modifier->subdivisions, 1), 64);

        // the lattice variant: the loaded one draws until its shaders are built
        if (!lattice.variantActive && mesh.loadCalled)
            mesh.needVariantReload = true;

        state.filled.push_back(entity);
    };

    // One pass down the hierarchy from the first modifier: the path holds the ancestors of the
    // entity visited, so what it inherits is there; past the branches that hand bindings down
    // it skips to the next modifier
    size_t next = 0;
    size_t inheriting = 0;
    size_t i = state.modifierStarts.empty() ? transforms->size() : state.modifierStarts[0];
    while (i < transforms->size()){
        const Entity entity = transforms->getEntity(i);
        Transform& transform = transforms->getComponentFromIndex(i);
        while (!state.path.empty() && state.path.back().entity != transform.parent){
            if (state.path.back().inherited)
                inheriting--;
            state.path.pop_back();
        }

        const Signature signature = scene->getSignature(entity);
        LatticeModifierComponent* own = nullptr;
        size_t ownIndex = 0;
        if (signature.test(modifierId)){
            own = &scene->getComponent<LatticeModifierComponent>(entity);
            ownIndex = modifiers->getIndex(entity);
        }
        // a terrain is never deformed
        if (signature.test(meshId) && !signature.test(terrainId)){
            fill(entity, scene->getComponent<MeshComponent>(entity), transform, own, ownIndex);
        }

        LatticeRenderState::PathEntry entry;
        entry.entity = entity;
        entry.inherited = (own && own->enabled && own->includeChildren) ? own : nullptr;
        entry.modifierIndex = ownIndex;
        state.path.push_back(entry);
        if (entry.inherited)
            inheriting++;
        i++;

        if (inheriting == 0){
            state.path.clear();
            while (next < state.modifierStarts.size() && state.modifierStarts[next] < i)
                next++;
            if (next == state.modifierStarts.size())
                break;
            i = state.modifierStarts[next];
        }
    }

    // more than LATTICE_MAX_PER_MESH: once per modifier until it fits again
    for (size_t m = 0; m < modifiers->size(); m++){
        LatticeModifierComponent& modifier = modifiers->getComponentFromIndex(m);
        if (state.overflow[m] == NULL_ENTITY){
            modifier.warnedOverflow = false;
        }else if (!modifier.warnedOverflow){
            modifier.warnedOverflow = true;
            Log::warn("Lattice Modifier of '%s': '%s' is bound to more than %i lattices, the ones after the first %i are ignored",
                      scene->getEntityName(modifiers->getEntity(m)).c_str(), scene->getEntityName(state.overflow[m]).c_str(),
                      LATTICE_MAX_PER_MESH, LATTICE_MAX_PER_MESH);
        }
    }

    // the meshes filled last frame and not this one lost their bindings
    std::sort(state.filled.begin(), state.filled.end());
    size_t f = 0;
    for (Entity entity : state.meshes){
        while (f < state.filled.size() && state.filled[f] < entity)
            f++;
        if (f < state.filled.size() && state.filled[f] == entity)
            continue;
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity))
            resetMesh(*mesh);
    }
    state.meshes.swap(state.filled);
}

void LatticeRender::getUniformBlock(const LatticeMeshState& lattice, unsigned int submesh, bool stretch, Vector4 block[2]){
    uint32_t active = 0;
    for (int b = 0; b < lattice.count && b < LATTICE_MAX_PER_MESH; b++){
        if ((lattice.movingBindings & (1u << b)) && (lattice.submesh[b] < 0 || lattice.submesh[b] == (int)submesh))
            active |= (1u << b);
    }
    block[0] = Vector4(static_cast<float>(lattice.count), static_cast<float>(active), stretch ? 1.0f : 0.0f, 0.0f);
    block[1] = Vector4(lattice.recordStart[0], lattice.recordStart[1], lattice.recordStart[2], lattice.recordStart[3]);
}

bool LatticeRender::isLatticeTarget(EntityRegistry* registry, Entity entity){
    if (!registry || !registry->isEntityCreated(entity))
        return false;
    auto bindsLattice = [registry](const LatticeModifierComponent& modifier){
        for (const LatticeBinding& binding : modifier.bindings){
            if (binding.lattice != NULL_ENTITY && registry->isEntityCreated(binding.lattice) &&
                    registry->findComponent<LatticeComponent>(binding.lattice))
                return true;
        }
        return false;
    };
    if (const LatticeModifierComponent* own = registry->findComponent<LatticeModifierComponent>(entity)){
        if (bindsLattice(*own))
            return true;
    }
    const Transform* transform = registry->findComponent<Transform>(entity);
    // a parent cycle cannot happen through the registry, the bound only guards bad data
    for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < 1024; depth++){
        const Entity parent = transform->parent;
        if (const LatticeModifierComponent* inherited = registry->findComponent<LatticeModifierComponent>(parent)){
            if (inherited->includeChildren && bindsLattice(*inherited))
                return true;
        }
        transform = registry->findComponent<Transform>(parent);
    }
    return false;
}
