// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a project reaches of the optional engine modules (docs/specs/S34-player-size.md R5, AC2):
// every kind of evidence makes its module needed on its own, nothing else does, and the tables the
// detection reads (the Lua API's module names, the shader-neutral API) stay true to the bindings.

#include "players/ModuleUsage.h"
#include "players/ShaderNeutralApi.h"
#include "players/PlayerRuntimes.h"
#include "util/LuaApiData.h"
#include "player/PlayerModules.h"

#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/AudioSnapshotComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScriptComponent.h"
#include "component/TerrainComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/Transform.h"
#include "component/UIDocumentComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/VideoPlayerComponent.h"
#include "component/VoxelVolumeComponent.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace module_usage_test {

    // the modules as one string, "htmlui runtimeshaders" ("" for none)
    std::string modules(const ModuleUsage& usage) {
        std::string out;
        for (const std::string& module : usage.list()) out += (out.empty() ? "" : " ") + module;
        return out;
    }

    ModuleScan empty() {
        ModuleScan scan;
        scan.navigationInBuilds = false;
        return scan;
    }

    ModuleUsage withLua(const std::string& code) {
        ModuleScan scan = empty();
        scan.luaScripts.push_back({"lua/test.lua", code});
        return ModuleUsageDetector::analyze(scan);
    }

    // a scene with one entity holding a component
    struct OneEntity {
        Scene scene;
        Entity entity;
        OneEntity() {
            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            scene.setEntityName(entity, "Thing");
        }
        ModuleUsage usage() {
            ModuleScan scan = empty();
            scan.contents.push_back({"scene Main", &scene, {entity}});
            return ModuleUsageDetector::analyze(scan);
        }
    };

}

using namespace module_usage_test;

TEST(ModuleUsage, AnEmptyProjectNeedsNothing) {
    EXPECT_EQ(modules(ModuleUsageDetector::analyze(empty())), "");
    OneEntity plain;
    EXPECT_EQ(modules(plain.usage()), "");
    // a Lua script of math, input and logs
    EXPECT_EQ(modules(withLua("local v = Vector3(1, 2, 3)\nif Input.isKeyPressed(1) then Log.info(tostring(v:length())) end")), "");
}

// T5 AC17: a tree's capsule is a body the physics module has to build
TEST(ModuleUsage, TreeCapsulesNeedPhysics3d) {
    OneEntity plain;
    TerrainComponent terrain;
    TerrainTreePrototype prototype;
    prototype.id = 1;
    terrain.treePrototypes.push_back(prototype);
    plain.scene.addComponent<TerrainComponent>(plain.entity, terrain);
    EXPECT_EQ(modules(plain.usage()).find("physics3d"), std::string::npos) << "no collision, no module";

    plain.scene.getComponent<TerrainComponent>(plain.entity).treePrototypes[0].collision = TerrainTreeCollision::Capsule;
    EXPECT_NE(modules(plain.usage()).find("physics3d"), std::string::npos);
}

TEST(ModuleUsage, TheSoundLayerFollowsTheImportTable) {
    ModuleScan scan = empty();
    EXPECT_EQ(modules(ModuleUsageDetector::analyze(scan)), "");
    scan.audioImportTable = true;
    const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
    EXPECT_EQ(modules(usage), "soundlayer");
    EXPECT_NE(usage.firstEvidence("soundlayer").find("import settings"), std::string::npos);
}

TEST(ModuleUsage, NavigationFollowsItsProjectSetting) {
    ModuleScan scan = empty();
    scan.navigationInBuilds = true;
    const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
    EXPECT_EQ(modules(usage), "navigation");
    EXPECT_NE(usage.firstEvidence("navigation").find("Include in Builds"), std::string::npos);
}

TEST(ModuleUsage, ComponentsNeedTheirModules) {
    struct Case { const char* expected; std::function<void(Scene&, Entity)> add; };
    const std::vector<Case> cases = {
        {"htmlui", [](Scene& s, Entity e) { s.addComponent<UIDocumentComponent>(e, {}); }},
        {"physics3d", [](Scene& s, Entity e) { s.addComponent<RigidbodyComponent>(e, {}); }},
        {"physics3d", [](Scene& s, Entity e) { s.addComponent<Joint3DComponent>(e, {}); }},
        {"physics3d", [](Scene& s, Entity e) { s.addComponent<CharacterControllerComponent>(e, {}); }},
        {"physics3d", [](Scene& s, Entity e) { s.addComponent<BuoyancyComponent>(e, {}); }},
        {"physics2d", [](Scene& s, Entity e) { s.addComponent<Rigidbody2DComponent>(e, {}); }},
        {"physics2d", [](Scene& s, Entity e) { s.addComponent<Joint2DComponent>(e, {}); }},
        {"physics2d", [](Scene& s, Entity e) { s.addComponent<CharacterController2DComponent>(e, {}); }},
    };
    for (const Case& c : cases) {
        OneEntity one;
        c.add(one.scene, one.entity);
        const ModuleUsage usage = one.usage();
        EXPECT_EQ(modules(usage), c.expected);
        EXPECT_NE(usage.firstEvidence(c.expected).find("on Thing"), std::string::npos) << usage.firstEvidence(c.expected);
    }
}

TEST(ModuleUsage, ASplineExtrudeCollisionNeedsPhysics) {
    for (const auto& [collision, expected] : std::vector<std::pair<SplineCollision, std::string>>{
             {SplineCollision::None, ""}, {SplineCollision::Mesh, "physics3d"}, {SplineCollision::Chain2D, "physics2d"}}) {
        OneEntity one;
        SplineExtrudeComponent extrude;
        extrude.collision = collision;
        one.scene.addComponent<SplineExtrudeComponent>(one.entity, extrude);
        EXPECT_EQ(modules(one.usage()), expected);
    }
}

// T6 R48: each kind of evidence, and what a volume's fields add
TEST(ModuleUsage, VoxelComponentNeedsTheModule) {
    OneEntity one;
    VoxelVolumeComponent volume;
    volume.collision = false;
    volume.navigationUpdate = VoxelNavigationUpdate::Off;
    one.scene.addComponent<VoxelVolumeComponent>(one.entity, volume);
    const ModuleUsage usage = one.usage();
    // its generators reach Noise Graphs (R48), on the volume's own evidence
    EXPECT_EQ(modules(usage), "noise voxel");
    EXPECT_EQ(usage.firstEvidence("voxel"), "scene Main: Voxel Volume on Thing");
    EXPECT_EQ(usage.firstEvidence("noise"), "scene Main: Voxel Volume on Thing: Voxel Volume (its generators use Noise Graphs)");
}

TEST(ModuleUsage, VoxelCollisionAddsPhysics) {
    OneEntity one;
    VoxelVolumeComponent volume;
    volume.navigationUpdate = VoxelNavigationUpdate::Off;
    one.scene.addComponent<VoxelVolumeComponent>(one.entity, volume);
    const ModuleUsage usage = one.usage();
    EXPECT_EQ(modules(usage), "physics3d noise voxel");
    EXPECT_EQ(usage.firstEvidence("physics3d"), "scene Main: Voxel Volume collision on Thing");
}

TEST(ModuleUsage, VoxelNavigationUpdateAddsNavigationOnlyWithASurface) {
    OneEntity one;
    VoxelVolumeComponent volume;
    volume.collision = false;
    one.scene.addComponent<VoxelVolumeComponent>(one.entity, volume);
    // Automatic, and nothing to rebake
    EXPECT_EQ(modules(one.usage()), "noise voxel");
    one.scene.addComponent<NavMeshSurfaceComponent>(one.entity, {});
    const ModuleUsage usage = one.usage();
    EXPECT_EQ(modules(usage), "navigation noise voxel");
    EXPECT_EQ(usage.firstEvidence("navigation"), "scene Main: Voxel Volume navigation update on Thing");
    // Off: nothing rebakes it
    one.scene.getComponent<VoxelVolumeComponent>(one.entity).navigationUpdate = VoxelNavigationUpdate::Off;
    EXPECT_EQ(modules(one.usage()), "noise voxel");
}

TEST(ModuleUsage, VoxelScriptPropertyNeedsTheModule) {
    OneEntity one;
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.className = "Digger";
    ScriptProperty property;
    property.name = "cave";
    property.ptrTypeName = "VoxelVolume*";
    entry.properties.push_back(property);
    scripts.scripts.push_back(entry);
    one.scene.addComponent<ScriptComponent>(one.entity, scripts);
    const ModuleUsage usage = one.usage();
    EXPECT_TRUE(usage.needs("voxel"));
    EXPECT_NE(usage.firstEvidence("voxel").find("script property cave (VoxelVolume)"), std::string::npos) << usage.firstEvidence("voxel");
}

TEST(ModuleUsage, VoxelLuaNameNeedsTheModule) {
    const ModuleUsage usage = withLua("local v = VoxelVolume.fromGameObject(self)");
    EXPECT_TRUE(usage.needs("voxel"));
    EXPECT_NE(usage.firstEvidence("voxel").find("lua/test.lua:1"), std::string::npos) << usage.firstEvidence("voxel");
    EXPECT_FALSE(withLua("local v = 1").needs("voxel"));
}

TEST(ModuleUsage, ScriptPropertyTypesNeedTheirModules) {
    OneEntity one;
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.className = "Hud";
    ScriptProperty property;
    property.name = "document";
    property.ptrTypeName = "UIDocument*";
    entry.properties.push_back(property);
    scripts.scripts.push_back(entry);
    one.scene.addComponent<ScriptComponent>(one.entity, scripts);
    const ModuleUsage usage = one.usage();
    EXPECT_TRUE(usage.needs("htmlui"));
    EXPECT_NE(usage.firstEvidence("htmlui").find("script property document (UIDocument)"), std::string::npos) << usage.firstEvidence("htmlui");
    // a Rigidbody property: physics, and no shader input
    OneEntity body;
    scripts.scripts[0].properties[0].ptrTypeName = "Rigidbody*";
    body.scene.addComponent<ScriptComponent>(body.entity, scripts);
    EXPECT_EQ(modules(body.usage()), "physics3d");
}

TEST(ModuleUsage, LuaNamesNeedTheirModules) {
    // a class of the module
    EXPECT_TRUE(withLua("local doc = UIDocument(self.scene, self.entity)").needs("htmlui"));
    EXPECT_TRUE(withLua("local body = Rigidbody(self.scene, self.entity)").needs("physics3d"));
    EXPECT_TRUE(withLua("local body = Rigidbody2D(self.scene, self.entity)").needs("physics2d"));
    EXPECT_TRUE(withLua("anime({targets = t, x = 100})").needs("htmlui"));
    // a member under the module's condition on a class outside it
    const ModuleUsage member = withLua("local go = GameObject(self.scene, self.entity)\nlocal b = go:getRigidbody()");
    EXPECT_EQ(modules(member), "physics3d");
    EXPECT_EQ(member.firstEvidence("physics3d"), "lua/test.lua:2: getRigidbody");
    EXPECT_TRUE(withLua("InputActions.bindVirtualStick(el, 'move')").needs("htmlui"));
    // an enum of the module
    EXPECT_TRUE(withLua("local t = Joint3DType.HINGE").needs("physics3d"));
    // a member that makes the engine create bodies (a Spline Extrude's collision)
    const ModuleUsage creating = withLua("extrude.collision = 1");
    EXPECT_TRUE(creating.needs("physics3d"));
    EXPECT_TRUE(creating.needs("physics2d"));
    // a member's name in a string counts (go["getRigidbody"]); a class's does not (only _G and the
    // like reach a global by its name), nor a name in a comment
    EXPECT_EQ(modules(withLua("local go = GameObject(self.scene, self.entity)\nlocal body = go['getRigidbody'](go)")), "physics3d");
    EXPECT_EQ(modules(withLua("print('Light on, press the Button to open the UIDocument')")), "");
    EXPECT_EQ(modules(withLua("-- UIDocument Rigidbody\n--[[ getRigidbody ]] local x = 1")), "");
    EXPECT_EQ(modules(withLua("--[==[ UIDocument ]==] local x = [[ text ]]")), "");
    // a name on its own is a local or a global, never a member: a local, a function or a table key
    // named like a member reaches nothing; the same name after . or : (or in a string) does
    EXPECT_EQ(modules(withLua("local getRigidbody = 3\nlocal t = {getRigidbody = 2, collision = 1}\nlocal function volume() end")), "");
    EXPECT_EQ(modules(withLua("local go = GameObject(self.scene, self.entity)\nlocal b = go.getRigidbody")), "physics3d");
    const std::string ui = "local doc = UIDocument(self.scene, self.entity)\n";
    EXPECT_FALSE(withLua(ui + "local duration = 3\nlocal t = {muted = true}").needs("video"));
    EXPECT_TRUE(withLua(ui + "local d = el.duration").needs("video"));
    EXPECT_TRUE(withLua(ui + "local d = el['duration']").needs("video"));
}

TEST(ModuleUsage, DynamicAccessNeedsEveryModule) {
    const std::string every = "htmlui video runtimeshaders physics3d physics2d noise voxel";
    for (const char* code : {"local f = _G[name]", "local f = _ENV[name]", "local chunk = load(text)", "local s = loadstring(text)",
                             "dofile(path)", "local r = debug.getregistry()", "local g = package.loaded", "local m = require(name)",
                             "local g = require('_G')"}) {
        const ModuleUsage usage = withLua(code);
        EXPECT_EQ(modules(usage), every) << code;
    }
    // a project module's require, and a method named load, are not
    EXPECT_EQ(modules(withLua("local util = require('lib.util')\nlocal other = require \"lib.other\"")), "");
    EXPECT_EQ(modules(withLua("self.prefab:load()\nlocal x = self.load")), "");
}

TEST(ModuleUsage, CppScriptsNeedEveryModule) {
    ModuleScan scan = empty();
    scan.cppScripts.push_back("scripts/Player.cpp");
    const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
    EXPECT_EQ(modules(usage), "htmlui video runtimeshaders physics3d physics2d noise voxel");
    EXPECT_NE(usage.firstEvidence("htmlui").find("scripts/Player.cpp"), std::string::npos);
}

TEST(ModuleUsage, StripEngineCodeOffNeedsEveryModule) {
    ModuleScan scan = empty();
    scan.stripEngineCode = false;
    EXPECT_EQ(ModuleUsageDetector::analyze(scan).list(), player::allModules());
}

// a Video Player plays without a document (upstream's component): its project needs the video module,
// which comes with the HTML UI, or its build would play nothing
TEST(ModuleUsage, AVideoPlayerNeedsVideoAndTheHtmlUI) {
    OneEntity one;
    one.scene.addComponent<VideoPlayerComponent>(one.entity, {});
    const ModuleUsage byComponent = one.usage();
    EXPECT_TRUE(byComponent.needs("video"));
    EXPECT_TRUE(byComponent.needs("htmlui"));

    const ModuleUsage byLua = withLua("local v = GameObject(self.scene, self.entity):getVideoPlayer()");
    EXPECT_TRUE(byLua.needs("video"));
    EXPECT_TRUE(byLua.needs("htmlui"));
}

// the debug overlay is an HTML UI document: a project that ships it, or asks for it, keeps the module
TEST(ModuleUsage, TheDebugOverlayNeedsTheHtmlUI) {
    ModuleScan scan = empty();
    EXPECT_FALSE(ModuleUsageDetector::analyze(scan).needs("htmlui"));
    scan.debugOverlayInBuilds = true;
    EXPECT_TRUE(ModuleUsageDetector::analyze(scan).needs("htmlui"));
    EXPECT_TRUE(withLua("DebugOverlay.showConsole()").needs("htmlui"));
}

TEST(ModuleUsage, VideoNeedsTheHtmlUIAndAVideo) {
    auto withDocument = [](const std::string& html, bool component) {
        OneEntity one;
        if (component) one.scene.addComponent<UIDocumentComponent>(one.entity, {});
        ModuleScan scan = empty();
        scan.contents.push_back({"scene Main", &one.scene, {one.entity}});
        scan.documents.push_back({"ui/menu.html", html});
        return modules(ModuleUsageDetector::analyze(scan));
    };
    EXPECT_EQ(withDocument("<body><video src='intro.webm' autoplay></video></body>", true), "htmlui video");
    EXPECT_EQ(withDocument("<body><p>Video settings</p></body>", true), "htmlui");
    // no document loads without the HTML UI: its video is not needed either
    EXPECT_EQ(withDocument("<body><VIDEO src='intro.webm'></VIDEO></body>", false), "");
    // a script making one
    ModuleScan scan = empty();
    scan.luaScripts.push_back({"lua/hud.lua", "local doc = UIDocument(self.scene, self.entity)\nlocal v = doc:createElement('video')"});
    EXPECT_EQ(modules(ModuleUsageDetector::analyze(scan)), "htmlui video runtimeshaders");
}

TEST(ModuleUsage, DocumentScriptsAreScanned) {
    ModuleScan scan = empty();
    scan.documents.push_back({"ui/menu.html", "<body onclick=\"go:getRigidbody()\">\n<script type=\"text/lua\">\nlocal m = Mesh(scene)\n</script></body>"});
    const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
    EXPECT_TRUE(usage.needs("physics3d"));
    EXPECT_TRUE(usage.needs("runtimeshaders"));
    EXPECT_EQ(usage.firstEvidence("runtimeshaders"), "ui/menu.html:3: Mesh");
}

// the debug drawing's lines are a LineRenderer's, made at runtime; a physics query draws nothing
TEST(ModuleUsage, PhysicsDebugDrawingNeedsRuntimeShaders) {
    EXPECT_TRUE(withLua("Physics.debugDraw = true").needs("runtimeshaders"));
    EXPECT_TRUE(withLua("Physics.debugDrawContacts = true").needs("runtimeshaders"));
    EXPECT_FALSE(withLua("local hit = Physics.raycast(self.scene, Ray(Vector3(0, 0, 0), Vector3(0, 1, 0)))").needs("runtimeshaders"));
}

TEST(ModuleUsage, RuntimeShadersFollowShaderRelevantNames) {
    // moves, spawns prefabs, plays sounds and animations, reads input: every variant is cooked
    const std::string neutral =
        "local Gem = {}\nfunction Gem:init()\n  self.go = GameObject(self.scene, self.entity)\n"
        "  local player = self.scene:findEntity('Player')\n  self.body = Rigidbody(self.scene, player)\n"
        "  RegisterEngineEvent(self, 'onUpdate')\nend\nfunction Gem:onUpdate()\n"
        "  self.go:setRotation(0, Engine.deltatime * 90, 0)\n  local p = self.go:getWorldPosition()\n"
        "  self.go:setActive(false)\n  self.scene.shadowQuality = ShadowQuality.HIGH\n  self.scene.maxLightsPerObject = 8\nend\nreturn Gem";
    EXPECT_FALSE(withLua(neutral).needs("runtimeshaders"));
    // a mesh made at runtime, a material, a light, a scene setting the cook does not expand
    for (const char* code : {"local m = Mesh(self.scene)", "local mat = Material()", "local l = Light(self.scene)",
                             "self.scene.postProcessing = settings", "GameObject.instantiate(original, parent)",
                             "local c = Camera(self.scene, e)\nc.renderToTexture = true"}) {
        EXPECT_TRUE(withLua(code).needs("runtimeshaders")) << code;
    }
    // a script property of a drawing type
    OneEntity one;
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    ScriptProperty property;
    property.name = "target";
    property.ptrTypeName = "Mesh*";
    entry.properties.push_back(property);
    scripts.scripts.push_back(entry);
    one.scene.addComponent<ScriptComponent>(one.entity, scripts);
    EXPECT_EQ(modules(one.usage()), "runtimeshaders");
}

TEST(ModuleUsage, TheLuaEntryPointsComeFromTheBindingConditions) {
    const auto entry = ModuleUsageDetector::moduleEntryPoints(LuaApiData::json());
    for (const char* module : {"htmlui", "video", "physics3d", "physics2d"}) {
        ASSERT_TRUE(entry.count(module)) << module;
        EXPECT_FALSE(entry.at(module).empty()) << module;
    }
    const std::set<std::string>& html = entry.at("htmlui");
    for (const char* name : {"UIDocument", "UIElement", "anime", "bindVirtualStick", "UIItemsView"}) EXPECT_TRUE(html.count(name)) << name;
    // the members of the module's own classes are reached through those
    EXPECT_FALSE(html.count("getElementById"));
    EXPECT_TRUE(entry.at("video").count("canPlayType"));
    for (const char* name : {"Rigidbody", "getRigidbody", "raycast", "Joint3DType", "CharacterController"})
        EXPECT_TRUE(entry.at("physics3d").count(name)) << name;
    EXPECT_TRUE(entry.at("physics2d").count("Rigidbody2D"));
    // nothing of the always-built API
    for (const auto& [module, names] : entry)
        for (const char* name : {"Vector3", "GameObject", "setPosition", "Engine", "Scene"}) EXPECT_FALSE(names.count(name)) << module << " " << name;
}

TEST(ModuleUsage, TheBodyCreatingMembersAreBound) {
    // creatingMembers() names SplineExtrude.collision and the SplineCollision enum
    const mcp::Json extrude = LuaApiData::findClass("SplineExtrude");
    ASSERT_TRUE(extrude.is_object());
    bool found = false;
    for (const mcp::Json& property : extrude["properties"]) found = found || property.value("name", "") == "collision";
    EXPECT_TRUE(found);
    bool enumFound = false;
    for (const mcp::Json& en : LuaApiData::json()["enums"]) enumFound = enumFound || en.value("name", "") == "SplineCollision";
    EXPECT_TRUE(enumFound);
}

TEST(ShaderNeutralApi, NeutralClassesAreTheReviewedOnes) {
    // a member added to a neutral class must be reviewed: can it change what an object draws
    // with? Then the class is mixed (or relevant); else its hash is updated here
    for (const auto& [name, hash] : ShaderNeutralApi::neutralClasses()) {
        const mcp::Json cls = LuaApiData::findClass(name);
        ASSERT_TRUE(cls.is_object()) << name << " is not in the Lua API any more";
        std::set<std::string> members;
        for (const char* key : {"methods", "properties"})
            for (const mcp::Json& member : cls[key]) members.insert(member.value("name", ""));
        std::string list;
        for (const std::string& member : members) list += " " + member;
        EXPECT_EQ(ShaderNeutralApi::memberHash(members), hash) << name << "'s Lua members changed (now:" << list
                                                               << "): review them and update its hash in ShaderNeutralApi.cpp";
    }
    for (const auto& [name, neutral] : ShaderNeutralApi::mixedClasses()) {
        const mcp::Json cls = LuaApiData::findClass(name);
        ASSERT_TRUE(cls.is_object()) << name;
        std::set<std::string> members;
        for (const char* key : {"methods", "properties"})
            for (const mcp::Json& member : cls[key]) members.insert(member.value("name", ""));
        for (const std::string& member : neutral) EXPECT_TRUE(members.count(member)) << name << "." << member << " is not bound";
    }
}

TEST(ShaderNeutralApi, NeutralMembersGiveNoDrawingObject) {
    // what a neutral member gives a script is neutral, mixed or a value: a member typed with a
    // shader-relevant class would be an entry point to it, and its class could not be neutral
    std::set<std::string> relevant;
    for (const mcp::Json& cls : LuaApiData::json()["classes"]) {
        const std::string name = cls.value("name", "");
        if (!ShaderNeutralApi::isNeutralClass(name) && !ShaderNeutralApi::mixedClasses().count(name)) relevant.insert(name);
    }
    auto typesOf = [](const mcp::Json& member) {
        std::vector<std::string> types;
        std::string text = member.value("type", "");
        for (const mcp::Json& overload : member.value("overloads", mcp::Json::array()))
            for (const mcp::Json& ret : overload.value("returns", mcp::Json::array())) text += " " + ret.get<std::string>();
        std::string word;
        for (char c : text + " ") {
            if (std::isalnum((unsigned char)c) || c == '_') word += c;
            else if (!word.empty()) { types.push_back(word); word.clear(); }
        }
        return types;
    };
    for (const auto& [name, hash] : ShaderNeutralApi::neutralClasses()) {
        const mcp::Json cls = LuaApiData::findClass(name);
        for (const char* key : {"methods", "properties"})
            for (const mcp::Json& member : cls[key])
                for (const std::string& type : typesOf(member))
                    EXPECT_FALSE(relevant.count(type)) << name << "." << member.value("name", "") << " gives a " << type;
    }
    for (const auto& [name, neutral] : ShaderNeutralApi::mixedClasses()) {
        const mcp::Json cls = LuaApiData::findClass(name);
        for (const char* key : {"methods", "properties"})
            for (const mcp::Json& member : cls[key]) {
                if (!neutral.count(member.value("name", ""))) continue;
                for (const std::string& type : typesOf(member))
                    EXPECT_FALSE(relevant.count(type)) << name << "." << member.value("name", "") << " gives a " << type;
            }
    }
    // the names a script uses most are neutral; what draws is not
    const std::set<std::string> relevantNames = ModuleUsageDetector::shaderRelevantNames(LuaApiData::json());
    for (const char* name : {"setPosition", "getWorldPosition", "findEntity", "deltatime", "Vector3", "Rigidbody", "AudioSource", "Animator"})
        EXPECT_FALSE(relevantNames.count(name)) << name;
    for (const char* name : {"Mesh", "Material", "Light", "Texture", "instantiate", "postProcessing", "renderToTexture", "Sprite"})
        EXPECT_TRUE(relevantNames.count(name)) << name;
}

TEST(PlayerVariants, TheSmallestVariantHoldingTheModules) {
    // engine/player/variants.json, as the editor reads it
    const std::vector<PlayerVariant>& variants = PlayerRuntimes::variants();
    ASSERT_FALSE(variants.empty());
    EXPECT_EQ(variants.front().suffix, "");
    EXPECT_EQ(variants.front().modules, player::allModules());
    EXPECT_EQ(PlayerRuntimes::variantFor({"physics3d", "navigation"}).name, "lite3d");
    EXPECT_EQ(PlayerRuntimes::variantFor({"physics2d"}).name, "lite2d");
    // of two with as many modules, the later (smaller) one
    EXPECT_EQ(PlayerRuntimes::variantFor({}).name, "lite2d");
    EXPECT_EQ(PlayerRuntimes::variantFor({"navigation"}).name, "lite2d");
    EXPECT_EQ(PlayerRuntimes::variantFor({"runtimeshaders", "physics3d"}).name, "core");
    EXPECT_EQ(PlayerRuntimes::variantFor({"htmlui", "physics2d"}).name, "ui");
    EXPECT_EQ(PlayerRuntimes::variantFor({"htmlui", "video"}).name, "full");
    // T6 R47: full, ui and core hold Voxel Volumes, the lite ones do not
    EXPECT_EQ(PlayerRuntimes::variantFor({"voxel"}).name, "core");
    EXPECT_EQ(PlayerRuntimes::variantFor({"htmlui", "voxel"}).name, "ui");
    EXPECT_EQ(PlayerRuntimes::variantFor({"htmlui", "video", "voxel"}).name, "full");
    for (const PlayerVariant& variant : variants) {
        const bool has = std::find(variant.modules.begin(), variant.modules.end(), "voxel") != variant.modules.end();
        EXPECT_EQ(has, variant.name != "lite3d" && variant.name != "lite2d") << variant.name;
    }
    for (const PlayerVariant& variant : variants)
        for (const std::string& module : variant.modules)
            EXPECT_NE(std::find(player::allModules().begin(), player::allModules().end(), module), player::allModules().end()) << module;

    // of installed runtimes, the one that stages the fewest bytes and has the modules
    PlayerInfo full, lite;
    full.id = "linux-x64-glcore-release";
    full.features = player::allModules();
    full.shippedBytes = 40;
    lite.id = "linux-x64-glcore-release-lite3d";
    lite.features = {"physics3d", "navigation"};
    lite.shippedBytes = 30;
    const std::vector<PlayerInfo> installed = {full, lite};
    EXPECT_EQ(PlayerRuntimes::smallestWith(installed, {"physics3d"})->id, lite.id);
    EXPECT_EQ(PlayerRuntimes::smallestWith(installed, {"htmlui"})->id, full.id);
    EXPECT_EQ(PlayerRuntimes::smallestWith({lite}, {"htmlui"}), nullptr);
    EXPECT_EQ(PlayerRuntimes::runtimeId("linux", "x64", "glcore", false, "lite3d"), "linux-x64-glcore-release-lite3d");
    EXPECT_EQ(PlayerRuntimes::runtimeId("linux", "x64", "glcore", false), "linux-x64-glcore-release");
}

// T2 R43, AC34: each kind of evidence for noise, and what is not evidence
TEST(ModuleUsage, NoiseEvidence) {
    EXPECT_TRUE(withLua("local v = Noise.perlin(1, 2)").needs("noise"));
    EXPECT_TRUE(withLua("local g = NoiseGraph.load('a.noisegraph')").needs("noise"));
    // dynamic access and C++ code reach anything, which detectedModules covers
    EXPECT_TRUE(withLua("local t = _G[name]").needs("noise"));
    ModuleScan cpp = empty();
    cpp.cppScripts.push_back("scripts/Player.cpp");
    EXPECT_TRUE(ModuleUsageDetector::analyze(cpp).needs("noise"));
}

TEST(ModuleUsage, ABakedProjectDoesNotNeedNoise) {
    EXPECT_FALSE(ModuleUsageDetector::analyze(empty()).needs("noise"));
    // a script that never names it; a .noisegraph on disk is not part of the scan at all
    EXPECT_FALSE(withLua("local noise = 1; print(noise)").needs("noise"));
}

// docs/specs/S37-sound-events.md 6.6: what needs the soundlayer module, and every variant has it
TEST(ModuleUsageSoundLayer, ASceneWithoutItsFeaturesDoesNotNeedIt) {
    Scene scene;
    const Entity plain = scene.createEntity();
    scene.addComponent<AudioSourceComponent>(plain, AudioSourceComponent());
    ModuleScan scan;
    scan.contents.push_back({"scene Plain", &scene, {plain}});
    EXPECT_FALSE(ModuleUsageDetector::analyze(scan).needs("soundlayer"));
}

TEST(ModuleUsageSoundLayer, EachFeatureOfASourceNeedsIt) {
    const std::vector<std::pair<std::function<void(AudioSourceComponent&)>, std::string>> rows = {
        {[](AudioSourceComponent& a) { a.event = "e.soundevent"; }, "plays a Sound Event"},
        {[](AudioSourceComponent& a) { a.playOn = AudioPlayOn_UIClick; }, "Play On triggers"},
        {[](AudioSourceComponent& a) { a.pitchRandomSemitones = 1.0f; }, "random ranges"},
        {[](AudioSourceComponent& a) { a.volumeRandomDb = 1.0f; }, "random ranges"},
    };
    for (const auto& [set, evidence] : rows) {
        Scene scene;
        const Entity entity = scene.createEntity();
        AudioSourceComponent audio;
        set(audio);
        scene.addComponent<AudioSourceComponent>(entity, audio);
        ModuleScan scan;
        scan.contents.push_back({"scene Main", &scene, {entity}});
        const ModuleUsage usage = ModuleUsageDetector::analyze(scan);
        EXPECT_TRUE(usage.needs("soundlayer")) << evidence;
        EXPECT_NE(usage.firstEvidence("soundlayer").find(evidence), std::string::npos) << usage.firstEvidence("soundlayer");
    }
}

TEST(ModuleUsageSoundLayer, LuaNamesFromTheBindingsNeedIt) {
    for (const char* code : {"Audio.play('x')", "Audio.stopAll()", "local h = SoundHandle()", "local f = AudioPlayOn.UIClick"}) {
        ModuleScan scan;
        scan.luaScripts.push_back({"scripts/a.lua", code});
        EXPECT_TRUE(ModuleUsageDetector::analyze(scan).needs("soundlayer")) << code;
    }
    ModuleScan plain;
    plain.luaScripts.push_back({"scripts/a.lua", "local s = AudioSource(self.scene, self.entity); s:play()"});
    EXPECT_FALSE(ModuleUsageDetector::analyze(plain).needs("soundlayer"));
}

// docs/specs/S40-timeline-audio.md 6.6: clips with audio cues and the legacy timeline's audio and snapshot actions
TEST(ModuleUsageSoundLayer, TimelineAudioNeedsIt) {
    ModuleScan clips;
    clips.cueClips.push_back("assets/anim/walk.anim");
    const ModuleUsage fromClip = ModuleUsageDetector::analyze(clips);
    EXPECT_TRUE(fromClip.needs("soundlayer"));
    EXPECT_NE(fromClip.firstEvidence("soundlayer").find("audio cues"), std::string::npos) << fromClip.firstEvidence("soundlayer");

    Scene scene;
    const Entity audioAction = scene.createEntity();
    scene.addComponent<ActionComponent>(audioAction, ActionComponent());
    scene.addComponent<AudioSourceComponent>(audioAction, AudioSourceComponent());
    ModuleScan actions;
    actions.contents.push_back({"scene Main", &scene, {audioAction}});
    const ModuleUsage fromAction = ModuleUsageDetector::analyze(actions);
    EXPECT_TRUE(fromAction.needs("soundlayer"));
    EXPECT_NE(fromAction.firstEvidence("soundlayer").find("audio action"), std::string::npos) << fromAction.firstEvidence("soundlayer");

    Scene snapshots;
    const Entity snapshotAction = snapshots.createEntity();
    snapshots.addComponent<ActionComponent>(snapshotAction, ActionComponent());
    snapshots.addComponent<AudioSnapshotComponent>(snapshotAction, AudioSnapshotComponent());
    ModuleScan snapshotScan;
    snapshotScan.contents.push_back({"scene Main", &snapshots, {snapshotAction}});
    EXPECT_TRUE(ModuleUsageDetector::analyze(snapshotScan).needs("soundlayer"));
}

TEST(ModuleUsageSoundLayer, EveryPrebuiltVariantHasIt) {
    const std::vector<PlayerVariant>& variants = PlayerRuntimes::variants();
    ASSERT_FALSE(variants.empty());
    for (const PlayerVariant& variant : variants) {
        EXPECT_NE(std::find(variant.modules.begin(), variant.modules.end(), "soundlayer"), variant.modules.end()) << variant.name;
    }
}
