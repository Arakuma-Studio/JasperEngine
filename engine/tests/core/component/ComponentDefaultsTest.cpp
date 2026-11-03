// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Fields every component starts with. The editor decodes a component into a local
// (`RigidbodyComponent body;`), which is default-initialised: a field without an initialiser keeps
// whatever the stack held, and the generated scene code of an export wrote that garbage, so two
// exports of one project differed. Each type is made here over memory filled with 0xA5 first.

#include <gtest/gtest.h>

#include "Engine.h"
#include "component/AlphaTweenComponent.h"
#include "component/BoneComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TilemapComponent.h"

#include <cstddef>
#include <new>

using namespace jasperengine;

namespace {

    // `check` sees a T default-initialised (as `T value;` is) over bytes of 0xA5
    template <typename T, typename Check>
    void defaultInitialisedOverGarbage(Check check) {
        alignas(T) unsigned char storage[sizeof(T)];
        // volatile: the compiler may not drop stores to memory an object is then made in
        volatile unsigned char* bytes = storage;
        for (size_t i = 0; i < sizeof(T); i++) bytes[i] = 0xA5;
        T* value = new (storage) T;
        check(*value);
        value->~T();
    }

}

TEST(ComponentDefaults, RigidbodyOverrideMassBoxHasItsDensity) {
    defaultInitialisedOverGarbage<RigidbodyComponent>([](const RigidbodyComponent& body) {
        EXPECT_FLOAT_EQ(body.solidBoxDensity, 1000.0f) << "Collider3D's default density, kg/m3";
        EXPECT_FLOAT_EQ(body.solidBoxSize.x, 0.0f);
        EXPECT_FLOAT_EQ(body.solidBoxSize.y, 0.0f);
        EXPECT_FLOAT_EQ(body.solidBoxSize.z, 0.0f);
    });
}

TEST(ComponentDefaults, BoneIndexStartsAtZero) {
    defaultInitialisedOverGarbage<BoneComponent>([](const BoneComponent& bone) {
        EXPECT_EQ(bone.index, 0);
    });
}

TEST(ComponentDefaults, TileDataStartsEmpty) {
    defaultInitialisedOverGarbage<TileData>([](const TileData& tile) {
        EXPECT_EQ(tile.rectId, 0);
        EXPECT_FLOAT_EQ(tile.width, 0.0f);
        EXPECT_FLOAT_EQ(tile.height, 0.0f);
    });
}

TEST(ComponentDefaults, AlphaTweenStartsAtZeroAlphas) {
    defaultInitialisedOverGarbage<AlphaTweenComponent>([](const AlphaTweenComponent& tween) {
        EXPECT_FLOAT_EQ(tween.startAlpha, 0.0f);
        EXPECT_FLOAT_EQ(tween.endAlpha, 0.0f);
    });
}
