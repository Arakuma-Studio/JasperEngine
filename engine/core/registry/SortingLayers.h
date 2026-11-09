// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SORTINGLAYERS_H
#define SORTINGLAYERS_H

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    // the layer every renderer is on by default; it always exists and keeps its name
    inline constexpr int32_t DEFAULT_SORTING_LAYER_ID = 0;

    struct SortingLayerInfo{
        int32_t id = DEFAULT_SORTING_LAYER_ID;
        std::string name;

        bool operator==(const SortingLayerInfo& o) const { return id == o.id && name == o.name; }
        bool operator!=(const SortingLayerInfo& o) const { return !(*this == o); }
    };

    // Unity's Graphics > Transparency Sort Mode: Default has no axis term in 2D passes; Custom Axis
    // draws a larger dot(axis, point) first
    enum class TransparencySortMode : uint8_t{
        DEFAULT,
        CUSTOM_AXIS
    };

    // Project-wide Sorting Layers (Unity's Tags and Layers > Sorting Layers, spec S52 R1) and the
    // Transparency Sort settings (R4). A renderer stores a layer's id, which never changes, so
    // renaming or reordering rewrites no scene; the draw order is the layer's value (its place in
    // the list). The editor sets them from project.yaml, an exported game from its PlayerConfig or
    // its generated init(). Main thread.
    class JASPERENGINE_API SortingLayers{
    public:
        // The ordered list, first drawn first. Default (id 0) is added at the front when missing
        // and keeps its name; names are trimmed; an empty name, a repeated name or id, or a
        // negative id is dropped with a warning.
        static void setLayers(const std::vector<SortingLayerInfo>& layers);
        static const std::vector<SortingLayerInfo>& getLayers();
        // A list of names: names already in the table keep their ids, new ones take new ids
        // (getNextId onward: an id once given is never given again, so a removed layer's
        // renderers never land on a later layer)
        static std::vector<SortingLayerInfo> fromNames(const std::vector<std::string>& names);
        // The same for entries that may name their id (a rename keeps it: MCP's {id, name}); an
        // entry with id -1 goes by its name as in fromNames. An id the table does not have is
        // refused: false and `error` names it
        static bool resolve(const std::vector<SortingLayerInfo>& entries, std::vector<SortingLayerInfo>& out, std::string& error);
        // The id the next new layer takes: past every id the table ever held (project.yaml keeps
        // it once a layer was removed); setNextId never lowers it below the ids held
        static int32_t getNextId();
        static void setNextId(int32_t id);

        // Unity SortingLayer.NameToID: -1 when no layer has the name
        static int32_t nameToId(const std::string& name);
        // Unity SortingLayer.IDToName: "" when no layer has the id
        static std::string idToName(int32_t id);
        static bool isValid(int32_t id);
        // Unity SortingLayer.GetLayerValueFromID: the layer's place in the list (an unknown id
        // draws on Default)
        static int getLayerValue(int32_t id);
        // the draw order of an id, without checks beyond the table's range (render loop)
        static uint16_t valueOf(int32_t id){
            const Table& t = table();
            return (id >= 0 && (size_t)id < t.values.size()) ? t.values[(size_t)id] : t.defaultValue;
        }

        // Project Settings > Graphics > Transparency Sort Mode / Axis
        static void setTransparencySortMode(TransparencySortMode mode);
        static TransparencySortMode getTransparencySortMode();
        static void setTransparencySortAxis(const Vector3& axis);
        static Vector3 getTransparencySortAxis();

        // bumped by every change: a renderer's cached order is checked against it
        static uint32_t getRevision();

        // Default alone, mode Default, axis (0, 1, 0)
        static void reset();

    private:
        struct Table{
            std::vector<SortingLayerInfo> layers;
            std::vector<uint16_t> values;   // by id; ids without a layer take Default's value
            uint16_t defaultValue = 0;
            TransparencySortMode mode = TransparencySortMode::DEFAULT;
            Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);
            uint32_t revision = 1;
            int32_t nextId = 1;
        };
        // function-local, so a scene built during static initialization still sees it
        static Table& table();
        static void rebuildValues(Table& t);
    };

}

#endif //SORTINGLAYERS_H
