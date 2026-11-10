// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DepEdge.h"

using namespace jasperengine;

namespace {

    struct DepKindName {
        editor::DepEdgeKind kind;
        const char* name;
    };

    const DepKindName kindNames[] = {
        {editor::DepEdgeKind::Other, "other"},
        {editor::DepEdgeKind::SceneComponent, "scene_component"},
        {editor::DepEdgeKind::PrefabComponent, "prefab_component"},
        {editor::DepEdgeKind::MaterialTexture, "material_texture"},
        {editor::DepEdgeKind::MaterialShader, "material_shader"},
        {editor::DepEdgeKind::MeshMaterialFile, "mesh_material_file"},
        {editor::DepEdgeKind::SceneSettings, "scene_settings"},
        {editor::DepEdgeKind::SceneList, "scene_list"},
        {editor::DepEdgeKind::ChildScene, "child_scene"},
        {editor::DepEdgeKind::ScenePrefabList, "scene_prefab_list"},
        {editor::DepEdgeKind::ScriptProperty, "script_property"},
        {editor::DepEdgeKind::CppScript, "cpp_script"},
        {editor::DepEdgeKind::UIStylesheet, "ui_stylesheet"},
        {editor::DepEdgeKind::UIImage, "ui_image"},
        {editor::DepEdgeKind::LuaRequire, "lua_require"},
        {editor::DepEdgeKind::ShaderInclude, "shader_include"},
        {editor::DepEdgeKind::ModelUri, "model_uri"},
        {editor::DepEdgeKind::TitleArtAsset, "title_art_asset"},
        {editor::DepEdgeKind::TitleArtOutput, "title_art_output"},
    };

}

const char* editor::depEdgeKindName(DepEdgeKind kind) {
    for (const DepKindName& entry : kindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "other";
}

editor::DepEdgeKind editor::depEdgeKindFromName(const std::string& name) {
    for (const DepKindName& entry : kindNames) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return DepEdgeKind::Other;
}

editor::FieldLocatorParts editor::parseFieldLocator(const std::string& locator) {
    FieldLocatorParts parts;
    const size_t slash = locator.find('/');
    parts.scope = locator.substr(0, slash);
    if (parts.scope.compare(0, 7, "entity:") == 0) {
        const std::string id = parts.scope.substr(7);
        if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
            parts.entity = std::stoull(id);
            parts.hasEntity = true;
        }
    }
    if (slash == std::string::npos) {
        return parts;
    }
    const std::string rest = locator.substr(slash + 1);
    const size_t dot = rest.find_first_of(".[");
    if (parts.hasEntity && dot != std::string::npos && rest[dot] == '.') {
        parts.component = rest.substr(0, dot);
        parts.field = rest.substr(dot + 1);
    } else {
        parts.field = rest;
    }
    return parts;
}
