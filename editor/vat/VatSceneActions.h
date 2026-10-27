// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.3, 5.3, 5.4)
// - Purpose: a GameObject that plays a Vertex Animation, made in one scene undo step: a .vat
//   dragged into the Scene view or the Hierarchy (Model = "<stem>.vat.glb", Vertex Animator =
//   "<stem>.vat"), and the crowd of Create Crowd (VatCrowd.h: + an Instanced Mesh). Callable
//   without UI (MCP, the baker's Place).
// - Owner: W5. Thread-safety: main thread (scene commands).
// - Invariants: CreateVatObjectCmd is one step of the scene's history: execute creates the
//   GameObject (CreateEntityCmd, MODEL), loads the mesh (ModelLoadCmd) and adds the components;
//   undo removes it all; redo makes it again with the same entity id.

#pragma once

#include "animation/VertexAnimation.h"
#include "command/Command.h"
#include "component/MeshComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;
    class CreateEntityCmd;
    class ModelLoadCmd;

    // A .vat as a GameObject plays it: its forms and its data (the file is read, not opened
    // as a document)
    struct VatObjectSource{
        std::string vat;            // asset-relative (what VertexAnimatorComponent::animation stores)
        std::string mesh;           // asset-relative "<dir>/<data.mesh>" (the Model's filename)
        std::string guid;
        std::string stem;           // "hero" for hero.vat (the GameObject's name)
        jasperengine::vat::VatData data;
    };
    // False with a sentence when the path is not a .vat inside the assets folder, does not parse,
    // or its mesh file is missing
    bool resolveVatObjectSource(Project* project, const std::string& path, VatObjectSource& out, std::string* error);

    class CreateVatObjectCmd : public Command{
    public:
        struct Spec{
            std::string name;
            Entity parent = NULL_ENTITY;
            Vector3 position = Vector3(0, 0, 0);
            Quaternion rotation;
            Vector3 scale = Vector3(1, 1, 1);
            std::string mesh;                       // the Model's filename (asset-relative)
            VertexAnimatorComponent animator;       // its authored fields
            bool instanced = false;                 // + an Instanced Mesh with `instances`
            std::vector<InstanceData> instances;
        };

        CreateVatObjectCmd(Project* project, uint32_t sceneId, Spec spec);
        ~CreateVatObjectCmd() override;

        CreateVatObjectCmd(const CreateVatObjectCmd&) = delete;
        CreateVatObjectCmd& operator=(const CreateVatObjectCmd&) = delete;

        bool execute() override;
        void undo() override;

        // the GameObject (after execute)
        Entity getEntity() const { return entity; }

    private:
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Spec spec;
        Entity entity = NULL_ENTITY;
        bool wasModified = false;
        std::unique_ptr<CreateEntityCmd> createEntity;
        std::unique_ptr<ModelLoadCmd> modelLoad;
    };

    // The drag of a .vat into the Scene view / Hierarchy: a GameObject "<stem>" at `position`
    // (under `parent` when given) with Model = the .vat.glb and Vertex Animator = the .vat, one
    // scene undo step, selected. NULL_ENTITY + error when the .vat can't be used
    Entity createVatObject(Project* project, uint32_t sceneId, const std::string& vatPath, const Vector3& position,
                           Entity parent, std::string* error);

}
