// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The script status MCP reports (get_script_status, reload_scripts, editor_status.scripts): a
// build's events, posted from its worker thread, count as soon as they are posted. A build that
// finished after this frame's update() used to read as still building: reload_scripts' wait ended
// on the build itself and answered "building", and so did the get_script_status right after it
// (test_script_status.py::test_cpp_status_states, intermittently on loaded hosts).

#include "Generator.h"
#include "util/ScriptStatus.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

namespace {

    ScriptBuildEvent buildEvent(ScriptBuildEvent::Kind kind) {
        ScriptBuildEvent event;
        event.kind = kind;
        event.row = "cpp";
        event.projectPath = "/projects/Status";
        return event;
    }

}

TEST(ScriptStatus, ReportsWhatABuildPostedBeforeTheFrameApplies) {
    ScriptStatus& status = ScriptStatus::get();
    status.followBuilds();
    status.update();

    Generator::buildEvents().invoke(buildEvent(ScriptBuildEvent::Kind::Started));
    status.update();   // a frame saw the build start
    EXPECT_EQ(status.toJson()["cpp"]["state"], "building");

    // the build ends on its worker; no frame has applied it yet
    ScriptBuildEvent finished = buildEvent(ScriptBuildEvent::Kind::Finished);
    finished.success = false;
    finished.seconds = 2.0;
    finished.output = {"/projects/Status/scripts/Spinner.cpp:10:14: error: 'notDeclaredAnywhere' was not declared in this scope"};
    Generator::buildEvents().invoke(finished);

    const auto json = status.toJson();
    EXPECT_EQ(json["cpp"]["state"], "failed");
    ASSERT_EQ(json["cpp"]["findings"].size(), 1u);
    EXPECT_EQ(json["cpp"]["findings"][0]["file"], "scripts/Spinner.cpp");
    EXPECT_EQ(json["cpp"]["findings"][0]["line"], 10);

    // the next frame finds nothing left to apply
    EXPECT_FALSE(status.update());
    EXPECT_EQ(status.toJson()["cpp"]["state"], "failed");

    status.setCpp(CppScriptState::None);
}
