// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateEntityCmd.h"
#include "component/SpriteBatchComponent.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/AudioSnapshotComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/FogComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UISystem.h"
#include "component/TrailRendererComponent.h"
#include "spline/SplinePresets.h"

#include "audio/AudioMixerDocument.h"

#include "editor/Out.h"
#include "util/PrimitiveParameters.h"
#include "util/ProjectUtils.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "object/environment/Water.h"
#include "component/FluidSimulationComponent.h"
#include "component/VideoPlayerComponent.h"
#include "particles/LegacyParticleConverter.h"

#include <algorithm>
#include <utility>

using namespace jasperengine;

editor::CreateEntityCmd::CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, bool addToPrefab)
    : CreateEntityCmd(project, sceneId, std::move(entityName), EntityCreationType::EMPTY, NULL_ENTITY, addToPrefab){
}

editor::CreateEntityCmd::CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, bool addToPrefab)
    : CreateEntityCmd(project, sceneId, std::move(entityName), type, NULL_ENTITY, addToPrefab){
}

editor::CreateEntityCmd::CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, Entity parent, bool addToPrefab){
    this->project = project;
    this->sceneId = sceneId;
    this->entityName = entityName;
    this->entity = NULL_ENTITY;
    this->parent = parent;
    this->type = type;
    this->addToPrefab = addToPrefab;
    this->wasModified = project->getScene(sceneId)->isModified;
    this->wasMainCamera = false;
    this->updateFlags = 0;
}

bool editor::CreateEntityCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (!sceneProject){
        return false;
    }

    Scene* scene = sceneProject->scene;
    childEntities.clear();

    auto registerChildEntity = [this](Entity childEntity, Entity childParent){
        childEntities.push_back({childEntity, childParent});
    };

    if (entity == NULL_ENTITY){
        entity = scene->createUserEntity();
    }else{
        if (!scene->recreateEntity(entity)){
            entity = scene->createUserEntity();
        }
    }

    entityName = ProjectUtils::makeUniqueEntityName(scene, sceneProject->entities, entityName);

    if (type == EntityCreationType::GAMEOBJECT){

        scene->addComponent<Transform>(entity, {});

    }else if (type == EntityCreationType::BOX){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createBox(mesh, shapeDefs.boxWidth, shapeDefs.boxHeight, shapeDefs.boxDepth);

    }else if (type == EntityCreationType::PLANE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createPlane(mesh, shapeDefs.planeWidth, shapeDefs.planeDepth);

    }else if (type == EntityCreationType::WALL){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createWall(mesh, shapeDefs.wallWidth, shapeDefs.wallHeight);

    }else if (type == EntityCreationType::MIRROR){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<MirrorComponent>(entity, {}); // default normal +Z matches the wall
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::MirrorComponent);

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createWall(mesh, shapeDefs.wallWidth, shapeDefs.wallHeight);

    }else if (type == EntityCreationType::SPHERE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createSphere(mesh, shapeDefs.sphereRadius, shapeDefs.sphereSlices, shapeDefs.sphereStacks);

    }else if (type == EntityCreationType::CYLINDER){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createCylinder(mesh, shapeDefs.cylinderBaseRadius, shapeDefs.cylinderTopRadius, shapeDefs.cylinderHeight, shapeDefs.cylinderSlices, shapeDefs.cylinderStacks);

    }else if (type == EntityCreationType::CAPSULE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createCapsule(mesh, shapeDefs.capsuleBaseRadius, shapeDefs.capsuleTopRadius, shapeDefs.capsuleHeight, shapeDefs.capsuleSlices, shapeDefs.capsuleStacks);

    }else if (type == EntityCreationType::TORUS){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        PrimitiveParameters shapeDefs;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createTorus(mesh, shapeDefs.torusRadius, shapeDefs.torusRingRadius, shapeDefs.torusSides, shapeDefs.torusRings);

    }else if (type == EntityCreationType::IMAGE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 100;
        layout.height = 100;

    }else if (type == EntityCreationType::TEXT){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<TextComponent>(entity, {});

        TextComponent& textComp = scene->getComponent<TextComponent>(entity);
        textComp.text = "Text";
        textComp.needUpdateText = true;

    }else if (type == EntityCreationType::BUTTON){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ButtonComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 150;
        layout.height = 50;

        ButtonComponent& buttonComp = scene->getComponent<ButtonComponent>(entity);
        scene->getSystem<UISystem>()->createButtonObjects(entity, buttonComp);

        registerChildEntity(buttonComp.label, entity);

        scene->setEntityName(buttonComp.label, "Label");

        TextComponent& textComp = scene->getComponent<TextComponent>(buttonComp.label);
        textComp.text = "Button";
        textComp.needUpdateText = true;

    }else if (type == EntityCreationType::SCROLLBAR){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ScrollbarComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 16;
        layout.height = 200;

        ScrollbarComponent& scrollbarComp = scene->getComponent<ScrollbarComponent>(entity);
        scene->getSystem<UISystem>()->createScrollbarObjects(entity, scrollbarComp);

        registerChildEntity(scrollbarComp.bar, entity);

        scene->setEntityName(scrollbarComp.bar, "Bar");

        UIComponent& barUi = scene->getComponent<UIComponent>(scrollbarComp.bar);
        barUi.color = Vector4(0.45f, 0.45f, 0.5f, 1.0f);

    }else if (type == EntityCreationType::PROGRESSBAR){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ProgressbarComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 200;
        layout.height = 16;

        ProgressbarComponent& progressbarComp = scene->getComponent<ProgressbarComponent>(entity);
        scene->getSystem<UISystem>()->createProgressbarObjects(entity, progressbarComp);

        registerChildEntity(progressbarComp.fill, entity);

        scene->setEntityName(progressbarComp.fill, "Fill");

        UIComponent& fillUi = scene->getComponent<UIComponent>(progressbarComp.fill);
        fillUi.color = Vector4(0.3f, 0.7f, 0.4f, 1.0f);

        progressbarComp.value = 0.5f;
        progressbarComp.needUpdateProgressbar = true;

    }else if (type == EntityCreationType::ONSCREEN_STICK_FIXED || type == EntityCreationType::ONSCREEN_STICK_FLOATING ||
              type == EntityCreationType::ONSCREEN_STICK_DYNAMIC){

        // the area a press may start in (transparent), its base circle and the knob inside it
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});
        scene->addComponent<OnScreenStickComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 300;
        layout.height = 300;
        layout.anchorPreset = AnchorPreset::BOTTOM_LEFT;
        layout.usingAnchors = true;
        scene->getComponent<UIComponent>(entity).color = Vector4(1.0f, 1.0f, 1.0f, 0.0f);

        const std::string baseTexture = ProjectUtils::ensureOnScreenControlTexture(project->getAssetsPath(), false);
        const std::string knobTexture = ProjectUtils::ensureOnScreenControlTexture(project->getAssetsPath(), true);
        auto addCircle = [&](Entity parent, const char* name, unsigned int size, const std::string& texture){
            Entity child = scene->createEntity();
            scene->addComponent<Transform>(child, {});
            scene->addComponent<RectTransformComponent>(child, {});
            scene->addComponent<UIComponent>(child, {});
            scene->addComponent<ImageComponent>(child, {});
            scene->addEntityChild(parent, child, false);
            RectTransformComponent& childLayout = scene->getComponent<RectTransformComponent>(child);
            childLayout.width = size;
            childLayout.height = size;
            childLayout.anchorPreset = AnchorPreset::CENTER;
            childLayout.usingAnchors = true;
            if (!texture.empty())
                scene->getComponent<UIComponent>(child).texture.setPath(texture);
            scene->setEntityName(child, name);
            registerChildEntity(child, parent);
            return child;
        };
        const Entity background = addCircle(entity, "Background", 160, baseTexture);
        const Entity handle = addCircle(background, "Handle", 64, knobTexture);

        OnScreenStickComponent& stick = scene->getComponent<OnScreenStickComponent>(entity);
        stick.background = background;
        stick.handle = handle;
        stick.mode = (type == EntityCreationType::ONSCREEN_STICK_FLOATING) ? OnScreenStickMode::FLOATING
                   : (type == EntityCreationType::ONSCREEN_STICK_DYNAMIC) ? OnScreenStickMode::DYNAMIC
                   : OnScreenStickMode::FIXED;

    }else if (type == EntityCreationType::ONSCREEN_BUTTON){

        // a round Button (its visuals follow the press of any finger) with the control
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ButtonComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});
        scene->addComponent<OnScreenButtonComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 120;
        layout.height = 120;
        layout.anchorPreset = AnchorPreset::BOTTOM_RIGHT;
        layout.usingAnchors = true;

        const std::string baseTexture = ProjectUtils::ensureOnScreenControlTexture(project->getAssetsPath(), false);
        if (!baseTexture.empty())
            scene->getComponent<UIComponent>(entity).texture.setPath(baseTexture);

        ButtonComponent& buttonComp = scene->getComponent<ButtonComponent>(entity);
        buttonComp.colorPressed = Vector4(0.6f, 0.6f, 0.6f, 1.0f);
        scene->getSystem<UISystem>()->createButtonObjects(entity, buttonComp);
        registerChildEntity(buttonComp.label, entity);
        scene->setEntityName(buttonComp.label, "Label");

        TextComponent& textComp = scene->getComponent<TextComponent>(buttonComp.label);
        textComp.text = "Fire";
        textComp.needUpdateText = true;
        scene->getComponent<UIComponent>(buttonComp.label).color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

    }else if (type == EntityCreationType::TOUCH_AREA){

        // the right half of the screen, transparent: drags there look around (Mouse X / Mouse Y)
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});
        scene->addComponent<TouchAreaComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 100;
        layout.height = 100;
        layout.usingAnchors = true;
        layout.anchorPreset = AnchorPreset::NONE;
        layout.anchorPointLeft = 0.5f;
        layout.anchorPointRight = 1.0f;
        layout.anchorPointTop = 0.0f;
        layout.anchorPointBottom = 1.0f;
        scene->getComponent<UIComponent>(entity).color = Vector4(1.0f, 1.0f, 1.0f, 0.0f);

    }else if (type == EntityCreationType::INPUT_FIELD){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<InputFieldComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 200;
        layout.height = 32;

        InputFieldComponent& inputFieldComp = scene->getComponent<InputFieldComponent>(entity);
        scene->getSystem<UISystem>()->createInputFieldObjects(entity, inputFieldComp);

        scene->setEntityName(inputFieldComp.text, "Text");
        scene->setEntityName(inputFieldComp.selection, "Selection");
        scene->setEntityName(inputFieldComp.cursor, "Cursor");

        registerChildEntity(inputFieldComp.text, entity);
        registerChildEntity(inputFieldComp.selection, entity);
        registerChildEntity(inputFieldComp.cursor, entity);

        TextComponent& textComp = scene->getComponent<TextComponent>(inputFieldComp.text);
        textComp.text = "";
        textComp.multiline = false;
        textComp.needUpdateText = true;

        inputFieldComp.placeholder = "Enter text...";
        inputFieldComp.needUpdateInputField = true;

    }else if (type == EntityCreationType::PANEL){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<PanelComponent>(entity, {});
        scene->addComponent<ImageComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 250;
        layout.height = 150;

        ImageComponent& imgComp = scene->getComponent<ImageComponent>(entity);
        imgComp.patchMarginTop = 32;

        PanelComponent& panelComp = scene->getComponent<PanelComponent>(entity);
        scene->getSystem<UISystem>()->createPanelObjects(entity, panelComp);

        scene->setEntityName(panelComp.headerimage, "HeaderImage");
        scene->setEntityName(panelComp.headercontainer, "HeaderContainer");
        scene->setEntityName(panelComp.headertext, "HeaderText");

        registerChildEntity(panelComp.headerimage, entity);
        registerChildEntity(panelComp.headercontainer, panelComp.headerimage);
        registerChildEntity(panelComp.headertext, panelComp.headercontainer);

        TextComponent& titleComp = scene->getComponent<TextComponent>(panelComp.headertext);
        titleComp.text = "Panel";
        titleComp.needUpdateText = true;

        panelComp.needUpdatePanel = true;

    }else if (type == EntityCreationType::POLYGON){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<UIComponent>(entity, {});
        scene->addComponent<PolygonComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 100;
        layout.height = 100;

        PolygonComponent& polygonComp = scene->getComponent<PolygonComponent>(entity);
        polygonComp.points.push_back({Vector3(0, 0, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(100, 0, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(0, 100, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(100, 100, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.needUpdatePolygon = true;

    }else if (type == EntityCreationType::CANVAS_GROUP){

        // a container stretched over its parent with a Canvas Group (Unity: add the children
        // to fade, disable or make click-through together)
        scene->addComponent<Transform>(entity, {});
        RectTransformComponent layout;
        layout.width = 100;
        layout.height = 100;
        layout.anchorPreset = AnchorPreset::FULL_LAYOUT;
        layout.usingAnchors = true;
        scene->addComponent<RectTransformComponent>(entity, layout);
        scene->addComponent<CanvasGroupComponent>(entity, {});
    }else if (type == EntityCreationType::SAFE_AREA){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<SafeAreaComponent>(entity, {});

        // stretched over its parent (the canvas at the root) until the component drives it
        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.usingAnchors = true;
        layout.anchorPreset = AnchorPreset::FULL_LAYOUT;
        layout.width = 100;
        layout.height = 100;

    }else if (type == EntityCreationType::LAYOUT_GROUP){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<LayoutGroupComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 100;
        layout.height = 100;

    }else if (type == EntityCreationType::SPRITE || type == EntityCreationType::SPRITE_BATCH){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<SpriteComponent>(entity, {});

        SpriteComponent& sprite = scene->getComponent<SpriteComponent>(entity);
        sprite.width = 100;
        sprite.height = 100;
        if (type == EntityCreationType::SPRITE_BATCH)
            scene->addComponent<SpriteBatchComponent>(entity, {});

    }else if (type == EntityCreationType::TILEMAP){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<TilemapComponent>(entity, {});

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        mesh.numSubmeshes = 1;

        TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
        tilemap.width = 100;
        tilemap.height = 100;

    }else if (type == EntityCreationType::DIRECTIONAL_LIGHT){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LightComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent);

        LightComponent& light = scene->getComponent<LightComponent>(entity);
        light.type = LightType::DIRECTIONAL;
        light.direction = Vector3(0.0f, -1.0f, 0.0f);
        light.intensity = 4.0f;

    }else if (type == EntityCreationType::POINT_LIGHT){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LightComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent);

        LightComponent& light = scene->getComponent<LightComponent>(entity);
        light.type = LightType::POINT;
        light.range = 10.0f;
        light.intensity = 30.0f;

    }else if (type == EntityCreationType::SPOT_LIGHT){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LightComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent);

        LightComponent& light = scene->getComponent<LightComponent>(entity);
        light.type = LightType::SPOT;
        light.direction = Vector3(0.0f, -1.0f, 0.0f);
        light.range = 10.0f;
        light.intensity = 30.0f;

    }else if (type == EntityCreationType::POINT_LIGHT_2D){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<Light2DComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::Light2DComponent);

        Light2DComponent& light = scene->getComponent<Light2DComponent>(entity);
        light.range = 200.0f;
        light.intensity = 1.0f;
        // the virtual height normal maps respond to; sprites without one are not affected
        light.height = 64.0f;

    }else if (type == EntityCreationType::SHADOW_CASTER_2D){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<ShadowCaster2DComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::ShadowCaster2DComponent);

        // a standalone occluder entity has no sibling mesh for AUTO_QUAD to derive
        // an outline from, so start as an editable polygon with a default square
        ShadowCaster2DComponent& occluder = scene->getComponent<ShadowCaster2DComponent>(entity);
        occluder.shape = ShadowCaster2DShape::POLYGON;
        occluder.points.push_back(Vector2(-50.0f, -50.0f));
        occluder.points.push_back(Vector2(50.0f, -50.0f));
        occluder.points.push_back(Vector2(50.0f, 50.0f));
        occluder.points.push_back(Vector2(-50.0f, 50.0f));

    }else if (type == EntityCreationType::JOINT2D){

        // a Transform so it can live under its body (it follows it, a prefab carries it); made
        // under a Rigidbody 2D, the joint is on that body (local anchors, Unity's default)
        scene->addComponent<Transform>(entity, {});
        Joint2DComponent joint;
        if (parent != NULL_ENTITY && scene->findComponent<Rigidbody2DComponent>(parent)){
            joint.bodyA = parent;
        }
        scene->addComponent<Joint2DComponent>(entity, joint);

    }else if (type == EntityCreationType::JOINT3D){

        // as 2D: a Transform so it can live under its body; made under a Rigidbody, the joint
        // is on that body (local anchors, Unity's default)
        scene->addComponent<Transform>(entity, {});
        Joint3DComponent joint = makeJoint3D(Joint3DType::FIXED);
        if (parent != NULL_ENTITY && scene->findComponent<RigidbodyComponent>(parent)){
            joint.bodyA = parent;
        }
        scene->addComponent<Joint3DComponent>(entity, joint);

    }else if (type == EntityCreationType::RIGIDBODY2D){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<Rigidbody2DComponent>(entity, {});

    }else if (type == EntityCreationType::RIGIDBODY){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RigidbodyComponent>(entity, {});

    }else if (type == EntityCreationType::SKYBOX){

        scene->addComponent<SkyboxComponent>(entity, {});

    }else if (type == EntityCreationType::FOG){

        scene->addComponent<FogComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::FogComponent);

    }else if (type == EntityCreationType::LOCAL_VOLUMETRIC_FOG){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LocalVolumetricFogComponent>(entity, {});

    }else if (type == EntityCreationType::VOLUMETRIC_CLOUDS){

        scene->addComponent<VolumetricCloudsComponent>(entity, {});

    }else if (type == EntityCreationType::REFLECTION_PROBE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<ReflectionProbeComponent>(entity, {});
        updateFlags |= Catalog::getComponentStructuralUpdateFlags(ComponentType::ReflectionProbeComponent);

    }else if (type == EntityCreationType::UI_DOCUMENT){

        // the document file is chosen in the Inspector (Source)
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<UIDocumentComponent>(entity, {});

    }else if (type == EntityCreationType::FLUID_SIMULATION){

        // A Sprite showing the fluid (output MATERIAL, the component's default): seen and
        // dragged in the Scene view, placed, sized and sorted like any sprite, in 2D and 3D
        // scenes alike (aspectRatio 0 follows the sprite's size). SCREEN (an overlay over the
        // whole main camera) and TEXTURE ("fluid|<entity>" links) are one Output change away.
        // 512 px in a 2D scene; 4 units, a quad standing in the world, in a 3D one.
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<SpriteComponent>(entity, {});
        scene->addComponent<FluidSimulationComponent>(entity, {});

        const unsigned int size = sceneProject->sceneType == SceneType::SCENE_3D ? 4 : 512;
        SpriteComponent& sprite = scene->getComponent<SpriteComponent>(entity);
        sprite.width = size;
        sprite.height = size;
        sprite.pivotPreset = PivotPreset::CENTER;

    }else if (type == EntityCreationType::VIDEO_PLAYER || type == EntityCreationType::VIDEO_BACKGROUND){

        // Unity's Video Player GameObject: the frames go into its render texture (UI Images and
        // materials show it through "video|<entity>"), or behind the main camera's scene
        scene->addComponent<Transform>(entity, {});
        VideoPlayerComponent player;
        if (type == EntityCreationType::VIDEO_BACKGROUND){
            player.renderMode = VideoRenderMode::CAMERA_FAR_PLANE;
            player.aspectRatio = VideoAspectRatio::FIT_OUTSIDE;
            player.isLooping = true;
        }
        scene->addComponent<VideoPlayerComponent>(entity, player);

    }else if (type == EntityCreationType::ANIMATION){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<AnimationComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::AUDIO_ACTION || type == EntityCreationType::SOUND_EVENT_ACTION){

        // no Transform: a 3D audio action plays at its target (S40 R17)
        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<AudioSourceComponent>(entity, AudioMixerDocument::newAudioSource());
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::MIXER_SNAPSHOT_ACTION){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<AudioSnapshotComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::SPRITE_ANIMATION){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<SpriteAnimationComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::POSITION_TWEEN){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<TweenComponent>(entity, {});
        scene->addComponent<PositionTweenComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::ROTATION_TWEEN){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<TweenComponent>(entity, {});
        scene->addComponent<RotationTweenComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::SCALE_TWEEN){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<TweenComponent>(entity, {});
        scene->addComponent<ScaleTweenComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::COLOR_TWEEN){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<TweenComponent>(entity, {});
        scene->addComponent<ColorTweenComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::ALPHA_TWEEN){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<TweenComponent>(entity, {});
        scene->addComponent<AlphaTweenComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::TRANSLATE_TRACKS){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<KeyframeTracksComponent>(entity, {});
        scene->addComponent<TranslateTracksComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::ROTATE_TRACKS){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<KeyframeTracksComponent>(entity, {});
        scene->addComponent<RotateTracksComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::SCALE_TRACKS){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<KeyframeTracksComponent>(entity, {});
        scene->addComponent<ScaleTracksComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::MORPH_TRACKS){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<KeyframeTracksComponent>(entity, {});
        scene->addComponent<MorphTracksComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::LATTICE_TRACKS){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<KeyframeTracksComponent>(entity, {});
        scene->addComponent<LatticeTracksComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::CAMERA){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<CameraComponent>(entity, {});

        CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
        camera.useTarget = false;

        if (sceneProject->sceneType == SceneType::SCENE_2D || sceneProject->sceneType == SceneType::SCENE_UI){
            camera.type = CameraType::CAMERA_ORTHO;

            float width = static_cast<float>(Engine::getCanvasWidth());
            float height = static_cast<float>(Engine::getCanvasHeight());
            if (width <= 0) width = 800;
            if (height <= 0) height = 600;

            camera.leftClip = 0;
            camera.rightClip = width;
            camera.bottomClip = 0;
            camera.topClip = height;
            camera.nearClip = DEFAULT_ORTHO_NEAR;
            camera.farClip = DEFAULT_ORTHO_FAR;
            camera.transparentSort = false;
            camera.depthTest = false;

            Transform& cameratransform = scene->getComponent<Transform>(entity);
            cameratransform.position = Vector3(0.0f, 0.0f, 1.0f);
        }else{
            camera.type = CameraType::CAMERA_PERSPECTIVE;
            camera.yfov = 0.75f;

            if (Engine::getCanvasWidth() != 0 && Engine::getCanvasHeight() != 0) {
                camera.aspect = (float) Engine::getCanvasWidth() / (float) Engine::getCanvasHeight();
            }else{
                camera.aspect = 1.0;
            }

            camera.nearClip = DEFAULT_PERSPECTIVE_NEAR;
            camera.farClip = DEFAULT_PERSPECTIVE_FAR;
        }

        if (sceneProject->mainCamera == NULL_ENTITY){
            sceneProject->mainCamera = entity;
            wasMainCamera = true;
        }

    }else if (type == EntityCreationType::AUDIO_SOURCE){

        scene->addComponent<AudioSourceComponent>(entity, AudioMixerDocument::newAudioSource());

    }else if (type == EntityCreationType::AUDIO_SOURCE_3D){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<AudioSourceComponent>(entity, AudioMixerDocument::newAudioSource());

    }else if (type == EntityCreationType::MODEL){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<ModelComponent>(entity, {});

    }else if (type == EntityCreationType::PARTICLE_SYSTEM){

        // it renders itself (an inline effect: Unity's default system)
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<ParticleSystemComponent>(entity, {});

    }else if (type == EntityCreationType::FORCE_FIELD){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<ForceFieldComponent>(entity, {});

    }else if (type == EntityCreationType::UI_PARTICLE_SYSTEM){

        // under a UI element it draws between its siblings, in UI units, clipped by the nearest
        // panel (it renders itself: one GameObject); a soft sparkle rising and falling (UI is y
        // down), at time scale 0 too: origin/main's legacy fields, through the old system's
        // converter (as create_particles makes its presets)
        scene->addComponent<Transform>(entity, {});
        LegacyParticleSystem legacy;
        legacy.localSpace = true;
        legacy.maxParticles = 100;
        legacy.rate = 25;
        legacy.lifeInitializer.minLife = 0.8f;
        legacy.lifeInitializer.maxLife = 1.6f;
        legacy.positionInitializer.shape = ParticleEmitterShape::Box;
        legacy.positionInitializer.minPosition = Vector3(-20.0f, -5.0f, 0.0f);
        legacy.positionInitializer.maxPosition = Vector3(20.0f, 5.0f, 0.0f);
        legacy.velocityInitializer.minVelocity = Vector3(-60.0f, -220.0f, 0.0f);
        legacy.velocityInitializer.maxVelocity = Vector3(60.0f, -120.0f, 0.0f);
        legacy.accelerationInitializer.minAcceleration = Vector3(0.0f, 240.0f, 0.0f);
        legacy.accelerationInitializer.maxAcceleration = Vector3(0.0f, 240.0f, 0.0f);
        legacy.sizeInitializer.minSize = 16.0f;
        legacy.sizeInitializer.maxSize = 28.0f;
        legacy.alphaModifier.fromTime = 0.0f;
        legacy.alphaModifier.toTime = 1.6f;
        legacy.alphaModifier.fromAlpha = 1.0f;
        legacy.alphaModifier.toAlpha = 0.0f;
        particles::LegacyTarget softTarget;
        // the soft round image (ui/particle_soft.png, made once per project)
        const std::string softTexture = ProjectUtils::ensureSoftParticleTexture(project);
        if (!softTexture.empty()) {
            softTarget.texture = project->normalizeToAssetsRelative(project->getProjectPath() / softTexture).generic_string();
            softTarget.textureGuid = project->makeAssetRef(softTarget.texture, AssetPathBase::Assets).guid.str();
        }

        ParticleSystemComponent uiParticles;
        uiParticles.inlineEffect = particles::LegacyParticleConverter::convert(legacy, softTarget);
        uiParticles.inlineEffect.renderer.untexturedShape = particles::UntexturedShape::Disc;
        uiParticles.useUnscaledTime = true;
        scene->addComponent<ParticleSystemComponent>(entity, uiParticles);

    }else if (type == EntityCreationType::POINTS){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<PointsComponent>(entity, {});

    }else if (type == EntityCreationType::LINE_RENDERER){

        // Unity's new Line: a 2-point world-space polyline with a width (lines and trails 6.8)
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LineRendererComponent>(entity, {});
        ProjectUtils::setupNewLineRenderer(scene->getComponent<LineRendererComponent>(entity), sceneProject->sceneType != SceneType::SCENE_3D);

    }else if (type == EntityCreationType::TRAIL_RENDERER){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<TrailRendererComponent>(entity, {});
        ProjectUtils::setupNewTrailRenderer(scene->getComponent<TrailRendererComponent>(entity), sceneProject->sceneType != SceneType::SCENE_3D);

    }else if (type == EntityCreationType::MESH_POLYGON){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<MeshPolygonComponent>(entity, {});

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLE_STRIP;

        MeshPolygonComponent& polygonComp = scene->getComponent<MeshPolygonComponent>(entity);
        polygonComp.points.push_back({Vector3(0, 0, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(100, 0, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(0, 100, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.points.push_back({Vector3(100, 100, 0), Vector4(1.0f, 1.0f, 1.0f, 1.0f)});
        polygonComp.needUpdatePolygon = true;

    }else if (type == EntityCreationType::TERRAIN){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<TerrainComponent>(entity, {});

    }else if (type >= EntityCreationType::SPLINE_EMPTY && type <= EntityCreationType::SPLINE_CIRCLE_2D){

        // the preset's Spline and what it builds (one definition with build_spline's presets)
        static const SplinePreset presets[] = {
            SplinePreset::Empty, SplinePreset::Empty, SplinePreset::Road, SplinePreset::Path, SplinePreset::Wall,
            SplinePreset::Fence, SplinePreset::River, SplinePreset::Lake, SplinePreset::Circle, SplinePreset::Square,
            SplinePreset::Edge2D, SplinePreset::Fill2D, SplinePreset::Circle2D};
        const SplinePreset preset = presets[static_cast<int>(type) - static_cast<int>(EntityCreationType::SPLINE_EMPTY)];
        const SplinePresetSetup setup = SplinePresets::make(project, preset, sceneProject->sceneType == SceneType::SCENE_2D);
        SplinePresets::addTo(scene, entity, setup);

    }else if (type == EntityCreationType::WATER){

        // a 20 x 20 grid fine enough for the waves, drawn with library:water: the engine's
        // ripples and foam noise (no image files), Gerstner waves, reflection, the underside,
        // shore foam and refraction, lit per pixel
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createGrid(mesh, 20.0f, 20.0f, 64);

        Material& material = mesh.submeshes[0].material;
        material.name = "Water";
        material.shader = Water::SHADER;
        material.shaderFeatures = {"BUILTIN_TEXTURES", "WAVES", "GERSTNER", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "REFRACTION"};
        material.lighting = MaterialLighting::PIXEL;
        ShaderUniforms::set(material.shaderParams, "waveHeight", Vector4(0.1f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "waveLength", Vector4(4.0f, 0.0f, 0.0f, 0.0f));

        // the view under its surface: a library:underwater pass, off (the engine switches it
        // on while the main camera is under a water, Water::updateUnderwater)
        addedUnderwaterPass = -1;
        std::vector<PostProcessPass> passes = scene->getPostProcessPasses();
        const bool hasUnderwater = std::any_of(passes.begin(), passes.end(), [](const PostProcessPass& pass){
            return Water::isUnderwaterShader(pass.shader);
        });
        if (!hasUnderwater){
            PostProcessPass pass;
            pass.shader = Water::UNDERWATER_SHADER;
            pass.enabled = false;
            passes.push_back(pass);
            addedUnderwaterPass = (int)passes.size() - 1;
            scene->setPostProcessPasses(passes);
        }

    }else if (type == EntityCreationType::LATTICE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LatticeComponent>(entity, {});

    }

    scene->setEntityName(entity, entityName);

    if (parent != NULL_ENTITY){
        scene->addEntityChild(parent, entity, false);
    }

    // Apply all property setters
    for (const auto& [componentType, properties] : propertySetters) {
        for (const auto& [propertyName, propertySetter] : properties) {
            propertySetter(entity);
        }
    }

    Catalog::updateEntity(scene, entity, updateFlags);

    sceneProject->entities.push_back(entity);
    for (const auto& child : childEntities){
        Catalog::updateEntity(scene, child.entity, updateFlags);
        sceneProject->entities.push_back(child.entity);
    }

    if (!quiet){
        project->setSelectedEntity(sceneId, entity);
    }

    sceneProject->isModified = true;

    if (addToPrefab){
        project->addEntityToPrefab(sceneId, entity, parent, false);
        for (const auto& child : childEntities){
            project->addEntityToPrefab(sceneId, child.entity, child.parent, false);
        }
    }

    if (!quiet){
        if (ImGui::GetCurrentContext()){
            ImGui::SetWindowFocus(("###Scene" + std::to_string(sceneId)).c_str());
        }

        editor::Out::info("Created GameObject '%s' in scene '%s'", entityName.c_str(), sceneProject->name.c_str());
    }

    return true;
}

void editor::CreateEntityCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (sceneProject){
        if (addToPrefab){
            for (const auto& child : childEntities){
                project->removeEntityFromPrefab(sceneId, child.entity, false);
            }
            project->removeEntityFromPrefab(sceneId, entity, false);
        }

        if (wasMainCamera && sceneProject->mainCamera == entity){
            sceneProject->mainCamera = NULL_ENTITY;
        }

        std::vector<Entity> entitiesToDestroy;
        entitiesToDestroy.reserve(childEntities.size() + 1);
        for (auto it = childEntities.rbegin(); it != childEntities.rend(); ++it) {
            entitiesToDestroy.push_back(it->entity);
        }
        entitiesToDestroy.push_back(entity);
        DeleteEntityCmd::destroyEntities(sceneProject->scene, entitiesToDestroy,
            sceneProject->entities, project, sceneId);

        // the underwater pass a Water added
        if (addedUnderwaterPass >= 0){
            std::vector<PostProcessPass> passes = sceneProject->scene->getPostProcessPasses();
            if (addedUnderwaterPass < (int)passes.size() &&
                    Water::isUnderwaterShader(passes[addedUnderwaterPass].shader)){
                passes.erase(passes.begin() + addedUnderwaterPass);
                sceneProject->scene->setPostProcessPasses(passes);
            }
            addedUnderwaterPass = -1;
        }

        sceneProject->isModified = wasModified;
    }
}

Entity editor::CreateEntityCmd::getEntity(){
    return entity;
}

void editor::CreateEntityCmd::setQuiet(bool quiet){
    this->quiet = quiet;
}
