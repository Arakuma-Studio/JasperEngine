// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Values across a change of fields (docs/specs/S08-reflection.md R13, hot reload of scripts,
// S23): a field keeps its value when one of the same name and kind was there; a few kind
// changes convert; anything else takes the default and is reported.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    bool isVector(Kind kind) {
        return kind == Kind::Vector2 || kind == Kind::Vector3 || kind == Kind::Vector4;
    }

    size_t lanes(Kind kind) {
        return kind == Kind::Vector2 ? 2 : kind == Kind::Vector3 ? 3 : 4;
    }

    bool isAsset(const TypeOps* ops) {
        return ops->propertyType == PropertyType::Asset;
    }

    // `to` from `from` of another kind; false when they do not convert
    bool convert(const FieldDesc& source, const void* from, const FieldDesc& target, void* to) {
        const Kind a = source.ops->kind;
        const Kind b = target.ops->kind;
        if (a == Kind::Int && b == Kind::Float) {
            *static_cast<float*>(to) = static_cast<float>(*static_cast<const int*>(from));
            return true;
        }
        if (a == Kind::Float && b == Kind::Int) {
            const float value = *static_cast<const float*>(from);
            *static_cast<int*>(to) = std::isfinite(value) ? static_cast<int>(std::lround(value)) : 0;
            return true;
        }
        if (a == Kind::Int && b == Kind::Double) {
            *static_cast<double*>(to) = *static_cast<const int*>(from);
            return true;
        }
        if (a == Kind::Float && b == Kind::Double) {
            *static_cast<double*>(to) = *static_cast<const float*>(from);
            return true;
        }
        if (a == Kind::Double && b == Kind::Float) {
            *static_cast<float*>(to) = static_cast<float>(*static_cast<const double*>(from));
            return true;
        }
        if (isVector(a) && isVector(b)) {
            // widening fills the new lanes with 0 (a Vector4's w with 1), narrowing drops lanes
            float values[4] = {0.0f, 0.0f, 0.0f, b == Kind::Vector4 ? 1.0f : 0.0f};
            const float* in = static_cast<const float*>(from);
            for (size_t i = 0; i < lanes(a) && i < 4; i++) values[i] = in[i];
            float* out = static_cast<float*>(to);
            for (size_t i = 0; i < lanes(b); i++) out[i] = values[i];
            return true;
        }
        if (source.ops->propertyType == PropertyType::String && isAsset(target.ops)) {
            *static_cast<AssetRef*>(to) = *static_cast<const std::string*>(from);
            return true;
        }
        if (isAsset(source.ops) && target.ops->propertyType == PropertyType::String) {
            *static_cast<std::string*>(to) = static_cast<const AssetRef*>(from)->path;
            return true;
        }
        return false;
    }

}

RemapResult reflect::remapByName(const FieldSet& from, const ValueSet& values, const FieldSet& to) {
    RemapResult result;
    result.values.reserve(to.fields.size());
    for (const FieldDesc* target : to.fields) {
        Value value(target->ops, to.owner ? defaultValue(*to.owner, *target) : nullptr);
        const FieldDesc* source = nullptr;
        size_t sourceIndex = 0;
        for (size_t i = 0; i < from.fields.size(); i++) {
            if (std::string_view(from.fields[i]->name) == target->name) {
                source = from.fields[i];
                sourceIndex = i;
                break;
            }
        }
        const bool hasValue = source && sourceIndex < values.size() && !values[sourceIndex].empty();
        if (!hasValue) {
            if (!source) result.report.push_back(std::string(target->name) + ": new, takes its default");
        } else if (source->ops == target->ops ||
                   (source->ops->kind == target->ops->kind && source->ops->size == target->ops->size &&
                    source->ops->propertyType == target->ops->propertyType &&
                    (source->ops->kind != Kind::Enum || source->enumDesc == target->enumDesc))) {
            target->ops->copy(value.get(), values[sourceIndex].get());
        } else if (!convert(*source, values[sourceIndex].get(), *target, value.get())) {
            result.report.push_back(std::string(target->name) + ": " + kindName(source->ops->kind) + " is now " +
                                    kindName(target->ops->kind) + ", reset to its default");
        }
        result.values.push_back(std::move(value));
    }
    return result;
}
