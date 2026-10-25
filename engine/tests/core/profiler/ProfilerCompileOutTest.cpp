// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The profiling macros compiled out (spec S15 AC1, R13): this translation unit sees
// JASPERENGINE_PROFILER=0, as a release game does, and every macro must expand to ((void)0)
// without evaluating its arguments. (The other half of AC1, no profiler symbol in a release
// export, is checked on the exported binary: tools/smoke-test.sh with SMOKE_CHECK_NO_PROFILER.)

#undef JASPERENGINE_PROFILER
#define JASPERENGINE_PROFILER 0
#undef JASPERENGINE_TRACY
#define JASPERENGINE_TRACY 0
#include "profiler/Profile.h"

#include <gtest/gtest.h>

namespace {
    int sideEffects = 0;
    [[maybe_unused]] const char* touch(){
        sideEffects++;
        return "touched";
    }
    [[maybe_unused]] double touchValue(){
        sideEffects++;
        return 1.0;
    }
}

TEST(ProfilerCompileOut, MacrosEvaluateNothing){
    sideEffects = 0;
    JE_PROFILE_SCOPE(touch());
    JE_PROFILE_FUNCTION();
    JE_PROFILE_SCOPE_DYN(std::string(touch()));
    JE_PROFILE_THREAD(touch());
    JE_PROFILE_FRAME();
    JE_PROFILE_COUNTER(touch(), touchValue());
    JE_GPU_ALLOC_SCOPE(PostFX, touch());
    EXPECT_EQ(sideEffects, 0);
}
