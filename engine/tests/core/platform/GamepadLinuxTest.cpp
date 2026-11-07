// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An exported Linux game's pads while its window is hidden (engine/platform/linux/GamepadLinux;
// platform-lifecycle spec D5): release() lets go of what is held and closes the node, the next
// poll() reopens it and reports what is held as new input, and a node another pad took meanwhile
// disconnects. The joystick node is a FIFO this test writes js_events into, the sysfs ids are
// files of its own: an Xbox 360 controller, which the community database maps.

#if defined(__linux__) && defined(JASPER_TEST_HAS_GPU)

#include "GamepadLinux.h"

#include "Engine.h"
#include "Input.h"
#include "TestTempDir.h"

#include <linux/joystick.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <fstream>
#include <vector>

using namespace jasperengine;

namespace {

    const char* callbackTag = "core-tests-gamepad-linux";

    class GamepadLinuxTest : public ::testing::Test {
    protected:
        test::TestTempDir dir{"gamepad-linux"};
        int writer = -1;
        std::vector<std::string> events;

        void SetUp() override {
            std::filesystem::create_directories(dir / "dev");
            ASSERT_EQ(mkfifo((dir / "dev/js0").c_str(), 0600), 0);
            // read and write: the pipe stays while the pad's own end is closed
            writer = open((dir / "dev/js0").c_str(), O_RDWR | O_NONBLOCK);
            ASSERT_GE(writer, 0);
            writeIds("045e");
            Engine::onGamepadConnect.add(callbackTag, [this](int id) { events.push_back("connect " + std::to_string(id)); });
            Engine::onGamepadDisconnect.add(callbackTag, [this](int id) { events.push_back("disconnect " + std::to_string(id)); });
            Engine::onGamepadButtonDown.add(callbackTag, [this](int, int button) { events.push_back("down " + std::to_string(button)); });
            Engine::onGamepadButtonUp.add(callbackTag, [this](int, int button) { events.push_back("up " + std::to_string(button)); });
        }

        void TearDown() override {
            Engine::onGamepadConnect.remove(callbackTag);
            Engine::onGamepadDisconnect.remove(callbackTag);
            Engine::onGamepadButtonDown.remove(callbackTag);
            Engine::onGamepadButtonUp.remove(callbackTag);
            if (writer >= 0) close(writer);
        }

        void writeIds(const std::string& vendor) {
            const std::filesystem::path ids = dir / "sys/js0/device/id";
            std::filesystem::create_directories(ids);
            std::ofstream(ids / "bustype") << "0003\n";
            std::ofstream(ids / "vendor") << vendor << "\n";
            std::ofstream(ids / "product") << "028e\n";
            std::ofstream(ids / "version") << "0114\n";
        }

        void send(unsigned char type, unsigned char number, short value) {
            js_event event{};
            event.type = type;
            event.number = number;
            event.value = value;
            ASSERT_EQ(write(writer, &event, sizeof(event)), (ssize_t)sizeof(event));
        }

        GamepadLinux gamepads{(dir / "dev/js*").string(), (dir / "sys").string()};
    };

}

TEST_F(GamepadLinuxTest, ReleaseLetsGoOfHeldInput){
    gamepads.poll(0.0);
    ASSERT_TRUE(Input::isGamepadConnected(0));
    // A (b0) held, the left trigger (a2) pulled
    send(JS_EVENT_BUTTON, 0, 1);
    send(JS_EVENT_AXIS, 2, 32767);
    gamepads.poll(0.1);
    EXPECT_TRUE(Input::isGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A));
    EXPECT_FLOAT_EQ(Input::getGamepadAxis(0, D_GAMEPAD_AXIS_LEFT_TRIGGER), 1.0f);

    events.clear();
    gamepads.release();
    EXPECT_FALSE(Input::isGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A));
    EXPECT_FLOAT_EQ(Input::getGamepadAxis(0, D_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f);
    EXPECT_EQ(events, std::vector<std::string>({"up " + std::to_string(D_GAMEPAD_BUTTON_A)}));
    // still connected: the pad comes back on show
    EXPECT_TRUE(Input::isGamepadConnected(0));
    gamepads.shutdown();
}

TEST_F(GamepadLinuxTest, ShowReportsStillHeldAsNewPress){
    gamepads.poll(0.0);
    send(JS_EVENT_BUTTON, 0, 1);
    gamepads.poll(0.1);
    gamepads.release();
    ASSERT_FALSE(Input::isGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A));

    // what the kernel sends on open: the state of every control
    events.clear();
    send(JS_EVENT_BUTTON | JS_EVENT_INIT, 0, 1);
    gamepads.poll(0.2);
    EXPECT_TRUE(Input::isGamepadButtonPressed(0, D_GAMEPAD_BUTTON_A));
    EXPECT_EQ(events, std::vector<std::string>({"down " + std::to_string(D_GAMEPAD_BUTTON_A)}));
    gamepads.shutdown();
}

TEST_F(GamepadLinuxTest, ReopenRefusesAnotherPad){
    gamepads.poll(0.0);
    gamepads.release();
    events.clear();
    // another vendor's pad on js0 while hidden
    writeIds("054c");
    gamepads.poll(0.1);
    EXPECT_EQ(events, std::vector<std::string>({"disconnect 0"}));
    EXPECT_FALSE(Input::isGamepadConnected(0));
    // the next scan connects it as a new pad
    gamepads.poll(1.1);
    EXPECT_TRUE(Input::isGamepadConnected(0));
    gamepads.shutdown();
}

#endif
