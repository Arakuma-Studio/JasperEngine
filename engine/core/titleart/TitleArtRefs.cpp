// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "titleart/TitleArtRefs.h"

#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

using namespace jasperengine;

namespace {

    // the member key says what the reference names: the schema types them all as AssetRef
    titleart::TitleAssetKind kindOfKey(const std::string& key, bool isParam) {
        if (!isParam) {
            if (key == "font" || key == "fallback_fonts") return titleart::TitleAssetKind::Font;
            if (key == "svg") return titleart::TitleAssetKind::Vector;
            if (key == "map") return titleart::TitleAssetKind::Environment;
        }
        return titleart::TitleAssetKind::Image;
    }

}

const char* titleart::titleAssetKindName(TitleAssetKind kind) {
    switch (kind) {
        case TitleAssetKind::Font: return "font";
        case TitleAssetKind::Image: return "image";
        case TitleAssetKind::Vector: return "vector";
        case TitleAssetKind::Environment: return "environment";
        case TitleAssetKind::Sound: return "sound";
    }
    return "image";
}

void titleart::visitTitleArtRefs(Json& root, const TitleRefVisitor& visit) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* document = schema.kind("document");
    if (!document || !root.is_object()) return;
    bool stopped = false;
    forEachMember(root, *document, [&](const SchemaMember* member, const ParamDef* param, Json& value, const std::string& path) {
        if (stopped) return false;
        if (member) {
            if (member->type == ValueType::AssetRef) {
                if (!visit(value, path, kindOfKey(member->key, false))) stopped = true;
                return false;
            }
            if (member->type == ValueType::AssetRefList) {
                if (value.is_array()) {
                    for (size_t i = 0; i < value.size() && !stopped; i++) {
                        if (!visit(value[i], path + "[" + std::to_string(i) + "]", kindOfKey(member->key, false))) stopped = true;
                    }
                }
                return false;
            }
            return true;
        }
        if (param && param->kind == ParamKind::Asset) {
            if (!visit(value, path, kindOfKey(param->key, true))) stopped = true;
            return false;
        }
        return true;
    });
    // S85's marker sounds: `animation` is one opaque member of the schema (R62)
    auto animation = root.find("animation");
    if (stopped || animation == root.end() || !animation->is_object()) return;
    auto animations = animation->find("animations");
    if (animations == animation->end() || !animations->is_array()) return;
    for (size_t a = 0; a < animations->size() && !stopped; a++){
        auto markers = (*animations)[a].find("markers");
        if (markers == (*animations)[a].end() || !markers->is_array()) continue;
        for (size_t m = 0; m < markers->size() && !stopped; m++){
            auto sound = (*markers)[m].find("sound");
            if (sound == (*markers)[m].end()) continue;
            const std::string path = "animation.animations[" + std::to_string(a) + "].markers[" + std::to_string(m) + "].sound";
            if (!visit(*sound, path, TitleAssetKind::Sound)) stopped = true;
        }
    }
}

void titleart::visitTitleArtRefs(const Json& root, const std::function<bool(const TitleAssetRef& ref, const std::string& field, TitleAssetKind kind)>& visit) {
    // forEachMember walks a mutable tree; a copy keeps the caller's const
    Json copy = root;
    visitTitleArtRefs(copy, [&](Json& ref, const std::string& field, TitleAssetKind kind) {
        return visit(decodeAssetRef(ref), field, kind);
    });
}
