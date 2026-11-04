// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderNeutralApi.h"

using namespace jasperengine::editor;

const std::map<std::string, uint32_t>& ShaderNeutralApi::neutralClasses() {
    // math and values; input; logs, files and the system; physics and water queries; audio;
    // navigation; the animators (bones, not materials); scenes, prefabs and entities (spawned
    // prefabs are cooked into every scene); actions that move, turn or scale; spline followers
    static const std::map<std::string, uint32_t> classes = {
        {"Vector2", 0x37d5512au},
        {"Vector3", 0xbdf2f4bdu},
        {"Vector4", 0x2fa2b675u},
        {"Rect", 0x0fdd61eau},
        {"Sphere", 0x35e0411fu},
        {"Matrix3", 0x5afdc774u},
        {"Matrix4", 0xce8af031u},
        {"Quaternion", 0x6f2e30e1u},
        {"Plane", 0xdc3127fcu},
        {"AABB", 0xd4223d35u},
        {"OBB", 0xbf5e79a2u},
        {"RayReturn", 0x6da341d2u},
        {"Ray", 0x3727652du},
        {"Angle", 0x213e3b03u},
        {"Base64", 0x811c9dc5u},
        {"Color", 0xc47156c1u},
        {"Keyframe", 0x73fa28f4u},
        {"AnimationCurve", 0xdac0a235u},
        {"GradientColorKey", 0x8fd5ad9fu},
        {"GradientAlphaKey", 0x51d0e120u},
        {"Gradient", 0x3b609eadu},
        {"MinMaxCurve", 0x9309ffd5u},
        {"MinMaxGradient", 0x18ba741eu},
        {"Ease", 0x9897fd4eu},
        {"LayerMask", 0xc0bc567fu},
        {"Input", 0x42affea3u},
        {"InputActionEvent", 0x0f075b4cu},
        {"InputBinding", 0xca780f92u},
        {"InputAction", 0x82b8c81bu},
        {"InputActionMap", 0xcddbff0bu},
        {"InputActions", 0x74622d4du},
        {"Log", 0xeeef4a57u},
        {"LogCategory", 0xe96f7290u},
        {"System", 0x252009b2u},
        {"FileData", 0x80c088d6u},
        {"File", 0xb59cd2d9u},
        {"Data", 0x812fbef9u},
        {"UserSettings", 0xf73aa4a8u},
        {"ThreadPoolManager", 0x47d517d2u},
        {"ResourceProgress", 0x71d03622u},
        {"ResourceBuildInfo", 0x044de46au},
        {"OverallBuildProgress", 0x2336bf6fu},
        {"PhysicsMaterial", 0xbfd7eb57u},
        {"ControllerStateInfo", 0x4c569349u},
        {"Physics2D", 0x4a1ccb1bu},
        {"PhysicsSystem", 0xf3e9bffeu},
        {"Rigidbody", 0x93244959u},
        {"Rigidbody2D", 0x6b62a5d7u},
        {"Joint2D", 0x752b4ab5u},
        {"Joint3D", 0x9e244509u},
        {"Contact2D", 0x9896e5eau},
        {"Contact3D", 0x1ed07a59u},
        {"Manifold2D", 0x44d54ffau},
        {"CollideShapeResult3D", 0x5fa5c569u},
        {"CharacterController", 0x0d4ce962u},
        {"CharacterController2D", 0x1552b032u},
        {"ControllerColliderHit", 0x8de113a0u},
        {"Buoyancy", 0x4d7d14b5u},
        {"WaterVolume", 0x1399452eu},
        {"Water", 0x91f19d94u},
        {"WaterSample", 0x13bb9f0cu},
        {"MeshRaycast", 0x5dfe6f84u},
        {"MeshRaycastHit", 0x79d628f8u},
        {"Audio", 0xa4eff354u},
        {"SoundHandle", 0x3606cf71u},
        {"AudioSource", 0xd25afbe8u},
        {"AudioSystem", 0xe61632ceu},
        {"AudioSourceComponent", 0xff06d789u},
        {"NavMeshQueryFilter", 0x811f5959u},
        {"GridCell", 0xa297eb80u},
        {"NavigationSystem", 0x26e60eaau},
        {"NavMeshAgent", 0xd22cd366u},
        {"NavMeshSurface", 0x6c97bf5au},
        {"NavGrid", 0x5911c33cu},
        {"Animator", 0x0fc1a132u},
        {"AnimationPlayer", 0xc4bf72c9u},
        {"PoseAnimator", 0xe172172au},
        {"AnimationEvent", 0x3191cc4eu},
        {"AnimationState", 0xd696bd82u},
        {"AnimatorStateInfo", 0x6a5a657eu},
        {"SceneManager", 0x0261a82cu},
        {"PrefabManager", 0x49a7b220u},
        {"PrefabAsset", 0xd4cce93du},
        {"AssetRef", 0x0fdd37b1u},
        {"EntityRegistry", 0x6e532592u},
        {"EntityManager", 0x11ab3e29u},
        {"EntityHandle", 0x51fd7a05u},
        {"Action", 0xecfbfddeu},
        {"Tween", 0x54f157bbu},
        {"PositionTween", 0x65f4ac81u},
        {"RotationTween", 0x2b655209u},
        {"ScaleTween", 0x65f4ac81u},
        {"Animation", 0x3a6f5527u},
        {"KeyframeTracks", 0x932a474au},
        {"RotateTracks", 0x47b47619u},
        {"ScaleTracks", 0x47b47619u},
        {"TranslateTracks", 0x47b47619u},
        {"ActionComponent", 0x016b7ef8u},
        {"ActionFrame", 0x598a12d6u},
        {"AnimationComponent", 0x1f9ef76eu},
        {"TweenComponent", 0x6fb2e681u},
        {"ActionSystem", 0x1d297e9du},
        {"AnimationPlayerComponent", 0x7910401bu},
        {"AnimatorComponent", 0x1d9d525cu},
        {"PoseAnimatorComponent", 0xef0fbae3u},
        {"SplineFrame", 0x2e6a235du},
        {"SplineHit", 0x923264d8u},
        {"SplineAnimate", 0x00658326u},
    };
    return classes;
}

const std::map<std::string, std::set<std::string>>& ShaderNeutralApi::mixedClasses() {
    static const std::map<std::string, std::set<std::string>> classes = {
        // queries and collision rules, not the debug drawing: its lines are a LineRenderer's, made at
        // runtime, whose shader no scene's cook asked for
        {"Physics", {
            "ALL_LAYERS", "DEFAULT_RAYCAST_LAYERS", "IGNORE_RAYCAST_LAYER", "getIgnoreCollision",
            "getIgnoreLayerCollision", "getLayerCollisionMask", "ignoreCollision",
            "ignoreLayerCollision", "raycast", "raycastAll", "resetLayerCollisionMatrix",
            "setLayerCollisionMask"
        }},
        // scene layers and loading, the canvas and input routing, time, threads, events (not the
        // texture or atmosphere quality setters, nor rendering into a framebuffer)
        {"Engine", {
            "setScene", "getScene", "addSceneLayer", "executeSceneOnce", "removeScene",
            "removeAllSceneLayers", "removeAllScenes", "isSceneRunning", "hasScenesToExecuteOnce",
            "getMainScene", "getLastScene", "pauseGameEvents", "setCanvasSize", "getTextureQuality",
            "getTextureMemoryBudget", "getTextureMemoryUsage", "isTextureFormatSupported",
            "isTextureLowMemoryFallback", "getAtmosphereQuality", "setCallTouchInMouseEvent",
            "isUIEventReceived", "wantsGamepadNavigation", "setUpdateTimeMS", "setMousePosition",
            "isHeadless", "quit", "startAsyncThread", "commitThreadQueue", "endAsyncThread", "isAsyncThread",
            "isViewLoaded", "setMaxResourceLoadingThreads", "getQueuedResourceCount",
            "clearAllSubscriptions", "clearComponentSubscriptions", "scene", "canvasWidth", "canvasHeight",
            "preferredCanvasWidth", "preferredCanvasHeight", "viewRect", "scalingMode",
            "callMouseInTouchEvent", "callTouchInMouseEvent", "useDegrees", "allowEventsOutCanvas",
            "ignoreEventsHandledByUI", "updateTime", "interpolationAlpha", "mouseCursor", "mouseMode",
            "platform", "graphicBackend", "openGL", "framerate", "deltatime", "systemTime", "timeScale",
            "fixedDeltatime", "gameTime", "frameCount", "asyncLoading", "onViewLoaded", "onCanvasChanged",
            "onViewDestroyed", "onDraw", "onUpdate", "onFixedUpdate", "onPostUpdate", "onPause", "onResume",
            "onShutdown", "onSceneLoaded", "onTouchStart", "onTouchEnd", "onTouchMove", "onTouchCancel",
            "onMouseDown", "onMouseUp", "onMouseScroll", "onMouseMove", "onMouseEnter", "onMouseLeave",
            "onKeyDown", "onKeyUp", "onCharInput", "onGamepadConnect", "onGamepadDisconnect",
            "onGamepadButtonDown", "onGamepadButtonUp", "onGamepadAxisMove"}},
        // loading, the camera, background colour, gravity, systems without drawing, ECS queries,
        // and the switches the collector cooks every state of (light capacity, shadow and lit
        // quality)
        {"Scene", {
            "load", "destroy", "draw", "update", "updateCameraSize", "setCamera", "setBackgroundColor",
            "getGlobalIlluminationColorLinear", "getAmbientLight2DColorLinear", "setGravity2D",
            "setGravity3D", "getPostProcessUniform", "isPostProcessPassEnabled", "canReceiveUIEvents",
            "getActionSystem", "getAudioSystem", "getPhysicsSystem", "getNavigationSystem", "getChangeTick",
            "findEntitiesWith", "findChangedEntities", "markChanged", "camera", "backgroundColor",
            "gravity2D", "gravity3D", "physicsInterpolation", "enableUIEvents", "maxLightsPerObject",
            "shadowQuality", "litQuality", "effectiveShadowTier"}},
        // the hierarchy, the transform, visibility, destruction, and the components above that draw
        // nothing (not instantiate: a clone can land in a scene of other lighting; not
        // billboarding)
        {"GameObject", {
            "destroy", "destroyImmediate", "addChild", "removeChild", "removeParent", "moveToTop", "moveUp",
            "moveDown", "moveToBottom", "setPosition", "getWorldPosition", "setRotation", "getWorldRotation",
            "setScale", "getWorldScale", "setVisibleOnly", "updateTransform", "getRigidbody2D",
            "removeRigidbody2D", "getCharacterController2D", "removeCharacterController2D", "getRigidbody",
            "removeRigidbody", "getCharacterController", "removeCharacterController", "getBuoyancy",
            "getWaterVolume", "getSplineAnimate", "getRay", "getAnimationPlayer", "hasAnimationPlayer",
            "hasVertexAnimator", "getAnimator", "hasAnimator", "position", "rotation", "scale", "visible",
            "localMatrix", "modelMatrix", "normalMatrix"}},
        // its projection, transform and culling (not its render target, sorting or depth test)
        {"Camera", {
            "activate", "setOrtho", "setPerspective", "setType", "setTarget", "disableTarget",
            "isUsingTarget", "setUp", "getUp", "getDirection", "getRight", "getWorldTarget",
            "getWorldDirection", "getWorldUp", "getWorldRight", "getViewMatrix", "getProjectionMatrix",
            "getViewProjectionMatrix", "rotateView", "rotatePosition", "elevateView", "elevatePosition",
            "walkForward", "zoom", "slide", "slideForward", "slideUp", "screenToRay",
            "getDistanceFromTarget", "updateCamera", "autoResize", "nearClip", "farClip", "leftClip",
            "rightClip", "bottomClip", "topClip", "aspect", "yfov", "type", "target", "up", "cullingMask"}},
    };
    return classes;
}

bool ShaderNeutralApi::isNeutralClass(const std::string& name) {
    // the event objects (FunctionSubscribe_*) only call back
    return neutralClasses().count(name) > 0 || name.rfind("FunctionSubscribe_", 0) == 0;
}

uint32_t ShaderNeutralApi::memberHash(const std::set<std::string>& names) {
    uint32_t hash = 0x811c9dc5u;
    for (const std::string& name : names) {
        for (unsigned char c : name) hash = (hash ^ c) * 0x01000193u;
        hash = (hash ^ (unsigned char)'\n') * 0x01000193u;
    }
    return hash;
}
