// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector of 2D physics: Rigidbody 2D (body fields, colliders without a count or point
// limit, materials), Joint 2D (every parameter, laid out like Unity's joints, runtime read-outs in
// Play) and the asset view of a .physicsmaterial.

#include "InspectorWindow.h"

#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "object/physics/Joint2D.h"
#include "object/physics/Rigidbody2D.h"
#include "physics/PhysicsMaterial.h"
#include "physics/PolygonDecomposition2D.h"
#include "subsystem/PhysicsSystem.h"
#include "physics/AttachedColliders.h"
#include "util/Angle.h"
#include "util/PhysicsMaterialAssets.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::vector<EnumEntry> entriesBodyType2D = {
        { (int)BodyType::STATIC, "Static" },
        { (int)BodyType::KINEMATIC, "Kinematic" },
        { (int)BodyType::DYNAMIC, "Dynamic" }
    };

    std::vector<EnumEntry> entriesColliderType2D = {
        { (int)Collider2DType::POLYGON, "Polygon" },
        { (int)Collider2DType::CIRCLE, "Circle" },
        { (int)Collider2DType::CAPSULE, "Capsule" },
        { (int)Collider2DType::SEGMENT, "Segment" },
        { (int)Collider2DType::CHAIN, "Chain" }
    };

    std::vector<EnumEntry> entriesCollisionDetection2D = {
        { (int)CollisionDetectionMode2D::DISCRETE, "Discrete" },
        { (int)CollisionDetectionMode2D::CONTINUOUS, "Continuous" }
    };

    std::vector<EnumEntry> entriesSleepMode2D = {
        { (int)RigidbodySleepMode2D::NEVER_SLEEP, "Never Sleep" },
        { (int)RigidbodySleepMode2D::START_AWAKE, "Start Awake" },
        { (int)RigidbodySleepMode2D::START_ASLEEP, "Start Asleep" }
    };

    std::vector<EnumEntry> entriesJointType2D = {
        { (int)Joint2DType::DISTANCE, "Distance" },
        { (int)Joint2DType::REVOLUTE, "Revolute (Hinge)" },
        { (int)Joint2DType::PRISMATIC, "Prismatic (Slider)" },
        { (int)Joint2DType::MOUSE, "Mouse (Target)" },
        { (int)Joint2DType::WHEEL, "Wheel" },
        { (int)Joint2DType::WELD, "Weld (Fixed)" },
        { (int)Joint2DType::MOTOR, "Motor (Relative)" }
    };

    std::vector<EnumEntry> entriesAnchorSpace = {
        { (int)JointAnchorSpace::LOCAL, "Local" },
        { (int)JointAnchorSpace::WORLD, "World" }
    };

    std::vector<EnumEntry> entriesBreakAction = {
        { (int)JointBreakAction::IGNORE, "Ignore" },
        { (int)JointBreakAction::CALLBACK_ONLY, "Callback Only" },
        { (int)JointBreakAction::DISABLE, "Disable" },
        { (int)JointBreakAction::DESTROY, "Destroy" }
    };

    // the polygon's area (shoelace, any simple polygon) in square points
    float polygonArea(const std::vector<Vector2>& points){
        double area = 0.0;
        for (size_t i = 0; i < points.size(); i++){
            const Vector2& a = points[i];
            const Vector2& b = points[(i + 1) % points.size()];
            area += (double)a.x * b.y - (double)b.x * a.y;
        }
        return (float)std::fabs(area * 0.5);
    }

    // the mass Box2D gives the body's colliders (area x density, metres), before it is built
    float estimateColliderMass(const Rigidbody2DComponent& body, const Vector2& scale, float pointsToMeter){
        const float areaScale = std::fabs(scale.x * scale.y) / (pointsToMeter * pointsToMeter);
        const float radiusScale = std::max(std::fabs(scale.x), std::fabs(scale.y)) / pointsToMeter;
        float mass = 0.0f;
        for (const Collider2D& collider : body.colliders){
            if (collider.sensor) continue;
            float area = 0.0f;
            switch (collider.type){
                case Collider2DType::POLYGON:
                    if (collider.points.size() >= 3){
                        area = polygonArea(collider.points) * areaScale;
                    }else{
                        const Vector2 size = collider.pointB - collider.pointA;
                        area = std::fabs(size.x * size.y) * areaScale;
                    }
                    break;
                case Collider2DType::CIRCLE: {
                    const float r = collider.radius * radiusScale;
                    area = (float)M_PI * r * r;
                    break;
                }
                case Collider2DType::CAPSULE: {
                    const float r = collider.radius * radiusScale;
                    const Vector2 d = (collider.pointB - collider.pointA) * scale;
                    area = (float)M_PI * r * r + 2.0f * r * std::sqrt(d.x * d.x + d.y * d.y) / pointsToMeter;
                    break;
                }
                default:
                    break;
            }
            mass += area * std::max(0.0f, collider.density);
        }
        return mass;
    }

    std::string formatPoints(const std::vector<Vector2>& points){
        std::ostringstream out;
        for (const Vector2& point : points){
            // round-trip precision (paste gives the same floats back)
            char line[64];
            std::snprintf(line, sizeof(line), "%.9g,%.9g\n", point.x, point.y);
            out << line;
        }
        return out.str();
    }

    // "x,y" (or "x y", "x;y", "[x, y]") lines; false when no point can be read
    bool parsePoints(const std::string& text, std::vector<Vector2>& points){
        points.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)){
            for (char& c : line){
                if (c == ',' || c == ';' || c == '[' || c == ']' || c == '(' || c == ')' || c == '\t') c = ' ';
            }
            std::istringstream values(line);
            float x = 0.0f, y = 0.0f;
            if (values >> x >> y){
                points.push_back(Vector2(x, y));
            }
        }
        return !points.empty();
    }

    // every .physicsmaterial below the assets directory, asset-relative
    std::vector<std::string> listPhysicsMaterials(Project* project){
        std::vector<std::string> files;
        std::error_code ec;
        const fs::path root = project->getAssetsPath();
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)){
            if (ec) break;
            const fs::path& path = it->path();
            const std::string name = path.filename().string();
            if (!name.empty() && name[0] == '.' && it->is_directory(ec)){
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && PhysicsMaterialAssets::isPhysicsMaterialFile(path.string())){
                files.push_back(project->normalizeToAssetsRelative(path).generic_string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // a joint field's value on every selected joint
    template<typename T>
    void setJointField(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType cpType,
                       const std::string& property, const T& value, MultiPropertyCmd* multi){
        for (Entity entity : entities){
            if (sceneProject->scene->findComponent<Joint2DComponent>(entity)){
                multi->addPropertyCmd<T>(project, sceneProject->id, entity, cpType, property, value);
            }
        }
    }

}

bool editor::InspectorWindow::physicsMaterialRow(ComponentType cpType, const std::string& property, const std::string& label,
                                                 SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& help){
    std::string current;
    bool mixed = false;
    for (size_t i = 0; i < entities.size(); i++){
        PropertyData data = Catalog::findProperty(sceneProject->scene, entities[i], cpType, property);
        const std::string value = data.ref ? *static_cast<std::string*>(data.ref) : std::string();
        if (i == 0) current = value;
        else if (value != current) mixed = true;
    }

    propertyHeader(label);
    if (!help.empty() && ImGui::IsItemHovered()){
        ImGui::SetTooltip("%s", help.c_str());
    }

    bool changed = false;
    auto apply = [&](const std::string& value){
        MultiPropertyCmd* multi = new MultiPropertyCmd();
        for (Entity entity : entities){
            if (Catalog::findProperty(sceneProject->scene, entity, cpType, property).ref){
                multi->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, property, value);
            }
        }
        multi->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
        changed = true;
    };

    ImGui::PushID(property.c_str());
    const float buttonWidth = ImGui::GetFrameHeight();
    const float fieldWidth = std::max(60.0f, ImGui::GetContentRegionAvail().x - (buttonWidth + ImGui::GetStyle().ItemSpacing.x) * (current.empty() ? 1 : 2));
    std::string shown = mixed ? std::string("\xE2\x80\x94") : (current.empty() ? std::string("None (Physics Material)") : fs::path(current).stem().string());
    if (!current.empty() && !mixed && !fs::exists(project->getAssetsPath() / current)){
        shown += " (missing)";
    }
    if (current.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::Button((std::string(ICON_FA_CIRCLE_HALF_STROKE " ") + shown + "##material_field").c_str(), ImVec2(fieldWidth, 0));
    if (current.empty()) ImGui::PopStyleColor();
    if (!current.empty() && ImGui::IsItemHovered()){
        ImGui::SetTooltip("%s\nDrop a .physicsmaterial from the Project window to change it", current.c_str());
    }else if (ImGui::IsItemHovered()){
        ImGui::SetTooltip("Drop a .physicsmaterial from the Project window, or pick one");
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery)){
            std::vector<std::string> received = editor::Util::getStringsFromPayload(payload);
            if (!received.empty() && PhysicsMaterialAssets::isPhysicsMaterialFile(received[0]) && payload->IsDelivery()){
                const fs::path absolute = project->resolveAssetPath(fs::path(received[0]));
                apply(project->normalizeToAssetsRelative(absolute).generic_string());
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LIST "##pick_material", ImVec2(buttonWidth, 0))){
        ImGui::OpenPopup("##physics_material_picker");
    }
    if (ImGui::IsItemHovered()){
        ImGui::SetTooltip("Pick a Physics Material of the project");
    }
    if (!current.empty()){
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK "##clear_material", ImVec2(buttonWidth, 0))){
            apply(std::string());
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("None: the friction and restitution below (or the body's material)");
        }
    }
    if (ImGui::BeginPopup("##physics_material_picker")){
        if (ImGui::Selectable("None", current.empty())){
            apply(std::string());
        }
        for (const std::string& file : listPhysicsMaterials(project)){
            if (ImGui::Selectable(file.c_str(), file == current)){
                apply(file);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void editor::InspectorWindow::drawRigidbody2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Rigidbody2DComponent& body = sceneProject->scene->getComponent<Rigidbody2DComponent>(entities[0]);
    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;
    const float pointsToMeter = sceneProject->scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();

    auto getSizeForShape = [sceneProject](Entity entity, float defaultSize = 100.0f) -> Vector2 {
        float width = defaultSize;
        float height = defaultSize;

        if (SpriteComponent* spriteComp = sceneProject->scene->findComponent<SpriteComponent>(entity)){
            // world units: the pixel size over its Pixels Per Unit (S41)
            const Vector2 worldSize = spriteWorldSize(*spriteComp);
            if (spriteComp->width > 0){
                width = worldSize.x;
            }
            if (spriteComp->height > 0){
                height = worldSize.y;
            }
        }else if (RectTransformComponent* layoutComp = sceneProject->scene->findComponent<RectTransformComponent>(entity)){
            if (layoutComp->width > 0){
                width = (float)layoutComp->width;
            }
            if (layoutComp->height > 0){
                height = (float)layoutComp->height;
            }
        }

        return Vector2(width, height);
    };

    auto addCommand = [this](MultiPropertyCmd* multi){
        multi->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
    };

    // the body: Unity's Rigidbody 2D order
    RowSettings settingsBodyType;
    settingsBodyType.enumEntries = &entriesBodyType2D;
    RowSettings settingsDetection;
    settingsDetection.enumEntries = &entriesCollisionDetection2D;
    settingsDetection.help = "Continuous: swept against fast moving bodies too (a Box2D bullet), so it never tunnels; costs more";
    RowSettings settingsSleep;
    settingsSleep.enumEntries = &entriesSleepMode2D;
    RowSettings settingsMass;
    settingsMass.help = "Mass in kg. With Auto it comes from the colliders (area x density)";
    RowSettings settingsDamping;
    settingsDamping.help = "Slows the body down (Unity Linear / Angular Drag)";
    RowSettings settingsGravityScale;
    settingsGravityScale.help = "How much of the scene's 2D gravity the body gets (0: none, -1: up)";
    RowSettings settingsCenter;
    settingsCenter.help = "Body-local, in collider units (points). Auto: the colliders' centroid";
    RowSettings settingsInertia;
    settingsInertia.help = "Rotational inertia about the centre of mass, kg m\xC2\xB2. Auto: from the colliders (scaled with Mass)";

    beginTable(cpType, getLabelSize("Attach To Parent Body"));
    // S96 WP-C: Unity's colliders below a Rigidbody 2D; the body fields of an attached child are not used
    RowSettings settingsAttach;
    settingsAttach.help = "Its colliders join the body of the nearest parent with a Rigidbody 2D (their contacts reach this GameObject too); it has no body of its own";
    propertyRow(RowPropertyType::Bool, cpType, "attachToParentBody", "Attach To Parent Body", sceneProject, entities, settingsAttach);
    bool attachedToParent = false;
    if (body.attachToParentBody && entities.size() == 1){
        const Entity joined = AttachedColliders::findBody2D(sceneProject->scene, entities[0]);
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
    physicsMaterialRow(cpType, "material", "Material", sceneProject, entities,
                       "The material of the colliders that have none of their own (friction, bounciness, combine modes)");
    if (body.type == BodyType::DYNAMIC){
        propertyRowWithAutoButton(RowPropertyType::FloatPositive, cpType, "mass", "Mass", "useAutoMass", "Use Auto Mass (from the colliders)", sceneProject, entities, settingsMass);
        if (body.useAutoMass){
            // what the colliders give, as Box2D will compute it
            const Vector3 scale = sceneProject->scene->findComponent<Transform>(entities[0]) ? sceneProject->scene->getComponent<Transform>(entities[0]).worldScale : Vector3(1, 1, 1);
            float autoMass = estimateColliderMass(body, Vector2(scale.x, scale.y), pointsToMeter);
            if (playing && b2Body_IsValid(body.body)){
                autoMass = b2Body_GetMass(body.body);
            }
            propertyHeader("");
            ImGui::TextDisabled("Auto: %.3f kg", autoMass);
        }
        propertyRow(RowPropertyType::FloatPositive, cpType, "linearDamping", "Linear Damping", sceneProject, entities, settingsDamping);
        propertyRow(RowPropertyType::FloatPositive, cpType, "angularDamping", "Angular Damping", sceneProject, entities, settingsDamping);
        propertyRow(RowPropertyType::Float, cpType, "gravityScale", "Gravity Scale", sceneProject, entities, settingsGravityScale);
        propertyRow(RowPropertyType::Enum, cpType, "collisionDetection", "Collision Detection", sceneProject, entities, settingsDetection);
    }
    if (body.type != BodyType::STATIC){
        propertyRow(RowPropertyType::Enum, cpType, "sleepingMode", "Sleeping Mode", sceneProject, entities, settingsSleep);
    }
    if (body.type == BodyType::DYNAMIC){
        RowSettings settingsFixedRotation;
        settingsFixedRotation.help = "The body never turns (Unity Constraints > Freeze Rotation)";
        propertyRow(RowPropertyType::Bool, cpType, "fixedRotation", "Fixed Rotation", sceneProject, entities, settingsFixedRotation);
        propertyRowWithAutoButton(RowPropertyType::Vector2, cpType, "centerOfMass", "Center of Mass", "autoCenterOfMass", "Auto Center of Mass (the colliders' centroid)", sceneProject, entities, settingsCenter);
        propertyRowWithAutoButton(RowPropertyType::FloatPositive, cpType, "inertia", "Inertia", "autoInertia", "Auto Inertia (from the colliders)", sceneProject, entities, settingsInertia);
    }
    endTable();
    if (attachedToParent){
        ImGui::EndDisabled();
    }

    if (playing && b2Body_IsValid(body.body) && entities.size() == 1){
        // the live body (read only)
        ImGui::SeparatorText("Runtime");
        Rigidbody2D live(sceneProject->scene, entities[0]);
        const Vector2 velocity = live.getLinearVelocity();
        ImGui::TextDisabled("Mass %.3f kg   Inertia %.4f kg m\xC2\xB2", b2Body_GetMass(body.body), b2Body_GetRotationalInertia(body.body));
        ImGui::TextDisabled("Velocity (%.1f, %.1f) pt/s   Angular %.1f rad/s   %s", velocity.x, velocity.y, live.getAngularVelocity(),
                            b2Body_IsAwake(body.body) ? "awake" : "asleep");
    }

    ImGui::SeparatorText("Colliders");
    ImGui::Text("Colliders: %zu", body.colliders.size());
    if (!entities.empty()){
        if (const TilemapCollider2DComponent* tilemapCollider = sceneProject->scene->findComponent<TilemapCollider2DComponent>(entities[0])){
            if (tilemapCollider->generatedPolygons > 0){
                ImGui::TextDisabled("Generated by Tilemap Collider 2D: %u chains, %u polygons", tilemapCollider->generatedChains, tilemapCollider->generatedPolygons);
            }else{
                ImGui::TextDisabled("Generated by Tilemap Collider 2D: %u chains", tilemapCollider->generatedChains);
            }
        }
    }

    static int createCollider2DType = (int)Collider2DType::POLYGON;
    const char* createCollider2DLabel = "Polygon";
    for (const EnumEntry& entry : entriesColliderType2D){
        if (entry.value == createCollider2DType){
            createCollider2DLabel = entry.name;
            break;
        }
    }

    ImGui::SetNextItemWidth(9 * ImGui::GetFontSize());
    if (ImGui::BeginCombo("##shape2d_create_type", createCollider2DLabel)){
        for (const EnumEntry& entry : entriesColliderType2D){
            bool selected = (createCollider2DType == entry.value);
            if (ImGui::Selectable(entry.name, selected)){
                createCollider2DType = entry.value;
            }
            if (selected){
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Collider##rigidbody2d")){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

        for (Entity entity : entities){
            if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){

                Collider2D shape;
                shape.type = (Collider2DType)createCollider2DType;

                switch (shape.type){
                case Collider2DType::POLYGON:
                {
                    Vector2 layoutSize = getSizeForShape(entity, 0.0f);

                    if (layoutSize != Vector2::ZERO){
                        shape.pointA = Vector2(0.0f, 0.0f);
                        shape.pointB = Vector2(layoutSize.x, layoutSize.y);
                        shape.points = {Vector2(0.0f, 0.0f), Vector2(layoutSize.x, 0.0f), Vector2(layoutSize.x, layoutSize.y), Vector2(0.0f, layoutSize.y)};
                    }else{
                        layoutSize = Vector2(100.0f, 100.0f);

                        const float halfW = layoutSize.x * 0.5f;
                        const float halfH = layoutSize.y * 0.5f;

                        shape.pointA = Vector2(-halfW, -halfH);
                        shape.pointB = Vector2(halfW, halfH);
                        shape.points = {Vector2(-halfW, -halfH), Vector2(halfW, -halfH), Vector2(halfW, halfH), Vector2(-halfW, halfH)};
                    }

                    break;
                }
                case Collider2DType::CIRCLE:
                    shape.pointA = Vector2::ZERO;
                    shape.radius = 0.5f;
                    break;
                case Collider2DType::CAPSULE:
                    shape.pointA = Vector2(0.0f, -0.5f);
                    shape.pointB = Vector2(0.0f, 0.5f);
                    shape.radius = 0.5f;
                    break;
                case Collider2DType::SEGMENT:
                    shape.pointA = Vector2(-1.0f, 0.0f);
                    shape.pointB = Vector2(1.0f, 0.0f);
                    break;
                case Collider2DType::CHAIN:
                    shape.points = {Vector2(-0.5f, -0.5f), Vector2(0.5f, -0.5f), Vector2(0.5f, 0.5f), Vector2(-0.5f, 0.5f)};
                    break;
                }

                std::vector<Collider2D> colliders = bodyComp->colliders;
                colliders.push_back(shape);
                multiCmd->addPropertyCmd<std::vector<Collider2D>>(project, sceneProject->id, entity, cpType, "colliders", colliders);
            }
        }

        addCommand(multiCmd);
    }

    size_t numColliders = body.colliders.size();
    for (Entity entity : entities){
        numColliders = std::min(numColliders, sceneProject->scene->getComponent<Rigidbody2DComponent>(entity).colliders.size());
    }

    RowSettings settingsShapeType;
    settingsShapeType.enumEntries = &entriesColliderType2D;

    RowSettings settingsShapeValue;

    for (size_t s = 0; s < numColliders; s++){
        Rigidbody2DComponent& bodyRef = sceneProject->scene->getComponent<Rigidbody2DComponent>(entities[0]);
        if (s >= bodyRef.colliders.size()){
            break;
        }
        const Collider2D shape = bodyRef.colliders[s];

        ImGui::SeparatorText(("Collider " + std::to_string(s + 1)).c_str());
        // every widget of the collider under its own ID (Reverse, Add Point, copy / paste, the
        // point list... of collider 2 are not collider 1's), popped however the iteration ends
        ImGui::PushID((int)s);
        struct PopIdAtEnd{ ~PopIdAtEnd(){ ImGui::PopID(); } } popColliderId;
        if (ImGui::Button("Remove Collider")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                    if (s >= bodyComp->colliders.size()){
                        continue;
                    }
                    std::vector<Collider2D> colliders = bodyComp->colliders;
                    colliders.erase(colliders.begin() + (std::ptrdiff_t)s);
                    multiCmd->addPropertyCmd<std::vector<Collider2D>>(project, sceneProject->id, entity, cpType, "colliders", colliders);
                }
            }

            addCommand(multiCmd);
            break;
        }

        std::string shapeKey = "colliders[" + std::to_string(s) + "]";

        // a whole collider written on every selected body (points lists, presets)
        auto editCollider = [&](const std::function<bool(Collider2D&, Entity)>& edit){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                    if (s >= bodyComp->colliders.size()) continue;
                    Collider2D shapeValue = bodyComp->colliders[s];
                    if (edit(shapeValue, entity)){
                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
            }
            addCommand(multiCmd);
        };

        beginTable(cpType, getLabelSize("Enable Hit Events"), "rigidbody2d_collider");
        // a slice of the sheet sprite's (S44 R17): the collider is that slice's box on every frame
        const SpriteComponent* sheetSprite = sceneProject->scene->findComponent<SpriteComponent>(entities[0]);
        std::shared_ptr<const SpriteSheetData> sliceSheet = sheetSprite && !sheetSprite->sheet.empty() ? SpriteSheetPool::get(sheetSprite->sheet.path) : nullptr;
        if ((sliceSheet && !sliceSheet->slices.empty()) || !shape.slice.empty()) {
            propertyHeader("Slice");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##collider_slice", shape.slice.empty() ? "(None)" : shape.slice.c_str())) {
                std::vector<std::string> names = {""};
                if (sliceSheet) {
                    for (const SpriteSlice& slice : sliceSheet->slices) names.push_back(slice.name);
                }
                for (const std::string& name : names) {
                    if (ImGui::Selectable(name.empty() ? "(None)" : name.c_str(), name == shape.slice) && name != shape.slice) {
                        editCollider([&](Collider2D& value, Entity) {
                            value.slice = name;
                            if (!name.empty()) value.type = Collider2DType::POLYGON;
                            return true;
                        });
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Follows a slice of the Sprite's sheet: its box on every frame, no shape where the slice is hidden");
            }
        }
        propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".type", "Type", sceneProject, entities, settingsShapeType);

        RowSettings settingsFloat;
        settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

        bool removedPoint = false;
        if (shape.type == Collider2DType::POLYGON || shape.type == Collider2DType::CHAIN){
            const bool chain = shape.type == Collider2DType::CHAIN;
            if (chain){
                RowSettings loopSettings;
                loopSettings.help = "Closes the chain. An open chain of 4+ points doesn't collide on its first and last edges; 2 or 3 points make two-sided segments";
                propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".loop", "Loop", sceneProject, entities, loopSettings);

                propertyHeader("Collision Side");
                if (ImGui::Button("Reverse")){
                    editCollider([](Collider2D& value, Entity){ std::reverse(value.points.begin(), value.points.end()); return true; });
                }
                if (ImGui::IsItemHovered()){
                    ImGui::SetTooltip("Flip the side that collides, shown by the ticks in the scene");
                }
            }

            // the pieces Box2D gets: a convex polygon of up to 8 points is one, others are split
            // (the body's own, cached on the collider: not decomposed again every frame)
            if (!chain && shape.points.size() >= 3){
                Collider2D& cached = bodyRef.colliders[s];
                sceneProject->scene->getSystem<PhysicsSystem>()->updateCollider2DPieces(entities[0], cached);
                propertyHeader("Shape");
                if (!cached.decompositionError.empty()){
                    ImGui::TextColored(Theme::Colors::WarningText, "%s", cached.decompositionError.c_str());
                }else if (cached.selfIntersecting){
                    ImGui::TextColored(Theme::Colors::WarningText, "Crosses itself: its convex hull is used");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reorder the points into one outline, or use several colliders");
                }else if (cached.pieces.size() == 1){
                    ImGui::TextDisabled("Convex: 1 piece");
                }else{
                    ImGui::TextDisabled("%zu convex pieces%s", cached.pieces.size(), cached.droppedPieces > 0 ? " (slivers left out)" : "");
                    if (ImGui::IsItemHovered()){
                        ImGui::SetTooltip("Box2D polygons are convex with up to 8 points: this one is split into convex pieces "
                                          "(one collider for contacts and rays). For ground that wheels roll over, a Chain has no inner seams.");
                    }
                }
            }else if (chain){
                propertyHeader("Shape");
                if (shape.points.size() < 2){
                    ImGui::TextColored(Theme::Colors::WarningText, "A chain needs at least 2 points");
                }else if (shape.points.size() < 4){
                    ImGui::TextDisabled("%zu two-sided segment(s)", shape.loop && shape.points.size() == 3 ? (size_t)3 : shape.points.size() - 1);
                }else{
                    ImGui::TextDisabled("One-sided chain of %zu segments", shape.loop ? shape.points.size() : shape.points.size() - 1);
                }
            }

            propertyHeader("Points");
            ImGui::Text("%zu", shape.points.size());
            ImGui::SameLine();
            const bool addPoint = ImGui::Button("Add Point");
            UIProbe::item("physics2d.collider." + std::to_string(s) + ".add_point");
            if (addPoint){
                editCollider([](Collider2D& value, Entity){
                    const size_t count = value.points.size();
                    if (count >= 2){
                        value.points.push_back(value.points[count - 1] + (value.points[count - 1] - value.points[count - 2]));
                    }else if (count == 1){
                        value.points.push_back(value.points[0] + Vector2(10.0f, 0.0f));
                    }else{
                        value.points.push_back(Vector2::ZERO);
                    }
                    return true;
                });
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_COPY "##copy_points")){
                ImGui::SetClipboardText(formatPoints(shape.points).c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the points as x,y lines");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_PASTE "##paste_points")){
                std::vector<Vector2> pasted;
                const char* clipboard = ImGui::GetClipboardText();
                if (clipboard && parsePoints(clipboard, pasted)){
                    editCollider([&pasted](Collider2D& value, Entity){ value.points = pasted; return true; });
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace the points with the clipboard's x,y lines (one point per line)");

            float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
            float iconButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + clearButtonFramePadding * 2;
            ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - (iconButtonWidth + ImGui::GetStyle().ItemSpacing.x) * 3, 0);
            if (inputVerSize.x < 100.0f){
                inputVerSize.x = 100.0f;
            }

            RowSettings settingsVertex = settingsShapeValue;
            settingsVertex.secondColSize = inputVerSize.x;

            // long outlines (a terrain) start folded
            propertyHeader("");
            ImGui::SetNextItemOpen(shape.points.size() <= 16, ImGuiCond_Once);
            const bool pointsOpen = ImGui::TreeNodeEx("##points_list", ImGuiTreeNodeFlags_SpanAvailWidth, "Point list");
            if (pointsOpen){
                ImGui::TreePop();
                for (int v = 0; v < (int)shape.points.size(); v++){
                    propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".points[" + std::to_string(v) + "]", "Point " + std::to_string(v + 1), sceneProject, entities, settingsVertex);

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    const std::string suffix = "##point_" + std::to_string(s) + "_" + std::to_string(v);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(v == 0);
                    if (ImGui::Button((std::string(ICON_FA_ARROW_UP) + suffix + "_up").c_str())){
                        editCollider([v](Collider2D& value, Entity){
                            if (v <= 0 || v >= (int)value.points.size()) return false;
                            std::swap(value.points[v], value.points[v - 1]);
                            return true;
                        });
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(v + 1 >= (int)shape.points.size());
                    if (ImGui::Button((std::string(ICON_FA_ARROW_DOWN) + suffix + "_down").c_str())){
                        editCollider([v](Collider2D& value, Entity){
                            if (v + 1 >= (int)value.points.size()) return false;
                            std::swap(value.points[v], value.points[v + 1]);
                            return true;
                        });
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button((std::string(ICON_FA_TRASH_CAN) + suffix + "_remove").c_str())){
                        editCollider([v](Collider2D& value, Entity){
                            if (v >= (int)value.points.size()) return false;
                            value.points.erase(value.points.begin() + v);
                            return true;
                        });
                        removedPoint = true;
                    }
                    if (ImGui::IsItemHovered()){
                        ImGui::SetTooltip("Remove point");
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (removedPoint){
                        break;
                    }
                }
            }

            if (removedPoint){
                endTable();
                return;
            }

            if (!chain){
                propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
            }
        }else if (shape.type == Collider2DType::CIRCLE){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Center", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
        }else if (shape.type == Collider2DType::CAPSULE){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Center A", sceneProject, entities);
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointB", "Center B", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
        }else if (shape.type == Collider2DType::SEGMENT){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Point A", sceneProject, entities);
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointB", "Point B", sceneProject, entities);
        }

        if (shape.type != Collider2DType::SEGMENT && shape.type != Collider2DType::CHAIN){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".density", "Density", sceneProject, entities, settingsFloat);
        }
        physicsMaterialRow(cpType, shapeKey + ".material", "Material", sceneProject, entities,
                           "Friction, bounciness and how they combine with the other collider's; over the body's material and the values below");
        // a material (this collider's or the body's) replaces them
        const bool hasMaterial = !shape.material.empty() || !bodyRef.material.empty();
        ImGui::BeginDisabled(hasMaterial);
        RowSettings settingsSurface = settingsFloat;
        settingsSurface.help = hasMaterial ? "Unused: the material decides" : "Without a material: combined as Box2D does (geometric mean friction, maximum restitution)";
        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".friction", "Friction", sceneProject, entities, settingsSurface);
        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".restitution", "Restitution", sceneProject, entities, settingsSurface);
        ImGui::EndDisabled();
        if (shape.type != Collider2DType::CHAIN){
            RowSettings sensorSettings;
            sensorSettings.help = "Overlap only (Unity Is Trigger): never collides and always reports its sensor events";
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".sensor", "Is Trigger", sceneProject, entities, sensorSettings);
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".enableHitEvents", "Enable Hit Events", sceneProject, entities);
            RowSettings contactSettings;
            contactSettings.help = "Reports the global beginContact2D / endContact2D events (on for new colliders)";
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".contactEvents", "Contact Events", sceneProject, entities, contactSettings);
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".preSolveEvents", "PreSolve Events", sceneProject, entities);
            if (!shape.sensor){
                propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".sensorEvents", "Sensor Events", sceneProject, entities);
            }
            // Box2D's broad phase never pairs a kinematic sensor with static or kinematic bodies
            if (shape.sensor && bodyRef.type == BodyType::KINEMATIC){
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Note");
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 64, 255));
                ImGui::TextWrapped("Box2D never pairs kinematic sensors with static or kinematic bodies: make the other body dynamic (gravity scale 0) to detect it");
                ImGui::PopStyleColor();
            }
        }
        // physics-contacts D9: the collider's onCollision*2D / onTrigger*2D also go to this GameObject
        RowSettings receiverSettings;
        receiverSettings.help = "Its contact callbacks (onCollisionEnter2D, onTriggerEnter2D...) also go to this GameObject's scripts (Unity: a child collider's own scripts)";
        propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".receiver", "Send Callbacks To", sceneProject, entities, receiverSettings);
        if (shape.type == Collider2DType::POLYGON){
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
            propertyHeader("Reset Collider");
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(("Box##shape_preset_box_" + std::to_string(s)).c_str())){
                editCollider([&](Collider2D& value, Entity entity){
                    const Vector2 layoutSize = getSizeForShape(entity);
                    value.radius = 0.0f;
                    value.pointA = Vector2(0.0f, 0.0f);
                    value.pointB = Vector2(layoutSize.x, layoutSize.y);
                    value.points = {Vector2(0.0f, 0.0f), Vector2(layoutSize.x, 0.0f), Vector2(layoutSize.x, layoutSize.y), Vector2(0.0f, layoutSize.y)};
                    return true;
                });
            }
            if (ImGui::SmallButton(("Centered Box##shape_preset_centered_box_" + std::to_string(s)).c_str())){
                editCollider([&](Collider2D& value, Entity entity){
                    const Vector2 layoutSize = getSizeForShape(entity);
                    const float halfW = layoutSize.x * 0.5f;
                    const float halfH = layoutSize.y * 0.5f;
                    value.radius = 0.0f;
                    value.pointA = Vector2(-halfW, -halfH);
                    value.pointB = Vector2(halfW, halfH);
                    value.points = {Vector2(-halfW, -halfH), Vector2(halfW, -halfH), Vector2(halfW, halfH), Vector2(-halfW, halfH)};
                    return true;
                });
            }
            if (ImGui::SmallButton(("Rounded Box##shape_preset_rounded_box_" + std::to_string(s)).c_str())){
                editCollider([&](Collider2D& value, Entity entity){
                    const Vector2 layoutSize = getSizeForShape(entity);
                    const float halfW = layoutSize.x * 0.5f;
                    const float halfH = layoutSize.y * 0.5f;
                    value.radius = std::max(1.0f, std::min(layoutSize.x, layoutSize.y) * 0.04f);
                    value.pointA = Vector2(-halfW, -halfH);
                    value.pointB = Vector2(halfW, halfH);
                    value.points = {Vector2(-halfW, -halfH), Vector2(halfW, -halfH), Vector2(halfW, halfH), Vector2(-halfW, halfH)};
                    return true;
                });
            }
        }
        endTable();
    }

    // Play: what the body touches, and the contact callbacks its scripts define
    if (playing && entities.size() == 1){
        drawPhysicsContacts(sceneProject, entities[0], true);
    }
}

void editor::InspectorWindow::drawJoint2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Joint2DComponent& joint = sceneProject->scene->getComponent<Joint2DComponent>(entities[0]);
    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;
    Scene* scene = sceneProject->scene;

    auto addCommand = [this](MultiPropertyCmd* multi){
        multi->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
    };

    RowSettings settingsJointValue;
    RowSettings settingsBody;
    settingsBody.entityFilter.set(scene->getComponentId<Rigidbody2DComponent>());

    const Joint2DType type = joint.type;
    const bool local = joint.anchorSpace == JointAnchorSpace::LOCAL;
    const bool usesAnchors = type != Joint2DType::MOUSE && type != Joint2DType::MOTOR;
    const bool usesAxis = type == Joint2DType::WHEEL || type == Joint2DType::PRISMATIC;

    beginTable(cpType, getLabelSize("Connected Anchor"));

    // Joint Type: the fields still at the old type's defaults take the new one's (Unity's
    // per-type components have their own defaults)
    propertyHeader("Joint Type");
    const char* typeLabel = "Distance";
    for (const EnumEntry& entry : entriesJointType2D){
        if (entry.value == (int)type) typeLabel = entry.name;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##joint2d_type", typeLabel)){
        for (const EnumEntry& entry : entriesJointType2D){
            const bool selected = entry.value == (int)type;
            if (ImGui::Selectable(entry.name, selected) && !selected){
                MultiPropertyCmd* multi = new MultiPropertyCmd();
                for (Entity entity : entities){
                    Joint2DComponent* component = scene->findComponent<Joint2DComponent>(entity);
                    if (!component) continue;
                    Joint2DComponent changed = *component;
                    changeJoint2DType(changed, (Joint2DType)entry.value);
                    multi->addPropertyCmd<Joint2DType>(project, sceneProject->id, entity, cpType, "type", changed.type);
                    if (changed.enableSpring != component->enableSpring) multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "enableSpring", changed.enableSpring);
                    if (changed.frequency != component->frequency) multi->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "frequency", changed.frequency);
                    if (changed.dampingRatio != component->dampingRatio) multi->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "dampingRatio", changed.dampingRatio);
                    if (changed.maxForce != component->maxForce) multi->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, "maxForce", changed.maxForce);
                }
                addCommand(multi);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()){
        ImGui::SetTooltip("Unity: Wheel = WheelJoint2D, Revolute = HingeJoint2D, Prismatic = SliderJoint2D,\n"
                          "Distance = DistanceJoint2D / SpringJoint2D, Weld = FixedJoint2D,\n"
                          "Motor = RelativeJoint2D, Mouse = TargetJoint2D");
    }

    RowSettings settingsBodyA = settingsBody;
    settingsBodyA.help = "The body the joint is on (Unity: the joint's own Rigidbody 2D)";
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyA", "Rigidbody A", sceneProject, entities, settingsBodyA);
    if (type != Joint2DType::MOUSE){
        RowSettings settingsBodyB = settingsBody;
        settingsBodyB.help = local ? "The connected body (Unity connectedBody); none holds Rigidbody A to the world"
                                   : "The connected body (Unity connectedBody)";
        propertyRow(RowPropertyType::LocalEntity, cpType, "bodyB", "Connected Body", sceneProject, entities, settingsBodyB);
    }

    if (usesAnchors || usesAxis){
        RowSettings settingsSpace;
        settingsSpace.enumEntries = &entriesAnchorSpace;
        settingsSpace.help = "Local: anchors in each body's own space, the axis in Rigidbody A's (Unity; works inside prefabs).\n"
                             "World: world points taken when the joint is made (scenes saved before local anchors)";
        propertyRow(RowPropertyType::Enum, cpType, "anchorSpace", "Anchor Space", sceneProject, entities, settingsSpace);
        if (!local){
            propertyHeader("");
            ImGui::BeginDisabled(playing);
            if (ImGui::Button("Convert to Local")){
                // the same world points, now in each body's space (from the poses the bodies have)
                MultiPropertyCmd* multi = new MultiPropertyCmd();
                for (Entity entity : entities){
                    Joint2DComponent* component = scene->findComponent<Joint2DComponent>(entity);
                    if (!component || component->anchorSpace == JointAnchorSpace::LOCAL) continue;
                    Transform* transformA = component->bodyA != NULL_ENTITY ? scene->findComponent<Transform>(component->bodyA) : nullptr;
                    Transform* transformB = component->bodyB != NULL_ENTITY ? scene->findComponent<Transform>(component->bodyB) : nullptr;
                    if (!transformA) continue;
                    auto toLocal = [](Transform* transform, const Vector2& world){
                        if (!transform) return world;
                        const Vector3 local = transform->modelMatrix.inverse() * Vector3(world.x, world.y, transform->worldPosition.z);
                        return Vector2(local.x, local.y);
                    };
                    Vector2 worldA = component->anchorA;
                    Vector2 worldB = component->type == Joint2DType::DISTANCE ? component->anchorB : component->anchorA;
                    if (component->type == Joint2DType::DISTANCE && component->autoAnchors){
                        worldA = Vector2(transformA->worldPosition.x, transformA->worldPosition.y);
                        worldB = transformB ? Vector2(transformB->worldPosition.x, transformB->worldPosition.y) : Vector2::ZERO;
                    }
                    multi->addPropertyCmd<JointAnchorSpace>(project, sceneProject->id, entity, cpType, "anchorSpace", JointAnchorSpace::LOCAL);
                    multi->addPropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, "anchorA", toLocal(transformA, worldA));
                    multi->addPropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, "anchorB", toLocal(transformB, worldB));
                    multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "autoAnchors", false);
                    if (component->type == Joint2DType::WHEEL || component->type == Joint2DType::PRISMATIC){
                        // the world axis in A's frame, before A's scale (local axes are scaled by it)
                        Vector2 axis = component->axis;
                        if (axis.x * axis.x + axis.y * axis.y < 1.0e-6f) axis = Vector2(1.0f, 0.0f);
                        const Vector3 rotated = transformA->worldRotation.inverse().getRotationMatrix() * Vector3(axis.x, axis.y, 0.0f);
                        Vector2 localAxis(rotated.x, rotated.y);
                        const Vector3 scale = transformA->worldScale;
                        if (scale.x != 0.0f) localAxis.x /= scale.x;
                        if (scale.y != 0.0f) localAxis.y /= scale.y;
                        const float length = std::sqrt(localAxis.x * localAxis.x + localAxis.y * localAxis.y);
                        if (length > 0.0f) localAxis = localAxis * (1.0f / length);
                        multi->addPropertyCmd<Vector2>(project, sceneProject->id, entity, cpType, "axis", localAxis);
                    }
                }
                addCommand(multi);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
                ImGui::SetTooltip("Turns the world anchors into anchors local to each body, from where the bodies are now (one undo step).\n"
                                  "Local joints also keep the bodies' relative angle (World ones aligned them).");
            }
        }
    }

    if (usesAnchors){
        RowSettings settingsAnchor;
        settingsAnchor.help = local ? "Where the joint is on Rigidbody A, in its local space (collider units)" : "The joint's world point";
        if (type == Joint2DType::DISTANCE && !local){
            propertyRow(RowPropertyType::Bool, cpType, "autoAnchors", "Body Origins", sceneProject, entities);
            if (!joint.autoAnchors){
                propertyRow(RowPropertyType::Vector2, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsAnchor);
                propertyRow(RowPropertyType::Vector2, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsAnchor);
            }
        }else{
            propertyRow(RowPropertyType::Vector2, cpType, "anchorA", "Anchor", sceneProject, entities, settingsAnchor);
            if (local){
                RowSettings settingsConnected;
                settingsConnected.help = "Where the joint is on the connected body, in its local space (the world point without one)";
                if (type == Joint2DType::DISTANCE){
                    propertyRow(RowPropertyType::Vector2, cpType, "anchorB", "Connected Anchor", sceneProject, entities, settingsConnected);
                }else{
                    propertyRowWithAutoButton(RowPropertyType::Vector2, cpType, "anchorB", "Connected Anchor", "autoAnchors",
                                              "Auto Configure Connected Anchor: where Anchor is when the joint is made", sceneProject, entities, settingsConnected);
                }
            }
        }
    }

    if (usesAxis){
        RowSettings settingsAxis;
        settingsAxis.help = local ? "The line of motion in Rigidbody A's frame ((0, 1) = up, Unity's angle 90)" : "The line of motion, world";
        propertyRow(RowPropertyType::Vector2, cpType, "axis", "Axis", sceneProject, entities, settingsAxis);
        // Unity's suspension / slider angle from +x, in the project's angle unit
        propertyHeader("Angle");
        const bool degrees = Engine::isUseDegrees();
        float angle = Angle::radToDefault(std::atan2(joint.axis.y, joint.axis.x));
        ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
        if (ImGui::DragFloat("##joint2d_axis_angle", &angle, Angle::degToDefault(0.5f), Angle::degToDefault(-360.0f), Angle::degToDefault(360.0f),
                             degrees ? "%.1f\xC2\xB0" : "%.4f rad")){
            const float radians = Angle::defaultToRad(angle);
            MultiPropertyCmd* multi = new MultiPropertyCmd();
            setJointField<Vector2>(project, sceneProject, entities, cpType, "axis", Vector2(std::cos(radians), std::sin(radians)), multi);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
            cmd = multi;
        }
        UIProbe::item("physics2d.joint.axis_angle");
        // one undo step per drag
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd){
            cmd->setNoMerge();
            cmd = nullptr;
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("The axis as an angle from +x (Unity's suspension angle: 90 = up)");
        }
    }

    RowSettings settingsCollision;
    settingsCollision.help = "The two connected bodies collide with each other";
    propertyRow(RowPropertyType::Bool, cpType, "enableCollision", "Enable Collision", sceneProject, entities, settingsCollision);
    endTable();

    // per-type sections
    RowSettings settingsHz;
    settingsHz.help = "Stiffness in Hz (oscillations per second); higher is stiffer";
    RowSettings settingsDamping;
    settingsDamping.help = "0: oscillates; 1: critically damped (no overshoot)";
    RowSettings settingsAngle;
    settingsAngle.help = "Degrees, relative to the angle the bodies have when the joint is made";
    RowSettings settingsSpeedAngular;
    settingsSpeedAngular.help = "Degrees per second";
    RowSettings settingsSpeedLinear;
    settingsSpeedLinear.help = "Points per second";

    if (type == Joint2DType::WHEEL){
        ImGui::SeparatorText("Suspension");
        beginTable(cpType, getLabelSize("Damping Ratio"), "joint2d_suspension");
        RowSettings settingsSuspension;
        settingsSuspension.help = "The suspension spring along the axis (off: the wheel slides freely along it)";
        propertyRow(RowPropertyType::Bool, cpType, "enableSpring", "Spring", sceneProject, entities, settingsSuspension);
        propertyRow(RowPropertyType::FloatPositive, cpType, "frequency", "Frequency", sceneProject, entities, settingsHz);
        propertyRow(RowPropertyType::FloatPositive, cpType, "dampingRatio", "Damping Ratio", sceneProject, entities, settingsDamping);
        endTable();
    }else if (type == Joint2DType::REVOLUTE || type == Joint2DType::PRISMATIC || type == Joint2DType::DISTANCE){
        ImGui::SeparatorText("Spring");
        beginTable(cpType, getLabelSize("Damping Ratio"), "joint2d_spring");
        RowSettings settingsSpring;
        settingsSpring.help = type == Joint2DType::DISTANCE ? "Unity SpringJoint2D: the length is soft instead of rigid" : "A spring towards the rest position";
        propertyRow(RowPropertyType::Bool, cpType, "enableSpring", "Enable Spring", sceneProject, entities, settingsSpring);
        if (joint.enableSpring){
            propertyRow(RowPropertyType::FloatPositive, cpType, "frequency", "Frequency", sceneProject, entities, settingsHz);
            propertyRow(RowPropertyType::FloatPositive, cpType, "dampingRatio", "Damping Ratio", sceneProject, entities, settingsDamping);
        }
        endTable();
    }

    if (type == Joint2DType::DISTANCE){
        ImGui::SeparatorText("Distance");
        beginTable(cpType, getLabelSize("Max Distance Only"), "joint2d_distance");
        RowSettings settingsLength;
        settingsLength.help = "The distance kept between the anchors. Auto: their distance when the joint is made";
        propertyRowWithAutoButton(RowPropertyType::FloatPositive, cpType, "length", "Length", "autoLength", "Auto Configure Distance", sceneProject, entities, settingsLength);
        RowSettings settingsRope;
        settingsRope.help = "Unity maxDistanceOnly: the length is a maximum, the bodies may get closer (a rope)";
        propertyRow(RowPropertyType::Bool, cpType, "rope", "Max Distance Only", sceneProject, entities, settingsRope);
        RowSettings settingsLimit;
        settingsLimit.help = "Keeps the length between Min and Max (Max 0: the length)";
        propertyRow(RowPropertyType::Bool, cpType, "useLimits", "Use Limits", sceneProject, entities, settingsLimit);
        if (joint.useLimits){
            propertyRow(RowPropertyType::FloatPositive, cpType, "minLength", "Min Length", sceneProject, entities);
            propertyRow(RowPropertyType::FloatPositive, cpType, "maxLength", "Max Length", sceneProject, entities);
        }
        endTable();
    }

    if (type == Joint2DType::REVOLUTE || type == Joint2DType::PRISMATIC || type == Joint2DType::WHEEL){
        ImGui::SeparatorText(type == Joint2DType::WHEEL ? "Translation Limits" : (type == Joint2DType::REVOLUTE ? "Angle Limits" : "Translation Limits"));
        beginTable(cpType, getLabelSize("Lower Translation"), "joint2d_limits");
        propertyRow(RowPropertyType::Bool, cpType, "useLimits", "Use Limits", sceneProject, entities);
        if (joint.useLimits){
            if (type == Joint2DType::REVOLUTE){
                propertyRow(RowPropertyType::Float, cpType, "lowerAngle", "Lower Angle", sceneProject, entities, settingsAngle);
                propertyRow(RowPropertyType::Float, cpType, "upperAngle", "Upper Angle", sceneProject, entities, settingsAngle);
            }else{
                RowSettings settingsTranslation;
                settingsTranslation.help = "Points along the axis from where the anchors meet";
                propertyRow(RowPropertyType::Float, cpType, "lowerTranslation", "Lower Translation", sceneProject, entities, settingsTranslation);
                propertyRow(RowPropertyType::Float, cpType, "upperTranslation", "Upper Translation", sceneProject, entities, settingsTranslation);
            }
        }
        endTable();
    }

    if (type == Joint2DType::REVOLUTE || type == Joint2DType::PRISMATIC || type == Joint2DType::WHEEL || type == Joint2DType::DISTANCE){
        ImGui::SeparatorText("Motor");
        beginTable(cpType, getLabelSize("Max Motor Torque"), "joint2d_motor");
        RowSettings settingsUseMotor;
        settingsUseMotor.help = type == Joint2DType::DISTANCE ? "Drives the length (needs Enable Spring: a rigid distance overrides it)" : "Drives the joint at Motor Speed";
        propertyRow(RowPropertyType::Bool, cpType, "useMotor", "Use Motor", sceneProject, entities, settingsUseMotor);
        if (joint.useMotor){
            const bool angular = type == Joint2DType::REVOLUTE || type == Joint2DType::WHEEL;
            propertyRow(RowPropertyType::Float, cpType, "motorSpeed", "Motor Speed", sceneProject, entities, angular ? settingsSpeedAngular : settingsSpeedLinear);
            if (angular){
                RowSettings settingsTorque;
                settingsTorque.help = "N m: the most the motor can apply";
                propertyRow(RowPropertyType::FloatPositive, cpType, "maxMotorTorque", "Max Motor Torque", sceneProject, entities, settingsTorque);
            }else{
                RowSettings settingsForce;
                settingsForce.help = "N: the most the motor can apply";
                propertyRow(RowPropertyType::FloatPositive, cpType, "maxMotorForce", "Max Motor Force", sceneProject, entities, settingsForce);
            }
        }
        endTable();
    }

    if (type == Joint2DType::WELD){
        ImGui::SeparatorText("Stiffness");
        beginTable(cpType, getLabelSize("Angular Damping Ratio"), "joint2d_weld");
        RowSettings settingsWeldHz = settingsHz;
        settingsWeldHz.help = "0: rigid. Above 0: a spring in Hz (Unity FixedJoint2D frequency)";
        propertyRow(RowPropertyType::FloatPositive, cpType, "linearFrequency", "Linear Frequency", sceneProject, entities, settingsWeldHz);
        propertyRow(RowPropertyType::FloatPositive, cpType, "linearDampingRatio", "Linear Damping Ratio", sceneProject, entities, settingsDamping);
        propertyRow(RowPropertyType::FloatPositive, cpType, "angularFrequency", "Angular Frequency", sceneProject, entities, settingsWeldHz);
        propertyRow(RowPropertyType::FloatPositive, cpType, "angularDampingRatio", "Angular Damping Ratio", sceneProject, entities, settingsDamping);
        endTable();
    }

    if (type == Joint2DType::MOTOR){
        ImGui::SeparatorText("Offsets");
        beginTable(cpType, getLabelSize("Correction Factor"), "joint2d_offsets");
        RowSettings settingsOffset;
        settingsOffset.help = "The connected body's position relative to Rigidbody A, in A's frame (points). Auto: the pose they have when the joint is made";
        propertyRowWithAutoButton(RowPropertyType::Vector2, cpType, "linearOffset", "Linear Offset", "autoOffset", "Auto Configure Offset", sceneProject, entities, settingsOffset);
        RowSettings settingsAngularOffset;
        settingsAngularOffset.help = "Degrees";
        propertyRow(RowPropertyType::Float, cpType, "angularOffset", "Angular Offset", sceneProject, entities, settingsAngularOffset);
        RowSettings settingsMaxForce;
        settingsMaxForce.help = "N";
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxForce", "Max Force", sceneProject, entities, settingsMaxForce);
        RowSettings settingsMaxTorque;
        settingsMaxTorque.help = "N m";
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxTorque", "Max Torque", sceneProject, entities, settingsMaxTorque);
        RowSettings settingsCorrection;
        settingsCorrection.floatMin = 0.0f;
        settingsCorrection.floatMax = 1.0f;
        settingsCorrection.help = "How fast the offset is corrected, 0-1 (0: no position correction, Unity FrictionJoint2D)";
        propertyRow(RowPropertyType::Float_0_1, cpType, "correctionFactor", "Correction Factor", sceneProject, entities, settingsCorrection);
        endTable();
    }

    if (type == Joint2DType::MOUSE){
        ImGui::SeparatorText("Target");
        beginTable(cpType, getLabelSize("Damping Ratio"), "joint2d_target");
        RowSettings settingsTarget;
        settingsTarget.help = "The world point Rigidbody A is pulled to";
        propertyRow(RowPropertyType::Vector2, cpType, "target", "Target", sceneProject, entities, settingsTarget);
        propertyRow(RowPropertyType::FloatPositive, cpType, "frequency", "Frequency", sceneProject, entities, settingsHz);
        propertyRow(RowPropertyType::FloatPositive, cpType, "dampingRatio", "Damping Ratio", sceneProject, entities, settingsDamping);
        RowSettings settingsMouseForce;
        settingsMouseForce.help = "N; 0: 1000 x the body's mass (at least 10)";
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxForce", "Max Force", sceneProject, entities, settingsMouseForce);
        endTable();
    }

    // Break
    ImGui::SeparatorText("Break");
    beginTable(cpType, getLabelSize("Break Action"), "joint2d_break");
    auto thresholdRow = [&](const char* property, const char* label, const char* unit){
        const bool force = std::string(property) == "breakForce";
        const float value = force ? joint.breakForce : joint.breakTorque;
        const bool infinite = std::isinf(value);
        if (!infinite){
            RowSettings settingsThreshold;
            settingsThreshold.help = std::string("Breaks when the reaction passes it (") + unit + ")";
            settingsThreshold.secondColSize = std::max(60.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 3.0f);
            propertyRow(RowPropertyType::FloatPositive, cpType, property, label, sceneProject, entities, settingsThreshold);
            ImGui::SameLine();
        }else{
            propertyHeader(label);
            ImGui::TextDisabled("Infinity");
            ImGui::SameLine();
        }
        bool never = infinite;
        if (ImGui::Checkbox((std::string("\xE2\x88\x9E##infinite_") + property).c_str(), &never)){
            MultiPropertyCmd* multi = new MultiPropertyCmd();
            setJointField<float>(project, sceneProject, entities, cpType, property,
                                 never ? std::numeric_limits<float>::infinity() : 1000.0f, multi);
            addCommand(multi);
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("Infinity: never breaks");
        }
    };
    thresholdRow("breakForce", "Break Force", "N");
    thresholdRow("breakTorque", "Break Torque", "N m");
    RowSettings settingsAction;
    settingsAction.enumEntries = &entriesBreakAction;
    settingsAction.help = "Ignore: nothing. Callback Only: onJointBreak2D(joint). Disable: the joint is disabled, then the callback\n"
                          "(enable it again for a fresh joint).\n"
                          "Destroy: the callback, then the Joint 2D component is removed";
    propertyRow(RowPropertyType::Enum, cpType, "breakAction", "Break Action", sceneProject, entities, settingsAction);
    endTable();

    // Play: what the joint does now (read only)
    if (playing && entities.size() == 1){
        ImGui::SeparatorText("Runtime");
        Joint2D live(scene, entities[0]);
        if (!live.isLive()){
            ImGui::TextDisabled(live.isBroken() ? "Broken" : "Not in the simulation (disabled, inactive or missing a body)");
        }else{
            static const char* limitStates[] = {"Inactive", "Lower", "Upper", "Equal"};
            const Vector2 force = live.getReactionForce();
            ImGui::TextDisabled("Reaction Force (%.1f, %.1f) N   Torque %.2f N m", force.x, force.y, live.getReactionTorque());
            if (type == Joint2DType::REVOLUTE || type == Joint2DType::WHEEL){
                ImGui::TextDisabled("Angle %.1f   Speed %.1f /s", live.getJointAngle(), live.getJointSpeed());
                ImGui::TextDisabled("Motor Torque %.2f N m", live.getMotorTorque());
            }
            if (type == Joint2DType::PRISMATIC || type == Joint2DType::WHEEL){
                ImGui::TextDisabled("Translation %.2f pt   Speed %.2f pt/s", live.getJointTranslation(), live.getJointLinearSpeed());
            }
            if (type == Joint2DType::PRISMATIC || type == Joint2DType::DISTANCE){
                ImGui::TextDisabled("Motor Force %.2f N", live.getMotorForce());
            }
            if (type == Joint2DType::DISTANCE){
                ImGui::TextDisabled("Current Length %.2f pt", live.getCurrentLength());
            }
            if (type == Joint2DType::REVOLUTE || type == Joint2DType::PRISMATIC || type == Joint2DType::WHEEL || type == Joint2DType::DISTANCE){
                ImGui::TextDisabled("Limit State %s", limitStates[(int)live.getLimitState()]);
            }
        }
    }
}

void editor::InspectorWindow::drawPhysicsMaterialAsset(const fs::path& asset){
    drawAssetHeader(ICON_FA_CIRCLE_HALF_STROKE, asset, "Physics Material");
    ImGui::Separator();

    PhysicsMaterialData data;
    std::string error;
    if (!PhysicsMaterialAssets::read(asset, data, error)){
        ImGui::TextColored(Theme::Colors::WarningText, "Cannot read this physics material: %s", error.c_str());
        return;
    }

    // one undo step per drag of one property: the key names the file, the property and the drag
    // (a new one after each release), so Friction and Bounciness, or two drags, are separate steps
    static unsigned int dragSerial = 0;
    auto save = [&](const PhysicsMaterialData& changed, const char* property){
        std::string writeError;
        const std::string mergeKey = property ? "physicsmaterial:" + asset.generic_string() + ":" + property + "#" + std::to_string(dragSerial)
                                              : std::string();
        if (!PhysicsMaterialAssets::write(project, asset, changed, writeError, mergeKey)){
            Out::error("Physics material: " + writeError);
        }
    };

    static const char* combineNames[] = {"Average", "Geometric Mean", "Minimum", "Multiply", "Maximum"};
    auto combineCombo = [&](const char* id, PhysicsMaterialCombine mode, PhysicsMaterialCombine& out){
        int index = std::max(0, (int)mode - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo(id, &index, combineNames, 5)){
            out = (PhysicsMaterialCombine)(index + 1);
            return true;
        }
        return false;
    };

    if (ImGui::BeginTable("physics_material_asset", 2, ImGuiTableFlags_SizingFixedFit)){
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Friction Combine"));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Friction");
        ImGui::TableNextColumn();
        float friction = data.friction;
        ImGui::SetNextItemWidth(-1);
        const bool frictionChanged = ImGui::DragFloat("##friction", &friction, 0.01f, 0.0f, 100.0f, "%.3f");
        UIProbe::item("physics2d.material.friction");
        if (frictionChanged){
            PhysicsMaterialData changed = data;
            changed.friction = std::max(0.0f, friction);
            save(changed, "friction");
        }
        if (ImGui::IsItemDeactivated()) dragSerial++;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coulomb friction, 0 (ice) and up (above 1 is allowed, a tyre is about 2)");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Bounciness");
        ImGui::TableNextColumn();
        float bounciness = data.bounciness;
        ImGui::SetNextItemWidth(-1);
        const bool bouncinessChanged = ImGui::SliderFloat("##bounciness", &bounciness, 0.0f, 1.0f, "%.3f");
        UIProbe::item("physics2d.material.bounciness");
        if (bouncinessChanged){
            PhysicsMaterialData changed = data;
            changed.bounciness = std::min(1.0f, std::max(0.0f, bounciness));
            save(changed, "bounciness");
        }
        if (ImGui::IsItemDeactivated()) dragSerial++;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: no bounce, 1: bounces back to its height");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Friction Combine");
        ImGui::TableNextColumn();
        PhysicsMaterialCombine mode = data.frictionCombine;
        if (combineCombo("##friction_combine", data.frictionCombine, mode)){
            PhysicsMaterialData changed = data;
            changed.frictionCombine = mode;
            save(changed, nullptr);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Bounce Combine");
        ImGui::TableNextColumn();
        mode = data.bounceCombine;
        if (combineCombo("##bounce_combine", data.bounceCombine, mode)){
            PhysicsMaterialData changed = data;
            changed.bounceCombine = mode;
            save(changed, nullptr);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Two colliders combine with the highest mode of the two (Maximum > Multiply > Minimum > Geometric Mean > Average); "
                       "a collider without a material keeps the rule of before (geometric mean friction, maximum bounce). "
                       "Drop it on a collider's or a Rigidbody 2D's Material field.");
}
