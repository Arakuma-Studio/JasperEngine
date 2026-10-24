// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Primitives.h"
#include "jobs/detail/TimedWait.h"

using namespace jasperengine;
using namespace jasperengine::jobs;

static std::chrono::nanoseconds toDuration(double milliseconds){
    return std::chrono::nanoseconds((int64_t)(milliseconds * 1.0e6));
}

void Event::set(){
    std::lock_guard<std::mutex> lock(mutex);
    signalled = true;
    cv.notify_all();
}

void Event::reset(){
    std::lock_guard<std::mutex> lock(mutex);
    signalled = false;
}

bool Event::isSet() const{
    std::lock_guard<std::mutex> lock(mutex);
    return signalled;
}

void Event::wait(){
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this]{ return signalled; });
}

bool Event::waitFor(double milliseconds){
    std::unique_lock<std::mutex> lock(mutex);
    return jobs::detail::timedWait(cv, lock, toDuration(milliseconds), [this]{ return signalled; });
}

Semaphore::Semaphore(uint32_t initial): count(initial){
}

void Semaphore::release(uint32_t n){
    std::lock_guard<std::mutex> lock(mutex);
    count += n;
    if (n == 1)
        cv.notify_one();
    else
        cv.notify_all();
}

void Semaphore::acquire(){
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this]{ return count > 0; });
    count--;
}

bool Semaphore::tryAcquire(){
    std::lock_guard<std::mutex> lock(mutex);
    if (count == 0)
        return false;
    count--;
    return true;
}

bool Semaphore::tryAcquireFor(double milliseconds){
    std::unique_lock<std::mutex> lock(mutex);
    if (!jobs::detail::timedWait(cv, lock, toDuration(milliseconds), [this]{ return count > 0; }))
        return false;
    count--;
    return true;
}

void jobs::detail::retainCancel(CancelState* state){
    if (state)
        state->refs.fetch_add(1, std::memory_order_relaxed);
}

void jobs::detail::releaseCancel(CancelState* state){
    if (state && state->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete state;
}

CancelToken::CancelToken(): state(new jobs::detail::CancelState()){
}

CancelToken::CancelToken(const CancelToken& other): state(other.state){
    jobs::detail::retainCancel(state);
}

CancelToken::CancelToken(CancelToken&& other) noexcept: state(other.state){
    other.state = nullptr;
}

CancelToken& CancelToken::operator=(const CancelToken& other){
    if (this != &other){
        jobs::detail::retainCancel(other.state);
        jobs::detail::releaseCancel(state);
        state = other.state;
    }
    return *this;
}

CancelToken& CancelToken::operator=(CancelToken&& other) noexcept{
    if (this != &other){
        jobs::detail::releaseCancel(state);
        state = other.state;
        other.state = nullptr;
    }
    return *this;
}

CancelToken::~CancelToken(){
    jobs::detail::releaseCancel(state);
}

void CancelToken::cancel(){
    if (state)
        state->cancelled.store(true, std::memory_order_release);
}

bool CancelToken::isCancelled() const{
    return state && state->cancelled.load(std::memory_order_acquire);
}
