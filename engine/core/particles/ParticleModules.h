// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEMODULES_H
#define PARTICLES_PARTICLEMODULES_H

// The module registry (docs/specs/S20-particles.md D10): one descriptor per module, with the
// table of its fields. The JSON codec, the editor's module inspector, MCP patch validation and
// the Lua exposed parameters all walk these tables, so a field is declared once (S08's
// reflection tables will adopt the same shape).

#include "Export.h"
#include "particles/ParticleEffect.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine { namespace particles {

    // Stable ids: they seed the per-module random streams, so a value is part of the format
    enum class ModuleId : uint8_t {
        Main = 0,
        Emission = 1,
        Shape = 2,
        StartVelocity = 3,
        VelocityOverLifetime = 4,
        LimitVelocity = 5,
        ForceOverLifetime = 6,
        Noise = 7,
        Orbit = 8,
        ExternalForces = 9,
        ColorOverLifetime = 10,
        ColorBySpeed = 11,
        SizeOverLifetime = 12,
        SizeBySpeed = 13,
        RotationOverLifetime = 14,
        RotationBySpeed = 15,
        TextureSheetAnimation = 16,
        Collision = 17,
        Trails = 18,
        SubEmitters = 19,
        CustomData = 20,
        Renderer = 21,
        SetOverAge = 22,
        PixelArt = 23,      // S49
        Count = 24
    };

    enum class FieldType : uint8_t {
        Bool,
        Int,
        UInt,
        Float,
        Enum,           // a uint8_t enum class
        Mask,           // uint32_t bits (layers)
        Vector2,
        Vector3,
        Vector4,
        Color,          // Vector4, sRGB rgb + alpha
        String,
        MinMaxCurve,
        MinMaxGradient,
        Curve,
        Gradient,
        Struct,         // a nested struct (MeshSource)
        Array,          // a std::vector of structs
        IntArray,       // std::vector<int>
        StringArray     // std::vector<std::string>
    };

    enum FieldFlags : uint16_t {
        FieldFlag_None = 0,
        // kept for old data, not shown in the inspector
        FieldFlag_Hidden = 1 << 1,
        // a project-relative asset path; the field after it (FieldFlag_Guid) holds its GUID
        FieldFlag_AssetPath = 1 << 2,
        FieldFlag_Guid = 1 << 3,
        FieldFlag_Degrees = 1 << 4,
        // shown in the inspector's advanced foldout
        FieldFlag_Advanced = 1 << 5
    };

    struct EnumOption {
        const char* json;      // "stretchedBillboard"
        const char* label;     // "Stretched Billboard"
        uint8_t value;
    };

    struct StructDesc;

    struct ArrayOps {
        size_t (*size)(const void* vector);
        void (*resize)(void* vector, size_t count);
        void* (*at)(void* vector, size_t index);
        void (*erase)(void* vector, size_t index);
    };

    struct FieldDesc {
        const char* key = "";
        const char* label = "";
        FieldType type = FieldType::Float;
        void* (*get)(void* owner) = nullptr;
        float minValue = -3.0e38f;
        float maxValue = 3.0e38f;
        const EnumOption* options = nullptr;
        uint8_t optionCount = 0;
        const StructDesc* element = nullptr;    // Struct, Array
        ArrayOps array{};                       // Array, IntArray, StringArray
        uint32_t maxCount = 0;                  // arrays: most elements (0: no limit)
        uint16_t flags = FieldFlag_None;
        // shown and validated only when it returns true (startSizeY only with startSize3D)
        bool (*visible)(const void* owner) = nullptr;
        const char* tooltip = nullptr;

        const void* field(const void* owner) const { return get(const_cast<void*>(owner)); }
        void* field(void* owner) const { return get(owner); }
        const EnumOption* findOption(std::string_view json) const;
        const EnumOption* findOption(uint8_t value) const;
    };

    struct StructDesc {
        const char* name = "";
        std::vector<FieldDesc> fields;
        // a default-constructed instance: the codec leaves out fields equal to it
        const void* defaults = nullptr;
        const FieldDesc* find(std::string_view key) const;
    };

    struct ModuleDesc {
        ModuleId id = ModuleId::Main;
        const char* key = "";          // JSON "type" and the path prefix: "limitVelocity"
        const char* label = "";        // "Limit Velocity over Lifetime"
        bool alwaysPresent = false;    // Main and Renderer
        bool hasEnable = true;         // Main has no checkbox
        void* (*get)(ParticleEffectDocument& document) = nullptr;
        bool* (*present)(void* module) = nullptr;
        bool* (*enabled)(void* module) = nullptr;
        StructDesc fields;
        const char* tooltip = nullptr;

        void* module(ParticleEffectDocument& document) const { return get(document); }
        const void* module(const ParticleEffectDocument& document) const { return get(const_cast<ParticleEffectDocument&>(document)); }
        bool isPresent(const ParticleEffectDocument& document) const;
        bool isEnabled(const ParticleEffectDocument& document) const;
        void setPresent(ParticleEffectDocument& document, bool value) const;
        void setEnabled(ParticleEffectDocument& document, bool value) const;
    };

    class JASPERENGINE_API ParticleModules {
    public:
        // every module, in the inspector's (and the JSON's) order
        static const std::vector<ModuleDesc>& all();
        static const ModuleDesc* find(std::string_view key);
        static const ModuleDesc& get(ModuleId id);
        // the document's own fields (duration, looping, ...), and those of its lists
        static const StructDesc& documentFields();
        static const StructDesc& subEmitterFields();
        static const StructDesc& exposedFields();
        static const StructDesc& triggerFields();

        // Resolves "emission.rateOverTime.constant" (a module key, then field keys; array
        // elements as "bursts[0]") to the field and its storage in `document`
        struct Resolved {
            const FieldDesc* field = nullptr;
            void* storage = nullptr;
            const ModuleDesc* module = nullptr;
            // the trailing member of a MinMaxCurve / MinMaxGradient ("constant", "color", ...)
            std::string member;
        };
        static Resolved resolve(ParticleEffectDocument& document, std::string_view path);

        // An exposed parameter's value (Lua setFloat / setVector3 / setColor, the component's
        // exposed values): Float in x, Vector3 in xyz, Color in rgba (sRGB). A MinMaxCurve without
        // a member takes it as its constant (both constants in Constant mode) or, as a curve, as
        // its multiplier; a MinMaxGradient without a member as its colour. False when the
        // document has no such parameter or its field does not take the value.
        static bool setExposed(ParticleEffectDocument& document, std::string_view name, const Vector4& value);
        // what setExposed writes into a curve or colour value, by member ("" for the value as above)
        static bool writeExposedMember(MinMaxCurve& value, const std::string& member, float x);
        static bool writeExposedMember(MinMaxGradient& value, const std::string& member, const Vector4& color);

        // Every asset path the document holds (textures, models, child effects; FieldFlag_AssetPath)
        // with its GUID field, in modules present or not: what an editor resolves, renames and
        // exports. The visitor may change both. A Ramp gradient's palette comes as a Palette with
        // no GUID (an empty string the visitor may write to, kept nowhere).
        enum class AssetKind : uint8_t { Texture, Model, Effect, SoundEvent, Palette };
        using AssetVisitor = std::function<void(std::string& path, std::string& guid, AssetKind kind, const std::string& field)>;
        static void visitAssets(ParticleEffectDocument& document, const AssetVisitor& visit);
        static bool getExposed(const ParticleEffectDocument& document, std::string_view name, Vector4& value);

        // Enum tables shared with the editor (names in JSON and in the UI)
        static const std::vector<EnumOption>& curveModes();
        // MinMaxCurve's Ease presets (EaseType values)
        static const std::vector<EnumOption>& easeTypes();
        static const std::vector<EnumOption>& gradientModes();
        static const std::vector<EnumOption>& curveDrivers();
        static const std::vector<EnumOption>& curveWraps();
        static const std::vector<EnumOption>& gradientBlendModes();
    };

}}

#endif // PARTICLES_PARTICLEMODULES_H
