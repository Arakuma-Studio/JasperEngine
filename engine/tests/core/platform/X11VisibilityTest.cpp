// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The hidden state of an exported game's X11 window (engine/platform/linux/X11Visibility;
// platform-lifecycle spec D3): unmapped, or _NET_WM_STATE_HIDDEN as mutter sets it when it
// minimizes without unmapping. On the tests' Xvfb (no window manager: the test sets the property
// itself), on a display connection of its own, not WindowLinux's.

#if defined(__linux__) && defined(JASPER_TEST_HAS_GPU)

// gtest first: Xlib defines None, Bool and Status as macros, which break its declarations
#include <gtest/gtest.h>

#include "X11Visibility.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <chrono>
#include <functional>
#include <thread>

using namespace jasperengine;

namespace {

    class X11VisibilityTest : public ::testing::Test {
    protected:
        Display* display = nullptr;
        Window window = 0;
        X11Visibility visibility;
        Atom netWmState = 0;
        Atom netWmStateHidden = 0;

        void SetUp() override {
            display = XOpenDisplay(nullptr);
            ASSERT_NE(display, nullptr) << "no X display (the unit tests run under Xvfb)";
            const int screen = DefaultScreen(display);
            XSetWindowAttributes attributes{};
            attributes.event_mask = StructureNotifyMask | PropertyChangeMask;
            window = XCreateWindow(display, RootWindow(display, screen), 0, 0, 64, 64, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWEventMask, &attributes);
            netWmState = XInternAtom(display, "_NET_WM_STATE", False);
            netWmStateHidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
            visibility.attach(display, window, false);
            XMapWindow(display, window);
            ASSERT_TRUE(waitUntil([this]() { return !visibility.isHidden(); })) << "no MapNotify";
        }

        void TearDown() override {
            if (display) {
                if (window) XDestroyWindow(display, window);
                XCloseDisplay(display);
            }
        }

        // feeds the window's events to visibility until done() or two seconds
        bool waitUntil(const std::function<bool()>& done, int* changes = nullptr) {
            XFlush(display);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < end) {
                while (XPending(display)) {
                    XEvent event{};
                    XNextEvent(display, &event);
                    if (visibility.handleEvent(event) && changes) (*changes)++;
                }
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return done();
        }

        void setStates(const Atom* states, int count) {
            XChangeProperty(display, window, netWmState, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(states), count);
        }
    };

}

TEST_F(X11VisibilityTest, UnmapAndMapAreHiddenAndShown){
    int changes = 0;
    XUnmapWindow(display, window);
    EXPECT_TRUE(waitUntil([this]() { return visibility.isHidden(); }, &changes));
    XMapWindow(display, window);
    EXPECT_TRUE(waitUntil([this]() { return !visibility.isHidden(); }, &changes));
    EXPECT_EQ(changes, 2);
}

TEST_F(X11VisibilityTest, NetWmStateHiddenIsHidden){
    const Atom hidden[] = {netWmStateHidden};
    setStates(hidden, 1);
    EXPECT_TRUE(waitUntil([this]() { return visibility.isHidden(); }));
    EXPECT_TRUE(visibility.isWmHidden());
    EXPECT_TRUE(X11Visibility::hasNetWmState(display, window, netWmStateHidden));

    setStates(nullptr, 0);
    EXPECT_TRUE(waitUntil([this]() { return !visibility.isHidden(); }));

    setStates(hidden, 1);
    EXPECT_TRUE(waitUntil([this]() { return visibility.isHidden(); }));
    XDeleteProperty(display, window, netWmState);
    EXPECT_TRUE(waitUntil([this]() { return !visibility.isHidden(); }));
}

TEST_F(X11VisibilityTest, OtherPropertiesChangeNothing){
    int changes = 0;
    XStoreName(display, window, "JasperEngine");
    const Atom maximized[] = {XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False),
                              XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False)};
    setStates(maximized, 2);
    XSync(display, False);
    waitUntil([]() { return false; }, &changes);
    EXPECT_EQ(changes, 0);
    EXPECT_FALSE(visibility.isHidden());
}

// the one property read WindowLinux's maximize state shares
TEST_F(X11VisibilityTest, HasNetWmStateReadsMaximized){
    const Atom vertical = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    const Atom horizontal = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    EXPECT_FALSE(X11Visibility::hasNetWmState(display, window, vertical));
    const Atom maximized[] = {vertical, horizontal};
    setStates(maximized, 2);
    XSync(display, False);
    EXPECT_TRUE(X11Visibility::hasNetWmState(display, window, vertical));
    EXPECT_TRUE(X11Visibility::hasNetWmState(display, window, horizontal));
    EXPECT_FALSE(X11Visibility::hasNetWmState(display, window, netWmStateHidden));
}

TEST_F(X11VisibilityTest, AttachReadsAHiddenWindow){
    const Atom hidden[] = {netWmStateHidden};
    setStates(hidden, 1);
    XSync(display, False);
    X11Visibility adopted;
    adopted.attach(display, window, true);
    EXPECT_TRUE(adopted.isHidden());
}

#endif
