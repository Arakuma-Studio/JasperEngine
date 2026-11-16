// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "manager/PrefabAsset.h"
#include "asset/AssetRef.h"

// Macro with optional type parameter
#define DPROPERTY_1(DisplayName) \
public:

#define DPROPERTY_2(DisplayName, Type) \
public: /* @DPROPERTY_TYPE: Type */

// Macro overload selector
#define GET_DPROPERTY_MACRO(_1, _2, NAME, ...) NAME
#define DPROPERTY(...) GET_DPROPERTY_MACRO(__VA_ARGS__, DPROPERTY_2, DPROPERTY_1)(__VA_ARGS__)

namespace jasperengine {

    class Scene;

    enum class ScriptPropertyType {
        Bool,
        Int,
        Float,
        String,
        Vector2,
        Vector3,
        Vector4,
        Color3,
        Color4,
        EntityReference,
        Prefab,         // a PrefabAsset: a prefab dragged from the Project window
        Asset,          // an AssetRef of the type assetType names ("texture", "model", ...); "dataasset": a Data Asset
        List            // a ScriptPropertyList of elementType values (S95 REQ-20); appended: the cooked files store the number
    };

    struct EntityReference {
        Entity entity = NULL_ENTITY;
        uint32_t sceneId = 0;

        bool operator==(const EntityReference& other) const { return entity == other.entity && sceneId == other.sceneId; }
    };

    // One value of a list property: every kind a single property holds, lists aside (no list of lists, as Unity)
    using ScriptPropertyElement = std::variant<
        std::monostate,
        bool,
        int,
        float,
        std::string,
        Vector2,
        Vector3,
        Vector4,
        EntityReference,
        PrefabAsset,
        AssetRef
    >;

    // A list property's value (`type = "list", of = <type>`): its elements, all of the property's elementType
    struct ScriptPropertyList {
        std::vector<ScriptPropertyElement> items;

        bool operator==(const ScriptPropertyList& other) const { return items == other.items; }
        bool operator!=(const ScriptPropertyList& other) const { return !(*this == other); }
    };

    using ScriptPropertyValue = std::variant<
        std::monostate,  // For empty/uninitialized state
        bool,
        int,
        float,
        std::string,
        Vector2,
        Vector3,
        Vector4,
        EntityReference,
        PrefabAsset,
        AssetRef,
        ScriptPropertyList
    >;

    // An element as a property value (and back: a list's value gives monostate), the alternatives sharing their order
    JASPERENGINE_API ScriptPropertyValue scriptPropertyValueOf(const ScriptPropertyElement& element);
    JASPERENGINE_API ScriptPropertyElement scriptPropertyElementOf(const ScriptPropertyValue& value);
    // the value a new element of a list of `type` starts with (0, "", None...)
    JASPERENGINE_API ScriptPropertyElement scriptPropertyElementDefault(ScriptPropertyType type);
    // A `properties` entry's `type` string, case-insensitive ("int", "vec3", "list", "entity"...); false
    // for any other name (a wrapper, script or data class name)
    JASPERENGINE_API bool scriptPropertyTypeFromName(const std::string& name, ScriptPropertyType& type);

    struct JASPERENGINE_API ScriptProperty {
        std::string name;
        std::string displayName;
        ScriptPropertyType type = ScriptPropertyType::Bool;
        ScriptPropertyValue value;
        ScriptPropertyValue defaultValue;

        void* memberPtr = nullptr; // For editor use only
        void* ownedInstance = nullptr; // Wrapper created to fill an entity reference member
        int luaRef = 0; // For Lua script instance

        // Optional: Store the actual type name for editor UI/debugging
        std::string ptrTypeName; // e.g., "Mesh*", "GameObject*", "EntityHandle*"

        // Asset properties: the asset type the value must have ("texture", "model", ...; empty = any).
        // "dataasset": a .dataasset, whose class ptrTypeName names (Lua gets the asset's table)
        std::string assetType;

        // List properties: the type of every element (assetType and ptrTypeName apply to them)
        ScriptPropertyType elementType = ScriptPropertyType::Float;

        // a Data Asset reference (an Asset property, or a list of them, of assetType "dataasset")
        bool isDataAssetReference() const;

        // Helper template to get typed value
        template<typename T>
        T getValue() const {
            if (std::holds_alternative<T>(value)) {
                return std::get<T>(value);
            }
            return T{};
        }

        // Helper template to get typed default value
        template<typename T>
        T getDefaultValue() const {
            if (std::holds_alternative<T>(defaultValue)) {
                return std::get<T>(defaultValue);
            }
            return T{};
        }

        // Helper template to set typed value
        template<typename T>
        void setValue(const T& val) {
            value = val;
        }

        // Value the caller can write through. A value left from the type the property
        // had before is replaced, std::get on it would throw
        template<typename T>
        T& valueRef() {
            if (!std::holds_alternative<T>(value)) {
                reportStaleValue();
                value = T{};
            }
            return std::get<T>(value);
        }

        template<typename T>
        T& defaultValueRef() {
            if (!std::holds_alternative<T>(defaultValue)) {
                reportStaleValue();
                defaultValue = T{};
            }
            return std::get<T>(defaultValue);
        }

        // Synchronize the stored value to the actual member variable
        void syncToMember();

        // Synchronize from the actual member variable to the stored value
        void syncFromMember();

    private:
        void reportStaleValue() const;
    };

}
