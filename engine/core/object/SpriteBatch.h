// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Sprite Batch of a Sprite GameObject (docs/specs/S52-sprite-instancing.md R10-R12, §6.6): Lua
// `SpriteBatch(scene, entity)` and C++ scripts.
// - SpriteBatch(scene, entity) wraps the GameObject's Sprite Batch and ADDS one when it has none
//   (the GameObject must have a Sprite: its frames, material and pivot are the instances').
// - The bulk calls write whole streams from flat arrays and validate once (length, frame range):
//   setPositions sets the count; the other streams cover the first `count` instances (a shorter
//   array is an error). An error is returned as text ("" on success), nothing is changed then.
// - Instances are runtime data: never saved, empty in a copy.
// - Main thread only (ECS).

#ifndef SPRITE_BATCH_H
#define SPRITE_BATCH_H

#include "object/EntityHandle.h"
#include "component/SpriteBatchComponent.h"

#include <cstdint>
#include <span>
#include <string>

namespace jasperengine{

    class JASPERENGINE_API SpriteBatch: public EntityHandle{

    public:
        SpriteBatch(Scene* scene, Entity entity);
        virtual ~SpriteBatch();

        SpriteBatch(const SpriteBatch& rhs);
        SpriteBatch& operator=(const SpriteBatch& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setMode(SpriteBatchMode mode);
        SpriteBatchMode getMode() const;
        void setSpace(SpriteBatchSpace space);
        SpriteBatchSpace getSpace() const;
        void setCapacity(uint32_t capacity);
        uint32_t getCapacity() const;
        void setSortInstances(bool sortInstances);
        bool isSortInstances() const;

        // the instances drawn
        uint32_t getCount() const;
        // the Sprite's frames: their count (-1 while its sheet is not loaded) and an index by name
        // (-1 when the Sprite has no such frame)
        int getFrameCount() const;
        int frameId(const std::string& name) const;

        // whole streams: positions as x, y pairs (stride 2) or x, y, z triples (stride 3), which
        // sets the count; rotations in radians about z; scales uniform (stride 1) or x, y pairs
        // (stride 2); frame indices; sRGB r, g, b, a colours (as Sprite colours); one FX slot
        // ("palette", "flash", "fade" or "dissolve") of every instance
        std::string setPositions(std::span<const float> values, int stride = 2);
        std::string setRotations(std::span<const float> radians);
        std::string setScales(std::span<const float> values, int stride = 1);
        std::string setFrames(std::span<const int32_t> frames);
        std::string setColors(std::span<const float> rgba);
        std::string setFx(const std::string& slot, std::span<const float> amounts);

        // small edits: add returns the new instance's index (-1 and an error logged when the frame
        // is out of range)
        int add(float x, float y, int frame = 0);
        bool set(uint32_t index, float x, float y);
        bool setFrame(uint32_t index, int frame);
        // the last instance takes the removed one's index (order is not kept)
        bool remove(uint32_t index);
        void clear();

        // the slot's index (0-3) of an FX name, -1 when unknown
        static int fxSlot(const std::string& name);

    private:
        // the state scripts write, made on the first write; the first write of a frame clears a
        // Clear Each Frame batch drawn in an earlier frame
        SpriteBatchState& writeState();
        std::string checkFrames(std::span<const int32_t> frames) const;
        void written(SpriteBatchState& state);
    };

}

#endif //SPRITE_BATCH_H
