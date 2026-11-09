// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PostProcessingSettings on a StructDesc (docs/specs/S08-reflection.md R15): its engine field
// table (SceneSettings.cpp: the names, types, ranges and help the runtime's clamp also reads)
// seen as reflection fields, so the scene codec writes and reads it with the generic codec and
// jasper://reflection describes it. Built once from the engine table: a field added there is
// reflected here without an edit.

#include "reflect/Reflect.h"
#include "reflect/Field.h"
#include "reflect/TypeOps.h"

#include "SceneSettings.h"

#include <array>
#include <cctype>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    // the engine table's accessors behind FieldDesc's void* thunks, one per row
    constexpr size_t kMaxPostProcessingFields = 192;

    template<size_t I>
    void* postProcessingRef(void* owner) {
        return PostProcessingSettings::getFields()[I].ref(*static_cast<PostProcessingSettings*>(owner));
    }

    template<size_t... I>
    constexpr std::array<void* (*)(void*), sizeof...(I)> refThunks(std::index_sequence<I...>) {
        return {&postProcessingRef<I>...};
    }

    constexpr std::array<void* (*)(void*), kMaxPostProcessingFields> kRefs = refThunks(std::make_index_sequence<kMaxPostProcessingFields>());

    // non-finite floats keep the current value (as the hand-written decode's decodeFinite)
    const TypeOps& opsOf(PostProcessingSettings::FieldType type) {
        using FT = PostProcessingSettings::FieldType;
        switch (type) {
            case FT::Bool: return ops::Bool;
            case FT::Int: return ops::Int;
            case FT::Float: return ops::FiniteFloat;
            case FT::Vector3: return ops::Vector3;
            case FT::Enum: return ops::Enum<int>;
            case FT::String: return ops::String;
            case FT::Asset: return ops::AssetRef;
        }
        return ops::Int;
    }

    // "academy_137" -> "Academy 137"
    std::string labelOf(const char* token) {
        std::string label;
        bool start = true;
        for (const char* c = token; *c; c++) {
            if (*c == '_') {
                label += ' ';
                start = true;
                continue;
            }
            label += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(*c))) : *c;
            start = false;
        }
        return label;
    }

    struct Tables {
        std::vector<FieldDesc> fields;
        // the storage the fields and enums point into (stable addresses)
        std::deque<std::string> strings;
        std::deque<std::vector<EnumItem>> items;
        std::deque<EnumDesc> enums;
        std::deque<std::array<const char*, 1>> aliases;
        StructDesc desc{};
    };

    const char* keep(Tables& tables, std::string text) {
        tables.strings.push_back(std::move(text));
        return tables.strings.back().c_str();
    }

    const Tables& tables() {
        static const Tables* built = []() {
            auto* t = new Tables();
            const std::vector<PostProcessingSettings::Field>& engine = PostProcessingSettings::getFields();
            for (size_t i = 0; i < engine.size() && i < kMaxPostProcessingFields; i++) {
                const PostProcessingSettings::Field& source = engine[i];
                FieldDesc field{source.name, source.name, Kind::Custom, &opsOf(source.type), kRefs[i]};
                field.kind = field.ops->kind;
                // written only away from its default: older scenes re-save unchanged
                field.flags = OmitIfDefault;
                // the asset fields: the LUT's texture, Palette Quantize's palette
                if (source.type == PostProcessingSettings::FieldType::Asset)
                    field.asset = std::string(source.name) == "palette" ? AssetKind::Palette : AssetKind::Texture;
                if (source.minValue < source.maxValue &&
                        (source.type == PostProcessingSettings::FieldType::Int || source.type == PostProcessingSettings::FieldType::Float ||
                         source.type == PostProcessingSettings::FieldType::Vector3)) {
                    field.min = source.minValue;
                    field.max = source.maxValue;
                }
                field.tooltip = source.help;
                // the Catalog and MCP name it in snake_case ("post_processing.color_grading_enabled")
                if (std::string(source.snakeName) != source.name) {
                    t->aliases.push_back({source.snakeName});
                    field.aliases = std::span<const char* const>(t->aliases.back());
                }
                if (source.type == PostProcessingSettings::FieldType::Enum && source.enumNames) {
                    std::vector<EnumItem>& items = t->items.emplace_back();
                    for (int value = 0; source.enumNames[value]; value++) {
                        const char* cppName = keep(*t, std::string(source.enumType) + "::" + source.enumValues[value]);
                        items.push_back({value, source.enumNames[value], keep(*t, labelOf(source.enumNames[value])), cppName});
                    }
                    t->enums.push_back(EnumDesc{source.enumType, items, 0});
                    field.enumDesc = &t->enums.back();
                }
                t->fields.push_back(field);
            }
            t->desc = StructDesc{"PostProcessingSettings", t->fields, &defaultInstance<PostProcessingSettings>,
                                 &ops::Value<PostProcessingSettings>, "PostProcessingSettings"};
            return t;
        }();
        return *built;
    }

}

const StructDesc& reflect::postProcessingStruct() {
    return tables().desc;
}

std::vector<const EnumDesc*> reflect::postProcessingEnums() {
    std::vector<const EnumDesc*> out;
    for (const EnumDesc& desc : tables().enums) out.push_back(&desc);
    return out;
}

size_t reflect::postProcessingFieldCapacity() {
    return kMaxPostProcessingFields;
}
