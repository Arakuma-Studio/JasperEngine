// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GamepadLinux_h
#define GamepadLinux_h

#include "GamepadDB.h"

#include <linux/input.h>

#include <array>
#include <string>

namespace jasperengine {

    // Linux joystick (/dev/input/js*) polling for exported games. Device
    // layouts disagree on which axes are the right stick and which are the
    // triggers, so the GUID is looked up in the community database shared with
    // the editor backend (GamepadDB / GamepadMappings.h).
    class GamepadLinux {
    public:
        static constexpr int COUNT = 16;
        static constexpr int BUTTON_COUNT = D_GAMEPAD_BUTTON_LAST + 1;
        static constexpr int AXIS_COUNT = D_GAMEPAD_AXIS_LAST + 1;
        static constexpr int RAW_BUTTON_COUNT = 32;
        static constexpr int RAW_AXIS_COUNT = 16;

        // devicePattern globs the joystick nodes, sysClassRoot holds their
        // <node>/device/id/ files (the tests point both at a directory of their own)
        explicit GamepadLinux(std::string devicePattern = "/dev/input/js*",
                              std::string sysClassRoot = "/sys/class/input");

        void shutdown();
        // now is a monotonic clock in seconds; new devices are scanned for once
        // per second rather than on every frame.
        void poll(double now);
        // A hidden window's game: held buttons go up and the axes rest (with
        // their events), and the nodes are closed so nothing is read. The next
        // poll() reopens them and reports what is still held as new input.
        void release();

    private:
        struct Pad {
            // connected with fd -1: released, reopened by the next poll()
            bool connected = false;
            int fd = -1;
            std::string name;
            std::string guid;
            std::array<unsigned char, ABS_CNT> axisMap{};
            // js_event.number is a device index, not an input code; the kernel
            // reports the code each index stands for.
            std::array<unsigned short, KEY_MAX - BTN_MISC + 1> buttonMap{};
            std::array<unsigned char, BUTTON_COUNT> buttons{};
            std::array<float, AXIS_COUNT> axes{};
            int hatX = 0;
            int hatY = 0;
            int hat = 0;

            editor::GamepadMapping mapping;
            bool mapped = false;
            std::array<float, RAW_AXIS_COUNT> rawAxes{};
            std::array<unsigned char, RAW_BUTTON_COUNT> rawButtons{};
        };

        void scan(double now);
        void connect(int id, const char* path);
        // false (and a disconnect) when the node is gone or another pad took it
        bool reopen(int id, Pad& pad);
        std::string devicePath(int id) const;
        bool readGuid(int id, std::string& guid) const;
        void disconnect(int id);
        void readEvents(int id, Pad& pad);

        void setButton(int id, Pad& pad, int button, bool pressed);
        void setAxis(int id, Pad& pad, int axis, float value);
        void updateHat(int id, Pad& pad, bool horizontal, int value);
        void applyMapping(int id, Pad& pad);

        std::string devicePattern;
        std::string sysClassRoot;
        double nextScan = 0.0;
        std::array<Pad, COUNT> pads;
    };

}

#endif /* GamepadLinux_h */
