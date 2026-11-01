// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "UIParticleSystemElement.h"

#include "Scene.h"
#include "Log.h"
#include "object/ParticleSystem.h"
#include "component/ActionComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/Transform.h"
#include "manager/ObjectManager.h"
#include "manager/PrefabManager.h"
#include "registry/EntityCloner.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/Color.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

namespace {

    std::unique_ptr<Rml::ElementInstancerGeneric<UIParticleSystemElement>>& particleInstancer(){
        static std::unique_ptr<Rml::ElementInstancerGeneric<UIParticleSystemElement>> value;
        return value;
    }

    // every element alive (releaseDetached)
    std::unordered_set<UIParticleSystemElement*>& liveElements(){
        static std::unordered_set<UIParticleSystemElement*> elements;
        return elements;
    }

    Rml::byte toByte(float value){
        return (Rml::byte)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    }

}

// ---------------------------------------------------------------- the particle source seam

bool UIParticleSource::read(Scene* scene, Entity emitter, std::vector<UIParticleQuad>& out){
    out.clear();
    if (!scene || emitter == NULL_ENTITY)
        return false;

    if (PointsComponent* points = scene->findComponent<PointsComponent>(emitter)){
        out.reserve(points->points.size());
        for (const PointData& point : points->points){
            if (!point.visible)
                continue;
            UIParticleQuad quad;
            quad.position = point.position;
            quad.size = point.size;
            quad.rotation = point.rotation;
            quad.color = point.color;
            quad.textureRect = point.textureRect;
            out.push_back(quad);
        }
        return true;
    }

    // a quad InstancedMesh target (the particles feature's default): position, uniform scale
    // as the size, the colour and sprite frame of each visible instance. Its roll joins here
    // when InstanceData gets one (the particles feature).
    if (InstancedMeshComponent* instances = scene->findComponent<InstancedMeshComponent>(emitter)){
        out.reserve(instances->instances.size());
        for (const InstanceData& instance : instances->instances){
            if (!instance.visible)
                continue;
            UIParticleQuad quad;
            quad.position = instance.position;
            quad.size = std::max(std::fabs(instance.scale.x), std::fabs(instance.scale.y));
            quad.rotation = 0.0f;
            quad.color = instance.color;
            quad.textureRect = instance.textureRect;
            out.push_back(quad);
        }
        return true;
    }
    return false;
}

bool UIParticleSource::material(Scene* scene, Entity emitter, std::string& texturePath, bool& additive){
    texturePath.clear();
    additive = false;
    if (!scene || emitter == NULL_ENTITY)
        return false;
    if (PointsComponent* points = scene->findComponent<PointsComponent>(emitter)){
        texturePath = points->texture.getPath();
        additive = points->blendMode == MaterialBlendMode::ADDITIVE;
        return true;
    }
    if (scene->findComponent<InstancedMeshComponent>(emitter)){
        // the quad mesh's material (the particles feature's built-in soft circle by default)
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(emitter)){
            if (mesh->numSubmeshes > 0){
                texturePath = mesh->submeshes[0].material.baseColorTexture.getPath();
                additive = mesh->submeshes[0].material.blendMode == MaterialBlendMode::ADDITIVE;
            }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the element

RMLUI_RTTI_Define(UIParticleSystemElement)

UIParticleSystemElement::UIParticleSystemElement(const Rml::String& tag): Rml::Element(tag){
    liveElements().insert(this);
}

UIParticleSystemElement::~UIParticleSystemElement(){
    liveElements().erase(this);
    destroyEntity();
}

void UIParticleSystemElement::releaseDetached(Scene* scene){
    std::vector<UIParticleSystemElement*> detached;
    for (UIParticleSystemElement* element : liveElements()){
        if (element->scene == scene && element->root != NULL_ENTITY && !element->GetContext())
            detached.push_back(element);
    }
    for (UIParticleSystemElement* element : detached){
        if (!liveElements().count(element))
            continue;
        element->destroyEntity();
        element->sourceDirty = true;
    }
}

void UIParticleSystemElement::registerInstancer(){
    if (!particleInstancer())
        particleInstancer() = std::make_unique<Rml::ElementInstancerGeneric<UIParticleSystemElement>>();
    Rml::Factory::RegisterElementInstancer("particle-system", particleInstancer().get());
}

void UIParticleSystemElement::releaseInstancer(){
    particleInstancer().reset();
}

bool UIParticleSystemElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio){
    // 0 x 0 unless CSS sizes it: the particles fly out of it
    dimensions = Rml::Vector2f(0.0f, 0.0f);
    ratio = 0.0f;
    return true;
}

void UIParticleSystemElement::fail(const std::string& message){
    error = message;
    // HTMLUI-HTML-0033 (particle-system-source; 0029 upstream): logged and in the document's diagnostics
    const std::string source = GetAttribute<Rml::String>("src", "");
    Log::warn("[HTML UI] HTMLUI-HTML-0029 <particle-system src=\"%s\">: %s", source.c_str(), message.c_str());
    if (Rml::ElementDocument* document = GetOwnerDocument()){
        htmlui::compat::Diagnostic diagnostic;
        diagnostic.code = htmlui::compat::codes::ParticleSystemSource;
        diagnostic.message = "<particle-system src=\"" + source + "\">: " + message;
        diagnostic.subject = "particle-system";
        htmlui::compat::DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
    }
}

void UIParticleSystemElement::destroyEntity(){
    if (scene && !sceneLifetime.expired() && root != NULL_ENTITY && scene->isEntityCreated(root)){
        if (UIDocumentSystem* documents = scene->findSystem<UIDocumentSystem>().get())
            documents->removeRuntimeEntity(root);
        // the prefab's whole hierarchy (its children and the actions on them) goes
        ObjectManager::destroyImmediate(scene, root);
    }
    root = system = emitter = NULL_ENTITY;
    quads.clear();
    drawn = 0;
}

bool UIParticleSystemElement::instantiate(){
    error.clear();
    scene = UIDocumentSystem::sceneOfContext(GetContext());
    if (!scene)
        return false;
    sceneLifetime = scene->getLifetime();

    const std::string source = GetAttribute<Rml::String>("src", "");
    if (source.empty()){
        fail("no src: name a prefab with a Particle System (src=\"fx/confetti.prefab\")");
        return false;
    }
    const uint32_t id = PrefabManager::findPrefab(source);
    if (id == 0){
        fail("no prefab of that name in the build (prefabs load in Play and in exported games)");
        return false;
    }
    root = PrefabManager::instantiate(id, scene);
    if (root == NULL_ENTITY){
        fail("the prefab could not be made");
        return false;
    }
    if (UIDocumentSystem* documents = scene->findSystem<UIDocumentSystem>().get())
        documents->addRuntimeEntity(root);

    // the Particle System: on the root, else the first of its descendants (hierarchy order)
    const std::vector<Entity> hierarchy = EntityCloner::collectHierarchy(scene, root);
    for (Entity entity : hierarchy){
        if (scene->findComponent<ParticleSystemComponent>(entity) && scene->findComponent<ActionComponent>(entity)){
            system = entity;
            break;
        }
    }
    if (system == NULL_ENTITY){
        fail("the prefab has no Particle System");
        destroyEntity();
        return false;
    }
    ActionComponent& action = scene->getComponent<ActionComponent>(system);
    emitter = (action.target != NULL_ENTITY) ? action.target : system;

    // simulated where the prefab is made, drawn only here: none of its renderers draws
    if (Transform* transform = scene->findComponent<Transform>(root)){
        transform->position = Vector3(0.0f, 0.0f, 0.0f);
        transform->needUpdate = true;
    }
    for (Entity entity : hierarchy){
        if (Transform* transform = scene->findComponent<Transform>(entity)){
            transform->visible = false;
            transform->needUpdate = true;
        }
    }
    UIParticleSource::material(scene, emitter, texturePath, additive);

    applyAttributes();
    // a play / pause / stop asked before the system existed wins over autoplay
    const Command command = pending;
    pending = Command::None;
    if (command == Command::Play || (command == Command::None && HasAttribute("autoplay"))){
        play();
    }else{
        stop();
        if (command == Command::Pause)
            pause();
    }
    return true;
}

void UIParticleSystemElement::applyAttributes(){
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired())
        return;
    ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(system);
    if (!particles)
        return;
    // SYNC4-ABSORB(particles): time="ui" (unscaled: a pause menu plays it) and loop= on the
    // legacy particle system; S20's main module takes them with the particles absorption
    (void)particles;
}

void UIParticleSystemElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (changed_attributes.count("src"))
        sourceDirty = true;
    if (changed_attributes.count("time") || changed_attributes.count("loop"))
        applyAttributes();
}

void UIParticleSystemElement::play(){
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired()){
        // queued until the prefab is made (document scripts run before the first update)
        pending = Command::Play;
        return;
    }
    ParticleSystem(scene, system).start();
    completeSent = false;
}

void UIParticleSystemElement::stop(){
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired()){
        // queued until the prefab is made (document scripts run before the first update)
        pending = Command::Stop;
        return;
    }
    ParticleSystem(scene, system).stop();
}

void UIParticleSystemElement::pause(){
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired()){
        // queued until the prefab is made (document scripts run before the first update)
        pending = Command::Pause;
        return;
    }
    ParticleSystem(scene, system).pause();
}

void UIParticleSystemElement::clear(){
    // the live particles go, the system keeps its state
    if (!scene || emitter == NULL_ENTITY || sceneLifetime.expired())
        return;
    if (PointsComponent* points = scene->findComponent<PointsComponent>(emitter)){
        for (PointData& point : points->points)
            point.visible = false;
    }
    // SYNC4-ABSORB(particles): the legacy system's particles end here; S20's clear with the
    // particles absorption
    quads.clear();
}

bool UIParticleSystemElement::isPlaying() const{
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired())
        return false;
    const ActionComponent* action = scene->findComponent<ActionComponent>(system);
    return action && action->state == ActionState::Running;
}

Scene* UIParticleSystemElement::getScene() const{
    return sceneLifetime.expired() ? nullptr : scene;
}

Entity UIParticleSystemElement::getParticleEntity() const{
    return sceneLifetime.expired() ? NULL_ENTITY : system;
}

const std::string& UIParticleSystemElement::getError() const{
    return error;
}

size_t UIParticleSystemElement::getDrawnParticleCount() const{
    return drawn;
}

void UIParticleSystemElement::OnUpdate(){
    Rml::Element::OnUpdate();

    if (scene && sceneLifetime.expired()){
        // the scene went first: what was made went with it
        scene = nullptr;
        root = system = emitter = NULL_ENTITY;
    }
    if (sourceDirty){
        destroyEntity();
        if (instantiate() || !error.empty())
            sourceDirty = false;
    }
    if (!scene || system == NULL_ENTITY)
        return;

    UIParticleSource::read(scene, emitter, quads);

    // `complete`: a system that does not loop stopped by itself
    const bool running = isPlaying();
    if (wasRunning && !running && !completeSent){
        const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(system);
        // SYNC4-ABSORB(particles): a system that does not loop (S20's main module with the absorption)
        if (particles){
            completeSent = true;
            DispatchEvent("complete", Rml::Dictionary(), false, false);
        }
    }
    wasRunning = running;
}

void UIParticleSystemElement::OnRender(){
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager)
        return;
    drawn = 0;
    if (quads.empty())
        return;

    if (texturePath != loadedTexture){
        loadedTexture = texturePath;
        texture = texturePath.empty() ? Rml::Texture() : renderManager->LoadTexture(texturePath);
    }

    // the emitter at the content box's centre, one unit = scale px, y up
    const float scale = std::max(0.0f, (float)std::atof(GetAttribute<Rml::String>("scale", "1").c_str()));
    const Rml::Vector2f boxSize = GetBox().GetSize(Rml::BoxArea::Content);
    const Rml::Vector2f centre = boxSize * 0.5f;
    const float opacity = GetComputedValues().opacity();

    Rml::Mesh mesh;
    mesh.vertices.reserve(quads.size() * 4);
    mesh.indices.reserve(quads.size() * 6);
    for (const UIParticleQuad& quad : quads){
        const float half = quad.size * scale * 0.5f;
        if (!(half > 0.0f))
            continue;
        const Rml::Vector2f at(centre.x + quad.position.x * scale, centre.y - quad.position.y * scale);
        // y down: a counter-clockwise turn of the world is clockwise on screen
        const float c = std::cos(-quad.rotation), s = std::sin(-quad.rotation);
        const Rml::Vector2f corners[4] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};

        const Vector3 srgb = Color::linearTosRGB(Vector3(quad.color.x, quad.color.y, quad.color.z));
        const float alpha = std::clamp(quad.color.w * opacity, 0.0f, 1.0f);
        // premultiplied; additive particles add their colour (alpha 0 in a premultiplied blend)
        const Rml::ColourbPremultiplied colour(toByte(srgb.x * alpha), toByte(srgb.y * alpha), toByte(srgb.z * alpha),
                                               additive ? (Rml::byte)0 : toByte(alpha));
        const float u0 = quad.textureRect.getX(), v0 = quad.textureRect.getY();
        const float u1 = u0 + quad.textureRect.getWidth(), v1 = v0 + quad.textureRect.getHeight();
        const Rml::Vector2f uvs[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};

        const int first = (int)mesh.vertices.size();
        for (int i = 0; i < 4; i++){
            Rml::Vertex vertex;
            vertex.position = at + Rml::Vector2f(corners[i].x * c - corners[i].y * s, corners[i].x * s + corners[i].y * c);
            vertex.colour = colour;
            vertex.tex_coord = uvs[i];
            mesh.vertices.push_back(vertex);
        }
        for (int index : {0, 1, 2, 0, 2, 3})
            mesh.indices.push_back(first + index);
        drawn++;
    }
    if (mesh.indices.empty())
        return;

    geometry = renderManager->MakeGeometry(std::move(mesh));
    geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content), texture);
}
