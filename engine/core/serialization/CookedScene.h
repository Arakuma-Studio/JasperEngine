// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Cooked scenes (.jscn) and prefabs (.jpfb) v1 (docs/specs/S30-shipping.md D2): what a prebuilt
// player builds its scenes from, instead of the generated C++ of a Compile build.
//
//   header   magic "JSCN" / "JPFB", u16 major, u16 minor, u32 scene (prefab) id, u32 entity
//            count, u32 flags, u64 string table offset                                  (28 bytes)
//   body     a scene: the settings record, the entity records, the camera, the resources;
//            a prefab: its link, the root and slot records, the components, the hierarchy
//   strings  varint count, then each string as varint length and UTF-8 bytes
//
// An entity record: u32 length, entity reference, flags, name, active, tag, layer, component
// count, the component records (RuntimeFields.h). A scene's entities keep their editor ids (as
// a Compile build's do); a prefab's are numbered from 1 (its root) and made when instantiated.
// Readers refuse a newer major version; a newer minor adds only fields and records they skip.

#include "Export.h"
#include "ecs/Entity.h"
#include "manager/SceneManager.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    namespace cook {
        constexpr char SCENE_MAGIC[4] = {'J', 'S', 'C', 'N'};
        constexpr char PREFAB_MAGIC[4] = {'J', 'P', 'F', 'B'};
        constexpr uint16_t FORMAT_MAJOR = 1;
        // 1: the scene settings' capsule shadows (S57); 2: the LOD cross-fade's (S54); 3: the default shaders'
        // property values; 4: the planar reflections (water-reflections)
        constexpr uint16_t FORMAT_MINOR = 4;
        constexpr size_t HEADER_BYTES = 28;

        // entity record flags
        constexpr uint8_t ENTITY_SYSTEM = 1;     // made with createSystemEntity (a default camera)
        constexpr uint8_t ENTITY_PRESENT = 2;    // a prefab slot that holds a GameObject

        // "1.0": the content-manifest's and player-info's contentFormatVersion
        JASPERENGINE_API std::string contentFormatVersion();

        struct FileHeader {
            char magic[4] = {};
            uint16_t major = 0;
            uint16_t minor = 0;
            uint32_t id = 0;
            uint32_t entityCount = 0;
            uint32_t flags = 0;
            uint64_t stringsOffset = 0;
        };

        // A cooked file split into its parts; false (with a message naming what to do) when it
        // is not one, or of a newer major version
        struct CookedFile {
            FileHeader header;
            std::vector<std::string> strings;
            std::span<const uint8_t> body;
            bool parse(std::span<const uint8_t> bytes, const char expectedMagic[4], std::string& error);
        };

        // the bytes of a cooked file: header, body, string table
        JASPERENGINE_API std::vector<uint8_t> assembleFile(const char magic[4], uint32_t id, uint32_t entityCount,
                                                          const std::vector<uint8_t>& body, const std::vector<std::string>& strings);
    }

    class JASPERENGINE_API CookedSceneLoader {
    public:
        // Builds a scene from its cooked file (content://scenes/<id>.jscn), as the generated
        // create_<scene>() does: its entities with their ids, names, state and components,
        // the render-to-texture links, the camera and the scene settings. False with a message.
        static bool loadScene(Scene* scene, const std::string& path, std::string* error = nullptr);
        static bool loadScene(Scene* scene, std::span<const uint8_t> bytes, std::string* error = nullptr);

        // The files SceneManager::preloadScene() loads before the scene is built
        static SceneResources sceneResources(const std::string& path);
        static SceneResources sceneResources(std::span<const uint8_t> bytes);

        // A prefab's GameObjects under `root` (PrefabManager's factory; a bare root gets the
        // prefab's components, a scene instance's root only the ones it lacks). `members`
        // receives every slot's GameObject (NULL_ENTITY for an empty slot).
        static bool instantiatePrefab(Scene* scene, Entity root, const std::string& path, std::vector<Entity>* members = nullptr,
                                      std::string* error = nullptr);
        static bool instantiatePrefab(Scene* scene, Entity root, std::span<const uint8_t> bytes, std::vector<Entity>* members = nullptr,
                                      std::string* error = nullptr);
    };

}
