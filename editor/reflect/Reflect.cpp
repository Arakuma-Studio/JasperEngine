// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "reflect/Reflect.h"

#include "command/CommandNames.h"

#include <cmath>
#include <cstring>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

// ---------------------------------------------------------------- fields

std::string reflect::label(const FieldDesc& field) {
    if (field.label) return field.label;
    return CommandNames::property(field.name);
}

const char* reflect::unitSymbol(Unit unit) {
    switch (unit) {
        case Unit::None: return "";
        case Unit::Meters: return "m";
        case Unit::Pixels: return "px";
        case Unit::Degrees: return "\xC2\xB0";
        case Unit::Radians: return "rad";
        case Unit::Seconds: return "s";
        case Unit::Percent: return "%";
        case Unit::Lux: return "lx";
        case Unit::Lumens: return "lm";
        case Unit::Candela: return "cd";
        case Unit::Kelvin: return "K";
        case Unit::Hertz: return "Hz";
        case Unit::Decibels: return "dB";
        case Unit::MetersPerSecond: return "m/s";
        case Unit::Kilograms: return "kg";
        case Unit::Newtons: return "N";
    }
    return "";
}

bool reflect::hasRange(const FieldDesc& field) {
    return std::isfinite(field.min) || std::isfinite(field.max);
}

bool reflect::hasUiRange(const FieldDesc& field) {
    return std::isfinite(field.uiMin) && std::isfinite(field.uiMax);
}

namespace {

    std::string number(float value) {
        std::ostringstream out;
        out << value;
        return out.str();
    }

}

std::string reflect::tooltip(const FieldDesc& field) {
    std::string text = field.tooltip ? field.tooltip : "";
    const char* unit = unitSymbol(field.unit);
    if (*unit) {
        if (!text.empty()) text += " ";
        text += std::string("(") + unit + ")";
    }
    if (hasRange(field)) {
        if (!text.empty()) text += " ";
        if (std::isfinite(field.min) && std::isfinite(field.max)) {
            text += "Range " + number(field.min) + " to " + number(field.max) + ".";
        } else if (std::isfinite(field.min)) {
            text += "At least " + number(field.min) + ".";
        } else {
            text += "At most " + number(field.max) + ".";
        }
    }
    return text;
}

const char* reflect::kindName(Kind kind) {
    switch (kind) {
        case Kind::Bool: return "bool";
        case Kind::Int: return "int";
        case Kind::UInt: return "uint";
        case Kind::Float: return "float";
        case Kind::Double: return "double";
        case Kind::Vector2: return "vector2";
        case Kind::Vector3: return "vector3";
        case Kind::Vector4: return "vector4";
        case Kind::Quat: return "quat";
        case Kind::Matrix4: return "matrix4";
        case Kind::Rect: return "rect";
        case Kind::String: return "string";
        case Kind::Enum: return "enum";
        case Kind::LayerMask: return "layer_mask";
        case Kind::Ease: return "ease";
        case Kind::Entity: return "entity";
        case Kind::EntityReference: return "entity_reference";
        case Kind::Texture: return "texture";
        case Kind::Font: return "font";
        case Kind::Material: return "material";
        case Kind::AssetPath: return "asset";
        case Kind::Struct: return "struct";
        case Kind::Array: return "array";
        case Kind::Custom: return "custom";
    }
    return "custom";
}

const char* reflect::assetKindName(AssetKind kind) {
    switch (kind) {
        case AssetKind::None: return "";
        case AssetKind::Texture: return "texture";
        case AssetKind::Cubemap: return "cubemap";
        case AssetKind::Font: return "font";
        case AssetKind::Material: return "material";
        case AssetKind::Model: return "model";
        case AssetKind::Audio: return "audio";
        case AssetKind::Script: return "script";
        case AssetKind::Prefab: return "prefab";
        case AssetKind::Shader: return "shader";
        case AssetKind::UIDocument: return "ui_document";
        case AssetKind::Scene: return "scene";
        case AssetKind::Any: return "any";
        case AssetKind::PoseAnimation: return "pose_animation";
        case AssetKind::AnimationClip: return "animation_clip";
        case AssetKind::VertexAnimation: return "vertex_animation";
        case AssetKind::AnimatorController: return "animator_controller";
        case AssetKind::AvatarMask: return "avatar_mask";
        case AssetKind::SoundEvent: return "sound_event";
        case AssetKind::NavMesh: return "navmesh";
        case AssetKind::NavGrid: return "navgrid";
        case AssetKind::ParticleEffect: return "particle_effect";
        case AssetKind::ModelOrPrefab: return "model_or_prefab";
        case AssetKind::Palette: return "palette";
        case AssetKind::TitleArt: return "title_art";
    }
    return "";
}

bool reflect::isProperty(const FieldDesc& field) {
    return !(field.flags & Derived);
}

bool reflect::isSaved(const FieldDesc& field) {
    return !(field.flags & (NoSave | Proxy | Transient));
}

bool reflect::isStructural(const ComponentDesc& desc, const FieldDesc& field) {
    if (field.flags & Structural) return true;
    if (field.flags & NonStructural) return false;
    return desc.structural;
}

const void* reflect::defaultValue(const StructDesc& desc, const FieldDesc& field) {
    if (!desc.defaults) return nullptr;
    return field.ref(const_cast<void*>(desc.defaults()));
}

// ---------------------------------------------------------------- enums

const EnumItem* reflect::enumItem(const EnumDesc& desc, int value) {
    for (const EnumItem& item : desc.items) {
        if (item.value == value) return &item;
    }
    return nullptr;
}

const EnumItem* reflect::enumItemByToken(const EnumDesc& desc, std::string_view token, bool* viaAlias) {
    std::string normalized;
    if (desc.normalize) {
        normalized = desc.normalize(token);
        token = normalized;
    }
    for (const EnumItem& item : desc.items) {
        if (token == item.token) {
            if (viaAlias) *viaAlias = false;
            return &item;
        }
    }
    for (const EnumItem& item : desc.items) {
        if (item.mcpToken && token == item.mcpToken) {
            if (viaAlias) *viaAlias = false;
            return &item;
        }
    }
    for (const EnumItem& item : desc.items) {
        for (const char* alias : item.aliases) {
            if (token == alias) {
                if (viaAlias) *viaAlias = true;
                return &item;
            }
        }
    }
    return nullptr;
}

const char* reflect::enumToken(const EnumDesc& desc, int value) {
    if (const EnumItem* item = enumItem(desc, value)) return item->token;
    if (const EnumItem* fallback = enumItem(desc, desc.fallback)) return fallback->token;
    return desc.items.empty() ? "" : desc.items.front().token;
}

const char* reflect::enumMcpToken(const EnumItem& item) {
    return item.mcpToken ? item.mcpToken : item.token;
}

std::vector<std::string> reflect::enumTokens(const EnumDesc& desc) {
    std::vector<std::string> tokens;
    tokens.reserve(desc.items.size());
    for (const EnumItem& item : desc.items) tokens.emplace_back(item.token);
    return tokens;
}

bool reflect::enumOffered(const FieldDesc& field, const EnumItem& item) {
    if (item.hidden) return false;
    if (!field.enumMask) return true;
    return item.value >= 0 && item.value < 64 && (field.enumMask & (uint64_t(1) << item.value));
}

int reflect::readEnum(const FieldDesc& field, const void* value) {
    switch (field.ops->size) {
        case 1: { int8_t v; std::memcpy(&v, value, 1); return v; }
        case 2: { int16_t v; std::memcpy(&v, value, 2); return v; }
        case 8: { int64_t v; std::memcpy(&v, value, 8); return static_cast<int>(v); }
        default: { int32_t v; std::memcpy(&v, value, 4); return v; }
    }
}

void reflect::writeEnum(const FieldDesc& field, void* value, int enumValue) {
    switch (field.ops->size) {
        case 1: { const int8_t v = static_cast<int8_t>(enumValue); std::memcpy(value, &v, 1); break; }
        case 2: { const int16_t v = static_cast<int16_t>(enumValue); std::memcpy(value, &v, 2); break; }
        case 8: { const int64_t v = enumValue; std::memcpy(value, &v, 8); break; }
        default: { const int32_t v = enumValue; std::memcpy(value, &v, 4); break; }
    }
}

// ---------------------------------------------------------------- values

Value::Value(const TypeOps* ops, const void* from): typeOps(ops) {
    if (!typeOps) return;
    storage = ::operator new(typeOps->size, std::align_val_t(alignof(std::max_align_t)));
    if (from && typeOps->copyConstruct) {
        typeOps->copyConstruct(storage, from);
        return;
    }
    typeOps->construct(storage);
    if (from) typeOps->copy(storage, from);
}

Value::Value(const Value& other): Value(other.typeOps, other.storage) {
}

Value::Value(Value&& other) noexcept: typeOps(other.typeOps), storage(other.storage) {
    other.typeOps = nullptr;
    other.storage = nullptr;
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        this->~Value();
        typeOps = other.typeOps;
        storage = other.storage;
        other.typeOps = nullptr;
        other.storage = nullptr;
    }
    return *this;
}

Value::~Value() {
    if (storage) {
        typeOps->destroy(storage);
        ::operator delete(storage, std::align_val_t(alignof(std::max_align_t)));
        storage = nullptr;
    }
}
