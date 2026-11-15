// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode, Record and curves view through the window's own actions
// (animation clips spec 4.4, 9.R4-R7/R31/R39/R50/R54), on a headless copy of the pose fixture
// project (Hero: hero_tpose.gltf, Mixamo bone names; Robot). The window is driven by
// AnimationWindowClipDriver.h (the functions its buttons, menus and shortcuts run, by name; its state
// as JSON), and draws in a headless ImGui frame where the frame matters (shortcuts, responsiveness).
// What needs the real editor (real X key presses and clicks: the App's key routing, R5b's keyboard
// undo, focus, the row menu, the gizmo drag) is tools/mcp/tests/test_animation_window.py.
//
// Covered: the mode switch's empty state, Create... (one scene step), Add Property (R39 names refused),
// keys (select, Shift, box by row and time, drag snapped to frames, delete, copy/paste with skipped /
// replaced keys, duplicate, ease, K after play/pause on the Samples grid, the value column's pending
// value, an Euler edit), missing bindings and Rebind, the events lane with a Lua script on the root
// (handler found / missing, selection by identity across undo), Samples and Snap keys to frames, the
// read-only sub-clip (Preview on its model, Extract alone and through the player's state), clips
// dropped on the window, preview isolation (a save writes the unpreviewed scene), a 36 000-key clip
// staying responsive; Record (gizmo, Inspector rows, multi-selection with an unbound GameObject,
// blend shapes, lattice drags, R, leaving Record restoring the scene); the curves view (curves of every
// component, rotation as continuous Euler degrees, vertical / time drags in one undo step, a drag of
// keys of two properties, visibility, framing, a large clip).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "component/Transform.h"

#include "AnimationWindowClipDriver.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/ClipRecordHooks.h"
#include "animation/PreviewOwner.h"
#include "shortcuts/DefaultCommands.h"
#include "shortcuts/ShortcutDispatcher.h"
#include "window/AnimationWindowClip.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/AudioSystem.h"
#include "audio/WavWriter.h"
#include "audio/TimelineAudioPreview.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

    // the App's part: what Project calls on the editor host reaches the clip hooks
    struct WindowHost : public EditorHost {
        std::mutex postedMutex;
        std::condition_variable taskPosted;
        uint64_t postedGeneration = 0;

        void enqueueMainThreadTask(std::function<void()> task) override {
            EditorHost::enqueueMainThreadTask(std::move(task));
            {
                std::lock_guard<std::mutex> lock(postedMutex);
                postedGeneration++;
            }
            taskPosted.notify_all();
        }
        uint64_t taskGeneration() {
            std::lock_guard<std::mutex> lock(postedMutex);
            return postedGeneration;
        }
        void waitForTaskAfter(uint64_t generation) {
            std::unique_lock<std::mutex> lock(postedMutex);
            taskPosted.wait(lock, [&]() { return postedGeneration != generation; });
        }
        void stopTransientPreviews() override {
            if (animationClipAppHooks().stopTransientPreviews) animationClipAppHooks().stopTransientPreviews();
        }
        void clearSceneWindowState(uint32_t sceneId) override {
            if (animationClipAppHooks().sceneClosed) animationClipAppHooks().sceneClosed(sceneId);
        }
        void prepareForProjectSwitch() override {
            if (animationClipAppHooks().projectClosed) animationClipAppHooks().projectClosed();
        }
    };

    // A worker's post may finish after TearDown restores the default host.
    WindowHost& animationWindowClipHost() {
        static WindowHost* host = new WindowHost();
        return *host;
    }

    fs::path repoPath(const fs::path& relative) {
        return (fs::path(JASPER_TEST_REPO_DIR) / relative).lexically_normal();
    }

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    // the pose fixture project, copied (its Main Scene: Hero, Robot), loaded headless
    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-clip-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::copy(repoPath("tools/fixtures/pose-animation"), out.dir, fs::copy_options::recursive);
            fs::create_directories(out.dir / ".jasperengine");
            fs::copy_file(repoPath("tools/fixtures/runtime-shaders/assets/skinned_morph.gltf"), out.dir / "assets" / "models" / "skinned_morph.gltf",
                          fs::copy_options::overwrite_existing);
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            // the project's settings may turn it on again: models load synchronously here
            Engine::setAsyncLoading(false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool headlessStarted = false;

    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }

    struct TempProjectCleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const tempProjectCleanup = ::testing::AddGlobalTestEnvironment(new TempProjectCleanup);

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    bool close(const Json& a, const std::vector<float>& b, float tolerance = 1e-3f) {
        if (!a.is_array() || a.size() != b.size()) return false;
        for (size_t i = 0; i < b.size(); i++) {
            if (!a[i].is_number() || std::fabs(a[i].get<float>() - b[i]) > tolerance) return false;
        }
        return true;
    }

    bool close(const Vector3& a, const Vector3& b, float tolerance = 1e-3f) {
        return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
    }

    bool sameRotation(const Json& value, const std::vector<float>& xyzw, float tolerance = 1e-3f) {
        std::vector<float> negated;
        for (float v : xyzw) negated.push_back(-v);
        return close(value, xyzw, tolerance) || close(value, negated, tolerance);
    }

    // the clip's Euler convention (anim::eulerDegreesToQuaternion, ZYX: q = qz * qy * qx), x y z w
    std::vector<float> eulerXYZW(float x, float y, float z) {
        const float hx = x * (float)M_PI / 360.0f, hy = y * (float)M_PI / 360.0f, hz = z * (float)M_PI / 360.0f;
        const float cx = std::cos(hx), sx = std::sin(hx), cy = std::cos(hy), sy = std::sin(hy), cz = std::cos(hz), sz = std::sin(hz);
        return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
    }

    const Json* row(const Json& st, const std::string& path, const std::string& channel) {
        for (const Json& r : st["rows"]) {
            if (r.contains("path") && r["path"] == path && r.contains("channel") && r["channel"] == channel) return &r;
        }
        return nullptr;
    }

    std::vector<float> keysOf(const Json& st, const std::string& path, const std::string& channel) {
        std::vector<float> out;
        if (const Json* r = row(st, path, channel)) {
            for (const Json& t : (*r)["keys"]) out.push_back(std::round(t.get<float>() * 10000.0f) / 10000.0f);
        }
        return out;
    }

    int trackOf(const Json& st, const std::string& path, const std::string& channel) {
        const Json* r = row(st, path, channel);
        return r ? (*r)["track"].get<int>() : -1;
    }

    const Json* curve(const Json& st, int track, int component) {
        if (!st.contains("curves")) return nullptr;
        for (const Json& c : st["curves"]) {
            if (c["track"] == track && c["component"] == component) return &c;
        }
        return nullptr;
    }

    // the clip mode's draw budget for a large clip (I1 / I3); the sanitizers slow everything several
    // times, where only the rest of the test means something
#if defined(__SANITIZE_ADDRESS__)
    const double DRAW_BUDGET_MS = 1e9;
#else
    const double DRAW_BUDGET_MS = 30.0;
#endif

    bool onGrid(float time, float fps) {
        return std::fabs(time * fps - std::round(time * fps)) < 1e-3f;
    }

    const char* const SCENE_FILE = "Main Scene.scene";
    const char* const CLIP = "assets/anim/hero_clip.anim";
    const char* const SPINE = "Hero/mixamorig:Hips/mixamorig:Spine";

    // The window on the fixture's Main Scene (reopened from its file for every test), a headless
    // ImGui context drawing it, and an Inspector of the project (the record sources' rows)
    class AnimationWindowClipTest : public ::testing::Test {
    protected:
        WindowHost& host = animationWindowClipHost();
        ImGuiContext* previousContext = nullptr;
        ImGuiContext* context = nullptr;
        std::unique_ptr<AnimationWindow> window;
        // the command catalog's animation.* keys (S25), dispatched as the App does
        CommandCatalog catalog;
        std::unique_ptr<ShortcutDispatcher> dispatcher;
        std::unique_ptr<InspectorWindow> inspector;
        uint32_t sceneId = 0;

        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
            AnimationClipController::installHistoryTimeline(project());
            previousContext = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1400, 800);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            // a fresh copy of the fixture's scene and no clips from an earlier test
            fs::remove_all(headless().dir / "assets" / "anim");
            fs::remove_all(headless().dir / "scripts");
            fs::copy_file(repoPath("tools/fixtures/pose-animation") / SCENE_FILE, headless().dir / SCENE_FILE, fs::copy_options::overwrite_existing);
            AnimationClipDocument::checkDiskChanges(project());
            // the Main Scene closed (another scene kept open) and opened again from its file
            const uint32_t open = project()->findSceneByPath(headless().dir / SCENE_FILE);
            if (open != NULL_PROJECT_SCENE && project()->getScene(open) && project()->getScene(open)->opened) {
                if (spareScene() == NULL_PROJECT_SCENE) {
                    project()->createNewScene("Spare", SceneType::SCENE_3D);
                    spareScene() = project()->getSelectedSceneId();
                }
                project()->setSelectedSceneId(spareScene());
                std::string error;
                ASSERT_TRUE(project()->closeSceneNow(open, UnsavedChoice::Discard, &error)) << error;
            }
            project()->openScene(headless().dir / SCENE_FILE, false);
            sceneId = project()->findSceneByPath(headless().dir / SCENE_FILE);
            ASSERT_NE(scene(), nullptr) << "the Main Scene opened";
            project()->setSelectedSceneId(sceneId);
            // the models load on the scene's first frame (the Scene view's MeshSystem update)
            sceneFrame();
            ASSERT_FALSE(scene()->scene->getComponent<ModelComponent>(entity("Hero")).loadedFilename.empty()) << "Hero's model loaded";
            inspector.reset(new InspectorWindow(project()));
            setDriverInspector(inspector.get());
            window.reset(new AnimationWindow(project()));
            for (CommandDef& def : defaultCommands()) catalog.add(std::move(def));
            AnimationWindowClip::bindCommands(catalog, [this]() { return window ? window->getClipMode() : nullptr; });
            dispatcher = std::make_unique<ShortcutDispatcher>(catalog);
            frame();
        }

        void TearDown() override {
            ClipRecordHooks::endAllGestures();
            window.reset();
            setDriverInspector(nullptr);
            inspector.reset();
            AnimationClipController::release(project());
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previousContext);
            setEditorHost(nullptr);
        }

        static uint32_t& spareScene() {
            static uint32_t id = NULL_PROJECT_SCENE;
            return id;
        }

        SceneProject* scene() { return project()->getScene(sceneId); }
        CommandHistory* sceneHistory() { return CommandHandle::get(sceneId); }
        CommandHistory* projectHistory() { return project()->getProjectCommandHistory(); }
        bool modified() { return scene()->isModified; }

        Entity entity(const std::string& path) {
            const std::vector<Entity> found = EntityPath::resolve(*scene(), path);
            return found.size() == 1 ? found.front() : NULL_ENTITY;
        }
        Transform& transform(const std::string& path) {
            const Entity e = entity(path);
            EXPECT_NE(e, NULL_ENTITY) << path;
            return scene()->scene->getComponent<Transform>(e);
        }
        Vector3 position(const std::string& path) { return transform(path).position; }
        std::vector<float> rotation(const std::string& path) {
            const Quaternion q = transform(path).rotation;
            return {q.x, q.y, q.z, q.w};
        }
        size_t countNamed(const std::string& prefix) {
            size_t count = 0;
            auto transforms = scene()->scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                if (scene()->scene->getEntityName(transforms->getEntity(i)).rfind(prefix, 0) == 0) count++;
            }
            return count;
        }
        float morphWeight(const std::string& path, int index = 0) {
            return scene()->scene->getComponent<MeshComponent>(entity(path)).morphWeights[index];
        }

        // what the Scene view does with its scene every frame in edit mode: models and meshes made
        // (MeshSystem), then the world matrices (the transform system), parents first
        void sceneFrame() {
            Scene* s = scene()->scene;
            s->getSystem<MeshSystem>()->update(0);
            RenderSystem* render = s->getSystem<RenderSystem>().get();
            auto transforms = s->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& t = transforms->getComponentFromIndex(i);
                if (t.needUpdate) render->updateTransform(t, transforms->getEntity(i));
            }
        }

        // one editor frame: the clip controller's per-frame update (App::engineRender's hook), the
        // scene's frame, then the window drawn (focused when `focused`) in a headless ImGui frame
        void frame(bool focused = true, double dt = 1.0 / 60.0) {
            ImGui::SetCurrentContext(context);
            ImGui::GetIO().DeltaTime = (float)dt;
            if (animationClipAppHooks().frame) animationClipAppHooks().frame(project(), dt, ImGui::GetFrameCount());
            sceneFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(1400, 800));
            if (focused) ImGui::SetNextWindowFocus();
            window->show();
            if (!focused) {
                ImGui::SetNextWindowFocus();
                ImGui::Begin("Elsewhere");
                ImGui::End();
            }
            // the App's one dispatcher: the Animation context while the clip window takes keys
            ShortcutState keys;
            keys.wantTextInput = ImGui::GetIO().WantTextInput;
            keys.popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (animationClipAppHooks().takesKeys && animationClipAppHooks().takesKeys()) keys.panels.push_back(ShortcutContext::Animation);
            dispatcher->process(ImGuiShortcutKeys(), keys);
            ImGui::EndFrame();
        }
        void frames(int count, bool focused = true, double dt = 1.0 / 60.0) {
            for (int i = 0; i < count; i++) frame(focused, dt);
        }

        // the window's actions (AnimationWindowTestAccess::drive); the state right after them, then a few
        // editor frames (the window and the controller go on as between two user actions)
        Json act(const Json& actions) {
            Json out;
            std::string error;
            EXPECT_TRUE(AnimationWindowTestAccess::drive(*window, actions, out, error)) << actions.dump() << ": " << error;
            frames(3);
            return out;
        }
        Json state() { return act(Json::object()); }
        // an action the window refuses: its message
        std::string refused(const Json& actions) {
            Json out;
            std::string error;
            EXPECT_FALSE(AnimationWindowTestAccess::drive(*window, actions, out, error)) << actions.dump() << " was accepted";
            frames(3);
            return error;
        }
        // real key events for the next frame (ImGui), then the frame (the window focused)
        void key(ImGuiKey key, ImGuiKeyChord mods = 0) {
            ImGuiIO& io = ImGui::GetIO();
            if (mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, true);
            if (mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, true);
            io.AddKeyEvent(key, true);
            frame();
            io.AddKeyEvent(key, false);
            if (mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, false);
            if (mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, false);
            if (mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
        }

        bool save() { return project()->saveSceneToPath(sceneId, headless().dir / SCENE_FILE); }
        std::string savedScene() { return readAll(headless().dir / SCENE_FILE); }
        Json clipFile(const std::string& path) { return Json::parse(readAll(headless().dir / path)); }
        void writeClip(const std::string& path, const Json& clip) {
            writeAll(headless().dir / path, clip.dump(2) + "\n");
            AnimationClipDocument::checkDiskChanges(project());
        }

        bool undoScene() { return sceneHistory()->undo(); }
        bool redoScene() { return sceneHistory()->redo(); }
        bool undoProject() { return projectHistory()->undo(); }
        bool redoProject() { return projectHistory()->redo(); }

        // a scene step setting a Transform property (MCP set_properties' PropertyCmd)
        void setPosition(const std::string& path, const Vector3& value) {
            sceneHistory()->addCommandNoMerge(new PropertyCmd<Vector3>(project(), sceneId, entity(path), ComponentType::Transform, "position", value));
        }
        void setScale(const std::string& path, const Vector3& value) {
            sceneHistory()->addCommandNoMerge(new PropertyCmd<Vector3>(project(), sceneId, entity(path), ComponentType::Transform, "scale", value));
        }

        // the window on Hero with hero_clip.anim: a root position track keyed 0 (rest) and 1 (y + 2),
        // the spine's rotation keyed at 0 and 1 (30 degrees about x), as test_add_property_and_keys leaves it
        Json heroClip() {
            Json st = act({{"entity", "Hero"}, {"create_clip", CLIP}});
            st = act({{"add_property", {{"path", ""}, {"channel", "position"}}}});
            st = act({{"add_property", {{"path", SPINE}, {"channel", "rotation"}}}});
            st = act({{"frame", 30}, {"set_value", {{"track", 0}, {"component", 1}, {"value", 2.0}}}});
            st = act({{"key", "all"}});
            const int spine = trackOf(st, SPINE, "rotation");
            act({{"set_value", {{"track", spine}, {"component", 0}, {"value", 30.0}}}});
            st = act({{"key", "all"}});
            act({{"preview", false}});
            EXPECT_TRUE(save());
            return st;
        }
    };

}

// ---------------------------------------------------------------------------- empty state, Create

TEST_F(AnimationWindowClipTest, EmptyStateAndCreateClip) {
    ASSERT_TRUE(save());
    const Vector3 rest = position("Hero");
    const size_t heroes = countNamed("Hero");
    EXPECT_NE(refused({{"bogus", 1}}).find("unknown key"), std::string::npos);
    Json st = act({{"entity", "Hero"}});
    EXPECT_EQ(st["mode"], "clip") << "a GameObject without player or entity Animation: clip mode";
    EXPECT_TRUE(st["empty_state"].get<bool>());
    EXPECT_EQ(st["root"], "Hero") << "bound to the selection";
    EXPECT_NE(st.value("empty_text", std::string()).find("To begin animating Hero"), std::string::npos);
    EXPECT_TRUE(st["clip"].is_null() && !st["has_player"].get<bool>()) << st.dump();
    frames(3);
    EXPECT_FALSE(modified()) << "opening the window does not modify the scene";
    EXPECT_EQ(countNamed("Hero"), heroes);

    st = act({{"create_clip", CLIP}});
    EXPECT_EQ(st["clip"], CLIP) << "Create... opens the new clip";
    EXPECT_TRUE(st["has_player"].get<bool>()) << "an Animation Player was added";
    EXPECT_EQ(st["states"], Json::array({"hero_clip"}));
    EXPECT_EQ(st["state"], "hero_clip");
    EXPECT_TRUE(fs::is_regular_file(headless().dir / CLIP)) << "the file was written";
    EXPECT_FALSE(st["empty_state"].get<bool>());
    EXPECT_TRUE(modified()) << "Create... is a scene step (the player)";
    EXPECT_FALSE(st["undo_to_project"].get<bool>()) << "its undo is the scene's (the newest step is a scene step)";
    st = act({{"list_paths", true}});
    EXPECT_NE(std::find(st["paths"].begin(), st["paths"].end(), Json(SPINE)), st["paths"].end()) << "the Mixamo bones are offered by Add Property: " << st["paths"].dump().substr(0, 600);
    EXPECT_TRUE(close(position("Hero"), rest));

    // undone, the window shows its empty state again (the file and the player went)
    ASSERT_TRUE(undoScene());
    frames(2);
    st = state();
    EXPECT_FALSE(fs::exists(headless().dir / CLIP)) << "the file went with the undo";
    EXPECT_TRUE(st["clip"].is_null() && st["empty_state"].get<bool>()) << "the deleted clip is no longer shown: " << st.dump();
}

// ---------------------------------------------------------------------------- Add Property, values, keys

TEST_F(AnimationWindowClipTest, AddPropertyValuesAndKeys) {
    const Vector3 rest = position("Hero");
    act({{"entity", "Hero"}, {"create_clip", CLIP}});
    ASSERT_TRUE(save());
    Json st = act({{"add_property", {{"path", ""}, {"channel", "position"}}}});
    const Json* r = row(st, "", "position");
    ASSERT_TRUE(r) << st["rows"].dump();
    EXPECT_EQ((*r)["keys"], Json::array({0.0})) << "Add Property keys the current value at the playhead";
    EXPECT_TRUE(close((*r)["value"], {rest.x, rest.y, rest.z})) << (*r)["value"].dump();
    EXPECT_TRUE(st["undo_to_project"].get<bool>()) << "a clip edit: Ctrl+Z goes to the project history";
    EXPECT_FALSE(modified()) << "clip edits never modify the scene";
    st = act({{"add_property", {{"path", SPINE}, {"channel", "rotation"}}}});
    EXPECT_TRUE(row(st, SPINE, "rotation"));
    bool unfolded = false;
    for (const Json& x : st["rows"]) unfolded = unfolded || (x.contains("component") && x["component"] == 2);
    EXPECT_TRUE(unfolded) << "new rows are unfolded (x/y/z)";

    // R39: a name ending in "#<digits>" can't be addressed: refused, not listed
    const Entity bone = scene()->scene->createUserEntity();
    scene()->scene->setEntityName(bone, "Bone#1");
    scene()->scene->addComponent<Transform>(bone, {});
    scene()->scene->addEntityChild(entity("Hero"), bone, false);
    scene()->entities.push_back(bone);
    st = act({{"list_paths", true}});
    for (const Json& p : st["paths"]) EXPECT_EQ(p.get<std::string>().find("Bone#1"), std::string::npos) << "'Bone#1' is not offered";
    refused({{"add_property", {{"path", "Bone#1"}, {"channel", "position"}}}});
    scene()->scene->destroyEntity(bone);
    scene()->entities.erase(std::remove(scene()->entities.begin(), scene()->entities.end(), bone), scene()->entities.end());

    // the value column: an unkeyed (pending, pink) value, keyed by K
    st = act({{"frame", 30}, {"set_value", {{"track", 0}, {"component", 1}, {"value", 2.0}}}});
    EXPECT_TRUE(st["preview"].get<bool>()) << "editing a value previews it";
    EXPECT_TRUE((*row(st, "", "position"))["pending"].get<bool>()) << "the edit is pending";
    EXPECT_NEAR(position("Hero").y, 2.0f, 1e-3f) << "the Scene view shows the pending value";
    EXPECT_EQ((*row(st, "", "position"))["keys"], Json::array({0.0})) << "not keyed yet";
    st = act({{"key", "all"}});
    r = row(st, "", "position");
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 1.0f})) << "K keys it at the playhead";
    EXPECT_NEAR((*r)["value"][1].get<float>(), 2.0f, 1e-3f);
    EXPECT_FALSE((*r)["pending"].get<bool>()) << "keyed: no longer pending";
    // an Euler edit of the spine rotation keys the quaternion of (30, 0, 0)
    const int spine = trackOf(st, SPINE, "rotation");
    act({{"set_value", {{"track", spine}, {"component", 0}, {"value", 30.0}}}});
    st = act({{"key", "all"}});
    const float s = std::sin(15.0f * (float)M_PI / 180.0f), c = std::cos(15.0f * (float)M_PI / 180.0f);
    EXPECT_TRUE(sameRotation((*row(st, SPINE, "rotation"))["value"], {s, 0, 0, c})) << "30 degrees about x";
    EXPECT_FALSE(modified()) << "the preview does not modify the scene";
    act({{"preview", false}});
    EXPECT_TRUE(close(position("Hero"), rest)) << "stopping the preview restores Hero";
}

TEST_F(AnimationWindowClipTest, PreviewIsolationAndSave) {
    heroClip();
    const Vector3 rest = position("Hero");
    ASSERT_TRUE(save());
    const std::string saved = savedScene();
    Json st = act({{"time", 1.0}, {"preview", true}});
    EXPECT_TRUE(st["preview"].get<bool>());
    EXPECT_NEAR(position("Hero").y, 2.0f, 1e-3f) << "the preview shows the clip at 1 s";
    ASSERT_TRUE(save());
    EXPECT_EQ(savedScene(), saved) << "a save during the preview writes the unpreviewed scene";
    EXPECT_FALSE(modified());
    st = act({{"preview", false}});
    EXPECT_FALSE(st["preview"].get<bool>());
    EXPECT_TRUE(close(position("Hero"), rest)) << "stop restores the scene";
}

TEST_F(AnimationWindowClipTest, PlayPauseKeysOnTheGrid) {
    heroClip();
    act({{"time", 0.0}, {"play", true}});
    // 0.37 s of editor frames at an uneven rate
    for (int i = 0; i < 22; i++) frame(true, 1.0 / 59.3);
    Json st = act({{"play", false}});
    EXPECT_TRUE(onGrid(st["time"].get<float>(), st["frame_rate"].get<float>())) << "the paused playhead is on a frame: " << st["time"];
    st = act({{"key", "all"}});
    for (const Json& r : st["rows"]) {
        if (!r.contains("keys")) continue;
        for (const Json& t : r["keys"]) EXPECT_TRUE(onGrid(t.get<float>(), st["frame_rate"].get<float>())) << t << " in " << r["label"];
    }
    ASSERT_TRUE(undoProject());
    act({{"preview", false}});
}

// ---------------------------------------------------------------------------- keys: select, drag, copy, paste

TEST_F(AnimationWindowClipTest, SelectDragDeleteCopyPaste) {
    heroClip();
    Json st = act({{"add_key", {{"track", 0}, {"time", 0.5}}}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.5f, 1.0f})) << "Add Key keeps the curve (sampled value)";
    const float restY = clipFile(CLIP)["tracks"][0]["keys"][0][1][1].get<float>();
    st = act({{"time", 0.5}});
    EXPECT_NEAR((*row(st, "", "position"))["value"][1].get<float>(), (restY + 2.0f) / 2.0f, 1e-3f) << "the new key holds the linear midpoint";
    st = act({{"select", {{{"track", 0}, {"time", 0.5}}}}});
    ASSERT_EQ(st["selection"].size(), 1u);
    EXPECT_EQ(st["selection"][0]["track"], 0);
    EXPECT_NEAR(st["selection"][0]["time"].get<float>(), 0.5f, 1e-5f) << "click select";
    st = act({{"select", {{{"path", SPINE}, {"channel", "rotation"}, {"time", 0.0}}}}, {"add", true}});
    EXPECT_EQ(st["selection"].size(), 2u) << "Shift adds";
    // box by row and time: only the 0.5 key of row 1; the summary row (0) takes every track
    st = act({{"box", {{"rows", {1, 1}}, {"times", {0.4, 0.6}}}}});
    ASSERT_EQ(st["selection"].size(), 1u) << "box in time: " << st["selection"].dump();
    EXPECT_NEAR(st["selection"][0]["time"].get<float>(), 0.5f, 1e-4f);
    st = act({{"box", {{"rows", {0, 0}}, {"times", {0.0, 0.01}}}}});
    std::vector<std::string> paths;
    for (const Json& k : st["selection"]) paths.push_back(k["path"]);
    std::sort(paths.begin(), paths.end());
    EXPECT_EQ(paths, (std::vector<std::string>{"", SPINE})) << "the summary row boxes every track";
    // a drag of 0.21 s lands on frames (30 fps: the grabbed key at 0.7)
    st = act({{"select", {{{"track", 0}, {"time", 0.5}}}}, {"drag", 0.21}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f, 1.0f})) << "the key moved to frame 21, nothing lost";
    EXPECT_NEAR(st["selection"][0]["time"].get<float>(), 0.7f, 1e-4f) << "selection follows";
    ASSERT_TRUE(undoProject());
    EXPECT_EQ(keysOf(state(), "", "position"), (std::vector<float>{0.0f, 0.5f, 1.0f})) << "the drag is one undo step";
    ASSERT_TRUE(redoProject());
    // copy the 0.7 key: paste at 1.5 adds it (source kept); paste onto 1.0 replaces that key
    st = act({{"select", {{{"track", 0}, {"time", 0.7}}}}, {"copy", true}});
    EXPECT_EQ(st["clipboard"], 1) << "copied";
    st = act({{"time", 1.5}, {"paste", true}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f, 1.0f, 1.5f})) << "pasted at the playhead, the source kept";
    st = act({{"paste", 1.0}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f, 1.0f, 1.5f})) << "pasted onto 1.0: replaced, no new key";
    EXPECT_NE(st["notice"].get<std::string>().find("replacing 1"), std::string::npos) << st["notice"];
    // a copied key without a matching property is skipped with a notice
    const int spine = trackOf(st, SPINE, "rotation");
    act({{"select", {{{"track", 0}, {"time", 0.7}}, {{"track", spine}, {"time", 0.0}}}}, {"copy", true}});
    act({{"remove_track", spine}});
    st = act({{"paste", 2.0}});
    EXPECT_TRUE(st["notice"].get<std::string>().find("skipped") != std::string::npos && st["notice_warning"].get<bool>()) << st["notice"];
    // (the copied keys keep their spacing: the position key was 0.7 s after the spine key)
    const std::vector<float> pasted = keysOf(st, "", "position");
    EXPECT_NE(std::find(pasted.begin(), pasted.end(), 2.7f), pasted.end()) << "the matched key pasted";
    ASSERT_TRUE(undoProject());
    ASSERT_TRUE(undoProject());
    st = state();
    EXPECT_TRUE(row(st, SPINE, "rotation")) << "the spine track is back";
    // duplicate: one more key, the source unchanged
    st = act({{"select", {{{"track", 0}, {"time", 0.7}}}}, {"duplicate", true}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f, 0.7333f, 1.0f, 1.5f})) << "duplicated one frame later, source kept";
    // interpolation of the selection
    act({{"select", {{{"track", 0}, {"time", 0.0}}}}, {"ease", "step"}});
    const Json data = clipFile(CLIP);
    bool stepped = false;
    for (const Json& t : data["tracks"]) {
        if (t["path"] == "" && t["channel"] == "position") stepped = t["keys"][0].dump().find("step") != std::string::npos;
    }
    EXPECT_TRUE(stepped) << "the key's ease in the file";
    // delete
    st = act({{"select", {{{"track", 0}, {"time", 1.5}}}}, {"delete", true}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f, 0.7333f, 1.0f})) << "Delete removes the selected key";
    EXPECT_EQ(st["selection"], Json::array()) << "nothing selected after a delete";
    EXPECT_FALSE(modified()) << "key edits never modify the scene";
}

// The window's shortcuts while it is focused (the catalog's animation.* commands on a drawn frame; the App's key routing
// with real X key presses is test_animation_window.py)
TEST_F(AnimationWindowClipTest, ShortcutsWhileFocused) {
    const Vector3 rest = position("Hero");
    heroClip();
    act({{"time", 0.0}});
    key(ImGuiKey_Period);
    key(ImGuiKey_Period);
    Json st = state();
    EXPECT_EQ(st["frame"], 2) << ". steps one frame each";
    key(ImGuiKey_Comma);
    EXPECT_EQ(state()["frame"], 1) << ", steps back";
    key(ImGuiKey_Period, ImGuiMod_Alt);
    st = state();
    EXPECT_NEAR(st["time"].get<float>(), 1.0f, 1e-4f) << "Alt+. goes to the next key";
    key(ImGuiKey_Comma, ImGuiMod_Alt);
    EXPECT_NEAR(state()["time"].get<float>(), 0.0f, 1e-4f) << "Alt+, back to the previous key (0)";
    key(ImGuiKey_Space);
    frames(20);
    st = state();
    EXPECT_TRUE(st["playing"].get<bool>() && st["preview"].get<bool>()) << "Space plays the preview";
    key(ImGuiKey_Space);
    st = act({{"preview", false}});
    EXPECT_FALSE(st["playing"].get<bool>()) << "Space pauses";
    EXPECT_TRUE(close(position("Hero"), rest)) << "stopping the preview restores Hero";
    // K with the window not focused keys nothing
    const std::vector<float> before = keysOf(state(), "", "position");
    // another window takes the focus first, then K
    frames(2, false);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_K, true);
    frame(false);
    io.AddKeyEvent(ImGuiKey_K, false);
    frame(false);
    st = state();
    EXPECT_FALSE(st["takes_keys"].get<bool>()) << "another window focused: the window does not take keys";
    EXPECT_EQ(keysOf(st, "", "position"), before) << "K elsewhere keyed nothing";
    // and Delete / Backspace / Ctrl+D / F2 in the focused window never touch the scene selection
    frame();
    project()->replaceSelectedEntities(sceneId, {entity("Hero")});
    const size_t heroes = countNamed("Hero");
    act({{"select", Json::array()}});
    for (ImGuiKey k : {ImGuiKey_Delete, ImGuiKey_Backspace, ImGuiKey_F2}) key(k);
    key(ImGuiKey_D, ImGuiMod_Ctrl);
    EXPECT_EQ(countNamed("Hero"), heroes) << "Hero was neither deleted nor duplicated";
    EXPECT_FALSE(modified()) << "no scene step";
    EXPECT_EQ(keysOf(state(), "", "position"), before) << "nothing selected: no key deleted or duplicated";
}

// ---------------------------------------------------------------------------- bindings, events, samples

TEST_F(AnimationWindowClipTest, MissingBindingsAndRebind) {
    heroClip();
    const Json clip = {{"format", "jasperengine.anim"}, {"version", 1}, {"frame_rate", 30}, {"wrap", "loop"},
                       {"tracks", {{{"path", std::string(SPINE) + "e"}, {"channel", "rotation"},
                                    {"keys", {{0, {0, 0, 0, 1}}, {1, {0, 0.3826834, 0, 0.9238795}}}}},
                                   {{"path", ""}, {"channel", "position"}, {"keys", {{0, {0, 0, 0}}, {1, {0, 1, 0}}}}}}},
                       {"events", Json::array()}};
    writeClip("assets/anim/missing.anim", clip);
    Json st = act({{"clip", "assets/anim/missing.anim"}, {"root", "Hero"}});
    EXPECT_EQ(st["clip"], "assets/anim/missing.anim") << "opened on Hero";
    ASSERT_EQ(st["missing"].size(), 1u) << st["missing"].dump();
    EXPECT_NE(st["missing"][0].get<std::string>().find("(Missing!)"), std::string::npos);
    const Json* bad = nullptr;
    for (const Json& r : st["rows"]) if (r["missing"].get<bool>() && r.contains("path")) bad = &r;
    ASSERT_TRUE(bad);
    EXPECT_TRUE(bad->value("message", std::string()).find("Did you mean") != std::string::npos &&
                bad->value("message", std::string()).find(SPINE) != std::string::npos) << "nearest names: " << bad->dump();
    EXPECT_EQ(st["bindings"], Json({{"resolved", 1}, {"total", 2}, {"legacy_model_layout", false}})) << "1 of 2 resolve";
    const int badTrack = (*bad)["track"];
    st = act({{"rebind", {{"track", badTrack}, {"path", SPINE}}}});
    EXPECT_EQ(st["missing"], Json::array()) << "rebound";
    EXPECT_NE(st["notice"].get<std::string>().find("changes the clip for"), std::string::npos) << st["notice"];
    bool rebound = false;
    const Json rebind = clipFile("assets/anim/missing.anim");
    for (const Json& t : rebind["tracks"]) rebound = rebound || t["path"] == SPINE;
    EXPECT_TRUE(rebound) << "the file has the new path";
    st = act({{"remove_track", 0}});
    size_t tracks = 0;
    for (const Json& r : st["rows"]) tracks += r.contains("path") ? 1 : 0;
    EXPECT_EQ(tracks, 1u) << "one row left";
    ASSERT_TRUE(undoProject());
    tracks = 0;
    st = state();
    for (const Json& r : st["rows"]) tracks += r.contains("path") ? 1 : 0;
    EXPECT_EQ(tracks, 2u) << "undo of Remove (project history)";
}

TEST_F(AnimationWindowClipTest, EventsLaneWithAScript) {
    heroClip();
    writeAll(headless().dir / "scripts" / "HeroEvents.lua",
             "local HeroEvents = {}\n\nfunction HeroEvents:init()\nend\n\nfunction HeroEvents:onStep(side)\nend\n\nreturn HeroEvents\n");
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.path = "scripts/HeroEvents.lua";
    entry.className = "HeroEvents";
    entry.enabled = true;
    scripts.scripts.push_back(entry);
    scene()->scene->addComponent<ScriptComponent>(entity("Hero"), scripts);
    Json st = act({{"state", "hero_clip"}});
    EXPECT_EQ(st["clip"], CLIP) << "back on the player's clip";
    // (the window reads the root's scripts again at most once a second)
    frames(6, true, 0.25);
    st = act({{"add_event", {{"time", 0.5}, {"function", "onStep"}, {"args", {1, "left", true}}}}});
    EXPECT_NE(std::find(st["script_functions"].begin(), st["script_functions"].end(), Json("onStep")), st["script_functions"].end())
        << "the root's script methods: " << st["script_functions"].dump();
    ASSERT_EQ(st["events"].size(), 1u);
    const Json ev = st["events"][0];
    EXPECT_EQ(ev["time"], 0.5);
    EXPECT_EQ(ev["function"], "onStep");
    EXPECT_EQ(ev["args"], Json::array({1, "left", true}));
    EXPECT_EQ(ev["handler"], "found") << "the Lua script on Hero defines onStep";
    EXPECT_EQ(st["selected_event"], 0) << "the new event is selected (its editor opens)";
    st = act({{"event", {{"function", "onMissing"}, {"args", {2}}}}});
    EXPECT_EQ(st["events"][0]["function"], "onMissing");
    EXPECT_EQ(st["events"][0]["args"], Json::array({2})) << "edited in the event editor";
    EXPECT_EQ(st["events"][0]["handler"], "missing") << "no script defines onMissing (WarningText)";
    EXPECT_NE(refused({{"event", {{"function", "not an id"}}}}).find("identifier"), std::string::npos);
    EXPECT_NE(refused({{"event", {{"function", "end"}}}}).find("keyword"), std::string::npos);
    Json nine = Json::array();
    for (int i = 0; i < 9; i++) nine.push_back(i);
    EXPECT_NE(refused({{"event", {{"args", nine}}}}).find("at most"), std::string::npos);
    st = act({{"drag_event", 0.21}});
    EXPECT_NEAR(st["events"][0]["time"].get<float>(), 0.7f, 1e-4f) << "dragged to a frame";
    ASSERT_TRUE(undoProject());
    EXPECT_NEAR(state()["events"][0]["time"].get<float>(), 0.5f, 1e-4f) << "the drag is one undo step";
    // I4: the selection is the event itself, never an index: an event added before it and undone
    st = act({{"add_event", {{"time", 0.1}, {"function", "onStep"}}}});
    EXPECT_EQ(st["selected_event"], 0);
    EXPECT_EQ(st["events"].size(), 2u) << "B at 0.1 selected (index 0)";
    ASSERT_TRUE(undoProject());
    st = state();
    EXPECT_EQ(st["selected_event"], -1);
    EXPECT_EQ(st["events"].size(), 1u) << "B undone: nothing selected, A kept";
    EXPECT_NE(refused({{"delete_event", true}}).find("no event selected"), std::string::npos);
    EXPECT_EQ(state()["events"].size(), 1u) << "Delete with nothing selected deleted nothing";
    st = act({{"select_event", 0}, {"delete_event", true}});
    EXPECT_EQ(st["events"], Json::array()) << "deleted";
    // the event editor draws for the selected event (one frame with it open)
    act({{"add_event", {{"time", 0.2}, {"function", "onStep"}}}});
    frames(2);
    EXPECT_FALSE(modified()) << "events are clip edits";
}

// ---------------------------------------------------------------------------- the Audio lane (S40 R9-R11)

TEST_F(AnimationWindowClipTest, AudioLaneDropAlignAndPreview) {
    heroClip();
    // a 0.5 s tick, loudest at 0.2 s (48 kHz mono)
    std::vector<float> samples(24000, 0.0f);
    for (int i = 0; i < 480; i++) samples[9600 + i] = (i % 2 ? -0.9f : 0.9f) * (1.0f - (float)i / 480.0f);
    const fs::path tickFile = headless().dir / "assets" / "sounds" / "tick.wav";
    fs::create_directories(tickFile.parent_path());
    writeAll(tickFile, WavWriter::pcm16(samples, 1, 48000));
    const std::string tick = project()->normalizeToAssetsRelative(tickFile).generic_string();
    Json st = act({{"state", "hero_clip"}});
    EXPECT_EQ(st["audio_rows"], Json::array()) << "no lane without cues";

    // a sound dropped on the lane: one cue at the drop time, one undo step
    st = act({{"drop_sounds", {{"paths", {tick}}, {"time", 0.3}}}});
    ASSERT_EQ(st["audio"].size(), 1u) << st["audio"].dump();
    EXPECT_EQ(st["audio"][0]["kind"], "sound");
    EXPECT_EQ(st["audio"][0]["sound"], tick);
    EXPECT_NEAR(st["audio"][0]["time"].get<float>(), 0.3f, 1e-4f);
    EXPECT_EQ(st["audio_rows"], Json::array({"sound"}));
    EXPECT_EQ(st["selected_cue"], 0) << "the dropped cue is selected";
    ASSERT_TRUE(undoProject());
    EXPECT_EQ(state()["audio"].size(), 0u) << "the drop is one undo step";
    ASSERT_TRUE(redoProject());

    // S36's analysis gives the block its length and peak (on the worker)
    for (;;) {
        const uint64_t generation = host.taskGeneration();
        if (!(state()["audio"][0]["length"].get<double>() <= 0.0)) break;
        if (!TimelineSounds::get(project(), tick).pending) break;
        host.waitForTaskAfter(generation);
        frames(1);
    }
    st = state();
    ASSERT_NEAR(st["audio"][0]["length"].get<double>(), 0.5, 0.002) << "the tick's length";

    // R10: Align Peak puts the analysed peak on the playhead; Start and End as their names say
    st = act({{"time", 1.0}, {"preview", false}, {"select_cue", 0}, {"align_cue", "peak"}});
    EXPECT_NEAR(st["audio"][0]["time"].get<float>(), 0.8f, 0.002f) << "peak at 0.2 s of the tick on 1.0 s";
    std::shared_ptr<AnimationClipDocument> doc = AnimationClipDocument::open(project(), CLIP, nullptr);
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->data().audio.size(), 1u);
    EXPECT_NEAR(doc->data().audio[0].time, 0.8f, 0.002f) << "saved with the clip";
    st = act({{"align_cue", "end"}});
    EXPECT_NEAR(st["audio"][0]["time"].get<float>(), 0.5f, 0.002f);
    st = act({{"command", "animation.alignCueStart"}});
    EXPECT_NEAR(st["audio"][0]["time"].get<float>(), 1.0f, 0.002f) << "the catalog command acts on the selected cue";
    ASSERT_TRUE(undoProject());
    EXPECT_NEAR(state()["audio"][0]["time"].get<float>(), 0.5f, 0.002f) << "each alignment is one undo step";
    EXPECT_EQ(state()["selected_cue"], -1) << "the selection is the cue itself (at 1.0 s): the undo left none";
    act({{"time", 0.1}, {"preview", false}, {"select_cue", 0}});
    EXPECT_NE(refused({{"align_cue", "peak"}}).find("before 0"), std::string::npos) << "a peak before the clip's start is refused";

    // drag snapped to the frames, one undo step; a snapshot cue gets its own row
    st = act({{"drag_cue", 0.21}});
    EXPECT_NEAR(st["audio"][0]["time"].get<float>(), 0.7f, 1e-4f);
    ASSERT_TRUE(undoProject());
    EXPECT_NEAR(state()["audio"][0]["time"].get<float>(), 0.5f, 0.002f);
    st = act({{"add_cue", {{"kind", "snapshot"}, {"snapshot", "Build"}, {"time", 1.0}}}});
    EXPECT_EQ(st["audio_rows"], Json::array({"sound", "snapshot"}));
    EXPECT_EQ(st["audio"][1]["kind"], "snapshot");
    st = act({{"delete_cue", true}});
    EXPECT_EQ(st["audio"].size(), 1u) << "Delete removes the selected cue";
    EXPECT_NE(refused({{"add_cue", {{"kind", "sound"}}}}).find("needs a sound"), std::string::npos);

    // R11: the playing preview starts a preview voice per crossed cue; none while Play captures
    st = act({{"select_cue", 0}, {"align_cue", "start"}});
    act({{"time", 0.0}, {"play", true}});
    for (int i = 0; i < 50; i++) frame(true, 1.0 / 60.0);
    st = act({{"play", false}});
    EXPECT_GE(st["preview_audio_starts"].get<int>(), 1) << "the cue at 0.1 s played in the preview";
    const int before = st["preview_audio_starts"].get<int>();
    ASSERT_TRUE(AudioSystem::beginDeterministic());
    act({{"time", 0.0}, {"play", true}});
    for (int i = 0; i < 50; i++) frame(true, 1.0 / 60.0);
    st = act({{"play", false}});
    AudioSystem::endDeterministic();
    EXPECT_EQ(st["preview_audio_starts"].get<int>(), before) << "no preview voice while the driver is held (a capture)";
    act({{"preview", false}});
    EXPECT_FALSE(modified()) << "cues are clip edits";
}

TEST_F(AnimationWindowClipTest, SamplesAndSnap) {
    heroClip();
    const Json clip = {{"format", "jasperengine.anim"}, {"version", 1}, {"frame_rate", 24}, {"wrap", "once"},
                       {"tracks", {{{"path", ""}, {"channel", "position"},
                                    {"keys", {{0, {0, 0, 0}}, {0.25, {0, 0.25, 0}}, {0.26, {0, 0.26, 0}}, {1, {0, 1, 0}}}}}}},
                       {"events", Json::array()}};
    writeClip("assets/anim/snap.anim", clip);
    act({{"clip", "assets/anim/snap.anim"}, {"root", "Hero"}});
    Json st = act({{"snap_keys", true}});
    EXPECT_TRUE(st["notice_warning"].get<bool>() && st["notice"].get<std::string>().find("merged") != std::string::npos) << st["notice"];
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.25f, 1.0f})) << "one key at 6/24 s";
    st = act({{"time", 0.25}});
    EXPECT_NEAR((*row(st, "", "position"))["value"][1].get<float>(), 0.25f, 1e-4f) << "the nearer key (0.25) survived";
    EXPECT_NE(refused({{"frame_rate", 500}}).find("1 to 240"), std::string::npos);
    st = act({{"frame_rate", 30}});
    EXPECT_EQ(st["frame_rate"], 30) << "Samples";
    EXPECT_NE(st["notice"].get<std::string>().find("between frames"), std::string::npos) << st["notice"];
    st = act({{"time_format", "seconds"}});
    EXPECT_EQ(st["time_format"], "seconds");
    frames(2);
    st = act({{"time_format", "frames"}, {"state", "hero_clip"}});
    EXPECT_EQ(st["clip"], CLIP) << "back to hero_clip";
}

// I1 / I3: a 60 x 600 key clip, every key selected: the dopesheet and the curves view draw in a few
// ms, a drag of every key is one step
TEST_F(AnimationWindowClipTest, LargeClipStaysResponsive) {
    heroClip();
    std::vector<std::string> paths;
    const Json listed = act({{"list_paths", true}});
    for (const Json& p : listed["paths"]) {
        if (p.get<std::string>().rfind("Hero/", 0) == 0 && paths.size() < 20) paths.push_back(p);
    }
    ASSERT_EQ(paths.size(), 20u);
    Json tracks = Json::array();
    for (size_t i = 0; i < paths.size(); i++) {
        for (const char* channel : {"position", "rotation", "scale"}) {
            Json keys = Json::array();
            for (int k = 0; k < 600; k++) {
                const float t = std::round(k / 60.0f * 1e6f) / 1e6f;
                const float wave = std::sin(k / 20.0f + (float)i);
                if (std::string(channel) == "rotation") {
                    const std::vector<float> q = eulerXYZW(30 * wave, 10 * wave, 0);
                    keys.push_back({t, {q[0], q[1], q[2], q[3]}});
                } else if (std::string(channel) == "scale") {
                    keys.push_back({t, {1 + 0.1f * wave, 1, 1}});
                } else {
                    keys.push_back({t, {wave, 0, 0}});
                }
            }
            tracks.push_back({{"path", paths[i]}, {"channel", channel}, {"keys", keys}});
        }
    }
    writeClip("assets/anim/big.anim", {{"format", "jasperengine.anim"}, {"version", 1}, {"frame_rate", 60}, {"wrap", "loop"},
                                      {"tracks", tracks}, {"events", Json::array()}});
    act({{"clip", "assets/anim/big.anim"}, {"root", "Hero"}, {"brief", true}});
    Json st = act({{"box", {{"rows", {0, 0}}, {"times", {0, 11}}}}, {"brief", true}});
    EXPECT_EQ(st["selection_count"], 36000) << "every key selected";
    frames(30);
    st = act({{"brief", true}});
    EXPECT_LT(st["draw_ms"].get<double>(), DRAW_BUDGET_MS) << "the clip mode draws in " << st["draw_ms"] << " ms (moving average)";
    // a drag of every key by one frame is one undo step
    act({{"drag", 1.0 / 60.0}, {"brief", true}});
    EXPECT_NEAR(clipFile("assets/anim/big.anim")["tracks"][0]["keys"][0][0].get<float>(), 1.0f / 60.0f, 1e-4f) << "moved by a frame";
    ASSERT_TRUE(undoProject());
    EXPECT_NEAR(clipFile("assets/anim/big.anim")["tracks"][0]["keys"][0][0].get<float>(), 0.0f, 1e-6f) << "one undo step";
    // the curves view of the same clip (I3: nothing selected, every curve framed)
    act({{"select", Json::array()}, {"view", "curves"}, {"frame_curves", "all"}, {"brief", true}});
    frames(30);
    st = act({{"brief", true}});
    EXPECT_EQ(st["view"], "curves");
    EXPECT_LT(st["draw_ms"].get<double>(), DRAW_BUDGET_MS) << "the curves view draws in " << st["draw_ms"] << " ms (moving average)";
    act({{"view", "dopesheet"}, {"state", "hero_clip"}});
}

// ---------------------------------------------------------------------------- sub-clips, extract, drop

TEST_F(AnimationWindowClipTest, ReadOnlySubClipAndExtract) {
    Json st = act({{"entity", nullptr}, {"clip", "assets/models/hero_animated.gltf#Walk"}});
    EXPECT_EQ(st["mode"], "clip") << "the Project window's double-click opens clip mode";
    EXPECT_TRUE(st["read_only"].get<bool>() && st["targetless"].get<bool>()) << "a read-only target-less sub-clip";
    EXPECT_GT(st["rows"].size(), 0u) << "its tracks show";
    EXPECT_NE(st["preview_on"].get<std::string>().find("hero_animated.gltf"), std::string::npos) << "it previews on its own model";
    EXPECT_NE(st["preview_blocker"].get<std::string>().find("No instance of hero_animated.gltf"), std::string::npos) << st["preview_blocker"];
    EXPECT_NE(refused({{"add_event", {{"time", 0.1}}}}).find("Extract"), std::string::npos);
    // Preview on the Hero (same rig): a session override, nothing written
    st = act({{"preview_model", "models/hero_tpose.gltf"}});
    EXPECT_EQ(st["preview_on"], "models/hero_tpose.gltf");
    EXPECT_EQ(st["preview_blocker"], "") << "previewable on Hero";
    st = act({{"time", 0.3}, {"preview", true}});
    EXPECT_TRUE(st["preview"].get<bool>() && st["preview_root"] == "Hero") << "the read-only clip previews on Hero: " << st.dump().substr(0, 400);
    frames(2);
    st = act({{"preview", false}});
    st = act({{"extract", "assets/anim/walk.anim"}});
    EXPECT_EQ(st["clip"], "assets/anim/walk.anim") << "Extract opens the new file";
    EXPECT_FALSE(st["read_only"].get<bool>()) << "editable";
    EXPECT_TRUE(fs::is_regular_file(headless().dir / "assets/anim/walk.anim")) << "written";
    EXPECT_NE(st["preview_model"].get<std::string>().find("hero_animated.gltf"), std::string::npos) << "Preview on the model";
    st = act({{"preview_model", "models/hero_tpose.gltf"}});
    EXPECT_EQ(st["preview_model"], "models/hero_tpose.gltf") << "Preview on (written into the editable clip)";
    EXPECT_FALSE(modified()) << "no scene change";
}

TEST_F(AnimationWindowClipTest, ExtractThroughThePlayersStateAndDrops) {
    // Walker: a cube whose Animation Player plays the model's Walk sub-clip
    const Entity walker = scene()->scene->createUserEntity();
    scene()->scene->setEntityName(walker, "Walker");
    scene()->scene->addComponent<Transform>(walker, {});
    scene()->entities.push_back(walker);
    AnimationPlayerComponent player;
    AnimationPlayerClip walk;
    walk.clip = "models/hero_animated.gltf#Walk";
    player.clips = {walk};
    scene()->scene->addComponent<AnimationPlayerComponent>(walker, player);
    Json st = act({{"entity", "Walker"}});
    EXPECT_TRUE(st["read_only"].get<bool>() && st["state"] == "Walk") << "the player's sub-clip state: " << st.dump().substr(0, 300);
    st = act({{"extract", "assets/anim/walker_walk.anim"}});
    EXPECT_EQ(st["clip"], "assets/anim/walker_walk.anim");
    EXPECT_EQ(st["state"], "Walk");
    EXPECT_FALSE(st["read_only"].get<bool>()) << "switched";
    EXPECT_EQ(scene()->scene->getComponent<AnimationPlayerComponent>(walker).clips[0].clip, "anim/walker_walk.anim") << "the player plays the file";
    ASSERT_TRUE(undoScene());
    EXPECT_EQ(scene()->scene->getComponent<AnimationPlayerComponent>(walker).clips[0].clip, "models/hero_animated.gltf#Walk")
        << "undo puts the sub-clip back";

    // Spec 4.1: a .anim dropped on the window opens it for the bound GameObject: added to its
    // Animation Player (one scene step; a player is added when it has none, a listed clip is
    // reused), or target-less when nothing is bound; other files are refused
    act({{"entity", nullptr}, {"clip", "assets/models/hero_animated.gltf#Walk"}});
    act({{"extract", "assets/anim/walk.anim"}});
    const Entity dropper = scene()->scene->createUserEntity();
    scene()->scene->setEntityName(dropper, "Dropper");
    scene()->scene->addComponent<Transform>(dropper, {});
    scene()->entities.push_back(dropper);
    scene()->isModified = false;
    st = act({{"entity", "Dropper"}});
    EXPECT_TRUE(st["empty_state"].get<bool>()) << "the empty state of Dropper";
    st = act({{"drop", {"assets/anim/walk.anim"}}});
    EXPECT_EQ(st["mode"], "clip");
    EXPECT_EQ(st["root"], "Dropper");
    EXPECT_EQ(st["clip"], "assets/anim/walk.anim");
    EXPECT_EQ(st["state"], "walk") << "the drop opens the clip as the new player's state";
    EXPECT_TRUE(st["has_player"].get<bool>() && st["states"] == Json::array({"walk"})) << "a player was added listing it";
    EXPECT_TRUE(modified()) << "a scene step";
    ASSERT_TRUE(undoScene());
    EXPECT_FALSE(scene()->scene->findComponent<AnimationPlayerComponent>(dropper)) << "one undo step: the player is gone";
    // a player with clips: appended (a state of its own), and the same file again reuses its entry
    st = act({{"entity", "Walker"}, {"drop", {(headless().dir / "assets/anim/walk.anim").string()}}});
    EXPECT_EQ(st["root"], "Walker");
    EXPECT_EQ(st["state"], "walk") << "opened as Walker's new state (an absolute path)";
    EXPECT_EQ(st["states"], Json::array({"Walk", "walk"})) << "appended after the player's own clip";
    st = act({{"drop", {"assets/anim/walk.anim"}}});
    EXPECT_EQ(st["states"], Json::array({"Walk", "walk"}));
    EXPECT_EQ(st["state"], "walk") << "a listed clip is reused";
    ASSERT_TRUE(undoScene());
    EXPECT_EQ(act({{"entity", "Walker"}})["states"], Json::array({"Walk"})) << "the second drop made no step of its own";
    // nothing bound (a clip open target-less): target-less too (the file is edited, nothing is added)
    st = act({{"entity", nullptr}, {"clip", "assets/models/hero_animated.gltf#Walk"}});
    EXPECT_TRUE(st["targetless"].get<bool>());
    st = act({{"drop", {"assets/anim/walk.anim"}}});
    EXPECT_TRUE(st["targetless"].get<bool>() && st["clip"] == "assets/anim/walk.anim") << "target-less";
    EXPECT_NE(refused({{"drop", {SCENE_FILE}}}).find(".anim"), std::string::npos);
}

// ---------------------------------------------------------------------------- Record

TEST_F(AnimationWindowClipTest, RecordSources) {
    const Vector3 restHero = position("Hero");
    const Vector3 restRobot = position("Robot");
    const std::string spineEntity = std::string("Hero/") + SPINE;
    act({{"entity", "Hero"}, {"create_clip", "assets/anim/rec.anim"}});
    // (the player reloaded the model in its node hierarchy: the bone's path has the glTF root node)
    const std::vector<float> restSpine = rotation(spineEntity);
    Json st = act({{"add_property", {{"path", ""}, {"channel", "position"}}}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f})) << "a root position track with a key at 0";
    EXPECT_FALSE(st["recording"].get<bool>()) << "Record is off";
    ASSERT_TRUE(save());

    // a gizmo move of the root keys it at the playhead, no scene step, one undo step
    st = act({{"frame", 15}, {"record", true}});
    EXPECT_TRUE(st["recording"].get<bool>() && st["preview"].get<bool>()) << "Record on previews";
    EXPECT_NE(st["record_status"].get<std::string>().find("Recording"), std::string::npos) << st["record_status"];
    const Vector3 target(restHero.x, restHero.y + 1.0f, restHero.z);
    st = act({{"gizmo", {{"entities", {"Hero"}}, {"tool", "move"}, {"value", {target.x, target.y, target.z}}}}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.5f})) << "the gizmo drag keyed the root at the playhead";
    EXPECT_TRUE(close((*row(st, "", "position"))["value"], {target.x, target.y, target.z})) << "the key holds the dragged value";
    EXPECT_TRUE(close(position("Hero"), target)) << "the live value shows";
    EXPECT_FALSE(modified()) << "no scene step";
    EXPECT_TRUE(st["undo_to_project"].get<bool>()) << "a recorded key is the newest step";
    ASSERT_TRUE(undoProject());
    EXPECT_EQ(keysOf(state(), "", "position"), (std::vector<float>{0.0f})) << "one undo step removed the drag's key";
    ASSERT_TRUE(redoProject());
    EXPECT_EQ(keysOf(state(), "", "position"), (std::vector<float>{0.0f, 0.5f})) << "redo";

    // the rotate gizmo adds the bone's rotation track; an Inspector Rotation row edit re-keys it
    st = act({{"gizmo", {{"entities", {spineEntity}}, {"tool", "rotate"}, {"value", {0, 0, 40}}}}});
    ASSERT_TRUE(row(st, SPINE, "rotation")) << "the rotate gizmo added the bone's rotation track";
    EXPECT_EQ(keysOf(st, SPINE, "rotation"), (std::vector<float>{0.5f})) << "keyed at the playhead";
    EXPECT_TRUE(sameRotation((*row(st, SPINE, "rotation"))["value"], eulerXYZW(0, 0, 40))) << "40 degrees about z";
    st = act({{"inspector", {{"entities", {spineEntity}}, {"property", "rotation"}, {"value", {20, 0, 0}}}}});
    EXPECT_TRUE(sameRotation((*row(st, SPINE, "rotation"))["value"], eulerXYZW(20, 0, 0))) << "the Inspector row";
    EXPECT_TRUE(sameRotation(Json(rotation(spineEntity)), eulerXYZW(20, 0, 0))) << "the live bone";
    const Vector3 raised(restHero.x, restHero.y + 1.5f, restHero.z);
    st = act({{"inspector", {{"entities", {"Hero"}}, {"property", "position"}, {"value", {raised.x, raised.y, raised.z}}}}});
    EXPECT_TRUE(close((*row(st, "", "position"))["value"], {raised.x, raised.y, raised.z})) << "the root key took the row value";
    EXPECT_FALSE(modified()) << "Inspector edits under Record are not scene steps";
    ASSERT_TRUE(undoProject());
    st = state();
    EXPECT_TRUE(close((*row(st, "", "position"))["value"], {target.x, target.y, target.z})) << "the position edit undone";
    EXPECT_TRUE(sameRotation((*row(st, SPINE, "rotation"))["value"], eulerXYZW(20, 0, 0))) << "the rotation edit stays";

    // a multi-selection with a GameObject outside the bound hierarchy: only Hero keyed
    const Vector3 both(restHero.x + 0.5f, restHero.y + 1.0f, restHero.z);
    st = act({{"gizmo", {{"entities", {"Hero", "Robot"}}, {"tool", "move"}, {"value", {both.x, both.y, both.z}}}}});
    EXPECT_TRUE(close((*row(st, "", "position"))["value"], {both.x, both.y, both.z})) << "Hero keyed";
    EXPECT_TRUE(close(position("Robot"), restRobot)) << "Robot, not under Hero, stays as it is";
    EXPECT_NE(st["record_status"].get<std::string>().find("not under Hero"), std::string::npos) << st["record_status"];
    EXPECT_TRUE(st["record_status_short"].get<std::string>().find("Rec frame 15") != std::string::npos &&
                st["record_status_short"].get<std::string>().find("1 not keyed") != std::string::npos) << st["record_status_short"];
    EXPECT_FALSE(modified()) << "no scene step";
    // Robot alone selected: the window keeps its binding while recording (Unity's lock); Record
    // does not claim Robot, the drag is a scene step
    const Vector3 moved(restRobot.x + 0.3f, restRobot.y, restRobot.z);
    act({{"gizmo", {{"entities", {"Robot"}}, {"tool", "move"}, {"value", {moved.x, moved.y, moved.z}}}}});
    frames(4);
    st = state();
    EXPECT_TRUE(st["recording"].get<bool>() && st["root"] == "Hero") << "still recording Hero with Robot selected";
    EXPECT_TRUE(close(position("Robot"), moved)) << "Robot moved";
    EXPECT_TRUE(modified()) << "a scene step";
    EXPECT_FALSE(st["undo_to_project"].get<bool>()) << "the scene step is the newest: Ctrl+Z goes to the scene";
    ASSERT_TRUE(undoScene());
    EXPECT_TRUE(close(position("Robot"), restRobot)) << "undone";
    // MCP set_properties is never recorded: a scene step of Hero's rest (the snapshot follows it)
    const std::vector<float> keys = keysOf(state(), "", "position");
    const Json value = (*row(state(), "", "position"))["value"];
    setScale("Hero", Vector3(1.2f, 1.2f, 1.2f));
    st = state();
    EXPECT_EQ(keysOf(st, "", "position"), keys) << "no key added";
    EXPECT_EQ((*row(st, "", "position"))["value"], value) << "the clip unchanged";
    EXPECT_FALSE(row(st, "", "scale")) << "no scale track";
    EXPECT_TRUE(modified()) << "set_properties is a scene step";
    ASSERT_TRUE(undoScene());
    act({{"entity", "Hero"}});

    // blend shapes: the Blend Shapes row of a mesh under Hero keys a morph track
    // (create_entity {type: model, model, parent}: a model GameObject under Hero, then its file loaded)
    CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, "Jelly", EntityCreationType::MODEL, entity("Hero"));
    create->setQuiet(true);
    sceneHistory()->addCommandNoMerge(create);
    const Entity jelly = create->getEntity();
    sceneHistory()->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, jelly, "models/skinned_morph.gltf"));
    ASSERT_FALSE(scene()->scene->getComponent<ModelComponent>(jelly).loadedFilename.empty()) << "the morph model loaded";
    frames(2);
    // what the render system's mesh load derives from the MORPHTARGET0 attribute (no GPU here): the
    // submeshes of a mesh with blend shapes have morph targets (the Inspector shows its rows for those)
    {
        auto meshes = scene()->scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++) {
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.morphTargetCount <= 0) continue;
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++) mesh.submeshes[s].hasMorphTarget = true;
        }
    }
    std::string hit;
    std::string tried;
    const Json listed = act({{"list_paths", true}});
    for (const Json& p : listed["paths"]) {
        if (p.get<std::string>().rfind("Jelly", 0) != 0 || !hit.empty()) continue;
        Json out;
        std::string error;
        if (AnimationWindowTestAccess::drive(*window, {{"morph", {{"entities", {"Hero/" + p.get<std::string>()}}, {"index", 0}, {"value", 0.8}}}}, out, error)) {
            hit = p;
            st = out;
        }
        tried += p.get<std::string>() + ": " + error + "; ";
    }
    ASSERT_FALSE(hit.empty()) << "a mesh with blend shapes under Jelly: " << tried;
    ASSERT_TRUE(row(st, hit, "morph")) << "the Blend Shapes row keyed a morph track";
    EXPECT_EQ(keysOf(st, hit, "morph"), (std::vector<float>{0.5f})) << "at the playhead";
    EXPECT_NEAR((*row(st, hit, "morph"))["value"][0].get<float>(), 0.8f, 1e-4f);
    // Record off by a save (it restores the preview, 9.R6); then previewing with Record off, the same
    // row is a scene step (one per drag) that changes the weight's rest only
    scene()->isModified = false;
    ASSERT_TRUE(save());
    st = act({{"record", false}});
    EXPECT_FALSE(st["recording"].get<bool>() || st["preview"].get<bool>()) << "the save ended Record and the preview";
    st = act({{"frame", 15}, {"preview", true}});
    EXPECT_TRUE(st["preview"].get<bool>() && !st["recording"].get<bool>()) << "previewing, Record off";
    st = act({{"morph", {{"entities", {"Hero/" + hit}}, {"index", 0}, {"value", 0.3}}}});
    EXPECT_TRUE(modified()) << "an unrecorded Blend Shapes edit is a scene step";
    EXPECT_NEAR((*row(st, hit, "morph"))["value"][0].get<float>(), 0.8f, 1e-4f) << "the clip is unchanged";
    act({{"preview", false}});
    EXPECT_NEAR(morphWeight("Hero/" + hit), 0.3f, 1e-4f) << "after the preview the scene has the edited weight, not the previewed one";
    ASSERT_TRUE(undoScene());
    EXPECT_NEAR(morphWeight("Hero/" + hit), 0.0f, 1e-4f) << "one undo step for the slider drag";

    // a lattice drag of a cage under Hero keys a lattice track
    const Entity cage = scene()->scene->createUserEntity();
    scene()->scene->setEntityName(cage, "Cage");
    scene()->scene->addComponent<Transform>(cage, {});
    scene()->scene->addComponent<LatticeComponent>(cage, {});
    scene()->scene->addEntityChild(entity("Hero"), cage, false);
    scene()->entities.push_back(cage);
    scene()->isModified = false;
    st = act({{"frame", 15}, {"record", true}});
    EXPECT_TRUE(st["recording"].get<bool>());
    st = act({{"lattice_drag", {{"entity", "Hero/Cage"}, {"points", {13}}, {"offsets", {{0, 0.3, 0}}}}}});
    ASSERT_TRUE(row(st, "Cage", "lattice")) << "the lattice drag keyed a lattice track";
    EXPECT_EQ(keysOf(st, "Cage", "lattice"), (std::vector<float>{0.5f})) << "at the playhead";
    EXPECT_TRUE(close((*row(st, "Cage", "lattice"))["value"], {0, 0.3f, 0}, 1e-4f)) << "point 13's offset";
    EXPECT_FALSE(modified()) << "no scene step";
    EXPECT_TRUE(close(scene()->scene->getComponent<LatticeComponent>(cage).offsets[13], Vector3(0, 0.3f, 0), 1e-4f)) << "the live point";

    // R leaves Record (started with the preview on: the preview stays, Unity), the preview's stop
    // restores everything; R starts Record again (and the preview) and stops it with the preview
    key(ImGuiKey_R);
    st = state();
    EXPECT_TRUE(!st["recording"].get<bool>() && st["preview"].get<bool>()) << "R left Record, the preview stays";
    st = act({{"preview", false}});
    EXPECT_FALSE(st["preview"].get<bool>());
    EXPECT_TRUE(close(position("Hero"), restHero)) << "Hero back to how it was";
    EXPECT_TRUE(sameRotation(Json(rotation(spineEntity)), restSpine)) << "the spine too";
    EXPECT_TRUE(close(scene()->scene->getComponent<LatticeComponent>(cage).offsets[13], Vector3(0, 0, 0), 1e-6f)) << "the cage at rest";
    EXPECT_FALSE(modified()) << "the scene as saved";
    key(ImGuiKey_R);
    st = state();
    EXPECT_TRUE(st["recording"].get<bool>() && st["preview"].get<bool>()) << "R starts Record again (and the preview)";
    key(ImGuiKey_R);
    st = state();
    EXPECT_FALSE(st["recording"].get<bool>() || st["preview"].get<bool>()) << "and stops it with the preview it started";
}

// ---------------------------------------------------------------------------- curves view

TEST_F(AnimationWindowClipTest, CurvesView) {
    const Vector3 rest = position("Hero");
    heroClip();
    Json st = act({{"view", "curves"}, {"frame_curves", "all"}});
    EXPECT_EQ(st["view"], "curves") << "the Curves toggle";
    const int root = trackOf(st, "", "position");
    const int spine = trackOf(st, SPINE, "rotation");
    ASSERT_GE(root, 0);
    ASSERT_GE(spine, 0);
    for (int axis = 0; axis < 3; axis++) EXPECT_TRUE(curve(st, root, axis)) << "position x/y/z curves";
    const Json* y = curve(st, root, 1);
    EXPECT_TRUE(close((*y)["key_values"], {rest.y, 2.0f})) << "the position y key points: " << (*y)["key_values"].dump();
    EXPECT_NEAR((*curve(st, spine, 0))["key_values"][1].get<float>(), 30.0f, 0.05f) << "rotation as Euler degrees";
    EXPECT_TRUE(st["curve_range"][0].get<float>() < std::min(rest.y, 2.0f) && st["curve_range"][1].get<float>() > std::max(rest.y, 2.0f)) << "A frames every curve";
    frames(2);

    // a vertical drag of the y point at 1 s: one undo step, x untouched
    const Json before = (*curve(st, root, 1))["key_values"];
    const Json xBefore = (*curve(st, root, 0))["key_values"];
    st = act({{"curve_drag", {{"track", root}, {"time", 1.0}, {"component", 1}, {"value_delta", 0.5}}}});
    EXPECT_NEAR((*curve(st, root, 1))["key_values"][1].get<float>(), before[1].get<float>() + 0.5f, 1e-4f) << "the y value moved by 0.5";
    EXPECT_EQ((*curve(st, root, 0))["key_values"], xBefore) << "x untouched (only the y point was selected)";
    EXPECT_EQ((*curve(st, root, 1))["selected"], Json::array({1.0})) << "the y point stays selected";
    EXPECT_EQ((*curve(st, root, 0))["selected"], Json::array()) << "x is not";
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 1.0f})) << "no key lost or added";
    EXPECT_FALSE(modified()) << "a clip edit";
    ASSERT_TRUE(undoProject());
    EXPECT_TRUE(close((*curve(state(), root, 1))["key_values"], {before[0].get<float>(), before[1].get<float>()}, 1e-5f)) << "one undo step";

    // a time drag lands on a frame (30 fps); a rotation key's Euler x by +10 degrees
    st = act({{"curve_drag", {{"track", root}, {"time", 1.0}, {"component", 1}, {"time_delta", -0.29}}}});
    EXPECT_EQ(keysOf(st, "", "position"), (std::vector<float>{0.0f, 0.7f})) << "the key lands on a frame";
    ASSERT_TRUE(undoProject());
    EXPECT_EQ(keysOf(state(), "", "position"), (std::vector<float>{0.0f, 1.0f})) << "undone";
    st = act({{"curve_drag", {{"track", spine}, {"time", 1.0}, {"component", 0}, {"value_delta", 10}}}});
    EXPECT_NEAR((*curve(st, spine, 0))["key_values"][1].get<float>(), 40.0f, 0.05f) << "30 -> 40 degrees";
    EXPECT_TRUE(sameRotation((*row(act({{"frame", 30}}), SPINE, "rotation"))["value"], eulerXYZW(40, 0, 0))) << "the key's quaternion";
    ASSERT_TRUE(undoProject());
    // points of two properties (a position y and a rotation x) drag together, each on its curve
    st = act({{"curve_select", {{{"track", root}, {"time", 1.0}, {"component", 1}}, {{"track", spine}, {"time", 1.0}, {"component", 0}}}}});
    const float yBefore = (*curve(st, root, 1))["key_values"][1];
    const float rxBefore = (*curve(st, spine, 0))["key_values"][1];
    st = act({{"curve_drag", {{"track", root}, {"time", 1.0}, {"component", 1}, {"value_delta", 1.0}}}});
    EXPECT_NEAR((*curve(st, root, 1))["key_values"][1].get<float>(), yBefore + 1.0f, 1e-4f) << "position y +1";
    EXPECT_NEAR((*curve(st, spine, 0))["key_values"][1].get<float>(), rxBefore + 1.0f, 0.05f) << "rotation x +1 degree";
    ASSERT_TRUE(undoProject());
    st = state();
    EXPECT_NEAR((*curve(st, root, 1))["key_values"][1].get<float>(), yBefore, 1e-4f);
    EXPECT_NEAR((*curve(st, spine, 0))["key_values"][1].get<float>(), rxBefore, 0.05f) << "one undo step for the mixed drag";
    // visibility and framing
    st = act({{"curve_visible", {{"track", root}, {"component", 0}, {"visible", false}}}});
    EXPECT_TRUE(!(*curve(st, root, 0))["visible"].get<bool>() && (*curve(st, root, 1))["visible"].get<bool>()) << "x hidden, y shown";
    frames(2);
    st = act({{"select", {{{"track", root}, {"time", 1.0}}}}, {"frame_curves", "selected"}});
    const float yKey = (*curve(st, root, 1))["key_values"][1];
    EXPECT_TRUE(st["curve_range"][0].get<float>() <= yKey && yKey <= st["curve_range"][1].get<float>()) << "F frames the selected key";
    act({{"curve_visible", {{"track", root}, {"component", 0}, {"visible", true}}}});
    // Record in the curves view, then back to the dopesheet with Record off
    st = act({{"frame", 15}, {"record", true}});
    EXPECT_TRUE(st["recording"].get<bool>()) << "recording in the curves view";
    frames(2);
    st = act({{"record", false}, {"preview", false}, {"view", "dopesheet"}});
    EXPECT_TRUE(!st["recording"].get<bool>() && st["view"] == "dopesheet");
    EXPECT_TRUE(close(position("Hero"), rest)) << "restored";
}

// ---------------------------------------------------------------------------- final review fixes

namespace {

    // a GameObject under `parent`, tracked by the scene (as a Hierarchy drop puts it there)
    Entity addTracked(SceneProject* sceneProject, Entity parent, const std::string& name, const Vector3& position) {
        Scene* s = sceneProject->scene;
        const Entity entity = s->createEntity();
        s->setEntityName(entity, name);
        Transform transform;
        transform.position = position;
        s->addComponent<Transform>(entity, transform);
        if (parent != NULL_ENTITY) s->addEntityChild(parent, entity, false);
        sceneProject->entities.push_back(entity);
        return entity;
    }

    // an entity Animation of `target`: frames starting together (duration 1 s each), all stored on track 0
    Entity addEntityAnimation(SceneProject* sceneProject, Entity target, const std::string& name, size_t frames) {
        Scene* s = sceneProject->scene;
        const Entity animation = s->createEntity();
        s->setEntityName(animation, name);
        ActionComponent action;
        action.target = target;
        s->addComponent<ActionComponent>(animation, action);
        AnimationComponent component;
        for (size_t i = 0; i < frames; i++) {
            const Entity tween = s->createEntity();
            s->setEntityName(tween, name + " tween " + std::to_string(i));
            ActionComponent tweenAction;
            tweenAction.target = target;
            s->addComponent<ActionComponent>(tween, tweenAction);
            sceneProject->entities.push_back(tween);
            ActionFrame frame;
            frame.startTime = 0.0f;
            frame.duration = 1.0f;
            frame.action = tween;
            frame.track = 0;
            component.actions.push_back(frame);
        }
        s->addComponent<AnimationComponent>(animation, component);
        sceneProject->entities.push_back(animation);
        return animation;
    }

}

// Final review I1: Clear Model during a clip preview saves the rest. What hangs on a model node keeps
// its rest world (not the previewed spine's), and undoing the clear brings back the rest values
TEST_F(AnimationWindowClipTest, ClearModelDuringAPreviewKeepsTheRest) {
    heroClip();
    const Entity spine = entity(std::string("Hero/") + SPINE);   // the scene path: the model GameObject first
    ASSERT_NE(spine, NULL_ENTITY);
    const Entity held = addTracked(scene(), spine, "Held", Vector3(0, 0, 1));
    frames(2);
    Scene* s = scene()->scene;

    // the reference: a clear without a preview, undone
    ASSERT_TRUE(sceneHistory()->addCommand(new ModelClearCmd(project(), sceneId, entity("Hero"))));
    frames(2);
    const Vector3 restLocal = s->getComponent<Transform>(held).position;
    const Quaternion restRotation = s->getComponent<Transform>(held).rotation;
    ASSERT_TRUE(undoScene());
    frames(3);
    const Vector3 restWorld = s->getComponent<Transform>(held).worldPosition;

    // the spine previewed at 30 degrees (1 s), then Clear Model
    Json st = act({{"entity", "Hero"}, {"time", 1.0}, {"preview", true}});
    ASSERT_TRUE(st["preview"].get<bool>());
    frames(2);
    ASSERT_FALSE(close(s->getComponent<Transform>(held).worldPosition, restWorld)) << "the preview moves the held prop";
    ASSERT_TRUE(sceneHistory()->addCommand(new ModelClearCmd(project(), sceneId, entity("Hero"))));
    frames(3);
    EXPECT_FALSE(state()["preview"].get<bool>()) << "the clear stopped the preview first";
    EXPECT_TRUE(close(s->getComponent<Transform>(held).position, restLocal))
        << "the held prop keeps its rest world: " << s->getComponent<Transform>(held).position.toString();
    EXPECT_TRUE(s->getComponent<Transform>(held).rotation == restRotation) << "no previewed rotation";

    // undo during another preview: the restored model is the rest, and the preview stops before it
    ASSERT_TRUE(undoScene());
    frames(3);
    EXPECT_TRUE(close(s->getComponent<Transform>(held).worldPosition, restWorld)) << "back under the spine at its rest";
    st = act({{"entity", "Hero"}, {"time", 1.0}, {"preview", true}});
    ASSERT_TRUE(st["preview"].get<bool>());
    ASSERT_TRUE(redoScene());
    frames(2);
    ASSERT_TRUE(undoScene());
    frames(3);
    act({{"preview", false}});
    EXPECT_TRUE(close(s->getComponent<Transform>(held).worldPosition, restWorld)) << "the rest after redo + undo under a preview";
}

// Final review M6: the entity mode's dropdown entries (R31) and the explicit frame move, both cases:
// an entity Animation of Robot (no Animation Player: Create New Clip... only), and one of a Hero bone
// shown while the bone is selected (the Hero's player's states, then Create New Clip...)
TEST_F(AnimationWindowClipTest, EntityModeEntriesAndFrameMove) {
    heroClip();
    addEntityAnimation(scene(), entity("Robot"), "RobotTweens", 2);
    frames(2);
    const bool wasModified = modified();
    Json st = act({{"entity", "Robot"}, {"entity_mode_entries", true}});
    EXPECT_EQ(st["mode"], "entity") << st.dump();
    EXPECT_EQ(st["entity_animation"], "RobotTweens");
    EXPECT_EQ(st["entity_mode_entries"], Json::array({"Create New Clip..."}));
    EXPECT_EQ(st["entity_lanes"], Json::array({0, 1})) << "laid on two lanes for display";
    EXPECT_EQ(st["entity_tracks"], Json::array({0, 0})) << "stored tracks unchanged";
    EXPECT_EQ(modified(), wasModified) << "the lanes are view only";

    // the second frame released on lane 2: one scene step
    st = act({{"entity_move_frame", {{"index", 1}, {"lane", 2}}}});
    EXPECT_EQ(st["entity_tracks"], Json::array({0, 2}));
    EXPECT_TRUE(modified());
    ASSERT_TRUE(undoScene());
    st = state();
    EXPECT_EQ(st["entity_tracks"], Json::array({0, 0})) << "one undo step";
    EXPECT_NE(refused({{"entity_move_frame", {{"index", 5}, {"lane", 1}}}}).find("no such frame"), std::string::npos);

    // a Hero bone's entity Animation shown with the bone selected: the bone's player is the Hero's
    const std::string spinePath = std::string("Hero/") + SPINE;
    addEntityAnimation(scene(), entity(spinePath), "SpineTweens", 1);
    frames(2);
    st = act({{"entity", spinePath}});
    EXPECT_EQ(st["mode"], "clip") << "a GameObject under a player opens clip mode";
    st = act({{"entity_animation", "SpineTweens"}, {"entity_mode_entries", true}});
    EXPECT_EQ(st["mode"], "entity");
    EXPECT_EQ(st["entity_animation"], "SpineTweens");
    EXPECT_EQ(st["entity_mode_entries"], Json::array({"hero_clip (clip)", "Create New Clip..."}));
}

// The entity mode's timeline audio (S40 R22, R23): Add Property > Sound and a drop on the tracks add
// an audio action and its frame in one undo step, the block says what it plays, Align Peak moves the
// frame, and the playing preview plays it as a preview voice
TEST_F(AnimationWindowClipTest, EntityModeAudioActions) {
    std::vector<float> samples(24000, 0.0f);
    for (int i = 0; i < 480; i++) samples[9600 + i] = (i % 2 ? -0.9f : 0.9f) * (1.0f - (float)i / 480.0f);
    const fs::path tickFile = headless().dir / "assets" / "sounds" / "tick.wav";
    fs::create_directories(tickFile.parent_path());
    writeAll(tickFile, WavWriter::pcm16(samples, 1, 48000));
    const std::string tick = project()->normalizeToAssetsRelative(tickFile).generic_string();
    addEntityAnimation(scene(), entity("Robot"), "RobotSounds", 1);
    frames(2);
    Json st = act({{"entity", "Robot"}});
    ASSERT_EQ(st["mode"], "entity") << st.dump();
    const size_t frameCount = st["entity_frames"].size();
    st = act({{"entity_audio_action", {{"type", "audio"}, {"content", tick}, {"time", 0.5}}}});
    ASSERT_EQ(st["entity_frames"].size(), frameCount + 1) << st["entity_frames"].dump();
    const Json added = st["entity_frames"].back();
    EXPECT_EQ(added["label"], "Audio: tick");
    EXPECT_NEAR(added["start"].get<float>(), 0.5f, 1e-4f);
    EXPECT_EQ(st["entity_selected_frame"], (int)frameCount);
    ASSERT_TRUE(undoScene());
    EXPECT_EQ(state()["entity_frames"].size(), frameCount) << "the action and its frame are one undo step";
    ASSERT_TRUE(redoScene());
    st = act({{"entity_audio_action", {{"type", "snapshot"}, {"content", "Build"}, {"time", 1.0}}}});
    EXPECT_EQ(st["entity_frames"].back()["label"], "Mixer Snapshot: Build");

    // Align Peak on the selected audio block, once the analysis has the peak
    for (;;) {
        const uint64_t generation = host.taskGeneration();
        float time = 0.0f;
        act({{"entity_select_frame", (int)frameCount}, {"entity_time", 1.0}});
        if (AnimationWindowTestAccess::alignProblem(*window, 1, &time).empty()) break;
        if (!TimelineSounds::get(project(), tick).pending) break;
        host.waitForTaskAfter(generation);
    }
    st = act({{"entity_select_frame", (int)frameCount}, {"entity_time", 1.0}, {"entity_command", "animation.alignCuePeak"}});
    EXPECT_NEAR(st["entity_frames"][frameCount]["start"].get<float>(), 0.8f, 0.002f) << "the tick's peak (0.2 s) on 1.0 s";
    ASSERT_TRUE(undoScene());
    EXPECT_NEAR(state()["entity_frames"][frameCount]["start"].get<float>(), 0.5f, 1e-4f) << "one undo step";

    // the playing preview plays the action as a preview voice
    st = act({{"entity_time", 0.0}, {"entity_play", true}});
    for (int i = 0; i < 60; i++) frame(true, 1.0 / 60.0);
    st = act({{"entity_play", false}});
    EXPECT_GE(st["entity_audio_starts"].get<int>(), 1) << "the audio action at 0.5 s played in the preview";
}

// Keys copied from one property paste onto the selected key's property only when it is of their
// kind and width: a bool key on a float curve goes back to its own property
TEST_F(AnimationWindowClipTest, PastedPropertyKeysKeepTheirKind) {
    heroClip();
    const Json clip = {{"format", "jasperengine.anim"}, {"version", 1}, {"frame_rate", 30}, {"wrap", "once"},
                       {"tracks", {{{"path", ""}, {"channel", "property"}, {"component", "gameobject"}, {"property", "active"},
                                    {"keys", {{0, true}, {0.5, false}}}},
                                   {{"path", ""}, {"channel", "property"}, {"component", "light"}, {"property", "intensity"},
                                    {"keys", {{0, 2.0}, {1, 3.0}}}}}},
                       {"events", Json::array()}};
    writeClip("assets/anim/kinds.anim", clip);
    act({{"clip", "assets/anim/kinds.anim"}, {"root", "Hero"}});
    act({{"select", {{{"track", 0}, {"time", 0.5}}}}, {"copy", true}});
    act({{"select", {{{"track", 1}, {"time", 1.0}}}}, {"paste", 0.8}});
    const Json file = clipFile("assets/anim/kinds.anim");
    size_t activeKeys = 0, intensityKeys = 0;
    bool pastedOnActive = false;
    for (const Json& t : file["tracks"]) {
        if (t["property"] == "active") {
            activeKeys = t["keys"].size();
            for (const Json& k : t["keys"]) pastedOnActive = pastedOnActive || std::fabs(k[0].get<float>() - 0.8f) < 1e-4f;
        }
        if (t["property"] == "intensity") intensityKeys = t["keys"].size();
    }
    EXPECT_EQ(intensityKeys, 2u) << "the float curve is not given a bool key: " << file.dump();
    EXPECT_EQ(activeKeys, 3u);
    EXPECT_TRUE(pastedOnActive) << "pasted on its own property at the playhead";
}
