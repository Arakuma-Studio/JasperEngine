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
        {"AnimationCurve", 0x2b5181f7u},
        {"GradientColorKey", 0x8fd5ad9fu},
        {"GradientAlphaKey", 0x51d0e120u},
        {"Gradient", 0xb160bd69u},
        {"MinMaxCurve", 0x9309ffd5u},
        {"MinMaxGradient", 0x18ba741eu},
        {"Ease", 0x9897fd4eu},
        {"LayerMask", 0xc0bc567fu},
        {"Input", 0xa4979e1cu},
        {"InputActionEvent", 0x0f075b4cu},
        {"InputBinding", 0xca780f92u},
        {"InputAction", 0x82b8c81bu},
        {"InputActionMap", 0xcddbff0bu},
        {"InputActions", 0x74622d4du},
        {"Log", 0x2ff1e011u},
        {"LogCategory", 0xe96f7290u},
        {"System", 0xb2945894u},
        {"FileData", 0x80c088d6u},
        {"File", 0x89110c2bu},
        {"Data", 0x812fbef9u},
        {"UserSettings", 0x4807e06eu},
        {"ThreadPoolManager", 0x47d517d2u},
        {"ResourceProgress", 0x71d03622u},
        {"ResourceBuildInfo", 0x044de46au},
        {"OverallBuildProgress", 0x2336bf6fu},
        {"Physics", 0xc8ecb88du},
        {"Physics2D", 0xc8ecb88du},
        {"PhysicsSystem", 0xb021d2f4u},
        {"Rigidbody", 0x212aced5u},
        {"Rigidbody2D", 0xb3c72e21u},
        {"Joint2D", 0x2981985bu},
        {"Joint3D", 0x6b1ad9bau},
        {"Contact2D", 0x9896e5eau},
        {"Contact3D", 0xb3ded249u},
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
        {"AudioSource", 0xda21e7bbu},
        {"AudioSystem", 0x6bd206a0u},
        {"AudioSourceComponent", 0x379fd279u},
        {"NavMeshQueryFilter", 0x811f5959u},
        {"GridCell", 0xa297eb80u},
        {"NavigationSystem", 0x26e60eaau},
        {"NavMeshAgent", 0xd22cd366u},
        {"NavMeshSurface", 0x6c97bf5au},
        {"NavGrid", 0x5911c33cu},
        {"Animator", 0x04f2822cu},
        {"AnimationPlayer", 0x3164042eu},
        {"PoseAnimator", 0xe172172au},
        {"AnimationEvent", 0x3191cc4eu},
        {"AnimationState", 0x95340871u},
        {"AnimatorStateInfo", 0x8d600b48u},
        {"SceneManager", 0xfdb4d350u},
        {"PrefabManager", 0x49a7b220u},
        {"PrefabAsset", 0xd4cce93du},
        {"AssetRef", 0x0fdd37b1u},
        {"EntityRegistry", 0x6e532592u},
        {"EntityManager", 0x11ab3e29u},
        {"EntityHandle", 0x6b5c2567u},
        {"Action", 0xecfbfddeu},
        {"Tween", 0x23ed85c4u},
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
        {"TweenComponent", 0x4c56eb7eu},
        {"ActionSystem", 0x1d297e9du},
        {"AnimationPlayerComponent", 0x46232d9fu},
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
