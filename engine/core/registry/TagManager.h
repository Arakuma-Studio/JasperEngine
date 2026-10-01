// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TAGMANAGER_H
#define TAGMANAGER_H

#include "Export.h"
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace jasperengine{

    // Unity's 32 GameObject layers: a layer mask has one bit per layer
    inline constexpr int MAX_LAYERS = 32;
    inline constexpr uint32_t LAYER_MASK_NOTHING = 0u;
    inline constexpr uint32_t LAYER_MASK_EVERYTHING = 0xFFFFFFFFu;
    // Built-in layer 2, which ray casts skip unless their layer mask names it
    inline constexpr int IGNORE_RAYCAST_LAYER_INDEX = 2;
    // Unity's Physics.DefaultRaycastLayers: every layer but Ignore Raycast
    inline constexpr uint32_t LAYER_MASK_DEFAULT_RAYCAST = ~(1u << IGNORE_RAYCAST_LAYER_INDEX);

    // Project-wide tags and layer names, Unity's Tags and Layers settings. The built-in
    // tags and layers are fixed; the editor sets the user ones from project.yaml and an
    // exported game from its generated init(). Set them from the main thread.
    class JASPERENGINE_API TagManager{
    public:
        // Untagged, Respawn, Finish, EditorOnly, MainCamera, Player, GameController
        static const std::vector<std::string>& getBuiltinTags();
        static bool isBuiltinTag(const std::string& tag);
        // Layers 0, 1, 2, 4 and 5: Default, TransparentFX, Ignore Raycast, Water, UI
        static bool isBuiltinLayer(int layer);
        // "" for a user layer or an index outside 0..31
        static const char* getBuiltinLayerName(int layer);

        // User tags, after the built-in ones. Names are trimmed; empty names and
        // repeats (of a built-in tag too) are dropped.
        static void setTags(const std::vector<std::string>& tags);
        static const std::vector<std::string>& getTags();
        // built-in tags first, then the user tags
        static std::vector<std::string> getAllTags();
        static bool isTagDefined(const std::string& tag);

        // Name of user layer 3 or 6..31, trimmed; "" leaves the layer unnamed. A
        // built-in layer, an index outside 0..31 or a name another layer has is refused.
        static bool setLayerName(int layer, const std::string& name);
        // Every user layer name, index = layer (built-in entries stay empty)
        static void setLayerNames(const std::vector<std::string>& names);

        // Unity LayerMask.LayerToName: "" when the layer has no name
        static std::string layerToName(int layer);
        // Unity LayerMask.NameToLayer: -1 when no layer has the name
        static int nameToLayer(const std::string& name);
        // Unity LayerMask.GetMask: names no layer has are skipped
        static uint32_t getMask(const std::vector<std::string>& layerNames);

        // Only the built-in tags and layers left
        static void reset();

        // A tag or layer name as stored: leading and trailing whitespace removed
        static std::string trimName(const std::string& name);
    };

    // Unity's LayerMask helpers
    class JASPERENGINE_API LayerMask{
    public:
        static std::string layerToName(int layer){ return TagManager::layerToName(layer); }
        static int nameToLayer(const std::string& name){ return TagManager::nameToLayer(name); }
        static uint32_t getMask(const std::vector<std::string>& layerNames){ return TagManager::getMask(layerNames); }
        static uint32_t getMask(std::initializer_list<std::string> layerNames){ return TagManager::getMask(std::vector<std::string>(layerNames)); }
    };

}

#endif //TAGMANAGER_H
