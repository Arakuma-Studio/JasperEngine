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
#include <string>
#include <vector>

namespace jasperengine{
    struct SpriteSheetData;

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

        // A sheet sprite (docs/specs/S44-aseprite-import.md R38, R51): its frames come from a
        // sprite sheet (an .aseprite file, a texture). setFrame(index|name) works on it too.
        // "" goes back to the inline frames.
        void setSheet(const std::string& path);
        std::string getSheet() const;
        // the sheet (loaded now when the sprite has not drawn it yet); null without one
        std::shared_ptr<const SpriteSheetData> getSheetData() const;
        int getFrameIndex() const;
        std::string getFrameName() const;
        // the sheet's frames, or the inline ones
        int getFrameCount() const;

        // A sheet tag at the sheet's durations, following its direction and repeat (repeat 0
        // loops); `speed` scales time. No clip events (the Animation Player has them).
        void playTag(const std::string& name, float speed = 1.0f);
        void stopTag();
        std::string getTag() const;          // the tag playing (or that finished last), "" for none
        bool isTagPlaying() const;
        // the slice's box on the shown frame in the GameObject's local units (y up), with its
        // centre and pivot in the same units; false when absent or hidden on that frame
        bool getSliceRect(const std::string& name, Rect& bounds, Rect* center = nullptr, Vector2* pivot = nullptr) const;

    };
}

#endif //SPRITE_H
