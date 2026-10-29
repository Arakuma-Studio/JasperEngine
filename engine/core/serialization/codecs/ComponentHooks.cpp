// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The components whose cooked state the generated field tables do not hold whole, and what the
// exported code (editor/Factory.cpp, the reflection tables' codegen) sets on a new component
// besides its fields. Each hook writes and reads the same members with the same codecs, so a
// cooked component reads back as written; the editor's CookedScene.RoundTripEveryComponent
// checks every component of the reflection tables against its scene codec.

#include "serialization/RuntimeFields.h"
#include "serialization/codecs/CookTypes.h"

#include "util/Fnv.h"

using namespace jasperengine;
using namespace jasperengine::cook;

namespace {

    // ---------------------------------------------------------------- Mesh

    // Factory::createMeshComponent: the geometry of a mesh whose model or heightmapped terrain
    // rebuilds it at load is not shipped; nor are the bones of a generated mesh
    void writeMesh(Writer& w, const void* component, const WriteContext& ctx) {
        const MeshComponent& mesh = *static_cast<const MeshComponent*>(component);
        const bool runtimeGeometry = ctx.modelBackedMesh || ctx.terrainHeightmap;
        w.varint(mesh.vertexCount);
        w.boolean(mesh.shadowsBillboard);
        write(w, mesh.windingOrder);
        w.f32(mesh.normAdjustJoint);
        w.f32(mesh.normAdjustWeight);
        // the weights a load or an edit set, or non-zero ones set before any load, up to the
        // targets they cover (Stream and the exported code agree on it)
        int present = 0;
        for (int i = 0; i < MAX_MORPHTARGETS; i++)
            if (mesh.morphWeights[i] != 0.0f) present = i + 1;
        const bool weightsSet = mesh.morphWeightsSet || present > 0;
        w.boolean(weightsSet);
        w.svarint(weightsSet ? (mesh.morphWeightsSet ? mesh.morphTargetCount : present) : mesh.morphTargetCount);

        w.varint(mesh.numSubmeshes);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            const Submesh& submesh = mesh.submeshes[s];
            w.varint(submesh.vertexCount);
            w.u64(submesh.overrideFields);
            w.boolean(submesh.hasSkinningNormalization);
            w.f32(submesh.normAdjustJoint);
            w.f32(submesh.normAdjustWeight);
            // a model load rebuilds the attributes
            if (ctx.modelBackedMesh) {
                write(w, std::map<AttributeType, Attribute>());
            } else {
                write(w, submesh.attributes);
            }
        }

        w.boolean(!runtimeGeometry);
        if (!runtimeGeometry) {
            write(w, mesh.buffer);
            write(w, mesh.indices);
            w.varint(mesh.numExternalBuffers);
            for (unsigned int b = 0; b < mesh.numExternalBuffers; b++) write(w, mesh.eBuffers[b]);
        }

        // the bones that pose a mesh no skeleton drives (Factory: the non-identity ones)
        const bool bones = !runtimeGeometry && !ctx.generatedMesh;
        std::vector<std::pair<uint32_t, Matrix4>> posed;
        if (bones) {
            const Matrix4 identity;
            for (size_t b = 0; b < mesh.bonesMatrix.size(); b++)
                if (!(mesh.bonesMatrix[b] == identity)) posed.emplace_back(static_cast<uint32_t>(b), mesh.bonesMatrix[b]);
        }
        write(w, posed);
    }

    void readMesh(Reader& r, void* component, const LoadContext&) {
        MeshComponent& mesh = *static_cast<MeshComponent*>(component);
        mesh.vertexCount = static_cast<unsigned int>(r.varint());
        mesh.shadowsBillboard = r.boolean();
        read(r, mesh.windingOrder);
        mesh.normAdjustJoint = r.f32();
        mesh.normAdjustWeight = r.f32();
        mesh.morphWeightsSet = r.boolean();
        mesh.morphTargetCount = static_cast<int>(r.svarint());

        const uint64_t submeshes = r.varint();
        for (uint64_t s = 0; s < submeshes && !r.failed(); s++) {
            Submesh dropped;
            Submesh& submesh = mesh.submeshes.validIndex(static_cast<int>(s)) ? mesh.submeshes[static_cast<size_t>(s)] : dropped;
            submesh.vertexCount = static_cast<unsigned int>(r.varint());
            submesh.overrideFields = r.u64();
            submesh.hasSkinningNormalization = r.boolean();
            submesh.normAdjustJoint = r.f32();
            submesh.normAdjustWeight = r.f32();
            read(r, submesh.attributes);
        }

        if (r.boolean()) {
            read(r, mesh.buffer);
            read(r, mesh.indices);
            const uint64_t external = r.varint();
            unsigned int kept = 0;
            for (uint64_t b = 0; b < external && !r.failed(); b++) {
                if (mesh.eBuffers.validIndex(static_cast<int>(b))) {
                    read(r, mesh.eBuffers[static_cast<size_t>(b)]);
                    kept++;
                } else {
                    ExternalBuffer dropped;
                    read(r, dropped);
                }
            }
            mesh.numExternalBuffers = kept;
        }

        std::vector<std::pair<uint32_t, Matrix4>> posed;
        read(r, posed);
        for (const auto& [index, matrix] : posed)
            if (mesh.bonesMatrix.validIndex(static_cast<int>(index))) mesh.bonesMatrix[index] = matrix;
        if (!posed.empty()) mesh.needUpdateBones = true;
    }

    // ---------------------------------------------------------------- Audio Source

    // a build starts every source stopped; a playing one through its start trigger
    void afterAudioSource(void* component, const LoadContext&) {
        AudioSourceComponent& audio = *static_cast<AudioSourceComponent*>(component);
        audio.needUpdate = true;
        if (audio.state == AudioSourceState::Playing) audio.startTrigger = true;
        audio.state = AudioSourceState::Stopped;
    }

    // ---------------------------------------------------------------- Action

    // an exported action starts as it was saved: running or paused ones through their triggers
    void afterAction(void* component, const LoadContext&) {
        ActionComponent& action = *static_cast<ActionComponent*>(component);
        if (action.state == ActionState::Running) {
            action.state = ActionComponent().state;
            action.startTrigger = true;
        } else if (action.state == ActionState::Paused) {
            action.state = ActionComponent().state;
            action.startTrigger = true;
            action.pauseTrigger = true;
        }
    }

    // ---------------------------------------------------------------- Terrain

    void afterTerrain(void* component, const LoadContext&) {
        TerrainComponent& terrain = *static_cast<TerrainComponent*>(component);
        terrain.needUpdateTerrain = true;
        terrain.needUpdateTexture = true;
        terrain.needUpdateFoliage = true;
        terrain.heightMapLoaded = false;
    }

    // ---------------------------------------------------------------- Rigidbodies

    // the exported code carries the editor's body flags over
    template<typename Body>
    void writeBodyFlags(Writer& w, const void* component, const WriteContext&) {
        const Body& body = *static_cast<const Body*>(component);
        w.boolean(body.needReloadBody);
        w.boolean(body.needUpdateColliders);
    }

    template<typename Body>
    void readBodyFlags(Reader& r, void* component, const LoadContext&) {
        Body& body = *static_cast<Body*>(component);
        body.needReloadBody = r.boolean();
        body.needUpdateColliders = r.boolean();
    }

    const ComponentHooks kHooks[] = {
        {fnv1a32("mesh"), &writeMesh, &readMesh, nullptr},
        {fnv1a32("audiosource"), nullptr, nullptr, &afterAudioSource},
        {fnv1a32("action"), nullptr, nullptr, &afterAction},
        {fnv1a32("terrain"), nullptr, nullptr, &afterTerrain},
#ifdef JASPERENGINE_PHYSICS_2D
        {fnv1a32("rigidbody2d"), &writeBodyFlags<Rigidbody2DComponent>, &readBodyFlags<Rigidbody2DComponent>, nullptr},
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        {fnv1a32("rigidbody"), &writeBodyFlags<RigidbodyComponent>, &readBodyFlags<RigidbodyComponent>, nullptr},
#endif
    };

}

const ComponentHooks* cook::componentHooks(uint32_t keyHash) {
    for (const ComponentHooks& hooks : kHooks)
        if (hooks.keyHash == keyHash) return &hooks;
    return nullptr;
}
