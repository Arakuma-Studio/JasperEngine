// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCENE_H
#define SCENE_H

#include "Entity.h"
#include "SubSystem.h"
#include "EntityRegistry.h"
#include "SystemScheduler.h"
#include "Export.h"
#include "SceneSettings.h"
#include "render/shadow/ShadowTiers.h"
#include "render/Render.h"
#include "math/Vector2.h"
#include <memory>
#include <string>
#include <vector>


namespace jasperengine{

    class Camera;
    class SceneBvh;

    // LightState, ShadowQuality and SceneColorScale live in SceneSettings.h (they are scene-setting types).

    enum class UIEventState {
        NOT_SET,
        ENABLED,
        DISABLED
    };

    class JASPERENGINE_API Scene : public EntityRegistry {
    private:

        Entity camera;
        Entity defaultCamera;

        // All persistent scene configuration lives here (defaults from SceneSettings). The
        // public getters/setters below delegate to it, keeping the side effects (shader
        // reloads) and color-space conversions that can't live in a plain-data struct.
        // Gravity's live value stays in PhysicsSystem; settings.gravity* is only the default.
        SceneSettings settings;
        // the Scene view's shadow debug view: editor state, not a setting (never saved)
        ShadowDebugView shadowDebugView = ShadowDebugView::NONE;

        UIEventState uiEventState;

        // the Canvas Scaler's last result (updateCanvasScale)
        CanvasScalerResult canvasScale;

        std::vector<std::pair<std::string, std::shared_ptr<SubSystem>>> systems;
        // runs the systems' phases (the SubSystems' and the system functions)
        SystemScheduler scheduler;

        // Alive from construction to the start of the destructor (see getLifetime)
        std::shared_ptr<char> lifetime;
        bool tearingDown = false;

        void init();
        Entity createDefaultCamera();
        // the SubSystem's FixedUpdate, Update and Draw entries in the scheduler
        void scheduleSubSystem(const std::string& name, SubSystem* system, const SystemDescriptor& descriptor);
        // the change tick at the end of the last frames (Removed<T> trimming without a gate)
        Tick frameEndTicks[2] = {0, 0};

        // the picking BVH, made by the first query (getPickBvh); until then the hooks below
        // cost one pointer test
        std::unique_ptr<SceneBvh> pickBvh;
        void notifyPickStructure(Signature components);
        void notifyPickDirty(Entity entity);

    protected:

        // The add/remove hooks go only to the systems that handle the type (declareHookType)
        void onComponentAdded(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                if (pair.second->handlesHookType(componentId)) {
                    pair.second->onComponentAdded(entity, componentId);
                }
            }
            if (pickBvh) {
                Signature added;
                added.set(componentId);
                notifyPickStructure(added);
            }
        }

        void onComponentRemoved(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                if (pair.second->handlesHookType(componentId)) {
                    pair.second->onComponentRemoved(entity, componentId);
                }
            }
            if (pickBvh) {
                Signature removed;
                removed.set(componentId);
                notifyPickStructure(removed);
            }
        }

        // physics re-filters the entity's bodies; picking's layer masks see the new layer
        void onEntityLayerChanged(Entity entity) override {
            for (auto const& pair : systems) {
                pair.second->onEntityLayerChanged(entity);
            }
            if (pickBvh) {
                notifyPickDirty(entity);
            }
        }

        // the scripts' onEnable / onDisable (and a first init) once they started (ScriptRuntime)
        bool isActivationObserved() const override;
        void onActiveInHierarchyChanged(const std::vector<Entity>& changed, bool nowActive) override;

        void onEntityDestroyed(Entity entity, Signature signature) override {
            for (auto const& pair : systems) {
                SubSystem* system = pair.second.get();
                system->forEachHookType(signature, [&](size_t componentId) {
                    system->onComponentRemoved(entity, (ComponentId)componentId);
                });
            }
            if (pickBvh) {
                notifyPickStructure(signature);
            }
        }

        // GameObject.destroy semantics (ObjectManager: scripts stopped first)
        void destroyHierarchyNow(Entity entity) override;

    public:

        Scene();
        Scene(EntityPool defaultPool);
        virtual ~Scene();

        Scene* asScene() override { return this; }

        // A scene is its entities' only registry: a copy would share their sub-resources
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        // Expires when the scene starts being deleted. A handle that owns an entity
        // (EntityHandle(Scene*)) keeps it to know whether the scene is still there when it
        // goes: a GameObject made from Lua goes when the garbage collector takes it, which
        // can be after its scene was deleted (a Play copy at play stop, or any scene before
        // lua_close at shutdown). The scene destroys all its entities, so nothing leaks.
        std::weak_ptr<char> getLifetime() const;

        // Being deleted (set first thing in the destructor): what goes with it sends no
        // callbacks (physics contact Exits...). Not Scene::destroy, which also runs when a
        // view is recreated.
        bool isTearingDown() const { return tearingDown; }

        void load();
        void destroy();
        void draw();
        void update(double dt);
        void fixedUpdate(double dt);
        // The frame's end (draw calls it): Removed<T> entries every enabled system's gate is
        // past go, or those older than two frames when no system ran
        void endFrame();

        // Deprecated, as Engine::removeSubscriptionsByTag: SubscriptionOwnerIndex::releaseOwner
        void removeSubscriptionsByTag(const std::string& substring);

        // the Canvas Scaler of the current view, then the camera: a UI scene's camera spans the
        // UI size, any other the game canvas (Engine::systemViewChanged, includeScene, setCamera)
        void updateCameraSize();

        // Canvas Scaler (the UI's own space). The scene's settings, used unless
        // useProjectDefault (then Engine::getDefaultCanvasScaler()); a change applies at once
        // when the view exists (the next frame lays the UI out in it)
        void setCanvasScaler(const CanvasScalerSettings& settings);
        const CanvasScalerSettings& getCanvasScaler() const;
        void setCanvasScalerUseProjectDefault(bool useProjectDefault);
        bool isCanvasScalerUseProjectDefault() const;
        // what applies to this scene: its own settings or the project default
        const CanvasScalerSettings& getEffectiveCanvasScaler() const;
        // Recomputes the result for a view of viewWidth x viewHeight physical pixels showing a
        // game canvas of canvasWidth x canvasHeight (updateCameraSize passes the Engine's; the
        // editor passes the Game view's for the Scene view)
        void updateCanvasScale(float viewWidth, float viewHeight, float canvasWidth, float canvasHeight, float screenDpi);
        // {uiWidth, uiHeight, scaleFactor} of the current view (the safe-area contract);
        // scaled is false under FollowGameCanvas
        const CanvasScalerResult& getCanvasScale() const;
        // the native UI of this scene lays out in the scaled UI size: a scaler other than
        // FollowGameCanvas and a UI camera (CAMERA_UI)
        bool isUIScaled() const;
        // the screen HTML UI documents lay out in the scaled UI size (a scaler other than
        // FollowGameCanvas, whatever the camera)
        bool isDocumentScaled() const;
        // game canvas pixels <-> this scene's UI units (identity when not scaled)
        Vector2 canvasToUI(const Vector2& canvasPoint) const;
        Vector2 uiToCanvas(const Vector2& uiPoint) const;

        void setCamera(Camera* camera);
        void setCamera(Entity camera);
        Entity getCamera() const;
        // Back to the scene's own default camera (a UI camera, made if needed): its camera
        // GameObject went to another scene (SceneManager::moveGameObjectToScene)
        void useDefaultCamera();

        void setBackgroundColor(Vector4 color);
        void setBackgroundColor(float red, float green, float blue);
        void setBackgroundColor(float red, float green, float blue, float alpha);
        Vector4 getBackgroundColor() const;

        void setShadowQuality(ShadowQuality quality);
        ShadowQuality getShadowQuality() const;
        // directional shadow distance (metres, 0: the quality tier's), its fade and the cascade blend
        void setShadowDistance(float distance);
        float getShadowDistance() const;
        void setShadowFadeFraction(float fraction);
        float getShadowFadeFraction() const;
        void setShadowCascadeBlend(bool blend);
        bool isShadowCascadeBlend() const;
        // the shadow quality tier the scene renders with (its Quality, or the platform's)
        ShadowTier getEffectiveShadowTier() const;
        // the Scene view's shadow debug view (editor only, not saved)
        void setShadowDebugView(ShadowDebugView view);
        ShadowDebugView getShadowDebugView() const;

        // The picking BVH of the scene (MeshRaycast, the editor's Scene view picking): made on
        // the first call, destroyed with the scene
        SceneBvh& getPickBvh();
        // null until a query made it
        SceneBvh* findPickBvh() const { return pickBvh.get(); }
        // the entity's bounds changed (RenderSystem): its leaf is refitted at the next query
        void markPickDirty(Entity entity) { if (pickBvh) notifyPickDirty(entity); }

        void setMeshLodEnabled(bool enabled);
        bool isMeshLodEnabled() const;

        void setMeshLodThreshold(float pixels);
        float getMeshLodThreshold() const;

        // Cross Fade and Cross-fade Duration (seconds, 0.05..2) of the detail levels
        void setMeshLodCrossFade(bool crossFade);
        bool isMeshLodCrossFade() const;
        void setMeshLodCrossFadeDuration(float seconds);
        float getMeshLodCrossFadeDuration() const;

        void setDepthPrepassEnabled(bool enabled);
        bool isDepthPrepassEnabled() const;

        void setSceneColorScale(SceneColorScale scale);
        SceneColorScale getSceneColorScale() const;

        void setSSAOEnabled(bool ssaoEnabled);
        bool isSSAOEnabled() const;

        void setSSAORadius(float radius);
        float getSSAORadius() const;

        void setSSAOIntensity(float intensity);
        float getSSAOIntensity() const;

        void setSSAOBias(float bias);
        float getSSAOBias() const;

        void setSSAODebug(bool debug);
        bool isSSAODebug() const;

        void setSSREnabled(bool ssrEnabled);
        bool isSSREnabled() const;

        void setSSRMaxDistance(float maxDistance);
        float getSSRMaxDistance() const;

        void setSSRThickness(float thickness);
        float getSSRThickness() const;

        void setSSRMaxSteps(int maxSteps);
        int getSSRMaxSteps() const;

        void setSSRIntensity(float intensity);
        float getSSRIntensity() const;

        void setSSRBlur(float blur);
        float getSSRBlur() const;

        // the SSR debug view: 0 off, 1 reflection, 2 normal, 3 roughness, 4 metallic, 5 albedo,
        // 6 environment specular, 7 reflection source, 8 occlusion (docs/rendering/reflections.md)
        void setSSRDebugMode(int mode);
        int getSSRDebugMode() const;

        void setFixedResolutionEnabled(bool fixedResolutionEnabled);
        bool isFixedResolutionEnabled() const;

        void setFixedResolutionWidth(unsigned int width);
        unsigned int getFixedResolutionWidth() const;

        void setFixedResolutionHeight(unsigned int height);
        unsigned int getFixedResolutionHeight() const;

        void setFixedResolutionSize(unsigned int width, unsigned int height);

        void setFixedResolutionFilter(TextureFilter filter);
        TextureFilter getFixedResolutionFilter() const;

        void setLightState(LightState state);
        LightState getLightState() const;

        void setGlobalIllumination(float intensity, Vector3 color);
        void setGlobalIllumination(float intensity);
        void setGlobalIllumination(Vector3 color);

        float getGlobalIlluminationIntensity() const;
        Vector3 getGlobalIlluminationColor() const;
        Vector3 getGlobalIlluminationColorLinear() const;

        // lights each object is lit by (1..16); the other lights reaching it are folded into
        // its spherical-harmonics ambient
        void setMaxLightsPerObject(int maxLightsPerObject);
        int getMaxLightsPerObject() const;

        // the built-in Lit's shading quality (a variant of every lit mesh: they reload)
        void setLitQuality(LitQuality litQuality);
        LitQuality getLitQuality() const;
        // the exposure (EV100, -6..24) lights in physical units are drawn at
        void setPhysicalLightEV100(float ev100);
        float getPhysicalLightEV100() const;

        // capsule shadows (spec S57 R2): the lit variants take them while on (they reload);
        // intensity 0..1, softness 0..2 (the penumbra's widening per metre along the light)
        void setCapsuleShadowsEnabled(bool enabled);
        bool isCapsuleShadowsEnabled() const;
        void setCapsuleShadowIntensity(float intensity);
        float getCapsuleShadowIntensity() const;
        void setCapsuleShadowSoftness(float softness);
        float getCapsuleShadowSoftness() const;

        void setAmbientLight2D(float intensity, Vector3 color);
        void setAmbientLight2D(float intensity);
        void setAmbientLight2D(Vector3 color);

        float getAmbientLight2DIntensity() const;
        Vector3 getAmbientLight2DColor() const;
        Vector3 getAmbientLight2DColorLinear() const;

        void setShadow2DQuality(ShadowQuality quality);
        ShadowQuality getShadow2DQuality() const;

        // convenience forwarders to PhysicsSystem gravity
        Vector2 getGravity2D() const;
        void setGravity2D(Vector2 gravity);
        void setGravity2D(float x, float y);

        Vector3 getGravity3D() const;
        void setGravity3D(Vector3 gravity);
        void setGravity3D(float x, float y, float z);

        // forwarder to PhysicsSystem::setInterpolation: bodies drawn between their last two
        // fixed steps, so they move smoothly at any frame rate
        void setPhysicsInterpolation(bool interpolation);
        bool isPhysicsInterpolation() const;

        // forwarder to PhysicsSystem::setSubSteps2D: Box2D sub-steps of each fixed step (1..16)
        void setPhysics2DSubSteps(int subSteps);
        int getPhysics2DSubSteps() const;

        void setDefaultMeshShader(const std::string& path);
        const std::string& getDefaultMeshShader() const;

        void setDefaultUIShader(const std::string& path);
        const std::string& getDefaultUIShader() const;

        void setDefaultSkyShader(const std::string& path);
        const std::string& getDefaultSkyShader() const;

        void setDefaultPointsShader(const std::string& path);
        const std::string& getDefaultPointsShader() const;

        void setDefaultLinesShader(const std::string& path);
        const std::string& getDefaultLinesShader() const;

        void setDefaultCustomShader(ShaderType type, const std::string& path);
        const std::string& getDefaultCustomShader(ShaderType type) const;
        // The default shader of a type with the asset GUIDs of its files (editor references)
        const ShaderRef& getDefaultShaderRef(ShaderType type) const;
        void setDefaultShaderRef(ShaderType type, const ShaderRef& ref);

        // Property values of a default shader (mesh, UI, sky, points, lines), inherited by the
        // objects that use it, under their own: Scene > Default Shaders > <Shader> Properties.
        // Other types log an error and change nothing.
        void setDefaultShaderUniforms(ShaderType type, const ShaderUniformValues& values);
        const ShaderUniformValues& getDefaultShaderUniforms(ShaderType type) const;
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector4& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector3& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector2& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, float value);
        Vector4 getDefaultShaderUniform(ShaderType type, const std::string& name) const;
        bool hasDefaultShaderUniform(ShaderType type, const std::string& name) const;
        bool removeDefaultShaderUniform(ShaderType type, const std::string& name);
        // What an object whose own shader is ownShader inherits: null when it has a shader of its
        // own, when the scene has no default shader of the type, or no values for it
        const ShaderUniformValues* getInheritedShaderUniforms(ShaderType type, const std::string& ownShader) const;

        // ordered user post-process chain; setting it rebuilds the RenderSystem passes
        // unless only uniform values changed
        const std::vector<PostProcessPass>& getPostProcessPasses() const;
        void setPostProcessPasses(const std::vector<PostProcessPass>& passes);

        // one u_fs_postParams member of a pass; rewrites the value without rebuilding the chain
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector4& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector3& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector2& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, float value);
        Vector4 getPostProcessUniform(unsigned int index, const std::string& name) const;
        bool removePostProcessUniform(unsigned int index, const std::string& name);
        // a u_fs_postParams member of pass index that follows a global shader value
        // (Shader.setGlobal*); "" unbinds. False when nothing changed
        bool setPostProcessGlobalBinding(unsigned int index, const std::string& member, const std::string& global);
        std::string getPostProcessGlobalBinding(unsigned int index, const std::string& member) const;
        void setPostProcessPassEnabled(unsigned int index, bool enabled);
        bool isPostProcessPassEnabled(unsigned int index) const;

        // the engine's post-processing of the main camera (PostProcessingSettings in
        // SceneSettings.h); setting it clamps the values and lets RenderSystem rebake what
        // changed (the colour LUT, the shaders' post-processing define, the uber pass)
        const PostProcessingSettings& getPostProcessing() const;
        void setPostProcessing(const PostProcessingSettings& postProcessing);

        bool canReceiveUIEvents();

        UIEventState getEnableUIEvents() const;
        void setEnableUIEvents(UIEventState enableUIEvents);
        void enableUIEvents();

        bool isEnableUIEvents() const;
        void setEnableUIEvents(bool enableUIEvents);

        // System methods

        // A SubSystem: its fixedUpdate, update and draw run as MainThread | Exclusive systems
        // named after the class
        template<typename T>
        std::shared_ptr<T> registerSystem(){
            SystemDescriptor descriptor;
            descriptor.resources = SystemResource::MainThread | SystemResource::Exclusive;
            return registerSystem<T>(descriptor);
        }

        // ...with its own descriptor (reads, writes, resources, dependsOn, everyNFrames) for
        // its three phase entries; the descriptor's phase is not used, and an empty name is
        // the class name
        template<typename T>
        std::shared_ptr<T> registerSystem(SystemDescriptor descriptor){
            const char* typeName = typeid(T).name();

            [[maybe_unused]] auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            assert(it == systems.end() && "Registering system more than once");

            auto system = std::make_shared<T>(this);
            systems.push_back(std::make_pair(typeName, system));
            if (descriptor.name.empty()){
                descriptor.name = ecs::readableTypeName(typeName);
            }
            scheduleSubSystem(descriptor.name, system.get(), descriptor);
            return system;
        }

        // the registered system of that type, or nullptr (getSystem asserts it exists)
        template<typename T>
        std::shared_ptr<T> findSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            return it != systems.end() ? std::dynamic_pointer_cast<T>(it->second) : nullptr;
        }

        // A system function run in the descriptor's phase (see SystemScheduler)
        ScheduledSystem& addSystemFunction(SystemDescriptor descriptor, std::function<void(SystemContext&)> function);

        SystemScheduler& getScheduler();

        template<typename T>
        std::shared_ptr<T> getSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            assert(it != systems.end() && "System not found.");

            return std::dynamic_pointer_cast<T>(it->second);
        }
    };

}

#endif //SCENE_H