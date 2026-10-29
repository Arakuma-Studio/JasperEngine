// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RuntimeFieldsEmitter.h"

#include "reflect/Reflect.h"
#include "util/Fnv.h"

#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    const char* kindToken(Kind kind) {
        switch (kind) {
            case Kind::Bool: return "Bool";
            case Kind::Int: return "Int";
            case Kind::UInt: return "UInt";
            case Kind::Float: return "Float";
            case Kind::Double: return "Double";
            case Kind::Vector2: return "Vector2";
            case Kind::Vector3: return "Vector3";
            case Kind::Vector4: return "Vector4";
            case Kind::Quat: return "Quat";
            case Kind::Matrix4: return "Matrix4";
            case Kind::Rect: return "Rect";
            case Kind::String: return "String";
            case Kind::Enum: return "Enum";
            case Kind::LayerMask: return "LayerMask";
            case Kind::Ease: return "Ease";
            case Kind::Entity: return "Entity";
            case Kind::EntityReference: return "EntityReference";
            case Kind::Texture: return "Texture";
            case Kind::Font: return "Font";
            case Kind::Material: return "Material";
            case Kind::AssetPath: return "AssetPath";
            case Kind::Struct: return "Struct";
            case Kind::Array: return "Array";
            case Kind::Custom: return "Custom";
        }
        return "Custom";
    }

    std::string hex32(uint32_t value) {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08xu", value);
        return text;
    }

    // a C++ identifier for a type name ("PBR::Sheen" -> "PBR_Sheen")
    std::string identifierOf(const std::string& type) {
        std::string out;
        for (char c : type) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        return out;
    }

    struct Emitter {
        std::vector<std::string>& errors;
        // struct tables in dependency order (a nested struct before its users), and the
        // physics guard of each (empty: none; "*" mixed users, so none)
        std::vector<const StructDesc*> order;
        std::map<std::string, std::string> guards;
        std::set<std::string> visiting;
        std::map<std::string, const StructDesc*> byType;

        // the type a field's elements are when it holds structs
        const StructDesc* nestedStruct(const FieldDesc& field) {
            if (field.structDesc && (field.kind == Kind::Struct || field.kind == Kind::Material || field.kind == Kind::Array ||
                                     field.kind == Kind::Rect || field.kind == Kind::Custom))
                return field.structDesc;
            return nullptr;
        }

        void visit(const StructDesc& desc, const std::string& guard) {
            // values with a codec of their own (engine/core/serialization/CookCodecs.h)
            static const std::set<std::string> handWritten = {"Rect"};
            if (desc.cppType && handWritten.count(desc.cppType)) return;
            if (!desc.cppType) {
                errors.push_back(std::string("struct ") + desc.name + " has no C++ type (JE_STRUCT_DESC)");
                return;
            }
            const std::string type = desc.cppType;
            auto known = guards.find(type);
            if (known != guards.end()) {
                if (known->second != guard) known->second = "*";
                return;
            }
            if (!visiting.insert(type).second) return;
            for (const FieldDesc& field : desc.fields) {
                if (!isSaved(field)) continue;
                if (const StructDesc* nested = nestedStruct(field)) visit(*nested, guard);
            }
            visiting.erase(type);
            guards[type] = guard;
            byType[type] = &desc;
            order.push_back(&desc);
        }

        std::string fieldLine(const std::string& ownerName, const std::string& ownerType, const FieldDesc& field) {
            const std::string member = field.cppPath ? field.cppPath : field.name;
            if (member.find_first_of(".[(-") != std::string::npos) {
                errors.push_back(ownerType + "." + field.name + ": member path \"" + member + "\" is not one member");
                return "";
            }
            const uint32_t id = cook::runtimeFieldId(ownerName, field.yamlKey ? field.yamlKey : field.name);
            std::string macro = "JE_RF";
            if (field.kind == Kind::Entity) macro = "JE_RF_ENTITY";
            else if (field.kind == Kind::Array && field.elementOps && field.elementOps->kind == Kind::Entity) macro = "JE_RF_ENTITIES";
            return "    " + macro + "(" + ownerType + ", " + member + ", " + hex32(id) + ", " + kindToken(field.kind) + "),\n";
        }

        std::string tableOf(const std::string& ownerName, const std::string& ownerType, std::span<const FieldDesc> fields,
                            const std::string& tableName) {
            std::string lines;
            for (const FieldDesc& field : fields) {
                if (!isSaved(field)) continue;
                lines += fieldLine(ownerName, ownerType, field);
            }
            std::string out;
            if (lines.empty()) {
                out += "const StructTable " + tableName + "{\"" + ownerName + "\", nullptr, 0};\n";
            } else {
                out += "const FieldEntry " + tableName + "_fields[] = {\n" + lines + "};\n";
                out += "const StructTable " + tableName + "{\"" + ownerName + "\", " + tableName + "_fields, std::size(" + tableName + "_fields)};\n";
            }
            return out;
        }
    };

    std::string guardOpen(const std::string& guard) {
        return (guard.empty() || guard == "*") ? "" : "#ifdef " + guard + "\n";
    }

    std::string guardClose(const std::string& guard) {
        return (guard.empty() || guard == "*") ? "" : "#endif // " + guard + "\n";
    }

}

uint32_t editor::cook::runtimeFieldId(const std::string& owner, const std::string& yamlKey) {
    return fnv1a32(owner + "." + yamlKey);
}

std::string editor::cook::emitRuntimeFields(std::vector<std::string>& errors) {
    Emitter emitter{errors, {}, {}, {}, {}};
    for (const ComponentDesc* desc : reflect::all()) {
        const std::string guard = desc->codegenGuard ? desc->codegenGuard : "";
        for (const FieldDesc& field : desc->body.fields) {
            if (!isSaved(field)) continue;
            if (const StructDesc* nested = emitter.nestedStruct(field)) emitter.visit(*nested, guard);
        }
    }
    // the scene's post-processing settings (the scene record)
    emitter.visit(reflect::postProcessingStruct(), "");

    std::string out;
    out += "// Generated by `jasperengine-editor reflect --emit-runtime-fields` from the editor's reflection\n";
    out += "// tables (docs/specs/S30-shipping.md R4). Do not edit: CI regenerates it and fails on a difference.\n";
    out += "// RuntimeFields.h includes it for the declarations, RuntimeFields.cpp for the definitions.\n\n";

    out += "#if defined(JE_RUNTIME_FIELDS_DECLARE)\n\n";
    out += "namespace jasperengine::cook {\n\n";
    for (const StructDesc* desc : emitter.order) {
        const std::string type = desc->cppType;
        out += guardOpen(emitter.guards[type]);
        out += "template<> struct Codec<" + type + "> {\n";
        out += "    static void write(Writer& w, const " + type + "& value);\n";
        out += "    static void read(Reader& r, " + type + "& value);\n";
        out += "};\n";
        out += "template<> struct TextureVisit<" + type + "> {\n";
        out += "    static void visit(" + type + "& value, const TextureVisitor& f);\n";
        out += "};\n";
        out += guardClose(emitter.guards[type]);
    }
    out += "\n}\n\n#endif // JE_RUNTIME_FIELDS_DECLARE\n\n";

    out += "#if defined(JE_RUNTIME_FIELDS_DEFINE)\n\n";
    out += "namespace jasperengine::cook {\n\n";
    for (const StructDesc* desc : emitter.order) {
        const std::string type = desc->cppType;
        const std::string table = "kStruct_" + identifierOf(type);
        out += guardOpen(emitter.guards[type]);
        out += emitter.tableOf(desc->name, type, desc->fields, table);
        out += "void Codec<" + type + ">::write(Writer& w, const " + type + "& value) { writeStruct(w, " + table + ", &value); }\n";
        out += "void Codec<" + type + ">::read(Reader& r, " + type + "& value) { readStruct(r, " + table + ", &value); }\n";
        out += "void TextureVisit<" + type + ">::visit(" + type + "& value, const TextureVisitor& f) { visitStructTextures(" + table + ", &value, f); }\n";
        out += guardClose(emitter.guards[type]);
        out += "\n";
    }
    for (const ComponentDesc* desc : reflect::all()) {
        const std::string guard = desc->codegenGuard ? desc->codegenGuard : "";
        out += guardOpen(guard);
        out += emitter.tableOf(desc->key, desc->cppName, desc->body.fields, std::string("kBody_") + desc->cppName);
        out += guardClose(guard);
        out += "\n";
    }
    out += "const ComponentTable kComponentTables[] = {\n";
    for (const ComponentDesc* desc : reflect::all()) {
        const std::string guard = desc->codegenGuard ? desc->codegenGuard : "";
        out += guardOpen(guard);
        out += std::string("    JE_RF_COMPONENT(") + desc->cppName + ", \"" + desc->key + "\", " + hex32(fnv1a32(desc->key)) + "),\n";
        out += guardClose(guard);
    }
    out += "    JE_RF_COMPONENTS_END\n";
    out += "};\n\n}\n\n#endif // JE_RUNTIME_FIELDS_DEFINE\n";
    return out;
}
