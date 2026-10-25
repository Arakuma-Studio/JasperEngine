// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BackgroundRelease.h"

#include "jobs/JobSystem.h"

using namespace jasperengine;

namespace {

    // A strand on the Background lane: the frees happen one batch at a time on a worker,
    // never on a thread of their own. Never destroyed: commands (and what they hold) may be
    // freed during static destruction; once the job system is shut down a release frees on
    // the caller.
    jobs::SerialQueue& releaser() {
        static jobs::SerialQueue* queue = new jobs::SerialQueue("background-release", jobs::Lane::Background);
        return *queue;
    }

}

void editor::BackgroundRelease::push(std::shared_ptr<void> holder) {
    // the job owns the last reference; it is dropped when the job ends
    releaser().push([holder = std::move(holder)]() mutable { holder.reset(); });
}
