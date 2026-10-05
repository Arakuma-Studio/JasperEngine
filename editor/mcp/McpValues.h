// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include "Catalog.h"
#include "command/Command.h"
#include "script/ScriptProperty.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "texture/Texture.h"
#include "yaml-cpp/yaml.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class MultiPropertyCmd;
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {

    // JSON <-> engine property values (DESIGN.md, Values). Reads return exactly the shapes
    // writes accept. Per PropertyType:
    //   Bool            true / false
    //   Int, UInt       integer (an integral number is accepted)
    //   Float, Double   number
    //   String          string ("filename" asset references: project-relative path)
    //   Vector2/3/4     [x,y(,z(,w))]; writes also take {x,y,z,w}, Vector4 [r,g,b] (keeps
    //                   alpha) and colors "#RRGGBB" / "#RRGGBBAA" (bytes / 255, no color-space
    //                   conversion)
    //   Quat            reads {"quat":[x,y,z,w], "euler":[x,y,z]} (degrees, the Inspector's ZYX
    //                   order); writes take [x,y,z,w], {"quat":[...]}, {"euler":[...]} or both
    //                   (then whichever differs from the current value wins)
    //   Enum, Ease      name string (snake_case, e.g. "dynamic", "quad_in_out"); writes also
    //                   take the Inspector label ("Quad In Out"), the scene-file name or the int
    //   Texture         null (none) | "path.png" (project-relative; "x.svg?svgScale=2") |
    //                   {"path"|"cube":[6 paths]|"cube":"one.png", "min_filter", "mag_filter",
    //                   "wrap_u", "wrap_v", "alpha_border", "svg_scale"} when the sampler is not
    //                   the default | {"camera": <entity>} (render texture of that camera)
    //   Font            [main font path, fallback paths...]; writes also take one path
    //   Material        {"name", "baseColorFactor", "metallicFactor", "roughnessFactor",
    //                   "alphaCutoff", "foliageTransmission", "alphaMode", "emissiveFactor",
    //                   "<x>Texture": <texture>, "<x>TexCoord": int, "file": ".material path"
    //                   when linked}. Writes: a ".material" path or {"file": path} links the file;
    //                   an object without "file" edits inline (partial: missing keys keep their
    //                   value; a linked file is unlinked); null unlinks.
    //   Entity          null | {id, name, path}; writes take any entity ref (id, path,
    //                   {"name"}...) or null
    //   EntityReference null | {id, name, path(, "scene": id when in another scene)}; writes take
    //                   an entity ref, optionally {"scene": <scene ref>, ...} for another scene
    //   Custom          the component's scene-file (YAML) encoding of the property as JSON
    //                   (asset paths there are relative to the assets dir, entities are ids);
    //                   writes replace it whole, or insert/remove an element of a list
    class McpValues {
    public:
        static std::string propertyTypeName(PropertyType type);

        // ---- reading -------------------------------------------------------------------

        // Where a property lives, for reads that need it: enum names, entity paths, custom
        // values (the component encode is made once, lazily, and cached here)
        struct ReadContext {
            Project* project = nullptr;
            SceneProject* scene = nullptr;
            Entity entity = NULL_ENTITY;
            ComponentType component = ComponentType::Transform;
            bool hasComponent = true;    // false: property maps without a component (defaults)
            YAML::Node componentYaml;
            bool yamlLoaded = false;
        };

        // The property's current value (property.ref) or default (property.def)
        static Json readProperty(ReadContext& ctx, const std::string& name, const PropertyData& property, bool readDefault = false);
        // A property that is not in Catalog but in the component's scene-file encoding
        // ("colliders", "frames", "spriteInitializer.frames"...): found = false when absent
        static Json readYamlProperty(ReadContext& ctx, const std::string& name, bool& found);

        // Without context (no enum names, no custom values): kept for callers that have none
        static Json toJson(Project* project, const std::string& propertyName, const PropertyData& property);

        // ---- writing -------------------------------------------------------------------

        // A command setting one typed property to `value` (not executed): a PropertyCmd<T>, or
        // a composite for values that need more (a model file loads, a material links, a
        // render texture turns its camera's renderToTexture on). Null with `error` set when
        // the value doesn't fit. Custom properties go through buildChanges.
        static Command* buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                             const std::string& propertyName, const PropertyData& property,
                                             const Json& value, std::string& error,
                                             std::function<void()> onChanged = nullptr);

        // One edit of set_properties / create_entity / add_component
        struct Change {
            ComponentType component = ComponentType::Transform;
            std::string property;
            std::string op = "set";      // set | insert | remove
            Json value;                  // set / insert
            long index = -1;             // insert (-1: append) / remove
        };
        // [{component, property, value}], [{component, property, insert: v, index?}],
        // [{component, property, remove: index}], or {component: {property: value}}
        static bool parseChanges(const Json& changes, std::vector<Change>& out, std::string& error);
        // The commands for `changes` on each entity, added to `multi`: per entity and
        // component, the custom (YAML) edits first as one component rewrite, then the typed
        // PropertyCmds. False with `error` (multi may hold part of the commands then).
        static bool appendChangeCommands(Project* project, SceneProject* scene, const std::vector<Entity>& entities,
                                         const std::vector<Change>& changes, MultiPropertyCmd& multi, std::string& error);
        // Valid property names of a component on an entity (for error messages)
        static std::string propertyNamesText(SceneProject* scene, Entity entity, ComponentType component, size_t max = 80);

        // ---- enums ---------------------------------------------------------------------

        struct EnumOption {
            int value;
            const char* name;    // canonical snake_case name
            const char* label;   // Inspector label
        };
        using EnumOptions = std::vector<EnumOption>;
        // The options of an Enum property ("colliders[2].type" works), null when unknown
        static const EnumOptions* enumOptions(ComponentType component, const std::string& propertyName);
        static const EnumOptions& easeOptions();
        // Named enum tables: light_state, shadow_quality, texture_filter, texture_wrap,
        // texture_alpha_border, material_alpha_mode, ease, ...
        static const EnumOptions* enumTable(const std::string& name);
        static Json enumToJson(int value, const EnumOptions& options);
        static bool enumFromJson(const Json& value, const EnumOptions& options, int& out, std::string& error);
        static Json enumNames(const EnumOptions& options);

        // ---- shapes --------------------------------------------------------------------

        static bool parseVector2(const Json& value, Vector2& out);
        static bool parseVector3(const Json& value, Vector3& out);
        static bool parseVector4(const Json& value, Vector4& out);
        // [x,y,z,w], {"quat"}, {"euler"} (degrees, ZYX), {x,y,z,w}; `out` holds the current value
        static bool parseQuaternion(const Json& value, Quaternion& out);
        // A float as JSON: its shortest round-trip decimal (0.1, not 0.10000000149)
        static double number(float value);
        static Json toJson(const Vector2& value);
        static Json toJson(const Vector3& value);
        static Json toJson(const Vector4& value);
        // {"quat": [x,y,z,w], "euler": [x,y,z]}
        static Json toJson(const Quaternion& value);
        static Vector3 eulerDegrees(const Quaternion& value);

        // Recomputes every Transform's world matrix and values now (the transform system only runs
        // between frames, so a GameObject made or moved this frame reads stale world values)
        static void refreshTransforms(SceneProject* scene);

        static Json entityToJson(SceneProject* scene, Entity entity);
        static bool entityFromJson(SceneProject* scene, const Json& value, Entity& out, std::string& error);
        static Json entityReferenceToJson(Project* project, SceneProject* scene, const EntityReference& value);
        static bool entityReferenceFromJson(Project* project, SceneProject* scene, const Json& value, EntityReference& out, std::string& error);

        static Json textureToJson(Project* project, SceneProject* scene, const Texture& texture);
        // `camera` gets the camera entity of a {"camera": ...} value (NULL_ENTITY otherwise)
        static bool textureFromJson(Project* project, SceneProject* scene, const Json& value, Texture& out,
                                    Entity& camera, std::string& error);
        static Json materialToJson(Project* project, SceneProject* scene, const Material& material, const std::string& linkedFile = {});
        // Inline values onto `out` (holding the current material); "file" is not handled here
        static bool materialFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error);

        // ShaderUniformValues (shaderUniforms, post-process pass uniforms): {name: [x,y,z,w]};
        // writes also take a number or 1-3 numbers per uniform (the rest 0)
        static Json shaderUniformsToJson(const ShaderUniformValues& values);
        static bool shaderUniformsFromJson(const Json& value, ShaderUniformValues& out, std::string& error);

        // Asset references: stored (assets-relative) <-> project-relative
        static std::string assetPathToJson(Project* project, const std::string& stored);
        static bool assetPathFromJson(Project* project, const std::string& projectRelative, std::string& stored, std::string& error);

        // ---- YAML <-> JSON ---------------------------------------------------------------

        // Scalars: true/false → bool, canonical integers / decimals → numbers, else strings
        static Json yamlToJson(const YAML::Node& node);
        static YAML::Node jsonToYaml(const Json& value);
        // The node of a property inside a component encode ("colliders[0].radius",
        // "spriteInitializer.frames", "scripts[1].speed" → that script property's value).
        // An invalid node when absent.
        static YAML::Node findYamlPath(const YAML::Node& root, const std::string& property);
        // Edits the node at `property` in `root` (op set / insert / remove, see Change)
        // `create`: make the missing map keys of the path (a property the encoder left out)
        static bool editYamlPath(YAML::Node root, const std::string& property, const std::string& op,
                                 const Json& value, long index, std::string& error, bool create = false);
    };

    // Commands built step by step, each from the state the steps before it left (a property
    // of a GameObject or component another step creates), applied, undone and redone as one
    // undo step. A step returns null with an error to abort: the steps done are undone and
    // execute() fails (its error is in error()).
    class McpStepsCmd : public Command {
    public:
        using Step = std::function<Command*(std::string& error)>;

        void addStep(Step step) { steps.push_back(std::move(step)); }
        // Recompute the scene's world matrices after each step (the transform system only runs
        // between frames): for steps that read them, like a reparent keeping the world pose
        void refreshTransforms(Project* project, uint32_t sceneId) { transformProject = project; transformSceneId = sceneId; }
        // A shared slot for the error, readable after the history deleted a failed command
        std::shared_ptr<std::string> errorSlot() const { return error; }
        const std::string& lastError() const { return *error; }

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return true; }

    private:
        std::vector<Step> steps;
        std::vector<std::unique_ptr<Command>> done;
        bool built = false;
        std::shared_ptr<std::string> error = std::make_shared<std::string>();
        Project* transformProject = nullptr;
        uint32_t transformSceneId = 0;
        void refresh();
    };

    // Runs a command through a scene's undo history as one discrete undo step (no merging).
    // False with `error` when it did not apply: the command's own error (McpStepsCmd), or
    // the warnings and errors the editor logged while trying, or `fallback`.
    bool runSceneCommand(uint32_t sceneId, Command* cmd, std::string& error, const std::string& fallback = {});
    // The same through the project history (asset files, scenes of the project)
    bool runProjectCommand(Project* project, Command* cmd, std::string& error, const std::string& fallback = {});

}
