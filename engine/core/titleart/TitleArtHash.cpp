// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtHash.h"
#include "TitleArtIO.h"
#include "TitleArtSchema.h"

#include <map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

uint64_t titleart::fnv1a64(std::string_view bytes, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t titleart::hashCombine(uint64_t a, uint64_t b) {
    const char bytes[16] = {(char)(a), (char)(a >> 8), (char)(a >> 16), (char)(a >> 24), (char)(a >> 32), (char)(a >> 40),
                            (char)(a >> 48), (char)(a >> 56), (char)(b), (char)(b >> 8), (char)(b >> 16), (char)(b >> 24),
                            (char)(b >> 32), (char)(b >> 40), (char)(b >> 48), (char)(b >> 56)};
    return fnv1a64(std::string_view(bytes, sizeof(bytes)));
}

namespace {

    constexpr int SCOPES = 10;

    // the canonical text each scope of one layer collects
    struct Texts {
        std::string scope[SCOPES];
        std::string stackAlpha;             // the Stack scope's text without its Color members
        std::vector<std::string> refs;      // layers it reads (mask layers, follow)

        std::string& at(HashScope s) { return scope[(int)s]; }
    };

    struct Walker {
        const TitleArtSchema& schema;
        const AssetRevisionFn& revision;

        void append(Texts& t, HashScope scope, const std::string& path, const std::string& text, bool colour = false) {
            if (scope == HashScope::Identity || scope == HashScope::Inherit) scope = HashScope::Node;
            for (std::string* out : {&t.at(scope), scope == HashScope::Stack && !colour ? &t.stackAlpha : nullptr}) {
                if (!out) continue;
                *out += path;
                *out += '=';
                *out += text;
                *out += ';';
            }
        }

        void asset(Texts& t, HashScope scope, const std::string& path, const Json& ref) {
            std::string text = writeTitleArtJson(ref);
            if (revision && ref.is_object()) text += "@" + std::to_string(revision(decodeAssetRef(ref)));
            append(t, scope, path, text);
        }

        // part keys name rims by id; hashes name them by position (rendering never depends on ids)
        std::string partKeyOf(const std::string& key, const Json& layer) {
            if (key.rfind("rim:", 0) != 0) return key;
            const Json& rims = layer.value("style", Json::object()).value("rims", Json::array());
            for (size_t i = 0; i < rims.size(); i++) {
                if ("rim:" + rims[i].value("id", std::string()) == key) return "rim#" + std::to_string(i);
            }
            return key;
        }

        void object(Texts& t, const Json& obj, const SchemaKind& base, HashScope inherited, const std::string& path, const Json& layer) {
            const SchemaKind* kind = schema.variantOf(base, obj);
            if (!kind) {
                append(t, inherited, path, writeTitleArtJson(obj));
                return;
            }
            const bool isLayer = base.name == "layer";
            for (const SchemaMember* m : schema.membersOf(*kind)) {
                if (isLayer && m->key == "children") continue;
                auto it = obj.find(m->key);
                if (it == obj.end()) continue;
                const HashScope scope = m->hash == HashScope::Inherit ? inherited : m->hash;
                if (scope == HashScope::Identity) continue;
                const Json& value = *it;
                const std::string at = path.empty() ? m->key : path + "." + m->key;
                const SchemaKind* child = m->kind.empty() ? nullptr : schema.kind(m->kind);
                switch (m->type) {
                    case ValueType::Object:
                        if (value.is_object() && child) object(t, value, *child, scope, at, layer);
                        else append(t, scope, at, writeTitleArtJson(value));
                        break;
                    case ValueType::ObjectArray:
                        append(t, scope, at, "#" + std::to_string(value.size()));
                        for (size_t i = 0; i < value.size(); i++) {
                            if (child) object(t, value[i], *child, scope, at + "[" + std::to_string(i) + "]", layer);
                        }
                        break;
                    case ValueType::ObjectMap:
                        for (auto e = value.begin(); e != value.end(); ++e) {
                            if (child) object(t, e.value(), *child, scope, at + "." + e.key(), layer);
                        }
                        break;
                    case ValueType::ObjectArrayMap:
                        for (auto e = value.begin(); e != value.end(); ++e) {
                            const std::string key = at + "." + partKeyOf(e.key(), layer);
                            append(t, scope, key, "#" + std::to_string(e.value().size()));
                            for (size_t i = 0; i < e.value().size(); i++) {
                                if (child) object(t, e.value()[i], *child, scope, key + "[" + std::to_string(i) + "]", layer);
                            }
                        }
                        break;
                    case ValueType::AssetRef:
                        asset(t, scope, at, value);
                        break;
                    case ValueType::AssetRefList:
                        for (size_t i = 0; i < value.size(); i++) asset(t, scope, at + "[" + std::to_string(i) + "]", value[i]);
                        break;
                    case ValueType::LayerRef:
                        // what a reference reads is the referenced layer's node hash, chained below
                        if (value.is_string() && !value.get<std::string>().empty()) t.refs.push_back(value.get<std::string>());
                        break;
                    case ValueType::Id:
                        break;
                    default:
                        append(t, scope, at, writeTitleArtJson(value), m->type == ValueType::Color);
                        break;
                }
            }
            // unknown members of the object (a later spec's data) feed the scope that holds them
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (schema.member(*kind, it.key())) continue;
                append(t, inherited, path.empty() ? it.key() : path + "." + it.key(), writeTitleArtJson(it.value()));
            }
        }
    };

    struct Pending {
        std::string id;
        LayerHashes hashes;
        std::vector<std::string> refs;
        std::vector<std::string> children;
    };

    uint64_t hashText(const std::string& text) {
        return fnv1a64(text);
    }

}

TitleArtHashes titleart::hashTitleArt(const TitleArtDoc& doc, const AssetRevisionFn& assetRevision) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    Walker walker{schema, assetRevision};
    TitleArtHashes out;
    const std::string seed = writeTitleArtJson(doc.root.value("seed", Json(1)));
    const std::string light = writeTitleArtJson(doc.root.value("global_light", Json::object()));
    const std::string canvas = writeTitleArtJson(doc.root.value("canvas", Json::object()));
    // S86: the document's 3D scene (camera, environment, lights, tone map) is part of every 3D layer's stack
    const std::string scene3d = writeTitleArtJson(doc.root.value("scene_3d", Json()));

    std::vector<Pending> pending;
    forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
        Texts t;
        walker.object(t, layer, *schema.kind("layer"), HashScope::Node, "", layer);
        Pending p;
        p.id = layer.value("id", std::string());
        LayerHashes& h = p.hashes;
        h.geometry = hashText("seed=" + seed + ";" + t.at(HashScope::Geometry));
        h.mask = hashCombine(h.geometry, hashText(t.at(HashScope::Mask)));
        h.field = hashCombine(h.mask, hashText(t.at(HashScope::Field)));
        const bool threeD = layer.value("dimension", std::string()) == "3d";
        const std::string scene = threeD ? "scene3d=" + scene3d + ";" : std::string();
        h.stack = hashCombine(h.field, hashText("light=" + light + ";" + scene + t.at(HashScope::Stack)));
        h.alpha = hashCombine(h.field, hashText("light=" + light + ";" + scene + t.stackAlpha));
        h.effects = hashText(t.at(HashScope::Effects));
        h.masks = hashText(t.at(HashScope::Masks));
        h.placement = hashText(t.at(HashScope::Placement));
        h.node = hashCombine(hashCombine(hashCombine(h.stack, h.effects), hashCombine(h.masks, h.placement)),
                             hashText("canvas=" + canvas + ";" + t.at(HashScope::Node)));
        h.content = hashCombine(hashCombine(h.stack, h.effects), hashCombine(h.masks, hashText("canvas=" + canvas)));
        p.refs = std::move(t.refs);
        for (const Json& child : layerChildren(layer)) p.children.push_back(child.value("id", std::string()));
        pending.push_back(std::move(p));
        return true;
    });
    // children before their groups (the walk lists a group first): fold from the end
    std::map<std::string, uint64_t> nodes;
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        uint64_t node = it->hashes.node;
        uint64_t content = it->hashes.content;
        for (const std::string& child : it->children) {
            auto found = nodes.find(child);
            node = hashCombine(node, found != nodes.end() ? found->second : 0);
            content = hashCombine(content, found != nodes.end() ? found->second : 0);
        }
        nodes[it->id] = node;
        it->hashes.node = node;
        it->hashes.content = content;
    }
    // the layers a node reads (one level: a source's own references are in its node hash already)
    for (Pending& p : pending) {
        for (const std::string& ref : p.refs) {
            auto found = nodes.find(ref);
            const uint64_t source = found != nodes.end() && ref != p.id ? found->second : 0;
            p.hashes.node = hashCombine(p.hashes.node, source);
            p.hashes.content = hashCombine(p.hashes.content, source);
        }
        out.layers[p.id] = p.hashes;
    }
    uint64_t document = hashText("canvas=" + canvas);
    for (const Json& layer : doc.layers()) {
        auto found = out.layers.find(layer.value("id", std::string()));
        document = hashCombine(document, found != out.layers.end() ? found->second.node : 0);
    }
    out.document = document;
    return out;
}
