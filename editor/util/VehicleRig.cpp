// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VehicleRig.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WheelColliderComponent.h"
#include "math/AABB.h"
#include "util/WaterEditorUtils.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;

Entity editor::createVehicleRig(Project* project, uint32_t sceneId, Entity target, Entity parent, std::string& error){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        error = "no such scene";
        return NULL_ENTITY;
    }
    Scene* scene = sceneProject->scene;
    if (target != NULL_ENTITY && (!scene->isEntityCreated(target) || !scene->findComponent<Transform>(target))){
        error = "the selected GameObject has no Transform";
        return NULL_ENTITY;
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    auto add = [&](Command* command){
        command->setNoMerge();
        return history->addCommand(command);
    };
    auto fail = [&](const std::string& why){
        while (history->position() > start && history->canUndo() && history->undo()) {}
        error = why;
        return NULL_ENTITY;
    };

    // the car's box in the vehicle's space: the mesh's bounds, else a 2 x 1 x 4 m body over the ground
    Entity vehicle = target;
    AABB bounds;
    if (vehicle != NULL_ENTITY){
        bounds = WaterEditorUtils::meshSourceLocalBounds(scene, vehicle);
    }
    if (vehicle == NULL_ENTITY){
        CreateEntityCmd* create = new CreateEntityCmd(project, sceneId, "Vehicle", EntityCreationType::GAMEOBJECT, parent);
        if (!add(create)){
            return fail("could not create the Vehicle GameObject");
        }
        vehicle = create->getEntity();
        CreateEntityCmd* body = new CreateEntityCmd(project, sceneId, "Body", EntityCreationType::BOX, vehicle);
        if (!add(body)){
            return fail("could not create the vehicle's Body mesh");
        }
        MultiPropertyCmd* bodyPose = new MultiPropertyCmd();
        bodyPose->addPropertyCmd<Vector3>(project, sceneId, body->getEntity(), ComponentType::Transform, "position", Vector3(0.0f, 0.8f, 0.0f));
        bodyPose->addPropertyCmd<Vector3>(project, sceneId, body->getEntity(), ComponentType::Transform, "scale", Vector3(2.0f, 0.8f, 4.0f));
        add(bodyPose);
    }
    if (bounds.isNull()){
        bounds = AABB(Vector3(-1.0f, 0.4f, -2.0f), Vector3(1.0f, 1.2f, 2.0f));
    }
    const Vector3 size = bounds.getSize();
    const Vector3 boundsCenter = bounds.getCenter();
    Vector3 scale(1.0f, 1.0f, 1.0f);
    if (const Transform* transform = scene->findComponent<Transform>(vehicle)){
        scale = Vector3(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z));
    }

    // the body: dynamic, a car's mass, its box collider
    RigidbodyComponent body;
    if (const RigidbodyComponent* existing = scene->findComponent<RigidbodyComponent>(vehicle)){
        body = *existing;
    }else if (!add(new AddComponentCmd(project, sceneId, vehicle, ComponentType::RigidbodyComponent))){
        return fail("could not add a Rigidbody to " + scene->getEntityName(vehicle));
    }
    body.type = BodyType::DYNAMIC;
    body.automaticMass = false;
    body.mass = 1500.0f;
    Collider3D box;
    box.type = Collider3DType::BOX;
    box.position = boundsCenter;
    box.width = std::max(0.01f, size.x);
    box.height = std::max(0.01f, size.y);
    box.depth = std::max(0.01f, size.z);
    body.colliders = {box};
    MultiPropertyCmd* bodyValues = new MultiPropertyCmd();
    bodyValues->addPropertyCmd<int>(project, sceneId, vehicle, ComponentType::RigidbodyComponent, "type", (int)body.type);
    bodyValues->addPropertyCmd<bool>(project, sceneId, vehicle, ComponentType::RigidbodyComponent, "automaticMass", body.automaticMass);
    bodyValues->addPropertyCmd<float>(project, sceneId, vehicle, ComponentType::RigidbodyComponent, "mass", body.mass);
    bodyValues->addPropertyCmd<std::vector<Collider3D>>(project, sceneId, vehicle, ComponentType::RigidbodyComponent, "colliders", body.colliders);
    add(bodyValues);

    // the wheels at the bounds' corners, a fifth of the way in from the ends; the wheel's
    // centre rests at the bounds' bottom (Wheel Collider: its position is the top of its travel)
    const WheelColliderComponent defaults;
    const float worldHeight = size.y * scale.y;
    const float radius = std::clamp(0.35f * worldHeight, 0.2f, 0.5f);
    const float restDrop = defaults.suspensionDistance * (1.0f - defaults.suspensionSpring.targetPosition);
    const float x = bounds.getMaximum().x - 0.5f * radius / std::max(1.0e-3f, scale.x);
    const float z = bounds.getMaximum().z - 0.2f * size.z;
    const float y = bounds.getMinimum().y + restDrop / std::max(1.0e-3f, scale.y);
    struct Corner{ const char* name; float x; float z; };
    const Corner corners[] = {
        {"Wheel Front Left", -1.0f, 1.0f}, {"Wheel Front Right", 1.0f, 1.0f},
        {"Wheel Rear Left", -1.0f, -1.0f}, {"Wheel Rear Right", 1.0f, -1.0f},
    };
    for (const Corner& corner : corners){
        CreateEntityCmd* create = new CreateEntityCmd(project, sceneId, corner.name, EntityCreationType::GAMEOBJECT, vehicle);
        if (!add(create)){
            return fail(std::string("could not create ") + corner.name);
        }
        const Entity wheel = create->getEntity();
        const Vector3 local(boundsCenter.x + corner.x * (x - boundsCenter.x), y, boundsCenter.z + corner.z * (z - boundsCenter.z));
        if (!add(new AddComponentCmd(project, sceneId, wheel, ComponentType::WheelColliderComponent))){
            return fail(std::string("could not add a Wheel Collider to ") + corner.name);
        }
        MultiPropertyCmd* values = new MultiPropertyCmd();
        values->addPropertyCmd<Vector3>(project, sceneId, wheel, ComponentType::Transform, "position", local);
        values->addPropertyCmd<float>(project, sceneId, wheel, ComponentType::WheelColliderComponent, "radius", radius);
        add(values);
    }

    history->groupSince(start);
    project->setSelectedEntity(sceneId, vehicle);
    return vehicle;
}
