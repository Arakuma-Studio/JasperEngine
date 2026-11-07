// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Paths into components (docs/specs/S08-reflection.md R6, R18): "submeshes[0].material.baseColorFactor",
// resolved over a per-struct sorted name index (names and aliases) without heap allocation.

#include "reflect/ReflectIndex.h"
#include "reflect/TypeOps.h"

#include "registry/EntityRegistry.h"

#include <algorithm>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    void indexStruct(const StructDesc& desc, std::unordered_map<const StructDesc*, NameIndex>& indexes) {
        if (indexes.count(&desc)) return;
        NameIndex& index = indexes[&desc];
        for (uint16_t i = 0; i < desc.fields.size(); i++) {
            const FieldDesc& field = desc.fields[i];
            index.entries.push_back({field.name, i, false});
            for (const char* alias : field.aliases) index.entries.push_back({alias, i, true});
        }
        std::sort(index.entries.begin(), index.entries.end(),
                  [](const NameIndex::Entry& a, const NameIndex::Entry& b) { return a.name < b.name; });
        for (const FieldDesc& field : desc.fields) {
            if (field.structDesc) indexStruct(*field.structDesc, indexes);
        }
    }

    // the registered components' structs, built once (immutable afterwards: lookups take no lock)
    const std::unordered_map<const StructDesc*, NameIndex>& registeredIndexes() {
        static const std::unordered_map<const StructDesc*, NameIndex> indexes = []() {
            std::unordered_map<const StructDesc*, NameIndex> built;
            for (const ComponentDesc* desc : all()) indexStruct(desc->body, built);
            return built;
        }();
        return indexes;
    }

}

const NameIndex& reflect::nameIndex(const StructDesc& desc) {
    const auto& registered = registeredIndexes();
    auto it = registered.find(&desc);
    if (it != registered.end()) return it->second;
    // a struct outside the registry (a test's): indexed on first use
    static std::mutex mutex;
    static std::unordered_map<const StructDesc*, NameIndex> others;
    std::lock_guard<std::mutex> lock(mutex);
    indexStruct(desc, others);
    return others.at(&desc);
}

size_t reflect::nameIndexBytes() {
    size_t bytes = 0;
    for (const auto& [desc, index] : registeredIndexes()) {
        bytes += sizeof(desc) + sizeof(index) + index.entries.capacity() * sizeof(NameIndex::Entry);
    }
    return bytes;
}

size_t reflect::buildNameIndexes() {
    return registeredIndexes().size();
}

const FieldDesc* reflect::findSegment(const StructDesc& desc, std::string_view name, bool* viaAlias) {
    const NameIndex& index = nameIndex(desc);
    auto it = std::lower_bound(index.entries.begin(), index.entries.end(), name,
                               [](const NameIndex::Entry& entry, std::string_view key) { return entry.name < key; });
    // a live name sorts with its aliases; prefer it
    const NameIndex::Entry* match = nullptr;
    for (; it != index.entries.end() && it->name == name; ++it) {
        if (!match || (match->alias && !it->alias)) match = &*it;
    }
    if (!match) return nullptr;
    if (viaAlias) *viaAlias = match->alias;
    return &desc.fields[match->field];
}

namespace {

    bool parseIndex(std::string_view path, size_t& pos, size_t& value) {
        if (pos >= path.size() || path[pos] < '0' || path[pos] > '9') return false;
        // one spelling per index ("00" would alias "0" under another override name), and no
        // value that wraps around
        if (path[pos] == '0' && pos + 1 < path.size() && path[pos + 1] >= '0' && path[pos + 1] <= '9') return false;
        size_t parsed = 0;
        size_t digits = 0;
        while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') {
            if (++digits > 9) return false;
            parsed = parsed * 10 + static_cast<size_t>(path[pos] - '0');
            pos++;
        }
        value = parsed;
        return true;
    }

    FieldRef fail(PathError* error, PathError code) {
        if (error) *error = code;
        return FieldRef();
    }

}

FieldRef reflect::resolve(const StructDesc& desc, void* owner, std::string_view path, PathError* error, bool append) {
    if (error) *error = PathError::None;
    if (!owner || path.empty()) return fail(error, PathError::Syntax);
    const StructDesc* current = &desc;
    void* currentOwner = owner;
    bool viaAlias = false;
    size_t pos = 0;
    while (true) {
        const size_t start = pos;
        while (pos < path.size() && path[pos] != '.' && path[pos] != '[') pos++;
        if (pos == start) return fail(error, PathError::Syntax);
        bool alias = false;
        const FieldDesc* field = findSegment(*current, path.substr(start, pos - start), &alias);
        if (!field) return fail(error, PathError::UnknownField);
        viaAlias = viaAlias || alias;
        void* ref = field->ref(currentOwner);
        const TypeOps* ops = field->ops;
        bool element = false;
        while (pos < path.size() && path[pos] == '[') {
            pos++;
            size_t index = 0;
            if (!parseIndex(path, pos, index) || pos >= path.size() || path[pos] != ']') return fail(error, PathError::Syntax);
            pos++;
            if (element || field->kind != Kind::Array || !field->arrayOps) return fail(error, PathError::NotIndexable);
            // one past the last element appends to an array that grows by index when the caller
            // writes (the Inspector's Add buttons, an agent adding a frame); never further, nor
            // past ARRAY_INDEX_LIMIT
            const size_t count = field->arrayOps->size(ref);
            const bool appends = append && field->arrayOps->appendsByIndex && index == count && index < ARRAY_INDEX_LIMIT;
            if (index >= count && !appends) return fail(error, PathError::IndexOutOfRange);
            ref = field->arrayOps->at(ref, index);
            ops = field->elementOps;
            element = true;
        }
        if (pos == path.size()) {
            FieldRef result;
            result.field = field;
            result.ref = ref;
            result.owner = currentOwner;
            result.ownerStruct = current;
            result.ops = ops;
            result.element = element;
            result.viaAlias = viaAlias;
            return result;
        }
        if (path[pos] != '.') return fail(error, PathError::Syntax);
        pos++;
        const bool structValue = (element && field->structDesc) || (!element && field->kind == Kind::Struct && field->structDesc);
        if (!structValue) return fail(error, PathError::NotAStruct);
        current = field->structDesc;
        currentOwner = ref;
    }
}

FieldRef reflect::resolve(const ComponentDesc& desc, void* comp, std::string_view path, PathError* error, bool append) {
    return resolve(desc.body, comp, path, error, append);
}

std::string reflect::canonicalPath(const ComponentDesc& desc, void* comp, std::string_view path) {
    std::string out;
    const StructDesc* current = &desc.body;
    void* owner = comp;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t start = pos;
        while (pos < path.size() && path[pos] != '.' && path[pos] != '[') pos++;
        const FieldDesc* field = current ? findSegment(*current, path.substr(start, pos - start)) : nullptr;
        if (!field) return std::string();
        out += field->name;
        void* ref = owner ? field->ref(owner) : nullptr;
        bool element = false;
        while (pos < path.size() && path[pos] == '[') {
            const size_t open = pos;
            while (pos < path.size() && path[pos] != ']') pos++;
            if (pos == path.size()) return std::string();
            pos++;
            out += path.substr(open, pos - open);
            size_t index = 0;
            size_t digits = open + 1;
            if (!parseIndex(path, digits, index)) return std::string();
            if (ref && field->arrayOps && index < field->arrayOps->size(ref)) {
                ref = field->arrayOps->at(ref, index);
            } else {
                ref = nullptr;
            }
            element = true;
        }
        if (pos < path.size()) {
            if (path[pos] != '.') return std::string();
            out += '.';
            pos++;
            current = (element || field->kind == Kind::Struct) ? field->structDesc : nullptr;
            owner = ref;
        }
    }
    return out;
}

const FieldDesc* reflect::findField(const StructDesc& desc, std::string_view path, bool* viaAlias) {
    const StructDesc* current = &desc;
    bool alias = false;
    size_t pos = 0;
    while (current) {
        const size_t start = pos;
        while (pos < path.size() && path[pos] != '.' && path[pos] != '[') pos++;
        if (pos == start) return nullptr;
        bool segmentAlias = false;
        const FieldDesc* field = findSegment(*current, path.substr(start, pos - start), &segmentAlias);
        if (!field) return nullptr;
        alias = alias || segmentAlias;
        bool element = false;
        while (pos < path.size() && path[pos] == '[') {
            while (pos < path.size() && path[pos] != ']') pos++;
            if (pos == path.size()) return nullptr;
            pos++;
            element = true;
        }
        if (pos == path.size()) {
            if (viaAlias) *viaAlias = alias;
            return field;
        }
        if (path[pos] != '.') return nullptr;
        pos++;
        current = (element || field->kind == Kind::Struct) ? field->structDesc : nullptr;
    }
    return nullptr;
}

const FieldDesc* reflect::findField(const ComponentDesc& desc, std::string_view path, bool* viaAlias) {
    return findField(desc.body, path, viaAlias);
}

// ---------------------------------------------------------------- asset references

AssetKind reflect::assetKindOf(const FieldDesc& field, const TypeOps* ops) {
    if (field.asset != AssetKind::None) return field.asset;
    if (ops->propertyType == PropertyType::Texture) return AssetKind::Texture;
    if (ops->propertyType == PropertyType::Shader) return AssetKind::Shader;
    if (ops->propertyType == PropertyType::Font) return AssetKind::Font;
    if (ops->propertyType == PropertyType::Asset) return AssetKind::Any;
    return AssetKind::None;
}

bool reflect::holdsAsset(const TypeOps* ops) {
    return ops->propertyType == PropertyType::Texture || ops->propertyType == PropertyType::Shader || ops->propertyType == PropertyType::Asset || ops->propertyType == PropertyType::Font;
}

namespace {

    void visitStruct(const StructDesc& desc, void* owner, const std::string& prefix, AssetVisitor visitor, void* user) {
        for (const FieldDesc& field : desc.fields) {
            if (field.flags & (Proxy | Transient)) continue;
            void* ref = field.ref(owner);
            const std::string path = prefix + field.name;
            if (field.kind == Kind::Struct && field.structDesc) {
                visitStruct(*field.structDesc, ref, path + ".", visitor, user);
            } else if (field.kind == Kind::Array && field.arrayOps) {
                const size_t size = field.arrayOps->size(ref);
                for (size_t i = 0; i < size; i++) {
                    void* element = field.arrayOps->at(ref, i);
                    const std::string elementPath = path + "[" + std::to_string(i) + "]";
                    if (field.structDesc) {
                        visitStruct(*field.structDesc, element, elementPath + ".", visitor, user);
                    } else if (holdsAsset(field.elementOps)) {
                        visitor(user, elementPath, assetKindOf(field, field.elementOps), element, field);
                    }
                }
            } else if (field.ops->propertyType == PropertyType::Font) {
                FontArray& fonts = *static_cast<FontArray*>(ref);
                for (size_t i = 0; i < fonts.size(); i++) {
                    if (!fonts[i].isSet()) continue;
                    visitor(user, path + "[" + std::to_string(i) + "]", AssetKind::Font, &fonts[i], field);
                }
            } else if (holdsAsset(field.ops)) {
                visitor(user, path, assetKindOf(field, field.ops), ref, field);
            }
        }
    }

}

void reflect::visitAssetRefs(const ComponentDesc& desc, void* comp, AssetVisitor visitor, void* user) {
    if (!comp) return;
    visitStruct(desc.body, comp, std::string(desc.key) + "/", visitor, user);
}

FieldRef reflect::resolveLocator(EntityRegistry* registry, Entity entity, std::string_view locator, const ComponentDesc** found) {
    const size_t slash = locator.find('/');
    if (slash == std::string_view::npos) return FieldRef();
    const ComponentDesc* desc = findByKey(locator.substr(0, slash));
    if (!desc) return FieldRef();
    if (found) *found = desc;
    void* comp = registry ? desc->find(registry, entity) : nullptr;
    if (!comp) return FieldRef();
    std::string_view path = locator.substr(slash + 1);
    // a font slot: "text/font[1]"
    FieldRef ref = resolve(*desc, comp, path);
    if (ref) return ref;
    const size_t bracket = path.rfind('[');
    if (bracket != std::string_view::npos && path.back() == ']') {
        FieldRef fonts = resolve(*desc, comp, path.substr(0, bracket));
        if (fonts && fonts.field->ops->propertyType == PropertyType::Font) {
            size_t pos = bracket + 1;
            size_t index = 0;
            while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') index = index * 10 + static_cast<size_t>(path[pos++] - '0');
            FontArray& array = *static_cast<FontArray*>(fonts.ref);
            if (index < array.size()) {
                fonts.ref = &array[index];
                fonts.ops = &ops::AssetRef;
                fonts.element = true;
                return fonts;
            }
        }
    }
    return FieldRef();
}
