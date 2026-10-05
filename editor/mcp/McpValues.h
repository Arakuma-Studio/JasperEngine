// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include "Catalog.h"

#include <functional>
#include <string>

namespace jasperengine::editor {
    class Command;
    class Project;
}

namespace jasperengine::editor::mcp {

    // JSON <-> engine property values (DESIGN.md, Values). Reads return the shapes writes
    // accept: vectors as [x,y,(z,(w))] (writes also take {x,y,z,w}), quaternions as
    // [x,y,z,w] (writes also take {"euler":[x,y,z]} in degrees), Ease by name, asset
    // references as project-relative paths.
    // Ported from the former agent's propertyValueToJson / buildPropertyCommand.
    // TODO(W1): Enum names, Material, Custom (Stream YAML round-trip), Entity refs by path.
    class McpValues {
    public:
        static std::string propertyTypeName(PropertyType type);

        // The property's current value
        static Json toJson(Project* project, const std::string& propertyName, const PropertyData& property);

        // A typed PropertyCmd setting the property to `value` (not executed). Null with
        // `error` set when the value doesn't fit the property type.
        static Command* buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                             const std::string& propertyName, const PropertyData& property,
                                             const Json& value, std::string& error,
                                             std::function<void()> onChanged = nullptr);

        static bool parseVector2(const Json& value, Vector2& out);
        static bool parseVector3(const Json& value, Vector3& out);
        static bool parseVector4(const Json& value, Vector4& out);
        static bool parseQuaternion(const Json& value, Quaternion& out);
        static Json toJson(const Vector2& value);
        static Json toJson(const Vector3& value);
        static Json toJson(const Vector4& value);
        static Json toJson(const Quaternion& value);

        // Asset references: stored (assets-relative) <-> project-relative
        static std::string assetPathToJson(Project* project, const std::string& stored);
        static bool assetPathFromJson(Project* project, const std::string& projectRelative, std::string& stored, std::string& error);
    };

}
