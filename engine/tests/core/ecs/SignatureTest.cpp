// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Signature: a 256-bit ComponentMask with the std::bitset subset the engine uses, its string
// form (MSB first, 64-character legacy strings accepted) and the 256-type registration limit.

#include "ecs/Signature.h"
#include "ecs/ComponentManager.h"

#include <gtest/gtest.h>

#include <random>

#include <bitset>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

TEST(SignatureTest, HighAndLowBits){
    Signature signature;
    EXPECT_TRUE(signature.none());
    for (size_t bit : {0u, 63u, 64u, 255u}){
        EXPECT_FALSE(signature.test(bit));
        signature.set(bit);
        EXPECT_TRUE(signature.test(bit)) << bit;
    }
    EXPECT_EQ(signature.count(), 4u);
    EXPECT_TRUE(signature.any());
    signature.reset(63);
    EXPECT_FALSE(signature.test(63));
    EXPECT_TRUE(signature.test(64));
    signature.set(64, false);
    EXPECT_FALSE(signature.test(64));
    EXPECT_EQ(signature.count(), 2u);
    EXPECT_EQ(Signature::size(), 256u);
    // out of range: nothing happens, nothing is set
    signature.set(256);
    EXPECT_FALSE(signature.test(256));
    EXPECT_EQ(signature.count(), 2u);
}

TEST(SignatureTest, Operators){
    Signature a, b;
    a.set(3).set(100);
    b.set(100).set(200);
    EXPECT_EQ((a & b).count(), 1u);
    EXPECT_TRUE((a & b).test(100));
    EXPECT_EQ((a | b).count(), 3u);
    EXPECT_TRUE(a.intersects(b));
    EXPECT_FALSE(a.intersects(Signature().set(4)));
    Signature c = a;
    EXPECT_TRUE(c == a);
    c |= b;
    EXPECT_TRUE(c != a);
    EXPECT_TRUE(c.contains(a));
    c &= b;
    EXPECT_TRUE(c == b);
    EXPECT_TRUE(Signature().set().count() == 256u);
    // count against a bit-by-bit count (the SWAR path on x86 without -mpopcnt)
    std::mt19937 random(7);
    for (int round = 0; round < 200; round++){
        Signature sig;
        size_t expected = 0;
        for (size_t bit = 0; bit < kMaxComponentTypes; bit++){
            if (random() % (round % 7 + 1) == 0){ sig.set(bit); expected++; }
        }
        EXPECT_EQ(sig.count(), expected);
    }
}

TEST(SignatureTest, ForEachSetOrder){
    Signature signature;
    const std::vector<size_t> bits{0, 1, 62, 63, 64, 127, 128, 200, 255};
    for (auto it = bits.rbegin(); it != bits.rend(); ++it){
        signature.set(*it);
    }
    std::vector<size_t> visited;
    signature.forEachSet([&visited](size_t bit){ visited.push_back(bit); });
    EXPECT_EQ(visited, bits);
}

TEST(SignatureTest, StringRoundTrip){
    Signature signature;
    signature.set(0).set(5).set(130).set(255);
    const std::string text = signature.to_string();
    ASSERT_EQ(text.size(), 256u);
    EXPECT_EQ(text[0], '1');       // bit 255, MSB first
    EXPECT_EQ(text[255], '1');     // bit 0
    EXPECT_EQ(text[255 - 5], '1');
    EXPECT_EQ(Signature(text), signature);
    Signature parsed;
    EXPECT_TRUE(Signature::fromString(text, parsed));
    EXPECT_EQ(parsed, signature);
}

TEST(SignatureTest, LegacyStringParsesToLowBits){
    std::bitset<64> legacy;
    legacy.set(0).set(10).set(63);
    const Signature signature(legacy.to_string());
    EXPECT_EQ(signature.count(), 3u);
    EXPECT_TRUE(signature.test(0));
    EXPECT_TRUE(signature.test(10));
    EXPECT_TRUE(signature.test(63));
    EXPECT_EQ(signature.to_string().substr(256 - 64), legacy.to_string());

    Signature unchanged;
    unchanged.set(1);
    EXPECT_FALSE(Signature::fromString("10x1", unchanged));
    EXPECT_TRUE(unchanged.test(1));
    EXPECT_THROW(Signature("12"), std::invalid_argument);
}

namespace {

    template<int N>
    struct Marker {
        int value = N;
    };

    template<int... N>
    std::vector<bool> registerMarkers(ComponentManager& manager, std::integer_sequence<int, N...>){
        return {manager.registerComponent<Marker<N>>()...};
    }

}

TEST(SignatureTest, TwoHundredFiftySeventhTypeFailsCleanly){
    ComponentManager manager;
    const std::vector<bool> registered = registerMarkers(manager, std::make_integer_sequence<int, 256>{});
    for (size_t i = 0; i < registered.size(); i++){
        ASSERT_TRUE(registered[i]) << i;
    }
    EXPECT_EQ(manager.getRegisteredCount(), 256u);
    EXPECT_EQ(manager.getComponentId<Marker<255>>(), (ComponentId)255);

    EXPECT_FALSE(manager.registerComponent<Marker<256>>());
    EXPECT_FALSE(manager.isRegistered<Marker<256>>());
    EXPECT_EQ(manager.getRegisteredCount(), 256u);
    EXPECT_THROW(manager.getComponentId<Marker<256>>(), std::out_of_range);
    // the ids did not wrap: the first type keeps bit 0
    EXPECT_EQ(manager.getComponentId<Marker<0>>(), (ComponentId)0);
}
