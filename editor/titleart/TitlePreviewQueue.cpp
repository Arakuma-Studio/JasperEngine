// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitlePreviewQueue.h"

#include "render/SystemRender.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;

const char* editor::titlePreviewPriorityName(TitlePreviewPriority priority) {
    switch (priority) {
        case TitlePreviewPriority::Hover: return "hover";
        case TitlePreviewPriority::Layers: return "layers";
        case TitlePreviewPriority::Tiles: return "tiles";
        case TitlePreviewPriority::Thumbnails: return "thumbnails";
        case TitlePreviewPriority::Prefetch: return "prefetch";
    }
    return "prefetch";
}

bool TitlePreviewQueue::Item::visible() const {
    return std::any_of(visibility.begin(), visibility.end(), [](const auto& v) { return v.second; });
}

TitlePreviewQueue::TitlePreviewQueue(std::function<double()> clockMs, std::function<uint64_t()> contextId)
    : clock(std::move(clockMs)), context(std::move(contextId)) {
    if (!clock) {
        clock = [] {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        };
    }
    if (context) lastContext = context();
}

TitlePreviewQueue::~TitlePreviewQueue() = default;

TitlePreviewQueue& TitlePreviewQueue::editor() {
    static TitlePreviewQueue* queue = new TitlePreviewQueue({}, [] { return SystemRender::getContextId(); });
    return *queue;
}

uint64_t TitlePreviewQueue::request(TitlePreviewPriority priority, const std::string& key, TitlePreviewFactory factory,
                                    TitlePreviewOwner owner, bool visible) {
    const uint64_t id = nextRequest++;
    Item* item = nullptr;
    for (const auto& existing : items) {
        if (existing->key == key && !key.empty()) item = existing.get();
    }
    if (!item) {
        auto made = std::make_unique<Item>();
        made->priority = priority;
        made->key = key;
        made->factory = std::move(factory);
        made->order = nextOrder++;
        item = made.get();
        items.push_back(std::move(made));
    } else if (priority < item->priority) {
        item->priority = priority;
    }
    item->requests[id] = owner;
    item->visibility[id] = visible;
    byRequest[id] = item;
    refreshCounts();
    return id;
}

void TitlePreviewQueue::setVisible(uint64_t id, bool visible) {
    auto it = byRequest.find(id);
    if (it != byRequest.end()) it->second->visibility[id] = visible;
}

void TitlePreviewQueue::drop(Item* item) {
    items.erase(std::remove_if(items.begin(), items.end(), [item](const auto& i) { return i.get() == item; }), items.end());
}

void TitlePreviewQueue::cancel(uint64_t id) {
    auto it = byRequest.find(id);
    if (it == byRequest.end()) return;
    Item* item = it->second;
    item->requests.erase(id);
    item->visibility.erase(id);
    byRequest.erase(it);
    if (item->requests.empty()) drop(item);
    refreshCounts();
}

void TitlePreviewQueue::cancelAll(TitlePreviewOwner owner) {
    std::vector<uint64_t> ids;
    for (const auto& [id, item] : byRequest) {
        auto r = item->requests.find(id);
        if (r != item->requests.end() && r->second == owner) ids.push_back(id);
    }
    for (uint64_t id : ids) cancel(id);
}

TitlePreviewState TitlePreviewQueue::state(uint64_t id) const {
    auto it = byRequest.find(id);
    return it != byRequest.end() ? it->second->state : TitlePreviewState::Unknown;
}

TitlePreviewJob* TitlePreviewQueue::result(uint64_t id) const {
    auto it = byRequest.find(id);
    return it != byRequest.end() && it->second->state == TitlePreviewState::Done ? it->second->job.get() : nullptr;
}

void TitlePreviewQueue::deviceLost() {
    for (const auto& item : items) {
        item->job.reset();
        item->state = TitlePreviewState::Queued;
        counters.restarts++;
    }
    restFrames = 0;
    refreshCounts();
}

void TitlePreviewQueue::resetStats() {
    const size_t queued = counters.queued, done = counters.done;
    counters = TitlePreviewStats();
    counters.queued = queued;
    counters.done = done;
}

void TitlePreviewQueue::refreshCounts() {
    counters.queued = 0;
    counters.done = 0;
    for (const auto& item : items) {
        if (item->state == TitlePreviewState::Queued || item->state == TitlePreviewState::Running) counters.queued++;
        else if (item->state == TitlePreviewState::Done) counters.done++;
    }
}

TitlePreviewQueue::Item* TitlePreviewQueue::pick() const {
    Item* best = nullptr;
    for (const auto& item : items) {
        if (item->state != TitlePreviewState::Queued && item->state != TitlePreviewState::Running) continue;
        if (!best) {
            best = item.get();
            continue;
        }
        const auto rank = [](const Item* i) { return std::make_tuple((int)i->priority, i->visible() ? 0 : 1, i->order); };
        if (rank(item.get()) < rank(best)) best = item.get();
    }
    return best;
}

void TitlePreviewQueue::runFrame(double budgetMs) {
    counters.frames++;
    if (context) {
        const uint64_t now = context();
        if (now != lastContext) {
            lastContext = now;
            deviceLost();
        }
    }
    if (restFrames > 0) {
        restFrames--;
        counters.lastFrameMs = 0;
        return;
    }
    const double start = clock();
    double spent = 0;
    bool ranAlone = false;
    for (;;) {
        Item* item = pick();
        if (!item) break;
        if (!item->job) {
            item->job = item->factory ? item->factory() : nullptr;
            if (!item->job) {
                item->state = TitlePreviewState::Failed;
                continue;
            }
        }
        const double cost = item->job->nextStepCostUs() / 1000.0;
        if (cost > budgetMs) {
            // runs alone: only as a frame's first step, then the queue rests
            if (spent > 0) break;
            ranAlone = true;
        } else if (spent > 0 && spent + cost > budgetMs) {
            break;
        }
        item->state = TitlePreviewState::Running;
        const double left = std::max(0.0, budgetMs - spent);
        const bool finished = item->job->step((uint32_t)std::max(1.0, std::floor(left * 1000.0)));
        counters.steps++;
        if (item->job->failed()) item->state = TitlePreviewState::Failed;
        else if (finished) item->state = TitlePreviewState::Done;
        spent = clock() - start;
        if (ranAlone) {
            counters.aloneFrames++;
            restFrames = std::max(0, (int)std::ceil(cost / std::max(budgetMs, 1e-6)) - 1);
            break;
        }
        if (spent >= budgetMs) break;
    }
    counters.lastFrameMs = spent;
    counters.maxFrameMs = std::max(counters.maxFrameMs, ranAlone ? 0.0 : spent);
    refreshCounts();
}
