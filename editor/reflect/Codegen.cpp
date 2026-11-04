// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The exported code of a component (docs/specs/S08-reflection.md R14): `var.field = <literal>;`
// for every saved field without NoCodegen (proxies give their own lanes), enums by their C++
// names, in the component's codegen order.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include "Factory.h"


using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    bool exported(const FieldDesc& field) {
        if (field.flags & (NoCodegen | Transient)) return false;
        // runtime state is not exported (but what the exported code sets anyway); a proxy is a
        // lane of a saved field
        if ((field.flags & NoSave) && !(field.flags & (Proxy | CodegenRuntime))) return false;
        return true;
    }

    void emit(const StructDesc& desc, const FieldDesc& field, const void* owner, const CodegenContext& ctx, std::string& out) {
        if (!exported(field) || (field.derivedIf && field.derivedIf(owner))) return;
        const void* value = field.ref(const_cast<void*>(owner));
        if (field.flags & CodegenIfChanged) {
            const void* def = defaultValue(desc, field);
            if (def && field.ops->equals && field.ops->equals(value, def, 0.0f)) return;
        }
        const std::string target = ctx.variable + "." + (field.cppPath ? field.cppPath : field.name);
        if (field.ops->cppStatements) {
            field.ops->cppStatements(field, value, target, ctx, out);
            return;
        }
        if (!field.ops->cppLiteral) return;
        out += Factory::indentation(ctx.indentSpaces) + target + " = " + field.ops->cppLiteral(field, value, ctx) + ";\n";
    }

}

std::string reflect::cppAssignments(const ComponentDesc& desc, const void* comp, const CodegenContext& ctx) {
    std::string out;
    if (desc.codegenOrder.empty()) {
        for (const FieldDesc& field : desc.body.fields) emit(desc.body, field, comp, ctx, out);
    } else {
        for (const char* name : desc.codegenOrder) {
            if (const FieldDesc* field = findSegment(desc.body, name)) emit(desc.body, *field, comp, ctx, out);
        }
    }
    if (desc.codegenExtra) desc.codegenExtra(comp, ctx, out);
    return out;
}
