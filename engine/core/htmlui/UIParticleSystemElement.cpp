// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "UIParticleSystemElement.h"

#include "Scene.h"
#include "Log.h"
#include "object/ParticleSystem.h"
#include "component/ActionComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "math/Matrix4.h"
#include "memory/Arena.h"
#include "particles/ParticleRender.h"
#include "manager/ObjectManager.h"
#include "manager/PrefabManager.h"
#include "registry/EntityCloner.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/Color.h"

#include <RmlUi/Core/CallbackTexture.h>
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
#include <memory>
#include <unordered_set>
#include <utility>
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

namespace {

    // the records of one read (the main thread's: the HTML UI updates there)
    memory::Arena& packArena(){
        static memory::Arena arena{"HtmlUI.ParticleSystem"};
        return arena;
    }

    Matrix4 matrixOf(const float* m){
        Matrix4 out;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                out[c][r] = m[c * 4 + r];
        return out;
    }

    // the rect (UV, v down) of a sheet frame, as the particle shader picks it (particles_vs.glsl)
    Rect frameRect(const particles::ParticleProgram& program, float frame01){
        const particles::RenderParams& rp = program.render;
        const float frames = (float)std::max<uint32_t>(rp.frameCount, 1u);
        const float frame = std::floor(frame01 * frames);
        if (rp.sheet == particles::SheetKind::Sprites && rp.spriteCount > 0){
            const float* rects = reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(program.pool.data()) + rp.sprites);
            const uint32_t index = (uint32_t)std::clamp(frame, 0.0f, (float)rp.spriteCount - 1.0f);
            return Rect(rects[index * 4], rects[index * 4 + 1], rects[index * 4 + 2], rects[index * 4 + 3]);
        }
        if (rp.sheet != particles::SheetKind::None){
            const float tilesX = (float)std::max<uint32_t>(rp.tilesX, 1u);
            const float tilesY = (float)std::max<uint32_t>(rp.tilesY, 1u);
            const float column = std::fmod(frame, tilesX);
            const float row = std::floor(frame / tilesX);
            return Rect(column / tilesX, row / tilesY, 1.0f / tilesX, 1.0f / tilesY);
        }
        return Rect(0.0f, 0.0f, 1.0f, 1.0f);
    }

}

namespace {

    // an emitter's billboards, in the GameObject's local space
    void readEmitterQuads(Scene* scene, Entity entity, const particles::EmitterRuntime& runtime, std::vector<UIParticleQuad>& out){
        const particles::CompiledEffect& effect = *runtime.effect();
        const particles::ParticleProgram& program = effect.program;
        const particles::RendererModule& renderer = effect.document.renderer;
        if (runtime.count() == 0 || !renderer.enabled || renderer.renderMode == particles::RenderMode::None || runtime.isPrewarming())
            return;

        // the records the billboards draw (one view: nothing sorts, nothing thins)
        memory::Arena& arena = packArena();
        arena.reset();
        particles::RenderView view;
        const particles::PackResult packed = particles::ParticleRenderPrep::pack(runtime, view, arena);

        // simulation space -> the GameObject's local space (the element's)
        Matrix4 toLocal = matrixOf(runtime.simToWorld());
        if (Transform* transform = scene->findComponent<Transform>(entity))
            toLocal = transform->modelMatrix.inverse() * toLocal;
        const Vector4& tiling = renderer.textureTiling;

        out.reserve(packed.count);
        for (uint32_t i = 0; i < packed.count; i++){
            const particles::ParticleInstance& record = packed.instances[i];
            float width = particles::ParticleRenderPrep::fromHalf((uint16_t)(record.size & 0xffffu));
            float height = particles::ParticleRenderPrep::fromHalf((uint16_t)(record.size >> 16));
            if (!(std::fabs(width) > 0.0f) || !(std::fabs(height) > 0.0f))
                continue;
            UIParticleQuad quad;
            quad.position = toLocal * Vector3(record.position[0], record.position[1], record.position[2]);
            quad.size = std::fabs(width);
            quad.height = std::fabs(height);
            // the shader turns a billboard clockwise as seen; the quads turn counter-clockwise
            const float turns = (float)(record.rotationFrame & 0xffffu) / 65535.0f;
            quad.rotation = -turns * 6.28318530718f;
            // UBYTE4N, the rgb encoded with gamma 2.2 (the shader's linearisation)
            auto channel = [&](int shift){ return std::pow((float)((record.color >> shift) & 255u) / 255.0f, 2.2f); };
            quad.color = Vector4(channel(0), channel(8), channel(16), (float)(record.color >> 24) / 255.0f);
            // the frame, then the Renderer's tiling and offset; a negative size mirrors the texture
            const Rect frame = frameRect(program, (float)(record.rotationFrame >> 16) / 65535.0f);
            float u0 = frame.getX() * tiling.x + tiling.z, v0 = frame.getY() * tiling.y + tiling.w;
            float u1 = u0 + frame.getWidth() * tiling.x, v1 = v0 + frame.getHeight() * tiling.y;
            if (width < 0.0f) std::swap(u0, u1);
            if (height < 0.0f) std::swap(v0, v1);
            quad.textureRect = Rect(u0, v0, u1 - u0, v1 - v0);
            out.push_back(quad);
        }
    }

    void materialOfDocument(const particles::ParticleEffectDocument& document, UIParticleMaterial& out){
        const particles::RendererModule& renderer = document.renderer;
        out.texturePath = renderer.texture;
        out.additive = renderer.blend == particles::ParticleBlend::Additive;
        // the particle shader's untextured billboard: a soft disc (Unity's default), else a square;
        // a sheet without a texture draws its frames' squares
        const bool sheet = document.textureSheetAnimation.present && document.textureSheetAnimation.enabled;
        out.softDisc = renderer.texture.empty() && renderer.untexturedShape == particles::UntexturedShape::Disc && !sheet;
    }

    void readChildBatches(Scene* scene, Entity entity, const particles::EmitterRuntime& parent, std::vector<UIParticleBatch>& out, size_t& used){
        for (size_t c = 0; c < parent.childCount(); c++){
            const particles::EmitterRuntime* child = parent.child(c);
            if (!child || !child->effect())
                continue;
            if (used == out.size())
                out.emplace_back();
            UIParticleBatch& batch = out[used++];
            batch.quads.clear();
            materialOfDocument(child->effect()->document, batch.material);
            readEmitterQuads(scene, entity, *child, batch.quads);
            readChildBatches(scene, entity, *child, out, used);
        }
    }

}

bool UIParticleSource::read(Scene* scene, Entity emitter, std::vector<UIParticleQuad>& out){
    out.clear();
    if (!scene || emitter == NULL_ENTITY || !scene->findComponent<ParticleSystemComponent>(emitter))
        return false;
    std::shared_ptr<ParticleSubsystem> subsystem = scene->getSystem<ParticleSubsystem>();
    particles::ParticleSystemState* state = subsystem ? subsystem->getState(emitter) : nullptr;
    if (!state || !state->effect)
        return false;
    readEmitterQuads(scene, emitter, state->runtime, out);
    return true;
}

void UIParticleSource::readChildren(Scene* scene, Entity emitter, std::vector<UIParticleBatch>& out){
    size_t used = 0;
    if (scene && emitter != NULL_ENTITY && scene->findComponent<ParticleSystemComponent>(emitter)){
        std::shared_ptr<ParticleSubsystem> subsystem = scene->getSystem<ParticleSubsystem>();
        particles::ParticleSystemState* state = subsystem ? subsystem->getState(emitter) : nullptr;
        if (state && state->effect && state->runtime.childCount() > 0)
            readChildBatches(scene, emitter, state->runtime, out, used);
    }
    out.resize(used);
}

bool UIParticleSource::material(Scene* scene, Entity emitter, UIParticleMaterial& out){
    out = UIParticleMaterial();
    if (!scene || emitter == NULL_ENTITY)
        return false;
    const ParticleSystemComponent* component = scene->findComponent<ParticleSystemComponent>(emitter);
    if (!component)
        return false;
    const particles::ParticleEffectDocument* document = ParticleSubsystem::getDocument(*component);
    if (!document)
        return true;
    materialOfDocument(*document, out);
    return true;
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
    root = system = NULL_ENTITY;
    quads.clear();
    childBatches.clear();
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

    // the Particle System: on the root, else the first of its descendants (hierarchy order); it
    // renders itself (S20), so it is the emitter too
    const std::vector<Entity> hierarchy = EntityCloner::collectHierarchy(scene, root);
    for (Entity entity : hierarchy){
        if (scene->findComponent<ParticleSystemComponent>(entity)){
            system = entity;
            break;
        }
    }
    if (system == NULL_ENTITY){
        fail("the prefab has no Particle System");
        destroyEntity();
        return false;
    }
    // the element plays it: an older prefab's controller action (D1) starts nothing of its own
    for (Entity entity : hierarchy){
        if (ActionComponent* action = scene->findComponent<ActionComponent>(entity); action && action->target == system){
            action->state = ActionState::Stopped;
            action->startTrigger = false;
        }
    }

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
    applyAttributes();
    UIParticleSource::material(scene, system, material);
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
    ParticleSystemComponent* component = scene->findComponent<ParticleSystemComponent>(system);
    if (!component)
        return;
    // time="ui" (default): unscaled, a pause menu plays it; "game" follows Engine.timeScale, the
    // effect's own Unscaled Time included
    const bool unscaled = GetAttribute<Rml::String>("time", "ui") != "game";
    component->useUnscaledTime = unscaled;
    ParticleSystem handle(scene, system);
    particles::ParticleEffectDocument document = handle.getEffectDocument();
    bool changed = false;
    if (!unscaled && document.useUnscaledTime){
        document.useUnscaledTime = false;
        changed = true;
    }
    // loop= the effect's Looping (the prefab's own otherwise)
    if (HasAttribute("loop")){
        const bool loop = GetAttribute<Rml::String>("loop", "") != "false";
        if (document.looping != loop){
            document.looping = loop;
            changed = true;
        }
    }
    // the GameObject's own copy of the effect (an asset stays as it is), its live particles kept
    if (changed)
        handle.setEffectDocument(document);
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
    ParticleSystem(scene, system).play();
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
    // the live particles go, the system keeps its state (Unity's Clear: playing, stopped or paused)
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired())
        return;
    ParticleSystem(scene, system).clear(false);
    quads.clear();
    childBatches.clear();
}

bool UIParticleSystemElement::isPlaying() const{
    // playing: not paused, not stopped (a system that does not loop plays until its last particle dies)
    if (!scene || system == NULL_ENTITY || sceneLifetime.expired())
        return false;
    return ParticleSystem(scene, system).isPlaying();
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
        root = system = NULL_ENTITY;
    }
    if (sourceDirty){
        destroyEntity();
        if (instantiate() || !error.empty())
            sourceDirty = false;
    }
    if (!scene || system == NULL_ENTITY)
        return;

    UIParticleSource::read(scene, system, quads);
    UIParticleSource::readChildren(scene, system, childBatches);
    // a script may have given it another effect or texture
    UIParticleSource::material(scene, system, material);
    // drawn here, so seen: its culling never pauses it (none of its renderers draws in a camera)
    if (std::shared_ptr<ParticleSubsystem> subsystem = scene->getSystem<ParticleSubsystem>())
        subsystem->markVisible(system);

    // `complete`: a system that does not loop stopped (by itself: its last particle died)
    const bool running = isPlaying();
    if (wasRunning && !running && !completeSent){
        const ParticleSystemComponent* component = scene->findComponent<ParticleSystemComponent>(system);
        const particles::ParticleEffectDocument* document = component ? ParticleSubsystem::getDocument(*component) : nullptr;
        if (document && !document->looping){
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
    renderQuads(renderManager, material, quads, rootDraw);
    childDraws.resize(childBatches.size());
    for (size_t b = 0; b < childBatches.size(); b++)
        renderQuads(renderManager, childBatches[b].material, childBatches[b].quads, childDraws[b]);
}

void UIParticleSystemElement::renderQuads(Rml::RenderManager* renderManager, const UIParticleMaterial& quadMaterial, const std::vector<UIParticleQuad>& quadList, DrawState& state){
    if (quadList.empty())
        return;

    if (quadMaterial.texturePath != state.loadedTexture || quadMaterial.softDisc != state.loadedDisc){
        state.loadedTexture = quadMaterial.texturePath;
        state.loadedDisc = quadMaterial.softDisc;
        if (!quadMaterial.texturePath.empty()){
            state.texture = renderManager->LoadTexture(quadMaterial.texturePath);
        }else if (quadMaterial.softDisc){
            // the particle shader's untextured billboard: a white disc whose alpha falls off as
            // (1 - r^2)^2 (RenderSystem's "particle" default), premultiplied
            constexpr int size = 64;
            auto pixels = std::make_shared<std::vector<Rml::byte>>((size_t)size * size * 4);
            for (int y = 0; y < size; y++){
                for (int x = 0; x < size; x++){
                    const float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    const float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    const float r2 = std::min(1.0f, u * u + v * v);
                    const Rml::byte a = toByte((1.0f - r2) * (1.0f - r2));
                    Rml::byte* pixel = pixels->data() + ((size_t)y * size + x) * 4;
                    pixel[0] = pixel[1] = pixel[2] = pixel[3] = a;
                }
            }
            const Rml::Vector2i dimensions(size, size);
            state.discTexture = renderManager->MakeCallbackTexture([pixels, dimensions](const Rml::CallbackTextureInterface& textureInterface) -> bool {
                return textureInterface.GenerateTexture({pixels->data(), pixels->size()}, dimensions);
            });
            state.texture = state.discTexture;
        }else{
            state.texture = Rml::Texture();
        }
    }

    // the emitter at the content box's centre, one unit = scale px, y up
    const float scale = std::max(0.0f, (float)std::atof(GetAttribute<Rml::String>("scale", "1").c_str()));
    const Rml::Vector2f boxSize = GetBox().GetSize(Rml::BoxArea::Content);
    const Rml::Vector2f centre = boxSize * 0.5f;
    const float opacity = GetComputedValues().opacity();

    Rml::Mesh mesh;
    mesh.vertices.reserve(quadList.size() * 4);
    mesh.indices.reserve(quadList.size() * 6);
    for (const UIParticleQuad& quad : quadList){
        const float half = quad.size * scale * 0.5f;
        const float halfHeight = (quad.height < 0.0f ? quad.size : quad.height) * scale * 0.5f;
        if (!(half > 0.0f) || !(halfHeight > 0.0f))
            continue;
        const Rml::Vector2f at(centre.x + quad.position.x * scale, centre.y - quad.position.y * scale);
        // y down: a counter-clockwise turn of the world is clockwise on screen
        const float c = std::cos(-quad.rotation), s = std::sin(-quad.rotation);
        const Rml::Vector2f corners[4] = {{-half, -halfHeight}, {half, -halfHeight}, {half, halfHeight}, {-half, halfHeight}};

        const Vector3 srgb = Color::linearTosRGB(Vector3(quad.color.x, quad.color.y, quad.color.z));
        const float alpha = std::clamp(quad.color.w * opacity, 0.0f, 1.0f);
        // premultiplied; additive particles add their colour (alpha 0 in a premultiplied blend)
        const Rml::ColourbPremultiplied colour(toByte(srgb.x * alpha), toByte(srgb.y * alpha), toByte(srgb.z * alpha),
                                               quadMaterial.additive ? (Rml::byte)0 : toByte(alpha));
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

    state.geometry = renderManager->MakeGeometry(std::move(mesh));
    state.geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content), state.texture);
}
