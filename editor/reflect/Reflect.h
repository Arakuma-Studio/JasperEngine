// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Reflection tables (docs/specs/S08-reflection.md): one hand-written description per component
// lists its fields (name, kind, YAML key, enum, ranges, unit, label, tooltip, asset kind, flags,
// rename aliases). The scene codec, the Catalog properties, the Inspector, MCP, the exported
// code, the asset locators and the migrations all read it.
//
// Editor-only: no engine or exported-player source includes this. Fields are reached through
// member-pointer thunks and their values through TypeOps deduced from the member type
// (editor/reflect/Field.h), never through offsets, so components stay non-POD.
//
// Table order is the YAML key order of the component: it is the persisted contract. The
// Inspector draws in uiOrder within groups.

#include "Catalog.h"
#include "ecs/Entity.h"
#include "ecs/Signature.h"
#include "mcp/McpJson.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine {
    class EntityRegistry;
}

namespace jasperengine::editor::reflect {

    using Json = mcp::Json;

    // append-only
    enum class Kind : uint8_t {
        Bool, Int, UInt, Float, Double, Vector2, Vector3, Vector4, Quat, Matrix4, Rect,
        String, Enum, LayerMask, Ease, Entity, EntityReference, Texture, Font, Material, AssetPath,
        Struct, Array, Custom
    };

    enum class AssetKind : uint8_t {
        None, Texture, Cubemap, Font, Material, Model, Audio, Script, Prefab, Shader, UIDocument, Scene, Any,
        PoseAnimation, AnimationClip, AnimatorController, VertexAnimation, AvatarMask, SoundEvent, NavMesh, NavGrid, ParticleEffect,
        ModelOrPrefab,  // by the path's extension (a spline pool's entry)
        TreeData,       // a .jetrees file of a Terrain's trees (T5 R22)
        TerrainWorld    // a Name.terrainworld folder (T4 R48)
    };

    // How a field holds an asset reference (editor/asset/AssetRefSchema generates its table from the
    // fields): a contributor adds the field, and gives this only for a shape the type does not tell
    enum class AssetForm : uint8_t {
        Deduced,        // Texture, ShaderRef, AssetRef, FontArray, ops::ShaderTextures and lists of them; or a
                        // string with .asset and its GUID beside it (a string "<name>Guid" or "guid")
        ClipEntry,      // an element's path string: an .anim by the "guid" beside it, or a "<model>#<clip>" sub-clip
        PrefabRoot,     // the component's own map {path, guid}
        ScriptEntries,  // a component's scripts: their paths and their Asset and Prefab properties
        InlineEffect    // an inline .particles document: what its modules name
    };

    enum class Unit : uint8_t {
        None, Meters, Pixels, Degrees, Radians, Seconds, Percent, Lux, Lumens, Candela, Kelvin, Hertz,
        Decibels, MetersPerSecond, Kilograms, Newtons
    };

    enum FieldFlags : uint32_t {
        NoFlags          = 0,
        Hidden           = 1u << 0,   // not drawn by the Inspector
        ReadOnly         = 1u << 1,   // drawn disabled; MCP refuses writes
        NoSave           = 1u << 2,   // runtime state: not written, not read
        NoMcpWrite       = 1u << 3,   // MCP refuses writes
        NoCodegen        = 1u << 4,   // not in the exported code
        OmitIfDefault    = 1u << 5,   // written only away from its default; absent reads as the default
        OmitIfNonFinite  = 1u << 6,   // a non-finite value is not written; one read keeps the base value
        Structural       = 1u << 7,   // an edit changes what the Hierarchy shows (component default otherwise)
        ColorLinear      = 1u << 8,   // a colour stored linear (the Inspector edits sRGB)
        ColorSRGB        = 1u << 9,   // a colour stored as shown
        Hdr              = 1u << 10,  // a colour that may exceed 1
        Proxy            = 1u << 11,  // a view onto another field: never saved, edited and overridden under its own path
        Derived          = 1u << 12,  // saved, not a property (computed: world matrices, world directions)
        NoMultiEdit      = 1u << 13,  // shown for one selected GameObject only
        // additions of the implementation (docs/specs/S08-reflection.md, Deviations)
        NoDefault        = 1u << 14,  // no meaningful default: no reset arrow, no default in MCP
        NonStructural    = 1u << 15,  // an edit never changes what the Hierarchy shows (component default otherwise)
        CodegenIfChanged = 1u << 16,  // exported only away from its default
        Transient        = 1u << 17,  // a property (Catalog, MCP) that is neither saved nor drawn
        EmptyIsDefault   = 1u << 18,  // a null or empty scalar reads as absent (the clipboard writes those)
        ListWhole        = 1u << 19,  // a struct listed as a property itself besides its members
        NullIfEmpty      = 1u << 20,  // an empty array is written as null ("~"), not as "[]"
        CodegenRuntime   = 1u << 21,  // runtime state the exported code sets anyway (a NoSave field exported)
        NotListed        = 1u << 22,  // a property reached by its path but left out of the component's property list
        ListElements     = 1u << 23,  // an array of values whose elements are listed as properties too ("pathPoints[2]")
        ListEachElement  = 1u << 24,  // each element of an array of structs is listed itself besides its members ("colliders[1]")
        ListTemplate     = 1u << 25,  // listed without a component as one element with no index ("submeshes[].material")
        ComponentOnly    = 1u << 26,  // listed for a component only, not for its type (list_component_types)
        Clamped          = 1u << 27,  // a value outside the hard range is clamped into it (load and edit), not refused
        NotWritten       = 1u << 28   // left out of documents (DocumentEncodeScope), read when one has it: the
                                      // engine recomputes it (the transform's world values, S29 R20)
    };

    constexpr uint32_t operator|(FieldFlags a, FieldFlags b) { return static_cast<uint32_t>(a) | static_cast<uint32_t>(b); }

    struct EnumItem {
        int value;
        const char* token;          // YAML token
        const char* label;          // Inspector label
        const char* cppName;        // exported code ("LightType::POINT")
        const char* mcpToken = nullptr;  // MCP token (default: the YAML token)
        std::span<const char* const> aliases = {};   // older or alternative tokens, read only
        bool hidden = false;        // not offered by the Inspector or MCP
    };

    struct EnumDesc {
        const char* cppType;        // "LightType"
        std::span<const EnumItem> items;
        int fallback;               // the value of an unknown token or value
        // tokens are compared after this (the easing names: case-insensitive, '-' for '_')
        std::string (*normalize)(std::string_view token) = nullptr;
        // the values in the order the Inspector and MCP list them, when not the items' order
        std::span<const int> uiOrder = {};
    };

    struct FieldDesc;
    struct StructDesc;
    struct ComponentDesc;

    // What an encode knows besides the value (EncodeContext of a whole component: entity, registry,
    // signature and embed flags, as the Mesh and UI encodes need)
    struct EncodeContext {
        const EntityRegistry* registry = nullptr;
        Entity entity = NULL_ENTITY;
        Signature signature;
        bool embedTextureData = true;
        bool encodeBuffers = true;
        bool encodeBones = true;
    };

    // One value a decode could not read: the field kept its base value (or its default)
    struct SkipRecord {
        std::string component;
        std::string path;
        int line = 0;
        std::string reason;
    };

    struct DecodeReport {
        std::vector<SkipRecord> skipped;
    };

    struct DecodeContext {
        EntityRegistry* registry = nullptr;
        Entity entity = NULL_ENTITY;
        // the component decoded over (null for a new one): what postDecode compares against
        const void* old = nullptr;
        // the component name of skip records
        const char* component = "";
        DecodeReport* report = nullptr;
    };

    struct CodegenContext {
        int indentSpaces = 0;
        std::string variable;            // "light"
        std::filesystem::path projectPath;
        // where the component goes (Factory::addComponentCode)
        std::string sceneName;
        std::string entityName;
        bool assignExisting = false;
        // the variables of the GameObjects an Entity field may name (Factory::formatEntity)
        const std::unordered_map<Entity, std::string>* entityVarNames = nullptr;
    };

    // How a field's values are stored, compared, written and read. Deduced from the member type
    // (ReflectTraits, Field.h); a field may name other ops for a quirk of its own encoding.
    struct TypeOps {
        const char* typeName;
        Kind kind;
        PropertyType propertyType;
        size_t size;
        void (*construct)(void* at);
        void (*destroy)(void* at);
        void (*copy)(void* dst, const void* src);
        // epsilon > 0: floats within it are equal (the Inspector's mixed values); 0: bitwise
        bool (*equals)(const void* a, const void* b, float epsilon);
        // the YAML node of a value (a null node writes "~")
        YAML::Node (*toYaml)(const FieldDesc& field, const void* value, const EncodeContext& ctx);
        // the value of a node over the base value already in `value`; false (with a reason) when
        // the node holds no value of the type: the caller then treats the key as absent
        bool (*fromYaml)(const FieldDesc& field, const YAML::Node& node, void* value, const DecodeContext& ctx, std::string& reason);
        // a value written as several keys of the parent map (an asset reference and its "<key>Guid");
        // these replace toYaml / fromYaml when set
        void (*encodeKey)(const FieldDesc& field, const void* value, YAML::Node& map, const std::string& key, const EncodeContext& ctx) = nullptr;
        bool (*decodeKey)(const FieldDesc& field, const YAML::Node& map, const std::string& key, void* value, const DecodeContext& ctx, std::string& reason) = nullptr;
        // a value not written at all (non-positive camera field of view...); null: always written
        bool (*skip)(const FieldDesc& field, const void* value) = nullptr;
        // true: fromYaml is called for an absent key too (with an undefined node)
        bool decodeWhenAbsent = false;
        // the C++ expression of a value for the exported code; null: the field is not exported
        std::string (*cppLiteral)(const FieldDesc& field, const void* value, const CodegenContext& ctx) = nullptr;
        // the value's JSON Schema (type, items, enum...) without description
        Json (*schema)(const FieldDesc& field) = nullptr;
        // a value is finite (float kinds; null for the others)
        bool (*finite)(const void* value) = nullptr;
        // the scalar text of a value written as one plain scalar (assigned without building a node)
        std::string (*scalar)(const FieldDesc& field, const void* value) = nullptr;
        // the exported code of a value that is not one assignment (a texture's setters, a list's
        // push_backs), appended to out for `target`; replaces cppLiteral when set
        void (*cppStatements)(const FieldDesc& field, const void* value, const std::string& target, const CodegenContext& ctx,
                              std::string& out) = nullptr;
        // a copy constructed in place (null: construct, then copy); a component whose default
        // constructor allocates (a camera's framebuffer) is copied without allocating
        void (*copyConstruct)(void* at, const void* src) = nullptr;
        // src moved into dst (null: copy); the whole components', so decoding over a stored one
        // does not copy its buffers
        void (*move)(void* dst, void* src) = nullptr;
    };

    // std::vector<T> of any kind
    struct ArrayOps {
        size_t (*size)(const void* array);
        void (*resize)(void* array, size_t size);
        void* (*at)(void* array, size_t index);
        void (*insert)(void* array, size_t index, const void* value);
        void (*erase)(void* array, size_t index);
        void (*move)(void* array, size_t from, size_t to);
        // a component array that grows when an index past its end is written (GrowArray,
        // StableArray): a path a write resolves may name one past its last element, which
        // appends one (resolve's append)
        bool appendsByIndex = false;
    };

    struct FieldDesc {
        const char* name;                 // the property path segment ("shadowBias")
        const char* yamlKey;              // the scene file key (default: name)
        Kind kind;
        const TypeOps* ops;
        void* (*ref)(void* owner);
        const EnumDesc* enumDesc = nullptr;
        const StructDesc* structDesc = nullptr;   // Struct kind, or the elements of an Array of structs
        const ArrayOps* arrayOps = nullptr;
        const TypeOps* elementOps = nullptr;      // the elements of an Array
        uint32_t flags = 0;
        uint64_t updateFlags = 0;
        AssetKind asset = AssetKind::None;
        Unit unit = Unit::None;
        float min = -std::numeric_limits<float>::infinity();    // hard range, enforced on edits
        float max = std::numeric_limits<float>::infinity();
        float uiMin = -std::numeric_limits<float>::infinity();  // slider range only
        float uiMax = std::numeric_limits<float>::infinity();
        float step = 0.0f;
        const char* format = nullptr;
        const char* label = nullptr;      // default: Unity-style from the name
        const char* tooltip = nullptr;
        const char* group = nullptr;
        const char* drawer = nullptr;     // a named drawer ("halfCone", "direction", "po2Slider"...)
        bool (*visibleIf)(const void* owner) = nullptr;
        bool (*enabledIf)(const void* owner) = nullptr;   // drawn disabled while false
        std::span<const char* const> aliases = {};
        int16_t uiOrder = 0;
        const char* cppPath = nullptr;    // the member expression of the exported code (default: name)
        uint64_t enumMask = 0;            // the enum values offered (bit per value; 0: every item)
        // the elements in use of a fixed array whose owner counts them (numFramesRect); null: all
        size_t (*count)(const void* owner) = nullptr;
        // the value read without making it, for a field whose ref makes it (an out-of-line value)
        const void* (*peek)(const void* owner) = nullptr;
        AssetForm assetForm = AssetForm::Deduced;
        // after a moved file rewrote this reference's path, besides updateFlags: what the flags, made
        // for an edit of the same file's value, leave out (null: nothing)
        void (*assetMoved)(void* owner) = nullptr;
        // the elements of an array that hold references, when the list lists them all (a mesh's
        // submesh slots past numSubmeshes hold no reference); null: elementCount
        size_t (*assetCount)(const void* owner) = nullptr;
        // while true the engine recomputes this value: a document and the exported code leave it
        // out, as NotWritten does for a value always computed
        bool (*derivedIf)(const void* owner) = nullptr;
    };

    struct StructDesc {
        const char* name;
        std::span<const FieldDesc> fields;
        // a default-constructed instance (the defaults of OmitIfDefault and of reset)
        const void* (*defaults)();
        const TypeOps* ops;
        // the C++ type ("PBR::Sheen"): the runtime field tables name it (S30 R4)
        const char* cppType = nullptr;
    };

    // A semantic change of a component's YAML: applied to the component node of a file older than
    // toFormatVersion before it decodes (StreamMigrations). No per-component version is written.
    struct Migration {
        int toFormatVersion;
        void (*apply)(YAML::Node& component);
    };

    // A component whose encoding one table cannot express: its own whole-component codec, the
    // table still drives the Inspector, MCP and locators (P4)
    struct ComponentCodec {
        YAML::Node (*encode)(const void* comp, const EncodeContext& ctx);
        // decodes over the base value already in `comp`
        void (*decode)(const YAML::Node& node, void* comp, const DecodeContext& ctx);
    };

    // How a decoded component is stored into its entity
    enum class DecodeApply : uint8_t {
        UpdateFlags,   // the changed fields' update flags run (new: the component's structural flags)
        MarkOnly       // stored and marked changed, no update flags
    };

    struct ComponentDesc {
        ComponentType type;
        const char* cppName;              // "LightComponent"
        const char* key;                  // YAML/MCP key "light"
        const char* displayName;          // "Light"
        const char* icon;
        StructDesc body;
        bool hasEnabled;
        bool structural;                  // its edits change what the Hierarchy shows (fields may override)
        void* (*find)(EntityRegistry* registry, Entity entity);
        void (*add)(EntityRegistry* registry, Entity entity, void* value);   // moves value in
        void (*remove)(EntityRegistry* registry, Entity entity);
        ComponentId (*componentId)(const EntityRegistry* registry);
        const ComponentCodec* codec = nullptr;   // null: the generic YAML codec
        void (*postDecode)(void* comp, const DecodeContext& ctx) = nullptr;
        std::span<const Migration> migrations = {};
        std::span<const char* const> keyAliases = {};
        uint64_t structuralUpdateFlags = 0;      // run when it is added or removed
        DecodeApply decodeApply = DecodeApply::UpdateFlags;
        // the exported code: the variable, the fields in its order (empty: table order), and
        // lines after them; codegen replaces it all (components the table does not describe)
        const char* codegenVar = nullptr;
        std::span<const char* const> codegenOrder = {};
        void (*codegenExtra)(const void* comp, const CodegenContext& ctx, std::string& out) = nullptr;
        std::string (*codegen)(EntityRegistry* registry, Entity entity, const CodegenContext& ctx) = nullptr;
        // the macro an export without that physics backend leaves undefined ("JASPERENGINE_PHYSICS_2D")
        const char* codegenGuard = nullptr;
        // keys of the YAML node the decode reads that are no fields (legacy keys); with a codec
        // of its own, every key it reads
        std::span<const char* const> extraKeys = {};
        // after an Inspector edit of one of its fields (a UI widget redraws itself)
        void (*edited)(EntityRegistry* registry, Entity entity) = nullptr;
        // properties no table describes (a script's properties, a material's shader parameters):
        // the one a path names (no ref: none), and every one of a component (null: none listed)
        PropertyData (*extraProperty)(void* comp, const std::string& path) = nullptr;
        void (*extraProperties)(void* comp, std::map<std::string, PropertyData>& out) = nullptr;
        // the update flags its extra properties can have: none, and changedUpdateFlags does not list them
        uint64_t extraUpdateFlags = ~uint64_t(0);
        // keys only old files have that hold a texture ("textureLayers[]"): read for the dependency
        // index, never written, no field
        std::span<const char* const> legacyTextureKeys = {};
    };

    // ---- registry -----------------------------------------------------------------------

    const ComponentDesc* find(ComponentType type);
    // by key, C++ name or alias, case-insensitive ("light", "LightComponent")
    const ComponentDesc* findByKey(std::string_view name);
    // every component in ComponentType order
    std::span<const ComponentDesc* const> all();
    // every component in the order a scene writes them (the `components` map)
    std::span<const ComponentDesc* const> serializationOrder();
    // every component in the order a GameObject lists them (Catalog::findComponents)
    std::span<const ComponentDesc* const> listingOrder();
    // every enum table, by C++ type name
    std::span<const EnumDesc* const> enumTables();

    // ---- fields -------------------------------------------------------------------------

    struct FieldRef {
        const FieldDesc* field = nullptr;
        void* ref = nullptr;
        void* owner = nullptr;
        const StructDesc* ownerStruct = nullptr;
        // the value's ops: the field's, or its elementOps when the path ends in an index
        const TypeOps* ops = nullptr;
        bool element = false;             // the path ends in an array index ("points[3]")
        bool viaAlias = false;            // a segment was matched by an alias
        explicit operator bool() const { return field && ref; }
    };

    enum class PathError : uint8_t { None, Syntax, UnknownField, IndexOutOfRange, NotIndexable, NotAStruct };

    // seg('.'seg | '['N']')*; aliases are accepted in every segment. No heap allocation.
    // `append`: the caller writes the value, so one past the last element of an array that grows
    // by index appends one; a read never grows the component
    FieldRef resolve(const ComponentDesc& desc, void* comp, std::string_view path, PathError* error = nullptr, bool append = false);
    FieldRef resolve(const StructDesc& desc, void* owner, std::string_view path, PathError* error = nullptr, bool append = false);
    // the path with every alias replaced by its field's name ("" when it does not resolve)
    std::string canonicalPath(const ComponentDesc& desc, void* comp, std::string_view path);
    // the field a path names, without a value (array indices are not checked)
    const FieldDesc* findField(const StructDesc& desc, std::string_view path, bool* viaAlias = nullptr);
    const FieldDesc* findField(const ComponentDesc& desc, std::string_view path, bool* viaAlias = nullptr);
    // one segment of a struct, by name or alias (sorted index, no allocation)
    const FieldDesc* findSegment(const StructDesc& desc, std::string_view name, bool* viaAlias = nullptr);

    // "Shadow Bias": the field's label, or its name Unity-style
    std::string label(const FieldDesc& field);
    // the field's tooltip with its unit and hard range ("Range (m). 0 to 1000000.")
    std::string tooltip(const FieldDesc& field);
    const char* unitSymbol(Unit unit);
    const char* kindName(Kind kind);
    const char* assetKindName(AssetKind kind);
    bool hasRange(const FieldDesc& field);
    bool hasUiRange(const FieldDesc& field);
    // a property of Catalog/MCP (not Derived)
    bool isProperty(const FieldDesc& field);
    // written to and read from YAML
    bool isSaved(const FieldDesc& field);
    // edits change what the Hierarchy shows
    bool isStructural(const ComponentDesc& desc, const FieldDesc& field);
    // the value, owned by `owner` struct of `desc`, of its default instance
    const void* defaultValue(const StructDesc& desc, const FieldDesc& field);

    // ---- enums --------------------------------------------------------------------------

    const EnumItem* enumItem(const EnumDesc& desc, int value);
    // by YAML token, MCP token or alias (normalized when the table says so)
    const EnumItem* enumItemByToken(const EnumDesc& desc, std::string_view token, bool* viaAlias = nullptr);
    // the YAML token of a value (an unlisted value: the fallback's)
    const char* enumToken(const EnumDesc& desc, int value);
    const char* enumMcpToken(const EnumItem& item);
    // the YAML tokens in table order (did-you-mean candidates)
    std::vector<std::string> enumTokens(const EnumDesc& desc);
    bool enumOffered(const FieldDesc& field, const EnumItem& item);
    int readEnum(const FieldDesc& field, const void* value);
    void writeEnum(const FieldDesc& field, void* value, int enumValue);

    // ---- codecs -------------------------------------------------------------------------

    // The component's YAML map, keys in table order (or its codec's encode)
    YAML::Node encode(const ComponentDesc& desc, const void* comp, const EncodeContext& ctx);
    // While one lives on a thread, its encodes are of a document (a scene or prefab file): they
    // leave out the NotWritten fields. Every other encode (undo snapshots, the clipboard) keeps
    // them, as a restore may need them before the next transform pass (S29 R20).
    class DocumentEncodeScope {
    public:
        DocumentEncodeScope();
        ~DocumentEncodeScope();
        DocumentEncodeScope(const DocumentEncodeScope&) = delete;
        DocumentEncodeScope& operator=(const DocumentEncodeScope&) = delete;
        static bool active();
    };
    // While one lives on a thread, a document holds what a frame computes that a file leaves out:
    // the Play snapshot Stop restores (Stream::encodePlaySnapshot)
    class PlaySnapshotEncodeScope {
    public:
        PlaySnapshotEncodeScope();
        ~PlaySnapshotEncodeScope();
        PlaySnapshotEncodeScope(const PlaySnapshotEncodeScope&) = delete;
        PlaySnapshotEncodeScope& operator=(const PlaySnapshotEncodeScope&) = delete;
    };
    // Whether the encode on this thread holds what a frame computes: all but a scene or prefab file
    bool encodesComputedValues();
    YAML::Node encode(const StructDesc& desc, const void* owner, const EncodeContext& ctx);
    // Decodes over the base value already in `comp`: absent keys keep it (OmitIfDefault ones take
    // the default), aliases are read, a value that does not read keeps the base and adds a skip
    // record. Never throws. Unknown keys are left to S03's unknown-data store (the caller).
    void decode(const ComponentDesc& desc, const YAML::Node& node, void* comp, const DecodeContext& ctx, DecodeReport& report);
    void decode(const StructDesc& desc, const YAML::Node& node, void* owner, const DecodeContext& ctx);
    // The keys the decode of a component reads (sorted): fields, their aliases, extra keys
    const std::vector<std::string>& knownKeys(const ComponentDesc& desc);
    const std::vector<std::string>& knownKeys(const StructDesc& desc);
    // Runs the component's migrations newer than the file's format version on its node
    int migrate(const ComponentDesc& desc, YAML::Node& component, int fileVersion);

    // ---- Catalog properties -------------------------------------------------------------

    // The elements in use of an array field (its count, or every element)
    size_t elementCount(const FieldDesc& field, const void* owner, const void* array);
    // The Catalog property a path names: type, update flags, the default instance's value
    // (NoDefault: none) and the value (an empty one when the path names nothing). `append` as
    // for resolve: only a write names one past the last element
    PropertyData property(const ComponentDesc& desc, void* comp, std::string_view path, bool append = false);
    // Every Catalog property of a component (null: of the type, arrays as one template element
    // with no index where the table says so), and its extra properties
    void listProperties(const ComponentDesc& desc, void* comp, std::map<std::string, PropertyData>& out);
    // The update flags of every property listProperties lists in both components and whose value
    // differs (a Custom one always does), read from the components without listing them. Makes nothing in them
    uint64_t changedUpdateFlags(const ComponentDesc& desc, void* oldComp, void* newComp);

    // A component value's YAML by its table (or codec), outside any entity, and a value decoded
    // over `old` (null: a default one) as the scene codec decodes it
    YAML::Node encodeValue(ComponentType type, const void* comp);
    void decodeInto(ComponentType type, const YAML::Node& node, void* comp, const void* old);
    template<typename T>
    T decodeValue(ComponentType type, const YAML::Node& node, const T* old = nullptr) {
        T value = old ? T(*old) : T{};
        decodeInto(type, node, &value, old);
        return value;
    }

    // ---- PostProcessingSettings (R15) ----------------------------------------------------

    // The scene's post-processing settings as reflection fields, from the engine's table
    // (SceneSettings.cpp): YAML key and name the camelCase member, the snake_case Catalog name
    // an alias, every field OmitIfDefault
    const StructDesc& postProcessingStruct();
    std::vector<const EnumDesc*> postProcessingEnums();
    // Project Settings > Graphics > Shader Creation (ShaderCreationProjectSettings, spec S32)
    const StructDesc& shaderCreationStruct();
    // Project Settings > Debug (DebugSettings, feature debug-overlay): the project file's debug
    // block, the names of its values and its settings rows (editor/reflect/DebugReflect.cpp)
    const StructDesc& debugSettingsStruct();
    std::vector<const EnumDesc*> debugSettingsEnums();
    // a field of it by its member name ("toggleKey"), null if none
    const FieldDesc* debugSettingsField(std::string_view name);
    // the rows the struct can hold (a static check of the engine table's size)
    size_t postProcessingFieldCapacity();

    // ---- multi-object editing (the Inspector) ------------------------------------------

    struct Mixed {
        bool any = false;                 // the values differ
        bool lanes[4] = {};               // per lane of a vector or a colour
        bool sizeDiffers = false;         // arrays of different lengths: their size and order read-only
    };
    // The values of a field on several components compared as the Inspector compares them (floats
    // within 1e-4, vectors lane by lane, arrays by length then element by element)
    Mixed mixedValues(const ComponentDesc& desc, const std::vector<void*>& comps, std::string_view path);

    // ---- MCP ----------------------------------------------------------------------------

    // The component's JSON Schema (Draft 2020-12): every property with type, enum tokens,
    // minimum/maximum, default, readOnly, description with unit, x-asset-kind, x-aliases
    Json schema(const ComponentDesc& desc);
    Json fieldSchema(const StructDesc& owner, const FieldDesc& field);
    // get_entity detail "full" metadata of a property: label, unit, range, ui_range, tooltip,
    // read_only, asset_kind, group
    Json fieldMetadata(const FieldDesc& field);
    // jasper://reflection: every component and enum, with schemas and the format version
    Json reflectionDocument();

    // ---- exported code ------------------------------------------------------------------

    // `var.field = <literal>;` per exported field (codegenOrder, then codegenExtra)
    std::string cppAssignments(const ComponentDesc& desc, const void* comp, const CodegenContext& ctx);

    // ---- asset references (S04) ---------------------------------------------------------

    // Its ops hold an asset reference (a Texture, ShaderRef, AssetRef or FontArray)
    bool holdsAsset(const TypeOps* ops);
    // What a field with those ops declares: its .asset, else what the type says
    AssetKind assetKindOf(const FieldDesc& field, const TypeOps* ops);
    using AssetVisitor = void (*)(void* user, const std::string& locator, AssetKind kind, void* ref, const FieldDesc& field);
    // Every asset field below the component (nested structs and arrays): its locator
    // "<componentKey>/<path>" ("mesh/submeshes[2].material.baseColorTexture")
    void visitAssetRefs(const ComponentDesc& desc, void* comp, AssetVisitor visitor, void* user);
    // The field a locator names, through aliases (an empty FieldRef when it does not)
    FieldRef resolveLocator(EntityRegistry* registry, Entity entity, std::string_view locator, const ComponentDesc** desc = nullptr);

    // ---- hot reload (S23) ---------------------------------------------------------------

    // A type-erased value of one field
    class Value {
    public:
        Value() = default;
        Value(const TypeOps* ops, const void* from = nullptr);
        Value(const Value& other);
        Value(Value&& other) noexcept;
        Value& operator=(const Value& other);
        Value& operator=(Value&& other) noexcept;
        ~Value();
        const TypeOps* ops() const { return typeOps; }
        void* get() { return storage; }
        const void* get() const { return storage; }
        bool empty() const { return storage == nullptr; }
    private:
        const TypeOps* typeOps = nullptr;
        void* storage = nullptr;
    };

    struct FieldSet {
        std::vector<const FieldDesc*> fields;
        // the struct the fields are of, for their defaults (null: the value types' own)
        const StructDesc* owner = nullptr;
    };
    using ValueSet = std::vector<Value>;   // parallel to a FieldSet's fields

    struct RemapResult {
        ValueSet values;                    // parallel to the target set
        std::vector<std::string> report;    // "speed: Int is now Vector3, reset to the default"
    };
    // Same name and kind keeps the value; Int<->Float, Vector widening/narrowing and
    // String<->AssetPath convert; anything else (and an added field) takes its default
    RemapResult remapByName(const FieldSet& from, const ValueSet& values, const FieldSet& to);

}
