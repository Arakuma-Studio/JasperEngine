// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_SYSTEM_H
#define TITLE_ART_SYSTEM_H

// Contract (docs/specs/S81-title-art-runtime.md R1-R8, R11, R17-R19, R33-R38, R41, R51)
// - Purpose: runs the scene's Title Art Renderers. Always built (core): a Baked title, the live
//   layers and the sidecar's anchors need nothing of the `titleart` module; Realtime and Glyph
//   Atlas titles go through TitleArtRuntime (the module, or its stub that shows the bake).
// - The companion (R1, R2): an Image (UIComponent + ImageComponent: UI space) or a Sprite
//   (MeshComponent + SpriteComponent: world space) on the same GameObject. The title's image is
//   lent at draw time (getDisplayTexture, FluidSystem's pattern): the companion's own texture is
//   never written, so nothing of it is saved or becomes a prefab override. With a Live Effect on,
//   the companion's custom shader becomes library:title_fx (UI) / library:title_fx_sprite
//   (world) with its uniforms; what it had is kept (getSavedCompanion: the editor's encoders save
//   that) and put back when the effects go or the component is removed.
// - Idle (R48): a title whose state did not change does no work but the per-entity checks, and
//   with no Live Effect the companion keeps its built-in shader: the same draws, passes and
//   bindings as an Image with that texture.
// - Live Time (R38): liveTime advances by scaled (or unscaled) time in Play only; no live layer
//   reads engine time, so two --fixed-fps runs give the same pixels.
// - Clip events (R41): subscribes, tag "titleart", to the onEvent of the Animation Player and the
//   Animator on each title's GameObject and its ancestors; the reserved titleArt* events change
//   the title, and count as handled.
// - Thread-safety: main thread.

#include "ecs/SubSystem.h"
#include "component/TitleArtRendererComponent.h"
#include "subsystem/TitleArtInstance.h"
#include "math/Vector4.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::titleanim{
    struct TitleAnimPlayOptions;
    class TitleAnimPlayer;
    struct CompiledMarker;
}

namespace jasperengine{

    class TextureRender;
    class EntityRegistry;
    struct AnimationEvent;

    // a value a script, a clip event or a tool gives an Exposed parameter (R64, R41)
    struct JASPERENGINE_API TitleArtValue{
        enum class Kind : uint8_t { Number, Bool, String, Color };
        Kind kind = Kind::Number;
        double number = 0.0;
        bool boolean = false;
        std::string text;
        Vector4 color = Vector4(0.0f, 0.0f, 0.0f, 1.0f);   // linear rgb
        bool colorHasAlpha = false;

        static TitleArtValue fromNumber(double value);
        static TitleArtValue fromBool(bool value);
        static TitleArtValue fromString(std::string value);
        static TitleArtValue fromColor(const Vector4& linear, bool hasAlpha = false);
        // "number", "boolean", "string", "Vector4"
        const char* typeName() const;
    };

    // The editor's saves, undo snapshots and exported code: while one lives, the companion of
    // `entity` holds its own values (those a Live Effect replaced: custom shader, uniforms,
    // shader textures and features, R2); the driven ones come back when it goes. Nothing when
    // no title drives the companion. Main thread.
    class JASPERENGINE_API TitleArtCompanionScope{
    public:
        TitleArtCompanionScope(const EntityRegistry* registry, Entity entity);
        ~TitleArtCompanionScope();
        TitleArtCompanionScope(const TitleArtCompanionScope&) = delete;
        TitleArtCompanionScope& operator=(const TitleArtCompanionScope&) = delete;

    private:
        EntityRegistry* registry = nullptr;
        Entity entity = NULL_ENTITY;
        bool swapped = false;
        void swap();
    };

    class JASPERENGINE_API TitleArtSystem : public SubSystem {
    public:
        explicit TitleArtSystem(Scene* scene);
        virtual ~TitleArtSystem();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // RenderSystem's draws: the image an entity's Image or Sprite shows instead of its own
        // texture (null: its own); `transparent` is set (straight alpha, blended)
        TextureRender* getDisplayTexture(Entity entity, bool* transparent = nullptr) const;
        bool hasDisplayTextures() const;
        // RenderSystem, before the camera passes: the Realtime and Glyph Atlas renders of the
        // frame (once per engine frame, whichever scene asks first)
        void renderTitles();

        // the editor's encoders and exported code: the companion's own values while a Live
        // Effect drives them (null when nothing is driven)
        static const titleart::TitleArtCompanionSaved* getSavedCompanion(const EntityRegistry* registry, Entity entity);
        // the state of a component (made on first use)
        static titleart::TitleArtInstance& instanceOf(TitleArtRendererComponent& title);

        // the script events of every title: gone at Stop before the C++ script library is unloaded
        // (Engine::clearComponentSubscriptions), or by tag on a hot reload (Scene::removeSubscriptionsByTag)
        void clearSubscriptions();
        void removeSubscriptionsByTag(const std::string& substring);

        // ---- the API Lua, C++ scripts, clip events and tools share (R64-R67) ----

        // Sets one Exposed parameter's override: false with `error` (R65's sentences, `name`
        // is the GameObject's); `warning` is set for a clamp or a cut text
        static bool setParameter(EntityRegistry* scene, Entity entity, const std::string& name, const TitleArtValue& value,
                                 std::string& error, std::string* warning = nullptr);
        // the effective value (R5): the override, else the document's
        static bool getParameter(EntityRegistry* scene, Entity entity, const std::string& name, TitleArtValue& out, std::string& error);
        static bool hasParameter(EntityRegistry* scene, Entity entity, const std::string& name);
        static bool resetParameter(EntityRegistry* scene, Entity entity, const std::string& name, std::string& error);
        static std::vector<std::string> parameterNames(EntityRegistry* scene, Entity entity);
        // the color scheme ids an instance may pick (the exposed parameter's options)
        static std::vector<std::string> colorSchemes(EntityRegistry* scene, Entity entity);
        // sizes the companion to the bake frame (R3); false with `error`
        static bool setNativeSize(EntityRegistry* scene, Entity entity, std::string& error);
        // the size Set Native Size gives (UI units or world units) and the pivot (top-left
        // convention), false when the title has no frame yet
        static bool nativeSize(EntityRegistry* scene, Entity entity, float& width, float& height, float& pivotX, float& pivotY);
        // R9: the resolution a Realtime title renders at (display scale x resolutionScale, rounded
        // to 1/8): the bake's output scale, times how large a UI companion shows the frame in
        // reference units (a world title renders at its canvas pixels). The export's prebake uses it.
        // `referenceScale`: a UI title's output pixels per canvas pixel for the Reference Height (R11)
        static float resolutionFor(const TitleArtBakeInfo& bake, bool ui, unsigned int rectWidth, float uiScale, float resolutionScale,
                                   float referenceScale = 1.0f);
        // re-render (Realtime, Glyph Atlas); nothing on Baked
        static void refresh(EntityRegistry* scene, Entity entity);
        // Engine::areScenesLoaded: every enabled Realtime or Glyph Atlas title that renders now
        // shows its state's image (or failed): the "Engine ready" line, the Recorder and smoke
        // screenshots wait for them; `waiting` names the first that does not ("Logo rendering 3/7")
        bool areTitlesReady(std::string* waiting = nullptr) const;
        // S86 R63: the mesh a Realtime title's 3D layer `layerPath` drew last; false with `error`
        // (a Baked title, a path that names no 3D layer), true with `found` false before a render
        static bool meshStats(EntityRegistry* scene, Entity entity, const std::string& layerPath, titleart::TitleArtMeshStats& out, bool& found,
                              std::string& error);
        static std::string displayName(EntityRegistry* scene, Entity entity);

        // the frame budget scripts set (TitleArt.setFrameBudget), 0.25..16 ms
        // the Realtime scheduler's steps of this frame, once a frame whoever asks first: a scene
        // that draws titles, or the Title Designer's Glyph Atlas preview while no scene draws
        static void runRuntimeFrame(bool editorEditMode);
        static void setFrameBudget(double ms);
        static double getFrameBudget();

        // R55: the editor's Play simulates the build: a document's Characters set (UTF-8, every
        // character once); "" clears it. Realtime and Glyph Atlas titles then draw characters
        // outside it (and outside their Additional Characters) as nothing
        static void setSimulatedCharacters(const std::string& titlePath, const std::string& characters);
        static void clearSimulatedCharacters();

        // R23: a committed change of a document or of an asset it names ("" = every title):
        // every instance resolves again (the editor's reload and document hooks)
        static void notifyTitleChanged(const std::string& titlePath);

        // ---- S85's animations (docs/specs/S85-title-animation.md R24-R31, §6.5) ----
        // false (one warning per name and component, R30) for a name the title does not have
        static bool playAnimation(EntityRegistry* scene, Entity entity, const std::string& name, const titleanim::TitleAnimPlayOptions& options,
                                  bool overlay = false);
        // "" stops every animation (the pose stays); a name stops that one; false when it does not play
        static bool stopAnimation(EntityRegistry* scene, Entity entity, const std::string& name);
        static void resetAnimation(EntityRegistry* scene, Entity entity);
        // R49: the default animation running in Edit mode, silently (no markers fire); stop resets it
        static bool startAnimationEditPreview(EntityRegistry* scene, Entity entity);
        static void stopAnimationEditPreview(EntityRegistry* scene, Entity entity);
        static bool isAnimationEditPreviewing(EntityRegistry* scene, Entity entity);
        static bool animationHasMarkerSounds(EntityRegistry* scene, Entity entity);
        static bool sampleAnimation(EntityRegistry* scene, Entity entity, const std::string& name, double time);
        // Realtime prerender and Glyph Atlas cells asked for now; true when it plays at once
        static bool prepareAnimation(EntityRegistry* scene, Entity entity, const std::string& name);
        static bool isAnimationReady(EntityRegistry* scene, Entity entity, const std::string& name);
        static std::vector<std::string> animationNames(EntityRegistry* scene, Entity entity);
        static double animationDuration(EntityRegistry* scene, Entity entity, const std::string& name);
        // the animations as the instance plays them (loaded on first use); null when none
        static titleanim::TitleAnimPlayer* animationPlayer(EntityRegistry* scene, Entity entity);

        // tests: TITLE_ART_TEST_* flags copied into every request of the instance (R21)
        static void setDisabledFeaturesForTesting(EntityRegistry* scene, Entity entity, uint32_t flags);

        // ---- the editor ----
        // the title's space by its companion
        static titleart::TitleArtSpace spaceOf(EntityRegistry* registry, Entity entity);
        // both an Image and a Sprite: the Image wins (the Inspector warns)
        static bool hasBothCompanions(EntityRegistry* registry, Entity entity);

    private:
        struct State;
        std::unique_ptr<State> state;
        // S85 R18: the titles whose animation plays this frame (their share of the frame budget)
        int animatingTitles = 0;

        void resolve(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void updateTitle(Entity entity, TitleArtRendererComponent& title, double dt);
        void updateLive(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void updateLiveEffects(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void applyLending(Entity entity, titleart::TitleArtInstance& instance, bool lending, bool realtime);
        void restoreCompanion(Entity entity, titleart::TitleArtInstance& instance);
        void updateStatus(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        // S87 R12-R15, R27: which language the title shows, and the Baked switch to its set
        void updateLanguage(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void finishLanguageSwitch(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void subscribeEvents(Entity entity);
        // S85: the library, the frame's player update and its markers, the companion's draw values
        static void loadAnimations(EntityRegistry* scene, Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance);
        void updateAnimation(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance, double dt);
        void dispatchAnimationEvents(Entity entity, TitleArtRendererComponent& title, titleart::TitleArtInstance& instance, double dt);
        void playMarkerSound(Entity entity, const titleanim::CompiledMarker& marker, float fraction, double dt);
        void handleEvent(Entity player, const AnimationEvent& event, const std::string& source);
        bool isVisible(Entity entity, const TitleArtRendererComponent& title, const titleart::TitleArtInstance& instance) const;
        float displayResolution(Entity entity, const TitleArtRendererComponent& title, const titleart::TitleArtInstance& instance) const;
    };

}

#endif // TITLE_ART_SYSTEM_H
