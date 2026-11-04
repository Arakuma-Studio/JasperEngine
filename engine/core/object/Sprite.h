// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_H
#define SPRITE_H

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "action/SpriteAnimation.h"

#include "Mesh.h"
#include "ShadowCaster2D.h"

#include <memory>
#include "component/SpriteComponent.h"

namespace jasperengine{
    class JASPERENGINE_API Sprite: public Mesh{

    private:
        // The action startAnimation makes (it owns an entity of its own). Copies of the
        // wrapper share it, as they view the same sprite: whichever drives the animation
        // drives that one, and it goes with the last wrapper holding it.
        std::shared_ptr<SpriteAnimation> animation;

        SpriteAnimation& ensureAnimation();

        void applyFrameRect(Rect frameRect);

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Sprite is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Sprite(Scene* scene);
        Sprite(Scene* scene, Entity entity);
        virtual ~Sprite();

        Sprite(const Sprite& rhs);
        Sprite& operator=(const Sprite& rhs);
        Sprite(Sprite&& rhs) noexcept;
        Sprite& operator=(Sprite&& rhs) noexcept;

        bool createSprite();

        ShadowCaster2D getShadowCaster2D();
        void removeShadowCaster2D();

        void setSize(unsigned int width, unsigned int height);
        void setWidth(unsigned int width);
        void setHeight(unsigned int height);

        unsigned int getWidth() const;
        unsigned int getHeight() const;

        void setFlipY(bool flipY);
        bool isFlipY() const;

        void setTextureScaleFactor(float textureScaleFactor);
        float getTextureScaleFactor() const;

        void setTextureRect(float x, float y, float width, float height);
        void setTextureRect(Rect textureRect);
        Rect getTextureRect() const;

        void setPivotPreset(PivotPreset pivotPreset);
        PivotPreset getPivotPreset() const;

        void addFrame(int id, const std::string& name, Rect rect);
        void addFrame(const std::string& name, float x, float y, float width, float height);
        void addFrame(float x, float y, float width, float height);
        void addFrame(Rect rect);
        void removeFrame(int id);
        void removeFrame(const std::string& name);

        void setFrame(int id);
        void setFrame(const std::string& name);

        void startAnimation(std::vector<int> frames, std::vector<int> framesTime, bool loop);
        void startAnimation(int startFrame, int endFrame, int interval, bool loop);
        void startAnimation(const std::string& name, int interval, bool loop);
        void pauseAnimation();
        void resumeAnimation();
        void stopAnimation();

    };
}

#endif //SPRITE_H
