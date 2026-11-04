// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a capture costs when none runs, and its names (docs/specs/S35-agent-audio.md R13, R28)

#include <gtest/gtest.h>

#include "audio/AudioCapture.h"
#include "audio/AudioMarkers.h"
#include "subsystem/AudioSystem.h"

#ifdef JE_TEST_COUNTS_ALLOCATIONS
#include "support/AllocationCounter.h"
#endif

using namespace jasperengine;

#if JASPERENGINE_AUDIO_CAPTURE

TEST(AudioCaptureTest, SessionIdsAreDatedNames) {
    EXPECT_TRUE(AudioCapture::isSessionId("20261104-163000-00"));
    EXPECT_FALSE(AudioCapture::isSessionId("notes"));
    EXPECT_FALSE(AudioCapture::isSessionId("20261104-163000-0"));
    EXPECT_FALSE(AudioCapture::isSessionId("2026110a-163000-00"));
    EXPECT_FALSE(AudioCapture::isSessionId("20261104_163000-00"));
}

TEST(AudioCaptureTest, NothingIsOnBeforeABegin) {
    EXPECT_FALSE(AudioCapture::isActive());
    EXPECT_FALSE(AudioMarkers::isEnabled());
    AudioCapture::mark("nothing", false);
    EXPECT_EQ(AudioMarkers::getCount(), 0u);
    EXPECT_EQ(AudioCapture::cursor(), 0u);
}

#endif

#ifdef JE_TEST_COUNTS_ALLOCATIONS
TEST(AudioCaptureTest, OffPathAllocatesNothing) {
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Deterministic);
    ASSERT_TRUE(AudioSystem::init());
    // what the first frame and the first stop make once: the voice list, a function's static
    AudioSystem::pumpDeterministic(1.0 / 60.0);
    AudioSystem::stopVoice(12345);
    uint64_t allocated = 0;
    {
        testsupport::ThreadAllocationCount count;
        for (int frame = 0; frame < 120; frame++) {
            AudioSystem::pumpDeterministic(1.0 / 60.0);
            // the stamping sites with no capture and no voice
            AudioSystem::stopVoice(12345);
            AudioCapture::mark("off", true);
        }
        allocated = count.count();
    }
    AudioSystem::setBackend(AudioBackend::Auto);
    AudioSystem::close();
    EXPECT_EQ(allocated, 0u);
}
#endif
