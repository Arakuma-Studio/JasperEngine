// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FRAMELIMITER_H
#define FRAMELIMITER_H

#include "Export.h"

namespace jasperengine {

    // Caps a frame loop at a frame rate (Engine.targetFrameRate without vsync, vSyncCount
    // past what the presenter supports): Engine::getFramePacing().limiterFps. Deadline based,
    // so a late frame is made up by the next; a deadline more than one period behind (a
    // hitch, a breakpoint) starts the schedule again from now.
    class JASPERENGINE_API FrameLimiter {
    private:
        double deadline = 0.0;
        bool started = false;

    public:
        // Blocks until the next frame is due: sleeps until 1 ms before the deadline, then
        // yields. fps <= 0 returns at once and starts the schedule again.
        void wait(double fps);

        // For loops the system drives (requestAnimationFrame, a timer): whether a frame is
        // due now, `tolerance` seconds early counting as due; a due frame moves the schedule on.
        // fps <= 0: always due.
        bool isDue(double fps, double tolerance = 0.002);
        // the same at a given time (monotonic seconds)
        bool isDueAt(double current, double fps, double tolerance = 0.002);

        // Seconds until the next frame is due: 0 when it is due, or before the first frame
        double secondsUntilDue() const;
        double secondsUntilDueAt(double current) const;

        void reset();

        // monotonic seconds
        static double now();
    };

}

#endif // FRAMELIMITER_H
