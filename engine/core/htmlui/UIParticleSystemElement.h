// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef UIPARTICLESYSTEMELEMENT_H
#define UIPARTICLESYSTEMELEMENT_H

#include "Export.h"
#include "Entity.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Rect.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Texture.h>

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    // One particle as the element draws it: the emitter's local position (y up, prefab units),
    // its width (and its height when that is not negative: a 3D start size), rotation (radians,
    // counter-clockwise in the emitter's plane), colour (linear) and sprite frame (uv rect; a
    // mirrored particle's runs backwards)
    struct UIParticleQuad {
        Vector3 position;
        float size = 1.0f;
        float height = -1.0f;
        float rotation = 0.0f;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Rect textureRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    };

    // What a system's particles are drawn with: its Renderer module's texture (a path, empty:
    // none), its blend, and the built-in soft disc of an untextured billboard
    struct UIParticleMaterial {
        std::string texturePath;
        bool additive = false;
        bool softDisc = false;
    };

    // One sub-emitter child's particles and what its own Renderer draws them with
    struct UIParticleBatch {
        UIParticleMaterial material;
        std::vector<UIParticleQuad> quads;
    };

    // The particles of a Particle System (docs/specs/S20-particles.md: it renders itself) as its
    // billboards show them this frame: the render extraction's records (interpolated position,
    // size, rotation, colour, sprite frame), in its GameObject's local space. The seam the
    // <particle-system> element reads through.
    namespace UIParticleSource {
        // false when `emitter` has no Particle System, or no effect it can run (nothing drawn)
        JASPERENGINE_API bool read(Scene* scene, Entity emitter, std::vector<UIParticleQuad>& out);
        // false without a Particle System
        JASPERENGINE_API bool material(Scene* scene, Entity emitter, UIParticleMaterial& out);
        // its sub-emitter children, in the effect's order down the tree (each in the same space
        // as read's); empty, with nothing allocated, without any
        JASPERENGINE_API void readChildren(Scene* scene, Entity emitter, std::vector<UIParticleBatch>& out);
    }

    // <particle-system src="fx/confetti.prefab" autoplay loop time="ui|game" scale="1">: a
    // replaced inline-block element that plays a prefab's Particle System on a hidden runtime
    // entity of its document's scene (its ParticleSubsystem simulates it) and draws the particles
    // as one mesh in document order, inside the element's clip and transform. Default size 0x0,
    // the emitter at the content box's centre, one prefab unit = `scale` CSS px, y up.
    // time="ui" (the default) is the system's useUnscaledTime, time="game" turns the effect's
    // own Unscaled Time off too; loop= sets the effect's Looping (the GameObject's own copy).
    class JASPERENGINE_INTERNAL UIParticleSystemElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(UIParticleSystemElement, Rml::Element)

        explicit UIParticleSystemElement(const Rml::String& tag);
        ~UIParticleSystemElement() override;

        static void registerInstancer();
        static void releaseInstancer();
        // the prefabs of `scene`'s elements no longer in a document (detached by a script, or
        // with an ancestor) go; such an element makes its prefab again once attached
        static void releaseDetached(Scene* scene);

        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;

        void play();
        void stop();
        void pause();
        void clear();
        bool isPlaying() const;
        // play / pause / stop before the prefab is made are queued (the last one applies)
        // the hidden entity's Particle System (NULL_ENTITY before it is made or without one)
        Scene* getScene() const;
        Entity getParticleEntity() const;
        // the source's load error ("" = none)
        const std::string& getError() const;

        // the quads of the last frame, in the element's content space (tests)
        size_t getDrawnParticleCount() const;

    protected:
        void OnUpdate() override;
        void OnRender() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

    private:
        void destroyEntity();
        bool instantiate();
        void applyAttributes();
        void fail(const std::string& message);
        // what one material's quads were last drawn with
        struct DrawState {
            Rml::Geometry geometry;
            Rml::Texture texture;
            // the soft disc of an untextured system, made here (kept alive while it is drawn)
            Rml::CallbackTexture discTexture;
            std::string loadedTexture;
            bool loadedDisc = false;
        };
        void renderQuads(Rml::RenderManager* renderManager, const UIParticleMaterial& quadMaterial, const std::vector<UIParticleQuad>& quadList, DrawState& state);

        enum class Command { None, Play, Pause, Stop };
        Command pending = Command::None;

        Scene* scene = nullptr;
        std::weak_ptr<char> sceneLifetime;
        Entity root = NULL_ENTITY;      // the prefab's root (hidden)
        Entity system = NULL_ENTITY;    // the GameObject with the Particle System (it is the emitter)
        bool sourceDirty = true;
        bool completeSent = false;
        bool wasRunning = false;
        std::string error;
        UIParticleMaterial material;

        std::vector<UIParticleQuad> quads;
        // the sub-emitter children's, each with its own Renderer
        std::vector<UIParticleBatch> childBatches;
        std::vector<DrawState> childDraws;
        size_t drawn = 0;
        DrawState rootDraw;
    };

}

#endif // UIPARTICLESYSTEMELEMENT_H
