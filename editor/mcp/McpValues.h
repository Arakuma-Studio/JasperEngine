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
#include "shader/ShaderManifest.h"
#include "yaml-cpp/yaml.h"

#include <functional>
#include <memory>
#include <set>
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
    //                   {"path"|"cube":[6 paths: front (+Z), back (-Z), left (-X), right (+X),
    //                   up (+Y), down (-Y)]|"cube":"one.png", "min_filter", "mag_filter",
    //                   "wrap_u", "wrap_v", "alpha_border", "svg_scale"} when the sampler is not
    //                   the default | {"camera": <entity>} (render texture of that camera) |
    //                   {"fluid": <entity>} (that FluidSimulation's output); writes also take
    //                   the link ids "camera|<entity id>" and "fluid|<entity id>"
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
        // The same knowing the component (its enum names; an Animation Player's clip list with
        // project-relative paths), still without the component's scene-file encoding
        static Json toJson(Project* project, ComponentType component, const std::string& propertyName, const PropertyData& property);

        // ---- writing -------------------------------------------------------------------

        // A command setting one typed property to `value` (not executed): a PropertyCmd<T>, or
        // a composite for values that need more (a model file loads, a material links, a
        // render texture turns its camera's renderToTexture on). Null with `error` set when
        // the value doesn't fit. Custom properties go through buildChanges.
        // shaderReference: the shader the property's shader-library values are for when a
        // change of the same call sets another one; null reads the current one.
        // editedKeys: for a shaderParams / shaderUniforms / shaderTextures collection, the
        // entries this edit names (only those are checked against the shader's manifest);
        // null checks every entry of `value`.
        // A field of a submesh material linked to a .material file (submeshes[N].material.<field>)
        // makes it the GameObject's own material first (unlinked, as an edit of the whole material
        // does): left linked, the edit would be replaced by the file's value the next time the
        // scene opens.
        static Command* buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                             const std::string& propertyName, const PropertyData& property,
                                             const Json& value, std::string& error,
                                             std::function<void()> onChanged = nullptr,
                                             const std::string* shaderReference = nullptr,
                                             const std::set<std::string>* editedKeys = nullptr);

    private:
        static Command* buildPropertyCommandUnlinked(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                                     const std::string& propertyName, const PropertyData& property,
                                                     const Json& value, std::string& error, std::function<void()> onChanged,
                                                     const std::string* shaderReference, const std::set<std::string>* editedKeys);
    public:

        // One edit of set_properties / create_entity / add_component
        struct Change {
            ComponentType component = ComponentType::Transform;
            std::string property;
            std::string op = "set";      // set | insert | remove
            Json value;                  // set / insert
            long index = -1;             // insert (-1: append) / remove
        };
        // [{component, property, value}], [{component, property, insert: v, index?}],
        // [{component, property, remove: index}], or {component: {property: value}}.
        // A FluidSimulation's "preset" (an id of FluidPresets.h) stands for the preset's fields:
        // it expands in place (in the object form before the component's other values, which
        // then override it)
        static bool parseChanges(const Json& changes, std::vector<Change>& out, std::string& error);
        // The changes as the components' reflection tables see them (docs/specs/S08-reflection.md
        // R26): a property named by an alias is renamed to its field, with a warning ("light.old
        // is now light.new"); one that is read-only, or a value outside its hard range, is refused.
        // Properties no table describes are left to the edit.
        static bool checkReflectedChanges(std::vector<Change>& changes, std::vector<std::string>& warnings, std::string& error);
        // A RectTransform property the entity's Safe Area component drives (anchors, offsets,
        // size): the error that refuses setting it, "" when it is free
        static std::string safeAreaDrivenError(Scene* scene, Entity entity, const std::string& property);

        // parseChanges without the renamed names mapped to the current ones
        static bool parseChangesAsGiven(const Json& changes, std::vector<Change>& out, std::string& error);
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
        // A rotation: [x,y,z,w], {"quat"}, {"euler"} (degrees, ZYX) or {x,y,z,w} (all four); `out` holds
        // the current value. Within 10% of unit length it is normalized; anything else (Euler degrees
        // typed as a quaternion, zero) is refused with the reason in `error`.
        static bool parseQuaternion(const Json& value, Quaternion& out, std::string* error = nullptr);
        // A float as JSON: its shortest round-trip decimal (0.1, not 0.10000000149)
        static double number(float value);
        static Json toJson(const Vector2& value);
        static Json toJson(const Vector3& value);
        static Json toJson(const Vector4& value);
        // {"quat": [x,y,z,w], "euler": [x,y,z]}
        static Json toJson(const Quaternion& value);
        static Vector3 eulerDegrees(const Quaternion& value);

        // Animation references as MCP passes them (project-relative; the stored form of an existing
        // file is taken too) to the stored (asset-relative) form and the file's GUID: a clip (.anim
        // that exists, or "<model>#<animation>" of an existing model, checked against its
        // animations), a .mask / .controller (it must exist and parse). Empty clears. The error
        // names what is wrong with an example (no field prefix).
        static bool clipReferenceFromJson(Project* project, const std::string& text, std::string& stored, std::string& guid, std::string& error);
        static bool animAssetReferenceFromJson(Project* project, const std::string& text, bool mask, std::string& stored, std::string& guid,
                                               std::string& error);
        // a stored clip reference shown project-relative ("assets/anim/walk.anim", "assets/hero.gltf#Walk")
        static std::string clipReferenceToJson(Project* project, const std::string& stored);

        // Recomputes every Transform's world matrix and values now (the transform system only runs
        // between frames, so a GameObject made or moved this frame reads stale world values)
        static void refreshTransforms(SceneProject* scene);

        // A Canvas Scaler as {mode, scale_factor, reference, match_mode, match, physical_unit,
        // fallback_dpi} (enums by snake name). FromJson changes the fields the object names on
        // `out` (aliases reference_resolution, screen_match_mode, match_width_or_height,
        // fallback_screen_dpi accepted); false with `error` on an unknown key or a bad value
        static Json canvasScalerToJson(const CanvasScalerSettings& settings);
        static bool canvasScalerFromJson(const Json& value, CanvasScalerSettings& out, std::string& error);

        static Json entityToJson(SceneProject* scene, Entity entity);
        static bool entityFromJson(SceneProject* scene, const Json& value, Entity& out, std::string& error);
        static Json entityReferenceToJson(Project* project, SceneProject* scene, const EntityReference& value);
        static bool entityReferenceFromJson(Project* project, SceneProject* scene, const Json& value, EntityReference& out, std::string& error);


        static Json textureToJson(Project* project, SceneProject* scene, const Texture& texture);
        // `camera` gets the camera entity of a {"camera": ...} value (NULL_ENTITY otherwise; a
        // {"fluid": ...} value is a plain texture bound to the fluid's output)
        static bool textureFromJson(Project* project, SceneProject* scene, const Json& value, Texture& out,
                                    Entity& camera, std::string& error);
        static Json materialToJson(Project* project, SceneProject* scene, const Material& material, const std::string& linkedFile = {});
        // Inline values onto `out` (holding the current material); "file" is not handled here
        static bool materialFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error);
        // Material::pbr, the built-in Lit's extended lobes and hex tiling (docs/mcp/guide/materials.md):
        // {"<lobe>": {"<field>": value}} with every lobe away from its defaults, each with all its
        // fields; a write merges a partial object into `out` (null clears a map) and checks each
        // value against its range, an error naming the field ("pbr.clearcoat.roughness: ...")
        static Json materialPbrToJson(Project* project, SceneProject* scene, const MaterialPbrExtensions& pbr);
        static bool materialPbrFromJson(Project* project, SceneProject* scene, const Json& value, MaterialPbrExtensions& out, std::string& error);

        // ShaderUniformValues (shaderUniforms, post-process pass uniforms): {name: [x,y,z,w]};
        // writes also take a number or 1-3 numbers per uniform (the rest 0)
        static Json shaderUniformsToJson(const ShaderUniformValues& values);
        static bool shaderUniformsFromJson(const Json& value, ShaderUniformValues& out, std::string& error);

        // ---- shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md)
        //   shader          "" (built-in), "library:<name>" (list_shaders) or a fork base path
        //   shaderParams    {member: number | [x,y(,z(,w))] | "#RRGGBB[AA]"}: values as stored
        //                   (colours linear); a hex string is sRGB, converted to linear. A
        //                   member left out takes the manifest default. (Components keep theirs
        //                   in shaderUniforms, same shape.)
        //   shaderTextures  {uniform: <texture> (see Texture) | null}: null / left out binds
        //                   the manifest default
        //   shaderFeatures  [define, ...] as stored ("!F" turns a default-on feature off, a
        //                   @keywords choice is "<GROUP>_<VALUE>"); writes also take
        //                   {define: true|false}, applied to the current list
        // Entry paths "submeshes[N].material.shaderParams.<member>",
        // ".shaderTextures.<uniform>", and a component's "shaderUniforms.<member>" /
        // "shaderTextures.<uniform>" read one value (the manifest default when unset) and
        // write it (null removes it).

        // The manifest of a shader reference (null: built-in, unknown, no manifest)
        static std::shared_ptr<const ShaderManifest> findManifest(const std::string& reference);
        // A shader reference a property may take for a shader of `type`: "" , an existing
        // library shader of that type, or a fork whose files exist. False with `error`.
        static bool checkShaderReference(Project* project, const std::string& reference, ShaderType type, std::string& error);
        // The library shaders, "library:a, library:b"
        static std::string libraryShaderNamesText();
        // A manifest as list_shaders gives it
        static Json manifestToJson(const std::string& reference, const ShaderManifest& manifest, bool detail = true);
        // ShaderRequire_SceneDepth / _SceneColor bits as names: [], ["scenedepth"], ["scenecolor"] or both
        static Json sceneInputsToJson(uint32_t inputs);
        // What an entity's mesh submeshes (their shaders and extra passes) and points read of
        // the scene in their current variants (get_entity, get_shader_uniforms): one entry per
        // reader, {component, submesh, reads, extra_pass_reads, error, fix}; empty when none
        static Json entitySceneInputsToJson(Scene* scene, Entity entity);
        // A FluidSimulation's runtime state (get_entity, get_runtime_entity: "fluid_stats"), read
        // only: FluidStats in snake_case plus the matching preset, the device's support and the
        // simulator's last error; null when the entity has no FluidSimulationComponent
        static Json fluidStatsToJson(Scene* scene, Entity entity);
        // A Video Player's playback state (get_entity, get_runtime_entity: "video"): prepared,
        // playing, time, frame, frameCount, length, backend, codecs, error, prepareProgress; null
        // when the entity has none or nothing was opened yet (edit mode)
        static Json videoStatusToJson(Scene* scene, Entity entity);
        // A UI element's Canvas Group chain (ui canvas group spec 7.8): {inherited_alpha,
        // interactable, blocks_raycasts, group: "<path>"}; null outside any group or without a
        // RectTransform; a FLATTEN group adds flatten {layered, layer_alpha, the scene's layer stats}
        static Json canvasGroupStateToJson(Scene* scene, Entity entity);
        // a Button's Selectable (ui-components): whether navigation reaches it and where each way
        // (and Tab) leads, from the current layout; live adds its state and whether it is selected
        static Json selectableToJson(Scene* scene, Entity entity, bool live);

        static Json shaderParamsToJson(const ShaderUniformValues& values);
        // `out` replaced by `value` (null clears); `manifest` (may be null) types colour hex
        // strings and, for a library shader, refuses members it does not declare (an unknown
        // library shader refuses them all). checkedKeys: only those members are checked against
        // the manifest; the others are values already stored (kept from a previous shader).
        static bool shaderParamsFromJson(const ShaderManifest* manifest, bool library, const Json& value, ShaderUniformValues& out,
                                         std::string& error, const std::set<std::string>* checkedKeys = nullptr);
        // one parameter: a number, 1-4 numbers or (colour kinds) "#RRGGBB[AA]" sRGB
        static bool shaderParamFromJson(const ShaderManifestProperty* property, const std::string& member, const Json& value, Vector4& out, std::string& error);
        static Json shaderTexturesToJson(Project* project, SceneProject* scene, const std::vector<std::pair<std::string, Texture>>& textures);
        // image paths only (a camera's render texture is refused); checkedKeys as above
        static bool shaderTexturesFromJson(Project* project, SceneProject* scene, const ShaderManifest* manifest, bool library, const Json& value,
                                           std::vector<std::pair<std::string, Texture>>& out, std::string& error,
                                           const std::set<std::string>* checkedKeys = nullptr);
        static Json shaderFeaturesToJson(const std::vector<std::string>& features);
        // a list replaces `inout`; an object {define: bool} edits it (ShaderLibraryValues::setFeature)
        static bool shaderFeaturesFromJson(const std::string& shader, const ShaderManifest* manifest, bool library, const Json& value,
                                           std::vector<std::string>& inout, std::string& error);
        // The shader-library keys of a material (shader, shaderParams, shaderTextures,
        // shaderFeatures, blendMode, cullMode, depthWrite, depthTest, depthOffsetFactor,
        // depthOffsetUnits, sortPriority, lighting, stencil, colorWrite, extraPass) added to
        // `out`; all = false leaves out the ones at their defaults
        static void materialShaderLibraryToJson(Project* project, SceneProject* scene, const Material& material, Json& out, bool all);
        // Applies those keys of `value` to `out` ("shader" first: the others follow its
        // manifest). False with `error`.
        static bool materialShaderLibraryFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error);
        static bool isMaterialShaderLibraryKey(const std::string& key);
        // Stochastic Tiling of a material: stochasticTiling, and stochasticCellSize (0.25-4),
        // stochasticBlendSharpness, stochasticContrast, stochasticRotation (0-1). The values are
        // written while it is on or away from their defaults; out of range is an error naming
        // the field, what it does and its default.
        static void materialStochasticToJson(const Material& material, Json& out);
        static bool materialStochasticFromJson(const Json& value, Material& out, std::string& error);
        static bool isMaterialStochasticKey(const std::string& key);

        // ---- shader library phase 2 (docs/superpowers/specs/2026-10-10-shader-library-phase2-design.md)
        //   stencil     {compare, ref, readMask, writeMask, pass, fail, depthFail}: compare
        //               "auto" takes the shader's @state stencil rule, "disabled" turns stencil
        //               off, any other test makes the whole block explicit; ref and masks
        //               0..255; ops keep, zero, replace, incr_clamp, decr_clamp, invert,
        //               incr_wrap, decr_wrap (the manifest's names are accepted too). Writes
        //               merge the keys given; null resets it (auto).
        //   colorWrite  "auto" (-1), "none" (0) or channels of "rgba" ("rgb", "a"...); writes
        //               also take a mask -1..15 (r 1, g 2, b 4, a 8)
        //   extraPass   {shader, shaderParams, shaderTextures, shaderFeatures, blendMode,
        //               cullMode, depthWrite, depthTest, depthOffsetFactor, depthOffsetUnits,
        //               stencil, colorWrite, order ("after" | "before")}: one more draw of the
        //               submesh with its own (mesh) shader, values checked against that
        //               shader's manifest like the material's; writes merge, null removes it
        static Json stencilToJson(const MaterialStencil& stencil);
        static bool stencilFromJson(const Json& value, MaterialStencil& inout, std::string& error);
        static Json colorWriteToJson(int colorWrite);
        static bool colorWriteFromJson(const Json& value, int& out, std::string& error);
        static Json materialPassToJson(Project* project, SceneProject* scene, const MaterialPass& pass, bool all);
        static bool materialPassFromJson(Project* project, SceneProject* scene, const Json& value, MaterialPass& inout, std::string& error);

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
    // execute() fails (its error is in errorSlot()).
    class McpStepsCmd : public Command {
    public:
        using Step = std::function<Command*(std::string& error)>;

        void addStep(Step step) { steps.push_back(std::move(step)); }
        // Recompute the scene's world matrices after each step (the transform system only runs
        // between frames): for steps that read them, like a reparent keeping the world pose
        void refreshTransforms(Project* project, uint32_t sceneId) { transformProject = project; transformSceneId = sceneId; }
        // A shared slot for the error, readable after the history deleted a failed command
        std::shared_ptr<std::string> errorSlot() const { return error; }

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return true; }
        // its steps' files and refusals (Command::touchedPaths; a step's own canUndo / canRedo)
        std::vector<Touched> touchedPaths(bool undo) const override;
        std::string undoRefusal() const override;
        std::string redoRefusal() const override;
        std::string failure() const override { return !redoFailure.empty() ? redoFailure : error ? *error : std::string(); }

        // Its first step's name, "(+N more)" for the others
        std::string describe() const override;
        const char* typeName() const override { return "McpStepsCmd"; }
        void collectKeepCommands(KeepContext& context) const override;
        bool isNoOp() const override;
        size_t memoryBytes() const override;
        // The commands it ran
        const std::vector<std::unique_ptr<Command>>& getDone() const { return done; }

    private:
        std::vector<Step> steps;
        std::vector<std::unique_ptr<Command>> done;
        bool built = false;
        std::shared_ptr<std::string> error = std::make_shared<std::string>();
        std::string redoFailure;   // why the last redo failed (a step's own failure, or a refused scoped write)
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
