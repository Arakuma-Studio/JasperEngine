// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FunctionSubscribe.h"
#include "util/Scheduler.h"
#include "util/Signal.h"
#include "util/SubscriptionOwnerIndex.h"

#include <gtest/gtest.h>

#include <memory>

using namespace jasperengine;

TEST(OwnerIndexTest, ReleaseRemovesSubscriptionsAndTimers) {
    int script = 0;
    const uint64_t owner = ownerKeyOf(&script);
    FunctionSubscribe<void()> onUpdate;
    FunctionSubscribe<void(int)> onClick;
    Signal<> signal;
    int calls = 0;
    onUpdate.add("Script_onUpdate", [&]() { calls++; }, owner);
    onClick.add("myTag", [&](int) { calls++; }, owner);
    Subscription kept = signal.subscribe([&]() { calls++; }, owner);
    onUpdate.add("someone else", [&]() { calls += 100; });

    Scheduler scheduler("owner-test");
    TimerOpts opts;
    opts.ownerKey = owner;
    scheduler.after(0.0, [&]() { calls += 1000; }, TimeDomain::Real, opts);

    EXPECT_GE(SubscriptionOwnerIndex::entryCount(owner), 3u);
    EXPECT_EQ(SubscriptionOwnerIndex::releaseOwner(owner), 3u);
    onUpdate.call();
    onClick.call(1);
    signal.invoke();
    scheduler.process(TimeDomain::Real);
    EXPECT_EQ(calls, 100);
    EXPECT_EQ(SubscriptionOwnerIndex::entryCount(owner), 0u);
}

TEST(OwnerIndexTest, EventGoneFirst) {
    int script = 0;
    const uint64_t owner = ownerKeyOf(&script);
    {
        auto event = std::make_unique<FunctionSubscribe<void()>>();
        event->add("t", []() {}, owner);
    }
    EXPECT_EQ(SubscriptionOwnerIndex::releaseOwner(owner), 0u);
}

TEST(OwnerIndexTest, RegistrationOwnerApplies) {
    int script = 0;
    const uint64_t previous = SubscriptionOwnerIndex::setRegistrationOwner(ownerKeyOf(&script));
    EXPECT_EQ(SubscriptionOwnerIndex::registrationOwner(), ownerKeyOf(&script));
    SubscriptionOwnerIndex::setRegistrationOwner(previous);
    EXPECT_EQ(SubscriptionOwnerIndex::registrationOwner(), previous);
}
