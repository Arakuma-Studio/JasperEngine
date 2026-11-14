// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CookedScene.h"

#include "Log.h"
#include "Scene.h"
#include "io/Data.h"
#include "serialization/RuntimeFields.h"
#include "serialization/SceneSettingsCodec.h"
#include "component/CameraComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/PrefabComponent.h"
#include "fluid/FluidSimulator.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::cook;

namespace {

    uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint64_t le64(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
        return v;
    }

    void setError(std::string* error, const std::string& message) {
        if (error) *error = message;
    }

    bool readContentFile(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
        Data data;
        if (data.open(path.c_str()) != 0) {
            setError(error, "cannot open " + path);
            return false;
        }
        const unsigned char* bytes = data.getMemPtr();
        out.assign(bytes, bytes + data.length());
        return true;
    }

    // what a texture link id names (Factory::createCameraTextureLinks): "camera|<entity>",
    // "fluid|<entity>"
    Entity linkedEntity(const std::string& id, const char* prefix) {
        const size_t length = std::strlen(prefix);
        if (id.compare(0, length, prefix) != 0) return NULL_ENTITY;
        try {
            return static_cast<Entity>(std::stoul(id.substr(length)));
        } catch (...) {
            return NULL_ENTITY;
        }
    }

    // the render-to-texture and fluid links of the components of these entities, once every
    // camera exists (Factory::createCameraTextureLinks); `map` gives the runtime entity of a
    // link's (null: links name runtime entities already)
    void bindTextureLinks(Scene* scene, const std::vector<Entity>& entities, const std::function<Entity(Entity)>& map) {
        for (Entity entity : entities) {
            if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) continue;
            for (const ComponentTable& table : componentTables()) {
                void* component = const_cast<void*>(table.find(scene, entity));
                if (!component) continue;
                visitStructTextures(*table.body, component, [&](Texture& texture) {
                    const std::string& id = texture.getId();
                    if (Entity camera = linkedEntity(id, "camera|"); camera != NULL_ENTITY) {
                        const Entity target = map ? map(camera) : camera;
                        if (target != NULL_ENTITY && scene->isEntityCreated(target))
                            if (CameraComponent* cam = scene->findComponent<CameraComponent>(target))
                                texture.setFramebuffer(cam->framebuffer);
                    } else if (Entity fluid = linkedEntity(id, "fluid|"); fluid != NULL_ENTITY) {
                        const Entity target = map ? map(fluid) : fluid;
                        if (target != NULL_ENTITY && scene->isEntityCreated(target)) {
                            if (FluidSimulationComponent* sim = scene->findComponent<FluidSimulationComponent>(target)) {
                                if (!sim->simulator) sim->simulator = std::make_shared<FluidSimulator>();
                                texture.setFramebuffer(sim->simulator->getOutputFramebuffer());
                            }
                        }
                    }
                });
            }
        }
    }

    struct EntityState {
        std::string name;
        bool active = true;
        std::string tag;
        int layer = 0;
    };

    EntityState readState(Reader& r) {
        EntityState state;
        state.name = r.string();
        state.active = r.boolean();
        state.tag = r.string();
        state.layer = static_cast<int>(r.svarint());
        return state;
    }

    void applyState(Scene* scene, Entity entity, const EntityState& state) {
        scene->setEntityName(entity, state.name);
        if (!state.active) scene->setEntityActive(entity, false);
        if (state.tag != DEFAULT_ENTITY_TAG) scene->setEntityTag(entity, state.tag);
        if (state.layer != 0) scene->setEntityLayer(entity, state.layer);
    }

    // the component records of one entity; `onlyMissing`: a component the entity has is kept
    bool readComponents(Reader& r, Scene* scene, Entity entity, bool onlyMissing, std::string* error) {
        const uint64_t count = r.varint();
        LoadContext ctx{scene, scene, entity};
        for (uint64_t c = 0; c < count && !r.failed(); c++) {
            if (onlyMissing) {
                // peek the key: a component the root has already stays as it is
                Reader probe = r;
                const uint32_t keyHash = probe.u32();
                const ComponentTable* table = findComponentTable(keyHash);
                if (table && table->find(scene, entity)) {
                    r.u32();
                    r.record();
                    continue;
                }
            }
            if (!readComponent(r, ctx)) {
                setError(error, "a component of entity " + std::to_string(entity) + ": " + r.error());
                return false;
            }
        }
        return !r.failed();
    }

}

std::string cook::contentFormatVersion() {
    return std::to_string(FORMAT_MAJOR) + "." + std::to_string(FORMAT_MINOR);
}

namespace {

    bool parseFormatVersion(std::string_view text, unsigned long& major, unsigned long& minor) {
        const size_t dot = text.find('.');
        if (dot == std::string_view::npos) return false;
        auto number = [](std::string_view digits, unsigned long& out) {
            if (digits.empty() || digits.size() > 5) return false;
            out = 0;
            for (char c : digits) {
                if (c < '0' || c > '9') return false;
                out = out * 10 + static_cast<unsigned long>(c - '0');
            }
            return true;
        };
        return number(text.substr(0, dot), major) && number(text.substr(dot + 1), minor);
    }

}

bool cook::readsContentFormat(std::string_view reader, std::string_view content) {
    unsigned long readerMajor = 0, readerMinor = 0, contentMajor = 0, contentMinor = 0;
    if (!parseFormatVersion(reader, readerMajor, readerMinor) || !parseFormatVersion(content, contentMajor, contentMinor)) return false;
    return readerMajor == contentMajor && contentMinor <= readerMinor;
}

bool cook::playerReadsContentFormat(std::string_view content) {
    unsigned long major = 0, minor = 0;
    if (!parseFormatVersion(content, major, minor) || major != FORMAT_MAJOR) return false;
    if (minor > FORMAT_MINOR) {
        static std::mutex mutex;
        static std::set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (warned.insert(std::string(content)).second) {
            Log::warn("Cooked content: content %.*s was made by a newer editor; features newer than %s are skipped",
                      static_cast<int>(content.size()), content.data(), contentFormatVersion().c_str());
        }
    }
    return true;
}

bool CookedFile::parse(std::span<const uint8_t> bytes, const char expectedMagic[4], std::string& error) {
    if (bytes.size() < HEADER_BYTES) {
        error = "too small to be a cooked file";
        return false;
    }
    std::memcpy(header.magic, bytes.data(), 4);
    if (std::memcmp(header.magic, expectedMagic, 4) != 0) {
        error = std::string("not a cooked ") + (std::memcmp(expectedMagic, SCENE_MAGIC, 4) == 0 ? "scene (.jscn)" : "prefab (.jpfb)");
        return false;
    }
    header.major = le16(bytes.data() + 4);
    header.minor = le16(bytes.data() + 6);
    header.id = le32(bytes.data() + 8);
    header.entityCount = le32(bytes.data() + 12);
    header.flags = le32(bytes.data() + 16);
    header.stringsOffset = le64(bytes.data() + 20);
    if (!playerReadsContentFormat(std::to_string(header.major) + "." + std::to_string(header.minor))) {
        error = "content format " + std::to_string(header.major) + "." + std::to_string(header.minor) + "; this player reads " +
                contentFormatVersion() + " (build the game with the editor of this player's version)";
        return false;
    }
    if (header.stringsOffset < HEADER_BYTES || header.stringsOffset > bytes.size()) {
        error = "corrupt string table offset";
        return false;
    }
    static const std::vector<std::string> none;
    Reader strings(bytes.subspan(static_cast<size_t>(header.stringsOffset)), none, [](uint32_t) { return NULL_ENTITY; });
    const uint64_t count = strings.varint();
    if (count > bytes.size()) {
        error = "corrupt string table";
        return false;
    }
    this->strings.clear();
    this->strings.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count && !strings.failed(); i++) {
        const std::span<const uint8_t> text = strings.blob();
        this->strings.emplace_back(reinterpret_cast<const char*>(text.data()), text.size());
    }
    if (strings.failed()) {
        error = "corrupt string table: " + strings.error();
        return false;
    }
    body = bytes.subspan(HEADER_BYTES, static_cast<size_t>(header.stringsOffset) - HEADER_BYTES);
    return true;
}

std::vector<uint8_t> cook::assembleFile(const char magic[4], uint32_t id, uint32_t entityCount, const std::vector<uint8_t>& body,
                                        const std::vector<std::string>& strings) {
    std::vector<uint8_t> out;
    auto put16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (i * 8))); };
    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (i * 8))); };
    out.insert(out.end(), magic, magic + 4);
    put16(FORMAT_MAJOR);
    put16(FORMAT_MINOR);
    put32(id);
    put32(entityCount);
    put32(0);   // flags
    const uint64_t stringsOffset = HEADER_BYTES + body.size();
    put64(stringsOffset);
    out.insert(out.end(), body.begin(), body.end());
    StringTable unused;
    Writer table(unused, [](Entity) { return 0u; });
    table.varint(strings.size());
    for (const std::string& text : strings) table.blob(text.data(), text.size());
    out.insert(out.end(), table.data().begin(), table.data().end());
    return out;
}

bool CookedSceneLoader::loadScene(Scene* scene, const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readContentFile(path, bytes, error)) return false;
    std::string why;
    if (!loadScene(scene, bytes, &why)) {
        setError(error, path + ": " + why);
        return false;
    }
    return true;
}

bool CookedSceneLoader::loadScene(Scene* scene, std::span<const uint8_t> bytes, std::string* error) {
    CookedFile file;
    std::string why;
    if (!file.parse(bytes, SCENE_MAGIC, why)) {
        setError(error, why);
        return false;
    }
    // a scene's GameObjects keep their ids; a system entity (a default camera) gets a new one
    std::unordered_map<uint32_t, Entity> systemEntities;
    auto entityOf = [&systemEntities](uint32_t ref) -> Entity {
        auto it = systemEntities.find(ref);
        return it != systemEntities.end() ? it->second : static_cast<Entity>(ref);
    };
    Reader r(file.body, file.strings, entityOf);

    Reader settings = r.record();
    Reader entityList = r.record();
    Reader camera = r.record();
    if (r.failed()) {
        setError(error, "corrupt scene: " + r.error());
        return false;
    }

    // all at once, so nothing made later takes an id the scene uses (Factory::createScene)
    struct Record {
        uint32_t ref;
        uint8_t flags;
        Reader body;
    };
    std::vector<Record> records;
    const uint64_t count = entityList.varint();
    Entity lastEntity = NULL_ENTITY;
    for (uint64_t i = 0; i < count && !entityList.failed(); i++) {
        Reader record = entityList.record();
        const uint32_t ref = record.u32();
        const uint8_t flags = record.u8();
        records.push_back({ref, flags, record});
        if (flags & ENTITY_SYSTEM) {
            systemEntities[ref] = scene->createSystemEntity();
        } else {
            scene->recreateEntity(static_cast<Entity>(ref));
            lastEntity = std::max(lastEntity, static_cast<Entity>(ref));
        }
    }
    if (entityList.failed()) {
        setError(error, "corrupt entity list: " + entityList.error());
        return false;
    }
    if (lastEntity != NULL_ENTITY) scene->setLastEntity(lastEntity);

    std::vector<Entity> made;
    for (Record& record : records) {
        const Entity entity = entityOf(record.ref);
        made.push_back(entity);
        const EntityState state = readState(record.body);
        if (!(record.flags & ENTITY_SYSTEM)) applyState(scene, entity, state);
        if (!readComponents(record.body, scene, entity, false, error)) return false;
    }

    // format 1.6: the hierarchy, each child after its parent (a 1.5 scene ends with its resources)
    r.record();
    if (!r.atEnd()) {
        Reader hierarchy = r.record();
        const uint64_t links = hierarchy.varint();
        for (uint64_t i = 0; i < links && !hierarchy.failed(); i++) {
            const Entity child = hierarchy.entity();
            const Entity parent = hierarchy.entity();
            if (child != NULL_ENTITY && parent != NULL_ENTITY) scene->addEntityChild(parent, child, false);
        }
        if (hierarchy.failed() || r.failed()) {
            setError(error, "corrupt scene hierarchy: " + (hierarchy.failed() ? hierarchy.error() : r.error()));
            return false;
        }
    }

    bindTextureLinks(scene, made, nullptr);

    const Entity cameraEntity = camera.entity();
    if (cameraEntity != NULL_ENTITY) scene->setCamera(cameraEntity);

    readSceneSettings(settings, *scene);
    if (settings.failed()) {
        setError(error, "corrupt scene settings: " + settings.error());
        return false;
    }
    return true;
}

SceneResources CookedSceneLoader::sceneResources(const std::string& path) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!readContentFile(path, bytes, &error)) {
        Log::error("Scene resources: %s", error.c_str());
        return {};
    }
    return sceneResources(bytes);
}

SceneResources CookedSceneLoader::sceneResources(std::span<const uint8_t> bytes) {
    SceneResources resources;
    CookedFile file;
    std::string error;
    if (!file.parse(bytes, SCENE_MAGIC, error)) {
        Log::error("Scene resources: %s", error.c_str());
        return resources;
    }
    Reader r(file.body, file.strings, [](uint32_t ref) { return static_cast<Entity>(ref); });
    r.record();   // settings
    r.record();   // entities
    r.record();   // camera
    Reader list = r.record();
    read(list, resources.textures);
    read(list, resources.sounds);
    read(list, resources.models);
    read(list, resources.clips);
    if (list.failed()) Log::error("Scene resources: %s", list.error().c_str());
    return resources;
}

bool CookedSceneLoader::instantiatePrefab(Scene* scene, Entity root, const std::string& path, std::vector<Entity>* members, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readContentFile(path, bytes, error)) return false;
    std::string why;
    if (!instantiatePrefab(scene, root, bytes, members, &why)) {
        setError(error, path + ": " + why);
        return false;
    }
    return true;
}

bool CookedSceneLoader::instantiatePrefab(Scene* scene, Entity root, std::span<const uint8_t> bytes, std::vector<Entity>* members,
                                          std::string* error) {
    CookedFile file;
    std::string why;
    if (!file.parse(bytes, PREFAB_MAGIC, why)) {
        setError(error, why);
        return false;
    }
    // references: 1 the root, 2 + i slot i (Factory::createPrefab's e<i>)
    std::vector<Entity> slots;
    auto entityOf = [&](uint32_t ref) -> Entity {
        if (ref == 1) return root;
        if (ref >= 2 && ref - 2 < slots.size()) return slots[ref - 2];
        return NULL_ENTITY;
    };
    Reader r(file.body, file.strings, entityOf);
    Reader link = r.record();
    Reader rootRecord = r.record();
    Reader slotList = r.record();
    Reader components = r.record();
    Reader hierarchy = r.record();
    if (r.failed()) {
        setError(error, "corrupt prefab: " + r.error());
        return false;
    }

    const std::string prefabName = link.string();
    const std::string prefabPath = link.string();
    const std::string prefabGuid = link.string();
    const std::string rootName = link.string();

    // the root: PrefabManager hands a bare one, which takes everything; a scene instance root
    // carries its own components and gets only the ones it lacks
    const bool bareRoot = !scene->findComponent<PrefabComponent>(root);
    if (scene->getEntityName(root).empty()) scene->setEntityName(root, rootName);
    const bool hasRoot = rootRecord.boolean();
    EntityState rootState;
    if (hasRoot) rootState = readState(rootRecord);
    if (bareRoot) {
        PrefabComponent prefab;
        prefab.name = prefabName;
        prefab.path = prefabPath;
        if (!prefabGuid.empty()) prefab.guid = prefabGuid;
        scene->addComponent<PrefabComponent>(root, prefab);
        if (hasRoot) {
            scene->setEntityActive(root, rootState.active);
            scene->setEntityTag(root, rootState.tag);
            scene->setEntityLayer(root, rootState.layer);
        }
    }

    // every slot's GameObject, in member order (a removed one keeps its slot empty)
    const uint64_t slotCount = slotList.varint();
    for (uint64_t i = 0; i < slotCount && !slotList.failed(); i++) {
        const uint8_t flags = slotList.u8();
        if (!(flags & ENTITY_PRESENT)) {
            slots.push_back(NULL_ENTITY);
            continue;
        }
        const Entity entity = scene->createEntity();
        applyState(scene, entity, readState(slotList));
        slots.push_back(entity);
        // a nested prefab's root keeps its link
        if (slotList.boolean()) {
            PrefabComponent nested;
            nested.name = slotList.string();
            nested.path = slotList.string();
            const std::string guid = slotList.string();
            if (!guid.empty()) nested.guid = guid;
            scene->addComponent<PrefabComponent>(entity, nested);
        }
    }
    if (members) members->insert(members->end(), slots.begin(), slots.end());
    if (slotList.failed()) {
        setError(error, "corrupt prefab slots: " + slotList.error());
        return false;
    }

    // the root's components it lacks, then every slot's
    if (hasRoot && !readComponents(rootRecord, scene, root, true, error)) return false;
    const uint64_t withComponents = components.varint();
    for (uint64_t i = 0; i < withComponents && !components.failed(); i++) {
        Reader entityRecord = components.record();
        const Entity entity = entityRecord.entity();
        if (entity == NULL_ENTITY) continue;
        if (!readComponents(entityRecord, scene, entity, false, error)) return false;
    }
    if (!hasRoot && !scene->findComponent<Transform>(root)) scene->addComponent<Transform>(root, {});

    // the hierarchy, in Transform order (each child appended to its parent)
    const uint64_t links = hierarchy.varint();
    for (uint64_t i = 0; i < links && !hierarchy.failed(); i++) {
        const Entity child = hierarchy.entity();
        const Entity parent = hierarchy.entity();
        if (child != NULL_ENTITY) scene->addEntityChild(parent != NULL_ENTITY ? parent : root, child, false);
    }
    if (hierarchy.failed() || components.failed()) {
        setError(error, "corrupt prefab: " + (hierarchy.failed() ? hierarchy.error() : components.error()));
        return false;
    }

    std::vector<Entity> all = slots;
    all.push_back(root);
    bindTextureLinks(scene, all, nullptr);
    return true;
}
