// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A hidden window as scripts see it (platform-lifecycle spec D4): Engine.isVisible and the
// onVisibilityChanged engine event, through RegisterEngineEvent as a script registers it.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"

#include "Engine.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class PlatformVisibilityTest : public scriptdata::ScriptDataTest {
    protected:
        void SetUp() override {
            ScriptDataTest::SetUp();
            Engine::resetApplicationState();
        }

        void TearDown() override {
            Engine::clearAllSubscriptions(true);
            Engine::resetApplicationState();
            ScriptDataTest::TearDown();
        }
    };

}

TEST_F(PlatformVisibilityTest, VisibilityEventReachesLua) {
    ASSERT_EQ(run("visibilityProbe = { seen = {} }\n"
                  "function visibilityProbe:onVisibilityChanged(visible) table.insert(self.seen, visible) end\n"
                  "RegisterEngineEvent(visibilityProbe, 'onVisibilityChanged')\n"), "");

    std::vector<std::string> values;
    ASSERT_EQ(run("return Engine.isVisible", &values), "");
    EXPECT_EQ(values, std::vector<std::string>({"true"}));

    Engine::systemVisibilityChanged(false);
    ASSERT_EQ(run("return Engine.isVisible, #visibilityProbe.seen", &values), "");
    EXPECT_EQ(values, std::vector<std::string>({"false", "1"}));

    Engine::systemVisibilityChanged(true);
    ASSERT_EQ(run("return tostring(visibilityProbe.seen[1]), tostring(visibilityProbe.seen[2]), Engine.isVisible", &values), "");
    EXPECT_EQ(values, std::vector<std::string>({"false", "true", "true"}));
}
