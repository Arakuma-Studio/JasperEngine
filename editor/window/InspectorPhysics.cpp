// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the physics components: bodies with their colliders and joints (their tables cannot
// draw them: per-collider-type rows, shape previews, body-picking rows).

#include "InspectorWindow.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "Engine.h"
#include "object/physics/Joint3D.h"
#include "reflect/Reflect.h"
#include "reflect/Enums.h"
#include "command/PlayChangeReview.h"
#include "command/type/RemovePreservedComponentCmd.h"
#include "StreamPreserved.h"
#include "DocumentFile.h"
#include "SceneWindow.h"

#include "imgui_internal.h"

#include "util/Util.h"
#include "util/UIUtils.h"
#include "util/FileDialogs.h"
#include "window/CodeEditor.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/HtmlUiImportPanel.h"
#include "util/EntityPayload.h"
#include "util/CameraTextureLink.h"
#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/DecomposeColliderCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/MeshChangeCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/ComponentToPrefabSharedCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/AtmospherePresetCmd.h"
#include "render/AtmospherePresets.h"
#include "render/RenderUtil.h"
#include "util/EntityClipboard.h"
#include "command/type/ForkShaderCmd.h"
#include "shader/ShaderManifest.h"
#include "command/type/ScenePropertyCmd.h"
#include "render/SceneRender2D.h"
#include "window/ProjectWindow.h"
#include "window/HierarchyWindow.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "Widgets.h"
#include "Factory.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/Transform.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/LineRendererComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PanelComponent.h"
#include "util/SHA1.h"
#include "util/ProjectUtils.h"
#include "Stream.h"
#include "Out.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "physics/AttachedColliders.h"
#include "util/WaterEditorUtils.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "component/TerrainComponent.h"
#include "subsystem/WaterInteractionSystem.h"
#include "util/WaterExclusion.h"
#include "subsystem/AudioSystem.h"
#include "action/Action.h"
#include "pool/TexturePool.h"
#include "yaml-cpp/yaml.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <map>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <cctype>
#include <cfloat>
#include <algorithm>
#include <unordered_set>
#include <memory>
#include "script/ScriptMessages.h"

using namespace jasperengine;

// drawRigidbody2DComponent / drawJoint2DComponent: upstream's (physics-2d) in InspectorPhysics2D.cpp


// upstream's (physics-3d) enums without a reflection table of their own yet
static std::vector<editor::EnumEntry> entriesJointMotion3D = {
    { (int)JointMotion::LOCKED, "Locked" },
    { (int)JointMotion::LIMITED, "Limited" },
    { (int)JointMotion::FREE, "Free" }
};

static std::vector<editor::EnumEntry> entriesJointSwingType3D = {
    { (int)JointSwingType::CONE, "Cone" },
    { (int)JointSwingType::PYRAMID, "Pyramid" }
};

static std::vector<editor::EnumEntry> entriesJointAnchorSpace3D = {
    { (int)JointAnchorSpace::WORLD, "World" },
    { (int)JointAnchorSpace::LOCAL, "Local" }
};

static std::vector<editor::EnumEntry> entriesJointBreakAction3D = {
    { (int)JointBreakAction::IGNORE, "Ignore" },
    { (int)JointBreakAction::CALLBACK_ONLY, "Callback Only" },
    { (int)JointBreakAction::DISABLE, "Disable" },
    { (int)JointBreakAction::DESTROY, "Destroy" }
};

static std::vector<editor::EnumEntry> entriesRigidbodyInterpolation = {
    { (int)RigidbodyInterpolation::SCENE, "Scene" },
    { (int)RigidbodyInterpolation::NONE, "None" },
    { (int)RigidbodyInterpolation::INTERPOLATE, "Interpolate" },
    { (int)RigidbodyInterpolation::EXTRAPOLATE, "Extrapolate" }
};

void editor::InspectorWindow::drawRigidbodyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RigidbodyComponent& body = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]);

    RowSettings settingsBodyType;
    settingsBodyType.enumEntries = enumEntries(reflect::enums::kBodyType);

    RowSettings settingsMotionQuality;
    settingsMotionQuality.enumEntries = enumEntries(reflect::enums::kRigidbodyMotionQuality);

    RowSettings settingsBodyValue;

    RowSettings settingsInterpolation;
    settingsInterpolation.enumEntries = &entriesRigidbodyInterpolation;
    settingsInterpolation.help = "How it is drawn between fixed steps. Scene: the scene's Physics Interpolation";
    RowSettings settingsMass;
    settingsMass.help = "Mass in kg. With Automatic it comes from the colliders (volume x density; triggers do not count)";
    RowSettings settingsDamping;
    settingsDamping.help = "Slows the body down (Unity Drag / Angular Drag); 0.05 by default, Jolt's";
    RowSettings settingsCenter;
    settingsCenter.help = "Body-local, in world units (not scaled). Automatic: the colliders' centre of mass";
    RowSettings settingsInertia;
    settingsInertia.help = "Principal moments of inertia (kg m\xC2\xB2) about the centre of mass. Automatic: from the colliders";
    RowSettings settingsKinematicContacts;
    settingsKinematicContacts.help = "A kinematic body also touches static and kinematic bodies (one with a trigger collider always does)";
    RowSettings settingsSurface;
    settingsSurface.help = "The surface of the colliders without a material (theirs or the body's): two of them combine by geometric mean friction and the larger restitution";

    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;

    beginTable(cpType, getLabelSize("Contacts With Static/Kinematic"));
    // S96 WP-C: Unity's colliders below a Rigidbody; the body fields of an attached child are not used
    RowSettings settingsAttach;
    settingsAttach.help = "Its colliders join the body of the nearest parent with a Rigidbody (their contacts reach this GameObject too); it has no body of its own";
    propertyRow(RowPropertyType::Bool, cpType, "attachToParentBody", "Attach To Parent Body", sceneProject, entities, settingsAttach);
    bool attachedToParent = false;
    if (body.attachToParentBody && entities.size() == 1){
        const Entity joined = AttachedColliders::findBody3D(sceneProject->scene, entities[0]);
        attachedToParent = joined != NULL_ENTITY;
        propertyHeader("");
        if (attachedToParent){
            ImGui::TextDisabled("Joins the body of \"%s\"", sceneProject->scene->getEntityName(joined).c_str());
        }else{
            ImGui::TextDisabled("No parent body: this is its own body");
        }
    }
    if (attachedToParent){
        ImGui::BeginDisabled();
    }
    propertyRow(RowPropertyType::Enum, cpType, "type", "Body Type", sceneProject, entities, settingsBodyType);
    if (body.type != BodyType::STATIC){
        propertyRowWithAutoButton(RowPropertyType::FloatPositive, cpType, "mass", "Mass", "automaticMass", "Automatic Mass (from the colliders)", sceneProject, entities, settingsMass);
        if (body.automaticMass && entities.size() == 1){
            // what the colliders give, as the body will have it
            PhysicsSystem::MassInfo3D info;
            if (sceneProject->scene->getSystem<PhysicsSystem>()->computeMassProperties3D(entities[0], body, info)){
                propertyHeader("");
                ImGui::TextDisabled("Automatic: %.3f kg", info.mass);
            }
        }
        propertyRow(RowPropertyType::FloatPositive, cpType, "linearDamping", "Linear Damping", sceneProject, entities, settingsDamping);
        propertyRow(RowPropertyType::FloatPositive, cpType, "angularDamping", "Angular Damping", sceneProject, entities, settingsDamping);
        propertyRow(RowPropertyType::Float, cpType, "gravityFactor", "Gravity Factor", sceneProject, entities, settingsBodyValue);
        propertyRow(RowPropertyType::Enum, cpType, "interpolation", "Interpolation", sceneProject, entities, settingsInterpolation);
        propertyRow(RowPropertyType::Enum, cpType, "motionQuality", "Collision Detection", sceneProject, entities, settingsMotionQuality);
        propertyRow(RowPropertyType::Bool, cpType, "allowSleeping", "Allow Sleeping", sceneProject, entities, settingsBodyValue);
    }
    physicsMaterialRow(cpType, "material", "Default Material", sceneProject, entities,
                       "The material of the colliders that have none of their own (friction, bounciness, combine modes)");
    // floating in a library:water (Water.h): 0 ignores the water
    propertyRow(RowPropertyType::Float, cpType, "buoyancy", "Buoyancy", sceneProject, entities, settingsBodyValue);
    if (body.buoyancy > 0.0f) {
        propertyRow(RowPropertyType::Float, cpType, "waterDrag", "Water Drag", sceneProject, entities, settingsBodyValue);
        propertyRow(RowPropertyType::Float, cpType, "waterAngularDrag", "Water Angular Drag", sceneProject, entities, settingsBodyValue);
    }
    // the body-level sensor of before per-collider triggers, shown only when it is on
    if (body.sensor) {
        RowSettings settingsSensor;
        settingsSensor.help = "Old body-wide trigger: prefer Is Trigger on the colliders";
        propertyRow(RowPropertyType::Bool, cpType, "sensor", "Is Trigger (body)", sceneProject, entities, settingsSensor);
        propertyHeader("");
        if (ImGui::SmallButton("Convert to collider triggers##rigidbody")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    for (size_t i = 0; i < bodyComp->colliders.size(); i++){
                        multiCmd->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(i) + "].isTrigger", true);
                    }
                    multiCmd->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "sensor", false);
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
    }
    endTable();

    if (attachedToParent){
        ImGui::EndDisabled();
    }

    if (!attachedToParent && ImGui::TreeNode("Advanced##rigidbody3d")){
        beginTable(cpType, getLabelSize("Contacts With Static/Kinematic"), "rigidbody_advanced");
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxLinearVelocity", "Max Linear Velocity", sceneProject, entities, settingsBodyValue);
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxAngularVelocity", "Max Angular Velocity", sceneProject, entities, settingsBodyValue);
        propertyRowWithAutoButton(RowPropertyType::Vector3, cpType, "centerOfMass", "Center of Mass", "automaticCenterOfMass", "Automatic Center of Mass (the colliders')", sceneProject, entities, settingsCenter);
        propertyRowWithAutoButton(RowPropertyType::Vector3, cpType, "inertiaTensor", "Inertia Tensor", "automaticInertiaTensor", "Automatic Inertia Tensor (from the colliders)", sceneProject, entities, settingsInertia);
        if (!body.automaticInertiaTensor){
            propertyRow(RowPropertyType::Quat, cpType, "inertiaTensorRotation", "Inertia Tensor Rotation", sceneProject, entities, settingsBodyValue);
        }
        if (body.type == BodyType::KINEMATIC){
            propertyRow(RowPropertyType::Bool, cpType, "collideKinematicVsNonDynamic", "Contacts With Static/Kinematic", sceneProject, entities, settingsKinematicContacts);
        }
        propertyRow(RowPropertyType::Float, cpType, "friction", "Friction (no material)", sceneProject, entities, settingsSurface);
        propertyRow(RowPropertyType::Float, cpType, "restitution", "Restitution (no material)", sceneProject, entities, settingsSurface);
        propertyRow(RowPropertyType::Bool, cpType, "overrideMassProperties", "Override Mass (solid box)", sceneProject, entities, settingsBodyValue);
        if (body.overrideMassProperties) {
            propertyRow(RowPropertyType::Vector3, cpType, "solidBoxSize", "Solid Box Size", sceneProject, entities, settingsBodyValue);
            propertyRow(RowPropertyType::Float, cpType, "solidBoxDensity", "Solid Box Density", sceneProject, entities, settingsBodyValue);
        }
        endTable();
        ImGui::TreePop();
    }

    if (playing && !body.body.IsInvalid() && entities.size() == 1){
        // the live body (read only)
        separatorText("Runtime");
        Rigidbody live(sceneProject->scene, entities[0]);
        const Vector3 velocity = live.getLinearVelocity();
        const Vector3 angular = live.getAngularVelocity();
        ImGui::TextDisabled("Mass %.3f kg   %s", live.getMass(), live.isSleeping() ? "asleep" : "awake");
        ImGui::TextDisabled("Velocity (%.2f, %.2f, %.2f) m/s", velocity.x, velocity.y, velocity.z);
        ImGui::TextDisabled("Angular (%.2f, %.2f, %.2f) rad/s", angular.x, angular.y, angular.z);
    }

    separatorText("Colliders");
    ImGui::Text("Colliders: %zu", body.colliders.size());

    static int createCollider3DType = (int)Collider3DType::BOX;
    const char* createCollider3DLabel = "Unknown";
    for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider3DType))){
        if (entry.value == createCollider3DType){
            createCollider3DLabel = entry.name;
            break;
        }
    }

    ImGui::SetNextItemWidth(9 * ImGui::GetFontSize());
    if (ImGui::BeginCombo("##shape3d_create_type", createCollider3DLabel)){
        for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider3DType))){
            bool selected = (createCollider3DType == entry.value);
            if (ImGui::Selectable(entry.name, selected)){
                createCollider3DType = entry.value;
            }
            if (selected){
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Collider##rigidbody")){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

        for (Entity entity : entities){
            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){

                Collider3D shape;
                shape.type = (Collider3DType)createCollider3DType;
                if (shape.type == Collider3DType::MESH){
                    shape.source = Collider3DSource::ENTITY_MESH;
                    if (sceneProject->scene->findComponent<MeshComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }else if (shape.type == Collider3DType::CONVEX_HULL){
                    shape.source = Collider3DSource::ENTITY_MESH;
                    if (sceneProject->scene->findComponent<MeshComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }else if (shape.type == Collider3DType::HEIGHTFIELD){
                    shape.source = Collider3DSource::ENTITY_HEIGHTFIELD;
                    shape.samplesSize = 256;
                    if (sceneProject->scene->findComponent<TerrainComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }

                // the whole list: one undo step
                std::vector<Collider3D> colliders = bodyComp->colliders;
                colliders.push_back(shape);
                multiCmd->addPropertyCmd<std::vector<Collider3D>>(project, sceneProject->id, entity, cpType, "colliders", colliders);
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    size_t numColliders = body.colliders.size();
    for (Entity entity : entities){
        numColliders = std::min(numColliders, sceneProject->scene->getComponent<RigidbodyComponent>(entity).colliders.size());
    }

    RowSettings settingsShapeSourceConvexHull;
    settingsShapeSourceConvexHull.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::RAW_VERTICES) | reflect::enums::bit((int)Collider3DSource::ENTITY_MESH));

    RowSettings settingsShapeSourceMesh;
    settingsShapeSourceMesh.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::RAW_MESH) | reflect::enums::bit((int)Collider3DSource::ENTITY_MESH));

    RowSettings settingsShapeSourceHeightfield;
    settingsShapeSourceHeightfield.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::ENTITY_HEIGHTFIELD));

    RowSettings settingsShapeValue;

    auto normalizeCollider3DSource = [](Collider3DType type, Collider3DSource source) {
        if (type == Collider3DType::CONVEX_HULL){
            if (source != Collider3DSource::RAW_VERTICES && source != Collider3DSource::ENTITY_MESH){
                return Collider3DSource::RAW_VERTICES;
            }
        }else if (type == Collider3DType::MESH){
            if (source != Collider3DSource::RAW_MESH && source != Collider3DSource::ENTITY_MESH){
                return Collider3DSource::RAW_MESH;
            }
        }else if (type == Collider3DType::HEIGHTFIELD){
            if (source != Collider3DSource::ENTITY_HEIGHTFIELD){
                return Collider3DSource::ENTITY_HEIGHTFIELD;
            }
        }

        return source;
    };

    for (size_t s = 0; s < numColliders; s++){
        RigidbodyComponent& bodyRef = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]);
        Collider3D& shape = bodyRef.colliders[s];
        Transform* bodyTransform = sceneProject->scene->findComponent<Transform>(entities[0]);
        const Vector3 bodyScale = bodyTransform ? bodyTransform->scale : Vector3::UNIT_SCALE;
        const float physicsOffset = (shape.position * bodyScale).length();
        const bool suspiciousSingleShapeOffset = bodyRef.colliders.size() == 1 && physicsOffset > Stream::MAX_SINGLE_SHAPE_PHYSICS_OFFSET;

        separatorText(("Collider " + std::to_string(s + 1)).c_str());
        ImGui::PushID((int)s);
        if (ImGui::Button("Remove Collider")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->colliders.size()){
                        continue;
                    }

                    std::vector<Collider3D> colliders = bodyComp->colliders;
                    colliders.erase(colliders.begin() + (std::ptrdiff_t)s);
                    multiCmd->addPropertyCmd<std::vector<Collider3D>>(project, sceneProject->id, entity, cpType, "colliders", colliders);
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();

        std::string shapeKey = "colliders[" + std::to_string(s) + "]";

        beginTable(cpType, getLabelSize("Send Callbacks To"), "rigidbody_collider");

        PropertyData shapeTypeProp = Catalog::getProperty(sceneProject->scene, entities[0], cpType, shapeKey + ".type");
        int* shapeTypeDef = static_cast<int*>(shapeTypeProp.def);
        int shapeTypeValue = *static_cast<int*>(shapeTypeProp.ref);
        bool shapeTypeMixed = false;
        for (size_t entityIndex = 1; entityIndex < entities.size(); entityIndex++){
            PropertyData otherShapeTypeProp = Catalog::findProperty(sceneProject->scene, entities[entityIndex], cpType, shapeKey + ".type");
            // shapes are counted from the first body, the others may have fewer
            if (!otherShapeTypeProp.ref){
                continue;
            }
            if (*static_cast<int*>(otherShapeTypeProp.ref) != shapeTypeValue){
                shapeTypeMixed = true;
                break;
            }
        }

        int shapeTypeItemCurrent = 0;
        for (size_t i = 0; i < (*enumEntries(reflect::enums::kCollider3DType)).size(); ++i) {
            if ((*enumEntries(reflect::enums::kCollider3DType))[i].value == shapeTypeValue) {
                shapeTypeItemCurrent = static_cast<int>(i);
                break;
            }
        }

        int shapeTypeItemDefault = shapeTypeItemCurrent;
        bool shapeTypeDefChanged = false;
        if (shapeTypeDef){
            for (size_t i = 0; i < (*enumEntries(reflect::enums::kCollider3DType)).size(); ++i) {
                if ((*enumEntries(reflect::enums::kCollider3DType))[i].value == *shapeTypeDef) {
                    shapeTypeItemDefault = static_cast<int>(i);
                    break;
                }
            }
            shapeTypeDefChanged = (shapeTypeItemCurrent != shapeTypeItemDefault);
        }

        if (propertyHeader("Type", -1, shapeTypeDefChanged, false)){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            Collider3DType resetType = static_cast<Collider3DType>((*enumEntries(reflect::enums::kCollider3DType))[shapeTypeItemDefault].value);

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->colliders.size()) continue;

                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".type", (int)resetType);

                    Collider3DSource normalizedSource = normalizeCollider3DSource(resetType, bodyComp->colliders[s].source);
                    if (normalizedSource != bodyComp->colliders[s].source){
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".source", (int)normalizedSource);
                    }
                    if (bodyComp->colliders[s].sourceEntity == NULL_ENTITY && normalizedSource != Collider3DSource::NONE){
                        multiCmd->addPropertyCmd<Entity>(project, sceneProject->id, entity, cpType, shapeKey + ".sourceEntity", entity);
                    }
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }

        std::vector<const char*> shapeTypeNames;
        for (const auto& entry : (*enumEntries(reflect::enums::kCollider3DType))) {
            shapeTypeNames.push_back(entry.name);
        }

        if (shapeTypeMixed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Combo(("##combo_" + shapeKey + ".type").c_str(), &shapeTypeItemCurrent, shapeTypeNames.data(), static_cast<int>(shapeTypeNames.size()))) {
            Collider3DType newShapeType = static_cast<Collider3DType>((*enumEntries(reflect::enums::kCollider3DType))[shapeTypeItemCurrent].value);
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->colliders.size()) continue;

                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".type", (int)newShapeType);

                    Collider3DSource normalizedSource = normalizeCollider3DSource(newShapeType, bodyComp->colliders[s].source);
                    if (normalizedSource != bodyComp->colliders[s].source){
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".source", (int)normalizedSource);
                    }
                    if (bodyComp->colliders[s].sourceEntity == NULL_ENTITY && normalizedSource != Collider3DSource::NONE){
                        multiCmd->addPropertyCmd<Entity>(project, sceneProject->id, entity, cpType, shapeKey + ".sourceEntity", entity);
                    }
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
        if (shapeTypeMixed)
            ImGui::PopStyleColor();

        Collider3DType shapeTypeUI = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]).colliders[s].type;
        Collider3DSource shapeSourceUI = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]).colliders[s].source;
        Collider3DSource normalizedSourceUI = normalizeCollider3DSource(shapeTypeUI, shapeSourceUI);

        propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".position", "Position", sceneProject, entities, settingsShapeValue);

        if (suspiciousSingleShapeOffset){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Warning");
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 64, 255));
            ImGui::TextUnformatted("Large scaled collider offset can destabilize joints.");
            ImGui::PopStyleColor();
            ImGui::SameLine();

            std::string resetButtonId = "Reset##shape3d_pos_reset_" + std::to_string(s);
            if (ImGui::SmallButton(resetButtonId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

                for (Entity entity : entities){
                    if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                        if (s >= bodyComp->colliders.size()){
                            continue;
                        }

                        multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, shapeKey + ".position", Vector3::ZERO);
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }

        propertyRow(RowPropertyType::Quat, cpType, shapeKey + ".rotation", "Rotation", sceneProject, entities, settingsShapeValue);

        if (shape.type == Collider3DType::BOX){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".width", "Width", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".height", "Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".depth", "Depth", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::SPHERE){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::CAPSULE || shape.type == Collider3DType::CYLINDER){
            // a capsule of half height 0 is a sphere of its radius (S96 REQ-24)
            propertyRow(shape.type == Collider3DType::CAPSULE ? RowPropertyType::FloatPositive : RowPropertyType::Float,
                        cpType, shapeKey + ".halfHeight", "Half Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::TAPERED_CAPSULE){
            propertyRow(RowPropertyType::FloatPositive, cpType, shapeKey + ".halfHeight", "Half Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".topRadius", "Top Radius", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".bottomRadius", "Bottom Radius", sceneProject, entities, settingsShapeValue);
        }

        if (shapeTypeUI == Collider3DType::CONVEX_HULL){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceConvexHull);
            if (normalizedSourceUI == Collider3DSource::ENTITY_MESH){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }else if (normalizedSourceUI == Collider3DSource::RAW_VERTICES){
                propertyHeader("Vertices");
                ImGui::Text("%zu", shape.vertices.size());
                ImGui::SameLine();
                if (ImGui::Button(("Add Vertex##convex_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->colliders.size()) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            const size_t oldCount = shapeValue.vertices.size();
                            if (oldCount >= 2){
                                shapeValue.vertices.push_back(shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]));
                            }else if (oldCount == 1){
                                shapeValue.vertices.push_back(shapeValue.vertices[0] + Vector3(1.0f, 0.0f, 0.0f));
                            }else{
                                shapeValue.vertices.push_back(Vector3::ZERO);
                            }
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
                if (inputVerSize.x < 100.0f){
                    inputVerSize.x = 100.0f;
                }
                RowSettings settingsVertex = settingsShapeValue;
                settingsVertex.secondColSize = inputVerSize.x;

                for (int v = 0; v < (int)shape.vertices.size(); v++){
                    propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);
                    ImGui::SameLine();

                    std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_convex_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeVertexId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->colliders.size()) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (v >= (int)shapeValue.vertices.size()) continue;
                                shapeValue.vertices.erase(shapeValue.vertices.begin() + v);
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove vertex");
                    }
                }
            }
        }else if (shapeTypeUI == Collider3DType::MESH){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceMesh);
            // S96 WP-C: Jolt moves no mesh; its convex hulls (V-HACD, on a job) a body can move with
            if (entities.size() == 1 && !playing){
                propertyHeader("");
                if (std::shared_ptr<ColliderDecomposition> running = ColliderDecomposition::find(sceneProject->id, entities[0], s)){
                    const std::string stage = running->stage();
                    ImGui::ProgressBar((float)running->progress(), ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 3.0f, 0.0f),
                                       stage.empty() ? "Decomposing" : stage.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Cancel##decompose_" + std::to_string(s)).c_str())){
                        running->cancel();
                    }
                }else{
                    const std::string popupId = "decompose_collider_" + std::to_string(s);
                    if (ImGui::Button(("Decompose Into Convex Hulls##" + std::to_string(s)).c_str())){
                        ImGui::OpenPopup(popupId.c_str());
                    }
                    if (ImGui::IsItemHovered()){
                        ImGui::SetTooltip("Replaces this Mesh collider with convex hull colliders (one undo step), so the body can be dynamic or kinematic");
                    }
                    static int decomposeQuality = 1;
                    static int decomposeMaxHulls = 16;
                    if (ImGui::BeginPopup(popupId.c_str())){
                        const char* qualities[] = {"Low", "Medium", "High"};
                        ImGui::Combo("Quality", &decomposeQuality, qualities, 3);
                        ImGui::SliderInt("Max Hulls", &decomposeMaxHulls, 1, (int)ColliderDecomposition::MAX_HULLS);
                        if (ImGui::Button("Decompose")){
                            ColliderDecomposition::Request request;
                            request.sceneId = sceneProject->id;
                            request.entity = entities[0];
                            request.index = s;
                            request.quality = (DecompositionQuality)std::clamp(decomposeQuality, 0, 2);
                            request.maxHulls = (uint32_t)std::clamp(decomposeMaxHulls, 1, (int)ColliderDecomposition::MAX_HULLS);
                            std::string error;
                            const bool started = (bool)ColliderDecomposition::start(project, request, [](const ColliderDecomposition::Outcome& outcome){
                                if (!outcome.error.empty() && !outcome.cancelled){
                                    Log::error("Decompose Into Convex Hulls: %s", outcome.error.c_str());
                                }
                            }, error);
                            if (!started){
                                Log::error("Decompose Into Convex Hulls: %s", error.c_str());
                            }
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }
            }
            if (normalizedSourceUI == Collider3DSource::ENTITY_MESH){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }else if (normalizedSourceUI == Collider3DSource::RAW_MESH){
                // Vertices
                propertyHeader("Vertices");
                ImGui::Text("%zu", shape.vertices.size());
                ImGui::SameLine();
                if (ImGui::Button(("Add Vertex##mesh_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->colliders.size()) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            const size_t oldCount = shapeValue.vertices.size();
                            if (oldCount >= 2){
                                shapeValue.vertices.push_back(shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]));
                            }else if (oldCount == 1){
                                shapeValue.vertices.push_back(shapeValue.vertices[0] + Vector3(1.0f, 0.0f, 0.0f));
                            }else{
                                shapeValue.vertices.push_back(Vector3::ZERO);
                            }
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
                if (inputVerSize.x < 100.0f){
                    inputVerSize.x = 100.0f;
                }
                RowSettings settingsVertex = settingsShapeValue;
                settingsVertex.secondColSize = inputVerSize.x;

                for (int v = 0; v < (int)shape.vertices.size(); v++){
                    propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);
                    ImGui::SameLine();

                    std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_mesh_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeVertexId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->colliders.size()) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (v >= (int)shapeValue.vertices.size()) continue;
                                shapeValue.vertices.erase(shapeValue.vertices.begin() + v);
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove vertex");
                    }
                }

                // Triangles (indices)
                const int triangleCount = (int)(shape.indices.size() / 3);
                propertyHeader("Triangles");
                ImGui::Text("%d", triangleCount);
                ImGui::SameLine();
                if (ImGui::Button(("Add Triangle##mesh_tri_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->colliders.size()) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            shapeValue.indices.resize((shapeValue.indices.size() / 3) * 3);
                            shapeValue.indices.push_back(0);
                            shapeValue.indices.push_back(shapeValue.vertices.size() > 1 ? 1u : 0u);
                            shapeValue.indices.push_back(shapeValue.vertices.size() > 2 ? 2u : 0u);
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                for (int t = 0; t < triangleCount; t++){
                    int baseIdx = t * 3;
                    std::string triLabel = "Triangle " + std::to_string(t + 1);
                    propertyHeader(triLabel);

                    int triIndices[3] = { (int)shape.indices[baseIdx], (int)shape.indices[baseIdx + 1], (int)shape.indices[baseIdx + 2] };
                    std::string triId = "##mesh_tri_" + std::to_string(s) + "_" + std::to_string(t);

                    ImGui::SetNextItemWidth(inputVerSize.x);
                    if (ImGui::InputInt3(triId.c_str(), triIndices)){
                        for (int k = 0; k < 3; k++){
                            triIndices[k] = std::max(0, std::min(triIndices[k], (int)shape.vertices.size() - 1));
                        }
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->colliders.size()) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (baseIdx + 2 >= (int)shapeValue.indices.size()) continue;
                                shapeValue.indices[baseIdx] = (uint32_t)triIndices[0];
                                shapeValue.indices[baseIdx + 1] = (uint32_t)triIndices[1];
                                shapeValue.indices[baseIdx + 2] = (uint32_t)triIndices[2];
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::SameLine();
                    std::string removeTriId = std::string(ICON_FA_TRASH_CAN) + "##remove_mesh_tri_" + std::to_string(s) + "_" + std::to_string(t);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeTriId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->colliders.size()) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (baseIdx + 2 >= (int)shapeValue.indices.size()) continue;
                                shapeValue.indices.erase(shapeValue.indices.begin() + baseIdx, shapeValue.indices.begin() + baseIdx + 3);
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove triangle");
                    }
                }
            }
        }else if (shapeTypeUI == Collider3DType::HEIGHTFIELD){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceHeightfield);
            if (normalizedSourceUI == Collider3DSource::ENTITY_HEIGHTFIELD){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }
            propertyRow(RowPropertyType::UInt, cpType, shapeKey + ".samplesSize", "Samples Size", sceneProject, entities, settingsShapeValue);
        }

        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".density", "Density", sceneProject, entities, settingsShapeValue);
        RowSettings settingsTrigger;
        settingsTrigger.help = "Overlaps are reported (trigger contacts), nothing is pushed; a trigger has no mass unless every collider is one";
        propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".isTrigger", "Is Trigger", sceneProject, entities, settingsTrigger);
        physicsMaterialRow(cpType, shapeKey + ".material", "Material", sceneProject, entities,
                           "Friction, bounciness and how they combine with the other collider's; over the body's material");
        RowSettings settingsReceiver;
        settingsReceiver.help = "Another GameObject that also gets this collider's contact callbacks";
        propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".receiver", "Send Callbacks To", sceneProject, entities, settingsReceiver);
        endTable();
    }

    // Play: what the body touches, and the contact callbacks its scripts define
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        drawPhysicsContacts(sceneProject, entities[0], false);
    }
}


void editor::InspectorWindow::drawJoint3DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Joint3DComponent& joint = sceneProject->scene->getComponent<Joint3DComponent>(entities[0]);

    auto markJoint3DDirty = [sceneProject, entities](){
        for (Entity entity : entities){
            if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                jointComp->needUpdateJoint = true;
            }
        }
    };

    RowSettings settingsJointType;
    settingsJointType.enumEntries = enumEntries(reflect::enums::kJoint3DType);
    settingsJointType.onValueChanged = markJoint3DDirty;

    RowSettings settingsJointValue;
    settingsJointValue.onValueChanged = markJoint3DDirty;
    settingsJointValue.entityFilter.set(sceneProject->scene->getComponentId<RigidbodyComponent>());

    // the parameters reach the live joint (no rebuild)
    RowSettings settingsParam;
    RowSettings settingsDistance;
    settingsDistance.help = "-1: the distance when the joint is made";
    RowSettings settingsSwingType;
    settingsSwingType.enumEntries = &entriesJointSwingType3D;
    settingsSwingType.onValueChanged = markJoint3DDirty;
    RowSettings settingsMotion;
    settingsMotion.enumEntries = &entriesJointMotion3D;
    // motions and limits reach the live joint (no rebuild: its zero stays)
    RowSettings settingsAnchorSpace;
    settingsAnchorSpace.enumEntries = &entriesJointAnchorSpace3D;
    settingsAnchorSpace.onValueChanged = markJoint3DDirty;
    settingsAnchorSpace.help = "Local: anchors and axes in the bodies' own frames, so the joint works wherever its prefab is placed (Unity). World: world points taken when the joint is made (scenes saved before)";
    RowSettings settingsAnchor;
    settingsAnchor.onValueChanged = markJoint3DDirty;
    RowSettings settingsCollision;
    settingsCollision.help = "The two connected bodies collide with each other (off: they pass through each other, Unity's default)";

    const bool local = joint.anchorSpace == JointAnchorSpace::LOCAL;
    const bool usesAnchor = joint.type != Joint3DType::GEAR && joint.type != Joint3DType::RACKANDPINON && joint.type != Joint3DType::PATH;

    beginTable(cpType, getLabelSize("Auto Configure Connected"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Joint Type", sceneProject, entities, settingsJointType);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyA", "Rigidbody A", sceneProject, entities, settingsJointValue);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyB", local ? "Connected Body" : "Rigidbody B", sceneProject, entities, settingsJointValue);
    propertyRow(RowPropertyType::Enum, cpType, "anchorSpace", "Anchor Space", sceneProject, entities, settingsAnchorSpace);
    if (!local && entities.size() == 1){
        propertyHeader("");
        if (ImGui::SmallButton("Convert to Local##joint3d")){
            // the world points and axes as the bodies' local ones, where the bodies stand now;
            // the joint behaves as before as long as the bodies start where they are
            Scene* scene = sceneProject->scene;
            Joint3DComponent converted = joint;
            auto frame = [scene](Entity e, Vector3& position, Quaternion& rotation, Vector3& scale){
                position = Vector3::ZERO; rotation = Quaternion::IDENTITY; scale = Vector3::UNIT_SCALE;
                if (Transform* t = (e != NULL_ENTITY) ? scene->findComponent<Transform>(e) : nullptr){
                    position = t->worldPosition; rotation = t->worldRotation; scale = t->worldScale;
                }
            };
            auto toLocal = [](const Vector3& p, const Vector3& position, const Quaternion& rotation, const Vector3& scale){
                Vector3 l = rotation.inverse() * (p - position);
                return Vector3(scale.x != 0 ? l.x / scale.x : l.x, scale.y != 0 ? l.y / scale.y : l.y, scale.z != 0 ? l.z / scale.z : l.z);
            };
            Vector3 posA, scaleA, posB, scaleB;
            Quaternion rotA, rotB;
            frame(joint.bodyA, posA, rotA, scaleA);
            frame(joint.bodyB, posB, rotB, scaleB);
            Vector3 worldA = joint.anchor, worldB = joint.anchor;
            if (joint.type == Joint3DType::DISTANCE){
                worldA = joint.autoAnchors ? posA : joint.anchorA;
                worldB = joint.autoAnchors ? posB : joint.anchorB;
            }else if (joint.type == Joint3DType::SIXDOF || joint.type == Joint3DType::PULLEY){
                worldA = joint.anchorA;
                worldB = joint.anchorB;
            }else if (joint.type == Joint3DType::PRISMATIC || joint.type == Joint3DType::FIXED){
                worldA = worldB = (posA + posB) * 0.5f;
            }
            converted.anchorSpace = JointAnchorSpace::LOCAL;
            converted.anchor = toLocal(worldA, posA, rotA, scaleA);
            converted.anchorB = joint.bodyB != NULL_ENTITY ? toLocal(worldB, posB, rotB, scaleB) : worldB;
            converted.autoAnchors = false;
            const Quaternion inverse = rotA.inverse();
            converted.axis = inverse * joint.axis;
            converted.normal = inverse * joint.normal;
            converted.twistAxis = inverse * joint.twistAxis;
            converted.planeAxis = inverse * joint.planeAxis;
            converted.axisX = inverse * joint.axisX;
            converted.axisY = inverse * joint.axisY;
            if (joint.type == Joint3DType::PRISMATIC && joint.useLimits){
                // a world slider starts at 0 inside its (shifted) range: the same range from the start
                float limitsMin = std::min(joint.limitsMin, joint.limitsMax);
                float limitsMax = std::max(joint.limitsMin, joint.limitsMax);
                if (limitsMin > 0.0f){ limitsMax -= limitsMin; limitsMin = 0.0f; }
                else if (limitsMax < 0.0f){ limitsMin -= limitsMax; limitsMax = 0.0f; }
                converted.limitsMin = limitsMin;
                converted.limitsMax = limitsMax;
            }
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const char* name : {"anchorSpace", "anchor", "anchorB", "autoAnchors", "axis", "normal", "twistAxis", "planeAxis", "axisX", "axisY", "limitsMin", "limitsMax"}){
                const std::string property = name;
                PropertyData data = Catalog::getProperty(scene, entities[0], cpType, property);
                if (!data.ref) continue;
                if (property == "anchorSpace") multiCmd->addPropertyCmd<int>(project, sceneProject->id, entities[0], cpType, property, (int)converted.anchorSpace);
                else if (property == "autoAnchors") multiCmd->addPropertyCmd<bool>(project, sceneProject->id, entities[0], cpType, property, converted.autoAnchors);
                else if (property == "limitsMin") multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType, property, converted.limitsMin);
                else if (property == "limitsMax") multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType, property, converted.limitsMax);
                else {
                    const Joint3DComponent& c = converted;
                    const Vector3 value = property == "anchor" ? c.anchor : property == "anchorB" ? c.anchorB : property == "axis" ? c.axis :
                                          property == "normal" ? c.normal : property == "twistAxis" ? c.twistAxis : property == "planeAxis" ? c.planeAxis :
                                          property == "axisX" ? c.axisX : c.axisY;
                    multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, property, value);
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("Anchors and axes in the bodies' frames (Unity), measured where the bodies are now");
        }
    }
    if (local && usesAnchor){
        RowSettings settingsLocalAnchor = settingsAnchor;
        settingsLocalAnchor.help = "Where the joint holds Rigidbody A, in its local space (collider units)";
        propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsLocalAnchor);
        if (joint.type != Joint3DType::DISTANCE){
            RowSettings settingsAuto = settingsAnchor;
            settingsAuto.help = "The connected anchor is where Anchor is when the joint is made (Unity Auto Configure Connected Anchor)";
            propertyRow(RowPropertyType::Bool, cpType, "autoAnchors", "Auto Configure Connected", sceneProject, entities, settingsAuto);
        }
        if (!joint.autoAnchors || joint.type == Joint3DType::DISTANCE){
            RowSettings settingsConnected = settingsAnchor;
            settingsConnected.help = joint.bodyB != NULL_ENTITY ? "Where the joint holds the connected body, in its local space"
                                                                 : "No connected body: a world point the joint holds Rigidbody A to";
            propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Connected Anchor", sceneProject, entities, settingsConnected);
        }
    }
    propertyRow(RowPropertyType::Bool, cpType, "enableCollision", "Enable Collision", sceneProject, entities, settingsCollision);

    endTable();

    bool hasTypeSettings = joint.type != Joint3DType::FIXED;
    if (hasTypeSettings){

    const char* joint3DTableLabel = "Anchor";
    switch (joint.type){
        case Joint3DType::DISTANCE:
            joint3DTableLabel = "Auto Anchors";
            break;
        case Joint3DType::POINT:
            joint3DTableLabel = "Anchor";
            break;
        case Joint3DType::HINGE:
            joint3DTableLabel = "Normal";
            break;
        case Joint3DType::CONE:
            joint3DTableLabel = "Twist Axis";
            break;
        case Joint3DType::PRISMATIC:
            joint3DTableLabel = "Limits Min";
            break;
        case Joint3DType::SWINGTWIST:
            joint3DTableLabel = "Normal HalfCone";
            break;
        case Joint3DType::SIXDOF:
            joint3DTableLabel = "Anchor A";
            break;
        case Joint3DType::GEAR:
            joint3DTableLabel = "Teeth Gear A";
            break;
        case Joint3DType::RACKANDPINON:
            joint3DTableLabel = "Rack Length";
            break;
        case Joint3DType::PULLEY:
            joint3DTableLabel = "Fixed Point A";
            break;
        case Joint3DType::PATH:
            joint3DTableLabel = "Position";
            break;
        default:
            break;
    }

    separatorText("Joint settings");
    std::string joint3DTypeTableId = "joint3d_type_settings_" + std::to_string((int)joint.type);
    beginTable(cpType, getLabelSize(joint3DTableLabel), joint3DTypeTableId);

    if (joint.type == Joint3DType::DISTANCE){
        if (!local){
            propertyRow(RowPropertyType::Bool, cpType, "autoAnchors", "Auto Anchors", sceneProject, entities, settingsJointValue);
            if (!joint.autoAnchors){
                propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
                propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
            }
        }
        propertyRow(RowPropertyType::Float, cpType, "minDistance", "Min Distance", sceneProject, entities, settingsDistance);
        propertyRow(RowPropertyType::Float, cpType, "maxDistance", "Max Distance", sceneProject, entities, settingsDistance);
    }else if (joint.type == Joint3DType::POINT){
        if (!local) propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::HINGE){
        if (!local) propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "axis", "Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "normal", "Secondary Axis", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::CONE){
        if (!local) propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "twistAxis", "Twist Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "coneHalfAngle", "Cone Half Angle", sceneProject, entities, settingsParam);
    }else if (joint.type == Joint3DType::PRISMATIC){
        propertyRow(RowPropertyType::Vector3, cpType, "axis", "Axis", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::SWINGTWIST){
        if (!local) propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "twistAxis", "Twist Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "planeAxis", "Swing Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Enum, cpType, "swingType", "Swing Type", sceneProject, entities, settingsSwingType);
        // Unity CharacterJoint's names: twist about the twist axis, swing 1 about the swing axis
        // (Jolt's normal half cone), swing 2 about their normal (Jolt's plane half cone)
        propertyRow(RowPropertyType::Float, cpType, "twistMinAngle", "Low Twist", sceneProject, entities, settingsParam);
        propertyRow(RowPropertyType::Float, cpType, "twistMaxAngle", "High Twist", sceneProject, entities, settingsParam);
        propertyRow(RowPropertyType::Float, cpType, "normalHalfConeAngle", "Swing 1", sceneProject, entities, settingsParam);
        propertyRow(RowPropertyType::Float, cpType, "planeHalfConeAngle", "Swing 2", sceneProject, entities, settingsParam);
    }else if (joint.type == Joint3DType::SIXDOF){
        if (!local){
            propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
            propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        }
        propertyRow(RowPropertyType::Vector3, cpType, "axisX", "Axis X", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "axisY", "Axis Y", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Enum, cpType, "xMotion", "X Motion", sceneProject, entities, settingsMotion);
        propertyRow(RowPropertyType::Enum, cpType, "yMotion", "Y Motion", sceneProject, entities, settingsMotion);
        propertyRow(RowPropertyType::Enum, cpType, "zMotion", "Z Motion", sceneProject, entities, settingsMotion);
        propertyRow(RowPropertyType::Enum, cpType, "angularXMotion", "Angular X Motion", sceneProject, entities, settingsMotion);
        propertyRow(RowPropertyType::Enum, cpType, "angularYMotion", "Angular Y Motion", sceneProject, entities, settingsMotion);
        propertyRow(RowPropertyType::Enum, cpType, "angularZMotion", "Angular Z Motion", sceneProject, entities, settingsMotion);
        const bool linearLimited = joint.xMotion == JointMotion::LIMITED || joint.yMotion == JointMotion::LIMITED || joint.zMotion == JointMotion::LIMITED;
        const bool angularLimited = joint.angularXMotion == JointMotion::LIMITED || joint.angularYMotion == JointMotion::LIMITED || joint.angularZMotion == JointMotion::LIMITED;
        if (linearLimited){
            propertyRow(RowPropertyType::Vector3, cpType, "linearLimitMin", "Linear Limit Min", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Vector3, cpType, "linearLimitMax", "Linear Limit Max", sceneProject, entities, settingsParam);
        }
        if (angularLimited){
            propertyRow(RowPropertyType::Vector3, cpType, "angularLimitMin", "Angular Limit Min", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Vector3, cpType, "angularLimitMax", "Angular Limit Max", sceneProject, entities, settingsParam);
        }
    }else if (joint.type == Joint3DType::GEAR){
        propertyRow(RowPropertyType::LocalEntity, cpType, "hingeA", "Hinge A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::LocalEntity, cpType, "hingeB", "Hinge B", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGearA", "Teeth Gear A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGearB", "Teeth Gear B", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::RACKANDPINON){
        propertyRow(RowPropertyType::LocalEntity, cpType, "hinge", "Hinge", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::LocalEntity, cpType, "slider", "Slider", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethRack", "Teeth Rack", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGear", "Teeth Gear", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "rackLength", "Rack Length", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::PULLEY){
        if (!local){
            propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
            propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        }
        propertyRow(RowPropertyType::Vector3, cpType, "fixedPointA", "Fixed Point A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "fixedPointB", "Fixed Point B", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::PATH){
        propertyRow(RowPropertyType::Vector3, cpType, "pathPosition", "Position", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Bool, cpType, "isLooping", "Looping", sceneProject, entities, settingsJointValue);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Points");
        ImGui::TableSetColumnIndex(1);

        ImGui::Text("%zu", joint.pathPoints.size());
        if (drawSummaryAddButton("Add Point##joint3d_path_add")) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                    std::vector<Vector3> newPoints = jointComp->pathPoints;
                    Vector3 newPoint = Vector3::ZERO;
                    if (!newPoints.empty()){
                        Vector3 lastPoint = newPoints.back();
                        newPoint = Vector3(lastPoint.x, lastPoint.y, lastPoint.z + 1.0f);
                    }
                    newPoints.push_back(newPoint);
                    multiCmd->addPropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "pathPoints", newPoints, settingsJointValue.onValueChanged);
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }

        bool removedPoint = false;
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 inputSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
        if (inputSize.x < 100.0f){
            inputSize.x = 100.0f;
        }

        for (size_t pointIndex = 0; pointIndex < joint.pathPoints.size(); pointIndex++){
            std::string pointId = "pathPoints[" + std::to_string(pointIndex) + "]";
            std::string pointLabel = "Point " + std::to_string(pointIndex);

            RowSettings settingsPathPoint = settingsJointValue;
            settingsPathPoint.secondColSize = inputSize.x;
            propertyRow(RowPropertyType::Vector3, cpType, pointId, pointLabel, sceneProject, entities, settingsPathPoint);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            std::string removePointId = std::string(ICON_FA_TRASH_CAN) + "##remove_path_point_" + std::to_string(pointIndex);
            if (ImGui::Button(removePointId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                        if (pointIndex < jointComp->pathPoints.size()){
                            std::vector<Vector3> newPoints = jointComp->pathPoints;
                            newPoints.erase(newPoints.begin() + (long int)pointIndex);
                            multiCmd->addPropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "pathPoints", newPoints, settingsJointValue.onValueChanged);
                        }
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                removedPoint = true;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                break;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove point");
            }
        }

        if (removedPoint){
            endTable();
            return;
        }
    }

    endTable();
    }

    const bool hinge = joint.type == Joint3DType::HINGE;
    const bool slider = joint.type == Joint3DType::PRISMATIC;
    const bool distance = joint.type == Joint3DType::DISTANCE;
    const bool swingTwist = joint.type == Joint3DType::SWINGTWIST;
    const bool sixDOF = joint.type == Joint3DType::SIXDOF;

    if (hinge || slider){
        separatorText("Limits");
        beginTable(cpType, getLabelSize("Spring Frequency"), "joint3d_limits");
        propertyRow(RowPropertyType::Bool, cpType, "useLimits", "Use Limits", sceneProject, entities, settingsParam);
        if (joint.useLimits){
            RowSettings settingsLimit;
            settingsLimit.help = hinge ? "Angles measured from the pose the joint starts in (any range, Unity's 10..50 too)"
                                       : "Metres along the axis, measured from where the joint starts";
            propertyRow(RowPropertyType::Float, cpType, "limitsMin", hinge ? "Min" : "Min (m)", sceneProject, entities, settingsLimit);
            propertyRow(RowPropertyType::Float, cpType, "limitsMax", hinge ? "Max" : "Max (m)", sceneProject, entities, settingsLimit);
            RowSettings settingsSoft;
            settingsSoft.help = "Soft limits: a spring of this frequency (Hz) pulls back past them; 0 = hard";
            propertyRow(RowPropertyType::FloatPositive, cpType, "limitsSpringFrequency", "Spring Frequency", sceneProject, entities, settingsSoft);
            propertyRow(RowPropertyType::FloatPositive, cpType, "limitsSpringDamping", "Spring Damping", sceneProject, entities, settingsParam);
            if (!local && ((joint.limitsMin > 0.0f && joint.limitsMax > 0.0f) || (joint.limitsMin < 0.0f && joint.limitsMax < 0.0f))){
                propertyHeader("");
                ImGui::TextColored(Theme::Colors::WarningText, "World joints start at 0: convert to Local for a range not containing 0");
            }
        }
        endTable();
    }

    if (hinge || slider || distance){
        separatorText("Spring");
        beginTable(cpType, getLabelSize("Target Position"), "joint3d_spring");
        RowSettings settingsUseSpring;
        settingsUseSpring.help = distance ? "A soft rope: a spring (Unity SpringJoint) between Min and Max Distance"
                                          : "A spring to the target (Unity HingeJoint.spring)";
        propertyRow(RowPropertyType::Bool, cpType, "useSpring", "Use Spring", sceneProject, entities, settingsUseSpring);
        if (joint.useSpring){
            propertyRow(RowPropertyType::FloatPositive, cpType, "springStiffness", "Spring", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::FloatPositive, cpType, "springDamper", "Damper", sceneProject, entities, settingsParam);
            if (!distance){
                propertyRow(RowPropertyType::Float, cpType, "springTargetPosition", "Target Position", sceneProject, entities, settingsParam);
            }
        }
        endTable();
    }

    if (hinge || slider || swingTwist){
        separatorText("Motor");
        beginTable(cpType, getLabelSize("Target Velocity"), "joint3d_motor");
        propertyRow(RowPropertyType::Bool, cpType, "useMotor", "Use Motor", sceneProject, entities, settingsParam);
        if (joint.useMotor){
            if (!swingTwist){
                RowSettings settingsVelocity;
                settingsVelocity.help = hinge ? "Angular speed per second" : "Metres per second";
                propertyRow(RowPropertyType::Float, cpType, "motorTargetVelocity", "Target Velocity", sceneProject, entities, settingsVelocity);
            }else{
                propertyRow(RowPropertyType::Quat, cpType, "motorTargetRotation", "Target Rotation", sceneProject, entities, settingsParam);
                propertyRow(RowPropertyType::FloatPositive, cpType, "motorSpringFrequency", "Frequency", sceneProject, entities, settingsParam);
                propertyRow(RowPropertyType::FloatPositive, cpType, "motorSpringDamping", "Damping", sceneProject, entities, settingsParam);
            }
            RowSettings settingsForce;
            settingsForce.help = slider ? "Newtons" : "Newton metres";
            propertyRow(RowPropertyType::FloatPositive, cpType, "motorMaxForce", "Max Force", sceneProject, entities, settingsForce);
            if (joint.useSpring && !swingTwist){
                propertyHeader("");
                ImGui::TextColored(Theme::Colors::WarningText, "Motor and spring are both on: the motor drives it");
            }
        }
        endTable();
    }

    if (hinge || slider || swingTwist || sixDOF){
        beginTable(cpType, getLabelSize("Max Friction"), "joint3d_friction");
        RowSettings settingsFriction;
        settingsFriction.help = slider ? "Friction force (N) when no motor drives it" : "Friction torque (N m) when no motor drives it";
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxFriction", "Max Friction", sceneProject, entities, settingsFriction);
        endTable();
    }

    if (sixDOF){
        separatorText("Drives");
        beginTable(cpType, getLabelSize("Angular Drive Stiffness"), "joint3d_drives");
        propertyRow(RowPropertyType::Bool, cpType, "useDrive", "Use Drive", sceneProject, entities, settingsParam);
        if (joint.useDrive){
            RowSettings settingsDrive;
            settingsDrive.help = "A stiffness drives to the target pose, a damping alone to the target velocity (Unity's drives)";
            propertyRow(RowPropertyType::FloatPositive, cpType, "driveStiffness", "Drive Stiffness", sceneProject, entities, settingsDrive);
            propertyRow(RowPropertyType::FloatPositive, cpType, "driveDamping", "Drive Damping", sceneProject, entities, settingsDrive);
            propertyRow(RowPropertyType::FloatPositive, cpType, "angularDriveStiffness", "Angular Drive Stiffness", sceneProject, entities, settingsDrive);
            propertyRow(RowPropertyType::FloatPositive, cpType, "angularDriveDamping", "Angular Drive Damping", sceneProject, entities, settingsDrive);
            propertyRow(RowPropertyType::Float, cpType, "driveMaxForce", "Max Force", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Float, cpType, "driveMaxTorque", "Max Torque", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Vector3, cpType, "targetPosition", "Target Position", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Quat, cpType, "targetRotation", "Target Rotation", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Vector3, cpType, "targetVelocity", "Target Velocity", sceneProject, entities, settingsParam);
            propertyRow(RowPropertyType::Vector3, cpType, "targetAngularVelocity", "Target Angular Velocity", sceneProject, entities, settingsParam);
        }
        endTable();
    }

    separatorText("Break");
    beginTable(cpType, getLabelSize("Break Torque"), "joint3d_break");
    RowSettings settingsBreak;
    settingsBreak.help = "Breaks past this force (N) / torque (N m); empty or infinite: never";
    propertyRow(RowPropertyType::Float, cpType, "breakForce", "Break Force", sceneProject, entities, settingsBreak);
    propertyRow(RowPropertyType::Float, cpType, "breakTorque", "Break Torque", sceneProject, entities, settingsBreak);
    if (std::isfinite(joint.breakForce) || std::isfinite(joint.breakTorque)){
        RowSettings settingsAction;
        settingsAction.enumEntries = &entriesJointBreakAction3D;
        settingsAction.help = "Ignore: nothing. Callback Only: onJointBreak(force, joint). Disable: the joint is disabled, then the callback.\nDestroy: the callback, then the component is removed (Unity)";
        propertyRow(RowPropertyType::Enum, cpType, "breakAction", "Action", sceneProject, entities, settingsAction);
        propertyHeader("");
        if (ImGui::SmallButton("Never##joint3d_break")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "breakForce", std::numeric_limits<float>::infinity());
                multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "breakTorque", std::numeric_limits<float>::infinity());
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
    }
    endTable();

    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;
    if (playing && entities.size() == 1){
        separatorText("Runtime");
        Joint3D live(sceneProject->scene, entities[0]);
        if (!live.isLive()){
            ImGui::TextDisabled(joint.broken ? "Broken" : "Not made (see the console)");
        }else{
            const Vector3 force = live.getCurrentForce();
            const Vector3 torque = live.getCurrentTorque();
            ImGui::TextDisabled("Force %.1f N   Torque %.1f N m", force.length(), torque.length());
            if (hinge){
                ImGui::TextDisabled("Angle %.2f   Velocity %.2f /s", live.getAngle(), live.getVelocity());
            }else if (slider){
                ImGui::TextDisabled("Position %.3f m   Speed %.3f m/s", live.getPosition(), live.getSpeed());
            }else if (distance){
                ImGui::TextDisabled("Length %.3f m", live.getCurrentLength());
            }
        }
    }
}

// S22's gameplay components: the character controllers, the water volume and the buoyancy

void editor::InspectorWindow::drawCharacterController2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSlope = settings;
    settingsSlope.help = "Surfaces steeper than this (degrees) are not walked on: they act as walls";
    RowSettings settingsStep = settings;
    settingsStep.help = "Steps up to this high (points) are climbed while walking on the ground; 0 climbs none";
    RowSettings settingsSkin = settings;
    settingsSkin.help = "How far (points) the capsule stays off what it touches";
    RowSettings settingsPush = settings;
    settingsPush.help = "Dynamic bodies it walks into get an impulse of this (N) x dt along the move";
    RowSettings settingsPlatforms;
    settingsPlatforms.help = "Carried by the kinematic or dynamic body it stands on";

    beginTable(cpType, getLabelSize("Min Move Distance"));
    propertyRow(RowPropertyType::Vector2, cpType, "center", "Center", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "slopeLimit", "Slope Limit", sceneProject, entities, settingsSlope);
    propertyRow(RowPropertyType::FloatPositive, cpType, "stepOffset", "Step Offset", sceneProject, entities, settingsStep);
    propertyRow(RowPropertyType::FloatPositive, cpType, "skinWidth", "Skin Width", sceneProject, entities, settingsSkin);
    propertyRow(RowPropertyType::FloatPositive, cpType, "minMoveDistance", "Min Move Distance", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pushStrength", "Push Strength", sceneProject, entities, settingsPush);
    propertyRow(RowPropertyType::Bool, cpType, "pushBodies", "Push Bodies", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "movingPlatforms", "Moving Platforms", sceneProject, entities, settingsPlatforms);
    endTable();

    for (Entity entity : entities){
        if (scene->findComponent<Rigidbody2DComponent>(entity)){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("A Character Controller 2D does nothing on a GameObject with a Rigidbody 2D: remove one of them.");
            break;
        }
    }

    // what the last move and physics step left, while the scene plays
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        const CharacterController2DComponent& controller = scene->getComponent<CharacterController2DComponent>(entities[0]);
        std::string flags;
        if (controller.collisionFlags & CharacterCollision_Sides) flags += " Sides";
        if (controller.collisionFlags & CharacterCollision_Above) flags += " Above";
        if (controller.collisionFlags & CharacterCollision_Below) flags += " Below";
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Grounded: %s   On Steep Slope: %s", controller.isGrounded ? "yes" : "no", controller.isOnSteepSlope ? "yes" : "no");
        ImGui::TextDisabled("Collision Flags:%s", flags.empty() ? " None" : flags.c_str());
        ImGui::TextDisabled("Velocity: %.1f, %.1f", controller.velocity.x, controller.velocity.y);
        ImGui::TextDisabled("Ground Velocity: %.1f, %.1f", controller.groundVelocity.x, controller.groundVelocity.y);
    }
}

void editor::InspectorWindow::drawCharacterControllerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSlope = settings;
    settingsSlope.help = "Surfaces steeper than this (degrees) are not walked on: they act as walls";
    RowSettings settingsStep = settings;
    settingsStep.help = "Steps up to this high are climbed while walking on the ground; 0 climbs none";
    RowSettings settingsSkin = settings;
    settingsSkin.help = "How far the capsule stays off what it touches (Jolt's character padding)";
    RowSettings settingsSize = settings;
    settingsSize.help = "While playing a new size is tried at once and refused when it does not fit there (a crouch under a ceiling)";
    RowSettings settingsPush = settings;
    settingsPush.help = "The most force (N) it pushes dynamic bodies with";
    RowSettings settingsFlag;
    RowSettings settingsDetect;
    settingsDetect.help = "A kinematic inner body: rays, triggers and dynamic bodies see it";
    RowSettings settingsCharacters;
    settingsCharacters.help = "Other controllers block it";
    RowSettings settingsEdges;
    settingsEdges.help = "Smoother over the inner edges of meshes, at a cost";

    beginTable(cpType, getLabelSize("Enhanced Edge Removal"));
    propertyRow(RowPropertyType::Vector3, cpType, "center", "Center", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "slopeLimit", "Slope Limit", sceneProject, entities, settingsSlope);
    propertyRow(RowPropertyType::FloatPositive, cpType, "stepOffset", "Step Offset", sceneProject, entities, settingsStep);
    propertyRow(RowPropertyType::FloatPositive, cpType, "skinWidth", "Skin Width", sceneProject, entities, settingsSkin);
    propertyRow(RowPropertyType::FloatPositive, cpType, "minMoveDistance", "Min Move Distance", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "mass", "Mass", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pushStrength", "Push Strength", sceneProject, entities, settingsPush);
    propertyRow(RowPropertyType::Bool, cpType, "pushBodies", "Push Bodies", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "movingPlatforms", "Moving Platforms", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "inheritPlatformYaw", "Inherit Platform Yaw", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "detectCollisions", "Detect Collisions", sceneProject, entities, settingsDetect);
    propertyRow(RowPropertyType::Bool, cpType, "characterCollisions", "Character Collisions", sceneProject, entities, settingsCharacters);
    propertyRow(RowPropertyType::Bool, cpType, "enhancedEdgeRemoval", "Enhanced Edge Removal", sceneProject, entities, settingsEdges);
    endTable();

    for (Entity entity : entities){
        if (scene->findComponent<RigidbodyComponent>(entity)){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("A Character Controller does nothing on a GameObject with a Rigidbody: remove one of them.");
            break;
        }
    }

    // what the last move and physics step left, while the scene plays
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        const CharacterControllerComponent& controller = scene->getComponent<CharacterControllerComponent>(entities[0]);
        std::string flags;
        if (controller.collisionFlags & CharacterCollision_Sides) flags += " Sides";
        if (controller.collisionFlags & CharacterCollision_Above) flags += " Above";
        if (controller.collisionFlags & CharacterCollision_Below) flags += " Below";
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Grounded: %s   On Steep Slope: %s", controller.isGrounded ? "yes" : "no", controller.isOnSteepSlope ? "yes" : "no");
        ImGui::TextDisabled("Collision Flags:%s", flags.empty() ? " None" : flags.c_str());
        ImGui::TextDisabled("Velocity: %.2f, %.2f, %.2f", controller.velocity.x, controller.velocity.y, controller.velocity.z);
        ImGui::TextDisabled("Ground Velocity: %.2f, %.2f, %.2f", controller.groundVelocity.x, controller.groundVelocity.y, controller.groundVelocity.z);
    }
}

void editor::InspectorWindow::drawWaterVolumeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const WaterVolumeComponent& volume = scene->getComponent<WaterVolumeComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSize = settings;
    settingsSize.help = "Local box under the surface: X and Z its extent, Y the depth";
    RowSettings settingsOffset = settings;
    settingsOffset.help = "Local height of the surface (the waves move it up and down)";
    RowSettings settingsInfinite;
    settingsInfinite.help = "An ocean: no extent and no bottom";
    RowSettings settingsDensity = settings;
    settingsDensity.help = "kg/m3: what floats in it settles as deep as its density is of this";
    RowSettings settingsFlow = settings;
    settingsFlow.help = "World m/s: what floats drifts with it";
    RowSettings settingsDrag = settings;
    settingsDrag.help = "Per second, against moving through the water (Linear) and turning in it (Angular)";
    RowSettings settingsWaves;
    settingsWaves.enumEntries = enumEntries(reflect::enums::kWaterVolumeWaves);
    settingsWaves.help = "From Material: the waves of this GameObject's library:water material (WAVES), the ones it draws";

    beginTable(cpType, getLabelSize("Surface Offset"));
    propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::Float, cpType, "surfaceOffset", "Surface Offset", sceneProject, entities, settingsOffset);
    propertyRow(RowPropertyType::Bool, cpType, "infinite", "Infinite", sceneProject, entities, settingsInfinite);
    propertyRow(RowPropertyType::FloatPositive, cpType, "density", "Density", sceneProject, entities, settingsDensity);
    propertyRow(RowPropertyType::Vector3, cpType, "flow", "Flow", sceneProject, entities, settingsFlow);
    propertyRow(RowPropertyType::FloatPositive, cpType, "linearDrag", "Linear Drag", sceneProject, entities, settingsDrag);
    propertyRow(RowPropertyType::FloatPositive, cpType, "angularDrag", "Angular Drag", sceneProject, entities, settingsDrag);
    propertyRow(RowPropertyType::Enum, cpType, "waves", "Waves", sceneProject, entities, settingsWaves);
    endTable();

    // Fit to Mesh: the volume over the GameObject's mesh, its surface at the mesh's top
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    const bool fits = entities.size() == 1 && mesh && !mesh->aabb.isNull();
    ImGui::BeginDisabled(!fits);
    if (ImGui::Button(ICON_FA_EXPAND " Fit to Mesh")){
        const Vector3 min = mesh->aabb.getMinimum();
        const Vector3 max = mesh->aabb.getMaximum();
        auto multiCmd = new MultiPropertyCmd();
        multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "size",
                                          Vector3(max.x - min.x, volume.size.y, max.z - min.z));
        multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType, "surfaceOffset", max.y);
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        multiCmd->setNoMerge();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(fits ? "Size X and Z and the surface from this GameObject's mesh bounds" : "Needs a mesh on this GameObject");

    // the drawn surface is linear between vertices; the physics follows the exact waves
    if (entities.size() == 1 && volume.waves == WaterVolumeWaves::FROM_MATERIAL){
        const float waveLength = WaterEditorUtils::materialWaveLength(scene, entities[0]);
        const float spacing = WaterEditorUtils::meshVertexSpacing(scene, entities[0]);
        if (waveLength > 0.0f && spacing > waveLength * 0.6f / 8.0f){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("The mesh's vertices are %.2f apart, more than an eighth of the shorter wave (%.2f): the drawn surface "
                               "departs from the one things float on by more than about 8%% of the wave height. Subdivide the mesh.",
                               spacing, waveLength * 0.6f / 8.0f);
        }
    }
}

void editor::InspectorWindow::drawWaterExcluderComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const WaterExcluderComponent& excluder = scene->getComponent<WaterExcluderComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsShape;
    settingsShape.enumEntries = enumEntries(reflect::enums::kWaterExcluderShape);
    settingsShape.help = "Convex Hull: the hull of this GameObject's mesh (or its model's meshes); Box and Sphere: Center and Size";
    RowSettings settingsCenter = settings;
    settingsCenter.help = "Local centre of the Box or Sphere";
    RowSettings settingsSize = settings;
    settingsSize.help = "Local size of the Box (a Sphere fills it)";

    beginTable(cpType, getLabelSize("Shape"));
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Shape", sceneProject, entities, settingsShape);
    if (excluder.shape != WaterExcluderShape::CONVEX_HULL){
        propertyRow(RowPropertyType::Vector3, cpType, "center", "Center", sceneProject, entities, settingsCenter);
        propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settingsSize);
    }
    endTable();

    auto warning = [](const char* text){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    };

    if (excluder.shape != WaterExcluderShape::CONVEX_HULL){
        // Fit to Mesh: the box over the GameObject's mesh (or its model's meshes)
        const AABB bounds = entities.size() == 1 ? WaterEditorUtils::meshSourceLocalBounds(scene, entities[0]) : AABB();
        ImGui::BeginDisabled(bounds.isNull());
        if (ImGui::Button(ICON_FA_EXPAND " Fit to Mesh")){
            auto multiCmd = new MultiPropertyCmd();
            multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "center", bounds.getCenter());
            multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "size", bounds.getSize());
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            multiCmd->setNoMerge();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(bounds.isNull() ? "Needs a mesh on this GameObject" : "Center and Size from this GameObject's mesh bounds");
    }else if (entities.size() == 1){
        std::shared_ptr<WaterInteractionSystem> system = scene->findSystem<WaterInteractionSystem>();
        const WaterInteractionSystem::HullInfo hull = system ? system->getHullInfo(entities[0]) : WaterInteractionSystem::HullInfo();
        switch (hull.status){
            case WaterHullStatus::READY:
                ImGui::TextDisabled("Convex hull: %zu faces from %zu vertices", hull.faces, hull.sourceVertices);
                break;
            case WaterHullStatus::PENDING:
                ImGui::TextDisabled("Waiting for the model to load");
                break;
            case WaterHullStatus::NO_MESH:
                warning("No mesh on this GameObject: it keeps nothing out. Choose Box or Sphere, or add a mesh.");
                break;
            case WaterHullStatus::FLAT:
                warning("The mesh is flat: it has no hull. Choose Box.");
                break;
            case WaterHullStatus::NO_PHYSICS:
                warning("This build has no 3D physics: the hull is the mesh's bounds box.");
                break;
            case WaterHullStatus::NONE:
                break;
        }
    }

    if (!WaterEditorUtils::sceneDrawsWaterInteraction(scene, false))
        warning("No water in this scene is cut by Water Excluders: use library:water (or a shader that requires waterexclusion).");
    for (Entity entity : entities){
        if (WaterEditorUtils::excluderDriesItsOwnBodies(scene, entity)){
            warning("A floating Rigidbody is under this Water Excluder, which belongs to no Rigidbody: it is kept dry and will not float. "
                    "Put the Water Excluder on the Rigidbody's GameObject or under it.");
            break;
        }
    }
}

void editor::InspectorWindow::drawWaterWakeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const WaterWakeComponent& wake = scene->getComponent<WaterWakeComponent>(entities[0]);

    RowSettings settingsFoam;
    settingsFoam.secondColSize = 6 * ImGui::GetFontSize();
    settingsFoam.floatMin = 0.0f;
    settingsFoam.floatMax = 2.0f;
    RowSettings settingsWaves = settingsFoam;
    settingsFoam.help = "Churn behind the stern, spray along the hull and breaking bow waves";
    settingsWaves.help = "The diverging and transverse waves of the wake";
    RowSettings settingsSize;
    settingsSize.secondColSize = 6 * ImGui::GetFontSize();
    settingsSize.help = "Waterline extent along local X (Width) and Z (Length), metres; 0: Auto, from the Water Excluder or the mesh";

    beginTable(cpType, getLabelSize("Length"));
    propertyRow(RowPropertyType::FloatPositive, cpType, "foam", "Foam", sceneProject, entities, settingsFoam);
    propertyRow(RowPropertyType::FloatPositive, cpType, "waves", "Waves", sceneProject, entities, settingsWaves);
    propertyRow(RowPropertyType::FloatPositive, cpType, "width", "Width", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "length", "Length", sceneProject, entities, settingsSize);
    endTable();

    if (entities.size() == 1 && (wake.width <= 0.0f || wake.length <= 0.0f)){
        // what Auto resolves to: the excluder's box or the mesh's, as WaterInteractionSystem reads them
        AABB bounds;
        if (const WaterExcluderComponent* excluder = scene->findComponent<WaterExcluderComponent>(entities[0])){
            std::shared_ptr<WaterInteractionSystem> system = scene->findSystem<WaterInteractionSystem>();
            bounds = getWaterExcluderLocalBox(excluder->shape, excluder->center, excluder->size,
                                              system ? system->getHull(entities[0]) : nullptr);
        }
        if (bounds.isNull())
            bounds = WaterEditorUtils::meshSourceLocalBounds(scene, entities[0]);
        if (const Transform* transform = scene->findComponent<Transform>(entities[0]); transform && !bounds.isNull()){
            const Vector3 size = bounds.getSize();
            ImGui::TextDisabled("Auto: %.2f m wide, %.2f m long", size.x * std::fabs(transform->worldScale.x),
                                size.z * std::fabs(transform->worldScale.z));
        }else if (bounds.isNull()){
            ImGui::TextDisabled("Auto: 1 m wide, 1 m long (no Water Excluder or mesh)");
        }
    }

    if (entities.size() == 1 && Engine::isSceneRunning(scene)){
        std::shared_ptr<WaterInteractionSystem> system = scene->findSystem<WaterInteractionSystem>();
        const WaterInteractionSystem::EmitterInfo info = system ? system->getEmitterInfo(entities[0]) : WaterInteractionSystem::EmitterInfo();
        if (info.emitting)
            ImGui::TextDisabled("Emitting at %.1f m/s on %s", info.speed, scene->getEntityName(info.water).c_str());
        else
            ImGui::TextDisabled("Not on water");
    }

    if (!WaterEditorUtils::sceneDrawsWaterInteraction(scene, true)){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("No water in this scene shows Water Wakes: use library:water (or a shader that requires waterwake).");
    }
}

void editor::InspectorWindow::drawBuoyancyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const BuoyancyComponent& buoyancy = scene->getComponent<BuoyancyComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsColumns;
    settingsColumns.enumEntries = enumEntries(reflect::enums::kBuoyancyColumns);
    settingsColumns.help = "Where it samples the water: a grid on the bottom of its colliders, or the points below";
    RowSettings settingsVolume = settings;
    settingsVolume.help = "Times the colliders' volume: more floats higher";
    RowSettings settingsDrag = settings;
    settingsDrag.help = "Times the water's drags";

    beginTable(cpType, getLabelSize("Volume Scale"));
    propertyRow(RowPropertyType::Enum, cpType, "columns", "Columns", sceneProject, entities, settingsColumns);
    propertyRow(RowPropertyType::FloatPositive, cpType, "volumeScale", "Volume Scale", sceneProject, entities, settingsVolume);
    propertyRow(RowPropertyType::FloatPositive, cpType, "dragScale", "Drag Scale", sceneProject, entities, settingsDrag);
    endTable();

    if (buoyancy.columns == BuoyancyColumns::CUSTOM && entities.size() == 1){
        const Entity entity = entities[0];
        auto setPoints = [&](const std::vector<Vector3>& points){
            auto* cmd = new editor::PropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "customPoints", points);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        };
        ImGui::SeparatorText("Column Points (local, at most 9)");
        std::vector<Vector3> points = buoyancy.customPoints;
        for (size_t i = 0; i < points.size(); i++){
            ImGui::PushID((int)i);
            Vector3 point = points[i];
            ImGui::SetNextItemWidth(-3.0f * ImGui::GetFontSize());
            if (ImGui::DragFloat3("##point", &point.x, 0.01f) ){
                points[i] = point;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                setPoints(points);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK)){
                points.erase(points.begin() + (long)i);
                setPoints(points);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::BeginDisabled(points.size() >= 9);
        if (ImGui::Button(ICON_FA_PLUS " Add Point")){
            points.push_back(Vector3(0.0f, 0.0f, 0.0f));
            setPoints(points);
        }
        ImGui::EndDisabled();
    }

    auto warning = [](const char* text){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    };
    for (Entity entity : entities){
        const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        if (!body || body->type != BodyType::DYNAMIC){
            warning("Buoyancy floats a dynamic Rigidbody: add one to this GameObject (or make it Dynamic).");
            break;
        }
    }
    if (scene->getComponentArray<WaterVolumeComponent>()->size() == 0)
        warning("There is no Water Volume in this scene: nothing floats it.");

    // its effective density against the water's
    if (entities.size() == 1){
        float volume = 0.0f, mass = 0.0f;
        const float water = WaterEditorUtils::waterDensity(scene, entities[0]);
        if (WaterEditorUtils::colliderMass(scene, entities[0], volume, mass) && water > 0.0f){
            const float effective = mass / (volume * std::max(buoyancy.volumeScale, 1e-6f));
            ImGui::TextDisabled("Effective density %.0f kg/m3 (water %.0f)", effective, water);
            if (effective >= water)
                warning("At least as dense as the water: it will sink or hover.");
        }
    }
}

// upstream (physics-contacts): the body's callbacks and what it touches, in Play
void editor::InspectorWindow::drawPhysicsContacts(SceneProject* sceneProject, Entity entity, bool is2D){
    Scene* scene = sceneProject->scene;
    // read again at most 10 times a second
    PhysicsContactsView& view = physicsContactsViews[{sceneProject->id, entity, is2D}];
    const double now = ImGui::GetTime();
    if (view.readAt < 0.0 || now - view.readAt >= 0.1){
        view.readAt = now;
        std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
        view.contacts.clear();
#ifdef JASPERENGINE_PHYSICS_2D
        if (is2D && scene->findComponent<Rigidbody2DComponent>(entity)) view.contacts = physics->getContactSummary2D(entity);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        if (!is2D && scene->findComponent<RigidbodyComponent>(entity)) view.contacts = physics->getContactSummary3D(entity);
#endif
        view.callbacks = ScriptMessages::describeReceivers(scene, entity, is2D ? PhysicsCallbacks::ALL_2D : PhysicsCallbacks::ALL_3D);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
    std::string callbacks;
    for (const std::string& name : view.callbacks){
        callbacks += (callbacks.empty() ? "" : ", ") + name;
    }
    ImGui::TextWrapped("Callbacks: %s", callbacks.empty() ? "none (define onCollisionEnter / onTriggerEnter on a script)" : callbacks.c_str());
    ImGui::PopStyleColor();

    const std::string title = "Contacts (" + std::to_string(view.contacts.size()) + ")##physics_contacts_" + std::string(is2D ? "2d" : "3d");
    if (ImGui::TreeNode(title.c_str())){
        if (view.contacts.empty()){
            ImGui::TextDisabled("Touching nothing");
        }
        for (const ContactSummary& contact : view.contacts){
            const std::string other = scene->isEntityCreated(contact.other) ? scene->getEntityName(contact.other) : ("#" + std::to_string(contact.other));
            std::string line = other + "  collider " + std::to_string(contact.collider) + " / " + std::to_string(contact.otherCollider);
            if (contact.trigger) line += "  trigger";
            if (contact.sinceStep > 0) line += "  since step " + std::to_string(contact.sinceStep);
            if (contact.asleep) line += "  asleep";
            ImGui::BulletText("%s", line.c_str());
        }
        ImGui::TreePop();
    }
}
