// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
#include "diag/detail/CrashFrames.h"
#include <gtest/gtest.h>

using jasperengine::diag::crash::detail::crashCallerStart;
namespace {
void* address(uintptr_t value) { return reinterpret_cast<void*>(value); }
}
TEST(CrashFrames, SkipsThroughInterruptedPc) {
    void* frames[] = {address(10), address(20), address(30), address(40), address(50)};
    EXPECT_EQ(crashCallerStart(frames, 5, 30, 2), 3u);
}
TEST(CrashFrames, MissingPcKeepsCallersAfterKnownHandlerFrames) {
    void* frames[] = {address(10), address(20), address(40), address(50)};
    EXPECT_EQ(crashCallerStart(frames, 4, 30, 2), 2u);
    EXPECT_EQ(crashCallerStart(frames, 4, 0, 2), 2u);
}
TEST(CrashFrames, ShortOrEmptyUnwindIsBounded) {
    void* frames[] = {address(10)};
    EXPECT_EQ(crashCallerStart(frames, 1, 30, 2), 1u);
    EXPECT_EQ(crashCallerStart(nullptr, 0, 30, 2), 0u);
}
TEST(CrashFrames, ThumbPcIgnoresInstructionSetBit) {
    void* frames[] = {address(10), address(20), address(31), address(40)};
    EXPECT_EQ(crashCallerStart(frames, 4, 30, 2, ~uintptr_t{1}), 3u);
}
TEST(CrashFrames, MissingPcAlsoSkipsKnownDispatchWrapper) {
    void* frames[] = {address(10), address(20), address(25), address(40), address(50)};
    EXPECT_EQ(crashCallerStart(frames, 5, 30, 2, ~uintptr_t{0}, 25), 3u);
}
