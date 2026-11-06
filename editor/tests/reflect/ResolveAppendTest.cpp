// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A property path a write resolves may name one past the last element of a component array that
// grows by index (the Inspector's Add buttons and MCP append that way), and not further
// (mesh-memory spec 6.6). It was refused at the array's starting size: a sprite of 128 frames
// could not take a 129th. A read never appends.

#include "component/SpriteComponent.h"
#include "reflect/Reflect.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    SpriteComponent spriteOf(unsigned int frames){
        SpriteComponent sprite;
        sprite.numFramesRect = frames;
        for (unsigned int i = 0; i < frames; i++)
            sprite.framesRect[i] = {"frame" + std::to_string(i), Rect(0.0f, 0.0f, 8.0f, 8.0f)};
        return sprite;
    }

}

TEST(ReflectResolve, IndexAtTheCountAppendsToAGrowArray){
    SpriteComponent sprite = spriteOf(128);
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::SpriteComponent);
    reflect::PathError error = reflect::PathError::None;
    reflect::FieldRef ref = reflect::resolve(desc, &sprite, "framesRect[128].name", &error, true);
    ASSERT_TRUE(ref.ref) << "framesRect[128] is refused (error " << static_cast<int>(error) << ")";
    *static_cast<std::string*>(ref.ref) = "frame128";
    EXPECT_EQ(sprite.framesRect[128].name, "frame128");
}

TEST(ReflectResolve, PastTheCountIsOutOfRange){
    SpriteComponent sprite = spriteOf(128);
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::SpriteComponent);
    reflect::PathError error = reflect::PathError::None;
    reflect::FieldRef ref = reflect::resolve(desc, &sprite, "framesRect[130].name", &error, true);
    EXPECT_FALSE(ref.ref);
    EXPECT_EQ(error, reflect::PathError::IndexOutOfRange);
}

// A read of one past the last element (the Inspector drawing, an agent's get, a revert's lookup)
// names nothing and grows nothing: it appended a default frame
TEST(ReflectResolve, AReadAtTheCountGrowsNothing){
    SpriteComponent sprite = spriteOf(128);
    const reflect::ComponentDesc& desc = *reflect::find(ComponentType::SpriteComponent);
    reflect::PathError error = reflect::PathError::None;
    EXPECT_FALSE(reflect::resolve(desc, &sprite, "framesRect[128].name", &error).ref);
    EXPECT_EQ(error, reflect::PathError::IndexOutOfRange);
    EXPECT_FALSE(reflect::property(desc, &sprite, "framesRect[128].name").ref);
    EXPECT_EQ(sprite.framesRect.stored(), 128u);
    EXPECT_EQ(sprite.numFramesRect, 128u);
}
