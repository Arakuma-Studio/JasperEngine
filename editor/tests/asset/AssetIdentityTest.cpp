// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset identity in the editor (docs/specs/S04-asset-identity.md §7): the meta format, the
// content hash, model sub-assets, the AssetDatabase on scratch projects (upgrade, external
// moves, copies, sub-asset matching, resolution), the dependency index and the reference
// field table.

#include <gtest/gtest.h>

#include "asset/AssetIndex.h"
#include "asset/AssetMeta.h"
#include "asset/AssetRefSchema.h"
#include "asset/AssetTypes.h"
#include "asset/ContentHash.h"
#include "asset/ModelSubAssets.h"
#include "asset/TextReferences.h"
#include "util/AssetDatabase.h"
#include "util/SafeWrite.h"

#include "Catalog.h"
#include "reflect/Reflect.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/PointsComponent.h"
#include "component/PrefabComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ScriptComponent.h"
#include "component/SkyboxComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "registry/EntityRegistry.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // A scratch project directory, removed afterwards
    class AssetTestDir {
    public:
        AssetTestDir() {
            std::random_device device;
            path = fs::temp_directory_path() / ("je-asset-test-" + std::to_string(device()) + std::to_string(device()));
            fs::create_directories(path);
        }
        ~AssetTestDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        fs::path path;
    };

    std::string read(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void write(const fs::path& file, const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // mtimes have coarse granularity on some filesystems: a rewrite must look changed
    void touchLater(const fs::path& file) {
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    }

    fs::path repoFixture(const std::string& name) {
        return fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / name;
    }

    fs::path copyFixture(const std::string& name, const fs::path& into) {
        fs::copy(repoFixture(name), into, fs::copy_options::recursive);
        return into;
    }

    std::string gltf(const std::vector<std::string>& meshNames, const std::string& padding = std::string()) {
        std::string meshes;
        for (size_t i = 0; i < meshNames.size(); i++) {
            meshes += std::string(i ? ", " : "") + (meshNames[i].empty() ? "{\"primitives\": []}" : "{\"name\": \"" + meshNames[i] + "\", \"primitives\": []}");
        }
        return "{\"asset\": {\"version\": \"2.0\"}, \"meshes\": [" + meshes + "], \"materials\": [{\"name\": \"Paint\"}]}" + padding + "\n";
    }

    std::map<std::string, Guid> subAssetGuids(const AssetDatabase& database, const std::string& path) {
        AssetDatabase::AssetInfo info;
        std::map<std::string, Guid> byKey;
        if (database.getInfo(fs::path(path), info)) {
            for (const AssetDatabase::SubAsset& sub : info.subAssets) byKey[sub.key] = sub.guid;
        }
        return byKey;
    }

}

// ==================================================================== AssetMeta

TEST(AssetMeta, NewMetaIsCanonical) {
    AssetMeta meta;
    meta.guid = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    meta.addFormerGuid(Guid::parse("9c1d0f3e5b7a42c8a1e06d2f4b8c7a19"));
    meta.addFormerGuid(Guid::parse("0c1d0f3e5b7a42c8a1e06d2f4b8c7a19"));
    meta.setSubAssets({{"mesh:Body", Guid::parse("7f2c0f3e5b7a42c8a1e06d2f4b8c7a19"), "Body"},
                       {"animation#3", Guid::parse("21d00f3e5b7a42c8a1e06d2f4b8c7a19"), ""}});
    EXPECT_EQ(meta.serialize(),
        "metaVersion: 1\n"
        "guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\n"
        "formerGuids: [0c1d0f3e5b7a42c8a1e06d2f4b8c7a19, 9c1d0f3e5b7a42c8a1e06d2f4b8c7a19]\n"
        "subAssets:\n"
        "  - {key: \"animation#3\", guid: 21d00f3e5b7a42c8a1e06d2f4b8c7a19, name: \"\"}\n"
        "  - {key: \"mesh:Body\", guid: 7f2c0f3e5b7a42c8a1e06d2f4b8c7a19, name: Body}\n");
    AssetMeta parsed;
    ASSERT_TRUE(AssetMeta::parse(meta.serialize(), parsed));
    EXPECT_EQ(parsed.guid, meta.guid);
    EXPECT_EQ(parsed.formerGuids, meta.formerGuids);
    ASSERT_EQ(parsed.subAssets.size(), 2u);
    EXPECT_EQ(parsed.subAssets[1].name, "Body");
}

// AC15: sections this editor does not own survive edits byte for byte
TEST(AssetMeta, PreservesUnknownSections) {
    const std::string text =
        "metaVersion: 1\n"
        "guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\n"
        "import:\n"
        "  textureImporter: {version: 1, compression: astc}   # S14\n"
        "  maxSize: 2048\n"
        "labels: [hero, ui]\n"
        "futureKey:\n"
        "  - {a: 1}\n"
        "  - b\n";
    AssetMeta meta;
    ASSERT_TRUE(AssetMeta::parse(text, meta));
    // unchanged: identical bytes
    EXPECT_EQ(meta.serialize(), text);
    // a former GUID added: one line in its place, the rest as it was
    meta.addFormerGuid(Guid::parse("9c1d0f3e5b7a42c8a1e06d2f4b8c7a19"));
    const std::string edited = meta.serialize();
    EXPECT_EQ(edited,
        "metaVersion: 1\n"
        "guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\n"
        "formerGuids: [9c1d0f3e5b7a42c8a1e06d2f4b8c7a19]\n"
        "import:\n"
        "  textureImporter: {version: 1, compression: astc}   # S14\n"
        "  maxSize: 2048\n"
        "labels: [hero, ui]\n"
        "futureKey:\n"
        "  - {a: 1}\n"
        "  - b\n");
    EXPECT_EQ(meta.getSection("labels"), "[hero, ui]\n");
    // and removed again: the original
    ASSERT_TRUE(AssetMeta::parse(edited, meta));
    meta.removeFormerGuid(Guid::parse("9c1d0f3e5b7a42c8a1e06d2f4b8c7a19"));
    EXPECT_EQ(meta.serialize(), text);
}

TEST(AssetMeta, ReadsCrlfAndRejectsMetasWithoutGuid) {
    AssetMeta meta;
    ASSERT_TRUE(AssetMeta::parse("metaVersion: 1\r\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\r\n", meta));
    EXPECT_EQ(meta.guid.str(), "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    EXPECT_EQ(meta.serialize().find('\r'), std::string::npos);
    EXPECT_FALSE(AssetMeta::parse("metaVersion: 1\nguid: nope\n", meta));
    EXPECT_FALSE(AssetMeta::parse("- just a list\n", meta));
}

// ==================================================================== content hash, models, text

TEST(ContentHash, MatchesXxh64Vectors) {
    EXPECT_EQ(ContentHash::of(""), 0xEF46DB3751D8E999ULL);
    EXPECT_EQ(ContentHash::of("a"), 0xD24EC4F1A98C6E5BULL);
    EXPECT_EQ(ContentHash::of("abc"), 0x44BC2CF5AD770999ULL);
    EXPECT_EQ(ContentHash::of("Nobody inspects the spammish repetition"), 0xFBCEA83C8A378BF1ULL);
    // streamed in pieces: the same digest
    const std::string text = "Nobody inspects the spammish repetition";
    ContentHash hash;
    for (char c : text) hash.update(&c, 1);
    EXPECT_EQ(hash.digest(), 0xFBCEA83C8A378BF1ULL);
}

TEST(ModelSubAssets, GltfGlbAndObj) {
    ModelContents contents;
    ASSERT_TRUE(scanModelBytes(".gltf", "{\"meshes\": [{\"name\": \"Hull\"}, {}, {\"name\": \"Hull\"}], \"animations\": [{\"name\": \"Idle\"}],"
                                        " \"images\": [{\"uri\": \"albedo.png\"}, {\"uri\": \"data:image/png;base64,AAAA\"}],"
                                        " \"buffers\": [{\"uri\": \"ship.bin\"}]}", contents));
    std::vector<std::string> keys;
    for (const ModelSubAsset& sub : contents.subAssets) keys.push_back(sub.key);
    EXPECT_EQ(keys, (std::vector<std::string>{"mesh#0", "mesh#1", "mesh#2", "animation:Idle", "image#0", "image#1"}));
    EXPECT_EQ(contents.uris, (std::vector<std::string>{"ship.bin", "albedo.png"}));

    const std::string json = "{\"meshes\": [{\"name\": \"Body\"}]}";
    std::string glb = "glTF";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) glb.push_back(static_cast<char>((v >> (i * 8)) & 0xFF)); };
    u32(2);
    u32(static_cast<uint32_t>(20 + json.size()));
    u32(static_cast<uint32_t>(json.size()));
    glb += "JSON" + json;
    ASSERT_TRUE(scanModelBytes(".GLB", glb, contents));
    ASSERT_EQ(contents.subAssets.size(), 1u);
    EXPECT_EQ(contents.subAssets[0].key, "mesh:Body");

    ASSERT_TRUE(scanModelBytes(".obj", "mtllib ship.mtl\no Hull\nusemtl Paint\nf 1 2 3\no Mast\nusemtl Paint\nusemtl Wood\nf 1 2 3\n", contents));
    keys.clear();
    for (const ModelSubAsset& sub : contents.subAssets) keys.push_back(sub.key);
    EXPECT_EQ(keys, (std::vector<std::string>{"mesh:Hull", "mesh:Mast", "material:Paint", "material:Wood"}));
    EXPECT_EQ(contents.uris, (std::vector<std::string>{"ship.mtl"}));
}

TEST(TextReferences, HtmlCssLuaShader) {
    const std::vector<TextReference> html = TextReferences::scanHtml(
        "<html>\n<link rel=\"stylesheet\" href=\"hud.css\">\n<!-- <img src=\"no.png\"> -->\n<img src='a.png?v=2'>\n"
        "<a href=\"https://example.com\">x</a>\n<style>\n.x { background: url(b.png) }\n</style>\n</html>\n");
    ASSERT_EQ(html.size(), 3u);
    EXPECT_EQ(html[0].target, "hud.css");
    EXPECT_EQ(html[0].kind, DepEdgeKind::UIStylesheet);
    EXPECT_EQ(html[0].line, 2u);
    EXPECT_EQ(html[1].target, "a.png");
    EXPECT_EQ(html[1].line, 4u);
    EXPECT_EQ(html[2].target, "b.png");
    EXPECT_EQ(html[2].line, 7u);

    // a video's source and its poster image (S14: the export ships what the HTML UI reads itself)
    const std::vector<TextReference> video = TextReferences::scanHtml(
        "<body>\n<video id=\"v\" src=\"video/clip.webm\" poster=\"video/poster.png\" autoplay></video>\n</body>\n");
    ASSERT_EQ(video.size(), 2u);
    EXPECT_EQ(video[0].target, "video/clip.webm");
    EXPECT_EQ(video[1].target, "video/poster.png");
    EXPECT_EQ(video[1].kind, DepEdgeKind::UIImage);
    EXPECT_EQ(video[1].line, 2u);

    const std::vector<TextReference> css = TextReferences::scanCss("@import url(\"base.css\");\n/* url(x.png) */\nbody{background:url( 'img/bg.png' )}\n");
    ASSERT_EQ(css.size(), 2u);
    EXPECT_EQ(css[0].kind, DepEdgeKind::UIStylesheet);
    EXPECT_EQ(css[1].target, "img/bg.png");
    EXPECT_EQ(css[1].line, 3u);

    const std::vector<TextReference> lua = TextReferences::scanLua("-- require(\"commented\")\nlocal a = require(\"util.helpers\")\nlocal b = require 'game'\nlocal s = \"require('no')\"\n");
    ASSERT_EQ(lua.size(), 2u);
    EXPECT_EQ(lua[0].target, "util.helpers");
    EXPECT_EQ(lua[0].line, 2u);
    EXPECT_EQ(TextReferences::luaModulePath(lua[0].target), "util/helpers.lua");
    EXPECT_EQ(lua[1].target, "game");

    const std::vector<TextReference> shader = TextReferences::scanShader("#version 450\n  #  include \"common.glsl\"\n#include <lib/noise.glsl>\n");
    ASSERT_EQ(shader.size(), 2u);
    EXPECT_EQ(shader[0].target, "common.glsl");
    EXPECT_EQ(shader[1].line, 3u);
}

TEST(AssetTypes, TableDecidesTracking) {
    EXPECT_EQ(AssetTypes::find("assets/a.PNG", "assets")->type, AssetType::Texture);
    EXPECT_EQ(AssetTypes::find("Main.scene", "assets")->identity, AssetIdentity::InFile);
    EXPECT_EQ(AssetTypes::find("shaders/x.vert", "assets")->base, AssetPathBase::Project);
    EXPECT_EQ(AssetTypes::find("assets/levels/a.json", "assets")->type, AssetType::Data);
    EXPECT_EQ(AssetTypes::find("tools/a.json", "assets"), nullptr);      // data: in the assets dir only
    EXPECT_EQ(AssetTypes::find("CMakeLists.txt", ""), nullptr);
    EXPECT_EQ(AssetTypes::find("project.yaml", ""), nullptr);
    EXPECT_EQ(AssetTypes::find("assets/a.png.meta", "assets"), nullptr);
    EXPECT_EQ(AssetTypes::find("assets/.a.png.123-0.je-tmp", "assets"), nullptr);
    EXPECT_EQ(AssetTypes::metaPathFor("a/hero.png"), fs::path("a/hero.png.meta"));
    EXPECT_EQ(AssetTypes::assetPathForMeta("a/hero.png.meta"), fs::path("a/hero.png"));
}

// BF23 F24: every file the texture cook takes has an identity
TEST(AssetTypes, ExrIsATexture) {
    const AssetTypeInfo* exr = AssetTypes::find("assets/env.exr", "assets");
    ASSERT_NE(exr, nullptr);
    EXPECT_EQ(exr->type, AssetType::Texture);
    EXPECT_EQ(exr->identity, AssetIdentity::Sidecar);
    EXPECT_NE(AssetTypes::find("assets/env.EXR", "assets"), nullptr);
}

// ==================================================================== AssetDatabase

// BF23 F24: an .exr of an existing project, one its database did not track, gains its sidecar on
// the next scan and answers to the path old references hold
TEST(AssetDatabase, ExistingExrGainsASidecar) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "png content");
    {
        AssetDatabase database;
        database.open(dir.path, "assets");
        database.waitForBackgroundWork();
    }
    write(dir.path / "assets/env.exr", "exr content");
    AssetDatabase database;
    database.open(dir.path, "assets");
    database.waitForBackgroundWork();
    const Guid guid = database.getAssetGuid("assets/env.exr");
    EXPECT_FALSE(guid.isNull());
    EXPECT_TRUE(fs::exists(dir.path / "assets/env.exr.meta"));
    const AssetDatabase::Resolution reference = database.lookupPath("assets/env.exr");
    EXPECT_TRUE(reference.found()) << "a reference by path resolves";
    EXPECT_EQ(reference.guid, guid);
}

// AC1: two machines upgrading the same commit write the same identities
TEST(AssetIdentity, UpgradeIsDeterministic) {
    AssetTestDir first, second;
    const fs::path a = copyFixture("legacy_paths_project", first.path / "project");
    const fs::path b = copyFixture("legacy_paths_project", second.path / "project");
    AssetDatabase dbA, dbB;
    dbA.open(a, "assets");
    dbB.open(b, "assets");

    size_t tracked = 0;
    std::set<Guid> guids;
    for (const auto& entry : fs::recursive_directory_iterator(repoFixture("legacy_paths_project"))) {
        if (!entry.is_regular_file()) continue;
        const std::string relative = entry.path().lexically_relative(repoFixture("legacy_paths_project")).generic_string();
        const AssetTypeInfo* info = AssetTypes::find(relative, "assets");
        if (!info) continue;
        tracked++;
        const Guid guid = dbA.getAssetGuid(relative);
        ASSERT_FALSE(guid.isNull()) << relative;
        EXPECT_TRUE(guids.insert(guid).second) << relative << " shares its GUID";
        EXPECT_EQ(guid.str(), AssetDatabase::legacyGuid(relative)) << relative;
        if (info->identity == AssetIdentity::Sidecar) {
            const std::string metaA = read(a / AssetTypes::metaPathFor(relative));
            EXPECT_FALSE(metaA.empty()) << relative;
            EXPECT_EQ(metaA, read(b / AssetTypes::metaPathFor(relative))) << relative;
            EXPECT_EQ(metaA.find('\r'), std::string::npos);
        } else {
            // the file is the original with its guid line inserted, nothing else
            std::string upgraded = read(a / relative);
            const std::string line = "guid: " + guid.str() + "\n";
            const size_t at = upgraded.find(line);
            ASSERT_NE(at, std::string::npos) << relative;
            upgraded.erase(at, line.size());
            EXPECT_EQ(upgraded, read(repoFixture("legacy_paths_project") / relative)) << relative;
        }
    }
    EXPECT_EQ(dbA.assetCount(), tracked);
    // the model's sub-assets: derived from its GUID and their keys
    const std::map<std::string, Guid> subs = subAssetGuids(dbA, "assets/models/ship.gltf");
    ASSERT_EQ(subs.count("mesh:Hull"), 1u);
    EXPECT_EQ(subs.at("mesh:Hull"), Guid::derive(dbA.getAssetGuid("assets/models/ship.gltf"), "mesh:Hull"));
    EXPECT_EQ(subs.count("mesh#2"), 1u);
}

TEST(AssetDatabase, OpeningAgainChangesNothing) {
    AssetTestDir dir;
    const fs::path project = copyFixture("legacy_paths_project", dir.path / "project");
    {
        AssetDatabase database;
        database.open(project, "assets");
    }
    const fs::file_time_type metaTime = fs::last_write_time(project / "assets/textures/brick.png.meta");
    const fs::file_time_type sceneTime = fs::last_write_time(project / "Main.scene");
    AssetDatabase database;
    database.open(project, "assets");
    EXPECT_TRUE(database.refresh().empty());
    // compared as clock ticks: gtest prints a time point with C++20 chrono formatting, which
    // libc++ makes unavailable below macOS 13.3, the editor's deployment target being 12.0
    EXPECT_EQ(fs::last_write_time(project / "assets/textures/brick.png.meta").time_since_epoch().count(), metaTime.time_since_epoch().count());
    EXPECT_EQ(fs::last_write_time(project / "Main.scene").time_since_epoch().count(), sceneTime.time_since_epoch().count());
}

// AC3: a file moved without its meta adopts it
TEST(AssetDatabase, ExternalMoveWithoutMeta) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "png a content");
    write(dir.path / "assets/other.png", "other");
    AssetDatabase database;
    database.open(dir.path, "assets");
    database.waitForBackgroundWork();
    const Guid guid = database.getAssetGuid("assets/a.png");
    ASSERT_FALSE(guid.isNull());

    fs::create_directories(dir.path / "assets/moved");
    fs::rename(dir.path / "assets/a.png", dir.path / "assets/moved/renamed.png");   // the meta stays behind
    const std::vector<AssetDatabase::Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].type, AssetDatabase::Change::Type::Moved);
    EXPECT_EQ(changes[0].path.generic_string(), "assets/moved/renamed.png");
    EXPECT_EQ(database.getAssetGuid("assets/moved/renamed.png"), guid);
    EXPECT_TRUE(fs::exists(dir.path / "assets/moved/renamed.png.meta"));
    EXPECT_FALSE(fs::exists(dir.path / "assets/a.png.meta"));
    EXPECT_TRUE(database.getOrphanMetas().empty());
}

// AC3 variant: no hash, two candidates with the orphan's name and size: the orphan stays
TEST(AssetDatabase, ExternalMoveWithoutMetaAmbiguousKeepsOrphan) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "same size");
    AssetDatabase database;
    database.setBackgroundHashing(false);
    database.open(dir.path, "assets");
    const Guid guid = database.getAssetGuid("assets/a.png");

    fs::create_directories(dir.path / "assets/x");
    fs::create_directories(dir.path / "assets/y");
    fs::rename(dir.path / "assets/a.png", dir.path / "assets/x/a.png");
    write(dir.path / "assets/y/a.png", "same size");
    database.refresh();
    const std::vector<AssetDatabase::OrphanMeta> orphans = database.getOrphanMetas();
    ASSERT_EQ(orphans.size(), 1u);
    EXPECT_EQ(orphans[0].metaPath, "assets/a.png.meta");
    EXPECT_EQ(orphans[0].guid, guid);
    EXPECT_NE(database.getAssetGuid("assets/x/a.png"), guid);
    EXPECT_NE(database.getAssetGuid("assets/y/a.png"), guid);

    // one candidate by name and size: adopted without a hash
    AssetTestDir single;
    write(single.path / "assets/b.png", "bbb");
    AssetDatabase other;
    other.setBackgroundHashing(false);
    other.open(single.path, "assets");
    const Guid b = other.getAssetGuid("assets/b.png");
    fs::create_directories(single.path / "assets/z");
    fs::rename(single.path / "assets/b.png", single.path / "assets/z/b.png");
    other.refresh();
    EXPECT_EQ(other.getAssetGuid("assets/z/b.png"), b);
}

// AC4: a directory moved outside the editor with its metas
TEST(AssetDatabase, ExternalDirectoryMove) {
    AssetTestDir dir;
    for (int i = 0; i < 50; i++) {
        write(dir.path / "assets/pack" / ("tex" + std::to_string(i) + ".png"), "texture " + std::to_string(i));
    }
    AssetDatabase database;
    database.open(dir.path, "assets");
    std::map<std::string, Guid> before;
    for (int i = 0; i < 50; i++) {
        before["tex" + std::to_string(i) + ".png"] = database.getAssetGuid("assets/pack/tex" + std::to_string(i) + ".png");
    }
    fs::create_directories(dir.path / "assets/moved");
    fs::rename(dir.path / "assets/pack", dir.path / "assets/moved/pack");
    const std::vector<AssetDatabase::Change> changes = database.refresh();
    ASSERT_EQ(changes.size(), 50u);
    for (const AssetDatabase::Change& change : changes) {
        EXPECT_EQ(change.type, AssetDatabase::Change::Type::Moved);
        EXPECT_EQ(Guid::parse(change.guid), before.at(change.path.filename().string()));
    }
}

// AC6: a copy made outside the editor gets a new GUID, its sub-assets derived from it
TEST(AssetDatabase, DuplicateMetaResolution) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "aaa");
    write(dir.path / "assets/ship.gltf", gltf({"Hull", "", "Mast"}));
    AssetDatabase database;
    database.open(dir.path, "assets");
    const Guid a = database.getAssetGuid("assets/a.png");
    const Guid ship = database.getAssetGuid("assets/ship.gltf");

    fs::copy_file(dir.path / "assets/a.png", dir.path / "assets/b.png");
    fs::copy_file(dir.path / "assets/a.png.meta", dir.path / "assets/b.png.meta");
    fs::copy_file(dir.path / "assets/ship.gltf", dir.path / "assets/copy.gltf");
    fs::copy_file(dir.path / "assets/ship.gltf.meta", dir.path / "assets/copy.gltf.meta");
    database.refresh();
    EXPECT_EQ(database.getAssetGuid("assets/a.png"), a);
    const Guid b = database.getAssetGuid("assets/b.png");
    EXPECT_FALSE(b.isNull());
    EXPECT_NE(b, a);
    EXPECT_EQ(database.getAssetGuid("assets/ship.gltf"), ship);
    const Guid copy = database.getAssetGuid("assets/copy.gltf");
    ASSERT_NE(copy, ship);
    for (const auto& [key, guid] : subAssetGuids(database, "assets/copy.gltf")) {
        EXPECT_EQ(guid, Guid::derive(copy, key)) << key;
    }
    // the copy's meta holds its new identity
    AssetMeta meta;
    ASSERT_TRUE(AssetMeta::parse(read(dir.path / "assets/b.png.meta"), meta));
    EXPECT_EQ(meta.guid, b);
}

// AC7: sub-asset GUIDs survive a reorder (by key) and a rename of an unnamed mesh (by index)
TEST(SubAssets, StableKeys) {
    AssetTestDir dir;
    write(dir.path / "assets/ship.gltf", gltf({"Hull", "Mast", ""}));
    AssetDatabase database;
    database.open(dir.path, "assets");
    const std::map<std::string, Guid> before = subAssetGuids(database, "assets/ship.gltf");
    ASSERT_EQ(before.size(), 4u); // three meshes, one material

    write(dir.path / "assets/ship.gltf", gltf({"Mast", "Hull", ""}, " "));
    touchLater(dir.path / "assets/ship.gltf");
    database.refresh();
    std::map<std::string, Guid> after = subAssetGuids(database, "assets/ship.gltf");
    EXPECT_EQ(after.at("mesh:Hull"), before.at("mesh:Hull"));
    EXPECT_EQ(after.at("mesh:Mast"), before.at("mesh:Mast"));
    EXPECT_EQ(after.at("mesh#2"), before.at("mesh#2"));

    write(dir.path / "assets/ship.gltf", gltf({"Mast", "Hull", "Keel"}, "  "));
    touchLater(dir.path / "assets/ship.gltf");
    database.refresh();
    after = subAssetGuids(database, "assets/ship.gltf");
    EXPECT_EQ(after.count("mesh#2"), 0u);
    EXPECT_EQ(after.at("mesh:Keel"), before.at("mesh#2"));
}

// AC9: path fallback for non-prefab types only (PREFABS.md §3)
TEST(AssetResolve, PathFallbackNonPrefab) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "aaa");
    write(dir.path / "enemy.prefab", "type: Prefab\nversion: 3\nnextEntity: 1\nroot: 0\nmembers: []\n");
    AssetDatabase database;
    database.open(dir.path, "assets");
    const Guid unknown = Guid::parse("0123456789abcdef0123456789abcdef");
    const AssetDatabase::Resolution texture = database.resolve(unknown, "assets/a.png", AssetType::Texture, true);
    EXPECT_EQ(texture.by, AssetDatabase::ResolvedBy::PathFallback);
    EXPECT_EQ(texture.guid, database.getAssetGuid("assets/a.png"));
    // the wrong type at the hint does not bind
    EXPECT_FALSE(database.resolve(unknown, "assets/a.png", AssetType::Model, true).found());
    EXPECT_FALSE(database.resolve(unknown, "enemy.prefab", AssetType::Prefab, false).found());

    // a former GUID resolves to its asset, one hop
    ASSERT_TRUE(database.addFormerGuid("assets/a.png", unknown));
    const AssetDatabase::Resolution former = database.resolve(unknown, std::string(), AssetType::Texture, false);
    EXPECT_EQ(former.by, AssetDatabase::ResolvedBy::Former);
    EXPECT_EQ(former.path, "assets/a.png");
    AssetMeta meta;
    ASSERT_TRUE(AssetMeta::parse(read(dir.path / "assets/a.png.meta"), meta));
    EXPECT_EQ(meta.formerGuids, std::vector<Guid>{unknown});

    // without a GUID: the legacy GUID of the hint, else the file at the hint
    const AssetDatabase::Resolution legacy = database.resolve(Guid(), "assets/a.png", AssetType::Texture, false);
    EXPECT_EQ(legacy.by, AssetDatabase::ResolvedBy::LegacyPath);
}

TEST(AssetDatabase, InFileFormerGuids) {
    AssetTestDir dir;
    write(dir.path / "level.scene", "id: 1\nname: Level\nentities: []\n");
    AssetDatabase database;
    database.open(dir.path, "assets");
    const Guid former = Guid::parse("0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(database.addFormerGuid("level.scene", former));
    Guid guid;
    std::vector<Guid> formerGuids;
    ASSERT_TRUE(AssetDatabase::readInFileIdentity(dir.path / "level.scene", guid, formerGuids));
    EXPECT_EQ(guid, database.getAssetGuid("level.scene"));
    EXPECT_EQ(formerGuids, std::vector<Guid>{former});
    EXPECT_EQ(read(dir.path / "level.scene"),
              "guid: " + guid.str() + "\nformerGuids: [" + former.str() + "]\nid: 1\nname: Level\nentities: []\n");
}

// ==================================================================== AssetIndex

// AC5: deleting the cache changes no answer
TEST(AssetIndex, CacheIsDisposable) {
    AssetTestDir dir;
    const fs::path project = copyFixture("legacy_paths_project", dir.path / "project");
    const AssetIndex::Roots roots{"assets", "scripts"};

    auto answers = [&]() {
        AssetDatabase database;
        database.open(project, "assets");
        AssetIndex index;
        index.open(&database, project, roots);
        index.waitUntilReady();
        std::vector<std::string> lines;
        database.forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            const AssetDatabase::Resolution found = database.resolve(info.guid, info.path, info.type, false);
            lines.push_back(info.path + " " + info.guid.str() + " " + found.path);
            std::vector<std::string> references;
            for (const DepEdge& edge : index.referencesTo(info.guid, info.path, false, 1000)) {
                references.push_back(index.referrerPath(edge.referrer) + " " + edge.field + " " + std::to_string(edge.ordinal));
            }
            std::sort(references.begin(), references.end());
            for (const std::string& reference : references) lines.push_back("  " + reference);
        });
        index.close();
        return lines;
    };

    const std::vector<std::string> first = answers();
    ASSERT_TRUE(fs::exists(project / ".jasperengine/assetdb/index.json"));
    fs::remove_all(project / ".jasperengine/assetdb");
    EXPECT_EQ(answers(), first);
    // and a warm start from the cache answers the same
    EXPECT_EQ(answers(), first);
}

TEST(AssetIndex, EdgesOfClosedFiles) {
    AssetTestDir dir;
    const fs::path project = copyFixture("legacy_paths_project", dir.path / "project");
    AssetDatabase database;
    database.open(project, "assets");
    AssetIndex index;
    index.open(&database, project, {"assets", "scripts"});
    index.waitUntilReady();

    auto referrers = [&](const std::string& path) {
        std::set<std::string> owners;
        for (const DepEdge& edge : index.referencesTo(database.getAssetGuid(path), path, false, 1000)) {
            owners.insert(index.referrerPath(edge.referrer) + "|" + edge.field);
        }
        return owners;
    };
    EXPECT_EQ(referrers("assets/textures/mask.png"),
              (std::set<std::string>{"Closed.scene|entity:1001/LightComponent.spotMask", "prefabs/Lamp.prefab|entity:1001/LightComponent.spotMask",
                                     "materials/Brick.material|material/baseColorTexture"}));
    EXPECT_EQ(referrers("assets/models/ship.gltf"),
              (std::set<std::string>{"Main.scene|entity:1001/ModelComponent.filename", "Closed.scene|entity:1004/ModelComponent.filename"}));
    EXPECT_EQ(referrers("assets/textures/brick.png"),
              (std::set<std::string>{"Closed.scene|entity:1003/LightComponent.spotMask", "assets/ui/hud.html|text:7"}));
    EXPECT_EQ(referrers("assets/ui/hud.css"), (std::set<std::string>{"assets/ui/hud.html|text:4"}));
    EXPECT_EQ(referrers("scripts/util/helpers.lua"), (std::set<std::string>{"scripts/Spinner.lua|text:2"}));
    EXPECT_EQ(referrers("shaders/common.glsl"), (std::set<std::string>{"shaders/tint.frag|text:2"}));
    EXPECT_EQ(referrers("Main.scene"), (std::set<std::string>{"project.yaml|project/scenes[0].filepath"}));

    // transitive: the scene that uses the prefab that uses the texture
    std::set<std::string> owners;
    for (const DepEdge& edge : index.referencesTo(database.getAssetGuid("assets/textures/mask.png"), "assets/textures/mask.png", true, 1000)) {
        owners.insert(index.referrerPath(edge.referrer));
    }
    EXPECT_TRUE(owners.count("Main.scene"));
    index.close();
}

// ==================================================================== AssetRefSchema

// AC12: every Catalog property that holds a reference, of every component, is a row. (The rows come
// from the reflection tables; AssetRefSchemaFrozen holds what they must keep.)
TEST(AssetRefSchema, CoversAllFields) {
    EntityRegistry registry;
    for (const reflect::ComponentDesc* desc : reflect::all()) {
        const Entity entity = registry.createEntity();
        if (!desc->find(&registry, entity)) {
            reflect::Value value(desc->body.ops);
            desc->add(&registry, entity, value.get());
        }
        if (MeshComponent* mesh = registry.findComponent<MeshComponent>(entity)) mesh->numSubmeshes = 1;
        if (TerrainComponent* terrain = registry.findComponent<TerrainComponent>(entity)) {
            terrain->blendMaps.resize(1);
            terrain->surfaceLayers.resize(1);
            terrain->foliageLayers.resize(1);
        }
        for (const auto& [name, property] : Catalog::findEntityProperties(&registry, entity, desc->type)) {
            const bool assetTyped = property.type == PropertyType::Texture || property.type == PropertyType::Asset ||
                                    property.type == PropertyType::Shader || property.type == PropertyType::Font;
            if (!assetTyped) continue;
            EXPECT_NE(AssetRefSchema::findRow(AssetRefScope::Component, desc->type, name == "font" ? "font[0]" : name), nullptr)
                << Catalog::getComponentName(desc->type) << " property " << name << " holds an asset but has no AssetRefSchema row";
        }
    }
}

TEST(AssetRefSchema, VisitorRewritesAndFlags) {
    EntityRegistry registry;
    const Entity entity = registry.createEntity();
    ModelComponent model;
    model.filename = "models/a.glb";
    model.needUpdateModel = false;
    registry.addComponent<ModelComponent>(entity, model);
    AudioSourceComponent audio;
    audio.filename = AssetRef("sounds/a.wav", Guid::parse("0123456789abcdef0123456789abcdef"));
    registry.addComponent<AudioSourceComponent>(entity, audio);

    std::vector<std::string> seen;
    const bool changed = AssetRefSchema::forEachAssetRef(registry, [&](AssetRef& ref, const AssetRefSite& site) {
        seen.push_back(site.locator() + " " + ref.path);
        if (site.component == "ModelComponent") ref.path = "models/b.glb";
    });
    EXPECT_TRUE(changed);
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, (std::vector<std::string>{"entity:" + std::to_string(entity) + "/AudioSourceComponent.filename sounds/a.wav",
                                              "entity:" + std::to_string(entity) + "/ModelComponent.filename models/a.glb"}));
    EXPECT_EQ(registry.findComponent<ModelComponent>(entity)->filename.path, "models/b.glb");
    EXPECT_TRUE(registry.findComponent<ModelComponent>(entity)->needUpdateModel);
    // a GUID filled in is no path change: nothing reloads
    registry.findComponent<ModelComponent>(entity)->needUpdateModel = false;
    AssetRefSchema::forEachAssetRef(registry, [&](AssetRef& ref, const AssetRefSite&) {
        if (ref.guid.isNull()) ref.guid = Guid::parse("00000000000000000000000000000007");
    });
    EXPECT_FALSE(registry.findComponent<ModelComponent>(entity)->needUpdateModel);
    EXPECT_EQ(registry.findComponent<ModelComponent>(entity)->filename.guid.str(), "00000000000000000000000000000007");
}

// An Animation Player's clip entries are references (Find References, Missing Assets, renames):
// an .anim by GUID and path, a sub-clip by its model's path, the clip part kept
TEST(AssetRefSchema, AnimationPlayerClipEntries) {
    EntityRegistry registry;
    const Entity entity = registry.createEntity();
    AnimationPlayerComponent player;
    player.clips = {{"anim/walk.anim", "0123456789abcdef0123456789abcdef", "Walk", ClipWrapMode::Loop},
                    {"models/hero.gltf#Run", "", "", ClipWrapMode::Default},
                    {"", "", "", ClipWrapMode::Default}};
    player.needsLoad = false;
    registry.addComponent<AnimationPlayerComponent>(entity, player);

    std::vector<std::string> seen;
    AssetRefSchema::forEachAssetRef(registry, [&](AssetRef& ref, const AssetRefSite& site) {
        seen.push_back(site.locator() + " " + ref.path + " " + ref.guid.str() + " " + assetTypeName(site.expected));
    });
    const std::string prefix = "entity:" + std::to_string(entity) + "/AnimationPlayerComponent.";
    EXPECT_EQ(seen, (std::vector<std::string>{prefix + "clips[0] anim/walk.anim 0123456789abcdef0123456789abcdef animation_clip",
                                              prefix + "clips[1] models/hero.gltf  model"}));
    EXPECT_NE(AssetRefSchema::findRow(AssetRefScope::Component, ComponentType::AnimationPlayerComponent, "clips[1]"), nullptr);

    // a GUID filled in for the model is no change: a sub-clip entry keeps none
    EXPECT_FALSE(AssetRefSchema::forEachAssetRef(registry, [&](AssetRef& ref, const AssetRefSite& site) {
        if (site.field == "clips[1]") ref.guid = Guid::parse("00000000000000000000000000000009");
    }));
    EXPECT_FALSE(registry.findComponent<AnimationPlayerComponent>(entity)->needsLoad);

    // moved files: the .anim takes its new path and GUID, the sub-clip its new model; the player reloads
    EXPECT_TRUE(AssetRefSchema::forEachAssetRef(registry, [&](AssetRef& ref, const AssetRefSite& site) {
        if (site.field == "clips[0]") ref = AssetRef("anim/moved/walk.anim", Guid::parse("00000000000000000000000000000008"));
        if (site.field == "clips[1]") ref.path = "models/moved/hero.gltf";
    }));
    const AnimationPlayerComponent& after = *registry.findComponent<AnimationPlayerComponent>(entity);
    EXPECT_EQ(after.clips[0].clip, "anim/moved/walk.anim");
    EXPECT_EQ(after.clips[0].guid, "00000000000000000000000000000008");
    EXPECT_EQ(after.clips[0].name, "Walk");
    EXPECT_EQ(after.clips[0].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(after.clips[1].clip, "models/moved/hero.gltf#Run");
    EXPECT_EQ(after.clips[1].guid, "");
    EXPECT_TRUE(after.needsLoad);

    // the same entries in a closed scene file (a bare path entry too)
    const YAML::Node root = YAML::Load(R"(
entities:
  - type: Entity
    entity: 12
    name: Hero
    components:
      animationplayer:
        clips:
          - {clip: anim/walk.anim, guid: 0123456789abcdef0123456789abcdef, name: Walk}
          - {clip: "models/hero.gltf#Run"}
          - anim/idle.anim
)");
    std::vector<std::string> found;
    AssetRefSchema::extractDocumentRefs(root, false, [&](const AssetRefFound& ref) {
        found.push_back(ref.site.locator(ref.scope) + " " + ref.ref.path + " " + ref.ref.guid.str() + " " + assetTypeName(ref.site.expected));
    });
    EXPECT_EQ(found, (std::vector<std::string>{
        "entity:12/AnimationPlayerComponent.clips[0] anim/walk.anim 0123456789abcdef0123456789abcdef animation_clip",
        "entity:12/AnimationPlayerComponent.clips[1] models/hero.gltf  model",
        "entity:12/AnimationPlayerComponent.clips[2] anim/idle.anim  animation_clip",
    }));
}

TEST(AssetRefSchema, ExtractsDocumentShapes) {
    const YAML::Node root = YAML::Load(R"(
scene:
  defaultMeshShader: shaders/tint
  defaultMeshShaderGuid: 00000000000000000000000000000001|00000000000000000000000000000002
  postProcessing: {lutTexture: luts/warm.png}
prefabs:
  - {path: prefabs/Lamp.prefab, guid: 00000000000000000000000000000003}
entities:
  - type: Entity
    entity: 12
    name: Root
    components:
      text: {font: [fonts/a.ttf, fonts/b.ttf], fontGuid: [00000000000000000000000000000004, ""]}
    children:
      - type: Entity
        entity: 13
        name: Child
        components:
          prefab: {name: Lamp, path: prefabs/Lamp.prefab, guid: 00000000000000000000000000000003}
        prefabInstance:
          overrides:
            - registryEntity: 1001
              components:
                light:
                  spotMask: {textureType: 2D, source: path, path: textures/m.png, guid: 00000000000000000000000000000005}
)");
    std::vector<std::string> found;
    AssetRefSchema::extractDocumentRefs(root, false, [&](const AssetRefFound& ref) {
        found.push_back(ref.site.locator(ref.scope) + "#" + std::to_string(ref.site.ordinal) + " " + ref.ref.path + " " + ref.ref.guid.str() + " " + ref.entityPath);
    });
    std::sort(found.begin(), found.end());
    EXPECT_EQ(found, (std::vector<std::string>{
        "entity:12/TextComponent.font[0]#0 fonts/a.ttf 00000000000000000000000000000004 Root",
        "entity:12/TextComponent.font[1]#0 fonts/b.ttf  Root",
        "entity:13/PrefabComponent.path#0 prefabs/Lamp.prefab 00000000000000000000000000000003 Root/Child",
        "entity:13/prefabInstance[1001]/LightComponent.spotMask#0 textures/m.png 00000000000000000000000000000005 Root/Child",
        "scene/prefabs[0]#0 prefabs/Lamp.prefab 00000000000000000000000000000003 ",
        "settings/defaultMeshShader#0 shaders/tint.vert 00000000000000000000000000000001 ",
        "settings/defaultMeshShader#1 shaders/tint.frag 00000000000000000000000000000002 ",
        "settings/postProcessing.lutTexture#0 luts/warm.png  ",
    }));
}

// A save registers its file: the index cache is written by the background thread, not by the
// save, and close() writes what is still pending (R23; the worker once took a hash job from
// an empty queue when woken only for the save)
TEST(AssetDatabase, CacheSaveIsDeferredAndFlushedOnClose) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "a");
    write(dir.path / "scenes/Main.scene", "id: 1\nname: Main\nentities: []\n");
    AssetDatabase database;
    database.open(dir.path, "assets");
    database.waitForBackgroundWork();
    const Guid scene = database.getAssetGuid("scenes/Main.scene");
    ASSERT_FALSE(scene.isNull());
    for (int i = 0; i < 20; i++) {
        write(dir.path / "scenes/Main.scene", "id: 1\nname: Main\nentities: []\n# save " + std::to_string(i) + "\n");
        database.registerAsset(dir.path / "scenes/Main.scene", scene.str());
    }
    database.waitForBackgroundWork();
    database.close();
    const std::string cache = read(dir.path / ".jasperengine/assetdb/index.json");
    EXPECT_NE(cache.find(scene.str()), std::string::npos);

    // the cache written on close describes the file as it is: a warm open finds nothing changed
    AssetDatabase again;
    again.open(dir.path, "assets");
    EXPECT_EQ(again.getAssetGuid("scenes/Main.scene"), scene);
    EXPECT_TRUE(again.refresh().empty());
}

// The poll's walk runs on another thread: a scan taken before the database changed is dropped
TEST(AssetDatabase, StaleScanIsDropped) {
    AssetTestDir dir;
    write(dir.path / "assets/a.png", "a");
    AssetDatabase database;
    database.setBackgroundHashing(false);
    database.open(dir.path, "assets");
    const AssetDatabase::DiskScan before = database.scanDisk();
    ASSERT_TRUE(before.valid);
    write(dir.path / "assets/b.png", "b");
    database.importAssets(dir.path / "assets/b.png");      // the editor imported it meanwhile
    EXPECT_TRUE(database.refresh(before).empty());          // b.png must not come back as removed
    EXPECT_FALSE(database.getAssetGuid("assets/b.png").isNull());
    const AssetDatabase::DiskScan now = database.scanDisk();
    EXPECT_TRUE(database.refresh(now).empty());
}

// An importer's settings written to a sidecar before the asset got its GUID (S45: a texture's Sprite
// Mode set while it was being imported) survive the GUID's write
TEST(AssetDatabase, SettingsWrittenBeforeTheGuidSurvive) {
    AssetTestDir dir;
    write(dir.path / "assets/walk.png", "w");
    write(dir.path / "assets/walk.png.meta", "metaVersion: 1\nimport:\n  spriteSheet:\n    mode: multiple\n");
    AssetDatabase database;
    database.setBackgroundHashing(false);
    database.open(dir.path, "assets");
    database.importAssets(dir.path / "assets/walk.png");
    EXPECT_FALSE(database.getAssetGuid("assets/walk.png").isNull());
    std::ifstream in(dir.path / "assets/walk.png.meta");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("guid:"), std::string::npos) << text;
    EXPECT_NE(text.find("spriteSheet"), std::string::npos) << "the settings are kept: " << text;
    EXPECT_NE(text.find("mode: multiple"), std::string::npos) << text;
}

// ==================================================================== links and hard links (S68)

namespace {

    const char* const LINK_GUID_A = "aaaa0000000011112222333344445555";
    const char* const LINK_GUID_B = "bbbb0000000011112222333344445555";

    std::string prefabText(const std::string& guid, const std::string& name = "Thing") {
        return "guid: " + guid + "\nname: " + name + "\n";
    }

    bool makeDirLink(const fs::path& target, const fs::path& link) {
        std::error_code ec;
        fs::create_directories(link.parent_path(), ec);
        fs::create_directory_symlink(target, link, ec);
        return !ec;
    }

    bool makeFileLink(const fs::path& target, const fs::path& link) {
        std::error_code ec;
        fs::create_symlink(target, link, ec);
        return !ec;
    }

    bool makeHardLink(const fs::path& target, const fs::path& link) {
        std::error_code ec;
        fs::create_hard_link(target, link, ec);
        return !ec;
    }

    // a project with its assets folder, and a place outside it
    struct LinkProject {
        AssetTestDir dir;
        fs::path project = dir.path / "project";
        fs::path outside = dir.path / "outside";
        AssetDatabase database;
        LinkProject() {
            fs::create_directories(project / "assets");
            fs::create_directories(outside);
        }
        void open() {
            database.setBackgroundHashing(false);
            database.open(project, "assets");
        }
    };

    bool tracked(const AssetDatabase& database, const std::string& path) {
        bool found = false;
        database.forEachAsset([&](const AssetDatabase::AssetInfo& info) { found = found || info.path == path; });
        return found;
    }

    bool runningAsRoot() {
#ifdef _WIN32
        return false;
#else
        return ::geteuid() == 0;
#endif
    }

}

// R4: the file reached by a file link or a hard link is that file: no new GUID, nothing written;
// a real copy still gets one
TEST(AssetIdentityLinks, OneFileIsNeverACopy) {
    LinkProject p;
    const fs::path file = p.project / "assets" / "a.prefab";
    write(file, prefabText(LINK_GUID_A));
    const std::string bytes = read(file);
    if (!makeFileLink(file, p.project / "assets" / "sym.prefab")) GTEST_SKIP() << "no symbolic links here";
    ASSERT_TRUE(makeHardLink(file, p.project / "assets" / "hard.prefab"));
    write(p.project / "assets" / "copy.prefab", bytes);

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) p.open();                // the scan on open
        else {
            p.database.close();
            p.open();                           // and from the cache
        }
        EXPECT_EQ(read(file), bytes) << "pass " << pass;
        const std::string copied = AssetDatabase::readGuid(p.project / "assets" / "copy.prefab");
        EXPECT_NE(copied, LINK_GUID_A) << "a real copy gets a GUID of its own";
        EXPECT_EQ(p.database.getGuid("assets/a.prefab"), LINK_GUID_A);
        EXPECT_EQ(p.database.getGuid("assets/sym.prefab"), LINK_GUID_A);
        EXPECT_EQ(p.database.getGuid("assets/hard.prefab"), LINK_GUID_A);
        EXPECT_EQ(p.database.getGuid(p.project / "assets" / "hard.prefab"), LINK_GUID_A);
        EXPECT_EQ(p.database.getGuid("assets/copy.prefab"), copied);
        EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_A)), fs::path("assets/a.prefab"));
        EXPECT_FALSE(tracked(p.database, "assets/sym.prefab"));
        EXPECT_FALSE(tracked(p.database, "assets/hard.prefab"));
    }

    // made while the project is open: the refresh and an import decide the same
    const fs::path other = p.project / "assets" / "b.prefab";
    write(other, prefabText(LINK_GUID_B));
    ASSERT_TRUE(makeHardLink(other, p.project / "assets" / "bhard.prefab"));
    p.database.refresh();
    EXPECT_EQ(p.database.getGuid("assets/bhard.prefab"), LINK_GUID_B);
    EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_B)), fs::path("assets/b.prefab")) << "the path tracked first stays the asset's";
    EXPECT_EQ(AssetDatabase::readGuid(other), LINK_GUID_B);
    EXPECT_TRUE(p.database.refresh().empty()) << "and the next refresh has nothing to say";

    ASSERT_TRUE(makeHardLink(other, p.project / "assets" / "bhard2.prefab"));
    p.database.importAssets("assets/bhard2.prefab");
    EXPECT_EQ(AssetDatabase::readGuid(other), LINK_GUID_B);
    EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_B)), fs::path("assets/b.prefab"));

    // a save by the hard link's path keeps the asset where it is
    p.database.registerAsset("assets/bhard.prefab", LINK_GUID_B);
    EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_B)), fs::path("assets/b.prefab"));
}

// R4: a sidecar asset's hard link is no asset of its own: no second meta, the same GUID. Written
// without a meta it never gets a new identity.
TEST(AssetIdentityLinks, HardLinkOfASidecarAssetGetsNoMeta) {
    LinkProject p;
    const fs::path file = p.project / "assets" / "tile.png";
    write(file, "not really a png");
    p.open();
    const std::string guid = p.database.getGuid("assets/tile.png");
    ASSERT_FALSE(guid.empty());
    if (!makeHardLink(file, p.project / "assets" / "tile2.png")) GTEST_SKIP() << "no hard links here";
    p.database.refresh();
    EXPECT_EQ(p.database.getGuid("assets/tile2.png"), guid);
    EXPECT_FALSE(fs::exists(p.project / "assets" / "tile2.png.meta"));
    p.database.close();
    p.open();
    EXPECT_EQ(p.database.getGuid("assets/tile2.png"), guid);
    EXPECT_EQ(p.database.getPath(guid), fs::path("assets/tile.png"));
    EXPECT_FALSE(fs::exists(p.project / "assets" / "tile2.png.meta"));
}

// R5: one asset, one path. A link to a project folder: lookups through it answer with the real path
// (no entries, no metas under it, no file system call needed), and a real copy made through it is a copy.
TEST(AssetIdentityLinks, LinkToAProjectFolderIsOneAssetUnderTwoNames) {
    LinkProject p;
    write(p.project / "assets" / "real" / "a.prefab", prefabText(LINK_GUID_A));
    write(p.project / "assets" / "real" / "tile.png", "png");
    if (!makeDirLink(p.project / "assets" / "real", p.project / "assets" / "alias")) GTEST_SKIP() << "no symbolic links here";
    p.open();
    const std::string tile = p.database.getGuid("assets/real/tile.png");
    ASSERT_FALSE(tile.empty());

    EXPECT_EQ(p.database.getGuid("assets/alias/a.prefab"), LINK_GUID_A);
    EXPECT_EQ(p.database.getGuid(p.project / "assets" / "alias" / "a.prefab"), LINK_GUID_A);
    EXPECT_EQ(p.database.getGuid("assets/alias/tile.png"), tile);
    EXPECT_EQ(p.database.toRelative("assets/alias/tile.png"), fs::path("assets/real/tile.png"));
    EXPECT_EQ(p.database.lookupPath("assets/alias/tile.png").path, "assets/real/tile.png");
    const AssetDatabase::Resolution byHint = p.database.resolve(Guid(), "assets/alias/tile.png", AssetType::Texture, true, true);
    EXPECT_EQ(byHint.path, "assets/real/tile.png") << "a reference made by the alias stores the real path";
    size_t count = 0;
    p.database.forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        EXPECT_EQ(info.path.find("alias"), std::string::npos) << info.path;
        count++;
    });
    EXPECT_EQ(count, 2u);

    // a real copy made through the link is a new asset, at its real path
    fs::copy_file(p.project / "assets" / "real" / "a.prefab", p.project / "assets" / "alias" / "c.prefab");
    p.database.refresh();
    const std::string copied = AssetDatabase::readGuid(p.project / "assets" / "real" / "c.prefab");
    EXPECT_NE(copied, LINK_GUID_A);
    EXPECT_EQ(p.database.getPath(copied), fs::path("assets/real/c.prefab"));
    EXPECT_EQ(p.database.getGuid("assets/alias/c.prefab"), copied);
    EXPECT_EQ(AssetDatabase::readGuid(p.project / "assets" / "real" / "a.prefab"), LINK_GUID_A);

    // the link itself deleted or moved: what it leads to stays
    fs::rename(p.project / "assets" / "alias", p.project / "assets" / "alias2");
    EXPECT_TRUE(p.database.moveAssets("assets/alias", "assets/alias2").empty());
    EXPECT_EQ(p.database.getGuid("assets/alias2/a.prefab"), LINK_GUID_A);
    EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_A)), fs::path("assets/real/a.prefab"));
    fs::remove(p.project / "assets" / "alias2");
    EXPECT_TRUE(p.database.removeAssets("assets/alias2").empty());
    EXPECT_EQ(p.database.getPath(std::string(LINK_GUID_A)), fs::path("assets/real/a.prefab"));
    EXPECT_TRUE(p.database.getGuid("assets/alias2/a.prefab").empty());
    EXPECT_TRUE(p.database.refresh().empty());
}

// R6: a folder moved and linked at its old name is a move: the files keep their bytes and GUIDs, the
// change says Moved, so references keep finding them; moved back, the same
TEST(AssetIdentityLinks, FolderMovedBehindALinkIsAMove) {
    LinkProject p;
    const fs::path old = p.project / "assets" / "kit";
    const fs::path moved = p.project / "assets" / "kit_real";
    write(old / "a.prefab", prefabText(LINK_GUID_A));
    write(old / "tile.png", "png");
    p.open();
    const std::string tile = p.database.getGuid("assets/kit/tile.png");
    const std::string prefabBytes = read(old / "a.prefab"), metaBytes = read(old / "tile.png.meta");

    fs::rename(old, moved);
    if (!makeDirLink(moved, old)) GTEST_SKIP() << "no symbolic links here";
    std::map<std::string, std::string> movedFrom;
    for (const AssetDatabase::Change& change : p.database.refresh()) {
        EXPECT_EQ(change.type, AssetDatabase::Change::Type::Moved) << change.path;
        movedFrom[change.path.generic_string()] = change.oldPath.generic_string();
    }
    EXPECT_EQ(movedFrom["assets/kit_real/a.prefab"], "assets/kit/a.prefab");
    EXPECT_EQ(movedFrom["assets/kit_real/tile.png"], "assets/kit/tile.png");
    EXPECT_EQ(read(moved / "a.prefab"), prefabBytes);
    EXPECT_EQ(read(moved / "tile.png.meta"), metaBytes);
    EXPECT_EQ(p.database.getGuid("assets/kit_real/a.prefab"), LINK_GUID_A);
    EXPECT_EQ(p.database.getGuid("assets/kit/a.prefab"), LINK_GUID_A);
    EXPECT_EQ(p.database.getPath(tile), fs::path("assets/kit_real/tile.png"));
    EXPECT_EQ(p.database.resolve(Guid::parse(tile), "assets/kit/tile.png", AssetType::Texture, true).path, "assets/kit_real/tile.png");

    p.database.close();
    p.open();
    EXPECT_EQ(p.database.getPath(tile), fs::path("assets/kit_real/tile.png"));
    EXPECT_EQ(read(moved / "a.prefab"), prefabBytes);

    fs::remove(old);
    fs::rename(moved, old);
    p.database.refresh();
    EXPECT_EQ(p.database.getPath(tile), fs::path("assets/kit/tile.png"));
    EXPECT_EQ(read(old / "a.prefab"), prefabBytes);
}

// R7: a link out of the project is a project folder: indexed by the project path, metas beside the
// files, the nested links followed, cycles and a link to a folder holding the project ended
TEST(AssetIdentityLinks, LinkOutOfTheProjectIsAProjectFolder) {
    LinkProject p;
    const fs::path shared = p.outside / "shared";
    write(shared / "tile.png", "png");
    write(shared / "sub" / "a.prefab", prefabText(LINK_GUID_A));
    write(p.outside / "extra" / "more.json", "{}");
    write(p.outside / "single.png", "single");
    if (!makeDirLink(shared, p.project / "assets" / "shared")) GTEST_SKIP() << "no symbolic links here";
    ASSERT_TRUE(makeDirLink(p.outside / "extra", shared / "more"));
    ASSERT_TRUE(makeDirLink(shared, shared / "loop"));             // leads to the folder it is in
    ASSERT_TRUE(makeDirLink(shared / "sub", shared / "sub" / "self"));
    ASSERT_TRUE(makeDirLink(p.project, p.project / "assets" / "up"));   // leads to the project
    ASSERT_TRUE(makeDirLink(p.dir.path, shared / "above"));        // leads to what holds both
    ASSERT_TRUE(makeDirLink(p.outside / "nowhere", p.project / "assets" / "broken"));
    ASSERT_TRUE(makeFileLink(p.outside / "single.png", p.project / "assets" / "single.png"));
    p.open();

    const std::string tile = p.database.getGuid("assets/shared/tile.png");
    ASSERT_FALSE(tile.empty());
    EXPECT_TRUE(fs::is_regular_file(shared / "tile.png.meta")) << "the meta lies beside the file";
    EXPECT_EQ(p.database.getGuid("assets/shared/sub/a.prefab"), LINK_GUID_A);
    EXPECT_FALSE(p.database.getGuid("assets/shared/more/more.json").empty()) << "a link inside a linked folder";
    EXPECT_TRUE(fs::is_regular_file(p.outside / "extra" / "more.json.meta"));
    EXPECT_FALSE(p.database.getGuid("assets/single.png").empty());
    EXPECT_TRUE(fs::is_regular_file(p.project / "assets" / "single.png.meta")) << "a linked file's meta lies beside its project path";
    EXPECT_EQ(p.database.getPath(tile), fs::path("assets/shared/tile.png"));
    EXPECT_EQ(p.database.getGuid(p.project / "assets" / "shared" / "tile.png"), tile);
    EXPECT_EQ(p.database.getGuid(shared / "tile.png"), tile) << "its real path names it too";
    EXPECT_EQ(p.database.getGuid("assets/shared/loop/loop/tile.png"), tile) << "the path through the cycle names the same asset";
    p.database.forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        EXPECT_EQ(info.path.find("/loop"), std::string::npos) << info.path;
        EXPECT_EQ(info.path.find("/self"), std::string::npos) << info.path;
        EXPECT_NE(info.path.rfind("assets/up", 0), 0u) << info.path;
        EXPECT_EQ(info.path.find(".."), std::string::npos) << info.path;
    });
    EXPECT_EQ(p.database.getOrphanMetas().size(), 0u);

    // the same on the next session, from the metas
    const std::string meta = read(shared / "tile.png.meta");
    p.database.close();
    p.open();
    EXPECT_EQ(p.database.getGuid("assets/shared/tile.png"), tile);
    EXPECT_EQ(read(shared / "tile.png.meta"), meta);
    EXPECT_TRUE(p.database.refresh().empty()) << "the walk of a linked folder finds it as it is";

    // two names for one outside folder: one asset, at the first name in path order
    ASSERT_TRUE(makeDirLink(shared, p.project / "assets" / "zshared"));
    p.database.refresh();
    EXPECT_EQ(p.database.getPath(tile), fs::path("assets/shared/tile.png"));
    EXPECT_EQ(p.database.getGuid("assets/zshared/tile.png"), tile);

    // a change seen through the link
    write(shared / "tile.png", "another png");
    touchLater(shared / "tile.png");
    bool modified = false;
    for (const AssetDatabase::Change& change : p.database.refresh()) {
        modified = modified || (change.type == AssetDatabase::Change::Type::Modified && change.path == fs::path("assets/shared/tile.png"));
    }
    EXPECT_TRUE(modified);

    // the link removed: its assets leave the project, the target's files stay
    fs::remove(p.project / "assets" / "shared");
    fs::remove(p.project / "assets" / "zshared");
    p.database.refresh();
    EXPECT_TRUE(p.database.getGuid("assets/shared/tile.png").empty());
    EXPECT_TRUE(fs::is_regular_file(shared / "tile.png.meta"));
}

// R7: bounded: the walk through links stops at its limit, with what it reached kept
TEST(AssetIdentityLinks, WalkThroughLinksIsBounded) {
    LinkProject p;
    for (int i = 0; i < 40; i++) write(p.outside / "big" / ("f" + std::to_string(i) + ".txt"), "x");
    if (!makeDirLink(p.outside / "big", p.project / "assets" / "big")) GTEST_SKIP() << "no symbolic links here";
    write(p.project / "assets" / "own.txt", "mine");
    AssetDatabase::setLinkedEntryLimitForTesting(10);
    p.open();
    size_t linked = 0;
    p.database.forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.path.rfind("assets/big/", 0) == 0) linked++;
    });
    AssetDatabase::setLinkedEntryLimitForTesting(0);
    EXPECT_LE(linked, 10u);
    EXPECT_FALSE(p.database.getGuid("assets/own.txt").empty());
}

// R7: a read-only target is reported, not a failure: the assets work with stable GUIDs, nothing is
// tried again by every refresh. (As root, who writes through a read-only folder, a folder at the
// meta's name makes the write fail the same way.)
TEST(AssetIdentityLinks, ReadOnlyTargetIsReported) {
    LinkProject p;
    const fs::path shared = p.outside / "shared";
    write(shared / "tile.png", "png");
    if (!makeDirLink(shared, p.project / "assets" / "shared")) GTEST_SKIP() << "no symbolic links here";
    const bool root = runningAsRoot();
    if (root) {
        fs::create_directories(shared / "tile.png.meta" / "blocker");
    } else {
        fs::permissions(shared, fs::perms::owner_read | fs::perms::owner_exec);
    }
    struct Restore {
        fs::path folder;
        ~Restore() {
            std::error_code ec;
            fs::permissions(folder, fs::perms::owner_all, ec);
        }
    } restore{shared};
    p.open();
    const std::string guid = p.database.getGuid("assets/shared/tile.png");
    EXPECT_EQ(guid, AssetDatabase::legacyGuid("assets/shared/tile.png")) << "derived from its path: the same next session";
    EXPECT_FALSE(fs::is_regular_file(shared / "tile.png.meta"));
    EXPECT_EQ(p.database.getReadOnlyLinkedAssets(), std::vector<std::string>{"assets/shared/tile.png"});
    EXPECT_TRUE(p.database.refresh().empty()) << "no write tried again, no change reported";

    p.database.close();
    p.open();
    EXPECT_EQ(p.database.getGuid("assets/shared/tile.png"), guid);
    EXPECT_EQ(p.database.getReadOnlyLinkedAssets().size(), 1u);

    // writable again: the identity lands beside the file and the report goes
    if (root) {
        fs::remove_all(shared / "tile.png.meta");
    } else {
        fs::permissions(shared, fs::perms::owner_all);
    }
    p.database.close();
    p.open();
    EXPECT_EQ(p.database.getGuid("assets/shared/tile.png"), guid);
    EXPECT_TRUE(fs::is_regular_file(shared / "tile.png.meta"));
    EXPECT_TRUE(p.database.getReadOnlyLinkedAssets().empty());
}

// A link out of the project that is not under the assets root is a plain folder of the disk: not walked,
// and nothing written there, even when an import names a file behind it
TEST(AssetIdentityLinks, LinkOutsideTheAssetRootIsNotScanned) {
    LinkProject p;
    write(p.outside / "tile.png", "png");
    if (!makeDirLink(p.outside, p.project / "lib")) GTEST_SKIP() << "no symbolic links here";
    p.open();
    EXPECT_FALSE(fs::exists(p.outside / "tile.png.meta"));
    EXPECT_TRUE(p.database.getGuid("lib/tile.png").empty());
    EXPECT_TRUE(p.database.refresh().empty());
    EXPECT_TRUE(p.database.importAssets("lib/tile.png").empty());
    EXPECT_TRUE(p.database.importAssets("lib").empty());
    EXPECT_FALSE(fs::exists(p.outside / "tile.png.meta"));
    EXPECT_FALSE(p.database.allowsWritesAt(p.project / "lib" / "tile.png"));
    EXPECT_TRUE(p.database.allowsWritesAt(p.project / "assets" / "tile.png"));

    // the same link under the assets root is what the user linked in
    ASSERT_TRUE(makeDirLink(p.outside, p.project / "assets" / "lib"));
    p.database.refresh();
    EXPECT_FALSE(p.database.getGuid("assets/lib/tile.png").empty());
    EXPECT_TRUE(fs::is_regular_file(p.outside / "tile.png.meta"));
}

// A pinned root swapped for a link: not read, not written, whichever operation comes; back, it works
TEST(AssetIdentityLinks, RootSwappedForALinkIsNotFollowed) {
    LinkProject p;
    write(p.outside / "tile.png", "png");
    write(p.project / "assets" / "own.png", "own");
    const fs::path root = p.project / "assets";
    p.database.setPinnedRoots({{root, safewrite::resolvedPath(root)}}, fs::path());
    p.open();
    const std::string own = p.database.getGuid("assets/own.png");
    ASSERT_FALSE(own.empty());

    fs::rename(root, p.project / "assets_moved");
    if (!makeDirLink(p.outside, root)) GTEST_SKIP() << "no symbolic links here";
    p.database.refresh();
    EXPECT_TRUE(p.database.getGuid("assets/tile.png").empty());
    p.database.importAssets("assets/tile.png");
    p.database.importAssets("assets");
    p.database.registerAsset("assets/tile.png", LINK_GUID_A);
    EXPECT_FALSE(fs::exists(p.outside / "tile.png.meta")) << "nothing written through the swapped root";
    EXPECT_FALSE(p.database.allowsWritesAt(root / "tile.png"));

    p.database.close();
    p.open();
    EXPECT_FALSE(fs::exists(p.outside / "tile.png.meta")) << "nor by the scan on open";
    EXPECT_TRUE(p.database.getGuid("assets/tile.png").empty());

    fs::remove(root);
    fs::rename(p.project / "assets_moved", root);
    p.database.refresh();
    EXPECT_EQ(p.database.getGuid("assets/own.png"), own);
    EXPECT_TRUE(p.database.allowsWritesAt(root / "tile.png"));

    // a root that was a link when it was pinned stays followed
    const fs::path linked = p.dir.path / "linked_project";
    fs::create_directories(linked);
    write(p.outside / "second" / "b.png", "png");
    ASSERT_TRUE(makeDirLink(p.outside / "second", linked / "assets"));
    AssetDatabase other;
    other.setBackgroundHashing(false);
    other.setPinnedRoots({{linked / "assets", safewrite::resolvedPath(linked / "assets")}}, fs::path());
    other.open(linked, "assets");
    EXPECT_FALSE(other.getGuid("assets/b.png").empty());
    EXPECT_TRUE(fs::is_regular_file(p.outside / "second" / "b.png.meta"));
}

// R8: a project without links: no table, keys as written, no change
TEST(AssetIdentityLinks, NoLinksChangeNothing) {
    LinkProject p;
    write(p.project / "assets" / "a.prefab", prefabText(LINK_GUID_A));
    write(p.project / "assets" / "tile.png", "png");
    p.open();
    EXPECT_TRUE(p.database.getLinkedOutTargets().empty());
    EXPECT_EQ(p.database.toRelative("assets/nothing/../tile.png"), fs::path("assets/tile.png"));
    EXPECT_EQ(p.database.toRelative(p.project / "assets" / "tile.png"), fs::path("assets/tile.png"));
    EXPECT_EQ(p.database.getReadOnlyLinkedAssets().size(), 0u);
    EXPECT_TRUE(p.database.refresh().empty());
    // (what a real copy does is in OneFileIsNeverACopy; the cache keeps the format it had)
    EXPECT_TRUE(fs::is_regular_file(p.project / ".jasperengine" / "assetdb" / "index.json"));
}
