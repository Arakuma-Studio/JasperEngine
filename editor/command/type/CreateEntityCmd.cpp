// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateEntityCmd.h"

#include "editor/Out.h"
#include "util/PrimitiveParameters.h"
#include "util/ProjectUtils.h"
#include "Stream.h"
#include "command/type/DeleteEntityCmd.h"
#include "object/environment/Water.h"
#include "component/FluidSimulationComponent.h"
#include "component/VideoPlayerComponent.h"

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

    }else if (type == EntityCreationType::LAYOUT_GROUP){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<RectTransformComponent>(entity, {});
        scene->addComponent<LayoutGroupComponent>(entity, {});

        RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        layout.width = 100;
        layout.height = 100;

    }else if (type == EntityCreationType::SPRITE){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<SpriteComponent>(entity, {});

        SpriteComponent& sprite = scene->getComponent<SpriteComponent>(entity);
        sprite.width = 100;
        sprite.height = 100;

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

        scene->addComponent<Joint2DComponent>(entity, {});

    }else if (type == EntityCreationType::JOINT3D){

        scene->addComponent<Joint3DComponent>(entity, {});

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

        scene->addComponent<AudioSourceComponent>(entity, {});

    }else if (type == EntityCreationType::AUDIO_SOURCE_3D){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<AudioSourceComponent>(entity, {});

    }else if (type == EntityCreationType::MODEL){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});
        scene->addComponent<ModelComponent>(entity, {});

    }else if (type == EntityCreationType::PARTICLE_SYSTEM){

        scene->addComponent<ActionComponent>(entity, {});
        scene->addComponent<ParticleSystemComponent>(entity, {});
        if (parent != NULL_ENTITY){
            scene->getComponent<ActionComponent>(entity).target = parent;
        }

    }else if (type == EntityCreationType::POINTS){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<PointsComponent>(entity, {});

    }else if (type == EntityCreationType::LINE_RENDERER){

        scene->addComponent<Transform>(entity, {});
        scene->addComponent<LineRendererComponent>(entity, {});

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

    }else if (type == EntityCreationType::WATER){

        // a 20 x 20 grid fine enough for the waves, drawn with library:water: the engine's
        // ripples and foam noise (no image files), waves, reflection, the underside, shore
        // foam and refraction, lit per pixel
        scene->addComponent<Transform>(entity, {});
        scene->addComponent<MeshComponent>(entity, {});

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        scene->getSystem<MeshSystem>()->createGrid(mesh, 20.0f, 20.0f, 64);

        Material& material = mesh.submeshes[0].material;
        material.name = "Water";
        material.shader = Water::SHADER;
        material.shaderFeatures = {"BUILTIN_TEXTURES", "WAVES", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "REFRACTION"};
        material.lighting = MaterialLighting::PIXEL;
        ShaderUniforms::set(material.shaderParams, "waveHeight", Vector4(0.1f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "waveLength", Vector4(4.0f, 0.0f, 0.0f, 0.0f));

        // the view under its surface: a library:underwater pass, off (the engine switches it
        // on while the main camera is under a water, Water::updateUnderwater)
        addedUnderwaterPass = -1;
        std::vector<PostProcessPass> passes = scene->getPostProcessPasses();
        const bool hasUnderwater = std::any_of(passes.begin(), passes.end(), [](const PostProcessPass& pass){
            return ShaderReference::getBase(pass.shader) == Water::UNDERWATER_SHADER;
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
                    ShaderReference::getBase(passes[addedUnderwaterPass].shader) == Water::UNDERWATER_SHADER){
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
