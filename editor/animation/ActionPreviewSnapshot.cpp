// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ActionPreviewSnapshot.h"

#include "Catalog.h"
#include "Stream.h"
#include "animation/ClipRecordHooks.h"
#include "component/KeyframeTracksComponent.h"
#include "component/Transform.h"
#include "reflect/Reflect.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // A reflected value kept outside its component (constructed and copied through its TypeOps)
    class PreviewHeldValue{
    public:
        PreviewHeldValue(const reflect::TypeOps* ops, const void* from): ops(ops) {
            storage.reset(new std::max_align_t[(ops->size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1]);
            ops->construct(storage.get());
            ops->copy(storage.get(), from);
        }
        ~PreviewHeldValue(){
            ops->destroy(storage.get());
        }
        PreviewHeldValue(const PreviewHeldValue&) = delete;
        PreviewHeldValue& operator=(const PreviewHeldValue&) = delete;

        bool copyTo(const reflect::FieldRef& ref) const{
            if (ref.ops != ops) return false;
            ops->copy(ref.ref, storage.get());
            return true;
        }
    private:
        const reflect::TypeOps* ops;
        std::unique_ptr<std::max_align_t[]> storage;
    };

    // A path's value can be held and copied back: a value field with its own ops
    bool previewCopyable(const reflect::FieldRef& ref){
        return ref && ref.ops && ref.ops->construct && ref.ops->copy && ref.ops->destroy && ref.ops->size > 0;
    }

    // The node a property path names in a component's YAML ("submeshes[0].material.baseColorFactor"):
    // false when the YAML has no value there (the field is at its default, or the path is not a
    // plain field path). `field` and `owner` are the last segment's.
    bool previewYamlAt(const reflect::ComponentDesc& desc, const YAML::Node& component, std::string_view path,
                YAML::Node& out, const reflect::FieldDesc*& field, const reflect::StructDesc*& owner, bool& absent){
        absent = false;
        field = nullptr;
        owner = &desc.body;
        YAML::Node node = component;
        const reflect::StructDesc* structDesc = &desc.body;
        size_t i = 0;
        while (i < path.size()){
            if (path[i] == '.'){
                i++;
                continue;
            }
            if (path[i] == '['){
                const size_t close = path.find(']', i);
                if (close == std::string_view::npos || !node || !node.IsSequence()) return false;
                const long index = std::strtol(std::string(path.substr(i + 1, close - i - 1)).c_str(), nullptr, 10);
                if (index < 0 || (size_t)index >= node.size()){
                    absent = true;
                    return false;
                }
                // (reset rebinds: assigning a yaml-cpp node would write into the tree it refers to)
                node.reset(node[(size_t)index]);
                i = close + 1;
                continue;
            }
            size_t end = path.find_first_of(".[", i);
            if (end == std::string_view::npos) end = path.size();
            if (!structDesc) return false;
            const reflect::FieldDesc* segment = reflect::findSegment(*structDesc, path.substr(i, end - i));
            if (!segment || !node || !node.IsMap()) return false;
            owner = structDesc;
            field = segment;
            const char* key = segment->yamlKey ? segment->yamlKey : segment->name;
            if (!node[key]){
                absent = true;
                return false;
            }
            node.reset(node[key]);
            structDesc = segment->structDesc;
            i = end;
        }
        out.reset(node);
        return field != nullptr;
    }

}

struct ActionPreviewSnapshot::Data{
    struct PathValue{
        std::string path;
        std::shared_ptr<PreviewHeldValue> value;
    };
    struct Component{
        ComponentType type;
        std::string key;                    // the component's YAML key
        YAML::Node base;                    // its authored YAML (taken, or the last whole write)
        std::vector<PathValue> paths;       // values commands wrote since, in order
    };
    struct EntityState{
        Entity entity = NULL_ENTITY;
        Entity parent = NULL_ENTITY;
        std::vector<Component> components;
        std::vector<ComponentType> excluded;
        // a keyframe track's playhead (saved fields the preview moves) when its key data is
        // left out: the keys authored meanwhile stay, the playhead goes back
        bool keyframePlayhead = false;
        int keyframeIndex = 0;
        float keyframeInterpolation = 0;
    };

    uint32_t sceneId = 0;
    Scene* scene = nullptr;
    std::vector<EntityState> states;
    std::vector<Entity> entities;
    ClipRecordHooks::Id beforeId = 0;
    ClipRecordHooks::Id afterId = 0;
    bool changed = false;

    ~Data(){
        stopListening();
    }

    void stopListening(){
        ClipRecordHooks::removeChannelWriteListener(beforeId);
        ClipRecordHooks::removeChannelWriteListener(afterId);
        beforeId = 0;
        afterId = 0;
    }

    Component* find(uint32_t writtenScene, Entity entity, ComponentType type){
        if (writtenScene != sceneId || !scene || !scene->isEntityCreated(entity)) return nullptr;
        for (EntityState& state : states){
            if (state.entity != entity) continue;
            for (Component& component : state.components){
                if (component.type == type) return &component;
            }
        }
        return nullptr;
    }

    EntityState* stateOf(Entity entity){
        for (EntityState& state : states){
            if (state.entity == entity) return &state;
        }
        return nullptr;
    }

    // The component back to its authored state: its YAML decoded, then the paths written since
    void restoreComponent(Entity entity, Entity parent, const Component& component) const{
        YAML::Node node(YAML::NodeType::Map);
        node[component.key] = component.base;
        Stream::decodeComponents(entity, parent, scene, node);
        applyPaths(entity, component);
    }

    void applyPaths(Entity entity, const Component& component) const{
        if (component.paths.empty()) return;
        const reflect::ComponentDesc* desc = reflect::find(component.type);
        void* live = desc ? desc->find(scene, entity) : nullptr;
        if (!live) return;
        uint64_t updateFlags = 0;
        for (const PathValue& written : component.paths){
            const reflect::FieldRef ref = reflect::resolve(*desc, live, written.path);
            if (ref && written.value->copyTo(ref)) updateFlags |= ref.field->updateFlags;
        }
        if (updateFlags) Catalog::updateEntity(scene, entity, updateFlags);
    }

    // The path's authored value into the live component (the value written before, else the
    // authored YAML's, else the field's default); false when it cannot (the caller then treats
    // the write as a whole-component one)
    bool restorePath(Entity entity, const Component& component, const std::string& path){
        const reflect::ComponentDesc* desc = reflect::find(component.type);
        void* live = desc ? desc->find(scene, entity) : nullptr;
        if (!live) return false;
        const reflect::FieldRef ref = reflect::resolve(*desc, live, path);
        if (!previewCopyable(ref)) return false;
        for (auto it = component.paths.rbegin(); it != component.paths.rend(); ++it){
            if (it->path == path){
                if (!it->value->copyTo(ref)) return false;
                if (ref.field->updateFlags) Catalog::updateEntity(scene, entity, ref.field->updateFlags);
                return true;
            }
        }
        YAML::Node node;
        const reflect::FieldDesc* field = nullptr;
        const reflect::StructDesc* owner = nullptr;
        bool absent = false;
        if (previewYamlAt(*desc, component.base, path, node, field, owner, absent)){
            if (field != ref.field || ref.ops->encodeKey || !ref.ops->fromYaml) return false;
            reflect::DecodeContext ctx;
            ctx.registry = scene;
            ctx.entity = entity;
            std::string reason;
            if (!ref.ops->fromYaml(*field, node, ref.ref, ctx, reason)) return false;
        }else if (absent && field == ref.field && owner && !ref.element){
            // an omitted key is the default value
            const void* defaults = reflect::defaultValue(*owner, *field);
            if (!defaults) return false;
            ref.ops->copy(ref.ref, defaults);
        }else{
            return false;
        }
        if (ref.field->updateFlags) Catalog::updateEntity(scene, entity, ref.field->updateFlags);
        return true;
    }

    YAML::Node encodeLive(Entity entity, const Component& component) const{
        Signature signature;
        signature.set(reflect::find(component.type)->componentId(scene));
        const YAML::Node encoded = Stream::encodeComponents(entity, scene, signature);
        return encoded[component.key] ? YAML::Node(encoded[component.key]) : YAML::Node(YAML::NodeType::Map);
    }

    void beforeWrite(uint32_t writtenScene, Entity entity, ComponentType type, const std::string& property){
        Component* component = find(writtenScene, entity, type);
        if (!component) return;
        const EntityState* state = stateOf(entity);
        // a path write puts back that value only: the preview's runtime state elsewhere in the
        // component (an action's state and time) stays
        if (!property.empty() && restorePath(entity, *component, property)) return;
        restoreComponent(entity, state->parent, *component);
    }

    void afterWrite(uint32_t writtenScene, Entity entity, ComponentType type, const std::string& property){
        Component* component = find(writtenScene, entity, type);
        if (!component) return;
        changed = true;
        if (!property.empty()){
            const reflect::ComponentDesc* desc = reflect::find(type);
            void* live = desc ? desc->find(scene, entity) : nullptr;
            const reflect::FieldRef ref = live ? reflect::resolve(*desc, live, property) : reflect::FieldRef();
            if (previewCopyable(ref)){
                component->paths.erase(std::remove_if(component->paths.begin(), component->paths.end(),
                                                      [&property](const PathValue& value) { return value.path == property; }),
                                       component->paths.end());
                component->paths.push_back({property, std::make_shared<PreviewHeldValue>(ref.ops, ref.ref)});
                return;
            }
        }
        // the whole component (beforeWrite put it back to its authored state, so it holds the
        // authored values and what the command wrote, no previewed one)
        component->base.reset(encodeLive(entity, *component));
        component->paths.clear();
    }
};

ActionPreviewSnapshot::ActionPreviewSnapshot(): data(std::make_unique<Data>()) {
}

const std::vector<ComponentType>& ActionPreviewSnapshot::readOnlyTypes(){
    static const std::vector<ComponentType> types = {
        ComponentType::AnimationComponent, ComponentType::KeyframeTracksComponent, ComponentType::TranslateTracksComponent,
        ComponentType::RotateTracksComponent, ComponentType::ScaleTracksComponent, ComponentType::MorphTracksComponent,
        ComponentType::LatticeTracksComponent};
    return types;
}

ActionPreviewSnapshot::~ActionPreviewSnapshot() = default;

void ActionPreviewSnapshot::add(uint32_t sceneId, Scene* scene, Entity entity, const std::vector<ComponentType>& excluded){
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return;
    if (data->scene && (data->scene != scene || data->sceneId != sceneId)) clear();
    if (contains(entity)) return;
    data->scene = scene;
    data->sceneId = sceneId;

    Data::EntityState state;
    state.entity = entity;
    state.excluded = excluded;
    if (const Transform* transform = scene->findComponent<Transform>(entity)) {
        state.parent = transform->parent;
    }
    if (const KeyframeTracksComponent* keyframe = scene->findComponent<KeyframeTracksComponent>(entity)) {
        if (std::find(excluded.begin(), excluded.end(), ComponentType::KeyframeTracksComponent) != excluded.end()) {
            state.keyframePlayhead = true;
            state.keyframeIndex = keyframe->index;
            state.keyframeInterpolation = keyframe->interpolation;
        }
    }
    const YAML::Node encoded = Stream::encodeComponents(entity, scene, scene->getSignature(entity));
    for (const reflect::ComponentDesc* desc : reflect::serializationOrder()){
        if (std::find(excluded.begin(), excluded.end(), desc->type) != excluded.end()) continue;
        const std::string key = Catalog::getComponentName(desc->type, true);
        if (!encoded[key]) continue;
        Data::Component component;
        component.type = desc->type;
        component.key = key;
        component.base.reset(YAML::Clone(encoded[key]));
        state.components.push_back(std::move(component));
    }
    data->states.push_back(std::move(state));
    data->entities.push_back(entity);

    if (data->beforeId == 0){
        Data* self = data.get();
        data->beforeId = ClipRecordHooks::addBeforeComponentWrite(
            [self](uint32_t writtenScene, Entity written, ComponentType type, const std::string& property){
                self->beforeWrite(writtenScene, written, type, property);
            });
        data->afterId = ClipRecordHooks::addAfterComponentWrite(
            [self](uint32_t writtenScene, Entity written, ComponentType type, const std::string& property){
                self->afterWrite(writtenScene, written, type, property);
            });
    }
}

bool ActionPreviewSnapshot::contains(Entity entity) const{
    return std::find(data->entities.begin(), data->entities.end(), entity) != data->entities.end();
}

bool ActionPreviewSnapshot::empty() const{
    return data->states.empty();
}

const std::vector<Entity>& ActionPreviewSnapshot::entities() const{
    return data->entities;
}

void ActionPreviewSnapshot::restore(Scene* scene) const{
    if (!scene || scene != data->scene) return;
    for (const Data::EntityState& state : data->states){
        if (state.entity == NULL_ENTITY || !scene->isEntityCreated(state.entity)) continue;

        YAML::Node components(YAML::NodeType::Map);
        for (const Data::Component& component : state.components){
            components[component.key] = component.base;
        }
        Stream::decodeComponents(state.entity, state.parent, scene, components);
        for (const Data::Component& component : state.components){
            data->applyPaths(state.entity, component);
        }
        ProjectUtils::clearPreviewParticles(scene, state.entity);

        // a relative track takes its base again from the restored pose when the preview starts it
        if (KeyframeTracksComponent* keyframe = scene->findComponent<KeyframeTracksComponent>(state.entity)) {
            keyframe->hasBase = false;
            if (state.keyframePlayhead) {
                keyframe->index = state.keyframeIndex;
                keyframe->interpolation = state.keyframeInterpolation;
            }
        }
    }
}

void ActionPreviewSnapshot::clear(){
    data->stopListening();
    data->states.clear();
    data->entities.clear();
    data->scene = nullptr;
    data->sceneId = 0;
    data->changed = false;
}

bool ActionPreviewSnapshot::takeChanged(){
    const bool was = data->changed;
    data->changed = false;
    return was;
}
