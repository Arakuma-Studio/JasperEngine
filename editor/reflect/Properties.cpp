// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Catalog's properties from the tables (docs/specs/S08-reflection.md R7): the property a path
// names and the list of a component's properties, with the default instance's values beside them.
//
// What is listed: every field that is a property (not Derived, not NotListed); a struct's members
// ("positionInitializer.radius"), the struct too when ListWhole; an array itself when ListWhole;
// the elements of an array of values when ListElements ("pathPoints[2]"); the members of an
// array of structs' elements ("framesRect[0].name"), and each element when ListEachElement.
// Without a component an array lists one element with no index when ListTemplate.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include <mutex>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    // a default-constructed value of an element type: the default of the elements of an array whose
    // default instance has none
    const void* valueDefault(const TypeOps* ops) {
        static std::mutex mutex;
        static std::unordered_map<const TypeOps*, Value>* values = new std::unordered_map<const TypeOps*, Value>();
        std::lock_guard<std::mutex> lock(mutex);
        auto it = values->find(ops);
        if (it == values->end()) it = values->emplace(ops, Value(ops)).first;
        return it->second.get();
    }

    // the default of an element: the default array's first, else a default-constructed one
    const void* elementDefault(const FieldDesc& field, const void* defaultArray) {
        if (defaultArray && field.arrayOps->size(defaultArray) > 0) {
            return field.arrayOps->at(const_cast<void*>(defaultArray), 0);
        }
        if (field.structDesc && field.structDesc->defaults) return field.structDesc->defaults();
        return valueDefault(field.elementOps);
    }

    PropertyData make(const FieldDesc& field, const TypeOps* ops, const void* def, void* ref) {
        PropertyData data;
        data.type = ops->propertyType;
        data.updateFlags = field.updateFlags;
        data.def = (field.flags & NoDefault) ? nullptr : const_cast<void*>(def);
        data.ref = ref;
        return data;
    }

    void listStruct(const StructDesc& desc, void* owner, const void* defaults, const std::string& prefix,
                    std::map<std::string, PropertyData>& out) {
        for (const FieldDesc& field : desc.fields) {
            if (field.flags & (NotListed | Derived)) continue;
            if ((field.flags & ComponentOnly) && !owner) continue;
            const std::string name = prefix + field.name;
            void* ref = owner ? field.ref(owner) : nullptr;
            const void* def = defaults ? field.ref(const_cast<void*>(defaults)) : nullptr;

            if (field.kind == Kind::Struct && field.structDesc) {
                if (field.flags & ListWhole) out[name] = make(field, field.ops, def, ref);
                listStruct(*field.structDesc, ref, def, name + ".", out);
                continue;
            }
            if (field.kind == Kind::Array && field.arrayOps) {
                const bool structs = field.structDesc != nullptr;
                if (field.flags & ListWhole) out[name] = make(field, field.ops, def, ref);
                if (!structs && !(field.flags & ListElements)) continue;
                const size_t count = owner ? elementCount(field, owner, ref) : ((field.flags & ListTemplate) ? 1 : 0);
                const void* defElement = elementDefault(field, def);
                for (size_t i = 0; i < count; i++) {
                    const std::string elementName = name + "[" + (owner ? std::to_string(i) : std::string()) + "]";
                    void* element = owner ? field.arrayOps->at(ref, i) : nullptr;
                    if (structs) {
                        if (field.flags & ListEachElement) out[elementName] = make(field, field.elementOps, defElement, element);
                        listStruct(*field.structDesc, element, defElement, elementName + ".", out);
                    } else {
                        out[elementName] = make(field, field.elementOps, defElement, element);
                    }
                }
                continue;
            }
            out[name] = make(field, field.ops, def, ref);
        }
    }

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

    // the table's property, or no ref when the table names nothing at that path
    PropertyData tableProperty(const ComponentDesc& desc, void* comp, std::string_view path) {
        const StructDesc* current = &desc.body;
        void* owner = comp;
        const void* defaults = desc.body.defaults ? desc.body.defaults() : nullptr;
        size_t pos = 0;
        while (true) {
            const size_t start = pos;
            while (pos < path.size() && path[pos] != '.' && path[pos] != '[') pos++;
            if (pos == start) return PropertyData();
            const FieldDesc* field = findSegment(*current, path.substr(start, pos - start));
            if (!field || (field->flags & Derived)) return PropertyData();
            void* ref = field->ref(owner);
            const void* def = defaults ? field->ref(const_cast<void*>(defaults)) : nullptr;
            const TypeOps* ops = field->ops;
            bool element = false;
            if (pos < path.size() && path[pos] == '[') {
                pos++;
                size_t index = 0;
                if (!parseIndex(path, pos, index) || pos >= path.size() || path[pos] != ']') return PropertyData();
                pos++;
                if (field->kind != Kind::Array || !field->arrayOps || index >= field->arrayOps->size(ref)) return PropertyData();
                def = elementDefault(*field, def);
                ref = field->arrayOps->at(ref, index);
                ops = field->elementOps;
                element = true;
            }
            if (pos == path.size()) return make(*field, ops, def, ref);
            if (path[pos] != '.') return PropertyData();
            pos++;
            const bool structValue = (element && field->structDesc) || (!element && field->kind == Kind::Struct && field->structDesc);
            if (!structValue) return PropertyData();
            current = field->structDesc;
            owner = ref;
            defaults = def;
        }
    }

}

size_t reflect::elementCount(const FieldDesc& field, const void* owner, const void* array) {
    const size_t size = field.arrayOps->size(array);
    if (!field.count || !owner) return size;
    const size_t count = field.count(owner);
    return count < size ? count : size;
}

PropertyData reflect::property(const ComponentDesc& desc, void* comp, std::string_view path) {
    if (!comp || path.empty()) return PropertyData();
    PropertyData data = tableProperty(desc, comp, path);
    if (data.ref) return data;
    if (desc.extraProperty) return desc.extraProperty(comp, std::string(path));
    return PropertyData();
}

void reflect::listProperties(const ComponentDesc& desc, void* comp, std::map<std::string, PropertyData>& out) {
    listStruct(desc.body, comp, desc.body.defaults ? desc.body.defaults() : nullptr, std::string(), out);
    if (desc.extraProperties) desc.extraProperties(comp, out);
}

Mixed reflect::mixedValues(const ComponentDesc& desc, const std::vector<void*>& comps, std::string_view path) {
    constexpr float epsilon = 1e-4f;
    Mixed mixed;
    std::vector<FieldRef> refs;
    for (void* comp : comps) {
        FieldRef ref = comp ? resolve(desc, comp, path) : FieldRef();
        if (ref) refs.push_back(ref);
    }
    if (refs.size() < 2) return mixed;
    const FieldRef& first = refs.front();
    size_t lanes = 0;
    switch (first.ops->kind) {
        case Kind::Vector2: lanes = 2; break;
        case Kind::Vector3: lanes = 3; break;
        case Kind::Vector4: case Kind::Quat: case Kind::Rect: lanes = 4; break;
        default: break;
    }
    for (size_t i = 1; i < refs.size(); i++) {
        const FieldRef& other = refs[i];
        if (other.ops != first.ops) {
            mixed.any = true;
            continue;
        }
        if (lanes) {
            const float* a = static_cast<const float*>(first.ref);
            const float* b = static_cast<const float*>(other.ref);
            for (size_t lane = 0; lane < lanes; lane++) {
                if (!reflect::detail::floatEquals(a[lane], b[lane], epsilon)) {
                    mixed.lanes[lane] = true;
                    mixed.any = true;
                }
            }
            continue;
        }
        if (first.ops->kind == Kind::Array && !first.element && first.field->arrayOps) {
            if (first.field->arrayOps->size(first.ref) != first.field->arrayOps->size(other.ref)) {
                mixed.sizeDiffers = true;
                mixed.any = true;
                continue;
            }
        }
        if (!first.ops->equals || !first.ops->equals(first.ref, other.ref, epsilon)) mixed.any = true;
    }
    return mixed;
}
