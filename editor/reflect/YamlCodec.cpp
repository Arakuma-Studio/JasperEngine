// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generic YAML codec of the tables (docs/specs/S08-reflection.md R7): keys in table order,
// OmitIfDefault / OmitIfNonFinite / NoSave / NotWritten honoured, the legacy (node, base) decode (absent keys
// keep the base value), aliases read, never throwing: a value that does not read keeps the base
// and is reported to S03 (which keeps the authored text) and to the DecodeReport.

#include "reflect/Reflect.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    void encodeField(const FieldDesc& field, const void* value, YAML::Node& node, const EncodeContext& ctx) {
        const TypeOps* ops = field.ops;
        if ((field.flags & OmitIfNonFinite) && ops->finite && !ops->finite(value)) return;
        if (ops->skip && ops->skip(field, value)) return;
        if (ops->encodeKey) {
            ops->encodeKey(field, value, node, field.yamlKey, ctx);
        } else if (ops->scalar) {
            node[field.yamlKey] = ops->scalar(field, value);
        } else if (ops->toYaml) {
            node[field.yamlKey] = ops->toYaml(field, value, ctx);
        }
    }

    bool given(const FieldDesc& field, const YAML::Node& value) {
        if (!value) return false;
        if (!(field.flags & EmptyIsDefault)) return true;
        return !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    }

    // The key of a field present in the node: its YAML key, else the first alias present
    const char* presentKey(const FieldDesc& field, const YAML::Node& node) {
        if (given(field, node[field.yamlKey])) return field.yamlKey;
        for (const char* alias : field.aliases) {
            if (given(field, node[alias])) return alias;
        }
        return nullptr;
    }

    void skipRecord(const DecodeContext& ctx, const FieldDesc& field, const YAML::Node& value, const std::string& reason) {
        if (!ctx.report) return;
        SkipRecord record;
        record.component = ctx.component ? ctx.component : "";
        record.path = field.name;
        const YAML::Mark mark = value ? value.Mark() : YAML::Mark::null_mark();
        record.line = mark.line >= 0 ? mark.line + 1 : 0;
        record.reason = reason;
        ctx.report->skipped.push_back(std::move(record));
    }

    void restoreDefault(const StructDesc& desc, const FieldDesc& field, void* value) {
        if (const void* def = defaultValue(desc, field)) field.ops->copy(value, def);
    }

}

namespace {
    thread_local int documentEncodes = 0;
    thread_local int playSnapshotEncodes = 0;
}

reflect::DocumentEncodeScope::DocumentEncodeScope() {
    documentEncodes++;
}

reflect::DocumentEncodeScope::~DocumentEncodeScope() {
    documentEncodes--;
}

bool reflect::DocumentEncodeScope::active() {
    return documentEncodes > 0;
}

reflect::PlaySnapshotEncodeScope::PlaySnapshotEncodeScope() {
    playSnapshotEncodes++;
}

reflect::PlaySnapshotEncodeScope::~PlaySnapshotEncodeScope() {
    playSnapshotEncodes--;
}

bool reflect::encodesComputedValues() {
    return documentEncodes == 0 || playSnapshotEncodes > 0;
}

YAML::Node reflect::encode(const StructDesc& desc, const void* owner, const EncodeContext& ctx) {
    YAML::Node node;
    const bool document = DocumentEncodeScope::active();
    for (const FieldDesc& field : desc.fields) {
        if (!isSaved(field) || (document && (field.flags & NotWritten)) || (field.derivedIf && !encodesComputedValues() && field.derivedIf(owner))) continue;
        const void* value = field.ref(const_cast<void*>(owner));
        if (field.flags & OmitIfDefault) {
            const void* def = defaultValue(desc, field);
            if (def && field.ops->equals && field.ops->equals(value, def, 0.0f)) continue;
        }
        encodeField(field, value, node, ctx);
    }
    return node;
}

YAML::Node reflect::encode(const ComponentDesc& desc, const void* comp, const EncodeContext& ctx) {
    if (desc.codec) return desc.codec->encode(comp, ctx);
    return encode(desc.body, comp, ctx);
}

void reflect::decode(const StructDesc& desc, const YAML::Node& node, void* owner, const DecodeContext& ctx) {
    for (const FieldDesc& field : desc.fields) {
        if (!isSaved(field)) continue;
        void* value = field.ref(owner);
        const TypeOps* ops = field.ops;
        const char* key = presentKey(field, node);
        if (!key) {
            if (field.flags & OmitIfDefault) {
                restoreDefault(desc, field, value);
            } else if (ops->decodeWhenAbsent && ops->fromYaml) {
                std::string reason;
                ops->fromYaml(field, node[field.yamlKey], value, ctx, reason);
            }
            continue;
        }
        std::string reason;
        bool read = false;
        if (ops->decodeKey) {
            read = ops->decodeKey(field, node, key, value, ctx, reason);
        } else if (ops->fromYaml) {
            read = ops->fromYaml(field, node[key], value, ctx, reason);
        }
        if (!read) {
            skipRecord(ctx, field, node[key], reason);
            if (field.flags & OmitIfDefault) restoreDefault(desc, field, value);
        }
    }
}

void reflect::decode(const ComponentDesc& desc, const YAML::Node& node, void* comp, const DecodeContext& ctx, DecodeReport& report) {
    DecodeContext local = ctx;
    local.report = &report;
    if (!local.component || !*local.component) local.component = desc.key;
    if (desc.codec) {
        desc.codec->decode(node, comp, local);
    } else {
        decode(desc.body, node, comp, local);
    }
    if (desc.postDecode) desc.postDecode(comp, local);
}

YAML::Node reflect::encodeValue(ComponentType type, const void* comp) {
    const ComponentDesc* desc = find(type);
    if (!desc || !comp) return YAML::Node();
    return encode(*desc, comp, EncodeContext{});
}

void reflect::decodeInto(ComponentType type, const YAML::Node& node, void* comp, const void* old) {
    const ComponentDesc* desc = find(type);
    if (!desc || !comp) return;
    DecodeContext ctx;
    ctx.old = old;
    ctx.component = desc->key;
    DecodeReport report;
    decode(*desc, node, comp, ctx, report);
}

namespace {

    void addKeys(const StructDesc& desc, std::vector<std::string>& keys) {
        for (const FieldDesc& field : desc.fields) {
            if (!isSaved(field)) continue;
            keys.emplace_back(field.yamlKey);
            for (const char* alias : field.aliases) keys.emplace_back(alias);
            // the fonts' GUIDs beside their paths (fontDecodeKey)
            if (field.ops->propertyType == PropertyType::Font) keys.emplace_back(std::string(field.yamlKey) + "Guid");
        }
    }

}

const std::vector<std::string>& reflect::knownKeys(const StructDesc& desc) {
    static std::mutex mutex;
    static std::unordered_map<const StructDesc*, std::vector<std::string>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(&desc);
    if (it != cache.end()) return it->second;
    std::vector<std::string>& keys = cache[&desc];
    addKeys(desc, keys);
    return keys;
}

const std::vector<std::string>& reflect::knownKeys(const ComponentDesc& desc) {
    static std::mutex mutex;
    static std::unordered_map<const ComponentDesc*, std::vector<std::string>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(&desc);
    if (it != cache.end()) return it->second;
    std::vector<std::string>& keys = cache[&desc];
    // a component with a codec of its own lists the keys it reads (extraKeys)
    if (!desc.codec) addKeys(desc.body, keys);
    for (const char* key : desc.extraKeys) keys.emplace_back(key);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

int reflect::migrate(const ComponentDesc& desc, YAML::Node& component, int fileVersion) {
    int applied = 0;
    for (const Migration& migration : desc.migrations) {
        if (fileVersion < migration.toFormatVersion) {
            migration.apply(component);
            applied++;
        }
    }
    return applied;
}
