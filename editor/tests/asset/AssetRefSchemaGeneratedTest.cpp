// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The table of asset references, generated from the reflection tables (docs/specs/S73-code-cleanups.md
// R1). asset_ref_rows_frozen.txt and asset_ref_results_frozen.txt are what the hand-written table
// and walk gave: every row of it, and what its two walks find in
// tools/fixtures/reflection-parity/AllComponents.scene. They are not to be rewritten: a new field
// adds a row, and these still hold. A moved reference reloads what it feeds, component by
// component, as that walk did.

#include "support/EditorTestSupport.h"

#include "asset/AssetRefSchema.h"

#include "Catalog.h"
#include "reflect/Field.h"
#include "reflect/Reflect.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/PrefabComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ScriptComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "registry/EntityRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace asset_ref_schema_generated_test {

    const char* scopeName(AssetRefScope scope) {
        switch (scope) {
            case AssetRefScope::Component: return "component";
            case AssetRefScope::Material: return "material";
            case AssetRefScope::SceneSettings: return "settings";
            case AssetRefScope::SceneFile: return "scene";
            case AssetRefScope::Project: return "project";
        }
        return "";
    }

    const char* shapeName(AssetRefShape shape) {
        switch (shape) {
            case AssetRefShape::Asset: return "asset";
            case AssetRefShape::Shader: return "shader";
            case AssetRefShape::Texture: return "texture";
            case AssetRefShape::AssetList: return "list";
            case AssetRefShape::PrefabRef: return "prefab";
            case AssetRefShape::ScriptProperty: return "script_property";
            case AssetRefShape::ParticleEffect: return "particle_effect";
            case AssetRefShape::ClipEntry: return "clip_entry";
            case AssetRefShape::GuidSibling: return "guid_sibling";
        }
        return "";
    }

    const char* baseName(AssetPathBase base) {
        switch (base) {
            case AssetPathBase::Project: return "project";
            case AssetPathBase::Assets: return "assets";
            case AssetPathBase::Lua: return "lua";
        }
        return "";
    }

    std::string rowText(const AssetRefField& row) {
        std::string text = scopeName(row.scope);
        text += "\t";
        text += row.scope == AssetRefScope::Component ? Catalog::getComponentName(row.component) : std::string("-");
        text += "\t" + row.path + "\t" + shapeName(row.shape) + "\t" + assetTypeName(row.expected) + "\t" + baseName(row.base) + "\t" +
                depEdgeKindName(row.kind) + (row.legacy ? "\tlegacy" : "");
        return text;
    }

    std::vector<std::string> allRows() {
        std::vector<std::string> rows;
        for (const auto* table : {&AssetRefSchema::fields(), &AssetRefSchema::materialFields(), &AssetRefSchema::documentFields()}) {
            for (const AssetRefField& row : *table) rows.push_back(rowText(row));
        }
        return rows;
    }

    fs::path frozenPath(const char* name) {
        return fs::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "asset" / name;
    }

    std::vector<std::string> readLines(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(in.good()) << path;
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        return lines;
    }

    std::string siteText(const char* walk, const std::string& locator, uint32_t ordinal, const AssetRef& ref, const AssetRefSite& site) {
        return std::string(walk) + "\t" + locator + "#" + std::to_string(ordinal) + "\t" + ref.path + "\t" + ref.guid.str() + "\t" +
               assetTypeName(site.expected) + "\t" + baseName(site.base) + "\t" + depEdgeKindName(site.kind);
    }

    std::vector<std::string> allComponentsResults() {
        const fs::path scenePath = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "reflection-parity" / "AllComponents.scene";
        std::vector<std::string> lines;
        AssetRefSchema::extractDocumentRefs(YAML::LoadFile(scenePath.string()), false, [&](const AssetRefFound& found) {
            lines.push_back(siteText("file", found.site.locator(found.scope), found.site.ordinal, found.ref, found.site));
        });
        DecodedScene decoded = decodeSceneFile(scenePath);
        AssetRefSchema::forEachAssetRef(*decoded.scene->scene, [&](AssetRef& ref, const AssetRefSite& site) {
            lines.push_back(siteText("memory", site.locator(), site.ordinal, ref, site));
        });
        std::sort(lines.begin(), lines.end());
        return lines;
    }

    TEST(AssetRefSchemaFrozen, EveryRowOfTheHandTable) {
        const std::vector<std::string> rows = allRows();
        const std::vector<std::string> frozen = readLines(frozenPath("asset_ref_rows_frozen.txt"));
        ASSERT_FALSE(frozen.empty());
        const std::set<std::string> generated(rows.begin(), rows.end());
        for (const std::string& row : frozen) EXPECT_TRUE(generated.count(row)) << "not in the table: " << row;
    }

    // What a test's visitor does: the path of the one field named moves
    bool moveField(EntityRegistry& registry, Entity entity, const std::string& field) {
        bool visited = false;
        const bool changed = AssetRefSchema::forEachAssetRef(registry, entity, [&](AssetRef& ref, const AssetRefSite& site) {
            if (site.field != field) return;
            visited = true;
            ref.path = "moved/" + ref.path;
        });
        EXPECT_TRUE(visited) << field;
        return changed;
    }

    template<typename T>
    struct OneComponent {
        EntityRegistry registry;
        Entity entity;
        explicit OneComponent(T value) {
            entity = registry.createEntity();
            registry.addComponent<T>(entity, std::move(value));
        }
        T& get() { return *registry.findComponent<T>(entity); }
    };

    Texture textureAt(const char* path) {
        Texture texture;
        texture.setPath(path);
        return texture;
    }

    // The same, but for the reference the hand table had missed (docs/specs/S73-code-cleanups.md,
    // Deviations): a row of its own now, in both walks
    TEST(AssetRefSchemaFrozen, WhatItsWalksFindInAScene) {
        std::vector<std::string> results = allComponentsResults();
        const auto isWeatherMap = [](const std::string& line) { return line.find("VolumetricCloudsComponent.weatherMap") != std::string::npos; };
        EXPECT_EQ(std::count_if(results.begin(), results.end(), isWeatherMap), 2);
        results.erase(std::remove_if(results.begin(), results.end(), isWeatherMap), results.end());
        EXPECT_EQ(results, readLines(frozenPath("asset_ref_results_frozen.txt")));
    }

    // A reference field of a component's table is all a row takes

    struct GeneratedProbe {
        Texture map;
        std::vector<Texture> layers;
        AssetRef model;
        AssetRef unnamed;
        ShaderRef shader;
        FontArray fonts;
        std::vector<std::pair<std::string, Texture>> uniforms;
        std::string clip;
        std::string clipGuid;
        std::string animation;
        std::string guid;
        float notAReference = 0.0f;
        Texture runtimeOnly;
    };

    constexpr reflect::FieldDesc kGeneratedProbeFields[] = {
        JE_FIELD(GeneratedProbe, map),
        JE_FIELD(GeneratedProbe, layers),
        JE_FIELD(GeneratedProbe, model, .asset = reflect::AssetKind::Model),
        JE_FIELD(GeneratedProbe, unnamed),
        JE_FIELD(GeneratedProbe, shader),
        JE_FIELD(GeneratedProbe, fonts),
        JE_FIELD(GeneratedProbe, uniforms, .ops = &reflect::ops::ShaderTextures),
        JE_FIELD(GeneratedProbe, clip, .asset = reflect::AssetKind::AnimationClip),
        JE_FIELD(GeneratedProbe, clipGuid),
        JE_FIELD(GeneratedProbe, animation, .asset = reflect::AssetKind::VertexAnimation),
        JE_FIELD(GeneratedProbe, guid),
        JE_FIELD(GeneratedProbe, notAReference),
        JE_FIELD(GeneratedProbe, runtimeOnly, .flags = reflect::NoSave),
    };

    TEST(AssetRefSchemaGenerated, ARowForEachReferenceFieldOfATable) {
        reflect::ComponentDesc desc{};
        desc.type = ComponentType::LightComponent;
        desc.body = JE_STRUCT_DESC(GeneratedProbe, "GeneratedProbe", kGeneratedProbeFields);
        std::vector<std::string> rows;
        for (const AssetRefField& row : AssetRefSchema::fieldsOf(desc)) rows.push_back(rowText(row));
        const std::string kind = depEdgeKindName(DepEdgeKind::SceneComponent);
        const auto line = [&](const std::string& path, const std::string& shape, const std::string& expected, const std::string& base) {
            return "component\tLightComponent\t" + path + "\t" + shape + "\t" + expected + "\t" + base + "\t" + kind;
        };
        EXPECT_EQ(rows, (std::vector<std::string>{
            line("map", "texture", "texture", "assets"),
            line("layers[]", "texture", "texture", "assets"),
            line("model", "asset", "model", "assets"),
            line("unnamed", "asset", assetTypeName(AssetType::Unknown), "assets"),
            line("shader", "shader", "shader", "project"),
            line("fonts", "list", "font", "assets"),
            line("uniforms{}", "texture", "texture", "assets"),
            line("clip", "asset", "animation_clip", "assets"),
            line("animation", "guid_sibling", "vertex_animation", "assets"),
        }));
    }

    // Two references no one wrote a row for: they appear from the component tables, in memory and
    // in a closed file, and a moved file rewrites them
    TEST(AssetRefSchemaGenerated, ReferencesTheHandTableMissed) {
        VolumetricCloudsComponent clouds;
        clouds.weatherMap = textureAt("textures/weather.png");
        OneComponent<VolumetricCloudsComponent> c(clouds);
        EXPECT_TRUE(moveField(c.registry, c.entity, "weatherMap"));
        EXPECT_EQ(c.get().weatherMap.getPath(), "moved/textures/weather.png");

        AnimationPlayerComponent player;
        player.layers.resize(1);
        player.layers[0].mask = "masks/upper.mask";
        player.layers[0].maskGuid = "0123456789abcdef0123456789abcdef";
        OneComponent<AnimationPlayerComponent> p(player);
        p.get().needsLoad = false;
        EXPECT_TRUE(moveField(p.registry, p.entity, "layers[0].mask"));
        EXPECT_EQ(p.get().layers[0].mask, "moved/masks/upper.mask");
        EXPECT_EQ(p.get().layers[0].maskGuid, "0123456789abcdef0123456789abcdef");
        EXPECT_TRUE(p.get().needsLoad);

        const YAML::Node root = YAML::Load(R"(
entities:
  - type: Entity
    entity: 12
    name: Hero
    components:
      animationplayer:
        layers:
          - {name: Upper, mask: masks/upper.mask, maskGuid: 0123456789abcdef0123456789abcdef}
      volumetricclouds:
        weatherMap: {path: textures/weather.png}
)");
        std::vector<std::string> found;
        AssetRefSchema::extractDocumentRefs(root, false, [&](const AssetRefFound& ref) {
            found.push_back(ref.site.locator(ref.scope) + " " + ref.ref.path + " " + ref.ref.guid.str() + " " + assetTypeName(ref.site.expected));
        });
        EXPECT_EQ(found, (std::vector<std::string>{
            "entity:12/AnimationPlayerComponent.layers[0].mask masks/upper.mask 0123456789abcdef0123456789abcdef " + std::string(assetTypeName(AssetType::AvatarMask)),
            "entity:12/VolumetricCloudsComponent.weatherMap textures/weather.png  texture",
        }));
    }

    const char* const kCustomShader = "shaders/a.vert|shaders/a.frag";

    TEST(AssetRefSchemaFlags, Mesh) {
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].material.baseColorTexture = textureAt("textures/a.png");
        mesh.submeshes[0].material.shader = ShaderRef(kCustomShader);
        mesh.submeshes[1].material.baseColorTexture = textureAt("textures/unused.png");
        mesh.customShader = ShaderRef(kCustomShader);
        mesh.customDepthShader = ShaderRef(kCustomShader);
        OneComponent<MeshComponent> c(mesh);
        // a slot past numSubmeshes is no reference
        std::vector<std::string> paths;
        AssetRefSchema::forEachAssetRef(c.registry, c.entity, [&](AssetRef& ref, const AssetRefSite&) { paths.push_back(ref.path); });
        EXPECT_EQ(std::count(paths.begin(), paths.end(), "textures/unused.png"), 0);

        c.get().submeshes[0].needUpdateTexture = false;
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "submeshes[0].material.baseColorTexture"));
        EXPECT_TRUE(c.get().submeshes[0].needUpdateTexture);
        EXPECT_FALSE(c.get().needReload);
        EXPECT_EQ(c.get().submeshes[0].material.baseColorTexture.getPath(), "moved/textures/a.png");

        EXPECT_TRUE(moveField(c.registry, c.entity, "submeshes[0].material.shader"));
        EXPECT_TRUE(c.get().needReload);
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "customShader"));
        EXPECT_TRUE(c.get().needReload);
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "customDepthShader"));
        EXPECT_TRUE(c.get().needReload);
    }

    TEST(AssetRefSchemaFlags, UIFamily) {
        UIComponent ui;
        ui.texture = textureAt("textures/a.png");
        ui.customShader = ShaderRef(kCustomShader);
        ui.shaderTextures = {{"u_tex", textureAt("textures/b.png")}};
        OneComponent<UIComponent> c(ui);
        c.get().needUpdateTexture = false;
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "texture"));
        EXPECT_TRUE(c.get().needUpdateTexture);
        EXPECT_FALSE(c.get().needReload);
        EXPECT_TRUE(moveField(c.registry, c.entity, "customShader"));
        EXPECT_TRUE(c.get().needReload);
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "shaderTextures.u_tex"));
        EXPECT_TRUE(c.get().needReload);

        ButtonComponent button;
        button.textureHovered = textureAt("textures/a.png");
        OneComponent<ButtonComponent> b(button);
        b.get().needUpdateButton = false;
        EXPECT_TRUE(moveField(b.registry, b.entity, "textureHovered"));
        EXPECT_TRUE(b.get().needUpdateButton);

        TextComponent text;
        text.font[1].set("fonts/a.ttf", Guid());
        OneComponent<TextComponent> t(text);
        t.get().needReloadAtlas = false;
        EXPECT_TRUE(moveField(t.registry, t.entity, "font[1]"));
        EXPECT_TRUE(t.get().needReloadAtlas);

        UIDocumentComponent document;
        document.filename = AssetRef("ui/a.html");
        OneComponent<UIDocumentComponent> d(document);
        d.get().needReload = false;
        EXPECT_TRUE(moveField(d.registry, d.entity, "filename"));
        EXPECT_TRUE(d.get().needReload);
    }

    TEST(AssetRefSchemaFlags, Terrain) {
        TerrainComponent terrain;
        terrain.heightMap = textureAt("textures/h.png");
        terrain.blendMaps = {textureAt("textures/b.png")};
        terrain.surfaceLayers.resize(1);
        terrain.surfaceLayers[0].roughnessTexture = textureAt("textures/r.png");
        terrain.foliageLayers.resize(1);
        terrain.foliageLayers[0].meshPath = AssetRef("models/grass.glb");
        terrain.foliageLayers[0].densityMap = textureAt("textures/d.png");
        OneComponent<TerrainComponent> c(terrain);
        auto clear = [&]() {
            c.get().needUpdateTexture = false;
            c.get().needUpdateFoliage = false;
            c.get().needUpdateTerrain = false;
        };
        clear();
        c.get().heightMapLoaded = true;
        EXPECT_TRUE(moveField(c.registry, c.entity, "heightMap"));
        EXPECT_FALSE(c.get().heightMapLoaded) << "the node tree and the physics heightfield are built from the map";
        EXPECT_TRUE(c.get().needUpdateTerrain);
        EXPECT_TRUE(c.get().needUpdateTexture);
        clear();
        EXPECT_TRUE(moveField(c.registry, c.entity, "blendMaps[0]"));
        EXPECT_TRUE(c.get().needUpdateTexture);
        EXPECT_FALSE(c.get().needUpdateTerrain);
        clear();
        EXPECT_TRUE(moveField(c.registry, c.entity, "surfaceLayers[0].roughnessTexture"));
        EXPECT_TRUE(c.get().needUpdateTexture);
        EXPECT_FALSE(c.get().needUpdateFoliage);
        clear();
        EXPECT_TRUE(moveField(c.registry, c.entity, "foliageLayers[0].meshPath"));
        EXPECT_TRUE(c.get().needUpdateFoliage);
        clear();
        EXPECT_TRUE(moveField(c.registry, c.entity, "foliageLayers[0].densityMap"));
        EXPECT_TRUE(c.get().needUpdateFoliage);
    }

    TEST(AssetRefSchemaFlags, LightsAndSky) {
        LightComponent light;
        light.spotMask = textureAt("textures/a.png");
        OneComponent<LightComponent> l(light);
        l.get().needUpdateShadowCamera = false;
        EXPECT_TRUE(moveField(l.registry, l.entity, "spotMask"));
        EXPECT_TRUE(l.get().needUpdateShadowCamera);

        ReflectionProbeComponent probe;
        probe.texture = textureAt("textures/a.png");
        OneComponent<ReflectionProbeComponent> p(probe);
        p.get().needUpdate = false;
        EXPECT_TRUE(moveField(p.registry, p.entity, "texture"));
        EXPECT_TRUE(p.get().needUpdate);

        SkyboxComponent sky;
        sky.texture = textureAt("textures/a.png");
        sky.customShader = ShaderRef(kCustomShader);
        sky.shaderTextures = {{"u_tex", textureAt("textures/b.png")}};
        OneComponent<SkyboxComponent> s(sky);
        s.get().needUpdateTexture = false;
        s.get().needReload = false;
        EXPECT_TRUE(moveField(s.registry, s.entity, "texture"));
        EXPECT_TRUE(s.get().needUpdateTexture);
        EXPECT_FALSE(s.get().needReload);
        EXPECT_TRUE(moveField(s.registry, s.entity, "customShader"));
        EXPECT_TRUE(s.get().needReload);
        s.get().needReload = false;
        EXPECT_TRUE(moveField(s.registry, s.entity, "shaderTextures.u_tex"));
        EXPECT_TRUE(s.get().needReload);
    }

    template<typename T>
    void shaderReloads() {
        T value;
        value.customShader = ShaderRef(kCustomShader);
        value.shaderTextures = {{"u_tex", textureAt("textures/b.png")}};
        OneComponent<T> c(value);
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "customShader"));
        EXPECT_TRUE(c.get().needReload);
        c.get().needReload = false;
        EXPECT_TRUE(moveField(c.registry, c.entity, "shaderTextures.u_tex"));
        EXPECT_TRUE(c.get().needReload);
    }

    TEST(AssetRefSchemaFlags, CustomShadedRenderers) {
        shaderReloads<PointsComponent>();
        shaderReloads<LineRendererComponent>();
        shaderReloads<TrailRendererComponent>();

        PointsComponent points;
        points.texture = textureAt("textures/a.png");
        OneComponent<PointsComponent> p(points);
        p.get().needUpdateTexture = false;
        EXPECT_TRUE(moveField(p.registry, p.entity, "texture"));
        EXPECT_TRUE(p.get().needUpdateTexture);
    }

    TEST(AssetRefSchemaFlags, ModelsAudioAndEffects) {
        ModelComponent model;
        model.filename = AssetRef("models/a.glb");
        OneComponent<ModelComponent> m(model);
        m.get().needUpdateModel = false;
        EXPECT_TRUE(moveField(m.registry, m.entity, "filename"));
        EXPECT_TRUE(m.get().needUpdateModel);

        AudioSourceComponent audio;
        audio.filename = AssetRef("sounds/a.wav");
        OneComponent<AudioSourceComponent> a(audio);
        a.get().loaded = true;
        a.get().loadFailed = true;
        a.get().needUpdate = false;
        EXPECT_TRUE(moveField(a.registry, a.entity, "filename"));
        EXPECT_FALSE(a.get().loaded) << "loaded from the old path";
        EXPECT_FALSE(a.get().loadFailed);
        EXPECT_TRUE(a.get().needUpdate);

        ParticleSystemComponent particles;
        particles.effect = AssetRef("effects/a.particles");
        OneComponent<ParticleSystemComponent> p(particles);
        p.get().needReload = false;
        EXPECT_TRUE(moveField(p.registry, p.entity, "effect"));
        EXPECT_TRUE(p.get().needReload);
    }

    TEST(AssetRefSchemaFlags, Animators) {
        PoseAnimatorComponent pose;
        pose.animation = AssetRef("anim/a.poseanim");
        OneComponent<PoseAnimatorComponent> p(pose);
        p.get().needsLoad = false;
        EXPECT_TRUE(moveField(p.registry, p.entity, "animation"));
        EXPECT_TRUE(p.get().needsLoad);

        AnimatorComponent animator;
        animator.controller = AssetRef("anim/a.controller");
        OneComponent<AnimatorComponent> a(animator);
        a.get().needsLoad = false;
        EXPECT_TRUE(moveField(a.registry, a.entity, "controller"));
        EXPECT_TRUE(a.get().needsLoad);

        VertexAnimatorComponent vertex;
        vertex.animation = "anim/a.vat";
        vertex.guid = "0123456789abcdef0123456789abcdef";
        OneComponent<VertexAnimatorComponent> v(vertex);
        v.get().needsLoad = false;
        EXPECT_TRUE(moveField(v.registry, v.entity, "animation"));
        EXPECT_TRUE(v.get().needsLoad);
        EXPECT_EQ(v.get().animation, "moved/anim/a.vat");
        EXPECT_EQ(v.get().guid, "0123456789abcdef0123456789abcdef");
    }

    TEST(AssetRefSchemaFlags, ThoseWithNothingToReload) {
        SplineInstantiateComponent spline;
        spline.posts.resize(1);
        spline.posts[0].asset = AssetRef("prefabs/post.prefab");
        spline.caps.resize(1);
        spline.caps[0].asset = AssetRef("models/cap.glb");
        OneComponent<SplineInstantiateComponent> s(spline);
        std::vector<std::string> kinds;
        AssetRefSchema::forEachAssetRef(s.registry, s.entity, [&](AssetRef& ref, const AssetRefSite& site) {
            kinds.push_back(site.field + " " + assetTypeName(site.expected));
        });
        std::sort(kinds.begin(), kinds.end());
        EXPECT_EQ(kinds, (std::vector<std::string>{"caps[0].asset model", "posts[0].asset prefab"}));
        EXPECT_TRUE(moveField(s.registry, s.entity, "posts[0].asset"));
        EXPECT_EQ(s.get().posts[0].asset.path, "moved/prefabs/post.prefab");

        ScriptComponent scripts;
        scripts.scripts.resize(2);
        scripts.scripts[0].type = ScriptType::LUA;
        scripts.scripts[0].path = AssetRef("lua/a.lua");
        scripts.scripts[1].type = ScriptType::CPP;
        scripts.scripts[1].path = AssetRef("src/b.cpp");
        scripts.scripts[1].headerPath = AssetRef("src/b.h");
        OneComponent<ScriptComponent> c(scripts);
        std::vector<std::string> bases;
        AssetRefSchema::forEachAssetRef(c.registry, c.entity, [&](AssetRef& ref, const AssetRefSite& site) {
            bases.push_back(site.field + " " + baseName(site.base));
        });
        std::sort(bases.begin(), bases.end());
        EXPECT_EQ(bases, (std::vector<std::string>{"scripts[0].path lua", "scripts[1].headerPath project", "scripts[1].path project"}));
        EXPECT_TRUE(moveField(c.registry, c.entity, "scripts[1].headerPath"));
        EXPECT_EQ(c.get().scripts[1].headerPath.path, "moved/src/b.h");

        PrefabComponent prefab;
        prefab.path = "prefabs/a.prefab";
        prefab.guid = "0123456789abcdef0123456789abcdef";
        OneComponent<PrefabComponent> f(prefab);
        EXPECT_TRUE(moveField(f.registry, f.entity, "path"));
        EXPECT_EQ(f.get().path, "moved/prefabs/a.prefab");
    }

}
