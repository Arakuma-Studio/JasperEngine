// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetRefSchema.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/ScriptComponent.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "animation/ModelSubClips.h"
#include "pool/TexturePool.h"
#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"
#include "registry/EntityRegistry.h"
#include "util/Util.h"
#include "texture/MaterialPbr.h"

#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // post-processing's LUT is an image strip or a .cube (S13): a .cube path expects a colour LUT,
    // anything else the row's texture
    AssetType lutExpectedType(const std::string& path) {
        return Util::isColorLutFile(path) ? AssetType::ColorLut : AssetType::Unknown;
    }

    // a Spline Instantiate pool entry is a model (instanced) or a prefab (GameObjects)
    AssetType splinePieceExpectedType(const std::string& path) {
        const std::string suffix = ".prefab";
        const bool prefab = path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        return prefab ? AssetType::Prefab : AssetType::Model;
    }

    using Scope = AssetRefScope;
    using Shape = AssetRefShape;
    using Base = AssetPathBase;
    using Kind = DepEdgeKind;

    // ---------------------------------------------------------------- the table

    std::vector<AssetRefField> buildMaterialFields() {
        std::vector<AssetRefField> rows;
        auto row = [&](const char* path, Shape shape, AssetType type, Base base, Kind kind) {
            rows.push_back({Scope::Material, ComponentType::MeshComponent, path, shape, type, base, kind});
        };
        row("baseColorTexture", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        row("emissiveTexture", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        row("metallicRoughnessTexture", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        row("occlusionTexture", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        row("normalTexture", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        row("shaderTextures{}", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        // the built-in Lit's extended lobes' maps (S11, MaterialPbr's field table)
        for (const PbrField& field : MaterialPbr::getFields()) {
            if (field.type == PbrFieldType::Texture) {
                rows.push_back({Scope::Material, ComponentType::MeshComponent, "pbr." + MaterialPbr::getFieldPath(field), Shape::Texture,
                                AssetType::Texture, Base::Assets, Kind::MaterialTexture});
            }
        }
        row("shader", Shape::Shader, AssetType::Shader, Base::Project, Kind::MaterialShader);
        row("extraPass.shader", Shape::Shader, AssetType::Shader, Base::Project, Kind::MaterialShader);
        row("extraPass.shaderTextures{}", Shape::Texture, AssetType::Texture, Base::Assets, Kind::MaterialTexture);
        // Palette Swap's bindings (S43): the source palette and each target palette
        row("shaderPalettes{}.source", Shape::Asset, AssetType::Palette, Base::Assets, Kind::Other);
        row("shaderPalettes{}.targets[].palette", Shape::Asset, AssetType::Palette, Base::Assets, Kind::Other);
        // the linked .material file (in a .material file: the file itself, not a reference)
        row("name", Shape::Asset, AssetType::Material, Base::Project, Kind::MeshMaterialFile);
        return rows;
    }

    // ---- the component rows and the walk, from the reflection tables

    // How the walk reaches what a field holds
    enum class RefStep : uint8_t {
        Struct, Array, Texture, Shader, Asset, Font, ShaderTextures, StringPath, ClipEntry, PrefabRoot, Scripts, InlineEffect, Material
    };

    struct RefWalkNode {
        const reflect::FieldDesc* field = nullptr;
        RefStep step = RefStep::Struct;
        RefStep element = RefStep::Struct;                // Array: its elements (Struct: `children` of each)
        size_t row = 0;                             // the field's row in the table
        const reflect::FieldDesc* guid = nullptr;   // StringPath, ClipEntry, PrefabRoot: the GUID beside the path
        std::vector<RefWalkNode> children;
    };

    struct RefComponentWalk {
        const reflect::ComponentDesc* desc = nullptr;
        std::vector<RefWalkNode> nodes;
    };

    AssetType rowExpectedType(reflect::AssetKind kind) {
        using K = reflect::AssetKind;
        switch (kind) {
            case K::Texture: case K::Cubemap: return AssetType::Texture;
            case K::Font: return AssetType::Font;
            case K::Material: return AssetType::Material;
            case K::Model: return AssetType::Model;
            case K::Audio: return AssetType::Audio;
            case K::Script: return AssetType::Script;
            case K::Prefab: return AssetType::Prefab;
            case K::Shader: return AssetType::Shader;
            case K::UIDocument: return AssetType::UIDocument;
            case K::Scene: return AssetType::Scene;
            case K::PoseAnimation: return AssetType::PoseAnimation;
            case K::AnimationClip: return AssetType::AnimationClip;
            case K::AnimatorController: return AssetType::AnimatorController;
            case K::VertexAnimation: return AssetType::VertexAnimation;
            case K::AvatarMask: return AssetType::AvatarMask;
            case K::SoundEvent: return AssetType::SoundEvent;
            case K::NavMesh: return AssetType::NavMesh;
            case K::NavGrid: return AssetType::NavGrid;
            case K::ParticleEffect: return AssetType::ParticleEffect;
            case K::Palette: return AssetType::Palette;
            case K::TreeData: return AssetType::TreeData;
            case K::TerrainWorld: return AssetType::TerrainWorld;
            case K::Tileset: return AssetType::Tileset;
            case K::TitleArt: return AssetType::TitleArt;
            case K::None: case K::Any: case K::ModelOrPrefab: break;
        }
        return AssetType::Unknown;
    }

    // the root each reference of that kind has always used
    AssetPathBase rowPathBase(reflect::AssetKind kind) {
        using K = reflect::AssetKind;
        return kind == K::Shader || kind == K::Script || kind == K::Prefab || kind == K::Material || kind == K::Scene ? Base::Project : Base::Assets;
    }

    const reflect::FieldDesc* siblingRefField(const reflect::StructDesc& desc, const std::string& name) {
        for (const reflect::FieldDesc& field : desc.fields) {
            if (name == field.name) return &field;
        }
        return nullptr;
    }

    // Generates one component's rows and walk: fields that lead to no reference are not in the
    // walk, so a long list of plain values costs it nothing
    class ComponentRefRows {
    public:
        ComponentRefRows(ComponentType component, std::vector<AssetRefField>& rows): component(component), rows(rows) {}

        void collect(const reflect::StructDesc& desc, const std::string& prefix, std::vector<RefWalkNode>& out) {
            for (const reflect::FieldDesc& field : desc.fields) {
                if (!reflect::isSaved(field)) continue;
                RefWalkNode node;
                node.field = &field;
                if (describe(desc, field, prefix, node)) out.push_back(std::move(node));
            }
        }

        void legacyTexture(const char* key) {
            rows.push_back({Scope::Component, component, key, Shape::Texture, AssetType::Texture, Base::Assets, Kind::SceneComponent, true});
        }

    private:
        size_t add(const std::string& path, Shape shape, AssetType expected, Base base, Kind kind = Kind::SceneComponent) {
            rows.push_back({Scope::Component, component, path, shape, expected, base, kind});
            return rows.size() - 1;
        }

        size_t add(const std::string& path, Shape shape, reflect::AssetKind declared) {
            const size_t row = add(path, shape, rowExpectedType(declared), rowPathBase(declared));
            rows[row].modelOrPrefab = declared == reflect::AssetKind::ModelOrPrefab;
            return row;
        }

        static bool refValueStep(const reflect::TypeOps* ops, RefStep& step, Shape& shape) {
            switch (ops->propertyType) {
                case PropertyType::Texture: step = RefStep::Texture; shape = Shape::Texture; return true;
                case PropertyType::Shader: step = RefStep::Shader; shape = Shape::Shader; return true;
                case PropertyType::Asset: step = RefStep::Asset; shape = Shape::Asset; return true;
                case PropertyType::Font: step = RefStep::Font; shape = Shape::AssetList; return true;
                default: return false;
            }
        }

        // the path below an element without the dot after it ("clips[]"; "" at the component's root)
        static std::string withoutDot(const std::string& prefix) {
            return prefix.empty() ? prefix : prefix.substr(0, prefix.size() - 1);
        }

        bool describe(const reflect::StructDesc& desc, const reflect::FieldDesc& field, const std::string& prefix, RefWalkNode& node) {
            const std::string path = prefix + field.yamlKey;
            switch (field.assetForm) {
                case reflect::AssetForm::ClipEntry:
                    node.step = RefStep::ClipEntry;
                    node.guid = siblingRefField(desc, "guid");
                    node.row = add(withoutDot(prefix), Shape::ClipEntry, field.asset);
                    return true;
                case reflect::AssetForm::PrefabRoot:
                    node.step = RefStep::PrefabRoot;
                    node.guid = siblingRefField(desc, "guid");
                    node.row = add(withoutDot(prefix), Shape::PrefabRef, field.asset);
                    return true;
                case reflect::AssetForm::ScriptEntries:
                    node.step = RefStep::Scripts;
                    // Lua entries are relative to the Lua directory (the extractor and visitor switch base)
                    node.row = add(path + "[].path", Shape::Asset, AssetType::Script, Base::Project);
                    add(path + "[].headerPath", Shape::Asset, AssetType::Script, Base::Project);
                    add(path + "[].properties[]", Shape::ScriptProperty, AssetType::Unknown, Base::Project, Kind::ScriptProperty);
                    return true;
                case reflect::AssetForm::InlineEffect:
                    node.step = RefStep::InlineEffect;
                    node.row = add(path, Shape::ParticleEffect, AssetType::Unknown, Base::Assets);
                    return true;
                case reflect::AssetForm::Deduced:
                    break;
            }
            RefStep step;
            Shape shape;
            if (field.ops->propertyType == PropertyType::Material) {
                // the material rows below it, the material being an explicit list
                node.step = RefStep::Material;
                node.row = rows.size();
                for (const AssetRefField& material : AssetRefSchema::materialFields()) {
                    add(path + "." + material.path, material.shape, material.expected, material.base, material.kind);
                }
                return true;
            }
            if (field.ops == &reflect::ops::ShaderTextures) {
                node.step = RefStep::ShaderTextures;
                node.row = add(path + "{}", Shape::Texture, AssetType::Texture, Base::Assets);
                return true;
            }
            if (field.kind == reflect::Kind::Struct && field.structDesc) {
                node.step = RefStep::Struct;
                collect(*field.structDesc, path + ".", node.children);
                return !node.children.empty();
            }
            if (field.kind == reflect::Kind::Array && field.arrayOps) {
                node.step = RefStep::Array;
                if (field.structDesc) {
                    collect(*field.structDesc, path + "[].", node.children);
                    return !node.children.empty();
                }
                if (field.elementOps && refValueStep(field.elementOps, step, shape)) {
                    node.element = step;
                    node.row = add(path + "[]", shape, reflect::assetKindOf(field, field.elementOps));
                    return true;
                }
                return false;
            }
            if (refValueStep(field.ops, step, shape)) {
                node.step = step;
                node.row = add(path, shape, reflect::assetKindOf(field, field.ops));
                return true;
            }
            if (field.kind == reflect::Kind::String && field.asset != reflect::AssetKind::None) {
                // a path string: its GUID as "<key>Guid", as an AssetRef writes it, or as "guid"
                node.step = RefStep::StringPath;
                node.guid = siblingRefField(desc, std::string(field.name) + "Guid");
                if (node.guid) {
                    node.row = add(path, Shape::Asset, field.asset);
                } else {
                    node.guid = siblingRefField(desc, "guid");
                    node.row = add(path, Shape::GuidSibling, field.asset);
                }
                return true;
            }
            return false;
        }

        ComponentType component;
        std::vector<AssetRefField>& rows;
    };

    void generateComponentRefs(const reflect::ComponentDesc& desc, std::vector<AssetRefField>& rows, std::vector<RefWalkNode>& nodes) {
        ComponentRefRows generator(desc.type, rows);
        generator.collect(desc.body, std::string(), nodes);
        for (const char* key : desc.legacyTextureKeys) {
            generator.legacyTexture(key);
        }
    }

    struct GeneratedRefTable {
        std::vector<AssetRefField> rows;
        std::vector<RefComponentWalk> walks;

        GeneratedRefTable() {
            for (const reflect::ComponentDesc* desc : reflect::all()) {
                RefComponentWalk walk;
                walk.desc = desc;
                const size_t before = rows.size();
                generateComponentRefs(*desc, rows, walk.nodes);
                if (rows.size() != before && !walk.nodes.empty()) walks.push_back(std::move(walk));
            }
        }
    };

    const GeneratedRefTable& generatedRefTable() {
        static const GeneratedRefTable table;
        return table;
    }

    std::vector<AssetRefField> buildDocumentFields() {
        std::vector<AssetRefField> rows;
        auto row = [&](Scope scope, const char* path, Shape shape, AssetType type, Base base, Kind kind) {
            rows.push_back({scope, ComponentType::Transform, path, shape, type, base, kind});
        };
        for (const char* shader : {"defaultMeshShader", "defaultUIShader", "defaultSkyShader", "defaultPointsShader", "defaultLinesShader"}) {
            row(Scope::SceneSettings, shader, Shape::Shader, AssetType::Shader, Base::Project, Kind::SceneSettings);
        }
        row(Scope::SceneSettings, "postProcess[].shader", Shape::Shader, AssetType::Shader, Base::Project, Kind::SceneSettings);
        row(Scope::SceneSettings, "postProcess[].textures{}", Shape::Texture, AssetType::Texture, Base::Assets, Kind::SceneSettings);
        row(Scope::SceneSettings, "postProcessing.lutTexture", Shape::Asset, AssetType::Texture, Base::Assets, Kind::SceneSettings);
        // Palette Quantize's palette (S43)
        row(Scope::SceneSettings, "postProcessing.palette", Shape::Asset, AssetType::Palette, Base::Assets, Kind::SceneSettings);

        row(Scope::SceneFile, "prefabs[]", Shape::PrefabRef, AssetType::Prefab, Base::Project, Kind::ScenePrefabList);
        row(Scope::SceneFile, "cppScripts[].path", Shape::Asset, AssetType::Script, Base::Project, Kind::CppScript);
        row(Scope::SceneFile, "cppScripts[].headerPath", Shape::Asset, AssetType::Script, Base::Project, Kind::CppScript);
        row(Scope::SceneFile, "childScenes[]", Shape::Asset, AssetType::Scene, Base::Project, Kind::ChildScene);

        row(Scope::Project, "scenes[].filepath", Shape::Asset, AssetType::Scene, Base::Project, Kind::SceneList);
        row(Scope::Project, "standalonePrefabs[]", Shape::PrefabRef, AssetType::Prefab, Base::Project, Kind::ScenePrefabList);
        return rows;
    }

    const AssetRefField& findRow(const std::vector<AssetRefField>& rows, Scope scope, ComponentType component, const std::string& path) {
        for (const AssetRefField& row : rows) {
            if (row.scope == scope && (scope != Scope::Component || row.component == component) && row.path == path) {
                return row;
            }
        }
        static const AssetRefField none{Scope::Component, ComponentType::Transform, "", Shape::Asset, AssetType::Unknown, Base::Project, Kind::Other};
        return none;
    }

    const AssetRefField& componentRow(ComponentType component, const std::string& path) {
        return findRow(AssetRefSchema::fields(), Scope::Component, component, path);
    }

    const AssetRefField& materialRow(const std::string& path) {
        return findRow(AssetRefSchema::materialFields(), Scope::Material, ComponentType::MeshComponent, path);
    }

    const AssetRefField& documentRow(Scope scope, const std::string& path) {
        return findRow(AssetRefSchema::documentFields(), scope, ComponentType::Transform, path);
    }

    // ---------------------------------------------------------------- memory

    void applyTextureChange(Texture& texture, Texture updated) {
        // Renders keep drawing the old GPU texture until the new one exists, so the new path
        // loads before the old texture is released
        if (TexturePool::get(texture.getRenderId())) {
            const bool asyncLoading = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            updated.getRender();
            Engine::setAsyncLoading(asyncLoading);
        }
        texture.destroy();
        texture = updated;
    }

    // One walk: the visitor, the site being visited and what changed
    class RefVisit {
    public:
        RefVisit(const AssetRefVisitor& visitor): visitor(visitor) {}

        bool changed = false;
        Entity entity = NULL_ENTITY;
        std::string component;
        // the entity is a sheet sprite (S44 R48): submesh 0's base colour is its sheet's page, the
        // sprite's reference, not the material's
        bool sheetPage = false;

        AssetRefSite site(const AssetRefField& row, const std::string& field, uint32_t ordinal = 0,
                          AssetPathBase base = AssetPathBase::Project, bool baseOverride = false) const {
            AssetRefSite site;
            site.row = &row;
            site.entity = entity;
            site.component = component;
            site.field = field;
            site.ordinal = ordinal;
            site.expected = row.expected;
            site.base = baseOverride ? base : row.base;
            site.kind = row.kind;
            return site;
        }

        // true when the path changed (what the field feeds must reload)
        bool asset(AssetRef& ref, const AssetRefField& row, const std::string& field,
                   AssetPathBase base = AssetPathBase::Project, bool baseOverride = false, AssetType expected = AssetType::Unknown) {
            if (!ref.isSet()) {
                return false;
            }
            AssetRef copy = ref;
            AssetRefSite where = site(row, field, 0, base, baseOverride);
            if (expected != AssetType::Unknown) {
                where.expected = expected;
            }
            visitor(copy, where);
            if (copy == ref) {
                return false;
            }
            changed = true;
            const bool pathChanged = copy.path != ref.path;
            ref.path = copy.path;
            ref.guid = copy.guid;
            return pathChanged;
        }

        bool shader(ShaderRef& ref, const AssetRefField& row, const std::string& field) {
            if (ref.empty() || ref.isLibrary()) {
                return false;
            }
            std::string vert, frag;
            AssetRefSchema::shaderFiles(ref.path, vert, frag);
            AssetRef vertRef(vert, ref.vertGuid);
            AssetRef fragRef(frag, ref.fragGuid);
            const AssetRef vertBefore = vertRef;
            const AssetRef fragBefore = fragRef;
            if (!vert.empty()) visitor(vertRef, site(row, field, 0));
            if (!frag.empty()) visitor(fragRef, site(row, field, 1));
            if (vertRef == vertBefore && fragRef == fragBefore) {
                return false;
            }
            changed = true;
            const bool pathChanged = vertRef.path != vertBefore.path || fragRef.path != fragBefore.path;
            if (pathChanged) {
                ref.path = vertRef.path.empty() || fragRef.path.empty() ? std::string() : Util::makeCustomShader(vertRef.path, fragRef.path);
            }
            if (ref.path.empty()) {
                ref.clear();
            } else {
                ref.vertGuid = vertRef.guid;
                ref.fragGuid = fragRef.guid;
            }
            return pathChanged;
        }

        bool texture(Texture& texture, const AssetRefField& row, const std::string& field) {
            if (texture.isFramebuffer()) {
                return false;
            }
            // Per-face paths only exist on cubemaps, incomplete ones included
            bool faces = false;
            for (size_t face = 1; face < 6; face++) {
                faces |= !texture.getPath(face).empty();
            }
            Texture updated(texture);
            bool any = false;
            bool pathChanged = false;
            bool cleared = false;
            for (size_t face = 0; face < (faces ? 6u : 1u) && !cleared; face++) {
                const std::string path = texture.getPath(face);
                if (path.empty()) {
                    continue;
                }
                AssetRef ref(path, texture.getPathGuid(face));
                const AssetRef before = ref;
                visitor(ref, site(row, field, static_cast<uint32_t>(face)));
                if (ref == before) {
                    continue;
                }
                any = true;
                if (ref.path != before.path) {
                    pathChanged = true;
                    if (ref.path.empty()) {
                        // one missing face leaves the whole cubemap unloadable; setPath("")
                        // would leave a 2D texture waiting for a load that cannot happen
                        updated = Texture();
                        cleared = true;
                        continue;
                    }
                    if (faces) {
                        updated.setCubePath(face, ref.path);
                    } else if (texture.isCubeMap()) {
                        updated.setCubeMap(ref.path);
                    } else {
                        const float svgScale = updated.getSvgScale();
                        updated.setPath(ref.path);
                        updated.setSvgScale(svgScale);
                    }
                }
                updated.setPathGuid(face, ref.guid);
            }
            if (!any) {
                return false;
            }
            changed = true;
            if (pathChanged) {
                applyTextureChange(texture, updated);
            } else {
                texture = updated;
            }
            return pathChanged;
        }

        bool textureMap(std::vector<std::pair<std::string, Texture>>& textures, const AssetRefField& row, const std::string& prefix) {
            bool pathChanged = false;
            for (auto& [uniform, texture] : textures) {
                pathChanged |= this->texture(texture, row, prefix + uniform);
            }
            return pathChanged;
        }

        // an Animation Player entry: true when what it names moved (the player loads again)
        bool clipEntry(std::string& clip, std::string& guid, const AssetRefField& row, const std::string& field) {
            anim::SubClipReference sub;
            if (anim::parseSubClipReference(clip, sub)) {
                // the model by path: its GUID has no place in the entry (empty for sub-clips)
                AssetRef ref(sub.model);
                if (!ref.isSet()) {
                    return false;
                }
                AssetRefSite where = site(row, field);
                where.expected = AssetType::Model;
                visitor(ref, where);
                if (ref.path == sub.model) {
                    return false;
                }
                changed = true;
                clip = ref.path.empty() ? std::string() : anim::formatSubClipReference(ref.path, sub.clip);
                return true;
            }
            return prefab(clip, guid, row, field);
        }

        bool prefab(std::string& path, std::string& guid, const AssetRefField& row, const std::string& field) {
            AssetRef ref(path, Guid::parse(guid));
            if (!ref.isSet()) {
                return false;
            }
            const AssetRef before = ref;
            visitor(ref, site(row, field));
            if (ref == before) {
                return false;
            }
            changed = true;
            path = ref.path;
            guid = ref.guid.str();
            return before.path != ref.path;
        }

    private:
        const AssetRefVisitor& visitor;
    };

    struct MaterialChange {
        bool texture = false;
        bool shader = false;
    };

    // skipBaseColor: a sheet sprite's page (S44), the sheet's output, not a reference of the scene
    MaterialChange visitMaterial(RefVisit& visit, Material& material, const std::string& prefix, bool skipBaseColor = false) {
        MaterialChange change;
        static const char* textureFields[] = {"baseColorTexture", "emissiveTexture", "metallicRoughnessTexture", "occlusionTexture", "normalTexture"};
        Texture* textures[] = {&material.baseColorTexture, &material.emissiveTexture, &material.metallicRoughnessTexture,
                               &material.occlusionTexture, &material.normalTexture};
        for (size_t i = skipBaseColor ? 1 : 0; i < 5; i++) {
            change.texture |= visit.texture(*textures[i], materialRow(textureFields[i]), prefix + textureFields[i]);
        }
        change.texture |= visit.textureMap(material.shaderTextures, materialRow("shaderTextures{}"), prefix + "shaderTextures.");
        // the extended lobes' maps, only on a material that has them (an unset pbr stays unallocated)
        if (MaterialPbrExtensions* pbr = material.pbr.editIfSet()) {
            for (const PbrField& field : MaterialPbr::getFields()) {
                if (field.type != PbrFieldType::Texture)
                    continue;
                const std::string path = "pbr." + MaterialPbr::getFieldPath(field);
                change.texture |= visit.texture(*static_cast<Texture*>(field.ref(*pbr)), materialRow(path.c_str()), prefix + path);
            }
        }
        change.shader |= visit.shader(material.shader, materialRow("shader"), prefix + "shader");
        if (MaterialPass* pass = material.extraPass.editIfSet()) {
            change.shader |= visit.shader(pass->shader, materialRow("extraPass.shader"), prefix + "extraPass.shader");
            change.texture |= visit.textureMap(pass->shaderTextures, materialRow("extraPass.shaderTextures{}"), prefix + "extraPass.shaderTextures.");
        }
        // the palette bindings, only on a material that has them; the draw binds a changed one itself
        if (jasperengine::pixelart::ShaderPalettes* palettes = material.shaderPalettes.editIfSet()) {
            for (auto& [uniform, binding] : *palettes) {
                const std::string at = prefix + "shaderPalettes." + uniform + ".";
                visit.asset(binding.source, materialRow("shaderPalettes{}.source"), at + "source");
                for (size_t t = 0; t < binding.targets.size(); t++) {
                    visit.asset(binding.targets[t].palette, materialRow("shaderPalettes{}.targets[].palette"),
                                at + "targets[" + std::to_string(t) + "].palette");
                }
            }
        }
        if (!prefix.empty()) {
            visit.asset(material.name, materialRow("name"), prefix + "name");
        }
        return change;
    }

    void visitScriptProperty(RefVisit& visit, ScriptProperty& property, const std::string& field) {
        const AssetRefField& row = componentRow(ComponentType::ScriptComponent, "scripts[].properties[]");
        if (property.type == ScriptPropertyType::Prefab && std::holds_alternative<PrefabAsset>(property.value)) {
            const PrefabAsset& value = std::get<PrefabAsset>(property.value);
            std::string path = value.getPath();
            std::string guid = value.getGuid();
            if (visit.prefab(path, guid, row, field) || guid != value.getGuid() || path != value.getPath()) {
                property.value = PrefabAsset(guid, path);
            }
        } else if (property.type == ScriptPropertyType::Asset && std::holds_alternative<AssetRef>(property.value)) {
            AssetRef& value = std::get<AssetRef>(property.value);
            visit.asset(value, row, field, AssetPathBase::Project, false, assetTypeFromName(property.assetType));
        }
    }

    AssetType effectAssetType(particles::ParticleModules::AssetKind kind) {
        switch (kind) {
            case particles::ParticleModules::AssetKind::Model: return AssetType::Model;
            case particles::ParticleModules::AssetKind::Effect: return AssetType::ParticleEffect;
            case particles::ParticleModules::AssetKind::Palette: return AssetType::Palette;
            case particles::ParticleModules::AssetKind::SoundEvent: return AssetType::SoundEvent;
            case particles::ParticleModules::AssetKind::Texture: break;
        }
        return AssetType::Texture;
    }

    // the asset paths an inline effect's modules hold ("inlineEffect.renderer.texture"); true
    // when one changed
    bool visitInlineEffect(RefVisit& visit, particles::ParticleEffectDocument& document, const AssetRefField& row, const std::string& prefix) {
        bool changed = false;
        particles::ParticleModules::visitAssets(document, [&](std::string& path, std::string& guid, particles::ParticleModules::AssetKind kind, const std::string& field) {
            AssetRef ref(path, Guid::parse(guid));
            if (visit.asset(ref, row, prefix + "." + field, AssetPathBase::Assets, false, effectAssetType(kind)) || ref.guid.str() != guid) {
                path = ref.path;
                guid = ref.guid.isNull() ? std::string() : ref.guid.str();
                changed = true;
            }
        });
        return changed;
    }

    void visitScriptEntries(RefVisit& visit, std::vector<ScriptEntry>& scripts) {
        const AssetRefField& path = componentRow(ComponentType::ScriptComponent, "scripts[].path");
        const AssetRefField& header = componentRow(ComponentType::ScriptComponent, "scripts[].headerPath");
        for (size_t i = 0; i < scripts.size(); i++) {
            ScriptEntry& entry = scripts[i];
            const std::string prefix = "scripts[" + std::to_string(i) + "].";
            const bool lua = entry.type == ScriptType::LUA;
            visit.asset(entry.path, path, prefix + "path", lua ? AssetPathBase::Lua : AssetPathBase::Project, true);
            visit.asset(entry.headerPath, header, prefix + "headerPath");
            for (size_t p = 0; p < entry.properties.size(); p++) {
                visitScriptProperty(visit, entry.properties[p], prefix + "properties[" + std::to_string(p) + "]");
            }
        }
    }

    // One component's moved references: what reloads
    struct MovedRefs {
        uint64_t flags = 0;
        bool any = false;
    };

    bool visitRefValue(RefVisit& visit, RefStep step, void* value, const AssetRefField& row, const std::string& field) {
        switch (step) {
            case RefStep::Texture:
                return visit.texture(*static_cast<Texture*>(value), row, field);
            case RefStep::Shader:
                return visit.shader(*static_cast<ShaderRef*>(value), row, field);
            case RefStep::Asset: {
                AssetRef& ref = *static_cast<AssetRef*>(value);
                return visit.asset(ref, row, field, AssetPathBase::Project, false, row.modelOrPrefab ? splinePieceExpectedType(ref.path) : AssetType::Unknown);
            }
            default:
                return false;
        }
    }

    void walkRefFields(RefVisit& visit, const std::vector<RefWalkNode>& nodes, void* owner, const std::string& prefix, MovedRefs& moved) {
        const std::vector<AssetRefField>& rows = AssetRefSchema::fields();
        for (const RefWalkNode& node : nodes) {
            const reflect::FieldDesc& field = *node.field;
            void* value = field.ref(owner);
            const std::string path = prefix + field.yamlKey;
            bool pathChanged = false;
            switch (node.step) {
                case RefStep::Struct:
                    walkRefFields(visit, node.children, value, path + ".", moved);
                    continue;
                case RefStep::Array: {
                    size_t count = reflect::elementCount(field, owner, value);
                    if (field.assetCount) count = std::min(count, field.assetCount(owner));
                    for (size_t i = 0; i < count; i++) {
                        void* element = field.arrayOps->at(value, i);
                        const std::string elementPath = path + "[" + std::to_string(i) + "]";
                        if (node.element == RefStep::Struct) {
                            walkRefFields(visit, node.children, element, elementPath + ".", moved);
                        } else {
                            pathChanged |= visitRefValue(visit, node.element, element, rows[node.row], elementPath);
                        }
                    }
                    break;
                }
                case RefStep::Font: {
                    FontArray& fonts = *static_cast<FontArray*>(value);
                    for (size_t i = 0; i < fonts.size(); i++) {
                        // an engine font ("builtin:pixel", S51 R16) is no file
                        if (fonts[i].path.compare(0, 8, "builtin:") == 0) continue;
                        pathChanged |= visit.asset(fonts[i], rows[node.row], path + "[" + std::to_string(i) + "]");
                    }
                    break;
                }
                case RefStep::ShaderTextures:
                    pathChanged = visit.textureMap(*static_cast<std::vector<std::pair<std::string, Texture>>*>(value), rows[node.row], path + ".");
                    break;
                case RefStep::StringPath:
                    pathChanged = visit.prefab(*static_cast<std::string*>(value), *static_cast<std::string*>(node.guid->ref(owner)), rows[node.row], path);
                    break;
                case RefStep::ClipEntry:
                    // the entry is the element: "clips[2]"
                    pathChanged = visit.clipEntry(*static_cast<std::string*>(value), *static_cast<std::string*>(node.guid->ref(owner)), rows[node.row],
                                                  prefix.substr(0, prefix.size() - 1));
                    break;
                case RefStep::PrefabRoot:
                    pathChanged = visit.prefab(*static_cast<std::string*>(value), *static_cast<std::string*>(node.guid->ref(owner)), rows[node.row], path);
                    break;
                case RefStep::Scripts:
                    visitScriptEntries(visit, *static_cast<std::vector<ScriptEntry>*>(value));
                    continue;
                case RefStep::InlineEffect:
                    pathChanged = visitInlineEffect(visit, *static_cast<particles::ParticleEffectDocument*>(value), rows[node.row], path);
                    break;
                case RefStep::Material: {
                    const MaterialChange change = visitMaterial(visit, *static_cast<Material*>(value), path + ".",
                                                                visit.sheetPage && path == "submeshes[0].material");
                    if (change.texture) moved.flags |= UpdateFlags_Mesh_Texture;
                    if (change.shader) moved.flags |= UpdateFlags_Mesh_Reload;
                    moved.any |= change.texture || change.shader;
                    continue;
                }
                default:
                    pathChanged = visitRefValue(visit, node.step, value, rows[node.row], path);
                    break;
            }
            if (pathChanged) {
                moved.flags |= field.updateFlags;
                moved.any = true;
                if (field.assetMoved) field.assetMoved(owner);
            }
        }
    }

    void visitComponentRefs(EntityRegistry& registry, Entity entity, const RefComponentWalk& walk, RefVisit& visit) {
        void* component = walk.desc->find(&registry, entity);
        if (!component) {
            return;
        }
        visit.entity = entity;
        visit.component = Catalog::getComponentName(walk.desc->type);
        const SpriteComponent* sprite = walk.desc->type == ComponentType::MeshComponent ? registry.findComponent<SpriteComponent>(entity) : nullptr;
        visit.sheetPage = sprite && !sprite->sheet.empty();
        MovedRefs moved;
        walkRefFields(visit, walk.nodes, component, std::string(), moved);
        if (moved.any) {
            Catalog::updateEntity(&registry, entity, moved.flags);
            if (walk.desc->edited) walk.desc->edited(&registry, entity);
        }
    }

    void visitRegistry(EntityRegistry& registry, Entity only, RefVisit& visit) {
        for (const RefComponentWalk& walk : generatedRefTable().walks) {
            if (only != NULL_ENTITY) {
                visitComponentRefs(registry, only, walk, visit);
                continue;
            }
            ComponentArrayBase* array = registry.getComponentArray(walk.desc->componentId(&registry));
            if (!array) {
                continue;
            }
            for (size_t i = 0; i < array->size(); i++) {
                visitComponentRefs(registry, array->getEntity(i), walk, visit);
            }
        }
    }

    // ---------------------------------------------------------------- files

    // The nodes a pattern reaches below a node: "a.b", "a[]" (each element), "a{}" (each
    // value of a map). The callback gets the value node, the map holding its last key, that
    // key, and the field with indices / map keys filled in.
    using PatternCallback = std::function<void(const YAML::Node& value, const YAML::Node& parent,
                                               const std::string& key, const std::string& field)>;

    void walkPattern(const YAML::Node& node, const std::string& pattern, const std::string& field, const PatternCallback& cb) {
        if (pattern.empty()) {
            cb(node, YAML::Node(), std::string(), field);
            return;
        }
        if (!node || !node.IsMap()) {
            return;
        }
        const size_t dot = pattern.find('.');
        std::string segment = pattern.substr(0, dot);
        const std::string rest = dot == std::string::npos ? std::string() : pattern.substr(dot + 1);
        const std::string separator = field.empty() ? "" : ".";

        auto descend = [&](const YAML::Node& child, const std::string& childField, const std::string& key) {
            if (rest.empty()) {
                cb(child, node, key, childField);
            } else {
                walkPattern(child, rest, childField, cb);
            }
        };

        if (segment.size() > 2 && segment.compare(segment.size() - 2, 2, "[]") == 0) {
            const std::string key = segment.substr(0, segment.size() - 2);
            const YAML::Node list = node[key];
            if (!list || !list.IsSequence()) {
                return;
            }
            for (size_t i = 0; i < list.size(); i++) {
                const std::string childField = field + separator + key + "[" + std::to_string(i) + "]";
                if (rest.empty()) {
                    cb(list[i], YAML::Node(), std::string(), childField);
                } else {
                    walkPattern(list[i], rest, childField, cb);
                }
            }
            return;
        }
        if (segment.size() > 2 && segment.compare(segment.size() - 2, 2, "{}") == 0) {
            const std::string key = segment.substr(0, segment.size() - 2);
            const YAML::Node map = node[key];
            if (!map || !map.IsMap()) {
                return;
            }
            for (const auto& entry : map) {
                const std::string name = entry.first.as<std::string>();
                const std::string childField = field + separator + key + "." + name;
                if (rest.empty()) {
                    cb(entry.second, YAML::Node(), std::string(), childField);
                } else {
                    walkPattern(entry.second, rest, childField, cb);
                }
            }
            return;
        }
        const YAML::Node child = node[segment];
        if (!child) {
            return;
        }
        descend(child, field + separator + segment, segment);
    }

    std::string scalar(const YAML::Node& node) {
        return node && node.IsScalar() ? node.as<std::string>() : std::string();
    }

    struct Extraction {
        const AssetRefEmitter& emit;
        bool inPrefab = false;
        uint64_t fileEntity = 0;
        std::string entityPath;
        std::string scope;
        std::string component;

        void found(const AssetRefField& row, const AssetRef& ref, const std::string& field, uint32_t ordinal = 0,
                   AssetPathBase base = AssetPathBase::Project, bool baseOverride = false, AssetType expected = AssetType::Unknown) {
            if (!ref.isSet()) {
                return;
            }
            AssetRefFound out;
            out.ref = ref;
            out.site.row = &row;
            out.site.entity = static_cast<Entity>(fileEntity);
            out.site.component = component;
            out.site.field = field;
            out.site.ordinal = ordinal;
            out.site.expected = expected != AssetType::Unknown ? expected : row.expected;
            out.site.base = baseOverride ? base : row.base;
            out.site.kind = inPrefab && row.kind == DepEdgeKind::SceneComponent ? DepEdgeKind::PrefabComponent : row.kind;
            out.fileEntity = fileEntity;
            out.entityPath = entityPath;
            out.scope = scope;
            emit(out);
        }

        void texture(const AssetRefField& row, const YAML::Node& node, const std::string& field) {
            if (!node || !node.IsMap()) {
                return;
            }
            if (node["path"]) {
                found(row, AssetRef(scalar(node["path"]), Guid::parse(scalar(node["guid"]))), field);
            }
            const YAML::Node paths = node["paths"];
            if (paths && paths.IsSequence()) {
                const YAML::Node guids = node["guids"];
                for (size_t face = 0; face < paths.size(); face++) {
                    // an engine font ("builtin:pixel", S51 R16) is no file
                    if (scalar(paths[face]).compare(0, 8, "builtin:") == 0) continue;
                    const Guid guid = guids && guids.IsSequence() && face < guids.size() ? Guid::parse(scalar(guids[face])) : Guid();
                    found(row, AssetRef(scalar(paths[face]), guid), field, static_cast<uint32_t>(face));
                }
            }
        }

        void shader(const AssetRefField& row, const std::string& value, const std::string& guids, const std::string& field) {
            if (value.empty() || value.compare(0, 8, "library:") == 0) {
                return;
            }
            std::string vert, frag;
            AssetRefSchema::shaderFiles(value, vert, frag);
            const size_t bar = guids.find('|');
            const Guid vertGuid = Guid::parse(bar == std::string::npos ? guids : guids.substr(0, bar));
            const Guid fragGuid = bar == std::string::npos ? Guid() : Guid::parse(guids.substr(bar + 1));
            found(row, AssetRef(vert, vertGuid), field, 0);
            found(row, AssetRef(frag, fragGuid), field, 1);
        }

        void row(const AssetRefField& row, const YAML::Node& root) {
            walkPattern(root, row.path, std::string(), [&](const YAML::Node& value, const YAML::Node& parent, const std::string& key, const std::string& field) {
                switch (row.shape) {
                    case AssetRefShape::Texture:
                        texture(row, value, field);
                        break;
                    case AssetRefShape::Asset: {
                        const Guid guid = parent && !key.empty() ? Guid::parse(scalar(parent[key + "Guid"])) : Guid();
                        // Lua script entries are relative to the Lua directory
                        bool lua = false;
                        if (row.path == "scripts[].path" && parent) {
                            lua = scalar(parent["type"]) == "lua";
                        }
                        const AssetType expected = row.path == "postProcessing.lutTexture" ? lutExpectedType(scalar(value))
                                                 : row.modelOrPrefab ? splinePieceExpectedType(scalar(value)) : AssetType::Unknown;
                        found(row, AssetRef(scalar(value), guid), field, 0, lua ? AssetPathBase::Lua : AssetPathBase::Project, lua, expected);
                        break;
                    }
                    case AssetRefShape::Shader: {
                        const std::string guids = parent && !key.empty() ? scalar(parent[key + "Guid"]) : std::string();
                        shader(row, scalar(value), guids, field);
                        break;
                    }
                    case AssetRefShape::AssetList: {
                        const YAML::Node guids = parent && !key.empty() ? parent[key + "Guid"] : YAML::Node();
                        if (value.IsSequence()) {
                            for (size_t i = 0; i < value.size(); i++) {
                                const Guid guid = guids && guids.IsSequence() && i < guids.size() ? Guid::parse(scalar(guids[i])) : Guid();
                                found(row, AssetRef(scalar(value[i]), guid), field + "[" + std::to_string(i) + "]");
                            }
                        } else {
                            found(row, AssetRef(scalar(value)), field + "[0]");
                        }
                        break;
                    }
                    case AssetRefShape::PrefabRef:
                        if (value.IsMap()) {
                            found(row, AssetRef(scalar(value["path"]), Guid::parse(scalar(value["guid"]))), field.empty() ? "path" : field);
                        } else {
                            found(row, AssetRef(scalar(value)), field);
                        }
                        break;
                    case AssetRefShape::ParticleEffect: {
                        particles::ParticleEffectDocument document;
                        std::vector<particles::Diagnostic> diagnostics;
                        particles::ParticleEffectCodec::read(scalar(value), document, diagnostics);
                        particles::ParticleModules::visitAssets(document, [&](std::string& path, std::string& guid, particles::ParticleModules::AssetKind kind, const std::string& inner) {
                            found(row, AssetRef(path, Guid::parse(guid)), field + "." + inner, 0, AssetPathBase::Assets, true, effectAssetType(kind));
                        });
                        break;
                    }
                    case AssetRefShape::GuidSibling:
                        found(row, AssetRef(scalar(value), parent ? Guid::parse(scalar(parent["guid"])) : Guid()), field);
                        break;
                    case AssetRefShape::ClipEntry: {
                        const std::string clip = value.IsMap() ? scalar(value["clip"]) : scalar(value);
                        anim::SubClipReference sub;
                        if (anim::parseSubClipReference(clip, sub)) {
                            found(row, AssetRef(sub.model), field, 0, AssetPathBase::Project, false, AssetType::Model);
                        } else {
                            found(row, AssetRef(clip, value.IsMap() ? Guid::parse(scalar(value["guid"])) : Guid()), field);
                        }
                        break;
                    }
                    case AssetRefShape::ScriptProperty: {
                        const std::string type = scalar(value["type"]);
                        const YAML::Node propertyValue = value["value"];
                        if ((type != "Prefab" && type != "Asset") || !propertyValue || !propertyValue.IsMap()) {
                            break;
                        }
                        const AssetType expected = type == "Prefab" ? AssetType::Prefab : assetTypeFromName(scalar(value["assetType"]));
                        found(row, AssetRef(scalar(propertyValue["path"]), Guid::parse(scalar(propertyValue["guid"]))), field,
                              0, AssetPathBase::Project, false, expected);
                        break;
                    }
                }
            });
        }

        void components(const YAML::Node& componentsNode) {
            if (!componentsNode || !componentsNode.IsMap()) {
                return;
            }
            static const std::unordered_map<std::string, ComponentType> byKey = [] {
                std::unordered_map<std::string, ComponentType> map;
                for (const AssetRefField& field : AssetRefSchema::fields()) {
                    map[Catalog::getComponentName(field.component, true)] = field.component;
                }
                return map;
            }();
            for (const auto& entry : componentsNode) {
                auto type = byKey.find(entry.first.as<std::string>());
                if (type == byKey.end()) {
                    continue;
                }
                component = Catalog::getComponentName(type->second);
                for (const AssetRefField& field : AssetRefSchema::fields()) {
                    if (field.component == type->second) {
                        row(field, entry.second);
                    }
                }
            }
            component.clear();
        }

        void prefabInstance(const YAML::Node& block, uint64_t root, const std::string& rootPath) {
            if (!block || !block.IsMap()) {
                return;
            }
            const YAML::Node overrides = block["overrides"];
            if (overrides && overrides.IsSequence()) {
                for (const YAML::Node& entry : overrides) {
                    fileEntity = root;
                    entityPath = rootPath;
                    scope = "entity:" + std::to_string(root) + "/prefabInstance[" + scalar(entry["registryEntity"]) + "]";
                    components(entry["components"]);
                }
            }
            const YAML::Node added = block["addedGameObjects"];
            if (added && added.IsSequence()) {
                for (const YAML::Node& entity : added) {
                    this->entity(entity, rootPath);
                }
            }
            const YAML::Node nested = block["nested"];
            if (nested && nested.IsSequence()) {
                for (const YAML::Node& entry : nested) {
                    prefabInstance(entry, root, rootPath);
                }
            }
        }

        void entity(const YAML::Node& node, const std::string& parentPath) {
            if (!node || !node.IsMap()) {
                return;
            }
            const uint64_t id = node["entity"] ? node["entity"].as<uint64_t>(0) : 0;
            const std::string name = scalar(node["name"]);
            const std::string path = parentPath.empty() ? name : parentPath + "/" + name;
            fileEntity = id;
            entityPath = path;
            scope = "entity:" + std::to_string(id);
            components(node["components"]);
            prefabInstance(node["prefabInstance"], id, path);
            const YAML::Node children = node["children"];
            if (children && children.IsSequence()) {
                for (const YAML::Node& child : children) {
                    entity(child, path);
                }
            }
        }

        void material(const YAML::Node& node, bool file) {
            for (const AssetRefField& field : AssetRefSchema::materialFields()) {
                if (file && field.path == "name") {
                    continue; // a .material file's own path
                }
                row(field, node);
            }
        }
    };

}

std::string AssetRefSite::locator(const std::string& scope) const {
    std::string text = scope;
    if (text.empty()) {
        text = entity != NULL_ENTITY ? "entity:" + std::to_string(entity) : std::string("settings");
    }
    text += "/";
    if (!component.empty()) {
        text += component + ".";
    }
    return text + field;
}

const std::vector<AssetRefField>& AssetRefSchema::materialFields() {
    static const std::vector<AssetRefField> rows = buildMaterialFields();
    return rows;
}

const std::vector<AssetRefField>& AssetRefSchema::fields() {
    return generatedRefTable().rows;
}

const std::vector<AssetRefField>& AssetRefSchema::documentFields() {
    static const std::vector<AssetRefField> rows = buildDocumentFields();
    return rows;
}

namespace {

    std::vector<std::string> splitDots(const std::string& text) {
        std::vector<std::string> parts;
        size_t begin = 0;
        while (begin <= text.size()) {
            const size_t dot = text.find('.', begin);
            parts.push_back(text.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin));
            if (dot == std::string::npos) break;
            begin = dot + 1;
        }
        return parts;
    }

    // "a[]" matches "a[3]", "a{}" matches "a" and the map key after it, a name matches itself;
    // a list row ("font") matches its elements ("font[1]")
    bool fieldMatches(const std::string& pattern, const std::string& field, AssetRefShape shape) {
        const std::vector<std::string> wanted = pattern.empty() ? std::vector<std::string>() : splitDots(pattern);
        const std::vector<std::string> parts = field.empty() ? std::vector<std::string>() : splitDots(field);
        size_t p = 0;
        for (size_t w = 0; w < wanted.size(); w++) {
            const std::string& segment = wanted[w];
            if (p >= parts.size()) return false;
            if (segment.size() > 2 && segment.compare(segment.size() - 2, 2, "{}") == 0) {
                if (parts[p] != segment.substr(0, segment.size() - 2) || p + 1 >= parts.size()) return false;
                p += 2;
                continue;
            }
            std::string name = segment;
            bool indexed = name.size() > 2 && name.compare(name.size() - 2, 2, "[]") == 0;
            if (shape == AssetRefShape::AssetList && w + 1 == wanted.size()) indexed = true;
            if (indexed && name.compare(name.size() - 2, 2, "[]") == 0) name.resize(name.size() - 2);
            const std::string& part = parts[p];
            if (indexed) {
                if (part.compare(0, name.size() + 1, name + "[") != 0 || part.back() != ']') return false;
            } else if (part != name) {
                return false;
            }
            p++;
        }
        // an inline effect's row holds the paths inside it ("inlineEffect.renderer.texture")
        return p == parts.size() || (pattern.empty() && field == "path") || (shape == AssetRefShape::ParticleEffect && p > 0);
    }

}

const AssetRefField* AssetRefSchema::findRow(AssetRefScope scope, ComponentType component, const std::string& field) {
    const std::vector<AssetRefField>& rows = scope == AssetRefScope::Component ? fields()
                                           : scope == AssetRefScope::Material ? materialFields() : documentFields();
    for (const AssetRefField& row : rows) {
        if (row.scope != scope || (scope == AssetRefScope::Component && row.component != component)) continue;
        if (fieldMatches(row.path, field, row.shape)) return &row;
    }
    return nullptr;
}

bool AssetRefSchema::componentByName(const std::string& name, ComponentType& component) {
    for (const AssetRefField& row : fields()) {
        if (Catalog::getComponentName(row.component) == name) {
            component = row.component;
            return true;
        }
    }
    return false;
}

void AssetRefSchema::shaderFiles(const std::string& value, std::string& vert, std::string& frag) {
    vert.clear();
    frag.clear();
    if (value.empty() || value.compare(0, 8, "library:") == 0) {
        return;
    }
    const Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(value);
    vert = paths.vert;
    frag = paths.frag;
}

bool AssetRefSchema::forEachAssetRef(EntityRegistry& registry, const AssetRefVisitor& visitor) {
    RefVisit visit(visitor);
    visitRegistry(registry, NULL_ENTITY, visit);
    return visit.changed;
}

bool AssetRefSchema::forEachAssetRef(EntityRegistry& registry, Entity entity, const AssetRefVisitor& visitor) {
    RefVisit visit(visitor);
    if (entity != NULL_ENTITY) {
        visitRegistry(registry, entity, visit);
    }
    return visit.changed;
}

bool AssetRefSchema::forEachMaterialRef(Material& material, const std::string& fieldPrefix, const AssetRefVisitor& visitor,
                                        Entity entity, const std::string& component) {
    RefVisit visit(visitor);
    visit.entity = entity;
    visit.component = component;
    visitMaterial(visit, material, fieldPrefix);
    return visit.changed;
}

bool AssetRefSchema::forEachSceneSettingsRef(Scene& scene, const AssetRefVisitor& visitor) {
    RefVisit visit(visitor);
    const std::pair<const char*, ShaderType> defaults[] = {
        {"defaultMeshShader", ShaderType::MESH}, {"defaultUIShader", ShaderType::UI}, {"defaultSkyShader", ShaderType::SKYBOX},
        {"defaultPointsShader", ShaderType::POINTS}, {"defaultLinesShader", ShaderType::LINES}};
    for (const auto& [name, type] : defaults) {
        ShaderRef ref = scene.getDefaultShaderRef(type);
        if (visit.shader(ref, documentRow(Scope::SceneSettings, name), name) || ref != scene.getDefaultShaderRef(type)) {
            scene.setDefaultShaderRef(type, ref);
        }
    }

    std::vector<PostProcessPass> passes = scene.getPostProcessPasses();
    bool passesChanged = false;
    for (size_t i = 0; i < passes.size(); i++) {
        const std::string prefix = "postProcess[" + std::to_string(i) + "].";
        const bool before = visit.changed;
        visit.changed = false;
        visit.shader(passes[i].shader, documentRow(Scope::SceneSettings, "postProcess[].shader"), prefix + "shader");
        visit.textureMap(passes[i].textures, documentRow(Scope::SceneSettings, "postProcess[].textures{}"), prefix + "textures.");
        passesChanged |= visit.changed;
        visit.changed |= before;
    }
    if (passesChanged) {
        scene.setPostProcessPasses(passes);
    }

    PostProcessingSettings settings = scene.getPostProcessing();
    if (visit.asset(settings.lutTexture, documentRow(Scope::SceneSettings, "postProcessing.lutTexture"), "postProcessing.lutTexture",
                    AssetPathBase::Project, false, lutExpectedType(settings.lutTexture.path)) ||
        settings.lutTexture != scene.getPostProcessing().lutTexture) {
        scene.setPostProcessing(settings);
    }
    settings = scene.getPostProcessing();
    if (visit.asset(settings.palette, documentRow(Scope::SceneSettings, "postProcessing.palette"), "postProcessing.palette") ||
        settings.palette != scene.getPostProcessing().palette) {
        scene.setPostProcessing(settings);
    }
    return visit.changed;
}

void AssetRefSchema::extractDocumentRefs(const YAML::Node& root, bool inPrefab, const AssetRefEmitter& emit) {
    if (!root || !root.IsMap()) {
        return;
    }
    Extraction extraction{emit, inPrefab};

    // scene settings and the lists at the root of a scene file
    if (const YAML::Node settings = root["scene"]) {
        extraction.scope = "settings";
        for (const AssetRefField& field : documentFields()) {
            if (field.scope == Scope::SceneSettings) {
                extraction.row(field, settings);
            }
        }
    }
    extraction.scope = "scene";
    for (const AssetRefField& field : documentFields()) {
        if (field.scope != Scope::SceneFile) {
            continue;
        }
        if (field.kind == DepEdgeKind::ChildScene) {
            const YAML::Node children = root["childScenes"];
            if (children && children.IsSequence()) {
                for (size_t i = 0; i < children.size(); i++) {
                    AssetRefFound out;
                    out.site.row = &field;
                    out.site.field = "childScenes[" + std::to_string(i) + "]";
                    out.site.expected = AssetType::Scene;
                    out.site.kind = DepEdgeKind::ChildScene;
                    out.sceneId = children[i]["id"] ? children[i]["id"].as<uint32_t>(0) : 0;
                    out.scope = "scene";
                    if (out.sceneId != 0) {
                        emit(out);
                    }
                }
            }
            continue;
        }
        extraction.row(field, root);
    }

    // the GameObjects: a scene's entities, a prefab's members (each with its children)
    for (const char* key : {"entities", "members"}) {
        const YAML::Node list = root[key];
        if (list && list.IsSequence()) {
            for (const YAML::Node& entity : list) {
                extraction.entity(entity, std::string());
            }
        }
    }
    // a version 1 prefab file is the root entity node itself
    if (inPrefab && !root["members"] && root["components"]) {
        extraction.entity(root, std::string());
    }
}

void AssetRefSchema::extractMaterialFileRefs(const YAML::Node& root, const AssetRefEmitter& emit) {
    if (!root || !root.IsMap()) {
        return;
    }
    Extraction extraction{emit, false};
    extraction.scope = "material";
    extraction.material(root, true);
}

void AssetRefSchema::extractProjectRefs(const YAML::Node& root, const AssetRefEmitter& emit) {
    if (!root || !root.IsMap()) {
        return;
    }
    Extraction extraction{emit, false};
    extraction.scope = "project";
    for (const AssetRefField& field : documentFields()) {
        if (field.scope == Scope::Project) {
            extraction.row(field, root);
        }
    }
}
